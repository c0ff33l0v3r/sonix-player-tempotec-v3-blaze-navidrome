#define _GNU_SOURCE 1
#include "navidromedebug.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#ifndef ND_DEBUG_FILE_BYTES
#define ND_DEBUG_FILE_BYTES (2 * 1024 * 1024)
#endif
#define KEEP_FILES 5
#define QUEUE_LINES 32
#define LINE_BYTES 512
#define PREFIX "navidrome-debug-"

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_t writer;
static bool started, stopping, enabled;
static unsigned generation, applied;
static char root_path[1024];
static char queue[QUEUE_LINES][LINE_BYTES];
static unsigned head, queued, lost;
static nd_debug_state_t state;

static void initialize(void) {
	pthread_condattr_t attr;
	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(&ready, &attr);
	pthread_condattr_destroy(&attr);
}

static bool owned_name(const char *name) {
	size_t prefix = strlen(PREFIX), len = strlen(name);
	if (strncmp(name, PREFIX, prefix) || len < prefix + 23 || strcmp(name + len - 4, ".log"))
		return false;
	for (size_t i = prefix; i < len - 4; i++) {
		if (i == prefix + 8 || i == prefix + 15) {
			if (name[i] != '-')
				return false;
		} else if (name[i] < '0' || name[i] > '9')
			return false;
	}
	return true;
}

// Constant memory even when a directory contains many unrelated files.
static bool retain(const char *folder, const char *active) {
	for (;;) {
		DIR *dir = opendir(folder);
		if (!dir)
			return false;
		unsigned count = 0;
		char oldest[1200] = "";
		struct timespec first = {0};
		struct dirent *entry;
		while ((entry = readdir(dir))) {
			char path[1200];
			struct stat st;
			if (!owned_name(entry->d_name) || snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name) >= (int)sizeof(path) || lstat(path, &st) || !S_ISREG(st.st_mode))
				continue;
			count++;
			if (!strcmp(path, active))
				continue;
			if (!*oldest || st.st_mtim.tv_sec < first.tv_sec || (st.st_mtim.tv_sec == first.tv_sec && st.st_mtim.tv_nsec < first.tv_nsec)) {
				snprintf(oldest, sizeof(oldest), "%s", path);
				first = st.st_mtim;
			}
		}
		closedir(dir);
		if (count <= KEEP_FILES)
			return true;
		if (!*oldest || unlink(oldest))
			return false;
	}
}

static int open_session(const char *root, char path[1200]) {
	char folder[1200], stamp[32];
	struct stat st;
	struct statvfs fs;
	if (!*root || stat(root, &st) || !S_ISDIR(st.st_mode) || !(st.st_mode & 0222) || statvfs(root, &fs) || (fs.f_flag & ST_RDONLY) || snprintf(folder, sizeof(folder), "%s/.local", root) >= (int)sizeof(folder))
		return -1;
	if (mkdir(folder, 0700) && errno != EEXIST)
		return -1;
	if (lstat(folder, &st) || !S_ISDIR(st.st_mode) || !(st.st_mode & 0222))
		return -1;
	time_t now = time(NULL);
	struct tm local;
	if (!localtime_r(&now, &local) || !strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local))
		return -1;
	for (unsigned n = 0; n < 100000; n++) {
		if (snprintf(path, 1200, "%s/" PREFIX "%s-%03u.log", folder, stamp, n) >= 1200)
			return -1;
		int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (fd >= 0) {
			if (retain(folder, path))
				return fd;
			close(fd);
			unlink(path);
			return -1;
		}
		if (errno != EEXIST)
			return -1;
	}
	return -1;
}

static bool write_line(int fd, const char *line, size_t len) {
	while (len) {
		ssize_t n = write(fd, line, len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return false;
		line += n;
		len -= (size_t)n;
	}
	return true;
}

static void close_session(int fd) {
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
}

static void *run(void *unused) {
	(void)unused;
	int fd = -1;
	size_t bytes = 0;
	char active_root[1024] = "", active_path[1200] = "";
	pthread_mutex_lock(&mutex);
	for (;;) {
		// Off drains already accepted lines; card changes discard their queue.
		bool drain = !enabled && (queued || lost) && fd >= 0 && !strcmp(active_root, root_path);
		if (applied != generation && !drain) {
			unsigned next = generation;
			bool wanted = enabled && !stopping;
			char root[1024];
			snprintf(root, sizeof(root), "%s", root_path);
			pthread_mutex_unlock(&mutex);
			close_session(fd);
			fd = wanted ? open_session(root, active_path) : -1;
			bytes = 0;
			snprintf(active_root, sizeof(active_root), "%s", root);
			pthread_mutex_lock(&mutex);
			applied = next;
			state.recording = fd >= 0;
			state.error = wanted && fd < 0 ? "nd_debug_storage" : NULL;
			snprintf(state.path, sizeof(state.path), "%s", fd >= 0 ? active_path : "");
			if (fd < 0)
				queued = head = lost = 0;
			pthread_cond_broadcast(&ready);
			continue;
		}
		if (stopping && !queued)
			break;
		if (fd >= 0 && (queued || lost)) {
			char line[LINE_BYTES];
			if (lost) {
				snprintf(line, sizeof(line), "navidrome: debug dropped_lines=%u", lost);
				lost = 0;
			} else {
				memcpy(line, queue[head], sizeof(line));
				head = (head + 1) % QUEUE_LINES;
				queued--;
			}
			pthread_mutex_unlock(&mutex);
			struct timespec now;
			clock_gettime(CLOCK_REALTIME, &now);
			struct tm local;
			localtime_r(&now.tv_sec, &local);
			char text[LINE_BYTES + 80], stamp[40];
			strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
			int n = snprintf(text, sizeof(text), "%s.%03ld %s\n", stamp, now.tv_nsec / 1000000, line);
			if (bytes + (size_t)n > ND_DEBUG_FILE_BYTES) {
				close_session(fd);
				fd = open_session(active_root, active_path);
				bytes = 0;
			}
			bool ok = fd >= 0 && write_line(fd, text, (size_t)n);
			if (ok)
				bytes += (size_t)n;
			else {
				close_session(fd);
				fd = -1;
			}
			pthread_mutex_lock(&mutex);
			state.recording = fd >= 0;
			state.error = ok ? NULL : "nd_debug_storage";
			snprintf(state.path, sizeof(state.path), "%s", fd >= 0 ? active_path : "");
			if (fd < 0)
				queued = head = lost = 0;
			pthread_cond_broadcast(&ready);
			continue;
		}
		struct timespec deadline;
		clock_gettime(CLOCK_MONOTONIC, &deadline);
		deadline.tv_sec += 2;
		int waited = pthread_cond_timedwait(&ready, &mutex, &deadline);
		if (waited == ETIMEDOUT && fd >= 0) {
			pthread_mutex_unlock(&mutex);
			bool synced = fsync(fd) == 0;
			if (!synced) {
				close(fd);
				fd = -1;
			}
			pthread_mutex_lock(&mutex);
			if (!synced) {
				state.recording = false;
				state.error = "nd_debug_storage";
				state.path[0] = 0;
				queued = head = lost = 0;
			}
		} else if (waited == ETIMEDOUT && enabled && *root_path)
			generation++; // Retry local storage only; never network work.
	}
	pthread_mutex_unlock(&mutex);
	close_session(fd);
	return NULL;
}

void nd_debug_enable(bool wanted) {
	pthread_once(&once, initialize);
	pthread_mutex_lock(&mutex);
	if (enabled != wanted) {
		enabled = wanted;
		generation++;
	}
	if (wanted && !started) {
		stopping = false;
		if (!pthread_create(&writer, NULL, run, NULL))
			started = true;
		else
			state.error = "nd_worker_error";
	}
	pthread_cond_broadcast(&ready);
	pthread_mutex_unlock(&mutex);
}

void nd_debug_snapshot(nd_debug_state_t *out) {
	pthread_mutex_lock(&mutex);
	*out = state;
	out->enabled = enabled;
	out->pending = started && applied != generation;
	pthread_mutex_unlock(&mutex);
}

void nd_debug_card(const char *root) {
	pthread_once(&once, initialize);
	pthread_mutex_lock(&mutex);
	snprintf(root_path, sizeof(root_path), "%s", root && strlen(root) < sizeof(root_path) ? root : "");
	queued = head = lost = 0;
	generation++;
	pthread_cond_broadcast(&ready);
	while (started && applied != generation)
		pthread_cond_wait(&ready, &mutex);
	pthread_mutex_unlock(&mutex);
}

void nd_debug_line(const char *line) {
	if (strncmp(line, "navidrome: ", 11) && strncmp(line, "http: protected ", 16))
		return;
	pthread_mutex_lock(&mutex);
	if (enabled && started && *root_path) {
		if (queued == QUEUE_LINES)
			lost++;
		else {
			size_t len = strcspn(line, "\r\n");
			if (len >= LINE_BYTES)
				len = LINE_BYTES - 1;
			memcpy(queue[(head + queued) % QUEUE_LINES], line, len);
			queue[(head + queued) % QUEUE_LINES][len] = 0;
			queued++;
		}
		pthread_cond_broadcast(&ready);
	}
	pthread_mutex_unlock(&mutex);
}

void nd_log(const char *format, ...) {
	char line[LINE_BYTES];
	va_list ap;
	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	fputs(line, stderr);
	nd_debug_line(line);
}

void nd_debug_shutdown(void) {
	pthread_once(&once, initialize);
	pthread_mutex_lock(&mutex);
	bool join = started;
	enabled = false;
	stopping = true;
	generation++;
	pthread_cond_broadcast(&ready);
	pthread_mutex_unlock(&mutex);
	if (join)
		pthread_join(writer, NULL);
	pthread_mutex_lock(&mutex);
	started = false;
	memset(&state, 0, sizeof(state));
	pthread_mutex_unlock(&mutex);
}

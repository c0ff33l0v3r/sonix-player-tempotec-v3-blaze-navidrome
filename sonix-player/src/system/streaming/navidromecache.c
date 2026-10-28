#include "navidromecache.h"
#include "src/gui/shell/gui.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t usage_lock = PTHREAD_MUTEX_INITIALIZER;
static nd_cache_usage_t saved;
static void (*usage_listener)(void);
void nd_cache_set_listener(void (*listener)(void)) { usage_listener = listener; }
static void usage_notice(void *unused) {
	(void)unused;
	if (usage_listener)
		usage_listener();
}
static bool hex32(const char *name) { return strlen(name) == 32 && strspn(name, "0123456789abcdef") == 32; }
static bool owned(const char *name, bool art) {
	if (strlen(name) < 34 || strspn(name, "0123456789abcdef") != 32 || name[32] != '.')
		return false;
	const char *ext = name + 32;
	if (art)
		return !strcmp(ext, ".img") || !strcmp(ext, ".img.tmp");
	static const char *extensions[] = {".complete", ".tmp", ".flac", ".mp3", ".m4a", ".ogg", ".opus", ".wav", ".aiff", ".caf", ".aac", ".dsf", ".dff", ".wv", ".ape"};
	for (size_t i = 0; i < sizeof(extensions) / sizeof(*extensions); i++)
		if (!strcmp(ext, extensions[i]))
			return true;
	return false;
}
// Refuse symlinks in every component, including the card root and cache parent.
static int open_directory(const char *path) {
	if (!path || !*path || strlen(path) >= 1100) {
		errno = EINVAL;
		return -1;
	}
	char copy[1100];
	snprintf(copy, sizeof(copy), "%s", path);
	int fd = open(*path == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	char *save = NULL;
	for (char *part = strtok_r(copy, "/", &save); part && fd >= 0; part = strtok_r(NULL, "/", &save)) {
		// A dot keeps the current descriptor; it cannot escape the checked path.
		if (!strcmp(part, "."))
			continue;
		if (!strcmp(part, "..")) {
			close(fd);
			errno = EINVAL;
			return -1;
		}
		int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		int error = errno;
		close(fd);
		fd = next;
		errno = error;
	}
	return fd;
}
static bool files(int parent, const char *name, bool audio_root, bool clear, uint64_t *bytes, const nd_context_t *context) {
	int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return errno == ENOENT || errno == ELOOP || errno == ENOTDIR;
	DIR *dir = fdopendir(fd);
	if (!dir) {
		close(fd);
		return false;
	}
	bool ok = true;
	for (;;) {
		if (context && !navidrome_playback_current(context, false)) {
			ok = false;
			break;
		}
		errno = 0;
		struct dirent *entry = readdir(dir);
		if (!entry) {
			if (errno)
				ok = false;
			break;
		}
		if (audio_root) {
			if (hex32(entry->d_name))
				ok = files(fd, entry->d_name, false, clear, bytes, context) && ok;
			continue;
		}
		// Audio scopes contain originals/completion records; art contains image/temp pairs.
		bool art = !strcmp(name, "art");
		if (!owned(entry->d_name, art))
			continue;
		struct stat st;
		if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
			ok = false;
			continue;
		}
		if (!S_ISREG(st.st_mode) || st.st_size < 0)
			continue;
		if (context && !navidrome_playback_current(context, false)) {
			ok = false;
			break;
		}
		if (clear && unlinkat(fd, entry->d_name, 0) == 0)
			continue;
		if (clear)
			ok = false;
		if ((uint64_t)st.st_size > UINT64_MAX - *bytes) {
			ok = false;
			continue;
		}
		*bytes += (uint64_t)st.st_size;
	}
	closedir(dir);
	return ok;
}
// Disposable ordering is derived metadata, outside the audio/art byte totals.
// Clear only these generated leaves after cache-I/O ownership has drained.
static bool clear_order(int parent) {
	int guard = openat(parent, "browse-order.db.lock", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (guard >= 0 && flock(guard, LOCK_EX | LOCK_NB)) {
		close(guard);
		return false;
	}
	if (guard < 0 && errno != ENOENT)
		return false;
	static const char *names[] = {"browse-order.db", "browse-order.db-journal", "browse-order.db.tmp", "browse-order.db.tmp-journal"};
	bool ok = true;
	for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++) {
		struct stat st;
		if (fstatat(parent, names[i], &st, AT_SYMLINK_NOFOLLOW)) {
			if (errno != ENOENT)
				ok = false;
			continue;
		}
		if (S_ISREG(st.st_mode) && unlinkat(parent, names[i], 0) && errno != ENOENT)
			ok = false;
	}
	if (guard >= 0) {
		flock(guard, LOCK_UN);
		close(guard);
	}
	return ok;
}
static bool scan(const char *path, bool clear, nd_cache_usage_t *usage, const nd_context_t *context) {
	char base[1100];
	if (!path || strlen(path) >= sizeof(base))
		return false;
	snprintf(base, sizeof(base), "%s", path);
	char *slash = strrchr(base, '/');
	if (!slash || (strcmp(slash, "/library.db") && strcmp(slash, "/favorites.db")))
		return false;
	*slash = 0;
	const char *tail = "/.local/navidrome/.cache";
	size_t size = strlen(base), n = strlen(tail);
	bool current = size >= n && !strcmp(base + size - n, tail);
	const char *old = "/.local/navidrome";
	if (!current && (size < strlen(old) || strcmp(base + size - strlen(old), old)))
		return false;
	usage->audio = usage->art = 0;
	bool ok = true, found = false;
	// Always account for both supported layouts, without following migration links.
	if (current)
		base[size - strlen("/.cache")] = 0;
	int legacy = open_directory(base);
	if (legacy >= 0) {
		found = true;
		if (clear)
			ok = clear_order(legacy) && ok;
		ok = files(legacy, "audio", true, clear, &usage->audio, context) && ok;
		ok = files(legacy, "art", false, clear, &usage->art, context) && ok;
		int modern = openat(legacy, ".cache", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (modern >= 0) {
			if (clear)
				ok = clear_order(modern) && ok;
			ok = files(modern, "audio", true, clear, &usage->audio, context) && ok;
			ok = files(modern, "art", false, clear, &usage->art, context) && ok;
			close(modern);
		} else if (errno != ENOENT && errno != ELOOP && errno != ENOTDIR)
			ok = false;
		close(legacy);
	}
	usage->ready = found;
	return found && ok;
}
bool nd_cache_scan(const char *path, bool clear, nd_cache_usage_t *usage) { return scan(path, clear, usage, NULL); }
typedef struct {
	nd_library_t library;
	nd_context_t context;
	bool clear;
	bool (*reader_active)(void);
} request_t;
static void *cache_worker(void *data) {
	request_t *q = data;
	nd_cache_usage_t result = {.card_epoch = q->library.card_epoch, .clearing = q->clear};
	const char *error = NULL;
	if (q->clear) {
		// Existing finite HTTP owners may take up to 30 seconds to retire.
		for (int waited = 0; waited < 35000 && (nd_cache_io_count() || (q->reader_active && q->reader_active())); waited += 10)
			usleep(10000);
		if (nd_cache_io_count() || (q->reader_active && q->reader_active()))
			error = "nd_cache_busy";
	}
	if (!error && navidrome_playback_begin(&q->context)) {
		if (!scan(q->library.path, q->clear, &result, &q->context))
			error = result.ready ? "nd_cache_partial" : "nd_cache_storage";
		navidrome_cache_end();
	} else if (!error)
		error = "nd_cache_storage";
	if (!navidrome_playback_current(&q->context, false)) {
		result.ready = false;
		error = "nd_cache_storage";
	}
	if (q->clear) {
		nd_cache_maintenance_end();
		navidrome_external_end();
	}
	pthread_mutex_lock(&usage_lock);
	result.revision = saved.revision + 1;
	result.error = error;
	result.clearing = false;
	saved = result;
	pthread_mutex_unlock(&usage_lock);
	nd_wipe(&q->context, sizeof(q->context));
	free(q);
	gui_post_signal(usage_notice, NULL);
	return NULL;
}
static bool request(const nd_library_t *lib, bool clear, bool (*reader_active)(void)) {
	request_t *q = calloc(1, sizeof(*q));
	if (!q)
		return false;
	q->library = *lib;
	q->clear = clear;
	q->reader_active = reader_active;
	nd_library_t published;
	navidrome_library_snapshot(&published);
	if (lib->card_epoch != published.card_epoch || strcmp(lib->path, published.path)) {
		free(q);
		return false;
	}
	if (!*lib->path || !navidrome_context(&q->context, false) || q->context.card_epoch != lib->card_epoch || (clear && !lib->writable)) {
		nd_wipe(&q->context, sizeof(q->context));
		free(q);
		return false;
	}
	nd_wipe(q->context.password, sizeof(q->context.password));
	pthread_mutex_lock(&usage_lock);
	bool ok = !saved.busy;
	if (ok && clear)
		ok = navidrome_external_begin(&q->context, false);
	if (ok && clear && !nd_cache_maintenance_begin()) {
		navidrome_external_end();
		ok = false;
	}
	if (ok) {
		saved.busy = true;
		saved.clearing = clear;
		pthread_t thread;
		if (pthread_create(&thread, NULL, cache_worker, q)) {
			saved.busy = saved.clearing = false;
			if (clear) {
				nd_cache_maintenance_end();
				navidrome_external_end();
			}
			ok = false;
		} else
			pthread_detach(thread);
	}
	pthread_mutex_unlock(&usage_lock);
	if (!ok) {
		nd_wipe(&q->context, sizeof(q->context));
		free(q);
	}
	return ok;
}
bool nd_cache_usage_request(const nd_library_t *library) { return request(library, false, NULL); }
bool nd_cache_clear_request(const nd_library_t *library, bool (*reader_active)(void)) { return request(library, true, reader_active); }
void nd_cache_usage_snapshot(nd_cache_usage_t *usage) {
	pthread_mutex_lock(&usage_lock);
	*usage = saved;
	pthread_mutex_unlock(&usage_lock);
}

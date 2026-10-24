#include "navidromeart.h"
#include "src/system/core/md5.h"
#include "src/system/core/utils.h"
#include "src/system/image/stb_image_decl.h"
#include "src/system/streaming/navidrome.h"
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#define ART_BUDGET (32 * 1024 * 1024)
typedef struct {
	unsigned serial;
	bool pending, done, found;
	nd_library_t library;
	char id[ND_ID_MAX];
	int size;
	cover_image_t image;
} art_slot_t;
static art_slot_t slots[ND_WINDOW];
static pthread_mutex_t art_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static bool started;

// One worker serializes fetches. Duplicate keys hit the newly completed file;
// failures share a bounded cooldown table, including when rows are recycled.
static struct {
	char key[33];
	time_t until;
} failures[ND_WINDOW];
static unsigned failed_next;

static bool cache_name(const char *name) { return strlen(name) == 36 && strspn(name, "0123456789abcdef") == 32 && !strcmp(name + 32, ".img"); }

static bool trim(const char *dir, size_t incoming) {
	for (;;) {
		DIR *d = opendir(dir);
		if (!d)
			return false;
		size_t used = 0;
		time_t oldest = 0;
		char victim[1300] = "";
		struct dirent *entry;
		while ((entry = readdir(d))) {
			if (!cache_name(entry->d_name))
				continue;
			char path[1300];
			struct stat st;
			if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >= (int)sizeof(path) || lstat(path, &st) || !S_ISREG(st.st_mode))
				continue;
			used += st.st_size;
			if (!*victim || st.st_mtime < oldest) {
				oldest = st.st_mtime;
				snprintf(victim, sizeof(victim), "%s", path);
			}
		}
		closedir(d);
		if (used + incoming <= ART_BUDGET)
			return true;
		if (!*victim || unlink(victim))
			return false;
	}
}

static bool load(const art_slot_t *job, cover_image_t *out) {
	nd_context_t context;
	if (!navidrome_context(&context, false))
		return false;
	bool valid = context.epoch == job->library.epoch && context.card_epoch == job->library.card_epoch;
	char dir[1100], path[1200], key[33], input[ND_ID_MAX + 100];
	snprintf(dir, sizeof(dir), "%s", job->library.path);
	char *slash = strrchr(dir, '/');
	if (!valid || !slash) {
		nd_wipe(&context, sizeof(context));
		return false;
	}
	strcpy(slash, "/art");
	snprintf(input, sizeof(input), "%s:%zu:%s:%d:v1", job->library.scope, strlen(job->id), job->id, job->size);
	md5_hex(input, strlen(input), key);
	snprintf(path, sizeof(path), "%s/%s.img", dir, key);
	bool found = false;
	if (navidrome_cache_begin(context.epoch, context.card_epoch)) {
		int fd = open(path, O_RDONLY | O_NOFOLLOW);
		struct stat st;
		char *cached = NULL;
		size_t length = 0;
		if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= 256 * 1024) {
			cached = malloc((size_t)st.st_size);
			while (cached && length < (size_t)st.st_size) {
				ssize_t n = read(fd, cached + length, (size_t)st.st_size - length);
				if (n <= 0)
					break;
				length += n;
			}
			if (length != (size_t)st.st_size) {
				free(cached);
				cached = NULL;
			}
		}
		if (fd >= 0)
			close(fd);
		if (cached && job->library.writable)
			utime(path, NULL);
		navidrome_cache_end();
		int w = 0, h = 0, components = 0;
		found = cached && stbi_info_from_memory((const stbi_uc *)cached, (int)length, &w, &h, &components) && w > 0 && h > 0 && w <= 512 && h <= 512 && cover_load_image_memory(cached, length, job->size, job->size, COVER_FIT_COVER, out);
		free(cached);
	}
	if (found) {
		valid = navidrome_context_current(&context, false);
		nd_wipe(&context, sizeof(context));
		if (!valid)
			cover_free(out);
		return valid;
	}
	bool cooldown = false;
	for (int i = 0; i < ND_WINDOW; i++)
		if (!strcmp(failures[i].key, key) && failures[i].until > time(NULL))
			cooldown = true;
	if (!cooldown && job->library.writable && navidrome_context_current(&context, true)) {
		char *body = NULL;
		size_t length = 0;
		const char *error = nd_api_artwork(context.host, context.user, context.password, job->id, job->size, &body, &length);
		int w = 0, h = 0, components = 0;
		if (!error && stbi_info_from_memory((const stbi_uc *)body, (int)length, &w, &h, &components) && w > 0 && h > 0 && w <= 512 && h <= 512 && navidrome_context_current(&context, true) && cover_load_image_memory(body, length, job->size, job->size, COVER_FIT_COVER, out)) {
			bool leased = navidrome_cache_begin(context.epoch, context.card_epoch);
			if (leased)
				mkdir(dir, 0700);
			if (leased && trim(dir, length) && navidrome_context_current(&context, true)) {
				char temp[1220];
				snprintf(temp, sizeof(temp), "%s.tmp", path);
				int fd = open(temp, O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW, 0600);
				bool ok = fd >= 0;
				size_t done = 0;
				while (ok && done < length) {
					ssize_t n = write(fd, body + done, length - done);
					if (n <= 0)
						ok = false;
					else
						done += n;
				}
				if (fd >= 0 && close(fd))
					ok = false;
				if (ok)
					navidrome_context_publish(&context, temp, path);
				unlink(temp);
			}
			if (leased)
				navidrome_cache_end();
			found = navidrome_context_current(&context, true);
			if (!found)
				cover_free(out);
		}
		if (body)
			nd_wipe(body, length);
		free(body);
	}
	nd_wipe(&context, sizeof(context));
	if (!found) {
		unsigned slot = failed_next++ % ND_WINDOW;
		snprintf(failures[slot].key, sizeof(failures[slot].key), "%s", key);
		failures[slot].until = time(NULL) + 30;
	}
	return found;
}

static void *art_worker(void *unused) {
	(void)unused;
	thread_be_background("navidrome art");
	pthread_mutex_lock(&art_lock);
	for (;;) {
		int i;
		for (i = 0; i < ND_WINDOW && !slots[i].pending; i++) {
		}
		if (i == ND_WINDOW) {
			pthread_cond_wait(&wake, &art_lock);
			continue;
		}
		art_slot_t job = slots[i];
		slots[i].pending = false;
		pthread_mutex_unlock(&art_lock);
		cover_image_t image = {0};
		bool found = load(&job, &image);
		pthread_mutex_lock(&art_lock);
		if (job.serial != slots[i].serial) {
			if (found)
				cover_free(&image);
		} else {
			slots[i].image = image;
			slots[i].found = found;
			slots[i].done = true;
		}
	}
	return NULL;
}

void navidromeart_release(int i) {
	if (i < 0 || i >= ND_WINDOW)
		return;
	pthread_mutex_lock(&art_lock);
	art_slot_t *slot = &slots[i];
	slot->serial++;
	slot->pending = slot->done = false;
	if (slot->found)
		cover_free(&slot->image);
	slot->found = false;
	pthread_mutex_unlock(&art_lock);
}

void navidromeart_request(int i, const nd_library_t *library, const char *id, int size) {
	if (i < 0 || i >= ND_WINDOW)
		return;
	navidromeart_release(i);
	pthread_mutex_lock(&art_lock);
	art_slot_t *slot = &slots[i];
	slot->library = *library;
	snprintf(slot->id, sizeof(slot->id), "%s", id);
	slot->size = size;
	slot->pending = id && *id;
	slot->done = !slot->pending;
	if (!started) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, art_worker, NULL) == 0) {
			started = true;
			pthread_detach(thread);
		} else {
			slot->pending = false;
			slot->done = true;
		}
	}
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&art_lock);
}

bool navidromeart_take(int i, cover_image_t *out, bool *finished) {
	pthread_mutex_lock(&art_lock);
	art_slot_t *slot = &slots[i];
	*finished = slot->done;
	bool found = slot->done && slot->found;
	if (found) {
		*out = slot->image;
		memset(&slot->image, 0, sizeof(slot->image));
		slot->found = false;
	}
	pthread_mutex_unlock(&art_lock);
	return found;
}

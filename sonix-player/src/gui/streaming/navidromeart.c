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
	bool pending, done, found, player;
	unsigned profile_epoch;
	nd_library_t library;
	char id[ND_ID_MAX];
	int size;
	int backdrop_w, backdrop_h;
	cover_image_t image, backdrop;
} art_slot_t;
static art_slot_t slots[ND_WINDOW + 1];
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

static void cache_key(const art_slot_t *job, int size, char key[33]) {
	char input[ND_ID_MAX + 100];
	snprintf(input, sizeof(input), "%s:%zu:%s:%d:v1", job->library.scope, strlen(job->id), job->id, size);
	md5_hex(input, strlen(input), key);
}

// Encoded artwork can be decoded at the new display size. Reads stay scoped to
// the same profile/card and keep the existing byte/dimension limits.
static bool cached_image(const art_slot_t *job, const nd_context_t *context, const char *path, cover_image_t *out) {
	bool leased = job->player ? navidrome_playback_begin(context) : navidrome_cache_begin(context->epoch, context->card_epoch);
	if (!leased)
		return false;
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
	bool found = cached && stbi_info_from_memory((const stbi_uc *)cached, (int)length, &w, &h, &components) && w > 0 && h > 0 && w <= 512 && h <= 512 && cover_load_image_memory(cached, length, job->size, job->size, COVER_FIT_COVER, out);
	free(cached);
	return found;
}

static bool load(const art_slot_t *job, cover_image_t *out) {
	nd_context_t context;
	if (!navidrome_context(&context, false))
		return false;
	bool valid = (job->player ? context.profile_epoch == job->profile_epoch : context.epoch == job->library.epoch) && context.card_epoch == job->library.card_epoch;
	char dir[1100], path[1200], key[33];
	snprintf(dir, sizeof(dir), "%s", job->library.path);
	char *slash = strrchr(dir, '/');
	if (!valid || !slash) {
		nd_wipe(&context, sizeof(context));
		return false;
	}
	strcpy(slash, "/art");
	cache_key(job, job->size, key);
	snprintf(path, sizeof(path), "%s/%s.img", dir, key);
	bool found = cached_image(job, &context, path, out);
	if (!found && !job->player) {
		// Finite historical list-thumbnail sizes, largest first. Do not require
		// reconnect/Sync merely because a new layout uses different bounds.
		// Player art keeps its exact-size quality/cache policy.
		static const int sizes[] = {72, 64, 48, 43, 32};
		for (size_t i = 0; !found && i < sizeof(sizes) / sizeof(*sizes); i++) {
			if (sizes[i] == job->size)
				continue;
			char old_key[33], old_path[1200];
			cache_key(job, sizes[i], old_key);
			snprintf(old_path, sizeof(old_path), "%s/%s.img", dir, old_key);
			found = cached_image(job, &context, old_path, out);
		}
	}
	if (found) {
		valid = (job->player ? navidrome_playback_current(&context, false) : navidrome_context_current(&context, false));
		nd_wipe(&context, sizeof(context));
		if (!valid)
			cover_free(out);
		return valid;
	}
	bool cooldown = false;
	for (int i = 0; i < ND_WINDOW; i++)
		if (!strcmp(failures[i].key, key) && failures[i].until > time(NULL))
			cooldown = true;
	if (!cooldown && job->library.writable && (job->player ? navidrome_playback_current(&context, true) : navidrome_context_current(&context, true))) {
		char *body = NULL;
		size_t length = 0;
		const char *error = nd_api_artwork(context.host, context.user, context.password, job->id, job->size, &body, &length);
		int w = 0, h = 0, components = 0;
		if (!error && stbi_info_from_memory((const stbi_uc *)body, (int)length, &w, &h, &components) && w > 0 && h > 0 && w <= 512 && h <= 512 && (job->player ? navidrome_playback_current(&context, true) : navidrome_context_current(&context, true)) && cover_load_image_memory(body, length, job->size, job->size, COVER_FIT_COVER, out)) {
			bool leased = (job->player ? navidrome_playback_begin(&context) : navidrome_cache_begin(context.epoch, context.card_epoch));
			if (leased)
				mkdir(dir, 0700);
			if (leased && trim(dir, length) && (job->player ? navidrome_playback_current(&context, true) : navidrome_context_current(&context, true))) {
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
					job->player ? navidrome_playback_publish(&context, temp, path) : navidrome_context_publish(&context, temp, path);
				unlink(temp);
			}
			if (leased)
				navidrome_cache_end();
			found = (job->player ? navidrome_playback_current(&context, true) : navidrome_context_current(&context, true));
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
		for (i = 0; i < ND_WINDOW + 1 && !slots[i].pending; i++) {
		}
		if (i == ND_WINDOW + 1) {
			pthread_cond_wait(&wake, &art_lock);
			continue;
		}
		art_slot_t job = slots[i];
		slots[i].pending = false;
		pthread_mutex_unlock(&art_lock);
		cover_image_t image = {0}, backdrop = {0};
		bool found = load(&job, &image);
		if (found && job.player && job.backdrop_w > 0 && job.backdrop_w <= 512 && job.backdrop_h > 0 && job.backdrop_h <= 1024)
			cover_blur_fill(&image, job.backdrop_w, job.backdrop_h, &backdrop);
		pthread_mutex_lock(&art_lock);
		if (job.serial != slots[i].serial) {
			if (found)
				cover_free(&image);
			cover_free(&backdrop);
		} else {
			slots[i].image = image;
			slots[i].backdrop = backdrop;
			slots[i].found = found;
			slots[i].done = true;
		}
	}
	return NULL;
}

void navidromeart_release(int i) {
	if (i < 0 || i >= ND_WINDOW + 1)
		return;
	pthread_mutex_lock(&art_lock);
	art_slot_t *slot = &slots[i];
	slot->serial++;
	slot->pending = slot->done = false;
	if (slot->found)
		cover_free(&slot->image);
	cover_free(&slot->backdrop);
	slot->found = false;
	pthread_mutex_unlock(&art_lock);
}

static void request(int i, const nd_library_t *library, const char *id, int size, int backdrop_w, int backdrop_h) {
	if (i < 0 || i >= ND_WINDOW + 1)
		return;
	navidromeart_release(i);
	pthread_mutex_lock(&art_lock);
	art_slot_t *slot = &slots[i];
	slot->library = *library;
	slot->player = i == ND_WINDOW;
	nd_context_t context;
	if (navidrome_context(&context, false))
		slot->profile_epoch = context.profile_epoch;
	nd_wipe(&context, sizeof(context));
	snprintf(slot->id, sizeof(slot->id), "%s", id);
	slot->size = size;
	slot->backdrop_w = backdrop_w;
	slot->backdrop_h = backdrop_h;
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
	if (i < 0 || i > ND_WINDOW) {
		*finished = true;
		return false;
	}
	pthread_mutex_lock(&art_lock);
	art_slot_t *slot = &slots[i];
	nd_context_t context;
	bool current = navidrome_context(&context, false) && context.card_epoch == slot->library.card_epoch && (slot->player ? context.profile_epoch == slot->profile_epoch : context.epoch == slot->library.epoch);
	nd_wipe(&context, sizeof(context));
	if (slot->done && slot->found && !current) {
		cover_free(&slot->image);
		cover_free(&slot->backdrop);
		slot->found = false;
	}
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

void navidromeart_request(int i, const nd_library_t *library, const char *id, int size) { request(i, library, id, size, 0, 0); }
void navidromeart_request_player(const nd_library_t *library, const char *id, int size, int w, int h) { request(ND_ART_PLAYER_SLOT, library, id, size, w, h); }
bool navidromeart_take_player(cover_image_t *out, cover_image_t *backdrop, bool *finished) {
	memset(backdrop, 0, sizeof(*backdrop));
	bool found = navidromeart_take(ND_ART_PLAYER_SLOT, out, finished);
	if (found) {
		pthread_mutex_lock(&art_lock);
		*backdrop = slots[ND_ART_PLAYER_SLOT].backdrop;
		memset(&slots[ND_ART_PLAYER_SLOT].backdrop, 0, sizeof(*backdrop));
		pthread_mutex_unlock(&art_lock);
	}
	return found;
}

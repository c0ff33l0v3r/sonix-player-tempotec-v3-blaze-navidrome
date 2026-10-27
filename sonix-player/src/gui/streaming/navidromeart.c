#include "navidromeart.h"
#include "src/system/core/md5.h"
#include "src/system/core/utils.h"
#include "src/system/image/stb_image_decl.h"
#include "src/system/streaming/navidrome.h"
#include "src/system/streaming/navidromecache.h"
#include "src/system/streaming/navidromedebug.h"
#include "src/system/streaming/streamturn.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#define ART_BUDGET ND_ART_BUDGET
typedef struct {
	unsigned serial;
	bool pending, done, found, player, playback, album;
	bool audio_retried;
	unsigned profile_epoch, maintenance_epoch;
	nd_library_t library;
	char id[ND_ID_MAX];
	int size;
	int backdrop_w, backdrop_h;
	cover_image_t image, backdrop;
} art_slot_t;
#define ART_SLOTS ND_ART_SLOT_COUNT // Browser/player/Search/album heads/Queue/Downloads own disjoint slots.
static art_slot_t slots[ART_SLOTS];
static pthread_mutex_t art_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static bool started;

// One worker serializes fetches. Duplicate keys hit the newly completed file;
// failures share a bounded cooldown table, including when rows are recycled.
static struct {
	char key[33];
	time_t until;
	unsigned network_epoch;
} failures[ND_WINDOW];
static unsigned failed_next;

static bool cache_name(const char *name) { return strlen(name) == 36 && strspn(name, "0123456789abcdef") == 32 && !strcmp(name + 32, ".img"); }

static bool trim(const char *dir, const char *destination, size_t incoming) {
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
			if (!strcmp(path, destination))
				continue; // Count replacement once, never evict the old destination.
			if ((uint64_t)st.st_size > ART_BUDGET || used > ART_BUDGET - (size_t)st.st_size)
				used = ART_BUDGET + 1;
			else
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

#define ART_BASE 512
#define ART_EDGE 2048
#define ART_BYTES (2u * 1024u * 1024u)
#define ART_HEADER 32
// The envelope is explicitly little endian, independent of compiler/target ABI.
enum { ART_PENDING, ART_BASELINE, ART_SMALL, ART_CAPPED };
typedef struct {
	char *bytes;
	size_t length;
	int w, h, state, next;
} art_source_t;
// Worker-owned, single-entry encoded cache also coalesces reads when publication
// fails (e.g. full storage). No consumer ever references this buffer directly.
static art_source_t hot_source;
static char hot_path[1200];
static unsigned hot_profile, hot_card, hot_maintenance;

static void source_free(art_source_t *source) {
	free(source->bytes);
	memset(source, 0, sizeof(*source));
}

static void cache_key(const art_slot_t *job, int size, char key[33]) {
	char input[ND_ID_MAX + 100];
	if (size)
		snprintf(input, sizeof(input), "%s:%zu:%s:%d:v1", job->library.scope, strlen(job->id), job->id, size);
	else
		snprintf(input, sizeof(input), "%s:%zu:%s:source:v2", job->library.scope, strlen(job->id), job->id);
	md5_hex(input, strlen(input), key);
}

static bool job_current(const art_slot_t *job, const nd_context_t *context, bool online) {
	if (!nd_cache_io_current(job->maintenance_epoch) || !(job->playback ? navidrome_playback_current(context, online) : navidrome_context_current(context, online)))
		return false;
	bool live = false;
	pthread_mutex_lock(&art_lock);
	for (int i = 0; !live && i < ART_SLOTS; i++) {
		const art_slot_t *slot = &slots[i];
		// A same-source peer may still need the acquisition after its initiator
		// unloads. Never let an unrelated rebind keep retired work alive.
		live = slot->id[0] && !strcmp(slot->id, job->id) && !strcmp(slot->library.scope, job->library.scope) && slot->profile_epoch == job->profile_epoch && slot->library.card_epoch == job->library.card_epoch && slot->maintenance_epoch == job->maintenance_epoch;
	}
	pthread_mutex_unlock(&art_lock);
	return live;
}

typedef struct {
	const art_slot_t *job;
	const nd_context_t *context;
	bool audio_interrupted;
} art_request_t;
static bool request_current(void *arg) {
	art_request_t *request = arg;
	bool idle;
	streamturn_observe(&idle);
	request->audio_interrupted |= !idle;
	return idle && job_current(request->job, request->context, true);
}

static bool source_info(art_source_t *source, bool legacy) {
	int channels;
	const unsigned char *p = (const unsigned char *)source->bytes;
	// Limit formats before stb's generic header parser and production decode.
	bool signature = source->length >= 8 && ((p[0] == 0xff && p[1] == 0xd8) || !memcmp(p, "\x89PNG\r\n\x1a\n", 8));
	int edge = legacy ? 512 : ART_EDGE;
	return signature && source->length <= (legacy ? 256u * 1024u : ART_BYTES) && stbi_info_from_memory(p, (int)source->length, &source->w, &source->h, &channels) && source->w > 0 && source->h > 0 && source->w <= edge && source->h <= edge && (uint64_t)source->w * source->h <= (uint64_t)ART_EDGE * ART_EDGE;
}

static unsigned get_u32(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24; }
static void put_u32(unsigned char *p, unsigned n) {
	for (int i = 0; i < 4; i++)
		p[i] = (unsigned char)(n >> (8 * i));
}

static bool source_read(const art_slot_t *job, const nd_context_t *context, const char *path, bool legacy, art_source_t *source) {
	bool lease = job->playback ? navidrome_playback_begin(context) : navidrome_cache_begin(context->epoch, context->card_epoch);
	if (!lease)
		return false;
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	struct stat st;
	unsigned char header[ART_HEADER] = {0};
	bool ok = fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= (legacy ? 256 * 1024 : ART_BYTES + ART_HEADER);
	if (ok && !legacy) {
		ok = read(fd, header, sizeof(header)) == sizeof(header) && !memcmp(header, "NDART2\0\0", 8) && get_u32(header + 8) == (unsigned)(st.st_size - ART_HEADER) && get_u32(header + 20) <= ART_CAPPED && get_u32(header + 24) >= ART_BASE && get_u32(header + 24) <= ART_EDGE && get_u32(header + 28) == 0;
	}
	if (ok) {
		source->length = (size_t)st.st_size - (legacy ? 0 : ART_HEADER);
		source->bytes = malloc(source->length);
		ok = source->bytes != NULL;
		for (size_t done = 0; ok && done < source->length;) {
			ssize_t n = read(fd, source->bytes + done, source->length - done);
			if (n <= 0)
				ok = false;
			else
				done += n;
		}
	}
	if (fd >= 0)
		close(fd);
	if (ok && job->library.writable)
		utime(path, NULL);
	navidrome_cache_end();
	ok = ok && source_info(source, legacy);
	if (ok && !legacy) {
		source->state = (int)get_u32(header + 20);
		source->next = (int)get_u32(header + 24);
		ok = get_u32(header + 12) == (unsigned)source->w && get_u32(header + 16) == (unsigned)source->h && (source->state != ART_BASELINE || LV_MIN(source->w, source->h) >= ART_BASE);
	}
	if (!ok)
		source_free(source);
	return ok;
}

static bool source_decode(const art_slot_t *job, const nd_context_t *context, const art_source_t *source, cover_image_t *out) {
	bool idle;
	do {
		streamturn_observe(&idle);
		if (!job_current(job, context, false))
			return false;
		if (!idle)
			usleep(10000);
	} while (!idle);
	return cover_load_image_memory(source->bytes, source->length, job->size, job->size, COVER_FIT_COVER, out);
}

static bool write_all(int fd, const void *data, size_t length) {
	const char *p = data;
	while (length) {
		ssize_t n = write(fd, p, length);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return false;
		p += n;
		length -= n;
	}
	return true;
}

static bool source_publish(const art_slot_t *job, const nd_context_t *context, char *dir, const char *path, const art_source_t *source) {
	if (!job->library.writable || !job_current(job, context, false))
		return false;
	bool lease = job->playback ? navidrome_playback_begin(context) : navidrome_cache_begin(context->epoch, context->card_epoch);
	if (!lease)
		return false;
	bool ok = true;
	// Match existing cache roots, but never follow a symlink component.
	for (char *p = dir + 1;; p++) {
		if (*p && *p != '/')
			continue;
		char saved = *p;
		*p = 0;
		struct stat st;
		if (mkdir(dir, 0700) && errno != EEXIST)
			ok = false;
		if (lstat(dir, &st) || !S_ISDIR(st.st_mode))
			ok = false;
		*p = saved;
		if (!saved || !ok)
			break;
	}
	char temp[1240];
	snprintf(temp, sizeof(temp), "%s.tmp", path);
	unsigned char header[ART_HEADER] = "NDART2\0\0";
	put_u32(header + 8, (unsigned)source->length);
	put_u32(header + 12, source->w);
	put_u32(header + 16, source->h);
	put_u32(header + 20, source->state);
	put_u32(header + 24, source->next);
	ok = ok && trim(dir, path, source->length + ART_HEADER);
	int fd = ok ? open(temp, O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600) : -1;
	ok = fd >= 0 && write_all(fd, header, sizeof(header)) && write_all(fd, source->bytes, source->length);
	if (fd >= 0 && close(fd))
		ok = false;
	if (ok && job_current(job, context, false))
		ok = job->playback ? navidrome_playback_publish(context, temp, path) : navidrome_context_publish(context, temp, path);
	else
		ok = false;
	if (fd >= 0)
		unlink(temp);
	navidrome_cache_end();
	return ok;
}

static void prune_legacy(const art_slot_t *job, const nd_context_t *context, const char *dir) {
	if (!job_current(job, context, false))
		return;
	bool lease = job->playback ? navidrome_playback_begin(context) : navidrome_cache_begin(context->epoch, context->card_epoch);
	if (!lease)
		return;
	static const int sizes[] = {512, 480, 320, 300, 240, 200, 72, 64, 48, 43, 37, 32};
	for (size_t i = 0; i <= sizeof(sizes) / sizeof(*sizes) && job_current(job, context, false); i++) {
		int size = i == sizeof(sizes) / sizeof(*sizes) ? job->size : sizes[i];
		char key[33], path[1200];
		struct stat st;
		cache_key(job, size, key);
		snprintf(path, sizeof(path), "%s/%s.img", dir, key);
		if (!lstat(path, &st) && S_ISREG(st.st_mode))
			unlink(path);
	}
	navidrome_cache_end();
}

static bool load(const art_slot_t *job, cover_image_t *out, bool *audio_interrupted) {
	nd_context_t context;
	if (!navidrome_context(&context, false))
		return false;
	char dir[1100], path[1200], key[33];
	snprintf(dir, sizeof(dir), "%s", job->library.path);
	char *slash = strrchr(dir, '/');
	bool valid = context.profile_epoch == job->profile_epoch && (job->playback || context.epoch == job->library.epoch) && context.card_epoch == job->library.card_epoch && job_current(job, &context, false);
	if (!valid || !slash) {
		nd_wipe(&context, sizeof(context));
		return false;
	}
	strcpy(slash, "/art");
	cache_key(job, 0, key);
	snprintf(path, sizeof(path), "%s/%s.img", dir, key);
	art_source_t source = {0};
	bool canonical = false, dirty = false;
	if (!strcmp(hot_path, path) && hot_profile == context.profile_epoch && hot_card == context.card_epoch && hot_maintenance == job->maintenance_epoch) {
		source = hot_source;
		memset(&hot_source, 0, sizeof(hot_source));
		canonical = true;
	} else
		source_free(&hot_source);
	if (!source.bytes)
		canonical = source_read(job, &context, path, false, &source);
	bool found = source.bytes && source_decode(job, &context, &source, out);
	if (!found) {
		source_free(&source);
		canonical = false;
	}
	// All consumers can read the best bounded legacy encoding, including the
	// player. The exact old geometry covers Studio sizes outside this finite set.
	static const int sizes[] = {512, 480, 320, 300, 240, 200, 72, 64, 48, 43, 37, 32};
	for (size_t i = 0; !canonical && i <= sizeof(sizes) / sizeof(*sizes); i++) {
		int size = i == sizeof(sizes) / sizeof(*sizes) ? job->size : sizes[i];
		char old_key[33], old_path[1200];
		cache_key(job, size, old_key);
		snprintf(old_path, sizeof(old_path), "%s/%s.img", dir, old_key);
		art_source_t candidate = {0};
		cover_image_t image = {0};
		if (source_read(job, &context, old_path, true, &candidate) && LV_MIN(candidate.w, candidate.h) > LV_MIN(source.w, source.h) && source_decode(job, &context, &candidate, &image)) {
			source_free(&source);
			cover_free(out);
			source = candidate;
			memset(&candidate, 0, sizeof(candidate));
			*out = image;
			found = true;
		}
		source_free(&candidate);
	}
	if (found && !canonical) {
		source.state = LV_MIN(source.w, source.h) >= ART_BASE ? ART_BASELINE : ART_PENDING;
		source.next = ART_BASE;
		dirty = source.state == ART_BASELINE;
	}
	bool cooldown = false, attempted = false, failed = false;
	for (int i = 0; i < ND_WINDOW; i++)
		if (!strcmp(failures[i].key, key) && failures[i].network_epoch == context.network_epoch && failures[i].until > time(NULL))
			cooldown = true;
	bool online = job_current(job, &context, true);
	if ((!found || source.state == ART_PENDING) && !cooldown && job->library.writable && online) {
		// Legacy fallbacks still start at 512. A persisted pending response resumes
		// at its one computed follow-up, avoiding a second initial download.
		int pixels = canonical && source.next > ART_BASE ? source.next : ART_BASE;
		int attempts = pixels == ART_BASE ? 2 : 1;
		for (int step = 0; step < attempts && job_current(job, &context, true); step++) {
			bool idle;
			do {
				streamturn_observe(&idle);
				if (!job_current(job, &context, true))
					break;
				if (!idle)
					usleep(10000);
			} while (!idle);
			if (!job_current(job, &context, true))
				break;
			art_source_t candidate = {0};
			cover_image_t image = {0};
			art_request_t request = {.job = job, .context = &context};
			attempted = true;
			const char *error = nd_api_artwork_source(context.host, context.user, context.password, job->id, pixels, request_current, &request, &candidate.bytes, &candidate.length);
			bool accepted = !error && source_info(&candidate, false) && job_current(job, &context, true) && source_decode(job, &context, &candidate, &image);
			if (!accepted) {
				source_free(&candidate);
				cover_free(&image);
				// Audio preemption and retired ownership are cancellations, not
				// server failures. Later visible demand must be able to retry.
				failed = !request.audio_interrupted && job_current(job, &context, true);
				if (audio_interrupted && request.audio_interrupted && job_current(job, &context, false))
					*audio_interrupted = true;
				nd_log("navidrome: artwork unavailable target_px=%d reason=%s\n", job->size,
					request.audio_interrupted ? "audio_priority" : !failed ? "retired" : error ? "request" : "image");
				break;
			}
			int shorter = LV_MIN(candidate.w, candidate.h), longer = LV_MAX(candidate.w, candidate.h);
			int state = shorter >= ART_BASE ? ART_BASELINE : candidate.w == candidate.h || step + 1 == attempts ? (pixels == ART_EDGE ? ART_CAPPED : ART_SMALL) : ART_PENDING;
			int next = LV_MIN(ART_EDGE, (int)(((uint64_t)ART_BASE * longer + shorter - 1) / shorter));
			if (!found || shorter >= LV_MIN(source.w, source.h)) {
				source_free(&source);
				cover_free(out);
				source = candidate;
				memset(&candidate, 0, sizeof(candidate));
				*out = image;
				memset(&image, 0, sizeof(image));
				found = true;
			}
			source.state = state;
			source.next = next;
			dirty = true;
			source_free(&candidate);
			cover_free(&image);
			if (state != ART_PENDING)
				break;
			pixels = next;
		}
	}
	if (attempted && failed) {
		unsigned i = failed_next++ % ND_WINDOW;
		snprintf(failures[i].key, sizeof(failures[i].key), "%s", key);
		failures[i].until = time(NULL) + 30;
		failures[i].network_epoch = context.network_epoch;
	}
	valid = job_current(job, &context, false);
	if (found && valid) {
		if (dirty && online && source_publish(job, &context, dir, path, &source) && source.state != ART_PENDING)
			prune_legacy(job, &context, dir);
		nd_log("navidrome: artwork target_px=%d source_w=%d source_h=%d state=%d attempted=%d\n", job->size, source.w, source.h, source.state, attempted);
		hot_source = source;
		memset(&source, 0, sizeof(source));
		snprintf(hot_path, sizeof(hot_path), "%s", path);
		hot_profile = context.profile_epoch;
		hot_card = context.card_epoch;
		hot_maintenance = job->maintenance_epoch;
	} else {
		cover_free(out);
		found = false;
	}
	source_free(&source);
	nd_wipe(&context, sizeof(context));
	return found;
}

static void *art_worker(void *unused) {
	(void)unused;
	thread_be_background("navidrome art");
	pthread_mutex_lock(&art_lock);
	for (;;) {
		int i;
		for (i = 0; i < ART_SLOTS && !slots[i].pending; i++) {
		}
		if (i == ART_SLOTS) {
			pthread_cond_wait(&wake, &art_lock);
			continue;
		}
		art_slot_t job = slots[i];
		slots[i].pending = false;
		pthread_mutex_unlock(&art_lock);
		cover_image_t image = {0}, backdrop = {0};
		bool admitted = nd_cache_io_begin(job.maintenance_epoch);
		bool audio_interrupted = false;
		bool found = admitted && load(&job, &image, &audio_interrupted);
		if (admitted)
			nd_cache_io_end();
		if (found && job.album) {
			int luma;
			image.has_tone = cover_page_tone(&image, COVER_PAGE_LIGHT_FROM, &image.tone, &luma, &image.tone_light);
			image.tone_luma = image.has_tone ? luma : 0;
		}
		if (found && job.player && job.backdrop_w > 0 && job.backdrop_w <= 512 && job.backdrop_h > 0 && job.backdrop_h <= 1024)
			cover_blur_fill(&image, job.backdrop_w, job.backdrop_h, &backdrop);
		pthread_mutex_lock(&art_lock);
		if (job.serial != slots[i].serial || !nd_cache_io_current(job.maintenance_epoch)) {
			if (found)
				cover_free(&image);
			cover_free(&backdrop);
			if (job.serial == slots[i].serial) {
				slots[i].done = true;
				slots[i].found = false;
			}
		} else if (!found && job.player && audio_interrupted && !job.audio_retried) {
			// Resume a current player's canceled miss once after audio yields.
			// load waits for admission; repeated interruptions terminate.
			slots[i].audio_retried = true;
			slots[i].pending = true;
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
	if (i < 0 || i >= ART_SLOTS)
		return;
	pthread_mutex_lock(&art_lock);
	art_slot_t *slot = &slots[i];
	slot->serial++;
	slot->id[0] = 0;
	slot->pending = slot->done = false;
	if (slot->found)
		cover_free(&slot->image);
	cover_free(&slot->backdrop);
	slot->found = false;
	pthread_mutex_unlock(&art_lock);
}

static void request(int i, const nd_library_t *library, const char *id, int size, int backdrop_w, int backdrop_h) {
	if (i < 0 || i >= ART_SLOTS || !library || !id || size < 1 || size > 512)
		return;
	navidromeart_release(i);
	pthread_mutex_lock(&art_lock);
	art_slot_t *slot = &slots[i];
	slot->library = *library;
	slot->maintenance_epoch = nd_cache_epoch();
	slot->player = i == ND_WINDOW;
	slot->playback = slot->player || i >= ND_ART_QUEUE_SLOT;
	slot->album = i == ND_ART_ALBUM_SLOT || i == ND_ART_SEARCH_ALBUM_SLOT || i == ND_ART_DISCOVERY_ALBUM_SLOT;
	nd_context_t context;
	if (navidrome_context(&context, false))
		slot->profile_epoch = context.profile_epoch;
	nd_wipe(&context, sizeof(context));
	snprintf(slot->id, sizeof(slot->id), "%s", id);
	slot->size = size;
	slot->audio_retried = false;
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
	if (i < 0 || i >= ART_SLOTS) {
		*finished = true;
		return false;
	}
	pthread_mutex_lock(&art_lock);
	art_slot_t *slot = &slots[i];
	nd_context_t context;
	bool current = nd_cache_io_current(slot->maintenance_epoch) && navidrome_context(&context, false) && context.card_epoch == slot->library.card_epoch && (slot->playback ? context.profile_epoch == slot->profile_epoch : context.epoch == slot->library.epoch);
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

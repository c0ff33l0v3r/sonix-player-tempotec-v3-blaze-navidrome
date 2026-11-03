#include "navidromeart.h"
#include "src/system/core/diagnostic.h"
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
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#define ART_BUDGET ND_ART_BUDGET
typedef struct {
	diagnostic_t diagnostic;
	bool delivered;
	unsigned serial;
	bool pending, done, found, player, playback, album;
	bool local_only, staged, waiting;
	unsigned age;
	char source_key[33];
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
static bool started, network_started, local_busy, local_candidate;
static atomic_int network_wake = -1;
static void artwork_wake(void) {
	int saved_errno = errno;
	int fd = atomic_load(&network_wake);
	uint64_t value = 1;
	if (fd >= 0) {
		ssize_t n;
		do {
			n = write(fd, &value, sizeof(value));
		} while (n < 0 && errno == EINTR);
		// EAGAIN means a retained wake is already pending.
	}
	errno = saved_errno;
}
static unsigned playback_pressure(bool *idle) { return streamturn_observe_playback ? streamturn_observe_playback(idle) : streamturn_observe(idle); }
static uint64_t art_now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
// This table contains identities/state only. One acquisition or ready handoff
// owns encoded bytes; no per-consumer network buffers or threads.
enum { NET_FREE, NET_WAIT, NET_ACTIVE, NET_READY };
typedef struct {
	char key[33];
	unsigned profile, card, maintenance;
	int state, pixels;
	bool error;
	uint64_t due, order, queued_ms;
	char *bytes;
	size_t length;
} art_network_t;
static art_network_t network[ART_SLOTS];
static uint64_t network_order;
static void *art_network_worker(void *unused);

// The local worker alone owns source validation/publication and cooldowns.
// Preemption remains in the separate network queue and never poisons failures.
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

static bool network_matches(const art_network_t *n, const art_slot_t *s) { return s->id[0] && !s->local_only && !s->done && n->profile == s->profile_epoch && n->card == s->library.card_epoch && n->maintenance == s->maintenance_epoch && !strcmp(n->key, s->source_key); }
static int slot_priority(const art_slot_t *s) { return s->player ? 3 : s->album ? 2 : 1; }
// art_lock held. Prefer a live player peer when the initiator has gone away.
static int network_peer(const art_network_t *n) {
	int best = -1;
	for (int i = 0; i < ART_SLOTS; i++)
		if (network_matches(n, &slots[i]) && (best < 0 || slot_priority(&slots[i]) > slot_priority(&slots[best])))
			best = i;
	return best;
}
static const char *staged_fetch(const art_slot_t *job, int *pixels, char **bytes, size_t *length, bool *deferred) {
	pthread_mutex_lock(&art_lock);
	int empty = -1, match = -1;
	for (int i = 0; i < ART_SLOTS; i++) {
		if (!network[i].state)
			empty = i;
		else if (network_matches(&network[i], job)) {
			match = i;
			break;
		}
	}
	if (match >= 0 && network[match].state == NET_READY) {
		art_network_t *n = &network[match];
		*bytes = n->bytes;
		*length = n->length;
		*pixels = n->pixels;
		bool error = n->error;
		// A cached fallback plus this candidate can own both encoded buffers.
		// Ordinary one-source local reads may overlap acquisition; this handoff
		// temporarily prevents starting another acquisition until local completion.
		local_candidate = true;
		memset(n, 0, sizeof(*n));
		pthread_mutex_unlock(&art_lock);
		artwork_wake();
		return error ? "nd_response_error" : NULL;
	}
	if (match < 0 && empty >= 0 && network_started) {
		art_network_t *n = &network[empty];
		snprintf(n->key, sizeof(n->key), "%s", job->source_key);
		n->profile = job->profile_epoch;
		n->card = job->library.card_epoch;
		n->maintenance = job->maintenance_epoch;
		n->pixels = *pixels;
		n->state = NET_WAIT;
		n->order = ++network_order;
		n->queued_ms = art_now_ms();
		match = empty;
	}
	*deferred = match >= 0;
	pthread_mutex_unlock(&art_lock);
	artwork_wake();
	return *deferred ? "nd_art_deferred" : "nd_json_memory";
}

typedef struct {
	const art_slot_t *job;
	const nd_context_t *context;
	bool audio_interrupted;
	bool player_preempted;
} art_request_t;
static bool request_current(void *arg) {
	art_request_t *request = arg;
	bool idle;
	playback_pressure(&idle);
	request->audio_interrupted |= !idle;
	bool player = false;
	if (request->job->staged && !request->job->player) {
		pthread_mutex_lock(&art_lock);
		const art_slot_t *p = &slots[ND_ART_PLAYER_SLOT];
		player = p->id[0] && p->waiting && !p->done && !p->local_only && strcmp(p->source_key, request->job->source_key);
		pthread_mutex_unlock(&art_lock);
	}
	request->player_preempted |= player;
	return idle && !player && job_current(request->job, request->context, true);
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
	if (!job_current(job, context, false))
		return false;
	// Cached/local preparation never waits for a network transfer to finish.
	DIAG(job->diagnostic, D_PRIORITY_WAIT, D_OK, DV(D_WAIT_US, 0));
	uint64_t started = diagnostic_clock();
	bool ok = cover_load_image_memory(source->bytes, source->length, job->size, job->size, COVER_FIT_COVER, out);
	DIAG(job->diagnostic, D_DECODE, ok ? D_OK : D_FAILED, DV(D_DURATION_US, diagnostic_since(started)), DV(D_WIDTH, source->w), DV(D_HEIGHT, source->h));
	return ok;
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
	DIAGNOSTIC_SCOPE(D_ART);
	if (!job->library.writable || !job_current(job, context, false))
		DIAGNOSTIC_RETURN_BOOL(false);
	bool lease = job->playback ? navidrome_playback_begin(context) : navidrome_cache_begin(context->epoch, context->card_epoch);
	if (!lease)
		DIAGNOSTIC_RETURN_BOOL(false);
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
	DIAGNOSTIC_RETURN_BOOL(ok);
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
	DIAGNOSTIC_SCOPE(D_ART);
	nd_context_t context;
	if (!navidrome_context(&context, false))
		DIAGNOSTIC_RETURN_BOOL(false);
	char dir[1100], path[1200], key[33];
	snprintf(dir, sizeof(dir), "%s", job->library.path);
	char *slash = strrchr(dir, '/');
	bool valid = context.profile_epoch == job->profile_epoch && (job->playback || context.epoch == job->library.epoch) && context.card_epoch == job->library.card_epoch && job_current(job, &context, false);
	if (!valid || !slash) {
		nd_wipe(&context, sizeof(context));
		DIAGNOSTIC_RETURN_BOOL(false);
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
	DIAG(_diagnostic_scope.context, found ? D_CACHE_HIT : D_CACHE_MISS, found ? D_OK : D_PENDING);
	if (!found) {
		source_free(&source);
		canonical = false;
	}
	// All consumers can read the best bounded legacy encoding, including the
	// player. The exact old geometry covers Studio sizes outside this finite set.
	static const int sizes[] = {512, 480, 320, 300, 240, 200, 72, 64, 48, 43, 37, 32};
	char best_legacy[1200] = "";
	int best_short = 0;
	for (size_t i = 0; !canonical && i <= sizeof(sizes) / sizeof(*sizes); i++) {
		int size = i == sizeof(sizes) / sizeof(*sizes) ? job->size : sizes[i];
		char old_key[33], old_path[1200];
		cache_key(job, size, old_key);
		snprintf(old_path, sizeof(old_path), "%s/%s.img", dir, old_key);
		art_source_t candidate = {0};
		cover_image_t image = {0};
		if (source_read(job, &context, old_path, true, &candidate) && LV_MIN(candidate.w, candidate.h) > best_short && source_decode(job, &context, &candidate, &image)) {
			source_free(&source);
			cover_free(out);
			best_short = LV_MIN(candidate.w, candidate.h);
			snprintf(best_legacy, sizeof(best_legacy), "%s", old_path);
			*out = image;
			found = true;
		}
		source_free(&candidate);
	}
	if (found && !canonical && !source_read(job, &context, best_legacy, true, &source)) {
		cover_free(out);
		found = false;
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
	bool online = !job->local_only && job_current(job, &context, true);
	if ((!found || source.state == ART_PENDING) && !cooldown && job->library.writable && online) {
		// Legacy fallbacks still start at 512. A persisted pending response resumes
		// at its one computed follow-up, avoiding a second initial download.
		int pixels = canonical && source.next > ART_BASE ? source.next : ART_BASE;
		int attempts = pixels == ART_BASE ? 2 : 1;
		for (int step = 0; step < attempts && job_current(job, &context, true); step++) {
			bool idle;
			if (!job->staged)
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
			bool deferred = false;
			const char *error = job->staged ? staged_fetch(job, &pixels, &candidate.bytes, &candidate.length, &deferred) : nd_api_artwork_source(context.host, context.user, context.password, job->id, pixels, request_current, &request, &candidate.bytes, &candidate.length);
			if (deferred) {
				if (audio_interrupted)
					*audio_interrupted = true;
				break;
			}
			bool accepted = !error && source_info(&candidate, false) && job_current(job, &context, true) && source_decode(job, &context, &candidate, &image);
			if (!accepted) {
				source_free(&candidate);
				cover_free(&image);
				// Audio preemption and retired ownership are cancellations, not
				// server failures. Later visible demand must be able to retry.
				failed = !request.audio_interrupted && !(job->staged && error && !strcmp(error, "nd_json_memory")) && job_current(job, &context, true);
				if (audio_interrupted && request.audio_interrupted && job_current(job, &context, false))
					*audio_interrupted = true;
				nd_log("navidrome: artwork unavailable target_px=%d reason=%s\n", job->size, request.audio_interrupted ? "audio_priority" : !failed ? "retired" : error ? "request" : "image");
				break;
			}
			int shorter = LV_MIN(candidate.w, candidate.h), longer = LV_MAX(candidate.w, candidate.h);
			int state = shorter >= ART_BASE ? ART_BASELINE : candidate.w == candidate.h || pixels > ART_BASE ? (pixels == ART_EDGE ? ART_CAPPED : ART_SMALL) : ART_PENDING;
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
	DIAGNOSTIC_RETURN_BOOL(found);
}

// One remote request at a time. Its eventfd wake cannot recurse into a
// streaming/service mutex, and retained counters prevent lost notifications.
static void *art_network_worker(void *unused) {
	(void)unused;
	thread_be_background("navidrome art net");
	unsigned observed = 0;
	uint64_t quiet_until = 0;
	for (;;) {
		bool idle;
		unsigned epoch = playback_pressure(&idle);
		uint64_t now = art_now_ms();
		if (epoch != observed) {
			observed = epoch;
			quiet_until = now + 250;
		}
		pthread_mutex_lock(&art_lock);
		int selected = -1, peer = -1;
		bool ready = false;
		uint64_t next = 0;
		for (int i = 0; i < ART_SLOTS; i++) {
			art_network_t *n = &network[i];
			if (!n->state)
				continue;
			int p = network_peer(n);
			if (p < 0) {
				free(n->bytes);
				memset(n, 0, sizeof(*n));
				continue;
			}
			if (n->state == NET_READY) {
				ready = true;
				continue;
			}
			if (n->state != NET_WAIT)
				continue;
			if (n->due > now) {
				if (!next || n->due < next)
					next = n->due;
				continue;
			}
			if (selected < 0 || slot_priority(&slots[p]) > slot_priority(&slots[peer]) || (slot_priority(&slots[p]) == slot_priority(&slots[peer]) && n->order < network[selected].order)) {
				selected = i;
				peer = p;
			}
		}
		if (selected >= 0 && idle && now >= quiet_until && !local_candidate && !ready) {
			art_slot_t job = slots[peer];
			job.staged = true;
			art_network_t *n = &network[selected];
			int pixels = n->pixels;
			uint64_t waited_ms = art_now_ms() - n->queued_ms;
			n->state = NET_ACTIVE;
			pthread_mutex_unlock(&art_lock);
			DIAGNOSTIC_ATTACH(job.diagnostic);
			DIAG(job.diagnostic, D_PRIORITY_WAIT, D_OK, DV(D_KIND, 1), DV(D_WAIT_US, waited_ms * 1000));
			DIAG(job.diagnostic, D_WORK, D_PENDING, DV(D_KIND, 1));
			nd_context_t context;
			bool admitted = nd_cache_io_begin(job.maintenance_epoch);
			bool current = admitted && navidrome_context(&context, false) && context.profile_epoch == job.profile_epoch && context.card_epoch == job.library.card_epoch && job_current(&job, &context, true);
			char *bytes = NULL;
			size_t length = 0;
			art_request_t request = {.job = &job, .context = &context};
			const char *error = current ? nd_api_artwork_source(context.host, context.user, context.password, job.id, pixels, request_current, &request, &bytes, &length) : "nd_audio_cancelled";
			current = current && job_current(&job, &context, true);
			if (admitted)
				nd_cache_io_end();
			nd_wipe(&context, sizeof(context));
			pthread_mutex_lock(&art_lock);
			n = &network[selected];
			if (network_peer(n) < 0 || !current) {
				// Only this source needs to reconsider its current fallback/eligibility.
				for (int j = 0; j < ART_SLOTS; j++)
					if (slots[j].waiting && network_matches(n, &slots[j])) {
						slots[j].waiting = false;
						slots[j].pending = true;
					}
				free(bytes);
				memset(n, 0, sizeof(*n));
			} else if (request.audio_interrupted || request.player_preempted) {
				free(bytes);
				n->state = NET_WAIT;
				n->due = art_now_ms() + 250;
				n->order = ++network_order;
				DIAG(job.diagnostic, D_RETRY, D_PENDING, DV(D_REASON, request.audio_interrupted ? 1 : 3), DV(D_RETRIES, 1));
			} else {
				n->bytes = bytes;
				n->length = length;
				n->error = error != NULL;
				n->state = NET_READY;
				for (int j = 0; j < ART_SLOTS; j++)
					if (network_matches(n, &slots[j])) {
						slots[j].waiting = false;
						slots[j].pending = true;
					}
			}
			pthread_cond_broadcast(&wake);
			pthread_mutex_unlock(&art_lock);
			continue;
		}
		if (selected >= 0 && idle && quiet_until > now && (!next || quiet_until < next))
			next = quiet_until;
		pthread_mutex_unlock(&art_lock);
		int timeout = next ? (int)LV_MIN(next > now ? next - now : 1, 2147483647) : -1;
		// Standalone legacy fixtures can omit the optional observer implementation.
		if (!streamturn_set_artwork_observer && timeout < 0)
			timeout = 100;
		struct pollfd fd = {.fd = atomic_load(&network_wake), .events = POLLIN};
		int result = poll(&fd, 1, timeout);
		if (result > 0) {
			uint64_t count;
			ssize_t nread = read(fd.fd, &count, sizeof(count));
			(void)nread;
		}
	}
	return NULL;
}

static void *art_worker(void *unused) {
	(void)unused;
	thread_be_background("navidrome art");
	pthread_mutex_lock(&art_lock);
	for (;;) {
		int i = -1;
		for (int j = 0; j < ART_SLOTS; j++)
			if (slots[j].pending) {
				if (slots[j].age < ART_SLOTS + 8)
					slots[j].age++;
				int score = slots[j].age > 8 ? 4 : slot_priority(&slots[j]);
				int best = i < 0 ? -1 : slots[i].age > 8 ? 4 : slot_priority(&slots[i]);
				if (score > best || (score == best && slots[j].age > slots[i].age))
					i = j;
			}
		if (i < 0) {
			pthread_cond_wait(&wake, &art_lock);
			continue;
		}
		art_slot_t job = slots[i];
		job.staged = true;
		slots[i].age = 0;
		local_busy = true;
		slots[i].pending = false;
		pthread_mutex_unlock(&art_lock);
		DIAGNOSTIC_ATTACH(job.diagnostic);
		DIAG(job.diagnostic, D_WORK, D_PENDING, DV(D_KIND, 0));
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
		local_busy = local_candidate = false;
		pthread_cond_broadcast(&wake);
		artwork_wake();
		if (job.serial != slots[i].serial || !nd_cache_io_current(job.maintenance_epoch)) {
			DIAG(job.diagnostic, D_RETIRED, D_SUPERSEDED);
			if (found)
				cover_free(&image);
			cover_free(&backdrop);
			if (job.serial == slots[i].serial) {
				slots[i].done = true;
				slots[i].found = false;
			}
		} else if (audio_interrupted) {
			// Waiting for an owned source handoff, not a terminal missing image.
			cover_free(&image);
			cover_free(&backdrop);
			slots[i].waiting = true;
			for (int j = 0; j < ART_SLOTS; j++)
				if (network[j].state == NET_READY && network_matches(&network[j], &slots[i])) {
					slots[i].pending = true;
					slots[i].waiting = false;
				}
		} else {
			DIAG(job.diagnostic, D_DECODE, found ? D_OK : D_FAILED, DV(D_REASON, audio_interrupted ? 1 : !admitted ? 2 : 0));
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
	if (slot->diagnostic.id && !slot->delivered)
		DIAG(slot->diagnostic, D_END, D_SUPERSEDED);
	slot->diagnostic = (diagnostic_t){0};
	slot->serial++;
	slot->id[0] = 0;
	slot->pending = slot->done = slot->waiting = false;
	artwork_wake();
	if (slot->found)
		cover_free(&slot->image);
	cover_free(&slot->backdrop);
	slot->found = false;
	pthread_mutex_unlock(&art_lock);
}

// Service shutdown retires every consumer before draining. The two fixed
// workers/eventfd stay dormant for process lifetime (and a later service init),
// but no request, encoded candidate, unclaimed image or cache lease survives.
// Invoked outside the service lock; never from an interactive GUI callback.
static void artwork_shutdown(void) {
	pthread_mutex_lock(&art_lock);
	for (int i = 0; i < ART_SLOTS; i++) {
		slots[i].serial++;
		slots[i].id[0] = 0;
		slots[i].pending = slots[i].waiting = false;
		slots[i].done = true;
		if (slots[i].found)
			cover_free(&slots[i].image);
		cover_free(&slots[i].backdrop);
		slots[i].found = false;
	}
	artwork_wake();
	pthread_cond_broadcast(&wake);
	for (;;) {
		bool busy = local_busy;
		for (int i = 0; i < ART_SLOTS; i++)
			busy |= network[i].state == NET_ACTIVE;
		if (!busy)
			break;
		pthread_cond_wait(&wake, &art_lock);
	}
	for (int i = 0; i < ART_SLOTS; i++) {
		free(network[i].bytes);
		memset(&network[i], 0, sizeof(network[i]));
	}
	source_free(&hot_source);
	hot_path[0] = 0;
	memset(failures, 0, sizeof(failures));
	pthread_mutex_unlock(&art_lock);
}

// Fixed process-lifetime workers, matching existing artwork lifetime. Keep
// stacks bounded on the device; all large image/HTTP storage remains on heap.
#define ART_STACK_BYTES (256u * 1024u)
static bool start_art_thread(void *(*entry)(void *)) {
	pthread_attr_t attr;
	if (pthread_attr_init(&attr))
		return false;
	pthread_t thread;
	bool ok = !pthread_attr_setstacksize(&attr, ART_STACK_BYTES) && !pthread_create(&thread, &attr, entry, NULL);
	pthread_attr_destroy(&attr);
	if (ok)
		pthread_detach(thread);
	return ok;
}

static void request(int i, const nd_library_t *library, const char *id, int size, int backdrop_w, int backdrop_h, bool local_only) {
	if (i < 0 || i >= ART_SLOTS || !library || !id || size < 1 || size > 512)
		return;
	navidromeart_release(i);
	pthread_mutex_lock(&art_lock);
	art_slot_t *slot = &slots[i];
	slot->diagnostic = diagnostic_begin(D_ART, diagnostic_current().id);
	slot->delivered = false;
	DIAG(slot->diagnostic, D_QUEUED, D_PENDING, DV(D_TARGET, i), DV(D_WIDTH, size), DV(D_FLAGS, local_only));
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
	slot->local_only = local_only;
	slot->backdrop_w = backdrop_w;
	slot->backdrop_h = backdrop_h;
	cache_key(slot, 0, slot->source_key);
	slot->age = 0;
	slot->waiting = false;
	slot->pending = id && *id;
	slot->done = !slot->pending;
	if (!network_started) {
		int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (fd >= 0) {
			atomic_store(&network_wake, fd);
			if (start_art_thread(art_network_worker)) {
				network_started = true;
				if (streamturn_set_artwork_observer)
					streamturn_set_artwork_observer(artwork_wake);
			} else {
				atomic_store(&network_wake, -1);
				close(fd);
			}
		}
	}
	if (!started) {
		if (start_art_thread(art_worker)) {
			started = true;
		} else {
			slot->pending = false;
			slot->done = true;
		}
	}
	if (started || network_started)
		navidrome_set_artwork_shutdown(artwork_shutdown);
	pthread_cond_signal(&wake);
	artwork_wake();
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
	if (!current) {
		slot->pending = slot->waiting = false;
		slot->done = true;
		artwork_wake();
	}
	if (slot->done && slot->found && !current) {
		cover_free(&slot->image);
		cover_free(&slot->backdrop);
		slot->found = false;
	}
	if (slot->done && !slot->delivered) {
		DIAG(slot->diagnostic, D_DELIVER, !current ? D_SUPERSEDED : slot->found ? D_OK : D_UNAVAILABLE);
		DIAG(slot->diagnostic, D_END, !current ? D_SUPERSEDED : slot->found ? D_OK : D_UNAVAILABLE);
		slot->delivered = true;
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

void navidromeart_request(int i, const nd_library_t *library, const char *id, int size) { request(i, library, id, size, 0, 0, false); }
void navidromeart_request_player(const nd_library_t *library, const char *id, int size, int w, int h) { request(ND_ART_PLAYER_SLOT, library, id, size, w, h, false); }
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

void navidromeart_request_player_local(const nd_library_t *library, const char *id, int size, int w, int h) { request(ND_ART_PLAYER_SLOT, library, id, size, w, h, true); }

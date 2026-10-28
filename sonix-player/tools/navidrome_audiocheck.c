// Real protected HTTP, SD cache, growfile and production decoders. No output device.
#include "src/system/decode/decode.h"
#include "src/system/decode/growfile.h"
#include "src/system/streaming/navidromeaudio.h"
#include "src/system/streaming/streamturn.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static const char *fault_mode;
static bool fault_used;
static unsigned space_checks;
static bool cache_descriptor(int fd, bool marker) {
	char link[64], path[1200];
	snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
	ssize_t n = readlink(link, path, sizeof(path) - 1);
	if (n < 0)
		return false;
	path[n] = 0;
	return strstr(path, "/audio/") && (marker ? strstr(path, ".tmp") != NULL : strstr(path, ".flac") != NULL);
}
ssize_t __real_write(int, const void *, size_t);
ssize_t __wrap_write(int fd, const void *data, size_t bytes) {
	if (!fault_used && fault_mode && ((!strcmp(fault_mode, "write-fail") && cache_descriptor(fd, false)) || (!strcmp(fault_mode, "marker-fail") && cache_descriptor(fd, true)))) {
		fault_used = true;
		errno = ENOSPC;
		return -1;
	}
	return __real_write(fd, data, bytes);
}
int __real_close(int);
int __wrap_close(int fd) {
	bool fail = !fault_used && fault_mode && !strcmp(fault_mode, "close-fail") && cache_descriptor(fd, false);
	int result = __real_close(fd);
	if (fail) {
		fault_used = true;
		errno = EIO;
		return -1;
	}
	return result;
}
int __real_statvfs(const char *, struct statvfs *);
int __wrap_statvfs(const char *path, struct statvfs *space) {
	int result = __real_statvfs(path, space);
	if (!result && fault_mode && !strcmp(fault_mode, "free-loss") && ++space_checks >= 2) {
		space->f_bavail = 0;
		fault_used = true;
	}
	return result;
}
typedef struct {
	uint32_t version;
	char scope[33], id[ND_ID_MAX], extension[8];
	int64_t bytes;
} fixture_complete_t;
static atomic_bool connected = true, cancelled_test;
static atomic_int leases, lease_calls;
// This standalone fixture supplies the service boundary, including the cache
// lifetime gate introduced by cache maintenance. Clear-cache policy is covered
// by its own fixture; these counters verify balanced transfer I/O ownership.
static atomic_uint fixture_cache_io;
unsigned nd_cache_epoch(void) { return 1; }
bool nd_cache_io_current(unsigned epoch) { return epoch == 1 && !cancelled_test; }
bool nd_cache_io_begin(unsigned epoch) {
    if (!nd_cache_io_current(epoch)) return false;
    atomic_fetch_add(&fixture_cache_io,1);
    return true;
}
void nd_cache_io_end(void) { assert(atomic_fetch_sub(&fixture_cache_io,1)>0); }

static int deliveries;
static bool reject_publish;
static long delivered_bytes;
static char delivered_path[512];
static char saved_permanent[512];
static bool saved_original(const nd_library_t *lib, const nd_context_t *ctx, const char *id, char out[512]) {
	(void)ctx;
	if (strcmp(lib->scope, "0123456789abcdef0123456789abcdef") || strcmp(id, "saved-permanent")) return false;
	snprintf(out, 512, "%s", saved_permanent);
	return true;
}
void nd_log(const char *format, ...) {
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}
const char *tr(const char *key) { return key; }
const char *config_get(const char *section, const char *key, const char *fallback) {
	(void)section;
	(void)key;
	return fallback;
}
long config_get_int(const char *section, const char *key, long fallback) {
	(void)section;
	(void)key;
	return fallback;
}
bool navidrome_playback_current(const nd_context_t *c, bool network) {
	(void)c;
	return !cancelled_test && (!network || connected);
}
bool navidrome_playback_begin(const nd_context_t *c) {
	(void)c;
	if (cancelled_test)
		return false;
	leases++;
	lease_calls++;
	return true;
}
void navidrome_cache_end(void) {
	assert(leases > 0);
	leases--;
}
bool navidrome_playback_publish(const nd_context_t *c, const char *temp, const char *path) {
	(void)c;
	if (reject_publish)
		cancelled_test = true;
	return !cancelled_test && !rename(temp, path);
}
void navidrome_network_failed(const nd_context_t *c) {
	(void)c;
	connected = false;
}
const char *navidrome_validate(const char *host, const char *user, const char *password, char out[ND_HOST_MAX]) {
	assert(user && password);
	snprintf(out, ND_HOST_MAX, "%s", host);
	return NULL;
}
static bool current(void *user) {
	(void)user;
	return !cancelled_test;
}
static void ready(const char *path, void *user) {
	const char *mode = user;
	assert(!leases);
	deliveries++;
	struct stat st;
	assert(!stat(path, &st));
	delivered_bytes = st.st_size;
	snprintf(delivered_path, sizeof(delivered_path), "%s", path);
	if (!strcmp(mode, "cancel") || !strcmp(mode, "network")) {
		if (!strcmp(mode, "network"))
			connected = false;
		else
			cancelled_test = true;
		nd_audio_cancel();
		return;
	}
	decode_format_t format = decode_detect_format(path);
	if (format != DECODE_FORMAT_UNKNOWN) {
		decoder_t *decoder = decoder_open_info(path, format);
		assert(decoder && decoder_sample_rate(decoder) == 44100);
		if (growfile_is_growing(path)) {
			long done, total;
			assert(decoder_is_growing(decoder) && decoder_grow_span(decoder, &done, &total));
			if (total <= 0)
				assert(decoder_seekable_limit(decoder) == 0);
			assert(decoder_seek_to_frame(decoder, 0));
		}
		short pcm[256];
		assert(decoder_read_pcm_frames_s16(decoder, 128, pcm) > 0);
		decoder_close(decoder);
	}
}
typedef struct {
	nd_library_t *library;
	nd_context_t *context;
	nd_playrow_t *track;
	const char *error;
} blocked_fetch_t;
static void *blocked_fetch(void *user) {
	blocked_fetch_t *job = user;
	job->error = nd_audio_fetch(job->library, job->context, job->track, true, current, ready, "detach");
	return NULL;
}
static void no_card_descriptors(const char *root) {
	DIR *dir = opendir("/proc/self/fd");
	assert(dir);
	struct dirent *entry;
	while ((entry = readdir(dir))) {
		char link[512], path[1024];
		assert(snprintf(link, sizeof(link), "/proc/self/fd/%s", entry->d_name) < (int)sizeof(link));
		ssize_t n = readlink(link, path, sizeof(path) - 1);
		if (n < 0)
			continue;
		path[n] = 0;
		assert(strncmp(path, root, strlen(root)));
	}
	closedir(dir);
}
int main(int argc, char **argv) {
	assert(argc == 4);
	fault_mode=argv[3];
	bool storage_fault=strstr(fault_mode,"-fail") || !strcmp(fault_mode,"free-loss");
	nd_library_t lib = {.ready = true, .writable = true};
	strcpy(lib.scope, "0123456789abcdef0123456789abcdef");
	snprintf(lib.path, sizeof(lib.path), "%s/.local/navidrome/.cache/library.db", argv[2]);
	nd_context_t ctx = {0};
	snprintf(ctx.host, sizeof(ctx.host), "%s", argv[1]);
	strcpy(ctx.user, "fixture user");
	strcpy(ctx.password, "synthetic-server-password");
	nd_playrow_t track = {0};
	strcpy(track.row.id, "opaque:/track ?+");
	track.row.duration = 20;
	char sparse[512] = "";
	if (!strcmp(argv[3], "protected") || !strcmp(argv[3], "evict")) {
		char audio[512], scope[512];
		assert(snprintf(audio, sizeof(audio), "%s/.local/navidrome/.cache/audio", argv[2]) < (int)sizeof(audio));
		assert(!mkdir(audio, 0700));
		assert(snprintf(scope, sizeof(scope), "%s/%s", audio, lib.scope) < (int)sizeof(scope));
		assert(!mkdir(scope, 0700));
		assert(snprintf(sparse, sizeof(sparse), "%s/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.flac", scope) < (int)sizeof(sparse));
		int fd = open(sparse, O_CREAT | O_WRONLY, 0600);
		assert(fd >= 0);
		assert(!ftruncate(fd, 1024LL * 1024 * 1024));
		close(fd);
		char complete[512];
		snprintf(complete, sizeof(complete), "%s", sparse);
		strcpy(strrchr(complete, '.'), ".complete");
		fixture_complete_t record = {.version = 1, .bytes = 1024LL * 1024 * 1024};
		strcpy(record.scope, lib.scope);
		strcpy(record.id, "old fixture");
		strcpy(record.extension, ".flac");
		fd = open(complete, O_CREAT | O_WRONLY, 0600);
		assert(fd >= 0);
		assert(write(fd, &record, sizeof(record)) == sizeof(record));
		close(fd);
		char orphan[512];
		assert(snprintf(orphan, sizeof(orphan), "%s/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.tmp", scope) < (int)sizeof(orphan));
		fd = open(orphan, O_CREAT | O_WRONLY, 0600);
		assert(fd >= 0);
		close(fd);
		if (!strcmp(argv[3], "protected")) {
			const char *paths[3] = {sparse, NULL, NULL};
			nd_audio_protect(paths);
		}
	}
	if (!strcmp(argv[3], "detach") || !strcmp(argv[3], "switch") || !strcmp(argv[3], "turn")) {
		bool waiting_turn = !strcmp(argv[3], "turn");
		if (waiting_turn)
			streamturn_acquire();
		blocked_fetch_t job = {.library = &lib, .context = &ctx, .track = &track};
		pthread_t thread;
		assert(!pthread_create(&thread, NULL, blocked_fetch, &job));
		for (int i = 0; atomic_load(&lease_calls) < (waiting_turn ? 1 : 2) && i < 10000; i++)
			usleep(1000);
		assert(atomic_load(&lease_calls) >= (waiting_turn ? 1 : 2));
		usleep(100000);
		assert(!leases);
		no_card_descriptors(argv[2]);
		if (!strcmp(argv[3], "detach"))
			cancelled_test = true;
		nd_audio_cancel();
		pthread_join(thread, NULL);
		if (waiting_turn)
			streamturn_release();
		assert(job.error && !strcmp(job.error, "nd_audio_cancelled") && !leases && !deliveries);
		no_card_descriptors(argv[2]);
		puts("blocked network holds no card descriptor or lease; invalidation wakes and drains writer: passed");
		assert(!atomic_load(&fixture_cache_io));
		return 0;
	}
	reject_publish = !strcmp(argv[3], "publish");
	const char *error = nd_audio_fetch(&lib, &ctx, &track, !storage_fault && !reject_publish && strcmp(argv[3], "prefetch"), current, ready, argv[3]);
	assert(!leases);
	if (storage_fault) {
		assert(fault_used && error && !strcmp(error,!strcmp(fault_mode,"free-loss") ? "nd_audio_space" : "nd_cache_storage") && !deliveries);
		assert(!nd_audio_cached(&lib,&ctx,track.row.id));
	} else if (!strcmp(argv[3], "invalid"))
		assert(error && !strcmp(error, "nd_audio_invalid") && !deliveries);
	else if (!strcmp(argv[3], "publish"))
		assert(error && !strcmp(error, "nd_cache_storage") && !deliveries);
	else if (!strcmp(argv[3], "cut") || !strcmp(argv[3], "network"))
		assert(error && !strcmp(error, "nd_network_error"));
	else if (!strcmp(argv[3], "cancel"))
		assert(error && !strcmp(error, "nd_audio_cancelled"));
	else if (!strcmp(argv[3], "protected"))
		assert(error && !strcmp(error, "nd_audio_space") && !access(sparse, F_OK));
	else {
		assert(!error && deliveries == 1);
		struct stat st;
		assert(!stat(delivered_path, &st));
		if (!strcmp(argv[3], "progressive") || !strcmp(argv[3], "evict"))
			assert(delivered_bytes < st.st_size);
		else
			assert(delivered_bytes == st.st_size);
		connected = false;
		lib.writable = false;
		lib.ready = false; // A captured saved playlist needs no synced snapshot.
		strcpy(ctx.password, "replacement synthetic password");
		assert(!nd_audio_fetch(&lib, &ctx, &track, true, current, ready, argv[3]) && deliveries == 2 && !leases);
		assert(!growfile_is_growing(delivered_path));
		// Exercise the optional permanent-original hook on an ordinary path,
		// offline/read-only and without manufacturing library readiness.
		char cached_path[512];
		strcpy(cached_path, delivered_path);
		assert(snprintf(saved_permanent, sizeof(saved_permanent), "%s/permanent%s", argv[2], strrchr(cached_path, '.')) < (int)sizeof(saved_permanent));
		assert(!link(cached_path, saved_permanent));
		nd_playrow_t permanent_track = {.row = {.id = "saved-permanent"}};
		nd_audio_set_original_resolver(saved_original);
		assert(nd_audio_cached(&lib, &ctx, permanent_track.row.id));
		assert(!nd_audio_fetch(&lib, &ctx, &permanent_track, true, current, ready, "saved") && deliveries == 3 && !leases);
		assert(!strcmp(delivered_path, saved_permanent) && !nd_audio_owns(delivered_path));
		nd_audio_set_source(delivered_path);
		assert(nd_audio_source(delivered_path));
		nd_audio_set_source(NULL);
		assert(!nd_audio_source(delivered_path));
		nd_audio_set_original_resolver(NULL);
		strcpy(delivered_path, cached_path);
		assert(!unlink(saved_permanent));
		char scope[33];
		strcpy(scope, lib.scope);
		strcpy(lib.scope, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
		assert(!nd_audio_cached(&lib, &ctx, track.row.id));
		strcpy(lib.scope, scope);
		if (!strcmp(argv[3], "evict"))
			assert(access(sparse, F_OK));
		// A same-length corrupt file cannot remain a complete hit.
		int fd = open(delivered_path, O_WRONLY | O_TRUNC);
		assert(fd >= 0);
		assert(!ftruncate(fd, st.st_size));
		close(fd);
		error = nd_audio_fetch(&lib, &ctx, &track, true, current, ready, argv[3]);
		assert(error && !strcmp(error, "nd_audio_invalid") && deliveries == 3 && !leases);
	}
	assert(!atomic_load(&fixture_cache_io));
	puts("production audio cache/decoder fixture passed");
	return 0;
}

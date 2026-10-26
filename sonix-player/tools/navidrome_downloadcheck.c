// Actual download worker/store/API/HTTP, native WAV validation and filesystem
// publication. Synthetic service lifetime and metadata; opens no audio device.
#define _GNU_SOURCE
#include "src/system/decode/decode.h"
#include "src/system/library/metadata.h"
#include "src/system/streaming/navidromeaudio.h"
#include "src/system/streaming/navidromedownloads.h"
#include "src/system/streaming/navidromeplayliststore.h"
#include "src/system/streaming/streamturn.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static nd_context_t context;
static nd_library_t library;
static atomic_bool online = true, alive = true;
static atomic_int leases, cache_users;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t posts_lock = PTHREAD_MUTEX_INITIALIZER;
static void (*posts[32])(void *);
static void *post_users[32];
static int post_count;
static bool captured, capture_ok, unsupported, reject_posts, after_rename;
static int expected = 2;
static int callback_skipped;
static char destination[256] = "Music/Navidrome";
static char mode[40];
static int64_t saved_playlist;
static bool changed_saved;
bool __real_nd_pl_play_window(nd_playlist_store_t *, int64_t, int64_t, int, nd_playlist_entry_t[ND_WINDOW], int *, int *);
bool __wrap_nd_pl_play_window(nd_playlist_store_t *s, int64_t id, int64_t revision, int first,
		nd_playlist_entry_t out[ND_WINDOW], int *count, int *total) {
	bool ok = __real_nd_pl_play_window(s, id, revision, first, out, count, total);
	if (ok && !changed_saved && !strcmp(mode, "playlist_changed")) {
		changed_saved = true;
		nd_playlist_store_t edit;
		assert(nd_pl_open(&edit, s->path, s->scope, true));
		assert(nd_pl_edit(&edit, id, revision, "Changed", false) && nd_pl_sql(&edit, "COMMIT"));
		nd_pl_close(&edit);
	}
	return ok;
}
static char last_ready[512], cache_paths[2][512];
static int descriptors(void) {
	DIR *dir = opendir("/proc/self/fd");
	assert(dir);
	int count = 0;
	struct dirent *entry;
	while ((entry = readdir(dir)))
		if (entry->d_name[0] != '.')
			count++;
	closedir(dir);
	return count;
}
static bool own_partial(int fd) {
	char link[64], name[1024];
	snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
	ssize_t n = readlink(link, name, sizeof(name) - 1);
	if (n < 0)
		return false;
	name[n] = 0;
	return strstr(name, "/.nd-") != NULL;
}
ssize_t __real_write(int, const void *, size_t);
ssize_t __wrap_write(int fd, const void *data, size_t n) {
	if (!strcmp(mode, "write_fail") && own_partial(fd)) {
		errno = ENOSPC;
		return -1;
	}
	return __real_write(fd, data, n);
}
int __real_fsync(int);
int __wrap_fsync(int fd) {
	if (!strcmp(mode, "flush_fail") && own_partial(fd)) {
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}
#ifdef __USE_FILE_OFFSET64
#define __real_fstatvfs __real_fstatvfs64
#define __wrap_fstatvfs __wrap_fstatvfs64
#endif
int __real_fstatvfs(int, struct statvfs *);
int __wrap_fstatvfs(int fd, struct statvfs *st) {
	int rc = __real_fstatvfs(fd, st);
	if (!rc && !strcmp(mode, "full"))
		st->f_bavail = 0;
	return rc;
}
void nd_log(const char *text, ...) { (void)text; }
void thread_be_background(const char *name) { (void)name; }
void metadata_read(const char *path, song_metadata_t *out) {
	(void)path;
	memset(out, 0, sizeof(*out));
	if (strcmp(mode, "fallback"))
		snprintf(out->album_artist, sizeof(out->album_artist), "Various Artists");
}
decode_format_t decode_detect_format(const char *path) {
	(void)path;
	return DECODE_FORMAT_UNKNOWN;
}
decoder_t *decoder_open_info(const char *p, decode_format_t f) {
	(void)p;
	(void)f;
	return NULL;
}
int decoder_sample_rate(const decoder_t *d) {
	(void)d;
	return 0;
}
int decoder_channels(const decoder_t *d) {
	(void)d;
	return 0;
}
void decoder_close(decoder_t *d) { (void)d; }
unsigned nd_cache_epoch(void) { return 1; }
bool nd_cache_io_current(unsigned epoch) { return epoch == 1; }
bool nd_cache_io_begin(unsigned epoch) {
	if (epoch != 1)
		return false;
	cache_users++;
	return true;
}
void nd_cache_io_end(void) {
	assert(cache_users > 0);
	cache_users--;
}
bool gui_post(void (*fn)(void *), void *user) {
	if (reject_posts)
		return false;
	pthread_mutex_lock(&posts_lock);
	bool ok = post_count < 32;
	if (ok) {
		posts[post_count] = fn;
		post_users[post_count++] = user;
	}
	pthread_mutex_unlock(&posts_lock);
	return ok;
}
static void pump(void) {
	for (;;) {
		pthread_mutex_lock(&posts_lock);
		if (!post_count) {
			pthread_mutex_unlock(&posts_lock);
			break;
		}
		void (*fn)(void *) = posts[--post_count];
		void *u = post_users[post_count];
		pthread_mutex_unlock(&posts_lock);
		fn(u);
	}
	navidromedownloads_poll();
}
bool navidrome_context(nd_context_t *out, bool network) {
	*out = context;
	return alive && (!network || online);
}
bool navidrome_playback_current(const nd_context_t *c, bool network) { return alive && c->profile_epoch == context.profile_epoch && c->card_epoch == context.card_epoch && (!network || online); }
bool navidrome_playback_scope(const nd_context_t *c, const nd_library_t *lib) { return navidrome_playback_current(c, false) && lib->card_epoch == context.card_epoch && !strcmp(lib->scope, library.scope) && !strcmp(lib->path, library.path); }
bool navidrome_playback_begin(const nd_context_t *c) {
	if (!navidrome_playback_current(c, false))
		return false;
	leases++;
	return true;
}
void navidrome_cache_end(void) {
	assert(leases > 0);
	leases--;
}
bool navidrome_playback_pin(const nd_library_t *lib) { return nd_store_retain(lib->scope, lib->generation); }
bool navidrome_playback_pin_source(const nd_library_t *lib) { return nd_store_retain_source(lib); }
bool navidrome_playback_action(const nd_context_t *c, bool network, bool (*fn)(void *), void *user) {
	pthread_mutex_lock(&gate);
	bool ok = navidrome_playback_current(c, network) && fn(user);
	pthread_mutex_unlock(&gate);
	return ok;
}
bool navidrome_playback_publish(const nd_context_t *c, const char *from, const char *to) { return navidrome_playback_current(c, true) && !rename(from, to); }
void navidrome_set_download_hooks(void (*cancel)(void), void (*shutdown)(void)) {
	(void)cancel;
	(void)shutdown;
}
void navidrome_network_failed(const nd_context_t *c) {
	(void)c;
	online = false;
}
const char *navidrome_validate(const char *host, const char *u, const char *p, char out[ND_HOST_MAX]) {
	(void)u;
	(void)p;
	snprintf(out, ND_HOST_MAX, "%s", host);
	return NULL;
}
int nd_download_publish_test_call(int directory, const char *part, const char *final) {
	if (unsupported) {
		errno = EOPNOTSUPP;
		return -1;
	}
	int rc = syscall(SYS_renameat2, directory, part, directory, final, 1U);
	if (!rc && after_rename && !strncmp(part, ".nd-", 4))
		alive = false;
	return rc;
}
static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}
static void callback(bool ok, const char *error, int added, int skipped, void *user) {
	(void)error;
	(void)skipped;
	(void)user;
	capture_ok = ok;
	callback_skipped = skipped;
	captured = true;
	if (ok)
		assert(added == expected);
}
static void submit(nd_dl_action_t action) {
	nd_dl_request_t q = {.action = action, .library = library, .kind = ND_ALBUMS, .token = 1};
	strcpy(q.id, "album");
	strcpy(q.destination, destination);
	if (!strncmp(mode, "playlist", 8)) {
		q.playlist = saved_playlist;
		q.playlist_revision = 2;
	}
	if (!strcmp(mode, "selection") || !strcmp(mode, "case_collision"))
		q.session = 1;
	if (!strcmp(mode, "single_multi") || !strcmp(mode, "current") || !strcmp(mode, "current_remote")) {
		q.kind = ND_TRACKS;
		strcpy(q.id, "track/1?");
		q.direct = strcmp(mode, "single_multi") != 0;
		strcpy(q.track.row.id, q.id);
		strcpy(q.track.album_id, "album");
		strcpy(q.track.album, "Album");
		strcpy(q.track.row.title, "Track 1");
		q.track.row.track = 1;
	}
	double end = now() + 5;
	while (!navidromedownloads_request(&q, action == NDDL_CAPTURE ? callback : NULL, NULL)) {
		pump();
		assert(now() < end);
		usleep(1000);
	}
}
static nd_dl_view_t *folder_request(nd_dl_action_t action, const char *folder, const char *name) {
	static unsigned folder_token = 100;
	nd_dl_request_t q = {.action = action, .library = library, .token = ++folder_token};
	snprintf(q.folder, sizeof(q.folder), "%s", folder);
	snprintf(q.name, sizeof(q.name), "%s", name ? name : "");
	double end = now() + 5;
	while (!navidromedownloads_request(&q, NULL, NULL)) {
		pump();
		assert(now() < end);
		usleep(1000);
	}
	nd_dl_view_t *v = calloc(1, sizeof(*v));
	assert(v);
	for (;;) {
		pump();
		navidromedownloads_view(v);
		if (!v->busy && v->token == q.token)
			return v;
		assert(now() < end);
		usleep(1000);
	}
}
static void check_folders(const char *root, int baseline) {
	char path[1024];
	snprintf(path, sizeof(path), "%s/Music", root);
	assert(access(path, F_OK) && errno == ENOENT);
	// Reproduce the empty-card failure, then prepare the actual configured
	// destination on the real manager worker, including its missing parent.
	nd_dl_view_t *v = folder_request(NDDL_FOLDERS, "Music/Navidrome", NULL);
	assert(!strcmp(v->error, "nd_dl_path_error"));
	free(v);
	library.writable = false;
	v = folder_request(NDDL_PREPARE_FOLDER, "Music/Navidrome", NULL);
	assert(!strcmp(v->error, "nd_dl_readonly") && access(path, F_OK));
	free(v);
	library.writable = true;
	v = folder_request(NDDL_PREPARE_FOLDER, "Music/Navidrome", NULL);
	assert(!v->error[0] && !strcmp(v->folder, "Music/Navidrome") && !v->folder_total && !access(path, F_OK));
	free(v);
	v = folder_request(NDDL_MKDIR, "Music/Navidrome", "New folder");
	assert(!v->error[0] && !strcmp(v->folder, "Music/Navidrome/New folder"));
	free(v);
	library.writable = false;
	v = folder_request(NDDL_MKDIR, "Music/Navidrome", "Read only");
	assert(!strcmp(v->error, "nd_dl_readonly"));
	free(v);
	snprintf(path, sizeof(path), "%s/Music/Navidrome/Read only", root);
	assert(access(path, F_OK));
	library.writable = true;
	snprintf(path, sizeof(path), "%s/Link", root);
	assert(!symlink(root, path));
	v = folder_request(NDDL_PREPARE_FOLDER, "Link/Escape", NULL);
	assert(!strcmp(v->error, "nd_dl_path_error"));
	free(v);
	snprintf(path, sizeof(path), "%s/Escape", root);
	assert(access(path, F_OK));
	const char *excluded[] = {".hidden", "Podcast", "Audiobooks"};
	for (unsigned i = 0; i < sizeof(excluded) / sizeof(*excluded); i++) {
		int fd = nd_dl_directory(root, excluded[i], true);
		assert(fd >= 0);
		close(fd);
	}
	v = folder_request(NDDL_FOLDERS, "", NULL);
	assert(!v->error[0] && v->folder_count == 1 && !strcmp(v->folders[0], "Music"));
	free(v);
	v = folder_request(NDDL_PREPARE_FOLDER, "../Escape", NULL);
	assert(!strcmp(v->error, "nd_dl_path_error"));
	free(v);
	v = folder_request(NDDL_MKDIR, "Music/Navidrome", "../Escape");
	assert(!strcmp(v->error, "nd_dl_path_error"));
	free(v);
	navidromedownloads_shutdown();
	assert(!leases && !cache_users && descriptors() == baseline);
	puts("PASS real empty-card directory preparation, parent creation, new-folder entry, root/filtering, read-only and symlink/traversal guards; no network or leaked leases/descriptors");
}
static void counts(int values[9]) {
	nd_dl_store_t s;
	assert(nd_dl_open(&s, &library, false));
	nd_dl_job_t rows[ND_WINDOW];
	int n, total;
	assert(nd_dl_window(&s, 0, rows, &n, &total, values));
	nd_dl_close(&s);
}
static bool always(void *unused) {
	(void)unused;
	return true;
}
static void ready(const char *path, void *user) {
	(void)user;
	assert(path && *path);
	snprintf(last_ready, sizeof(last_ready), "%s", path);
}
int main(int argc, char **argv) {
	assert(argc == 4);
	snprintf(context.host, sizeof(context.host), "%s", argv[1]);
	strcpy(context.user, "fixture user");
	strcpy(context.password, "synthetic-download-password");
	context.card_epoch = context.profile_epoch = context.epoch = 1;
	snprintf(mode, sizeof(mode), "%s", argv[3]);
	if (!strcmp(mode, "root"))
		destination[0] = 0;
	unsupported = !strcmp(mode, "unsupported");
	if (!strcmp(mode, "large_batch") || !strcmp(mode, "playlist_large")) {
		unsupported = true;
		expected = !strcmp(mode, "large_batch") ? 17000 : 2;
	}
	reject_posts = !strcmp(mode, "reject_post");
	after_rename = !strcmp(mode, "after_rename");
	if (!strcmp(mode, "single_multi") || !strcmp(mode, "current") || !strcmp(mode, "current_remote"))
		expected = 1;
	library.ready = library.writable = true;
	library.favorites = !strcmp(mode, "favorites");
	library.card_epoch = library.epoch = 1;
	library.generation = 1;
	memset(library.scope, 'a', 32);
	snprintf(library.path, sizeof(library.path), "%s/.local/navidrome/.cache/library.db", argv[2]);
	int dir = nd_dl_directory(argv[2], ".local/navidrome/.cache", true);
	assert(dir >= 0);
	close(dir);
	nd_store_t source;
	assert(nd_store_open_source(&source, library.path, library.scope, true, false, library.favorites));
	char *sql = sqlite3_mprintf("INSERT INTO snapshot(id,scope,complete,finished) VALUES(1,%Q,1,0); INSERT INTO active VALUES(%Q,1); INSERT INTO entity(gen,kind,id,title,artist,album,cover,duration,disc,track,sortkey) VALUES(1,0,'album','Album','Various Artists','','',0,1,0,'album'),(1,1,'track/1?','Track 1','Performer 1','album','',1,1,1,'track1'),(1,1,'track/2?','Track 2','Performer 2','album','',1,1,2,'track2');", library.scope, library.scope);
	assert(sqlite3_exec(source.db, sql, NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_free(sql);
	if (!strcmp(mode, "single_multi"))
		assert(sqlite3_exec(source.db, "UPDATE entity SET disc=2 WHERE id='track/2?'", NULL, NULL, NULL) == SQLITE_OK);
	if (!strcmp(mode, "root"))
		assert(sqlite3_exec(source.db, "UPDATE entity SET artist='Podcast' WHERE kind=0", NULL, NULL, NULL) == SQLITE_OK);
	if (library.favorites)
		assert(sqlite3_exec(source.db, "INSERT INTO favorite_album VALUES(1,'album',2); INSERT INTO favorite_song VALUES(1,'track/1?',1),(1,'track/2?',1)", NULL, NULL, NULL) == SQLITE_OK);
	if (!strcmp(mode, "same_title"))
		assert(sqlite3_exec(source.db, "UPDATE entity SET title='Same title' WHERE kind=1", NULL, NULL, NULL) == SQLITE_OK);
	if (!strcmp(mode, "fallback"))
		assert(sqlite3_exec(source.db, "UPDATE entity SET title='',artist='',disc=0,track=0", NULL, NULL, NULL) == SQLITE_OK);
	if (!strcmp(mode, "large_batch"))
		assert(sqlite3_exec(source.db, "DELETE FROM entity WHERE kind=1; WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<17000) INSERT INTO entity(gen,kind,id,title,artist,album,cover,duration,disc,track,sortkey) SELECT 1,1,printf('large/%d',x),printf('Track %d',x),'Performer','album','',1,1,x,printf('%05d',x) FROM n", NULL, NULL, NULL) == SQLITE_OK);
	if (!strcmp(mode, "case_collision"))
		assert(sqlite3_exec(source.db, "INSERT INTO entity(gen,kind,id,title,artist,album,cover,duration,disc,track,sortkey) VALUES(1,0,'album2','album','VARIOUS ARTISTS','','',0,1,0,'album2'); UPDATE entity SET album='album2' WHERE id='track/2?'", NULL, NULL, NULL) == SQLITE_OK);
	nd_store_close(&source);
	if (!strncmp(mode, "playlist", 8)) {
		nd_playlist_store_t saved;
		char path[1200];
		assert(nd_pl_path(&library, path) && nd_pl_open(&saved, path, library.scope, true));
		assert(nd_pl_create(&saved, "Saved", &saved_playlist) && nd_pl_begin(&saved, saved_playlist, 1));
		int added = 0, skipped = 0;
		int count = !strcmp(mode, "playlist_large") ? 5000 : 3;
		for (int i = 0; i < count; i++) {
			nd_row_t row = {.title = "Saved track"};
			snprintf(row.id, sizeof(row.id), "track/%d?", i % 2 + 1);
			assert(nd_pl_append(&saved, saved_playlist, &row, false, &added, &skipped));
		}
		assert(nd_pl_finish(&saved, saved_playlist, true) && nd_pl_sql(&saved, "COMMIT"));
		if (!strcmp(mode, "playlist_stale"))
			assert(nd_pl_edit(&saved, saved_playlist, 2, "Changed", false) && nd_pl_sql(&saved, "COMMIT"));
		if (!strcmp(mode, "playlist_deleted"))
			assert(nd_pl_edit(&saved, saved_playlist, 2, NULL, true) && nd_pl_sql(&saved, "COMMIT"));
		nd_pl_close(&saved);
		if (!strcmp(mode, "playlist_remote") || !strcmp(mode, "playlist_badmeta")) {
			assert(!unlink(library.path)); library.ready = false;
		}
	}
	if (!strcmp(mode, "selection") || !strcmp(mode, "case_collision")) {
		nd_playlist_store_t s = {0};
		char path[1200];
		assert(nd_pl_path(&library, path) && nd_pl_open(&s, path, library.scope, true));
		assert(nd_pl_sql(&s, !strcmp(mode, "selection") ? "INSERT INTO selection VALUES(1,0,0,'album',0),(1,1,1,'track/1?',0)" : "INSERT INTO selection VALUES(1,0,1,'track/1?',0),(1,1,1,'track/2?',0)"));
		nd_pl_close(&s);
	}
	if (!strcmp(mode, "current_remote")) {
		assert(!unlink(library.path));
		library.ready = false;
	}
	if (!strcmp(mode, "readonly"))
		library.writable = false;
	if (!strcmp(mode, "offline") || !strcmp(mode, "active_cache") || !strcmp(mode, "fallback") || !strcmp(mode, "playlist_offline")) {
		nd_playrow_t row = {0};
		for (int i = 1; i <= 2; i++) {
			snprintf(row.row.id, sizeof(row.row.id), "track/%d?", i);
			assert(!nd_audio_fetch(&library, &context, &row, false, always, ready, NULL));
			snprintf(cache_paths[i - 1], sizeof(cache_paths[i - 1]), "%s", last_ready);
		}
		if (!strcmp(mode, "active_cache")) {
			assert(nd_audio_cached(&library, &context, "track/1?"));
			const char *paths[3] = {cache_paths[0], cache_paths[1], NULL};
			nd_audio_protect(paths);
		}
		online = false;
	}
	if (!strcmp(mode, "collision")) {
		dir = nd_dl_directory(argv[2], "Music/Navidrome/Various Artists/Album", true);
		assert(dir >= 0);
		int fd = openat(dir, "Track 1.wav", O_CREAT | O_EXCL | O_WRONLY, 0600);
		assert(fd >= 0 && write(fd, "USER", 4) == 4);
		close(fd);
		close(dir);
	}
	int baseline_descriptors = descriptors();
	navidromedownloads_init();
	if (!strcmp(mode, "folders")) {
		check_folders(argv[2], baseline_descriptors);
		return 0;
	}
	submit(NDDL_CAPTURE);
	double end = now() + 8;
	while (!captured) {
		pump();
		assert(now() < end);
		usleep(1000);
	}
	if (!strcmp(mode, "readonly")) {
		assert(!capture_ok);
		navidromedownloads_shutdown();
		pump();
		assert(!leases && !cache_users);
		assert(descriptors() == baseline_descriptors);
		puts("PASS read-only capture rejects before HTTP");
		return 0;
	}
	if (!strcmp(mode, "playlist_stale") || !strcmp(mode, "playlist_changed") || !strcmp(mode, "playlist_deleted")) {
		assert(!capture_ok);
		navidromedownloads_shutdown(); pump();
		nd_dl_store_t s;
		assert(nd_dl_open(&s, &library, false));
		sqlite3_stmt *st;
		assert(nd_dl_prepare(&s, &st, "SELECT count(*) FROM job") && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 0);
		sqlite3_finalize(st); nd_dl_close(&s);
		assert(!leases && !cache_users && descriptors() == baseline_descriptors);
		puts("PASS stale/deleted/during-capture playlist revision rolls back the whole batch");
		return 0;
	}
	assert(capture_ok);
	if (!strncmp(mode, "playlist", 8)) assert(callback_skipped == (!strcmp(mode, "playlist_large") ? 4998 : 1));
	if (!strcmp(mode, "large_batch"))
		printf("bounded downloads: request=%zu bytes, view=%zu bytes, jobs=%zu bytes for a %d-row window; 17,000 captured jobs\n", sizeof(nd_dl_request_t), sizeof(nd_dl_view_t), sizeof(nd_dl_job_t) * ND_WINDOW, ND_WINDOW);
	if (!strcmp(mode, "selection"))
		assert(callback_skipped == 1);
	if (!strcmp(mode, "capture_refresh"))
		assert(!unlink(library.path));
	if (!strcmp(mode, "pause") || !strcmp(mode, "cancel") || !strcmp(mode, "preempt") || !strcmp(mode, "resume") || !strcmp(mode, "preempt_headers")) {
		nd_dl_view_t *v = calloc(1, sizeof(*v));
		assert(v);
		while (1) {
			navidromedownloads_view(v);
			char entered[640];
			snprintf(entered, sizeof(entered), "%s/entered", argv[2]);
			if (v->bytes > 0 || (!strcmp(mode, "preempt_headers") && !access(entered, F_OK)))
				break;
			assert(now() < end);
			usleep(1000);
		}
		free(v);
		if (!strcmp(mode, "preempt") || !strcmp(mode, "preempt_headers")) {
			double begin = now();
			assert(streamturn_acquire_current(always, NULL));
			assert(now() - begin < 1.5);
			submit(NDDL_CANCEL);
			streamturn_release();
		} else
			submit(!strcmp(mode, "pause") || !strcmp(mode, "resume") ? NDDL_PAUSE : NDDL_CANCEL);
	}
	int values[9];
	if (!strcmp(mode, "clear_active")) {
		char entered[1024];
		snprintf(entered, sizeof(entered), "%s/entered", argv[2]);
		while (access(entered, F_OK)) {
			pump();
			assert(now() < end);
			usleep(1000);
		}
		nd_dl_view_t *cleared = folder_request(NDDL_CLEAR_HISTORY, "", NULL);
		assert(!cleared->error[0] && cleared->total == 1 && !cleared->counts[NDDL_COMPLETE]);
		free(cleared);
		expected = 1; // The first completed job is hidden; the second keeps running.
	}
	for (;;) {
		pump();
		counts(values);
		bool finished;
		if (!strcmp(mode, "after_rename"))
			finished = !alive && values[NDDL_INTENT] == 1 && !navidromedownloads_network_wanted();
		else if (!strcmp(mode, "partial_retry") && values[NDDL_FAILED] == 1 && values[NDDL_COMPLETE] == 1) {
			submit(NDDL_RETRY);
			strcpy(mode, "retried");
			finished = false;
		} else if (!strcmp(mode, "resume") && values[NDDL_PAUSED] == 2) {
			submit(NDDL_RESUME);
			strcpy(mode, "resumed");
			finished = false;
		} else if (!strcmp(mode, "pause"))
			finished = values[NDDL_PAUSED] == 2;
		else if (!strcmp(mode, "cancel") || !strcmp(mode, "preempt") || !strcmp(mode, "preempt_headers"))
			finished = values[NDDL_CANCELLED] == 2;
		else if (!strcmp(mode, "playlist_badmeta") || !strcmp(mode, "error") || !strcmp(mode, "truncated") || !strcmp(mode, "oversize") || unsupported || !strcmp(mode, "json") || !strcmp(mode, "html") || !strcmp(mode, "chunk_bad") || !strcmp(mode, "gzip") || !strcmp(mode, "unknown_limit") || !strcmp(mode, "auth") || !strcmp(mode, "write_fail") || !strcmp(mode, "flush_fail") || !strcmp(mode, "full"))
			finished = values[NDDL_FAILED] > 0;
		else
			finished = values[NDDL_COMPLETE] == expected;
		if (finished)
			break;
		if (now() > end) {
			fprintf(stderr, "fixture mode=%s counts=%d,%d,%d,%d,%d,%d,%d,%d,%d\n", mode, values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7], values[8]);
			assert(0);
		}
		usleep(2000);
	}
	if (!strcmp(mode, "after_rename")) {
		navidromedownloads_shutdown();
		alive = true;
		online = false;
		after_rename = false;
		navidromedownloads_init();
		submit(NDDL_VIEW);
		for (int turn = 0;; turn++) {
			pump();
			counts(values);
			if (values[NDDL_COMPLETE] == 1 && values[NDDL_PAUSED] == 1)
				break;
			assert(turn < 5000);
			usleep(1000);
		}
		char file[512];
		assert(nd_download_resolve(&library, &context, "track/1?", file));
	}
	if (values[NDDL_COMPLETE] == expected) {
		if (!strcmp(mode, "active_cache"))
			assert(!access(cache_paths[0], F_OK) && !access(cache_paths[1], F_OK));
		char path[512];
		assert(nd_download_resolve(&library, &context, "track/1?", path));
		if (!strcmp(mode, "same_title")) {
			char other[ND_DL_PATH];
			assert(strstr(path, "/Same title.wav"));
			assert(nd_download_resolve(&library, &context, "track/2?", other));
			assert(strstr(other, "/Same title [") && strcmp(other, path));
		}
		if (!strcmp(mode, "root"))
			assert(strstr(path, "/_Podcast/Album/Track 1.wav"));
		if (!strcmp(mode, "fallback"))
			assert(strstr(path, "/Unknown Album Artist/Unknown Album/Track.wav"));
		if (!strcmp(mode, "case_collision")) {
			char other[512];
			assert(nd_download_resolve(&library, &context, "track/2?", other));
			assert(strstr(other, "/Various Artists/album [") && strcmp(other, path));
		}
		if (!strcmp(mode, "single_multi"))
			assert(strstr(path, "/Disc 1/"));
		if (!strcmp(mode, "playlist_remote")) {
			assert(strstr(path, "/Disc 1/Track 1.wav"));
			nd_dl_store_t store;
			nd_dl_job_t saved;
			assert(nd_dl_open(&store, &library, false) && nd_dl_get(&store, 1, &saved));
			assert(!strcmp(saved.source.album_id, "album") && !strcmp(saved.source.album, "Album") && saved.source.row.disc == 1);
			nd_dl_close(&store);
		}
		if (!strcmp(mode, "recover")) {
			navidromedownloads_shutdown();
			nd_dl_store_t s;
			assert(nd_dl_open(&s, &library, true));
			assert(nd_dl_sql(&s, "UPDATE job SET state=3 WHERE key=1"));
			nd_dl_close(&s);
			online = false;
			navidromedownloads_init();
			submit(NDDL_VIEW);
			for (int turn = 0;; turn++) {
				pump();
				counts(values);
				if (values[NDDL_COMPLETE] == expected)
					break;
				assert(turn < 5000);
				usleep(1000);
			}
		}
		online = false;
		assert(nd_audio_cached(&library, &context, "track/1?"));
		nd_playrow_t row = {0};
		strcpy(row.row.id, "track/1?");
		assert(!nd_audio_fetch(&library, &context, &row, false, always, ready, NULL));
		assert(!nd_audio_source(path));
		nd_audio_set_source(path);
		assert(nd_audio_source(path) && !nd_audio_owns(path));
		nd_audio_set_source(NULL);
		assert(!nd_audio_source(path));
		if (!strcmp(mode, "modified")) {
			int fd = open(path, O_RDWR | O_NOFOLLOW);
			struct stat before;
			assert(fd >= 0 && !fstat(fd, &before));
			unsigned char original, changed;
			assert(pread(fd, &original, 1, 60) == 1);
			changed = original ^ 0x7f;
			assert(pwrite(fd, &changed, 1, 60) == 1);
			struct timespec times[2] = {{.tv_sec = before.st_atime}, {.tv_sec = before.st_mtime + 3}};
			assert(!futimens(fd, times));
			library.writable = false;
			char resolved[512];
			assert(!nd_download_resolve(&library, &context, "track/1?", resolved));
			assert(pwrite(fd, &original, 1, 60) == 1);
			times[1].tv_sec = before.st_mtime;
			assert(!futimens(fd, times) && !close(fd));
			assert(!nd_download_resolve(&library, &context, "track/1?", resolved) && !access(path, F_OK));
			library.writable = true;
		}
		if (!strcmp(mode, "success")) {
			int completed = expected;
			expected = 0;
			captured = false;
			strcpy(destination, "Another destination");
			submit(NDDL_CAPTURE);
			for (int turn = 0; !captured; turn++) {
				assert(turn < 5000);
				pump();
				usleep(1000);
			}
			assert(capture_ok && callback_skipped == completed);
			expected = completed;
			char old[512];
			assert(nd_download_resolve(&library, &context, "track/1?", old) && !strcmp(old, path));
		}
		if (!strcmp(mode, "clear_history")) {
			// Clearing terminal history must retain the completed registry and
			// never make a hidden failed attempt eligible for later Retry.
			nd_dl_store_t store;
			assert(nd_dl_open(&store, &library, true));
			assert(nd_dl_sql(&store, "INSERT INTO job SELECT NULL,batch,track,albumid,title,artist,album,cover,disc,number,multi,6,destination,albumartist,path,partial,digest,extension,error,bytes,total,mtime,inode FROM job WHERE key=1"));
			nd_dl_close(&store);
			nd_dl_view_t *cleared = folder_request(NDDL_CLEAR_HISTORY, "", NULL);
			assert(!cleared->error[0] && !cleared->total && !cleared->batches && !cleared->skipped);
			free(cleared);
			assert(!access(path, F_OK) && nd_download_resolve(&library, &context, "track/1?", path));
			char other[ND_DL_PATH];
			assert(nd_download_resolve(&library, &context, "track/2?", other) && !access(other, F_OK));
			cleared = folder_request(NDDL_RETRY, "", NULL);
			assert(!cleared->error[0] && !cleared->total);
			free(cleared);
			expected = 0;
			captured = false;
			submit(NDDL_CAPTURE);
			while (!captured) {
				pump();
				usleep(1000);
			}
			assert(capture_ok && callback_skipped == 2);
		}
		if (!strcmp(mode, "deleted_again") || !strcmp(mode, "deleted_history") || !strcmp(mode, "deleted_retry")) {
			if (!strcmp(mode, "deleted_history")) {
				nd_dl_store_t history;
				assert(nd_dl_open(&history, &library, true));
				assert(nd_dl_sql(&history, "INSERT INTO job SELECT NULL,batch,track,albumid,title,artist,album,cover,disc,number,multi,state,destination,albumartist,path,partial,digest,extension,error,bytes,total,mtime,inode FROM job WHERE key=1"));
				nd_dl_close(&history);
			}
			assert(!unlink(path));
			online = true;
			captured = false;
			if (!strcmp(mode, "deleted_retry")) {
				assert(!nd_download_resolve(&library, &context, "track/1?", path));
				submit(NDDL_RETRY);
			} else {
				expected = 1;
				submit(NDDL_CAPTURE);
				for (int turn = 0; !captured; turn++) {
					pump();
					assert(turn < 5000);
					usleep(1000);
				}
				assert(capture_ok && callback_skipped == 1);
			}
			for (int turn = 0;; turn++) {
				pump();
				counts(values);
				if (values[NDDL_COMPLETE] == 2 && nd_download_resolve(&library, &context, "track/1?", path))
					break;
				assert(turn < 5000);
				usleep(1000);
			}
			assert(!access(path, F_OK));
		}
		assert(!unlink(path));
		assert(!nd_download_resolve(&library, &context, "track/1?", path));
	}
	navidromedownloads_shutdown();
	pump();
	assert(!leases && !cache_users);
	assert(descriptors() == baseline_descriptors);
	puts("PASS real worker original transfer/cache reuse, job state, publication and source context");
	return 0;
}

// Real service, HTTP, SQLite and Unicode matching with synthetic fixture data.
#include "src/system/library/library.h"
#include "src/system/net/http.h"
#include "src/system/streaming/navidromeplaycatalog.h"
#include "src/system/streaming/navidromesearch.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static char config_file[1024], origin[512];
const char *tr(const char *key) { return key; }
const char *config_path(void) { return config_file; }
const char *config_get(const char *section, const char *key, const char *fallback) {
	(void)section;
	(void)key;
	return fallback;
}
bool gui_post(void (*cb)(void *), void *user) {
	(void)cb;
	(void)user;
	return true;
}
void thread_be_background(const char *name) { (void)name; }
static navidrome_snapshot_t wait_idle(void) {
	navidrome_snapshot_t state;
	for (int i = 0; i < 30000; i++) {
		navidrome_snapshot(&state);
		if (!state.busy)
			return state;
		usleep(1000);
	}
	assert(!"service timeout");
	return state;
}
static nd_library_t wait_library(void) {
	nd_library_t lib;
	for (int i = 0; i < 30000; i++) {
		navidrome_library_snapshot(&lib);
		if (!lib.loading)
			return lib;
		usleep(1000);
	}
	assert(!"library timeout");
	return lib;
}
static void no_child_handles(void) {
	DIR *dir = opendir("/proc/self/fd");
	assert(dir);
	struct dirent *entry;
	while ((entry = readdir(dir))) {
		char path[512], target[1500];
		snprintf(path, sizeof(path), "/proc/self/fd/%s", entry->d_name);
		ssize_t n = readlink(path, target, sizeof(target) - 1);
		if (n < 0)
			continue;
		target[n] = 0;
		assert(!strstr(target, "/search-") && !strstr(target, "/.playqueue-"));
	}
	closedir(dir);
}
static void control(const char *mode) {
	char url[700];
	snprintf(url, sizeof(url), "%s/control?mode=%s", origin, mode);
	char *body = NULL;
	size_t n = 0;
	int status = 0;
	http_req_t req = {.sensitive_url = true, .require_complete_body = true};
	assert(http_request(url, &req, &body, &n, 1024, 8, &status) && status == 200);
	free(body);
	http_clear_request_metadata();
}
#include "navidrome_search_seed.h"
static void reconnect(void) {
	navidrome_test_online(1);
	assert(navidrome_reconnect());
	assert(wait_idle().state == ND_CONNECTED);
}
static nd_search_result_t read_first(const char *q, nd_kind_t kind) {
	nd_search_t *s = nd_search_create(q);
	assert(s);
	nd_search_result_t r;
	nd_search_read(s, kind, 0, &r);
	nd_search_destroy(s);
	return r;
}
static bool capture_current(void *arg) { return navidrome_playback_current(arg, false); }
static void captures(nd_library_t lib) {
	nd_context_t identity;
	assert(navidrome_context(&identity, false));
	nd_wipe(identity.password, sizeof(identity.password));
	nd_query_t q = {.kind = ND_TRACKS};
	snprintf(q.search, sizeof(q.search), "Track");
	const char *error = NULL;
	nd_playcatalog_t *c = nd_playcatalog_capture(&lib, &identity, &q, "remote-song-10000", capture_current, &identity, &error);
	assert(c && !error && nd_playcatalog_count(c) == 17000 && nd_playcatalog_start(c) == 10000);
	nd_playrow_t row;
	assert(nd_playcatalog_read(c, 16999, &row) && !strcmp(row.row.id, "remote-song-16999") && !strcmp(row.album_id, "remote-album"));
	no_child_handles();
	nd_search_t *search = nd_search_create("Unrelated query");
	nd_search_destroy(search);
	assert(nd_playcatalog_read(c, 0, &row));
	nd_playcatalog_free(c);
	q.search[0] = 0;
	snprintf(q.filter, sizeof(q.filter), "new-album");
	c = nd_playcatalog_capture(&lib, &identity, &q, "new-song-0", capture_current, &identity, &error);
	assert(c && !error && nd_playcatalog_count(c) == 12 && nd_playcatalog_start(c) == 11);
	assert(nd_playcatalog_read(c, 0, &row) && !strcmp(row.row.id, "new-song-11") && !strcmp(row.album_id, "new-album"));
	nd_playcatalog_free(c);
	q.filter[0] = 0;
	snprintf(q.search, sizeof(q.search), "repeat");
	c = nd_playcatalog_capture(&lib, &identity, &q, "remote-song-00000", capture_current, &identity, &error);
	assert(!c && error && !strcmp(error, "nd_sync_inconsistent"));
	no_child_handles();
	assert(wait_library().generation == lib.generation);
	puts("Captured server queue: 17000 ordered matches, exact start/album metadata, complete disc-order album, duplicate-page rejection, independent search lifetime and no retained descriptors: passed");
}
static void large(const char *q, nd_kind_t kind, int total) {
	nd_search_t *s = nd_search_create(q);
	assert(s);
	nd_search_result_t r;
	int first = 0;
	for (int requests = 0; requests < 2000; requests++) {
		nd_search_read(s, kind, first, &r);
		assert(!r.error && !r.reason && r.source == ND_SEARCH_SERVER);
		assert(r.count <= ND_WINDOW && r.first + r.count <= total);
		if (!r.more)
			break;
		assert(r.total >= first && r.total < total + ND_WINDOW);
		first = r.total;
	}
	assert(!r.more && r.total == total);
	nd_search_read(s, kind, 0, &r);
	assert(r.first == 0 && r.count == ND_WINDOW && r.total == total && !r.more);
	nd_search_destroy(s);
}
static nd_search_t *delayed;
static nd_search_result_t delayed_result;
static void *delayed_read(void *unused) {
	(void)unused;
	nd_search_read(delayed, ND_TRACKS, 0, &delayed_result);
	return NULL;
}
int main(int argc, char **argv) {
	assert(argc == 3);
	alarm(120);
	snprintf(origin, sizeof(origin), "%s", argv[1]);
	assert(!chdir(argv[2]));
	snprintf(config_file, sizeof(config_file), "%s/config.ini", argv[2]);
	char host[600];
	snprintf(host, sizeof(host), "%s/base", origin);
	navidrome_init();
	navidrome_set_root("");
	assert(navidrome_save(host, "fixture user", "synthetic-server-password", NULL));
	assert(wait_idle().state == ND_CONNECTED);
	wait_library();
	nd_search_result_t result = read_first("empty", ND_TRACKS);
	assert(!result.error && !result.reason && !result.count && result.source == ND_SEARCH_SERVER);
	for (int k = ND_ALBUMS; k <= ND_ARTISTS; k++) {
		result = read_first("s", (nd_kind_t)k);
		assert(!result.error && !result.reason && !result.count && result.total == 0 && result.source == ND_SEARCH_SERVER);
	}
	result = read_first("so", ND_TRACKS);
	assert(!result.error && result.count == ND_WINDOW && result.source == ND_SEARCH_SERVER);
	result = read_first("%_\\ Björk QUERYSECRET", ND_ARTISTS);
	assert(!result.error && result.count == 1);
	result = read_first("broken", ND_TRACKS);
	assert(result.error && result.reason && !result.count);
	result = read_first("reduce", ND_TRACKS);
	assert(!result.error && result.count == 6 && result.total == 6 && !result.more);
	nd_context_t prior_connection;
	assert(navidrome_context(&prior_connection, false));
	nd_wipe(prior_connection.password, sizeof(prior_connection.password));
	reconnect();
	assert(!navidrome_search_current(&prior_connection, true) && navidrome_search_current(&prior_connection, false));
	navidrome_search_failed(&prior_connection, "nd_network_error");
	assert(wait_idle().state == ND_CONNECTED);
	large("Track", ND_TRACKS, 17000);
	large("Album", ND_ALBUMS, 1700);
	result = read_first("exact", ND_TRACKS);
	assert(result.more && result.total == ND_WINDOW);
	nd_search_t *s = nd_search_create("exact");
	assert(s);
	nd_search_read(s, ND_TRACKS, 0, &result);
	nd_search_read(s, ND_TRACKS, ND_WINDOW, &result);
	assert(!result.more && result.total == ND_WINDOW && result.count == ND_WINDOW);
	nd_search_destroy(s);
	result = read_first("blank", ND_TRACKS);
	assert(!result.error);
	result = read_first(" \t", ND_TRACKS);
	assert(result.source == ND_SEARCH_NONE && !result.count);
	char too_long[257];
	memset(too_long, 'a', 256);
	too_long[256] = 0;
	assert(!nd_search_create(too_long));
	nd_context_t stamp;
	assert(navidrome_context(&stamp, false));
	nd_wipe(stamp.password, sizeof(stamp.password));
	navidrome_set_root(argv[2]);
	nd_library_t lib = wait_library();
	assert(lib.writable && *lib.scope);
	seed(&lib);
	navidrome_set_root(argv[2]);
	lib = wait_library();
	assert(lib.ready);
	captures(lib);
	s = nd_search_create("eligible");
	assert(s);
	nd_search_read(s, ND_ARTISTS, 0, &result);
	assert(!result.error && result.count == 16 && result.total == 16 && result.more);
	nd_search_read(s, ND_ARTISTS, 16, &result);
	assert(!result.error && result.count == 11 && result.total == 27 && !result.more);
	nd_search_read(s, ND_ARTISTS, 5, &result);
	assert(!result.error && result.count == 16 && result.total == 27 && !strcmp(result.rows[0].id, "remote-artist-00015"));
	nd_search_destroy(s);
	assert(navidrome_search_current(&stamp, true) && !navidrome_context_current(&stamp, false));
	assert(navidrome_sync());
	control("sync-wait");
	result = read_first("auth", ND_TRACKS);
	assert(!result.error && result.source == ND_SEARCH_SYNCED && result.reason);
	assert(!wait_idle().busy);
	reconnect();
	s = nd_search_create("Album");
	assert(s);
	nd_search_child(s, 1, ND_ARTISTS, "new-artist", &result);
	assert(!result.error && result.count == ND_WINDOW && result.total == 100 && result.source == ND_SEARCH_SERVER);
	nd_search_child_read(s, 1, 96, &result);
	assert(!result.error && result.count == 4 && result.total == 100);
	no_child_handles();
	navidrome_set_root("");
	wait_library();
	nd_search_child_read(s, 1, 0, &result);
	assert(result.error && !result.count && nd_search_valid(s));
	navidrome_set_root(argv[2]);
	wait_library();
	nd_search_child(s, 2, ND_ALBUMS, "new-album", &result);
	assert(!result.error && result.total == 12 && result.rows[0].disc == 1 && result.rows[0].track == 1);
	nd_search_child(s, 1, ND_ALBUMS, "album-00000", &result);
	assert(!result.error && result.reason && result.source == ND_SEARCH_SYNCED && result.total == 10);
	nd_search_child(s, 1, ND_ALBUMS, "missing", &result);
	assert(result.error && !result.count);
	nd_search_child(s, 1, ND_ARTISTS, "oversized", &result);
	assert(result.error && !result.count);
	nd_search_destroy(s);
	const char *failures[] = {"broken", "html", "invalid", "depth", "longid", "close"};
	for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
		result = read_first(failures[i], ND_TRACKS);
		assert(result.source == ND_SEARCH_SYNCED && !result.error && result.reason);
		reconnect();
	}
	control("fail");
	result = read_first("Track", ND_TRACKS);
	assert(!result.error && result.reason && result.reset && result.source == ND_SEARCH_SYNCED && result.totals[ND_TRACKS] == 17000);
	control("ok");
	s = nd_search_create("Track");
	assert(s);
	nd_search_read(s, ND_TRACKS, 0, &result);
	assert(result.source == ND_SEARCH_SERVER);
	control("fail");
	nd_search_read(s, ND_TRACKS, ND_WINDOW, &result);
	assert(result.source == ND_SEARCH_SYNCED && !result.error && result.reason && result.first == 0);
	control("ok");
	nd_search_read(s, ND_ALBUMS, 0, &result);
	assert(result.source == ND_SEARCH_SYNCED);
	nd_search_destroy(s);
	result = read_first("auth", ND_TRACKS);
	assert(result.source == ND_SEARCH_SYNCED && result.reason && !strcmp(result.reason, "nd_auth_error"));
	reconnect();
	result = read_first("repeat", ND_TRACKS);
	assert(result.source == ND_SEARCH_SERVER);
	s = nd_search_create("repeat");
	assert(s);
	nd_search_read(s, ND_TRACKS, 0, &result);
	nd_search_read(s, ND_TRACKS, ND_WINDOW, &result);
	assert(result.source == ND_SEARCH_SYNCED && result.reason);
	nd_search_destroy(s);
	result = read_first("tokensingle", ND_TRACKS);
	assert(result.source == ND_SEARCH_SYNCED && result.reason);
	navidrome_test_online(0);
	navidrome_poll_network();
	result = read_first("TRACK", ND_TRACKS);
	assert(!result.error && result.total == 17000 && !result.more && result.source == ND_SEARCH_SYNCED);
	result = read_first("T", ND_TRACKS);
	assert(!result.error && result.total == 17000 && result.source == ND_SEARCH_SYNCED);
	s = nd_search_create("Track");
	assert(s);
	nd_search_read(s, ND_TRACKS, 0, &result);
	nd_search_read(s, ND_TRACKS, 16995, &result);
	assert(!result.error && result.count == 5 && result.first == 16995);
	nd_search_destroy(s);
	result = read_first("Album", ND_ALBUMS);
	assert(result.total == 1700);
	s = nd_search_create("Track");
	assert(s);
	reconnect();
	nd_search_read(s, ND_TRACKS, 0, &result);
	assert(result.source == ND_SEARCH_SYNCED && !result.error && nd_search_valid(s));
	nd_search_destroy(s);
	navidrome_test_online(0);
	navidrome_poll_network();
	char pattern[515], folded[512];
	assert(library_search_pattern("%_\\", pattern, sizeof(pattern)) && !strcmp(pattern, "%\\%\\_\\\\%"));
	library_fold_text("BJÖRK КИНО", folded, sizeof(folded));
	assert(!strcmp(folded, "bjork кино"));
	nd_store_t store;
	assert(nd_store_open(&store, lib.path, lib.scope, true, false));
	assert(sqlite3_exec(store.db, "UPDATE entity SET title='Björk %_\\',sortkey='bjork' WHERE kind=2;", NULL, NULL, NULL) == SQLITE_OK);
	assert(sqlite3_exec(store.db, "INSERT INTO entity SELECT gen,kind,'second-artist',title,artist,album,cover,duration,disc,track,sortkey FROM entity WHERE kind=2 LIMIT 1", NULL, NULL, NULL) == SQLITE_OK);
	assert(sqlite3_exec(store.db, "INSERT INTO rel SELECT gen,album,kind,'second-artist' FROM rel WHERE kind=2 LIMIT 1", NULL, NULL, NULL) == SQLITE_OK);
	nd_store_close(&store);
	result = read_first("bjork", ND_ARTISTS);
	assert(result.total == 2 && strcmp(result.rows[0].id, result.rows[1].id));
	result = read_first("%_\\", ND_ARTISTS);
	assert(result.total == 2);
	assert(!chmod(argv[2], 0500));
	navidrome_set_root(argv[2]);
	lib = wait_library();
	assert(lib.ready && !lib.writable);
	result = read_first("Track", ND_TRACKS);
	assert(!result.error && result.total == 17000);
	reconnect();
	s = nd_search_create("Album");
	assert(s);
	nd_search_child(s, 1, ND_ARTISTS, "new-artist", &result);
	assert(result.error && !result.count);
	nd_search_destroy(s);
	assert(!chmod(argv[2], 0700));
	navidrome_set_root("");
	wait_library();
	s = nd_search_create("Album");
	assert(s);
	nd_search_child(s, 1, ND_ALBUMS, "new-album", &result);
	assert(!result.error && result.total == 12);
	nd_search_destroy(s);
	assert(navidrome_encrypt("vault1"));
	assert(wait_idle().encrypted);
	delayed = nd_search_create("slow");
	assert(delayed);
	pthread_t worker;
	assert(!pthread_create(&worker, NULL, delayed_read, NULL));
	usleep(50000);
	navidrome_lock();
	pthread_join(worker, NULL);
	assert(delayed_result.error && !delayed_result.count && !nd_search_valid(delayed));
	nd_search_destroy(delayed);
	navidrome_shutdown();
	puts("Server paging (17000 tracks/1700 albums), exact counts, reduced pages, source/fallback, literal Unicode matching, no-card/read-only operation, exact-ID children, limits and stale profile rejection: passed");
	return 0;
}

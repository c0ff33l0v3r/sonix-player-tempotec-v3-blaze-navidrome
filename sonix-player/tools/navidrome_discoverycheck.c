// Share real service/network/config setup; run Discovery-specific scenarios below.
#define main search_suite_main
#include "navidrome_searchcheck.c"
#undef main
#include "src/system/streaming/navidromecache.h"
#include "src/system/streaming/navidromeplaylists.h"
#include "src/system/streaming/navidromediscovery.h"
#include <stdatomic.h>
static int fail_remote_append;
bool __real_nd_pl_append(nd_playlist_store_t *, int64_t, const nd_row_t *, bool, int *, int *);
bool __wrap_nd_pl_append(nd_playlist_store_t *s, int64_t id, const nd_row_t *row, bool dedupe, int *added, int *skipped) {
	if (fail_remote_append && !--fail_remote_append)
		return false;
	return __real_nd_pl_append(s, id, row, dedupe, added, skipped);
}
static atomic_bool slow_permitted;
static nd_search_result_t slow_result;
static bool slow_current(void *arg) {
	(void)arg;
	return atomic_load(&slow_permitted);
}
static void *slow_read(void *arg) {
	nd_discovery_read(arg, 0, &slow_result);
	return NULL;
}
static void no_discovery_handles(void) {
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
		assert(!strstr(target, "/discover-"));
	}
	closedir(dir);
	assert(!nd_cache_io_count());
}
static bool allow(void *arg) { return *(bool *)arg; }
static void fresh(void) {
	control("normal");
	reconnect();
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
	nd_search_result_t r;
	for (int mode = 0; mode < ND_DISCOVERY_MODES; mode++) {
		nd_discovery_t *s = nd_discovery_create(mode);
		assert(s);
		nd_discovery_read(s, 0, &r);
		assert(!r.error && r.count == ND_WINDOW && r.source == ND_SEARCH_SERVER);
		char id[ND_ID_MAX];
		snprintf(id, sizeof(id), "%s", r.rows[0].id);
		if (mode == ND_DISCOVERY_RANDOM) {
			assert(r.total == 50 && !r.more);
			nd_discovery_read(s, 32, &r);
			assert(!r.error && r.first == 32 && r.count == ND_WINDOW);
			nd_discovery_read(s, 0, &r);
			assert(!strcmp(r.rows[0].id, id));
		} else {
			int first = r.count;
			while (r.more) {
				nd_discovery_read(s, first, &r);
				assert(!r.error);
				first += r.count;
			}
			assert(first == 1700 && r.total == 1700);
			nd_discovery_read(s, 0, &r);
			assert(!r.error && r.count == ND_WINDOW && !strcmp(r.rows[0].id, id));
		}
		nd_discovery_album_open(s, "small", &r);
		assert(!r.error && r.total == 12);
		nd_discovery_album_read(s, 0, 108, 44, true, &r);
		assert(!r.error && r.alternate && r.album.extent > 0);
		nd_discovery_album_open(s, "wrong", &r);
		assert(r.error);
		nd_discovery_album_open(s, "large", &r);
		assert(r.error && !strcmp(r.error, "nd_cache_storage"));
		nd_discovery_free(s);
	}
	puts("Discovery: four authenticated types, 1700-album paging/backscroll, stable Random, exact unsynced children and absent-card bounds passed");
	const char *failures[] = {"duplicate", "repeat", "invalid", "malformed", "depth", "auth", "api", "oversize-one"};
	for (unsigned i = 0; i < sizeof(failures) / sizeof(*failures); i++) {
		fresh();
		control(failures[i]);
		nd_discovery_t *s = nd_discovery_create(ND_DISCOVERY_NEWEST);
		assert(s);
		nd_discovery_read(s, 0, &r);
		if (!strcmp(failures[i], "repeat")) {
			assert(!r.error);
			nd_discovery_read(s, ND_WINDOW, &r);
		}
		assert(r.error);
		nd_discovery_free(s);
	}
	fresh();
	control("reduce");
	nd_discovery_t *s = nd_discovery_create(ND_DISCOVERY_NEWEST);
	assert(s);
	nd_discovery_read(s, 0, &r);
	assert(!r.error && r.count == ND_WINDOW);
	nd_discovery_free(s);
	fresh();
	control("multiple");
	s = nd_discovery_create(ND_DISCOVERY_NEWEST);
	nd_discovery_read(s, 0, &r);
	assert(!r.error && r.more);
	nd_discovery_read(s, 16, &r);
	assert(!r.error && r.more);
	nd_discovery_read(s, 32, &r);
	assert(!r.error && !r.more && r.total == 32 && !r.count);
	bool ok = false;
	nd_discovery_cancel(s, allow, &ok);
	nd_discovery_read(s, 0, &r);
	assert(r.error);
	nd_discovery_free(s);
	fresh();
	control("empty");
	s = nd_discovery_create(ND_DISCOVERY_RANDOM);
	nd_discovery_read(s, 0, &r);
	assert(!r.error && !r.count && !r.total && !r.more);
	nd_discovery_free(s);
	fresh();
	control("small-random");
	s = nd_discovery_create(ND_DISCOVERY_RANDOM);
	nd_discovery_read(s, 0, &r);
	assert(!r.error && r.total == 3);
	nd_discovery_read(s, 0, &r);
	assert(!r.error && r.total == 3);
	nd_discovery_free(s);
	fresh();
	control("slow");
	s = nd_discovery_create(ND_DISCOVERY_RECENT);
	atomic_store(&slow_permitted, true);
	nd_discovery_cancel(s, slow_current, NULL);
	pthread_t reader;
	assert(!pthread_create(&reader, NULL, slow_read, s));
	usleep(20000);
	atomic_store(&slow_permitted, false);
	pthread_join(reader, NULL);
	assert(slow_result.error && !slow_result.count);
	nd_discovery_free(s);
	puts("Discovery: size reduction, exact-page termination, malformed/resource/API/auth failures, finite repeats, empty/small Random and cancellation passed");
	fresh();
	char card[1200];
	snprintf(card, sizeof(card), "%s/card", argv[2]);
	mkdir(card, 0700);
	navidrome_set_root(card);
	nd_library_t lib = wait_library();
	s = nd_discovery_create(ND_DISCOVERY_RECENT);
	nd_discovery_album_open(s, "large", &r);
	assert(!r.error && r.total == 65 && r.card_backed);
	nd_discovery_album_read(s, 60, 108, 44, false, &r);
	assert(!r.error && r.first == 60 && r.count == 5);
	nd_search_t *search = nd_search_create("empty");
	nd_search_destroy(search); // Search cleanup cannot remove discover-* staging.
	nd_discovery_album_read(s, 0, 108, 44, true, &r);
	assert(!r.error && r.alternate);
	nd_context_t identity;
	assert(navidrome_context(&identity, false));
	nd_wipe(identity.password, sizeof(identity.password));
	nd_query_t q = {.kind = ND_TRACKS, .filter_kind = ND_ALBUMS};
	snprintf(q.filter, sizeof(q.filter), "large");
	const char *error = NULL;
	nd_playcatalog_t *c = nd_playcatalog_capture(&lib, &identity, &q, "track-64", capture_current, &identity, &error);
	assert(c && !error && nd_playcatalog_count(c) == 65 && nd_playcatalog_start(c) == 64);
	nd_discovery_free(s);
	nd_playrow_t row;
	assert(nd_playcatalog_read(c, 64, &row));
	nd_playcatalog_free(c);

	c = nd_playcatalog_capture(&lib, &identity, &q, NULL, capture_current, &identity, &error);
	assert(c && !error && nd_playcatalog_count(c) == 65 && nd_playcatalog_start(c) == 0);
	assert(nd_playcatalog_read(c, 0, &row) && !strcmp(row.row.id, "track-0"));
	nd_playcatalog_free(c);
	c = nd_playcatalog_capture(&lib, &identity, &q, "removed-track", capture_current, &identity, &error);
	assert(!c && error);
	// Row-menu destination additions use complete unsynced membership atomically.
	nd_pl_result_t *playlist = calloc(1, sizeof(*playlist));
	assert(playlist);
	nd_pl_request_t add = {.action = NDPL_CREATE};
	snprintf(add.name, sizeof(add.name), "Discovery fixture");
	nd_pl_execute(&add, &identity, &lib, playlist);
	assert(!playlist->error && playlist->playlist.id > 0);
	add = (nd_pl_request_t){.action = NDPL_ADD, .id = playlist->playlist.id, .revision = 1, .remote_album = true};
	add.source.profile = identity.profile_epoch;
	add.source.card = identity.card_epoch;
	snprintf(add.source.scope, sizeof(add.source.scope), "%s", lib.scope);
	snprintf(add.source.row.id, sizeof(add.source.row.id), "large");
	nd_pl_execute(&add, &identity, &lib, playlist);
	assert(!playlist->error && playlist->added == 65 && playlist->skipped == 0);
	add.revision = 2;
	nd_pl_execute(&add, &identity, &lib, playlist);
	assert(!playlist->error && playlist->added == 0 && playlist->skipped == 65);
	snprintf(add.source.row.id, sizeof(add.source.row.id), "wrong");
	nd_pl_execute(&add, &identity, &lib, playlist);
	assert(playlist->error);
	snprintf(add.source.row.id, sizeof(add.source.row.id), "append-fault");
	fail_remote_append = 9;
	nd_pl_execute(&add, &identity, &lib, playlist);
	assert(playlist->error && !fail_remote_append);
	nd_pl_request_t entries = {.action = NDPL_ENTRIES, .id = add.id};
	nd_pl_execute(&entries, &identity, &lib, playlist);
	assert(!playlist->error && playlist->playlist.count == 65 && playlist->playlist.revision == 2);
	assert(!strcmp(playlist->entries[0].track.id, "track-0"));
	add.source.profile++;
	nd_pl_execute(&add, &identity, &lib, playlist);
	assert(playlist->error);
	free(playlist);
	puts("Discovery shared actions: whole 65-track capture, unsynced playlist append/dedupe/rollback and stale identity guards passed");
	bool cancelled = false;
	c = nd_playcatalog_capture(&lib, &identity, &q, "track-0", allow, &cancelled, &error);
	assert(!c && error);
	// New private child files never retain descriptors or survive owner cleanup.
	no_child_handles();
	no_discovery_handles();
	s = nd_discovery_create(ND_DISCOVERY_RECENT);
	nd_discovery_album_open(s, "large", &r);
	assert(!r.error && r.card_backed);
	assert(nd_cache_maintenance_begin());
	nd_cache_maintenance_end();
	nd_discovery_album_read(s, 0, 108, 44, false, &r);
	assert(r.error && !r.count);
	nd_discovery_free(s);
	assert(!chmod(card, 0500));
	navidrome_set_root(card);
	lib = wait_library();
	assert(!lib.writable);
	s = nd_discovery_create(ND_DISCOVERY_NEWEST);
	nd_discovery_album_open(s, "boundary", &r);
	assert(!r.error && r.total == 64 && !r.card_backed);
	nd_discovery_album_open(s, "large", &r);
	assert(r.error && !r.count);
	nd_discovery_album_open(s, "oversized", &r);
	assert(r.error && !r.count);
	nd_discovery_free(s);
	assert(!chmod(card, 0700));
	navidrome_set_root(card);
	lib = wait_library();
	s = nd_discovery_create(ND_DISCOVERY_NEWEST);
	nd_discovery_album_open(s, "large", &r);
	assert(!r.error);
	navidrome_set_root("");
	wait_library();
	nd_discovery_album_read(s, 0, 108, 44, false, &r);
	assert(r.error && !r.count);
	nd_discovery_free(s);
	puts("Discovery: exact 64/65 read-only boundary, oversized album, cache epoch revocation and card detach passed");
	puts("Discovery: staged 65-track child, alternate layout, independent Search cleanup and complete playback capture survive session retirement: passed");
	navidrome_shutdown();
	return 0;
}

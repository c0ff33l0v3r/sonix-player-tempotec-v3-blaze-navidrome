// Reuse the real HTTP/vault fixture helpers; all credentials are synthetic.
#define main unused_library_fixture_main
#include "navidrome_librarycheck.c"
#undef main
#include "src/system/streaming/navidromeplayliststore.h"
#include "src/system/streaming/navidromestars.h"
#include <signal.h>
#include <stdatomic.h>

static nd_library_t favorites(void) {
	nd_library_t result;
	navidrome_favorites_snapshot(&result);
	return result;
}

static nd_library_t refresh_ok(int albums, int songs) {
	assert(navidrome_favorites_refresh());
	assert(!navidrome_sync() && !navidrome_favorites_refresh() && !navidrome_reconnect());
	idle();
	nd_library_t result = favorites();
	assert(result.favorites && result.ready && !result.syncing && !result.error);
	assert(result.counts[ND_ALBUMS] == albums && result.counts[ND_TRACKS] == songs);
	return result;
}

static void query_favorites(nd_library_t cache, nd_kind_t kind, const char *album, int offset, int expected) {
	nd_store_t store;
	assert(nd_store_open_source(&store, cache.path, cache.scope, false, false, true));
	nd_query_t query = {.kind = kind, .favorites = true, .filter_kind = ND_ALBUMS};
	if (album)
		snprintf(query.filter, sizeof(query.filter), "%s", album);
	nd_row_t rows[ND_WINDOW];
	int count, total;
	assert(nd_store_query(&store, cache.generation, &query, offset, rows, &count, &total));
	assert(total == expected && count == (expected - offset > ND_WINDOW ? ND_WINDOW : expected - offset));
	if (kind == ND_TRACKS && !album)
		for (int i = 0; i < count; i++)
			assert(rows[i].favorite);
	if (kind == ND_ALBUMS)
		for (int i = 0; i < count; i++)
			assert(rows[i].favorite);
	for (int i = 1; album && i < count; i++)
		assert(rows[i - 1].disc < rows[i].disc || (rows[i - 1].disc == rows[i].disc && rows[i - 1].track <= rows[i].track));
	if (kind == ND_TRACKS && !album && expected == 3) {
		assert(!strcmp(rows[0].id, "song-00002"));
		assert(!strcmp(rows[1].id, "song-00011"));
		assert(!strcmp(rows[2].id, "song-00025"));
	}
	if (kind == ND_TRACKS && expected == 17000 && offset == 16984)
		assert(!strcmp(rows[ND_WINDOW - 1].id, "song-16999"));
	query.favorites = false;
	assert(!nd_store_query(&store, cache.generation, &query, 0, rows, &count, &total));
	nd_store_close(&store);
}

static void unchanged(nd_library_t old) {
	nd_library_t now = favorites();
	assert(now.ready && now.generation == old.generation && now.finished == old.finished);
	assert(now.counts[0] == old.counts[0] && now.counts[1] == old.counts[1]);
}

static void reload(const char *root) {
	navidrome_set_root(root);
	for (int i = 0; i < 10000; i++) {
		nd_library_t f = favorites(), l = library();
		if (!f.loading && !l.loading)
			return;
		usleep(1000);
	}
	assert(!"cache reload timeout");
}

static void cache_private_dir(const char *dir) {
	DIR *directory = opendir(dir);
	assert(directory);
	struct dirent *entry;
	while ((entry = readdir(directory))) {
		if (entry->d_name[0] == '.')
			continue;
		char path[1500];
		snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
		struct stat info;
		assert(!lstat(path, &info));
		if (!S_ISREG(info.st_mode))
			continue;
		FILE *file = fopen(path, "rb");
		assert(file);
		unsigned char bytes[8192 + 128];
		size_t carry = 0, amount;
		while ((amount = fread(bytes + carry, 1, 8192, file))) {
			size_t size = carry + amount;
			const char *forbidden[] = {"synthetic-server-password", "vault1", "?u=", "&t=", "subsonic-response"};
			for (size_t k = 0; k < sizeof(forbidden) / sizeof(*forbidden); k++) {
				size_t n = strlen(forbidden[k]);
				for (size_t i = 0; i + n <= size; i++)
					assert(memcmp(bytes + i, forbidden[k], n));
			}
			carry = size < 128 ? size : 128;
			memmove(bytes, bytes + size - carry, carry);
		}
		assert(!ferror(file));
		fclose(file);
	}
	closedir(directory);
}

static void cache_private(const char *root) {
	char dir[1200];
	const char *leaves[] = {".local/navidrome", ".local/navidrome/.cache"};
	for (int i = 0; i < 2; i++) {
		snprintf(dir, sizeof(dir), "%s/%s", root, leaves[i]);
		cache_private_dir(dir);
	}
}

static void favorite_migration(nd_library_t original, const char *root) {
	char legacy[1200];
	snprintf(legacy, sizeof(legacy), "%s/.local/navidrome/favorites.db", root);
	reload(NULL);
	assert(!rename(original.path, legacy) && !chmod(root, 0555));
	reload(root);
	nd_library_t old = favorites();
	assert(old.ready && !old.writable && !strcmp(old.path, legacy));
	query_favorites(old, ND_TRACKS, NULL, 0, 3);
	assert(!chmod(root, 0700));
	reload(NULL);
	reload(root);
	nd_library_t migrated = favorites();
	assert(migrated.ready && migrated.writable && !strcmp(migrated.path, original.path));
	assert(access(legacy, F_OK) && migrated.generation == original.generation && migrated.finished == original.finished);
	puts("Favorites legacy read-only access and detached-card migration to the disposable cache root: passed");
}

static void incremental_store_checks(const char *root) {
	char path[1200];
	snprintf(path, sizeof(path), "%s/incremental-fixture.db", root);
	nd_store_t store = {0};
	nd_library_t cache = {0};
	assert(nd_store_open_source(&store, path, "incremental-scope", true, false, true));
	nd_playrow_t track = {0};
	snprintf(track.row.id, sizeof(track.row.id), "exact-track");
	snprintf(track.row.title, sizeof(track.row.title), "Persistent Track");
	snprintf(track.album_id, sizeof(track.album_id), "exact-album");
	assert(nd_store_local_begin(&store) && nd_store_local_put(&store, ND_TRACKS, &track, true, 0) && nd_store_local_activate(&store));
	assert(sqlite3_exec(store.db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK && nd_store_status(&store, &cache));
	assert(cache.ready && !cache.base && !cache.finished && cache.counts[ND_TRACKS] == 1);
	int64_t pinned = cache.generation;
	assert(nd_store_retain(store.scope, pinned));
	nd_query_t query = {.kind = ND_TRACKS, .favorites = true};
	int64_t *handles = NULL;
	int count, start;
	assert(nd_store_play_index(&store, pinned, &query, "exact-track", &handles, &count, &start) && count == 1 && handles[0] < 0);
	nd_playrow_t album = {0};
	snprintf(album.row.id, sizeof(album.row.id), "exact-album");
	snprintf(album.row.title, sizeof(album.row.title), "Loading Album");
	assert(nd_store_local_begin(&store) && nd_store_local_put(&store, ND_TRACKS, &track, false, 0) && nd_store_local_put(&store, ND_ALBUMS, &album, true, 1) && nd_store_local_put(&store, ND_ARTISTS, &album, true, 0) && nd_store_local_activate(&store));
	assert(sqlite3_exec(store.db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK && nd_store_status(&store, &cache));
	assert(cache.counts[ND_TRACKS] == 0 && cache.counts[ND_ALBUMS] == 1 && !cache.finished);
	nd_store_cleanup(&store);
	nd_playrow_t captured;
	assert(nd_store_play_row(&store, pinned, handles[0], &captured) && !strcmp(captured.row.title, "Persistent Track"));
	assert(nd_store_metadata(&store, cache.generation, ND_ALBUMS, "exact-album", &captured) && captured.row.details == 1);
	bool star = false;
	assert(nd_store_annotation(&store, cache.generation, ND_ARTISTS, "exact-album", &star) && star);
	assert(nd_store_begin(&store));
	int64_t candidate = store.generation;
	assert(nd_store_local_begin(&store) && nd_store_local_put(&store, ND_TRACKS, &track, true, 0) && nd_store_local_activate(&store));
	assert(sqlite3_exec(store.db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
	store.generation = candidate;
	assert(nd_store_validate(&store) && nd_store_publish(&store) && nd_store_status(&store, &cache));
	assert(cache.base == candidate && cache.counts[ND_TRACKS] == 1 && cache.counts[ND_ALBUMS] == 0 && cache.finished);
	// A later successful full Refresh supersedes the covered old observation.
	assert(nd_store_begin(&store) && nd_store_validate(&store) && nd_store_publish(&store) && nd_store_status(&store, &cache));
	assert(cache.counts[ND_TRACKS] == 0 && cache.counts[ND_ALBUMS] == 0);
	free(handles);
	nd_store_release(store.scope, pinned);
	nd_store_cleanup(&store);
	nd_store_close(&store);
	assert(nd_store_open_source(&store, path, "incremental-scope", false, false, true) && nd_store_status(&store, &cache));
	assert(cache.counts[ND_TRACKS] == 0 && cache.counts[ND_ALBUMS] == 0 && cache.finished);
	assert(nd_store_annotation(&store, cache.generation, ND_ARTISTS, "exact-album", &star) && star);
	nd_store_close(&store);
	assert(!unlink(path));
	// Version 2 remains readable on a legacy card and upgrades without rebuilding.
	assert(nd_store_open_source(&store, path, "incremental-scope", true, false, true) && nd_store_begin(&store));
	char legacy[500];
	snprintf(legacy, sizeof(legacy), "INSERT INTO entity VALUES(%lld,0,'legacy-album','Legacy Album','','legacy-album','',0,0,0,'legacy'); INSERT INTO favorite_album VALUES(%lld,'legacy-album',0)", (long long)store.generation, (long long)store.generation);
	assert(sqlite3_exec(store.db, legacy, NULL, NULL, NULL) == SQLITE_OK && nd_store_validate(&store) && nd_store_publish(&store) && nd_store_status(&store, &cache));
	int64_t legacy_generation = cache.generation;
	time_t legacy_finished = cache.finished;
	assert(sqlite3_exec(store.db, "DROP TABLE local_view; DROP TABLE local_change; DROP TABLE artist_state; PRAGMA user_version=2", NULL, NULL, NULL) == SQLITE_OK);
	nd_store_close(&store);
	assert(nd_store_open_source(&store, path, "incremental-scope", false, false, true) && nd_store_status(&store, &cache) && cache.counts[ND_ALBUMS] == 1 && cache.generation == legacy_generation);
	nd_store_close(&store);
	assert(nd_store_open_source(&store, path, "another-scope", false, false, true) && !nd_store_annotation(&store, legacy_generation, ND_ALBUMS, "legacy-album", &star));
	nd_store_close(&store);
	assert(nd_store_open_source(&store, path, "incremental-scope", true, false, true) && store.local && nd_store_status(&store, &cache) && cache.counts[ND_ALBUMS] == 1 && cache.generation == legacy_generation && cache.finished == legacy_finished);
	nd_store_close(&store);
	assert(!unlink(path));
	puts("Incremental store: initial known entries, durable flags/artists, incomplete albums, negative source handles and pinned metadata: passed");
}

static atomic_bool star_delivered;
static nd_star_result_t star_result;
static void star_done(nd_star_result_t *result, void *unused) {
	(void)unused;
	star_result = *result;
	for (size_t i = 0; i < sizeof(result->context.password); i++)
		assert(!result->context.password[i]);
	free(result);
	atomic_store(&star_delivered, true);
}
static void star_wait(void) {
	for (int i = 0; i < 10000; i++) {
		if (!navidrome_star_busy())
			break;
		usleep(1000);
	}
	idle();
	assert(!navidrome_star_busy() && atomic_load(&star_delivered));
}
static void star_request(nd_star_action_t action, nd_kind_t kind, const char *id) {
	nd_star_request_t q = {.action = action, .kind = kind};
	snprintf(q.id, sizeof(q.id), "%s", id);
	atomic_store(&star_delivered, false);
	assert(navidrome_star_submit(&q, star_done, NULL));
	star_wait();
}
static void star_checks(nd_library_t cached, const char *root) {
	const char *id = "same opaque&entity/?:label";
	for (int kind = 0; kind < 3; kind++) {
		assert(navidrome_star_state(kind, id) == ND_STAR_UNKNOWN);
		star_request(ND_STAR_CHECK, kind, id);
		assert(!star_result.error && navidrome_star_state(kind, id) == ND_STAR_CLEAR);
		star_request(ND_STAR_ADD, kind, id);
		assert(!star_result.error && navidrome_star_state(kind, id) == ND_STAR_SET);
		for (int other = kind + 1; other < 3; other++)
			assert(navidrome_star_state(other, id) == ND_STAR_UNKNOWN);
		star_request(ND_STAR_REMOVE, kind, id);
		assert(!star_result.error && navidrome_star_state(kind, id) == ND_STAR_CLEAR);
	}
	star_request(ND_STAR_TOGGLE, ND_TRACKS, "first-use-toggle");
	assert(!star_result.error && navidrome_star_state(ND_TRACKS, "first-use-toggle") == ND_STAR_SET);
	star_request(ND_STAR_TOGGLE, ND_TRACKS, "first-use-toggle");
	assert(!star_result.error && navidrome_star_state(ND_TRACKS, "first-use-toggle") == ND_STAR_CLEAR);
	mode("star_reject");
	star_request(ND_STAR_ADD, ND_TRACKS, id);
	assert(star_result.error && !star_result.uncertain && !star_result.completed);
	assert(navidrome_star_state(ND_TRACKS, id) == ND_STAR_CLEAR);
	mode("star_badstate");
	star_request(ND_STAR_CHECK, ND_ARTISTS, "bad-starred");
	assert(star_result.error && navidrome_star_state(ND_ARTISTS, "bad-starred") == ND_STAR_UNKNOWN);
	mode("star_wrongid");
	star_request(ND_STAR_CHECK, ND_ALBUMS, "not-the-response-id");
	assert(star_result.error && navidrome_star_state(ND_ALBUMS, "not-the-response-id") == ND_STAR_UNKNOWN);
	mode("star_lost");
	star_request(ND_STAR_ADD, ND_TRACKS, id);
	assert(star_result.error && star_result.uncertain && navidrome_star_state(ND_TRACKS, id) == ND_STAR_UNCERTAIN);
	mode("normal");
	star_request(ND_STAR_TOGGLE, ND_TRACKS, id); // Explicit recheck only after an uncertain write.
	assert(!star_result.error && star_result.request.action == ND_STAR_CHECK && navidrome_star_state(ND_TRACKS, id) == ND_STAR_SET);
	star_request(ND_STAR_REMOVE, ND_TRACKS, id);
	assert(!star_result.error && navidrome_star_state(ND_TRACKS, id) == ND_STAR_CLEAR);
	mode("star_badresponse");
	star_request(ND_STAR_ADD, ND_TRACKS, "malformed-success");
	assert(star_result.error && star_result.uncertain);
	mode("normal");
	star_request(ND_STAR_TOGGLE, ND_TRACKS, "malformed-success");
	assert(!star_result.error && star_result.request.action == ND_STAR_CHECK && navidrome_star_state(ND_TRACKS, "malformed-success") == ND_STAR_SET);
	assert(favorites().base == cached.base && favorites().finished == cached.finished);
	mode("cache_retry");
	assert(!chmod(root, 0555));
	reload(root);
	star_request(ND_STAR_ADD, ND_ALBUMS, id);
	assert(star_result.cache_failed && star_result.completed == 1 && !favorites().writable);
	assert(!chmod(root, 0700));
	reload(root);
	star_request(ND_STAR_RETRY, ND_ALBUMS, id);
	assert(!star_result.error && star_result.saved == 1);
	mode("normal");
	assert(favorites().base == cached.base && favorites().finished == cached.finished);
	nd_star_request_t q = {.action = ND_STAR_ADD, .kind = ND_TRACKS};
	snprintf(q.id, sizeof(q.id), "busy-track");
	mode("star_slow");
	atomic_store(&star_delivered, false);
	assert(navidrome_star_submit(&q, star_done, NULL));
	assert(!navidrome_star_submit(&q, star_done, NULL) && !navidrome_sync() && !navidrome_favorites_refresh());
	star_wait();
	assert(!star_result.error);
	nd_context_t old;
	assert(navidrome_context(&old, true));
	atomic_store(&star_delivered, false);
	assert(navidrome_star_submit(&q, star_done, NULL));
	navidrome_lock();
	star_wait();
	assert(star_result.error && !navidrome_star_submit(&q, star_done, NULL));
	mode("normal");
	assert(navidrome_unlock("vault1"));
	assert(idle().state == ND_CONNECTED);
	q.bound_profile = true;
	q.profile_epoch = old.profile_epoch;
	assert(!navidrome_star_submit(&q, star_done, NULL));
	q.bound_profile = false;
	nd_star_result_t stale;
	nd_star_execute(&q, &old, &stale);
	assert(stale.error && !stale.completed);
	nd_wipe(&old, sizeof(old));
	// A captured selection spans three fixed windows; no SQLite/card lease crosses HTTP.
	nd_library_t source = favorites();
	char path[1200];
	nd_playlist_store_t selections = {0};
	assert(nd_pl_path(&source, path) && nd_pl_open(&selections, path, source.scope, true));
	for (int i = 0; i < 35; i++) {
		char sql[256];
		snprintf(sql, sizeof(sql), "INSERT INTO selection(session,ordinal,kind,track,occurrence) VALUES(4400,%d,1,'batch-%%-%d',0)", i, i);
		assert(nd_pl_sql(&selections, sql));
	}
	nd_pl_close(&selections);
	q = (nd_star_request_t){.action = ND_STAR_SELECTED, .kind = ND_TRACKS, .session = 4400, .library = source};
	atomic_store(&star_delivered, false);
	assert(navidrome_star_submit(&q, star_done, NULL));
	star_wait();
	assert(!star_result.error && star_result.completed == 35 && star_result.total == 35);
	// Album selection spans windows and uses album IDs without expanding songs.
	assert(nd_pl_open(&selections, path, source.scope, true));
	for (int i = 0; i < 18; i++) {
		char sql[256];
		snprintf(sql, sizeof(sql), "INSERT INTO selection(session,ordinal,kind,track,occurrence) VALUES(4401,%d,0,'selected-album-%d',0)", i * 7, i);
		assert(nd_pl_sql(&selections, sql));
	}
	assert(nd_pl_sql(&selections, "INSERT INTO selection(session,ordinal,kind,track,occurrence) VALUES(4401,9999,1,'wrong-kind-track',0)"));
	nd_pl_close(&selections);
	nd_star_request_t album_selection = {.action = ND_STAR_SELECTED, .kind = ND_ALBUMS, .session = 4401, .library = source};
	atomic_store(&star_delivered, false);
	assert(navidrome_star_submit(&album_selection, star_done, NULL));
	star_wait();
	assert(!star_result.error && star_result.completed == 18 && star_result.total == 18);
	assert(navidrome_star_state(ND_ALBUMS, "selected-album-17") == ND_STAR_SET);
	assert(navidrome_star_state(ND_TRACKS, "wrong-kind-track") == ND_STAR_UNKNOWN);
	mode("star_partial");
	atomic_store(&star_delivered, false);
	assert(navidrome_star_submit(&q, star_done, NULL));
	star_wait();
	assert(star_result.error && !star_result.uncertain && star_result.completed == 5 && star_result.failed == 1 && star_result.total == 35);
	mode("normal");
	star_request(ND_STAR_CHECK, ND_TRACKS, "batch-%-0");
	assert(!star_result.error && navidrome_star_state(ND_TRACKS, "batch-%-0") == ND_STAR_SET);
	mode("star_lost");
	atomic_store(&star_delivered, false);
	assert(navidrome_star_submit(&q, star_done, NULL));
	star_wait();
	assert(star_result.error && star_result.uncertain && star_result.completed == 0 && star_result.total == 35);
	mode("normal");
	atomic_store(&star_delivered, false);
	assert(navidrome_star_submit(&q, star_done, NULL));
	star_wait();
	assert(star_result.uncertain && star_result.completed == 0); // Explicit recovery is required before another batch.
	star_request(ND_STAR_CHECK, ND_TRACKS, "batch-%-0");
	assert(!star_result.error && navidrome_star_state(ND_TRACKS, "batch-%-0") == ND_STAR_SET);
	assert(favorites().base == cached.base && favorites().finished == cached.finished);
	mode("offline");
	assert(navidrome_reconnect());
	assert(idle().state == ND_DISCONNECTED);
	assert(!navidrome_star_submit(&q, star_done, NULL));
	mode("normal");
	assert(navidrome_reconnect());
	assert(idle().state == ND_CONNECTED);
	reload(NULL);
	star_request(ND_STAR_ADD, ND_ARTISTS, "without-card");
	assert(star_result.cache_failed && star_result.completed == 1);
	reload(root);
	puts("Protected track/album/artist Add/Remove, independent IDs, unknown/uncertain state, read-only/no-card writes, busy/lock races and bounded selection batches: passed");
}

static void incremental_action_checks(const char *root) {
	mode("local_edits");
	star_request(ND_STAR_ADD, ND_TRACKS, "pre-refresh-track");
	assert(!star_result.error && star_result.completed == 1 && star_result.saved == 1);
	nd_library_t known = favorites();
	assert(known.ready && !known.base && !known.finished && known.counts[ND_TRACKS] == 1);
	reload(NULL);
	reload(root);
	nd_store_t store = {0};
	bool star = false;
	assert(nd_store_open_source(&store, favorites().path, known.scope, false, false, true));
	assert(nd_store_annotation(&store, favorites().generation, ND_TRACKS, "pre-refresh-track", &star) && star);
	nd_store_close(&store);
	star_request(ND_STAR_ADD, ND_ARTISTS, "persistent-artist");
	assert(!star_result.error);
	for (int i = 0; i < 34; i++) {
		char id[80];
		snprintf(id, sizeof(id), "eviction-artist-%d", i);
		star_request(ND_STAR_ADD, ND_ARTISTS, id);
		assert(!star_result.error);
	}
	assert(navidrome_star_state(ND_ARTISTS, "persistent-artist") == ND_STAR_UNKNOWN);
	mode("artist_persisted");
	star_request(ND_STAR_TOGGLE, ND_ARTISTS, "persistent-artist");
	assert(!star_result.error && navidrome_star_state(ND_ARTISTS, "persistent-artist") == ND_STAR_CLEAR);
	mode("album_pending");
	nd_star_request_t q = {.action = ND_STAR_ADD, .kind = ND_ALBUMS};
	snprintf(q.id, sizeof(q.id), "hydrated-album");
	atomic_store(&star_delivered, false);
	assert(navidrome_star_submit(&q, star_done, NULL));
	bool saw_pending = false;
	for (int i = 0; i < 1000; i++) {
		nd_library_t now = favorites();
		if (now.counts[ND_ALBUMS] == 1) {
			nd_playrow_t album;
			if (nd_store_open_source(&store, now.path, now.scope, false, false, true) && nd_store_metadata(&store, now.generation, ND_ALBUMS, q.id, &album))
				saw_pending = album.row.details == 1;
			nd_store_close(&store);
			if (saw_pending)
				break;
		}
		usleep(1000);
	}
	assert(saw_pending);
	star_wait();
	assert(!star_result.error);
	nd_library_t ready = favorites();
	assert(ready.counts[ND_ALBUMS] == 1 && ready.counts[ND_TRACKS] == 2 && !ready.finished);
	assert(nd_store_open_source(&store, ready.path, ready.scope, false, false, true));
	nd_query_t contents = {.kind = ND_TRACKS, .favorites = true};
	snprintf(contents.filter, sizeof(contents.filter), "hydrated-album");
	nd_row_t rows[ND_WINDOW];
	int count, total;
	assert(nd_store_query(&store, ready.generation, &contents, 0, rows, &count, &total) && total == 2 && !strcmp(rows[0].id, "hydrated-album-starred") && rows[0].favorite && !rows[1].favorite);
	nd_store_close(&store);
	star_request(ND_STAR_REMOVE, ND_ALBUMS, "hydrated-album");
	assert(!star_result.error && favorites().counts[ND_ALBUMS] == 0 && favorites().counts[ND_TRACKS] == 2);
	const char *modes[] = {"album_details_fail", "album_duplicate", "album_oversize"};
	const char *ids[] = {"incomplete-album", "duplicate-album", "oversized-album"};
	for (int i = 0; i < 3; i++) {
		mode(modes[i]);
		star_request(ND_STAR_ADD, ND_ALBUMS, ids[i]);
		assert(star_result.cache_failed && star_result.completed == 1 && navidrome_star_state(ND_ALBUMS, ids[i]) == ND_STAR_SET);
		nd_library_t failed = favorites();
		nd_playrow_t album;
		assert(nd_store_open_source(&store, failed.path, failed.scope, false, false, true) && nd_store_metadata(&store, failed.generation, ND_ALBUMS, ids[i], &album) && album.row.details == 2);
		nd_store_close(&store);
		mode("album_details_ok");
		star_request(ND_STAR_RETRY, ND_ALBUMS, ids[i]);
		assert(!star_result.error && star_result.saved == 1);
	}
	mode("cache_retry");
	struct rlimit old_limit, limit;
	assert(!getrlimit(RLIMIT_FSIZE, &old_limit));
	limit = old_limit;
	limit.rlim_cur = 1;
	signal(SIGXFSZ, SIG_IGN);
	assert(!setrlimit(RLIMIT_FSIZE, &limit));
	star_request(ND_STAR_ADD, ND_TRACKS, "write-failure-track");
	assert(!setrlimit(RLIMIT_FSIZE, &old_limit));
	assert(star_result.completed == 1 && star_result.cache_failed && navidrome_star_state(ND_TRACKS, "write-failure-track") == ND_STAR_SET);
	star_request(ND_STAR_RETRY, ND_TRACKS, "write-failure-track");
	assert(!star_result.error && star_result.saved == 1);
	mode("normal");
	puts("Automatic favorite actions: known-only initial cache, durable state beyond memory eviction, album loading/ordering/independent flags, failed metadata and cache-only retries: passed");
}

int main(int argc, char **argv) {
	if (argc == 4 && !strcmp(argv[1], "--offline-probe")) {
		snprintf(configuration, sizeof(configuration), "%s/config.ini", argv[3]);
		navidrome_init();
		navidrome_set_root(argv[3]);
		assert(idle().state == ND_LOCKED && !favorites().ready && !*favorites().path);
		assert(navidrome_unlock("vault1"));
		assert(idle().state == ND_DISCONNECTED);
		nd_library_t cached = favorites();
		assert(cached.ready && cached.counts[ND_TRACKS] == 17000);
		query_favorites(cached, ND_TRACKS, NULL, 16984, 17000);
		query_favorites(cached, ND_TRACKS, "album-00000", 0, 10);
		navidrome_shutdown();
		return 0;
	}
	assert(argc == 3);
	alarm(180);
	setvbuf(stdout, NULL, _IONBF, 0);
	snprintf(endpoint, sizeof(endpoint), "%s", argv[1]);
	snprintf(configuration, sizeof(configuration), "%s/config.ini", argv[2]);
	char host[600];
	snprintf(host, sizeof(host), "%s/prefix", endpoint);
	navidrome_init();
	navidrome_set_root(argv[2]);
	assert(navidrome_save(host, "fixture user", "synthetic-server-password", "vault1"));
	assert(idle().state == ND_CONNECTED);
	assert(!library().ready && !favorites().ready);
	mode("normal");
	incremental_store_checks(argv[2]);
	incremental_action_checks(argv[2]);
	mode("normal");
	nd_library_t first = refresh_ok(2, 3);
	assert(!library().ready);
	nd_store_t annotations;
	assert(nd_store_open_source(&annotations, first.path, first.scope, false, false, true));
	assert(nd_store_favorite(&annotations, first.generation, ND_TRACKS, "song-00002"));
	assert(!nd_store_favorite(&annotations, first.generation, ND_TRACKS, "song-00000"));
	nd_store_close(&annotations);
	query_favorites(first, ND_ALBUMS, NULL, 0, 2);
	query_favorites(first, ND_TRACKS, NULL, 0, 3);
	query_favorites(first, ND_TRACKS, "album-00000", 0, 10);
	query_favorites(first, ND_TRACKS, "album-00002", 0, 10);
	puts("Favorites album/song independence, overlap, duplicate names and offline album metadata: passed");
	favorite_migration(first, argv[2]);
	star_checks(first, argv[2]);
	first = favorites();

	mode("library_empty");
	nd_library_t full = sync_ok(0, 0);
	unchanged(first);
	mode("normal");
	first = refresh_ok(2, 3);
	assert(library().generation == full.generation && library().finished == full.finished);
	mode("deleted");
	refresh_ok(0, 1);
	mode("empty");
	nd_library_t empty = refresh_ok(0, 0);
	assert(empty.ready && empty.finished);
	mode("exact");
	refresh_ok(2, 2000);
	mode("bytes");
	refresh_ok(170, 1700);
	mode("tokens");
	refresh_ok(2, 3);
	mode("large");
	nd_library_t large = refresh_ok(1700, 17000);
	query_favorites(large, ND_TRACKS, NULL, 16984, 17000);
	query_favorites(large, ND_ALBUMS, NULL, 1684, 1700);
	struct rusage usage;
	assert(!getrusage(RUSAGE_SELF, &usage) && usage.ru_maxrss < 128 * 1024);
	printf("Favorites host peak RSS: %ld KiB; fixed query window: %d rows\n", usage.ru_maxrss, ND_WINDOW);
	mode("offline");
	pid_t child = fork();
	assert(child >= 0);
	if (!child) {
		execl(argv[0], argv[0], "--offline-probe", endpoint, argv[2], (char *)NULL);
		_exit(127);
	}
	int child_status;
	assert(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) && !WEXITSTATUS(child_status));
	mode("normal");
	puts("Favorites process restart starts locked and restores complete cache after offline unlock: passed");
	puts("Favorites empty/removal, independent library timestamps, exact pages, decoded-byte/arena reduction and 17000-song reachability: passed");

	const char *failures[] = {"repeat", "truncated", "incomplete", "badid", "badstar", "badcount", "scanchange", "scanning", "oversingle", "gzip_bad", "reject"};
	for (size_t i = 0; i < sizeof(failures) / sizeof(*failures); i++) {
		mode(failures[i]);
		assert(navidrome_favorites_refresh());
		idle();
		assert(favorites().error);
		unchanged(large);
		if (idle().state != ND_CONNECTED) {
			mode("normal");
			assert(navidrome_reconnect());
			assert(idle().state == ND_CONNECTED);
		}
	}
	mode("slow");
	assert(navidrome_favorites_refresh());
	usleep(30000);
	navidrome_favorites_cancel();
	idle();
	assert(!strcmp(favorites().error, "nd_favorites_cancelled"));
	unchanged(large);
	puts("Favorites repeated discarded IDs, malformed/API/incomplete data, scan/resource failures and cancellation retain prior generation: passed");

	// An abandoned candidate is never promoted by reopening the cache.
	nd_store_t store;
	assert(nd_store_open_source(&store, large.path, large.scope, true, false, true));
	assert(nd_store_begin(&store));
	nd_store_close(&store);
	reload(argv[2]);
	unchanged(large);
	mode("large");
	struct stat st;
	struct rlimit old_limit, small;
	assert(!stat(large.path, &st) && !getrlimit(RLIMIT_FSIZE, &old_limit));
	small = old_limit;
	small.rlim_cur = st.st_size;
	signal(SIGXFSZ, SIG_IGN);
	assert(!setrlimit(RLIMIT_FSIZE, &small));
	assert(navidrome_favorites_refresh());
	idle();
	assert(!setrlimit(RLIMIT_FSIZE, &old_limit));
	assert(favorites().error);
	unchanged(large);
	query_favorites(large, ND_TRACKS, NULL, 16984, 17000);
	// Last-operation failure feedback must never authorize rebuilding a valid cache.
	mode("slow");
	assert(navidrome_favorites_refresh());
	usleep(30000);
	navidrome_favorites_cancel();
	idle();
	unchanged(large);
	query_favorites(large, ND_TRACKS, NULL, 16984, 17000);
	puts("Favorites interrupted candidate, storage write failure and cancelled retry preserve readable prior cache: passed");

	assert(!chmod(argv[2], 0555));
	reload(argv[2]);
	assert(favorites().ready && !favorites().writable && !navidrome_favorites_refresh());
	query_favorites(favorites(), ND_ALBUMS, NULL, 1684, 1700);
	assert(!chmod(argv[2], 0700));
	reload(argv[2]);
	mode("slow");
	assert(navidrome_favorites_refresh());
	usleep(30000);
	reload(NULL); // Same service boundary used for card removal/USB handoff.
	idle();
	assert(!favorites().ready);
	reload(argv[2]);
	unchanged(large);
	mode("slow");
	assert(navidrome_favorites_refresh());
	usleep(30000);
	navidrome_lock();
	idle();
	assert(!favorites().ready && !*favorites().path && !favorites().finished);
	usleep(300000); // Let the invalidated request drain before unlocking.
	mode("normal");
	assert(navidrome_unlock("vault1"));
	assert(idle().state == ND_CONNECTED);
	unchanged(large);
	query_favorites(favorites(), ND_TRACKS, NULL, 16984, 17000);
	puts("Favorites read-only card, storage handoff, lock, unlock and stale requests: passed");

	// Rebuild only Favorites after an unsupported schema; ordinary library survives.
	assert(nd_store_open_source(&store, large.path, large.scope, true, false, true));
	assert(sqlite3_exec(store.db, "PRAGMA user_version=99", NULL, NULL, NULL) == SQLITE_OK);
	nd_store_close(&store);
	reload(argv[2]);
	assert(!favorites().ready && !strcmp(favorites().error, "nd_favorites_cache_error"));
	first = refresh_ok(2, 3);
	assert(library().ready && library().generation == full.generation);
	char user_path[1200];
	nd_playlist_store_t user_store = {0};
	nd_library_t recovered = favorites();
	assert(nd_pl_path(&recovered, user_path) && nd_pl_open(&user_store, user_path, recovered.scope, false));
	sqlite3_stmt *selected;
	assert(nd_pl_prepare(&user_store, &selected, "SELECT count(*) FROM selection WHERE session=4400"));
	assert(sqlite3_step(selected) == SQLITE_ROW && sqlite3_column_int(selected, 0) == 35);
	sqlite3_finalize(selected);
	nd_pl_close(&user_store);
	assert(navidrome_save(host, "different fixture user", "synthetic-server-password", NULL));
	idle();
	assert(!favorites().ready);
	assert(navidrome_save(host, "fixture user", "synthetic-server-password", NULL));
	idle();
	assert(!favorites().ready);
	first = refresh_ok(2, 3);
	reload(NULL);
	assert(navidrome_forget());
	idle();
	assert(navidrome_save(host, "fixture user", "synthetic-server-password", "vault1"));
	assert(idle().state == ND_CONNECTED);
	reload(argv[2]);
	assert(!favorites().ready);
	assert(strcmp(first.scope, favorites().scope));
	refresh_ok(2, 3);
	puts("Favorites schema recovery, profile replacement and Forget with absent card: passed");
	assert(navidrome_unencrypt(NULL));
	assert(!idle().encrypted);
	star_request(ND_STAR_ADD, ND_TRACKS, "plain-mode-no-unlock");
	assert(!star_result.error);
	cache_private(argv[2]);
	puts("Favorites cache and diagnostic redaction: passed");
	navidrome_reset();
	return 0;
}

// Production action capture/storage and real loopback services; synthetic data.
#define main search_baseline_main
#include "navidrome_searchcheck.c"
#undef main
#include "src/system/streaming/navidromeactions.h"
#include "src/system/streaming/navidromeplaylists.h"
#include "src/system/streaming/navidromestars.h"

static int fail_capture_allocation, fail_selection_insert;
void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size) {
	if (__sync_bool_compare_and_swap(&fail_capture_allocation, 1, 0))
		return NULL;
	return __real_calloc(count, size);
}
int __real_sqlite3_step(sqlite3_stmt *statement);
int __wrap_sqlite3_step(sqlite3_stmt *statement) {
	const char *sql = sqlite3_sql(statement);
	if (sql && !strncmp(sql, "INSERT INTO item", 16) && __sync_bool_compare_and_swap(&fail_selection_insert, 1, 0))
		return SQLITE_FULL;
	return __real_sqlite3_step(statement);
}

static void limit_test(nd_library_t library, nd_context_t identity) {
	nd_owned_rows_t *selection = NULL;
	bool limited = false;
	for (int i = 0; i < 4096; i++) {
		nd_action_request_t q = {.op = ND_ACTION_TOGGLE, .library = library, .selection = selection, .kind = ND_TRACKS, .remote = true, .ordinal = i};
		snprintf(q.row.row.id, sizeof(q.row.row.id), "limit-%05d", i);
		nd_action_result_t result;
		nd_action_execute(&q, &identity, &result);
		nd_owned_release(selection);
		selection = result.selection;
		if (result.error) {
			assert(i >= 64 && selection && selection->count == i);
			nd_playrow_t row;
			assert(nd_owned_read(selection, i - 1, &row, 1) == 1);
			limited = true;
			break;
		}
	}
	assert(limited);
	nd_owned_release(selection);
	puts("Finite staging byte limit rejects an addition without losing earlier selection: passed");
}

static nd_owned_rows_t *choose(nd_owned_rows_t *selection, nd_library_t library, nd_context_t identity, int ordinal, bool allowed) {
	nd_action_request_t q = {.op = ND_ACTION_TOGGLE, .library = library, .selection = selection, .kind = ND_TRACKS, .remote = true, .ordinal = ordinal};
	snprintf(q.row.row.id, sizeof(q.row.row.id), "chosen-%05d", ordinal);
	snprintf(q.row.row.title, sizeof(q.row.row.title), "Chosen title %05d", ordinal);
	nd_action_result_t result;
	nd_action_execute(&q, &identity, &result);
	assert((result.error == NULL) == allowed);
	nd_owned_release(selection);
	return result.selection;
}
static nd_owned_rows_t *capture(nd_owned_rows_t *selection, nd_library_t library, nd_context_t identity, nd_kind_t kind, const char *id, bool expand, bool allowed) {
	nd_action_request_t q = {.op = ND_ACTION_CAPTURE, .selection = selection, .library = library, .kind = kind, .remote = true, .expand = expand};
	if (id) {
		snprintf(q.row.row.id, sizeof(q.row.row.id), "%s", id);
		snprintf(q.row.row.title, sizeof(q.row.row.title), "Represented title");
	}
	nd_action_result_t result;
	nd_action_execute(&q, &identity, &result);
	assert((result.error == NULL) == allowed);
	if (!allowed) {
		nd_owned_release(result.capture);
		return NULL;
	}
	return result.capture;
}
static nd_pl_result_t playlist_execute(nd_pl_request_t *q) {
	nd_context_t context;
	nd_library_t library;
	assert(navidrome_context(&context, false));
	navidrome_library_snapshot(&library);
	nd_pl_result_t result;
	nd_pl_execute(q, &context, &library, &result);
	nd_wipe(context.password, sizeof(context.password));
	return result;
}
int main(int argc, char **argv) {
	assert(argc == 3 || argc == 4);
	alarm(240);
	snprintf(origin, sizeof(origin), "%s", argv[1]);
	assert(!chdir(argv[2]));
	snprintf(config_file, sizeof(config_file), "%s/config.ini", argv[2]);
	char host[600];
	snprintf(host, sizeof(host), "%s/base", origin);
	navidrome_init();
	navidrome_set_root("");
	assert(navidrome_save(host, "fixture user", "synthetic-server-password", NULL));
	assert(wait_idle().state == ND_CONNECTED);
	nd_library_t library = wait_library();
	nd_context_t identity;
	assert(navidrome_context(&identity, false));
	nd_wipe(identity.password, sizeof(identity.password));
	nd_owned_rows_t *selection = NULL;
	for (int i = 0; i < 64; i++)
		selection = choose(selection, library, identity, i, true);
	assert(selection && selection->count == 64);
	selection = choose(selection, library, identity, 64, false);
	assert(selection->count == 64);
	nd_playrow_t row;
	assert(nd_owned_read(selection, 63, &row, 1) == 1 && !strcmp(row.row.id, "chosen-00063"));
	nd_owned_release(selection);
	selection = NULL;
	puts("No-card 64/65 selection boundary preserves earlier choices: passed");
	char card[1100];
	snprintf(card, sizeof(card), "%s/card", argv[2]);
	assert(!mkdir(card, 0700));
	navidrome_set_root(card);
	library = wait_library();
	assert(!library.ready && library.writable && library.path[0]);
	assert(navidrome_context(&identity, false));
	nd_wipe(identity.password, sizeof(identity.password));
	if (argc == 4) {
		limit_test(library, identity);
		navidrome_shutdown();
		return 0;
	}
	selection = choose(selection, library, identity, 5, true);
	selection = choose(selection, library, identity, 1, true);
	selection = choose(selection, library, identity, 3, true);
	nd_owned_rows_t *snapshot = capture(selection, library, identity, ND_TRACKS, NULL, true, true);
	assert(snapshot && snapshot->count == 3 && nd_owned_read(snapshot, 0, &row, 1) == 1 && !strcmp(row.row.id, "chosen-00001"));
	selection = choose(selection, library, identity, 1, true);
	assert(selection->count == 2 && snapshot->count == 3 && nd_owned_read(snapshot, 0, &row, 1) == 1 && !strcmp(row.row.id, "chosen-00001"));
	nd_owned_release(snapshot);
	nd_owned_release(selection);
	selection = NULL;
	nd_library_t readonly = library;
	readonly.writable = false;
	for (int i = 0; i < 64; i++)
		selection = choose(selection, readonly, identity, i, true);
	selection = choose(selection, readonly, identity, 64, false);
	assert(selection->count == 64);
	nd_owned_release(selection);
	selection = NULL;
	for (int i = 0; i < 17000; i++)
		selection = choose(selection, library, identity, i, true);
	assert(selection->count == 17000);
	fail_selection_insert = 1;
	selection = choose(selection, library, identity, 20000, false);
	assert(selection->count == 17000);
	fail_capture_allocation = 1;
	assert(!capture(selection, library, identity, ND_TRACKS, NULL, true, false));
	assert(selection->count == 17000);
	snapshot = capture(selection, library, identity, ND_TRACKS, NULL, true, true);
	assert(snapshot && snapshot->count == 17000 && nd_owned_read(snapshot, 16999, &row, 1) == 1 && !strcmp(row.row.id, "chosen-16999"));
	nd_owned_rows_t *single = capture(NULL, readonly, identity, ND_TRACKS, "one-unsynced-track", true, true);
	assert(single && single->count == 1 && nd_owned_read(single, 0, &row, 1) == 1 && !strcmp(row.row.id, "one-unsynced-track"));
	puts("17,000 selected rows, ordered immutable snapshots, read-only and single-item isolation: passed");
	nd_owned_rows_t *album = capture(NULL, library, identity, ND_ALBUMS, "new-album", true, true);
	assert(album && album->count == 12 && nd_owned_read(album, 0, &row, 1) == 1 && !strcmp(row.row.id, "new-song-11"));
	assert(album->album_multi(album, "new-album") == 1 && single->album_multi(single, "new-album") == -1);
	assert(!capture(NULL, library, identity, ND_ALBUMS, "bad-album", true, false));
	puts("Complete unsynced album order and mismatched membership rejection: passed");
	// Retain a disk-backed child, destroy its Search, and create a newer Search
	// that performs orphan cleanup. Offline action must still use the old child.
	nd_search_t *search = nd_search_create("retained child");
	assert(search);
	nd_search_result_t child;
	nd_search_child(search, 1, ND_ALBUMS, "retained-large", &child);
	assert(!child.error && child.total == 100 && child.card_backed);
	nd_owned_rows_t *retained = nd_search_child_retain(search, 1);
	assert(retained && retained->count == 100);
	nd_search_destroy(search);
	search = nd_search_create("new query");
	assert(search);
	nd_search_destroy(search);
	navidrome_test_online(0);
	navidrome_poll_network();
	nd_action_request_t retained_request = {.op = ND_ACTION_CAPTURE, .kind = ND_ALBUMS, .remote = true, .expand = true, .library = library, .albums = {retained}};
	snprintf(retained_request.row.row.id, ND_ID_MAX, "retained-large");
	snprintf(retained_request.album_ids[0], ND_ID_MAX, "retained-large");
	nd_action_result_t retained_result;
	nd_action_execute(&retained_request, &identity, &retained_result);
	assert(!retained_result.error && retained_result.capture->count == 100);
	assert(retained_result.capture->album_multi(retained_result.capture, "retained-large") == 0);
	nd_owned_release(retained_result.capture);
	nd_owned_release(retained);
	navidrome_test_online(1);
	navidrome_poll_network();
	reconnect();
	assert(navidrome_context(&identity, false));
	nd_wipe(identity.password, sizeof(identity.password));
	puts("Retained complete disk child survives newer-query cleanup and captures offline without HTTP: passed");
	nd_pl_request_t create = {.action = NDPL_CREATE};
	snprintf(create.name, sizeof(create.name), "Road Trip");
	nd_pl_result_t made = playlist_execute(&create);
	assert(!made.error && made.playlist.id > 0);
	nd_pl_request_t add = {.action = NDPL_ADD, .id = made.playlist.id, .revision = made.playlist.revision, .captured = album};
	nd_pl_result_t added = playlist_execute(&add);
	assert(!added.error && added.added == 12 && added.playlist.count == 12);
	add.revision = added.playlist.revision;
	added = playlist_execute(&add);
	assert(!added.error && !added.added && added.skipped == 12);
	add.revision = made.playlist.revision;
	added = playlist_execute(&add);
	assert(added.error && !added.added);
	puts("Real Navidrome local destination, unsynced album append, duplicate filtering and stale revision rollback: passed");
	nd_owned_rows_t *stars = capture(NULL, library, identity, ND_ALBUMS, "new-album", false, true);
	assert(stars && stars->count == 1);
	nd_star_request_t star = {.action = ND_STAR_SELECTED, .kind = ND_ALBUMS, .captured = stars};
	nd_star_result_t starred;
	nd_context_t connected;
	assert(navidrome_context(&connected, true));
	nd_star_execute(&star, &connected, &starred);
	assert(starred.completed == 1 && !starred.uncertain);
	nd_wipe(connected.password, sizeof(connected.password));
	puts("Captured album favorite uses one absolute album annotation without track expansion: passed");
	for (int phase = 0; phase < 2; phase++) {
		nd_owned_rows_t *batch_selection = NULL;
		for (int i = 0; i < 3; i++) {
			nd_action_request_t q = {.op = ND_ACTION_TOGGLE, .library = library, .selection = batch_selection, .kind = ND_TRACKS, .remote = true, .ordinal = i};
			snprintf(q.row.row.id, ND_ID_MAX, "batch-%s-%d", phase ? "uncertain" : "failed", i);
			nd_action_result_t result;
			nd_action_execute(&q, &identity, &result);
			assert(!result.error);
			nd_owned_release(batch_selection);
			batch_selection = result.selection;
		}
		nd_owned_rows_t *batch = capture(batch_selection, library, identity, ND_TRACKS, NULL, false, true);
		nd_star_request_t request = {.action = ND_STAR_SELECTED, .kind = ND_TRACKS, .captured = batch};
		assert(navidrome_context(&connected, true));
		nd_star_execute(&request, &connected, &starred);
		assert(starred.error && starred.completed == 1 && starred.total == 3);
		assert(starred.uncertain == (phase == 1) && starred.failed == (phase == 0));
		if (phase) {
			nd_star_execute(&request, &connected, &starred);
			assert(starred.uncertain && starred.completed == 1 && starred.total == 3);
		}
		nd_wipe(&connected, sizeof(connected));
		nd_owned_release(batch);
		nd_owned_release(batch_selection);
	}
	puts("Captured favorites: absolute Add, accurate confirmed/failed/untouched counts and uncertainty stops later writes without implicit replay: passed");
	nd_owned_release(stars);
	nd_owned_release(album);
	nd_owned_release(single);
	nd_owned_release(selection);
	nd_library_t unchanged = wait_library();
	assert(!unchanged.ready && unchanged.generation == library.generation);
	// A cloned local capture also owns its structural source generation for
	// Download; publication/cleanup cannot remove it before consumer admission.
	nd_library_t local = library;
	seed(&local);
	nd_store_t local_store;
	assert(nd_store_open(&local_store, local.path, local.scope, false, false));
	assert(nd_store_status(&local_store, &local));
	nd_store_close(&local_store);
	nd_action_request_t local_request = {.op = ND_ACTION_CAPTURE, .kind = ND_TRACKS, .library = local, .expand = true};
	strcpy(local_request.row.row.id, "song-00000");
	nd_action_result_t local_result;
	nd_action_execute(&local_request, &identity, &local_result);
	assert(!local_result.error && local_result.capture->local && local_result.capture->count == 1);
	seed(&local);
	assert(nd_store_open(&local_store, local.path, local.scope, true, false));
	nd_store_cleanup(&local_store);
	assert(nd_store_metadata(&local_store, local.generation, ND_TRACKS, "song-00000", &row));
	nd_store_close(&local_store);
	assert(nd_owned_read(local_result.capture, 0, &row, 1) == 1 && !strcmp(row.row.id, "song-00000"));
	nd_owned_release(local_result.capture);
	puts("Local capture retains its exact structural source across newer publication/cleanup: passed");
	navidrome_set_root("");
	wait_library();
	assert(nd_owned_read(snapshot, 0, &row, 1) < 0);
	nd_action_request_t retired_request = {.op = ND_ACTION_CAPTURE, .kind = ND_TRACKS, .remote = true, .library = library};
	strcpy(retired_request.row.row.id, "retired-card-row");
	assert(!navidrome_actions_submit(&retired_request, NULL, NULL));
	nd_context_t current;
	assert(navidrome_context(&current, false));
	nd_action_result_t rejected;
	nd_action_execute(&retired_request, &current, &rejected);
	assert(rejected.error && !rejected.capture);
	nd_wipe(&current, sizeof(current));
	nd_owned_release(snapshot);
	puts("Card invalidation rejects retained metadata and actions never publish Library Sync: passed");
	navidrome_shutdown();
	return 0;
}

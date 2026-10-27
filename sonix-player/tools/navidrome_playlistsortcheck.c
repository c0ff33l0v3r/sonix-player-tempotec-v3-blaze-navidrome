#define main existing_capture_checks
#define __wrap_sqlite3_step capture_sql_step
#include "navidrome_playlistplaycheck.c"
#undef __wrap_sqlite3_step
#undef main

static const char *full_statement;
static bool fail_migration;
int __wrap_sqlite3_step(sqlite3_stmt *st) {
	const char *sql = sqlite3_sql(st);
	if (full_statement && sql && !strncmp(sql, full_statement, strlen(full_statement))) {
		full_statement = NULL;
		return SQLITE_FULL;
	}
	return capture_sql_step(st);
}
int __real_sqlite3_exec(sqlite3 *, const char *, int (*)(void *, int, char **, char **), void *, char **);
int __wrap_sqlite3_exec(sqlite3 *db, const char *sql, int (*cb)(void *, int, char **, char **), void *user, char **error) {
	if (fail_migration && strstr(sql, "CREATE TABLE IF NOT EXISTS entry_sort")) {
		assert(__real_sqlite3_exec(db, "BEGIN IMMEDIATE; CREATE TABLE playlist_view(id INTEGER PRIMARY KEY,sort INT,version INT,metadata INT)", NULL, NULL, NULL) == SQLITE_OK);
		return SQLITE_FULL;
	}
	return __real_sqlite3_exec(db, sql, cb, user, error);
}
static const int expected_order[9][5] = {{0, 1, 2, 3, 4}, {1, 4, 3, 0, 2}, {0, 2, 3, 4, 1}, {1, 4, 0, 2, 3}, {0, 2, 1, 4, 3}, {4, 0, 2, 1, 3}, {1, 4, 0, 2, 3}, {1, 4, 0, 2, 3}, {4, 0, 2, 1, 3}};
static int64_t occurrences[5];
static nd_playrow_t details[5];
static int view_action;
static int reject_commit(void *unused) {
	(void)unused;
	return 1;
}
static bool change_view(void *unused) {
	(void)unused;
	if (++checks == 2 && view_action) {
		nd_playlist_store_t s;
		nd_playlist_t p;
		assert(nd_pl_open(&s, store_path, library.scope, true) && nd_pl_get(&s, request.playlist, &p));
		if (view_action == 1)
			assert(nd_pl_sort_set(&s, &p, p.sort == ND_PL_TITLE ? ND_PL_TITLE_DESC : ND_PL_TITLE));
		else {
			assert(nd_pl_sql(&s, "BEGIN IMMEDIATE"));
			nd_playrow_t info = details[0];
			snprintf(info.album_artist, sizeof(info.album_artist), "Revised credit");
			assert(nd_pl_sort_info(&s, occurrences[0], &info));
		}
		assert(nd_pl_sql(&s, "COMMIT"));
		nd_pl_close(&s);
	}
	return true;
}
static void choose(nd_playlist_store_t *s, nd_playlist_t *p, nd_pl_sort_t sort) { assert(nd_pl_sort_set(s, p, sort) && nd_pl_sql(s, "COMMIT") && nd_pl_get(s, p->id, p)); }
static void prepare_request(const nd_playlist_t *p, const nd_playlist_entry_t *e, int displayed) {
	request = (nd_playlist_capture_t){.playlist = p->id, .revision = p->revision, .count = p->count, .view = true, .sort = p->sort, .view_revision = p->view_revision, .metadata_revision = p->metadata_revision, .ordinal = displayed, .occurrence = e->occurrence, .stored_position = e->position, .stored_ordinal = e->ordinal};
	snprintf(request.track, sizeof(request.track), "%s", e->track.id);
}
int main(int argc, char **argv) {
	assert(argc == 3);
	snprintf(library.scope, sizeof(library.scope), "0123456789abcdef0123456789abcdef");
	snprintf(library.path, sizeof(library.path), "%s/.cache/library.db", argv[2]);
	library.writable = true;
	assert(nd_pl_path(&library, store_path));
	nd_playlist_store_t s;
	assert(nd_pl_open(&s, store_path, library.scope, true));
	if (!strcmp(argv[1], "read")) {
		nd_playlist_t p, other;
		assert(nd_pl_get(&s, 1, &p) && p.sort == ND_PL_ALBUM_ARTIST_DESC && p.manual);
		assert(nd_pl_get(&s, 2, &other) && other.sort == ND_PL_TITLE);
		assert(!strcmp(p.name, "Renamed"));
		choose(&s, &p, ND_PL_ORIGINAL);
		nd_playlist_entry_t restored[ND_WINDOW];
		int restored_count, restored_total;
		assert(nd_pl_view_window(&s, &p, 0, 0, restored, &restored_count, &restored_total) && restored_count == 5 && restored[0].position == 2 && !strcmp(restored[0].track.id, "A"));
		choose(&s, &p, ND_PL_ALBUM_ARTIST_DESC);
		nd_pl_close(&s);
		puts("Two independent playlist preferences, renamed local identity and sorting metadata across real process restart: passed");
		return 0;
	}
	assert(!strcmp(argv[1], "write"));
	const char *ids[] = {"A", "B", "A", "C", "D"};
	const char *titles[] = {"Zulu", "Alpha", "Zulu", "Étoile", "The Echo"};
	const char *artists[] = {"The Artist B", "Artist A", "The Artist B", "", "Artist B"};
	const char *albums[] = {"B album", "A album", "B album", "", "A album"};
	const char *album_ids[] = {"b", "a1", "b", "", "a2"};
	const char *credits[] = {"The Band A", "Band Z", "The Band A", "", "Band A"};
	int64_t id, second;
	assert(nd_pl_create(&s, "Same", &id) && id == 1 && nd_pl_begin(&s, id, 1));
	int added = 0, skipped = 0;
	for (int i = 0; i < 5; i++) {
		nd_playrow_t *info = &details[i];
		snprintf(info->row.id, sizeof(info->row.id), "%s", ids[i]);
		snprintf(info->row.title, sizeof(info->row.title), "%s", titles[i]);
		snprintf(info->row.artist, sizeof(info->row.artist), "%s", artists[i]);
		snprintf(info->album, sizeof(info->album), "%s", albums[i]);
		snprintf(info->album_id, sizeof(info->album_id), "%s", album_ids[i]);
		snprintf(info->album_artist, sizeof(info->album_artist), "%s", credits[i]);
		info->row.disc = i == 0 || i == 2 ? 2 : 1;
		info->row.track = i == 0 || i == 2 ? 2 : 1;
		assert(nd_pl_append(&s, id, &info->row, false, &added, &skipped));
		occurrences[i] = sqlite3_last_insert_rowid(s.db);
		assert(nd_pl_sort_info(&s, occurrences[i], info));
	}
	assert(nd_pl_finish(&s, id, true) && nd_pl_sql(&s, "COMMIT"));
	assert(nd_pl_create(&s, "Same", &second) && second == 2);
	assert(nd_pl_sql(&s, "UPDATE entry SET ordinal=-ordinal-1; UPDATE entry SET ordinal=(-ordinal-1)*3;"));
	nd_playlist_t p, other;
	assert(nd_pl_get(&s, id, &p));
	char original[ND_PL_HASH], after[ND_PL_HASH];
	assert(nd_pl_fingerprint(&s, id, original));
	assert(nd_pl_baseline(&s, id, "linked", original, false, "fixture"));
	assert(nd_pl_get(&s, id, &p));
	int64_t revision = p.revision;
	for (int sort = 0; sort < 9; sort++) {
		choose(&s, &p, (nd_pl_sort_t)sort);
		assert(p.revision == revision && !p.dirty && !strcmp(p.remote, "linked"));
		assert(nd_pl_fingerprint(&s, id, after) && !strcmp(original, after));
		assert(nd_pl_get(&s, second, &other) && other.sort == ND_PL_ORIGINAL);
		nd_playlist_entry_t rows[ND_WINDOW];
		int count, total;
		assert(nd_pl_view_window(&s, &p, 0, 0, rows, &count, &total) && count == 5 && total == 5);
		nd_playlist_entry_t stored[ND_WINDOW];
		int stored_n;
		assert(nd_pl_entries(&s, id, 0, 0, stored, &stored_n) && stored_n == 5);
		for (int j = 0; j < 5; j++)
			assert(stored[j].occurrence == occurrences[j]);
		request = (nd_playlist_capture_t){.playlist = id, .revision = p.revision, .count = p.count, .whole = true};
		const char *stored_error;
		nd_playcatalog_t *whole = capture(&stored_error);
		assert(whole && !stored_error);
		for (int j = 0; j < 5; j++) {
			nd_playrow_t item;
			assert(nd_playcatalog_read(whole, j, &item) && item.saved_occurrence == occurrences[j]);
		}
		retire_catalog(whole);
		int selected = -1;
		for (int i = 0; i < count; i++) {
			assert(rows[i].occurrence == occurrences[expected_order[sort][i]]);
			assert(rows[i].ordinal == rows[i].position * 3);
			if (rows[i].occurrence == occurrences[2])
				selected = i;
		}
		assert(selected >= 0);
		prepare_request(&p, &rows[selected], selected);
		const char *error;
		nd_playcatalog_t *catalog = capture(&error);
		assert(catalog && !error && nd_playcatalog_count(catalog) == 5 && nd_playcatalog_start(catalog) == selected);
		for (int i = 0; i < count; i++) {
			nd_playrow_t row;
			assert(nd_playcatalog_read(catalog, i, &row) && row.saved_occurrence == rows[i].occurrence);
			assert(!strcmp(row.album, rows[i].album) && !strcmp(row.album_artist, rows[i].album_artist));
		}
		choose(&s, &p, sort == ND_PL_TITLE ? ND_PL_TITLE_DESC : ND_PL_TITLE);
		nd_playrow_t row;
		assert(nd_playcatalog_read(catalog, selected, &row) && row.saved_occurrence == occurrences[2]);
		retire_catalog(catalog);
	}
	choose(&s, &p, ND_PL_ALBUM_ARTIST_DESC);
	choose(&s, &other, ND_PL_TITLE);
	// Preference/metadata changes before sealing reject the old view without
	// changing content revision or losing the already captured sequence.
	nd_playlist_entry_t rows[ND_WINDOW];
	int count, total;
	assert(nd_pl_view_window(&s, &p, 0, 0, rows, &count, &total));
	for (view_action = 1; view_action <= 2; view_action++) {
		assert(nd_pl_get(&s, id, &p));
		assert(nd_pl_view_window(&s, &p, 0, 0, rows, &count, &total));
		prepare_request(&p, &rows[0], 0);
		checks = 0;
		nd_context_t ctx = {0};
		const char *error;
		nd_playcatalog_t *catalog = nd_playcatalog_playlist(&library, &ctx, &request, change_view, NULL, &error);
		assert(!catalog && error && !strcmp(error, "nd_pl_stale"));
		assert(nd_pl_get(&s, id, &p) && p.revision == revision);
	}
	view_action = 0;
	// Failed update and read-only writes retain the acknowledged preference.
	full_statement = "INSERT INTO playlist_view";
	assert(!nd_pl_sort_set(&s, &p, ND_PL_ARTIST) && !full_statement);
	assert(nd_pl_sql(&s, "ROLLBACK"));
	assert(nd_pl_get(&s, id, &p));
	full_statement = "INSERT INTO entry_sort";
	assert(nd_pl_sql(&s, "BEGIN") && !nd_pl_sort_info(&s, occurrences[0], &details[0]) && !full_statement);
	assert(nd_pl_sql(&s, "ROLLBACK") && nd_pl_get(&s, id, &p));

	assert(nd_pl_sql(&s, "CREATE TRIGGER fail_view BEFORE UPDATE ON playlist_view BEGIN SELECT RAISE(ABORT,'fixture');END;"));
	nd_pl_sort_t previous = p.sort;
	assert(!nd_pl_sort_set(&s, &p, ND_PL_ARTIST));
	assert(nd_pl_sql(&s, "ROLLBACK; DROP TRIGGER fail_view;"));
	assert(nd_pl_get(&s, id, &p) && p.sort == previous);
	choose(&s, &p, ND_PL_ALBUM_ARTIST_DESC);
	assert(nd_pl_sort_set(&s, &p, ND_PL_ARTIST));
	sqlite3_commit_hook(s.db, reject_commit, NULL);
	assert(!nd_pl_sql(&s, "COMMIT"));
	sqlite3_commit_hook(s.db, NULL, NULL);
	assert(nd_pl_get(&s, id, &p) && p.sort == ND_PL_ALBUM_ARTIST_DESC);
	assert(nd_pl_edit(&s, id, p.revision, "Renamed", false) && nd_pl_sql(&s, "COMMIT"));
	assert(nd_pl_get(&s, id, &p) && p.sort == ND_PL_ALBUM_ARTIST_DESC);
	int64_t copy;
	assert(nd_pl_copy(&s, id, "Copied", &copy) && nd_pl_sql(&s, "COMMIT"));
	nd_playlist_t copied;
	assert(nd_pl_get(&s, copy, &copied) && copied.sort == ND_PL_ORIGINAL);
	assert(nd_pl_view_window(&s, &copied, 0, 0, rows, &count, &total) && count == 5);
	assert(!strcmp(rows[0].album, "B album") && !strcmp(rows[0].album_artist, "Revised credit"));
	nd_pl_close(&s);
	assert(nd_pl_open(&s, store_path, library.scope, false) && nd_pl_get(&s, id, &p));
	assert(!nd_pl_sort_set(&s, &p, ND_PL_TITLE));
	nd_pl_close(&s);
	// A schema-1 store without optional tables remains readable, then migrates
	// additively. Older insertions and mismatched side identities stay unknown.
	char legacy_path[1210];
	snprintf(legacy_path, sizeof(legacy_path), "%s-legacy", store_path);
	nd_playlist_store_t old;
	assert(nd_pl_open(&old, legacy_path, library.scope, true));
	int64_t legacy;
	assert(nd_pl_create(&old, "Legacy", &legacy) && nd_pl_begin(&old, legacy, 1));
	added = skipped = 0;
	nd_row_t legacy_row = {.id = "old-A", .title = "Legacy title", .artist = "Track artist"};
	assert(nd_pl_append(&old, legacy, &legacy_row, false, &added, &skipped));
	assert(nd_pl_finish(&old, legacy, true) && nd_pl_sql(&old, "COMMIT; DROP TABLE entry_sort; DROP TABLE playlist_view;"));
	nd_pl_close(&old);
	assert(nd_pl_open(&old, legacy_path, library.scope, false) && !old.sorting && nd_pl_get(&old, legacy, &copied));
	assert(copied.sort == ND_PL_ORIGINAL && nd_pl_view_window(&old, &copied, 0, 0, rows, &count, &total) && count == 1);
	assert(!rows[0].album[0] && !rows[0].album_artist[0] && !strcmp(rows[0].track.artist, "Track artist"));
	assert(!nd_pl_sort_set(&old, &copied, ND_PL_TITLE));
	nd_pl_close(&old);
	fail_migration = true;
	assert(!nd_pl_open(&old, legacy_path, library.scope, true));
	nd_pl_close(&old);
	fail_migration = false;
	assert(nd_pl_open(&old, legacy_path, library.scope, false) && !old.sorting && nd_pl_get(&old, legacy, &copied));
	assert(copied.sort == ND_PL_ORIGINAL && copied.count == 1);
	nd_pl_close(&old);
	assert(nd_pl_open(&old, legacy_path, library.scope, true) && old.sorting && nd_pl_get(&old, legacy, &copied));
	choose(&old, &copied, ND_PL_ARTIST_DESC);
	assert(nd_pl_sql(&old, "UPDATE playlist_view SET sort=100"));
	assert(nd_pl_get(&old, legacy, &copied) && copied.sort == ND_PL_ORIGINAL);
	assert(nd_pl_sql(&old, "UPDATE playlist_view SET sort=8"));
	assert(nd_pl_get(&old, legacy, &copied) && copied.sort == ND_PL_ARTIST_DESC);
	assert(nd_pl_view_window(&old, &copied, 0, 0, rows, &count, &total));
	nd_playrow_t exact = {.row = rows[0].track, .album = "Known album", .album_artist = "Actual album artist"};
	assert(nd_pl_sql(&old, "BEGIN") && nd_pl_sort_info(&old, rows[0].occurrence, &exact) && nd_pl_sql(&old, "COMMIT"));
	assert(nd_pl_get(&old, legacy, &copied));
	int64_t metadata = copied.metadata_revision;
	nd_playrow_t partial = {.row = rows[0].track};
	assert(nd_pl_sql(&old, "BEGIN") && nd_pl_sort_info(&old, rows[0].occurrence, &partial) && nd_pl_sql(&old, "COMMIT"));
	assert(nd_pl_get(&old, legacy, &copied) && copied.metadata_revision == metadata);
	assert(nd_pl_view_window(&old, &copied, 0, 0, rows, &count, &total) && !strcmp(rows[0].album, "Known album"));
	assert(nd_pl_sql(&old, "UPDATE entry SET track='replacement'"));
	assert(nd_pl_view_window(&old, &copied, 0, 0, rows, &count, &total) && !rows[0].album[0] && !rows[0].album_artist[0]);
	assert(nd_pl_edit(&old, legacy, copied.revision, NULL, true) && nd_pl_sql(&old, "COMMIT"));
	sqlite3_stmt *cleanup;
	assert(nd_pl_prepare(&old, &cleanup, "SELECT (SELECT count(*) FROM entry_sort)+(SELECT count(*) FROM playlist_view)"));
	assert(sqlite3_step(cleanup) == SQLITE_ROW && !sqlite3_column_int(cleanup, 0));
	sqlite3_finalize(cleanup);
	nd_pl_close(&old);
	assert(!unlink(legacy_path));

	// Manual drag order is a separate durable view, never a publishable definition.
	assert(nd_pl_open(&s, store_path, library.scope, true) && nd_pl_get(&s, id, &p));
	choose(&s, &p, ND_PL_ORIGINAL);
	assert(nd_pl_fingerprint(&s, id, original));
	int64_t content = p.revision, version = p.view_revision;
	bool dirty = p.dirty, changed;
	assert(nd_pl_sql(&s, "CREATE TRIGGER fail_local_order BEFORE INSERT ON entry_view BEGIN SELECT RAISE(ABORT,'fixture');END;"));
	assert(!nd_pl_view_move(&s, &p, 2, 0, occurrences[2], "A", &changed));
	assert(nd_pl_sql(&s, "ROLLBACK; DROP TRIGGER fail_local_order;") && nd_pl_get(&s, id, &p));
	assert(!p.manual && p.revision == content && p.view_revision == version);
	nd_playlist_t stale_view = p;
	assert(nd_pl_view_move(&s, &p, 2, 0, occurrences[2], "A", &changed) && changed && nd_pl_sql(&s, "COMMIT") && nd_pl_get(&s, id, &p));
	assert(p.manual && p.revision == content && p.view_revision == version + 1 && p.dirty == dirty);
	assert(!nd_pl_view_move(&s, &stale_view, 2, 0, occurrences[2], "A", &changed) && s.error && !strcmp(s.error, "nd_pl_stale"));
	assert(nd_pl_sql(&s, "ROLLBACK") && nd_pl_get(&s, id, &p));
	assert(nd_pl_fingerprint(&s, id, after) && !strcmp(original, after));
	assert(nd_pl_entries(&s, id, 0, 0, rows, &count) && rows[0].occurrence == occurrences[0] && rows[2].occurrence == occurrences[2]);
	assert(nd_pl_view_window(&s, &p, 0, 0, rows, &count, &total) && rows[0].occurrence == occurrences[2]);
	prepare_request(&p, &rows[0], 0);
	const char *manual_error;
	nd_playcatalog_t *manual = capture(&manual_error);
	assert(manual && !manual_error);
	nd_playrow_t manual_row;
	assert(nd_playcatalog_read(manual, 0, &manual_row) && manual_row.saved_occurrence == occurrences[2]);
	retire_catalog(manual);
	assert(nd_pl_sort_set(&s, &p, ND_PL_ORIGINAL) && nd_pl_view_reset(&s, id) && nd_pl_sql(&s, "COMMIT") && nd_pl_get(&s, id, &p));
	assert(!p.manual && nd_pl_view_window(&s, &p, 0, 0, rows, &count, &total) && rows[0].occurrence == occurrences[0]);
	assert(nd_pl_view_move(&s, &p, 2, 0, occurrences[2], "A", &changed) && nd_pl_sql(&s, "COMMIT") && nd_pl_get(&s, id, &p));
	choose(&s, &p, ND_PL_ALBUM_ARTIST_DESC);
	nd_pl_close(&s);
	// Large sorted captures still use bounded windows/private catalogues and
	// preserve every occurrence independently of repeated exact IDs.
	make_playlist(5000, 4999);
	assert(nd_pl_open(&s, store_path, library.scope, true));
	sqlite3_stmt *st;
	assert(nd_pl_prepare(&s, &st, "UPDATE entry SET title=printf('Title %05d',(ordinal*71)%5000) WHERE p=?"));
	sqlite3_bind_int64(st, 1, request.playlist);
	assert(sqlite3_step(st) == SQLITE_DONE);
	sqlite3_finalize(st);
	assert(nd_pl_sql(&s, "UPDATE entry_sort SET album_artist='Captured large credit',album='Captured large album'"));
	assert(nd_pl_get(&s, request.playlist, &p));
	choose(&s, &p, ND_PL_TITLE_DESC);
	assert(nd_pl_view_window(&s, &p, 4984, 0, rows, &count, &total) && count == 16 && total == 5000);
	prepare_request(&p, &rows[15], 4999);
	nd_pl_close(&s);
	const char *error;
	nd_playcatalog_t *catalog = capture(&error);
	assert(catalog && !error && nd_playcatalog_count(catalog) == 5000 && nd_playcatalog_start(catalog) == 4999);
	for (int i = 0; i < 5000; i++) {
		nd_playrow_t row;
		char title[32];
		snprintf(title, sizeof(title), "Title %05d", 4999 - i);
		assert(nd_playcatalog_read(catalog, i, &row) && !strcmp(row.row.title, title) && row.saved_occurrence > 0);
		assert(!strcmp(row.album_artist, "Captured large credit") && !strcmp(row.album, "Captured large album"));
	}
	retire_catalog(catalog);
	library.writable = false;
	nd_playlist_capture_t large_view = request;
	request.view = false;
	request.single = true;
	request.ordinal = request.stored_position;
	catalog = capture(&error);
	assert(catalog && !error && nd_playcatalog_count(catalog) == 1);
	nd_playrow_t single;
	assert(nd_playcatalog_read(catalog, 0, &single) && single.saved_occurrence == request.occurrence);
	retire_catalog(catalog);
	request = large_view;
	catalog = capture(&error);
	assert(!catalog && error && !strcmp(error, "nd_cache_readonly"));
	library.writable = true;
	puts("All nine Unicode playlist orders, disc/album groups, missing credits, exact repetitions/gaps, immutable sorted captures, 5000 entries and rollback/read-only guards: passed");
	return 0;
}

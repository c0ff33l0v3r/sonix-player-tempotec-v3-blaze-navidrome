// Real durable store and immutable catalogue; no network or LVGL prerequisites.
#include "src/system/streaming/navidromeplaycatalog.c"
#include <assert.h>
#include <stdatomic.h>

static atomic_int leases;
static atomic_bool eligible = true;
static nd_library_t library;
static nd_playlist_capture_t request;
static char store_path[1200];
static int checks, action, action_at = 20;
void thread_be_background(const char *name) { (void)name; }
void nd_wipe(void *p, size_t n) { memset(p, 0, n); }
bool navidrome_playback_current(const nd_context_t *ctx, bool network) {
	(void)ctx; (void)network; return atomic_load(&eligible);
}
bool navidrome_playback_begin(const nd_context_t *ctx) {
	if (!navidrome_playback_current(ctx, false)) return false;
	atomic_fetch_add(&leases, 1); return true;
}
void navidrome_cache_end(void) { assert(atomic_fetch_sub(&leases, 1) > 0); }
static bool current(void *unused) {
	(void)unused;
	if (++checks == action_at && action) {
		if (action == 1) return false;
		if (action == 4) { atomic_store(&eligible, false); return false; }
		nd_playlist_store_t s;
		assert(nd_pl_open(&s, store_path, library.scope, true));
		assert(nd_pl_edit(&s, request.playlist, request.revision, "Changed", action == 3));
		assert(nd_pl_sql(&s, "COMMIT"));
		nd_pl_close(&s);
	}
	return atomic_load(&eligible);
}
static int fail_insert = -1;
static bool fail_read;
static int fail_allocation = -1;
void *__real_calloc(size_t n, size_t size);
void *__wrap_calloc(size_t n, size_t size) {
	if (fail_allocation >= 0 && !fail_allocation--) return NULL;
	return __real_calloc(n, size);
}
int __real_sqlite3_step(sqlite3_stmt *st);
int __wrap_sqlite3_step(sqlite3_stmt *st) {
	const char *sql = sqlite3_sql(st);
	if (sql && fail_read && !strncmp(sql, "SELECT id,track,title,artist,cover,duration FROM entry", 51)) {
		fail_read = false;
		return SQLITE_IOERR;
	}
	if (sql && !strncmp(sql, "INSERT INTO track VALUES", 24) && fail_insert >= 0 && !fail_insert--)
		return SQLITE_FULL;
	return __real_sqlite3_step(st);
}
static void make_playlist(int count, int selected) {
	nd_playlist_store_t s;
	assert(nd_pl_open(&s, store_path, library.scope, true));
	int64_t id;
	assert(nd_pl_create(&s, "Same name", &id) && nd_pl_begin(&s, id, 1));
	int added = 0, skipped = 0;
	for (int i = 0; i < count; i++) {
		nd_row_t row = {.title = "Saved fallback", .artist = "Saved artist", .cover = "saved-cover", .duration = 12};
		snprintf(row.id, sizeof(row.id), "song-%05d", i % 7);
		assert(nd_pl_append(&s, id, &row, false, &added, &skipped));
	}
	assert(nd_pl_finish(&s, id, true) && nd_pl_sql(&s, "COMMIT"));
	nd_playlist_entry_t entries[ND_WINDOW];
	int n;
	assert(nd_pl_entries(&s, id, selected, 0, entries, &n));
	request = (nd_playlist_capture_t){.playlist = id, .revision = 2, .ordinal = selected, .occurrence = n ? entries[0].occurrence : 1};
	strcpy(request.track, n ? entries[0].track.id : "missing");
	nd_pl_close(&s);
}
static nd_playcatalog_t *capture(const char **error) {
	checks = 0;
	nd_context_t context = {0};
	return nd_playcatalog_playlist(&library, &context, &request, current, NULL, error);
}
static void rejected(const char *reason) {
	const char *error;
	nd_playcatalog_t *c = capture(&error);
	if (c || !error || strcmp(error, reason))
		fprintf(stderr, "expected=%s actual=%s captured=%d action=%d\n", reason, error ? error : "none", c != NULL, action);
	assert(!c && error && !strcmp(error, reason));
	assert(!atomic_load(&leases));
}
static void retire_catalog(nd_playcatalog_t *c) {
	char path[1200];
	strcpy(path, c->path);
	nd_playcatalog_free(c);
	for (int i = 0; *path && !access(path, F_OK) && i < 5000; i++) usleep(1000);
	assert(!*path || access(path, F_OK));
}
int main(int argc, char **argv) {
	(void)argv;
	char dir[] = "/tmp/sonix-playlist-play.XXXXXX";
	assert(mkdtemp(dir));
	snprintf(library.scope, sizeof(library.scope), "0123456789abcdef0123456789abcdef");
	snprintf(library.path, sizeof(library.path), "%s/.cache/library.db", dir);
	library.writable = true;
	assert(nd_pl_path(&library, store_path));
	const char *error;
	make_playlist(5000, 4999);
	if (argc > 1) {
		rejected("nd_pl_play_limit");
		puts("saved playlist catalogue finite SQLite page limit: passed");
		return 0;
	}
	nd_playcatalog_t *c = capture(&error);
	assert(c && !error && nd_playcatalog_count(c) == 5000 && nd_playcatalog_start(c) == 4999 && !c->rows);
	nd_playrow_t row;
	nd_playlist_capture_t tapped = request;
	request.whole = true; request.count = 5000;
	nd_playcatalog_t *whole = capture(&error);
	assert(whole && !error && nd_playcatalog_count(whole) == 5000 && nd_playcatalog_start(whole) == 0);
	retire_catalog(whole);
	request.count--; rejected("nd_pl_stale");
	request = tapped; request.single = true; request.count = 5000;
	library.writable = false;
	nd_playcatalog_t *single = capture(&error);
	assert(single && !error && single->rows && !single->path[0] && nd_playcatalog_count(single) == 1 && nd_playcatalog_start(single) == 0);
	assert(nd_playcatalog_read(single, 0, &row) && !strcmp(row.row.id, tapped.track) && single->occurrences[0] == tapped.occurrence);
	retire_catalog(single);
	request.occurrence++; rejected("nd_pl_stale"); request.occurrence--;
	request.count--; rejected("nd_pl_stale");
	request = tapped; library.writable = true;
	puts("saved capture actions: complete 5000-occurrence start, expected-count guard and one-entry capture from large read-only parent: passed");
	for (int i = 0; i < 5000; i += 4999) {
		assert(nd_playcatalog_read(c, i, &row));
		assert(!strcmp(row.row.title, "Saved fallback") && !strcmp(row.row.artist, "Saved artist") && !*row.album_id && !*row.artist_id);
		assert(atoi(row.row.id + 5) == i % 7);
	}
	// Saved definition changes and deletion cannot change the captured source.
	nd_playlist_store_t s;
	assert(nd_pl_open(&s, store_path, library.scope, true));
	assert(nd_pl_edit(&s, request.playlist, request.revision, NULL, true) && nd_pl_sql(&s, "COMMIT"));
	nd_pl_close(&s);
	assert(nd_playcatalog_read(c, 4999, &row) && nd_playcatalog_count(c) == 5000);
	// Another capture must not orphan-clean the still-live catalogue.
	make_playlist(65, 64);
	nd_playcatalog_t *other = capture(&error);
	assert(other && !error && !access(c->path, F_OK));
	retire_catalog(other); retire_catalog(c);
	// Removal leaves gaps in stored ordinals; view offsets remain dense.
	make_playlist(5, 4);
	assert(nd_pl_open(&s, store_path, library.scope, true) && nd_pl_begin(&s, request.playlist, request.revision));
	char removal[160];
	snprintf(removal, sizeof(removal), "DELETE FROM entry WHERE p=%lld AND ordinal=1", (long long)request.playlist);
	assert(nd_pl_sql(&s, removal) && nd_pl_finish(&s, request.playlist, true) && nd_pl_sql(&s, "COMMIT"));
	nd_pl_close(&s);
	request.revision++; request.ordinal--;
	c = capture(&error);
	assert(c && !error && nd_playcatalog_count(c) == 4 && nd_playcatalog_start(c) == 3);
	assert(nd_playcatalog_read(c, 1, &row) && !strcmp(row.row.id, "song-00002"));
	retire_catalog(c);
	make_playlist(64, 63);
	library.writable = false;
	request.whole = true; request.count = 64;
	other = capture(&error);
	assert(other && !error && nd_playcatalog_count(other) == 64 && nd_playcatalog_start(other) == 0);
	retire_catalog(other);
	request.whole = false; request.count = 0;
	c = capture(&error);
	assert(c && !error && c->rows && !*c->path && nd_playcatalog_start(c) == 63);
	retire_catalog(c);
	make_playlist(65, 64);
	request.whole = true; request.count = 65;
	rejected("nd_cache_readonly");
	request.whole = false; request.count = 0;
	rejected("nd_cache_readonly");
	library.writable = true;
	request.occurrence++;
	rejected("nd_pl_stale");
	make_playlist(65, 64);
	strcpy(request.track, "different-id"); rejected("nd_pl_stale");
	make_playlist(65, 64);
	request.revision++; rejected("nd_pl_stale");
	for (action = 1; action <= 4; action++) {
		make_playlist(65, 64);
		rejected(action == 1 || action == 4 ? "nd_audio_cancelled" : "nd_pl_stale");
		atomic_store(&eligible, true);
	}
	action = 0;
	make_playlist(500, 499);
	action = 1; action_at = 100; rejected("nd_audio_cancelled");
	action = 0; action_at = 20;
	make_playlist(65, 64);
	fail_insert = 64; rejected("nd_pl_storage_error"); fail_insert = -1;
	make_playlist(65, 64);
	fail_read = true; rejected("nd_pl_storage_error");
	for (int i = 0; i < 2; i++) {
		fail_allocation = i; rejected("nd_json_memory"); fail_allocation = -1;
	}
	make_playlist(0, 0); rejected("nd_pl_stale");
	assert(nd_pl_open(&s, store_path, library.scope, true));
	assert(nd_pl_sql(&s, "PRAGMA user_version=99")); nd_pl_close(&s);
	rejected("nd_pl_storage_error");
	FILE *bad = fopen(store_path, "w"); assert(bad); fputs("corrupt store", bad); assert(!fclose(bad));
	rejected("nd_pl_storage_error");
	assert(!atomic_load(&leases));
	puts("saved playlist catalogue: 5000 repeated occurrences, exact start/metadata, immutable deletion, live/retired cleanup, 64/65 read-only, stale tuples/revisions, cancellation/card loss, injected write and corrupt/schema failures: passed");
	return 0;
}

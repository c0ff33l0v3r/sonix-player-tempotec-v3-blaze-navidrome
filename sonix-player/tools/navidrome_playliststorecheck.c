#include "src/system/streaming/navidromeplayliststore.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void moves(nd_playlist_store_t *s) {
	int64_t id;
	assert(nd_pl_create(s, "Move fixture", &id) && nd_pl_begin(s, id, 1));
	nd_row_t row = {.id = "A", .title = "retained"};
	int added = 0, skipped = 0;
	for (int i = 0; i < 3; i++) {
		strcpy(row.id, i == 1 ? "B" : "A");
		assert(nd_pl_append(s, id, &row, false, &added, &skipped));
	}
	assert(nd_pl_finish(s, id, true) && nd_pl_sql(s, "COMMIT"));
	char hash[ND_PL_HASH];
	assert(nd_pl_fingerprint(s, id, hash) && nd_pl_baseline(s, id, "move-remote", hash, true, ""));
	// Simulate the gaps left by removals without changing occurrence identities.
	assert(nd_pl_sql(s, "UPDATE entry SET ordinal=ordinal+100 WHERE p=(SELECT id FROM playlist WHERE name='Move fixture')"));
	nd_playlist_entry_t before[ND_WINDOW], after[ND_WINDOW];
	nd_playlist_t p;
	int n;
	bool changed;
	assert(nd_pl_entries(s, id, 0, 0, before, &n) && n == 3);
	assert(nd_pl_move(s, id, 2, 2, 0, before[2].occurrence, "A", &changed) && changed && nd_pl_sql(s, "COMMIT"));
	assert(nd_pl_entries(s, id, 0, 0, after, &n));
	assert(after[0].occurrence == before[2].occurrence && after[1].occurrence == before[0].occurrence);
	assert(!strcmp(after[0].track.title, "retained"));
	assert(nd_pl_get(s, id, &p) && p.revision == 3 && p.count == 3 && p.dirty && p.readonly);
	assert(!nd_pl_move(s, id, 2, 0, 1, before[2].occurrence, "A", &changed));
	assert(!nd_pl_move(s, id, 3, 0, 1, before[0].occurrence, "A", &changed));
	assert(!nd_pl_move(s, id, 3, -1, 1, before[2].occurrence, "A", &changed));
	assert(!nd_pl_move(s, id, 3, 0, 3, before[2].occurrence, "A", &changed));
	assert(nd_pl_move(s, id, 3, 0, 0, before[2].occurrence, "A", &changed) && !changed && nd_pl_sql(s, "COMMIT"));
	assert(nd_pl_get(s, id, &p) && p.revision == 3);
	// A,A,B -> A,A,B: occurrence order changes but the remote fingerprint does not.
	char current[ND_PL_HASH];
	assert(nd_pl_fingerprint(s, id, current));
	assert(nd_pl_move(s, id, 3, 0, 1, before[2].occurrence, "A", &changed) && changed && nd_pl_sql(s, "COMMIT"));
	char identical[ND_PL_HASH];
	assert(nd_pl_fingerprint(s, id, identical) && !strcmp(current, identical));
	// Restore A,B,A and its original occurrence sequence and clean baseline.
	assert(nd_pl_move(s, id, 4, 1, 2, before[2].occurrence, "A", &changed) && nd_pl_sql(s, "COMMIT"));
	assert(nd_pl_get(s, id, &p) && p.revision == 5 && !p.dirty);
	assert(nd_pl_sql(s, "CREATE TEMP TRIGGER fail_move BEFORE UPDATE OF ordinal ON entry WHEN new.ordinal=1 BEGIN SELECT RAISE(ABORT,'fixture'); END"));
	assert(!nd_pl_move(s, id, 5, 0, 2, before[0].occurrence, "A", &changed));
	assert(nd_pl_get(s, id, &p) && p.revision == 5 && !p.dirty);
	assert(nd_pl_entries(s, id, 0, 0, after, &n));
	for (int i = 0; i < n; i++) assert(after[i].occurrence == before[i].occurrence);
	assert(nd_pl_sql(s, "DROP TRIGGER fail_move"));
	assert(nd_pl_edit(s, id, 5, NULL, true) && nd_pl_sql(s, "COMMIT"));
	puts("playlist moves: gapped/repeated identities, no-op/stale/bounds, readonly remote, baseline, rollback passed");
}
int main(void) {
	char dir[] = "/tmp/sonix-playlist-store.XXXXXX";
	assert(mkdtemp(dir));
	char path[1200];
	snprintf(path, sizeof(path), "%s/playlists.db", dir);
	const char *scope = "0123456789abcdef0123456789abcdef";
	nd_library_t library = {0};
	snprintf(library.scope, sizeof(library.scope), "%s", scope);
	char playlist_path[1200];
	const char *library_paths[] = {"/card/.local/navidrome/.cache/library.db", "/card/.local/navidrome/library.db"};
	for (int i = 0; i < 2; i++) {
		snprintf(library.path, sizeof(library.path), "%s", library_paths[i]);
		assert(nd_pl_path(&library, playlist_path));
		assert(!strcmp(playlist_path, "/card/.local/navidrome/playlists-0123456789abcdef0123456789abcdef.db"));
	}
	nd_playlist_store_t s;
	nd_playlist_t p;
	int64_t id, other;
	assert(nd_pl_open(&s, path, scope, true));
	moves(&s);
	assert(!nd_pl_name_valid(" ") && !nd_pl_name_valid("\xe3\x80\x80"));
	assert(nd_pl_name_valid("海 / Road:trip"));
	char long_name[802];
	memset(long_name, 'x', 201);
	long_name[201] = 0;
	assert(!nd_pl_name_valid(long_name));
	assert(nd_pl_create(&s, "Road:trip", &id) && nd_pl_create(&s, "Road:trip", &other) && id != other);
	assert(nd_pl_begin(&s, id, 1));
	nd_row_t row = {.id = "A", .title = "Track A"};
	int added = 0, skipped = 0;
	assert(nd_pl_append(&s, id, &row, true, &added, &skipped));
	assert(nd_pl_append(&s, id, &row, true, &added, &skipped));
	assert(added == 1 && skipped == 1);
	strcpy(row.id, "B");
	assert(nd_pl_append(&s, id, &row, true, &added, &skipped));
	strcpy(row.id, "A");
	assert(nd_pl_append(&s, id, &row, false, &added, &skipped));
	assert(nd_pl_finish(&s, id, true) && nd_pl_sql(&s, "COMMIT"));
	assert(nd_pl_get(&s, id, &p) && p.count == 3 && p.revision == 2);
	char hash[ND_PL_HASH];
	assert(nd_pl_fingerprint(&s, id, hash));
	assert(nd_pl_baseline(&s, id, "remote-A", hash, false, ""));
	assert(nd_pl_get(&s, id, &p) && !p.dirty);
	assert(nd_pl_edit(&s, id, p.revision, "Other", false) && nd_pl_sql(&s, "COMMIT"));
	assert(nd_pl_get(&s, id, &p) && p.dirty);
	assert(nd_pl_edit(&s, id, p.revision, "Road:trip", false) && nd_pl_sql(&s, "COMMIT"));
	assert(nd_pl_get(&s, id, &p) && !p.dirty);
	nd_playlist_entry_t entries[ND_WINDOW];
	int count;
	assert(nd_pl_entries(&s, id, 0, 0, entries, &count) && count == 3);
	assert(entries[0].occurrence != entries[2].occurrence);
	// A trigger simulates a storage write failure halfway through a batch.
	assert(nd_pl_sql(&s, "CREATE TEMP TRIGGER fail_insert BEFORE INSERT ON entry WHEN new.track='FAIL' BEGIN SELECT RAISE(ABORT,'fixture'); END"));
	assert(nd_pl_begin(&s, id, p.revision));
	strcpy(row.id, "C");
	assert(nd_pl_append(&s, id, &row, true, &added, &skipped));
	strcpy(row.id, "FAIL");
	assert(!nd_pl_append(&s, id, &row, true, &added, &skipped));
	assert(nd_pl_sql(&s, "ROLLBACK"));
	assert(nd_pl_get(&s, id, &p) && p.count == 3 && !p.dirty);
	// Imported candidates preserve A,B,A instead of applying the addition filter.
	const char *body = "{\"subsonic-response\":{\"status\":\"ok\",\"playlist\":{\"id\":\"source\",\"name\":\"Imported\",\"songCount\":3,\"entry\":[{\"id\":\"A\",\"title\":\"海\"},{\"id\":\"B\"},{\"id\":\"A\"}]}}}";
	nd_document_t doc = {0};
	assert(json_parse(body, &doc.doc));
	doc.response = json_get(&doc.doc, 0, "subsonic-response");
	int64_t candidate;
	nd_playlist_t remote;
	assert(nd_pl_sql(&s, "BEGIN IMMEDIATE"));
	assert(nd_pl_decode(&s, &doc, &candidate, &remote) && candidate < 0 && remote.count == 3);
	assert(nd_pl_entries(&s, candidate, 0, 0, entries, &count) && !strcmp(entries[0].track.title, "海"));
	assert(nd_pl_sql(&s, "ROLLBACK"));
	json_free(&doc.doc);
	// Large persistent lists render in fixed windows; a new snapshot is unrelated.
	assert(nd_pl_begin(&s, other, 1));
	added = skipped = 0;
	for (int i = 0; i < 5000; i++) {
		snprintf(row.id, sizeof(row.id), "track-%05d", i);
		assert(nd_pl_append(&s, other, &row, true, &added, &skipped));
	}
	assert(nd_pl_finish(&s, other, true) && nd_pl_sql(&s, "COMMIT"));
	assert(nd_pl_entries(&s, other, 4992, 0, entries, &count) && count == 8 && !strcmp(entries[7].track.id, "track-04999"));
	assert(nd_pl_get(&s, other, &p));
	assert(nd_pl_entries(&s, other, 4999, 0, entries, &count));
	int64_t last = entries[0].occurrence;
	bool changed;
	assert(nd_pl_move(&s, other, p.revision, 4999, 0, last, "track-04999", &changed) && changed && nd_pl_sql(&s, "COMMIT"));
	assert(nd_pl_get(&s, other, &p));
	assert(nd_pl_move(&s, other, p.revision, 0, 4999, last, "track-04999", &changed) && nd_pl_sql(&s, "COMMIT"));
	nd_pl_close(&s);
	assert(nd_pl_open(&s, path, scope, false));
	assert(nd_pl_get(&s, other, &p) && p.count == 5000);
	assert(nd_pl_entries(&s, other, 4999, 0, entries, &count) && entries[0].occurrence == last);
	assert(!nd_pl_move(&s, other, p.revision, 4999, 0, last, "track-04999", &changed));
	nd_pl_close(&s);
	assert(!nd_pl_open(&s, path, "ffffffffffffffffffffffffffffffff", true));
	nd_pl_close(&s);
	assert(nd_pl_open(&s, path, scope, true));
	assert(nd_pl_sql(&s, "PRAGMA user_version=99"));
	nd_pl_close(&s);
	assert(!nd_pl_open(&s, path, scope, true));
	nd_pl_close(&s);
	assert(unlink(path) == 0 && rmdir(dir) == 0);
	puts("playlist store: persistence, identity, repeats, dedupe, rollback, 5000 entries, scope/schema guards passed");
}

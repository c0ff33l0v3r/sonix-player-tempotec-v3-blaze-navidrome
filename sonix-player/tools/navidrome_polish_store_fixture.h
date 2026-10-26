// Optional navigation sidecar coverage using the real streamed batch/store paths.
#include "src/system/streaming/navidromebatch.h"
static void polish_store_check(const char *root) {
	const char *objects[] = {"{\"id\":\"legacy\",\"title\":\"Same\",\"albumId\":\"album\",\"artistId\":\"a\"}", "{\"id\":\"consistent\",\"title\":\"Same\",\"albumId\":\"album\",\"artistId\":\"b\",\"artists\":[{\"id\":\"a\"},{\"id\":\"b\"}]}", "{\"id\":\"conflict\",\"title\":\"Same\",\"albumId\":\"album\",\"artistId\":\"b\",\"artists\":[{\"id\":\"a\"}]}", "{\"id\":\"ambiguous\",\"title\":\"Same\",\"albumId\":\"album\",\"artists\":[{\"id\":\"a\"},{\"id\":\"b\"}]}", "{\"id\":\"single\",\"title\":\"Same\",\"albumId\":\"album\",\"artists\":[{\"id\":\"a\"}]}"};
	const char *expected[] = {"a", "b", "", "", ""};
	char path[1200];
	snprintf(path, sizeof(path), "%s/navigation.db", root);
	nd_store_t store = {0};
	const char *scope = "12345678901234567890123456789012";
	assert(nd_store_open(&store, path, scope, true, false) && nd_store_begin(&store));
	char sql_text[1024];
	snprintf(sql_text, sizeof(sql_text),
			 "INSERT INTO entity VALUES(%lld,0,'album','Album','','','',0,0,0,'album');"
			 "INSERT INTO entity VALUES(%lld,2,'a','Duplicate','','','',0,0,0,'duplicate');"
			 "INSERT INTO entity VALUES(%lld,2,'b','Duplicate','','','',0,0,0,'duplicate');",
			 (long long)store.generation, (long long)store.generation, (long long)store.generation);
	assert(sqlite3_exec(store.db, sql_text, NULL, NULL, NULL) == SQLITE_OK);
	nd_batch_t batch;
	assert(nd_batch_init(&batch, ND_TRACKS));
	for (size_t i = 0; i < sizeof(objects) / sizeof(*objects); i++) {
		json_doc_t document;
		assert(json_parse_limited(objects[i], &document, 128));
		nd_playrow_t parsed;
		assert(nd_metadata_row(&document, json_root(&document), ND_TRACKS, &parsed));
		assert(!strcmp(parsed.artist_id, expected[i]));
		assert(nd_batch_add(&batch, &document, json_root(&document)));
		json_free(&document);
	}
	assert(nd_store_import_batch(&store, &batch));
	nd_batch_free(&batch);
	assert(nd_store_validate(&store) && nd_store_publish(&store));
	int64_t generation = store.generation;
	const char *ids[] = {"legacy", "consistent", "conflict", "ambiguous", "single"};
	for (int i = 0; i < 5; i++) {
		nd_playrow_t row;
		assert(nd_store_metadata(&store, generation, ND_TRACKS, ids[i], &row));
		assert(!strcmp(row.artist_id, expected[i]));
	}
	assert(nd_store_artist_browsable(&store, generation, "a") && nd_store_artist_browsable(&store, generation, "b"));
	// Simulate a readable older database. Optional-table absence stays usable.
	assert(sqlite3_exec(store.db, "DROP TABLE track_navigation", NULL, NULL, NULL) == SQLITE_OK);
	nd_store_close(&store);
	assert(nd_store_open(&store, path, scope, false, false) && !store.navigation);
	nd_playrow_t row;
	assert(nd_store_metadata(&store, generation, ND_TRACKS, "legacy", &row) && !*row.artist_id && !strcmp(row.row.title, "Same"));
	nd_store_close(&store);
	assert(nd_store_open(&store, path, scope, true, false) && store.navigation);
	assert(nd_store_metadata(&store, generation, ND_TRACKS, "legacy", &row) && !*row.artist_id);
	nd_store_close(&store);
	nd_library_t pin = {.generation = generation};
	snprintf(pin.scope, sizeof(pin.scope), "%s", scope);
	assert(path[0] == '/');
	assert(strlen(path) + 2 <= sizeof(pin.path));
	pin.path[0] = '/';
	memcpy(pin.path + 1, path, strlen(path) + 1);
	assert(nd_store_open(&store, pin.path, scope, true, false) && nd_store_retain_source(&pin));
	assert(nd_store_begin(&store) && nd_store_validate(&store) && nd_store_publish(&store));
	nd_store_cleanup(&store);
	assert(nd_store_contains(&store, generation, ND_TRACKS, "legacy"));
	nd_store_release_source(&pin);
	nd_store_cleanup(&store);
	assert(!nd_store_contains(&store, generation, ND_TRACKS, "legacy"));
	nd_store_close(&store);

	puts("navigation store: explicit legacy primary, structured agreement/conflicts, ambiguous/single non-primary credits, duplicate labels, streamed batches and additive read-only old-store compatibility: passed");
}

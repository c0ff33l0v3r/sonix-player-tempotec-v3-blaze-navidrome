#include "src/system/streaming/navidromebatch.h"

static void album_import_object(nd_store_t *s, nd_kind_t kind, const char *text, bool expected) {
	json_doc_t doc;
	assert(json_parse(text, &doc));
	nd_batch_t batch;
	assert(nd_batch_init(&batch, kind));
	batch.favorites = s->favorites;
	assert(nd_batch_add(&batch, &doc, 0));
	assert(nd_store_import_batch(s, &batch) == expected);
	nd_batch_free(&batch);
	json_free(&doc);
}
static void album_store_check(void) {
	nd_store_t s;
	assert(nd_store_open(&s, "header-store.db", "header-scope", true, false));
	assert(nd_store_begin(&s));
	album_import_object(&s, ND_ALBUMS, "{\"id\":\"a\",\"name\":\"Album é\",\"artist\":\"Artist\",\"year\":2001,\"genre\":\"Jazz\"}", true);
	album_import_object(&s, ND_TRACKS, "{\"id\":\"t\",\"title\":\"Track\",\"albumId\":\"a\",\"track\":0}", true);
	assert(nd_store_validate(&s) && nd_store_publish(&s));
	int64_t gen = s.generation;
	nd_playrow_t meta;
	assert(nd_store_metadata(&s, gen, ND_ALBUMS, "a", &meta));
	assert(meta.year == 2001 && !strcmp(meta.genre, "Jazz"));
	nd_query_t q = {.kind = ND_TRACKS, .filter_kind = ND_ALBUMS};
	snprintf(q.filter, sizeof(q.filter), "a");
	nd_row_t rows[ND_WINDOW];
	nd_album_window_t layout;
	int count, total;
	assert(nd_store_album_window(&s, gen, &q, 0, 108, 44, rows, &count, &total, &layout));
	assert(count == 1 && total == 1 && !layout.heading[0] && !rows[0].track && layout.extent == 108);
	// Reject display-write failure atomically without changing a published album.
	assert(sqlite3_exec(s.db, "CREATE TRIGGER reject_display BEFORE INSERT ON album_display BEGIN SELECT RAISE(ABORT,'fixture'); END", NULL, NULL, NULL) == SQLITE_OK);
	assert(nd_store_begin(&s));
	album_import_object(&s, ND_ALBUMS, "{\"id\":\"b\",\"name\":\"Rejected\",\"year\":2024}", false);
	assert(nd_store_metadata(&s, gen, ND_ALBUMS, "a", &meta) && meta.year == 2001);
	assert(sqlite3_exec(s.db, "DROP TRIGGER reject_display; DROP TABLE album_display", NULL, NULL, NULL) == SQLITE_OK);
	nd_store_close(&s);
	assert(nd_store_open(&s, "header-store.db", "header-scope", false, false));
	assert(nd_store_metadata(&s, gen, ND_ALBUMS, "a", &meta) && !meta.year && !strcmp(meta.genre, "Jazz"));
	nd_store_close(&s);
	// Favorites revisions retain their own display fields while pinned older
	// physical data remains unchanged and readable.
	assert(nd_store_open_source(&s, "header-favorites.db", "header-scope", true, false, true));
	assert(nd_store_begin(&s));
	album_import_object(&s, ND_ALBUMS, "{\"id\":\"a\",\"name\":\"Album\",\"year\":2001,\"genre\":\"Jazz\",\"songCount\":1}", true);
	album_import_object(&s, ND_TRACKS, "{\"id\":\"t\",\"title\":\"Track\",\"albumId\":\"a\"}", true);
	assert(nd_store_validate(&s) && nd_store_publish(&s));
	gen = s.generation;
	assert(nd_store_metadata(&s, gen, ND_ALBUMS, "a", &meta));
	assert(meta.year == 2001 && !strcmp(meta.genre, "Jazz"));
	assert(nd_store_local_begin(&s));
	meta.year = 2024;
	snprintf(meta.genre, sizeof(meta.genre), "Rock");
	assert(nd_store_local_put(&s, ND_ALBUMS, &meta, true, 0) && nd_store_local_activate(&s));
	assert(sqlite3_exec(s.db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
	int64_t revision = s.generation;
	assert(nd_store_metadata(&s, revision, ND_ALBUMS, "a", &meta) && meta.year == 2024 && !strcmp(meta.genre, "Rock"));
	assert(nd_store_metadata(&s, gen, ND_ALBUMS, "a", &meta) && meta.year == 2001 && !strcmp(meta.genre, "Jazz"));
	nd_store_close(&s);
	assert(nd_store_open_source(&s, "header-favorites.db", "header-scope", false, false, true));
	assert(nd_store_metadata(&s, revision, ND_ALBUMS, "a", &meta) && meta.year == 2024);
	nd_store_close(&s);
	json_doc_t doc;
	assert(json_parse("{\"id\":\"bad-display\",\"name\":\"Valid album\",\"year\":\"bad\",\"genre\":false}", &doc));
	assert(nd_metadata_row(&doc, 0, ND_ALBUMS, &meta) && !meta.year && !*meta.genre);
	json_free(&doc);

	assert(json_parse("{\"id\":\"fraction\",\"name\":\"Album\",\"year\":2001.5,\"genres\":[{\"name\":\"Soul\"}]}", &doc));
	assert(nd_metadata_row(&doc, 0, ND_ALBUMS, &meta) && !meta.year && !strcmp(meta.genre, "Soul"));
	json_free(&doc);
	puts("album store: streamed optional fields, legacy/read-only fallback, atomic display failure, invalid optional fields and independent Favorites revisions: passed");
}

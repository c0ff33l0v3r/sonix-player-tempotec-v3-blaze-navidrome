#include "src/system/library/library.h"
#include "src/system/streaming/navidromestore.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sign(int value) { return (value > 0) - (value < 0); }

int main(void) {
	const char *names[] = {"", "!Live", "007", "alpha", "Alpha", "The Beatles", "Beatles", "An Album", "il Disco", "Étoile", "Ωμέγα", "Москва", "あいう", "中文", "한국어", "Zebra"};
	for (int skip = 0; skip < 2; skip++) {
		library_set_skip_articles(skip);
		for (unsigned a = 0; a < sizeof(names) / sizeof(names[0]); a++) {
			char ka[LIBRARY_SORT_KEY_MAX], legacy[LIBRARY_SORT_KEY_MAX];
			library_sort_key_ex(names[a], ka, sizeof(ka), skip);
			library_sort_key(names[a], legacy, sizeof(legacy));
			assert(!strcmp(ka, legacy));
			for (unsigned b = 0; b < sizeof(names) / sizeof(names[0]); b++) {
				char kb[LIBRARY_SORT_KEY_MAX];
				library_sort_key_ex(names[b], kb, sizeof(kb), skip);
				assert(sign(strcmp(ka, kb)) == sign(library_collate_listorder(NULL, strlen(names[a]), names[a], strlen(names[b]), names[b])));
			}
		}
	}
	char before[LIBRARY_SORT_KEY_MAX], after[LIBRARY_SORT_KEY_MAX];
	library_set_skip_articles(true);
	library_sort_key_ex("The Beatles", before, sizeof(before), true);
	library_set_skip_articles(false);
	library_sort_key_ex("The Beatles", after, sizeof(after), true);
	assert(!strcmp(before, after));
	// Query maps use persisted keys, not the current Music article preference.
	nd_store_t store = {0};
	snprintf(store.scope, sizeof(store.scope), "fixture");
	assert(sqlite3_open(":memory:", &store.db) == SQLITE_OK);
	assert(sqlite3_exec(store.db,
						"CREATE TABLE snapshot(id INTEGER,scope TEXT,complete INT); INSERT INTO snapshot VALUES(1,'fixture',1);"
						"CREATE TABLE entity(gen INT,kind INT,id TEXT,title TEXT,artist TEXT,cover TEXT,duration INT,disc INT,track INT,album TEXT,sortkey TEXT);"
						"CREATE TABLE rel(gen INT,album TEXT,kind INT,id TEXT);",
						NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_stmt *st;
	assert(sqlite3_prepare_v2(store.db, "INSERT INTO entity VALUES(1,0,?,?,'','',0,0,0,'',?)", -1, &st, NULL) == SQLITE_OK);
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		char id[32], key[LIBRARY_SORT_KEY_MAX];
		snprintf(id, sizeof(id), "id-%02u", i);
		library_sort_key_ex(names[i], key, sizeof(key), true);
		sqlite3_reset(st);
		sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 2, names[i], -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 3, key, -1, SQLITE_TRANSIENT);
		assert(sqlite3_step(st) == SQLITE_DONE);
	}
	sqlite3_finalize(st);
	int letters[ND_LETTER_BUCKETS], original[ND_LETTER_BUCKETS];
	nd_query_t query = {.kind = ND_ALBUMS};
	assert(nd_store_letters(&store, 1, &query, letters));
	memcpy(original, letters, sizeof(letters));
	assert(letters[0] == 0 && letters[2] >= 0 && letters[5] >= 0 && letters[27] > letters[26]);
	for (int bucket = 0; bucket < ND_LETTER_BUCKETS; bucket++) {
		if (letters[bucket] < 0)
			continue;
		nd_row_t rows[ND_WINDOW];
		int count, total;
		assert(nd_store_query(&store, 1, &query, letters[bucket], rows, &count, &total) && count);
		char key[LIBRARY_SORT_KEY_MAX];
		library_sort_key_ex(rows[0].title, key, sizeof(key), true);
		assert(library_index_slot(library_index_letter_of_key(key)) == bucket);
	}
	library_set_skip_articles(true);
	assert(nd_store_letters(&store, 1, &query, letters) && !memcmp(original, letters, sizeof(letters)));
	assert(sqlite3_exec(store.db, "INSERT INTO rel VALUES(1,'id-05',2,'selected'); INSERT INTO rel VALUES(1,'id-06',2,'selected');", NULL, NULL, NULL) == SQLITE_OK);
	query.filter_kind = ND_ARTISTS;
	snprintf(query.filter, sizeof(query.filter), "selected");
	assert(nd_store_letters(&store, 1, &query, letters));
	for (int i = 0; i < ND_LETTER_BUCKETS; i++)
		assert(letters[i] == (i == 2 ? 0 : -1));
	// Descending maps must address the first displayed member of each bucket,
	// rather than reversing the ascending bucket offsets.
	memset(&query, 0, sizeof(query));
	query.kind = ND_ALBUMS;
	query.order = ND_ORDER_NAME_DESC;
	assert(nd_store_letters(&store, 1, &query, letters));
	for (int bucket = 0; bucket < ND_LETTER_BUCKETS; bucket++) {
		if (letters[bucket] < 0)
			continue;
		nd_row_t rows[ND_WINDOW];
		int count, total;
		assert(nd_store_query(&store, 1, &query, letters[bucket], rows, &count, &total) && count);
		char key[LIBRARY_SORT_KEY_MAX];
		library_sort_key_ex(rows[0].title, key, sizeof(key), true);
		assert(library_index_slot(library_index_letter_of_key(key)) == bucket);
	}
	// Real optional years, unknowns, equal-name ties and neighbor wrapping.
	assert(sqlite3_exec(store.db,
						"CREATE TABLE album_display(gen INT,handle INT,year INT,genre TEXT,PRIMARY KEY(gen,handle));"
						"INSERT INTO album_display SELECT gen,rowid,CASE id WHEN 'id-05' THEN 1990 WHEN 'id-06' THEN 2020 ELSE 0 END,'' FROM entity;",
						NULL, NULL, NULL) == SQLITE_OK);
	store.display = true;
	store.view = -1;
	for (int direction = 0; direction < 2; direction++) {
		query.order = direction ? ND_ORDER_YEAR_DESC : ND_ORDER_YEAR;
		nd_row_t rows[ND_WINDOW];
		int count, total;
		assert(nd_store_query(&store, 1, &query, 0, rows, &count, &total));
		assert(!strcmp(rows[0].id, direction ? "id-06" : "id-05"));
		assert(!strcmp(rows[1].id, direction ? "id-05" : "id-06"));
		assert(nd_store_letters(&store, 1, &query, letters));
		for (int i = 0; i < ND_LETTER_BUCKETS; i++)
			assert(letters[i] == -1);
		char neighbor[ND_ID_MAX];
		assert(nd_store_album_beside_order(&store, 1, rows[0].id, true, query.order, neighbor));
		assert(!strcmp(neighbor, rows[1].id));
		assert(nd_store_album_beside_order(&store, 1, rows[0].id, false, query.order, neighbor));
		assert(!strcmp(neighbor, rows[count - 1].id));
	}
	// Read-only/old snapshots without the optional table retain all albums.
	assert(sqlite3_exec(store.db, "DROP TABLE album_display", NULL, NULL, NULL) == SQLITE_OK);
	store.display = false;
	store.view = -1;
	nd_row_t fallback[ND_WINDOW], ascending[ND_WINDOW];
	int nf, na, total;
	assert(nd_store_query(&store, 1, &query, 0, fallback, &nf, &total));
	query.order = ND_ORDER_NAME;
	assert(nd_store_query(&store, 1, &query, 0, ascending, &na, &total));
	assert(nf == na && !memcmp(fallback, ascending, sizeof(fallback)));
	// Complete 17,000-track index capture must follow the same order as every
	// displayed 16-row window; no first-window-only or ID-based inversion.
	assert(sqlite3_exec(store.db,
						"WITH RECURSIVE n(v) AS (VALUES(0) UNION ALL SELECT v+1 FROM n WHERE v<16999) "
						"INSERT INTO entity SELECT 1,1,printf('t%05d',v),printf('Track %05d',v),'','',1,0,0,'',printf('track %05d',v) FROM n;",
						NULL, NULL, NULL) == SQLITE_OK);
	query.kind = ND_TRACKS;
	for (int direction = 0; direction < 2; direction++) {
		query.order = direction ? ND_ORDER_NAME_DESC : ND_ORDER_NAME;
		int64_t *handles = NULL;
		int captured, start;
		assert(nd_store_play_index(&store, 1, &query, "t08500", &handles, &captured, &start));
		assert(captured == 17000 && start == (direction ? 8499 : 8500));
		for (int first = 0; first < 17000; first += ND_WINDOW) {
			nd_row_t rows[ND_WINDOW];
			int count;
			assert(nd_store_query(&store, 1, &query, first, rows, &count, &total) && total == captured);
			for (int i = 0; i < count; i++) {
				char id[32];
				snprintf(id, sizeof(id), "t%05d", direction ? 16999 - first - i : first + i);
				assert(!strcmp(id, rows[i].id));
				assert(sqlite3_prepare_v2(store.db, "SELECT id FROM entity WHERE rowid=?", -1, &st, NULL) == SQLITE_OK);
				sqlite3_bind_int64(st, 1, handles[first + i]);
				assert(sqlite3_step(st) == SQLITE_ROW && !strcmp(id, (const char *)sqlite3_column_text(st, 0)));
				sqlite3_finalize(st);
			}
		}
		free(handles);
	}
	// A Favorites view binds changed albums to negative local handles and
	// retained albums to physical base owners, not the logical view generation.
	assert(sqlite3_exec(store.db,
						"CREATE TABLE album_display(gen INT,handle INT,year INT,genre TEXT,PRIMARY KEY(gen,handle));"
						"INSERT INTO album_display SELECT 1,rowid,CASE id WHEN 'id-05' THEN 1970 ELSE 1990 END,'' FROM entity WHERE kind=0 AND id IN ('id-05','id-06');"
						"INSERT INTO snapshot VALUES(2,'fixture',1);"
						"CREATE TABLE favorite_song(gen INT,id TEXT,starred INT);"
						"CREATE TABLE local_view(gen INT,base INT);INSERT INTO local_view VALUES(2,1);"
						"CREATE TABLE local_change(key INTEGER PRIMARY KEY,seq INT,scope TEXT,kind INT,id TEXT,title TEXT,artist TEXT,album TEXT,cover TEXT,duration INT,disc INT,track INT,sortkey TEXT,starred INT,details INT);"
						"INSERT INTO local_change VALUES(1,2,'fixture',0,'id-05','Changed Beatles','','','',0,0,0,'changed beatles',1,0);"
						"INSERT INTO album_display VALUES(2,-1,2024,'');",
						NULL, NULL, NULL) == SQLITE_OK);
	store.favorites = store.local = store.display = true;
	store.view = -1;
	query = (nd_query_t){.kind = ND_ALBUMS, .favorites = true, .order = ND_ORDER_YEAR};
	assert(nd_store_query(&store, 2, &query, 0, fallback, &nf, &total) && total == 16);
	assert(!strcmp(fallback[0].id, "id-06") && !strcmp(fallback[1].id, "id-05"));
	query.order = ND_ORDER_YEAR_DESC;
	assert(nd_store_query(&store, 2, &query, 0, fallback, &nf, &total) && total == 16);
	assert(!strcmp(fallback[0].id, "id-05") && !strcmp(fallback[1].id, "id-06"));
	puts("Favorites year sort resolves positive base and negative changed metadata owners: passed");
	// A separate 1,700-album source exercises every fixed window in all four
	// modes, with repeated years and unknown values at the tail.
	assert(sqlite3_exec(store.db, "DELETE FROM local_change; DELETE FROM local_view; DELETE FROM entity; DELETE FROM album_display;", NULL, NULL, NULL) == SQLITE_OK);
	store.favorites = store.local = false;
	store.view = -1;
	assert(sqlite3_prepare_v2(store.db, "INSERT INTO entity VALUES(1,0,?,?,'','',0,0,0,'',?)", -1, &st, NULL) == SQLITE_OK);
	for (int i = 0; i < 1700; i++) {
		char name[40];
		snprintf(name, sizeof(name), "Album %04d", i);
		sqlite3_reset(st);
		sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 3, name, -1, SQLITE_TRANSIENT);
		assert(sqlite3_step(st) == SQLITE_DONE);
	}
	sqlite3_finalize(st);
	assert(sqlite3_exec(store.db, "INSERT INTO album_display SELECT 1,rowid,CASE WHEN rowid%11=0 THEN 0 ELSE 1990+rowid%30 END,'' FROM entity", NULL, NULL, NULL) == SQLITE_OK);
	for (int mode = 0; mode < 4; mode++) {
		query = (nd_query_t){.kind = ND_ALBUMS, .order = (nd_order_t)mode};
		int last_year = mode == ND_ORDER_YEAR ? 0 : 10000;
		bool unknown = false;
		char previous[ND_ID_MAX] = "";
		for (int first = 0; first < 1700; first += ND_WINDOW) {
			nd_row_t rows[ND_WINDOW];
			int n;
			assert(nd_store_query(&store, 1, &query, first, rows, &n, &total) && total == 1700);
			for (int i = 0; i < n; i++) {
				if (previous[0] && mode < 2)
					assert(mode == ND_ORDER_NAME ? strcmp(previous, rows[i].id) < 0 : strcmp(previous, rows[i].id) > 0);
				if (mode >= 2) {
					assert(sqlite3_prepare_v2(store.db, "SELECT year FROM album_display WHERE handle=(SELECT rowid FROM entity WHERE id=?)", -1, &st, NULL) == SQLITE_OK);
					sqlite3_bind_text(st, 1, rows[i].id, -1, SQLITE_TRANSIENT);
					assert(sqlite3_step(st) == SQLITE_ROW);
					int year = sqlite3_column_int(st, 0);
					sqlite3_finalize(st);
					if (!year)
						unknown = true;
					else {
						assert(!unknown && (mode == ND_ORDER_YEAR ? year >= last_year : year <= last_year));
						last_year = year;
					}
				}
				strcpy(previous, rows[i].id);
			}
		}
	}
	puts("All four complete-query orders across 1,700 albums with year ties/unknowns: passed");
	// Rich track sorts use exact retained albums/credits, with immutable index
	// capture matching every sampled window of a 17,000-entry complete query.
	assert(sqlite3_exec(store.db, "DELETE FROM entity; DELETE FROM album_display; CREATE INDEX IF NOT EXISTS fixture_entity ON entity(gen,kind,id); CREATE TABLE album_credit(gen INT,handle INT,credit TEXT,PRIMARY KEY(gen,handle));", NULL, NULL, NULL) == SQLITE_OK);
	assert(sqlite3_exec(store.db, "INSERT INTO entity VALUES(1,0,'a','A album','The Band Z','',0,0,0,'','a album'),(1,0,'b','B album','Band A','',0,0,0,'','b album'); INSERT INTO album_credit SELECT 1,rowid,CASE id WHEN 'a' THEN 'Explicit credit A' ELSE 'Explicit credit Z' END FROM entity;", NULL, NULL, NULL) == SQLITE_OK);
	assert(sqlite3_prepare_v2(store.db, "INSERT INTO entity VALUES(1,1,?,?,?,'',0,?,?,?,?)", -1, &st, NULL) == SQLITE_OK);
	for (int i = 0; i < 17000; i++) {
		char id[40], name[40], key[40];
		snprintf(id, sizeof(id), "rich-%05d", i);
		snprintf(name, sizeof(name), "Title %05d", 16999 - i);
		library_sort_key_ex(name, key, sizeof(key), true);
		sqlite3_reset(st);
		sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 3, i % 3 == 0 ? "Artist Z" : i % 3 == 1 ? "The Artist A" : "", -1, SQLITE_STATIC);
		sqlite3_bind_int(st, 4, 1 + i % 2);
		sqlite3_bind_int(st, 5, i % 7 ? 1 + i % 10 : 0);
		sqlite3_bind_text(st, 6, i % 3 == 0 ? "a" : i % 3 == 1 ? "b" : "missing", -1, SQLITE_STATIC);
		sqlite3_bind_text(st, 7, key, -1, SQLITE_TRANSIENT);
		assert(sqlite3_step(st) == SQLITE_DONE);
	}
	sqlite3_finalize(st);
	store.view = -1;
	for (int mode = ND_ORDER_ALBUM; mode <= ND_ORDER_ARTIST_DESC; mode++) {
		query = (nd_query_t){.kind = ND_TRACKS, .order = (nd_order_t)mode};
		int64_t *handles = NULL;
		int n, start;
		assert(nd_store_play_index(&store, 1, &query, "rich-08500", &handles, &n, &start) && n == 17000 && start >= 0);
		for (int sample = 0; sample < 3; sample++) {
			nd_row_t rows[ND_WINDOW];
			int count, first = sample == 0 ? 0 : sample == 1 ? 8500 : 16984;
			assert(nd_store_query(&store, 1, &query, first, rows, &count, &total) && count == 16 && total == 17000);
			for (int j = 0; j < count; j++) {
				assert(sqlite3_prepare_v2(store.db, "SELECT id FROM entity WHERE rowid=?", -1, &st, NULL) == SQLITE_OK);
				sqlite3_bind_int64(st, 1, handles[first + j]);
				assert(sqlite3_step(st) == SQLITE_ROW && !strcmp(rows[j].id, (const char *)sqlite3_column_text(st, 0)));
				sqlite3_finalize(st);
			}
			if (sample == 0)
				assert((mode <= ND_ORDER_ALBUM_DESC ? (!strcmp(rows[0].artist, "Artist Z") == (mode == ND_ORDER_ALBUM)) : mode <= ND_ORDER_ALBUM_ARTIST_DESC ? (!strcmp(rows[0].artist, "Artist Z") == (mode == ND_ORDER_ALBUM_ARTIST)) : (strstr(rows[0].artist, "Artist A") != NULL) == (mode == ND_ORDER_ARTIST)));
			if (sample == 2)
				assert(!rows[count - 1].artist[0]);
		}
		assert(nd_store_letters(&store, 1, &query, letters));
		for (int j = 0; j < ND_LETTER_BUCKETS; j++)
			assert(letters[j] == -1);
		free(handles);
	}
	puts("All Tracks album/album-artist/artist directions: exact credits, missing-last, 17000-entry capture/window agreement and no title strip: passed");
	sqlite3_close(store.db);
	puts("Shared Unicode ordering, legacy callers, and fixed snapshot policy: passed");
	puts("Complete-query letter positions, accent/article/non-Latin buckets, duplicate labels and filtered albums: passed");
	puts("Name directions, complete 17,000-track row/index agreement, year unknowns/legacy fallback and neighbor order: passed");
	return 0;
}

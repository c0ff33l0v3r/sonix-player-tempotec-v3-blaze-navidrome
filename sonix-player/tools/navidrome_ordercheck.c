#include "src/system/library/library.h"
#include "src/system/streaming/navidromestore.h"
#include <assert.h>
#include <stdio.h>
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
	sqlite3_close(store.db);
	puts("Shared Unicode ordering, legacy callers, and fixed snapshot policy: passed");
	puts("Complete-query letter positions, accent/article/non-Latin buckets, duplicate labels and filtered albums: passed");
	return 0;
}

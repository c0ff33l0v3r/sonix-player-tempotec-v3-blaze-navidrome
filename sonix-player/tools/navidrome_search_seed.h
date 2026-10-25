// Synthetic large snapshot shared by the focused search checks.
static void seed(const nd_library_t *library) {
	char dir[1100];
	snprintf(dir, sizeof(dir), "%s", library->path);
	*strrchr(dir, '/') = 0;
	for (char *p = dir + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = 0;
		assert(!mkdir(dir, 0700) || errno == EEXIST);
		*p = '/';
	}
	assert(!mkdir(dir, 0700) || errno == EEXIST);
	nd_store_t store;
	assert(nd_store_open(&store, library->path, library->scope, true, false));
	assert(nd_store_begin(&store));
	assert(sqlite3_exec(store.db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_stmt *st;
	assert(sqlite3_prepare_v2(store.db, "INSERT INTO entity VALUES(?,?,?,?,'Fixture',?,'',120,?,?,?)", -1, &st, NULL) == SQLITE_OK);
	for (int kind = 0; kind < 4; kind++) {
		int total = kind == ND_TRACKS ? 17000 : kind == ND_ALBUMS ? 1700 : 1;
		for (int i = 0; i < total; i++) {
			char id[64], title[64], album[64] = "";
			snprintf(id, sizeof(id), kind == ND_TRACKS ? "song-%05d" : kind == ND_ALBUMS ? "album-%05d" : kind == ND_ARTISTS ? "artist-%05d" : "genre-%05d", i);
			if (kind == ND_TRACKS)
				snprintf(title, sizeof(title), "%c Track %05d", 'A' + i / 654, i);
			else
				snprintf(title, sizeof(title), "%s %05d", kind == ND_ALBUMS ? "Album" : kind == ND_ARTISTS ? "Artist" : "Genre", i);
			if (kind == ND_TRACKS)
				snprintf(album, sizeof(album), "album-%05d", i / 10);
			sqlite3_reset(st);
			sqlite3_bind_int64(st, 1, store.generation);
			sqlite3_bind_int(st, 2, kind);
			sqlite3_bind_text(st, 3, id, -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(st, 4, title, -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(st, 5, album, -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(st, 6, i % 2);
			sqlite3_bind_int(st, 7, i % 10);
			char key[LIBRARY_SORT_KEY_MAX];
			library_sort_key_ex(title, key, sizeof(key), true);
			sqlite3_bind_text(st, 8, key, -1, SQLITE_TRANSIENT);
			assert(sqlite3_step(st) == SQLITE_DONE);
		}
	}
	sqlite3_finalize(st);
	for (int i = 0; i < 1700; i++) {
		char text[256];
		snprintf(text, sizeof(text), "INSERT INTO rel VALUES(%lld,'album-%05d',2,'artist-00000'); INSERT INTO rel VALUES(%lld,'album-%05d',3,'genre-00000');", (long long)store.generation, i, (long long)store.generation, i);
		assert(sqlite3_exec(store.db, text, NULL, NULL, NULL) == SQLITE_OK);
	}
	assert(sqlite3_exec(store.db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
	assert(nd_store_validate(&store) && nd_store_publish(&store));
	nd_store_close(&store);
}

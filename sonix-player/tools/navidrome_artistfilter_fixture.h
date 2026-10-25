// Store-level boundary fixtures; all names, identities and paths are synthetic.
static void artist_fixture_import(nd_store_t *store, nd_kind_t kind, const char *entries) {
	static const char *keys[] = {"album", "song", "artist"};
	nd_document_t doc = {0};
	size_t size = strlen(entries) + 100;
	doc.body = malloc(size);
	assert(doc.body);
	snprintf(doc.body, size, "{\"subsonic-response\":{\"searchResult3\":{\"%s\":%s}}}", keys[kind], entries);
	assert(json_parse_limited(doc.body, &doc.doc, ND_API_TOKENS));
	doc.response = json_get(&doc.doc, json_root(&doc.doc), "subsonic-response");
	int count;
	assert(nd_store_import(store, kind, &doc, &count));
	nd_api_free(&doc);
}

static void artist_filter_checks(const char *root) {
	char path[1200];
	snprintf(path, sizeof(path), "%s/artist-filter.db", root);
	nd_store_t store;
	assert(nd_store_open(&store, path, "artist-fixture", true, false));
	assert(nd_store_begin(&store));
	artist_fixture_import(&store, ND_ARTISTS,
						  "[{\"id\":\"album-artist\",\"name\":\"Alpha\"},"
						  "{\"id\":\"track-artist\",\"name\":\"Beta\",\"albumCount\":0},"
						  "{\"id\":\"hash\",\"name\":\"#Valid\",\"roles\":[\"unknown-role\"]},"
						  "{\"id\":\"duplicate-a\",\"name\":\"\\u65e5\\u672c\"},"
						  "{\"id\":\"duplicate-b\",\"name\":\"日本\",\"roles\":[]},"
						  "{\"id\":\"performer\",\"name\":\"########\",\"roles\":[\"performer\"]},"
						  "{\"id\":\"readable\",\"name\":\"A readable contributor\",\"albumCount\":99},"
						  "{\"id\":\"other-generation\",\"name\":\"Another\"}]");
	artist_fixture_import(&store, ND_ALBUMS,
						  "[{\"id\":\"one\",\"name\":\"One\",\"artistId\":\"album-artist\"},"
						  "{\"id\":\"two\",\"name\":\"Two\",\"artists\":[{\"id\":\"hash\"},{\"id\":\"duplicate-a\"},{\"id\":\"duplicate-b\"}]}]");
	artist_fixture_import(&store, ND_TRACKS,
						  "[{\"id\":\"song\",\"title\":\"Song\",\"albumId\":\"one\",\"artists\":[{\"id\":\"track-artist\"},{\"id\":\"track-artist\"}]},"
						  "{\"id\":\"song2\",\"title\":\"Song 2\",\"albumId\":\"two\",\"artistId\":\"track-artist\"}]");
	assert(nd_store_validate(&store) && nd_store_publish(&store));
	int64_t first = store.generation;
	nd_library_t status = {0};
	assert(nd_store_status(&store, &status));
	assert(status.counts[ND_ARTISTS] == 5 && status.counts[ND_ALBUMS] == 2 && status.counts[ND_TRACKS] == 2);
	time_t finished = status.finished;
	nd_query_t query = {.kind = ND_ARTISTS};
	nd_row_t rows[ND_WINDOW];
	int count, total, letters[ND_LETTER_BUCKETS];
	assert(nd_store_query(&store, first, &query, 0, rows, &count, &total));
	assert(total == 5 && count == 5);
	const char *ids[] = {"hash", "album-artist", "track-artist", "duplicate-a", "duplicate-b"};
	for (int i = 0; i < count; i++) {
		assert(!strcmp(rows[i].id, ids[i]));
		assert(nd_store_artist_browsable(&store, first, ids[i]));
	}
	assert(!strcmp(rows[3].title, rows[4].title));
	assert(nd_store_letters(&store, first, &query, letters));
	assert(letters[0] == 0 && letters[1] == 1 && letters[2] == 2 && letters[27] == 3);
	assert(nd_store_contains(&store, first, ND_ARTISTS, "performer"));
	assert(!nd_store_artist_browsable(&store, first, "performer"));
	nd_playrow_t hidden;
	assert(nd_store_metadata(&store, first, ND_ARTISTS, "performer", &hidden));
	assert(!strcmp(hidden.row.title, "########"));
	nd_store_close(&store);
	struct stat before, after;
	assert(!stat(path, &before) && !chmod(path, 0400));
	assert(nd_store_open(&store, path, "artist-fixture", false, false));
	assert(sqlite3_db_readonly(store.db, "main") == 1);
	assert(nd_store_status(&store, &status) && status.finished == finished && status.counts[ND_ARTISTS] == 5);
	assert(nd_store_query(&store, first, &query, 4, rows, &count, &total) && total == 5 && count == 1);
	nd_store_close(&store);
	assert(!stat(path, &after) && before.st_size == after.st_size && before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec);
	assert(!chmod(path, 0600));
	assert(nd_store_open(&store, path, "artist-fixture", true, false));
	assert(nd_store_begin(&store));
	artist_fixture_import(&store, ND_ARTISTS, "[{\"id\":\"track-artist\",\"name\":\"Beta\"}]");
	assert(nd_store_validate(&store) && nd_store_publish(&store));
	int64_t empty = store.generation;
	// Keep the earlier complete generation and a later incomplete candidate.
	assert(nd_store_begin(&store));
	artist_fixture_import(&store, ND_ARTISTS, "[{\"id\":\"track-artist\",\"name\":\"Beta\"}]");
	artist_fixture_import(&store, ND_ALBUMS, "[{\"id\":\"one\",\"name\":\"One\",\"artistId\":\"track-artist\"}]");
	assert(!nd_store_artist_browsable(&store, store.generation, "track-artist"));
	assert(nd_store_status(&store, &status) && status.generation == empty && !status.counts[ND_ARTISTS]);
	assert(nd_store_contains(&store, empty, ND_ARTISTS, "track-artist"));
	assert(!nd_store_artist_browsable(&store, empty, "track-artist"));
	// Deliberate invalid persisted relation: test reads without relaxing validation.
	char sql[256];
	snprintf(sql, sizeof(sql), "INSERT INTO rel VALUES(%lld,'one',2,'track-artist')", (long long)empty);
	assert(sqlite3_exec(store.db, sql, NULL, NULL, NULL) == SQLITE_OK);
	assert(!nd_store_artist_browsable(&store, empty, "track-artist"));
	assert(nd_store_query(&store, empty, &query, 0, rows, &count, &total) && !total && !count);
	assert(nd_store_letters(&store, empty, &query, letters));
	for (int i = 0; i < ND_LETTER_BUCKETS; i++)
		assert(letters[i] == -1);
	assert(nd_store_artist_browsable(&store, first, "track-artist"));
	nd_store_close(&store);
	assert(nd_store_open(&store, path, "different-scope", false, false));
	assert(!nd_store_artist_browsable(&store, first, "track-artist"));
	nd_store_close(&store);
	assert(!unlink(path));
	puts("artist eligibility: album/track links, exact IDs/Unicode/hash names, hidden metadata, read-only unchanged cache, generations/scope/dangling links: passed");
}

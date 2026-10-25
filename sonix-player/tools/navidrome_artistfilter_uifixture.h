static nd_library_t artist_ui_reload(const char *root) {
	navidrome_set_root(root);
	nd_library_t library;
	for (int i = 0; i < 10000; i++) {
		navidrome_library_snapshot(&library);
		if (!library.loading)
			return library;
		usleep(1000);
	}
	assert(!"artist snapshot reload timeout");
	return library;
}

static void artist_ui_publish(nd_library_t library, int visible) {
	nd_store_t store;
	assert(nd_store_open(&store, library.path, library.scope, true, false));
	assert(nd_store_begin(&store));
	char sql[700];
	snprintf(sql, sizeof(sql), "BEGIN; INSERT INTO entity SELECT %lld,kind,id,title,artist,album,cover,duration,disc,track,sortkey FROM entity WHERE gen=%lld AND kind<>2; INSERT INTO rel SELECT %lld,album,kind,id FROM rel WHERE gen=%lld AND kind<>2;", (long long)store.generation, (long long)library.generation, (long long)store.generation, (long long)library.generation);
	assert(sqlite3_exec(store.db, sql, NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_stmt *st;
	assert(sqlite3_prepare_v2(store.db, "INSERT INTO entity VALUES(?,2,?,?,'','','',0,0,0,?)", -1, &st, NULL) == SQLITE_OK);
	for (int i = 0; i < 80; i++) {
		char id[64], title[64], key[LIBRARY_SORT_KEY_MAX];
		snprintf(id, sizeof(id), "credit-%02d", i);
		if (!i)
			snprintf(title, sizeof(title), "#Valid artist");
		else
			snprintf(title, sizeof(title), "%c %s %02d", 'A' + i % 26, i < 40 ? "Artist" : "Contributor", i);
		library_sort_key_ex(title, key, sizeof(key), true);
		sqlite3_reset(st);
		sqlite3_bind_int64(st, 1, store.generation);
		sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 3, title, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 4, key, -1, SQLITE_TRANSIENT);
		assert(sqlite3_step(st) == SQLITE_DONE);
		if (i < visible) {
			snprintf(sql, sizeof(sql), "INSERT INTO rel VALUES(%lld,'album-00000',2,'credit-%02d')", (long long)store.generation, i);
			assert(sqlite3_exec(store.db, sql, NULL, NULL, NULL) == SQLITE_OK);
		}
	}
	sqlite3_finalize(st);
	assert(sqlite3_exec(store.db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
	assert(nd_store_validate(&store) && nd_store_publish(&store));
	nd_store_close(&store);
}

static void artist_ui_idle(int level) {
	for (int i = 0; i < 10000; i++) {
		drain();
		lv_obj_update_layout(pages[level].screen);
		if (lv_screen_active() == pages[level].screen && !pages[level].loading)
			return;
		usleep(1000);
	}
	assert(!"artist UI timeout");
}

static nd_library_t artist_ui_checks(nd_library_t library, const char *root) {
	unsigned selections = playback_selections, notices = fixture_notices;
	artist_ui_publish(library, 40);
	library = artist_ui_reload(root);
	assert(library.counts[ND_ARTISTS] == 40);
	navidromebrowse_open(ND_ARTISTS);
	window_wait(0);
	assert(pages[0].total == 40 && pages[0].index->enabled && pages[0].letters[0] == 0);
	assert(!strcmp(pages[0].data[0].title, "#Valid artist"));
	go(pages[0].index, 26);
	window_wait(0);
	assert(pages[0].first + pages[0].count == 40);
	assert(!strcmp(pages[0].data[pages[0].count - 1].id, "credit-25"));
	for (int i = 0; i < pages[0].count; i++)
		assert(atoi(pages[0].data[i].id + 7) < 40);
	go(pages[0].index, 2);
	go(pages[0].index, 13);
	go(pages[0].index, 26);
	window_wait(0);
	assert(pages[0].first <= pages[0].letters[26] && pages[0].first + pages[0].count > pages[0].letters[26]);
	// Return to the tail of the old list after it shrinks below the strip threshold.
	artist_ui_publish(library, 29);
	library = artist_ui_reload(root);
	navidromebrowse_refresh();
	artist_ui_idle(0);
	assert(pages[0].total == 29 && pages[0].first < 29 && !pages[0].index->enabled);
	artist_ui_publish(library, 30);
	library = artist_ui_reload(root);
	navidromebrowse_refresh();
	artist_ui_idle(0);
	assert(pages[0].total == 30 && pages[0].index->enabled);
	// Open an artist's album and then its tracks; the album survives when the
	// artist loses every link, so identity-only validation would leave this path open.
	navidromebrowse_open(ND_ARTISTS);
	window_wait(0);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	window_wait(1);
	assert(pages[1].total == 1);
	lv_obj_send_event(pages[1].rows[0], LV_EVENT_CLICKED, NULL);
	window_wait(2);
	assert(pages[2].total == 10);
	// Hold a real old-generation row/map completion across publication.
	pages[2].map_ready = false;
	submit_window(&pages[2]);
	for (int i = 0; i < 10000; i++) {
		pthread_mutex_lock(&post_lock);
		bool queued = post_write > post_read;
		pthread_mutex_unlock(&post_lock);
		if (queued)
			break;
		usleep(1000);
		assert(i < 9999);
	}
	artist_ui_publish(library, 0);
	library = artist_ui_reload(root);
	drain(); // Rejected because the bound generation is no longer current.
	assert(lv_screen_active() == pages[2].screen);
	lv_tick_inc(2600);
	navidromebrowse_refresh();
	artist_ui_idle(0);
	assert(!pages[0].total && !pages[0].count && !pages[0].index->enabled);
	assert(!strcmp(lv_label_get_text(pages[0].status), "nd_library_empty"));
	assert(fixture_notices == notices + 1 && playback_selections == selections);
	nd_store_t store;
	assert(nd_store_open(&store, library.path, library.scope, false, false));
	assert(nd_store_contains(&store, library.generation, ND_ARTISTS, "credit-00"));
	assert(nd_store_contains(&store, library.generation, ND_ALBUMS, "album-00000"));
	nd_store_close(&store);
	// Repeat loss from the artist Albums page rather than its descendant.
	artist_ui_publish(library, 30);
	library = artist_ui_reload(root);
	navidromebrowse_open(ND_ARTISTS);
	window_wait(0);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	window_wait(1);
	artist_ui_publish(library, 0);
	library = artist_ui_reload(root);
	navidromebrowse_refresh();
	artist_ui_idle(0);
	assert(!pages[0].total && fixture_notices == notices + 2);
	// A direct album entry must not inherit the retained ancestor from page 1.
	switch_screen(navidrome_screen);
	assert(navidromebrowse_album("album-00000", "Album"));
	window_wait(0);
	assert(pages[0].total == 10);
	seed(&library);
	library = artist_ui_reload(root);
	lv_display_t *display = lv_display_get_default();
	printf("artist LVGL %dx%d: filtered windows/Z jumps, 29/30 threshold, empty lists, retained-artist navigation/descendants, late row/map rejection, direct album isolation: passed\n", lv_display_get_horizontal_resolution(display), lv_display_get_vertical_resolution(display));
	return library;
}

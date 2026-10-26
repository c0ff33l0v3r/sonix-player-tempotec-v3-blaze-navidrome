// A known Favorites album can exist outside the complete synced album catalogue.
static void polish_album_fallback_check(void) {
	nd_library_t favorite, full, changed;
	navidrome_favorites_snapshot(&favorite);
	navidrome_library_snapshot(&full);
	nd_context_t context;
	assert(navidrome_context(&context, false));
	nd_store_t store;
	assert(nd_store_open_source(&store, favorite.path, favorite.scope, true, false, true));
	nd_playrow_t album = {.row = {.id = "outside-album", .title = "Outside synced catalogue"}};
	nd_playrow_t track = {.row = {.id = "song-40000", .title = "Outside track"}, .album_id = "outside-album", .album = "Outside synced catalogue"};
	changed = favorite;
	assert(nd_store_local_begin(&store) && nd_store_local_put(&store, ND_ALBUMS, &album, true, 0) && nd_store_local_put(&store, ND_TRACKS, &track, true, 0) && nd_store_local_activate(&store));
	assert(navidrome_context_commit(&context, store.db) && nd_store_status(&store, &changed) && navidrome_favorites_adopt(&context, &changed));
	nd_store_close(&store);
	nd_wipe(&context, sizeof(context));
	navidrome_favorites_snapshot(&favorite);
	nd_query_t query = {.kind = ND_TRACKS, .favorites = true, .filter_kind = ND_ALBUMS, .filter = "outside-album"};
	chain_enabled = true;
	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	int before = starts;
	assert(navidromeplay_select(&favorite, &query, "song-40000", lv_screen_active()));
	wait_started(before + 1);
	assert(playlist_count() == 1 && navidromeplay_chain_boundary(true, true));
	album_coordinator_idle();
	assert(starts == before + 1 && playback.status == AUDIO_STATUS_STOPPED && playlist_count() == 1);
	playlist_set_mode(PLAYBACK_MODE_REPEAT_ALL);
	assert(navidromeplay_chain_boundary(true, true));
	wait_started(before + 2);
	assert(playlist_count() == 1);
	// Detach first: no live queue or card lease is moved. All paths belong to
	// this process's synthetic card, never the simulator/user card.
	char root[1200], saved[1200];
	snprintf(root, sizeof(root), "%s", full.path);
	char *relative = strstr(root, "/.local/navidrome/");
	assert(relative);
	*relative = 0;
	snprintf(saved, sizeof(saved), "%s.fixture-held", full.path);
	navidromeplay_stop();
	navidrome_set_root(NULL);
	drain();
	navidromeplay_tick();
	assert(!rename(full.path, saved));
	navidrome_set_root(root);
	for (int i = 0; i < 10000; i++) {
		navidrome_favorites_snapshot(&favorite);
		navidrome_library_snapshot(&full);
		if (!favorite.loading && !full.loading)
			break;
		usleep(1000);
	}
	assert(favorite.ready && !full.ready);
	before = starts;
	assert(navidromeplay_select(&favorite, &query, "song-40000", lv_screen_active()));
	wait_started(before + 1);
	unsigned notices = fixture_notices;
	int requests = atomic_load(&calls), transfers = atomic_load(&fetches);
	assert(!navidromeplay_chain_boundary(true, false) && fixture_notices == notices + 1 && starts == before + 1);
	assert(atomic_load(&calls) == requests && atomic_load(&fetches) == transfers);
	navidromeplay_stop();
	navidrome_set_root(NULL);
	drain();
	navidromeplay_tick();
	assert(access(full.path, F_OK) && !rename(saved, full.path));
	navidrome_set_root(root);
	for (int i = 0; i < 10000; i++) {
		navidrome_library_snapshot(&full);
		if (!full.loading)
			break;
		usleep(1000);
	}
	assert(full.ready);
	query.favorites = false;
	snprintf(query.filter, sizeof(query.filter), "album-00000");
	assert(navidromeplay_select(&full, &query, "song-00000", lv_screen_active()));
	wait_started(before + 2);
	chain_enabled = false;
	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	puts("album fallback: Favorites-only album honors normal/repeat-all when absent from the pinned order; missing full metadata explains Sync without HTTP or audio work: passed");
}

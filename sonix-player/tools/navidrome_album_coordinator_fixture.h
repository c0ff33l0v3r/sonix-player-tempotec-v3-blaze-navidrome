// Runs after the ordinary production-coordinator fixtures. No real audio device.
static void album_coordinator_idle(void) {
	for (int i = 0; i < 10000; i++) {
		drain();
		navidromeplay_tick();
		if (!navidromeplay_preparing()) {
			usleep(2000);
			drain();
			return;
		}
		usleep(1000);
	}
	assert(!"album coordinator timeout");
}
static void album_coordinator_check(const nd_library_t *lib, lv_obj_t *origin) {
	nd_query_t album = {.kind = ND_TRACKS, .filter_kind = ND_ALBUMS};
	snprintf(album.filter, sizeof(album.filter), "album-01699");
	lv_screen_load(origin);
	atomic_store(&allow_ready, true);
	atomic_store(&fail_track, -1);
	playlist_set_mode(PLAYBACK_MODE_SHUFFLE_REPEAT);
	int started = starts;
	assert(navidromeplay_album(lib, &album, 1, origin));
	wait_started(started + 1);
	assert(playlist_count() == 10 && playlist_get_mode() == PLAYBACK_MODE_NORMAL);
	nd_playrow_t represented;
	nd_library_t represented_library;
	assert(navidromeplay_track(playback.current_file, &represented, &represented_library));
	assert(!strcmp(represented.row.id, "song-16990"));
	album_coordinator_idle();
	// A delayed Shuffle cannot change the installed mode until preparation succeeds.
	atomic_store(&allow_ready, false);
	int before = atomic_load(&fetches);
	assert(navidromeplay_album(lib, &album, 0, origin));
	wait_fetch(before);
	assert(playlist_get_mode() == PLAYBACK_MODE_NORMAL);
	assert(navidromeplay_album(lib, &album, 0, origin)); // duplicate pending activation
	atomic_store(&allow_ready, true);
	wait_started(started + 2);
	assert(playlist_count() == 10 && playlist_get_mode() == PLAYBACK_MODE_SHUFFLE);
	assert(navidromeplay_track(playback.current_file, &represented, &represented_library));
	assert(!strcmp(represented.album_id, album.filter));
	album_coordinator_idle();
	// Failure preserves the installed queue and mode.
	atomic_store(&fail_track, 16990);
	before = atomic_load(&fetches);
	assert(navidromeplay_album(lib, &album, 1, origin));
	wait_fetch(before);
	album_coordinator_idle();
	assert(starts == started + 2 && playlist_count() == 10 && playlist_get_mode() == PLAYBACK_MODE_SHUFFLE && playback.status == AUDIO_STATUS_PLAYING);
	atomic_store(&fail_track, -1);
	navidromeplay_stop();
	album_coordinator_idle();
	const char *local[] = {"/fixture/local-a.flac", "/fixture/local-b.flac"};
	playlist_load_paths(local, 2, 0);
	navidromeplay_other(local[0]);
	snprintf(playback.current_file, sizeof(playback.current_file), "%s", local[0]);
	playback.status = AUDIO_STATUS_PLAYING;
	navidrome_test_online(0);
	navidrome_poll_network();
	before = atomic_load(&fetches);
	int count = playlist_count();
	assert(navidromeplay_album(lib, &album, 2, origin));
	for (int i = 0; i < 10000 && playlist_count() == count; i++) {
		drain();
		navidromeplay_tick();
		usleep(1000);
	}
	assert(playlist_count() == 3 && starts == started + 2 && atomic_load(&fetches) == before);
	assert(!strcmp(playback.current_file, local[0]) && playback.status == AUDIO_STATUS_PLAYING);
	char ref[80];
	assert(playlist_path_at(1, ref, sizeof(ref)) && navidromeplay_reference(ref));
	nd_playrow_t row;
	nd_library_t saved;
	bool resolved = false;
	for (int i = 0; i < 10000 && !resolved; i++) {
		resolved = navidromeplay_lookup(ref, &row, &saved);
		drain();
		navidromeplay_tick();
		usleep(1000);
	}
	assert(resolved && !strcmp(row.album_id, album.filter));
	assert(!strcmp(fixture_last_success, "medialist_random_track_queued"));
	// Enqueue onto an empty queue remains stopped and resolves its own provider.
	playlist_clear();
	playback.current_file[0] = 0;
	playback.status = AUDIO_STATUS_STOPPED;
	assert(navidromeplay_album(lib, &album, 2, origin));
	for (int i = 0; i < 10000 && !playlist_count(); i++) {
		drain();
		navidromeplay_tick();
		usleep(1000);
	}
	assert(playlist_count() == 1 && starts == started + 2 && atomic_load(&fetches) == before && playback.status == AUDIO_STATUS_STOPPED);
	puts("album coordinator: complete Play/Shuffle, staged mode/failure, duplicate guard, random single-entry mixed/empty queues and owned metadata: passed");
}

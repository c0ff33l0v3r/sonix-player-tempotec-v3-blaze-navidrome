// Reuse real LVGL/service/queue/coordinator boundaries without changing the
// parallel Search/album/polish scenarios or their expected start counts.
#define NAVIDROME_COORDINATOR_MAIN ordinary_coordinator_main
#define config_get_bool ordinary_config_get_bool
#include "navidrome_coordinatorcheck.c"
#undef config_get_bool
#include "navidrome_owned_fixture.h"
static pthread_t fixture_gui_thread;
static atomic_bool fail_worker_allocation;
void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size) {
	if (!pthread_equal(pthread_self(), fixture_gui_thread) && atomic_exchange(&fail_worker_allocation, false)) return NULL;
	return __real_calloc(count, size);
}
static bool open_on_tap = true;
static bool continuous_shuffle;
bool config_get_bool(const char *section, const char *key, bool fallback) {
	if (!strcmp(section, "player") && !strcmp(key, "open_player_on_tap")) return open_on_tap;
	if (!strcmp(section, "player") && !strcmp(key, "endless_shuffle")) return continuous_shuffle;
	return ordinary_config_get_bool(section, key, fallback);
}
static nd_playlist_t saved;
static nd_playlist_entry_t saved_entries[ND_WINDOW];
static void saved_create(nd_library_t *lib, int count) {
	char path[1200];
	nd_playlist_store_t s;
	assert(nd_pl_path(lib, path) && nd_pl_open(&s, path, lib->scope, true));
	int64_t id;
	assert(nd_pl_create(&s, "Saved", &id) && nd_pl_begin(&s, id, 1));
	int added = 0, skipped = 0;
	for (int i = 0; i < count; i++) {
		nd_row_t row = {.artist = "Fallback artist", .duration = 23};
		snprintf(row.id, sizeof(row.id), "song-%05d", i % 2);
		snprintf(row.title, sizeof(row.title), "Saved occurrence %d", i);
		assert(nd_pl_append(&s, id, &row, false, &added, &skipped));
	}
	int n;
	assert(nd_pl_finish(&s, id, true) && nd_pl_sql(&s, "COMMIT") && nd_pl_get(&s, id, &saved));
	assert(nd_pl_entries(&s, id, 0, 0, saved_entries, &n));
	nd_pl_close(&s);
}
static bool saved_start(nd_library_t *lib, int position, lv_obj_t *origin) {
	return navidromeplay_playlist(lib, saved.id, saved.revision, position,
		saved_entries[position].occurrence, saved_entries[position].track.id, origin);
}
static void saved_metadata(int at, const char *title) {
	song_metadata_t meta;
	bool ok = false;
	for (int i = 0; i < 10000; i++) {
		if (playlist_metadata_at(at, &meta)) { ok = true; break; }
		drain(); usleep(1000);
	}
	assert(ok && !strcmp(meta.title, title));
}
static void saved_add_idle(void) {
	for (int i = 0; i < 10000; i++) {
		drain(); navidromeplay_tick();
		if (!navidromeplay_enqueuing()) return;
		usleep(1000);
	}
	assert(!"saved enqueue timeout");
}
static void saved_actions_check(nd_library_t *lib, lv_obj_t *origin) {
	int begin = starts, requests = atomic_load(&calls);
	atomic_store(&allow_ready, false);
	playlist_set_mode(PLAYBACK_MODE_REPEAT_ONE);
	int fetched = atomic_load(&fetches);
	assert(navidromeplay_playlist_shortcut(lib, saved.id, saved.revision, saved.count, false, origin));
	wait_fetch(fetched);
	assert(playlist_get_mode() == PLAYBACK_MODE_REPEAT_ONE);
	atomic_store(&allow_ready, true); wait_started(begin + 1);
	assert(playlist_current_entry() == 0 && playlist_get_mode() == PLAYBACK_MODE_NORMAL && playlist_count() == 3);
	for (int endless = 0; endless < 2; endless++) {
		continuous_shuffle = endless;
		assert(navidromeplay_playlist_shortcut(lib, saved.id, saved.revision, saved.count, true, origin));
		wait_started(begin + 2 + endless);
		assert(playlist_get_mode() == (endless ? PLAYBACK_MODE_SHUFFLE_REPEAT : PLAYBACK_MODE_SHUFFLE));
		assert(playlist_current_entry() < 3 && playlist_count() == 3);
	}
	continuous_shuffle = false;
	atomic_store(&fail_track, 0); begin = starts;
	assert(navidromeplay_playlist_shortcut(lib, saved.id, saved.revision, saved.count, false, origin));
	album_coordinator_idle();
	assert(starts == begin && playlist_get_mode() == PLAYBACK_MODE_SHUFFLE_REPEAT && playlist_count() == 3);
	atomic_store(&fail_track, -1);
	navidromeplay_stop(); album_coordinator_idle();
	nd_playlist_capture_t all = {.playlist = saved.id, .revision = saved.revision, .count = saved.count, .whole = true};
	const char *local[] = {"/fixture/local-a.flac", "/fixture/local-b.flac"};
	for (int at_end = 0; at_end < 2; at_end++) {
		playlist_load_paths(local, 2, 0); navidromeplay_other(local[0]);
		snprintf(playback.current_file, sizeof(playback.current_file), "%s", local[0]); playback.status = AUDIO_STATUS_PLAYING;
		playlist_set_mode(PLAYBACK_MODE_SHUFFLE); playlist_set_enqueue_at_end(at_end);
		int preparations = atomic_load(&fetches), played = starts;
		atomic_store(&fixture_reject_posts, true);
		assert(navidromeplay_playlist_enqueue(lib, &all, origin)); saved_add_idle();
		atomic_store(&fixture_reject_posts, false);
		assert(playlist_count() == 5 && starts == played && atomic_load(&fetches) == preparations);
		assert(playlist_get_mode() == PLAYBACK_MODE_SHUFFLE && !strcmp(playback.current_file, local[0]) && playback.status == AUDIO_STATUS_PLAYING);
		int at = at_end ? 2 : 1;
		for (int i = 0; i < 3; i++) { char title[64]; snprintf(title, sizeof(title), "Saved occurrence %d", i); saved_metadata(at + i, title); }
		nd_playlist_capture_t single = all;
		single.whole = false; single.single = true; single.ordinal = 2;
		single.occurrence = saved_entries[2].occurrence; strcpy(single.track, saved_entries[2].track.id);
		assert(navidromeplay_playlist_enqueue(lib, &single, origin)); saved_add_idle();
		assert(playlist_count() == 6 && starts == played && atomic_load(&fetches) == preparations);
		saved_metadata(at_end ? 5 : 4, "Saved occurrence 2");
		// A completed saved capture must not coalesce into a Library-handle namespace.
		assert(playlist_move(at, at + 1)); int remove = at; assert(playlist_remove(&remove, 1));
		assert(playlist_count() == 5);
	}
	playlist_clear(); navidromeplay_other(local[0]);
	int preparations = atomic_load(&fetches), played = starts;
	assert(navidromeplay_playlist_enqueue(lib, &all, origin)); saved_add_idle();
	assert(playlist_count() == 3 && playback.status == AUDIO_STATUS_STOPPED && starts == played && atomic_load(&fetches) == preparations);
	// Metadata admission does not require audio availability.
	atomic_store(&missing_track, 0);
	assert(navidromeplay_playlist_enqueue(lib, &all, origin)); saved_add_idle();
	assert(playlist_count() == 6 && starts == played && atomic_load(&fetches) == preparations);
	atomic_store(&missing_track, -1);
	// Replacement is performed before GUI delivery, irrespective of worker timing.
	assert(navidromeplay_playlist_enqueue(lib, &all, origin)); playlist_load_paths(local, 2, 0); saved_add_idle();
	assert(playlist_count() == 2);
	for (int i = 0; i < 8; i++) {
		assert(navidromeplay_playlist_enqueue(lib, &all, origin)); saved_add_idle(); assert(playlist_count() == 2 + (i + 1) * 3);
	}
	assert(navidromeplay_playlist_enqueue(lib, &all, origin)); saved_add_idle(); assert(playlist_count() == 26);
	playlist_clear();
	assert(navidromeplay_playlist_enqueue(lib, &all, origin)); saved_add_idle(); assert(playlist_count() == 3);
	playlist_clear();
	atomic_store(&fail_worker_allocation, true);
	assert(navidromeplay_playlist_enqueue(lib, &all, origin)); saved_add_idle();
	assert(!atomic_load(&fail_worker_allocation) && !playlist_count());
	// Leaving and returning cannot revive feedback from an earlier page visit.
	playlist_clear(); fixture_last_success[0] = 0;
	assert(navidromeplay_playlist_enqueue(lib, &all, origin));
	navidromeplay_playlist_leave(origin); saved_add_idle();
	assert(playlist_count() == 3 && !fixture_last_success[0]);
	unsigned notices = fixture_notices;
	assert(navidromeplay_playlist_enqueue(lib, &all, origin));
	navidromeplay_playlist_leave(origin); playlist_load_paths(local, 2, 0); saved_add_idle();
	assert(playlist_count() == 2 && fixture_notices == notices);
	assert(atomic_load(&calls) == requests);
	puts("saved actions coordinator: staged Play/finite-continuous Shuffle and failure mode rollback; atomic whole/entry repeats, both placements, silent offline metadata admission, rejected-post/replacement and finite-provider release: passed");
}
static void saved_rearrange_check(nd_library_t *lib, lv_obj_t *origin) {
	int before = starts, requests = atomic_load(&calls);
	playlist_set_mode(PLAYBACK_MODE_REPEAT_ONE);
	assert(saved_start(lib, 2, origin)); wait_started(before + 1);
	int preparations = atomic_load(&fetches);
	char audible[sizeof(playback.current_file)];
	snprintf(audible, sizeof(audible), "%s", playback.current_file);
	audio_status_t status = playback.status;
	char path[1200]; nd_playlist_store_t store;
	assert(nd_pl_path(lib, path) && nd_pl_open(&store, path, lib->scope, true));
	bool changed;
	int64_t content_revision = saved.revision;
	assert(nd_pl_view_move(&store, &saved, 2, 0, saved_entries[2].occurrence, saved_entries[2].track.id, &changed) && changed && nd_pl_sql(&store, "COMMIT"));
	assert(nd_pl_get(&store, saved.id, &saved) && saved.revision == content_revision && saved.manual);
	nd_playlist_entry_t displayed[ND_WINDOW];
	int displayed_count, total;
	assert(nd_pl_view_window(&store, &saved, 0, 0, displayed, &displayed_count, &total) && displayed[0].occurrence == saved_entries[2].occurrence);
	nd_pl_close(&store);
	assert(starts == before + 1 && atomic_load(&fetches) == preparations && !strcmp(playback.current_file, audible));
	assert(playback.status == status && playlist_get_mode() == PLAYBACK_MODE_REPEAT_ONE && playlist_current_entry() == 2);
	saved_metadata(0, "Saved occurrence 0"); saved_metadata(2, "Saved occurrence 2");
	nd_playlist_capture_t local_view = {.playlist = saved.id, .revision = saved.revision, .count = saved.count, .view = true, .sort = saved.sort, .view_revision = saved.view_revision, .metadata_revision = saved.metadata_revision, .ordinal = 0, .stored_position = displayed[0].position, .stored_ordinal = displayed[0].ordinal, .occurrence = displayed[0].occurrence};
	strcpy(local_view.track, displayed[0].track.id);
	assert(navidromeplay_playlist_view(lib, &local_view, false, false, origin));
	wait_started(before + 2);
	assert(playlist_current_entry() == 0 && !strcmp(playback.metadata.title, "Saved occurrence 2"));
	saved_metadata(1, "Saved occurrence 0"); saved_metadata(2, "Saved occurrence 1");
	// Restore the saved definition for the existing shared mode/action fixtures.
	assert(nd_pl_open(&store, path, lib->scope, true));
	assert(nd_pl_sort_set(&store, &saved, ND_PL_ORIGINAL) && nd_pl_view_reset(&store, saved.id) && nd_pl_sql(&store, "COMMIT") && nd_pl_get(&store, saved.id, &saved));
	assert(saved.revision == content_revision && !saved.manual);
	nd_pl_close(&store);
	assert(atomic_load(&calls) == requests);
	navidromeplay_stop(); album_coordinator_idle();
	puts("saved rearrange coordinator: installed queue/audio/mode unchanged, new start uses exact moved occurrence and fresh order: passed");
}
static void saved_sort_check(nd_library_t *lib, lv_obj_t *origin) {
	char path[1200];
	nd_playlist_store_t s;
	assert(nd_pl_path(lib, path) && nd_pl_open(&s, path, lib->scope, true));
	assert(nd_pl_get(&s, saved.id, &saved) && nd_pl_sort_set(&s, &saved, ND_PL_TITLE_DESC) && nd_pl_sql(&s, "COMMIT"));
	assert(nd_pl_get(&s, saved.id, &saved));
	nd_playlist_entry_t rows[ND_WINDOW];
	int n, total;
	assert(nd_pl_view_window(&s, &saved, 0, 0, rows, &n, &total) && n == 3);
	nd_pl_close(&s);
	nd_playlist_capture_t view = {.playlist = saved.id, .revision = saved.revision, .count = saved.count, .view = true, .sort = saved.sort, .view_revision = saved.view_revision, .metadata_revision = saved.metadata_revision, .ordinal = 0, .stored_position = rows[0].position, .stored_ordinal = rows[0].ordinal, .occurrence = rows[0].occurrence};
	strcpy(view.track, rows[0].track.id);
	int requests = atomic_load(&calls), begin = starts;
	playlist_set_mode(PLAYBACK_MODE_REPEAT_ONE);
	assert(navidromeplay_playlist_view(lib, &view, false, false, origin));
	wait_started(begin + 1);
	assert(playlist_current_entry() == 0 && playlist_get_mode() == PLAYBACK_MODE_REPEAT_ONE);
	saved_metadata(0, "Saved occurrence 2");
	saved_metadata(1, "Saved occurrence 1");
	saved_metadata(2, "Saved occurrence 0");
	assert(navidromeplay_occurrence_matches(lib, saved.id, rows[0].occurrence, rows[0].track.id));
	assert(!navidromeplay_occurrence_matches(lib, saved.id, rows[2].occurrence, rows[2].track.id));
	assert(nd_pl_open(&s, path, lib->scope, true));
	assert(nd_pl_sort_set(&s, &saved, ND_PL_TITLE) && nd_pl_sql(&s, "COMMIT") && nd_pl_get(&s, saved.id, &saved));
	nd_pl_close(&s);
	saved_metadata(0, "Saved occurrence 2");
	saved_metadata(2, "Saved occurrence 0"); // Sealed queue remains descending.
	view.whole = true;
	view.sort = saved.sort;
	view.view_revision = saved.view_revision;
	view.metadata_revision = saved.metadata_revision;
	begin = starts;
	assert(navidromeplay_playlist_view(lib, &view, true, false, origin));
	wait_started(begin + 1);
	assert(playlist_current_entry() == 0 && playlist_get_mode() == PLAYBACK_MODE_NORMAL);
	saved_metadata(0, "Saved occurrence 0");
	begin = starts;
	assert(navidromeplay_playlist_view(lib, &view, true, true, origin));
	wait_started(begin + 1);
	assert(playlist_get_mode() == PLAYBACK_MODE_SHUFFLE && playlist_count() == 3);
	assert(nd_pl_open(&s, path, lib->scope, true));
	assert(nd_pl_sort_set(&s, &saved, ND_PL_ORIGINAL) && nd_pl_sql(&s, "COMMIT") && nd_pl_get(&s, saved.id, &saved));
	nd_pl_close(&s);
	navidromeplay_stop();
	album_coordinator_idle();
	playlist_clear();
	assert(atomic_load(&calls) == requests);
	puts("sorted coordinator: descending repeated tap and occurrence markers, ascending Play/Shuffle, preserved tap mode, sealed-order independence and no HTTP: passed");
}

static void captured_search_enqueue_check(nd_library_t *lib, lv_obj_t *origin) {
	int begin = starts, fetched = atomic_load(&fetches), requests = atomic_load(&calls);
	nd_context_t identity;
	assert(navidrome_context(&identity, false));
	const char *paths[] = {"/fixture/local-one.flac", "/fixture/local-two.flac"};
	for (int at_end = 0; at_end < 2; at_end++) {
		navidromeplay_stop();
		album_coordinator_idle();
		playlist_clear();
		playlist_load_paths(paths, 2, 0);
		navidromeplay_other(paths[0]);
		strcpy(playback.current_file, paths[0]);
		playback.status = AUDIO_STATUS_PLAYING;
		playlist_set_mode(PLAYBACK_MODE_REPEAT_ONE);
		playlist_set_enqueue_at_end(at_end);
		fixture_owned_t *rows = fixture_owned_new(lib, &identity, 3);
		assert(navidromeplay_enqueue_captured(&rows->owned, playlist_identity(), origin));
		saved_add_idle();
		assert(playlist_count() == 5 && playlist_current_entry() == 0);
		assert(playlist_get_mode() == PLAYBACK_MODE_REPEAT_ONE);
		int first = at_end ? 2 : 1;
		saved_metadata(first, "Captured occurrence 0");
		saved_metadata(first + 2, "Captured occurrence 2");
		assert(navidromeplay_enqueue_captured(&rows->owned, playlist_identity(), origin));
		saved_add_idle();
		assert(playlist_count() == 8);
		nd_owned_release(&rows->owned);
	}
	navidromeplay_stop();
	album_coordinator_idle();
	playlist_clear();
	fixture_owned_t *rows = fixture_owned_new(lib, &identity, 17000);
	assert(navidromeplay_enqueue_captured(&rows->owned, playlist_identity(), origin));
	saved_add_idle();
	assert(playlist_count() == 17000 && playback.status == AUDIO_STATUS_STOPPED);
	saved_metadata(16999, "Captured occurrence 1");
	nd_owned_release(&rows->owned);
	playlist_clear();
	album_coordinator_idle();
	playlist_load_paths(paths, 2, 0);
	rows = fixture_owned_new(lib, &identity, 1);
	rows->fail_read = true;
	assert(navidromeplay_enqueue_captured(&rows->owned, playlist_identity(), origin));
	saved_add_idle();
	assert(playlist_count() == 2);
	nd_owned_release(&rows->owned);
	rows = fixture_owned_new(lib, &identity, 1);
	assert(!navidromeplay_enqueue_captured(&rows->owned, playlist_identity() + 1, origin));
	atomic_store(&fixture_reject_posts, true);
	assert(navidromeplay_enqueue_captured(&rows->owned, playlist_identity(), origin));
	saved_add_idle();
	atomic_store(&fixture_reject_posts, false);
	assert(playlist_count() == 3);
	nd_owned_release(&rows->owned);
	playlist_clear();
	album_coordinator_idle();
	assert(starts == begin && atomic_load(&fetches) == fetched && atomic_load(&calls) == requests);
	assert(fixture_owned_destroyed == 5);
	nd_wipe(&identity, sizeof(identity));
	puts("Search enqueue: owned duplicate occurrences, next/end/repeated placement, 17,000 stopped entries, failed provider/stale queue/rejected post and no audio/HTTP: passed");
}

int main(void) {
	fixture_gui_thread = pthread_self();
	alarm(90);
	char dir[] = "/tmp/sonix-playlist-coordinator.XXXXXX";
	assert(mkdtemp(dir) && !chdir(dir));
	navidrome_init(); navidrome_set_root(dir);
	assert(navidrome_save("http://fixture/base", "fixture user", server_password, "vault1")); wait_idle();
	nd_library_t lib;
	navidrome_library_snapshot(&lib);
	assert(!lib.ready && *lib.path && *lib.scope);
	lv_init();
	lv_display_t *display = lv_display_create(320, 480);
	lv_obj_t *origin = lv_obj_create(NULL), *other = lv_obj_create(NULL);
	lv_screen_load(origin);
	navidromeplay_init();
	atomic_store(&allow_ready, true);
	saved_create(&lib, 3);
	saved_rearrange_check(&lib, origin);
	saved_actions_check(&lib, origin);
	saved_sort_check(&lib, origin);
	captured_search_enqueue_check(&lib, origin);
	int requests = atomic_load(&calls);
	for (int mode = PLAYBACK_MODE_NORMAL; mode <= PLAYBACK_MODE_SHUFFLE_REPEAT; mode++) {
		int begin = starts;
		playlist_set_mode(mode);
		assert(saved_start(&lib, 2, origin)); wait_started(begin + 1);
		assert(playlist_count() == 3 && playlist_current_entry() == 2 && playlist_get_mode() == (playback_mode_t)mode);
		assert(!strcmp(playback.metadata.title, "Saved occurrence 2"));
		bool seen[3] = {0};
		for (int i = 0; i < 3; i++) {
			char ref[80]; unsigned token; size_t entry;
			assert(playlist_path_at(i, ref, sizeof(ref)) && sscanf(ref, "ndq:%u:%zu", &token, &entry) == 2 && entry < 3 && !seen[entry]);
			seen[entry] = true;
		}
		assert(atomic_load(&calls) == requests);
	}
	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	saved_metadata(0, "Saved occurrence 0"); saved_metadata(2, "Saved occurrence 2");
	chain_enabled = true;
	assert(!navidromeplay_chain(true)); // A saved sequence is never an album origin.
	char ref[80];
	int begin = starts;
	assert(playlist_prev(ref, sizeof(ref)) && !navidromeplay_prepare(ref)); wait_started(begin + 1);
	assert(playlist_current_entry() == 1 && !strcmp(playback.metadata.title, "Saved occurrence 1"));
	assert(playlist_next(ref, sizeof(ref)) && !navidromeplay_prepare(ref)); wait_started(begin + 2);
	assert(playlist_current_entry() == 2 && !strcmp(playback.metadata.title, "Saved occurrence 2"));
	assert(playlist_move(0, 1));
	int remove = 0; assert(playlist_remove(&remove, 1) && playlist_count() == 2);
	char path[1200]; nd_playlist_store_t s;
	assert(nd_pl_path(&lib, path) && nd_pl_open(&s, path, lib.scope, true));
	nd_playlist_t check;
	assert(nd_pl_get(&s, saved.id, &check) && check.count == 3 && check.revision == saved.revision);
	assert(nd_pl_edit(&s, saved.id, saved.revision, "Renamed", false) && nd_pl_sql(&s, "COMMIT"));
	nd_pl_close(&s);
	begin = starts;
	assert(saved_start(&lib, 0, origin)); album_coordinator_idle();
	assert(starts == begin && playlist_count() == 2); // Stale revision preserves old queue.
	assert(nd_pl_open(&s, path, lib.scope, true));
	assert(nd_pl_get(&s, saved.id, &check) && nd_pl_edit(&s, saved.id, check.revision, NULL, true) && nd_pl_sql(&s, "COMMIT"));
	nd_pl_close(&s);
	saved_metadata(1, "Saved occurrence 2");
	puts("saved playlist coordinator: pre-Sync playback, all modes/repeated positions, queue edits independent of saved edits/deletion, stale rollback and no chaining/server metadata: passed");

	saved_create(&lib, 3);
	open_on_tap = false;
	int sheets = sheet_opens; begin = starts;
	atomic_store(&fixture_reject_posts, true);
	assert(saved_start(&lib, 0, origin)); wait_started(begin + 1);
	atomic_store(&fixture_reject_posts, false);
	assert(sheet_opens == sheets);
	open_on_tap = true;
	atomic_store(&allow_ready, false);
	int fetched = atomic_load(&fetches); begin = starts;
	assert(saved_start(&lib, 2, origin)); wait_fetch(fetched);
	navidromeplay_playlist_leave(origin); lv_screen_load(other); lv_screen_load(origin);
	atomic_store(&allow_ready, true); wait_started(begin + 1);
	assert(sheet_opens == sheets && playlist_current_entry() == 2);
	atomic_store(&allow_ready, false);
	fetched = atomic_load(&fetches); begin = starts;
	assert(saved_start(&lib, 0, origin)); wait_fetch(fetched);
	bool playing;
	assert(navidromeplay_toggle_pending(&playing) && !playing);
	atomic_store(&allow_ready, true); wait_started(begin + 1);
	assert(playback.status == AUDIO_STATUS_PAUSED);
	atomic_store(&allow_ready, false);
	fetched = atomic_load(&fetches); begin = starts;
	assert(saved_start(&lib, 1, origin)); wait_fetch(fetched);
	navidromeplay_stop(); atomic_store(&allow_ready, true); album_coordinator_idle();
	assert(starts == begin);
	puts("saved playlist coordinator: Open player preference, rejected-post fallback, leave/return origin, Pause-before-ready and Stop cancellation: passed");

	navidrome_test_online(0); navidrome_poll_network();
	lib.writable = false;
	begin = starts;
	assert(saved_start(&lib, 0, origin)); wait_started(begin + 1);
	assert(!lib.ready && playlist_count() == 3);
	atomic_store(&missing_track, 1);
	assert(playlist_next(ref, sizeof(ref)) && !navidromeplay_prepare(ref)); album_coordinator_idle();
	assert(starts == begin + 1 && playback.status == AUDIO_STATUS_STOPPED && !strcmp(playback.metadata.title, "Saved occurrence 1"));
	atomic_store(&missing_track, -1);
	assert(playlist_next(ref, sizeof(ref)) && !navidromeplay_prepare(ref)); wait_started(begin + 2);
	assert(atomic_load(&calls) == requests);
	puts("saved playlist coordinator: offline/read-only before Sync, selected unavailable occurrence and deliberate cached Next: passed");
	// Keep a real handle-backed source installed while a captured addition arrives.
	lib.writable = true; seed(&lib); navidrome_set_root(dir);
	for (int i = 0; i < 10000; i++) {
		navidrome_library_snapshot(&lib);
		if (!lib.loading) break;
		usleep(1000);
	}
	assert(lib.ready);
	nd_query_t tracks = {.kind = ND_TRACKS};
	begin = starts;
	assert(navidromeplay_select(&lib, &tracks, "song-00000", origin)); wait_started(begin + 1);
	playback.status = AUDIO_STATUS_PAUSED;
	playlist_set_enqueue_at_end(true);
	nd_playlist_capture_t additions = {.playlist = saved.id, .revision = saved.revision, .count = saved.count, .whole = true};
	assert(navidromeplay_playlist_enqueue(&lib, &additions, origin)); saved_add_idle();
	assert(playlist_count() == 17003);
	saved_metadata(17000, "Saved occurrence 0"); saved_metadata(17002, "Saved occurrence 2");
	assert(navidromeplay_enqueue(&lib, ND_TRACKS, "song-00002", 0)); saved_add_idle();
	assert(playlist_count() == 17004);
	saved_metadata(17000, "Saved occurrence 0"); saved_metadata(17002, "Saved occurrence 2");
	puts("mixed source ownership: saved occurrences retain their own positions beside live Library handles in both addition directions: passed");
	navidrome_lock(); drain(); navidromeplay_tick();
	assert(!playlist_has_source() && !playback.current_file[0]);
	snprintf(playback.current_file, sizeof(playback.current_file), "/fixture/local.flac"); playback.status = AUDIO_STATUS_PLAYING;
	int previous_stops = stops;
	navidrome_lock(); drain(); navidromeplay_tick();
	assert(stops == previous_stops && playback.status == AUDIO_STATUS_PLAYING);
	playlist_clear(); navidromeplay_shutdown(); drain(); navidrome_reset(); drain();
	lv_display_delete(display); lv_deinit();
	puts("saved playlist coordinator: Lock invalidation and unrelated local playback preservation: passed");
	return 0;
}

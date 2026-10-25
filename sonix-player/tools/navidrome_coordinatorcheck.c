// Real LVGL, service, SQLite, queue and playback coordinator; controlled audio
// preparation/output boundaries make late-result races deterministic.
#define NAVIDROME_COORDINATOR_TEST
#include "navidrome_browsecheck.c"
#define main unused_queuecheck_main
#include "navidrome_queuecheck.c"
#undef main
#include "src/system/decode/growfile.h"
#include "src/system/playback/device_state.h"

static device_state_t playback;
static atomic_bool allow_ready;
static atomic_int fetches;
static atomic_int fail_track = -1;
static int starts, sheet_opens, stops;
static int player_refreshes, favorite_refreshes;
static bool network_demand;
long config_get_int(const char *section, const char *key, long fallback) {
	(void)section;
	(void)key;
	return fallback;
}
void config_set_int(const char *section, const char *key, long value) {
	(void)section;
	(void)key;
	(void)value;
}
bool config_save(void) { return true; }
void device_state_get(device_state_t *out) { *out = playback; }
void audio_stop(void) {
	stops++;
	playback.status = AUDIO_STATUS_STOPPED;
}
bool audio_playback_context_active(void) { return false; }
void device_state_clear_prepared(void) {
	audio_stop();
	memset(&playback, 0, sizeof(playback));
}
void device_state_play_prepared(const char *path, const song_metadata_t *meta, bool paused, double duration) {
	starts++;
	snprintf(playback.current_file, sizeof(playback.current_file), "%s", path);
	playback.metadata = *meta;
	playback.progress_total_secs = duration;
	playback.status = paused ? AUDIO_STATUS_PAUSED : AUDIO_STATUS_PLAYING;
}
void player_refresh_now_playing(void) { player_refreshes++; }
void player_refresh_favorite(void) { favorite_refreshes++; }
void toast_busy_end(void) {}
void toast_busy_dismissable(const char *key) { (void)key; }
void player_sheet_open(bool animate) {
	(void)animate;
	sheet_opens++;
}
void trackmenu_source_metadata_ready(void *unused) { (void)unused; }
void metadata_read(const char *path, song_metadata_t *out) {
	(void)path;
	memset(out, 0, sizeof(*out));
}
void nd_audio_cancel(void) {}
void nd_audio_protect(const char *const paths[3]) { (void)paths; }
bool nd_audio_network_wanted(void) { return network_demand; }
void nd_audio_set_network_wanted(bool wanted) { network_demand = wanted; }
bool nd_audio_failed(const char *path) {
	(void)path;
	return false;
}
bool nd_audio_owns(const char *path) { return path && strstr(path, "/.local/navidrome/.cache/audio/"); }
bool nd_audio_cached(const nd_library_t *lib, const nd_context_t *ctx, const char *id) {
	(void)lib;
	(void)ctx;
	(void)id;
	return true;
}
const char *nd_audio_fetch(const nd_library_t *lib, const nd_context_t *ctx, const nd_playrow_t *track, bool progressive_start, nd_audio_current_cb current, nd_audio_ready_cb ready_cb, void *user) {
	(void)lib;
	(void)ctx;
	(void)progressive_start;
	atomic_fetch_add(&fetches, 1);
	while (!atomic_load(&allow_ready)) {
		if (!current(user))
			return "nd_audio_cancelled";
		usleep(1000);
	}
	if (!current(user))
		return "nd_audio_cancelled";
	if (atoi(track->row.id + 5) == atomic_load(&fail_track))
		return "nd_network_error";
	char path[512];
	snprintf(path, sizeof(path), "/fixture/.local/navidrome/.cache/audio/scope/%s.flac", track->row.id);
	ready_cb(path, user);
	return NULL;
}
static void wait_started(int expected) {
	for (int i = 0; i < 10000; i++) {
		drain();
		navidromeplay_tick();
		if (starts == expected)
			return;
		usleep(1000);
	}
	assert(!"prepared playback did not commit");
}
static void wait_fetch(int previous) {
	for (int i = 0; atomic_load(&fetches) == previous && i < 10000; i++) {
		drain();
		usleep(1000);
	}
	assert(atomic_load(&fetches) > previous);
}
int main(int argc, char **argv) {
	alarm(60);
	char dir[] = "/tmp/sonix-navidrome-coordinator-XXXXXX";
	assert(mkdtemp(dir) && !chdir(dir));
	navidrome_init();
	navidrome_set_root(dir);
	assert(navidrome_save(argc > 1 ? argv[1] : "http://fixture/base", "fixture user", server_password, "vault1"));
	wait_idle();
	nd_library_t lib;
	navidrome_library_snapshot(&lib);
	seed(&lib);
	navidrome_set_root(dir);
	for (int i = 0; i < 10000; i++) {
		navidrome_library_snapshot(&lib);
		if (!lib.loading)
			break;
		usleep(1000);
	}
	assert(lib.ready);
	lv_init();
	lv_display_t *display = lv_display_create(320, 480);
	lv_obj_t *origin = lv_obj_create(NULL), *other = lv_obj_create(NULL);
	lv_screen_load(origin);
	navidromeplay_init();
	nd_query_t query = {.kind = ND_TRACKS};
	int before = atomic_load(&fetches);
	assert(navidromeplay_select(&lib, &query, "song-16999", origin));
	wait_fetch(before);
	bool playing;
	assert(navidromeplay_toggle_pending(&playing) && !playing);
	atomic_store(&allow_ready, true);
	wait_started(1);
	assert(playlist_count() == 17000 && playlist_current_index() == 16999 && playback.status == AUDIO_STATUS_PAUSED);
	assert(!strcmp(playback.metadata.title, "Z Track 16999") && sheet_opens == 1);
	puts("coordinator complete-list selection, metadata and Pause-before-ready: passed");
	song_metadata_t queued_metadata;
	int before_metadata = atomic_load(&fetches);
	bool metadata_ready = false;
	for (int i = 0; i < 10000; i++) {
		if (playlist_metadata_at(10000, &queued_metadata)) {
			metadata_ready = true;
			break;
		}
		drain();
		usleep(1000);
	}
	assert(metadata_ready && !strcmp(queued_metadata.title, "P Track 10000") && atomic_load(&fetches) == before_metadata);
	puts("coordinator distant queue metadata loads without an audio request: passed");
	atomic_store(&allow_ready, false);
	assert(playlist_set_current(10));
	char ref[80];
	assert(playlist_current_path(ref, sizeof(ref)));
	before = atomic_load(&fetches);
	assert(!navidromeplay_prepare(ref));
	wait_fetch(before);
	assert(playlist_set_current(11) && playlist_current_path(ref, sizeof(ref)) && !navidromeplay_prepare(ref));
	assert(playlist_set_current(12) && playlist_current_path(ref, sizeof(ref)) && !navidromeplay_prepare(ref));
	atomic_store(&allow_ready, true);
	wait_started(2);
	assert(!strcmp(playback.metadata.title, "A Track 00012"));
	puts("coordinator rapid skips reject superseded results: passed");
	atomic_store(&allow_ready, false);
	assert(playlist_set_current(13) && playlist_current_path(ref, sizeof(ref)));
	before = atomic_load(&fetches);
	assert(!navidromeplay_prepare(ref));
	wait_fetch(before);
	navidromeplay_stop();
	atomic_store(&allow_ready, true);
	for (int i = 0; i < 100; i++) {
		drain();
		navidromeplay_tick();
		usleep(1000);
	}
	assert(starts == 2);
	puts("coordinator Stop-before-ready prevents autoplay: passed");
	atomic_store(&allow_ready, false);
	before = atomic_load(&fetches);
	assert(navidromeplay_select(&lib, &query, "song-00020", origin));
	wait_fetch(before);
	lv_screen_load(other);
	atomic_store(&fixture_reject_posts, true);
	atomic_store(&allow_ready, true);
	for (int i = 0; i < 1000; i++) {
		navidromeplay_tick();
		if (starts == 3)
			break;
		usleep(1000);
	}
	atomic_store(&fixture_reject_posts, false);
	drain();
	assert(starts == 3 && sheet_opens == 1 && lv_screen_active() == other);
	puts("coordinator failed GUI-post fallback and abandoned-page navigation: passed");

	atomic_store(&fail_track, 101);
	assert(navidromeplay_select(&lib, &query, "song-00100", other));
	wait_started(4);
	assert(playlist_path_at(101, ref, sizeof(ref)));
	bool failed = false;
	for (int i = 0; i < 10000; i++) {
		drain();
		navidromeplay_tick();
		if (!navidromeplay_auto(ref)) {
			failed = true;
			break;
		}
		usleep(1000);
	}
	assert(failed && playback.status == AUDIO_STATUS_PLAYING && strstr(playback.current_file, "song-00100"));
	for (int i = 0; i < 100; i++) {
		drain();
		navidromeplay_tick();
		usleep(1000);
	}
	int settled = atomic_load(&fetches);
	for (int i = 0; i < 100; i++) {
		drain();
		navidromeplay_tick();
		usleep(1000);
	}
	assert(atomic_load(&fetches) == settled);
	atomic_store(&fail_track, -1);
	assert(playlist_set_current(101) && !navidromeplay_prepare(ref));
	wait_started(5);
	assert(strstr(playback.current_file, "song-00101"));
	puts("coordinator prefetch failure preserves current audio, suppresses automatic retries, allows explicit retry: passed");
	atomic_store(&allow_ready, false);
	before = atomic_load(&fetches);
	assert(navidromeplay_select(&lib, &query, "song-00200", other));
	wait_fetch(before);
	assert(network_demand);
	navidrome_set_root(NULL);
	atomic_store(&allow_ready, true);
	for (int i = 0; i < 100; i++) {
		drain();
		navidromeplay_tick();
		usleep(1000);
	}
	assert(starts == 5 && !playlist_has_source() && !playback.current_file[0]);
	assert(!network_demand);
	puts("coordinator card detach cancels pending start and releases network demand: passed");
	navidrome_set_root(dir);
	for (int i = 0; i < 10000; i++) {
		navidrome_library_snapshot(&lib);
		if (!lib.loading)
			break;
		usleep(1000);
	}
	nd_library_t favorite_queue;
	navidrome_favorites_snapshot(&favorite_queue);
	seed(&favorite_queue);
	navidrome_set_root(dir);
	for (int i = 0; i < 10000; i++) {
		navidrome_favorites_snapshot(&favorite_queue);
		if (!favorite_queue.loading)
			break;
		usleep(1000);
	}
	navidrome_library_snapshot(&lib);
	nd_query_t full_query = {.kind = ND_TRACKS};
	assert(navidromeplay_select(&lib, &full_query, "song-00300", other));
	wait_started(6);
	nd_playrow_t full_starred_track;
	assert(navidromeplay_track(playback.current_file, &full_starred_track, NULL) && full_starred_track.row.favorite);
	nd_query_t favorite_query = {.kind = ND_TRACKS, .favorites = true};
	assert(favorite_queue.ready && navidromeplay_select(&favorite_queue, &favorite_query, "song-00300", other));
	wait_started(7);
	assert(playlist_count() == 17000 && playlist_current_index() == 300);
	nd_playrow_t starred_track;
	assert(navidromeplay_track(playback.current_file, &starred_track, NULL) && starred_track.row.favorite);
	nd_store_t changed = {0};
	nd_context_t change_context;
	nd_library_t changed_cache = favorite_queue;
	assert(navidrome_context(&change_context, false));
	assert(nd_store_open_source(&changed, favorite_queue.path, favorite_queue.scope, true, false, true));
	assert(nd_store_local_begin(&changed) && nd_store_local_put(&changed, ND_TRACKS, &starred_track, false, 0) && nd_store_local_activate(&changed));
	assert(navidrome_context_commit(&change_context, changed.db) && nd_store_status(&changed, &changed_cache) && navidrome_favorites_adopt(&change_context, &changed_cache));
	nd_store_cleanup(&changed);
	assert(changed_cache.counts[ND_TRACKS] == 16999 && playlist_count() == 17000 && playlist_current_index() == 300);
	assert(nd_store_contains(&changed, favorite_queue.generation, ND_TRACKS, "song-00300"));
	nd_store_close(&changed);
	nd_wipe(&change_context, sizeof(change_context));
	int previous_player_refreshes = player_refreshes, previous_favorite_refreshes = favorite_refreshes;
	playback.progress_current_secs = 42;
	bool annotation_updated = false;
	for (int i = 0; i < 1000; i++) {
		navidromeplay_tick();
		drain();
		nd_playrow_t represented;
		if (navidromeplay_track(playback.current_file, &represented, NULL) && represented.row.favorite_known && !represented.row.favorite) {
			annotation_updated = true;
			break;
		}
		usleep(1000);
	}
	assert(annotation_updated && playlist_count() == 17000 && playlist_current_index() == 300);
	assert(player_refreshes == previous_player_refreshes && favorite_refreshes > previous_favorite_refreshes);
	assert(starts == 7 && playback.progress_current_secs == 42 && playback.status == AUDIO_STATUS_PLAYING);
	puts("coordinator favorite annotation refreshes only the star without restarting playback or resetting progress: passed");
	nd_store_t refreshed = {0};
	assert(nd_store_open_source(&refreshed, favorite_queue.path, favorite_queue.scope, true, false, true));
	assert(nd_store_begin(&refreshed) && nd_store_validate(&refreshed) && nd_store_publish(&refreshed));
	nd_store_cleanup(&refreshed);
	assert(nd_store_contains(&refreshed, favorite_queue.generation, ND_TRACKS, "song-00300"));
	nd_store_close(&refreshed);
	assert(strstr(playback.current_file, "song-00300"));
	puts("coordinator Favorites complete queue, exact track selection and generation pin retention: passed");
	if (argc > 1) {
		nd_context_t identity;
		navidrome_library_snapshot(&lib);
		assert(navidrome_context(&identity, false));
		nd_wipe(identity.password, sizeof(identity.password));
		nd_query_t search_query = {.kind = ND_TRACKS};
		snprintf(search_query.search, sizeof(search_query.search), "Track");
		playlist_set_mode(PLAYBACK_MODE_SHUFFLE);
		assert(navidromeplay_search(&lib, &identity, &search_query, "remote-song-10000", other));
		wait_started(8);
		assert(playlist_get_mode() == PLAYBACK_MODE_SHUFFLE);
		assert(playlist_count() == 17000 && playlist_current_index() == 0);
		nd_playrow_t track;
		assert(navidromeplay_track(playback.current_file, &track, NULL) && !strcmp(track.row.id, "remote-song-10000"));
		bool seen[17000] = {0};
		for (int i = 0; i < 17000; i++) {
			char reference[80];
			unsigned token;
			size_t entry;
			assert(playlist_path_at(i, reference, sizeof(reference)) && sscanf(reference, "ndq:%u:%zu", &token, &entry) == 2 && entry < 17000 && !seen[entry]);
			seen[entry] = true;
		}
		playlist_set_mode(PLAYBACK_MODE_NORMAL);
		assert(playlist_current_index() == 10000 && playlist_count() == 17000);
		// A new ordinary search must capture only its matches, in stored order.
		snprintf(search_query.search, sizeof(search_query.search), "Track 000");
		assert(navidromeplay_select(&lib, &search_query, "song-00050", other));
		wait_started(9);
		assert(playlist_count() == 100 && playlist_current_index() == 50);
		search_query.search[0] = 0;
		snprintf(search_query.filter, sizeof(search_query.filter), "new-album");
		playlist_set_mode(PLAYBACK_MODE_SHUFFLE_REPEAT);
		assert(navidromeplay_search(&lib, &identity, &search_query, "new-song-0", other));
		wait_started(10);
		assert(playlist_get_mode() == PLAYBACK_MODE_SHUFFLE_REPEAT);
		assert(playlist_count() == 12 && playlist_current_index() == 0);
		assert(navidromeplay_track(playback.current_file, &track, NULL) && !strcmp(track.row.id, "new-song-0"));
		bool album_seen[12] = {0};
		for (int i = 0; i < 12; i++) {
			char reference[80];
			unsigned token;
			size_t entry;
			assert(playlist_path_at(i, reference, sizeof(reference)) && sscanf(reference, "ndq:%u:%zu", &token, &entry) == 2 && entry < 12 && !album_seen[entry]);
			album_seen[entry] = true;
		}
		assert(playlist_set_current(11));
		char next[80];
		assert(playlist_next(next, sizeof(next)) && playlist_current_index() == 0 && playlist_count() == 12);
		assert(playlist_get_mode() == PLAYBACK_MODE_SHUFFLE_REPEAT);
		memset(album_seen, 0, sizeof(album_seen));
		for (int i = 0; i < 12; i++) {
			char reference[80];
			unsigned token;
			size_t entry;
			assert(playlist_path_at(i, reference, sizeof(reference)) && sscanf(reference, "ndq:%u:%zu", &token, &entry) == 2 && entry < 12 && !album_seen[entry]);
			album_seen[entry] = true;
		}
		// Cancellation during remote enumeration cannot publish a partial queue.
		search_query.filter[0] = 0;
		snprintf(search_query.search, sizeof(search_query.search), "slow");
		assert(navidromeplay_search(&lib, &identity, &search_query, "remote-song-10000", other));
		usleep(50000);
		navidromeplay_stop();
		for (int i = 0; i < 400; i++) {
			drain();
			navidromeplay_tick();
			usleep(1000);
		}
		assert(starts == 10 && playlist_count() == 12);
		puts("coordinator search queues: exact match set, unsynced metadata/audio start, selected-first Shuffle, unique full search/album rounds, Shuffle Repeat and normal-order restoration: passed");
	}
	navidrome_lock();
	drain();
	navidromeplay_tick();
	assert(!playlist_has_source() && !playback.current_file[0]);
	snprintf(playback.current_file, sizeof(playback.current_file), "/fixture/local.flac");
	playback.status = AUDIO_STATUS_PLAYING;
	int previous_stops = stops;
	navidrome_lock();
	drain();
	navidromeplay_tick();
	assert(stops == previous_stops && playback.status == AUDIO_STATUS_PLAYING);
	puts("coordinator Lock invalidation and unrelated playback preservation: passed");
	playlist_clear();
	navidromeplay_shutdown();
	drain();
	navidrome_reset();
	drain();
	lv_display_delete(display);
	lv_deinit();
	return 0;
}

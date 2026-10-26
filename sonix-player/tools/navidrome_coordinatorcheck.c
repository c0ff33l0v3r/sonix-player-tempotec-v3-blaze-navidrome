// Real LVGL, service, SQLite, queue and playback coordinator; controlled audio
// preparation/output boundaries make late-result races deterministic.
#define NAVIDROME_COORDINATOR_TEST
#ifdef NAVIDROME_POLISH_QUEUE_UI
#define navidromeart_request polish_stub_art_request
#define navidromeart_release polish_stub_art_release
#define navidromeart_take polish_stub_art_take
#define cover_free polish_stub_cover_free
#endif
#include "navidrome_browsecheck.c"
#ifdef NAVIDROME_POLISH_QUEUE_UI
#undef navidromeart_request
#undef navidromeart_release
#undef navidromeart_take
#undef cover_free
void polish_fixture_drain(void) { drain(); }
void polish_queue_ui_check(const nd_library_t *library);
#endif
#define main unused_queuecheck_main
#include "navidrome_queuecheck.c"
#undef main
#include "src/system/decode/growfile.h"
#include "src/system/streaming/navidromecache.h"
#include "src/system/playback/device_state.h"

static device_state_t playback;
static atomic_bool allow_ready;
static atomic_int fetches;
static atomic_int fail_track = -1;
static atomic_int missing_track = -1;
static atomic_int failed_after_ready = -1;
static bool chain_enabled, queue_at_end, open_player_on_tap=true;
static int starts, sheet_opens, stops;
static int player_refreshes, favorite_refreshes;
static bool network_demand;
static bool permanent_download_paths;
static char permanent_source_binding[512];
bool nd_audio_source(const char *path) {
	return path && (nd_audio_owns(path) || (permanent_source_binding[0] && !strcmp(path, permanent_source_binding)));
}
void nd_audio_set_source(const char *path) { snprintf(permanent_source_binding, sizeof(permanent_source_binding), "%s", path ? path : ""); }
long config_get_int(const char *section, const char *key, long fallback) {
	if (!strcmp(section, "player") && !strcmp(key, "album_chaining")) return chain_enabled;
	if (!strcmp(section, "player") && !strcmp(key, "queue_add_end")) return queue_at_end;
	if (!strcmp(section, "player") && !strcmp(key, "open_player_on_tap")) return open_player_on_tap;
	return fallback;
}
void config_set_int(const char *section, const char *key, long value) {
	if (!strcmp(section,"player") && !strcmp(key,"queue_add_end")) queue_at_end=value!=0;
	(void)section;
	(void)key;
	(void)value;
}
bool config_save(void) { return true; }
void device_state_get(device_state_t *out) { *out = playback; }
void audio_get_current_file(char *out, size_t size) { snprintf(out,size,"%s",playback.current_file); }
void audio_stop(void) {
	stops++;
	playback.status = AUDIO_STATUS_STOPPED;
}
bool audio_playback_context_active(void) { return false; }
void device_state_represent_source(const char *reference, const song_metadata_t *meta, double duration) {
	if (strcmp(playback.current_file, reference))
		audio_stop();
	snprintf(playback.current_file, sizeof(playback.current_file), "%s", reference);
	playback.metadata = meta ? *meta : (song_metadata_t){0};
	playback.progress_current_secs = 0;
	playback.progress_total_secs = duration;
	playback.status = AUDIO_STATUS_STOPPED;
}
bool config_get_bool(const char *section, const char *key, bool fallback) { return config_get_int(section, key, fallback) != 0; }
bool device_state_play_queue_index(int index) {
	char ref[512];
	return playlist_set_current(index) && playlist_current_path(ref,sizeof(ref)) && !navidromeplay_prepare(ref);
}
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
bool nd_audio_owns(const char *path) { return path && (!strncmp(path, "ndq:", 4) || strstr(path, "/.local/navidrome/.cache/audio/")); }
bool nd_audio_cached(const nd_library_t *lib, const nd_context_t *ctx, const char *id) {
	(void)lib;
	(void)ctx;
	return atoi(id + 5) != atomic_load(&missing_track);
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
	if (permanent_download_paths)
		snprintf(path, sizeof(path), "/fixture/Music/Navidrome/%s.flac", track->row.id);
	else
		snprintf(path, sizeof(path), "/fixture/.local/navidrome/.cache/audio/scope/%s.flac", track->row.id);
	ready_cb(path, user);
	return atoi(track->row.id + 5) == atomic_load(&failed_after_ready) ? "nd_network_error" : NULL;
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
#include "navidrome_album_coordinator_fixture.h"

static atomic_bool cache_reader_held;
static bool fixture_cache_reader(void) { return atomic_load(&cache_reader_held); }
static void polish_cache_maintenance_check(void) {
	nd_library_t lib;
	navidrome_library_snapshot(&lib);
	char base[1200], audio[1400], art[1400], file[1500], image[1500], retained[1500];
	snprintf(base,sizeof(base),"%s",lib.path); *strrchr(base,'/')=0;
	snprintf(audio,sizeof(audio),"%s/audio",base); assert(!mkdir(audio,0700) || errno==EEXIST);
	snprintf(audio,sizeof(audio),"%s/audio/%s",base,lib.scope); assert(!mkdir(audio,0700) || errno==EEXIST);
	snprintf(art,sizeof(art),"%s/art",base); assert(!mkdir(art,0700) || errno==EEXIST);
	snprintf(file,sizeof(file),"%s/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.tmp",audio);
	snprintf(image,sizeof(image),"%s/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.img.tmp",art);
	snprintf(retained,sizeof(retained),"%s/retained-playlist-sentinel",base);
	const char *paths[]={file,image,retained};
	for(int i=0;i<3;i++) { int fd=open(paths[i],O_CREAT|O_TRUNC|O_WRONLY,0600); assert(fd>=0 && write(fd,"owned",5)==5 && !close(fd)); }
	unsigned old_epoch=nd_cache_epoch(); assert(nd_cache_io_begin(old_epoch));
	atomic_store(&cache_reader_held,true);
	int count=playlist_count(), started_before=starts;
	assert(nd_cache_clear_request(&lib,fixture_cache_reader));
	navidromeplay_cache_stop();
	assert(!nd_cache_clear_request(&lib,fixture_cache_reader));
	assert(!nd_cache_usage_request(&lib));
	assert(!nd_cache_io_current(old_epoch) && !nd_cache_io_begin(nd_cache_epoch()));
	nd_cache_io_end(); usleep(20000); assert(!access(file,F_OK));
	atomic_store(&cache_reader_held,false);
	nd_cache_usage_t usage;
	for(int i=0;;i++) { assert(i<10000); nd_cache_usage_snapshot(&usage); if(!usage.busy) break; drain(); usleep(1000); }
	assert(usage.ready && !usage.error && !usage.audio && !usage.art);
	assert(access(file,F_OK) && access(image,F_OK) && !access(retained,F_OK) && !access(lib.path,F_OK));
	assert(playlist_count()==count && starts==started_before && playback.status==AUDIO_STATUS_STOPPED && navidromeplay_reference(playback.current_file));
	int fetched_before=atomic_load(&fetches);
	for(int i=0;i<20;i++) { navidromeplay_tick(); drain(); usleep(1000); }
	assert(atomic_load(&fetches)==fetched_before && access(file,F_OK));
	puts("cache maintenance coordinator: serialized admission, real reader/writer drain barrier, stale epoch rejection, retained stopped selection/queue/metadata and no automatic refill: passed");
}

static void polish_cache_card_change_check(void) {
	nd_library_t old;
	navidrome_library_snapshot(&old);
	char original[1200], audio[1400], file[1500], replacement[]="/tmp/sonix-cache-replacement-XXXXXX";
	snprintf(original,sizeof(original),"%s",old.path);
	char *relative=strstr(original,"/.local/navidrome/"); assert(relative); *relative=0;
	snprintf(audio,sizeof(audio),"%s",old.path); *strrchr(audio,'/')=0;
	size_t used=strlen(audio); snprintf(audio+used,sizeof(audio)-used,"/audio/%s",old.scope);
	assert(!mkdir(audio,0700) || errno==EEXIST);
	snprintf(file,sizeof(file),"%s/cccccccccccccccccccccccccccccccc.mp3",audio);
	int fd=open(file,O_CREAT|O_WRONLY|O_TRUNC,0600); assert(fd>=0 && write(fd,"keep",4)==4 && !close(fd));
	assert(mkdtemp(replacement));
	assert(nd_cache_io_begin(nd_cache_epoch()));
	assert(nd_cache_clear_request(&old,NULL));
	navidromeplay_cache_stop();
	navidrome_set_root(replacement);
	nd_cache_io_end();
	nd_cache_usage_t usage;
	for(int i=0;;i++){ assert(i<10000); nd_cache_usage_snapshot(&usage); if(!usage.busy)break; drain();usleep(1000); }
	assert(!usage.ready && usage.error && !access(file,F_OK));
	assert(!nd_cache_clear_request(&old,NULL));
	navidromeplay_tick();
	navidrome_set_root(original);
	for(int i=0;i<10000;i++){nd_library_t restored;navidrome_library_snapshot(&restored);if(!restored.loading)break;usleep(1000);}
	puts("cache maintenance card replacement: old-context clear retires without deleting the prior card, stale re-admission is rejected and storage is restored: passed");
}

static void permanent_download_coordinator_check(lv_obj_t *origin) {
	navidromeplay_stop();
	for(int i=0;i<100;i++) { drain(); usleep(1000); }
	nd_library_t library;
	navidrome_library_snapshot(&library);
	assert(library.ready);
	permanent_download_paths=true;
	nd_query_t tracks={.kind=ND_TRACKS};
	int began=starts;
	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	assert(navidromeplay_select(&library,&tracks,"song-00000",origin)); wait_started(began+1);
	assert(strstr(playback.current_file,"/Music/Navidrome/") && nd_audio_source(playback.current_file) && !nd_audio_owns(playback.current_file));
	nd_playrow_t shown; assert(navidromeplay_track(playback.current_file,&shown,NULL) && !strcmp(shown.row.id,"song-00000"));
	char reference[512];
	assert(playlist_advance_auto(reference,sizeof(reference)) && navidromeplay_auto(reference));
	assert(!navidromeplay_prepare(reference)); wait_started(began+2);
	assert(navidromeplay_track(playback.current_file,&shown,NULL) && !strcmp(shown.row.id,"song-00001"));
	playlist_set_mode(PLAYBACK_MODE_REPEAT_ONE);
	assert(playlist_advance_auto(reference,sizeof(reference)) && !navidromeplay_prepare(reference)); wait_started(began+3);
	assert(navidromeplay_track(playback.current_file,&shown,NULL) && !strcmp(shown.row.id,"song-00001"));
	playlist_set_mode(PLAYBACK_MODE_SHUFFLE);
	assert(playlist_next(reference,sizeof(reference)) && !navidromeplay_prepare(reference)); wait_started(began+4);
	assert(navidromeplay_track(playback.current_file,&shown,NULL) && strstr(playback.current_file,shown.row.id));
	char same_file[512]; snprintf(same_file,sizeof(same_file),"%s",playback.current_file);
	nd_audio_set_source(NULL); navidromeplay_other(same_file); playlist_clear();
	assert(!navidromeplay_track(same_file,&shown,NULL) && !nd_audio_source(same_file));
	// Leave a permanent Navidrome source for the shared Lock check below.
	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	assert(navidromeplay_select(&library,&tracks,"song-00002",origin)); wait_started(began+5);
	assert(nd_audio_source(playback.current_file) && !nd_audio_owns(playback.current_file));
	puts("permanent-download coordinator: natural Next, Repeat one, Shuffle, exact source metadata, same-file local handoff and permanent-source Lock setup: passed");
}
static void polish_coordinator_check(const nd_library_t *baseline, lv_obj_t *origin) {
	nd_library_t current_library = *baseline;
	const nd_library_t *lib = &current_library;
	nd_query_t tracks = {.kind = ND_TRACKS};
	int begin = starts;
	navidrome_test_online(1);
	assert(navidrome_reconnect()); wait_idle();
	navidrome_library_snapshot(&current_library);
	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	int sheets_before=sheet_opens;open_player_on_tap=false;
	assert(navidromeplay_select(lib, &tracks, "song-00000", origin));
	wait_started(begin + 1);
	assert(sheet_opens==sheets_before);open_player_on_tap=true;
	navidrome_test_online(0); navidrome_poll_network();
	atomic_store(&missing_track, 1);
	char ref[80];
	assert(playlist_set_current(1) && playlist_current_path(ref, sizeof(ref)));
	assert(!navidromeplay_prepare(ref));
	album_coordinator_idle();
	assert(starts == begin + 1 && playback.status == AUDIO_STATUS_STOPPED && playback.progress_current_secs == 0);
	nd_playrow_t shown;
	assert(navidromeplay_track(playback.current_file, &shown, NULL) && !strcmp(shown.row.id, "song-00001"));
	assert(!strcmp(playback.metadata.title, "A Track 00001") && navidromeplay_matches(lib, ND_TRACKS, "song-00001"));
	navidrome_test_online(1); assert(navidrome_reconnect()); wait_idle();
	navidrome_library_snapshot(&current_library);
	for (int i = 0; i < 50; i++) { drain(); navidromeplay_tick(); usleep(1000); }
	assert(starts == begin + 1 && playback.status == AUDIO_STATUS_STOPPED);
	navidrome_test_online(0); navidrome_poll_network();
	assert(playlist_next(ref, sizeof(ref)) && !navidromeplay_prepare(ref));
	wait_started(begin + 2);
	assert(!strcmp(playback.metadata.title, "A Track 00002"));
	atomic_store(&missing_track, -1);
	puts("coordinator offline miss represents B stopped, Reconnect does not autoplay, deliberate Next plays cached C and markers match the selected ID: passed");
	navidromeplay_stop();
	for (int i = 0; i < 100; i++) { drain(); usleep(1000); }
	// Repeated additions from one pinned view coalesce ownership rather than
	// consuming the eight-context budget on every press.
	for (int i = 0; i < 12; i++) {
		int count = playlist_count(), fetch = atomic_load(&fetches);
		for (int attempt = 0; !navidromeplay_enqueue(lib, ND_TRACKS, "song-00003", 0); attempt++) { assert(attempt < 10000); drain(); usleep(1000); }
		for (int turn = 0; playlist_count() == count; turn++) { assert(turn < 10000); drain(); usleep(1000); }
		if (playlist_count() != count + 1 || starts != begin + 2 || atomic_load(&fetches) != fetch)
			fprintf(stderr, "addition fixture i=%d count=%d expected=%d starts=%d expected=%d fetches=%d expected=%d\n", i, playlist_count(), count + 1, starts, begin + 2, atomic_load(&fetches), fetch);
		assert(playlist_count() == count + 1 && starts == begin + 2 && atomic_load(&fetches) == fetch);
	}
	puts("coordinator twelve exact-ID additions share one metadata context and preserve audible playback without audio/network preparation: passed");
	char selections_path[1200];
	nd_playlist_store_t selections={0};
	assert(nd_pl_path(lib,selections_path) && nd_pl_open(&selections,selections_path,lib->scope,true));
	assert(sqlite3_exec(selections.db,"INSERT INTO selection VALUES(123456,0,1,'song-00001',NULL); INSERT INTO selection VALUES(123456,1,0,'album-00000',NULL); INSERT INTO selection VALUES(123456,2,1,'song-00000',NULL)",NULL,NULL,NULL)==SQLITE_OK);
	nd_pl_close(&selections);
	int old_count=playlist_count();
	assert(navidromeplay_enqueue(lib,ND_TRACKS,NULL,123456));
	for(int i=0;playlist_count()==old_count;i++){assert(i<10000);drain();usleep(1000);}
	assert(playlist_count()==old_count+10 && starts==begin+2);
	// This is after the twelve earlier Play-next additions; the selected
	// track stays first in this new deduplicated batch, followed by disc order.
	assert(playlist_path_at(playlist_current_index()+13,ref,sizeof(ref)));
	nd_playrow_t added;
	nd_library_t added_lib;
	for(int i=0;!navidromeplay_lookup(ref,&added,&added_lib);i++){assert(i<10000);drain();usleep(1000);}
	assert(!strcmp(added.row.id,"song-00001"));
	puts("coordinator selected track+album batches preserve selection order, deduplicate overlaps and keep intentional existing queue repetitions: passed");
	chain_enabled = true;
	assert(!navidromeplay_chain_boundary(true, false)); // All Tracks and custom additions remain track queues.
	nd_query_t album = {.kind = ND_TRACKS, .filter_kind = ND_ALBUMS};
	snprintf(album.filter, sizeof(album.filter), "album-01699");
	assert(navidromeplay_select(lib, &album, "song-16999", origin)); wait_started(begin + 3);
	assert(navidromeplay_chain_boundary(true, false)); wait_started(begin + 4);
	assert(navidromeplay_track(playback.current_file, &shown, NULL) && !strcmp(shown.album_id, "album-00000"));
	assert(playlist_count() == 10);
	assert(navidromeplay_chain_boundary(false, false)); wait_started(begin + 5);
	assert(navidromeplay_track(playback.current_file, &shown, NULL) && !strcmp(shown.row.id, "song-16999"));
	chain_enabled = false;
	puts("coordinator captured full album order wraps forward and Previous selects the preceding album's final disc/track entry: passed");
	atomic_store(&failed_after_ready,16990);
	assert(navidromeplay_select(lib,&album,"song-16990",origin));
	wait_started(begin+6); album_coordinator_idle(); drain();
	assert(navidromeplay_track(playback.current_file,&shown,NULL) && !strcmp(shown.row.id,"song-16990"));
	assert(navidromeplay_reference(playback.current_file) && playback.status==AUDIO_STATUS_STOPPED && playback.progress_current_secs==0);
	atomic_store(&failed_after_ready,-1);
	puts("coordinator failure after progressive admission retains the committed target stopped; pre-admission failures still preserve unrelated playback: passed");
	chain_enabled=true;
	nd_library_t favorite_addition; navidrome_favorites_snapshot(&favorite_addition);
	nd_store_t favorite_writer, full_reader;
	nd_playrow_t add_metadata;
	assert(nd_store_open(&full_reader,lib->path,lib->scope,false,false) && nd_store_metadata(&full_reader,lib->generation,ND_TRACKS,"song-00003",&add_metadata));
	nd_store_close(&full_reader);
	nd_context_t add_context; assert(navidrome_context(&add_context,false));
	assert(nd_store_open_source(&favorite_writer,favorite_addition.path,favorite_addition.scope,true,false,true));
	assert(nd_store_local_begin(&favorite_writer) && nd_store_local_put(&favorite_writer,ND_TRACKS,&add_metadata,true,0) && nd_store_local_activate(&favorite_writer));
	assert(navidrome_context_commit(&add_context,favorite_writer.db) && nd_store_status(&favorite_writer,&favorite_addition) && navidrome_favorites_adopt(&add_context,&favorite_addition));
	nd_store_close(&favorite_writer);nd_wipe(&add_context,sizeof(add_context));navidrome_favorites_snapshot(&favorite_addition);
	playlist_set_enqueue_at_end(true);
	int held=playlist_count();
	assert(navidromeplay_enqueue(&favorite_addition,ND_TRACKS,"song-00003",0));
	for(int i=0;playlist_count()==held;i++){assert(i<10000);drain();usleep(1000);}
	assert(playlist_set_current(playlist_count()-1) && playlist_current_path(ref,sizeof(ref)) && !navidromeplay_prepare(ref));
	wait_started(begin+7);
	assert(navidromeplay_chain_boundary(true,false));wait_started(begin+8);
	assert(navidromeplay_track(playback.current_file,&shown,NULL) && !strcmp(shown.album_id,"album-00000"));
	assert(playlist_enqueue("/fixture/queued-local.flac"));
	assert(playlist_set_current(playlist_count()-1));
	navidromeplay_other("/fixture/queued-local.flac");
	snprintf(playback.current_file,sizeof(playback.current_file),"/fixture/queued-local.flac"); playback.status=AUDIO_STATUS_PLAYING;
	assert(navidromeplay_chain_boundary(true,false));wait_started(begin+9);
	assert(navidromeplay_track(playback.current_file,&shown,NULL) && !strcmp(shown.album_id,"album-00001"));
	playlist_set_enqueue_at_end(false);chain_enabled=false;
	puts("coordinator album origin survives final additions from a different Favorites view and local Music, then continues through the original full Albums order: passed");
	int held_count=playlist_count(), held_starts=starts, fetch_before=atomic_load(&fetches);
	atomic_store(&allow_ready,false);
	assert(playlist_set_current(1) && playlist_current_path(ref,sizeof(ref)) && !navidromeplay_prepare(ref));wait_fetch(fetch_before);
	assert(navidromeplay_enqueue(lib,ND_TRACKS,"song-00050",0));
	assert(playlist_next(ref,sizeof(ref)) && !navidromeplay_prepare(ref));
	navidromeplay_stop();
	atomic_store(&fixture_reject_posts,true);
	for(int i=0;playlist_count()==held_count;i++){assert(i<10000);navidromeplay_tick();drain();usleep(1000);}
	atomic_store(&fixture_reject_posts,false);
	assert(starts==held_starts && playlist_count()==held_count+1 && !navidromeplay_enqueuing());
	atomic_store(&allow_ready,true);
	puts("coordinator queue addition completes on the metadata worker while audio preparation is blocked, survives Skip/Stop, and uses its independent rejected-post fallback: passed");

}

#include "navidrome_albumfallback_fixture.h"

#ifndef NAVIDROME_COORDINATOR_MAIN
#define NAVIDROME_COORDINATOR_MAIN main
#endif
int NAVIDROME_COORDINATOR_MAIN(int argc, char **argv) {
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
#ifdef NAVIDROME_POLISH_QUEUE_UI
	nd_store_t art_seed;
	assert(nd_store_open(&art_seed, lib.path, lib.scope, true, false));
	assert(sqlite3_exec(art_seed.db,"UPDATE entity SET cover='queue-fixture' WHERE kind=1",NULL,NULL,NULL)==SQLITE_OK);
	nd_store_close(&art_seed);
#endif
	lv_init();
	if (getenv("SONIX_POLISH_PANEL_WIDTH")) ui_panel_width=atoi(getenv("SONIX_POLISH_PANEL_WIDTH"));
	lv_display_t *display = lv_display_create(ui_panel_width, ui_panel_width==320?480:800);
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
	assert(playlist_set_current(0));
	playlist_set_mode(PLAYBACK_MODE_SHUFFLE);
	bool scattered_ready = false;
	for (int turn = 0; turn < 10000; turn++) {
		scattered_ready = true;
		for (int at = 1; at <= 12; at++) {
			char queued_ref[80];
			size_t entry;
			unsigned token;
			assert(playlist_path_at(at, queued_ref, sizeof(queued_ref)));
			assert(sscanf(queued_ref, "ndq:%u:%zu", &token, &entry) == 2);
			if (!playlist_metadata_at(at, &queued_metadata))
				scattered_ready = false;
			else
				assert(atoi(strrchr(queued_metadata.title, ' ') + 1) == (int)entry);
		}
		if (scattered_ready)
			break;
		drain();
		usleep(1000);
	}
	assert(scattered_ready && atomic_load(&fetches) == before_metadata);
	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	assert(playlist_set_current(16999));
	puts("coordinator 12 scattered shuffled titles resolve together without audio fetches: passed");

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
	nd_playrow_t stopped_selection;
	assert(navidromeplay_track(playback.current_file,&stopped_selection,NULL) && !strcmp(stopped_selection.row.id,"song-00013"));
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
	album_coordinator_check(&lib, other);
	polish_coordinator_check(&lib, other);
	polish_album_fallback_check();
	polish_cache_maintenance_check();
#ifdef NAVIDROME_POLISH_QUEUE_UI
	navidrome_library_snapshot(&lib);
	polish_queue_ui_check(&lib);
	playback.current_file[0]=0;
#endif
	polish_cache_card_change_check();
	permanent_download_coordinator_check(other);
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

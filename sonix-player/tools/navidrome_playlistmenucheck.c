// Production Now Playing callback: same physical file, distinct source behavior.
#include "src/gui/nowplaying/trackmenu.c"
#include <assert.h>
static bool fixture_navidrome, fixture_target_current = true;
static int fixture_unavailable, fixture_music, fixture_picker;
lv_obj_t *playlistpage_screen;
void device_state_get(device_state_t *out) { memset(out, 0, sizeof(*out)); strcpy(out->current_file, "/fixture/permanent.flac"); }
bool nd_audio_source(const char *path) { (void)path; return fixture_navidrome; }
bool navidromeplaylists_add_current(void) { fixture_picker++; return fixture_target_current; }
void gui_notify_popup(const char *text) { assert(!strcmp(text, "nd_audio_unavailable")); fixture_unavailable++; }
void player_sheet_close(bool animated) { (void)animated; }
void playlistpage_add_track(const char *path) { assert(!strcmp(path, "/fixture/permanent.flac")); fixture_music++; }
void switcher_set_player_return(lv_obj_t *page) { assert(page == playlistpage_screen); }
static bool artist_ready, artist_id, artist_open;
static int64_t diagnostic_reason, diagnostic_flags;
static diagnostic_t fixture_context;
bool logging_diagnostic_enabled(void) { return true; }
diagnostic_t logging_diagnostic_current(void) { return fixture_context; }
diagnostic_t logging_diagnostic_attach(diagnostic_t c) {
	diagnostic_t old = fixture_context;
	fixture_context = c;
	return old;
}
diagnostic_t logging_diagnostic_begin(diagnostic_component_t c, uint32_t parent) {
	static unsigned next;
	return (diagnostic_t){.id = ++next, .parent = parent, .component = c};
}
void logging_diagnostic_event(diagnostic_t c, diagnostic_event_t e, diagnostic_outcome_t o, const diagnostic_value_t *v, size_t n) {
	(void)c;
	(void)e;
	(void)o;
	for (size_t i = 0; i < n; i++) {
		if (v[i].key == D_REASON)
			diagnostic_reason = v[i].value;
		if (v[i].key == D_FLAGS)
			diagnostic_flags = v[i].value;
	}
}
bool navidromeplay_track(const char *path, nd_playrow_t *track, nd_library_t *lib) {
	(void)path;
	(void)lib;
	memset(track, 0, sizeof(*track));
	if (artist_ready)
		strcpy(track->row.artist, "PRIVATE_ARTIST_CANARY");
	if (artist_id)
		strcpy(track->artist_id, "PRIVATE_ID_CANARY");
	return artist_ready;
}
bool navidromebrowse_artist(const char *id, const char *title) {
	(void)id;
	(void)title;
	return artist_open;
}
bool library_track_artist(const char *path, char *out, size_t size) {
	(void)path;
	(void)out;
	(void)size;
	return false;
}
void medialist_open_artist(const char *name) { (void)name; }
lv_obj_t *medialist_albums_screen, *medialist_tracks_screen;
lv_obj_t *lv_screen_active(void) { return playlistpage_screen; }
int main(void) {
	fixture_navidrome = true; playlist_menu_navidrome = true;
	add_to_playlist_cb(NULL); assert(fixture_picker == 1 && !fixture_music && !fixture_unavailable);
	fixture_navidrome = false; fixture_target_current = false;
	add_to_playlist_cb(NULL); assert(fixture_picker == 2 && !fixture_music && fixture_unavailable == 1);
	playlist_menu_navidrome = false;
	add_to_playlist_cb(NULL); assert(fixture_music == 1 && fixture_picker == 2);
	fixture_navidrome = true;
	artist_ready = false;
	artist_id = false;
	open_artist_cb(NULL);
	assert(diagnostic_reason == 1);
	artist_ready = true;
	open_artist_cb(NULL);
	assert(diagnostic_reason == 2 && diagnostic_flags == 5);
	artist_id = true;
	open_artist_cb(NULL);
	assert(diagnostic_reason == 3);
	artist_open = true;
	open_artist_cb(NULL);
	assert(diagnostic_reason == 0);
	puts("Production Show Artist diagnostics distinguish stale source, present name/missing ID, unavailable browser and successful navigation.");
	puts("production Now Playing Add to playlist: Navidrome routing, stale-menu rejection and Music isolation for the same permanent file: passed");
}

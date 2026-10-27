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
int main(void) {
	fixture_navidrome = true; playlist_menu_navidrome = true;
	add_to_playlist_cb(NULL); assert(fixture_picker == 1 && !fixture_music && !fixture_unavailable);
	fixture_navidrome = false; fixture_target_current = false;
	add_to_playlist_cb(NULL); assert(fixture_picker == 2 && !fixture_music && fixture_unavailable == 1);
	playlist_menu_navidrome = false;
	add_to_playlist_cb(NULL); assert(fixture_music == 1 && fixture_picker == 2);
	puts("production Now Playing Add to playlist: Navidrome routing, stale-menu rejection and Music isolation for the same permanent file: passed");
}

// Production player artwork state and rendered pixels; no audio/network device.
#include "src/gui/nowplaying/player.c"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
static nd_playrow_t fixture_track;
static nd_library_t fixture_library;
static bool resolved = true, preparing = true, delivered;
static unsigned requests;
static int color;
int32_t ui_panel_width = 320;
lv_style_t theme_style_icon, theme_style_text, theme_style_text_dim;
const theme_palette_t *theme(void) {
	static theme_palette_t p;
	return &p;
}
void back_btn_over_cover(bool on) { (void)on; }
int back_btn_centre_y(void) { return 20; }
void topbar_set_hidden(bool on) { (void)on; }
void topbar_set_over_artwork(bool on, bool studio) {
	(void)on;
	(void)studio;
}
bool navidromeplay_track(const char *path, nd_playrow_t *row, nd_library_t *lib) {
	(void)path;
	if (!resolved)
		return false;
	*row = fixture_track;
	*lib = fixture_library;
	return true;
}
bool navidromeplay_reference(const char *path) { return path && !strncmp(path, "ndq:", 4); }
bool navidromeplay_preparing(void) { return preparing; }
bool playlist_has_source(void) { return true; }
void navidromeart_release(int slot) {
	(void)slot;
	delivered = false;
}
void navidromeart_request_player(const nd_library_t *lib, const char *id, int size, int w, int h) {
	(void)lib;
	(void)size;
	(void)w;
	(void)h;
	requests++;
	color = *id == 'b' ? 1 : *id == 'c' ? 2 : 0;
	delivered = false;
}
static void pixels(cover_image_t *out, int w, int h, int channel) {
	*out = (cover_image_t){0};
	out->pixels = calloc((size_t)w * h, 4);
	assert(out->pixels);
	for (int i = 0; i < w * h; i++) {
		out->pixels[4 * i + (2 - channel)] = 255;
		out->pixels[4 * i + 3] = 255;
	}
	out->dsc = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = w, .h = h, .stride = w * 4}, .data = out->pixels, .data_size = w * h * 4};
	out->has_tone = true;
	out->tone = channel == 1 ? 0x00ff00 : channel == 2 ? 0x0000ff : 0xff0000;
}
bool navidromeart_take_player(cover_image_t *out, cover_image_t *back, bool *finished) {
	*finished = delivered;
	if (!delivered || !fixture_track.row.cover[0])
		return false;
	pixels(out, 64, 64, color);
	pixels(back, 64, 32, color);
	return true;
}
void coverloader_request_player(const char *p, int w, int h, int bw, int bh) {
	(void)p;
	(void)w;
	(void)h;
	(void)bw;
	(void)bh;
}
void coverloader_request_player_id(const char *p) { (void)p; }
bool coverloader_take_player_id(uint64_t *id, bool *finished) {
	*id = 0;
	*finished = true;
	return false;
}
bool coverloader_take_player(cover_image_t *a, cover_image_t *b, bool *f) {
	(void)a;
	(void)b;
	*f = true;
	return false;
}
bool gui_post(void (*f)(void *), void *p) {
	(void)f;
	(void)p;
	return false;
}
static void displayed_pixels(int channel) {
	lv_obj_t *image = layout_studio_now ? studio_cover : cover_img;
	assert(!lv_obj_is_hidden(image));
	lv_draw_buf_t *frame = lv_snapshot_take(image, LV_COLOR_FORMAT_ARGB8888);
	assert(frame && frame->header.w == 64 && frame->header.h == 64);
	for (unsigned y = 0; y < frame->header.h; y++)
		for (unsigned x = 0; x < frame->header.w; x++) {
			const uint8_t *pixel = frame->data + y * frame->header.stride + x * 4;
			assert(pixel[2 - channel] == 255 && pixel[3] == 255);
		}
	lv_draw_buf_destroy(frame);
}
// Keep unrelated lyrics blur creation outside this focused rendering fixture.
int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *u) {
	(void)t;
	(void)a;
	(void)f;
	(void)u;
	return EAGAIN;
}
int main(int argc, char **argv) {
	alarm(30);
	ui_panel_width = argc > 1 ? atoi(argv[1]) : 320;
	lv_init();
	lv_display_t *display = lv_display_create(ui_panel_width, ui_panel_width == 320 ? 480 : 800);
	player_screen = lv_obj_create(NULL);
	lv_screen_load(player_screen);
	cover_panel = lv_obj_create(player_screen);
	lv_obj_set_size(cover_panel, 64, 64);
	player_menu = lv_obj_create(player_screen);
	lv_obj_set_size(player_menu, 64, 32);
	cover_img = lv_image_create(cover_panel);
	studio_cover = lv_image_create(cover_panel);
	studio_box_w = 128;
	studio_box_h = 256;
	cover_box_w = cover_box_h = 64;
	backdrop_w = 64;
	backdrop_h = backdrop_studio_h = 32;
	progress_slider_timer = lv_timer_create(NULL, 1000, NULL);
	fixture_library.epoch = 1;
	fixture_library.card_epoch = 2;
	strcpy(fixture_library.scope, "scope");
	for (int mode = 0; mode < 2; mode++) {
		layout_studio_now = mode;
		navidrome_cover_key.bound = false;
		strcpy(fixture_track.row.cover, "album-a");
		unsigned initial = requests;
		refresh_cover("ndq:1:1");
		assert(requests == initial + 1);
		delivered = true;
		apply_cover_result();
		assert(current_cover.pixels);
		displayed_pixels(0);
		uint8_t *old = current_cover.pixels;
		initial = requests;
		refresh_cover("ndq:1:2");
		assert(requests == initial && current_cover.pixels == old);
		displayed_pixels(0);
		refresh_cover("/fixture/prepared.flac");
		assert(requests == initial && current_cover.pixels == old);
		resolved = false;
		refresh_cover("ndq:1:3");
		assert(current_cover.pixels == old);
		displayed_pixels(0);
		resolved = true;
		strcpy(fixture_track.row.cover, "b");
		refresh_cover("ndq:1:3");
		assert(requests == initial + 1 && current_cover.pixels == old);
		apply_cover_result();
		assert(current_cover.pixels == old);
		displayed_pixels(0);
		delivered = true;
		apply_cover_result();
		assert(current_cover.pixels && current_cover.pixels[1] == 255);
		displayed_pixels(1);
		// A newer unresolved selection cancels the old in-flight picture only.
		strcpy(fixture_track.row.cover, "album-a");
		refresh_cover("ndq:1:4");
		resolved = false;
		refresh_cover("ndq:1:5");
		displayed_pixels(1);
		resolved = true;
		strcpy(fixture_track.row.cover, "c");
		refresh_cover("ndq:1:5");
		displayed_pixels(1);
		delivered = true;
		apply_cover_result();
		displayed_pixels(2);
		fixture_track.row.cover[0] = 0;
		refresh_cover("ndq:1:6");
		delivered = true;
		apply_cover_result();
		assert(!current_cover.pixels && !current_backdrop.pixels);
		strcpy(fixture_track.row.cover, "album-a");
		refresh_cover("ndq:1:7");
		delivered = true;
		apply_cover_result();
		assert(current_cover.pixels);
		fixture_library.card_epoch++;
		refresh_cover("ndq:1:8");
		assert(!current_cover.pixels);
	}
	lv_display_delete(display);
	lv_deinit();
	puts("production Now Playing cover pixels: same-art reuse, unresolved/ready paths, delayed/rapid replacement, missing art and card invalidation in normal/Studio: passed");
	return 0;
}

lv_font_t font_ui_14;
lv_font_t font_ui_16;
lv_font_t font_ui_18;
lv_font_t font_ui_20;
lv_font_t font_ui_20_bold;
lv_font_t font_ui_22;
lv_font_t font_ui_22_bold;
lv_font_t font_ui_24;
lv_font_t font_ui_24_bold;
lv_font_t font_ui_26;
lv_font_t font_ui_28;
lv_font_t font_ui_32;
lv_font_t font_ui_36_bold;
lv_font_t font_ui_64_bold;
lv_font_t font_ui_72;
void thread_set_background_level(bool on) { (void)on; }
playback_mode_t playlist_get_mode(void) { return PLAYBACK_MODE_NORMAL; }
int audiobook_speed_permille(void) { return 1000; }
int podcast_speed_permille(void) { return 1000; }
void device_state_get(device_state_t *out) { memset(out, 0, sizeof(*out)); }
bool navidromestar_player_update(lv_obj_t *b, lv_obj_t *i, const char *p, lv_color_t c) {
	(void)b;
	(void)i;
	(void)p;
	(void)c;
	return true;
}
bool radio_current_station(radio_station_t *out) {
	(void)out;
	return false;
}
bool radio_fav_contains(const char *id) {
	(void)id;
	return false;
}
bool podcastcache_feed_id(const char *p, long long *out) {
	(void)p;
	(void)out;
	return false;
}
bool podcastsubs_is_followed(long long id) {
	(void)id;
	return false;
}
long tidalcache_track_id(const char *p) {
	(void)p;
	return 0;
}
long qobuzcache_track_id(const char *p) {
	(void)p;
	return 0;
}
bool tidalsync_is_favorite(long id) {
	(void)id;
	return false;
}
bool qobuzsync_is_favorite(long id) {
	(void)id;
	return false;
}
void tidalsync_favorites_refresh(void (*done)(void)) { (void)done; }
void qobuzsync_favorites_refresh(void (*done)(void)) { (void)done; }
bool device_state_prefetched_fav(const char *p, bool *v) {
	(void)p;
	(void)v;
	return false;
}
bool library_fav_contains(const char *p) {
	(void)p;
	return false;
}
bool nd_audio_source(const char *p) {
	(void)p;
	return true;
}
void frameprof_begin(const char *p) { (void)p; }
void frameprof_end(const char *p) { (void)p; }

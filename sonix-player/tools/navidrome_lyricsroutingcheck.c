// Actual registry, chevron and pointer swipe routing; synthetic service/storage only.
#include "back-census.h"
#include "src/gui/nowplaying/lyricsearch.c"
#include "src/gui/shell/switcher.c"
#include <assert.h>
#include <stdlib.h>
LV_FONT_DECLARE(lv_font_unscii_8);
int32_t ui_panel_width = 320;
lv_obj_t *main_menu_screen, *player_screen, *browser_screen, *libraryscan_screen, *audiobookscan_screen;
lv_font_t font_ui_14, font_ui_20, font_ui_24;
void frameprof_begin(const char *s) { (void)s; }
void frameprof_end(const char *s) { (void)s; }
lv_style_t theme_style_icon, theme_style_card, theme_style_card_pressed, theme_style_screen, theme_style_text, theme_style_text_dim, theme_style_accent_bg;
#define IMAGE {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = 1, .h = 1, .stride = 4}, .data_size = 4, .data = (const uint8_t[]){255, 255, 255, 255}}
const lv_image_dsc_t icon_chevron_left = IMAGE;
static bool sheet, eligible = true;
static unsigned writes, reloads;
static nd_lyric_done_cb saved_done;
static void *saved_user;
static nd_lyric_target_t captured;
const char *tr(const char *s) { return s; }
const theme_palette_t *theme(void) {
	static theme_palette_t p;
	return &p;
}
bool player_sheet_is_open(void) { return sheet; }
void player_sheet_close(bool animate) {
	(void)animate;
	sheet = false;
}
void player_sheet_open(bool animate) {
	(void)animate;
	sheet = true;
}
bool player_sheet_drag_active(void) { return false; }
bool coverflow_drag_active(void) { return false; }
void player_lyrics_reload(const char *ref) {
	(void)ref;
	reloads++;
}
void topbar_set_hidden(bool hide) { (void)hide; }
void topbar_page_shown(lv_obj_t *p) { (void)p; }
void theme_notify_screen_shown(lv_obj_t *p) { (void)p; }
void animpark_wake(lv_obj_t *p) { (void)p; }
bool browser_can_go_up(void) { return false; }
bool browser_go_up(void) { return false; }
void browser_refresh(void) {}
void libraryscan_begin(void) {}
void toast_error(const char *s) { (void)s; }
void toast_success(const char *s) { (void)s; }
void nd_lyrics_cancel(unsigned t) { (void)t; }
void nd_lyrics_poll(void) {}
bool navidrome_playback_current(const nd_context_t *t, bool n) {
	(void)t;
	(void)n;
	return eligible;
}
unsigned nd_lyrics_submit(const nd_lyric_target_t *t, nd_lyric_action_t a, const char *s, int c, nd_lyric_done_cb done, void *u) {
	(void)a;
	(void)s;
	(void)c;
	writes++;
	captured = *t;
	saved_done = done;
	saved_user = u;
	return writes;
}
bool lrclib_save(const char *p, const char *s) {
	(void)p;
	(void)s;
	writes++;
	return false;
}
void lrclib_free(lrclib_record_t *r, int n) {
	for (int i = 0; i < n; i++)
		free(r[i].text);
	free(r);
}
lv_obj_t *settingsrow_title(lv_obj_t *p, gui_config_t *cfg, const char *s) {
	(void)cfg;
	lv_obj_t *o = lv_label_create(p);
	lv_label_set_text(o, s);
	return o;
}
void settingsrow_title_corner_slots(lv_obj_t *p, gui_config_t *cfg, int n) {
	(void)p;
	(void)cfg;
	(void)n;
}
int settingsrow_content_top(gui_config_t *cfg) { return cfg->top_bar_height + 40; }
static bool quiet_guard(void) { return false; }
static bool replaced_guard(void) { return true; }
static lv_point_t point;
static bool down;
static void read_touch(lv_indev_t *i, lv_indev_data_t *d) {
	(void)i;
	d->point = point;
	d->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static void step(lv_indev_t *i, int x, int y, bool pressed) {
	point = (lv_point_t){x, y};
	down = pressed;
	lv_tick_inc(20);
	lv_indev_read(i);
}
static void pump(void) {
	for (int i = 0; i < 12; i++) {
		lv_tick_inc(20);
		lv_timer_handler();
	}
}
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *p) {
	(void)a;
	(void)p;
	lv_display_flush_ready(d);
}
static void swipe(lv_indev_t *i, bool commit) {
	lv_obj_update_layout(lyricsearch_screen);
	int y = 150, start = 15, distance = commit ? ui_panel_width / 2 : ui_panel_width / 5;
	step(i, start, y, true);
	for (int n = 1; n <= 5; n++)
		step(i, start + distance * n / 5, y, true);
	step(i, start + distance, y, false);
	pump();
}
static lrclib_record_t *options(void) {
	lrclib_record_t *r = calloc(14, sizeof(*r));
	assert(r);
	for (int i = 0; i < 14; i++) {
		strcpy(r[i].track, "Track");
		r[i].text = strdup("one\ntwo\nthree");
		assert(r[i].text);
	}
	return r;
}
int main(int argc, char **argv) {
	ui_panel_width = argc > 1 ? atoi(argv[1]) : 320;
	int height = argc > 2 ? atoi(argv[2]) : 480;
	bool baseline = argc > 3;
	lv_init();
	lv_display_t *d = lv_display_create(ui_panel_width, height);
	uint8_t *buf = malloc(ui_panel_width * 20 * 4);
	assert(buf);
	lv_display_set_buffers(d, buf, NULL, ui_panel_width * 20 * 4, LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(d, flush);
	font_ui_14 = font_ui_20 = font_ui_24 = lv_font_unscii_8;
	main_menu_screen = lv_obj_create(NULL);
	player_screen = lv_obj_create(NULL);
	lv_screen_load(main_menu_screen);
	gui_config_t cfg = {.screen_width = ui_panel_width, .screen_height = height, .padding = 8, .top_bar_height = 20};
	back_btn_init(&cfg);
	for (int n = 0; n < CURRENT_BACK_GUARDS - 1; n++)
		switcher_set_back_guard(lv_obj_create(NULL), quiet_guard);
	lyricsearch_screen = lv_obj_create(NULL);
	lyricsearch_init(&cfg);
	nd_lyric_target_t t = {0};
	strcpy(t.reference, "ndq:a");
	strcpy(t.id, "a");
	lyricsearch_open_navidrome(&t, options(), 14, false);
	show_preview(3);
	if (baseline) {
		assert(!active_back_guard());
		back_btn_cb(NULL);
		assert(sheet && lv_screen_active() == main_menu_screen);
		puts("BASELINE reproduced: current startup census exhausts registry; preview Back leaves results for player");
		return 0;
	}
	assert(active_back_guard() == back_guard);
	printf("registry: %d startup screens, %d slots, %zu compiled bytes\n", CURRENT_BACK_GUARDS, BACK_GUARDS_MAX, sizeof(back_guards));
	lv_indev_t *i = lv_indev_create();
	lv_indev_set_type(i, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(i, read_touch);
	for (int kind = 0; kind < 3; kind++) {
		if (kind == 0)
			lyricsearch_open("local.flac", options(), 14);
		else
			lyricsearch_open_navidrome(&t, options(), 14, kind == 2);
		lv_obj_update_layout(lyricsearch_screen);
		lv_obj_scroll_to_y(list, ui_px(180), LV_ANIM_OFF);
		int scroll = lv_obj_get_scroll_y(list);
		assert(scroll > 0);
		show_preview(3);
		assert(back_stays_in_page());
		lv_obj_send_event(back_btn, LV_EVENT_CLICKED, NULL);
		assert(previewing == -1 && !sheet && lv_screen_active() == lyricsearch_screen && lv_obj_get_scroll_y(list) == scroll);
		show_preview(4);
		swipe(i, false);
		assert(previewing == 4 && !sheet);
		swipe(i, true);
		assert(previewing == -1 && lv_screen_active() == lyricsearch_screen && lv_obj_get_scroll_y(list) == scroll);
		show_preview(2);
		if (kind) {
			lv_obj_send_event(apply_button, LV_EVENT_CLICKED, NULL);
			assert(nd_applying);
			lv_obj_send_event(back_btn, LV_EVENT_CLICKED, NULL);
			nd_lyric_result_t result = {.target = captured};
			saved_done(&result, saved_user);
			assert(previewing == -1 && !sheet && !reloads);
		} else
			lv_obj_send_event(back_btn, LV_EVENT_CLICKED, NULL);
		lv_obj_send_event(back_btn, LV_EVENT_CLICKED, NULL);
		assert(sheet && lv_screen_active() == main_menu_screen && !records);
		sheet = false;
	}
	// Replacement consumes no extra slot; finite overflow keeps old registrations.
	lv_obj_t *first = back_guards[0].screen;
	switcher_set_back_guard(first, replaced_guard);
	assert(back_guards[0].handler == replaced_guard);
	for (int n = CURRENT_BACK_GUARDS; n < BACK_GUARDS_MAX; n++)
		switcher_set_back_guard(lv_obj_create(NULL), quiet_guard);
	switcher_set_back_guard(lv_obj_create(NULL), quiet_guard);
	assert(back_guards[BACK_GUARDS_MAX - 1].handler == quiet_guard);
	assert(back_guards[CURRENT_BACK_GUARDS - 1].handler == back_guard);
	puts("PASS real registry/chevron/swipe: Music, LRCLIB, server alternatives, retained scroll, canceled drag, Apply retirement, player history and overflow");
	lv_indev_delete(i);
	lv_display_delete(d);
	free(buf);
	lv_deinit();
	return 0;
}

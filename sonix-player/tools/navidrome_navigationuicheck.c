// Production cache/server forms and swipe-back with actual LVGL pointer input.
#define NAVIDROME_REAL_BACK_TEST
#define NAVIDROME_SETTINGS_MAIN unused_settings_main
#include "navidrome_uicheck.c"
#include "src/gui/shell/switcher.c"

lv_obj_t *main_menu_screen, *player_screen, *browser_screen, *libraryscan_screen, *audiobookscan_screen;
const lv_image_dsc_t icon_chevron_left = TEST_IMAGE;
bool player_sheet_is_open(void) { return false; }
void player_sheet_close(bool animate) { (void)animate; }
void player_sheet_open(bool animate) { (void)animate; }
void browser_refresh(void) {}
bool browser_go_up(void) { return false; }
bool browser_can_go_up(void) { return false; }
void libraryscan_begin(void) {}
void theme_notify_screen_shown(lv_obj_t *page) { (void)page; }
void topbar_page_shown(lv_obj_t *page) { (void)page; }
void topbar_set_hidden(bool hide) { (void)hide; }
void animpark_wake(lv_obj_t *root) { (void)root; }

static lv_point_t touch_point;
static bool touch_down;
static void touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
	(void)indev;
	data->point = touch_point;
	data->state = touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
	(void)area; (void)pixels;
	lv_display_flush_ready(display);
}
static void swipe_back_from(lv_indev_t *pointer, lv_obj_t *page, lv_obj_t *surface, lv_obj_t *destination, int height) {
	switch_screen(page);
	lv_obj_scroll_to_view(surface, LV_ANIM_OFF);
	lv_obj_update_layout(page);
	lv_area_t area;
	lv_obj_get_coords(surface, &area);
	touch_point = (lv_point_t){area.x1 + 8, (area.y1 + area.y2) / 2};
	assert(touch_point.y >= 65 && touch_point.y < height);
	touch_down = true;
	lv_indev_read(pointer);
	for (int step = 0; step < 5; step++) {
		touch_point.x += ui_panel_width / 10;
		assert(touch_point.x < ui_panel_width);
		lv_tick_inc(20);
		lv_indev_read(pointer);
	}
	touch_down = false;
	lv_indev_read(pointer);
	for (int step = 0; step < 12; step++) {
		lv_tick_inc(20);
		lv_timer_handler();
	}
	assert(lv_screen_active() == destination);
	assert(!prompt_open && !fixture_auto_preference && !atomic_load(&calls));
	assert(!keyboard_visible_fixture);
}
int main(int argc, char **argv) {
	ui_panel_width = argc > 1 ? atoi(argv[1]) : 320;
	int height = ui_panel_width == 320 ? 480 : 800;
	alarm(40);
	char dir[] = "/tmp/sonix-navigation-ui.XXXXXX";
	assert(mkdtemp(dir));
	snprintf(config_file, sizeof(config_file), "%s/config.ini", dir);
	navidrome_init();
	lv_init();
	font_ui_14 = font_ui_18 = font_ui_20 = font_ui_24 = lv_font_unscii_8;
	lv_display_t *display = lv_display_create(ui_panel_width, height);
	uint8_t *draw = malloc(ui_panel_width * 32 * 4);
	assert(draw);
	lv_display_set_buffers(display, draw, NULL, ui_panel_width * 32 * 4, LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(display, flush);
	main_menu_screen = lv_obj_create(NULL);
	lv_screen_load(main_menu_screen);
	gui_config_t cfg = {.screen_width = ui_panel_width, .screen_height = height, .padding = 8, .top_bar_height = 20};
	navidromesettings_init(&cfg);
	lv_indev_t *pointer = lv_indev_create();
	lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(pointer, touch_read);
	lv_obj_t *card = lv_obj_get_parent(cache_total);
	lv_obj_t *surfaces[] = {card, lv_obj_get_child(card, 0), cache_values[0], cache_values[1], cache_bars[0], cache_bars[1], cache_total};
	for (unsigned i = 0; i < sizeof(surfaces) / sizeof(*surfaces); i++) {
		switch_screen(settings_form.screen);
		lv_obj_scroll_to_view(card, LV_ANIM_OFF);
		lv_obj_update_layout(settings_form.screen);
		lv_area_t bounds;
		lv_obj_get_coords(surfaces[i], &bounds);
		touch_point = (lv_point_t){bounds.x1 + 8, (bounds.y1 + bounds.y2) / 2};
		if (!i) touch_point.y = bounds.y1 + 2; // Bare card background.
		assert(touch_point.y >= 65 && touch_point.y < height);
		touch_down = true;
		lv_indev_read(pointer);
		for (int step = 0; step < 5; step++) {
			touch_point.x += 42;
			lv_tick_inc(20);
			lv_indev_read(pointer);
		}
		touch_down = false;
		lv_indev_read(pointer);
		for (int step = 0; step < 12; step++) {
			lv_tick_inc(20);
			lv_timer_handler();
		}
		assert(lv_screen_active() == main_menu_screen);
		assert(!prompt_open && !fixture_auto_preference && !atomic_load(&calls));
	}
	// Field gestures must navigate without showing the keyboard or saving drafts.
	switch_screen(settings_form.screen);
	for (int i = 0; i < 3; i++) {
		swipe_back_from(pointer, main_form.screen, main_form.fields[i], settings_form.screen, height);
		assert(!*lv_textarea_get_text(main_form.fields[i]));
		switch_screen(main_form.screen);
		lv_obj_send_event(main_form.fields[i], LV_EVENT_CLICKED, NULL);
		assert(keyboard_visible_fixture);
		swipe_back_from(pointer, main_form.screen, main_form.fields[i], settings_form.screen, height);
	}
	// Disabled controls must remain inert while leaving their area navigable.
	lv_obj_t *disabled[] = {save_button, update_button, main_form.fields[0], main_form.fields[1], main_form.fields[2]};
	for (unsigned i = 0; i < sizeof(disabled) / sizeof(*disabled); i++) {
		switch_screen(main_form.screen);
		lv_obj_set_hidden(disabled[i], false);
		enable(disabled[i], false);
		assert(lv_obj_has_state(disabled[i], LV_STATE_DISABLED));
		swipe_back_from(pointer, main_form.screen, disabled[i], settings_form.screen, height);
	}
	// A canceled short Back drag cannot open the editor or reveal the password.
	lv_obj_t *cancel_surfaces[] = {main_form.fields[2], main_form.eyes[2]};
	for (unsigned i = 0; i < sizeof(cancel_surfaces) / sizeof(*cancel_surfaces); i++) {
		switch_screen(main_form.screen);
		lv_obj_scroll_to_view(cancel_surfaces[i], LV_ANIM_OFF);
		lv_obj_update_layout(main_form.screen);
		lv_area_t bounds;
		lv_obj_get_coords(cancel_surfaces[i], &bounds);
		touch_point = (lv_point_t){bounds.x1 + 4, (bounds.y1 + bounds.y2) / 2};
		touch_down = true; lv_indev_read(pointer);
		for (int step = 0; step < 5; step++) {
			touch_point.x += 4;
			lv_tick_inc(20); lv_indev_read(pointer);
		}
		touch_down = false; lv_indev_read(pointer);
		for (int step = 0; step < 12; step++) { lv_tick_inc(20); lv_timer_handler(); }
		assert(lv_screen_active() == main_form.screen && !keyboard_visible_fixture);
		assert(lv_textarea_get_password_mode(main_form.fields[2]));
	}
	switch_screen(settings_form.screen);
	// An ordinary tap still focuses the editor and brings up the shared keyboard.
	switch_screen(main_form.screen);
	lv_obj_scroll_to_view(main_form.fields[0], LV_ANIM_OFF);
	lv_obj_update_layout(main_form.screen);
	lv_tick_inc(30); lv_timer_handler();
	lv_area_t field_bounds;
	lv_obj_get_coords(main_form.fields[0], &field_bounds);
	touch_point = (lv_point_t){field_bounds.x1 + 15, (field_bounds.y1 + field_bounds.y2) / 2};
	touch_down = true; lv_indev_read(pointer);
	lv_tick_inc(25);
	touch_down = false; lv_indev_read(pointer);
	assert(keyboard_visible_fixture && lv_obj_has_state(main_form.fields[0], LV_STATE_FOCUSED));
	keyboard_visible(&main_form, false);
	lv_textarea_set_text(main_form.fields[0], "http://synthetic-host-with-a-long-address.example/long/path/for/cursor/scrolling/without/touch-scrolling");
	lv_textarea_set_cursor_pos(main_form.fields[0], LV_TEXTAREA_CURSOR_LAST);
	for (int step = 0; step < 15; step++) { lv_tick_inc(20); lv_timer_handler(); }
	assert(lv_obj_get_scroll_x(main_form.fields[0]) > 0);
	assert(lv_screen_active() == main_form.screen);
	// Vertical drags over the field still scroll the enclosing form while editing.
	keyboard_visible(&main_form, true);
	// Force overflow even with the small fixture font on the taller panel.
	lv_obj_set_height(main_form.body, ui_px(180));
	lv_obj_scroll_to_y(main_form.body, 0, LV_ANIM_OFF);
	lv_obj_scroll_to_view(main_form.fields[0], LV_ANIM_OFF);
	lv_obj_update_layout(main_form.screen);
	lv_obj_get_coords(main_form.fields[0], &field_bounds);
	touch_point = (lv_point_t){field_bounds.x1 + 15, (field_bounds.y1 + field_bounds.y2) / 2};
	int before_scroll = lv_obj_get_scroll_y(main_form.body);
	touch_down = true; lv_indev_read(pointer);
	for (int step = 0; step < 4; step++) {
		touch_point.y -= 10;
		lv_tick_inc(20); lv_indev_read(pointer);
	}
	touch_down = false; lv_indev_read(pointer);
	assert(lv_screen_active() == main_form.screen);
	assert(lv_obj_get_scroll_y(main_form.body) > before_scroll);
	assert(keyboard_visible_fixture && !atomic_load(&calls));
	keyboard_visible(&main_form, false);
	puts("production server swipe-back: Host/Username/Password and disabled actions/fields navigate without keyboard, submission or disclosure: passed");
	lv_indev_delete(pointer);
	navidrome_reset();
	lv_display_delete(display);
	free(draw);
	lv_deinit();
	puts("production swipe-back: actual pointer drags from cache background, heading, values, both bars and total return without Clear/toggle/network actions: passed");
	return 0;
}

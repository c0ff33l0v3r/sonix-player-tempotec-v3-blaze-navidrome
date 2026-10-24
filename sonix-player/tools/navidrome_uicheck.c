// Real LVGL lifecycle events and vault/service; reuse synthetic HTTP/storage fixtures.
#define main navidrome_service_test_main
#include "navidrome_selftest.c"
#undef main
#include "src/gui/streaming/navidromesettings.c"

// Stub surrounding shell presentation; the form, widgets, events and service are real.
int32_t ui_panel_width = 320;
lv_font_t font_ui_14, font_ui_18, font_ui_20;
extern const lv_font_t lv_font_unscii_8;
lv_style_t theme_style_screen, theme_style_card, theme_style_card_pressed;
lv_style_t theme_style_text, theme_style_text_dim, theme_style_icon;
static const uint8_t pixel[4] = {255, 255, 255, 255};
#define TEST_IMAGE { .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = 1, .h = 1, .stride = 4 }, .data_size = 4, .data = pixel }
const lv_image_dsc_t icon_eye = TEST_IMAGE, icon_eye_off = TEST_IMAGE;
const char *tr(const char *key) { return key; }
void gui_notify_popup(const char *key) { (void)key; }
bool switcher_back_drag_active(void) { return false; }
void switcher_attach_back_gesture(lv_obj_t *screen) { (void)screen; }
void switch_screen(lv_obj_t *screen) { lv_screen_load(screen); }
void switch_screen_return_to(lv_obj_t *screen) { lv_screen_load(screen); }
int settingsrow_content_top(gui_config_t *cfg) { (void)cfg; return 65; }
lv_obj_t *settingsrow_title(lv_obj_t *screen, gui_config_t *cfg, const char *text) {
	(void)cfg;
	lv_obj_t *label = lv_label_create(screen);
	lv_label_set_text(label, text);
	return label;
}
void settingsrow_title_corner_slots(lv_obj_t *title, gui_config_t *cfg, int n) { (void)title; (void)cfg; (void)n; }
void confirm_show(const char *title, const char *text, const char *ok, void (*cb)(void *), void *user) {
	(void)title; (void)text; (void)ok; (void)cb; (void)user;
}
keyboard_t *keyboard_create(lv_obj_t *parent, int w, int h, lv_obj_t *field, const lv_image_dsc_t *icon,
		const char *text, lv_event_cb_t cb, void *user) {
	(void)parent; (void)w; (void)h; (void)field; (void)icon; (void)text; (void)cb; (void)user;
	return NULL;
}
void keyboard_set_visible(keyboard_t *kb, bool show) { (void)kb; (void)show; }
void keyboard_set_field(keyboard_t *kb, lv_obj_t *field) { (void)kb; (void)field; }
void keyboard_reset(keyboard_t *kb) { (void)kb; }
void keyboard_style_caret(lv_obj_t *field) { (void)field; }
void keyboard_show_caret(lv_obj_t *field, bool show) { (void)field; (void)show; }
void keyboard_style_password(lv_obj_t *field, uint32_t ms) { (void)ms; lv_textarea_set_password_mode(field, true); }
void keyboard_refresh_password(lv_obj_t *field) { (void)field; }
void frameprof_begin(const char *tag) { (void)tag; }
void frameprof_end(const char *tag) { (void)tag; }
bool frameprof_timing(void) { return false; }
unsigned long long frameprof_now_us(void) { return 0; }
void frameprof_timer_ran(void *cb, unsigned us) { (void)cb; (void)us; }
void frameprof_event_ran(void *cb, unsigned us) { (void)cb; (void)us; }

static void enter_server(void) {
	lv_textarea_set_text(main_form.fields[0], "http://fixture/base");
	lv_textarea_set_text(main_form.fields[1], "fixture user");
	lv_textarea_set_text(main_form.fields[2], server_password);
	save_cb(NULL);
	assert(lv_screen_active() == pass_form.screen);
	assert(!strcmp(staged.host, "http://fixture/base"));
	assert(!strcmp(staged.user, "fixture user") && !strcmp(staged.password, server_password));
}
static void enter_unlock_password(void) {
	lv_textarea_set_text(pass_form.fields[1], "vault1");
	lv_textarea_set_text(pass_form.fields[2], "vault1");
	pass_accept_cb(NULL);
}
static void assert_cleared(void) {
	const unsigned char zero[sizeof(staged)] = {0};
	assert(!memcmp(&staged, zero, sizeof(staged)));
	for (int i = 0; i < 3; i++)
		assert(!*lv_textarea_get_text(pass_form.fields[i]));
}

int main(void) {
	alarm(60);
	char dir[] = "/tmp/sonix-navidrome-ui-XXXXXX";
	assert(mkdtemp(dir) && !chdir(dir));
	assert(!config_path());
	navidrome_init();
	lv_init();
	font_ui_14 = font_ui_18 = font_ui_20 = lv_font_unscii_8;
	lv_display_t *display = lv_display_create(320, 480);
	lv_obj_t *outside = lv_obj_create(NULL);
	gui_config_t cfg = {.screen_width = 320, .screen_height = 480, .padding = 8, .top_bar_height = 20};
	navidromesettings_init(&cfg);
	navidromesettings_open();
	enter_server();
	atomic_store(&fail_rename, 1);
	enter_unlock_password();
	navidrome_snapshot_t s = wait_idle();
	navidromesettings_refresh();
	assert(!s.configured && !strcmp(s.error, "nd_save_error") && !atomic_load(&calls));
	assert(lv_screen_active() == pass_form.screen && !lv_obj_has_state(pass_button, LV_STATE_DISABLED));
	assert(!strcmp(staged.password, server_password));
	assert(!*lv_textarea_get_text(pass_form.fields[1]));
	enter_unlock_password(); // Retry without re-entering server credentials.
	s = wait_idle();
	navidromesettings_refresh();
	assert(s.state == ND_CONNECTED && lv_screen_active() == main_form.screen);
	assert(!strcmp(lv_textarea_get_text(main_form.fields[0]), "http://fixture/base"));
	assert(!strcmp(lv_textarea_get_text(main_form.fields[1]), "fixture user"));
	assert(!strcmp(lv_textarea_get_text(main_form.fields[2]), server_password));
	assert(lv_textarea_get_password_mode(main_form.fields[2]));
	assert_cleared();
	puts("UI failed-save retry and masked saved fields: passed");

	assert(navidrome_forget());
	wait_idle();
	navidromesettings_refresh();
	enter_server();
	lv_textarea_set_text(pass_form.fields[1], "vault1");
	switch_screen_return_to(main_form.screen); // Back cancels setup immediately.
	assert_cleared();
	for (int i = 0; i < 3; i++)
		assert(!*lv_textarea_get_text(main_form.fields[i]));
	switch_screen(outside);
	navidromesettings_open();
	for (int i = 0; i < 3; i++)
		assert(!*lv_textarea_get_text(main_form.fields[i]));
	puts("UI Back cancellation and fresh entry: passed");

	enter_server();
	atomic_store(&fail_sync, 3);
	enter_unlock_password();
	for (int i = 0; i < 10000 && !atomic_load(&sync_blocked); i++)
		usleep(1000);
	assert(atomic_load(&sync_blocked));
	switch_screen(outside); // Submitted work survives leaving; drafts do not.
	assert_cleared();
	atomic_store(&release_sync, 1);
	s = wait_idle();
	navidromesettings_refresh();
	assert(s.state == ND_CONNECTED && lv_screen_active() == outside);
	navidromesettings_open();
	assert(!strcmp(lv_textarea_get_text(main_form.fields[2]), server_password));
	puts("UI leaving during save and late completion: passed");
	assert(navidrome_forget());
	wait_idle();
	navidrome_shutdown();
	lv_display_delete(display);
	lv_deinit();
	assert(!chdir("/") && !rmdir(dir));
	return 0;
}

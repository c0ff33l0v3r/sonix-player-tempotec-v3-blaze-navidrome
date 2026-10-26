// Real LVGL lifecycle events and vault/service; reuse synthetic HTTP/storage fixtures.
#define main navidrome_service_test_main
#include "navidrome_selftest.c"
#undef main
#include "src/gui/streaming/navidromesettings.c"
#include "src/gui/streaming/navidromealbum.c"

void navidromeplay_cache_stop(void) {}
bool navidromeplay_cache_reader_active(void) { return false; }

// Stub surrounding shell presentation; the form, widgets, events and service are real.
int32_t ui_panel_width = 320;
lv_font_t font_ui_14, font_ui_18, font_ui_20, font_ui_24;
const theme_palette_t *theme(void) { static theme_palette_t colors; return &colors; }
extern const lv_font_t lv_font_unscii_8;
lv_style_t theme_style_screen, theme_style_card, theme_style_card_pressed;
lv_style_t theme_style_text, theme_style_text_dim, theme_style_icon;
static unsigned prompt_count;
static bool prompt_open, prompt_enabled, keyboard_visible_fixture;
static void (*prompt_yes)(void *), (*prompt_no)(void *);
static void *prompt_user;
static void answer_later(void) {
	void (*callback)(void *) = prompt_no;
	void *user = prompt_user;
	prompt_open = false;
	assert(callback);
	callback(user);
}
static const uint8_t pixel[4] = {255, 255, 255, 255};
#define TEST_IMAGE {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = 1, .h = 1, .stride = 4}, .data_size = 4, .data = pixel}
const lv_image_dsc_t icon_eye = TEST_IMAGE, icon_eye_off = TEST_IMAGE;
const char *tr(const char *key) { return !strcmp(key, "nd_cache_total") ? "Total: %llu.%02llu MiB" : key; }
static bool fixture_cache_override;
static nd_cache_usage_t fixture_cache_usage;
void __real_nd_cache_usage_snapshot(nd_cache_usage_t *usage);
void __wrap_nd_cache_usage_snapshot(nd_cache_usage_t *usage) {
	if (fixture_cache_override) *usage = fixture_cache_usage;
	else __real_nd_cache_usage_snapshot(usage);
}
void __real_navidrome_library_snapshot(nd_library_t *library);
void __wrap_navidrome_library_snapshot(nd_library_t *library) {
	__real_navidrome_library_snapshot(library);
	if (fixture_cache_override) {
		snprintf(library->path, sizeof(library->path), "./card/.local/navidrome/.cache/library.db");
		library->writable = true;
		library->card_epoch = fixture_cache_usage.card_epoch;
	}
}
static char fixture_popup[100];
void gui_notify_popup(const char *key) { snprintf(fixture_popup,sizeof(fixture_popup),"%s",key); }
static bool fixture_navigation_drag;
bool switcher_back_drag_active(void) { return fixture_navigation_drag; }
bool player_sheet_drag_active(void) { return fixture_navigation_drag; }
void switcher_attach_back_gesture(lv_obj_t *screen) { (void)screen; }
void switch_screen(lv_obj_t *screen) { lv_screen_load(screen); }
void switch_screen_return_to(lv_obj_t *screen) { lv_screen_load(screen); }
int settingsrow_content_top(gui_config_t *cfg) {
	(void)cfg;
	return 65;
}
lv_obj_t *settingsrow_title(lv_obj_t *screen, gui_config_t *cfg, const char *text) {
	(void)cfg;
	lv_obj_t *label = lv_label_create(screen);
	lv_label_set_text(label, text);
	return label;
}
lv_obj_t *settingsrow_page(lv_obj_t *screen, gui_config_t *cfg, const char *title) {
	settingsrow_title(screen, cfg, title);
	lv_obj_t *body = lv_obj_create(screen);
	lv_obj_set_size(body, cfg->screen_width, cfg->screen_height - 65);
	lv_obj_set_y(body, 65);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	return body;
}
lv_obj_t *settingsrow_page_title(lv_obj_t *screen) { return lv_obj_get_child(screen, 0); }
lv_obj_t *settingsrow_add(lv_obj_t *parent, const char *name, lv_obj_t **value, lv_event_cb_t cb, void *user) {
	(void)value;
	lv_obj_t *row = lv_button_create(parent);
	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text(label, name);
	if (cb)
		lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user);
	return row;
}
lv_obj_t *settingsrow_action(lv_obj_t *parent, const char *name, lv_event_cb_t cb, void *user) { return settingsrow_add(parent, name, NULL, cb, user); }
lv_obj_t *settingsrow_toggle(lv_obj_t *parent, const char *name, lv_obj_t **toggle, lv_event_cb_t cb) {
	lv_obj_t *row = settingsrow_add(parent, name, NULL, NULL, NULL);
	*toggle = lv_switch_create(row);
	lv_obj_add_event_cb(*toggle, cb, LV_EVENT_VALUE_CHANGED, NULL);
	return row;
}
void settingsrow_title_corner_slots(lv_obj_t *title, gui_config_t *cfg, int n) {
	(void)title;
	(void)cfg;
	(void)n;
}
void confirm_show(const char *title, const char *text, const char *ok, void (*cb)(void *), void *user) {
	(void)title;
	(void)text;
	(void)ok;
	prompt_open = true;
	prompt_yes = cb;
	prompt_user = user;
}
keyboard_t *keyboard_create(lv_obj_t *parent, int w, int h, lv_obj_t *field, const lv_image_dsc_t *icon, const char *text, lv_event_cb_t cb, void *user) {
	(void)parent;
	(void)w;
	(void)h;
	(void)field;
	(void)icon;
	(void)text;
	(void)cb;
	(void)user;
	return NULL;
}
void keyboard_set_visible(keyboard_t *kb, bool show) {
	(void)kb;
	(void)show;
}
void keyboard_set_field(keyboard_t *kb, lv_obj_t *field) {
	(void)kb;
	(void)field;
}
void keyboard_reset(keyboard_t *kb) { (void)kb; }
void keyboard_style_caret(lv_obj_t *field) { (void)field; }
void keyboard_show_caret(lv_obj_t *field, bool show) {
	(void)field;
	(void)show;
}
void keyboard_style_password(lv_obj_t *field, uint32_t ms) {
	(void)ms;
	lv_textarea_set_password_mode(field, true);
}
void keyboard_refresh_password(lv_obj_t *field) { (void)field; }
void frameprof_begin(const char *tag) { (void)tag; }
void frameprof_end(const char *tag) { (void)tag; }
bool frameprof_timing(void) { return false; }
unsigned long long frameprof_now_us(void) { return 0; }
void frameprof_timer_ran(void *cb, unsigned us) {
	(void)cb;
	(void)us;
}
void frameprof_event_ran(void *cb, unsigned us) {
	(void)cb;
	(void)us;
}

static int fixture_album_preference = -1;
bool config_get_bool(const char *section, const char *key, bool fallback) {
	if (!strcmp(section, "navidrome") && !strcmp(key, "album_head") && fixture_album_preference >= 0) return fixture_album_preference != 0;
	(void)section;
	(void)key;
	return fallback;
}
void config_set_bool(const char *section, const char *key, bool value) {
	if (!strcmp(section, "navidrome") && !strcmp(key, "album_head")) fixture_album_preference = value;
	(void)section;
	(void)key;
	(void)value;
}
bool config_save(void) { return true; }

static void enter_server(void) {
	encrypt_selected = true;
	server_cb(NULL);
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
	alarm(90);
	char dir[] = "/tmp/sonix-navidrome-ui-XXXXXX";
	assert(mkdtemp(dir) && !chdir(dir));
	navidrome_init();
	lv_init();
	font_ui_14 = font_ui_18 = font_ui_20 = font_ui_24 = lv_font_unscii_8;
	lv_display_t *display = lv_display_create(320, 480);
	lv_obj_t *outside = lv_obj_create(NULL);
	gui_config_t cfg = {.screen_width = 320, .screen_height = 480, .padding = 8, .top_bar_height = 20};
	navidromesettings_init(&cfg);
	navidromesettings_open();
	assert(lv_screen_active() == settings_form.screen);
    assert(lv_obj_has_state(album_view_switch,LV_STATE_CHECKED) && navidromealbum_enabled());
    lv_obj_remove_state(album_view_switch,LV_STATE_CHECKED);
    lv_obj_send_event(album_view_switch,LV_EVENT_VALUE_CHANGED,NULL);
    assert(!navidromealbum_enabled() && !atomic_load(&calls));
    fixture_navigation_drag=true;
    lv_obj_add_state(album_view_switch,LV_STATE_CHECKED);
    lv_obj_send_event(album_view_switch,LV_EVENT_VALUE_CHANGED,NULL);
    assert(!navidromealbum_enabled() && !lv_obj_has_state(album_view_switch,LV_STATE_CHECKED));
    fixture_navigation_drag=false;
    lv_obj_add_state(album_view_switch,LV_STATE_CHECKED);
    lv_obj_send_event(album_view_switch,LV_EVENT_VALUE_CHANGED,NULL);
    assert(navidromealbum_enabled() && !atomic_load(&calls));

	enter_server();
	atomic_store(&fail_rename, 1);
	enter_unlock_password();
	navidrome_snapshot_t state = wait_idle();
	navidromesettings_refresh();
	assert(!state.configured && !strcmp(state.error, "nd_save_error") && !atomic_load(&calls));
	assert(lv_screen_active() == pass_form.screen && !lv_obj_has_state(pass_button, LV_STATE_DISABLED));
	assert(!strcmp(staged.password, server_password));
	enter_unlock_password();
	state = wait_idle();
	navidromesettings_refresh();
	assert(state.encrypted && state.state == ND_CONNECTED && lv_screen_active() == main_form.screen);
	assert(!strcmp(lv_textarea_get_text(main_form.fields[2]), server_password));
	assert(lv_textarea_get_password_mode(main_form.fields[2]));
	assert(lv_obj_has_state(update_button, LV_STATE_DISABLED));
	assert(lv_obj_get_style_opa(update_button, LV_PART_MAIN) == LV_OPA_40);
	assert_cleared();
	assert(!prompt_count); // The server page cannot be interrupted by a sync offer.
	lv_textarea_set_password_mode(main_form.fields[2], false);
	lv_textarea_set_text(main_form.fields[0], "http://another/base");
	lv_textarea_set_text(main_form.fields[1], "another user");
	assert(!strcmp(lv_textarea_get_text(main_form.fields[2]), server_password));
	assert(!lv_textarea_get_password_mode(main_form.fields[2]));
	switch_screen_return_to(settings_form.screen);
	assert(!*lv_textarea_get_text(main_form.fields[2]));
	navidromesettings_refresh();
	assert(prompt_count == 1 && prompt_open && !prompt_enabled);
	answer_later();
	server_cb(NULL);
	assert(!strcmp(lv_textarea_get_text(main_form.fields[0]), "http://fixture/base"));
	assert(lv_textarea_get_password_mode(main_form.fields[2]));
	puts("UI encrypted save retry, preserved identity/password draft, discard and deferred offer: passed");

	switch_screen_return_to(settings_form.screen);
	navidrome_lock();
	navidromesettings_refresh();
	lv_obj_remove_state(encrypt_switch, LV_STATE_CHECKED);
	encrypt_cb(NULL);
	assert(lv_screen_active() == pass_form.screen && pass_mode == PASSPHRASE_PLAIN);
	int requests = atomic_load(&calls);
	lv_textarea_set_text(pass_form.fields[0], "wrong password");
	pass_accept_cb(NULL);
	wait_idle();
	navidromesettings_refresh();
	assert(lv_screen_active() == pass_form.screen);
	lv_textarea_set_text(pass_form.fields[0], "vault1");
	pass_accept_cb(NULL);
	state = wait_idle();
	navidromesettings_refresh();
	assert(!state.encrypted && state.unlocked && lv_screen_active() == settings_form.screen);
	assert(atomic_load(&calls) == requests && lv_obj_is_hidden(lock_button));
	lv_obj_add_state(encrypt_switch, LV_STATE_CHECKED);
	encrypt_cb(NULL);
	assert(pass_mode == PASSPHRASE_ENCRYPT);
	atomic_store(&fail_rename, 1);
	enter_unlock_password();
	wait_idle();
	navidromesettings_refresh();
	assert(lv_screen_active() == pass_form.screen);
	enter_unlock_password();
	state = wait_idle();
	navidromesettings_refresh();
	assert(state.encrypted && state.unlocked && lv_screen_active() == settings_form.screen);
	assert(atomic_load(&calls) == requests);
	puts("UI locked conversion authentication, failed conversion retry, conditional controls: passed");

	forget_confirmed(NULL);
	wait_idle();
	assert(!encrypt_selected);
	navidromesettings_refresh();
	server_cb(NULL);
	lv_textarea_set_text(main_form.fields[0], "http://fixture/base");
	lv_textarea_set_text(main_form.fields[1], "fixture user");
	lv_textarea_set_text(main_form.fields[2], server_password);
	save_cb(NULL);
	assert(lv_screen_active() == main_form.screen);
	state = wait_idle();
	navidromesettings_refresh();
	assert(!state.encrypted && state.state == ND_CONNECTED);
	switch_screen_return_to(settings_form.screen);
	navidromesettings_refresh();
	assert(prompt_count == 2 && prompt_open);
	answer_later();
	assert(navidrome_forget());
	wait_idle();
	enter_server();
	switch_screen_return_to(main_form.screen); // Cancel password creation.
	assert_cleared();
	assert(!*lv_textarea_get_text(main_form.fields[2]));
	switch_screen_return_to(settings_form.screen);
	enter_server();
	atomic_store(&fail_sync, 3);
	enter_unlock_password();
	for (int i = 0; i < 10000 && !atomic_load(&sync_blocked); i++)
		usleep(1000);
	assert(atomic_load(&sync_blocked));
	switch_screen(outside);
	assert_cleared();
	atomic_store(&release_sync, 1);
	wait_idle();
	navidromesettings_refresh();
	assert(lv_screen_active() == outside);
	navidromesettings_open();
	navidromesettings_refresh();
	assert(prompt_count == 3 && prompt_open);
	answer_later();
	assert(navidrome_update_password("http://fixture/base", "fixture user", server_password));
	wait_idle();
	navidromesettings_refresh();
	assert(prompt_count == 3);
	navidrome_lock();
	assert(navidrome_unlock("vault1"));
	wait_idle();
	navidromesettings_refresh();
	assert(prompt_count == 3);
	atomic_store(&mode, 1);
	assert(navidrome_reconnect());
	wait_idle();
	navidromesettings_refresh();
	assert(prompt_count == 3);
	puts("UI plain setup, cancellation, late completion and operation-origin guards: passed");
	fixture_cache_override = true;
	fixture_cache_usage = (nd_cache_usage_t){.audio = 1024 * 1024, .art = 512 * 1024, .ready = true, .card_epoch = 123};
	cache_last_request = lv_tick_get();
	cache_refresh();
	assert(!lv_obj_has_state(cache_clear, LV_STATE_DISABLED));
	assert(!strcmp(lv_label_get_text(cache_total), "Total: 1.50 MiB"));
	fixture_cache_usage.busy = true; // A periodic usage scan keeps Clear available.
	cache_refresh();
	assert(!lv_obj_has_state(cache_clear, LV_STATE_DISABLED));
	fixture_cache_usage.clearing = true;
	cache_refresh();
	assert(lv_obj_has_state(cache_clear, LV_STATE_DISABLED));
	fixture_cache_override = false;
	puts("UI Clear stays enabled during counting, disables during clearing, and total uses fractional MiB: passed");
	cache_clear_cb(NULL); assert(prompt_open);
	navidrome_set_root(NULL); // Card identity changes while the confirmation is open.
	confirm_close(); assert(prompt_yes); prompt_yes(prompt_user);
	assert(!strcmp(fixture_popup,"nd_cache_changed"));
	nd_cache_usage_t cache_state; nd_cache_usage_snapshot(&cache_state); assert(!cache_state.busy);
	nd_library_t no_card;
	for(int i=0;i<10000;i++){navidrome_library_snapshot(&no_card);if(!no_card.loading)break;usleep(1000);}
	cache_refresh();
	assert(!strcmp(lv_label_get_text(cache_total),"nd_cache_storage") && lv_obj_has_state(cache_clear,LV_STATE_DISABLED));
	puts("UI cache confirmation rejects a changed card before admission, and absent storage stops Working with a disabled Clear action: passed");
	assert(navidrome_forget());
	wait_idle();
	navidrome_shutdown();
	lv_display_delete(display);
	lv_deinit();
	assert(!chdir("/") && !rmdir(dir));
	return 0;
}

void confirm_close(void) { prompt_open = false; }
bool confirm_visible(void) { return prompt_open; }
bool gui_modal_visible(void) { return false; }
bool keyboard_is_visible(keyboard_t *kb) {
	(void)kb;
	return keyboard_visible_fixture;
}
void confirm_show_choice(const char *title, const char *message, const char *yes, const char *no, bool enabled, void (*on_yes)(void *), void (*on_no)(void *), void *user) {
	(void)title;
	(void)message;
	(void)yes;
	(void)no;
	prompt_count++;
	prompt_open = true;
	prompt_enabled = enabled;
	prompt_yes = on_yes;
	prompt_no = on_no;
	prompt_user = user;
}

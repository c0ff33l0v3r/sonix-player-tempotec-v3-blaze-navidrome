// Production persistent pull-down panel, shared controller and real favorite
// worker/store with synthetic service responses. No output device or full build.
#define NAVIDROME_QUICKPANEL_UI
#include "navidrome_star_uicheck.c"
#include "src/gui/shell/quickpanel.c"

const lv_image_dsc_t icon_airplay_quick = FIXTURE_ICON, icon_bluetooth = FIXTURE_ICON, icon_control_center_line = FIXTURE_ICON, icon_dlna_quick = FIXTURE_ICON, icon_equalizer = FIXTURE_ICON, icon_fade_track = FIXTURE_ICON, icon_gapless_off_quick = FIXTURE_ICON, icon_gapless_on_quick = FIXTURE_ICON, icon_high_gain = FIXTURE_ICON, icon_lineout = FIXTURE_ICON, icon_low_gain = FIXTURE_ICON, icon_mid_gain = FIXTURE_ICON, icon_mseb = FIXTURE_ICON, icon_next_10 = FIXTURE_ICON, icon_next_30 = FIXTURE_ICON, icon_next_60 = FIXTURE_ICON, icon_pause_large = FIXTURE_ICON, icon_peq_quick = FIXTURE_ICON, icon_play_large = FIXTURE_ICON, icon_prev_10 = FIXTURE_ICON, icon_prev_30 = FIXTURE_ICON, icon_prev_60 = FIXTURE_ICON, icon_repeat_all = FIXTURE_ICON, icon_repeat_off = FIXTURE_ICON, icon_repeat_one = FIXTURE_ICON, icon_shuffle_repeat = FIXTURE_ICON, icon_skip_back_large = FIXTURE_ICON, icon_skip_forward_large = FIXTURE_ICON, icon_sleep_audiobook_quick = FIXTURE_ICON, icon_sleep_music_quick = FIXTURE_ICON, icon_sleep_podcast_quick = FIXTURE_ICON, icon_sonixlink_quick = FIXTURE_ICON, icon_stop = FIXTURE_ICON, icon_sun = FIXTURE_ICON, icon_wifi = FIXTURE_ICON, icon_wifi_transfer_quick = FIXTURE_ICON;

// Unrelated quick controls have inert getters and fail if activated. Only the
// actual favorite routes below are allowed to mutate a synthetic source.
const lv_image_dsc_t icon_loader_big = FIXTURE_ICON;
lv_obj_t *spinner_create(lv_obj_t *parent, const lv_image_dsc_t *icon) {
	lv_obj_t *image = lv_image_create(parent);
	lv_image_set_src(image, icon);
	return image;
}
void config_set(const char *section, const char *key, const char *value) {
	(void)value;
	assert(!strcmp(section, "quickpanel") && (!strcmp(key, "order") || !strcmp(key, "hidden") || !strcmp(key, "order_more") || !strcmp(key, "hidden_more")));
}
lv_font_t font_ui_22, font_ui_26;
lv_style_t theme_style_switch;
lv_obj_t *wifisettings_screen, *btsettings_screen, *airplay_screen, *sonixlink_screen, *dlna_screen, *wifitransfer_screen, *audiobooksettings_screen;
static bool fixture_book, fixture_radio, fixture_podcast, fixture_local_fav, fixture_radio_fav, fixture_followed;
static unsigned fixture_local_writes, fixture_radio_writes, fixture_follow_writes, fixture_full_refreshes, fixture_favorite_refreshes;
static bool fixture_usb;
#define FALSE_GETTER(name)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             \
	bool name(void) { return false; }
FALSE_GETTER(eq_get_enabled)
FALSE_GETTER(mseb_get_enabled)
FALSE_GETTER(wifi_get_enabled)
FALSE_GETTER(bluetooth_get_enabled)
FALSE_GETTER(musicsettings_fade_enabled)
FALSE_GETTER(airplay_get_enabled)
FALSE_GETTER(sonixlink_get_enabled)
FALSE_GETTER(dlna_get_enabled)
FALSE_GETTER(peq_get_enabled)
FALSE_GETTER(wifitransfer_get_enabled)
FALSE_GETTER(musicsettings_gapless_enabled)
FALSE_GETTER(lineout_is_active)
FALSE_GETTER(bluetooth_audio_active)
FALSE_GETTER(radio_can_step)
FALSE_GETTER(radio_is_connecting)
FALSE_GETTER(btreceiver_is_active)
FALSE_GETTER(wifi_available)
FALSE_GETTER(bluetooth_available)
FALSE_GETTER(airplay_available)
FALSE_GETTER(dlna_available)
FALSE_GETTER(wifitransfer_available)
FALSE_GETTER(player_sheet_is_open)
FALSE_GETTER(topbar_is_hidden)
#undef FALSE_GETTER
bool usbdac_is_active(void) { return fixture_usb; }
bool audiobook_is_playing(void) { return fixture_book; }
int audiobook_skip_back(void) { return 10; }
int audiobook_skip_forward(void) { return 30; }
int podcast_skip_back(void) { return 10; }
int podcast_skip_forward(void) { return 30; }
bool podcastcache_is_episode(const char *p) {
	(void)p;
	return fixture_podcast;
}
bool player_current_podcast_feed(long long *id, podcast_feed_t *feed) {
	if (id)
		*id = 77;
	if (feed) {
		memset(feed, 0, sizeof(*feed));
		feed->id = 77;
	}
	return fixture_podcast;
}
bool podcastsubs_is_followed(long long id) {
	assert(id == 77);
	return fixture_followed;
}
void podcastsubs_follow(const podcast_feed_t *feed) {
	assert(feed->id == 77);
	fixture_followed = true;
	fixture_follow_writes++;
}
void podcastsubs_unfollow(long long id) {
	assert(id == 77);
	fixture_followed = false;
	fixture_follow_writes++;
}
bool radio_current_station(radio_station_t *out) {
	memset(out, 0, sizeof(*out));
	snprintf(out->uuid, sizeof(out->uuid), "station");
	return fixture_radio;
}
bool radio_fav_contains(const char *id) {
	assert(!strcmp(id, "station"));
	return fixture_radio_fav;
}
void radio_fav_add(const radio_station_t *station) {
	assert(!strcmp(station->uuid, "station"));
	fixture_radio_fav = true;
	fixture_radio_writes++;
}
void radio_fav_remove(const char *id) {
	assert(!strcmp(id, "station"));
	fixture_radio_fav = false;
	fixture_radio_writes++;
}
bool library_fav_contains(const char *p) {
	assert(!nd_audio_source(p));
	return fixture_local_fav;
}
bool library_fav_toggle(const char *p, const char *title, const char *artist) {
	(void)title;
	(void)artist;
	assert(!nd_audio_source(p));
	fixture_local_fav = !fixture_local_fav;
	fixture_local_writes++;
	return fixture_local_fav;
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
	assert(false);
	return false;
}
bool qobuzsync_is_favorite(long id) {
	(void)id;
	assert(false);
	return false;
}
void tidalsync_favorites_refresh(void (*done)(void)) {
	(void)done;
	assert(false);
}
void tidalsync_favorite_toggle(long id, bool want, tidalsync_done_cb done, void *user) {
	(void)id;
	(void)want;
	(void)done;
	(void)user;
	assert(false);
}
void qobuzsync_favorite_toggle(long id, bool want, qobuzsync_done_cb done, void *user) {
	(void)id;
	(void)want;
	(void)done;
	(void)user;
	assert(false);
}
int musicsettings_gain_level(void) { return 0; }
int gain_level_count(void) { return 2; }
int get_volume_percent(void) { return 50; }
int headphone_jack_state(void) { return 1; }
long power_get_brightness(void) { return 50; }
long power_get_max_brightness(void) { return 100; }
void power_set_brightness(long value) {
	(void)value;
	assert(false);
}
const char *metadata_shown_artist(const song_metadata_t *m) { return m->artist; }
playback_mode_t playlist_get_mode(void) { return PLAYBACK_MODE_NORMAL; }
playback_mode_t playlist_cycle_mode(void) {
	assert(false);
	return PLAYBACK_MODE_NORMAL;
}
void player_refresh_favorite(void) {
	fixture_favorite_refreshes++;
	if (player_icon && nd_audio_source(represented.current_file))
		paint_player();
}
void player_refresh_now_playing(void) { fixture_full_refreshes++; }
#define UNEXPECTED_VOID(name)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          \
	void name(void) { assert(false); }
UNEXPECTED_VOID(player_key_play_pause)
UNEXPECTED_VOID(player_screen_next)
UNEXPECTED_VOID(player_screen_prev)
UNEXPECTED_VOID(musicsettings_gain_step)
UNEXPECTED_VOID(sonixlink_pump_pace)
UNEXPECTED_VOID(wifitransfer_page_stop)
UNEXPECTED_VOID(wifitransfer_page_open_started)
#undef UNEXPECTED_VOID
#define UNEXPECTED_SETTER(name)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        \
	void name(bool on) {                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               \
		(void)on;                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      \
		assert(false);                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 \
	}
UNEXPECTED_SETTER(musicsettings_set_gapless_enabled)
UNEXPECTED_SETTER(musicsettings_set_fade_enabled)
UNEXPECTED_SETTER(musicsettings_set_eq_enabled)
UNEXPECTED_SETTER(musicsettings_set_mseb_enabled)
UNEXPECTED_SETTER(peq_set_enabled)
UNEXPECTED_SETTER(wifi_set_enabled)
UNEXPECTED_SETTER(bluetooth_set_enabled)
UNEXPECTED_SETTER(airplay_set_enabled)
UNEXPECTED_SETTER(dlna_set_enabled)
UNEXPECTED_SETTER(sonixlink_set_enabled)
UNEXPECTED_SETTER(lineout_set)
UNEXPECTED_SETTER(player_sheet_close)
#undef UNEXPECTED_SETTER
bool sleeptimer_enabled(sleeptimer_kind_t kind) {
	(void)kind;
	return false;
}
int sleeptimer_minutes(sleeptimer_kind_t kind) {
	(void)kind;
	return 0;
}
void sleeptimer_set_enabled(sleeptimer_kind_t kind, bool on) {
	(void)kind;
	(void)on;
	assert(false);
}
void wifi_get_status(wifi_status_t *out) { memset(out, 0, sizeof(*out)); }
void confirm_show(const char *title, const char *message, const char *ok, void (*cb)(void *), void *user) {
	(void)title;
	(void)message;
	(void)ok;
	(void)cb;
	(void)user;
	assert(false);
}
void topbar_set_hidden(bool on) { (void)on; }
void topbar_tint_hold(bool held) { (void)held; }
void topbar_bring_to_front(void) {}
void topbar_refresh_radios(void) {}
void topbar_refresh_volume(int percent) { (void)percent; }
void player_lyrics_covered(unsigned who, bool covered) {
	(void)who;
	(void)covered;
}
void switcher_set_player_return(lv_obj_t *screen) { (void)screen; }
void theme_apply_slider_knob(lv_obj_t *slider) { (void)slider; }
void scrolltext_apply(lv_obj_t *label) { lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR); }
void scrolltext_set(lv_obj_t *label, const char *text) { lv_label_set_text(label, text); }
#define SETTINGS_PAGE(name)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            \
	lv_obj_t *name(void) { return lv_screen_active(); }
SETTINGS_PAGE(musicsettings_eq_screen)
SETTINGS_PAGE(musicsettings_mseb_screen)
SETTINGS_PAGE(musicsettings_fade_screen)
SETTINGS_PAGE(musicsettings_playback_screen)
SETTINGS_PAGE(peqpage_screen)
SETTINGS_PAGE(podcastpage_settings_screen)
#undef SETTINGS_PAGE

static atomic_bool fixture_readonly_cache, fixture_commit_failure, fixture_alloc_failure;
void __real_navidrome_favorites_snapshot(nd_library_t *cache);
void __wrap_navidrome_favorites_snapshot(nd_library_t *cache) {
	__real_navidrome_favorites_snapshot(cache);
	if (atomic_load(&fixture_readonly_cache))
		cache->writable = false;
}
bool __real_navidrome_context_commit(const nd_context_t *c, sqlite3 *db);
bool __wrap_navidrome_context_commit(const nd_context_t *c, sqlite3 *db) { return !atomic_load(&fixture_commit_failure) && __real_navidrome_context_commit(c, db); }
void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size) { return atomic_exchange(&fixture_alloc_failure, false) ? NULL : __real_malloc(size); }
static lv_point_t qp_point;
static bool qp_down;
static lv_indev_t *qp_pointer;
static void qp_read(lv_indev_t *i, lv_indev_data_t *data) {
	(void)i;
	data->point = qp_point;
	data->state = qp_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static void qp_step(int x, int y, bool pressed) {
	qp_point = (lv_point_t){x, y};
	qp_down = pressed;
	lv_tick_inc(20);
	lv_indev_read(qp_pointer);
	lv_obj_update_layout(lv_layer_top());
}
static void qp_settle(void) {
	for (int i = 0; i < 12; i++) {
		lv_tick_inc(20);
		lv_timer_handler();
	}
	lv_obj_update_layout(lv_layer_top());
}
static void qp_show(void) {
	quickpanel_open();
	qp_settle();
	assert(panel_open && favorite_current(favorite_visit));
}
static void qp_select(const char *name) {
	snprintf(represented.current_file, sizeof(represented.current_file), "%s", name);
	refresh_now_playing_card();
	if (nd_audio_source(represented.current_file))
		paint_player();
	lv_obj_update_layout(lv_layer_top());
}
static lv_point_t qp_star_center(void) {
	lv_area_t a;
	lv_obj_get_coords(np_star_btn, &a);
	assert(a.x1 >= 0 && a.x2 < lv_display_get_horizontal_resolution(lv_display_get_default()));
	assert(a.y1 >= 0 && a.y2 < lv_display_get_vertical_resolution(lv_display_get_default()));
	assert(lv_obj_get_width(np_star_btn) == ui_px(56));
	return (lv_point_t){(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
}
static void qp_tap(void) {
	poll_cb(NULL); // the existing visible poll observes shared admission/state changes
	lv_point_t p = qp_star_center();
	qp_step(p.x, p.y, true);
	qp_step(p.x, p.y, false);
}
static void qp_start_blocked(void) {
	atomic_store(&star_entered, false);
	atomic_store(&star_block, true);
	qp_tap();
	block_wait();
}
static void qp_release(void) {
	atomic_store(&star_block, false);
	stars_wait();
}
static void qp_wait_connected(void) {
	assert(wait_idle().state == ND_CONNECTED);
	drain();
}
static void qp_assert_unchanged(device_state_t before, unsigned full_refreshes) {
	assert(!memcmp(&before, &represented, sizeof(before)));
	assert(fixture_full_refreshes == full_refreshes);
	assert(playlist_get_mode() == PLAYBACK_MODE_NORMAL && fixture_local_writes == 0);
}
static void qp_cache_contains(const char *id, bool expected) {
	nd_library_t cache;
	navidrome_favorites_snapshot(&cache);
	nd_store_t store = {0};
	bool starred = false;
	assert(cache.ready && nd_store_open_source(&store, cache.path, cache.scope, false, false, true));
	assert(nd_store_annotation(&store, cache.generation, ND_TRACKS, id, &starred) && starred == expected);
	nd_store_close(&store);
}
static unsigned qp_press_losses;
static void qp_lost(lv_event_t *event) {
	(void)event;
	qp_press_losses++;
}
static void qp_drag(bool close, bool lose_press) {
	lv_point_t p = qp_star_center();
	int writes = atomic_load(&star_writes), requests = atomic_load(&calls);
	qp_step(p.x, p.y, true);
	// Slow travel exercises distance-based close and the retained drag latch.
	int distance = close ? ui_px(210) : ui_px(20);
	for (int j = 1; j <= 10; j++) {
		lv_tick_inc(80);
		qp_step(p.x, p.y - distance * j / 10, true);
	}
	unsigned losses = qp_press_losses;
	if (lose_press)
		lv_indev_wait_release(qp_pointer);
	qp_step(p.x, p.y - distance, false);
	if (lose_press)
		assert(qp_press_losses > losses);
	qp_settle();
	assert(atomic_load(&star_writes) == writes && atomic_load(&calls) == requests);
	if (!lose_press)
		assert(press_dragged);
	assert(panel_open == !close);
}
static void qp_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
	(void)area;
	(void)pixels;
	lv_display_flush_ready(display);
}
int main(int argc, char **argv) {
	alarm(90);
	setvbuf(stdout, NULL, _IONBF, 0);
	assert(argc == 4);
	int width = atoi(argv[1]), height = atoi(argv[2]);
	ui_panel_width = width;
	char dir[] = "/tmp/sonix-quickpanel-fixture-XXXXXX";
	assert(mkdtemp(dir) && !chdir(dir));
	navidrome_init();
	navidrome_set_root(dir);
	assert(navidrome_save("http://fixture/base", "fixture user", server_password, "vault1"));
	qp_wait_connected();
	lv_init();
	font_ui_28 = font_ui_26 = font_ui_24_bold = font_ui_24 = font_ui_22 = font_ui_14 = font_ui_18 = font_ui_20 = font_ui_64_bold = lv_font_unscii_8;
	lv_display_t *display = lv_display_create(width, height);
	size_t buffer_size = (size_t)width * 32 * 2;
	void *buffer = calloc(1, buffer_size);
	assert(buffer);
	lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(display, buffer, NULL, buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(display, qp_flush);
	theme_palette_t *palette = (theme_palette_t *)theme();
	palette->dark = !strcmp(argv[3], "dark");
	palette->accent = lv_color_hex(0x007aff);
	palette->screen_bg = palette->dark ? lv_color_black() : lv_color_white();
	palette->surface_pressed = lv_color_hex(0x888888);
	palette->text_primary = palette->dark ? lv_color_white() : lv_color_black();
	lv_style_init(&theme_style_screen);
	lv_style_set_bg_color(&theme_style_screen, palette->screen_bg);
	gui_config_t cfg = {.screen_width = width, .screen_height = height, .padding = ui_px(12), .top_bar_height = ui_px(30)};
	lv_obj_t *origin = lv_screen_active(), *other = lv_obj_create(NULL);
	player_button = lv_button_create(origin);
	player_icon = lv_image_create(player_button);
	toast_init(&cfg);
	quickpanel_init(&cfg);
	lv_obj_add_event_cb(np_star_btn, qp_lost, LV_EVENT_PRESS_LOST, NULL);
	qp_pointer = lv_indev_create();
	lv_indev_set_type(qp_pointer, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(qp_pointer, qp_read);
	represented.status = AUDIO_STATUS_PLAYING;
	represented.progress_current_secs = 43.25;
	represented.progress_total_secs = 240;
	snprintf(represented.metadata.title, sizeof(represented.metadata.title), "Synthetic represented track");
	qp_select("nd:cached-favorite");
	int requests = atomic_load(&calls);
	qp_show();
	assert(!lv_obj_is_hidden(np_star_btn) && lv_image_get_src(np_star_icon) == &icon_star_filled);
	qp_star_center();
	poll_cb(NULL);
	assert(atomic_load(&calls) == requests);
	qp_select("nd:panel-track");
	assert(lv_image_get_src(np_star_icon) == &icon_star && lv_obj_get_style_opa(np_star_icon, 0) == LV_OPA_40);
	device_state_t before = represented;
	unsigned full_refreshes = fixture_full_refreshes, notices = fixture_notices;
	qp_tap();
	stars_wait();
	assert(atomic_load(&star_writes) == 1 && !strcmp(written_ids[0], "panel-track") && written_kinds[0] == ND_TRACKS && written_state[0]);
	assert(lv_image_get_src(np_star_icon) == &icon_star_filled && lv_image_get_src(player_icon) == &icon_star_filled);
	assert(fixture_notices == notices && lv_obj_is_hidden(toast_veil));
	qp_cache_contains("panel-track", true);
	qp_assert_unchanged(before, full_refreshes);
	qp_tap();
	stars_wait();
	qp_cache_contains("panel-track", false);
	assert(!written_state[1] && lv_image_get_src(np_star_icon) == &icon_star && lv_image_get_src(player_icon) == &icon_star);
	// A full-player action is observed by the panel's normal visible poll.
	assert(navidromestar_player_toggle(represented.current_file, paint_player));
	stars_wait();
	poll_cb(NULL);
	assert(lv_image_get_src(np_star_icon) == &icon_star_filled);
	qp_assert_unchanged(before, full_refreshes);
	// A saved clear annotation is fully opaque; an album star never fills its song.
	qp_select("nd:known-clear");
	assert(lv_image_get_src(np_star_icon) == &icon_star && lv_obj_get_style_opa(np_star_icon, 0) == LV_OPA_COVER);
	nd_star_request_t album = {.kind = ND_ALBUMS, .action = ND_STAR_ADD};
	snprintf(album.id, sizeof(album.id), "targeted-album");
	assert(navidrome_star_submit(&album, NULL, NULL));
	stars_wait();
	assert(navidrome_star_state(ND_ALBUMS, album.id) == ND_STAR_SET);
	requests = atomic_load(&calls);
	qp_select("nd:album-member");
	assert(lv_image_get_src(np_star_icon) == &icon_star && atomic_load(&calls) == requests);
	qp_select("nd:panel-track");
	qp_start_blocked();
	refresh_favorite(&represented);
	assert(lv_obj_has_state(np_star_btn, LV_STATE_DISABLED));
	requests = atomic_load(&calls);
	qp_tap();
	assert(atomic_load(&calls) == requests);
	qp_release();
	refresh_favorite(&represented);
	assert(!lv_obj_has_state(np_star_btn, LV_STATE_DISABLED));
	// An authoritative represented preparation reference acts independently of audio.
	qp_select("nd:preparing-track");
	qp_tap();
	stars_wait();
	assert(!strcmp(written_ids[written_count - 1], "preparing-track"));
	qp_select("nd:missing");
	assert(lv_obj_has_state(np_star_btn, LV_STATE_DISABLED));
	requests = atomic_load(&calls);
	qp_tap();
	assert(atomic_load(&calls) == requests && !fixture_local_writes);
	qp_select("nd:panel-track");
	qp_drag(false, false);
	qp_tap();
	stars_wait();
	qp_drag(false, true);
	qp_tap();
	stars_wait();
	qp_drag(true, false);
	qp_show();
	qp_tap();
	stars_wait();
	// A canceled topbar-driven opening also retires its tentative visit.
	quickpanel_close();
	qp_settle();
	uint64_t previous_visit = favorite_visit;
	quickpanel_drag_begin();
	quickpanel_drag_update(ui_px(20));
	lv_tick_inc(1000);
	quickpanel_drag_end();
	qp_settle();
	assert(!panel_open && favorite_visit != previous_visit);
	qp_show();
	// Expand/collapse from the star: event bubbling follows the production cards.
	quickpanel_move_hidden(QP_BTN_DLNA, 0);
	quickpanel_move_in_use(QP_BTN_DLNA, 8);
	assert(expand_full > 0);
	lv_point_t p = qp_star_center();
	requests = atomic_load(&calls);
	qp_step(p.x, p.y, true);
	qp_step(p.x, p.y + ui_px(18), true);
	qp_step(p.x, p.y + ui_px(18), false);
	qp_settle();
	assert(atomic_load(&calls) == requests && press_dragged);
	expand_to(expand_full, false);
	lv_obj_update_layout(lv_layer_top());
	assert(lv_obj_is_hidden(np_card));
	lv_area_t handle;
	lv_obj_get_coords(expand_handle, &handle);
	p = (lv_point_t){(handle.x1 + handle.x2) / 2, (handle.y1 + handle.y2) / 2};
	qp_step(p.x, p.y, true);
	for (int j = 1; j <= 10; j++)
		qp_step(p.x, p.y - ui_px(120) * j / 10, true);
	qp_step(p.x, p.y - ui_px(120), false);
	qp_settle();
	assert(panel_open && expand_h == 0 && !lv_obj_is_hidden(np_card) && atomic_load(&calls) == requests);
	// Rejected and uncertain server replies retain shared semantics.
	atomic_store(&fixture_star_reply, 1);
	notices = fixture_notices;
	qp_tap();
	stars_wait();
	assert(fixture_notices == notices + 1);
	atomic_store(&fixture_star_reply, 2);
	qp_tap();
	stars_wait();
	assert(navidrome_star_state(ND_TRACKS, "panel-track") == ND_STAR_UNCERTAIN);
	requests = atomic_load(&calls);
	poll_cb(NULL);
	assert(atomic_load(&calls) == requests);
	atomic_store(&fixture_star_reply, 0);
	int writes = atomic_load(&star_writes);
	qp_tap();
	stars_wait();
	assert(atomic_load(&star_writes) == writes); // deliberate recheck, no blind toggle
	// Close/reopen with identical screen/track rejects old visit error ownership.
	atomic_store(&fixture_star_reply, 1);
	qp_start_blocked();
	quickpanel_close();
	notices = fixture_notices;
	unsigned refreshes = fixture_favorite_refreshes;
	qp_show();
	qp_release();
	assert(fixture_notices == notices && fixture_favorite_refreshes == refreshes);
	// Same protection applies before the closing animation has finished.
	qp_start_blocked();
	quickpanel_close();
	notices = fixture_notices;
	qp_release();
	assert(fixture_notices == notices);
	qp_settle();
	qp_show();
	qp_start_blocked();
	qp_select("nd:another-track");
	notices = fixture_notices;
	qp_release();
	assert(fixture_notices == notices && lv_obj_get_style_opa(np_star_icon, 0) == LV_OPA_40);
	qp_select("nd:panel-track");
	qp_start_blocked();
	lv_screen_load(other);
	notices = fixture_notices;
	assert(!panel_open);
	lv_screen_load(origin);
	qp_show();
	qp_release();
	assert(fixture_notices == notices);
	atomic_store(&fixture_star_reply, 0);
	qp_select("nd:closed-success");
	qp_start_blocked();
	quickpanel_close();
	qp_show();
	notices = fixture_notices;
	qp_release();
	poll_cb(NULL);
	assert(fixture_notices == notices && lv_image_get_src(np_star_icon) == &icon_star_filled);
	qp_cache_contains("closed-success", true);
	// Read-only/cache errors keep confirmed server state; explicit retry is local only.
	qp_select("nd:readonly-track");
	atomic_store(&fixture_readonly_cache, true);
	notices = fixture_notices;
	qp_tap();
	stars_wait();
	assert(fixture_notices == notices + 1 && lv_image_get_src(np_star_icon) == &icon_star_filled);
	atomic_store(&fixture_readonly_cache, false);
	writes = atomic_load(&star_writes);
	qp_tap();
	stars_wait();
	assert(atomic_load(&star_writes) == writes);
	qp_cache_contains("readonly-track", true);
	qp_select("nd:cache-failure");
	atomic_store(&fixture_commit_failure, true);
	notices = fixture_notices;
	qp_tap();
	stars_wait();
	assert(fixture_notices == notices + 1 && lv_image_get_src(np_star_icon) == &icon_star_filled);
	atomic_store(&fixture_commit_failure, false);
	writes = atomic_load(&star_writes);
	qp_tap();
	stars_wait();
	assert(atomic_load(&star_writes) == writes);
	qp_cache_contains("cache-failure", true);
	// No action allocation or rejected-post result survives its retired owner.
	notices = fixture_notices;
	requests = atomic_load(&calls);
	atomic_store(&fixture_alloc_failure, true);
	star_cb(NULL);
	assert(!atomic_load(&fixture_alloc_failure) && fixture_notices == notices + 1 && atomic_load(&calls) == requests);
	atomic_store(&fixture_reject_posts, true);
	qp_tap();
	wait_idle();
	quickpanel_close();
	atomic_store(&fixture_reject_posts, false);
	notices = fixture_notices;
	navidrome_star_poll();
	assert(!navidrome_star_busy() && fixture_notices == notices);
	qp_show();
	// Profile/card/credential changes invalidate even already queued GUI delivery.
	for (int invalidation = 0; invalidation < 5; invalidation++) {
		char name[64];
		snprintf(name, sizeof(name), "nd:lifetime-%d", invalidation);
		qp_select(name);
		qp_tap();
		wait_idle();
		notices = fixture_notices;
		unsigned owner_refreshes = fixture_favorite_refreshes;
		if (invalidation == 0)
			navidrome_set_root(NULL);
		if (invalidation == 1)
			navidrome_lock();
		if (invalidation == 2) {
			assert(navidrome_update_password("http://fixture/base", "fixture user", "replacement-synthetic"));
			wait_idle();
		}
		if (invalidation == 3) {
			assert(navidrome_forget());
			wait_idle();
		}
		if (invalidation == 4) {
			assert(navidrome_save("http://replacement/base", "fixture user", server_password, "vault1"));
			wait_idle();
		}
		drain();
		assert(fixture_notices == notices && fixture_favorite_refreshes == owner_refreshes);
		navidrome_set_root(dir);
		if (invalidation == 1)
			assert(navidrome_unlock("vault1"));
		else
			assert(navidrome_save("http://fixture/base", "fixture user", server_password, "vault1"));
		qp_wait_connected();
	}
	// Offline/busy eligibility reports without implicit reconnect or local writes.
	qp_select("nd:offline-track");
	navidrome_test_online(0);
	navidrome_poll_network();
	notices = fixture_notices;
	requests = atomic_load(&calls);
	qp_tap();
	stars_wait();
	assert(fixture_notices > notices && atomic_load(&calls) == requests);
	navidrome_test_online(1);
	assert(navidrome_reconnect());
	qp_wait_connected();
	nd_context_t context;
	assert(navidrome_context(&context, false));
	assert(navidrome_external_begin(&context, true));
	notices = fixture_notices;
	requests = atomic_load(&calls);
	qp_tap();
	assert(fixture_notices == notices + 1 && atomic_load(&calls) == requests);
	navidrome_external_end();
	nd_wipe(&context, sizeof(context));
	navidrome_lock();
	notices = fixture_notices;
	requests = atomic_load(&calls);
	qp_tap();
	assert(fixture_notices == notices + 1 && atomic_load(&calls) == requests);
	assert(navidrome_unlock("vault1"));
	qp_wait_connected();
	// Other sources recover normal opacity/enablement and retain their own semantics.
	qp_select("/card/local.flac");
	refresh_favorite(&represented);
	assert(!lv_obj_is_hidden(np_star_btn) && !lv_obj_has_state(np_star_btn, LV_STATE_DISABLED) && lv_obj_get_style_opa(np_star_icon, 0) == LV_OPA_COVER);
	qp_tap();
	assert(fixture_local_writes == 1 && fixture_local_fav);
	fixture_radio = represented.live = true;
	represented.current_file[0] = 0;
	refresh_now_playing_card();
	qp_tap();
	assert(fixture_radio_writes == 1 && fixture_radio_fav);
	fixture_radio = represented.live = false;
	fixture_podcast = true;
	snprintf(represented.current_file, sizeof(represented.current_file), "/card/episode");
	refresh_now_playing_card();
	qp_tap();
	assert(fixture_follow_writes == 1 && fixture_followed);
	fixture_podcast = false;
	fixture_book = true;
	refresh_now_playing_card();
	assert(lv_obj_is_hidden(np_star_btn));
	fixture_book = false;
	qp_select("nd:teardown");
	qp_start_blocked();
	unsigned toast_owner = toast_busy_owned("unrelated-work");
	notices = fixture_notices;
	lv_obj_delete(veil);
	assert(!panel && !poll_timer);
	qp_release();
	assert(navidrome_star_state(ND_TRACKS, "teardown") == ND_STAR_SET);
	assert(fixture_notices == notices && toast_busy_showing() && toast_revision() == toast_owner);
	toast_busy_end_owned(toast_owner);
	navidrome_reset();
	drain();
	lv_indev_delete(qp_pointer);
	lv_display_delete(display);
	lv_deinit();
	free(buffer);
	char path[1200];
	for (const char **part = (const char *[]){"/.local/navidrome/.cache", "/.local/navidrome", "/.local", NULL}; *part; part++) {
		snprintf(path, sizeof(path), "%s%s", dir, *part);
		assert(!rmdir(path));
	}
	assert(!chdir("/") && !rmdir(dir));
	printf("Production pull-down star %dx%d %s: exact-ID/state/cache, gestures, visit/source/profile/card lifetimes and failure cleanup passed\n", width, height, argv[3]);
	return 0;
}

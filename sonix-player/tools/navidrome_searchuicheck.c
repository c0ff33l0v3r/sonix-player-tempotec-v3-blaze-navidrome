// Reuse the existing real-LVGL service/GUI-post harness and large snapshot.
#define NAVIDROME_BROWSE_FIXTURE_ONLY 1
#include "navidrome_browsecheck.c"
#undef NAVIDROME_BROWSE_FIXTURE_ONLY
#include "src/gui/streaming/navidromesearch.c"

static atomic_int fail_search_allocation;
void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size) {
	if (size == sizeof(search_job_t) && atomic_exchange(&fail_search_allocation, 0))
		return NULL;
	return __real_malloc(size);
}
lv_font_t font_ui_16, font_ui_24;
static unsigned remote_selections;
static nd_query_t remote_query;
bool navidromeplay_search(const nd_library_t *library, const nd_context_t *identity, const nd_query_t *query, const char *id, void *origin) {
	assert(library->card_epoch && navidrome_search_current(identity, false) && query->kind == ND_TRACKS && *id && origin == lv_screen_active());
	remote_selections++;
	remote_query = *query;
	return true;
}
const lv_image_dsc_t icon_search = {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = 1, .h = 1, .stride = 4}, .data = fixture_pixel, .data_size = 4};
static const uint8_t clear_pixels[30 * 30 * 4];
const lv_image_dsc_t icon_clear = {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = 30, .h = 30, .stride = 120}, .data = clear_pixels, .data_size = sizeof(clear_pixels)};
struct keyboard_s {
	lv_obj_t *tray;
	bool visible;
};
keyboard_t *keyboard_create(lv_obj_t *parent, int w, int h, lv_obj_t *field, const lv_image_dsc_t *icon, const char *text, lv_event_cb_t cb, void *user) {
	(void)field;
	(void)icon;
	(void)text;
	(void)cb;
	(void)user;
	keyboard_t *kb = calloc(1, sizeof(*kb));
	assert(kb);
	kb->tray = lv_obj_create(parent);
	lv_obj_set_size(kb->tray, w, h);
	lv_obj_align(kb->tray, LV_ALIGN_BOTTOM_MID, 0, 0);
	return kb;
}
void keyboard_set_visible(keyboard_t *kb, bool visible) {
	kb->visible = visible;
	lv_obj_set_hidden(kb->tray, !visible);
}
bool keyboard_is_visible(keyboard_t *kb) { return kb->visible; }
void keyboard_style_caret(lv_obj_t *field) { (void)field; }
bool switcher_in_history(lv_obj_t *screen) {
	(void)screen;
	return false;
}
static void search_wait(int level) {
	for (int i = 0; i < 10000; i++) {
		drain();
		lv_obj_update_layout(search_pages[level].screen);
		if (!search_pages[level].loading && search_pages[level].populated)
			return;
		usleep(1000);
	}
	assert(!"search GUI timeout");
}
static void search_start(const char *text) {
	lv_textarea_set_text(search_field, text);
	search_accept(NULL);
	search_wait(0);
}
static void deliver_empty(nd_search_source_t source) {
	search_job_t *job = calloc(1, sizeof(*job));
	assert(job);
	job->generation = search_generation;
	job->serial = search_pages[0].serial;
	job->result.kind = search_pages[0].kind;
	job->result.source = source;
	navidrome_library_snapshot(&job->result.library);
	search_done(job);
	lv_obj_update_layout(search_pages[0].screen);
}
static int first_track(int level) {
	for (int i = 0; i < search_pages[level].data.count; i++)
		if (*search_pages[level].data.rows[i].id)
			return i;
	assert(!"no track row");
	return 0;
}
static lv_indev_data_t search_touch;
static void search_touch_read(lv_indev_t *input, lv_indev_data_t *data) {
	(void)input;
	*data = search_touch;
}
static void search_tap(lv_display_t *display, lv_obj_t *row) {
	lv_obj_update_layout(lv_screen_active());
	lv_area_t area;
	lv_obj_get_coords(row, &area);
	search_touch.point = (lv_point_t){.x = (area.x1 + area.x2) / 2, .y = (area.y1 + area.y2) / 2};
	assert(search_touch.point.y >= 0 && search_touch.point.y < lv_display_get_vertical_resolution(display));
	lv_indev_t *pointer = lv_indev_create();
	lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
	lv_indev_set_display(pointer, display);
	lv_indev_set_read_cb(pointer, search_touch_read);
	search_touch.state = LV_INDEV_STATE_PRESSED;
	lv_indev_read(pointer);
	search_touch.state = LV_INDEV_STATE_RELEASED;
	lv_indev_read(pointer);
	lv_indev_delete(pointer);
}
int main(int argc, char **argv) {
	// Search must ignore every independently changed browsing preference.
	config_set_int("navidrome", "sort_albums", ND_ORDER_YEAR_DESC);
	config_set_int("navidrome", "sort_tracks", ND_ORDER_ALBUM_ARTIST_DESC);
	config_set_int("navidrome", "sort_artists", ND_ORDER_NAME_DESC);
	config_set_int("navidrome", "sort_genres", ND_ORDER_NAME_DESC);
	config_set_int("navidrome", "sort_favorite_albums", ND_ORDER_YEAR);
	config_set_int("navidrome", "sort_favorite_tracks", ND_ORDER_ARTIST_DESC);
	alarm(60);
	int width = argc > 1 ? atoi(argv[1]) : 320;
	char dir[] = "/tmp/sonix-nd-search-ui-XXXXXX";
	assert(mkdtemp(dir) && !chdir(dir));
	navidrome_init();
	navidrome_set_root(dir);
	assert(navidrome_save("http://fixture/base", "fixture user", server_password, "vault1"));
	wait_idle();
	nd_library_t library;
	navidrome_library_snapshot(&library);
	seed(&library);
	navidrome_set_root(dir);
	for (int i = 0; i < 10000; i++) {
		navidrome_library_snapshot(&library);
		if (!library.loading)
			break;
		usleep(1000);
	}
	assert(library.ready);
	navidrome_test_online(0);
	navidrome_poll_network();
	lv_init();
	ui_panel_width = width;
	font_ui_28 = font_ui_24_bold = font_ui_24 = font_ui_14 = font_ui_16 = font_ui_18 = font_ui_20 = font_ui_64_bold = lv_font_unscii_8;
	lv_display_t *display = lv_display_create(width, width == 320 ? 480 : 720);
	navidrome_screen = lv_obj_create(NULL);
	gui_config_t cfg = {.screen_width = width, .screen_height = width == 320 ? 480 : 720, .padding = ui_px(12), .top_bar_height = ui_px(30)};
	navidromesearch_init(&cfg);
	int before = atomic_load(&calls);
	navidromesearch_open();
	assert(lv_screen_active() == search_pages[0].screen && keyboard_is_visible(search_keyboard));
	assert(atomic_load(&calls) == before);
	search_start("s");
	deliver_empty(ND_SEARCH_SERVER);
	assert(!strcmp(lv_label_get_text(search_pages[0].reason), "nd_search_more_text"));
	assert(search_pages[0].data.source == ND_SEARCH_SERVER && !search_pages[0].data.count);
	assert(!lv_obj_is_hidden(search_pages[0].reason));
	assert(lv_obj_get_y(search_pages[0].list) >= lv_obj_get_y(search_pages[0].reason) + lv_obj_get_height(search_pages[0].reason));
	deliver_empty(ND_SEARCH_SYNCED);
	assert(!strcmp(lv_label_get_text(search_pages[0].reason), "no_results"));
	search_start("Track");
	lv_obj_update_layout(search_pages[0].screen);
	assert(lv_obj_is_hidden(search_pages[0].reason));
	assert(lv_obj_is_hidden(search_pages[0].status));
	assert(lv_obj_get_y(search_pages[0].list) == search_pages[0].status_top);
	assert(lv_obj_get_scroll_bottom(search_clear) <= 0 && lv_obj_get_scroll_right(search_clear) <= 0);
	for (int k = 0; k < 3; k++) {
		assert(!lv_obj_get_style_border_width(search_pill[k], LV_PART_MAIN));
		assert(!lv_obj_get_style_shadow_width(search_pill[k], LV_PART_MAIN));
	}
	assert(search_pages[0].data.total == 17000 && !keyboard_is_visible(search_keyboard));
	assert(lv_obj_get_height(search_pages[0].rows[0]) == ui_px(100));
	assert(lv_obj_get_width(search_pages[0].images[0]) == ui_px(ND_ART_THUMB_SIZE));
	assert(lv_image_get_inner_align(search_pages[0].images[0]) == LV_IMAGE_ALIGN_CONTAIN);
	unsigned selections_before = playback_selections;
	search_tap(display, search_pages[0].rows[0]);
	assert(playback_selections == selections_before + 1);
	search_pages[0].data.source = ND_SEARCH_SERVER;
	search_tap(display, search_pages[0].rows[0]);
	assert(remote_selections == 1 && !strcmp(remote_query.search, "Track") && !*remote_query.filter);
	search_pages[0].data.source = ND_SEARCH_SYNCED;
	assert(search_totals[ND_TRACKS] == 17000 && search_totals[ND_ALBUMS] == 0);
	lv_obj_update_layout(search_pages[0].screen);
	assert(lv_obj_get_y(search_pages[0].list) + lv_obj_get_height(search_pages[0].list) <= (int)cfg.screen_height);
	search_keyboard_visible(true);
	lv_obj_update_layout(search_pages[0].screen);
	if (!lv_obj_is_hidden(search_pages[0].list))
		assert(lv_obj_get_y(search_pages[0].list) + lv_obj_get_height(search_pages[0].list) <= (int)cfg.screen_height - search_keyboard_height);
	search_keyboard_visible(false);
	lv_obj_scroll_to_y(search_pages[0].list, 5000 * search_pitch, LV_ANIM_OFF);
	lv_obj_update_layout(search_pages[0].screen);
	lv_obj_send_event(search_pages[0].list, LV_EVENT_SCROLL, NULL);
	lv_obj_scroll_to_y(search_pages[0].list, 16000 * search_pitch, LV_ANIM_OFF);
	lv_obj_update_layout(search_pages[0].screen);
	lv_obj_send_event(search_pages[0].list, LV_EVENT_SCROLL, NULL);
	search_wait(0);
	assert(search_pages[0].data.first > 15980);
	lv_obj_scroll_to_y(search_pages[0].list, 16995 * search_pitch, LV_ANIM_OFF);
	lv_obj_update_layout(search_pages[0].screen);
	lv_obj_send_event(search_pages[0].list, LV_EVENT_SCROLL, NULL);
	search_wait(0);
	assert(search_pages[0].data.first > 16980 && search_pages[0].data.first + search_pages[0].data.count == 17000);
	for (int i = 0; i < ND_WINDOW; i++)
		assert(search_pages[0].rows[i]);
	nd_library_t old_library = search_pages[0].data.library;
	seed(&library);
	navidrome_set_root(dir);
	for (int i = 0; i < 10000; i++) {
		navidrome_library_snapshot(&library);
		if (!library.loading)
			break;
		usleep(1000);
	}
	assert(library.ready && library.generation != old_library.generation);
	search_job_t *stale = calloc(1, sizeof(*stale));
	assert(stale);
	stale->generation = search_generation;
	stale->serial = search_pages[0].serial;
	stale->result = search_pages[0].data;
	stale->result.library = old_library;
	search_done(stale);
	search_wait(0);
	assert(search_pages[0].data.library.generation == library.generation);
	search_start("Album");
	lv_obj_send_event(search_pill[ND_ALBUMS], LV_EVENT_CLICKED, NULL);
	search_wait(0);
	assert(search_pages[0].kind == ND_ALBUMS && search_pages[0].data.total == 1700);
	lv_obj_send_event(search_pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	search_wait(1);
	assert(search_pages[1].data.kind == ND_TRACKS && search_pages[1].data.total == 10);
	selections_before = playback_selections;
	lv_obj_send_event(search_pages[1].rows[first_track(1)], LV_EVENT_CLICKED, NULL);
	assert(playback_selections == selections_before + 1);
	search_pages[1].data.source = ND_SEARCH_SERVER;
	lv_obj_send_event(search_pages[1].rows[first_track(1)], LV_EVENT_CLICKED, NULL);
	assert(remote_selections == 2 && !strcmp(remote_query.filter, search_pages[1].album) && !*remote_query.search);
	search_pages[1].data.source = ND_SEARCH_SYNCED;
	switch_screen(search_pages[0].screen);
	search_wait(0);
	assert(search_pages[0].kind == ND_ALBUMS);
	search_start("Artist");
	lv_obj_send_event(search_pill[ND_ARTISTS], LV_EVENT_CLICKED, NULL);
	search_wait(0);
	lv_obj_send_event(search_pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	search_wait(1);
	assert(search_pages[1].data.total == 1700);
	lv_obj_send_event(search_pages[1].rows[0], LV_EVENT_CLICKED, NULL);
	search_wait(2);
	assert(search_pages[2].data.total == 10);
	selections_before = playback_selections;
	lv_obj_send_event(search_pages[2].rows[first_track(2)], LV_EVENT_CLICKED, NULL);
	assert(playback_selections == selections_before + 1);
	switch_screen(search_pages[0].screen);
	search_wait(0);
	dragging = true;
	unsigned prior = search_generation;
	search_accept(NULL);
	assert(search_generation == prior);
	dragging = false;
	lv_textarea_set_text(search_field, "Track");
	search_accept(NULL);
	lv_textarea_set_text(search_field, "No such name");
	search_accept(NULL);
	search_wait(0);
	assert(!search_pages[0].data.count && !strcmp(search_query, "No such name"));
	assert(atomic_load(&calls) == before);
	atomic_store(&fail_search_allocation, 1);
	lv_textarea_set_text(search_field, "Track");
	search_accept(NULL);
	for (int i = 0; i < 10000 && search_pages[0].loading; i++) {
		drain();
		search_tick(search_timer);
		usleep(1000);
	}
	assert(!search_pages[0].loading && search_pages[0].data.error && !strcmp(search_pages[0].data.error, "nd_json_memory"));
	navidrome_lock();
	navidromesearch_refresh();
	assert(lv_screen_active() == navidrome_screen);
	assert(!*lv_textarea_get_text(search_field));
	// A historical Search page must check credentials before repainting.
	switch_screen(search_pages[0].screen);
	assert(lv_screen_active() == navidrome_screen && !search_pages[0].populated);
	for (int i = 0; i < ND_WINDOW; i++)
		assert(lv_obj_is_hidden(search_pages[0].rows[i]));
	puts("Real LVGL search: bounded large-list windows, category/child navigation, keyboard geometry, stale-query/lock/gesture guards and zero offline HTTP: passed");
	navidrome_shutdown();
	lv_display_delete(display);
	return 0;
}

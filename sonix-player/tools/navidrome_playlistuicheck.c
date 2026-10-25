#define NAVIDROME_PLAYLIST_UI
#define __wrap_http_request fixture_ping_request
#include "navidrome_browsecheck.c"
#undef __wrap_http_request
#include "src/gui/streaming/navidromeplaylists.c"
lv_font_t font_ui_16, font_ui_24;
const lv_image_dsc_t icon_list_music = FIXTURE_ICON;
const lv_image_dsc_t icon_plus = FIXTURE_ICON, icon_import = FIXTURE_ICON, icon_refresh = FIXTURE_ICON, icon_trash = FIXTURE_ICON;
static struct {
	char id[ND_ID_MAX];
	int size;
	bool pending;
} fixture_art[ND_ART_SLOT_COUNT];
static bool fixture_art_ready;
static unsigned fixture_art_requests;
static unsigned fixture_art_releases;
void navidromeart_release(int slot) {
	assert(slot >= 0 && slot < ND_ART_SLOT_COUNT);
	fixture_art_releases++;
	memset(&fixture_art[slot], 0, sizeof(fixture_art[slot]));
}
void navidromeart_request(int slot, const nd_library_t *library, const char *id, int size) {
	assert(slot >= 0 && slot < ND_ART_SLOT_COUNT && *library->scope && *library->path);
	assert(slot >= ND_ART_ALBUM_SLOT || size == ui_px(ND_ART_THUMB_SIZE));
	snprintf(fixture_art[slot].id, sizeof(fixture_art[slot].id), "%s", id);
	fixture_art[slot].size = size;
	fixture_art[slot].pending = true;
	fixture_art_requests++;
}
bool navidromeart_take(int slot, cover_image_t *out, bool *finished) {
	*finished = fixture_art_ready || !fixture_art[slot].pending;
	if (!fixture_art_ready || !fixture_art[slot].pending) return false;
	int size = fixture_art[slot].size;
	out->pixels = calloc((size_t)size * size, 4);
	assert(out->pixels);
	for (int i = 0; i < size * size; i++) {
		out->pixels[i * 4 + 2] = 255;
		out->pixels[i * 4 + 3] = 255;
	}
	out->dsc = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = size, .h = size, .stride = size * 4}, .data = out->pixels, .data_size = (uint32_t)(size * size * 4)};
	fixture_art[slot].pending = false;
	return true;
}
static void assert_cover_rendered(lv_obj_t *image) {
	lv_obj_update_layout(image);
	lv_draw_buf_t *snapshot = lv_snapshot_take(image, LV_COLOR_FORMAT_ARGB8888);
	assert(snapshot);
	int red = 0;
	for (unsigned y = 0; y < snapshot->header.h; y++)
		for (unsigned x = 0; x < snapshot->header.w; x++) {
			const unsigned char *px = snapshot->data + y * snapshot->header.stride + x * 4;
			red += px[2] > 200 && px[1] < 30 && px[0] < 30 && px[3] > 200;
		}
	int expected = ui_px(ND_ART_THUMB_SIZE) * ui_px(ND_ART_THUMB_SIZE);
	fprintf(stderr, "cover snapshot: drawn_pixels=%d expected=%d scale=%d\n", red, expected, (int)lv_image_get_scale_x(image));
	lv_draw_buf_destroy(snapshot);
	assert(red >= expected * 95 / 100);
	assert(lv_image_get_scale_x(image) == LV_SCALE_NONE);
}
static atomic_bool fixture_delay_import, fixture_import_entered, fixture_finish_import;
bool __wrap_http_request(const char *url, const http_req_t *req, char **out, size_t *size, size_t limit, int timeout, int *status) {
	if (!strstr(url, "/getPlaylist"))
		return fixture_ping_request(url, req, out, size, limit, timeout, status);
	assert(limit == ND_PL_BYTES + 1 && req->sensitive_url && !req->method);
	atomic_fetch_add(&calls, 1);
	const char *body;
	if (strstr(url, "/getPlaylists."))
		body = "{\"subsonic-response\":{\"status\":\"ok\",\"playlists\":{\"playlist\":[{\"id\":\"fixture-empty\",\"name\":\"Empty remote\"},{\"id\":\"fixture-art\",\"name\":\"Artwork remote\"},{\"id\":\"fixture-delayed\",\"name\":\"Delayed remote\"}]}}}";
	else if (strstr(url, "id=fixture-art"))
		body = "{\"subsonic-response\":{\"status\":\"ok\",\"playlist\":{\"id\":\"fixture-art\",\"name\":\"Artwork remote\",\"songCount\":2,\"entry\":[{\"id\":\"remote-art-track\",\"title\":\"With art\",\"coverArt\":\"remote-cover\"},{\"id\":\"remote-no-art\",\"title\":\"No art\"}]}}}";
	else if (strstr(url, "id=fixture-delayed")) {
		atomic_store(&fixture_import_entered, true);
		while (atomic_load(&fixture_delay_import) && !atomic_load(&fixture_finish_import)) usleep(1000);
		body = "{\"subsonic-response\":{\"status\":\"ok\",\"playlist\":{\"id\":\"fixture-delayed\",\"name\":\"Delayed remote\",\"songCount\":0,\"entry\":[]}}}";
	} else {
		assert(strstr(url, "id=fixture-empty"));
		body = "{\"subsonic-response\":{\"status\":\"ok\",\"playlist\":{\"id\":\"fixture-empty\",\"name\":\"Empty remote\",\"songCount\":0,\"entry\":[]}}}";
	}
	*out = strdup(body);
	*size = strlen(body);
	*status = 200;
	return true;
}
void confirm_close(void) {}
static void (*fixture_confirm)(void *);
static void *fixture_confirm_user;
bool switcher_in_history(lv_obj_t *screen) {
	(void)screen;
	return true;
}
keyboard_t *keyboard_create(lv_obj_t *parent, int width, int height, lv_obj_t *field, const lv_image_dsc_t *icon, const char *text, lv_event_cb_t cb, void *user) {
	(void)parent;
	(void)width;
	(void)height;
	(void)field;
	(void)icon;
	(void)text;
	(void)cb;
	(void)user;
	return NULL;
}
void keyboard_style_caret(lv_obj_t *field) { (void)field; }
void keyboard_reset(keyboard_t *kb) { (void)kb; }
void confirm_show(const char *title, const char *message, const char *ok, void (*cb)(void *), void *user) {
	(void)title;
	(void)message;
	(void)ok;
	fixture_confirm = cb;
	fixture_confirm_user = user;
}
static lv_indev_data_t fixture_touch;
static void fixture_touch_read(lv_indev_t *input, lv_indev_data_t *data) {
	(void)input;
	*data = fixture_touch;
}
static void pl_wait(int page) {
	for (int i = 0; i < 10000; i++) {
		drain();
		lv_obj_update_layout(pl_pages[page].screen);
		if (!navidrome_playlists_busy() && !pl_pages[page].loading)
			return;
		usleep(1000);
	}
	assert(!"playlist UI timeout");
}
static void selection_wait(void) {
	for (int i = 0; i < 10000; i++) {
		drain();
		if (!navidrome_playlists_busy() && !pages[0].select_busy && pages[0].selecting) {
			window_wait(0);
			return;
		}
		usleep(1000);
	}
	assert(!"selection timeout");
}
static void library_wait(nd_library_t *lib) {
	for (int i = 0; i < 10000; i++) {
		navidrome_library_snapshot(lib);
		if (!lib->loading)
			return;
		usleep(1000);
	}
	assert(!"library timeout");
}
static void pick_wait(int level) {
	for (int i = 0; i < 10000; i++) {
		drain();
		if (!navidrome_playlists_busy() && !pages[level].select_busy) {
			window_wait(level);
			return;
		}
		usleep(1000);
	}
	assert(!"picker selection timeout");
}
static int remote_index(const char *id) {
	for (int i = 0; i < pl_pages[3].count; i++)
		if (!strcmp(pl_pages[3].playlists[i].remote, id)) return i;
	assert(!"remote catalogue ID missing");
	return -1;
}
int main(int argc, char **argv) {
	ui_panel_width = argc > 1 ? atoi(argv[1]) : 320;
	assert(ui_panel_width == 320 || ui_panel_width == 480);
	int panel_height = ui_panel_width == 320 ? 480 : 800;
	alarm(60);
	char dir[] = "/tmp/sonix-playlist-ui.XXXXXX";
	assert(mkdtemp(dir) && !chdir(dir));
	navidrome_init();
	navidrome_set_root(dir);
	assert(navidrome_save("http://fixture/base", "fixture user", server_password, "vault1"));
	wait_idle();
	nd_library_t lib;
	navidrome_library_snapshot(&lib);
	seed(&lib);
	nd_store_t art_seed;
	assert(nd_store_open(&art_seed, lib.path, lib.scope, true, false));
	assert(sqlite3_exec(art_seed.db, "UPDATE entity SET cover='fixture-browse-cover' WHERE (kind=0 AND id='album-00000') OR (kind=1 AND id='song-00000')", NULL, NULL, NULL) == SQLITE_OK);
	nd_store_close(&art_seed);
	navidrome_set_root(dir);
	library_wait(&lib);
	assert(lib.ready);
	lv_init();
	font_ui_28 = font_ui_24_bold = font_ui_14 = font_ui_16 = font_ui_18 = font_ui_20 = font_ui_24 = lv_font_unscii_8;
	lv_display_t *display = lv_display_create(ui_panel_width, panel_height);
	lv_style_init(&theme_style_icon);
	lv_style_set_image_recolor_opa(&theme_style_icon, LV_OPA_COVER);
	navidrome_screen = lv_obj_create(NULL);
	gui_config_t cfg = {.screen_width = (unsigned)ui_panel_width, .screen_height = (unsigned)panel_height, .padding = 8, .top_bar_height = 20};
	navidromealbum_set_enabled(false); // Retained thumbnail-layout coverage.
	navidromebrowse_init(&cfg);
	navidromeplaylists_init(&cfg);
	int requests = atomic_load(&calls);
	navidromeplaylists_open();
	pl_wait(0);
	assert(pl_active(0) && pl_pages[0].total == 0);
	assert(!lv_obj_is_hidden(pl_pages[0].new_row) && !lv_obj_is_hidden(pl_pages[0].empty));
	assert(!lv_obj_is_hidden(pl_pages[0].buttons[0]) && lv_obj_is_hidden(pl_pages[0].buttons[1]));
	assert(lv_obj_get_height(pl_pages[0].new_row) == ui_px(88));
	assert(pl_pages[0].pitch == ui_px(88) + ui_px(8));
	assert(lv_obj_get_style_radius(pl_pages[0].new_row, 0) == ui_px(12));
	assert(lv_obj_get_width(pl_pages[0].list) == (int)cfg.screen_width);
	assert(lv_obj_get_style_pad_left(pl_pages[0].list, 0) == cfg.padding);
	assert(lv_obj_get_y(pl_pages[0].list) == settingsrow_content_top(&cfg));
	lv_obj_send_event(pl_pages[0].new_row, LV_EVENT_CLICKED, NULL);
	assert(!lv_obj_is_hidden(pl_name_layer));
	assert(lv_obj_get_parent(pl_name_layer) == pl_pages[0].screen);
	assert(lv_obj_get_parent(pl_name_layer) != lv_layer_top());
	lv_obj_update_layout(pl_name_layer);
	lv_area_t heading_area, close_area;
	lv_obj_get_coords(lv_obj_get_child(pl_name_layer, 0), &heading_area);
	lv_obj_get_coords(lv_obj_get_child(pl_name_layer, -1), &close_area);
	assert(heading_area.x1 >= cfg.padding + ui_px(56) + ui_px(14));
	assert(heading_area.x2 < close_area.x1);
	assert(heading_area.y1 == cfg.top_bar_height + cfg.padding + ui_px(10));
	assert(lv_obj_get_height(pl_name_field) == ui_px(62));
	assert(lv_obj_get_style_text_font(pl_name_field, 0) == &font_ui_24);
	assert(lv_obj_has_state(pl_name_field, LV_STATE_FOCUSED));
	lv_textarea_set_text(pl_name_field, "Road playlist");
	pl_name_accept(NULL);
	pl_wait(1);
	assert(pl_active(1) && pl_pages[1].playlist.count == 0 && !strcmp(pl_pages[1].playlist.name, "Road playlist"));
	assert(!strcmp(lv_label_get_text(pl_pages[1].empty), "No tracks yet."));
	// The detail Add tracks flow selects an album and returns to this exact playlist.
	lv_obj_send_event(pl_pages[1].buttons[2], LV_EVENT_CLICKED, NULL);
	assert(fixture_menu_count == 2);
	fixture_menu[1].action(fixture_menu[1].user);
	window_wait(0);
	assert(pages[0].query.kind == ND_ALBUMS);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_LONG_PRESSED, NULL);
	selection_wait();
	assert(pages[0].selected == 1);
	lv_obj_send_event(pages[0].select_add, LV_EVENT_CLICKED, NULL);
	pl_wait(1);
	assert(pl_active(1) && pl_pages[1].playlist.count == 10);
	int64_t rev = pl_pages[1].playlist.revision;
	// Readding that album skips all tracks, without changing the revision.
	lv_obj_send_event(pl_pages[1].buttons[2], LV_EVENT_CLICKED, NULL);
	fixture_menu[1].action(fixture_menu[1].user);
	window_wait(0);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_LONG_PRESSED, NULL);
	selection_wait();
	lv_obj_send_event(pages[0].select_add, LV_EVENT_CLICKED, NULL);
	pl_wait(1);
	assert(pl_pages[1].playlist.count == 10 && pl_pages[1].playlist.revision == rev);
	// Detail selection removes only chosen occurrences in a confirmed local batch.
	lv_obj_send_event(pl_pages[1].rows[0], LV_EVENT_LONG_PRESSED, NULL);
	pl_wait(1);
	assert(pl_pages[1].selecting && pl_pages[1].selected == 1);
	lv_obj_send_event(pl_pages[1].rows[1], LV_EVENT_CLICKED, NULL);
	pl_wait(1);
	assert(pl_pages[1].selected == 2);
	lv_obj_send_event(pl_pages[1].buttons[1], LV_EVENT_CLICKED, NULL);
	assert(fixture_confirm);
	fixture_confirm(fixture_confirm_user);
	pl_wait(1);
	assert(pl_pages[1].playlist.count == 8 && !pl_pages[1].selecting);
	// Header exposes an explicit Upload choice, never an implicit write on icon tap.
	lv_obj_send_event(pl_pages[1].buttons[1], LV_EVENT_CLICKED, NULL);
	assert(fixture_menu_count == 1 && !strcmp(fixture_menu[0].label, "nd_pl_upload"));
	// No local edit sent HTTP.
	assert(atomic_load(&calls) == requests);
	lv_obj_send_event(pl_pages[1].buttons[0], LV_EVENT_CLICKED, NULL);
	assert(fixture_menu_count == 1 && !strcmp(fixture_menu[0].label, "rename"));
	fixture_menu[0].action(NULL);
	lv_textarea_set_text(pl_name_field, "Renamed locally");
	pl_name_accept(NULL);
	pl_wait(1);
	assert(!strcmp(pl_pages[1].playlist.name, "Renamed locally"));
	// The collection and destination picker share Music's New playlist row,
	// track counts, card typography and row menu without a second header plus.
	navidromeplaylists_open();
	pl_wait(0);
	assert(pl_pages[0].total == 1 && lv_obj_is_hidden(pl_pages[0].empty));
	assert(lv_obj_get_y(pl_pages[0].rows[0]) == pl_pages[0].offset);
	assert(lv_obj_get_style_text_font(pl_pages[0].names[0], 0) == &font_ui_24);
	assert(lv_obj_get_style_text_font(pl_pages[0].details[0], 0) == &font_ui_16);
	assert(!strcmp(lv_label_get_text(pl_pages[0].details[0]), "8 tracks · nd_pl_local"));
	assert(lv_image_get_src(lv_obj_get_child(pl_pages[0].rows[0], 0)) == &icon_list_music);
	lv_area_t name_area, menu_area;
	lv_obj_get_coords(pl_pages[0].names[0], &name_area);
	lv_obj_get_coords(pl_pages[0].menus[0], &menu_area);
	assert(name_area.x1 < name_area.x2 && name_area.x2 < menu_area.x1);
	lv_obj_send_event(pl_pages[0].menus[0], LV_EVENT_CLICKED, NULL);
	assert(fixture_menu_count == 2 && !strcmp(fixture_menu[0].label, "rename") && !strcmp(fixture_menu[1].label, "nd_pl_delete_local"));
	lv_obj_send_event(pl_pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	pl_wait(1);
	navidromeplaylists_pick(9000, lib.generation, pl_pages[1].screen);
	pl_wait(2);
	assert(pl_active(2) && !lv_obj_is_hidden(pl_pages[2].new_row));
	assert(lv_obj_is_hidden(pl_pages[2].buttons[0]));
	lv_obj_send_event(pl_pages[2].new_row, LV_EVENT_CLICKED, NULL);
	assert(!lv_obj_is_hidden(pl_name_layer));
	assert(pl_back() && lv_obj_is_hidden(pl_name_layer));
	switch_screen_return_to(pl_pages[1].screen);
	pl_wait(1);
	assert(atomic_load(&calls) == requests);
	// Only playlist Add tracks changes a track tap into selection. The mode
	// propagates through album drill-down, and normal category entry clears it.
	unsigned played = playback_selections;
	lv_obj_send_event(pl_pages[1].buttons[2], LV_EVENT_CLICKED, NULL);
	fixture_menu[0].action(fixture_menu[0].user);
	window_wait(0);
	assert(pages[0].picking && pages[0].query.kind == ND_TRACKS);
	fixture_art_ready = true;
	navidromebrowse_refresh();
	assert_cover_rendered(pages[0].images[0]);
	unsigned art_requests = fixture_art_requests, art_releases = fixture_art_releases;
	void *cover_pixels = pages[0].artwork[0].pixels;
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	pick_wait(0);
	assert(pages[0].selected == 1 && playback_selections == played);
	assert(fixture_art_requests == art_requests && fixture_art_releases == art_releases && pages[0].artwork[0].pixels == cover_pixels);
	lv_obj_send_event(pages[0].rows[1], LV_EVENT_CLICKED, NULL);
	pick_wait(0);
	assert(pages[0].selected == 2);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	pick_wait(0);
	assert(pages[0].selected == 1 && !pages[0].chosen[0]);
	lv_obj_send_event(pages[0].rows[1], LV_EVENT_CLICKED, NULL);
	pick_wait(0);
	assert(!pages[0].selecting && pages[0].selected == 0);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	pick_wait(0);
	assert(browse_selection_back());
	window_wait(0);
	assert(fixture_art_requests == art_requests && fixture_art_releases == art_releases && pages[0].artwork[0].pixels == cover_pixels);
	assert_cover_rendered(pages[0].images[0]);
	switch_screen_return_to(pl_pages[1].screen);
	pl_wait(1);
	lv_obj_send_event(pl_pages[1].buttons[2], LV_EVENT_CLICKED, NULL);
	fixture_menu[1].action(fixture_menu[1].user);
	window_wait(0);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	window_wait(1);
	assert(pages[1].picking && pages[1].query.kind == ND_TRACKS);
	fixture_art_ready = false;
	art_requests = fixture_art_requests;
	art_releases = fixture_art_releases;
	assert(fixture_art[0].pending);
	lv_obj_send_event(pages[1].rows[0], LV_EVENT_CLICKED, NULL);
	pick_wait(1);
	assert(pages[1].selected == 1 && playback_selections == played);
	assert(fixture_art_requests == art_requests && fixture_art_releases == art_releases && fixture_art[0].pending);
	assert(browse_selection_back());
	window_wait(1);
	assert(fixture_art_requests == art_requests && fixture_art_releases == art_releases && fixture_art[0].pending);
	fixture_art_ready = true;
	navidromebrowse_refresh();
	assert_cover_rendered(pages[1].images[0]);
	navidromebrowse_open(ND_TRACKS);
	window_wait(0);
	assert(!pages[0].picking);
	fixture_art_ready = true;
	navidromebrowse_refresh();
	assert_cover_rendered(pages[0].images[0]);
	// Retention must not keep a cover when the same row gets a new cover ID.
	art_requests = fixture_art_requests;
	assert(nd_store_open(&art_seed, lib.path, lib.scope, true, false));
	assert(sqlite3_exec(art_seed.db, "UPDATE entity SET cover='fixture-replacement-cover' WHERE kind=1 AND id='song-00000'", NULL, NULL, NULL) == SQLITE_OK);
	nd_store_close(&art_seed);
	submit_window(&pages[0]);
	window_wait(0);
	assert(fixture_art_requests == art_requests + 1 && !strcmp(fixture_art[0].id, "fixture-replacement-cover"));
	assert(!pages[0].artwork[0].pixels && lv_image_get_src(pages[0].images[0]) == &icon_music_note);
	navidromebrowse_refresh();
	assert_cover_rendered(pages[0].images[0]);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	assert(playback_selections == ++played);
	navidromebrowse_open(ND_ALBUMS);
	window_wait(0);
	navidromebrowse_refresh();
	assert_cover_rendered(pages[0].images[0]);
	art_requests = fixture_art_requests;
	art_releases = fixture_art_releases;
	cover_pixels = pages[0].artwork[0].pixels;
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_LONG_PRESSED, NULL);
	pick_wait(0);
	assert(pages[0].selected == 1);
	assert(browse_selection_back());
	window_wait(0);
	assert(fixture_art_requests == art_requests && fixture_art_releases == art_releases && pages[0].artwork[0].pixels == cover_pixels);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	window_wait(1);
	assert(!pages[1].picking);
	navidromebrowse_refresh();
	assert_cover_rendered(pages[1].images[0]);
	lv_obj_send_event(pages[1].rows[0], LV_EVENT_CLICKED, NULL);
	assert(playback_selections == ++played);
	switch_screen_return_to(pl_pages[1].screen);
	pl_wait(1);
	// Back cancels selection on this page, and gestures cannot select a row.
	dragging = true;
	lv_obj_send_event(pl_pages[1].rows[0], LV_EVENT_LONG_PRESSED, NULL);
	assert(!pl_pages[1].selecting);
	dragging = false;
	lv_obj_send_event(pl_pages[1].rows[0], LV_EVENT_LONG_PRESSED, NULL);
	pl_wait(1);
	assert(pl_back());
	pl_wait(1);
	assert(!pl_pages[1].selecting);
	// Exercise real LVGL pointer long-press and release, with the worker completing while held.
	lv_indev_t *pointer = lv_indev_create();
	lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
	lv_indev_set_display(pointer, display);
	lv_indev_set_read_cb(pointer, fixture_touch_read);
	lv_indev_set_long_press_time(pointer, 50);
	lv_obj_update_layout(pl_pages[1].screen);
	lv_area_t hit;
	lv_obj_get_coords(pl_pages[1].rows[0], &hit);
	fixture_touch.point = (lv_point_t){.x = hit.x1 + 10, .y = hit.y1 + 10};
	fixture_touch.state = LV_INDEV_STATE_PRESSED;
	lv_indev_read(pointer);
	lv_tick_inc(80);
	lv_indev_read(pointer);
	pl_wait(1);
	assert(pl_pages[1].selecting && pl_pages[1].selected == 1);
	fixture_touch.state = LV_INDEV_STATE_RELEASED;
	lv_indev_read(pointer);
	pl_wait(1);
	assert(pl_pages[1].selecting && pl_pages[1].selected == 1);
	assert(pl_back());
	pl_wait(1);
	lv_indev_delete(pointer);
	// A dropped GUI completion recovers through a local read without replaying a write.
	int calls_before = atomic_load(&calls);
	pl_pages[1].loading = true;
	pl_pages[1].submitted = lv_tick_get() - 3000;
	pl_tick(pl_timer);
	pl_wait(1);
	assert(pl_pages[1].playlist.count == 8 && atomic_load(&calls) == calls_before);
	// Five thousand local entries use the same fixed visible pool and reach their final row.
	nd_playlist_t saved = pl_pages[1].playlist, big;
	nd_playlist_store_t store;
	char store_path[1200];
	assert(nd_pl_path(&lib, store_path));
	assert(nd_pl_open(&store, store_path, lib.scope, true));
	int64_t big_id;
	assert(nd_pl_create(&store, "Large local", &big_id) && nd_pl_begin(&store, big_id, 1));
	int added = 0, skipped = 0;
	nd_row_t row = {.title = "Bulk track"};
	for (int i = 0; i < 5000; i++) {
		snprintf(row.id, sizeof(row.id), "bulk-%05d", i);
		assert(nd_pl_append(&store, big_id, &row, false, &added, &skipped));
	}
	assert(nd_pl_finish(&store, big_id, true) && nd_pl_sql(&store, "COMMIT") && nd_pl_get(&store, big_id, &big));
	nd_pl_close(&store);
	pl_open_detail(&big);
	pl_wait(1);
	assert(pl_pages[1].count == ND_WINDOW && pl_pages[1].total == 5000);
	lv_obj_scroll_to_y(pl_pages[1].list, 4998 * pl_pages[1].pitch, LV_ANIM_OFF);
	lv_obj_update_layout(pl_pages[1].screen);
	lv_obj_send_event(pl_pages[1].list, LV_EVENT_SCROLL, NULL);
	pl_wait(1);
	assert(pl_pages[1].first + pl_pages[1].count == 5000 && !strcmp(pl_pages[1].entries[pl_pages[1].count - 1].track.id, "bulk-04999"));
	assert(nd_pl_open(&store, store_path, lib.scope, true) && nd_pl_edit(&store, big_id, big.revision, NULL, true) && nd_pl_sql(&store, "COMMIT"));
	nd_pl_close(&store);
	// The leading New playlist row must not shift window ordinals or hide
	// the last playlist when the collection exceeds the recycled row pool.
	assert(nd_pl_open(&store, store_path, lib.scope, true) && nd_pl_sql(&store, "BEGIN IMMEDIATE"));
	for (int i = 0; i < 40; i++) {
		char name[40];
		int64_t window_id;
		snprintf(name, sizeof(name), "Window %02d", i);
		assert(nd_pl_create(&store, name, &window_id));
	}
	assert(nd_pl_sql(&store, "COMMIT"));
	nd_pl_close(&store);
	navidromeplaylists_open();
	pl_wait(0);
	assert(pl_pages[0].total == 41 && pl_pages[0].count == ND_WINDOW);
	lv_obj_scroll_to_y(pl_pages[0].list, pl_pages[0].offset + 40 * pl_pages[0].pitch, LV_ANIM_OFF);
	lv_obj_update_layout(pl_pages[0].screen);
	lv_obj_send_event(pl_pages[0].list, LV_EVENT_SCROLL, NULL);
	pl_wait(0);
	assert(pl_pages[0].first + pl_pages[0].count == 41);
	assert(!strcmp(pl_pages[0].playlists[pl_pages[0].count - 1].name, "Window 39"));
	assert(nd_pl_open(&store, store_path, lib.scope, true));
	assert(nd_pl_sql(&store, "DELETE FROM playlist WHERE name LIKE 'Window %'"));
	nd_pl_close(&store);
	pl_open_detail(&saved);
	pl_wait(1);
	// Real asynchronous catalogue/import jobs: returning from an imported
	// detail reloads the saved catalogue, with no discovery request or stuck busy label.
	navidromeplaylists_open();
	pl_wait(0);
	lv_obj_send_event(pl_pages[0].buttons[0], LV_EVENT_CLICKED, NULL);
	pl_wait(3);
	assert(pl_pages[3].count == 3);
	lv_obj_send_event(pl_pages[3].rows[remote_index("fixture-empty")], LV_EVENT_CLICKED, NULL);
	pl_wait(1);
	assert(pl_pages[1].playlist.count == 0 && !strcmp(lv_label_get_text(pl_pages[1].empty), "No tracks yet."));
	int after_import = atomic_load(&calls);
	switch_screen_return_to(pl_pages[3].screen);
	pl_wait(3);
	assert(pl_pages[3].count == 3 && !lv_obj_is_hidden(pl_pages[3].rows[0]));
	assert(strcmp(lv_label_get_text(pl_pages[3].status), "nd_working") && atomic_load(&calls) == after_import);
	lv_obj_send_event(pl_pages[3].rows[remote_index("fixture-art")], LV_EVENT_CLICKED, NULL);
	pl_wait(1);
	nd_playlist_t linked = pl_pages[1].playlist;
	assert(linked.count == 2 && *linked.remote);
	navidromeplaylists_open();
	pl_wait(0);
	int linked_index = -1;
	for (int i = 0; i < pl_pages[0].count; i++)
		if (pl_pages[0].playlists[i].id == linked.id) linked_index = i;
	assert(linked_index >= 0);
	lv_obj_send_event(pl_pages[0].menus[linked_index], LV_EVENT_CLICKED, NULL);
	assert(fixture_menu_count == 3 && !strcmp(fixture_menu[2].label, "nd_pl_save_copy"));
	lv_obj_send_event(pl_pages[0].rows[linked_index], LV_EVENT_CLICKED, NULL);
	pl_wait(1);
	assert(fixture_art_requests && !strcmp(fixture_art[0].id, "remote-cover"));
	assert(lv_obj_get_width(pl_pages[1].images[0]) == ui_px(ND_ART_THUMB_SIZE));
	assert(lv_image_get_inner_align(pl_pages[1].images[0]) == LV_IMAGE_ALIGN_CONTAIN);
	assert(lv_obj_get_width(lv_obj_get_parent(pl_pages[1].images[0])) == ui_px(ND_ART_THUMB_SIZE));
	assert(lv_obj_get_width(pl_pages[1].images[1]) == ui_px(ND_ART_THUMB_SIZE));
	lv_area_t art_area, track_area, text_area;
	lv_obj_get_coords(pl_pages[1].images[1], &art_area);
	lv_obj_get_coords(pl_pages[1].rows[1], &track_area);
	lv_obj_get_coords(pl_pages[1].names[1], &text_area);
	assert(art_area.y1 >= track_area.y1 && art_area.y2 <= track_area.y2);
	assert(art_area.x2 < text_area.x1);
	fixture_art_ready = true;
	navidromeplaylists_refresh();
	lv_obj_update_layout(pl_pages[1].screen);
	assert(lv_image_get_src(pl_pages[1].images[0]) == &pl_pages[1].artwork[0].dsc);
	assert(lv_obj_get_width(pl_pages[1].images[0]) == ui_px(ND_ART_THUMB_SIZE));
	assert_cover_rendered(pl_pages[1].images[0]);
	assert(lv_obj_get_style_image_recolor_opa(pl_pages[1].images[0], 0) == LV_OPA_TRANSP);
	assert(lv_image_get_src(pl_pages[1].images[1]) == &icon_music_note);
	art_requests = fixture_art_requests;
	art_releases = fixture_art_releases;
	cover_pixels = pl_pages[1].artwork[0].pixels;
	lv_obj_send_event(pl_pages[1].rows[0], LV_EVENT_LONG_PRESSED, NULL);
	pl_wait(1);
	lv_obj_send_event(pl_pages[1].rows[1], LV_EVENT_CLICKED, NULL);
	pl_wait(1);
	assert(pl_pages[1].selected == 2);
	assert(pl_back());
	pl_wait(1);
	assert(fixture_art_requests == art_requests && fixture_art_releases == art_releases && pl_pages[1].artwork[0].pixels == cover_pixels);
	assert_cover_rendered(pl_pages[1].images[0]);
	// Copy asks first; Cancel creates nothing. Accepting the unchanged name
	// makes a distinct unlinked copy, while a chosen name is saved atomically.
	lv_obj_send_event(pl_pages[1].buttons[0], LV_EVENT_CLICKED, NULL);
	assert(fixture_menu_count == 2 && !strcmp(fixture_menu[1].label, "nd_pl_save_copy"));
	fixture_menu[1].action(NULL);
	assert(!lv_obj_is_hidden(pl_name_layer) && pl_copying);
	assert(lv_obj_get_parent(pl_name_layer) == pl_pages[1].screen);
	assert(!strcmp(lv_textarea_get_text(pl_name_field), linked.name));
	assert(pl_back());
	assert(nd_pl_open(&store, store_path, lib.scope, false));
	nd_playlist_t list_rows[ND_WINDOW];
	int listed, total_before_copy;
	assert(nd_pl_list(&store, 0, list_rows, &listed, &total_before_copy) && total_before_copy == 3);
	nd_pl_close(&store);
	lv_obj_send_event(pl_pages[1].buttons[0], LV_EVENT_CLICKED, NULL);
	fixture_menu[1].action(NULL);
	pl_name_accept(NULL);
	pl_wait(1);
	assert(pl_pages[1].playlist.id != linked.id && !*pl_pages[1].playlist.remote);
	assert(!strcmp(pl_pages[1].playlist.name, linked.name) && pl_pages[1].playlist.count == 2);
	pl_open_detail(&linked);
	pl_wait(1);
	lv_obj_send_event(pl_pages[1].buttons[0], LV_EVENT_CLICKED, NULL);
	fixture_menu[1].action(NULL);
	lv_textarea_set_text(pl_name_field, "Named local copy");
	pl_name_accept(NULL);
	pl_wait(1);
	assert(!*pl_pages[1].playlist.remote && !strcmp(pl_pages[1].playlist.name, "Named local copy"));
	assert(nd_pl_open(&store, store_path, lib.scope, false));
	nd_playlist_t unchanged;
	assert(nd_pl_get(&store, linked.id, &unchanged) && !strcmp(unchanged.name, linked.name) && !strcmp(unchanged.remote, linked.remote));
	nd_pl_close(&store);
	assert(atomic_load(&calls) == after_import + 1);
	// Leaving during a delayed import defers only the local collection read;
	// the accepted import finishes without pulling the user back to its detail.
	switch_screen_return_to(pl_pages[3].screen);
	pl_wait(3);
	for (int i = 0; i < ND_WINDOW; i++) assert(!fixture_art[i].pending);
	atomic_store(&fixture_delay_import, true);
	lv_obj_send_event(pl_pages[3].rows[remote_index("fixture-delayed")], LV_EVENT_CLICKED, NULL);
	for (int i = 0; i < 10000 && !atomic_load(&fixture_import_entered); i++) usleep(1000);
	assert(atomic_load(&fixture_import_entered));
	switch_screen_return_to(pl_pages[0].screen);
	assert(pl_pages[0].reload && !pl_pages[0].loading);
	atomic_store(&fixture_finish_import, true);
	for (int i = 0; i < 10000; i++) {
		drain();
		pl_tick(pl_timer);
		if (!navidrome_playlists_busy() && !pl_pages[0].reload && !pl_pages[0].loading) break;
		usleep(1000);
	}
	pl_wait(0);
	assert(pl_active(0) && pl_pages[0].total == 6 && !lv_obj_is_hidden(pl_pages[0].rows[0]));
	assert(nd_pl_open(&store, store_path, lib.scope, true));
	sqlite3_stmt *cleanup;
	assert(nd_pl_prepare(&store, &cleanup, "DELETE FROM playlist WHERE id<>?"));
	sqlite3_bind_int64(cleanup, 1, saved.id);
	assert(sqlite3_step(cleanup) == SQLITE_DONE);
	sqlite3_finalize(cleanup);
	nd_pl_close(&store);
	pl_open_detail(&saved);
	pl_wait(1);
	// Local lists remain available without a library snapshot and offline.
	assert(unlink(lib.path) == 0);
	navidrome_set_root(dir);
	library_wait(&lib);
	assert(!lib.ready);
	navidromeplaylists_open();
	pl_wait(0);
	assert(pl_pages[0].total == 1);
	lv_obj_send_event(pl_pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	pl_wait(1);
	assert(pl_pages[1].playlist.count == 8);
	atomic_store(&mode, 6);
	assert(navidrome_reconnect());
	wait_idle();
	navidromeplaylists_open();
	pl_wait(0);
	assert(pl_pages[0].total == 1);
	// Read-only card blocks writes while retaining readable playlist contents.
	assert(chmod(dir, 0500) == 0);
	navidrome_set_root(dir);
	library_wait(&lib);
	assert(!lib.writable);
	navidromeplaylists_open();
	pl_wait(0);
	assert(pl_pages[0].total == 1);
	pl_name_show(0, false, NULL);
	lv_textarea_set_text(pl_name_field, "Cannot write");
	pl_name_accept(NULL);
	pl_wait(0);
	assert(pl_pages[0].total == 1);
	assert(chmod(dir, 0700) == 0);
	navidrome_lock();
	navidromeplaylists_refresh();
	assert(lv_screen_active() == navidrome_screen);
	navidrome_set_root(dir);
	navidrome_reset();
	drain();
	lv_display_delete(display);
	lv_deinit();
	char path[1200];
	snprintf(path, sizeof(path), "%s/.local/navidrome/.cache", dir);
	assert(rmdir(path) == 0);
	snprintf(path, sizeof(path), "%s/.local/navidrome", dir);
	assert(rmdir(path) == 0);
	snprintf(path, sizeof(path), "%s/.local", dir);
	assert(rmdir(path) == 0);
	assert(!chdir("/") && rmdir(dir) == 0);
	printf("Music-style playlists at %dx%d: cards, scoped picking, naming/Back, bounded art, import return and named copies passed\n", ui_panel_width, panel_height);
	puts("real LVGL playlist creation, empty Add tracks, album expansion/dedupe, selection/removal, rename, gestures, offline/no-snapshot/read-only/lock: passed");
}

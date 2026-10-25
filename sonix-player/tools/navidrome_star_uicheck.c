// Real LVGL browser/Favorites/Now Playing star controls with controlled source
// metadata and HTTP delivery; the loopback check separately validates real HTTP.
#define NAVIDROME_STAR_UI
#define __wrap_http_request fixture_ping_request
#include "navidrome_browsecheck.c"
#undef __wrap_http_request
#define screen favorites_screen
#define gesture favorites_gesture
#define loaded favorites_loaded
#define unloaded favorites_unloaded
#define tick favorites_tick
#include "src/gui/streaming/navidromefavorites.c"
#undef screen
#undef gesture
#undef loaded
#undef unloaded
#undef tick
#include "src/gui/streaming/navidromestar.h"
#include "src/system/playback/device_state.h"

const lv_image_dsc_t icon_star = FIXTURE_ICON, icon_star_filled = FIXTURE_ICON;
const lv_image_dsc_t icon_circle_check = FIXTURE_ICON, icon_circle_alert = FIXTURE_ICON;
lv_font_t font_ui_24;
#define veil toast_veil
#define card toast_card
#define icon toast_icon
#define label toast_label
#include "src/gui/shell/toast.c"
#undef veil
#undef card
#undef icon
#undef label

static device_state_t represented;
void device_state_get(device_state_t *out) { *out = represented; }
bool nd_audio_owns(const char *path) { return path && !strncmp(path, "nd:", 3); }
bool navidromeplay_track(const char *path, nd_playrow_t *track, nd_library_t *library) {
	if (!nd_audio_owns(path) || !strcmp(path, "nd:missing"))
		return false;
	memset(track, 0, sizeof(*track));
	snprintf(track->row.id, sizeof(track->row.id), "%s", path + 3);
	track->row.favorite = !strcmp(path, "nd:cached-favorite");
	if (library)
		navidrome_library_snapshot(library);
	return true;
}
// Small shared-row scaffold; LVGL objects, page callbacks and list code are real.
lv_obj_t *settingsrow_page(lv_obj_t *screen, gui_config_t *cfg, const char *title) {
	settingsrow_title(screen, cfg, title);
	lv_obj_t *column = lv_obj_create(screen);
	lv_obj_set_size(column, cfg->screen_width - 16, cfg->screen_height - 80);
	lv_obj_set_pos(column, 8, 70);
	lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
	return column;
}
lv_obj_t *settingsrow_add(lv_obj_t *parent, const char *name, lv_obj_t **out, lv_event_cb_t cb, void *user) {
	lv_obj_t *row = lv_button_create(parent);
	lv_obj_set_size(row, lv_pct(100), 55);
	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text(label, name);
	if (out)
		*out = lv_label_create(row);
	if (cb)
		lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user);
	return row;
}
lv_obj_t *settingsrow_action(lv_obj_t *parent, const char *name, lv_event_cb_t cb, void *user) { return settingsrow_add(parent, name, NULL, cb, user); }

const char *__wrap_nd_api_finite_json(const char *host, const char *user, const char *password, const char *method, const char *query, size_t limit, bool (*current)(void *), void *arg, nd_document_t *out) {
	if (!current(arg))
		return "nd_sync_cancelled";
	return nd_api_json(host, user, password, method, query, limit, out);
}
static atomic_bool star_block, star_entered, metadata_block, metadata_entered;
static atomic_int star_writes;
static char written_ids[128][ND_ID_MAX];
static nd_kind_t written_kinds[128];
static bool written_state[128];
static int written_count;
static bool fixture_remote(nd_kind_t kind, const char *id) {
	for (int i = written_count - 1; i >= 0; i--)
		if (written_kinds[i] == kind && !strcmp(written_ids[i], id))
			return written_state[i];
	return false;
}
bool __wrap_http_request(const char *url, const http_req_t *req, char **out, size_t *size, size_t limit, int timeout, int *status) {
	if (strstr(url, "/ping.view?"))
		return fixture_ping_request(url, req, out, size, limit, timeout, status);
	assert(req->sensitive_url && req->require_complete_body && !strstr(url, server_password));
	atomic_fetch_add(&calls, 1);
	char body[1300];
	if (strstr(url, "/getScanStatus.view?"))
		snprintf(body, sizeof(body), "{\"subsonic-response\":{\"status\":\"ok\",\"scanStatus\":{\"scanning\":false,\"lastScan\":\"fixed\"}}}");
	else if (strstr(url, "/getAlbumList2.view?"))
		snprintf(body, sizeof(body), "{\"subsonic-response\":{\"status\":\"ok\",\"albumList2\":{\"album\":[{\"id\":\"live-album\",\"name\":\"Live Album\",\"songCount\":1}]}}}");
	else if (strstr(url, "/search3.view?"))
		snprintf(body, sizeof(body), "{\"subsonic-response\":{\"status\":\"ok\",\"searchResult3\":{\"song\":[{\"id\":\"live-song\",\"albumId\":\"live-album\",\"title\":\"Live Song\",\"starred\":\"2026-10-25T00:00:00Z\"}]}}}");
	else {
		bool mutation = strstr(url, "/star.view?") || strstr(url, "/unstar.view?");
		nd_kind_t kind = ND_TRACKS;
		const char *value = strstr(url, "&id=");
		if (mutation && strstr(url, "&albumId=")) {
			kind = ND_ALBUMS;
			value = strstr(url, "&albumId=") + 5;
		}
		if (mutation && strstr(url, "&artistId=")) {
			kind = ND_ARTISTS;
			value = strstr(url, "&artistId=") + 6;
		}
		assert(value);
		value += 4;
		char id[ND_ID_MAX];
		size_t length = strcspn(value, "&");
		assert(length && length < sizeof(id));
		memcpy(id, value, length);
		id[length] = 0;
		if (mutation) {
			atomic_store(&star_entered, true);
			while (atomic_load(&star_block))
				usleep(1000);
			assert(written_count < 128);
			snprintf(written_ids[written_count], ND_ID_MAX, "%s", id);
			written_kinds[written_count] = kind;
			written_state[written_count++] = strstr(url, "/unstar.view?") == NULL;
			atomic_fetch_add(&star_writes, 1);
			snprintf(body, sizeof(body), "{\"subsonic-response\":{\"status\":\"ok\"}}");
		} else {
			if (strstr(url, "/getAlbum.view?"))
				kind = ND_ALBUMS;
			if (strstr(url, "/getArtist.view?"))
				kind = ND_ARTISTS;
			if (kind == ND_ALBUMS && atomic_load(&metadata_block)) {
				atomic_store(&metadata_entered, true);
				while (atomic_load(&metadata_block))
					usleep(1000);
			}
			const char *objects[] = {"album", "song", "artist"};
			snprintf(body, sizeof(body), "{\"subsonic-response\":{\"status\":\"ok\",\"%s\":{\"id\":\"%s\",\"title\":\"Favorite track\",\"name\":\"Favorite album\",\"albumId\":\"targeted-album\",\"songCount\":0,\"song\":[]%s}}}", objects[kind], id, fixture_remote(kind, id) ? ",\"starred\":\"2026-10-25T00:00:00Z\"" : "");
		}
	}
	assert(strlen(body) < limit);
	*out = strdup(body);
	*size = strlen(body);
	*status = 200;
	return true;
}
static void stars_wait(void) {
	for (int i = 0; i < 10000; i++) {
		drain();
		navidrome_star_poll();
		if (!navidrome_star_busy()) {
			drain();
			return;
		}
		usleep(1000);
	}
	assert(!"star timeout");
}
static void selection_wait(void) {
	for (int i = 0; i < 10000 && (navidrome_playlists_busy() || pages[0].select_busy); i++) {
		drain();
		usleep(1000);
	}
	window_wait(0);
}
static void block_wait(void) {
	for (int i = 0; i < 10000 && !atomic_load(&star_entered); i++)
		usleep(1000);
	assert(atomic_load(&star_entered));
}
static void choose(const char *label) {
	int favorite_actions = 0;
	for (int i = 0; i < fixture_menu_count; i++) {
		assert(strcmp(fixture_menu[i].label, "nd_favorites_check"));
		favorite_actions += !strcmp(fixture_menu[i].label, "remove_from_favourites") || !strcmp(fixture_menu[i].label, "medialist_add_to_favourites");
	}
	assert(favorite_actions == 1);
	for (int i = 0; i < fixture_menu_count; i++)
		if (!strcmp(fixture_menu[i].label, label)) {
			fixture_menu[i].action(fixture_menu[i].user);
			return;
		}
	assert(!"menu action absent");
}
static lv_obj_t *player_button, *player_icon;
static unsigned player_paints;
static void paint_player(void) { assert(navidromestar_player_update(player_button, player_icon, represented.current_file, lv_color_hex(0x999999))); }
static void cached_same(nd_library_t before) {
	nd_library_t now;
	navidrome_favorites_snapshot(&now);
	assert(before.ready && now.ready && before.base == now.base && before.finished == now.finished);
}
static void empty_detail_wait(const char *key) {
	for (int i = 0; i < 10000; i++) {
		drain();
		lv_obj_update_layout(pages[1].screen);
		if (!pages[1].loading && !strcmp(lv_label_get_text(pages[1].status), key))
			return;
		usleep(1000);
	}
	assert(!"empty album detail timeout");
}
static void check_ui(int width, int height) {
	char dir[] = "/tmp/sonix-navidrome-star-ui-XXXXXX";
	assert(mkdtemp(dir) && !chdir(dir));
	written_count = 0;
	navidrome_init();
	navidrome_set_root(dir);
	assert(navidrome_save("http://fixture/base", "fixture user", server_password, "vault1"));
	wait_idle();
	lv_init();
	font_ui_28 = font_ui_24_bold = font_ui_24 = font_ui_14 = font_ui_18 = font_ui_20 = font_ui_64_bold = lv_font_unscii_8;
	lv_display_t *display = lv_display_create(width, height);
	navidrome_screen = lv_obj_create(NULL);
	gui_config_t cfg = {.screen_width = width, .screen_height = height, .padding = 8, .top_bar_height = 20};
	toast_init(&cfg);
	navidromebrowse_init(&cfg);
	navidromefavorites_init(&cfg);
	nd_star_request_t initial = {.action = ND_STAR_ADD, .kind = ND_TRACKS};
	snprintf(initial.id, sizeof(initial.id), "known-before-refresh");
	snprintf(initial.metadata.row.id, sizeof(initial.metadata.row.id), "%s", initial.id);
	snprintf(initial.metadata.row.title, sizeof(initial.metadata.row.title), "Known favorite");
	snprintf(initial.metadata.album_id, sizeof(initial.metadata.album_id), "known-album");
	assert(navidrome_star_submit(&initial, NULL, NULL));
	stars_wait();
	navidromefavorites_open();
	assert(!strcmp(lv_label_get_text(track_count), "1") && !strcmp(lv_label_get_text(status), "nd_favorites_known") && !strcmp(lv_label_get_text(last_refresh), "nd_favorites_never_refreshed"));
	lv_obj_send_event(lv_obj_get_parent(track_count), LV_EVENT_CLICKED, NULL);
	window_wait(0);
	assert(pages[0].total == 1 && !strcmp(pages[0].data[0].id, initial.id));
	switch_screen(navidrome_screen);
	nd_library_t library, favorite_cache;
	navidrome_library_snapshot(&library);
	seed(&library);
	navidrome_favorites_snapshot(&favorite_cache);
	favorite_cache.favorites = true;
	seed(&favorite_cache);
	navidrome_set_root(dir);
	for (int i = 0; i < 10000; i++) {
		navidrome_favorites_snapshot(&favorite_cache);
		if (!favorite_cache.loading)
			break;
		usleep(1000);
	}
	assert(favorite_cache.ready);

	// Membership is visible while targeted album details are still in flight.
	nd_star_request_t loading = {.action = ND_STAR_ADD, .kind = ND_ALBUMS};
	snprintf(loading.id, sizeof(loading.id), "loading-ui-album");
	snprintf(loading.metadata.row.id, sizeof(loading.metadata.row.id), "%s", loading.id);
	snprintf(loading.metadata.row.title, sizeof(loading.metadata.row.title), "AAA Loading");
	atomic_store(&metadata_block, true);
	atomic_store(&metadata_entered, false);
	assert(navidrome_star_submit(&loading, NULL, NULL));
	for (int i = 0; i < 10000 && !atomic_load(&metadata_entered); i++) {
		drain();
		usleep(1000);
	}
	assert(atomic_load(&metadata_entered));
	navidromefavorites_open();
	assert(!strcmp(lv_label_get_text(album_count), "1701"));
	lv_obj_send_event(lv_obj_get_parent(album_count), LV_EVENT_CLICKED, NULL);
	window_wait(0);
	assert(!strcmp(pages[0].data[0].id, loading.id) && pages[0].data[0].details == 1);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	empty_detail_wait("nd_favorites_loading");
	assert(!strcmp(lv_label_get_text(pages[1].status), "nd_favorites_loading"));
	atomic_store(&metadata_block, false);
	stars_wait();
	navidromebrowse_refresh();
	empty_detail_wait("nd_favorites_empty");
	assert(!strcmp(lv_label_get_text(pages[1].status), "nd_favorites_empty"));
	loading.action = ND_STAR_REMOVE;
	assert(navidrome_star_submit(&loading, NULL, NULL));
	stars_wait();
	navidromebrowse_refresh();
	for (int i = 0; i < 10000 && lv_screen_active() != pages[0].screen; i++) {
		drain();
		usleep(1000);
	}
	window_wait(0);
	assert(pages[0].total == 1700);
	puts("Real LVGL known-only Favorites, background album loading/completion and removed-album Back: passed");
	switch_screen(navidrome_screen);
	int before_requests = atomic_load(&calls);
	navidromefavorites_open();
	assert(lv_screen_active() == favorites_screen && !strcmp(lv_label_get_text(track_count), "17000"));
	lv_obj_send_event(lv_obj_get_parent(album_count), LV_EVENT_CLICKED, NULL);
	window_wait(0);
	assert(pages[0].query.favorites && pages[0].total == 1700);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_LONG_PRESSED, NULL);
	selection_wait();
	assert(pages[0].selecting && !lv_obj_is_hidden(pages[0].select_favorite));
	int album_writes = atomic_load(&star_writes);
	lv_obj_send_event(pages[0].select_favorite, LV_EVENT_CLICKED, NULL);
	stars_wait();
	assert(atomic_load(&star_writes) == album_writes + 1 && written_kinds[written_count - 1] == ND_ALBUMS);
	assert(!strcmp(written_ids[written_count - 1], pages[0].data[0].id));
	assert(!lv_obj_is_hidden(toast_veil) && lv_image_get_src(toast_icon) == &icon_circle_check);
	lv_color_t green = lv_obj_get_style_image_recolor(toast_icon, 0);
	assert(green.red == 0x2e && green.green == 0xc2 && green.blue == 0x7e);
	toast_dismiss();
	assert(browse_selection_back());
	window_wait(0);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	window_wait(1);
	assert(pages[1].total == 10 && pages[1].query.favorites);
	switch_screen_return_to(pages[0].screen);
	window_wait(0);
	before_requests = atomic_load(&calls);
	navidromebrowse_open_favorites(ND_TRACKS);
	window_wait(0);
	assert(pages[0].total == 17000 && atomic_load(&calls) == before_requests);
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_LONG_PRESSED, NULL);
	selection_wait();
	assert(pages[0].selecting && pages[0].selected == 1 && !lv_obj_is_hidden(pages[0].select_favorite));
	lv_obj_t *star_glyph = lv_obj_get_child(pages[0].select_favorite, 0);
	assert(lv_image_get_src(star_glyph) == &icon_star_plus);
	lv_obj_update_layout(pages[0].screen);
	lv_area_t button_bounds;
	lv_obj_get_coords(pages[0].select_favorite, &button_bounds);
	assert(button_bounds.x1 >= 0 && button_bounds.x2 < width);
	lv_obj_scroll_to_y(pages[0].list, 500 * pitch, LV_ANIM_OFF);
	lv_obj_send_event(pages[0].list, LV_EVENT_SCROLL, NULL);
	window_wait(0);
	lv_obj_send_event(pages[0].rows[1], LV_EVENT_CLICKED, NULL);
	selection_wait();
	assert(pages[0].selected == 2);
	int before_writes = atomic_load(&star_writes);
	lv_obj_send_event(pages[0].select_favorite, LV_EVENT_CLICKED, NULL);
	stars_wait();
	assert(atomic_load(&star_writes) == before_writes + 2 && pages[0].selecting && !pages[0].select_busy);
	navidromebrowse_refresh();
	assert(!lv_obj_has_state(pages[0].select_favorite, LV_STATE_DISABLED));
	cached_same(favorite_cache);
	assert(browse_selection_back());
	window_wait(0);
	unsigned plays = playback_selections;
	lv_obj_send_event(pages[0].rows[0], LV_EVENT_CLICKED, NULL);
	assert(playback_selections == plays + 1);
	// Menu appearance does no metadata work; a stale captured menu cannot target a recycled row.
	navidromebrowse_open(ND_TRACKS);
	window_wait(0);
	lv_obj_send_event(pages[0].menus[0], LV_EVENT_CLICKED, NULL);
	before_requests = atomic_load(&calls);
	browse_menu_serial--;
	choose("remove_from_favourites");
	assert(atomic_load(&calls) == before_requests);
	lv_obj_send_event(pages[0].menus[0], LV_EVENT_CLICKED, NULL);
	choose("remove_from_favourites");
	stars_wait();
	assert(written_kinds[written_count - 1] == ND_TRACKS);
	for (int kind = ND_ALBUMS; kind <= ND_ARTISTS; kind += 2) {
		navidromebrowse_open(kind);
		window_wait(0);
		before_writes = atomic_load(&star_writes);
		lv_obj_send_event(pages[0].menus[0], LV_EVENT_CLICKED, NULL);
		choose(kind == ND_ALBUMS ? "remove_from_favourites" : "medialist_add_to_favourites");
		stars_wait();
		assert(atomic_load(&star_writes) == before_writes + 1 && written_kinds[written_count - 1] == (nd_kind_t)kind);
	}
	cached_same(favorite_cache);
	// Real Now Playing star controller: direct track toggle, gestures, skip and source isolation.
	lv_obj_t *playing_screen = lv_obj_create(NULL);
	switch_screen(playing_screen);
	player_button = lv_button_create(playing_screen);
	player_icon = lv_image_create(player_button);
	toast_dismiss();
	unsigned notices_before_toggle = fixture_notices;
	snprintf(represented.current_file, sizeof(represented.current_file), "nd:cached-favorite");
	int cached_requests = atomic_load(&calls);
	paint_player();
	assert(lv_image_get_src(player_icon) == &icon_star_filled && lv_obj_get_style_opa(player_icon, 0) == LV_OPA_COVER);
	assert(atomic_load(&calls) == cached_requests);
	assert(navidromestar_player_toggle(represented.current_file, paint_player));
	stars_wait();
	assert(lv_image_get_src(player_icon) == &icon_star_filled);
	assert(lv_obj_is_hidden(toast_veil) && fixture_notices == notices_before_toggle);
	assert(navidromestar_player_toggle(represented.current_file, paint_player));
	stars_wait();
	assert(lv_image_get_src(player_icon) == &icon_star);
	assert(lv_obj_is_hidden(toast_veil) && fixture_notices == notices_before_toggle);
	snprintf(represented.current_file, sizeof(represented.current_file), "nd:playing-A");
	paint_player();
	assert(lv_image_get_src(player_icon) == &icon_star && lv_obj_get_style_opa(player_icon, 0) == LV_OPA_40);
	dragging = true;
	before_writes = atomic_load(&star_writes);
	assert(navidromestar_player_toggle(represented.current_file, paint_player));
	dragging = false;
	assert(atomic_load(&star_writes) == before_writes);
	atomic_store(&star_block, true);
	atomic_store(&star_entered, false);
	assert(navidromestar_player_toggle(represented.current_file, paint_player));
	block_wait();
	assert(navidromestar_player_toggle(represented.current_file, paint_player));
	snprintf(represented.current_file, sizeof(represented.current_file), "nd:playing-B");
	paint_player();
	atomic_store(&star_block, false);
	stars_wait();
	assert(!strcmp(written_ids[written_count - 1], "playing-A") && lv_image_get_src(player_icon) == &icon_star);
	assert(navidrome_star_state(ND_TRACKS, "playing-B") == ND_STAR_UNKNOWN);
	assert(navidromestar_player_toggle(represented.current_file, paint_player));
	stars_wait();
	assert(lv_image_get_src(player_icon) == &icon_star_filled);
	assert(navidromestar_player_toggle(represented.current_file, paint_player));
	stars_wait();
	assert(lv_image_get_src(player_icon) == &icon_star);
	assert(!navidromestar_player_toggle("/local/music.flac", paint_player));
	assert(!navidromestar_player_update(player_button, player_icon, "/local/music.flac", lv_color_black()));
	assert(navidromestar_player_update(player_button, player_icon, "nd:missing", lv_color_black()) && lv_obj_has_state(player_button, LV_STATE_DISABLED));
	unsigned notices_before_error = fixture_notices;
	assert(navidromestar_player_toggle("nd:missing", paint_player));
	assert(fixture_notices == notices_before_error + 1);
	cached_same(favorite_cache);
	// A rejected gui_post retains one bounded result for the GUI timer, with no server replay.
	atomic_store(&fixture_reject_posts, true);
	assert(navidromestar_player_toggle(represented.current_file, paint_player));
	wait_idle();
	atomic_store(&fixture_reject_posts, false);
	navidrome_star_poll();
	assert(!navidrome_star_busy());
	// Credential replacement after worker completion still rejects the queued GUI result.
	assert(navidromestar_player_toggle(represented.current_file, paint_player));
	wait_idle();
	unsigned captured_paints = player_paints;
	server_password = "replacement-synthetic-password";
	assert(navidrome_update_password("http://fixture/base", "fixture user", server_password));
	wait_idle();
	drain();
	assert(player_paints == captured_paints && navidrome_star_state(ND_TRACKS, "playing-B") == ND_STAR_UNKNOWN);
	// Card invalidation during a write cannot update a replacement page/source.
	navidromebrowse_open(ND_TRACKS);
	window_wait(0);
	atomic_store(&star_block, true);
	atomic_store(&star_entered, false);
	lv_obj_send_event(pages[0].menus[1], LV_EVENT_CLICKED, NULL);
	choose("remove_from_favourites");
	block_wait();
	navidrome_set_root(NULL);
	navidromebrowse_refresh();
	switch_screen(navidrome_screen);
	atomic_store(&star_block, false);
	stars_wait();
	assert(lv_screen_active() == navidrome_screen);
	navidrome_set_root(dir);
	for (int i = 0; i < 10000; i++) {
		navidrome_library_snapshot(&library);
		if (!library.loading)
			break;
		usleep(1000);
	}
	// Late row results after navigation/lock cannot restore content.
	navidromebrowse_open(ND_TRACKS);
	window_wait(0);
	atomic_store(&star_block, true);
	atomic_store(&star_entered, false);
	lv_obj_send_event(pages[0].menus[0], LV_EVENT_CLICKED, NULL);
	choose("medialist_add_to_favourites");
	block_wait();
	navidrome_lock();
	navidromebrowse_refresh();
	switch_screen(navidrome_screen);
	atomic_store(&star_block, false);
	stars_wait();
	assert(lv_screen_active() == navidrome_screen);
	navidrome_set_root(dir);
	navidrome_reset();
	drain();
	lv_display_delete(display);
	lv_deinit();
	// SQLite-backed selections are durable user data and are removed only by explicit Forget/reset above.
	char path[1200];
	snprintf(path, sizeof(path), "%s/.local/navidrome/.cache", dir);
	assert(!rmdir(path));
	snprintf(path, sizeof(path), "%s/.local/navidrome", dir);
	assert(!rmdir(path));
	snprintf(path, sizeof(path), "%s/.local", dir);
	assert(!rmdir(path));
	assert(!chdir("/") && !rmdir(dir));
}
int main(int argc, char **argv) {
	alarm(90);
	setvbuf(stdout, NULL, _IONBF, 0);
	int width = argc == 3 ? atoi(argv[1]) : 320, height = argc == 3 ? atoi(argv[2]) : 480;
	check_ui(width, height);
	printf("Real LVGL Favorites/selection/menus/Now Playing, confirmed states, source/skip/lock races and rejected-post fallback at %dx%d: passed\n", width, height);
	// The browser owns an intentionally persistent worker; run the second LVGL lifecycle in a fresh process.
	return 0;
}

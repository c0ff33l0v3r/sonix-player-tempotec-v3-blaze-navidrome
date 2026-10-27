// Production Discovery controller/widgets, real LVGL and loopback service.
#define NAVIDROME_BROWSE_FIXTURE_ONLY 1
#define NAVIDROME_DISCOVERY_OFFLINE_UI 1
#define icon_download fixture_download_stub
#define NAVIDROME_DOWNLOAD_UI 1
#include "navidrome_browsecheck.c"
#undef NAVIDROME_BROWSE_FIXTURE_ONLY
#undef icon_download
#include "src/system/decode/decode.h"
#include <fcntl.h>
#include "src/system/streaming/navidromeoffline.c"
static bool fixture_discovery_star_submit(const nd_star_request_t *, void (*)(nd_star_result_t *, void *), void *);
#define navidrome_star_submit fixture_discovery_star_submit
#include "src/gui/streaming/navidromediscovery.c"
#undef navidrome_star_submit
#include "src/gui/streaming/navidromepage.c"
lv_font_t font_ui_16;
const lv_image_dsc_t icon_menu_year = FIXTURE_ICON, icon_menu_album = FIXTURE_ICON, icon_menu_most_played = FIXTURE_ICON, icon_menu_explorer = FIXTURE_ICON;
const lv_image_dsc_t icon_menu_all = FIXTURE_ICON, icon_menu_artist = FIXTURE_ICON, icon_menu_genre = FIXTURE_ICON, icon_menu_playlist = FIXTURE_ICON, icon_menu_favourites = FIXTURE_ICON;
const lv_image_dsc_t icon_search = FIXTURE_ICON, icon_music_settings = FIXTURE_ICON;
void navidromeplay_init(void) {}
void navidromeplay_shutdown(void) {}
void navidromesettings_init(gui_config_t *cfg) { (void)cfg; }
void navidromesettings_refresh(void) {}
void navidromesettings_open(void) {}
void navidromeplaylists_init(gui_config_t *cfg) { (void)cfg; }
void navidromeplaylists_refresh(void) {}
void navidromeplaylists_open(void) {}
void navidromefavorites_init(gui_config_t *cfg) { (void)cfg; }
void navidromefavorites_update(void) {}
void navidromefavorites_open(void) {}
void navidromesearch_init(gui_config_t *cfg) {
	// Reserve the same three shared album views as production Search.
	static nd_album_view_t search_views[3];
	for (int i = 0; i < 3; i++) {
		lv_obj_t *screen = lv_obj_create(NULL);
		navidromealbum_init(&search_views[i], cfg, screen, lv_obj_create(screen), lv_label_create(screen), lv_label_create(screen), lv_label_create(screen), ND_ART_SEARCH_ALBUM_SLOT, NULL, NULL);
	}
}
void navidromesearch_refresh(void) {}
void navidromesearch_open(void) {}
void switch_screen_cb(lv_event_t *e) { switch_screen(lv_event_get_user_data(e)); }
bool switcher_in_history(lv_obj_t *screen) { return screen == discovery_pages[0].screen && lv_screen_active() == discovery_pages[1].screen; }
static unsigned discovery_selections;
bool navidromeplay_discovery(const nd_library_t *lib, const nd_context_t *identity, const char *album, const char *id, void *origin) {
	assert(lib && navidrome_search_current(identity, false) && *album && *id && origin == discovery_pages[1].screen);
	discovery_selections++;
	return true;
}
void navidromeplay_discovery_leave(void *origin) { (void)origin; }
static unsigned header_actions[3], header_downloads, menu_queues, menu_playlists, menu_stars;
bool navidromeplay_discovery_album(const nd_library_t *lib, const nd_context_t *identity, const char *album, int action, void *origin) {
	assert(lib && navidrome_search_current(identity, false) && *album && action >= 0 && action < 3 && origin == discovery_pages[1].screen);
	header_actions[action]++;
	return true;
}
bool navidromedownloads_request(const nd_dl_request_t *q, nd_dl_captured_cb callback, void *user) {
	assert(q->remote_album && q->kind == ND_ALBUMS && q->action == NDDL_CAPTURE && q->profile_epoch);
	assert(callback && user);
	assert(!strcmp(q->id, discovery_active_page() == 1 ? discovery_pages[1].album : discovery_menu_target.album.id));
	header_downloads++;
	return true;
}
void navidromedownloads_poll(void) {}
bool navidromeplay_discovery_enqueue(const nd_library_t *lib, const nd_context_t *identity, const char *album, void *origin) {
	assert(lib && navidrome_search_current(identity, false) && *album && origin == discovery_pages[0].screen);
	menu_queues++;
	return true;
}
bool navidromeplaylists_pick_album(const nd_row_t *album, const nd_library_t *lib, const nd_context_t *identity, lv_obj_t *origin) {
	assert(album && *album->id && lib && identity && origin == discovery_pages[0].screen);
	menu_playlists++;
	return true;
}
static bool fixture_discovery_star_submit(const nd_star_request_t *q, void (*done)(nd_star_result_t *, void *), void *user) {
	assert(q->kind == ND_ALBUMS && q->bound_profile && *q->id && done && user);
	menu_stars++;
	return true;
}
static atomic_bool fail_discovery_malloc;
void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size) {
	if (size == sizeof(discovery_job_t) && atomic_exchange(&fail_discovery_malloc, false))
		return NULL;
	return __real_malloc(size);
}
static void discovery_wait(int page) {
	for (int i = 0; i < 12000; i++) {
		drain();
		discovery_tick(discovery_timer);
		lv_obj_update_layout(discovery_pages[page].screen);
		if (!discovery_pages[page].loading)
			return;
		usleep(1000);
	}
	assert(!"Discovery GUI timeout");
}
static void fixture_control(const char *origin, const char *mode) {
	char url[700];
	snprintf(url, sizeof(url), "%s/control?mode=%s", origin, mode);
	http_session_t *s = http_session_new(NULL, NULL);
	assert(s && http_session_get(s, url, false, false));
	char data[1024];
	while (http_session_read(s, data, sizeof(data)) > 0) {
	}
	http_session_free(s);
}
static void fixture_click(lv_obj_t *object) { lv_obj_send_event(object, LV_EVENT_CLICKED, NULL); }
static void discovery_green_pixels(lv_obj_t *image) {
	lv_obj_update_layout(lv_obj_get_screen(image));
	lv_draw_buf_t *pixels = lv_snapshot_take(image, LV_COLOR_FORMAT_ARGB8888);
	assert(pixels);
	unsigned green = 0;
	for (unsigned y = 0; y < pixels->header.h; y++)
		for (unsigned x = 0; x < pixels->header.w; x++) {
			const unsigned char *v = pixels->data + y * pixels->header.stride + x * 4;
			if (v[1] > 200 && v[0] < 20 && v[2] < 20 && v[3] > 200)
				green++;
		}
	assert(green > 100);
	lv_draw_buf_destroy(pixels);
}
static void discovery_retained_pixels(void) {
	discovery_page_t *p = &discovery_pages[0];
	cover_image_t *art = &p->art[1];
	art->pixels = calloc(16 * 16, 4);
	assert(art->pixels);
	for (int i = 0; i < 16 * 16; i++) {
		art->pixels[4 * i + 1] = 255;
		art->pixels[4 * i + 3] = 255;
	}
	art->dsc = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = 16, .h = 16, .stride = 64}, .data = art->pixels, .data_size = 16 * 16 * 4};
	p->art_done[1] = true;
	lv_obj_remove_style(p->images[1], &theme_style_icon, 0);
	lv_image_set_src(p->images[1], &art->dsc);
	lv_obj_t *widget = p->rows[1];
	void *retained = art->pixels;
	nd_search_result_t *next = malloc(sizeof(*next));
	assert(next);
	*next = p->data;
	next->first = 1;
	next->count = ND_WINDOW - 1;
	memmove(next->rows, next->rows + 1, (ND_WINDOW - 1) * sizeof(*next->rows));
	discovery_art_remap(p, next);
	p->data = *next;
	free(next);
	discovery_paint_rows(p);
	assert(p->rows[0] == widget && p->art[0].pixels == retained);
	discovery_green_pixels(p->images[0]);
	assert(lv_obj_is_hidden(p->offline.icons[0]) && p->offline.state[0] == ND_OFF_UNKNOWN);
	unsigned serial = p->serial;
	for (int i = 0; i < 5; i++) {
		discovery_paint_rows(p);
		discovery_green_pixels(p->images[0]);
	}
	assert(p->serial == serial && p->art[0].pixels == retained);
	discovery_send(p, 0, NULL, false);
	discovery_wait(0);
}
static void discovery_refresh_pixels(void) {
	lv_obj_t *button = discovery_refresh_button;
	lv_obj_t *icon = lv_obj_get_child(button, 0);
	assert(lv_obj_check_type(icon, &lv_image_class));
	assert(lv_image_get_src(icon) == &icon_refresh);
	assert(lv_obj_get_style_bg_opa(button, 0) == LV_OPA_TRANSP);
	assert(!lv_obj_get_style_border_width(button, 0) && !lv_obj_get_style_shadow_width(button, 0));
	// Render the production bitmap with light, dark and AMOLED-style header ink.
	const unsigned inks[] = {0x202020, 0xeeeeee, 0xffffff};
	for (unsigned ink = 0; ink < sizeof(inks) / sizeof(*inks); ink++) {
		lv_obj_set_style_image_recolor(icon, lv_color_hex(inks[ink]), 0);
		lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
		lv_obj_update_layout(discovery_pages[0].screen);
		lv_draw_buf_t *pixels = lv_snapshot_take(button, LV_COLOR_FORMAT_ARGB8888);
		assert(pixels);
		unsigned painted = 0, solid = 0;
		for (unsigned y = 0; y < pixels->header.h; y++)
			for (unsigned x = 0; x < pixels->header.w; x++) {
				const unsigned char *v = pixels->data + y * pixels->header.stride + x * 4;
				if (v[3]) painted++;
				if (v[3] == 255) {
					assert(v[0] == (inks[ink] & 255) && v[1] == ((inks[ink] >> 8) & 255) && v[2] == (inks[ink] >> 16));
					solid++;
				}
			}
		assert(solid > 25 && painted < pixels->header.w * pixels->header.h / 2);
		assert(!pixels->data[3]); // Transparent corner, no grey card behind the icon.
		lv_draw_buf_destroy(pixels);
	}
}
decode_format_t decode_detect_format(const char *path) { (void)path; return DECODE_FORMAT_UNKNOWN; }
decoder_t *decoder_open_info(const char *path, decode_format_t format) { (void)path; (void)format; return NULL; }
int decoder_sample_rate(const decoder_t *decoder) { (void)decoder; return 0; }
int decoder_channels(const decoder_t *decoder) { (void)decoder; return 0; }
void decoder_close(decoder_t *decoder) { (void)decoder; }
static void discovery_coverage_wait(discovery_page_t *p, nd_off_state_t state) {
	for (int i = 0; i < 10000; i++) {
		drain();
		navidromeofflinebadge_tick(&p->offline);
		if (!nd_offline_busy(p->offline.token) && p->album_view.download_state == state)
			return;
		usleep(1000);
	}
	fprintf(stderr, "coverage expected=%d actual=%d token=%u bound=%d busy=%d active=%d membership_current=%d\n", state, p->album_view.download_state, p->offline.token, p->offline.bound, nd_offline_busy(p->offline.token), p->album_view.active, nd_album_membership_matches(p->members, &p->data.library, p->album_view.info.row.id));
	assert(!"Discovery coverage timeout");
}
static void discovery_original(discovery_page_t *p, int index) {
	nd_dl_store_t store;
	assert(nd_dl_open(&store, &p->data.library, true));
	assert(nd_dl_sql(&store, "INSERT OR IGNORE INTO batch(id,destination,created) VALUES(1,'Music/Navidrome',0)"));
	nd_playrow_t row = {0};
	nd_row_t members[ND_WINDOW];
	int count, total;
	assert(nd_album_membership_read(p->members, index, members, &count, &total) && count > 0);
	strcpy(row.row.id, members[0].id);
	int added = 0, skipped = 0;
	assert(nd_dl_insert(&store, 1, "Music/Navidrome", &row, 0, &added, &skipped) && added == 1);
	nd_dl_job_t job;
	assert(nd_dl_next(&store, &job));
	int folder = nd_dl_directory(store.root, "Music/Navidrome/Discovery", true);
	char leaf[32]; snprintf(leaf, sizeof(leaf), "%d.wav", index);
	int in = open(getenv("SONIX_CHECK_WAV"), O_RDONLY), out = openat(folder, leaf, O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(folder >= 0 && in >= 0 && out >= 0);
	char data[4096]; ssize_t n;
	while ((n = read(in, data, sizeof(data))) > 0) assert(write(out, data, n) == n);
	assert(!n && !close(in) && !close(out));
	struct stat st;
	assert(nd_dl_fingerprint(folder, leaf, job.digest, &st, NULL, NULL));
	close(folder);
	job.state = NDDL_COMPLETE; job.bytes = st.st_size; job.mtime = st.st_mtime; job.inode = st.st_ino;
	snprintf(job.path, sizeof(job.path), "Music/Navidrome/Discovery/%s", leaf);
	strcpy(job.extension, ".wav");
	assert(nd_dl_update(&store, &job)); nd_dl_close(&store);
	nd_dl_availability_changed(); // the production publisher emits this event
}
static void discovery_header_coverage(discovery_page_t *p) {
	assert(!p->data.library.ready && p->members && p->offline.request.header.members == p->members);
	discovery_coverage_wait(p, ND_OFF_NONE);
	unsigned submissions = header_downloads;
	discovery_original(p, 0);
	discovery_coverage_wait(p, ND_OFF_PARTIAL);
	for (int i = 1; i < p->data.total; i++) discovery_original(p, i);
	discovery_coverage_wait(p, ND_OFF_COMPLETE);
	assert(header_downloads == submissions);
	lv_obj_t *icon = lv_obj_get_child(p->album_view.controls[ND_ALBUM_DOWNLOAD], 0);
	assert(lv_image_get_src(icon) == &icon_download && icon_download.header.w > 1);
	assert(lv_color_eq(lv_obj_get_style_image_recolor(icon, 0), nd_offline_color(ND_OFF_COMPLETE, theme()->screen_bg)));
	unsigned retired = p->offline.token;
	discovery_paint_rows(p);
	assert(p->album_view.download_state == ND_OFF_COMPLETE);
	discovery_coverage_wait(p, ND_OFF_COMPLETE);
	badge_done(retired, ND_OFF_HEADER, ND_OFF_PARTIAL, &p->offline);
	assert(p->album_view.download_state == ND_OFF_COMPLETE);
	// Removing a completed original is detected by periodic passive validation.
	char missing[1400];
	nd_dl_store_t store;
	assert(nd_dl_open(&store, &p->data.library, false));
	snprintf(missing, sizeof(missing), "%s/Music/Navidrome/Discovery/0.wav", store.root);
	nd_dl_close(&store);
	assert(!unlink(missing));
	p->offline.settled = lv_tick_get() - OFF_REFRESH_MS;
	discovery_coverage_wait(p, ND_OFF_PARTIAL);
	assert(header_downloads == submissions);
	// A source/card mismatch invalidates even a completed but delayed header delivery.
	p->offline.request.library.scope[0] ^= 1;
	badge_done(p->offline.token, ND_OFF_HEADER, ND_OFF_COMPLETE, &p->offline);
	assert(p->album_view.download_state == ND_OFF_UNKNOWN);
	p->offline.request.library.scope[0] ^= 1;
	discovery_paint_rows(p);
	discovery_coverage_wait(p, ND_OFF_PARTIAL);
	puts("PASS Discovery unsynced complete child with real worker/permanent WAVs: normal/partial/green, retained repaint, stale delivery and external deletion; no status-triggered actions");
}
int main(int argc, char **argv) {
	assert(argc == 5);
	alarm(90);
	(void)seed;
	int width = atoi(argv[3]), height = atoi(argv[4]);
	assert(!chdir(argv[2]));
	snprintf(config_file, sizeof(config_file), "%s/config.ini", argv[2]);
	navidrome_init();
	navidrome_set_root("");
	char host[700];
	snprintf(host, sizeof(host), "%s/base", argv[1]);
	assert(navidrome_save(host, "fixture user", server_password, NULL));
	assert(wait_idle().state == ND_CONNECTED);
	lv_init();
	ui_panel_width = width;
	font_ui_28 = font_ui_24_bold = font_ui_24 = font_ui_14 = font_ui_16 = font_ui_18 = font_ui_20 = font_ui_64_bold = lv_font_unscii_8;
	lv_display_t *display = lv_display_create(width, height);
	gui_config_t cfg = {.screen_width = width, .screen_height = height, .padding = ui_px(12), .top_bar_height = ui_px(30)};
	navidromealbum_set_enabled(false);
	navidromepage_init(&cfg);
	switch_screen(navidrome_screen);
	lv_obj_update_layout(navidrome_screen);
	assert(lv_obj_get_child_count(category_grid) == 7 && lv_obj_is_scrollable(category_grid));
	lv_obj_t *tile = lv_obj_get_child(category_grid, 6);
	assert(!strcmp(lv_label_get_text(lv_obj_get_child(tile, 1)), "nd_discovery"));
	assert(lv_obj_get_y(tile) > lv_obj_get_y(lv_obj_get_child(category_grid, 5)));
	lv_obj_scroll_to_y(category_grid, lv_obj_get_y(tile), LV_ANIM_OFF);
	int saved_y = lv_obj_get_scroll_y(category_grid), before = atomic_load(&calls);
	fixture_click(tile);
	assert(lv_screen_active() == discovery_menu && lv_obj_get_child_count(discovery_grid) == 4);
	assert(atomic_load(&calls) == before);
	for (int i = 0; i < 4; i++)
		assert(!strcmp(lv_label_get_text(lv_obj_get_child(lv_obj_get_child(discovery_grid, i), 1)), discovery_labels[i]));
	dragging = true;
	fixture_click(lv_obj_get_child(discovery_grid, 0));
	dragging = false;
	assert(lv_screen_active() == discovery_menu);
	fixture_click(lv_obj_get_child(discovery_grid, 3));
	discovery_wait(0);
	assert(discovery_pages[0].populated && discovery_pages[0].data.total == 50);
	discovery_refresh_pixels();
	char random_id[ND_ID_MAX];
	snprintf(random_id, sizeof(random_id), "%s", discovery_pages[0].data.rows[0].id);
	discovery_retained_pixels();
	// The shared three-dot target exposes the usual album actions, never a row tap.
	lv_obj_t *dots = lv_obj_get_child(discovery_pages[0].menus[0], 0);
	assert(icon_ellipsis_vertical.header.w > 1 && lv_image_get_src(dots) == &icon_ellipsis_vertical);
	fixture_click(discovery_pages[0].menus[0]);
	assert(lv_screen_active() == discovery_pages[0].screen && fixture_menu_count == 4);
	const char *menu_labels[] = {"medialist_add_to_favourites", "add_to_queue", "add_to_playlist", "nd_dl_download"};
	for (int i = 0; i < 4; i++) {
		assert(!strcmp(fixture_menu[i].label, menu_labels[i]));
		fixture_menu[i].action(fixture_menu[i].user);
	}
	assert(menu_stars == 1 && menu_queues == 1 && menu_playlists == 1 && header_downloads == 1);
	unsigned old_visit = discovery_visit;
	discovery_visit++;
	fixture_menu[1].action(fixture_menu[1].user);
	assert(menu_queues == 1);
	discovery_visit = old_visit;
	// Make a writable card available without Sync; retained Discovery membership stays remote.
	navidrome_set_root(argv[2]);
	nd_library_t status_library;
	for (int i = 0; i < 10000; i++) {
		drain(); navidrome_library_snapshot(&status_library);
		if (!status_library.loading) break;
		usleep(1000);
	}
	assert(status_library.writable && !status_library.ready);
	navidromediscovery_refresh();
	// A real row tap owns the parent album identity before widget reuse.
	discovery_page_t *child = &discovery_pages[1];
	fixture_click(discovery_pages[0].rows[0]);
	discovery_wait(1);
	assert(!strcmp(child->album, random_id));
	assert(child->populated && child->data.total == 12 && !child->album_view.active);
	fixture_click(child->rows[0]);
	assert(discovery_selections == 1);
	strcpy(child->data.rows[0].artist, "An artist credit long enough to require ellipsis beside a duration");
	child->data.rows[0].duration = 599999;
	marker_library = child->data.library;
	strcpy(selected_track, child->data.rows[0].id);
	discovery_paint_rows(child);
	lv_obj_update_layout(child->screen);
	assert(!lv_obj_is_hidden(child->marks[0]) && !lv_obj_is_hidden(child->durations[0]));
	assert(lv_obj_get_x(child->details[0]) + lv_obj_get_width(child->details[0]) <= lv_obj_get_x(child->durations[0]));
	bool layout = navidromealbum_enabled();
	navidromealbum_set_enabled(!layout);
	navidromediscovery_refresh();
	discovery_wait(1);
	assert(child->data.alternate == !layout);
	assert(lv_obj_get_width(child->numbers[0]) == ui_px(44));
	assert(lv_obj_get_style_text_align(child->numbers[0], 0) == LV_TEXT_ALIGN_CENTER);
	assert(child->album_view.active && !lv_obj_is_hidden(child->album_view.buttons));
	discovery_header_coverage(child);
	for (int i = 0; i < 3; i++) {
		fixture_click(child->album_view.controls[i]);
		assert(header_actions[i] == 1);
	}
	dragging = true;
	fixture_click(child->album_view.controls[ND_ALBUM_PLAY]);
	dragging = false;
	assert(header_actions[ND_ALBUM_PLAY] == 1);
	fixture_click(child->album_view.controls[ND_ALBUM_DOWNLOAD]);
	assert(header_downloads == 2 && child->action_busy);
	discovery_download_done(true, NULL, 12, 0, (void *)(((uintptr_t)child->serial << 1) | 1));
	assert(!child->action_busy);
	bool registered = false;
	for (unsigned i = 0; i < sizeof(album_views) / sizeof(album_views[0]); i++)
		registered |= album_views[i] == &child->album_view;
	assert(registered);
	fixture_album_art_ready = true;
	navidromediscovery_refresh();
	assert(child->album_view.tinted && child->album_view.image.pixels);
	discovery_coverage_wait(child, ND_OFF_PARTIAL);
	assert(lv_color_eq(lv_obj_get_style_image_recolor(lv_obj_get_child(child->album_view.controls[ND_ALBUM_DOWNLOAD], 0), 0), nd_offline_color(ND_OFF_PARTIAL, child->album_view.bg)));
	lv_color_t bg = lv_obj_get_style_bg_color(child->screen, 0);
	assert(bg.green > bg.red && bg.green > bg.blue);
	assert(lv_color_eq(bg, child->album_view.bg));
	// Theme refresh must retain the cover tint for these last-registered views.
	lv_obj_set_style_bg_color(child->screen, lv_color_black(), 0);
	album_theme_refresh();
	assert(lv_color_eq(lv_obj_get_style_bg_color(child->screen, 0), bg));
	navidromealbum_set_enabled(false);
	assert(!child->album_view.tinted && !child->album_view.image.pixels && lv_obj_is_hidden(child->album_view.head));
	navidromediscovery_refresh();
	discovery_wait(1);
	switch_screen_return_to(discovery_pages[0].screen);
	discovery_wait(0);
	assert(!strcmp(discovery_pages[0].data.rows[0].id, random_id));
	fixture_control(argv[1], "api");
	discovery_refresh_clicked(NULL);
	discovery_wait(0);
	assert(discovery_retry_valid && discovery_pages[0].data.total == 50 && !strcmp(discovery_pages[0].data.rows[0].id, random_id));
	fixture_control(argv[1], "empty");
	discovery_retry_clicked(NULL);
	discovery_wait(0);
	assert(!discovery_retry_valid && discovery_pages[0].populated && !discovery_pages[0].data.total);
	fixture_control(argv[1], "normal");
	atomic_store(&fail_discovery_malloc, true);
	discovery_refresh_clicked(NULL);
	discovery_wait(0);
	assert(discovery_retry_valid && !discovery_pages[0].loading);
	discovery_retry_clicked(NULL);
	discovery_wait(0);
	assert(discovery_pages[0].data.total == 50);
	// Exercise lost GUI delivery with the production fixed fallback slot.
	pthread_mutex_lock(&post_lock);
	post_write = post_read + 64;
	pthread_mutex_unlock(&post_lock);
	discovery_refresh_clicked(NULL);
	for (int i = 0; i < 10000; i++) {
		pthread_mutex_lock(&discovery_lock);
		bool ready = discovery_undelivered != NULL;
		pthread_mutex_unlock(&discovery_lock);
		if (ready)
			break;
		usleep(1000);
	}
	pthread_mutex_lock(&post_lock);
	post_read = post_write;
	pthread_mutex_unlock(&post_lock);
	discovery_tick(discovery_timer);
	assert(!discovery_pages[0].loading && discovery_pages[0].data.total == 50);
	// Returning after a retired in-flight read offers deliberate Retry, never a stuck spinner.
	lv_obj_t *interruption = lv_obj_create(NULL);
	discovery_pages[0].loading = true;
	switch_screen(interruption);
	switch_screen(discovery_pages[0].screen);
	assert(!discovery_pages[0].loading && discovery_retry_valid);
	assert(!strcmp(lv_label_get_text(discovery_pages[0].reason), "nd_discovery_retry"));
	discovery_retry_clicked(NULL);
	discovery_wait(0);
	// A stale response cannot restore a retired visit.
	discovery_job_t *late = calloc(1, sizeof(*late));
	assert(late);
	late->visit = discovery_visit;
	late->request = discovery_request;
	late->serial = discovery_pages[0].serial;
	switch_screen(discovery_menu);
	discovery_done(late);
	assert(!discovery_pages[0].populated);
	switch_screen_return_to(navidrome_screen);
	assert(lv_obj_get_scroll_y(category_grid) == saved_y);
	lv_obj_t *outside = lv_obj_create(NULL);
	switch_screen(outside);
	discovery_tick(discovery_timer);
	assert(lv_timer_get_paused(discovery_timer));
	assert(!playback_selections && atomic_load(&calls) == before);
	printf("Discovery production LVGL %dx%d: seventh tile/grid, shared rows/three-dot menu/header actions, gestures, Random/child return, album layout/tap, Refresh failure/empty, allocation/post recovery, stale visit and timer pause passed\n", width, height);
	(void)display;
	navidrome_shutdown();
	return 0;
}

// Actual LVGL Downloads page, shared settings rows/keyboard and current-track
// controller. Worker responses are synthetic; downloadcheck covers real I/O.
#define _GNU_SOURCE
#include "src/gui/shell/animpark.h"
#include "src/gui/shell/popover.h"
#include "src/gui/streaming/navidromedownloadmenu.h"
#include "src/gui/streaming/navidromedownloadpage.c"
#include "src/system/playback/device_state.h"
#include "src/system/streaming/navidromeplay.h"
#include <assert.h>
#include <unistd.h>

void power_slow_in_standby(lv_timer_t *timer, uint32_t period) {
	(void)timer;
	(void)period;
}
void navidromedownloads_set_listener(void (*listener)(void)) { (void)listener; }
extern const lv_font_t lv_font_unscii_8;
int32_t ui_panel_width = 320;
lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_22, font_ui_24, font_ui_24_bold, font_ui_26, font_ui_28, font_ui_32;
lv_style_t theme_style_screen, theme_style_card, theme_style_card_pressed, theme_style_text, theme_style_text_dim, theme_style_icon;
lv_obj_t *browser_screen;
static nd_library_t lib;
static nd_context_t life = {.profile_epoch = 1, .card_epoch = 1};
static device_state_t playing;
static bool available = true, dragging, allow_request = true, fail_save, scan_busy, detect = true, saved_notice;
static struct {
	char id[ND_ID_MAX];
	int size;
	bool pending;
} fixture_art[ND_ART_SLOT_COUNT];
static bool art_ready, history_cleared;
static unsigned art_requests, art_releases;
void navidromeart_release(int slot) {
	assert(slot >= ND_ART_DOWNLOAD_SLOT && slot < ND_ART_SLOT_COUNT);
	fixture_art[slot].pending = false;
	art_releases++;
}
void navidromeart_request(int slot, const nd_library_t *library, const char *id, int size) {
	assert(slot >= ND_ART_DOWNLOAD_SLOT && slot < ND_ART_SLOT_COUNT && library->card_epoch == life.card_epoch);
	fixture_art[slot].pending = true;
	fixture_art[slot].size = size;
	snprintf(fixture_art[slot].id, sizeof(fixture_art[slot].id), "%s", id);
	art_requests++;
}
bool navidromeart_take(int slot, cover_image_t *out, bool *finished) {
	*finished = art_ready;
	if (!art_ready || !fixture_art[slot].pending)
		return false;
	fixture_art[slot].pending = false;
	int size = fixture_art[slot].size;
	out->pixels = calloc((size_t)size * size, 4);
	assert(out->pixels);
	for (int i = 0; i < size * size; i++) {
		out->pixels[i * 4 + 1] = 255;
		out->pixels[i * 4 + 3] = 255;
	}
	out->dsc = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = size, .h = size, .stride = size * 4}, .data = out->pixels, .data_size = (uint32_t)(size * size * 4)};
	return true;
}
void cover_free(cover_image_t *image) {
	free(image->pixels);
	memset(image, 0, sizeof(*image));
}
static unsigned admissions, updates, refreshes, notices, successes;
static nd_dl_request_t admitted;
static nd_dl_view_t *response;
static nd_dl_captured_cb callback;
static void *callback_user;
static char current_id[ND_ID_MAX] = "exact/current?", message[160];
const char *lang_current(void) { return "English"; }
theme_mode_t theme_get_mode(void) { return (theme_mode_t)0; }
const theme_palette_t *theme(void) {
	static theme_palette_t p;
	return &p;
}
void theme_register_refresh(theme_refresh_cb_t fn) { (void)fn; }
void fonts_register_change(void (*fn)(void)) { (void)fn; }
bool switcher_back_drag_active(void) { return dragging; }
bool player_sheet_drag_active(void) { return dragging; }
bool switcher_in_history(lv_obj_t *s) {
	(void)s;
	return false;
}
void switcher_attach_back_gesture(lv_obj_t *s) { (void)s; }
void switcher_set_back_guard(lv_obj_t *s, bool (*fn)(void)) {
	(void)s;
	(void)fn;
}
void switcher_set_back_guard_peek(lv_obj_t *s, bool (*fn)(void)) {
	(void)s;
	(void)fn;
}
void switch_screen(lv_obj_t *s) { lv_screen_load(s); }
void back_btn_cb(lv_event_t *e) {
	(void)e;
	if (!back())
		lv_screen_load(screen);
}
void gui_notify_popup(const char *s) {
	notices++;
	snprintf(message, sizeof(message), "%s", s);
}
void toast_success(const char *s) {
	successes++;
	snprintf(message, sizeof(message), "%s", s);
}
void popover_show(lv_obj_t *o, const popover_item_t *items, int n) {
	(void)o;
	(void)items;
	(void)n;
}
void animpark_watch(lv_obj_t *o, animpark_cb_t p, animpark_cb_t u) {
	(void)o;
	(void)p;
	(void)u;
}
bool animpark_is_parked(const lv_obj_t *o) {
	(void)o;
	return false;
}
const char *tr(const char *s) {
	if (!strcmp(s, "nd_dl_changed"))
		return "The saved file is missing or changed. Download it again. Existing files are kept.";
	if (!strcmp(s, "nd_dl_changed_short"))
		return "Missing or changed file";
	if (!strcmp(s, "nd_dl_summary"))
		return "%d saved · %d failed · %d paused · %d skipped · %d jobs";
	if (!strcmp(s, "nd_dl_batch_track"))
		return "Batch %lld · %s";
	if (!strcmp(s, "nd_dl_admitted"))
		return "%d queued; %d skipped.";
	return s;
}
void nd_wipe(void *p, size_t n) { memset(p, 0, n); }
bool navidrome_context(nd_context_t *out, bool network) {
	(void)network;
	*out = life;
	return available;
}
bool navidrome_playback_current(const nd_context_t *c, bool network) {
	(void)network;
	return available && c->profile_epoch == life.profile_epoch && c->card_epoch == life.card_epoch;
}
void navidrome_library_snapshot(nd_library_t *out) { *out = lib; }
void navidrome_favorites_snapshot(nd_library_t *out) { *out = lib; }
void device_state_get(device_state_t *out) { *out = playing; }
bool navidromeplay_track(const char *p, nd_playrow_t *row, nd_library_t *library_out) {
	if (!nd_audio_source(p))
		return false;
	memset(row, 0, sizeof(*row));
	snprintf(row->row.id, sizeof(row->row.id), "%s", current_id);
	strcpy(row->album_id, "exact/album?");
	strcpy(row->row.title, "Current track");
	if (library_out)
		*library_out = lib;
	return true;
}
bool __wrap_config_save(void) { return !fail_save; }
void navidromedownloads_poll(void) {}
void navidromedownloads_view(nd_dl_view_t *out) { *out = *response; }
bool navidromedownloads_take_saved(nd_library_t *out) {
	if (!saved_notice)
		return false;
	saved_notice = false;
	*out = lib;
	return true;
}
bool library_scan_running(void) { return scan_busy; }
bool library_is_open(void) { return true; }
bool library_detect_changes(void) { return detect; }
int library_track_count(void) { return 100; }
bool library_card_returned(const char *root) {
	assert(!strcmp(root, "/tmp/card"));
	updates++;
	return true;
}
void browser_refresh(void) { refreshes++; }
bool navidromedownloads_request(const nd_dl_request_t *q, nd_dl_captured_cb cb, void *user) {
	if (!allow_request || !available)
		return false;
	admissions++;
	admitted = *q;
	callback = cb;
	callback_user = user;
	memset(response, 0, sizeof(*response));
	response->token = q->token;
	response->offset = q->offset;
	response->filter = q->filter;
	snprintf(response->folder, sizeof(response->folder), "%s", q->folder);
	if (q->action == NDDL_MKDIR)
		assert(snprintf(response->folder, sizeof(response->folder), "%s/%s", q->folder, q->name) < (int)sizeof(response->folder));
	response->profile_epoch = life.profile_epoch;
	response->card_epoch = life.card_epoch;
	strcpy(response->scope, lib.scope);
	if (q->action == NDDL_CLEAR_HISTORY)
		history_cleared = true;
	response->total = history_cleared && (q->filter == NDDL_SUCCESS || q->filter == NDDL_FAILURE) ? 0 : q->filter == NDDL_PENDING ? 17000 : q->filter == NDDL_SUCCESS || q->filter == NDDL_FAILURE ? 2 : 17004;
	response->skipped = 3;
	response->count = q->offset + ND_WINDOW <= response->total ? ND_WINDOW : response->total - q->offset;
	for (int i = 0; i < response->count; i++) {
		nd_dl_job_t *j = &response->jobs[i];
		j->key = q->offset + i + 1;
		j->batch = 1;
		j->state = q->filter == NDDL_SUCCESS ? NDDL_COMPLETE : q->filter == NDDL_FAILURE ? NDDL_FAILED : NDDL_QUEUED;
		if (j->key % 2)
			snprintf(j->source.row.cover, sizeof(j->source.row.cover), "cover/%lld", (long long)j->key);
		if (q->filter == NDDL_FAILURE)
			strcpy(j->error, "nd_dl_changed");
		j->bytes = 1572864;
		j->total = 3145728;
		snprintf(j->source.row.title, sizeof(j->source.row.title), "Track %lld", (long long)j->key);
		strcpy(j->destination, "Captured/Old destination");
	}
	response->counts[NDDL_COMPLETE] = 2;
	response->counts[NDDL_FAILED] = 1;
	response->counts[NDDL_CANCELLED] = 1;
	response->counts[NDDL_QUEUED] = 16999;
	response->counts[NDDL_PAUSED] = 1;
	if (history_cleared) {
		response->counts[NDDL_COMPLETE] = response->counts[NDDL_FAILED] = response->counts[NDDL_CANCELLED] = 0;
	}
	response->folder_count = 2;
	response->folder_total = 2;
	strcpy(response->folders[0], "Music");
	strcpy(response->folders[1], "Albums");
	return true;
}
void frameprof_begin(const char *t) { (void)t; }
void frameprof_end(const char *t) { (void)t; }
bool frameprof_timing(void) { return false; }
unsigned long long frameprof_now_us(void) { return 0; }
void frameprof_timer_ran(void *fn, unsigned us) {
	(void)fn;
	(void)us;
}
void frameprof_event_ran(void *fn, unsigned us) {
	(void)fn;
	(void)us;
}
static void assert_cover_rendered(nd_thumb_t *thumb) {
	lv_obj_update_layout(thumb->image);
	lv_draw_buf_t *snapshot = lv_snapshot_take(thumb->image, LV_COLOR_FORMAT_ARGB8888);
	assert(snapshot);
	int green = 0;
	for (unsigned y = 0; y < snapshot->header.h; y++)
		for (unsigned x = 0; x < snapshot->header.w; x++) {
			const unsigned char *pixel = snapshot->data + y * snapshot->header.stride + x * 4;
			green += pixel[1] > 200 && pixel[0] < 30 && pixel[2] < 30 && pixel[3] > 200;
		}
	assert(green >= thumb->size * thumb->size * 95 / 100);
	assert(lv_image_get_scale_x(thumb->image) == LV_SCALE_NONE);
	lv_draw_buf_destroy(snapshot);
}
static void paint(void) {
	lv_tick_inc(210);
	tick(NULL);
	lv_obj_update_layout(lv_screen_active());
}
static void click(lv_obj_t *o) { lv_obj_send_event(o, LV_EVENT_CLICKED, NULL); }
static void polish_header_geometry(gui_config_t *config) {
	lv_obj_t *page = lv_obj_create(NULL);
	lv_obj_t *title = settingsrow_title(page, config, "A very long album and artist name with translated characters é ñ ü");
	lv_obj_t *controls[2] = {lv_btn_create(page), lv_btn_create(page)};
	for (int i = 0; i < 2; i++) {
		lv_obj_set_size(controls[i], ui_px(56), ui_px(56));
		lv_obj_align(controls[i], LV_ALIGN_TOP_RIGHT, -config->padding - i * ui_px(62), config->top_bar_height);
		lv_obj_set_hidden(controls[i], true);
	}
	nd_row_title(title, config, controls, 2);
	int full = lv_obj_get_style_width(title, 0);
	assert(full == (int)config->screen_width - (int)config->padding - lv_obj_get_x(title));
	lv_obj_set_hidden(controls[0], false);
	nd_row_title(title, config, controls, 2);
	int single = lv_obj_get_style_width(title, 0);
	assert(single < full);
	lv_obj_set_hidden(controls[1], false);
	nd_row_title(title, config, controls, 2);
	assert(lv_obj_get_style_width(title, 0) < single);
	lv_label_set_text(title, "Short");
	lv_obj_set_hidden(controls[0], true);
	lv_obj_set_hidden(controls[1], true);
	nd_row_title(title, config, controls, 2);
	assert(lv_obj_get_style_width(title, 0) == full);
	assert(lv_label_get_long_mode(title) == LV_LABEL_LONG_DOT);
	lv_obj_delete(page);
	puts("production title fitting: hidden/one/two controls, dynamic text and restored full width: passed");
}
int main(int argc, char **argv) {
	alarm(30);
	int width = argc > 1 ? atoi(argv[1]) : 320, height = width == 320 ? 480 : 800;
	ui_panel_width = width;
	lv_init();
	lv_display_create(width, height);
	font_ui_14 = font_ui_16 = font_ui_18 = font_ui_20 = font_ui_22 = font_ui_24 = font_ui_24_bold = font_ui_26 = font_ui_28 = font_ui_32 = lv_font_unscii_8;
	config_init(NULL);
	lv_style_set_image_recolor(&theme_style_icon, lv_color_white());
	lv_style_set_image_recolor_opa(&theme_style_icon, LV_OPA_COVER);
	response = calloc(1, sizeof(*response));
	assert(response);
	lib.ready = lib.writable = true;
	lib.card_epoch = 1;
	memset(lib.scope, 'a', 32);
	strcpy(lib.path, "/tmp/card/.local/navidrome/.cache/library.db");
	gui_config_t config = {.screen_width = width, .screen_height = height, .padding = ui_px(12), .top_bar_height = ui_px(24)};
	polish_header_geometry(&config);
	navidromedownloadpage_init(&config);
	navidromedownloadpage_open();
	paint();
	assert(result.count == ND_WINDOW && result.total == 17004 && strstr(lv_label_get_text(summary), "3 skipped"));
	assert(lv_obj_get_child_count(manager_body) == 4);
	lv_obj_t *manage_row = lv_obj_get_child(manager_body, 3);
	bool chevron = false;
	for (unsigned i = 0; i < lv_obj_get_child_count(manage_row); i++) {
		lv_obj_t *child = lv_obj_get_child(manage_row, i);
		if (lv_obj_check_type(child, &lv_image_class) && lv_image_get_src(child) == &icon_chevron_right)
			chevron = true;
	}
	assert(chevron && lv_obj_get_user_flag(manager, 3) && lv_obj_get_user_flag(picker, 3));
	manager_open_cb(NULL);
	paint();
	assert(result.filter == NDDL_PENDING && result.total == 17000);
	assert(art_requests && thumbnails[0].bound && !thumbnails[0].cover.pixels);
	assert(lv_obj_get_width(thumbnails[0].image) == ui_px(32));
	assert(lv_image_get_src(thumbnails[0].image) == &icon_music_note);
	assert(lv_obj_get_width(thumbnails[0].frame) == ui_px(ND_ART_THUMB_SIZE));
	art_ready = true;
	paint();
	assert_cover_rendered(&thumbnails[0]);
	assert(lv_obj_get_width(thumbnails[1].image) == ui_px(32));
	void *pixels = thumbnails[0].cover.pixels;
	unsigned requests_before = art_requests, releases_before = art_releases;
	assert(strstr(lv_label_get_text(job_sizes[0]), "1.50 / 3.00 MiB"));
	assert(!lv_obj_has_state(header_actions[0], LV_STATE_DISABLED) && !lv_obj_has_state(header_actions[1], LV_STATE_DISABLED));
	request(NDDL_VIEW);
	assert(loading && !lv_obj_has_state(header_actions[0], LV_STATE_DISABLED) && !lv_obj_has_state(header_actions[1], LV_STATE_DISABLED));
	assert(thumbnails[0].cover.pixels == pixels && art_requests == requests_before && art_releases == releases_before);
	paint();
	// A visible overlap owns the same thumbnail even when its logical slot moves.
	lv_obj_t *kept_row = jobs[12];
	void *kept_pixels = thumbnails[12].cover.pixels;
	assert(kept_pixels);
	lv_obj_scroll_to_y(job_list, 12 * row_pitch, LV_ANIM_OFF);
	manager_scroll(NULL);
	paint();
	int kept = 12 - result.offset;
	assert(kept >= 0 && kept < result.count);
	assert(jobs[kept] == kept_row && thumbnails[kept].cover.pixels == kept_pixels);
	assert_cover_rendered(&thumbnails[kept]);
	lv_obj_scroll_to_y(job_list, 0, LV_ANIM_OFF);
	manager_scroll(NULL);
	paint();
	unsigned edge_admissions = admissions, edge_requests = art_requests;
	for (int bounce = 0; bounce < 5; bounce++) {
		lv_obj_scroll_by(job_list, 0, row_pitch / 2, LV_ANIM_OFF);
		manager_scroll(NULL);
		assert(admissions == edge_admissions && art_requests == edge_requests);
		assert_cover_rendered(&thumbnails[0]);
		lv_obj_scroll_by(job_list, 0, -row_pitch / 2, LV_ANIM_OFF);
	}
	for (int i = 0; i < 3; i++) {
		lv_area_t a;
		lv_obj_get_coords(header_actions[i], &a);
		lv_obj_t *glyph = lv_obj_get_child(header_actions[i], 0);
		const lv_image_dsc_t *source = lv_image_get_src(glyph);
		assert(source == (i == 0 ? &icon_play_small : i == 1 ? &icon_pause_small : &icon_clear));
		assert(lv_obj_get_width(glyph) == source->header.w && lv_obj_get_height(glyph) == source->header.h);
		assert(lv_obj_get_width(glyph) <= lv_obj_get_width(header_actions[i]));
		assert(lv_image_get_scale_x(glyph) == LV_SCALE_NONE);
		assert(a.x1 >= ui_px(56) && a.x2 < width);
		if (i) {
			lv_area_t b;
			lv_obj_get_coords(header_actions[i - 1], &b);
			assert(b.x2 < a.x1);
		}
	}
	lib.writable = false;
	paint();
	assert(lv_obj_has_state(header_actions[0], LV_STATE_DISABLED) && !lv_obj_is_hidden(jobs[0]));
	lib.writable = true;
	paint();
	allow_request = false;
	click(tabs[1]);
	paint();
	assert(manager_waiting && lv_obj_is_hidden(jobs[0]));
	click(tabs[2]);
	allow_request = true;
	paint();
	assert(result.filter == NDDL_FAILURE && selected_tab == 2);
	assert(!strcmp(lv_label_get_text(job_locations[0]), "Missing or changed file"));
	unsigned taps_before = admissions;
	device_state_t before_tap = playing;
	click(jobs[0]);
	assert(admissions == taps_before && !memcmp(&playing, &before_tap, sizeof(playing)));
	assert(details_visible() && strstr(lv_label_get_text(details_text), tr("nd_dl_changed")) && strstr(lv_label_get_text(details_text), "Captured/Old destination"));
	assert(details_back() && !details_visible());
	for (size_t c = 0; c < sizeof(response->jobs[0].source.row.title) - 1; c++)
		response->jobs[0].source.row.title[c] = c % 3 ? 'x' : '\n';
	response->jobs[0].source.row.title[sizeof(response->jobs[0].source.row.title) - 1] = 0;
	memset(response->jobs[0].path, 'x', sizeof(response->jobs[0].path) - 1);
	response->jobs[0].path[sizeof(response->jobs[0].path) - 1] = 0;
	paint();
	click(jobs[0]);
	lv_obj_update_layout(manager);
	assert(strstr(lv_label_get_text(details_text), response->jobs[0].path));
	lv_obj_t *details_body = lv_obj_get_parent(details_text);
	assert(lv_obj_get_scroll_bottom(details_body) > 0);
	assert(details_back());
	dragging = true;
	click(jobs[0]);
	assert(!details_visible());
	dragging = false;
	click(tabs[1]);
	paint();
	assert(result.filter == NDDL_SUCCESS && result.total == 2 && result.count == 2);
	assert(!strcmp(lv_label_get_text(tab_counts[0]), "17000"));
	assert(!strcmp(lv_label_get_text(tab_counts[1]), "2"));
	click(tabs[2]);
	paint();
	assert(result.filter == NDDL_FAILURE && result.total == 2);
	click(header_actions[0]);
	assert(admitted.action == NDDL_RETRY && admitted.filter == NDDL_FAILURE);
	paint();
	click(tabs[0]);
	paint();
	click(header_actions[1]);
	assert(admitted.action == NDDL_PAUSE);
	paint();
	click(header_actions[0]);
	assert(admitted.action == NDDL_RESUME);
	paint();
	click(header_actions[2]);
	assert(admitted.action == NDDL_CANCEL);
	paint();
	lv_obj_scroll_to_y(job_list, 5000 * row_pitch, LV_ANIM_OFF);
	manager_scroll(NULL);
	paint();
	assert(result.offset == 4998 && result.count == ND_WINDOW);
	lv_obj_scroll_to_y(job_list, 17000 * row_pitch, LV_ANIM_OFF);
	manager_scroll(NULL);
	paint();
	assert(result.offset > 16970 && result.count <= ND_WINDOW && result.count > 0);
	assert(!lv_obj_is_hidden(jobs[result.count - 1]));
	assert(strstr(lv_label_get_text(job_locations[0]), "Captured/Old destination"));
	assert(lv_obj_get_width(jobs[0]) <= width);
	assert(!lv_obj_is_hidden(clear_history) && lv_obj_get_width(clear_history) == ui_px(72));
	lv_area_t fab;
	lv_obj_get_coords(clear_history, &fab);
	assert(fab.x1 >= 0 && fab.y1 >= 0 && fab.x2 < width && fab.y2 < height);
	unsigned clear_before = admissions;
	dragging = true;
	click(clear_history);
	assert(admissions == clear_before && !clearing);
	dragging = false;
	lib.writable = false;
	paint();
	click(clear_history);
	assert(admissions == clear_before && lv_obj_has_state(clear_history, LV_STATE_DISABLED));
	lib.writable = true;
	paint();
	clear_before = admissions;
	allow_request = false;
	click(clear_history);
	assert(clearing && clear_pending && admissions == clear_before && !details_visible());
	allow_request = true;
	paint();
	assert(admitted.action == NDDL_CLEAR_HISTORY && !clearing && !clear_pending && !details_visible());
	assert(lv_obj_is_hidden(clear_history) && result.counts[NDDL_QUEUED] == 16999);
	click(tabs[1]);
	paint();
	assert(result.total == 0 && lv_obj_is_hidden(jobs[0]));
	click(tabs[0]);
	paint();
	// Ordinary job taps show copied details only; there is no player call.
	click(jobs[0]);
	assert(details_visible());
	life.profile_epoch++;
	response->busy = true;
	paint();
	assert(!details_visible());
	life.profile_epoch--;
	for (int i = 0; i < ND_WINDOW; i++)
		assert(!thumbnails[i].bound && !thumbnails[i].cover.pixels);
	// Return to settings before choosing a folder.
	navidromedownloadpage_open();
	paint();
	assert(!details_visible());
	for (int i = 0; i < ND_WINDOW; i++)
		assert(!thumbnails[i].bound && !thumbnails[i].cover.pixels);
	allow_request = false;
	click(lv_obj_get_child(manager_body, 0));
	paint();
	assert(folder_waiting && loading && lv_obj_has_state(use_folder, LV_STATE_DISABLED));
	allow_request = true;
	paint();
	assert(!folder_waiting && !loading);
	assert(lv_screen_active() == picker && !strcmp(folder, "Music/Navidrome"));
	assert(admitted.action == NDDL_PREPARE_FOLDER);
	assert(!lv_obj_has_state(use_folder, LV_STATE_DISABLED) && !lv_obj_has_state(new_folder, LV_STATE_DISABLED));
	lv_area_t use_box, list_box;
	lv_obj_get_coords(use_folder, &use_box);
	lv_obj_get_coords(picker_body, &list_box);
	assert(list_box.y2 < use_box.y1 && use_box.y2 < height);
	assert(!lv_obj_get_style_shadow_width(use_folder, LV_PART_MAIN));
	assert(!lv_obj_get_style_shadow_width(new_folder, LV_PART_MAIN));
	assert(!lv_obj_get_style_shadow_width(folders[0], LV_PART_MAIN));
	assert(lv_obj_is_event_bubble(folders[0]) && lv_obj_is_event_bubble(picker_body));
	for (int i = 0; i < 2; i++) {
		lv_obj_t *button = lv_obj_get_child(picker_pages, i);
		assert(!lv_obj_get_style_shadow_width(button, LV_PART_MAIN));
		lv_obj_add_state(button, LV_STATE_PRESSED);
		assert(!lv_obj_get_style_shadow_width(button, LV_PART_MAIN));
		lv_obj_remove_state(button, LV_STATE_PRESSED);
	}
	new_cb(NULL);
	lv_obj_update_layout(picker);
	assert(naming && keyboard_is_visible(name_keyboard));
	lv_area_t field_box, key_box;
	lv_obj_get_coords(name_field, &field_box);
	lv_obj_get_coords(keyboard_obj(name_keyboard), &key_box);
	fprintf(stderr, "keyboard geometry field=%d,%d,%d,%d keyboard=%d,%d,%d,%d panel=%dx%d\n", field_box.x1, field_box.y1, field_box.x2, field_box.y2, key_box.x1, key_box.y1, key_box.x2, key_box.y2, width, height);
	assert(field_box.y2 < key_box.y1 && key_box.y2 < height && field_box.x1 >= 0 && field_box.x2 < width);
	lv_textarea_set_text(name_field, "A new folder");
	name_accept(NULL);
	paint();
	assert(admitted.action == NDDL_MKDIR && !strcmp(admitted.name, "A new folder") && !naming);
	assert(!strcmp(folder, "Music/Navidrome/A new folder"));
	new_cb(NULL);
	lv_textarea_set_text(name_field, "discard");
	assert(back());
	assert(!naming && !keyboard_is_visible(name_keyboard));
	assert(back());
	paint();
	assert(!strcmp(folder, "Music/Navidrome"));
	assert(back());
	paint();
	assert(!strcmp(folder, "Music"));
	assert(back());
	paint();
	assert(!folder[0]);
	unsigned saves = successes;
	fail_save = true;
	use_cb(NULL);
	assert(successes == saves && lv_screen_active() == picker);
	fail_save = false;
	use_cb(NULL);
	assert(successes == saves + 1 && lv_screen_active() == screen);
	assert(!strcmp(config_get("navidrome", "download_folder", "bad"), ""));
	pick_cb(NULL);
	paint();
	cancel_picker(NULL);
	assert(lv_screen_active() == screen && !strcmp(config_get("navidrome", "download_folder", "bad"), ""));
	pick_cb(NULL);
	paint();
	strcpy(response->error, "nd_dl_readonly");
	paint();
	assert(lv_obj_has_state(use_folder, LV_STATE_DISABLED) && lv_obj_has_state(new_folder, LV_STATE_DISABLED));
	unsigned before = admissions;
	dragging = true;
	new_cb(NULL);
	assert(!naming && admissions == before);
	dragging = false;
	response->profile_epoch = 0;
	paint();
	assert(result.count == 0 && result.folder_count == 0);
	before = admissions;
	life.profile_epoch++;
	assert(!request(NDDL_MKDIR) && admissions == before);
	assert(!strcmp(result.error, "nd_dl_unavailable"));
	navidromedownloadpage_open();
	available = false;
	paint();
	assert(result.count == 0);
	available = true;
	strcpy(playing.current_file, "/tmp/card/Music/Navidrome/same.flac");
	nd_audio_set_source(NULL);
	assert(!navidromedownloadmenu_bind(playing.current_file));
	nd_audio_set_source(playing.current_file);
	assert(navidromedownloadmenu_bind(playing.current_file));
	before = admissions;
	assert(admissions == before);
	navidromedownloadmenu_activate(NULL);
	assert(admissions == before + 1 && admitted.direct && !strcmp(admitted.id, current_id) && !strcmp(admitted.track.album_id, "exact/album?"));
	nd_audio_set_source(NULL);
	before = admissions;
	navidromedownloadmenu_activate(NULL);
	assert(admissions == before);
	nd_audio_set_source(playing.current_file);
	assert(navidromedownloadmenu_bind(playing.current_file));
	strcpy(current_id, "replacement");
	navidromedownloadmenu_activate(NULL);
	assert(admissions == before);
	assert(navidromedownloadmenu_bind(playing.current_file));
	life.profile_epoch++;
	navidromedownloadmenu_activate(NULL);
	assert(admissions == before);
	assert(navidromedownloadmenu_bind(playing.current_file));
	navidromedownloadmenu_activate(NULL);
	assert(admissions == before + 1);
	assert(callback);
	unsigned toasts = successes;
	nd_audio_set_source(NULL);
	callback(true, NULL, 1, 0, callback_user);
	assert(successes == toasts);
	saved_notice = true;
	scan_busy = true;
	paint();
	assert(discovery_pending && !updates);
	scan_busy = false;
	paint();
	assert(updates == 1 && !discovery_pending);
	detect = false;
	saved_notice = true;
	paint();
	assert(updates == 1 && !detect);
	printf("PASS actual Downloads LVGL rows, shared File Explorer picker, production icon bounds, rendered/retained covers, scrollable details-only row taps, immediate history-only FAB, three filtered tabs/MiB, 17,000-job scrolling windows, shared keyboard, folder root/save/discard/read-only, stale results, discovery policy and same-file Now Playing source contexts at %dx%d\n", width, height);
	return 0;
}

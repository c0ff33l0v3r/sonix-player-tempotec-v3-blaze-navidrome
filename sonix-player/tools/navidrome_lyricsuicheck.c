// Production player artwork state and rendered pixels; no audio/network device.
#include "src/gui/nowplaying/player.c"
void fixture_close_sheet(bool animate);
#define player_sheet_close fixture_close_sheet
#include "src/gui/nowplaying/lyricsearch.c"
#undef player_sheet_close
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
LV_FONT_DECLARE(lv_font_unscii_8);
static nd_playrow_t fixture_track;
static nd_library_t fixture_library;
static bool resolved = true, preparing = true, delivered;
static unsigned requests;
static int color;
int32_t ui_panel_width = 320;
lv_style_t theme_style_icon, theme_style_text, theme_style_text_dim;
const theme_palette_t *theme(void) {
	static theme_palette_t p;
	return &p;
}
void back_btn_over_cover(bool on) { (void)on; }
void back_btn_player_below_bar(bool on) { (void)on; }
int back_btn_centre_y(void) { return 20; }
void topbar_set_hidden(bool on) { (void)on; }
void topbar_set_over_artwork(bool on, bool studio) {
	(void)on;
	(void)studio;
}
bool navidromeplay_track(const char *path, nd_playrow_t *row, nd_library_t *lib) {
	(void)path;
	if (!resolved)
		return false;
	*row = fixture_track;
	*lib = fixture_library;
	return true;
}
bool navidromeplay_reference(const char *path) { return path && !strncmp(path, "ndq:", 4); }
bool navidromeplay_preparing(void) { return preparing; }
bool playlist_has_source(void) { return true; }
void navidromeart_release(int slot) {
	(void)slot;
	delivered = false;
}
void navidromeart_request_player(const nd_library_t *lib, const char *id, int size, int w, int h) {
	(void)lib;
	(void)size;
	(void)w;
	(void)h;
	requests++;
	color = *id == 'b' ? 1 : *id == 'c' ? 2 : 0;
	delivered = false;
}
static void pixels(cover_image_t *out, int w, int h, int channel) {
	*out = (cover_image_t){0};
	out->pixels = calloc((size_t)w * h, 4);
	assert(out->pixels);
	for (int i = 0; i < w * h; i++) {
		out->pixels[4 * i + (2 - channel)] = 255;
		out->pixels[4 * i + 3] = 255;
	}
	out->dsc = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = w, .h = h, .stride = w * 4}, .data = out->pixels, .data_size = w * h * 4};
	out->has_tone = true;
	out->tone = channel == 1 ? 0x00ff00 : channel == 2 ? 0x0000ff : 0xff0000;
}
bool navidromeart_take_player(cover_image_t *out, cover_image_t *back, bool *finished) {
	*finished = delivered;
	if (!delivered || !fixture_track.row.cover[0])
		return false;
	pixels(out, 64, 64, color);
	pixels(back, 64, 32, color);
	return true;
}
void coverloader_request_player(const char *p, int w, int h, int bw, int bh) {
	(void)p;
	(void)w;
	(void)h;
	(void)bw;
	(void)bh;
}
void coverloader_request_player_id(const char *p) { (void)p; }
bool coverloader_take_player_id(uint64_t *id, bool *finished) {
	*id = 0;
	*finished = true;
	return false;
}
bool coverloader_take_player(cover_image_t *a, cover_image_t *b, bool *f) {
	(void)a;
	(void)b;
	*f = true;
	return false;
}
bool gui_post(void (*f)(void *), void *p) {
	(void)f;
	(void)p;
	return false;
}
static void displayed_pixels(int channel) {
	lv_obj_t *image = layout_studio_now ? studio_cover : cover_img;
	assert(!lv_obj_is_hidden(image));
	lv_draw_buf_t *frame = lv_snapshot_take(image, LV_COLOR_FORMAT_ARGB8888);
	assert(frame && frame->header.w == 64 && frame->header.h == 64);
	for (unsigned y = 0; y < frame->header.h; y++)
		for (unsigned x = 0; x < frame->header.w; x++) {
			const uint8_t *pixel = frame->data + y * frame->header.stride + x * 4;
			assert(pixel[2 - channel] == 255 && pixel[3] == 255);
		}
	lv_draw_buf_destroy(frame);
}
// Keep unrelated lyrics blur creation outside this focused rendering fixture.
int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *u) {
	(void)t;
	(void)a;
	(void)f;
	(void)u;
	return EAGAIN;
}
static device_state_t fixture_state;
static unsigned lyric_requests, lyric_network;
static nd_lyric_action_t last_action;
static nd_lyric_done_cb lyric_done;
static void *lyric_user;
static nd_lyric_target_t lyric_target;
static bool held;
static double sought = -1;
static bool accessible = true;
static unsigned backs, wifi_toggles;
static bool reject_submit;
static bool wifi_on = true, wifi_connected = true;
static int captured_choice;
bool nd_lyrics_target(const char *reference, nd_lyric_target_t *out) {
	memset(out, 0, sizeof(*out));
	if (!accessible || !reference || !*reference)
		return false;
	snprintf(out->id, sizeof(out->id), "%s", reference);
	snprintf(out->reference, sizeof(out->reference), "%s", reference);
	out->context.profile_epoch = 1;
	out->context.card_epoch = 1;
	out->queue = 1;
	return true;
}
bool nd_lyrics_same(const nd_lyric_target_t *a, const nd_lyric_target_t *b) { return a->queue == b->queue && !strcmp(a->id, b->id) && a->context.profile_epoch == b->context.profile_epoch && a->context.card_epoch == b->context.card_epoch; }
unsigned nd_lyrics_submit(const nd_lyric_target_t *t, nd_lyric_action_t a, const char *text, int choice, nd_lyric_done_cb done, void *u) {
	(void)text;
	if (reject_submit)
		return 0;
	captured_choice = choice;
	lyric_target = *t;
	last_action = a;
	lyric_requests++;
	lyric_network += a != ND_LYRIC_LOCAL;
	lyric_done = done;
	lyric_user = u;
	return lyric_requests;
}
void nd_lyrics_cancel(unsigned token) { (void)token; }
void power_hold_screen_for_view(bool on) { held = on; }
static void injected(const char *text) {
	nd_lyric_result_t result = {.target = lyric_target, .action = last_action};
	assert(lyrics_parse_text(text, &result.set.versions[0].lyrics));
	result.set.count = 1;
	lyric_done(&result, lyric_user);
	nd_lyrics_free(&result.set);
	lyrics_fill_now();
}
static lv_point_t touch_point;
static bool pressed;
static void read_touch(lv_indev_t *input, lv_indev_data_t *data) {
	(void)input;
	data->point = touch_point;
	data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static void touch_step(lv_indev_t *input, int x, int y, bool down) {
	touch_point = (lv_point_t){x, y};
	pressed = down;
	lv_tick_inc(20);
	lv_indev_read(input);
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
	(void)area;
	(void)pixels;
	lv_display_flush_ready(display);
}

static lv_obj_t *label_text(lv_obj_t *parent, const char *text) {
	if (lv_obj_check_type(parent, &lv_label_class) && !strcmp(lv_label_get_text(parent), text))
		return parent;
	for (unsigned n = 0; n < lv_obj_get_child_count(parent); n++) {
		lv_obj_t *found = label_text(lv_obj_get_child(parent, n), text);
		if (found)
			return found;
	}
	return NULL;
}
static void answer(const char *label) {
	lv_obj_t *text = label_text(lv_layer_top(), label);
	assert(text && lv_obj_is_visible(text));
	lv_obj_send_event(lv_obj_get_parent(text), LV_EVENT_CLICKED, NULL);
	lv_tick_inc(10);
	lv_timer_handler();
}
static void dismiss_toast(void) {
	lv_obj_t *text = label_text(lv_layer_top(), "lyrics_downloading");
	if (!text || !lv_obj_is_visible(text))
		text = label_text(lv_layer_top(), "lyrics_wifi_connecting");
	assert(text);
	lv_obj_send_event(lv_obj_get_parent(lv_obj_get_parent(text)), LV_EVENT_CLICKED, NULL);
}
static void result_error(const char *error) {
	nd_lyric_result_t result = {.target = lyric_target, .action = last_action, .error = error};
	lyric_done(&result, lyric_user);
}
static void clear_feedback(void) {
	// Exercise production tap/timer dismissal rather than resetting feedback internals.
	toast_busy_end();
	confirm_close();
	lv_tick_inc(7000);
	lv_timer_handler();
}
static void owned_feedback_checks(void) {
	unsigned first = toast_busy_owned("first");
	assert(first && toast_busy_showing());
	toast_busy_dismissable("legacy");
	unsigned legacy = toast_revision();
	toast_busy_end_owned(first);
	assert(toast_busy_showing() && toast_revision() == legacy);
	toast_busy_end();
	assert(!toast_visible());
	const char *replacement[] = {"plain", "error", "busy"};
	for (unsigned n = 0; n < 3; n++) {
		first = toast_busy_owned("lyrics_downloading");
		if (n == 0)
			toast_plain(replacement[n]);
		else if (n == 1)
			toast_error(replacement[n]);
		else
			toast_busy(replacement[n]);
		unsigned revision = toast_revision();
		toast_busy_end_owned(first);
		assert(!toast_busy_update_owned(first, "wrong") && toast_visible() && toast_revision() == revision);
		clear_feedback();
	}
	first = toast_busy_owned("lyrics_downloading");
	dismiss_toast();
	assert(!toast_visible() && toast_revision() == first && !toast_busy_update_owned(first, "wrong"));
	unsigned prompt = confirm_show_owned("first", NULL, "ok", NULL, NULL, NULL);
	confirm_show("second", NULL, "ok", NULL, NULL);
	confirm_close_owned(prompt);
	assert(confirm_visible());
	confirm_close();
	prompt = confirm_show_owned("first", NULL, "ok", NULL, NULL, NULL);
	confirm_close_owned(prompt);
	assert(!confirm_visible());
}

static void visible_visit(void) {
	static unsigned serial;
	clear_feedback();
	lv_screen_load(player_screen);
	char ref[80];
	snprintf(ref, sizeof(ref), "ndq:feedback:%u", ++serial);
	strcpy(fixture_state.current_file, ref);
	sheet_open = true;
	lyrics_wanted = lyrics_allowed = lyrics_now = true;
	lyrics_progress = LYRICS_FULL;
	wifi_on = wifi_connected = true;
	lyrics_request(&fixture_state);
	assert(nd_lyrics_visit_open && nd_lyrics_lookup_pending && !confirm_visible());
	assert(!lv_obj_is_hidden(lyrics_note) && !strcmp(lv_label_get_text(lyrics_note), "nd_lyrics_loading"));
}
static void valid_search_results(void) {
	nd_lyric_result_t result = {.target = lyric_target, .action = ND_LYRIC_SEARCH, .count = 2};
	result.records = calloc(2, sizeof(*result.records));
	assert(result.records);
	for (int n = 0; n < 2; n++) {
		strcpy(result.records[n].track, "Found");
		result.records[n].text = strdup("found lyrics");
	}
	lyric_done(&result, lyric_user);
	assert(!result.records && !result.count);
}
static void lyrics_feedback_checks(void) {
	const char *misses[] = {"lyrics_not_found", "nd_lyrics_unsupported", "nd_lyrics_offline", "nd_api_error", "nd_lyrics_limit", "nd_lyrics_storage"};
	for (int studio = 0; studio < 2; studio++) {
		layout_studio_now = studio;
		for (unsigned n = 0; n < sizeof(misses) / sizeof(*misses); n++) {
			visible_visit();
			unsigned before = lyric_requests;
			result_error(misses[n]);
			assert(confirm_visible() && nd_lyrics_offer_done && lyric_requests == before);
			assert(!strcmp(lv_label_get_text(lyrics_note), misses[n]));
			lv_draw_buf_t *shot = lv_snapshot_take(lv_layer_top(), LV_COLOR_FORMAT_ARGB8888);
			assert(shot);
			lv_draw_buf_destroy(shot);
			answer("cancel");
			assert(!confirm_visible());
			for (int k = 0; k < 3; k++)
				lyrics_request(&fixture_state);
			assert(!confirm_visible() && lyric_requests == before);
			nd_lyrics_action(ND_LYRIC_REFRESH);
			result_error(misses[n]);
			assert(!confirm_visible()); // refresh/reconnect/poll cannot repeat a claimed offer
			nd_lyrics_maybe_offer();
			assert(!confirm_visible());
		}
		// Explicit close/reopen grants a new visit; a cached hit never offers.
		lyrics_commit(false);
		lyrics_commit(true);
		assert(nd_lyrics_lookup_pending);
		result_error("lyrics_not_found");
		assert(confirm_visible());
		answer("cancel");
		visible_visit();
		injected("Saved words");
		assert(!confirm_visible());
		nd_lyrics_action(ND_LYRIC_REFRESH);
		result_error("nd_api_error");
		assert(lyrics_cur.count && !confirm_visible());
		clear_feedback();
		toast_busy("unrelated refresh feedback");
		unsigned refresh_feedback = toast_revision();
		nd_lyrics_action(ND_LYRIC_REFRESH);
		result_error("nd_api_error");
		assert(toast_busy_showing() && toast_revision() == refresh_feedback);
		clear_feedback();
		// An unrelated visible card delays the offer, without forgetting its reason.
		visible_visit();
		toast_busy("other work");
		unsigned other = toast_revision();
		result_error("lyrics_not_found");
		assert(!confirm_visible() && toast_revision() == other);
		toast_busy_end();
		nd_lyrics_maybe_offer();
		assert(confirm_visible());
		confirm_show("other question", NULL, "ok", NULL, NULL);
		nd_lyrics_retire();
		assert(confirm_visible());
		confirm_close();
		// A deferred miss cannot reappear after another target's pending lookup.
		visible_visit();
		toast_plain("other feedback");
		result_error("lyrics_not_found");
		assert(!confirm_visible());
		strcpy(fixture_state.current_file, "ndq:new-target");
		lyrics_request(&fixture_state);
		clear_feedback();
		nd_lyrics_maybe_offer();
		assert(!confirm_visible());
		// A click already queued when A changes to B cannot search either track.
		visible_visit();
		result_error("lyrics_not_found");
		lv_obj_t *search = label_text(lv_layer_top(), "lyrics_download");
		lv_obj_send_event(lv_obj_get_parent(search), LV_EVENT_CLICKED, NULL);
		strcpy(fixture_state.current_file, "ndq:changed-before-confirm");
		lyrics_request(&fixture_state);
		unsigned before = lyric_requests;
		lv_tick_inc(10);
		lv_timer_handler();
		assert(lyric_requests == before && last_action == ND_LYRIC_VIEW);
		// Pending search is single, dismissible, and can still deliver its results.
		visible_visit();
		result_error("lyrics_not_found");
		answer("lyrics_download");
		assert(nd_lyrics_search_pending && toast_busy_showing());
		unsigned visit = nd_lyrics_visit;
		before = lyric_requests;
		lyrics_search_start();
		lyrics_search_start();
		assert(lyric_requests == before);
		dismiss_toast();
		assert(!toast_visible());
		lyrics_request(&fixture_state);
		assert(!toast_visible());
		valid_search_results();
		assert(lv_screen_active() == lyricsearch_screen && !sheet_open && nd_lyrics_visit == visit);
		assert(record_count == 2 && !toast_busy_showing());
		// Round trip retains the offer claim even though the player sheet closed.
		unloaded_cb(NULL);
		lv_screen_load(player_screen);
		sheet_open = true;
		lyrics_request(&fixture_state);
		result_error("lyrics_not_found");
		assert(!confirm_visible() && nd_lyrics_visit == visit && nd_lyrics_offer_done);
		// Failed/empty/instrumental results always end their own card.
		const char *errors[] = {"lyrics_not_found", "lyrics_instrumental", "lyrics_download_failed", "nd_lyrics_limit"};
		for (unsigned n = 0; n < sizeof(errors) / sizeof(*errors); n++) {
			visible_visit();
			injected("existing words");
			lyrics_search_start();
			assert(toast_busy_showing());
			result_error(errors[n]);
			assert(!toast_busy_showing() && !nd_lyrics_search_intent);
			assert(label_text(lv_layer_top(), errors[n]));
			clear_feedback();
		}
		// Old completion cannot dismiss or replace unrelated busy/plain/error feedback.
		for (int n = 0; n < 3; n++) {
			visible_visit();
			injected("existing words");
			lyrics_search_start();
			if (n == 0)
				toast_busy("unrelated");
			else if (n == 1)
				toast_plain("unrelated");
			else
				toast_error("unrelated");
			other = toast_revision();
			result_error("lyrics_download_failed");
			assert(toast_visible() && toast_revision() == other && label_text(lv_layer_top(), "unrelated"));
			clear_feedback();
		}
		visible_visit();
		injected("words");
		reject_submit = true;
		lyrics_search_start();
		reject_submit = false;
		assert(!nd_lyrics_search_pending && !nd_lyrics_search_intent && !toast_busy_showing());
		clear_feedback();
		// Retire before completion and ensure its callback cannot navigate or affect a new visit.
		visible_visit();
		injected("words");
		lyrics_search_start();
		nd_lyric_done_cb old_done = lyric_done;
		void *old_user = lyric_user;
		nd_lyric_result_t stale = {.target = lyric_target, .action = ND_LYRIC_SEARCH, .error = "lyrics_download_failed"};
		lyrics_commit(false);
		assert(!toast_busy_showing());
		visible_visit();
		other = toast_revision();
		old_done(&stale, old_user);
		assert(nd_lyrics_lookup_pending && !confirm_visible() && !toast_busy_showing() && toast_revision() == other && lv_screen_active() == player_screen);
		// Inaccessible profile/card revokes the prompt and stale action.
		result_error("lyrics_not_found");
		assert(confirm_visible());
		accessible = false;
		lyrics_request(&fixture_state);
		assert(!confirm_visible());
		accessible = true;
#ifndef HOST_BUILD
		// Device waiting uses owned progress and respects tap dismissal/timeout/invalidation.
		visible_visit();
		injected("words");
		wifi_on = false;
		wifi_connected = false;
		lyrics_search_start();
		assert(confirm_visible());
		answer("lyrics_enable_wifi");
		assert(lyrics_wifi_timer && toast_busy_showing() && !nd_lyrics_search_pending);
		before = lyric_requests;
		lyrics_search_start();
		assert(lyric_requests == before);
		dismiss_toast();
		wifi_connected = true;
		lyrics_wifi_tick_cb(NULL);
		assert(nd_lyrics_search_pending && !toast_visible());
		result_error("lyrics_not_found");
		clear_feedback();
		visible_visit();
		injected("words");
		wifi_on = false;
		wifi_connected = false;
		lyrics_search_start();
		answer("lyrics_enable_wifi");
		lv_tick_inc(LYRICS_WIFI_WAIT_MS + 1);
		lyrics_wifi_tick_cb(NULL);
		assert(!lyrics_wifi_timer && !nd_lyrics_search_intent && !toast_busy_showing());
		clear_feedback();
		visible_visit();
		injected("words");
		wifi_on = false;
		lyrics_search_start();
		assert(confirm_visible());
		confirm_show("replacement", NULL, "ok", NULL, NULL);
		lyrics_request(&fixture_state);
		confirm_close();
		lyrics_search_start();
		assert(confirm_visible());
		answer("cancel");
#endif
	}
	layout_studio_now = false;
	clear_feedback();
	nd_lyrics_retire();
	// Music keeps its normal list delivery and outcome feedback after adopting ownership.
	nd_lyrics_active = false;
	strcpy(fixture_state.current_file, "local.flac");
	sheet_open = true;
	lyrics_fetch_t *music = calloc(1, sizeof(*music));
	assert(music);
	strcpy(music->path, "local.flac");
	music->result = LRCLIB_OK;
	music->count = 1;
	music->records = calloc(1, sizeof(*music->records));
	assert(music->records);
	music->records[0].text = strdup("Music words");
	strcpy(music->records[0].track, "Music");
	lyrics_fetch_busy = toast_busy_owned("lyrics_downloading");
	lyrics_fetching = true;
	lyrics_fetched(music);
	assert(lv_screen_active() == lyricsearch_screen && record_count == 1 && !nd_active && !toast_busy_showing());
	unloaded_cb(NULL);
	lv_screen_load(player_screen);
	sheet_open = true;
	music = calloc(1, sizeof(*music));
	assert(music);
	strcpy(music->path, "local.flac");
	music->result = LRCLIB_NOT_FOUND;
	lyrics_fetch_busy = toast_busy_owned("lyrics_downloading");
	lyrics_fetching = true;
	lyrics_fetched(music);
	assert(!toast_busy_showing() && toast_visible() && label_text(lv_layer_top(), "lyrics_not_found"));
	clear_feedback();
}

int main(int argc, char **argv) {
	alarm(30);
	ui_panel_width = argc > 1 ? atoi(argv[1]) : 320;
	int height = argc > 2 ? atoi(argv[2]) : 480;
	lv_init();
	lv_display_t *display = lv_display_create(ui_panel_width, height);
	font_ui_14 = font_ui_20 = font_ui_22 = font_ui_24 = font_ui_26 = lv_font_unscii_8;
	gui_config_t feedback_cfg = {.screen_width = ui_panel_width, .screen_height = height};
	confirm_init(&feedback_cfg);
	toast_init(&feedback_cfg);
	player_screen = lv_obj_create(NULL);
	lv_screen_load(player_screen);
	cover_panel = lv_obj_create(player_screen);
	lv_obj_set_size(cover_panel, ui_panel_width, height);
	navidrome_badge = lv_obj_create(cover_panel);
	lv_obj_set_size(navidrome_badge, 20, 20);
	player_set_statusbar_always(true);
	lv_obj_update_layout(player_screen);
	assert(lv_obj_get_style_align(navidrome_badge, 0) == LV_ALIGN_BOTTOM_LEFT);
	studio_badges_move(cover_panel, 50);
	lv_obj_update_layout(player_screen);
	assert(lv_obj_get_style_align(navidrome_badge, 0) == LV_ALIGN_BOTTOM_LEFT);
	assert(lv_obj_get_x(navidrome_badge) >= 0 && lv_obj_get_x(navidrome_badge) < ui_panel_width / 2);
	assert(lv_obj_get_y(navidrome_badge) > height / 2);
	player_set_statusbar_always(false);
	lyrics_pane = lv_obj_create(cover_panel);
	lv_obj_set_size(lyrics_pane, ui_panel_width, height);
	lyrics_view = lv_obj_create(lyrics_pane);
	lv_obj_set_size(lyrics_view, ui_panel_width, height - 100);
	lv_obj_set_flex_flow(lyrics_view, LV_FLEX_FLOW_COLUMN);
	lyrics_note = lv_label_create(lyrics_pane);
	lyrics_head_title = lv_label_create(player_screen);
	lyrics_head_artist = lv_label_create(player_screen);
	lyrics_view_h = height - 100;
	cover_box_w = ui_panel_width;
	lv_obj_remove_style_all(cover_panel);
	lv_obj_set_size(cover_panel, ui_panel_width, height);
	lv_obj_set_pos(cover_panel, 0, 0);
	lv_obj_set_scrollable(cover_panel, false);
	lv_obj_set_hidden(lyrics_pane, true);
	lv_obj_add_event_cb(cover_panel, lyrics_drag_cb, LV_EVENT_PRESSED, (void *)1);
	lv_obj_add_event_cb(cover_panel, lyrics_drag_cb, LV_EVENT_PRESSING, (void *)1);
	lv_obj_add_event_cb(cover_panel, lyrics_drag_cb, LV_EVENT_RELEASED, (void *)1);
	uint8_t *pixels = malloc(ui_panel_width * 16 * 4);
	assert(pixels);
	lv_display_set_buffers(display, pixels, NULL, ui_panel_width * 16 * 4, LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(display, flush);
	lv_indev_t *pointer = lv_indev_create();
	lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(pointer, read_touch);

	owned_feedback_checks();
	strcpy(fixture_state.current_file, "ndq:1:0");
	lyrics_request(&fixture_state);
	assert(lyric_requests == 1 && !lyric_network && last_action == ND_LYRIC_LOCAL);
	sheet_open = true;
	lv_obj_update_layout(player_screen);
	touch_step(pointer, ui_panel_width - 30, 100, true);
	for (int x = ui_panel_width - 40; x > 25; x -= 20)
		touch_step(pointer, x, 100, true);
	touch_step(pointer, 25, 100, false);
	if (!(lyrics_wanted && lyrics_now && lyric_network == 1))
		fprintf(stderr, "swipe wanted=%d now=%d requests=%u network=%u progress=%d\n", lyrics_wanted, lyrics_now, lyric_requests, lyric_network, (int)lyrics_progress);
	assert(lyrics_wanted && lyrics_now && lyric_network == 1);
	injected("[00:01.000]First line\n[00:02.500]Second line");
	assert(lv_obj_get_child_count(lyrics_view) == 2);
	assert(!strcmp(lv_label_get_text(lv_obj_get_child(lyrics_view, 0)), "First line"));
	sheet_open = true;
	lyrics_wanted = true;
	lyrics_allowed = true;
	lyrics_now = true;
	lyrics_request(&fixture_state);
	assert(last_action == ND_LYRIC_VIEW && lyric_network == 1);
	injected("[00:01.000]First line\n[00:02.500]Second line");
	for (int studio = 0; studio < 2; studio++) {
		layout_studio_now = studio;
		preparing = false;
		lyrics_follow(2.6);
		assert(lyrics_lit == 1);
		lv_obj_update_layout(player_screen);
		lv_draw_buf_t *snapshot = lv_snapshot_take(lyrics_view, LV_COLOR_FORMAT_ARGB8888);
		assert(snapshot && snapshot->header.w);
		lv_draw_buf_destroy(snapshot);
	}
	// Real pointer taps use production seek guards and retain millisecond timing.
	layout_studio_now = false;
	preparing = false;
	lyrics_tap_ms = -1;
	lv_tick_inc(300);
	lv_timer_handler();
	lyrics_progress_apply(LYRICS_FULL);
	lv_obj_add_event_cb(lyrics_view, lyrics_tap_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(lyrics_view, lyrics_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
	lv_obj_set_scroll_dir(lyrics_view, LV_DIR_VER);
	lv_obj_update_layout(player_screen);
	lv_area_t first;
	lv_obj_get_coords(lv_obj_get_child(lyrics_view, 0), &first);
	int tap_x = (first.x1 + first.x2) / 2, tap_y = (first.y1 + first.y2) / 2;
	touch_step(pointer, tap_x, tap_y, true);
	touch_step(pointer, tap_x, tap_y, false);
	assert(sought == 1.0);
	preparing = true;
	sought = -1;
	touch_step(pointer, tap_x, tap_y, true);
	touch_step(pointer, tap_x, tap_y, false);
	assert(sought < 0);
	preparing = false;
	lyrics_screen_hold_update();
	assert(held);
	sheet_open = false;
	lyrics_screen_hold_update();
	assert(!held);
	unsigned count = lyric_network;
	lyrics_request(&fixture_state);
	assert(lyric_network == count);
	// A retired A result cannot populate B, including before B is loaded.
	nd_lyric_result_t stale = {.target = lyric_target, .action = ND_LYRIC_VIEW};
	assert(lyrics_parse_text("wrong track", &stale.set.versions[0].lyrics));
	stale.set.count = 1;
	strcpy(fixture_state.current_file, "ndq:1:1");
	sheet_open = true;
	lyrics_request(&fixture_state);
	nd_lyrics_arrived(&stale, NULL);
	nd_lyrics_free(&stale.set);
	assert(!lyrics_cur.count);
	injected("Plain lyrics\n\nSecond verse");
	assert(!lyrics_cur.synced && lv_obj_get_child_count(lyrics_view) == 3);
#ifdef HOST_BUILD
	// Host networking remains usable when the device WLAN controls are off.
	nd_lyrics_menu_target = nd_lyrics_current;
	wifi_on = false;
	unsigned before_search = lyric_requests, toggles = wifi_toggles;
	nd_lyrics_search_action(NULL);
	assert(last_action == ND_LYRIC_SEARCH && lyric_requests == before_search + 1);
	assert(!confirm_visible() && !lyrics_wifi_timer && wifi_toggles == toggles && toast_busy_showing());
#else
	// Real device confirmation and owned Wi-Fi/search progress stay scoped to the target.
	nd_lyrics_menu_target = nd_lyrics_current;
	wifi_on = false;
	unsigned before_search = lyric_requests;
	nd_lyrics_search_action(NULL);
	assert(confirm_visible() && lyric_requests == before_search);
	answer("lyrics_enable_wifi");
	assert(wifi_on && lyrics_wifi_timer);
	lyrics_wifi_tick_cb(NULL);
	assert(last_action == ND_LYRIC_SEARCH && lyric_requests == before_search + 1 && toast_busy_showing());
	result_error("lyrics_not_found");
	clear_feedback();
	wifi_on = false;
	nd_lyrics_search_action(NULL);
	unsigned toggles = wifi_toggles;
	lv_obj_t *enable = label_text(lv_layer_top(), "lyrics_enable_wifi");
	lv_obj_send_event(lv_obj_get_parent(enable), LV_EVENT_CLICKED, NULL);
	strcpy(fixture_state.current_file, "ndq:retired");
	lv_tick_inc(10);
	lv_timer_handler();
	assert(wifi_toggles == toggles);
	nd_lyrics_retire();

#endif
	strcpy(fixture_state.current_file, nd_lyrics_current.reference);
	sheet_open = true;
	nd_lyrics_menu_target = nd_lyrics_current;
	nd_lyrics_server_action(NULL);
	assert(last_action == ND_LYRIC_SERVER);
	unsigned before_stale = lyric_requests;
	strcpy(fixture_state.current_file, "ndq:retired");
	nd_lyrics_server_action(NULL);
	assert(lyric_requests == before_stale);
	strcpy(fixture_state.current_file, nd_lyrics_current.reference);
	// Exercise the production picker controls and callback contract.
	lyricsearch_screen = lv_obj_create(NULL);
	gui_config_t cfg = {.screen_width = ui_panel_width, .screen_height = height, .padding = 8, .top_bar_height = 20};
	lyricsearch_init(&cfg);
	nd_lyric_target_t captured;
	assert(nd_lyrics_target("track-a", &captured));
	lrclib_record_t *options = calloc(1, sizeof(*options));
	assert(options);
	options[0].text = strdup("[00:01.000]chosen");
	options[0].synced = true;
	strcpy(options[0].track, "Track A");
	lyricsearch_open_navidrome(&captured, options, 1, false);
	lv_obj_send_event(lv_obj_get_child(list, 0), LV_EVENT_CLICKED, NULL);
	assert(previewing == 0);
	strcpy(fixture_state.current_file, "track-b");
	lv_obj_send_event(apply_button, LV_EVENT_CLICKED, NULL);
	assert(nd_applying && last_action == ND_LYRIC_APPLY && !strcmp(lyric_target.id, "track-a"));
	unsigned submitted = lyric_requests;
	lv_obj_send_event(apply_button, LV_EVENT_CLICKED, NULL);
	assert(lyric_requests == submitted);
	nd_lyric_result_t failed = {.target = captured, .action = ND_LYRIC_APPLY, .error = "nd_lyrics_storage"};
	lyric_done(&failed, lyric_user);
	assert(!nd_applying && previewing == 0 && !backs);
	lv_obj_send_event(apply_button, LV_EVENT_CLICKED, NULL);
	assert(nd_applying);
	nd_lyric_result_t success = {.target = captured, .action = ND_LYRIC_APPLY};
	lyric_done(&success, lyric_user);
	assert(backs == 1 && !nd_applying && toast_visible());
	// No successful save for A may retarget or reload B.
	assert(lyric_requests == submitted + 1);
	unloaded_cb(NULL);
	assert(!records && !nd_active);
	// Server alternatives reuse the same picker, submitting selection rather than an override.
	options = calloc(2, sizeof(*options));
	assert(options);
	for (int i = 0; i < 2; i++) {
		options[i].text = strdup(i ? "alternate" : "first");
		strcpy(options[i].track, "Server lyric");
	}
	lyricsearch_open_navidrome(&captured, options, 2, true);
	lv_obj_send_event(lv_obj_get_child(list, 1), LV_EVENT_CLICKED, NULL);
	lv_obj_send_event(apply_button, LV_EVENT_CLICKED, NULL);
	assert(last_action == ND_LYRIC_SELECT && captured_choice == 1);
	unsigned prior_backs = backs;
	assert(back_guard());
	lyric_done(&success, lyric_user);
	assert(backs == prior_backs);
	accessible = false;
	nd_picker_tick(NULL);
	assert(!nd_active && !records);
	accessible = true;
	unloaded_cb(NULL);
	lyrics_feedback_checks();
	lv_tick_inc(50);
	lv_timer_handler();
	lyrics_free(&lyrics_cur);
	nd_lyrics_free(&nd_lyrics_versions);
	lv_indev_delete(pointer);
	lv_display_delete(display);
	free(pixels);
	lv_deinit();
	puts("production lyrics: real pointer swipe before load, rendered timed/plain lines, normal/Studio following, hidden demand, screen holds, scoped Wi-Fi/server actions, picker retry/skip/back/invalidation, owned prompts/spinners, miss offers, duplicate/dismissal/timeout/late delivery passed");
	return 0;
}

lv_font_t font_ui_14;
lv_font_t font_ui_16;
lv_font_t font_ui_18;
lv_font_t font_ui_20;
lv_font_t font_ui_20_bold;
lv_font_t font_ui_22;
lv_font_t font_ui_22_bold;
lv_font_t font_ui_24;
lv_font_t font_ui_24_bold;
lv_font_t font_ui_26;
lv_font_t font_ui_28;
lv_font_t font_ui_32;
lv_font_t font_ui_36_bold;
lv_font_t font_ui_64_bold;
lv_font_t font_ui_72;
void thread_set_background_level(bool on) { (void)on; }
playback_mode_t playlist_get_mode(void) { return PLAYBACK_MODE_NORMAL; }
int audiobook_speed_permille(void) { return 1000; }
int podcast_speed_permille(void) { return 1000; }
void device_state_get(device_state_t *out) { *out = fixture_state; }
bool navidromestar_player_update(lv_obj_t *b, lv_obj_t *i, const char *p, lv_color_t c) {
	(void)b;
	(void)i;
	(void)p;
	(void)c;
	return true;
}
bool radio_current_station(radio_station_t *out) {
	(void)out;
	return false;
}
bool radio_fav_contains(const char *id) {
	(void)id;
	return false;
}
bool podcastcache_feed_id(const char *p, long long *out) {
	(void)p;
	(void)out;
	return false;
}
bool podcastsubs_is_followed(long long id) {
	(void)id;
	return false;
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
	return false;
}
bool qobuzsync_is_favorite(long id) {
	(void)id;
	return false;
}
void tidalsync_favorites_refresh(void (*done)(void)) { (void)done; }
void qobuzsync_favorites_refresh(void (*done)(void)) { (void)done; }
bool device_state_prefetched_fav(const char *p, bool *v) {
	(void)p;
	(void)v;
	return false;
}
bool library_fav_contains(const char *p) {
	(void)p;
	return false;
}
bool nd_audio_source(const char *p) {
	(void)p;
	return true;
}
void frameprof_begin(const char *p) { (void)p; }
void frameprof_end(const char *p) { (void)p; }

const char *tr(const char *s) { return s; }
bool audiobook_is_playing(void) { return false; }
void airplay_get_state(airplay_state_t *s) { memset(s, 0, sizeof(*s)); }
bool qobuzcache_owns(const char *p) {
	(void)p;
	return false;
}
bool tidalcache_owns(const char *p) {
	(void)p;
	return false;
}
bool podcastcache_is_episode(const char *p) {
	(void)p;
	return false;
}
bool dlna_owns_path(const char *p) {
	(void)p;
	return false;
}
bool dlna_owns_playback(void) { return false; }
audio_status_t audio_get_status(void) { return AUDIO_STATUS_PLAYING; }
void scrolltext_set(lv_obj_t *o, const char *s) { lv_label_set_text(o, s); }


void lrclib_free(lrclib_record_t *r, int n) {
	if (!r)
		return;
	for (int i = 0; i < n; i++)
		free(r[i].text);
	free(r);
}

void audiobookextras_open_current_summary(void) {}
void audiobookextras_open_current_bookmarks(void) {}
void audiobookextras_add_bookmark(void) {}
void chapters_open(void) {}
void gui_notify_popup(const char *s) { (void)s; }
bool wifi_get_enabled(void) { return wifi_on; }
bool wifi_available(void) { return true; }
void wifi_set_enabled(bool on) {
	wifi_on = on;
	wifi_toggles++;
}
void wifi_get_status(wifi_status_t *s) {
	memset(s, 0, sizeof(*s));
	if (wifi_connected) {
		s->state = WIFI_STATE_CONNECTED;
		strcpy(s->ip, "127.0.0.1");
	}
}
lrclib_result_t lrclib_search(const char *t, const char *a, const char *b, int d, lrclib_record_t **r, int *n) {
	(void)t;
	(void)a;
	(void)b;
	(void)d;
	*r = NULL;
	*n = 0;
	return LRCLIB_NOT_FOUND;
}

void fixture_close_sheet(bool animate) {
	(void)animate;
	sheet_open = false;
	if (nd_lyrics_active && !nd_lyrics_picker_transfer)
		nd_lyrics_retire();
}
bool switcher_back_drag_active(void) { return false; }
void back_btn_cb(lv_event_t *e) {
	(void)e;
	backs++;
}
void switch_screen(lv_obj_t *o) { lv_screen_load(o); }
void switcher_set_player_return(lv_obj_t *o) { (void)o; }
void switcher_attach_back_gesture(lv_obj_t *o) { (void)o; }
void switcher_set_back_guard(lv_obj_t *o, bool (*f)(void)) {
	(void)o;
	(void)f;
}
void switcher_set_back_guard_peek(lv_obj_t *o, bool (*f)(void)) {
	(void)o;
	(void)f;
}
lv_obj_t *settingsrow_title(lv_obj_t *screen, gui_config_t *cfg, const char *key) {
	(void)cfg;
	lv_obj_t *o = lv_label_create(screen);
	lv_label_set_text(o, key);
	return o;
}
int settingsrow_content_top(gui_config_t *cfg) { return cfg->top_bar_height + 40; }
void settingsrow_title_corner_slots(lv_obj_t *o, gui_config_t *cfg, int count) {
	(void)o;
	(void)cfg;
	(void)count;
}
void nd_lyrics_poll(void) {}
bool navidrome_playback_current(const nd_context_t *c, bool net) {
	(void)c;
	(void)net;
	return accessible;
}
bool lrclib_save(const char *path, const char *text) {
	(void)path;
	(void)text;
	return false;
}
lv_style_t theme_style_card, theme_style_card_pressed, theme_style_screen, theme_style_accent_bg, theme_style_switch;

void device_state_seek(double seconds) { sought = seconds; }

void theme_register_refresh(void (*f)(void)) { (void)f; }
void animpark_watch(lv_obj_t *o, void (*stop)(lv_obj_t *), void (*start)(lv_obj_t *)) {
	(void)o;
	(void)stop;
	(void)start;
}

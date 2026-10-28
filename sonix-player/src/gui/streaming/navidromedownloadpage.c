#include "navidromedownloadpage.h"
#include "navidromepage.h"
#include "navidromerow.h"
#include "navidromethumb.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/browser.h"
#include "src/gui/library/folderpicker.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/uiscale.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/library/library.h"
#include "src/system/streaming/navidromedownloads.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static gui_config_t *cfg;
static lv_obj_t *screen, *manager, *picker, *manager_body, *picker_body, *summary, *folder_value, *folder_label, *picker_status, *use_folder, *new_folder;
static lv_obj_t *picker_pages, *job_list, *job_extent, *job_status, *tabs[3], *tab_counts[3], *header_actions[3], *clear_history;
static lv_obj_t *jobs[ND_WINDOW], *job_titles[ND_WINDOW], *job_notes[ND_WINDOW], *job_sizes[ND_WINDOW], *job_locations[ND_WINDOW], *folders[ND_WINDOW];
static nd_thumb_t thumbnails[ND_WINDOW];
static lv_obj_t *details_layer, *details_text;
static lv_obj_t *name_layer, *name_field;
static keyboard_t *name_keyboard;
static lv_timer_t *timer;
static nd_dl_view_t result;
static unsigned token, picker_profile, picker_card, manager_profile, manager_card;
static bool loading, naming, folder_waiting, manager_waiting, painting, clearing, clear_pending;
static unsigned clear_token;
static int selected_tab, row_pitch;
static nd_dl_action_t waiting_action;
static int first;
static char folder[ND_DL_PATH];
static uint32_t submitted, refreshed;
static nd_library_t discovery;
static bool discovery_pending;
static bool gesture(void) { return switcher_back_drag_active(); }
static bool request(nd_dl_action_t action);
static void repaint(void);
static void manager_open_cb(lv_event_t *e);
static void manager_scroll(lv_event_t *e);
static void details_close(lv_event_t *e);
static const char *state_label(int state) {
	static const char *keys[] = {"nd_dl_queued", "nd_dl_running", "nd_dl_validating", "nd_dl_registering", "nd_dl_complete", "nd_dl_paused", "nd_dl_failed", "nd_dl_cancelled", "nd_dl_waiting"};
	return state >= 0 && state < 9 ? keys[state] : "nd_dl_failed";
}
static nd_library_t library(void) {
	nd_library_t lib;
	navidrome_library_snapshot(&lib);
	if (!lib.scope[0])
		navidrome_favorites_snapshot(&lib);
	return lib;
}
static void action_cb(lv_event_t *e) {
	if (!gesture() && !clearing)
		request((nd_dl_action_t)(uintptr_t)lv_event_get_user_data(e) == NDDL_RESUME && selected_tab == 2 ? NDDL_RETRY : (nd_dl_action_t)(uintptr_t)lv_event_get_user_data(e));
}
static bool request(nd_dl_action_t action) {
	nd_dl_request_t q = {.action = action, .library = library(), .token = token + 1, .offset = first, .filter = lv_screen_active() == manager ? (nd_dl_filter_t)(selected_tab + 1) : NDDL_ALL};
	nd_context_t context;
	bool picking = action == NDDL_FOLDERS || action == NDDL_MKDIR || action == NDDL_PREPARE_FOLDER;
	bool managing = lv_screen_active() == manager;
	bool available = navidrome_context(&context, false) && context.card_epoch == q.library.card_epoch;
	if (picking) {
		available = available && context.profile_epoch == picker_profile && context.card_epoch == picker_card;
		q.profile_epoch = picker_profile;
	}
	if (managing) {
		available = available && context.profile_epoch == manager_profile && context.card_epoch == manager_card;
		q.profile_epoch = manager_profile;
	}
	nd_wipe(&context, sizeof(context));
	if (!available) {
		folder_waiting = manager_waiting = clearing = clear_pending = false;
		memset(&result, 0, sizeof(result));
		snprintf(result.error, sizeof(result.error), "nd_dl_unavailable");
		loading = false;
		repaint();
		return false;
	}
	snprintf(q.folder, sizeof(q.folder), "%s", folder);
	if (action == NDDL_MKDIR)
		snprintf(q.name, sizeof(q.name), "%s", lv_textarea_get_text(name_field));
	if (navidromedownloads_request(&q, NULL, NULL)) {
		token = q.token;
		if (action == NDDL_CLEAR_HISTORY) {
			clear_token = q.token;
			clear_pending = false;
		}
		folder_waiting = manager_waiting = false;
		loading = true;
		if (timer)
			lv_timer_resume(timer);
		submitted = lv_tick_get();
		if (picking)
			lv_obj_scroll_to_y(picker_body, 0, LV_ANIM_OFF);
		repaint();
		return true;
	} else {
		if (action == NDDL_CLEAR_HISTORY)
			return false; // Retry busy admission without a confirmation or lost tap.
		if (action == NDDL_FOLDERS || action == NDDL_PREPARE_FOLDER) {
			// Another manager request may still own the worker. Keep the
			// latest navigation intent and reject its predecessor's result.
			token = q.token;
			folder_waiting = loading = true;
			waiting_action = action;
			submitted = lv_tick_get();
			repaint();
			return false;
		}
		if (managing && action == NDDL_VIEW) {
			token = q.token;
			manager_waiting = loading = true;
			submitted = lv_tick_get();
			repaint();
			return false;
		}
		loading = false;
		if (action != NDDL_VIEW)
			gui_notify_popup("nd_working");
		return false;
	}
}
static void pick_cb(lv_event_t *e) {
	(void)e;
	if (gesture())
		return;
	nd_context_t context;
	bool available = navidrome_context(&context, false);
	picker_profile = available ? context.profile_epoch : 0;
	picker_card = available ? context.card_epoch : 0;
	nd_wipe(&context, sizeof(context));
	const char *saved = config_get("navidrome", "download_folder", "Music/Navidrome");
	snprintf(folder, sizeof(folder), "%s", saved);
	first = 0;
	loading = false;
	switch_screen(picker);
	memset(&result, 0, sizeof(result));
	request(NDDL_PREPARE_FOLDER);
}
static void cancel_picker(lv_event_t *e) {
	(void)e;
	if (gesture())
		return;
	folder[0] = 0;
	first = 0;
	loading = false;
	back_btn_cb(NULL);
}
static void use_cb(lv_event_t *e) {
	(void)e;
	if (gesture() || loading)
		return;
	nd_context_t context;
	bool current = navidrome_context(&context, false) && result.profile_epoch == context.profile_epoch && result.card_epoch == context.card_epoch && context.profile_epoch == picker_profile && context.card_epoch == picker_card;
	nd_wipe(&context, sizeof(context));
	if (!current) {
		gui_notify_popup("nd_dl_unavailable");
		return;
	}
	if (result.error[0] || !nd_dl_destination(folder)) {
		gui_notify_popup("nd_dl_path_error");
		return;
	}
	char old[ND_DL_DEST];
	snprintf(old, sizeof(old), "%s", config_get("navidrome", "download_folder", "Music/Navidrome"));
	config_set("navidrome", "download_folder", folder);
	if (!config_save()) {
		config_set("navidrome", "download_folder", old);
		gui_notify_popup("nd_dl_store_error");
		return;
	}
	lv_label_set_text(folder_value, folder[0] ? folder : tr("nd_dl_card_root"));
	folder[0] = 0;
	first = 0;
	back_btn_cb(NULL);
	toast_success("nd_dl_folder_saved");
}
static void folder_cb(lv_event_t *e) {
	if (gesture() || loading)
		return;
	int i = (int)(uintptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= result.folder_count)
		return;
	char next[512];
	int n = snprintf(next, sizeof(next), "%s%s%s", folder, folder[0] ? "/" : "", result.folders[i]);
	if (n < 0 || n >= (int)sizeof(next)) {
		gui_notify_popup("nd_dl_path_error");
		return;
	}
	snprintf(folder, sizeof(folder), "%s", next);
	first = 0;
	request(NDDL_FOLDERS);
}
static void name_accept(lv_event_t *e) {
	(void)e;
	if (gesture())
		return;
	if (!request(NDDL_MKDIR))
		return;
	naming = false;
	lv_obj_set_hidden(name_layer, true);
	keyboard_set_visible(name_keyboard, false);
}
static void new_cb(lv_event_t *e) {
	(void)e;
	if (gesture() || loading)
		return;
	naming = true;
	lv_textarea_set_text(name_field, "");
	keyboard_reset(name_keyboard);
	lv_obj_set_hidden(name_layer, false);
	keyboard_set_visible(name_keyboard, true);
	lv_obj_move_foreground(name_layer);
}
static bool back_peek(void) { return naming || folder[0]; }
static bool back(void) {
	if (naming) {
		naming = false;
		lv_obj_set_hidden(name_layer, true);
		keyboard_set_visible(name_keyboard, false);
		return true;
	}
	if (folder[0]) {
		char *slash = strrchr(folder, '/');
		if (slash)
			*slash = 0;
		else
			folder[0] = 0;
		first = 0;
		loading = false;
		request(NDDL_FOLDERS);
		return true;
	}
	return false;
}
static void page_cb(lv_event_t *e) {
	if (gesture() || loading)
		return;
	int direction = (int)(intptr_t)lv_event_get_user_data(e);
	int total = result.folder_total;
	if (direction > 0 && first + ND_WINDOW >= total)
		return;
	first += direction * ND_WINDOW;
	if (first < 0)
		first = 0;
	request(NDDL_FOLDERS);
}
static void page_state(lv_obj_t *row, int total) {
	for (int i = 0; i < 2; i++) {
		lv_obj_t *button = lv_obj_get_child(row, i);
		if (loading || (i ? first + ND_WINDOW >= total : !first))
			lv_obj_add_state(button, LV_STATE_DISABLED);
		else
			lv_obj_remove_state(button, LV_STATE_DISABLED);
	}
}
static void manager_paint(void) {
	painting = true;
	int totals[3] = {0, result.counts[NDDL_COMPLETE], result.counts[NDDL_FAILED] + result.counts[NDDL_CANCELLED]};
	for (int state = 0; state < 9; state++)
		if (state != NDDL_COMPLETE && state != NDDL_FAILED && state != NDDL_CANCELLED)
			totals[0] += result.counts[state];
	for (int i = 0; i < 3; i++) {
		char count[24];
		lv_snprintf(count, sizeof(count), "%d", totals[i]);
		lv_label_set_text(tab_counts[i], count);
		lv_obj_set_style_bg_color(tabs[i], i == selected_tab ? theme()->accent : theme()->surface, 0);
		for (int j = 0; j < 2; j++)
			lv_obj_set_style_text_color(lv_obj_get_child(tabs[i], j), i == selected_tab ? lv_color_white() : theme()->text_primary, 0);
	}
	// Periodic read-only refresh keeps controls stable. Only eligibility and
	// actual job-state changes affect them, not the worker's transient busy bit.
	bool unavailable = result.error[0] != 0;
	nd_library_t art_library = library();
	bool readonly = !art_library.writable;
	bool history = result.counts[NDDL_COMPLETE] + result.counts[NDDL_FAILED] + result.counts[NDDL_CANCELLED] > 0;
	lv_obj_set_hidden(clear_history, !history && !clearing);
	lv_obj_set_state(clear_history, LV_STATE_DISABLED, unavailable || readonly || clearing);
	lv_obj_set_style_pad_bottom(job_list, history || clearing ? ui_px(84) : 0, 0);
	lv_obj_set_state(header_actions[0], LV_STATE_DISABLED, unavailable || readonly || clearing || (selected_tab == 2 ? !result.counts[NDDL_FAILED] : !result.counts[NDDL_PAUSED]));
	lv_obj_set_state(header_actions[1], LV_STATE_DISABLED, unavailable || readonly || clearing || !(result.counts[NDDL_QUEUED] + result.counts[NDDL_RUNNING] + result.counts[NDDL_VALIDATING] + result.counts[NDDL_WAITING]));
	lv_obj_set_state(header_actions[2], LV_STATE_DISABLED, unavailable || readonly || clearing || !(totals[0] + result.counts[NDDL_FAILED]));
	bool matching = result.filter == (nd_dl_filter_t)(selected_tab + 1);
	lv_label_set_text(job_status, result.error[0] ? tr(result.error) : readonly ? tr("nd_dl_readonly") : !matching ? tr("nd_working") : !result.total ? tr("nd_library_empty") : "");
	int total = matching && !unavailable ? result.total : 0;
	if (total > INT_MAX / row_pitch) {
		total = 0;
		lv_label_set_text(job_status, tr("nd_dl_store_error"));
	}
	lv_obj_set_height(job_extent, LV_MAX(1, total * row_pitch));
	for (int i = 0; i < ND_WINDOW; i++) {
		bool visible = matching && !unavailable && i < result.count;
		lv_obj_set_hidden(jobs[i], !visible);
		if (!visible) {
			navidromethumb_release(&thumbnails[i]);
			continue;
		}
		nd_dl_job_t *job = &result.jobs[i];
		navidromethumb_bind(&thumbnails[i], &art_library, manager_profile, job->source.row.cover);
		lv_obj_set_y(jobs[i], (result.offset + i) * row_pitch);
		lv_label_set_text(job_titles[i], job->source.row.title);
		int64_t bytes = job->key == result.active_key ? result.bytes : job->bytes;
		int64_t length = job->key == result.active_key ? result.length : job->total;
		bytes = LV_MAX(0, bytes);
		length = LV_MAX(0, length);
		char size[96], location[ND_DL_PATH + 128];
		if (length > 0)
			lv_snprintf(size, sizeof(size), "%lld.%02lld / %lld.%02lld MiB", (long long)(bytes / 1048576), (long long)((bytes % 1048576) * 100 / 1048576), (long long)(length / 1048576), (long long)((length % 1048576) * 100 / 1048576));
		else
			lv_snprintf(size, sizeof(size), "%lld.%02lld MiB", (long long)(bytes / 1048576), (long long)((bytes % 1048576) * 100 / 1048576));
		lv_label_set_text(job_notes[i], tr(state_label(job->state)));
		lv_label_set_text(job_sizes[i], size);
		const char *destination = job->path[0] ? job->path : job->destination[0] ? job->destination : tr("nd_dl_card_root");
		lv_snprintf(location, sizeof(location), "%s", job->error[0] ? tr(!strcmp(job->error, "nd_dl_changed") ? "nd_dl_changed_short" : "nd_dl_error_details") : destination);
		lv_label_set_text(job_locations[i], location);
	}
	painting = false;
}
static void repaint(void) {
	if (lv_screen_active() == picker) {
		page_state(picker_pages, result.folder_total);
		lv_label_set_text(folder_label, folder[0] ? folder : tr("nd_dl_card_root"));
		lv_label_set_text(picker_status, loading ? tr("nd_working") : result.error[0] ? tr(result.error) : result.folder_count ? "" : tr("files_this_folder_is_empty"));
		lv_obj_set_hidden(picker_pages, result.folder_total <= ND_WINDOW);
		if (loading || result.error[0]) {
			lv_obj_add_state(use_folder, LV_STATE_DISABLED);
			lv_obj_add_state(new_folder, LV_STATE_DISABLED);
		} else {
			lv_obj_remove_state(use_folder, LV_STATE_DISABLED);
			lv_obj_remove_state(new_folder, LV_STATE_DISABLED);
		}
		for (int i = 0; i < ND_WINDOW; i++) {
			lv_obj_set_hidden(folders[i], loading || i >= result.folder_count);
			if (i < result.folder_count)
				lv_label_set_text(folderpicker_name_label(folders[i]), result.folders[i]);
		}
	} else if (lv_screen_active() == manager) {
		manager_paint();
	} else {
		const char *saved = config_get("navidrome", "download_folder", "Music/Navidrome");
		lv_label_set_text(folder_value, saved[0] ? saved : tr("nd_dl_card_root"));
		char text[400];
		int total = 0;
		for (int i = 0; i < 9; i++)
			total += result.counts[i];
		if (result.error[0])
			snprintf(text, sizeof(text), "%s", tr(result.error));
		else
			lv_snprintf(text, sizeof(text), tr("nd_dl_summary"), result.counts[NDDL_COMPLETE], result.counts[NDDL_FAILED], result.counts[NDDL_PAUSED], result.skipped, total);
		lv_label_set_text(summary, text);
	}
}

static void tick(lv_timer_t *unused) {
	(void)unused;
	navidromedownloads_poll();
	if (navidromedownloads_take_saved(&discovery))
		discovery_pending = true;
	if (discovery_pending && !library_scan_running()) {
		nd_library_t active = library();
		bool same = active.card_epoch == discovery.card_epoch && !strcmp(active.scope, discovery.scope);
		char root[512], path[1200];
		if (same && nd_dl_paths(&discovery, root, path)) {
			if (browser_screen && lv_screen_active() == browser_screen)
				browser_refresh();
			if (library_is_open() && library_detect_changes() && library_track_count() > 0)
				library_card_returned(root);
		}
		discovery_pending = false;
	}
	if (lv_screen_active() != screen && lv_screen_active() != picker && lv_screen_active() != manager) {
		loading = folder_waiting = manager_waiting = clearing = clear_pending = false;
		if (timer) {
			if (discovery_pending)
				lv_timer_resume(timer);
			else
				lv_timer_pause(timer);
		}
		return;
	}
	if (clear_pending && lv_screen_active() == manager)
		request(NDDL_CLEAR_HISTORY);
	if (folder_waiting)
		request(waiting_action);
	if (manager_waiting && !clearing)
		request(NDDL_VIEW);
	nd_dl_view_t *fresh = malloc(sizeof(*fresh));
	if (!fresh)
		return;
	navidromedownloads_view(fresh);
	nd_context_t context;
	bool available = navidrome_context(&context, false);
	bool manager_current = available && context.profile_epoch == manager_profile && context.card_epoch == manager_card;
	bool eligible = available && fresh->profile_epoch == context.profile_epoch && fresh->card_epoch == context.card_epoch && (lv_screen_active() != manager || manager_current);
	nd_wipe(&context, sizeof(context));
	if (lv_screen_active() == manager && !manager_current) {
		memset(&result, 0, sizeof(result));
		snprintf(result.error, sizeof(result.error), "nd_dl_unavailable");
		loading = manager_waiting = clearing = clear_pending = false;
		manager_paint();
	}
	if (fresh->token == token && !fresh->busy) {
		loading = false;
		if (clearing && !clear_pending && fresh->token == clear_token)
			clearing = false;
		if (!eligible && !fresh->error[0])
			snprintf(fresh->error, sizeof(fresh->error), "nd_dl_unavailable");
		if (!eligible) {
			fresh->count = fresh->folder_count = 0;
			memset(fresh->counts, 0, sizeof(fresh->counts));
		}
		if (lv_screen_active() == picker && !fresh->error[0]) {
			snprintf(folder, sizeof(folder), "%s", fresh->folder);
			first = fresh->offset;
		}
		if (lv_screen_active() == manager) {
			int map[ND_WINDOW];
			bool same = result.profile_epoch == fresh->profile_epoch && result.card_epoch == fresh->card_epoch && result.filter == fresh->filter;
			for (int i = 0; i < ND_WINDOW; i++) {
				map[i] = -1;
				if (same && i < fresh->count)
					for (int j = 0; j < result.count; j++)
						if (result.jobs[j].key > 0 && result.jobs[j].key == fresh->jobs[i].key) {
							map[i] = j;
							break;
						}
			}
			nd_row_complete_map(map);
			ND_ROW_REORDER(jobs, map);
			ND_ROW_REORDER(job_titles, map);
			ND_ROW_REORDER(job_notes, map);
			ND_ROW_REORDER(job_sizes, map);
			ND_ROW_REORDER(job_locations, map);
			ND_ROW_REORDER(thumbnails, map);
			for (int i = 0; i < ND_WINDOW; i++)
				if (thumbnails[i].cover.pixels)
					lv_image_set_src(thumbnails[i].image, &thumbnails[i].cover.dsc);
		}
		result = *fresh;
		repaint();
	}
	bool running = fresh->busy || fresh->armed;
	bool artwork_pending = false;
	free(fresh);
	if (lv_screen_active() == manager) {
		if (!eligible)
			details_close(NULL);
		else
			for (int i = 0; i < ND_WINDOW; i++) {
				navidromethumb_tick(&thumbnails[i]);
				artwork_pending |= !thumbnails[i].finished;
			}
	}
	if (lv_screen_active() == manager && !loading && !clearing)
		manager_scroll(NULL);
	if (loading && lv_tick_elaps(submitted) > 2500)
		loading = false;
	if (running && !loading && !naming && !clearing && (lv_screen_active() == screen || lv_screen_active() == manager) && lv_tick_elaps(refreshed) > 1000) {
		refreshed = lv_tick_get();
		request(NDDL_VIEW);
	}
	if (timer) {
		if (artwork_pending || running || loading || folder_waiting || manager_waiting || discovery_pending || clear_pending)
			lv_timer_resume(timer);
		else
			lv_timer_pause(timer);
	}
}
static void download_changed(void) { tick(NULL); }
void navidromedownloadpage_open(void) {
	first = 0;
	folder[0] = 0;
	loading = folder_waiting = manager_waiting = clearing = clear_pending = false;
	switch_screen(screen);
	request(NDDL_VIEW);
}
static lv_obj_t *note(lv_obj_t *parent) {
	lv_obj_t *label = lv_label_create(parent);
	lv_obj_set_width(label, lv_pct(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_18, 0);
	return label;
}
static lv_obj_t *pagination(lv_obj_t *body) {
	lv_obj_t *row = lv_obj_create(body);
	lv_obj_remove_style_all(row);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, ui_px(8), 0);
	for (int i = 0; i < 2; i++) {
		lv_obj_t *button = lv_btn_create(row);
		lv_obj_set_flex_grow(button, 1);
		lv_obj_set_height(button, ui_px(44));
		lv_obj_add_style(button, &theme_style_card, 0);
		lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_shadow_width(button, 0, 0);
		lv_obj_set_style_border_width(button, 0, 0);
		lv_obj_t *label = lv_label_create(button);
		lv_obj_add_style(label, &theme_style_text, 0);
		lv_obj_set_style_text_font(label, &font_ui_18, 0);
		lv_label_set_text(label, tr(i ? "nd_dl_next" : "nd_dl_previous"));
		lv_obj_center(label);
		lv_obj_add_event_cb(button, page_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(i ? 1 : -1));
	}
	return row;
}
static void manager_open_cb(lv_event_t *e) {
	(void)e;
	if (gesture())
		return;
	nd_context_t context;
	bool available = navidrome_context(&context, false);
	manager_profile = available ? context.profile_epoch : 0;
	manager_card = available ? context.card_epoch : 0;
	nd_wipe(&context, sizeof(context));
	selected_tab = first = 0;
	loading = folder_waiting = manager_waiting = clearing = clear_pending = false;
	memset(&result, 0, sizeof(result));
	switch_screen(manager);
	painting = true;
	lv_obj_scroll_to_y(job_list, 0, LV_ANIM_OFF);
	painting = false;
	request(NDDL_VIEW);
}
static void tab_cb(lv_event_t *e) {
	if (gesture() || clearing)
		return;
	int tab = (int)(intptr_t)lv_event_get_user_data(e);
	if (tab == selected_tab)
		return;
	selected_tab = tab;
	first = 0;
	painting = true;
	lv_obj_scroll_to_y(job_list, 0, LV_ANIM_OFF);
	painting = false;
	request(NDDL_VIEW);
}
static void manager_scroll(lv_event_t *e) {
	(void)e;
	if (painting || clearing || lv_screen_active() != manager)
		return;
	int top = nd_row_scroll(job_list, result.total * row_pitch) / row_pitch;
	int visible = lv_obj_get_height(job_list) / row_pitch + 2;
	if (result.filter == (nd_dl_filter_t)(selected_tab + 1) && top >= result.offset && LV_MIN(result.total, top + visible) <= result.offset + result.count)
		return;
	int wanted = nd_row_first(job_list, result.total, row_pitch, 0);
	if (wanted == first && loading)
		return;
	first = wanted;
	request(NDDL_VIEW);
}
static void details_close(lv_event_t *e) {
	(void)e;
	if (details_layer)
		lv_obj_set_hidden(details_layer, true);
}
static bool details_visible(void) { return details_layer && !lv_obj_is_hidden(details_layer); }
static bool details_back(void) {
	if (!details_visible())
		return false;
	details_close(NULL);
	return true;
}
static void manager_unloaded(lv_event_t *e) {
	(void)e;
	clearing = clear_pending = false;
	details_close(NULL);
	for (int i = 0; i < ND_WINDOW; i++)
		navidromethumb_release(&thumbnails[i]);
}
static void job_details(lv_event_t *e) {
	int i = -1;
	for (int row = 0; row < result.count; row++)
		if (jobs[row] == lv_event_get_current_target(e)) { i = row; break; }
	if (gesture() || lv_screen_active() != manager || i < 0 || i >= result.count || result.filter != (nd_dl_filter_t)(selected_tab + 1) || result.error[0])
		return;
	nd_context_t context;
	bool valid = navidrome_context(&context, false) && context.profile_epoch == manager_profile && context.card_epoch == manager_card;
	nd_wipe(&context, sizeof(context));
	if (!valid)
		return;
	nd_dl_job_t *job = &result.jobs[i];
	char message[ND_TEXT_MAX + ND_DL_PATH + 512];
	const char *path = job->path[0] ? job->path : job->destination[0] ? job->destination : tr("nd_dl_card_root");
	lv_snprintf(message, sizeof(message), "%s\n\n%s\n\n%s", job->source.row.title, tr(job->error[0] ? job->error : state_label(job->state)), path);
	lv_label_set_text(details_text, message);
	lv_obj_scroll_to_y(lv_obj_get_parent(details_text), 0, LV_ANIM_OFF);
	lv_obj_set_hidden(details_layer, false);
	lv_obj_move_foreground(details_layer);
}
static void details_build(void) {
	details_layer = lv_obj_create(manager);
	lv_obj_remove_style_all(details_layer);
	lv_obj_set_size(details_layer, cfg->screen_width, cfg->screen_height);
	lv_obj_set_style_bg_color(details_layer, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(details_layer, LV_OPA_60, 0);
	lv_obj_set_scrollable(details_layer, false);
	lv_obj_t *card = lv_obj_create(details_layer);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_size(card, LV_MIN(ui_px(400), (int)cfg->screen_width - 2 * (int)cfg->padding), cfg->screen_height - 2 * settingsrow_content_top(cfg));
	lv_obj_center(card);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(card, false);
	lv_obj_t *body = lv_obj_create(card);
	lv_obj_remove_style_all(body);
	lv_obj_set_width(body, lv_pct(100));
	lv_obj_set_height(body, 0);
	lv_obj_set_flex_grow(body, 1);
	lv_obj_set_scroll_dir(body, LV_DIR_VER);
	details_text = lv_label_create(body);
	lv_obj_add_style(details_text, &theme_style_text, 0);
	lv_obj_set_style_text_font(details_text, &font_ui_20, 0);
	lv_obj_set_width(details_text, lv_pct(100));
	lv_label_set_long_mode(details_text, LV_LABEL_LONG_WRAP);
	lv_obj_t *button = lv_btn_create(card);
	lv_obj_add_style(button, &theme_style_card, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_size(button, lv_pct(100), ui_px(56));
	lv_obj_add_event_cb(button, details_close, LV_EVENT_CLICKED, NULL);
	lv_obj_t *label = lv_label_create(button);
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_20, 0);
	lv_label_set_text(label, tr("ok"));
	lv_obj_center(label);
	details_close(NULL);
}
static void clear_history_cb(lv_event_t *e) {
	(void)e;
	if (gesture() || clearing || lv_screen_active() != manager || details_visible() || result.error[0] || !library().writable)
		return;
	if (!(result.counts[NDDL_COMPLETE] + result.counts[NDDL_FAILED] + result.counts[NDDL_CANCELLED]))
		return;
	clearing = clear_pending = true;
	first = 0;
	painting = true;
	lv_obj_scroll_to_y(job_list, 0, LV_ANIM_OFF);
	painting = false;
	request(NDDL_CLEAR_HISTORY);
	manager_paint();
}
static void clear_history_build(void) {
	// Match Music's bottom-right red delete FAB, including its touch target.
	clear_history = lv_btn_create(manager);
	lv_obj_set_size(clear_history, ui_px(72), ui_px(72));
	lv_obj_set_style_radius(clear_history, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(clear_history, lv_color_make(224, 27, 36), 0);
	lv_obj_set_style_bg_color(clear_history, lv_color_make(192, 28, 40), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(clear_history, LV_OPA_COVER, 0);
	lv_obj_set_style_opa(clear_history, LV_OPA_40, LV_STATE_DISABLED);
	lv_obj_set_style_border_width(clear_history, 0, 0);
	lv_obj_set_style_shadow_width(clear_history, 0, 0);
	lv_obj_set_style_pad_all(clear_history, 0, 0);
	lv_obj_align(clear_history, LV_ALIGN_BOTTOM_RIGHT, -(int)cfg->padding - ui_px(6), -(int)cfg->padding - ui_px(6));
	lv_obj_set_event_bubble(clear_history, true);
	lv_obj_add_event_cb(clear_history, clear_history_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *glyph = lv_image_create(clear_history);
	lv_image_set_src(glyph, &icon_trash);
	lv_obj_set_style_image_recolor(glyph, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
	lv_obj_center(glyph);
	lv_obj_set_hidden(clear_history, true);
}
static void manager_build(void) {
	row_pitch = ui_px(108);
	manager = lv_obj_create(NULL);
	navidromepage_register(manager);
	lv_obj_add_style(manager, &theme_style_screen, 0);
	lv_obj_t *title = settingsrow_title(manager, cfg, "nd_dl_manage");
	settingsrow_title_corner_slots(title, cfg, 3);
	const lv_image_dsc_t *icons[] = {&icon_play_small, &icon_pause_small, &icon_clear};
	const nd_dl_action_t actions[] = {NDDL_RESUME, NDDL_PAUSE, NDDL_CANCEL};
	for (int i = 0; i < 3; i++) {
		lv_obj_t *button = header_actions[i] = lv_btn_create(manager);
		lv_obj_set_size(button, ui_px(56), ui_px(56));
		lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - (2 - i) * (ui_px(56) + ui_px(6)), cfg->padding + cfg->top_bar_height);
		lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(button, 0, 0);
		lv_obj_set_style_shadow_width(button, 0, 0);
		lv_obj_set_style_pad_all(button, 0, 0);
		lv_obj_set_event_bubble(button, true);
		lv_obj_add_event_cb(button, action_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)actions[i]);
		lv_obj_t *image = lv_image_create(button);
		lv_image_set_src(image, icons[i]);
		lv_obj_add_style(image, &theme_style_icon, 0);
		lv_obj_center(image);
	}
	int top = settingsrow_content_top(cfg);
	lv_obj_t *pills = lv_obj_create(manager);
	lv_obj_remove_style_all(pills);
	lv_obj_set_pos(pills, cfg->padding, top);
	lv_obj_set_size(pills, cfg->screen_width - 2 * cfg->padding, ui_px(52));
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(pills, ui_px(6), 0);
	lv_obj_set_scrollable(pills, false);
	const char *labels[] = {"nd_dl_queued", "nd_dl_success", "nd_dl_failed"};
	for (int i = 0; i < 3; i++) {
		tabs[i] = lv_btn_create(pills);
		lv_obj_set_style_radius(tabs[i], LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_border_width(tabs[i], 0, 0);
		lv_obj_set_style_shadow_width(tabs[i], 0, 0);
		lv_obj_set_size(tabs[i], 0, ui_px(52));
		lv_obj_set_flex_grow(tabs[i], 1);
		lv_obj_set_style_pad_all(tabs[i], ui_px(4), 0);
		lv_obj_set_style_pad_row(tabs[i], 0, 0);
		lv_obj_set_flex_flow(tabs[i], LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(tabs[i], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_event_bubble(tabs[i], true);
		lv_obj_add_event_cb(tabs[i], tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		lv_obj_t *label = lv_label_create(tabs[i]);
		lv_label_set_text(label, tr(labels[i]));
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_style_max_width(label, lv_pct(100), 0);
		lv_obj_set_style_text_font(label, &font_ui_16, 0);
		tab_counts[i] = lv_label_create(tabs[i]);
		lv_obj_set_style_text_font(tab_counts[i], &font_ui_16, 0);
	}
	top += ui_px(58);
	job_status = lv_label_create(manager);
	lv_obj_add_style(job_status, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(job_status, &font_ui_16, 0);
	lv_obj_set_pos(job_status, cfg->padding, top);
	lv_obj_set_size(job_status, cfg->screen_width - 2 * cfg->padding, ui_px(24));
	lv_label_set_long_mode(job_status, LV_LABEL_LONG_DOT);
	top += ui_px(28);
	job_list = lv_obj_create(manager);
	lv_obj_remove_style_all(job_list);
	lv_obj_set_pos(job_list, cfg->padding, top);
	lv_obj_set_size(job_list, cfg->screen_width - 2 * cfg->padding, cfg->screen_height - top - cfg->padding);
	lv_obj_set_scroll_dir(job_list, LV_DIR_VER);
	job_extent = lv_obj_create(job_list);
	lv_obj_remove_style_all(job_extent);
	lv_obj_set_size(job_extent, 1, 1);
	lv_obj_set_clickable(job_extent, false);
	lv_obj_set_scrollable(job_extent, false);
	for (int i = 0; i < ND_WINDOW; i++) {
		jobs[i] = lv_btn_create(job_list);
		lv_obj_add_style(jobs[i], &theme_style_card, 0);
		lv_obj_add_style(jobs[i], &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_size(jobs[i], lv_pct(100), ui_px(100));
		lv_obj_set_style_shadow_width(jobs[i], 0, 0);
		lv_obj_set_style_pad_all(jobs[i], ui_px(14), 0);
		lv_obj_set_scrollable(jobs[i], false);
		navidromethumb_init(&thumbnails[i], jobs[i], ND_ART_DOWNLOAD_SLOT + i);
		job_titles[i] = lv_label_create(jobs[i]);
		job_notes[i] = lv_label_create(jobs[i]);
		job_locations[i] = lv_label_create(jobs[i]);
		lv_obj_t *lines[] = {job_titles[i], job_notes[i], job_locations[i]};
		for (int j = 0; j < 3; j++) {
			lv_obj_add_style(lines[j], j ? &theme_style_text_dim : &theme_style_text, 0);
			lv_obj_set_style_text_font(lines[j], j ? &font_ui_16 : &font_ui_20, 0);
			lv_obj_set_pos(lines[j], ui_px(86), ui_px(4 + j * 24));
			lv_obj_set_width(lines[j], cfg->screen_width - 2 * cfg->padding - ui_px(128));
			lv_obj_set_height(lines[j], lv_font_get_line_height(j ? &font_ui_16 : &font_ui_20));
			lv_label_set_long_mode(lines[j], LV_LABEL_LONG_DOT);
		}
		int detail_width = cfg->screen_width - 2 * cfg->padding - ui_px(128);
		int size_width = LV_MIN(ui_px(240), detail_width * 3 / 4);
		lv_obj_set_width(job_notes[i], detail_width - size_width - ui_px(6));
		job_sizes[i] = lv_label_create(jobs[i]);
		lv_obj_add_style(job_sizes[i], &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(job_sizes[i], &font_ui_16, 0);
		lv_obj_set_pos(job_sizes[i], ui_px(86) + detail_width - size_width, ui_px(28));
		lv_obj_set_size(job_sizes[i], size_width, lv_font_get_line_height(&font_ui_16));
		lv_obj_set_style_text_align(job_sizes[i], LV_TEXT_ALIGN_RIGHT, 0);
		lv_label_set_long_mode(job_sizes[i], LV_LABEL_LONG_DOT);
		lv_obj_set_event_bubble(jobs[i], true);
		lv_obj_set_hidden(jobs[i], true);
		lv_obj_add_event_cb(jobs[i], job_details, LV_EVENT_CLICKED, (void *)(intptr_t)i);
	}
	lv_obj_set_event_bubble(job_list, true);
	lv_obj_set_event_bubble(pills, true);
	lv_obj_add_event_cb(job_list, manager_scroll, LV_EVENT_SCROLL, NULL);
	switcher_attach_back_gesture(manager);
	clear_history_build();
	details_build();
	switcher_set_back_guard(manager, details_back);
	switcher_set_back_guard_peek(manager, details_visible);
	lv_obj_add_event_cb(manager, manager_unloaded, LV_EVENT_SCREEN_UNLOADED, NULL);
}
void navidromedownloadpage_init(gui_config_t *config) {
	cfg = config;
	screen = lv_obj_create(NULL);
	navidromepage_register(screen);
	lv_obj_add_style(screen, &theme_style_screen, 0);
	manager_body = settingsrow_page(screen, cfg, "nd_dl_downloads");
	settingsrow_add(manager_body, "nd_dl_folder", &folder_value, pick_cb, NULL);
	summary = note(manager_body);
	lv_label_set_text(note(manager_body), tr("nd_dl_discovery"));
	settingsrow_add(manager_body, "nd_dl_manage", NULL, manager_open_cb, NULL);
	manager_build();
	folderpicker_t destination = folderpicker_create(NULL, cfg, "nd_dl_folder", "nd_dl_use_folder", use_cb, new_cb, cancel_picker);
	picker = destination.root;
	navidromepage_register(picker);
	picker_body = destination.list;
	use_folder = destination.use;
	new_folder = destination.new_folder;
	folder_label = note(picker_body);
	picker_status = note(picker_body);
	for (int i = 0; i < ND_WINDOW; i++) {
		folders[i] = folderpicker_row(picker_body, "", folder_cb, (void *)(uintptr_t)i);
		lv_obj_set_hidden(folders[i], true);
	}
	picker_pages = pagination(picker_body);

	switcher_set_back_guard(picker, back);
	switcher_set_back_guard_peek(picker, back_peek);
	switcher_attach_back_gesture(screen);
	name_layer = lv_obj_create(picker);
	lv_obj_remove_style_all(name_layer);
	lv_obj_add_style(name_layer, &theme_style_screen, 0);
	lv_obj_set_style_pad_all(name_layer, 0, 0);
	lv_obj_set_style_border_width(name_layer, 0, 0);
	lv_obj_set_style_bg_opa(name_layer, LV_OPA_COVER, 0);
	lv_obj_set_size(name_layer, cfg->screen_width, cfg->screen_height);
	lv_obj_set_pos(name_layer, 0, 0);
	lv_obj_set_scrollable(name_layer, false);
	lv_obj_t *heading = lv_label_create(name_layer);
	lv_label_set_text(heading, tr("nd_dl_new_folder"));
	lv_obj_add_style(heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(heading, &font_ui_24, 0);
	int left = cfg->padding + ui_px(70);
	lv_obj_set_pos(heading, left, cfg->top_bar_height + cfg->padding + ui_px(10));
	lv_obj_set_width(heading, cfg->screen_width - left - cfg->padding);
	lv_obj_set_height(heading, lv_font_get_line_height(&font_ui_24));
	lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);
	name_field = lv_textarea_create(name_layer);
	lv_textarea_set_one_line(name_field, true);
	lv_textarea_set_max_length(name_field, 80);
	lv_obj_set_size(name_field, cfg->screen_width - 2 * cfg->padding, ui_px(62));
	lv_obj_set_pos(name_field, cfg->padding, settingsrow_content_top(cfg));
	lv_obj_add_style(name_field, &theme_style_card, 0);
	lv_obj_set_style_text_font(name_field, &font_ui_24, 0);
	lv_obj_set_style_radius(name_field, ui_px(12), 0);
	lv_obj_set_style_border_width(name_field, 0, 0);
	lv_obj_set_style_pad_all(name_field, ui_px(14), 0);
	lv_textarea_set_placeholder_text(name_field, tr("name"));
	keyboard_style_caret(name_field);
	name_keyboard = keyboard_create(name_layer, cfg->screen_width, ui_px(316), name_field, NULL, "ok", name_accept, NULL);
	keyboard_set_visible(name_keyboard, false);
	lv_obj_set_hidden(name_layer, true);
	timer = lv_timer_create(tick, 200, NULL);
	power_slow_in_standby(timer, 1000);
	lv_timer_pause(timer);
	navidromedownloads_set_listener(download_changed);
}

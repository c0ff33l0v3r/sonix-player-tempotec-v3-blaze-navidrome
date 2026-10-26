#include "src/system/streaming/navidromeplay.h"
#include "navidromeplaylists.h"
#include "navidromeart.h"
#include "navidromebrowse.h"
#include "navidromepage.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/playmark.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/uiscale.h"
#include "src/system/core/lang.h"
#include "src/system/core/config.h"
#include "src/system/streaming/navidromedownloads.h"
#include "src/system/streaming/navidromeplaylists.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
	lv_obj_t *screen, *list, *extent, *title, *status, *buttons[3];
	lv_obj_t *download;
	unsigned download_token;
	bool download_pending;
	int64_t download_playlist, download_revision;
	lv_obj_t *new_row, *new_icon, *new_label, *empty;
	lv_obj_t *images[ND_WINDOW];
	cover_image_t artwork[ND_WINDOW];
	bool art_finished[ND_WINDOW], art_bound[ND_WINDOW];
	lv_obj_t *rows[ND_WINDOW], *names[ND_WINDOW], *details[ND_WINDOW], *menus[ND_WINDOW], *checks[ND_WINDOW];
	lv_obj_t *playmarks[ND_WINDOW];
	nd_playlist_t playlists[ND_WINDOW], playlist;
	nd_playlist_entry_t entries[ND_WINDOW];
	int first, count, total, selected, pitch, offset;
	unsigned serial, epoch, card;
	uint32_t submitted;
	char scope[33];
	bool loading, selecting, reload;
	int64_t session;
} pl_page_t;
static pl_page_t pl_pages[4]; // collection, detail, destination, remote catalogue
static gui_config_t *pl_cfg;
static lv_timer_t *pl_timer;
static lv_obj_t *pl_name_layer, *pl_name_field;
static keyboard_t *pl_name_keyboard;
static int pl_name_page;
static bool pl_copying, pl_renaming, pl_fixed, pl_recovering, pl_skip_load;
static nd_playlist_t pl_menu_playlist;
static int64_t pl_session, pl_generation, pl_recovery_id;
static bool pl_source_favorites;
static char pl_recovery_name[ND_PL_NAME];
static lv_obj_t *pl_origin;
static nd_pl_request_t pl_confirm_request, pl_removal;
static int64_t pl_menu_occurrence;
static int pl_confirm_page;
static unsigned pl_download_token;
static unsigned pl_download_menu_token;
static void pl_load(int page);
static void pl_action(int page, nd_pl_request_t *q);
static bool pl_gesture(void) { return player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active(); }
static bool pl_active(int page) { return lv_screen_active() == pl_pages[page].screen; }
static void pl_buttons(pl_page_t *p) {
	int page = (int)(p - pl_pages);
	for (int i = 0; i < 3; i++)
		lv_obj_set_hidden(p->buttons[i], page == 1 ? false : (page == 2 || i > (page == 3 && pl_recovering ? 1 : 0)));
	if (page == 1) {
		if (p->download) {
			nd_library_t lib;
			navidrome_library_snapshot(&lib);
			lv_obj_set_hidden(p->download, p->selecting);
			if (p->loading || p->download_pending || !p->playlist.count || !lib.writable || !lib.scope[0])
				lv_obj_add_state(p->download, LV_STATE_DISABLED);
			else lv_obj_remove_state(p->download, LV_STATE_DISABLED);
		}
		lv_obj_set_hidden(p->buttons[2], p->selecting);
		lv_image_set_src(lv_obj_get_child(p->buttons[0], 0), p->selecting ? &icon_close : &icon_ellipsis_vertical);
		lv_image_set_src(lv_obj_get_child(p->buttons[1], 0), p->selecting ? &icon_trash : &icon_refresh);
	}
	if (page == 3)
		lv_image_set_src(lv_obj_get_child(p->buttons[1], 0), &icon_plus);
}
static void pl_status(pl_page_t *p) {
	if (p->selecting) {
		char text[100];
		lv_snprintf(text, sizeof(text), tr("nd_pl_selected"), p->selected);
		lv_label_set_text(p->status, text);
		return;
	}
	if (p == &pl_pages[1]) {
		const nd_playlist_t *v = &p->playlist;
		char text[220], date[48] = "";
		if (v->synced) {
			struct tm tm;
			if (localtime_r(&v->synced, &tm))
				strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &tm);
		}
		lv_snprintf(text, sizeof(text), "%d · %s%s%s%s%s", v->count, tr(v->pending ? "nd_pl_verify_needed" : v->dirty ? "nd_pl_local_changes" : *v->remote ? "nd_pl_linked" : "nd_pl_local"), *date ? " · " : "", *date ? tr("nd_pl_last_sync") : "", *date ? " " : "", date);
		lv_label_set_text(p->status, text);
	} else {
		char text[80];
		lv_snprintf(text, sizeof(text), tr("nd_item_count"), p->total);
		lv_label_set_text(p->status, p->total ? text : tr("nd_pl_empty"));
	}
}
static void pl_release_art_row(pl_page_t *p, int i) {
	navidromeart_release(i);
	lv_image_set_src(p->images[i], &icon_music_note);
	lv_obj_set_size(p->images[i], ui_px(ND_ART_THUMB_SIZE), ui_px(ND_ART_THUMB_SIZE));
	lv_image_set_inner_align(p->images[i], LV_IMAGE_ALIGN_CONTAIN);
	lv_obj_center(p->images[i]);
	lv_obj_add_style(p->images[i], &theme_style_icon, 0);
	cover_free(&p->artwork[i]);
	p->art_finished[i] = true;
	p->art_bound[i] = false;
}
static void pl_release_art(pl_page_t *p) {
	if (p == &pl_pages[1])
		for (int i = 0; i < ND_WINDOW; i++)
			pl_release_art_row(p, i);
}
static void pl_take_art(pl_page_t *p) {
	if (p != &pl_pages[1])
		return;
	for (int i = 0; i < p->count; i++) {
		if (!p->art_finished[i] && navidromeart_take(i, &p->artwork[i], &p->art_finished[i])) {
			lv_obj_remove_style(p->images[i], &theme_style_icon, 0);
			lv_obj_set_size(p->images[i], ui_px(ND_ART_THUMB_SIZE), ui_px(ND_ART_THUMB_SIZE));
			// Keep automatic fitting across the glyph-to-cover transition.
			lv_image_set_inner_align(p->images[i], LV_IMAGE_ALIGN_CONTAIN);
			lv_image_set_src(p->images[i], &p->artwork[i].dsc);
			lv_obj_center(p->images[i]);
		}
	}
}
static void pl_show_rows(pl_page_t *p, const bool keep_art[ND_WINDOW]) {
	int page = (int)(p - pl_pages);
	nd_library_t library;
	navidrome_library_snapshot(&library);
	lv_obj_set_height(p->extent, p->offset + p->total * p->pitch + ui_px(12));
	lv_obj_set_hidden(p->empty, p->total > 0);
	for (int i = 0; i < ND_WINDOW; i++) {
		if (page == 1 && !keep_art[i] && (p->art_bound[i] || i < p->count))
			pl_release_art_row(p, i);
		lv_obj_set_hidden(p->rows[i], i >= p->count);
		if (i >= p->count)
			continue;
		lv_obj_set_y(p->rows[i], p->offset + (p->first + i) * p->pitch);
		if (page == 1) {
			lv_label_set_text(p->names[i], p->entries[i].track.title);
			lv_label_set_text(p->details[i], p->entries[i].track.artist);
			p->art_bound[i] = true;
			if (!keep_art[i] && *p->entries[i].track.cover) {
				p->art_finished[i] = false;
				navidromeart_request(i, &library, p->entries[i].track.cover, ui_px(ND_ART_THUMB_SIZE));
			}
		} else {
			lv_label_set_text(p->names[i], p->playlists[i].name);
			const nd_playlist_t *v = &p->playlists[i];
			char count[80], subtitle[256];
			lv_snprintf(count, sizeof(count), tr(v->count == 1 ? "playlist_track_2" : "playlist_tracks"), v->count);
			lv_snprintf(subtitle, sizeof(subtitle), "%s · %s", count, tr(v->pending ? "nd_pl_verify_needed" : v->dirty ? "nd_pl_local_changes" : *v->remote ? "nd_pl_linked" : "nd_pl_local"));
			lv_label_set_text(p->details[i], page == 3 ? tr("nd_pl_remote") : subtitle);
		}
		lv_obj_set_hidden(p->menus[i], (page == 1 && p->selecting) || page == 2 || page == 3);
		lv_obj_set_hidden(p->checks[i], page != 1 || !p->entries[i].selected);
		lv_obj_set_style_bg_opa(p->rows[i], page == 1 && p->entries[i].selected ? LV_OPA_50 : LV_OPA_COVER, 0);
	}
	pl_status(p);
	pl_buttons(p);
}
static void pl_confirm_ok(void *unused) {
	(void)unused;
	if (pl_active(pl_confirm_page) && (pl_confirm_page != 1 || pl_pages[1].playlist.id == pl_confirm_request.id))
		pl_action(pl_confirm_page, &pl_confirm_request);
}
static void pl_open_detail(const nd_playlist_t *playlist) {
	pl_page_t *p = &pl_pages[1];
	p->download_token = ++pl_download_token;
	p->download_pending = false;
	p->playlist = *playlist;
	p->first = 0;
	p->selecting = false;
	p->selected = 0;
	p->session++;
	lv_label_set_text(p->title, playlist->name);
	lv_obj_scroll_to_y(p->list, 0, LV_ANIM_OFF);
	if (pl_active(1))
		pl_load(1);
	else
		switch_screen(p->screen);
}
static void pl_done(nd_pl_result_t *r, void *value) {
	uintptr_t token = (uintptr_t)value;
	int page = (int)(token & 3);
	unsigned serial = (unsigned)(token >> 2);
	pl_page_t *p = &pl_pages[page];
	if (p->serial != serial || !pl_active(page)) {
		free(r);
		return;
	}
	p->loading = false;
	if (r->error) {
		lv_label_set_text(p->status, tr(r->error));
		gui_notify_popup(r->error);
		free(r);
		return;
	}
	nd_pl_action_t action = r->request.action;
	if (r->confirm) {
		pl_confirm_request = r->request;
		pl_confirm_request.confirmed = true;
		pl_confirm_request.candidate = r->candidate;
		snprintf(pl_confirm_request.checked, sizeof(pl_confirm_request.checked), "%s", r->checked);
		if (action == NDPL_PULL)
			pl_confirm_request.action = NDPL_PULL_ACCEPT;
		pl_confirm_page = page;
		confirm_show("nd_pl_sync_title", tr(action == NDPL_PULL ? "nd_pl_pull_confirm" : r->conflict ? "nd_pl_conflict_confirm" : "nd_pl_publish_confirm"), "ok", pl_confirm_ok, NULL);
		free(r);
		return;
	}
	if (action == NDPL_SELECT || action == NDPL_SELECT_ONLY) {
		p->selected = r->selected;
		p->selecting = p->selected > 0;
		pl_load(page);
		free(r);
		return;
	}
	if (action == NDPL_CREATE && page == 2) {
		nd_pl_request_t q = {.action = NDPL_ADD, .id = r->playlist.id, .revision = r->playlist.revision, .session = pl_session, .generation = pl_generation, .favorites = pl_source_favorites};
		pl_action(page, &q);
		free(r);
		return;
	}
	if (action == NDPL_ADD) {
		char message[160];
		if (r->added == 0 && r->skipped == 1)
			snprintf(message, sizeof(message), "%s", tr("nd_pl_already"));
		else
			lv_snprintf(message, sizeof(message), tr("nd_pl_added_skipped"), r->added, r->skipped);
		gui_notify_popup(message);
		pl_fixed = false;
		// Navigate only while the initiating picker remains current.
		if (pl_origin && page == 2)
			switch_screen_return_to(pl_origin);
		if (pl_active(1)) {
			pl_pages[1].playlist = r->playlist;
			pl_pages[1].selecting = false;
			pl_load(1);
		}
		free(r);
		return;
	}
	if (action == NDPL_CREATE || action == NDPL_IMPORT || action == NDPL_COPY || action == NDPL_RECOVER) {
		pl_open_detail(&r->playlist);
		free(r);
		return;
	}
	if (action == NDPL_DELETE) {
		pl_load(0);
		free(r);
		return;
	}
	if (action == NDPL_LIST || action == NDPL_CATALOGUE || action == NDPL_CATALOGUE_WINDOW || action == NDPL_ENTRIES) {
		if (!r->count && r->total > 0 && r->request.first >= r->total) {
			// The old scroll window may no longer exist after deletions or
			// returning to a smaller catalogue. Retry only the local window.
			p->loading = true;
			lv_obj_set_height(p->extent, p->offset + r->total * p->pitch + ui_px(12));
			lv_obj_scroll_to_y(p->list, 0, LV_ANIM_OFF);
			p->loading = false;
			pl_load(page);
			free(r);
			return;
		}
		bool keep_art[ND_WINDOW] = {0};
		// Selection markers may change without changing the row's artwork.
		if (page == 1 && p->epoch == r->context.epoch && p->card == r->context.card_epoch && p->playlist.id == r->playlist.id)
			for (int i = 0; i < ND_WINDOW; i++)
				keep_art[i] = p->art_bound[i] && i < p->count && i < r->count && p->entries[i].occurrence == r->entries[i].occurrence && !strcmp(p->entries[i].track.id, r->entries[i].track.id) && !strcmp(p->entries[i].track.cover, r->entries[i].track.cover);
		p->first = r->request.first;
		p->count = r->count;
		p->total = r->total;
		p->selected = r->selected;
		p->epoch = r->context.epoch;
		p->card = r->context.card_epoch;
		nd_library_t lib;
		navidrome_library_snapshot(&lib);
		snprintf(p->scope, sizeof(p->scope), "%s", lib.scope);
		memcpy(p->playlists, r->playlists, sizeof(p->playlists));
		memcpy(p->entries, r->entries, sizeof(p->entries));
		if (action == NDPL_ENTRIES) {
			p->playlist = r->playlist;
			lv_label_set_text(p->title, p->playlist.name);
		}
		pl_show_rows(p, keep_art);
	} else {
		if (r->playlist.id && page == 1)
			p->playlist = r->playlist;
		p->selecting = false;
		p->selected = 0;
		pl_load(page);
	}
	free(r);
}
static void pl_action(int page, nd_pl_request_t *q) {
	if (page == 3 && pl_recovering && (q->action == NDPL_CATALOGUE || q->action == NDPL_CATALOGUE_WINDOW))
		snprintf(q->name, sizeof(q->name), "%s", pl_recovery_name);
	pl_page_t *p = &pl_pages[page];
	nd_library_t current;
	navidrome_library_snapshot(&current);
	if (p->epoch && (p->epoch != current.epoch || p->card != current.card_epoch || strcmp(p->scope, current.scope))) {
		gui_notify_popup("nd_pl_stale");
		return;
	}
	if (p->loading || navidrome_playlists_busy()) {
		gui_notify_popup("nd_working");
		return;
	}
	unsigned serial = p->serial + 1;
	if (!navidrome_playlists_submit(q, pl_done, (void *)(uintptr_t)(((uintptr_t)serial << 2) | (unsigned)page))) {
		navidrome_snapshot_t s;
		navidrome_snapshot(&s);
		gui_notify_popup(s.state == ND_CONNECTED ? "nd_working" : navidrome_feedback(&s));
		return;
	}
	p->serial = serial;
	p->reload = false;
	p->loading = true;
	pl_buttons(p);
	p->submitted = lv_tick_get();
	lv_label_set_text(p->status, tr("nd_working"));
}
static void pl_load(int page) {
	pl_page_t *p = &pl_pages[page];
	nd_pl_request_t q = {.action = page == 1 ? NDPL_ENTRIES : page == 3 ? NDPL_CATALOGUE_WINDOW : NDPL_LIST, .id = page == 1 ? p->playlist.id : 0, .session = p->session};
	q.first = (lv_obj_get_scroll_y(p->list) - p->offset) / p->pitch - 2;
	if (q.first < 0)
		q.first = 0;
	p->reload = true;
	pl_action(page, &q);
}
static void pl_name_hide(void) { lv_obj_set_hidden(pl_name_layer, true); }
static void pl_name_show(int page, bool rename, const nd_playlist_t *playlist) {
	pl_name_page = page;
	pl_copying = false;
	pl_renaming = rename;
	// The shared Back control lives on the top layer, above page-owned dialogs.
	lv_obj_set_parent(pl_name_layer, pl_pages[page].screen);
	if (playlist)
		pl_menu_playlist = *playlist;
	lv_textarea_set_text(pl_name_field, rename ? pl_menu_playlist.name : "");
	keyboard_reset(pl_name_keyboard);
	lv_obj_add_state(pl_name_field, LV_STATE_FOCUSED);
	lv_obj_send_event(pl_name_field, LV_EVENT_FOCUSED, NULL);
	lv_obj_set_hidden(pl_name_layer, false);
	lv_obj_move_foreground(pl_name_layer);
}
static void pl_name_accept(lv_event_t *event) {
	(void)event;
	if (pl_gesture())
		return;
	nd_pl_request_t q = {.action = pl_copying ? NDPL_COPY : pl_renaming ? NDPL_RENAME : NDPL_CREATE, .id = pl_copying || pl_renaming ? pl_menu_playlist.id : 0, .revision = pl_copying || pl_renaming ? pl_menu_playlist.revision : 0};
	snprintf(q.name, sizeof(q.name), "%s", lv_textarea_get_text(pl_name_field));
	if (!nd_pl_name_valid(q.name)) {
		gui_notify_popup("nd_pl_invalid_name");
		return;
	}
	pl_name_hide();
	pl_action(pl_name_page, &q);
}
static void pl_name_close(lv_event_t *event) {
	(void)event;
	pl_name_hide();
}
static void pl_rename(void *unused) {
	(void)unused;
	pl_name_show(pl_active(1) ? 1 : 0, true, &pl_menu_playlist);
}
static void pl_delete_confirm(void *unused) {
	(void)unused;
	nd_pl_request_t q = {.action = NDPL_DELETE, .id = pl_menu_playlist.id, .revision = pl_menu_playlist.revision};
	pl_action(0, &q);
}
static void pl_delete(void *unused) {
	(void)unused;
	confirm_show("nd_pl_delete_title", tr("nd_pl_delete_confirm"), "delete", pl_delete_confirm, NULL);
}
static void pl_copy(void *unused) {
	(void)unused;
	if (!*pl_menu_playlist.remote)
		return;
	pl_name_show(pl_active(1) ? 1 : 0, true, &pl_menu_playlist);
	pl_renaming = false;
	pl_copying = true;
}
static void pl_remote_action(void *value) {
	int a = (int)(intptr_t)value;
	nd_playlist_t *p = &pl_pages[1].playlist;
	nd_pl_request_t q = {.action = a, .id = p->id, .revision = p->revision};
	pl_action(1, &q);
}
static void pl_remove_confirm(void *unused) {
	(void)unused;
	if (pl_active(1) && pl_pages[1].playlist.id == pl_removal.id)
		pl_action(1, &pl_removal);
}
static void pl_remove(void *one) {
	pl_removal = (nd_pl_request_t){.action = NDPL_REMOVE, .id = pl_pages[1].playlist.id, .revision = pl_pages[1].playlist.revision, .session = pl_pages[1].session, .occurrence = one ? pl_menu_occurrence : 0};
	confirm_show("nd_pl_remove_title", tr("nd_pl_remove_confirm"), "remove", pl_remove_confirm, NULL);
}
static void pl_recreate(void *unused) {
	(void)unused;
	nd_pl_request_t q = {.action = NDPL_RECREATE, .id = pl_recovery_id, .confirmed = true};
	pl_action(3, &q);
}
static void pl_recovery(void *unused) {
	(void)unused;
	pl_recovering = true;
	pl_recovery_id = pl_pages[1].playlist.id;
	snprintf(pl_recovery_name, sizeof(pl_recovery_name), "%s", *pl_pages[1].playlist.intent_name ? pl_pages[1].playlist.intent_name : pl_pages[1].playlist.name);
	pl_pages[3].first = 0;
	lv_obj_scroll_to_y(pl_pages[3].list, 0, LV_ANIM_OFF);
	pl_skip_load = true;
	switch_screen(pl_pages[3].screen);
	pl_skip_load = false;
	nd_pl_request_t q = {.action = NDPL_CATALOGUE};
	pl_action(3, &q);
}
static void pl_add_kind(void *value) {
	pl_fixed = true;
	navidromebrowse_pick((nd_kind_t)(intptr_t)value);
}
static void pl_download_done(bool ok, const char *error, int added, int skipped, void *user) {
	pl_page_t *p = &pl_pages[1];
	if ((unsigned)(uintptr_t)user != p->download_token) return;
	p->download_pending = false;
	if (!pl_active(1)) return;
	nd_library_t lib;
	navidrome_library_snapshot(&lib);
	pl_buttons(p);
	if (p->card != lib.card_epoch || p->epoch != lib.epoch || strcmp(p->scope, lib.scope) ||
		p->playlist.id != p->download_playlist || p->playlist.revision != p->download_revision) return;
	if (!ok) { gui_notify_popup(error ? error : "nd_dl_capture_error"); return; }
	char text[160];
	lv_snprintf(text, sizeof(text), tr("nd_dl_admitted"), added, skipped);
	toast_success(text);
}
static void pl_download(lv_event_t *event) {
	(void)event;
	pl_page_t *p = &pl_pages[1];
	if (!pl_active(1) || pl_gesture() || p->selecting || p->loading || p->download_pending || !p->playlist.count) return;
	nd_dl_request_t q = {.action = NDDL_CAPTURE, .playlist = p->playlist.id, .playlist_revision = p->playlist.revision};
	navidrome_library_snapshot(&q.library);
	if (p->epoch != q.library.epoch || p->card != q.library.card_epoch || strcmp(p->scope, q.library.scope)) {
		gui_notify_popup("nd_pl_stale"); return;
	}
	if (!q.library.writable) { gui_notify_popup("nd_dl_readonly"); return; }
	const char *folder = config_get("navidrome", "download_folder", "Music/Navidrome");
	if (!nd_dl_destination(folder)) { gui_notify_popup("nd_dl_path_error"); return; }
	snprintf(q.destination, sizeof(q.destination), "%s", folder);
	unsigned token = ++pl_download_token;
	if (!navidromedownloads_request(&q, pl_download_done, (void *)(uintptr_t)token)) {
		gui_notify_popup("nd_working"); return;
	}
	p->download_token = token;
	p->download_playlist = q.playlist;
	p->download_revision = q.playlist_revision;
	p->download_pending = true;
	pl_buttons(p);
}
static void pl_download_menu(void *user) {
	if ((unsigned)(uintptr_t)user == pl_download_menu_token && pl_active(1) &&
		pl_pages[1].playlist.id == pl_menu_playlist.id && pl_pages[1].playlist.revision == pl_menu_playlist.revision)
		pl_download(NULL);
}
static void pl_button(lv_event_t *event) {
	if (pl_gesture())
		return;
	uintptr_t key = (uintptr_t)lv_event_get_user_data(event);
	int page = (int)(key / 3), slot = (int)(key % 3);
	pl_page_t *p = &pl_pages[page];
	if (p->loading)
		return;
	if (page == 0 || page == 2) {
		if (slot == (page == 0 ? 1 : 0)) {
			pl_name_show(page, false, NULL);
			return;
		}
		pl_recovering = false;
		pl_recovery_name[0] = 0;
		lv_obj_scroll_to_y(pl_pages[3].list, 0, LV_ANIM_OFF);
		pl_skip_load = true;
		switch_screen(pl_pages[3].screen);
		pl_skip_load = false;
		nd_pl_request_t q = {.action = NDPL_CATALOGUE};
		pl_action(3, &q);
		return;
	}
	if (page == 3) {
		if (slot == 1 && pl_recovering)
			confirm_show("nd_pl_sync_title", tr("nd_pl_recreate_confirm"), "ok", pl_recreate, NULL);
		else {
			nd_pl_request_t q = {.action = NDPL_CATALOGUE};
			pl_action(3, &q);
		}
		return;
	}
	if (p->selecting) {
		if (slot == 0) {
			p->selecting = false;
			p->selected = 0;
			p->session++;
			pl_load(1);
		} else if (slot == 1)
			pl_remove(NULL);
		return;
	}
	pl_menu_playlist = p->playlist;
	if (slot == 0) {
		popover_item_t items[] = {{"rename", pl_rename, NULL, false},
			{"nd_pl_download", pl_download_menu, (void *)(uintptr_t)++pl_download_menu_token, false},
			{"nd_pl_save_copy", pl_copy, NULL, false}};
		popover_show(lv_event_get_current_target(event), items, *p->playlist.remote ? 3 : 2);
	} else if (slot == 1) {
		if (p->playlist.pending && !*p->playlist.remote) {
			popover_item_t items[] = {{"nd_pl_reconcile", pl_recovery, NULL, false}};
			popover_show(lv_event_get_current_target(event), items, 1);
		} else if (*p->playlist.remote) {
			popover_item_t items[] = {{"nd_pl_pull", pl_remote_action, (void *)(intptr_t)NDPL_PULL, false}, {"nd_pl_publish", pl_remote_action, (void *)(intptr_t)NDPL_PUBLISH, false}, {"nd_pl_verify", pl_remote_action, (void *)(intptr_t)NDPL_VERIFY, false}};
			popover_show(lv_event_get_current_target(event), items, p->playlist.pending ? 3 : 2);
		} else {
			popover_item_t items[] = {{"nd_pl_upload", pl_remote_action, (void *)(intptr_t)NDPL_UPLOAD, false}};
			popover_show(lv_event_get_current_target(event), items, 1);
		}
	} else {
		nd_library_t lib;
		navidrome_library_snapshot(&lib);
		if (!lib.ready) {
			gui_notify_popup("nd_not_synced");
			return;
		}
		popover_item_t items[] = {{"navidrome_tracks", pl_add_kind, (void *)(intptr_t)ND_TRACKS, false}, {"albums", pl_add_kind, (void *)(intptr_t)ND_ALBUMS, false}};
		popover_show(lv_event_get_current_target(event), items, 2);
	}
}
static void pl_toggle_row(int index) {
	pl_page_t *p = &pl_pages[1];
	nd_pl_request_t q = {.action = p->selecting ? NDPL_SELECT : NDPL_SELECT_ONLY, .id = p->playlist.id, .revision = p->playlist.revision, .session = p->session, .ordinal = p->first + index, .occurrence = p->entries[index].occurrence, .kind = ND_TRACKS};
	snprintf(q.track, sizeof(q.track), "%s", p->entries[index].track.id);
	pl_action(1, &q);
}
static void pl_row(lv_event_t *event) {
	if (pl_gesture())
		return;
	uintptr_t key = (uintptr_t)lv_event_get_user_data(event);
	int page = (int)(key / ND_WINDOW), index = (int)(key % ND_WINDOW);
	pl_page_t *p = &pl_pages[page];
	if (index >= p->count || p->loading)
		return;
	if (page == 1) {
		if (p->selecting)
			pl_toggle_row(index);

		return;
	}
	if (page == 0) {
		pl_fixed = false;
		pl_open_detail(&p->playlists[index]);
		return;
	}
	if (page == 2) {
		nd_pl_request_t q = {.action = NDPL_ADD, .id = p->playlists[index].id, .revision = p->playlists[index].revision, .session = pl_session, .generation = pl_generation, .favorites = pl_source_favorites};
		pl_action(2, &q);
		return;
	}
	nd_pl_request_t q = {.action = pl_recovering ? NDPL_RECOVER : NDPL_IMPORT, .id = pl_recovering ? pl_recovery_id : 0, .confirmed = pl_recovering};
	snprintf(q.remote, sizeof(q.remote), "%s", p->playlists[index].remote);
	pl_action(3, &q);
}
static void pl_long(lv_event_t *event) {
	if (pl_gesture())
		return;
	uintptr_t key = (uintptr_t)lv_event_get_user_data(event);
	int page = (int)(key / ND_WINDOW), index = (int)(key % ND_WINDOW);
	if (page != 1 || index >= pl_pages[1].count || pl_pages[1].loading || (pl_pages[1].selecting && pl_pages[1].entries[index].selected))
		return;
	lv_indev_t *indev = lv_indev_active();
	if (indev)
		lv_indev_wait_release(indev);
	pl_toggle_row(index);
}
static void pl_menu(lv_event_t *event) {
	lv_event_stop_bubbling(event);
	if (pl_gesture())
		return;
	uintptr_t key = (uintptr_t)lv_event_get_user_data(event);
	int page = (int)(key / ND_WINDOW), index = (int)(key % ND_WINDOW);
	pl_page_t *p = &pl_pages[page];
	if (index >= p->count || p->loading)
		return;
	if (page == 1) {
		pl_menu_occurrence = p->entries[index].occurrence;
		popover_item_t items[] = {{"medialist_remove_from_playlist", pl_remove, (void *)1, false}};
		popover_show(lv_event_get_current_target(event), items, 1);
	} else if (page == 0) {
		pl_menu_playlist = p->playlists[index];
		popover_item_t items[] = {{"rename", pl_rename, NULL, false}, {"nd_pl_delete_local", pl_delete, NULL, false}, {"nd_pl_save_copy", pl_copy, NULL, false}};
		popover_show(lv_event_get_current_target(event), items, *pl_menu_playlist.remote ? 3 : 2);
	}
}
static void pl_scroll(lv_event_t *event) {
	pl_page_t *p = lv_event_get_user_data(event);
	if (!pl_active((int)(p - pl_pages)) || p->loading || pl_gesture())
		return;
	int first = (lv_obj_get_scroll_y(p->list) - p->offset) / p->pitch;
	if (first < p->first || first + 10 > p->first + p->count)
		pl_load((int)(p - pl_pages));
}
static void pl_loaded(lv_event_t *event) {
	pl_page_t *p = lv_event_get_user_data(event);
	p->loading = false;
	nd_library_t lib;
	navidrome_library_snapshot(&lib);
	p->epoch = lib.epoch;
	p->card = lib.card_epoch;
	snprintf(p->scope, sizeof(p->scope), "%s", lib.scope);
	for (int i = 0; i < ND_WINDOW; i++)
		lv_obj_set_hidden(p->rows[i], true);
	lv_timer_resume(pl_timer);
	if (!pl_skip_load)
		pl_load((int)(p - pl_pages));
	else
		pl_buttons(p);
}
static void pl_unloaded(lv_event_t *event) {
	pl_page_t *p = lv_event_get_user_data(event);
	p->download_token = ++pl_download_token;
	pl_download_menu_token++;
	p->download_pending = false;
	p->serial++;
	p->loading = p->reload = false;
	pl_release_art(p);
	popover_close();
	confirm_close();
	pl_name_hide();
}
static bool pl_back_peek(void) { return !lv_obj_is_hidden(pl_name_layer) || (pl_active(1) && pl_pages[1].selecting); }
static bool pl_back(void) {
	if (!lv_obj_is_hidden(pl_name_layer)) {
		pl_name_hide();
		return true;
	}
	if (pl_active(1) && pl_pages[1].selecting) {
		pl_pages[1].selecting = false;
		pl_pages[1].session++;
		pl_load(1);
		return true;
	}
	return false;
}
void navidromeplaylists_refresh(void) {
	nd_library_t lib;
	navidrome_library_snapshot(&lib);
	navidrome_snapshot_t state;
	navidrome_snapshot(&state);
	for (int i = 0; i < 4; i++) {
		pl_page_t *p = &pl_pages[i];
		if (!p->screen || !pl_active(i))
			continue;
		if (!state.unlocked || !*lib.path || (p->epoch && (strcmp(p->scope, lib.scope) || p->card != lib.card_epoch))) {
			p->serial++;
			p->loading = p->reload = false;
			pl_release_art(p);
			p->count = 0;
			p->selecting = false;
			for (int row = 0; row < ND_WINDOW; row++)
				lv_obj_set_hidden(p->rows[row], true);
			pl_name_hide();
			switch_screen_return_to(navidrome_screen);
			return;
		}
		if (i == 1) for (int row = 0; row < p->count; row++) {
			bool marked = navidromeplay_matches(&lib, ND_TRACKS, p->entries[row].track.id);
			playmark_show(p->playmarks[row], marked);
		}
		pl_take_art(p);
		if (p->epoch != lib.epoch && !navidrome_playlists_busy()) {
			pl_release_art(p);
			p->serial++;
			p->loading = false;
			p->epoch = lib.epoch;
			pl_load(i);
		}
	}
}
static void pl_tick(lv_timer_t *timer) {
	bool active = false;
	for (int i = 0; i < 4; i++)
		active |= pl_active(i);
	if (active) {
		navidromeplaylists_refresh();
		for (int i = 0; i < 4; i++) {
			if (pl_active(i) && pl_pages[i].reload && !pl_pages[i].loading && !navidrome_playlists_busy())
				pl_load(i);
			if (pl_active(i) && pl_pages[i].loading && !navidrome_playlists_busy() && lv_tick_elaps(pl_pages[i].submitted) > 2500) {
				// Recover a dropped GUI post with a local read, never by replaying a mutation.
				pl_pages[i].serial++;
				pl_pages[i].loading = false;
				pl_load(i);
			}
		}
	} else
		lv_timer_pause(timer);
}
void navidromeplaylists_open(void) {
	nd_library_t lib;
	navidrome_library_snapshot(&lib);
	navidrome_snapshot_t s;
	navidrome_snapshot(&s);
	if (!s.unlocked) {
		gui_notify_popup(navidrome_feedback(&s));
		return;
	}
	if (!*lib.path || !*lib.scope) {
		gui_notify_popup("nd_cache_storage");
		return;
	}
	pl_fixed = false;
	pl_pages[0].epoch = lib.epoch;
	pl_pages[0].card = lib.card_epoch;
	snprintf(pl_pages[0].scope, sizeof(pl_pages[0].scope), "%s", lib.scope);
	if (pl_active(0))
		pl_load(0);
	else
		switch_screen(pl_pages[0].screen);
}
void navidromeplaylists_pick(int64_t session, int64_t generation, lv_obj_t *origin) { navidromeplaylists_pick_source(session, generation, origin, false); }
void navidromeplaylists_pick_source(int64_t session, int64_t generation, lv_obj_t *origin, bool favorites) {
	pl_source_favorites = favorites;
	pl_session = session;
	pl_generation = generation;
	pl_origin = origin;
	if (pl_fixed && switcher_in_history(pl_pages[1].screen)) {
		pl_origin = pl_pages[1].screen;
		nd_pl_request_t q = {.action = NDPL_ADD, .id = pl_pages[1].playlist.id, .revision = pl_pages[1].playlist.revision, .session = session, .generation = generation, .favorites = favorites};
		pl_skip_load = true;
		switch_screen_return_to(pl_origin);
		pl_skip_load = false;
		pl_pages[1].loading = false;
		pl_pages[1].serial++;
		pl_action(1, &q);
		return;
	}
	pl_fixed = false;
	lv_obj_scroll_to_y(pl_pages[2].list, 0, LV_ANIM_OFF);
	switch_screen(pl_pages[2].screen);
}
static void pl_repaint(void) {
	for (int page = 0; page < 4; page++) {
		pl_page_t *p = &pl_pages[page];
		if (p->new_row) {
			lv_obj_set_style_image_recolor(p->new_icon, theme()->accent, 0);
			lv_obj_set_style_text_color(p->new_label, theme()->accent, 0);
		}
	}
}

// Music's playlist cards, with a fixed row pool instead of one widget per item.
static lv_obj_t *pl_card(lv_obj_t *parent, int height) {
	lv_obj_t *row = lv_btn_create(parent);
	lv_obj_set_size(row, lv_pct(100), height);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, ui_px(12), 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, ui_px(16), 0);
	lv_obj_set_style_pad_ver(row, ui_px(10), 0);
	lv_obj_set_style_pad_column(row, ui_px(14), 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_event_bubble(row, true);
	return row;
}
void navidromeplaylists_init(gui_config_t *cfg) {
	pl_cfg = cfg;
	for (int page = 0; page < 4; page++) {
		pl_page_t *p = &pl_pages[page];
		p->session = 1000 + page;
		p->pitch = ui_px(page == 1 ? 100 : 88) + ui_px(8);
		p->offset = page == 0 || page == 2 ? p->pitch : 0;
		p->screen = lv_obj_create(NULL);
		lv_obj_add_style(p->screen, &theme_style_screen, 0);
		p->title = settingsrow_title(p->screen, cfg, page == 2 ? "add_to_playlist" : page == 3 ? "nd_pl_import" : "playlists");
		settingsrow_title_corner_slots(p->title, cfg, page == 1 ? 4 : page == 2 ? 0 : 1);
		p->status = lv_label_create(p->screen);
		lv_obj_add_style(p->status, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(p->status, &font_ui_18, 0);
		lv_obj_set_pos(p->status, cfg->padding, settingsrow_content_top(cfg));
		lv_obj_set_width(p->status, cfg->screen_width - 2 * cfg->padding);
		lv_label_set_long_mode(p->status, LV_LABEL_LONG_DOT);
		lv_obj_set_height(p->status, ui_px(40));
		lv_obj_set_hidden(p->status, page == 0 || page == 2);
		int content_top = settingsrow_content_top(cfg) + (p->offset ? 0 : ui_px(44));
		p->list = lv_obj_create(p->screen);
		lv_obj_remove_style_all(p->list);
		lv_obj_set_pos(p->list, 0, content_top);
		lv_obj_set_size(p->list, cfg->screen_width, cfg->screen_height - content_top);
		lv_obj_set_style_pad_hor(p->list, cfg->padding, 0);
		lv_obj_set_scroll_dir(p->list, LV_DIR_VER);
		lv_obj_set_scrollbar_mode(p->list, LV_SCROLLBAR_MODE_AUTO);
		p->extent = lv_obj_create(p->list);
		lv_obj_remove_style_all(p->extent);
		lv_obj_set_size(p->extent, 1, 1);
		lv_obj_set_clickable(p->extent, false);
		lv_obj_set_scrollable(p->extent, false);
		if (p->offset) {
			p->new_row = pl_card(p->list, ui_px(88));
			p->new_icon = lv_image_create(p->new_row);
			lv_image_set_src(p->new_icon, &icon_plus);
			lv_obj_add_style(p->new_icon, &theme_style_icon, 0);
			lv_obj_set_style_image_recolor_opa(p->new_icon, LV_OPA_COVER, 0);
			p->new_label = lv_label_create(p->new_row);
			lv_label_set_text(p->new_label, tr("playlist_new_playlist"));
			lv_obj_add_style(p->new_label, &theme_style_text, 0);
			lv_obj_set_style_text_font(p->new_label, &font_ui_24, 0);
			lv_obj_set_flex_grow(p->new_label, 1);
			lv_label_set_long_mode(p->new_label, LV_LABEL_LONG_DOT);
			lv_obj_set_height(p->new_label, lv_font_get_line_height(&font_ui_24));
			lv_obj_add_event_cb(p->new_row, pl_button, LV_EVENT_CLICKED, (void *)(uintptr_t)(page * 3 + (page == 0 ? 1 : 0)));
			switcher_attach_back_gesture(p->new_row);
			player_sheet_attach_drag(p->new_row, true);
		}
		p->empty = lv_label_create(p->list);
		lv_label_set_text(p->empty, tr(page == 1 ? "nd_pl_no_tracks" : page == 0 || page == 2 ? "playlist_empty_note" : "nd_pl_empty"));
		lv_obj_add_style(p->empty, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(p->empty, &font_ui_24, 0);
		lv_obj_set_style_text_align(p->empty, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_width(p->empty, cfg->screen_width - 2 * cfg->padding);
		lv_obj_align(p->empty, LV_ALIGN_TOP_MID, 0, p->offset + ui_px(52));
		lv_obj_set_hidden(p->empty, true);
		for (int slot = 0; slot < 3; slot++) {
			lv_obj_t *button = p->buttons[slot] = lv_btn_create(p->screen);
			lv_obj_set_size(button, ui_px(56), ui_px(56));
			lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (ui_px(56) + ui_px(6)), cfg->padding + cfg->top_bar_height);
			lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
			lv_obj_set_style_border_width(button, 0, 0);
			lv_obj_set_style_shadow_width(button, 0, 0);
			lv_obj_set_style_pad_all(button, 0, 0);
			lv_obj_add_event_cb(button, pl_button, LV_EVENT_CLICKED, (void *)(uintptr_t)(page * 3 + slot));
			lv_obj_t *glyph = lv_image_create(button);
			lv_image_set_src(glyph, page == 1 ? (slot == 0 ? &icon_ellipsis_vertical : slot == 1 ? &icon_refresh : &icon_list_plus) : slot == 0 ? (page == 2 ? &icon_plus : page == 3 ? &icon_refresh : &icon_import) : &icon_plus);
			lv_obj_add_style(glyph, &theme_style_icon, 0);
			lv_obj_center(glyph);
			switcher_attach_back_gesture(button);
			player_sheet_attach_drag(button, true);
		}
		if (page == 1) {
			p->download = lv_btn_create(p->screen);
			lv_obj_set_size(p->download, ui_px(56), ui_px(56));
			lv_obj_align(p->download, LV_ALIGN_TOP_RIGHT, -cfg->padding - 3 * (ui_px(56) + ui_px(6)), cfg->padding + cfg->top_bar_height);
			lv_obj_set_style_bg_opa(p->download, LV_OPA_TRANSP, 0);
			lv_obj_set_style_border_width(p->download, 0, 0);
			lv_obj_set_style_shadow_width(p->download, 0, 0);
			lv_obj_set_style_pad_all(p->download, 0, 0);
			lv_obj_t *glyph = lv_image_create(p->download);
			lv_image_set_src(glyph, &icon_download);
			lv_obj_set_size(glyph, ui_px(24), ui_px(24));
			lv_image_set_inner_align(glyph, LV_IMAGE_ALIGN_CONTAIN);
			lv_obj_add_style(glyph, &theme_style_icon, 0);
			lv_obj_center(glyph);
			lv_obj_add_event_cb(p->download, pl_download, LV_EVENT_CLICKED, NULL);
			switcher_attach_back_gesture(p->download);
			player_sheet_attach_drag(p->download, true);
		}
		for (int i = 0; i < ND_WINDOW; i++) {
			uintptr_t key = (uintptr_t)(page * ND_WINDOW + i);
			lv_obj_t *row = p->rows[i] = pl_card(p->list, ui_px(page == 1 ? 100 : 88));
			if (page == 1) p->playmarks[i] = playmark_create(row);
			lv_obj_t *icon_parent = row;
			if (page == 1) {
				icon_parent = lv_obj_create(row);
				lv_obj_remove_style_all(icon_parent);
				lv_obj_set_size(icon_parent, ui_px(ND_ART_THUMB_SIZE), ui_px(ND_ART_THUMB_SIZE));
				lv_obj_set_scrollable(icon_parent, false);
				lv_obj_set_clickable(icon_parent, false);
			}
			lv_obj_t *icon = p->images[i] = lv_image_create(icon_parent);
			lv_image_set_src(icon, page == 1 ? &icon_music_note : &icon_list_music);
			int icon_size = ui_px(page == 1 ? ND_ART_THUMB_SIZE : 32);
			lv_obj_set_size(icon, icon_size, icon_size);
			lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
			if (page == 1)
				lv_obj_center(icon);
			lv_obj_add_style(icon, &theme_style_icon, 0);
			if (page != 1)
				lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
			lv_obj_t *texts = lv_obj_create(row);
			lv_obj_remove_style_all(texts);
			lv_obj_set_flex_grow(texts, 1);
			lv_obj_set_height(texts, lv_pct(100));
			lv_obj_set_scrollable(texts, false);
			lv_obj_set_clickable(texts, false);
			lv_obj_set_event_bubble(texts, true);
			lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
			lv_obj_set_flex_align(texts, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
			p->names[i] = lv_label_create(texts);
			p->details[i] = lv_label_create(texts);
			lv_obj_t *labels[] = {p->names[i], p->details[i]};
			for (int j = 0; j < 2; j++) {
				const lv_font_t *font = j ? (page == 1 ? &font_ui_20 : &font_ui_16) : &font_ui_24;
				lv_obj_add_style(labels[j], j ? &theme_style_text_dim : &theme_style_text, 0);
				lv_obj_set_style_text_font(labels[j], font, 0);
				lv_obj_set_width(labels[j], lv_pct(100));
				lv_label_set_long_mode(labels[j], LV_LABEL_LONG_DOT);
				lv_obj_set_height(labels[j], lv_font_get_line_height(font));
			}
			p->menus[i] = lv_btn_create(row);
			lv_obj_set_size(p->menus[i], ui_px(44), ui_px(44));
			lv_obj_set_style_pad_all(p->menus[i], 0, 0);
			lv_obj_align(p->menus[i], LV_ALIGN_RIGHT_MID, 0, 0);
			lv_obj_set_style_bg_opa(p->menus[i], LV_OPA_TRANSP, 0);
			lv_obj_set_style_border_width(p->menus[i], 0, 0);
			lv_obj_set_style_shadow_width(p->menus[i], 0, 0);
			lv_obj_t *glyph = lv_image_create(p->menus[i]);
			lv_image_set_src(glyph, &icon_ellipsis_vertical);
			lv_obj_add_style(glyph, &theme_style_icon, 0);
			lv_obj_center(glyph);
			p->checks[i] = lv_image_create(row);
			lv_image_set_src(p->checks[i], &icon_check);
			lv_obj_add_style(p->checks[i], &theme_style_icon, 0);
			lv_obj_set_floating(p->checks[i], true);
			lv_obj_align(p->checks[i], LV_ALIGN_TOP_LEFT, -ui_px(8), -ui_px(8));
			lv_obj_set_hidden(p->checks[i], true);
			lv_obj_add_event_cb(row, pl_row, LV_EVENT_CLICKED, (void *)key);
			lv_obj_add_event_cb(row, pl_long, LV_EVENT_LONG_PRESSED, (void *)key);
			lv_obj_add_event_cb(p->menus[i], pl_menu, LV_EVENT_CLICKED, (void *)key);
			switcher_attach_back_gesture(row);
			player_sheet_attach_drag(row, true);
			lv_obj_set_hidden(row, true);
		}
		lv_obj_add_event_cb(p->list, pl_scroll, LV_EVENT_SCROLL, p);
		lv_obj_add_event_cb(p->screen, pl_loaded, LV_EVENT_SCREEN_LOADED, p);
		lv_obj_add_event_cb(p->screen, pl_unloaded, LV_EVENT_SCREEN_UNLOADED, p);
		switcher_attach_back_gesture(p->screen);
		switcher_attach_back_gesture(p->list);
		player_sheet_attach_drag(p->screen, true);
		player_sheet_attach_drag(p->list, true);
		switcher_set_back_guard(p->screen, pl_back);
		switcher_set_back_guard_peek(p->screen, pl_back_peek);
		pl_buttons(p);
	}
	pl_repaint();
	theme_register_refresh(pl_repaint);
	pl_name_layer = lv_obj_create(pl_pages[0].screen);
	lv_obj_set_size(pl_name_layer, cfg->screen_width, cfg->screen_height);
	lv_obj_set_pos(pl_name_layer, 0, 0);
	lv_obj_set_style_border_width(pl_name_layer, 0, 0);
	lv_obj_set_style_radius(pl_name_layer, 0, 0);
	lv_obj_set_style_pad_all(pl_name_layer, 0, 0);
	lv_obj_add_style(pl_name_layer, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(pl_name_layer, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(pl_name_layer, false);
	lv_obj_t *heading = lv_label_create(pl_name_layer);
	lv_label_set_text(heading, tr("playlist_name"));
	lv_obj_add_style(heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(heading, &font_ui_24, 0);
	int heading_left = cfg->padding + ui_px(56) + ui_px(14);
	lv_obj_set_pos(heading, heading_left, cfg->top_bar_height + cfg->padding + ui_px(10));
	lv_obj_set_width(heading, cfg->screen_width - heading_left - cfg->padding - ui_px(70));
	lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);
	lv_obj_set_height(heading, lv_font_get_line_height(&font_ui_24));
	pl_name_field = lv_textarea_create(pl_name_layer);
	lv_textarea_set_one_line(pl_name_field, true);
	lv_textarea_set_max_length(pl_name_field, 200);
	lv_textarea_set_placeholder_text(pl_name_field, tr("name"));
	lv_obj_set_size(pl_name_field, cfg->screen_width - 2 * cfg->padding, ui_px(62));
	lv_obj_set_pos(pl_name_field, cfg->padding, cfg->top_bar_height + cfg->padding + ui_px(60));
	lv_obj_set_scrollbar_mode(pl_name_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_style(pl_name_field, &theme_style_card, 0);
	lv_obj_set_style_radius(pl_name_field, ui_px(12), 0);
	lv_obj_set_style_border_width(pl_name_field, 0, 0);
	lv_obj_set_style_shadow_width(pl_name_field, 0, 0);
	lv_obj_set_style_pad_all(pl_name_field, ui_px(14), 0);
	lv_obj_set_style_text_font(pl_name_field, &font_ui_24, 0);
	keyboard_style_caret(pl_name_field);
	pl_name_keyboard = keyboard_create(pl_name_layer, cfg->screen_width, ui_px(316), pl_name_field, NULL, "ok", pl_name_accept, NULL);
	lv_obj_t *cancel = lv_btn_create(pl_name_layer);
	lv_obj_set_size(cancel, ui_px(56), ui_px(56));
	lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_set_style_pad_all(cancel, 0, 0);
	lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_t *glyph = lv_image_create(cancel);
	lv_image_set_src(glyph, &icon_close);
	lv_obj_add_style(glyph, &theme_style_icon, 0);
	lv_obj_center(glyph);
	lv_obj_add_event_cb(cancel, pl_name_close, LV_EVENT_CLICKED, NULL);
	pl_name_hide();
	pl_timer = lv_timer_create(pl_tick, 200, NULL);
	lv_timer_pause(pl_timer);
}

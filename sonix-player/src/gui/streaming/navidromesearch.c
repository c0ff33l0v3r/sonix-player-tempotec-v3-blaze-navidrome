#include "navidromesearch.h"
#include "navidromealbum.h"
#include "navidromeart.h"
#include "navidromeofflinebadge.h"
#include "navidromepage.h"
#include "navidromeplaylists.h"
#include "navidromerow.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/uiscale.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/navidromeactions.h"
#include "src/system/streaming/navidromedownloads.h"
#include "src/system/streaming/navidromeplay.h"
#include "src/system/streaming/navidromeplaylists.h"
#include "src/system/streaming/navidromesearch.h"
#include "src/system/streaming/navidromestars.h"
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	lv_obj_t *screen, *list, *extent, *status, *reason, *title;
	lv_obj_t *rows[ND_WINDOW], *names[ND_WINDOW], *details[ND_WINDOW], *images[ND_WINDOW];
	cover_image_t art[ND_WINDOW];
	lv_obj_t *numbers[ND_WINDOW], *durations[ND_WINDOW];
	lv_obj_t *menus[ND_WINDOW], *checks[ND_WINDOW], *select_buttons[5], *selection_status;
	nd_owned_rows_t *selection, *child_rows;
	char retained_album[ND_ID_MAX];
	unsigned visit;
	int selected;
	bool chosen[ND_WINDOW], marks_dirty, action_busy;
	int art_slots[ND_WINDOW];
	bool art_bound[ND_WINDOW];
	nd_album_view_t album_view;
	nd_off_badges_t offline;
	char anchor[ND_ID_MAX];
	int anchor_delta;
	bool art_done[ND_WINDOW], loading, populated;
	nd_search_result_t data;
	nd_kind_t kind;
	int scroll[3], requested_first, status_top;
	char album[ND_ID_MAX];
	unsigned serial;
} search_page_t;
static search_page_t search_pages[3];
static lv_obj_t *search_field, *search_pills, *search_pill[3], *search_count[3], *search_clear;
static keyboard_t *search_keyboard;
static int search_pitch, search_width, search_height, search_keyboard_height, search_padding;
static lv_timer_t *search_timer;
static nd_context_t search_identity;
static char search_query[ND_SEARCH_QUERY];
static unsigned search_generation;
static int search_totals[3] = {-1, -1, -1};
static bool search_more[3];
static bool search_editing;
static const nd_kind_t search_kinds[] = {ND_TRACKS, ND_ALBUMS, ND_ARTISTS};
static const char *const search_labels[] = {"albums", "navidrome_tracks", "artists"};
typedef struct {
	unsigned generation, serial;
	int page, first;
	bool child, alternate, restore;
	unsigned layout_revision;
	int pitch, disc_pitch, anchor_delta;
	char anchor[ND_ID_MAX];
	nd_kind_t kind;
	char query[ND_SEARCH_QUERY], id[ND_ID_MAX];
	nd_search_result_t result;
	nd_owned_rows_t *child_rows;
} search_job_t;
static pthread_mutex_t search_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t search_ready = PTHREAD_COND_INITIALIZER;
static search_job_t search_pending;
static bool search_has_pending, search_worker_started;
static search_job_t *search_undelivered;
static unsigned search_live_generation;
static bool search_allocation_failed;
static unsigned search_failed_generation, search_failed_serial;
static int search_failed_page;
static void search_submit(search_page_t *p, int first, const char *id, nd_kind_t parent);
static void search_layout(search_page_t *p);

static bool search_gesture(void) { return switcher_back_drag_active() || player_sheet_drag_active() || coverflow_drag_active(); }
static int search_active_page(void) {
	for (int i = 0; i < 3; i++)
		if (lv_screen_active() == search_pages[i].screen)
			return i;
	return -1;
}
#include "navidromesearchactions.inc"
static void search_art_release(search_page_t *p) {
	for (int i = 0; i < ND_WINDOW; i++) {
		navidromeart_release(p->art_slots[i]);
		lv_obj_set_hidden(p->images[i], true);
		lv_image_set_src(p->images[i], &icon_music_note);
		lv_image_set_inner_align(p->images[i], LV_IMAGE_ALIGN_CONTAIN);
		lv_obj_add_style(p->images[i], &theme_style_icon, 0);
		cover_free(&p->art[i]);
		p->art_done[i] = true;
		p->art_bound[i] = false;
	}
}
static void search_art_remap(search_page_t *p, const nd_search_result_t *next) {
	int map[ND_WINDOW];
	bool claimed[ND_WINDOW] = {0};
	bool keep[ND_WINDOW] = {0};
	bool same = p->populated && p->data.source == next->source && p->data.kind == next->kind &&
		p->data.library.epoch == next->library.epoch && p->data.library.card_epoch == next->library.card_epoch &&
		p->data.library.generation == next->library.generation && !strcmp(p->data.library.scope, next->library.scope);
	for (int i = 0; i < ND_WINDOW; i++) {
		map[i] = -1;
		if (same && i < next->count)
			for (int j = 0; j < p->data.count; j++)
				if (!claimed[j] && next->rows[i].id[0] && p->art_bound[j] && !strcmp(p->data.rows[j].id, next->rows[i].id) && !strcmp(p->data.rows[j].cover, next->rows[i].cover)) {
					map[i] = j;
					claimed[j] = true;
					keep[i] = true;
					break;
				}
	}
	nd_row_complete_map(map);
	navidromeofflinebadge_release(&p->offline);
	ND_ROW_REORDER(p->menus, map);
	ND_ROW_REORDER(p->checks, map);
	memset(p->chosen, 0, sizeof(p->chosen));
	p->marks_dirty = p->selected > 0;
	ND_ROW_REORDER(p->rows, map);
	ND_ROW_REORDER(p->names, map);
	ND_ROW_REORDER(p->details, map);
	ND_ROW_REORDER(p->images, map);
	ND_ROW_REORDER(p->numbers, map);
	ND_ROW_REORDER(p->durations, map);
	ND_ROW_REORDER(p->art, map);
	ND_ROW_REORDER(p->art_done, map);
	ND_ROW_REORDER(p->art_bound, map);
	ND_ROW_REORDER(p->art_slots, map);
	ND_ROW_REORDER(p->offline.icons, map);
	for (int i = 0; i < ND_WINDOW; i++) {
		if (keep[i] && p->art[i].pixels)
			lv_image_set_src(p->images[i], &p->art[i].dsc);
		if (!keep[i] && p->art_bound[i]) {
			lv_image_set_src(p->images[i], &icon_music_note);
			cover_free(&p->art[i]);
			navidromeart_release(p->art_slots[i]);
			p->art_bound[i] = false;
			p->art_done[i] = true;
		}
	}
}
static void search_rows_hide(search_page_t *p) {
	navidromeofflinebadge_release(&p->offline);
	search_art_release(p);
	for (int i = 0; i < ND_WINDOW; i++)
		lv_obj_set_hidden(p->rows[i], true);
}
static void search_retire(void) {
	search_generation++;
	pthread_mutex_lock(&search_lock);
	search_live_generation = search_generation;
	search_has_pending = false;
	pthread_cond_signal(&search_ready);
	pthread_mutex_unlock(&search_lock);
	for (int i = 0; i < 3; i++) {
		search_selection_clear(&search_pages[i]);
		nd_owned_release(search_pages[i].child_rows);
		search_pages[i].child_rows = NULL;
		search_pages[i].retained_album[0] = 0;
		navidromealbum_release(&search_pages[i].album_view);
		search_pages[i].serial++;
		search_pages[i].loading = false;
		search_pages[i].populated = false;
		memset(&search_pages[i].data, 0, sizeof(search_pages[i].data));
		memset(search_pages[i].scroll, 0, sizeof(search_pages[i].scroll));
		search_rows_hide(&search_pages[i]);
		lv_label_set_text(search_pages[i].status, "");
		lv_label_set_text(search_pages[i].reason, "");
		if (i)
			lv_label_set_text(search_pages[i].title, tr("navidrome_search"));
	}
	for (int i = 0; i < 3; i++)
		search_totals[i] = -1;
}
static void search_paint_pills(void) {
	for (int k = 0; k < 3; k++) {
		char text[32];
		if (search_totals[k] < 0)
			lv_snprintf(text, sizeof(text), "…");
		else
			lv_snprintf(text, sizeof(text), "%d%s", search_totals[k], search_more[k] ? "+" : "");
		lv_label_set_text(search_count[k], text);
		bool selected = search_pages[0].kind == (nd_kind_t)k;
		lv_obj_set_style_bg_color(search_pill[k], selected ? theme()->accent : theme()->surface, 0);
		for (int i = 0; i < 2; i++)
			lv_obj_set_style_text_color(lv_obj_get_child(search_pill[k], i), selected ? lv_color_white() : theme()->text_primary, 0);
	}
}
static void search_paint_rows(search_page_t *p) {
	navidromeofflinebadge_release(&p->offline);
	for (int i = 0; i < ND_WINDOW; i++)
		lv_obj_set_hidden(p->rows[i], i >= p->data.count);
	int total = p->data.total + (p->data.more ? 1 : 0);
	if (total > INT_MAX / search_pitch) {
		lv_label_set_text(p->reason, tr("nd_search_child_limit"));
		search_layout(p);
		return;
	}

	lv_label_set_text(p->status, "");
	char reason[384];
	const char *empty = "no_results";
	// Navidrome can return a valid empty response for a single ASCII letter.
	// Keep server results authoritative and leave local matching unrestricted.
	if (p == search_pages && p->data.source == ND_SEARCH_SERVER && search_query[0] && !search_query[1])
		empty = "nd_search_more_text";
	lv_snprintf(reason, sizeof(reason), "%s%s%s", p->data.reason ? tr(p->data.reason) : "", p->data.reason && (p->data.error || !p->data.count) ? " · " : "", p->data.error ? tr(p->data.error) : !p->data.count ? tr(empty) : "");
	lv_label_set_text(p->reason, reason);

	int old_height = p->album_view.height;
	navidromealbum_bind(&p->album_view, p != search_pages && p->kind == ND_TRACKS && p->data.alternate, &p->data.album.info, &p->data.library, false, false);
	search_layout(p);
	int extent = p->data.alternate ? p->data.album.extent : total * search_pitch;
	if (extent > INT_MAX - p->album_view.height) {
		lv_label_set_text(p->reason, tr("nd_search_child_limit"));
		return;
	}
	lv_obj_set_height(p->extent, LV_MAX(1, extent + p->album_view.height));
	if (!p->album_view.active && p != search_pages && *p->data.album.info.row.title)
		lv_label_set_text(p->title, p->data.album.info.row.title);
	nd_row_title(p->title, p->album_view.cfg, p->select_buttons, 5);
	if (old_height && p->album_view.height && old_height != p->album_view.height && lv_obj_get_scroll_y(p->list) >= old_height) {
		lv_obj_scroll_to_y(p->list, lv_obj_get_scroll_y(p->list) + p->album_view.height - old_height, LV_ANIM_OFF);
	}
	for (int i = 0; i < p->data.count; i++) {
		nd_row_t *row = &p->data.rows[i];
		lv_obj_set_y(p->rows[i], p->album_view.height + (p->album_view.active ? p->data.album.y[i] : (p->data.first + i) * search_pitch));
		lv_label_set_text(p->names[i], row->title);
		lv_label_set_text(p->details[i], row->artist);
		lv_obj_set_hidden(p->rows[i], false);
		if (!p->art_bound[i]) {
			lv_image_set_src(p->images[i], &icon_music_note);
			lv_image_set_inner_align(p->images[i], LV_IMAGE_ALIGN_CONTAIN);
			lv_obj_add_style(p->images[i], &theme_style_icon, 0);
			p->art_bound[i] = true;
			p->art_done[i] = p->album_view.active || !*row->cover;
			if (*row->cover && !p->album_view.active)
				navidromeart_request(p->art_slots[i], &p->data.library, row->cover, ui_px(ND_ART_THUMB_SIZE));
		}
		lv_obj_set_hidden(p->images[i], false);
		navidromealbum_row(&p->album_view, p->rows[i], p->names[i], p->details[i], p->numbers[i], p->images[i], row, p->album_view.active && p->data.album.heading[i]);

	}
	lv_obj_update_layout(p->screen);
	for (int i = 0; i < p->data.count; i++)
		nd_row_footer(p->details[i], p->durations[i], lv_obj_get_style_width(p->names[i], 0), p->kind == ND_TRACKS ? p->data.rows[i].duration : 0, p->album_view.active && p->data.album.heading[i]);
	search_selection_ui(p);
	if (p->kind == ND_TRACKS || p->kind == ND_ALBUMS) {
		nd_off_request_t q = {.library = p->data.library, .kind = p->kind == ND_TRACKS ? ND_OFF_TRACK : ND_OFF_ALBUM, .count = p->data.count};
		for (int i = 0; i < q.count; i++)
			if (!p->album_view.active || !p->data.album.heading[i]) memcpy(q.items[i].id, p->data.rows[i].id, ND_ID_MAX);
		navidromeofflinebadge_bind(&p->offline, p->screen, &q, p->details);
	}
}
static void search_job_free(search_job_t *job) {
	if (job)
		nd_owned_release(job->child_rows);
	free(job);
}
static void search_done(void *value) {
	search_job_t *job = value;
	search_page_t *p = &search_pages[job->page];
	if ((job->page && job->layout_revision != navidromealbum_revision()) || job->generation != search_generation || p->serial != job->serial || search_active_page() != job->page || !navidrome_search_current(&search_identity, false)) {
		search_job_free(job);
		return;
	}
	p->loading = false;
	if (job->result.source == ND_SEARCH_SYNCED && !job->result.error) {
		nd_library_t now;
		navidrome_library_snapshot(&now);
		if (!now.ready || now.epoch != job->result.library.epoch || now.generation != job->result.library.generation) {
			search_rows_hide(p);
			if (!job->page)
				search_submit(p, job->first, NULL, p->kind);
			else
				switch_screen_return_to(search_pages[0].screen);
			search_job_free(job);
			return;
		}
	}
	if (job->page && job->result.error) {
		gui_notify_popup(job->result.error);
		switch_screen_return_to(search_pages[job->page - 1].screen);
		search_job_free(job);
		return;
	}
	if (!job->page) {
		if (p->data.source == ND_SEARCH_SERVER && job->result.source != ND_SEARCH_SERVER)
			job->result.reset = true;
		memcpy(search_totals, job->result.totals, sizeof(search_totals));
		memcpy(search_more, job->result.remaining, sizeof(search_more));
		if (job->result.reset) {
			memset(p->scroll, 0, sizeof(p->scroll));
			lv_obj_scroll_to_y(p->list, 0, LV_ANIM_OFF);
			if (!job->result.error && search_totals[p->kind] == 0)
				for (int i = 0; i < 3; i++)
					if (search_totals[search_kinds[i]] > 0) {
						p->kind = search_kinds[i];
						break;
					}
		}
		search_paint_pills();
		if (job->result.kind != p->kind && !job->result.error) {
			if (job->result.reset || !p->populated)
				search_submit(p, 0, NULL, p->kind);
			else
				for (int i = 0; i < 3; i++)
					if (search_totals[i] < 0) {
						search_submit(p, 0, NULL, (nd_kind_t)i);
						break;
					}
			search_job_free(job);
			return;
		}
	}
	if (job->result.reset || (p->populated && (p->data.source != job->result.source || (p->data.source == ND_SEARCH_SYNCED && p->data.library.generation != job->result.library.generation))))
		search_selection_clear(p);
	search_art_remap(p, &job->result);
	if (job->page && job->result.kind == ND_TRACKS) {
		nd_owned_release(p->child_rows);
		p->child_rows = job->child_rows;
		job->child_rows = NULL;
		snprintf(p->retained_album, sizeof(p->retained_album), "%s", job->result.album.info.row.id);
	}
	p->data = job->result;
	p->populated = true;
	search_paint_rows(p);
	if (job->restore) {
		lv_obj_scroll_to_y(p->list, LV_MAX(0, job->first * (job->alternate ? 1 : search_pitch) + p->album_view.height - job->anchor_delta), LV_ANIM_OFF);
		p->anchor[0] = 0;
	}
	if (!job->page && !p->data.error) {
		for (int i = 0; i < 3; i++)
			if (search_totals[i] < 0) {
				search_submit(p, 0, NULL, (nd_kind_t)i);
				break;
			}
	}
	search_job_free(job);
}
static bool search_job_current(void *value) {
	unsigned generation = *(unsigned *)value;
	pthread_mutex_lock(&search_lock);
	bool ok = generation == search_live_generation;
	pthread_mutex_unlock(&search_lock);
	return ok;
}
static void *search_work(void *unused) {
	(void)unused;
	thread_be_background("navidrome search");
	nd_search_t *session = NULL;
	unsigned generation = 0;
	for (;;) {
		pthread_mutex_lock(&search_lock);
		while (!search_has_pending && generation == search_live_generation)
			pthread_cond_wait(&search_ready, &search_lock);
		if (!search_has_pending) {
			generation = search_live_generation;
			pthread_mutex_unlock(&search_lock);
			nd_search_destroy(session);
			session = NULL;
			continue;
		}
		search_job_t *job = malloc(sizeof(*job));
		if (job)
			*job = search_pending;
		else {
			search_allocation_failed = true;
			search_failed_generation = search_pending.generation;
			search_failed_serial = search_pending.serial;
			search_failed_page = search_pending.page;
		}
		search_has_pending = false;
		pthread_mutex_unlock(&search_lock);
		if (!job)
			continue;
		if (generation != job->generation || !session) {
			nd_search_destroy(session);
			session = nd_search_create(job->query);
			generation = job->generation;
			if (session)
				nd_search_cancel_check(session, search_job_current, &generation);
		}
		if (!session)
			job->result.error = "nd_json_memory";
		else if (job->child)
			nd_search_child(session, job->page, job->kind, job->id, &job->result);
		else if (job->page)
			nd_search_child_read(session, job->page, job->alternate ? 0 : job->first, &job->result);
		else
			nd_search_read(session, job->kind, job->first, &job->result);

		if (session && job->page && job->result.kind == ND_TRACKS && !job->result.error) {
			if (job->restore) {
				int position;
				if (nd_search_child_position(session, job->page, job->anchor, job->pitch, job->disc_pitch, job->alternate, &position))
					job->first = job->alternate ? position : position / job->pitch;
				else
					job->restore = false;
			}
			nd_search_child_album_read(session, job->page, job->child ? 0 : job->first, job->pitch, job->disc_pitch, job->alternate, &job->result);
		}
		if (session && job->page && job->result.kind == ND_TRACKS && !job->result.error)
			job->child_rows = nd_search_child_retain(session, job->page);
		if (!gui_post(search_done, job)) {
			pthread_mutex_lock(&search_lock);
			search_job_free(search_undelivered);
			search_undelivered = job;
			pthread_mutex_unlock(&search_lock);
		}
	}
	return NULL;
}
static void search_submit(search_page_t *p, int first, const char *id, nd_kind_t kind) {
	p->loading = true;
	p->requested_first = first;
	p->serial++;
	pthread_mutex_lock(&search_lock);
	memset(&search_pending, 0, sizeof(search_pending));
	search_pending.generation = search_generation;
	search_pending.serial = p->serial;
	search_pending.page = (int)(p - search_pages);
	search_pending.first = first;
	search_pending.kind = kind;
	search_pending.alternate = navidromealbum_enabled() && p != search_pages && p->kind == ND_TRACKS;
	search_pending.layout_revision = navidromealbum_revision();
	search_pending.pitch = search_pitch;
	search_pending.disc_pitch = ui_px(ND_ALBUM_DISC_HEIGHT);
	search_pending.restore = *p->anchor != 0;
	search_pending.anchor_delta = p->anchor_delta;
	snprintf(search_pending.anchor, sizeof(search_pending.anchor), "%s", p->anchor);
	snprintf(search_pending.query, sizeof(search_pending.query), "%s", search_query);
	if (id) {
		search_pending.child = true;
		snprintf(search_pending.id, sizeof(search_pending.id), "%s", id);
	}
	search_has_pending = true;
	if (!search_worker_started) {
		pthread_t worker;
		if (!pthread_create(&worker, NULL, search_work, NULL)) {
			search_worker_started = true;
			pthread_detach(worker);
		} else {
			p->loading = search_has_pending = false;
			lv_label_set_text(p->reason, tr("nd_worker_error"));
		}
	}
	pthread_cond_signal(&search_ready);
	pthread_mutex_unlock(&search_lock);
	if (!p->loading)
		search_layout(p);
}
static void search_scroll(lv_event_t *e) {
	search_page_t *p = lv_event_get_user_data(e);
	search_menu_token++;
	popover_close();
	if (search_gesture() || !p->populated || p->data.error || (p == search_pages && !*search_query))
		return;

	navidromealbum_tick(&p->album_view);
	if (p->album_view.active) {
		int y = LV_MAX(0, nd_row_scroll(p->list, p->album_view.height + p->data.album.extent) - p->album_view.height);
		int last = p->data.count ? p->data.album.y[p->data.count - 1] + (p->data.album.heading[p->data.count - 1] ? ui_px(ND_ALBUM_DISC_HEIGHT) : search_pitch) : 0;
		if (!p->loading && (y < p->data.album.offset || (y + lv_obj_get_height(p->list) > last && last < p->data.album.extent)))
			search_submit(p, LV_MAX(0, y - 2 * search_pitch), NULL, p->kind);
		return;
	}
	int wanted = nd_row_first(p->list, p->data.total + (p->data.more ? 1 : 0), search_pitch, p->album_view.height);
	if (p->loading && wanted == p->requested_first)
		return;
	if (!nd_row_covered(p->list, p->data.first, p->data.count, p->data.total + (p->data.more ? 1 : 0), search_pitch, p->album_view.height)) {
		int y = nd_row_scroll(p->list, p->album_view.height + p->data.total * search_pitch);
		if (!p->data.more && p->data.first + p->data.count >= p->data.total && y / search_pitch >= p->data.first)
			return;
		search_submit(p, wanted, NULL, p->kind);
	}
}
static void search_layout(search_page_t *p) {
	bool root = p == search_pages;
	if (p->album_view.active) {
		lv_obj_set_y(p->list, p->album_view.cfg->top_bar_height);
		lv_obj_set_height(p->list, search_height - p->album_view.cfg->top_bar_height - search_padding);
		lv_obj_set_hidden(p->list, false);
		return;
	}
	if (p != search_pages) {
		lv_obj_set_pos(p->status, search_padding, p->status_top);
		lv_obj_set_pos(p->reason, search_padding, p->status_top + ui_px(24));
	}

	bool visible = root && keyboard_is_visible(search_keyboard);
	bool feedback = *lv_label_get_text(p->reason) != 0;
	bool status = *lv_label_get_text(p->status) != 0;
	int selection_height = p->selected ? ui_px(28) : 0;
	lv_obj_set_y(p->status, p->status_top + selection_height);
	lv_obj_set_y(p->reason, p->status_top + selection_height + (status ? ui_px(24) : 0));
	int top = p->status_top + selection_height + (status ? ui_px(24) : 0) + (feedback ? ui_px(42) : 0) + ((status || feedback) ? ui_px(4) : 0);
	bool compact = visible && search_height - search_keyboard_height < top + ui_px(160);
	if (root)
		lv_obj_set_hidden(search_pills, compact || !*search_query);
	lv_obj_set_hidden(p->status, compact || !status);
	lv_obj_set_hidden(p->reason, compact || !feedback);
	lv_obj_set_hidden(p->list, compact || (root && !*search_query));
	lv_obj_set_y(p->list, top);
	int height = search_height - top - (visible ? search_keyboard_height : 0) - search_padding;
	lv_obj_set_height(p->list, height > ui_px(40) ? height : ui_px(40));
}
static void search_keyboard_visible(bool visible) {
	keyboard_set_visible(search_keyboard, visible);
	search_layout(&search_pages[0]);
}
static void search_accept(lv_event_t *e) {
	(void)e;
	if (search_gesture())
		return;
	const char *text = lv_textarea_get_text(search_field);
	if (strlen(text) >= ND_SEARCH_QUERY) {
		gui_notify_popup("nd_input_long");
		return;
	}
	search_retire();
	snprintf(search_query, sizeof(search_query), "%s", text);
	bool nonblank = false;
	for (const unsigned char *c = (const unsigned char *)text; *c; c++)
		nonblank |= *c > 32;
	if (!nonblank) {
		search_query[0] = 0;
		search_keyboard_visible(true);
		return;
	}
	if (!navidrome_context(&search_identity, false)) {
		search_query[0] = 0;
		gui_notify_popup("nd_unlock_required");
		return;
	}
	nd_wipe(search_identity.password, sizeof(search_identity.password));
	search_pages[0].kind = ND_TRACKS;
	lv_obj_scroll_to_y(search_pages[0].list, 0, LV_ANIM_OFF);
	search_paint_pills();
	search_keyboard_visible(false);
	lv_label_set_text(search_pages[0].status, tr("nd_working"));
	lv_label_set_text(search_pages[0].reason, "");
	search_layout(&search_pages[0]);
	search_submit(&search_pages[0], 0, NULL, ND_TRACKS);
}
static void search_changed(lv_event_t *e) {
	(void)e;
	lv_obj_set_hidden(search_clear, !*lv_textarea_get_text(search_field));
	if (!search_editing && *search_query && strcmp(search_query, lv_textarea_get_text(search_field))) {
		search_retire();
		search_query[0] = 0;
		search_keyboard_visible(true);
	}
}
static void search_clear_clicked(lv_event_t *e) {
	(void)e;
	if (search_gesture())
		return;
	lv_textarea_set_text(search_field, "");
	search_retire();
	search_query[0] = 0;
	search_keyboard_visible(true);
}
static void search_field_clicked(lv_event_t *e) {
	(void)e;
	if (!search_gesture())
		search_keyboard_visible(true);
}
static void search_category_clicked(lv_event_t *e) {
	if (search_gesture() || !*search_query)
		return;
	search_page_t *p = &search_pages[0];
	nd_kind_t kind = (nd_kind_t)(intptr_t)lv_event_get_user_data(e);
	if (p->kind == kind)
		return;
	search_selection_clear(p);
	p->scroll[p->kind] = lv_obj_get_scroll_y(p->list);
	p->kind = kind;
	p->populated = false;
	search_rows_hide(p);
	search_paint_pills();
	lv_obj_scroll_to_y(p->list, p->scroll[kind], LV_ANIM_OFF);
	search_submit(p, p->scroll[kind] / search_pitch, NULL, kind);
}
static void search_row_clicked(lv_event_t *e) {
	if (search_gesture())
		return;
	int level = search_active_page();
	if (level < 0)
		return;
	search_page_t *p = &search_pages[level];
	if (p->loading || p->data.error)
		return;
	for (int i = 0; i < p->data.count; i++)
		if (lv_event_get_target(e) == p->rows[i] && *p->data.rows[i].id) {
			if (p->selected) {
				search_action_start(p, SEARCH_TOGGLE, i, ND_STAR_ADD);
				return;
			}
			if (p->action_busy)
				return;
			if (p->kind == ND_TRACKS) {
				nd_query_t q = {.kind = ND_TRACKS, .filter_kind = ND_ALBUMS};
				if (level)
					snprintf(q.filter, sizeof(q.filter), "%s", p->album);
				else
					snprintf(q.search, sizeof(q.search), "%s", search_query);
				bool ok = p->data.source == ND_SEARCH_SERVER ? navidromeplay_search(&p->data.library, &search_identity, &q, p->data.rows[i].id, p->screen) : navidromeplay_select(&p->data.library, &q, p->data.rows[i].id, p->screen);
				if (!ok)
					gui_notify_popup(*p->data.library.path ? "nd_worker_error" : "nd_cache_storage");
				return;
			}
			if (level >= 2)
				return;
			search_page_t *next = &search_pages[level + 1];
			next->kind = p->kind == ND_ARTISTS ? ND_ALBUMS : ND_TRACKS;
			if (p->kind == ND_ALBUMS)
				memmove(next->album, p->data.rows[i].id, sizeof(next->album));
			else
				next->album[0] = 0;
			next->populated = false;
			next->loading = false;
			lv_label_set_text(next->title, p->data.rows[i].title);
			lv_obj_scroll_to_y(next->list, 0, LV_ANIM_OFF);
			switch_screen(next->screen);
			lv_label_set_text(next->status, tr("nd_working"));
			lv_label_set_text(next->reason, "");
			search_layout(next);
			search_submit(next, 0, p->data.rows[i].id, p->kind);
			return;
		}
}
static void search_loaded(lv_event_t *e) {
	search_page_t *p = lv_event_get_user_data(e);
	lv_timer_resume(search_timer);
	// Check hidden pages before repainting their saved rows after Back.
	navidromesearch_refresh();
	if (lv_screen_active() != p->screen)
		return;
	if (p != search_pages && !*search_query) {
		switch_screen_return_to(search_pages[0].screen);
		return;
	}
	if (p == search_pages)
		search_keyboard_visible(!*search_query);
	if (p->populated && !p->loading)
		search_paint_rows(p);
	else if (p == search_pages && *search_query && !p->loading)
		search_submit(p, lv_obj_get_scroll_y(p->list) / search_pitch, NULL, p->kind);
}
static void search_unloaded(lv_event_t *e) {
	search_page_t *p = lv_event_get_user_data(e);
	search_selection_clear(p);
	navidromeplay_playlist_leave(p->screen);
	navidromeofflinebadge_release(&p->offline);
	p->serial++;
	p->loading = false;
	search_art_release(p);
	navidromealbum_release(&p->album_view);
}
void navidromesearch_refresh(void) {
	int level = search_active_page();
	if (level < 0)
		return;
	search_page_t *p = &search_pages[level];
	navidrome_snapshot_t state;
	navidrome_snapshot(&state);
	if (!state.unlocked || (*search_query && !navidrome_search_current(&search_identity, false))) {
		search_retire();
		search_query[0] = 0;
		search_editing = true;
		lv_textarea_set_text(search_field, "");
		search_editing = false;
		search_paint_pills();
		switch_screen_return_to(navidrome_screen);
		return;
	}
	nd_library_t current_library;
	navidrome_library_snapshot(&current_library);
	if (p->populated && p->data.source == ND_SEARCH_SERVER && p->data.library.card_epoch != current_library.card_epoch) {
		search_selection_clear(p);
		nd_owned_release(p->child_rows);
		p->child_rows = NULL;
		p->retained_album[0] = 0;
		if (p->data.card_backed) {
			search_rows_hide(p);
			p->populated = false;
			gui_notify_popup("nd_cache_storage");
			switch_screen_return_to(search_pages[level - 1].screen);
			return;
		}
		p->data.library = current_library;
		search_paint_rows(p);
	}
	if (p->populated && p->data.source == ND_SEARCH_SYNCED) {
		nd_library_t now;
		navidrome_library_snapshot(&now);
		if (!now.ready) {
			search_retire();
			search_query[0] = 0;
			switch_screen_return_to(navidrome_screen);
			return;
		}
		if (!p->loading && now.epoch != p->data.library.epoch) {
			if (level) {
				switch_screen_return_to(search_pages[0].screen);
				return;
			}
			search_rows_hide(p);
			search_submit(p, lv_obj_get_scroll_y(p->list) / search_pitch, NULL, p->kind);
		}
	}

	if (p != search_pages && p->kind == ND_TRACKS && p->album_view.revision != navidromealbum_revision() && p->populated) {
		int y = lv_obj_get_scroll_y(p->list);
		for (int i = 0; i < p->data.count; i++) {
			if (*p->data.rows[i].id && lv_obj_get_y(p->rows[i]) + search_pitch > y) {
				memcpy(p->anchor, p->data.rows[i].id, sizeof(p->anchor));
				p->anchor_delta = lv_obj_get_y(p->rows[i]) - y;
				break;
			}
		}
		p->album_view.revision = navidromealbum_revision();
		search_submit(p, 0, NULL, p->kind);
	}
	bool was_tinted = p->album_view.tinted;
	navidromealbum_tick(&p->album_view);
	if (was_tinted != p->album_view.tinted) {
		for (int i = 0; i < p->data.count; i++) {
			navidromealbum_row(&p->album_view, p->rows[i], p->names[i], p->details[i], p->numbers[i], p->images[i], &p->data.rows[i], p->data.album.heading[i]);
		nd_row_footer(p->details[i], p->durations[i], lv_obj_get_style_width(p->names[i], 0), p->kind == ND_TRACKS ? p->data.rows[i].duration : 0, p->album_view.active && p->data.album.heading[i]);
		}
		search_selection_ui(p);
	}
	navidromeofflinebadge_tick(&p->offline);
	for (int i = 0; i < p->data.count; i++)
		if (!p->art_done[i] && navidromeart_take(p->art_slots[i], &p->art[i], &p->art_done[i])) {
			lv_obj_remove_style(p->images[i], &theme_style_icon, 0);
			lv_image_set_inner_align(p->images[i], LV_IMAGE_ALIGN_CONTAIN);
			lv_image_set_src(p->images[i], &p->art[i].dsc);
			lv_obj_set_hidden(p->images[i], false);
		}
}
static void search_tick(lv_timer_t *timer) {
	pthread_mutex_lock(&search_lock);
	search_job_t *done = search_undelivered;
	search_undelivered = NULL;
	bool failed = search_allocation_failed;
	unsigned failed_generation = search_failed_generation, failed_serial = search_failed_serial;
	int failed_page = search_failed_page;
	search_allocation_failed = false;
	pthread_mutex_unlock(&search_lock);
	if (failed && failed_generation == search_generation && search_active_page() == failed_page && search_pages[failed_page].serial == failed_serial) {
		search_page_t *p = &search_pages[failed_page];
		p->loading = false;
		p->populated = true;
		memset(&p->data, 0, sizeof(p->data));
		p->data.error = "nd_json_memory";
		search_paint_rows(p);
	}
	if (done)
		search_done(done);
	if (search_active_page() < 0) {
		if (!switcher_in_history(search_pages[0].screen) && *search_query) {
			search_retire();
			search_query[0] = 0;
		}
		lv_timer_pause(timer);
	} else {
		navidrome_poll_network();
		navidromesearch_refresh();
	}
}
void navidromesearch_open(void) {
	navidrome_snapshot_t state;
	nd_library_t library;
	navidrome_poll_network();
	navidrome_snapshot(&state);
	navidrome_library_snapshot(&library);
	if (!state.unlocked) {
		gui_notify_popup(navidrome_feedback(&state));
		return;
	}
	if (state.state != ND_CONNECTED && !library.ready) {
		gui_notify_popup(library.error ? library.error : "nd_not_synced");
		return;
	}
	search_retire();
	search_query[0] = 0;
	search_editing = true;
	lv_textarea_set_text(search_field, "");
	search_editing = false;
	switch_screen(search_pages[0].screen);
	search_keyboard_visible(true);
}
void navidromesearch_init(gui_config_t *cfg) {
	search_width = cfg->screen_width;
	search_height = cfg->screen_height;
	search_padding = cfg->padding;
	search_pitch = ui_px(108);
	search_keyboard_height = ui_px(316);
	for (int level = 0; level < 3; level++) {
		search_page_t *p = &search_pages[level];
		p->screen = lv_obj_create(NULL);
		navidromepage_register(p->screen);
		lv_obj_add_style(p->screen, &theme_style_screen, 0);
		p->title = settingsrow_title(p->screen, cfg, "navidrome_search");
		int top = settingsrow_content_top(cfg);
		if (!level)
			top += ui_px(124);
		p->status_top = top;
		p->status = lv_label_create(p->screen);
		p->reason = lv_label_create(p->screen);
		lv_obj_t *labels[] = {p->status, p->reason};
		for (int i = 0; i < 2; i++) {
			lv_obj_add_style(labels[i], &theme_style_text_dim, 0);
			lv_obj_set_style_text_font(labels[i], &font_ui_16, 0);
			lv_obj_set_width(labels[i], search_width - 2 * cfg->padding);
			lv_obj_set_height(labels[i], ui_px(i ? 42 : 24));
			lv_label_set_long_mode(labels[i], LV_LABEL_LONG_DOT);
			lv_obj_set_pos(labels[i], cfg->padding, top + ui_px(i ? 24 : 0));
		}
		top += ui_px(28);
		p->list = lv_obj_create(p->screen);
		lv_obj_remove_style_all(p->list);
		lv_obj_set_pos(p->list, cfg->padding, top);
		lv_obj_set_size(p->list, search_width - 2 * cfg->padding, search_height - top - cfg->padding);
		lv_obj_set_scroll_dir(p->list, LV_DIR_VER);
		p->extent = lv_obj_create(p->list);
		lv_obj_remove_style_all(p->extent);
		lv_obj_set_size(p->extent, 1, 1);
		lv_obj_set_clickable(p->extent, false);
		lv_obj_set_scrollable(p->extent, false);
		for (int i = 0; i < ND_WINDOW; i++) {
			nd_row_widgets_t row = nd_row_create(p->list, cfg);
			p->rows[i] = row.row;
			p->images[i] = row.image;
			p->names[i] = row.name;
			p->details[i] = row.detail;
			p->durations[i] = row.duration;
			p->numbers[i] = row.number;
			p->art_slots[i] = ND_ART_SEARCH_SLOT + i;
			lv_obj_add_event_cb(p->rows[i], search_row_clicked, LV_EVENT_CLICKED, NULL);
			switcher_attach_back_gesture(p->rows[i]);
			player_sheet_attach_drag(p->rows[i], true);
		}

		navidromealbum_init(&p->album_view, cfg, p->screen, p->list, p->title, p->status, p->reason, ND_ART_SEARCH_ALBUM_SLOT, NULL, p);
		search_actions_init(p, cfg);
		lv_obj_add_event_cb(p->list, search_scroll, LV_EVENT_SCROLL, p);
		lv_obj_add_event_cb(p->screen, search_loaded, LV_EVENT_SCREEN_LOADED, p);
		lv_obj_add_event_cb(p->screen, search_unloaded, LV_EVENT_SCREEN_UNLOADED, p);
		switcher_attach_back_gesture(p->screen);
		player_sheet_attach_drag(p->screen, true);
		switcher_attach_back_gesture(p->list);
		player_sheet_attach_drag(p->list, true);
	}
	lv_obj_t *screen = search_pages[0].screen;
	int top = settingsrow_content_top(cfg);
	search_field = lv_textarea_create(screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_max_length(search_field, ND_SEARCH_QUERY); // bytes checked separately at submission
	lv_textarea_set_placeholder_text(search_field, tr("search"));
	lv_obj_add_style(search_field, &theme_style_card, 0);
	lv_obj_set_style_border_width(search_field, 0, 0);
	lv_obj_set_style_shadow_width(search_field, 0, 0);
	lv_obj_set_scrollbar_mode(search_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_style_text_font(search_field, &font_ui_24, 0);
	lv_obj_set_style_pad_all(search_field, ui_px(12), 0);
	lv_obj_set_pos(search_field, cfg->padding, top);
	lv_obj_set_size(search_field, search_width - 2 * cfg->padding, ui_px(60));
	lv_obj_set_style_pad_right(search_field, ui_px(52), 0);
	keyboard_style_caret(search_field);
	lv_obj_add_event_cb(search_field, search_field_clicked, LV_EVENT_CLICKED, NULL);
	search_clear = lv_btn_create(screen);
	lv_obj_set_size(search_clear, ui_px(48), ui_px(48));
	lv_obj_set_pos(search_clear, search_width - cfg->padding - ui_px(50), top + ui_px(4));
	lv_obj_set_style_bg_opa(search_clear, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(search_clear, 0, 0);
	lv_obj_set_style_shadow_width(search_clear, 0, 0);
	lv_obj_set_style_pad_all(search_clear, 0, 0);
	lv_obj_set_scrollable(search_clear, false);
	lv_obj_set_scrollbar_mode(search_clear, LV_SCROLLBAR_MODE_OFF);
	lv_obj_t *glyph = lv_image_create(search_clear);
	lv_obj_add_style(glyph, &theme_style_icon, 0);
	lv_image_set_src(glyph, &icon_clear);
	lv_obj_center(glyph);
	lv_obj_add_event_cb(search_clear, search_clear_clicked, LV_EVENT_CLICKED, NULL);
	lv_obj_set_hidden(search_clear, true);
	lv_obj_add_event_cb(search_field, search_changed, LV_EVENT_VALUE_CHANGED, NULL);
	search_pills = lv_obj_create(screen);
	lv_obj_remove_style_all(search_pills);
	lv_obj_set_scrollable(search_pills, false);
	lv_obj_set_pos(search_pills, cfg->padding, top + ui_px(66));
	lv_obj_set_size(search_pills, search_width - 2 * cfg->padding, ui_px(52));
	lv_obj_set_flex_flow(search_pills, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(search_pills, ui_px(6), 0);
	for (int i = 0; i < 3; i++) {
		int k = search_kinds[i];
		search_pill[k] = lv_btn_create(search_pills);
		lv_obj_set_style_radius(search_pill[k], LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_border_width(search_pill[k], 0, 0);
		lv_obj_set_style_shadow_width(search_pill[k], 0, 0);
		lv_obj_set_scrollable(search_pill[k], false);
		lv_obj_set_width(search_pill[k], 0);
		lv_obj_set_flex_grow(search_pill[k], 1);
		lv_obj_set_style_pad_row(search_pill[k], 0, 0);
		lv_obj_set_height(search_pill[k], ui_px(52));
		lv_obj_set_style_pad_all(search_pill[k], ui_px(4), 0);
		lv_obj_set_flex_flow(search_pill[k], LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(search_pill[k], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_t *name = lv_label_create(search_pill[k]);
		lv_label_set_text(name, tr(search_labels[k]));
		lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
		lv_obj_set_style_max_width(name, lv_pct(100), 0);
		lv_obj_set_style_text_font(name, &font_ui_16, 0);
		search_count[k] = lv_label_create(search_pill[k]);
		lv_obj_set_style_text_font(search_count[k], &font_ui_16, 0);
		lv_obj_add_event_cb(search_pill[k], search_category_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)k);
	}
	search_keyboard = keyboard_create(screen, search_width, search_keyboard_height, search_field, &icon_search, NULL, search_accept, NULL);
	search_timer = lv_timer_create(search_tick, 100, NULL);
	lv_timer_create(search_actions_poll, 100, NULL);
	lv_timer_pause(search_timer);
	theme_register_refresh(search_paint_pills);
	search_paint_pills();
	search_keyboard_visible(true);
}

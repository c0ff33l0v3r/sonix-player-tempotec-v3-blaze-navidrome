#include "navidromediscovery.h"
#include "navidromealbum.h"
#include "navidromeofflinebadge.h"
#include "navidromeplaylists.h"
#include "navidromepage.h"
#include "navidromerow.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/playmark.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/popover.h"
#include "src/system/streaming/navidromestars.h"
#include "src/system/core/config.h"
#include "src/system/streaming/navidromedownloads.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/streaming/navidromecache.h"
#include "src/system/streaming/navidromediscovery.h"
#include "src/system/streaming/navidromeplay.h"
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	lv_obj_t *screen, *list, *extent, *status, *reason, *title;
	lv_obj_t *rows[ND_WINDOW], *names[ND_WINDOW], *details[ND_WINDOW], *images[ND_WINDOW];
	cover_image_t art[ND_WINDOW];
	lv_obj_t *numbers[ND_WINDOW], *durations[ND_WINDOW], *marks[ND_WINDOW], *menus[ND_WINDOW];
	int art_slots[ND_WINDOW];
	bool art_bound[ND_WINDOW];
	nd_album_view_t album_view;
	nd_off_badges_t offline;
	nd_album_membership_t *members;
	char anchor[ND_ID_MAX];
	int anchor_delta;
	bool art_done[ND_WINDOW], loading, populated, action_busy;
	nd_search_result_t data;
	nd_kind_t kind;
	int scroll[3], requested_first, status_top;
	char album[ND_ID_MAX];
	unsigned serial;
} discovery_page_t;

static discovery_page_t discovery_pages[2];
static lv_obj_t *discovery_menu, *discovery_grid, *discovery_refresh_button;
static int discovery_width, discovery_height, discovery_padding, discovery_pitch;
static lv_timer_t *discovery_timer;
static nd_context_t discovery_identity;
static nd_discovery_mode_t discovery_mode;
static unsigned discovery_visit, discovery_request, discovery_child_epoch;
static const char *const discovery_labels[] = {"nd_discovery_newest", "nd_discovery_recent", "nd_discovery_frequent", "nd_discovery_random"};
typedef struct {
	unsigned visit, request, serial, layout_revision, network_epoch;
	int page, first, pitch, disc_pitch;
	bool child, refresh, alternate, restore;
	char id[ND_ID_MAX], anchor[ND_ID_MAX];
	int anchor_delta;
	nd_discovery_mode_t mode;
	nd_search_result_t result;
	nd_album_membership_t *members;
} discovery_job_t;
static pthread_mutex_t discovery_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t discovery_ready = PTHREAD_COND_INITIALIZER;
static discovery_job_t discovery_pending, discovery_retry;
static discovery_job_t *discovery_undelivered;
static bool discovery_has_pending, discovery_started, discovery_failed;
static unsigned discovery_live_visit, discovery_live_request, discovery_failed_request;
static bool discovery_retry_valid;
static void discovery_submit(discovery_page_t *p, int first, const char *id, nd_kind_t kind);
static void discovery_layout(discovery_page_t *p);
static void discovery_album_action(void *user, nd_album_action_t action);
static void discovery_menu_clicked(lv_event_t *event);
static bool discovery_gesture(void) { return switcher_back_drag_active() || player_sheet_drag_active() || coverflow_drag_active(); }
static int discovery_active_page(void) {
	for (int i = 0; i < 2; i++)
		if (lv_screen_active() == discovery_pages[i].screen)
			return i;
	return -1;
}

static void discovery_art_release(discovery_page_t *p) {
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
static void discovery_art_remap(discovery_page_t *p, const nd_search_result_t *next) {
	int map[ND_WINDOW];
	bool claimed[ND_WINDOW] = {0};
	bool keep[ND_WINDOW] = {0};
	bool same = p->populated && p->data.source == next->source && p->data.kind == next->kind && p->data.library.epoch == next->library.epoch && p->data.library.card_epoch == next->library.card_epoch && p->data.library.generation == next->library.generation && !strcmp(p->data.library.scope, next->library.scope);
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
	ND_ROW_REORDER(p->rows, map);
	ND_ROW_REORDER(p->names, map);
	ND_ROW_REORDER(p->details, map);
	ND_ROW_REORDER(p->images, map);
	ND_ROW_REORDER(p->numbers, map);
	ND_ROW_REORDER(p->marks, map);
	ND_ROW_REORDER(p->menus, map);
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
static void discovery_rows_hide(discovery_page_t *p) {
	navidromeofflinebadge_release(&p->offline);
	nd_album_membership_release(p->members);
	p->members = NULL;
	discovery_art_release(p);
	for (int i = 0; i < ND_WINDOW; i++)
		lv_obj_set_hidden(p->rows[i], true);
}

static void discovery_paint_rows(discovery_page_t *p) {
	for (int i = 0; i < ND_WINDOW; i++)
		lv_obj_set_hidden(p->rows[i], i >= p->data.count);
	int64_t extent_rows = (int64_t)p->data.total + (p->data.more ? 1 : 0);
	if (extent_rows < 0 || extent_rows > LV_COORD_MAX / discovery_pitch) {
		navidromeofflinebadge_release(&p->offline);
		lv_label_set_text(p->reason, tr("nd_search_child_limit"));
		discovery_layout(p);
		return;
	}

	int total = (int)extent_rows;
	char count_text[64];
	lv_snprintf(count_text, sizeof(count_text), "%d%s", p->data.total, p->data.more ? "+" : "");
	lv_label_set_text(p->status, p == discovery_pages ? count_text : "");
	char reason[384];
	const char *empty = "no_results";
	lv_snprintf(reason, sizeof(reason), "%s%s%s", p->data.reason ? tr(p->data.reason) : "", p->data.reason && (p->data.error || !p->data.count) ? " · " : "", p->data.error ? tr(p->data.error) : !p->data.count ? tr(empty) : "");
	lv_label_set_text(p->reason, reason);

	int old_height = p->album_view.height;
	navidromealbum_bind(&p->album_view, p != discovery_pages && p->kind == ND_TRACKS && p->data.alternate, &p->data.album.info, &p->data.library, true, p->data.total > 0 && !p->loading && !p->action_busy);
	discovery_layout(p);
	int extent = p->data.alternate ? p->data.album.extent : total * discovery_pitch;
	if (extent > LV_COORD_MAX - p->album_view.height) {
		navidromeofflinebadge_release(&p->offline);
		lv_label_set_text(p->reason, tr("nd_search_child_limit"));
		return;
	}
	lv_obj_set_height(p->extent, LV_MAX(1, extent + p->album_view.height));
	if (!p->album_view.active && p != discovery_pages && *p->data.album.info.row.title)
		lv_label_set_text(p->title, p->data.album.info.row.title);
	lv_obj_t *controls[] = {discovery_refresh_button};
	nd_row_title(p->title, p->album_view.cfg, p == discovery_pages ? controls : NULL, p == discovery_pages ? 1 : 0);
	if (old_height && p->album_view.height && old_height != p->album_view.height && lv_obj_get_scroll_y(p->list) >= old_height) {
		lv_obj_scroll_to_y(p->list, lv_obj_get_scroll_y(p->list) + p->album_view.height - old_height, LV_ANIM_OFF);
	}
	for (int i = 0; i < p->data.count; i++) {
		nd_row_t *row = &p->data.rows[i];
		lv_obj_set_y(p->rows[i], p->album_view.height + (p->album_view.active ? p->data.album.y[i] : (p->data.first + i) * discovery_pitch));
		lv_label_set_text(p->names[i], row->title);
		lv_label_set_text(p->details[i], row->artist);
		lv_obj_set_hidden(p->rows[i], false);
		playmark_show(p->marks[i], navidromeplay_matches(&p->data.library, p->kind, row->id));
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
	if (p->kind == ND_TRACKS || p->kind == ND_ALBUMS) {
		nd_off_request_t q = {.library = p->data.library, .kind = p->kind == ND_TRACKS ? ND_OFF_TRACK : ND_OFF_ALBUM, .count = p->data.count};
		for (int i = 0; i < q.count; i++)
			if (!p->album_view.active || !p->data.album.heading[i])
				memcpy(q.items[i].id, p->data.rows[i].id, ND_ID_MAX);
		if (p->album_view.active && p->members) {
			memcpy(q.header.id, p->album_view.info.row.id, ND_ID_MAX);
			q.header.members = p->members;
		}
		navidromeofflinebadge_bind_album(&p->offline, p->screen, &q, p->details, p->album_view.active ? &p->album_view : NULL);
	}
}

static void discovery_layout(discovery_page_t *p) {
	bool feedback = *lv_label_get_text(p->reason) != 0;
	bool status = *lv_label_get_text(p->status) != 0;
	lv_obj_set_hidden(p->status, !status || p->album_view.active);
	lv_obj_set_hidden(p->reason, !feedback || p->album_view.active);
	lv_obj_set_y(p->reason, p->status_top + (status ? ui_px(24) : 0));
	int top = p->album_view.active ? p->album_view.cfg->top_bar_height : p->status_top + (status ? ui_px(24) : 0) + (feedback ? ui_px(42) : 0);
	lv_obj_set_y(p->list, top);
	lv_obj_set_height(p->list, LV_MAX(ui_px(40), discovery_height - top - discovery_padding));
}
static void discovery_cancel_request(void) {
	pthread_mutex_lock(&discovery_lock);
	discovery_live_request = ++discovery_request;
	discovery_has_pending = false;
	pthread_cond_signal(&discovery_ready);
	pthread_mutex_unlock(&discovery_lock);
}
static void discovery_retire(void) {
	discovery_cancel_request();
	pthread_mutex_lock(&discovery_lock);
	discovery_live_visit = ++discovery_visit;
	pthread_cond_signal(&discovery_ready);
	pthread_mutex_unlock(&discovery_lock);
	discovery_retry_valid = false;
	for (int i = 0; i < 2; i++) {
		discovery_page_t *p = &discovery_pages[i];
		navidromeplay_discovery_leave(p->screen);
		navidromealbum_release(&p->album_view);
		discovery_rows_hide(p);
		p->serial++;
		p->loading = p->populated = false;
		memset(&p->data, 0, sizeof(p->data));
		lv_label_set_text(p->status, "");
		lv_label_set_text(p->reason, "");
	}
}
static bool discovery_job_current(void *arg) {
	unsigned token = *(unsigned *)arg;
	pthread_mutex_lock(&discovery_lock);
	bool ok = token == discovery_live_request;
	pthread_mutex_unlock(&discovery_lock);
	return ok;
}
static void discovery_job_free(discovery_job_t *job) {
	if (job) {
		nd_album_membership_release(job->members);
		free(job);
	}
}
static void discovery_done(void *arg) {
	discovery_job_t *job = arg;
	discovery_page_t *p = &discovery_pages[job->page];
	if (job->visit != discovery_visit || job->request != discovery_request || job->serial != p->serial || discovery_active_page() != job->page || !navidrome_search_current(&discovery_identity, false)) {
		discovery_job_free(job);
		return;
	}
	p->loading = false;
	nd_context_t now;
	bool same_network = navidrome_context(&now, false) && now.search_network_epoch == job->network_epoch;
	nd_wipe(&now, sizeof(now));
	if (!same_network)
		job->result.error = "nd_reconnect_required";
	nd_library_t library;
	navidrome_library_snapshot(&library);
	if (job->result.card_backed && (job->result.library.card_epoch != library.card_epoch || !nd_cache_io_current(discovery_child_epoch)))
		job->result.error = "nd_cache_storage";
	else if (!job->result.card_backed)
		job->result.library = library;
	if ((int64_t)job->result.total + job->result.more > LV_COORD_MAX / discovery_pitch || job->result.total < 0 || job->result.album.extent > LV_COORD_MAX)
		job->result.error = "nd_search_child_limit";
	if (job->layout_revision != navidromealbum_revision() && job->page) {
		discovery_job_free(job);
		return;
	}
	if (job->result.error) {
		if (p->album_view.active)
			gui_notify_popup(job->result.error);
		discovery_retry = *job;
		discovery_retry.members = NULL;
		discovery_retry_valid = true;
		if (job->child) {
			gui_notify_popup(job->result.error);
			switch_screen_return_to(discovery_pages[0].screen);
			p = &discovery_pages[0];
		}
		lv_label_set_text(p->status, "");
		char text[384];
		lv_snprintf(text, sizeof(text), "%s · %s", tr(job->result.error), tr("nd_discovery_retry"));
		lv_label_set_text(p->reason, text);
		discovery_layout(p);
		discovery_job_free(job);
		return;
	}
	discovery_retry_valid = false;
	discovery_art_remap(p, &job->result);
	nd_album_membership_release(p->members);
	p->members = job->members;
	job->members = NULL;
	p->data = job->result;
	p->populated = true;
	if (job->refresh || job->child)
		lv_obj_scroll_to_y(p->list, 0, LV_ANIM_OFF);
	discovery_paint_rows(p);
	if (job->restore)
		lv_obj_scroll_to_y(p->list, LV_MAX(0, (job->alternate ? job->first : job->first * discovery_pitch) + p->album_view.height - job->anchor_delta), LV_ANIM_OFF);
	p->anchor[0] = 0;
	discovery_job_free(job);
}
static void *discovery_work(void *arg) {
	(void)arg;
	thread_be_background("navidrome discovery");
	nd_discovery_t *session = NULL;
	unsigned visit = 0, token = 0;
	for (;;) {
		nd_album_membership_collect();
		pthread_mutex_lock(&discovery_lock);
		while (!discovery_has_pending && visit == discovery_live_visit)
			pthread_cond_wait(&discovery_ready, &discovery_lock);
		if (!discovery_has_pending) {
			visit = discovery_live_visit;
			pthread_mutex_unlock(&discovery_lock);
			nd_discovery_free(session);
			session = NULL;
			continue;
		}
		discovery_job_t *job = malloc(sizeof(*job));
		if (job)
			*job = discovery_pending;
		else {
			discovery_failed = true;
			discovery_failed_request = discovery_pending.request;
		}
		discovery_has_pending = false;
		pthread_mutex_unlock(&discovery_lock);
		if (!job)
			continue;
		token = job->request;
		if (visit != job->visit) {
			nd_discovery_free(session);
			session = NULL;
			visit = job->visit;
		}
		bool candidate = !session || job->refresh;
		nd_discovery_t *next = candidate ? nd_discovery_create(job->mode) : session;
		if (next)
			nd_discovery_cancel(next, discovery_job_current, &token);
		nd_context_t now;
		bool same_network = navidrome_context(&now, false) && now.search_network_epoch == job->network_epoch;
		nd_wipe(&now, sizeof(now));
		if (!same_network)
			job->result.error = "nd_reconnect_required";
		else if (!next)
			job->result.error = "nd_json_memory";
		else if (job->child)
			nd_discovery_album_open(next, job->id, &job->result);
		else if (!job->page)
			nd_discovery_read(next, job->first, &job->result);
		if (next && job->page && !job->result.error) {
			if (job->restore) {
				int position;
				if (nd_discovery_album_position(next, job->anchor, job->pitch, job->disc_pitch, job->alternate, &position))
					job->first = job->alternate ? position : position / job->pitch;
				else
					job->restore = false;
			}
			nd_discovery_album_read(next, job->child ? 0 : job->first, job->pitch, job->disc_pitch, job->alternate, &job->result);
		}
		if (next && job->page && !job->result.error && discovery_job_current(&token))
			job->members = nd_discovery_album_membership(next);
		if (candidate) {
			if (!job->result.error && discovery_job_current(&token)) {
				nd_discovery_free(session);
				session = next;
			} else
				nd_discovery_free(next);
		}
		if (!gui_post(discovery_done, job)) {
			pthread_mutex_lock(&discovery_lock);
			discovery_job_free(discovery_undelivered);
			discovery_undelivered = job;
			pthread_mutex_unlock(&discovery_lock);
		}
	}
	return NULL;
}
static void discovery_send(discovery_page_t *p, int first, const char *id, bool refresh) {
	p->loading = true;
	p->requested_first = first;
	p->serial++;
	pthread_mutex_lock(&discovery_lock);
	memset(&discovery_pending, 0, sizeof(discovery_pending));
	discovery_pending.visit = discovery_visit;
	discovery_pending.request = discovery_live_request = ++discovery_request;
	discovery_pending.serial = p->serial;
	nd_context_t now;
	if (navidrome_context(&now, false))
		discovery_pending.network_epoch = now.search_network_epoch;
	nd_wipe(&now, sizeof(now));
	discovery_pending.page = (int)(p - discovery_pages);
	discovery_pending.first = first;
	discovery_pending.mode = discovery_mode;
	discovery_pending.refresh = refresh;
	discovery_pending.alternate = p != discovery_pages && navidromealbum_enabled();
	discovery_pending.layout_revision = navidromealbum_revision();
	discovery_pending.pitch = discovery_pitch;
	discovery_pending.disc_pitch = ui_px(ND_ALBUM_DISC_HEIGHT);
	discovery_pending.restore = *p->anchor != 0;
	discovery_pending.anchor_delta = p->anchor_delta;
	snprintf(discovery_pending.anchor, sizeof(discovery_pending.anchor), "%s", p->anchor);
	if (id) {
		discovery_child_epoch = nd_cache_epoch();
		discovery_pending.child = true;
		snprintf(discovery_pending.id, sizeof(discovery_pending.id), "%s", id);
	}
	discovery_has_pending = true;
	if (!discovery_started) {
		pthread_t worker;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 256 * 1024);
		if (!pthread_create(&worker, &attr, discovery_work, NULL)) {
			discovery_started = true;
			pthread_detach(worker);
		} else {
			p->loading = discovery_has_pending = false;
			lv_label_set_text(p->reason, tr("nd_worker_error"));
		}
		pthread_attr_destroy(&attr);
	}
	pthread_cond_signal(&discovery_ready);
	pthread_mutex_unlock(&discovery_lock);
	if (!p->populated)
		lv_label_set_text(p->status, tr("nd_working"));
	discovery_layout(p);
}
static void discovery_submit(discovery_page_t *p, int first, const char *id, nd_kind_t kind) {
	(void)kind;
	if (discovery_retry_valid)
		return; // Failed reads retry only through an explicit tap.
	discovery_send(p, first, id, false);
}
static bool discovery_admit(void) {
	navidrome_snapshot_t state;
	navidrome_poll_network();
	navidrome_snapshot(&state);
	if (!state.unlocked || state.state != ND_CONNECTED) {
		gui_notify_popup(!state.unlocked ? navidrome_feedback(&state) : "nd_reconnect_required");
		return false;
	}
	return true;
}
static void discovery_retry_clicked(lv_event_t *e) {
	(void)e;
	if (discovery_gesture() || !discovery_retry_valid || !discovery_admit())
		return;
	discovery_job_t *job = &discovery_retry;
	discovery_page_t *p = &discovery_pages[job->page];
	if (job->child)
		switch_screen(p->screen);
	discovery_retry_valid = false;
	discovery_send(p, job->first, job->child ? job->id : NULL, job->refresh);
}
static void discovery_refresh_clicked(lv_event_t *e) {
	(void)e;
	if (discovery_gesture() || !discovery_admit())
		return;
	discovery_retry_valid = false;
	discovery_send(&discovery_pages[0], 0, NULL, true);
}
static void discovery_row_clicked(lv_event_t *e) {
	int page = discovery_active_page();
	if (discovery_gesture() || page < 0)
		return;
	discovery_page_t *p = &discovery_pages[page];
	if (p->loading || !p->populated)
		return;
	for (int i = 0; i < p->data.count; i++)
		if (lv_event_get_target(e) == p->rows[i] && *p->data.rows[i].id) {
			if (page) {
				nd_library_t library;
				navidrome_library_snapshot(&library);
				if (!navidromeplay_discovery(&library, &discovery_identity, p->album, p->data.rows[i].id, p->screen))
					gui_notify_popup("nd_cache_storage");
			} else if (discovery_admit()) {
				discovery_page_t *child = &discovery_pages[1];
				char id[ND_ID_MAX];
				snprintf(id, sizeof(id), "%s", p->data.rows[i].id);
				snprintf(child->album, sizeof(child->album), "%s", id);
				lv_label_set_text(child->title, p->data.rows[i].title);
				child->populated = false;
				child->action_busy = false;
				child->anchor[0] = 0;
				discovery_child_epoch = nd_cache_epoch();
				navidromealbum_release(&child->album_view);
				discovery_rows_hide(child);
				lv_label_set_text(child->reason, "");
				discovery_retry_valid = false;
				switch_screen(child->screen);
				discovery_send(child, 0, id, false);
			}
			return;
		}
}
static void discovery_loaded(lv_event_t *e) {
	discovery_page_t *p = lv_event_get_user_data(e);
	lv_timer_resume(discovery_timer);
	navidromediscovery_refresh();
	if (lv_screen_active() == p->screen && p->populated)
		discovery_paint_rows(p);
	if (lv_screen_active() == p->screen && discovery_retry_valid && discovery_retry.page == (int)(p - discovery_pages)) {
		lv_label_set_text(p->status, "");
		lv_label_set_text(p->reason, tr("nd_discovery_retry"));
		discovery_layout(p);
	}
}
static void discovery_unloaded(lv_event_t *e) {
	discovery_page_t *p = lv_event_get_user_data(e);
	navidromeplay_discovery_leave(p->screen);
	navidromeofflinebadge_release(&p->offline);
	if (p->loading) {
		pthread_mutex_lock(&discovery_lock);
		discovery_retry = discovery_pending;
		pthread_mutex_unlock(&discovery_lock);
		discovery_retry_valid = true;
		lv_label_set_text(p->status, "");
		lv_label_set_text(p->reason, tr("nd_discovery_retry"));
	}
	p->serial++;
	p->loading = false;
	p->action_busy = false;
	discovery_cancel_request();
	// Independent slots retain parent pixels while its album is open.
}
void navidromediscovery_refresh(void) {
	int page = discovery_active_page();
	if (page < 0)
		return;
	if (!navidrome_search_current(&discovery_identity, false)) {
		discovery_retire();
		switch_screen_return_to(discovery_menu);
		return;
	}
	discovery_page_t *p = &discovery_pages[page];
	nd_library_t now;
	navidrome_library_snapshot(&now);
	if (p->populated && (p->data.library.card_epoch != now.card_epoch || (p->data.card_backed && !nd_cache_io_current(discovery_child_epoch)))) {
		discovery_cancel_request();
		p->loading = false;
		if (p->data.card_backed) {
			p->populated = false;
			discovery_rows_hide(p);
			navidromealbum_release(&p->album_view);
			gui_notify_popup("nd_cache_storage");
			switch_screen_return_to(discovery_pages[0].screen);
			return;
		}
		p->data.library = now;
		discovery_paint_rows(p);
	}
	if (page && p->populated && !p->loading && p->album_view.revision != navidromealbum_revision()) {
		int y = lv_obj_get_scroll_y(p->list);
		for (int i = 0; i < p->data.count; i++)
			if (*p->data.rows[i].id && lv_obj_get_y(p->rows[i]) + discovery_pitch > y) {
				memcpy(p->anchor, p->data.rows[i].id, sizeof(p->anchor));
				p->anchor_delta = lv_obj_get_y(p->rows[i]) - y;
				break;
			}
		p->album_view.revision = navidromealbum_revision();
		discovery_submit(p, 0, NULL, ND_TRACKS);
	}
	bool tinted = p->album_view.tinted;
	navidromealbum_tick(&p->album_view);
	if (tinted != p->album_view.tinted)
		discovery_paint_rows(p);
	navidromeofflinebadge_tick(&p->offline);
	navidrome_star_poll();
	navidromedownloads_poll();
	for (int i = 0; i < p->data.count; i++) {
		playmark_show(p->marks[i], navidromeplay_matches(&p->data.library, p->kind, p->data.rows[i].id));
		if (!p->art_done[i] && navidromeart_take(p->art_slots[i], &p->art[i], &p->art_done[i])) {
			lv_obj_remove_style(p->images[i], &theme_style_icon, 0);
			lv_image_set_src(p->images[i], &p->art[i].dsc);
			lv_obj_set_hidden(p->images[i], false);
		}
	}
}
static void discovery_tick(lv_timer_t *timer) {
	pthread_mutex_lock(&discovery_lock);
	discovery_job_t *done = discovery_undelivered;
	discovery_undelivered = NULL;
	bool failed = discovery_failed;
	unsigned token = discovery_failed_request;
	discovery_failed = false;
	pthread_mutex_unlock(&discovery_lock);
	if (done)
		discovery_done(done);
	if (failed && token == discovery_request) {
		int page = discovery_active_page();
		if (page >= 0) {
			discovery_page_t *p = &discovery_pages[page];
			p->loading = false;
			pthread_mutex_lock(&discovery_lock);
			discovery_retry = discovery_pending;
			pthread_mutex_unlock(&discovery_lock);
			discovery_retry_valid = true;
			lv_label_set_text(p->status, "");
			lv_label_set_text(p->reason, tr("nd_json_memory"));
			discovery_layout(p);
		}
	}
	if (discovery_active_page() < 0) {
		if (!switcher_in_history(discovery_pages[0].screen))
			discovery_retire();
		lv_timer_pause(timer);
	} else {
		navidrome_poll_network();
		navidromediscovery_refresh();
	}
}
static void discovery_mode_open(nd_discovery_mode_t mode) {
	if (!discovery_admit())
		return;
	discovery_retire();
	discovery_mode = mode;
	if (!navidrome_context(&discovery_identity, false))
		return;
	nd_wipe(discovery_identity.password, sizeof(discovery_identity.password));
	discovery_pages[0].kind = ND_ALBUMS;
	discovery_pages[1].kind = ND_TRACKS;
	lv_label_set_text(discovery_pages[0].title, tr(discovery_labels[mode]));
	lv_obj_t *controls[] = {discovery_refresh_button};
	nd_row_title(discovery_pages[0].title, discovery_pages[0].album_view.cfg, controls, 1);
	lv_obj_scroll_to_y(discovery_pages[0].list, 0, LV_ANIM_OFF);
	switch_screen(discovery_pages[0].screen);
	discovery_send(&discovery_pages[0], 0, NULL, true);
}
static void discovery_newest(void) { discovery_mode_open(ND_DISCOVERY_NEWEST); }
static void discovery_recent(void) { discovery_mode_open(ND_DISCOVERY_RECENT); }
static void discovery_frequent(void) { discovery_mode_open(ND_DISCOVERY_FREQUENT); }
static void discovery_random(void) { discovery_mode_open(ND_DISCOVERY_RANDOM); }
static void discovery_menu_loaded(lv_event_t *e) {
	(void)e;
	discovery_retire();
}
void navidromediscovery_open(void) { switch_screen(discovery_menu); }

static void discovery_scroll(lv_event_t *e) {
	discovery_page_t *p = lv_event_get_user_data(e);
	if (discovery_gesture() || !p->populated || p->data.error)
		return;

	navidromealbum_tick(&p->album_view);
	if (p->album_view.active) {
		int y = LV_MAX(0, nd_row_scroll(p->list, p->album_view.height + p->data.album.extent) - p->album_view.height);
		int last = p->data.count ? p->data.album.y[p->data.count - 1] + (p->data.album.heading[p->data.count - 1] ? ui_px(ND_ALBUM_DISC_HEIGHT) : discovery_pitch) : 0;
		if (!p->loading && (y < p->data.album.offset || (y + lv_obj_get_height(p->list) > last && last < p->data.album.extent)))
			discovery_submit(p, LV_MAX(0, y - 2 * discovery_pitch), NULL, p->kind);
		return;
	}
	int wanted = nd_row_first(p->list, p->data.total + (p->data.more ? 1 : 0), discovery_pitch, p->album_view.height);
	if (p->loading && wanted == p->requested_first)
		return;
	if (!nd_row_covered(p->list, p->data.first, p->data.count, p->data.total + (p->data.more ? 1 : 0), discovery_pitch, p->album_view.height)) {
		int y = nd_row_scroll(p->list, p->album_view.height + p->data.total * discovery_pitch);
		if (!p->data.more && p->data.first + p->data.count >= p->data.total && y / discovery_pitch >= p->data.first)
			return;
		discovery_submit(p, wanted, NULL, p->kind);
	}
}

void navidromediscovery_init(gui_config_t *cfg) {
	discovery_width = cfg->screen_width;
	discovery_height = cfg->screen_height;
	discovery_padding = cfg->padding;
	discovery_pitch = ui_px(108);
	discovery_menu = lv_obj_create(NULL);
	navidromepage_register(discovery_menu);
	settingsrow_title(discovery_menu, cfg, "nd_discovery");
	const grid_entry_t entries[] = {
		{"nd_discovery_newest", &icon_menu_year, NULL, discovery_newest},
		{"nd_discovery_recent", &icon_menu_album, NULL, discovery_recent},
		{"nd_discovery_frequent", &icon_menu_most_played, NULL, discovery_frequent},
		{"nd_discovery_random", &icon_menu_explorer, NULL, discovery_random},
	};
	discovery_grid = gridpage_build(discovery_menu, cfg, entries, 4, 2, 2, true);
	switcher_attach_back_gesture(discovery_menu);
	player_sheet_attach_drag(discovery_menu, true);
	lv_obj_add_event_cb(discovery_menu, discovery_menu_loaded, LV_EVENT_SCREEN_LOADED, NULL);
	for (int level = 0; level < 2; level++) {
		discovery_page_t *p = &discovery_pages[level];
		p->screen = lv_obj_create(NULL);
		navidromepage_register(p->screen);
		lv_obj_add_style(p->screen, &theme_style_screen, 0);
		p->title = settingsrow_title(p->screen, cfg, "nd_discovery");
		int top = settingsrow_content_top(cfg);
		p->status_top = top;
		p->status = lv_label_create(p->screen);
		p->reason = lv_label_create(p->screen);
		lv_obj_t *labels[] = {p->status, p->reason};
		for (int i = 0; i < 2; i++) {
			lv_obj_add_style(labels[i], &theme_style_text_dim, 0);
			lv_obj_set_style_text_font(labels[i], &font_ui_16, 0);
			lv_obj_set_width(labels[i], discovery_width - 2 * cfg->padding);
			lv_obj_set_height(labels[i], ui_px(i ? 42 : 24));
			lv_label_set_long_mode(labels[i], LV_LABEL_LONG_DOT);
			lv_obj_set_pos(labels[i], cfg->padding, top + ui_px(i ? 24 : 0));
		}
		top += ui_px(28);
		p->list = lv_obj_create(p->screen);
		lv_obj_remove_style_all(p->list);
		lv_obj_set_pos(p->list, cfg->padding, top);
		lv_obj_set_size(p->list, discovery_width - 2 * cfg->padding, discovery_height - top - cfg->padding);
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
			p->art_slots[i] = ND_ART_DISCOVERY_SLOT + level * ND_WINDOW + i;
			p->marks[i] = playmark_create(p->rows[i]);
			if (!level) {
				p->menus[i] = nd_row_menu_create(p->rows[i]);
				lv_obj_add_event_cb(p->menus[i], discovery_menu_clicked, LV_EVENT_CLICKED, p);
			}

			lv_obj_add_event_cb(p->rows[i], discovery_row_clicked, LV_EVENT_CLICKED, NULL);
			switcher_attach_back_gesture(p->rows[i]);
			player_sheet_attach_drag(p->rows[i], true);
		}

		navidromealbum_init(&p->album_view, cfg, p->screen, p->list, p->title, p->status, p->reason, ND_ART_DISCOVERY_ALBUM_SLOT, discovery_album_action, p);
		lv_obj_add_event_cb(p->list, discovery_scroll, LV_EVENT_SCROLL, p);
		lv_obj_add_event_cb(p->screen, discovery_loaded, LV_EVENT_SCREEN_LOADED, p);
		lv_obj_add_event_cb(p->screen, discovery_unloaded, LV_EVENT_SCREEN_UNLOADED, p);
		switcher_attach_back_gesture(p->screen);
		player_sheet_attach_drag(p->screen, true);
		switcher_attach_back_gesture(p->list);
		player_sheet_attach_drag(p->list, true);
	}

	discovery_refresh_button = lv_btn_create(discovery_pages[0].screen);
	lv_obj_set_size(discovery_refresh_button, ui_px(56), ui_px(56));
	lv_obj_align(discovery_refresh_button, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_set_style_bg_opa(discovery_refresh_button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(discovery_refresh_button, 0, 0);
	lv_obj_set_style_shadow_width(discovery_refresh_button, 0, 0);
	lv_obj_set_style_pad_all(discovery_refresh_button, 0, 0);
	lv_obj_set_event_bubble(discovery_refresh_button, true);
	lv_obj_t *icon = lv_image_create(discovery_refresh_button);
	lv_image_set_src(icon, &icon_refresh);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
	lv_obj_add_event_cb(discovery_refresh_button, discovery_refresh_clicked, LV_EVENT_CLICKED, NULL);
	switcher_attach_back_gesture(discovery_refresh_button);
	player_sheet_attach_drag(discovery_refresh_button, true);
	for (int i = 0; i < 2; i++) {
		lv_obj_set_clickable(discovery_pages[i].reason, true);
		lv_obj_add_event_cb(discovery_pages[i].reason, discovery_retry_clicked, LV_EVENT_CLICKED, NULL);
		switcher_attach_back_gesture(discovery_pages[i].reason);
	}
	discovery_timer = lv_timer_create(discovery_tick, 100, NULL);
	lv_timer_pause(discovery_timer);
}

static void discovery_download_done(bool ok, const char *error, int added, int skipped, void *user) {
	uintptr_t token = (uintptr_t)user;
	discovery_page_t *p = &discovery_pages[token & 1];
	if (p->serial != (unsigned)(token >> 1) || lv_screen_active() != p->screen)
		return;
	p->action_busy = false;
	if (ok) {
		char text[160];
		lv_snprintf(text, sizeof(text), tr("nd_dl_admitted"), added, skipped);
		toast_success(text);
	} else
		gui_notify_popup(error ? error : "nd_dl_capture_error");
	discovery_paint_rows(p);
}
static void discovery_album_action(void *user, nd_album_action_t action) {
	discovery_page_t *p = user;
	if (discovery_gesture() || discovery_active_page() != 1 || p->loading || p->action_busy || !p->populated || !p->album_view.active || !discovery_admit())
		return;
	nd_library_t library;
	navidrome_library_snapshot(&library);
	if (action == ND_ALBUM_DOWNLOAD) {
		nd_dl_request_t q = {.action = NDDL_CAPTURE, .library = library, .kind = ND_ALBUMS, .remote_album = true, .profile_epoch = discovery_identity.profile_epoch};
		snprintf(q.id, sizeof(q.id), "%s", p->album);
		const char *folder = config_get("navidrome", "download_folder", "Music/Navidrome");
		if (strlen(folder) >= sizeof(q.destination)) {
			gui_notify_popup("nd_dl_path_error");
			return;
		}
		snprintf(q.destination, sizeof(q.destination), "%s", folder);
		if (navidromedownloads_request(&q, discovery_download_done, (void *)(((uintptr_t)p->serial << 1) | (unsigned)(p - discovery_pages)))) {
			p->action_busy = true;
			discovery_paint_rows(p);
		} else
			gui_notify_popup("nd_working");
	} else if (!navidromeplay_discovery_album(&library, &discovery_identity, p->album, (int)action, p->screen))
		gui_notify_popup("nd_audio_unavailable");
}

// Own exact menu identity before row reuse. Every callback also checks the visit.
static struct {
	nd_row_t album;
	nd_library_t library;
	unsigned visit, serial;
} discovery_menu_target;
static bool discovery_menu_current(void) {
	discovery_page_t *p = &discovery_pages[0];
	return discovery_active_page() == 0 && !discovery_gesture() && !p->loading &&
		discovery_menu_target.visit == discovery_visit && discovery_menu_target.serial == p->serial &&
		navidrome_search_current(&discovery_identity, false);
}
static void discovery_favorite_done(nd_star_result_t *result, void *user) {
	discovery_page_t *p = &discovery_pages[0];
	if (p->serial == (unsigned)(uintptr_t)user && discovery_active_page() == 0 && navidrome_star_current(&result->context)) {
		if (result->error)
			gui_notify_popup(result->error);
		else
			toast_success("nd_favorites_saved");
	}
	free(result);
}
static void discovery_menu_favorite(void *user) {
	if (!discovery_menu_current()) return;
	nd_star_request_t q = {.kind = ND_ALBUMS, .action = (nd_star_action_t)(uintptr_t)user,
		.bound_profile = true, .profile_epoch = discovery_identity.profile_epoch, .library = discovery_menu_target.library};
	q.metadata.row = discovery_menu_target.album;
	snprintf(q.id, sizeof(q.id), "%s", discovery_menu_target.album.id);
	if (!navidrome_star_submit(&q, discovery_favorite_done, (void *)(uintptr_t)discovery_pages[0].serial))
		gui_notify_popup("nd_working");
}
static void discovery_menu_queue(void *user) {
	(void)user;
	if (discovery_menu_current() && !navidromeplay_discovery_enqueue(&discovery_menu_target.library, &discovery_identity, discovery_menu_target.album.id, discovery_pages[0].screen))
		gui_notify_popup("nd_working");
}
static void discovery_menu_playlist(void *user) {
	(void)user;
	if (discovery_menu_current() && !navidromeplaylists_pick_album(&discovery_menu_target.album, &discovery_menu_target.library, &discovery_identity, discovery_pages[0].screen))
		gui_notify_popup("nd_cache_storage");
}
static void discovery_menu_download(void *user) {
	(void)user;
	if (!discovery_menu_current()) return;
	discovery_page_t *p = &discovery_pages[0];
	nd_dl_request_t q = {.action = NDDL_CAPTURE, .library = discovery_menu_target.library, .kind = ND_ALBUMS,
		.remote_album = true, .profile_epoch = discovery_identity.profile_epoch};
	snprintf(q.id, sizeof(q.id), "%s", discovery_menu_target.album.id);
	const char *folder = config_get("navidrome", "download_folder", "Music/Navidrome");
	if (strlen(folder) >= sizeof(q.destination)) { gui_notify_popup("nd_dl_path_error"); return; }
	snprintf(q.destination, sizeof(q.destination), "%s", folder);
	if (!navidromedownloads_request(&q, discovery_download_done, (void *)((uintptr_t)p->serial << 1)))
		gui_notify_popup("nd_working");
}
static void discovery_menu_clicked(lv_event_t *event) {
	lv_event_stop_bubbling(event);
	discovery_page_t *p = lv_event_get_user_data(event);
	if (p != discovery_pages || discovery_gesture() || p->loading || !p->populated)
		return;
	for (int i = 0; i < p->data.count; i++) {
		if (lv_event_get_current_target(event) != p->menus[i] || !p->data.rows[i].id[0]) continue;
		discovery_menu_target.album = p->data.rows[i];
		discovery_menu_target.visit = discovery_visit;
		discovery_menu_target.serial = p->serial;
		navidrome_library_snapshot(&discovery_menu_target.library);
		nd_star_state_t state = navidrome_star_state(ND_ALBUMS, discovery_menu_target.album.id);
		bool starred = state == ND_STAR_SET || ((state == ND_STAR_UNKNOWN || state == ND_STAR_UNCERTAIN) && discovery_menu_target.album.favorite);
		nd_star_action_t action = state == ND_STAR_UNCERTAIN ? ND_STAR_TOGGLE : starred ? ND_STAR_REMOVE : ND_STAR_ADD;
		popover_item_t items[5];
		int n = 0;
		items[n++] = (popover_item_t){starred ? "remove_from_favourites" : "medialist_add_to_favourites", discovery_menu_favorite, (void *)(uintptr_t)action, false};
		if (navidrome_star_retry(ND_ALBUMS, discovery_menu_target.album.id))
			items[n++] = (popover_item_t){"nd_favorites_retry", discovery_menu_favorite, (void *)(uintptr_t)ND_STAR_RETRY, false};
		items[n++] = (popover_item_t){"add_to_queue", discovery_menu_queue, NULL, false};
		items[n++] = (popover_item_t){"add_to_playlist", discovery_menu_playlist, NULL, false};
		items[n++] = (popover_item_t){"nd_dl_download", discovery_menu_download, NULL, false};
		popover_show(p->menus[i], items, n);
		return;
	}
}

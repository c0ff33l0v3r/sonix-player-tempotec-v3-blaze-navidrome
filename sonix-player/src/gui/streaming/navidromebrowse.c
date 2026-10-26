#include "navidromebrowse.h"
#include "navidromerow.h"
#include "navidromeofflinebadge.h"
#include "src/system/streaming/navidromedownloads.h"
#include "src/system/core/config.h"
#include "navidromealbum.h"
#include "navidromeart.h"
#include "navidromefavorites.h"
#include "navidromepage.h"
#include "navidromeplaylists.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/azindex.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/playmark.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/uiscale.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/streaming/navidrome.h"
#include "src/system/streaming/navidromeplay.h"
#include "src/system/streaming/navidromeplaylists.h"
#include "src/system/streaming/navidromestars.h"
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	lv_obj_t *screen, *list, *extent, *title, *status, *selection_status;
	lv_obj_t *rows[ND_WINDOW], *names[ND_WINDOW], *details[ND_WINDOW], *images[ND_WINDOW], *checks[ND_WINDOW];
	lv_obj_t *playmarks[ND_WINDOW];
	nd_off_badges_t offline;
	cover_image_t artwork[ND_WINDOW];
	lv_obj_t *numbers[ND_WINDOW], *durations[ND_WINDOW];
	int art_slots[ND_WINDOW];
	nd_album_view_t album;
	nd_album_window_t album_window;
	char anchor[ND_ID_MAX];
	int anchor_delta;
	bool art_finished[ND_WINDOW], art_bound[ND_WINDOW], loading, map_ready, map_bound;
	azindex_t *index;
	int letters[ND_LETTER_BUCKETS], requested_first;
	nd_row_t data[ND_WINDOW];
	nd_query_t query;
	nd_library_t library;
	unsigned serial;
	uint32_t submitted;
	int first, count, total, selected;
	bool selecting, select_busy, queue_busy, picking;
	int64_t session;
	int64_t favorite_revision;
	lv_obj_t *select_download, *select_queue, *select_add, *select_close, *select_favorite, *menus[ND_WINDOW];
	bool chosen[ND_WINDOW];
} browse_page_t;
static browse_page_t pages[3];
static int pitch;
static lv_timer_t *browse_timer;
static const char *const captions[] = {"albums", "navidrome_tracks", "artists", "music_genres"};
typedef struct {
	int page, first, count, total;
	int return_page, artist_parent;
	char artist[ND_ID_MAX]; // Owned ancestor identity; never read GUI pages on the worker.
	unsigned serial;
	nd_library_t library;
	nd_query_t query;
	nd_row_t rows[ND_WINDOW];
	bool ok, filter_exists, need_map, alternate, restore;
	nd_album_window_t album;
	int offset, pitch, disc_pitch, anchor_delta;
	char anchor[ND_ID_MAX];
	unsigned layout_revision;
	int metadata_details;
	int letters[ND_LETTER_BUCKETS];
	int64_t session;
	int64_t favorite_revision;
	int selected;
	bool chosen[ND_WINDOW];
} window_t;
static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t window_ready = PTHREAD_COND_INITIALIZER;
static bool worker_started, pending;
static window_t request;
static void submit_window(browse_page_t *page);
static void browse_selection_ui(browse_page_t *p);
static void browse_album_action(void *user, nd_album_action_t action);

static void browse_snapshot(bool favorites, nd_library_t *out) {
	if (favorites)
		navidrome_favorites_snapshot(out);
	else
		navidrome_library_snapshot(out);
}

static bool gesture(void) { return switcher_back_drag_active() || player_sheet_drag_active() || coverflow_drag_active(); }

static void release_art_row(browse_page_t *p, int i) {
	navidromeart_release(p->art_slots[i]);
	lv_obj_set_hidden(p->images[i], true);
	lv_image_set_src(p->images[i], &icon_music_note);
	cover_free(&p->artwork[i]);
	p->art_finished[i] = true;
	p->art_bound[i] = false;
}
static void release_art(browse_page_t *p) {
	for (int i = 0; i < ND_WINDOW; i++)
		release_art_row(p, i);
}

static void window_done(void *value) {
	window_t *result = value;
	browse_page_t *p = &pages[result->page];
	nd_library_t current;
	browse_snapshot(result->query.favorites, &current);
	if (result->layout_revision != navidromealbum_revision() || p->serial != result->serial || lv_screen_active() != p->screen || !current.ready || current.epoch != result->library.epoch || current.card_epoch != result->library.card_epoch || current.generation != result->library.generation) {
		free(result);
		return;
	}
	p->loading = false;
	p->selected = result->selected;
	p->selecting = p->selected > 0;
	memcpy(p->chosen, result->chosen, sizeof(p->chosen));
	browse_selection_ui(p);
	if (!result->ok) {
		lv_label_set_text(p->status, tr("nd_cache_error"));
		free(result);
		return;
	}
	if (!result->filter_exists) {
		gui_notify_popup("nd_item_removed");
		switch_screen_return_to(result->return_page >= 0 ? pages[result->return_page].screen : result->query.favorites ? navidromefavorites_page() : navidrome_screen);
		free(result);
		return;
	}
	if (!result->alternate && result->first > 0 && result->first >= result->total) {
		lv_obj_scroll_to_y(p->list, result->total ? (result->total - 1) * pitch : 0, LV_ANIM_OFF);
		free(result);
		submit_window(p);
		return;
	}
	bool keep_art[ND_WINDOW] = {0};
	int map[ND_WINDOW];
	bool claimed[ND_WINDOW] = {0};
	bool same_library = p->library.epoch == result->library.epoch && p->library.card_epoch == result->library.card_epoch && (p->library.generation == result->library.generation || (p->query.favorites && p->library.base == result->library.base)) && !strcmp(p->library.scope, result->library.scope);
	for (int i = 0; i < ND_WINDOW; i++) {
		map[i] = -1;
		if (same_library && i < result->count)
			for (int j = 0; j < p->count; j++)
				if (!claimed[j] && p->art_bound[j] && !strcmp(p->data[j].id, result->rows[i].id) && !strcmp(p->data[j].cover, result->rows[i].cover)) {
					map[i] = j;
					claimed[j] = true;
					keep_art[i] = true;
					break;
				}
	}
	nd_row_complete_map(map);
	navidromeofflinebadge_release(&p->offline);
	ND_ROW_REORDER(p->rows, map);
	ND_ROW_REORDER(p->names, map);
	ND_ROW_REORDER(p->details, map);
	ND_ROW_REORDER(p->images, map);
	ND_ROW_REORDER(p->checks, map);
	ND_ROW_REORDER(p->playmarks, map);
	ND_ROW_REORDER(p->numbers, map);
	ND_ROW_REORDER(p->durations, map);
	ND_ROW_REORDER(p->menus, map);
	ND_ROW_REORDER(p->artwork, map);
	ND_ROW_REORDER(p->art_finished, map);
	ND_ROW_REORDER(p->art_bound, map);
	ND_ROW_REORDER(p->art_slots, map);
	ND_ROW_REORDER(p->offline.icons, map);
	for (int i = 0; i < ND_WINDOW; i++) {
		if (keep_art[i] && p->artwork[i].pixels)
			lv_image_set_src(p->images[i], &p->artwork[i].dsc);
		else if (!keep_art[i] && p->art_bound[i])
			release_art_row(p, i);
	}
	p->library = result->library;
	p->favorite_revision = result->favorite_revision;
	p->first = result->first;
	p->count = result->count;
	p->total = result->total;
	p->album_window = result->album;
	if (result->need_map) {
		memcpy(p->letters, result->letters, sizeof(p->letters));
		p->map_ready = true;
	}
	if (!p->map_bound) {
		bool alphabetical = !(p->query.kind == ND_TRACKS && *p->query.filter);
		azindex_set_rows(p->index, alphabetical && p->map_ready ? p->letters : NULL, p->total, false);
		p->map_bound = true;
	}
	memcpy(p->data, result->rows, sizeof(p->data));
	if (p->total > INT_MAX / pitch) {
		lv_label_set_text(p->status, tr("nd_cache_error"));
		free(result);
		return;
	}

	char status[80];
	lv_snprintf(status, sizeof(status), tr("nd_item_count"), p->total);
	lv_label_set_text(p->status, result->metadata_details ? tr(result->metadata_details == 1 && navidrome_star_loading(ND_ALBUMS, p->query.filter) ? "nd_favorites_loading" : "nd_favorites_metadata_error") : p->total ? status : tr(p->query.favorites ? "nd_favorites_empty" : "nd_library_empty"));
    int old_height = p->album.height;
    navidromealbum_bind(&p->album, result->alternate, &result->album.info, &p->library,
                       !p->picking && !p->selecting, p->total > 0 && !result->metadata_details);
    gui_config_t *cfg = p->album.cfg;
    int top = result->alternate ? (int)cfg->top_bar_height : settingsrow_content_top(cfg) + ui_px(32);
    lv_obj_set_y(p->list, top);
    lv_obj_set_height(p->list, cfg->screen_height - top - cfg->padding);
    if (!result->alternate) {
        lv_obj_set_pos(p->status, cfg->padding, settingsrow_content_top(cfg));
        lv_label_set_text(p->title, p->query.kind == ND_TRACKS && *p->query.filter && *result->album.info.row.title ? result->album.info.row.title : lv_label_get_text(p->title));
    }
    int extent = result->alternate ? result->album.extent : p->total * pitch;
    if (extent > INT_MAX - p->album.height) {
        free(result);
        lv_label_set_text(p->status, tr("nd_cache_error"));
        return;
    }
    lv_obj_set_height(p->extent, LV_MAX(1, extent + p->album.height));
    if (result->restore) {
        lv_obj_scroll_to_y(p->list, LV_MAX(0, result->offset + p->album.height - result->anchor_delta), LV_ANIM_OFF);
        p->anchor[0] = 0;
    } else if (old_height && p->album.height && old_height != p->album.height && lv_obj_get_scroll_y(p->list) >= old_height) {
        lv_obj_scroll_to_y(p->list, lv_obj_get_scroll_y(p->list) + p->album.height - old_height, LV_ANIM_OFF);
    }

	navidromeofflinebadge_release(&p->offline);
	for (int i = 0; i < ND_WINDOW; i++) {
		lv_obj_set_hidden(p->rows[i], i >= p->count);
		lv_obj_set_hidden(p->menus[i], (result->alternate && result->album.heading[i]) || p->selecting || (p->query.kind != ND_TRACKS && p->query.kind != ND_ALBUMS && p->query.kind != ND_ARTISTS));
		lv_obj_set_hidden(p->checks[i], !p->chosen[i]);
		lv_obj_set_style_bg_opa(p->rows[i], p->chosen[i] ? LV_OPA_50 : LV_OPA_COVER, 0);
		if (i >= p->count)
			continue;
		nd_row_t *row = &p->data[i];
		if (!result->alternate && !keep_art[i]) {
			lv_obj_set_size(p->images[i], ui_px(ND_ART_THUMB_SIZE), ui_px(ND_ART_THUMB_SIZE));
			lv_image_set_inner_align(p->images[i], LV_IMAGE_ALIGN_CONTAIN);
			lv_image_set_src(p->images[i], &icon_music_note);
			lv_obj_add_style(p->images[i], &theme_style_icon, 0);
			lv_obj_align(p->images[i], LV_ALIGN_LEFT_MID, 0, 0);
			lv_obj_set_hidden(p->images[i], false);
			p->art_bound[i] = true;
			p->art_finished[i] = !row->cover[0];
			if (row->cover[0])
				navidromeart_request(p->art_slots[i], &p->library, row->cover, ui_px(ND_ART_THUMB_SIZE));
		}
		lv_obj_set_y(p->rows[i], p->album.height + (result->alternate ? result->album.y[i] : (p->first + i) * pitch));
		lv_label_set_text(p->names[i], row->title);
		lv_label_set_text(p->details[i], row->details ? tr(row->details == 1 && navidrome_star_loading(p->query.kind, row->id) ? "nd_favorites_loading" : "nd_favorites_metadata_error") : row->artist);
		navidromealbum_row(&p->album, p->rows[i], p->names[i], p->details[i], p->numbers[i], p->images[i], row, result->alternate && result->album.heading[i]);

		navidromealbum_icon(&p->album, lv_obj_get_child(p->menus[i], 0));
		navidromealbum_icon(&p->album, p->checks[i]);
		lv_obj_set_style_bg_opa(p->rows[i], result->alternate && result->album.heading[i] ? LV_OPA_TRANSP : p->chosen[i] ? LV_OPA_50 : LV_OPA_COVER, 0);
	}
	lv_obj_update_layout(p->screen);
	for (int i = 0; i < p->count; i++)
		nd_row_footer(p->details[i], p->durations[i], lv_obj_get_style_width(p->names[i], 0), p->query.kind == ND_TRACKS ? p->data[i].duration : 0, result->alternate && result->album.heading[i]);
	if (p->query.kind == ND_TRACKS || p->query.kind == ND_ALBUMS) {
		nd_off_request_t q = {.library = p->library, .kind = p->query.kind == ND_TRACKS ? ND_OFF_TRACK : ND_OFF_ALBUM, .count = p->count};
		for (int i = 0; i < p->count; i++)
			if (!result->alternate || !result->album.heading[i]) memcpy(q.items[i].id, p->data[i].id, ND_ID_MAX);
		navidromeofflinebadge_bind(&p->offline, p->screen, &q, p->details);
	}
	browse_selection_ui(p);
	free(result);
}

static void *read_windows(void *unused) {
	(void)unused;
	thread_be_background("navidrome lists");
	pthread_mutex_lock(&window_lock);
	for (;;) {
		while (!pending)
			pthread_cond_wait(&window_ready, &window_lock);
		window_t *result = malloc(sizeof(*result));
		if (!result) {
			pending = false;
			continue;
		}
		*result = request;
		pending = false;
		pthread_mutex_unlock(&window_lock);
		nd_store_t store = {0};
		bool leased = navidrome_cache_begin(result->library.epoch, result->library.card_epoch);
		result->ok = leased && nd_store_open_source(&store, result->library.path, result->library.scope, false, false, result->query.favorites);
		result->filter_exists = true;
		if (result->ok && *result->query.filter)
			result->filter_exists = nd_store_contains(&store, result->library.generation, result->query.filter_kind, result->query.filter);
		if (result->ok && result->artist[0] && !nd_store_artist_browsable(&store, result->library.generation, result->artist)) {
			result->filter_exists = false;
			result->return_page = result->artist_parent;
		}

        if (result->ok && result->restore) {
            int y;
            if (nd_store_album_position(&store, result->library.generation, &result->query, result->anchor,
                                        result->pitch, result->disc_pitch, result->alternate, &y)) {
                result->offset = y;
                result->first = y / result->pitch;
            } else result->restore = false;
        }
        if (result->ok) {
            if (result->alternate)
                result->ok = nd_store_album_window(&store, result->library.generation, &result->query,
                    LV_MAX(0, result->offset - (result->restore ? 0 : 2 * result->pitch)), result->pitch,
                    result->disc_pitch, result->rows, &result->count, &result->total, &result->album);
            else result->ok = nd_store_query(&store, result->library.generation, &result->query, result->first, result->rows, &result->count, &result->total);
        }
        if (!result->filter_exists) result->ok = true;

		if (result->ok && result->query.favorites && result->query.kind == ND_TRACKS && result->query.filter[0]) {
			nd_playrow_t album;
			if (nd_store_metadata(&store, result->library.generation, ND_ALBUMS, result->query.filter, &album))
				result->metadata_details = album.row.details;
		}
		if (result->ok && !result->alternate && result->query.kind == ND_TRACKS && *result->query.filter)
			nd_store_metadata(&store, result->library.generation, ND_ALBUMS, result->query.filter, &result->album.info);
		if (result->ok && result->need_map)
			result->ok = nd_store_letters(&store, result->library.generation, &result->query, result->letters);
		if (result->ok && result->session) {
			char path[1200];
			nd_playlist_store_t selections = {0};
			if (nd_pl_path(&result->library, path) && nd_pl_open(&selections, path, result->library.scope, false)) {
				sqlite3_stmt *st = NULL;
				if (nd_pl_prepare(&selections, &st, "SELECT kind,track FROM selection WHERE session=?")) {
					sqlite3_bind_int64(st, 1, result->session);
					while (sqlite3_step(st) == SQLITE_ROW) {
						const char *id = (const char *)sqlite3_column_text(st, 1);
						result->selected++;
						for (int i = 0; i < result->count; i++)
							if (sqlite3_column_int(st, 0) == (int)result->query.kind && id && !strcmp(id, result->rows[i].id))
								result->chosen[i] = true;
					}
				}
				sqlite3_finalize(st);
			}
			nd_pl_close(&selections);
		}
		nd_store_close(&store);
		if (leased && result->ok && !result->query.favorites) {
			nd_library_t favorites;
			navidrome_favorites_snapshot(&favorites);
			result->favorite_revision = favorites.generation;
			if (favorites.ready && favorites.epoch == result->library.epoch && favorites.card_epoch == result->library.card_epoch && !strcmp(favorites.scope, result->library.scope)) {
				if (nd_store_open_source(&store, favorites.path, favorites.scope, false, false, true))
					for (int i = 0; i < result->count; i++)
						result->rows[i].favorite_known = nd_store_annotation(&store, favorites.generation, result->query.kind, result->rows[i].id, &result->rows[i].favorite);
				nd_store_close(&store);
			}
		}
		if (leased)
			navidrome_cache_end();
		if (!gui_post(window_done, result))
			free(result);
		pthread_mutex_lock(&window_lock);
	}
	return NULL;
}

static void submit_window(browse_page_t *p) {
	nd_library_t library;
	browse_snapshot(p->query.favorites, &library);
	if (!library.ready)
		return;
	int first = nd_row_first(p->list, p->total, pitch, 0);
	p->loading = true;
	p->submitted = lv_tick_get();
	p->serial++;
	pthread_mutex_lock(&window_lock);
	memset(&request, 0, sizeof(request));
	request.page = (int)(p - pages);
	request.return_page = request.page - 1;
	// Capture only the current hierarchy, so a direct album entry cannot
	// inherit an artist from a previously visited child page.
	for (int level = 0; !p->query.favorites && level <= request.page; level++) {
		const nd_query_t *query = &pages[level].query;
		if (query->kind == ND_ALBUMS && query->filter_kind == ND_ARTISTS && query->filter[0]) {
			snprintf(request.artist, sizeof(request.artist), "%s", query->filter);
			request.artist_parent = level - 1;
		}
	}
	request.serial = p->serial;
	request.alternate = navidromealbum_enabled() && p->query.kind == ND_TRACKS && p->query.filter_kind == ND_ALBUMS && *p->query.filter;
	request.layout_revision = navidromealbum_revision();
	request.pitch = pitch; request.disc_pitch = ui_px(44);
	request.offset = LV_MAX(0, nd_row_scroll(p->list, p->album.height + p->album_window.extent) - p->album.height);
	request.restore = *p->anchor != 0;
	request.anchor_delta = p->anchor_delta;
	snprintf(request.anchor, sizeof(request.anchor), "%s", p->anchor);
	request.first = first;
	p->requested_first = first;
	request.need_map = !p->map_ready || p->library.epoch != library.epoch || p->library.generation != library.generation;
	if (request.need_map) {
		p->map_bound = false;
		azindex_set_rows(p->index, NULL, 0, false);
	}
	request.query = p->query;
	request.session = p->selecting ? p->session : 0;
	request.library = library;
	pending = true;
	if (!worker_started) {
		pthread_t worker;
		if (pthread_create(&worker, NULL, read_windows, NULL) == 0) {
			worker_started = true;
			pthread_detach(worker);
		} else {
			pending = false;
			p->loading = false;
			lv_label_set_text(p->status, tr("nd_worker_error"));
		}
	}
	pthread_cond_signal(&window_ready);
	pthread_mutex_unlock(&window_lock);
}

static void scrolling(lv_event_t *e) {
	browse_page_t *p = lv_event_get_user_data(e);
	if (lv_screen_active() != p->screen || p->select_busy || gesture())
		return;

    navidromealbum_tick(&p->album);
    if (p->album.active) {
        int y = LV_MAX(0, nd_row_scroll(p->list, p->album.height + p->album_window.extent) - p->album.height);
        int last = p->count ? p->album_window.y[p->count - 1] + (p->album_window.heading[p->count - 1] ? ui_px(44) : pitch) : 0;
        if (!p->loading && (y < p->album_window.offset || (y + lv_obj_get_height(p->list) > last && last < p->album_window.extent)))
            submit_window(p);
        return;
    }
	int wanted = nd_row_first(p->list, p->total, pitch, 0);
	azindex_flash(p->index);
	if ((p->loading && wanted != p->requested_first) ||
		(!p->loading && !nd_row_covered(p->list, p->first, p->count, p->total, pitch, 0)))
		submit_window(p);
}

static void browse_row_select(browse_page_t *p, lv_obj_t *row, bool picker);

static void row_clicked(lv_event_t *e) {
	if (gesture())
		return;
	lv_obj_t *row = lv_event_get_target(e);
	for (int level = 0; level < 3; level++) {
		browse_page_t *p = &pages[level];
		if (lv_screen_active() != p->screen)
			continue;
		if (p->select_busy)
			return;
		if (p->selecting) {
			browse_row_select(p, row, false);
			return;
		}
		nd_library_t current;
		browse_snapshot(p->query.favorites, &current);
		if (!current.ready || current.epoch != p->library.epoch || current.generation != p->library.generation)
			return;
		for (int i = 0; i < p->count; i++) {
			if (p->rows[i] != row || !*p->data[i].id)
				continue;
			if (p->query.kind == ND_TRACKS) {
				if (p->picking) {
					browse_row_select(p, row, false);
					return;
				}
				if (!navidromeplay_select(&p->library, &p->query, p->data[i].id, p->screen))
					gui_notify_popup("nd_audio_unavailable");
				return;
			}
			if (level == 2)
				return;
			browse_page_t *next = &pages[level + 1];
			next->picking = p->picking;
			next->selecting = next->select_busy = false;
			next->selected = 0;
			next->session++;
			next->map_ready = next->map_bound = false;
			memset(&next->query, 0, sizeof(next->query));
			next->query.favorites = p->query.favorites;
			next->query.kind = p->query.kind == ND_ALBUMS ? ND_TRACKS : ND_ALBUMS;
			next->query.filter_kind = p->query.kind;
			lv_snprintf(next->query.filter, sizeof(next->query.filter), "%s", p->data[i].id);
			lv_label_set_text(next->title, p->data[i].title);
			next->count = next->total = 0;
			lv_obj_scroll_to_y(next->list, 0, LV_ANIM_OFF);
			switch_screen(next->screen);
			return;
		}
	}
}

static void loaded(lv_event_t *e) {
	browse_page_t *p = lv_event_get_user_data(e);
	p->loading = false;
	lv_timer_resume(browse_timer);
	lv_label_set_text(p->status, tr("nd_working"));
	for (int i = 0; i < ND_WINDOW; i++)
		lv_obj_set_hidden(p->rows[i], true);
	submit_window(p);
}

static void unloaded(lv_event_t *e) {
	browse_page_t *p = lv_event_get_user_data(e);
	navidromeofflinebadge_release(&p->offline);
	p->serial++;
	p->loading = false;
	p->map_bound = false;
	azindex_set_rows(p->index, NULL, 0, false);
	p->select_busy = false;
	popover_close();
	release_art(p);
	navidromealbum_release(&p->album);
}

static void browse_marks(browse_page_t *p) {
	for (int i = 0; i < p->count; i++) {
		bool marked = navidromeplay_matches(&p->library, p->query.kind, p->data[i].id);
		playmark_show(p->playmarks[i], marked);
	}
}
void navidromebrowse_refresh(void) {
	navidrome_star_poll();
	nd_library_t library;
	for (int level = 0; level < 3; level++) {
		browse_page_t *p = &pages[level];
		if (!p->screen || lv_screen_active() != p->screen)
			continue;

        if (p->album.revision != navidromealbum_revision()) {
            int scroll = lv_obj_get_scroll_y(p->list);
            for (int i = 0; i < p->count; i++) {
                if (*p->data[i].id && lv_obj_get_y(p->rows[i]) + pitch > scroll) {
                    memcpy(p->anchor, p->data[i].id, sizeof(p->anchor));
                    p->anchor_delta = lv_obj_get_y(p->rows[i]) - scroll;
                    break;
                }
            }
            p->album.revision = navidromealbum_revision();
            release_art(p);
            submit_window(p);
        }
		if (p->queue_busy && !navidromeplay_enqueuing()) {
			p->queue_busy = p->select_busy = false;
			browse_selection_ui(p);
		}
		browse_marks(p);
		browse_snapshot(p->query.favorites, &library);
		if (p->select_favorite)
			lv_obj_set_state(p->select_favorite, LV_STATE_DISABLED, p->select_busy || navidrome_star_busy());

		if (!library.ready) {
			p->selecting = false;
			p->selected = 0;
			browse_selection_ui(p);
			release_art(p);
			switch_screen_return_to(navidrome_screen);
			return;
		}
		if (p->library.epoch != library.epoch || p->library.generation != library.generation) {
			p->map_ready = p->map_bound = false;
			azindex_set_rows(p->index, NULL, 0, false);
		}
		if (!p->loading && (p->library.epoch != library.epoch || p->library.generation != library.generation)) {
			if (p->select_busy)
				continue;
			bool keep_selection = p->query.favorites && p->library.epoch == library.epoch && p->library.card_epoch == library.card_epoch;
			if (!keep_selection) {
				p->select_busy = false;
				p->selecting = false;
				p->selected = 0;
				p->session++;
			}
			browse_selection_ui(p);
			if (!keep_selection)
				release_art(p);
			for (int i = 0; i < ND_WINDOW; i++)
				lv_obj_set_hidden(p->rows[i], true);
			submit_window(p);
		}
		if (!p->query.favorites && !p->loading && !p->select_busy) {
			nd_library_t favorites;
			navidrome_favorites_snapshot(&favorites);
			if (p->favorite_revision != favorites.generation)
				submit_window(p);
		}
		if (p->loading && lv_tick_elaps(p->submitted) > 2500)
			submit_window(p); // Recover from a dropped GUI post without a network retry.
		bool was_tinted = p->album.tinted;
		navidromealbum_tick(&p->album);
		if (was_tinted != p->album.tinted) {
            for (int i = 0; i < p->count; i++) {
                navidromealbum_row(&p->album, p->rows[i], p->names[i], p->details[i], p->numbers[i], p->images[i], &p->data[i], p->album_window.heading[i]);
				nd_row_footer(p->details[i], p->durations[i], lv_obj_get_style_width(p->names[i], 0), p->query.kind == ND_TRACKS ? p->data[i].duration : 0, p->album.active && p->album_window.heading[i]);
                navidromealbum_icon(&p->album,lv_obj_get_child(p->menus[i],0));
                navidromealbum_icon(&p->album,p->checks[i]);
                lv_obj_set_style_bg_opa(p->rows[i], p->album_window.heading[i] ? LV_OPA_TRANSP : p->chosen[i] ? LV_OPA_50 : LV_OPA_COVER, 0);
            }
            browse_selection_ui(p);
		}
		navidromeofflinebadge_tick(&p->offline);
		for (int i = 0; i < p->count; i++) {
			if (p->album.active || p->art_finished[i])
				continue;
			if (navidromeart_take(p->art_slots[i], &p->artwork[i], &p->art_finished[i])) {
				lv_obj_remove_style(p->images[i], &theme_style_icon, 0);
				lv_obj_set_size(p->images[i], ui_px(ND_ART_THUMB_SIZE), ui_px(ND_ART_THUMB_SIZE));
				// Keep automatic fitting across the glyph-to-cover transition.
				lv_image_set_inner_align(p->images[i], LV_IMAGE_ALIGN_CONTAIN);
				lv_obj_align(p->images[i], LV_ALIGN_LEFT_MID, 0, 0);
				lv_image_set_src(p->images[i], &p->artwork[i].dsc);
				lv_obj_set_hidden(p->images[i], false);
			}
		}
	}
}

static void tick(lv_timer_t *timer) {
	bool active = false;
	for (int i = 0; i < 3; i++)
		active |= lv_screen_active() == pages[i].screen;
	if (!active)
		lv_timer_pause(timer);
	else
		navidromebrowse_refresh();
}

static void browse_open(nd_kind_t kind, bool picking, bool favorites) {
	browse_page_t *p = &pages[0];
	p->picking = picking;
	p->map_ready = p->map_bound = false;
	azindex_set_rows(p->index, NULL, 0, false);
	memset(&p->query, 0, sizeof(p->query));
	p->query.kind = kind;
	p->query.favorites = favorites;
	p->selecting = false;
	p->selected = 0;
	p->session++;
	browse_selection_ui(p);
	lv_label_set_text(p->title, tr(captions[kind]));
	p->count = p->total = 0;
	lv_obj_set_height(p->extent, 1);
	lv_obj_scroll_to_y(p->list, 0, LV_ANIM_OFF);
	if (lv_screen_active() == p->screen) {
		release_art(p);
		for (int i = 0; i < ND_WINDOW; i++)
			lv_obj_set_hidden(p->rows[i], true);
		submit_window(p);
	} else
		switch_screen(p->screen);
}

void navidromebrowse_open(nd_kind_t kind) { browse_open(kind, false, false); }
void navidromebrowse_open_favorites(nd_kind_t kind) { browse_open(kind, false, true); }
void navidromebrowse_pick(nd_kind_t kind) { browse_open(kind, true, false); }

bool navidromebrowse_album(const char *id, const char *title) {
	nd_library_t library;
	navidrome_library_snapshot(&library);
	if (!library.ready || !id || !*id || strlen(id) >= ND_ID_MAX)
		return false;
	browse_page_t *p = &pages[0];
	p->picking = false;
	memset(&p->query, 0, sizeof(p->query));
	p->map_ready = p->map_bound = false;
	azindex_set_rows(p->index, NULL, 0, false);
	p->selecting = false;
	p->selected = 0;
	p->session++;
	browse_selection_ui(p);
	p->query.kind = ND_TRACKS;
	p->query.filter_kind = ND_ALBUMS;
	lv_snprintf(p->query.filter, sizeof(p->query.filter), "%s", id);
	lv_label_set_text(p->title, title && *title ? title : tr("albums"));
	p->count = p->total = 0;
	lv_obj_set_height(p->extent, 1);
	lv_obj_scroll_to_y(p->list, 0, LV_ANIM_OFF);
	if (lv_screen_active() == p->screen) {
		p->serial++;
		p->loading = false;
		release_art(p);
		navidromealbum_release(&p->album);
		for (int i = 0; i < ND_WINDOW; i++) lv_obj_set_hidden(p->rows[i], true);
		submit_window(p);
	} else switch_screen(p->screen);
	return true;
}

bool navidromebrowse_artist(const char *id, const char *title) {
	nd_library_t library;
	navidrome_library_snapshot(&library);
	if (!library.ready || !id || !*id || strlen(id) >= ND_ID_MAX)
		return false;
	browse_page_t *p = &pages[0];
	p->picking = false;
	memset(&p->query, 0, sizeof(p->query));
	p->map_ready = p->map_bound = false;
	azindex_set_rows(p->index, NULL, 0, false);
	p->selecting = false;
	p->selected = 0;
	p->session++;
	browse_selection_ui(p);
	p->query.kind = ND_ALBUMS;
	p->query.filter_kind = ND_ARTISTS;
	lv_snprintf(p->query.filter, sizeof(p->query.filter), "%s", id);
	lv_label_set_text(p->title, title && *title ? title : tr("artists"));
	p->count = p->total = 0;
	lv_obj_set_height(p->extent, 1);
	lv_obj_scroll_to_y(p->list, 0, LV_ANIM_OFF);
	if (lv_screen_active() == p->screen) {
		p->serial++;
		p->loading = false;
		release_art(p);
		navidromealbum_release(&p->album);
		for (int i = 0; i < ND_WINDOW; i++) lv_obj_set_hidden(p->rows[i], true);
		submit_window(p);
	} else switch_screen(p->screen);
	return true;
}

static void letter_jump(void) {
	for (int i = 0; i < 3; i++)
		if (lv_screen_active() == pages[i].screen && !pages[i].select_busy && !gesture())
			submit_window(&pages[i]);
}

static void browse_selection_ui(browse_page_t *p) {
	if (!p->select_add)
		return;

	if (p->album.active) {
		lv_obj_set_hidden(p->album.buttons, p->selecting || p->picking);
        lv_obj_set_state(p->album.controls[ND_ALBUM_DOWNLOAD], LV_STATE_DISABLED, p->select_busy || p->loading || p->total <= 0);
		lv_obj_move_foreground(p->select_add);
		lv_obj_move_foreground(p->select_close);
		lv_obj_move_foreground(p->select_favorite);
		lv_obj_move_foreground(p->select_queue);
	}
	lv_obj_set_hidden(p->selection_status, !p->album.active || !p->selecting);
	if (p->album.active) {
		lv_obj_set_hidden(p->status, p->selecting || !*lv_label_get_text(p->status));
		lv_obj_set_style_bg_color(p->selection_status, p->album.tinted ? p->album.bg : theme()->screen_bg, 0);
		lv_obj_set_style_text_color(p->selection_status, p->album.tinted ? p->album.ink : theme()->text_primary, 0);
		lv_obj_move_foreground(p->selection_status);
	}
	lv_obj_t *buttons[] = {p->select_add, p->select_close, p->select_favorite, p->select_queue, p->select_download};
	for (unsigned i = 0; i < sizeof(buttons) / sizeof(*buttons); i++) {
		if (buttons[i])
			navidromealbum_icon(&p->album, lv_obj_get_child(buttons[i], 0));
	}
	lv_obj_set_hidden(p->select_queue, !p->selecting || p->picking);
	lv_obj_set_state(p->select_queue, LV_STATE_DISABLED, p->select_busy);
	lv_obj_set_hidden(p->select_download, !p->selecting || p->picking);
	if (!p->album.active)
		lv_obj_set_hidden(p->title, p->selecting);
	lv_obj_set_state(p->select_download, LV_STATE_DISABLED, p->select_busy || p->selected <= 0);
	if (p->selecting)
		lv_obj_move_foreground(p->select_download);
	lv_obj_set_hidden(p->select_add, !p->selecting);
	lv_obj_set_hidden(p->select_close, !p->selecting);
	lv_obj_set_hidden(p->select_favorite, !p->selecting || (p->query.kind != ND_TRACKS && p->query.kind != ND_ALBUMS));
	lv_obj_set_state(p->select_favorite, LV_STATE_DISABLED, p->select_busy || navidrome_star_busy());
	nd_row_title(p->title, p->album.cfg, buttons, (int)(sizeof(buttons) / sizeof(*buttons)));
	if (p->selecting) {
		char text[80];
		lv_snprintf(text, sizeof(text), tr("nd_pl_selected"), p->selected);
		lv_label_set_text(p->status, text);
		lv_label_set_text(p->selection_status, text);
	}
}
static bool browse_selection_peek(void) {
	for (int i = 0; i < 3; i++)
		if (lv_screen_active() == pages[i].screen && pages[i].selecting)
			return true;
	return false;
}
static bool browse_selection_back(void) {
	for (int i = 0; i < 3; i++)
		if (lv_screen_active() == pages[i].screen && pages[i].selecting) {
			pages[i].selecting = false;
			pages[i].select_busy = false;
			pages[i].selected = 0;
			pages[i].session++;
			browse_selection_ui(&pages[i]);
			submit_window(&pages[i]);
			return true;
		}
	return false;
}
static void browse_favorite_done(nd_star_result_t *r, void *data) {
	uintptr_t token = (uintptr_t)data;
	browse_page_t *p = &pages[token & 3];
	if (p->serial == (unsigned)(token >> 2) && lv_screen_active() == p->screen) {
		p->select_busy = false;
		if (navidrome_star_current(&r->context)) {
			if (!r->error && r->request.action != ND_STAR_CHECK)
				toast_success("nd_favorites_saved");
			else if (r->request.action == ND_STAR_SELECTED) {
				char message[400], summary[200];
				lv_snprintf(summary, sizeof(summary), tr("nd_favorites_batch"), r->completed, r->total);
				lv_snprintf(message, sizeof(message), "%s%s%s", summary, r->error ? "\n" : "", r->error ? tr(r->error) : "");
				gui_notify_popup(message);
			} else
				gui_notify_popup(r->error ? r->error : r->request.action == ND_STAR_CHECK ? "nd_favorites_checked" : "nd_favorites_saved");
		}
		browse_selection_ui(p);
	}
	free(r);
}
static void browse_favorite_start(browse_page_t *p, nd_star_request_t *q) {
	nd_context_t captured;
	if (!navidrome_context(&captured, false)) {
		gui_notify_popup("nd_favorites_stale");
		return;
	}
	q->bound_profile = true;
	q->profile_epoch = captured.profile_epoch;
	q->library = p->library;
	nd_wipe(&captured, sizeof(captured));
	nd_library_t current;
	browse_snapshot(p->query.favorites, &current);
	if (!current.ready || p->library.epoch != current.epoch || p->library.card_epoch != current.card_epoch || p->library.generation != current.generation) {
		gui_notify_popup("nd_favorites_stale");
		return;
	}
	if (p->loading || p->select_busy)
		return;
	uintptr_t token = ((uintptr_t)p->serial << 2) | (unsigned)(p - pages);
	if (navidrome_star_submit(q, browse_favorite_done, (void *)token)) {
		p->select_busy = true;
		browse_selection_ui(p);
	} else {
		navidrome_snapshot_t snapshot;
		navidrome_snapshot(&snapshot);
		gui_notify_popup(snapshot.state == ND_CONNECTED ? "nd_working" : navidrome_feedback(&snapshot));
	}
}
static void browse_download_done(bool ok, const char *error, int added, int skipped, void *data) {
    uintptr_t token = (uintptr_t)data;
    browse_page_t *p = &pages[token & 3];
    if (p->serial != (unsigned)(token >> 2) || lv_screen_active() != p->screen) return;
    p->select_busy = false;
    if (ok) {
        char text[160]; lv_snprintf(text, sizeof(text), tr("nd_dl_admitted"), added, skipped);
        toast_success(text);
    } else gui_notify_popup(error ? error : "nd_dl_capture_error");
    browse_selection_ui(p);
}
static void browse_download_kind(browse_page_t *p, const char *id, nd_kind_t kind) {
    if (p->select_busy || p->picking || gesture()) return;
    nd_dl_request_t q = {.action = NDDL_CAPTURE, .library = p->library, .kind = kind, .session = id ? 0 : p->session};
    if (id) snprintf(q.id, sizeof(q.id), "%s", id);
    const char *folder = config_get("navidrome", "download_folder", "Music/Navidrome");
    if (strlen(folder) >= sizeof(q.destination)) { gui_notify_popup("nd_dl_path_error"); return; }
    snprintf(q.destination, sizeof(q.destination), "%s", folder);
    uintptr_t token = ((uintptr_t)p->serial << 2) | (unsigned)(p - pages);
    if (navidromedownloads_request(&q, browse_download_done, (void *)token)) {
        p->select_busy = true; browse_selection_ui(p);
    } else gui_notify_popup("nd_working");
}
static void browse_download(browse_page_t *p, const char *id) {
    browse_download_kind(p, id, p->query.kind);
}
static void browse_selection_button(lv_event_t *e) {
	if (gesture())
		return;
	browse_page_t *p = lv_event_get_user_data(e);
	if (lv_event_get_current_target(e) == p->select_close) {
		browse_selection_back();
		return;
	}
	if (lv_event_get_current_target(e) == p->select_download) {
		if (p->selected > 0) browse_download(p, NULL);
		return;
	}
	if (lv_event_get_current_target(e) == p->select_queue) {
		if (p->select_busy || p->selected <= 0) return;
		if (!navidromeplay_enqueue(&p->library, p->query.kind, NULL, p->session)) gui_notify_popup("nd_working");
		else { p->queue_busy = p->select_busy = true; browse_selection_ui(p); }
		return;
	}
	if (lv_event_get_current_target(e) == p->select_favorite) {
		if (p->selecting && p->selected > 0 && (p->query.kind == ND_TRACKS || p->query.kind == ND_ALBUMS)) {
			nd_star_request_t q = {.action = ND_STAR_SELECTED, .kind = p->query.kind, .session = p->session, .library = p->library};
			browse_favorite_start(p, &q);
		}
		return;
	}
	if (p->selected > 0)
		navidromeplaylists_pick_source(p->session, p->library.generation, p->screen, p->query.favorites);
}
static void browse_selection_done(nd_pl_result_t *r, void *data) {
	uintptr_t token = (uintptr_t)data;
	int level = (int)(token & 3);
	bool picker = (token & 4) != 0;
	unsigned serial = (unsigned)(token >> 3);
	browse_page_t *p = &pages[level];
	if (p->serial != serial || lv_screen_active() != p->screen) {
		free(r);
		return;
	}
	p->select_busy = false;
	if (r->error) {
		gui_notify_popup(r->error);
		free(r);
		return;
	}
	p->selected = r->selected;
	p->selecting = p->selected > 0;
	browse_selection_ui(p);
	if (picker)
		navidromeplaylists_pick_source(p->session, p->library.generation, p->screen, p->query.favorites);
	else
		submit_window(p);
	free(r);
}
static void browse_row_select(browse_page_t *p, lv_obj_t *row, bool picker) {
	nd_library_t current;
	browse_snapshot(p->query.favorites, &current);
	if (!current.ready || p->library.epoch != current.epoch || p->library.card_epoch != current.card_epoch || strcmp(p->library.scope, current.scope)) {
		gui_notify_popup("nd_pl_stale");
		return;
	}
	if (p->loading || p->select_busy || navidrome_playlists_busy() || (p->query.kind != ND_TRACKS && p->query.kind != ND_ALBUMS))
		return;
	for (int i = 0; i < p->count; i++)
		if (p->rows[i] == row && *p->data[i].id) {
			nd_pl_request_t q = {.action = (!p->selecting || picker) ? NDPL_SELECT_ONLY : NDPL_SELECT, .session = p->session, .ordinal = p->album.active ? p->album_window.ordinal[i] : p->first + i, .kind = p->query.kind, .generation = p->library.generation, .favorites = p->query.favorites};
			snprintf(q.track, sizeof(q.track), "%s", p->data[i].id);
			uintptr_t token = ((uintptr_t)p->serial << 3) | (picker ? 4 : 0) | (unsigned)(p - pages);
			if (!navidrome_playlists_submit(&q, browse_selection_done, (void *)token))
				gui_notify_popup("nd_working");
			else
				p->select_busy = true;
			return;
		}
}
static void browse_long_cb(lv_event_t *e) {
	if (gesture())
		return;
	browse_page_t *p = lv_event_get_user_data(e);
	if (p->query.kind != ND_TRACKS && p->query.kind != ND_ALBUMS)
		return;
	if (p->selecting)
		for (int i = 0; i < p->count; i++)
			if (p->rows[i] == lv_event_get_current_target(e) && p->chosen[i])
				return;
	lv_indev_t *indev = lv_indev_active();
	if (indev)
		lv_indev_wait_release(indev);
	browse_row_select(p, lv_event_get_current_target(e), false);
}
static browse_page_t *browse_menu_page;
static int browse_menu_index;
static unsigned browse_menu_serial;
static nd_star_request_t browse_menu_favorite;
static void browse_favorite_menu(void *arg) {
	if (!browse_menu_page || browse_menu_page->serial != browse_menu_serial || lv_screen_active() != browse_menu_page->screen || gesture())
		return;
	nd_star_request_t q = browse_menu_favorite;
	q.action = (nd_star_action_t)(uintptr_t)arg;
	browse_favorite_start(browse_menu_page, &q);
}
static void browse_add_menu(void *unused) {
	(void)unused;
	if (browse_menu_page && browse_menu_index < browse_menu_page->count && browse_menu_page->serial == browse_menu_serial)
		browse_row_select(browse_menu_page, browse_menu_page->rows[browse_menu_index], true);
}
static void browse_queue_menu(void *unused) {
	(void)unused;
	browse_page_t *p = browse_menu_page;
	if (!p || p->serial != browse_menu_serial || lv_screen_active() != p->screen || gesture()) return;
	if (!navidromeplay_enqueue(&p->library, p->query.kind, p->data[browse_menu_index].id, 0))
		gui_notify_popup("nd_working");
}
static void browse_download_menu(void *unused) {
    (void)unused;
    browse_page_t *p = browse_menu_page;
    if (p && p->serial == browse_menu_serial && browse_menu_index < p->count && lv_screen_active() == p->screen)
        browse_download(p, p->data[browse_menu_index].id);
}
static void browse_menu_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e);
	if (gesture())
		return;
	browse_page_t *p = lv_event_get_user_data(e);
	if (p->loading || p->select_busy)
		return;
	for (int i = 0; i < p->count; i++)
		if (p->menus[i] == lv_event_get_current_target(e) && *p->data[i].id) {
			browse_menu_page = p;
			browse_menu_index = i;
			browse_menu_serial = p->serial;
			browse_menu_favorite = (nd_star_request_t){.kind = p->query.kind};
			snprintf(browse_menu_favorite.id, sizeof(browse_menu_favorite.id), "%s", p->data[i].id);
			browse_menu_favorite.metadata.row = p->data[i];
			nd_star_state_t star = navidrome_star_state(p->query.kind, p->data[i].id);
			if (star == ND_STAR_UNKNOWN && (p->data[i].favorite_known || p->data[i].favorite))
				star = p->data[i].favorite ? ND_STAR_SET : ND_STAR_CLEAR;
			popover_item_t items[6];
			int n = 0;
			bool favorite = star == ND_STAR_SET || ((star == ND_STAR_UNKNOWN || star == ND_STAR_UNCERTAIN) && p->data[i].favorite);
			nd_star_action_t action = star == ND_STAR_UNCERTAIN ? ND_STAR_TOGGLE : favorite ? ND_STAR_REMOVE : ND_STAR_ADD;
			items[n++] = (popover_item_t){favorite ? "remove_from_favourites" : "medialist_add_to_favourites", browse_favorite_menu, (void *)(uintptr_t)action, false};
			if (navidrome_star_retry(p->query.kind, p->data[i].id) || p->data[i].details)
				items[n++] = (popover_item_t){"nd_favorites_retry", browse_favorite_menu, (void *)(uintptr_t)ND_STAR_RETRY, false};
			if (p->query.kind == ND_TRACKS || p->query.kind == ND_ALBUMS) {
				items[n++] = (popover_item_t){"add_to_queue", browse_queue_menu, NULL, false};
				items[n++] = (popover_item_t){"add_to_playlist", browse_add_menu, NULL, false};
				if (!p->picking) items[n++] = (popover_item_t){"nd_dl_download", browse_download_menu, NULL, false};
			}
			popover_show(p->menus[i], items, n);
			return;
		}
}

void navidromebrowse_init(gui_config_t *cfg) {
	pitch = ui_px(100) + ui_px(8);
	for (int level = 0; level < 3; level++) {
		browse_page_t *p = &pages[level];
		p->screen = lv_obj_create(NULL);
		navidromepage_register(p->screen);
		lv_obj_add_style(p->screen, &theme_style_screen, 0);
		p->title = settingsrow_title(p->screen, cfg, "navidrome");
		settingsrow_title_corner_slots(p->title, cfg, 0);
		p->session = 2000 + level;
		for (int slot = 0; slot < 5; slot++) {
			lv_obj_t *button = lv_btn_create(p->screen);
			if (slot == 4)
				p->select_download = button;
			else if (slot == 3)
				p->select_queue = button;
			else if (slot == 2)
				p->select_favorite = button;
			else if (slot == 1)
				p->select_add = button;
			else
				p->select_close = button;
			lv_obj_set_size(button, ui_px(48), ui_px(48));
			lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * ui_px(50), cfg->padding + cfg->top_bar_height);
			lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
			lv_obj_set_style_border_width(button, 0, 0);
			lv_obj_set_style_shadow_width(button, 0, 0);
			lv_obj_set_style_pad_all(button, 0, 0);
			lv_obj_t *glyph = lv_image_create(button);
			lv_image_set_src(glyph, slot == 4 ? &icon_download : slot == 3 ? &icon_list_queue : slot == 2 ? &icon_star_plus : slot == 1 ? &icon_list_plus : &icon_close);
			lv_obj_add_style(glyph, &theme_style_icon, 0);
			lv_obj_center(glyph);
			lv_obj_add_event_cb(button, browse_selection_button, LV_EVENT_CLICKED, p);
			switcher_attach_back_gesture(button);
			player_sheet_attach_drag(button, true);
			lv_obj_set_hidden(button, true);
		}
		switcher_set_back_guard(p->screen, browse_selection_back);
		switcher_set_back_guard_peek(p->screen, browse_selection_peek);
		p->status = lv_label_create(p->screen);
		lv_obj_add_style(p->status, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(p->status, &font_ui_18, 0);
		lv_obj_set_pos(p->status, cfg->padding, settingsrow_content_top(cfg));
		p->selection_status = lv_label_create(p->screen);
		lv_obj_set_pos(p->selection_status, cfg->padding, settingsrow_content_top(cfg));
		lv_obj_set_width(p->selection_status, cfg->screen_width - 2 * cfg->padding);
		lv_obj_set_style_text_font(p->selection_status, &font_ui_18, 0);
		lv_obj_set_style_bg_opa(p->selection_status, LV_OPA_COVER, 0);
		lv_obj_set_hidden(p->selection_status, true);
		p->list = lv_obj_create(p->screen);
		lv_obj_remove_style_all(p->list);
		lv_obj_set_pos(p->list, cfg->padding, settingsrow_content_top(cfg) + ui_px(32));
		lv_obj_set_size(p->list, cfg->screen_width - 2 * cfg->padding, cfg->screen_height - settingsrow_content_top(cfg) - ui_px(32) - cfg->padding);
		lv_obj_set_scroll_dir(p->list, LV_DIR_VER);
		p->extent = lv_obj_create(p->list);
		lv_obj_remove_style_all(p->extent);
		lv_obj_set_size(p->extent, 1, 1);
		lv_obj_set_clickable(p->extent, false);
		lv_obj_set_scrollable(p->extent, false);
		for (int i = 0; i < ND_WINDOW; i++) {
			p->rows[i] = lv_btn_create(p->list);
			lv_obj_set_size(p->rows[i], lv_pct(100), ui_px(100));
			lv_obj_add_style(p->rows[i], &theme_style_card, 0);
			lv_obj_add_style(p->rows[i], &theme_style_card_pressed, LV_STATE_PRESSED);
			lv_obj_set_style_shadow_width(p->rows[i], 0, 0);
			lv_obj_set_style_pad_all(p->rows[i], ui_px(14), 0);
			lv_obj_set_scrollable(p->rows[i], false);
			p->playmarks[i] = playmark_create(p->rows[i]);
			p->images[i] = lv_image_create(p->rows[i]);
			lv_image_set_inner_align(p->images[i], LV_IMAGE_ALIGN_CONTAIN);
			lv_obj_align(p->images[i], LV_ALIGN_LEFT_MID, 0, 0);
			p->names[i] = lv_label_create(p->rows[i]);
			p->details[i] = lv_label_create(p->rows[i]);
			p->durations[i] = nd_row_duration_create(p->details[i]);
			p->art_slots[i] = i;
			lv_obj_t *labels[] = {p->names[i], p->details[i]};
			for (int j = 0; j < 2; j++) {
				lv_obj_add_style(labels[j], j ? &theme_style_text_dim : &theme_style_text, 0);
				lv_obj_set_style_text_font(labels[j], j ? &font_ui_18 : &font_ui_20, 0);
				lv_label_set_long_mode(labels[j], LV_LABEL_LONG_DOT);
				lv_obj_set_width(labels[j], cfg->screen_width - 2 * cfg->padding - ui_px(168));
				// DOT mode needs a height limit to ellipsize instead of wrapping.
				lv_obj_set_height(labels[j], lv_font_get_line_height(j ? &font_ui_18 : &font_ui_20));
				lv_obj_set_pos(labels[j], ui_px(86), ui_px(j ? 39 : 8));
			}
			lv_obj_set_hidden(p->rows[i], true);
			lv_obj_set_hidden(p->images[i], true);
			lv_obj_add_event_cb(p->rows[i], row_clicked, LV_EVENT_CLICKED, NULL);
			switcher_attach_back_gesture(p->rows[i]);
			player_sheet_attach_drag(p->rows[i], true);
		}

        for (int i = 0; i < ND_WINDOW; i++) {
            p->numbers[i] = lv_label_create(p->rows[i]);
            lv_obj_add_style(p->numbers[i], &theme_style_text_dim, 0);
            lv_obj_set_style_text_font(p->numbers[i], &font_ui_20, 0);
            lv_obj_set_width(p->numbers[i], ui_px(44));
            lv_obj_set_style_text_align(p->numbers[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(p->numbers[i], LV_ALIGN_LEFT_MID, 0, 0);
            lv_obj_set_hidden(p->numbers[i], true);
        }
        navidromealbum_init(&p->album, cfg, p->screen, p->list, p->title, p->status, NULL,
                           ND_ART_ALBUM_SLOT, browse_album_action, p);
		p->index = azindex_create(p->screen, cfg, p->list, pitch, letter_jump);
		azindex_set_area(p->index, settingsrow_content_top(cfg) + ui_px(32), cfg->screen_height - settingsrow_content_top(cfg) - ui_px(32) - cfg->padding);
		for (int i = 0; i < ND_WINDOW; i++) {
			p->menus[i] = lv_btn_create(p->rows[i]);
			lv_obj_set_size(p->menus[i], ui_px(40), ui_px(52));
			lv_obj_align(p->menus[i], LV_ALIGN_RIGHT_MID, 0, 0);
			lv_obj_set_style_bg_opa(p->menus[i], LV_OPA_TRANSP, 0);
			lv_obj_set_style_border_width(p->menus[i], 0, 0);
			lv_obj_set_style_shadow_width(p->menus[i], 0, 0);
			lv_obj_t *glyph = lv_image_create(p->menus[i]);
			lv_image_set_src(glyph, &icon_ellipsis_vertical);
			lv_obj_add_style(glyph, &theme_style_icon, 0);
			lv_obj_center(glyph);
			p->checks[i] = lv_image_create(p->rows[i]);
			lv_image_set_src(p->checks[i], &icon_check);
			lv_obj_add_style(p->checks[i], &theme_style_icon, 0);
			lv_obj_align(p->checks[i], LV_ALIGN_RIGHT_MID, 0, 0);
			lv_obj_set_hidden(p->checks[i], true);
			lv_obj_add_event_cb(p->menus[i], browse_menu_cb, LV_EVENT_CLICKED, p);
			lv_obj_add_event_cb(p->rows[i], browse_long_cb, LV_EVENT_LONG_PRESSED, p);
		}
		lv_obj_add_event_cb(p->list, scrolling, LV_EVENT_SCROLL, p);
		lv_obj_add_event_cb(p->screen, loaded, LV_EVENT_SCREEN_LOADED, p);
		lv_obj_add_event_cb(p->screen, unloaded, LV_EVENT_SCREEN_UNLOADED, p);
		switcher_attach_back_gesture(p->screen);
		switcher_attach_back_gesture(p->list);
		player_sheet_attach_drag(p->screen, true);
		player_sheet_attach_drag(p->list, true);
	}
	browse_timer = lv_timer_create(tick, 100, NULL);
	lv_timer_pause(browse_timer);
}

static void browse_album_action(void *user, nd_album_action_t action) {
    browse_page_t *p = user;
    if (gesture() || p->loading || p->select_busy || p->selecting || p->picking || !p->album.active)
        return;
    nd_library_t library;
    browse_snapshot(p->query.favorites, &library);
    if (!library.ready || library.epoch != p->library.epoch || library.card_epoch != p->library.card_epoch || library.generation != p->library.generation)
        return;
    if (action == ND_ALBUM_DOWNLOAD) {
        if (p->query.kind == ND_TRACKS && p->query.filter[0])
            browse_download_kind(p, p->query.filter, ND_ALBUMS);
        return;
    }
    if (!navidromeplay_album(&p->library, &p->query, (int)action, p->screen))
        gui_notify_popup("nd_audio_unavailable");
}

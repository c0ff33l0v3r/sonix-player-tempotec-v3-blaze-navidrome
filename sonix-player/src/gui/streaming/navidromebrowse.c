#include "navidromebrowse.h"
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
	lv_obj_t *screen, *list, *extent, *title, *status;
	lv_obj_t *rows[ND_WINDOW], *names[ND_WINDOW], *details[ND_WINDOW], *images[ND_WINDOW], *checks[ND_WINDOW];
	cover_image_t artwork[ND_WINDOW];
	bool art_finished[ND_WINDOW], art_bound[ND_WINDOW], loading, map_ready, map_bound;
	azindex_t *index;
	int letters[ND_LETTER_BUCKETS], requested_first;
	nd_row_t data[ND_WINDOW];
	nd_query_t query;
	nd_library_t library;
	unsigned serial;
	uint32_t submitted;
	int first, count, total, selected;
	bool selecting, select_busy, picking;
	int64_t session;
	int64_t favorite_revision;
	lv_obj_t *select_add, *select_close, *select_favorite, *menus[ND_WINDOW];
	bool chosen[ND_WINDOW];
} browse_page_t;
static browse_page_t pages[3];
static int pitch;
static lv_timer_t *browse_timer;
static const char *const captions[] = {"albums", "navidrome_tracks", "artists", "music_genres"};
typedef struct {
	int page, first, count, total;
	unsigned serial;
	nd_library_t library;
	nd_query_t query;
	nd_row_t rows[ND_WINDOW];
	bool ok, filter_exists, need_map;
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

static void browse_snapshot(bool favorites, nd_library_t *out) {
	if (favorites)
		navidrome_favorites_snapshot(out);
	else
		navidrome_library_snapshot(out);
}

static bool gesture(void) { return switcher_back_drag_active() || player_sheet_drag_active() || coverflow_drag_active(); }

static void release_art_row(browse_page_t *p, int i) {
	navidromeart_release(i);
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
	if (p->serial != result->serial || lv_screen_active() != p->screen || !current.ready || current.epoch != result->library.epoch || current.card_epoch != result->library.card_epoch || current.generation != result->library.generation) {
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
		switch_screen_return_to(result->page ? pages[result->page - 1].screen : result->query.favorites ? navidromefavorites_page() : navidrome_screen);
		free(result);
		return;
	}
	if (result->first > 0 && result->first >= result->total) {
		lv_obj_scroll_to_y(p->list, result->total ? (result->total - 1) * pitch : 0, LV_ANIM_OFF);
		free(result);
		submit_window(p);
		return;
	}
	bool keep_art[ND_WINDOW];
	// Selection reloads metadata too; unchanged bindings keep decoded covers
	// and pending requests instead of flashing the placeholder for every tap.
	bool same_library = p->library.epoch == result->library.epoch && p->library.card_epoch == result->library.card_epoch && (p->library.generation == result->library.generation || (p->query.favorites && p->library.base == result->library.base)) && !strcmp(p->library.scope, result->library.scope);
	for (int i = 0; i < ND_WINDOW; i++) {
		keep_art[i] = same_library && p->art_bound[i] && i < p->count && i < result->count && !strcmp(p->data[i].id, result->rows[i].id) && !strcmp(p->data[i].cover, result->rows[i].cover);
		if (!keep_art[i] && p->art_bound[i])
			release_art_row(p, i);
	}
	p->library = result->library;
	p->favorite_revision = result->favorite_revision;
	p->first = result->first;
	p->count = result->count;
	p->total = result->total;
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
	lv_obj_set_height(p->extent, p->total ? p->total * pitch : 1);
	char status[80];
	lv_snprintf(status, sizeof(status), tr("nd_item_count"), p->total);
	lv_label_set_text(p->status, result->metadata_details ? tr(result->metadata_details == 1 && navidrome_star_loading(ND_ALBUMS, p->query.filter) ? "nd_favorites_loading" : "nd_favorites_metadata_error") : p->total ? status : tr(p->query.favorites ? "nd_favorites_empty" : "nd_library_empty"));
	for (int i = 0; i < ND_WINDOW; i++) {
		lv_obj_set_hidden(p->rows[i], i >= p->count);
		lv_obj_set_hidden(p->menus[i], p->selecting || (p->query.kind != ND_TRACKS && p->query.kind != ND_ALBUMS && p->query.kind != ND_ARTISTS));
		lv_obj_set_hidden(p->checks[i], !p->chosen[i]);
		lv_obj_set_style_bg_opa(p->rows[i], p->chosen[i] ? LV_OPA_50 : LV_OPA_COVER, 0);
		if (i >= p->count)
			continue;
		nd_row_t *row = &p->data[i];
		if (!keep_art[i]) {
			lv_obj_set_size(p->images[i], ui_px(ND_ART_THUMB_SIZE), ui_px(ND_ART_THUMB_SIZE));
			lv_image_set_inner_align(p->images[i], LV_IMAGE_ALIGN_CONTAIN);
			lv_image_set_src(p->images[i], &icon_music_note);
			lv_obj_add_style(p->images[i], &theme_style_icon, 0);
			lv_obj_align(p->images[i], LV_ALIGN_LEFT_MID, 0, 0);
			lv_obj_set_hidden(p->images[i], false);
			p->art_bound[i] = true;
			p->art_finished[i] = !row->cover[0];
			if (row->cover[0])
				navidromeart_request(i, &p->library, row->cover, ui_px(ND_ART_THUMB_SIZE));
		}
		lv_obj_set_y(p->rows[i], (p->first + i) * pitch);
		lv_label_set_text(p->names[i], row->title);
		char detail[ND_TEXT_MAX + 80];
		if (p->query.kind == ND_TRACKS && row->duration > 0)
			lv_snprintf(detail, sizeof(detail), "%s%s%d:%02d", row->artist, row->artist[0] ? "  ·  " : "", row->duration / 60, row->duration % 60);
		else
			lv_snprintf(detail, sizeof(detail), "%s", row->artist);
		lv_label_set_text(p->details[i], row->details ? tr(row->details == 1 && navidrome_star_loading(p->query.kind, row->id) ? "nd_favorites_loading" : "nd_favorites_metadata_error") : detail);
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
		if (result->ok)
			result->ok = nd_store_query(&store, result->library.generation, &result->query, result->first, result->rows, &result->count, &result->total);
		if (result->ok && result->query.favorites && result->query.kind == ND_TRACKS && result->query.filter[0]) {
			nd_playrow_t album;
			if (nd_store_metadata(&store, result->library.generation, ND_ALBUMS, result->query.filter, &album))
				result->metadata_details = album.row.details;
		}
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
	int first = lv_obj_get_scroll_y(p->list) / pitch - 2;
	if (first < 0)
		first = 0;
	p->loading = true;
	p->submitted = lv_tick_get();
	p->serial++;
	pthread_mutex_lock(&window_lock);
	memset(&request, 0, sizeof(request));
	request.page = (int)(p - pages);
	request.serial = p->serial;
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
	int first = lv_obj_get_scroll_y(p->list) / pitch;
	azindex_flash(p->index);
	if ((p->loading && first - 2 != p->requested_first && !(first < 2 && p->requested_first == 0)) || (!p->loading && (first < p->first || first + 10 > p->first + p->count)))
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
			if (p->rows[i] != row)
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
	p->serial++;
	p->loading = false;
	p->map_bound = false;
	azindex_set_rows(p->index, NULL, 0, false);
	p->select_busy = false;
	popover_close();
	release_art(p);
}

void navidromebrowse_refresh(void) {
	navidrome_star_poll();
	nd_library_t library;
	for (int level = 0; level < 3; level++) {
		browse_page_t *p = &pages[level];
		if (!p->screen || lv_screen_active() != p->screen)
			continue;
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
		for (int i = 0; i < p->count; i++) {
			if (p->art_finished[i])
				continue;
			if (navidromeart_take(i, &p->artwork[i], &p->art_finished[i])) {
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
	switch_screen(p->screen);
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
	lv_obj_set_hidden(p->select_add, !p->selecting);
	lv_obj_set_hidden(p->select_close, !p->selecting);
	lv_obj_set_hidden(p->select_favorite, !p->selecting || (p->query.kind != ND_TRACKS && p->query.kind != ND_ALBUMS));
	lv_obj_set_state(p->select_favorite, LV_STATE_DISABLED, p->select_busy || navidrome_star_busy());
	if (p->selecting) {
		char text[80];
		lv_snprintf(text, sizeof(text), tr("nd_pl_selected"), p->selected);
		lv_label_set_text(p->status, text);
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
static void browse_selection_button(lv_event_t *e) {
	if (gesture())
		return;
	browse_page_t *p = lv_event_get_user_data(e);
	if (lv_event_get_current_target(e) == p->select_close) {
		browse_selection_back();
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
		if (p->rows[i] == row) {
			nd_pl_request_t q = {.action = (!p->selecting || picker) ? NDPL_SELECT_ONLY : NDPL_SELECT, .session = p->session, .ordinal = p->first + i, .kind = p->query.kind, .generation = p->library.generation, .favorites = p->query.favorites};
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
static void browse_menu_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e);
	if (gesture())
		return;
	browse_page_t *p = lv_event_get_user_data(e);
	if (p->loading || p->select_busy)
		return;
	for (int i = 0; i < p->count; i++)
		if (p->menus[i] == lv_event_get_current_target(e)) {
			browse_menu_page = p;
			browse_menu_index = i;
			browse_menu_serial = p->serial;
			browse_menu_favorite = (nd_star_request_t){.kind = p->query.kind};
			snprintf(browse_menu_favorite.id, sizeof(browse_menu_favorite.id), "%s", p->data[i].id);
			browse_menu_favorite.metadata.row = p->data[i];
			nd_star_state_t star = navidrome_star_state(p->query.kind, p->data[i].id);
			if (star == ND_STAR_UNKNOWN && (p->data[i].favorite_known || p->data[i].favorite))
				star = p->data[i].favorite ? ND_STAR_SET : ND_STAR_CLEAR;
			popover_item_t items[3];
			int n = 0;
			bool favorite = star == ND_STAR_SET || ((star == ND_STAR_UNKNOWN || star == ND_STAR_UNCERTAIN) && p->data[i].favorite);
			nd_star_action_t action = star == ND_STAR_UNCERTAIN ? ND_STAR_TOGGLE : favorite ? ND_STAR_REMOVE : ND_STAR_ADD;
			items[n++] = (popover_item_t){favorite ? "remove_from_favourites" : "medialist_add_to_favourites", browse_favorite_menu, (void *)(uintptr_t)action, false};
			if (navidrome_star_retry(p->query.kind, p->data[i].id) || p->data[i].details)
				items[n++] = (popover_item_t){"nd_favorites_retry", browse_favorite_menu, (void *)(uintptr_t)ND_STAR_RETRY, false};
			if (p->query.kind == ND_TRACKS || p->query.kind == ND_ALBUMS)
				items[n++] = (popover_item_t){"add_to_playlist", browse_add_menu, NULL, false};
			popover_show(p->menus[i], items, n);
			return;
		}
}

void navidromebrowse_init(gui_config_t *cfg) {
	pitch = ui_px(100) + ui_px(8);
	for (int level = 0; level < 3; level++) {
		browse_page_t *p = &pages[level];
		p->screen = lv_obj_create(NULL);
		lv_obj_add_style(p->screen, &theme_style_screen, 0);
		p->title = settingsrow_title(p->screen, cfg, "navidrome");
		settingsrow_title_corner_slots(p->title, cfg, 3);
		p->session = 2000 + level;
		for (int slot = 0; slot < 3; slot++) {
			lv_obj_t *button = lv_btn_create(p->screen);
			if (slot == 2)
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
			lv_image_set_src(glyph, slot == 2 ? &icon_star_plus : slot == 1 ? &icon_list_plus : &icon_close);
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
			p->images[i] = lv_image_create(p->rows[i]);
			lv_image_set_inner_align(p->images[i], LV_IMAGE_ALIGN_CONTAIN);
			lv_obj_align(p->images[i], LV_ALIGN_LEFT_MID, 0, 0);
			p->names[i] = lv_label_create(p->rows[i]);
			p->details[i] = lv_label_create(p->rows[i]);
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

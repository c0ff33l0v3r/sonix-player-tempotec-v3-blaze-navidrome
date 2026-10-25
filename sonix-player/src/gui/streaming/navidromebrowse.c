#include "navidromebrowse.h"
#include "navidromeart.h"
#include "navidromepage.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/azindex.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/uiscale.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/streaming/navidrome.h"
#include "src/system/streaming/navidromeplay.h"
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	lv_obj_t *screen, *list, *extent, *title, *status;
	lv_obj_t *rows[ND_WINDOW], *names[ND_WINDOW], *details[ND_WINDOW], *images[ND_WINDOW];
	cover_image_t artwork[ND_WINDOW];
	bool art_finished[ND_WINDOW], loading, map_ready, map_bound;
	azindex_t *index;
	int letters[ND_LETTER_BUCKETS], requested_first;
	nd_row_t data[ND_WINDOW];
	nd_query_t query;
	nd_library_t library;
	unsigned serial;
	uint32_t submitted;
	int first, count, total;
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
	int letters[ND_LETTER_BUCKETS];
} window_t;
static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t window_ready = PTHREAD_COND_INITIALIZER;
static bool worker_started, pending;
static window_t request;
static void submit_window(browse_page_t *page);

static bool gesture(void) { return switcher_back_drag_active() || player_sheet_drag_active() || coverflow_drag_active(); }

static void release_art(browse_page_t *p) {
	for (int i = 0; i < ND_WINDOW; i++) {
		navidromeart_release(i);
		lv_obj_set_hidden(p->images[i], true);
		lv_image_set_src(p->images[i], &icon_music_note);
		cover_free(&p->artwork[i]);
		p->art_finished[i] = true;
	}
}

static void window_done(void *value) {
	window_t *result = value;
	browse_page_t *p = &pages[result->page];
	nd_library_t current;
	navidrome_library_snapshot(&current);
	if (p->serial != result->serial || lv_screen_active() != p->screen || !current.ready || current.epoch != result->library.epoch || current.generation != result->library.generation) {
		free(result);
		return;
	}
	p->loading = false;
	if (!result->ok) {
		lv_label_set_text(p->status, tr("nd_cache_error"));
		free(result);
		return;
	}
	if (!result->filter_exists) {
		gui_notify_popup("nd_item_removed");
		switch_screen_return_to(result->page ? pages[result->page - 1].screen : navidrome_screen);
		free(result);
		return;
	}
	release_art(p);
	p->library = result->library;
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
	lv_label_set_text(p->status, p->total ? status : tr("nd_library_empty"));
	for (int i = 0; i < ND_WINDOW; i++) {
		lv_obj_set_hidden(p->rows[i], i >= p->count);
		if (i >= p->count)
			continue;
		nd_row_t *row = &p->data[i];
		lv_obj_set_size(p->images[i], ui_px(32), ui_px(32));
		lv_image_set_src(p->images[i], &icon_music_note);
		lv_obj_add_style(p->images[i], &theme_style_icon, 0);
		lv_obj_align(p->images[i], LV_ALIGN_LEFT_MID, ui_px(16), 0);
		lv_obj_set_hidden(p->images[i], false);
		lv_obj_set_y(p->rows[i], (p->first + i) * pitch);
		lv_label_set_text(p->names[i], row->title);
		char detail[ND_TEXT_MAX + 80];
		if (p->query.kind == ND_TRACKS && row->duration > 0)
			lv_snprintf(detail, sizeof(detail), "%s%s%d:%02d", row->artist, row->artist[0] ? "  ·  " : "", row->duration / 60, row->duration % 60);
		else
			lv_snprintf(detail, sizeof(detail), "%s", row->artist);
		lv_label_set_text(p->details[i], detail);
		p->art_finished[i] = !row->cover[0];
		if (row->cover[0])
			navidromeart_request(i, &p->library, row->cover, ui_px(64));
	}
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
		result->ok = leased && nd_store_open(&store, result->library.path, result->library.scope, false, false);
		result->filter_exists = true;
		if (result->ok && *result->query.filter)
			result->filter_exists = nd_store_contains(&store, result->library.generation, result->query.filter_kind, result->query.filter);
		if (result->ok)
			result->ok = nd_store_query(&store, result->library.generation, &result->query, result->first, result->rows, &result->count, &result->total);
		if (result->ok && result->need_map)
			result->ok = nd_store_letters(&store, result->library.generation, &result->query, result->letters);
		nd_store_close(&store);
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
	navidrome_library_snapshot(&library);
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
	if (lv_screen_active() != p->screen || gesture())
		return;
	int first = lv_obj_get_scroll_y(p->list) / pitch;
	azindex_flash(p->index);
	if ((p->loading && first - 2 != p->requested_first && !(first < 2 && p->requested_first == 0)) || (!p->loading && (first < p->first || first + 10 > p->first + p->count)))
		submit_window(p);
}

static void row_clicked(lv_event_t *e) {
	if (gesture())
		return;
	lv_obj_t *row = lv_event_get_target(e);
	for (int level = 0; level < 3; level++) {
		browse_page_t *p = &pages[level];
		if (lv_screen_active() != p->screen)
			continue;
		nd_library_t current;
		navidrome_library_snapshot(&current);
		if (!current.ready || current.epoch != p->library.epoch)
			return;
		for (int i = 0; i < p->count; i++) {
			if (p->rows[i] != row)
				continue;
			if (p->query.kind == ND_TRACKS) {
				if (!navidromeplay_select(&p->library, &p->query, p->data[i].id, p->screen))
					gui_notify_popup("nd_audio_unavailable");
				return;
			}
			if (level == 2)
				return;
			browse_page_t *next = &pages[level + 1];
			next->map_ready = next->map_bound = false;
			memset(&next->query, 0, sizeof(next->query));
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
	release_art(p);
}

void navidromebrowse_refresh(void) {
	nd_library_t library;
	navidrome_library_snapshot(&library);
	for (int level = 0; level < 3; level++) {
		browse_page_t *p = &pages[level];
		if (!p->screen || lv_screen_active() != p->screen)
			continue;
		if (!library.ready) {
			release_art(p);
			switch_screen_return_to(navidrome_screen);
			return;
		}
		if (p->library.epoch != library.epoch) {
			p->map_ready = p->map_bound = false;
			azindex_set_rows(p->index, NULL, 0, false);
		}
		if (!p->loading && p->library.epoch != library.epoch) {
			release_art(p);
			for (int i = 0; i < ND_WINDOW; i++)
				lv_obj_set_hidden(p->rows[i], true);
			submit_window(p);
		}
		if (p->loading && lv_tick_elaps(p->submitted) > 2500)
			submit_window(p); // Recover from a dropped GUI post without a network retry.
		for (int i = 0; i < p->count; i++) {
			if (p->art_finished[i])
				continue;
			if (navidromeart_take(i, &p->artwork[i], &p->art_finished[i])) {
				lv_obj_remove_style(p->images[i], &theme_style_icon, 0);
				lv_obj_set_size(p->images[i], ui_px(64), ui_px(64));
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

void navidromebrowse_open(nd_kind_t kind) {
	browse_page_t *p = &pages[0];
	p->map_ready = p->map_bound = false;
	azindex_set_rows(p->index, NULL, 0, false);
	memset(&p->query, 0, sizeof(p->query));
	p->query.kind = kind;
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

bool navidromebrowse_album(const char *id, const char *title) {
	nd_library_t library;
	navidrome_library_snapshot(&library);
	if (!library.ready || !id || !*id || strlen(id) >= ND_ID_MAX)
		return false;
	browse_page_t *p = &pages[0];
	memset(&p->query, 0, sizeof(p->query));
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
		if (lv_screen_active() == pages[i].screen && !gesture())
			submit_window(&pages[i]);
}

void navidromebrowse_init(gui_config_t *cfg) {
	pitch = ui_px(88);
	for (int level = 0; level < 3; level++) {
		browse_page_t *p = &pages[level];
		p->screen = lv_obj_create(NULL);
		lv_obj_add_style(p->screen, &theme_style_screen, 0);
		p->title = settingsrow_title(p->screen, cfg, "navidrome");
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
			lv_obj_set_size(p->rows[i], lv_pct(100), pitch - ui_px(4));
			lv_obj_add_style(p->rows[i], &theme_style_card, 0);
			lv_obj_add_style(p->rows[i], &theme_style_card_pressed, LV_STATE_PRESSED);
			lv_obj_set_style_shadow_width(p->rows[i], 0, 0);
			lv_obj_set_style_pad_all(p->rows[i], ui_px(8), 0);
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
				lv_obj_set_width(labels[j], cfg->screen_width - 2 * cfg->padding - ui_px(104));
				// DOT mode needs a height limit to ellipsize instead of wrapping.
				lv_obj_set_height(labels[j], lv_font_get_line_height(j ? &font_ui_18 : &font_ui_20));
				lv_obj_set_pos(labels[j], ui_px(76), ui_px(j ? 39 : 8));
			}
			lv_obj_set_hidden(p->rows[i], true);
			lv_obj_set_hidden(p->images[i], true);
			lv_obj_add_event_cb(p->rows[i], row_clicked, LV_EVENT_CLICKED, NULL);
			switcher_attach_back_gesture(p->rows[i]);
			player_sheet_attach_drag(p->rows[i], true);
		}
		p->index = azindex_create(p->screen, cfg, p->list, pitch, letter_jump);
		azindex_set_area(p->index, settingsrow_content_top(cfg) + ui_px(32), cfg->screen_height - settingsrow_content_top(cfg) - ui_px(32) - cfg->padding);
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

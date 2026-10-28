#include "navidromefavorites.h"
#include "navidromebrowse.h"
#include "navidromepage.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/uiscale.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/streaming/navidrome.h"
#include <stdint.h>
#include <time.h>

static lv_timer_t *refresh_timer;
static lv_obj_t *screen, *album_count, *track_count, *refresh_row, *cancel_row, *status, *last_refresh;

static bool gesture(void) { return switcher_back_drag_active() || player_sheet_drag_active() || coverflow_drag_active(); }

static const char *refresh_reason(const navidrome_snapshot_t *s, const nd_library_t *cache) {
	if (!s->unlocked || s->state != ND_CONNECTED)
		return navidrome_feedback(s);
	if (s->busy || cache->loading)
		return "nd_working";
	if (!cache->writable || !*cache->path || !*cache->scope)
		return cache->error ? cache->error : "nd_cache_storage";
	return NULL;
}

static void category_clicked(lv_event_t *e) {
	if (gesture())
		return;
	navidrome_snapshot_t s;
	nd_library_t cache;
	navidrome_snapshot(&s);
	navidrome_favorites_snapshot(&cache);
	if (!s.unlocked) {
		gui_notify_popup(navidrome_feedback(&s));
		return;
	}
	if (!cache.ready || (!cache.base && !cache.counts[(nd_kind_t)(intptr_t)lv_event_get_user_data(e)])) {
		gui_notify_popup(cache.error ? cache.error : "nd_favorites_not_refreshed");
		return;
	}
	navidromebrowse_open_favorites((nd_kind_t)(intptr_t)lv_event_get_user_data(e));
}

static void refresh_clicked(lv_event_t *e) {
	(void)e;
	if (gesture())
		return;
	navidrome_poll_network();
	navidrome_snapshot_t s;
	nd_library_t cache;
	navidrome_snapshot(&s);
	navidrome_favorites_snapshot(&cache);
	const char *reason = refresh_reason(&s, &cache);
	if (reason)
		gui_notify_popup(reason);
	else if (!navidrome_favorites_refresh())
		gui_notify_popup("nd_working");
	navidromefavorites_update();
}

static void cancel_clicked(lv_event_t *e) {
	(void)e;
	if (!gesture())
		navidrome_favorites_cancel();
}

void navidromefavorites_update(void) {
	if (!screen || lv_screen_active() != screen)
		return;
	navidrome_snapshot_t s;
	nd_library_t cache;
	navidrome_snapshot(&s);
	navidrome_favorites_snapshot(&cache);
	if (!s.unlocked) {
		switch_screen_return_to(navidrome_screen);
		return;
	}
	char text[256];
	if (cache.ready) {
		lv_snprintf(text, sizeof(text), "%d", cache.counts[ND_ALBUMS]);
		lv_label_set_text(album_count, text);
		lv_snprintf(text, sizeof(text), "%d", cache.counts[ND_TRACKS]);
		lv_label_set_text(track_count, text);
	} else {
		lv_label_set_text(album_count, "—");
		lv_label_set_text(track_count, "—");
	}
	lv_obj_set_hidden(refresh_row, cache.syncing);
	lv_obj_set_hidden(cancel_row, !cache.syncing);
	lv_obj_set_style_opa(refresh_row, refresh_reason(&s, &cache) ? LV_OPA_40 : LV_OPA_COVER, 0);
	if (cache.syncing) {
		lv_snprintf(text, sizeof(text), tr("nd_favorites_progress"), tr(cache.stage == ND_ALBUMS ? "albums" : "navidrome_tracks"), cache.fetched);
		lv_label_set_text(status, text);
	} else if (cache.error)
		lv_label_set_text(status, tr(cache.error));
	else if (!cache.ready || (!cache.base && !cache.counts[ND_ALBUMS] && !cache.counts[ND_TRACKS]))
		lv_label_set_text(status, tr("nd_favorites_not_refreshed"));
	else
		lv_label_set_text(status, tr(cache.base ? "nd_favorites_cached" : "nd_favorites_known"));
	if (cache.ready && cache.finished) {
		struct tm tm;
		char stamp[64];
		localtime_r(&cache.finished, &tm);
		strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", &tm);
		lv_snprintf(text, sizeof(text), tr("nd_favorites_last_refresh"), stamp);
		lv_label_set_text(last_refresh, text);
	} else
		lv_label_set_text(last_refresh, tr("nd_favorites_never_refreshed"));
}

static void loaded(lv_event_t *e) {
	(void)e;
	lv_timer_resume(refresh_timer);
	navidromefavorites_update();
}

static void unloaded(lv_event_t *e) {
	(void)e;
	lv_timer_pause(refresh_timer);
}

static void tick(lv_timer_t *timer) {
	(void)timer;
	navidromefavorites_update();
}

lv_obj_t *navidromefavorites_page(void) { return screen; }

void navidromefavorites_open(void) {
	if (gesture())
		return;
	navidrome_snapshot_t s;
	navidrome_snapshot(&s);
	if (!s.unlocked) {
		gui_notify_popup(navidrome_feedback(&s));
		return;
	}
	switch_screen(screen);
}

void navidromefavorites_init(gui_config_t *cfg) {
	screen = lv_obj_create(NULL);
	navidromepage_register(screen);
	lv_obj_t *column = settingsrow_page(screen, cfg, "nd_favorites");
	player_sheet_attach_drag(column, true);
	settingsrow_add(column, "albums", &album_count, category_clicked, (void *)(intptr_t)ND_ALBUMS);
	settingsrow_add(column, "navidrome_tracks", &track_count, category_clicked, (void *)(intptr_t)ND_TRACKS);
	refresh_row = settingsrow_action(column, "nd_favorites_refresh", refresh_clicked, NULL);
	cancel_row = settingsrow_action(column, "nd_favorites_cancel", cancel_clicked, NULL);
	lv_obj_set_hidden(cancel_row, true);
	status = lv_label_create(column);
	last_refresh = lv_label_create(column);
	lv_obj_t *labels[] = {status, last_refresh};
	for (int i = 0; i < 2; i++) {
		lv_obj_add_style(labels[i], &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(labels[i], &font_ui_18, 0);
		lv_obj_set_width(labels[i], lv_pct(100));
	}
	refresh_timer = lv_timer_create(tick, 250, NULL);
	power_pause_in_standby(refresh_timer);
	lv_timer_pause(refresh_timer);
	lv_obj_add_event_cb(screen, loaded, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(screen, unloaded, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(screen);
	player_sheet_attach_drag(screen, true);
}

#include "navidromepage.h"
#include "navidromediscovery.h"
#include "navidromefavorites.h"
#include "navidromesearch.h"
#include "navidromebrowse.h"
#include "navidromeplaylists.h"
#include "navidromesettings.h"
#include "src/system/streaming/navidromeplay.h"
#include <stdlib.h>
#include <string.h>

#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/uiscale.h"
#include "src/system/core/lang.h"
#include "src/system/core/config.h"
#include "src/system/streaming/navidrome.h"

lv_obj_t *navidrome_screen;
static lv_obj_t *category_grid, *search_icon;
static lv_timer_t *entry_timer;
static lv_obj_t *entry_status;

static void unavailable_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active()) {
		return;
	}

	(void)e;
	navidromesearch_open();
}

static void category_action(nd_kind_t kind) {
	navidrome_snapshot_t s;
	nd_library_t library;
	navidrome_snapshot(&s);
	navidrome_library_snapshot(&library);
	if (!s.unlocked) {
		gui_notify_popup(navidrome_feedback(&s));
		return;
	}
	if (!library.ready) {
		gui_notify_popup(library.error ? library.error : "nd_not_synced");
		return;
	}
	navidromebrowse_open(kind);
}
static void albums_action(void) { category_action(ND_ALBUMS); }
static void tracks_action(void) { category_action(ND_TRACKS); }
static void artists_action(void) { category_action(ND_ARTISTS); }
static void genres_action(void) { category_action(ND_GENRES); }
static void playlists_action(void) { navidromeplaylists_open(); }

static void settings_cb(lv_event_t *e) {
	(void)e;
	if (!player_sheet_drag_active() && !switcher_back_drag_active() && !coverflow_drag_active())
		navidromesettings_open();
}

static void refresh(void) {
	navidrome_snapshot_t snapshot;
	navidrome_snapshot(&snapshot);
	bool dim = snapshot.state != ND_CONNECTED;
	if (entry_status)
		lv_label_set_text(entry_status, tr(snapshot.state == ND_DISCONNECTED && !snapshot.busy && !snapshot.auto_waiting ?
			"nd_connect_retry" : navidrome_feedback(&snapshot)));
	nd_library_t library;
	navidrome_library_snapshot(&library);
	for (int i = 0; i < 6; i++)
		gridpage_set_dimmed(category_grid, i, i == 5 ? !snapshot.unlocked : i == 4 ? !(snapshot.unlocked && *library.path && *library.scope) : !library.ready);
	lv_obj_set_style_opa(search_icon, (!snapshot.unlocked || (dim && !library.ready)) ? LV_OPA_40 : LV_OPA_COVER, 0);
	navidromesettings_refresh();
	navidromebrowse_refresh();
	navidromeplaylists_refresh();
	navidromefavorites_update();
	navidromesearch_refresh();
	navidromediscovery_refresh();
}

// Display notifications cover direct entry, every child and every exit.
// A sheet leaves the underlying screen active, so it does not restart a session.
static void entry_tick(lv_timer_t *timer) {
	(void)timer;
	lv_obj_t *screen = lv_screen_active();
	bool family = screen && lv_obj_get_user_flag(screen, 3);
	navidrome_auto_tick(family, config_get_bool("navidrome", "auto_connect", false), lv_tick_get());
	if (family)
		navidrome_poll_network();
	else if (entry_timer)
		lv_timer_pause(entry_timer);
	if (screen == navidrome_screen)
		refresh();
}
static void entry_shown(lv_event_t *event) {
	(void)event;
	if (lv_screen_active() == navidrome_screen) gridpage_hint_scroll(category_grid);
	entry_tick(NULL);
	lv_obj_t *screen = lv_screen_active();
	if (entry_timer && screen && lv_obj_get_user_flag(screen, 3))
		lv_timer_resume(entry_timer);
}
static void entry_retry(lv_event_t *event) {
	(void)event;
	if (player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active())
		return;
	navidrome_snapshot_t state;
	navidrome_snapshot(&state);
	if (state.unlocked && !state.busy && state.state == ND_DISCONNECTED)
		navidrome_reconnect();
	else if (!state.unlocked)
		navidromesettings_open();
}
static void entry_init(void) {
	entry_timer = lv_timer_create(entry_tick, 250, NULL);
	lv_timer_pause(entry_timer);
	lv_display_add_event_cb(lv_display_get_default(), entry_shown, LV_EVENT_SCREEN_LOADED, NULL);
}

// Match Music's two header controls; bubble presses to the screen so gestures
// starting on a button use the same navigation as the rest of the header.
static void corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph, const char *label) {
	lv_obj_t *button = lv_btn_create(navidrome_screen);
	lv_obj_set_size(button, ui_px(56), ui_px(56));
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (ui_px(56) + ui_px(6)), cfg->padding + cfg->top_bar_height);
	lv_obj_set_event_bubble(button, true);
	lv_obj_add_event_cb(button, unavailable_cb, LV_EVENT_CLICKED, (void *)label);
	if (!strcmp(label, "settings")) {
		lv_obj_remove_event_cb(button, unavailable_cb);
		lv_obj_add_event_cb(button, settings_cb, LV_EVENT_CLICKED, NULL);
	}

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
	if (!strcmp(label, "navidrome_search"))
		search_icon = icon;
}

void navidromepage_init(gui_config_t *cfg) {
	navidromeplay_init();
	atexit(navidromeplay_shutdown);
	navidrome_screen = lv_obj_create(NULL);
	navidromepage_register(navidrome_screen);

	const grid_entry_t entries[] = {
		{"albums", &icon_menu_album, NULL, albums_action}, {"navidrome_tracks", &icon_menu_all, NULL, tracks_action}, {"artists", &icon_menu_artist, NULL, artists_action}, {"music_genres", &icon_menu_genre, NULL, genres_action}, {"playlists", &icon_menu_playlist, NULL, playlists_action}, {"nd_favorites", &icon_menu_favourites, NULL, navidromefavorites_open}, {"nd_discovery", &icon_menu_explorer, NULL, navidromediscovery_open},
	};
	category_grid = gridpage_build(navidrome_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3, true);
	gridpage_set_scrolling(category_grid);
	settingsrow_title_corner_slots(settingsrow_title(navidrome_screen, cfg, "navidrome"), cfg, 2);
	corner_button(cfg, 0, &icon_music_settings, "settings");
	corner_button(cfg, 1, &icon_search, "navidrome_search");
	navidromesettings_init(cfg);
	navidromebrowse_init(cfg);
	navidromeplaylists_init(cfg);
	navidromefavorites_init(cfg);
	navidromesearch_init(cfg);
	navidromediscovery_init(cfg);
	navidrome_set_listener(refresh);
	entry_status = lv_label_create(navidrome_screen);
	lv_obj_add_style(entry_status, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(entry_status, &font_ui_14, 0);
	lv_obj_set_pos(entry_status, cfg->padding, settingsrow_content_top(cfg) - ui_px(8));
	lv_obj_set_size(entry_status, cfg->screen_width - 2 * cfg->padding, ui_px(20));
	lv_label_set_long_mode(entry_status, LV_LABEL_LONG_DOT);
	lv_obj_set_clickable(entry_status, true);
	lv_obj_add_event_cb(entry_status, entry_retry, LV_EVENT_CLICKED, NULL);
	switcher_attach_back_gesture(entry_status);
	entry_init();
	refresh();

	// The grid handles gestures over its tiles; the screen covers the header.
	switcher_attach_back_gesture(navidrome_screen);
	player_sheet_attach_drag(navidrome_screen, true);
}

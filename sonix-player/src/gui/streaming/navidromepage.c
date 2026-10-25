#include "navidromepage.h"
#include "navidromebrowse.h"
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
#include "src/system/streaming/navidrome.h"

lv_obj_t *navidrome_screen;
static lv_obj_t *category_grid, *search_icon;
#ifndef HOST_BUILD
static lv_timer_t *network_timer;
#endif

static void unavailable_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active()) {
		return;
	}

	const char *label = lv_event_get_user_data(e);
	navidrome_poll_network();
	navidrome_snapshot_t snapshot;
	navidrome_snapshot(&snapshot);
	if (snapshot.state != ND_CONNECTED) {
		gui_notify_popup(navidrome_feedback(&snapshot));
		return;
	}
	char message[160];
	lv_snprintf(message, sizeof(message), tr("grid_not_yet_available"), tr(label));
	gui_notify_popup(message);
}

static void tile_action(const char *label) {
	navidrome_poll_network();
	navidrome_snapshot_t s;
	navidrome_snapshot(&s);
	if (s.state != ND_CONNECTED) {
		gui_notify_popup(navidrome_feedback(&s));
		return;
	}
	char message[160];
	lv_snprintf(message, sizeof(message), tr("grid_not_yet_available"), tr(label));
	gui_notify_popup(message);
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
static void playlists_action(void) { tile_action("playlists"); }

static void settings_cb(lv_event_t *e) {
	(void)e;
	if (!player_sheet_drag_active() && !switcher_back_drag_active() && !coverflow_drag_active())
		navidromesettings_open();
}

static void refresh(void) {
	navidrome_snapshot_t snapshot;
	navidrome_snapshot(&snapshot);
	bool dim = snapshot.state != ND_CONNECTED;
	nd_library_t library;
	navidrome_library_snapshot(&library);
	for (int i = 0; i < 5; i++)
		gridpage_set_dimmed(category_grid, i, i == 4 ? dim : !library.ready);
	lv_obj_set_style_opa(search_icon, dim ? LV_OPA_40 : LV_OPA_COVER, 0);
	navidromesettings_refresh();
	navidromebrowse_refresh();
}

#ifndef HOST_BUILD
static void network_tick(lv_timer_t *timer) {
	(void)timer;
	navidrome_poll_network();
	refresh();
}
#endif

static void menu_loaded(lv_event_t *e) {
	(void)e;
	navidrome_poll_network();
	refresh();
#ifndef HOST_BUILD
	if (!network_timer)
		network_timer = lv_timer_create(network_tick, 1000, NULL);
#endif
}

static void menu_unloaded(lv_event_t *e) {
	(void)e;
#ifndef HOST_BUILD
	if (network_timer) {
		lv_timer_delete(network_timer);
		network_timer = NULL;
	}
#endif
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

	const grid_entry_t entries[] = {
		{"albums", &icon_menu_album, NULL, albums_action}, {"navidrome_tracks", &icon_menu_all, NULL, tracks_action}, {"artists", &icon_menu_artist, NULL, artists_action}, {"music_genres", &icon_menu_genre, NULL, genres_action}, {"playlists", &icon_menu_playlist, NULL, playlists_action},
	};
	category_grid = gridpage_build(navidrome_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3, true);
	settingsrow_title_corner_slots(settingsrow_title(navidrome_screen, cfg, "navidrome"), cfg, 2);
	corner_button(cfg, 0, &icon_music_settings, "settings");
	corner_button(cfg, 1, &icon_search, "navidrome_search");
	navidromesettings_init(cfg);
	navidromebrowse_init(cfg);
	navidrome_set_listener(refresh);
	lv_obj_add_event_cb(navidrome_screen, menu_loaded, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(navidrome_screen, menu_unloaded, LV_EVENT_SCREEN_UNLOADED, NULL);
	refresh();

	// The grid handles gestures over its tiles; the screen covers the header.
	switcher_attach_back_gesture(navidrome_screen);
	player_sheet_attach_drag(navidrome_screen, true);
}

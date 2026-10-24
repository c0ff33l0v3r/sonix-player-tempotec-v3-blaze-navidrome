#include "navidromepage.h"

#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/uiscale.h"
#include "src/system/core/lang.h"

lv_obj_t *navidrome_screen;

static void unavailable_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active()) {
		return;
	}

	const char *label = lv_event_get_user_data(e);
	char message[160];
	lv_snprintf(message, sizeof(message), tr("grid_not_yet_available"), tr(label));
	gui_notify_popup(message);
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

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
}

void navidromepage_init(gui_config_t *cfg) {
	navidrome_screen = lv_obj_create(NULL);

	// These categories will browse the server; for now the shared grid supplies
	// unavailable styling and feedback without opening the local Music library.
	const grid_entry_t entries[] = {
		{"albums", &icon_menu_album, NULL, NULL},
		{"navidrome_tracks", &icon_menu_all, NULL, NULL},
		{"artists", &icon_menu_artist, NULL, NULL},
		{"music_genres", &icon_menu_genre, NULL, NULL},
		{"playlists", &icon_menu_playlist, NULL, NULL},
	};
	gridpage_build(navidrome_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3, true);
	settingsrow_title_corner_slots(settingsrow_title(navidrome_screen, cfg, "navidrome"), cfg, 2);
	corner_button(cfg, 0, &icon_music_settings, "settings");
	corner_button(cfg, 1, &icon_search, "navidrome_search");

	// The grid handles gestures over its tiles; the screen covers the header.
	switcher_attach_back_gesture(navidrome_screen);
	player_sheet_attach_drag(navidrome_screen, true);
}

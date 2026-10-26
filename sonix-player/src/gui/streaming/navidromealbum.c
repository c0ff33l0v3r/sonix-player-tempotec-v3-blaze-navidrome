#include "navidromealbum.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/uiscale.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/streaming/navidrome.h"
#include <stdio.h>
#include <string.h>

static unsigned album_revision = 1;
static nd_album_view_t *album_views[6];
static bool album_theme_registered;
static void (*album_preference_refresh)(bool enabled);
static void album_theme_refresh(void);
static unsigned album_network_epoch(void) {
	nd_context_t context;
	unsigned epoch = navidrome_context(&context, true) ? context.network_epoch : 0;
	nd_wipe(&context, sizeof(context));
	return epoch;
}
bool navidromealbum_enabled(void) { return config_get_bool("navidrome", "album_head", true); }
unsigned navidromealbum_revision(void) { return album_revision; }
void navidromealbum_set_enabled(bool enabled) {
	if (enabled == navidromealbum_enabled())
		return;
	config_set_bool("navidrome", "album_head", enabled);
	config_save();
	album_revision++;
	if (album_preference_refresh)
		album_preference_refresh(enabled);
}
static void album_preference_changed(bool enabled) {
	for (int i = 0; i < 6; i++) {
		nd_album_view_t *view = album_views[i];
		if (view && view->active) {
			navidromealbum_release(view);
			if (!enabled)
				lv_obj_set_hidden(view->head, true);
		}
	}
}

static void album_color(lv_obj_t *object, lv_style_prop_t prop, bool on, lv_color_t value) {
	if (!object)
		return;
	if (on) {
		lv_style_value_t v = {.color = value};
		lv_obj_set_local_style_prop(object, prop, v, 0);
	} else
		lv_obj_remove_local_style_prop(object, prop, 0);
}
static void album_paint(nd_album_view_t *v) {
	album_color(v->screen, LV_STYLE_BG_COLOR, v->tinted, v->bg);
	album_color(v->page_title, LV_STYLE_TEXT_COLOR, v->tinted, v->ink);
	album_color(v->name, LV_STYLE_TEXT_COLOR, v->tinted, v->ink);
	album_color(v->artist, LV_STYLE_TEXT_COLOR, v->tinted, v->ink);
	album_color(v->meta, LV_STYLE_TEXT_COLOR, v->tinted, v->dim);
	album_color(v->status, LV_STYLE_TEXT_COLOR, v->tinted, v->dim);
	album_color(v->reason, LV_STYLE_TEXT_COLOR, v->tinted, v->dim);
	lv_obj_set_style_bg_color(v->mask, v->tinted ? v->bg : theme()->screen_bg, 0);
	for (int i = 0; i < 4; i++) {
		lv_obj_t *inside = lv_obj_get_child(v->controls[i], 0);
		if (i == ND_ALBUM_PLAY) {
			lv_obj_set_style_bg_color(v->controls[i], v->tinted ? v->ink : theme()->accent, 0);
			lv_obj_set_style_text_color(inside, v->tinted ? v->bg : lv_color_white(), 0);
		} else
			album_color(inside, LV_STYLE_IMAGE_RECOLOR, v->tinted, v->ink);
	}
	back_btn_tint_on_screen(v->screen, v->tinted, v->ink);
	lv_color_t bar = lv_color_darken(v->bg, LV_OPA_20);
	int luma = (bar.red * 299 + bar.green * 587 + bar.blue * 114) / 1000;
	topbar_tint_on_screen(v->screen, v->tinted, bar, luma >= COVER_PAGE_LIGHT_FROM ? lv_color_hex(0x161616) : lv_color_white());
}
static void album_tone(nd_album_view_t *v) {
	if (!v->image.has_tone)
		return;
	uint32_t tone = v->image.tone;
	int r = (tone >> 16) & 255, g = (tone >> 8) & 255, b = tone & 255;
	int now = (r * 299 + g * 587 + b * 114) / 1000;
	int want = v->image.tone_light ? LV_CLAMP(180, LV_MAX(v->image.tone_luma, now), 225) : LV_CLAMP(28, v->image.tone_luma * 3 / 4, 90);
	if (now > want && now) {
		r = r * want / now;
		g = g * want / now;
		b = b * want / now;
	} else if (now < want && now < 255) {
		int step = (want - now) * 255 / (255 - now);
		r += (255 - r) * step / 255;
		g += (255 - g) * step / 255;
		b += (255 - b) * step / 255;
	}
	int hi = LV_MAX(r, LV_MAX(g, b)), lo = LV_MIN(r, LV_MIN(g, b));
	if (hi && (hi - lo) * 256 > hi * 176) {
		int keep = 176 * hi, spread = (hi - lo) * 256;
		r = hi - (hi - r) * keep / spread;
		g = hi - (hi - g) * keep / spread;
		b = hi - (hi - b) * keep / spread;
	}
	v->bg = lv_color_make(r, g, b);
	v->ink = (r * 299 + g * 587 + b * 114) / 1000 >= COVER_PAGE_LIGHT_FROM ? lv_color_hex(0x161616) : lv_color_white();
	v->dim = lv_color_mix(v->ink, v->bg, 166);
	v->card = lv_color_mix(v->ink, v->bg, 22);
	v->tinted = true;
	album_paint(v);
}
static void album_pressed(lv_event_t *e) {
	nd_album_view_t *v = lv_event_get_user_data(e);
	if (!v->active || v->revision != album_revision || !navidromealbum_enabled() || !v->action || switcher_back_drag_active() || player_sheet_drag_active() || coverflow_drag_active())
		return;
	for (int i = 0; i < 4; i++)
		if (lv_event_get_current_target(e) == v->controls[i])
			v->action(v->user, (nd_album_action_t)i);
}
static lv_obj_t *album_line(lv_obj_t *parent, const lv_font_t *font) {
	lv_obj_t *label = lv_label_create(parent);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_clickable(label, false);
	return label;
}
void navidromealbum_init(nd_album_view_t *v, gui_config_t *cfg, lv_obj_t *screen, lv_obj_t *list, lv_obj_t *title, lv_obj_t *status, lv_obj_t *reason, int slot, nd_album_action_cb action, void *user) {
	album_preference_refresh = album_preference_changed;
	for (int i = 0; i < 6; i++) {
		if (album_views[i] == v)
			break;
		if (!album_views[i]) {
			album_views[i] = v;
			break;
		}
	}
	if (!album_theme_registered) {
		theme_register_refresh(album_theme_refresh);
		album_theme_registered = true;
	}
	v->cfg = cfg;
	v->screen = screen;
	v->list = list;
	v->page_title = title;
	v->status = status;
	v->reason = reason;
	v->slot = slot;
	v->action = action;
	v->user = user;
	v->head = lv_obj_create(list);
	lv_obj_remove_style_all(v->head);
	lv_obj_set_size(v->head, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(v->head, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(v->head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(v->head, ui_px(6), 0);
	lv_obj_set_style_pad_top(v->head, cfg->padding + ui_px(12), 0);
	lv_obj_set_style_pad_bottom(v->head, ui_px(24), 0);
	lv_obj_set_scrollable(v->head, false);
	lv_obj_set_clickable(v->head, false);
	lv_obj_set_event_bubble(v->head, true);
	v->cover = lv_image_create(v->head);
	int side = LV_MIN(ui_px(300), (int)cfg->screen_width - 2 * (int)cfg->padding);
	lv_obj_set_size(v->cover, side, side);
	lv_obj_add_style(v->cover, &theme_style_card, 0);
	lv_obj_add_style(v->cover, &theme_style_icon, 0);
	// Like Music, keep the small glyph native inside the fixed cover box.
	// Loaded artwork switches to CONTAIN below without inheriting its scale.
	lv_image_set_inner_align(v->cover, LV_IMAGE_ALIGN_CONTAIN_DOWNSCALE);
	lv_image_set_src(v->cover, &icon_album_big);
	lv_obj_set_clickable(v->cover, false);
	v->name = album_line(v->head, &font_ui_28);
	v->artist = album_line(v->head, &font_ui_24);
	v->meta = album_line(v->head, &font_ui_20);
	v->buttons = lv_obj_create(v->head);
	lv_obj_remove_style_all(v->buttons);
	lv_obj_set_size(v->buttons, LV_SIZE_CONTENT, ui_px(56));
	lv_obj_set_flex_flow(v->buttons, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(v->buttons, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(v->buttons, ui_px(20), 0);
	lv_obj_set_style_margin_top(v->buttons, ui_px(12), 0);
	lv_obj_set_scrollable(v->buttons, false);
	lv_obj_set_clickable(v->buttons, false);
	lv_obj_set_event_bubble(v->buttons, true);
	for (int i = 0; i < 4; i++) {
		lv_obj_t *button = v->controls[i] = lv_btn_create(v->buttons);
		lv_obj_set_size(button, i == ND_ALBUM_PLAY ? LV_SIZE_CONTENT : ui_px(56), ui_px(56));
		lv_obj_set_style_border_width(button, 0, 0);
		lv_obj_set_style_shadow_width(button, 0, 0);
		lv_obj_set_style_pad_all(button, 0, 0);
		lv_obj_set_style_bg_opa(button, i == ND_ALBUM_PLAY ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_opa(button, i == ND_ALBUM_PLAY ? LV_OPA_70 : LV_OPA_50, LV_STATE_PRESSED);
		lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
		lv_obj_set_event_bubble(button, true);
		lv_obj_add_event_cb(button, album_pressed, LV_EVENT_CLICKED, v);
		if (i == ND_ALBUM_PLAY) {
			lv_obj_set_style_pad_hor(button, ui_px(40), 0);
			int max_width = (int)cfg->screen_width - 2 * (int)cfg->padding - 3 * ui_px(56) - 3 * ui_px(20);
			lv_obj_set_style_max_width(button, max_width, 0);
			lv_obj_t *text = lv_label_create(button);
			lv_label_set_text(text, tr("medialist_play"));
			lv_obj_set_style_text_font(text, &font_ui_24_bold, 0);
			lv_obj_set_style_max_width(text, LV_MAX(ui_px(16), max_width - 2 * ui_px(40)), 0);
			lv_obj_set_height(text, lv_font_get_line_height(&font_ui_24_bold));
			lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
			lv_obj_center(text);
		} else {
			lv_obj_t *image = lv_image_create(button);
			lv_image_set_src(image, i == ND_ALBUM_SHUFFLE ? &icon_shuffle : i == ND_ALBUM_DOWNLOAD ? &icon_download : &icon_list_queue);
			lv_obj_add_style(image, &theme_style_icon, 0);
			lv_obj_center(image);
		}
		switcher_attach_back_gesture(button);
		player_sheet_attach_drag(button, true);
	}
	v->mask = lv_obj_create(screen);
	lv_obj_remove_style_all(v->mask);
	lv_obj_set_pos(v->mask, 0, cfg->top_bar_height);
	lv_obj_set_size(v->mask, cfg->screen_width, settingsrow_content_top(cfg) - cfg->top_bar_height);
	lv_obj_set_style_bg_opa(v->mask, LV_OPA_COVER, 0);
	lv_obj_set_clickable(v->mask, false);
	lv_obj_set_scrollable(v->mask, false);
	lv_obj_set_hidden(v->mask, true);
	lv_obj_move_foreground(title);
	lv_obj_set_hidden(v->head, true);
}
void navidromealbum_release(nd_album_view_t *v) {
	if (!v->head)
		return;
	navidromeart_release(v->slot);
	lv_image_set_inner_align(v->cover, LV_IMAGE_ALIGN_CONTAIN_DOWNSCALE);
	lv_image_set_src(v->cover, &icon_album_big);
	lv_obj_add_style(v->cover, &theme_style_icon, 0);
	cover_free(&v->image);
	v->art_bound = false;
	v->art_done = true;
	v->art_network_epoch = 0;
	v->tinted = false;
	album_paint(v);
	lv_obj_set_hidden(v->mask, true);
}
void navidromealbum_bind(nd_album_view_t *v, bool active, const nd_playrow_t *info, const nd_library_t *lib, bool controls, bool available) {
	bool same = active && v->active && (!*info->row.cover || v->art_bound) && !strcmp(v->info.row.id, info->row.id) && !strcmp(v->info.row.cover, info->row.cover) && v->library.epoch == lib->epoch && v->library.card_epoch == lib->card_epoch && v->library.generation == lib->generation;
	if (!same && (v->active || v->art_bound || v->image.pixels))
		navidromealbum_release(v);
	v->active = active;
	v->revision = navidromealbum_revision();
	lv_obj_set_hidden(v->head, !active);
	if (!active) {
		v->height = 0;
		if (lv_obj_get_parent(v->status) != v->screen)
			lv_obj_set_parent(v->status, v->screen);
		if (v->reason && lv_obj_get_parent(v->reason) != v->screen)
			lv_obj_set_parent(v->reason, v->screen);
		return;
	}
	v->info = *info;
	v->library = *lib;
	lv_label_set_text(v->name, info->row.title);
	lv_point_t size;
	lv_text_get_size(&size, info->row.title, &font_ui_28, 0, 0, (int)v->cfg->screen_width - 2 * (int)v->cfg->padding, LV_TEXT_FLAG_NONE);
	lv_obj_set_height(v->name, LV_MIN(size.y, 2 * lv_font_get_line_height(&font_ui_28)));
	lv_label_set_text(v->artist, info->row.artist);
	lv_obj_set_height(v->artist, lv_font_get_line_height(&font_ui_24));
	lv_obj_set_hidden(v->artist, !*info->row.artist);
	char meta[ND_TEXT_MAX + 24];
	if (info->year > 0)
		snprintf(meta, sizeof(meta), "%s%s%d", info->genre, *info->genre ? " · " : "", info->year);
	else
		snprintf(meta, sizeof(meta), "%s", info->genre);
	lv_label_set_text(v->meta, meta);
	lv_obj_set_height(v->meta, lv_font_get_line_height(&font_ui_20));
	lv_obj_set_hidden(v->meta, !*meta);
	lv_obj_set_hidden(v->buttons, !controls);
	for (int i = 0; i < 4; i++)
		lv_obj_set_state(v->controls[i], LV_STATE_DISABLED, !available);
	lv_obj_set_parent(v->status, v->head);
	lv_obj_set_hidden(v->status, !*lv_label_get_text(v->status));
	if (v->reason) {
		lv_obj_set_parent(v->reason, v->head);
		lv_obj_set_hidden(v->reason, !*lv_label_get_text(v->reason));
	}
	lv_obj_update_layout(v->head);
	v->height = lv_obj_get_height(v->head);
	v->title_y = lv_obj_get_y(v->name);
	if (!same && *info->row.cover) {
		v->art_bound = true;
		v->art_done = false;
		v->art_network_epoch = album_network_epoch();
		navidromeart_request(v->slot, lib, info->row.cover, LV_MIN(ui_px(300), (int)v->cfg->screen_width - 2 * (int)v->cfg->padding));
	}
	album_paint(v);
	navidromealbum_tick(v);
}
void navidromealbum_tick(nd_album_view_t *v) {
	if (!v->active || v->revision != album_revision)
		return;
	unsigned network_epoch = album_network_epoch();
	if (v->art_bound && v->art_done && network_epoch && network_epoch != v->art_network_epoch) {
		v->art_network_epoch = network_epoch;
		v->art_done = false;
		navidromeart_request(v->slot, &v->library, v->info.row.cover, LV_MIN(ui_px(300), (int)v->cfg->screen_width - 2 * (int)v->cfg->padding));
	}
	cover_image_t replacement = {0};
	if (!v->art_done && navidromeart_take(v->slot, &replacement, &v->art_done)) {
		// A reconnect upgrade keeps the previous image until its replacement
		// is ready. Detach the descriptor before freeing its old pixel buffer.
		lv_image_set_src(v->cover, NULL);
		cover_free(&v->image);
		v->image = replacement;
		lv_obj_remove_style(v->cover, &theme_style_icon, 0);
		lv_image_set_inner_align(v->cover, LV_IMAGE_ALIGN_CONTAIN);
		lv_image_set_src(v->cover, &v->image.dsc);
		album_tone(v);
	}
	bool followed = lv_obj_get_scroll_y(v->list) > v->title_y - ui_px(48);
	lv_label_set_text(v->page_title, followed ? v->info.row.title : "");
	lv_obj_set_hidden(v->mask, !followed);
}
void navidromealbum_row(nd_album_view_t *v, lv_obj_t *row, lv_obj_t *name, lv_obj_t *detail, lv_obj_t *number, lv_obj_t *image, const nd_row_t *data, bool heading) {
	lv_obj_set_hidden(number, !v->active || heading);
	if (v->active) {
		lv_obj_set_hidden(image, true);
		char text[32];
		if (data->track > 0)
			snprintf(text, sizeof(text), "%d", data->track);
		else
			snprintf(text, sizeof(text), "—");
		lv_label_set_text(number, text);
	}
	lv_obj_set_clickable(row, !heading);
	lv_obj_set_height(row, heading ? ui_px(44) : ui_px(100));
	lv_obj_set_style_bg_opa(row, heading ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
	// Match Music's centered 44-pixel number column plus 14-pixel text gap.
	lv_obj_set_x(name, ui_px(v->active ? 58 : 86));
	lv_obj_set_x(detail, ui_px(v->active ? 58 : 86));
	lv_obj_set_width(name, (int)v->cfg->screen_width - 2 * (int)v->cfg->padding - ui_px(v->active ? 140 : 168));
	lv_obj_set_width(detail, lv_obj_get_width(name));
	lv_obj_set_hidden(detail, heading);
	if (heading) {
		char text[64];
		snprintf(text, sizeof(text), "%s %d", tr("medialist_disc"), data->disc > 0 ? data->disc : 1);
		lv_label_set_text(name, text);
		lv_obj_set_x(name, 0);
	}
	album_color(row, LV_STYLE_BG_COLOR, v->tinted, v->card);
	if (v->tinted)
		lv_obj_set_style_bg_color(row, lv_color_mix(v->ink, v->bg, 48), LV_STATE_PRESSED);
	else
		lv_obj_remove_local_style_prop(row, LV_STYLE_BG_COLOR, LV_STATE_PRESSED);
	album_color(name, LV_STYLE_TEXT_COLOR, v->tinted, v->ink);
	album_color(detail, LV_STYLE_TEXT_COLOR, v->tinted, v->dim);
	album_color(number, LV_STYLE_TEXT_COLOR, v->tinted, v->dim);
}

static void album_theme_refresh(void) {
	for (int i = 0; i < 6; i++)
		if (album_views[i] && album_views[i]->active)
			album_paint(album_views[i]);
}

void navidromealbum_icon(nd_album_view_t *v, lv_obj_t *image) { album_color(image, LV_STYLE_IMAGE_RECOLOR, v->tinted, v->ink); }

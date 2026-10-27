#ifndef NAVIDROMEROW_H
#define NAVIDROMEROW_H

#include "lvgl/lvgl.h"
#include "navidromeart.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/uiscale.h"
#include "src/system/streaming/navidromestore.h"
#include <stdio.h>
#include <string.h>

// Permute a fixed pool without reallocating widgets or restarting owned jobs.
// map[new logical index] is the previous index; every old index occurs once.
#define ND_ROW_REORDER(array, map) do { \
	__typeof__((array)[0]) saved[ND_WINDOW]; \
	memcpy(saved, (array), sizeof(saved)); \
	for (int row_index = 0; row_index < ND_WINDOW; row_index++) \
		(array)[row_index] = saved[(map)[row_index]]; \
} while (0)

static inline void nd_row_complete_map(int map[ND_WINDOW]) {
	bool used[ND_WINDOW] = {0};
	for (int i = 0; i < ND_WINDOW; i++)
		if (map[i] >= 0)
			used[map[i]] = true;
	int next = 0;
	for (int i = 0; i < ND_WINDOW; i++)
		if (map[i] < 0) {
			while (used[next]) next++;
			map[i] = next;
			used[next] = true;
		}
}

// Elastic movement is visual only. Metadata follows the real content viewport.
static inline int nd_row_scroll(lv_obj_t *list, int extent) {
	int maximum = LV_MAX(0, extent - lv_obj_get_content_height(list));
	return LV_CLAMP(0, lv_obj_get_scroll_y(list), maximum);
}
static inline int nd_row_first(lv_obj_t *list, int total, int pitch, int offset) {
	int y = nd_row_scroll(list, offset + total * pitch) - offset;
	return LV_CLAMP(0, y / pitch - 2, LV_MAX(0, total - ND_WINDOW));
}
static inline bool nd_row_covered(lv_obj_t *list, int first, int count, int total, int pitch, int offset) {
	int y = LV_MAX(0, nd_row_scroll(list, offset + total * pitch) - offset);
	int top = y / pitch;
	int end = LV_MIN(total, (y + lv_obj_get_content_height(list) + pitch - 1) / pitch);
	return top >= first && end <= first + count;
}

static inline lv_obj_t *nd_row_duration_create(lv_obj_t *detail) {
	lv_obj_t *duration = lv_label_create(lv_obj_get_parent(detail));
	lv_obj_add_style(duration, &theme_style_text_dim, 0);
	lv_obj_set_floating(duration, true);
	lv_obj_set_clickable(duration, false);
	lv_obj_set_hidden(duration, true);
	return duration;
}
// Library, Search and Discovery share one ordinary/numbered row construction.
// Callers attach their own actions and bounded artwork slots.
typedef struct {
	lv_obj_t *row, *image, *name, *detail, *duration, *number;
} nd_row_widgets_t;
static inline nd_row_widgets_t nd_row_create(lv_obj_t *list, const gui_config_t *cfg) {
	nd_row_widgets_t w = {0};
	w.row = lv_btn_create(list);
	lv_obj_set_size(w.row, lv_pct(100), ui_px(100));
	lv_obj_add_style(w.row, &theme_style_card, 0);
	lv_obj_add_style(w.row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_shadow_width(w.row, 0, 0);
	lv_obj_set_style_pad_all(w.row, ui_px(14), 0);
	lv_obj_set_scrollable(w.row, false);
	w.image = lv_image_create(w.row);
	lv_obj_set_size(w.image, ui_px(ND_ART_THUMB_SIZE), ui_px(ND_ART_THUMB_SIZE));
	lv_image_set_inner_align(w.image, LV_IMAGE_ALIGN_CONTAIN);
	lv_obj_align(w.image, LV_ALIGN_LEFT_MID, 0, 0);
	w.name = lv_label_create(w.row);
	w.detail = lv_label_create(w.row);
	w.duration = nd_row_duration_create(w.detail);
	lv_obj_t *text[] = {w.name, w.detail};
	for (int j = 0; j < 2; j++) {
		const lv_font_t *font = j ? &font_ui_18 : &font_ui_20;
		lv_obj_add_style(text[j], j ? &theme_style_text_dim : &theme_style_text, 0);
		lv_obj_set_style_text_font(text[j], font, 0);
		lv_obj_set_pos(text[j], ui_px(86), ui_px(j ? 39 : 8));
		lv_obj_set_width(text[j], cfg->screen_width - 2 * cfg->padding - ui_px(168));
		lv_obj_set_height(text[j], lv_font_get_line_height(font));
		lv_label_set_long_mode(text[j], LV_LABEL_LONG_DOT);
	}
	w.number = lv_label_create(w.row);
	lv_obj_add_style(w.number, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(w.number, &font_ui_20, 0);
	lv_obj_set_width(w.number, ui_px(44));
	lv_obj_set_style_text_align(w.number, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(w.number, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_set_hidden(w.number, true);
	lv_obj_set_hidden(w.row, true);
	lv_obj_set_hidden(w.image, true);
	return w;
}
static inline lv_obj_t *nd_row_menu_create(lv_obj_t *row) {
	lv_obj_t *button = lv_btn_create(row);
	lv_obj_set_size(button, ui_px(40), ui_px(52));
	lv_obj_align(button, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_t *glyph = lv_image_create(button);
	lv_image_set_src(glyph, &icon_ellipsis_vertical);
	lv_obj_add_style(glyph, &theme_style_icon, 0);
	lv_obj_center(glyph);
	return button;
}
// Call after the row's full text width and baseline have been laid out, before
// offline badges reserve their own separate slice of the remaining artist line.
static inline void nd_row_footer(lv_obj_t *detail, lv_obj_t *duration, int width, int seconds, bool heading) {
	const lv_font_t *font = lv_obj_get_style_text_font(detail, 0);
	lv_obj_set_style_text_font(duration, font, 0);
	lv_obj_set_style_text_color(duration, lv_obj_get_style_text_color(detail, 0), 0);
	bool shown = seconds > 0 && !heading;
	lv_obj_set_hidden(duration, !shown);
	int reserved = 0;
	if (shown) {
		char text[32];
		snprintf(text, sizeof(text), "%d:%02d", seconds / 60, seconds % 60);
		lv_label_set_text(duration, text);
		lv_point_t size;
		lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		lv_obj_set_size(duration, size.x, lv_font_get_line_height(font));
		lv_obj_set_pos(duration, lv_obj_get_x(detail) + width - size.x, lv_obj_get_y(detail));
		reserved = size.x + ui_px(8);
	}
	lv_obj_set_width(detail, LV_MAX(1, width - reserved));
}

static inline void nd_row_title(lv_obj_t *title, gui_config_t *cfg, lv_obj_t *const *controls, int count) {
	// Refit the current text; initially created and dynamically replaced names
	// must use the same fitting policy as normal Music.
	settingsrow_title_corner_slots(title, cfg, 0);
	lv_obj_update_layout(title);
	int right = (int)cfg->screen_width - (int)cfg->padding;
	for (int i = 0; i < count; i++)
		if (controls[i] && !lv_obj_is_hidden(controls[i]))
			right = LV_MIN(right, lv_obj_get_x(controls[i]) - ui_px(14));
	settingsrow_title_fit_width(title, LV_MAX(1, right - lv_obj_get_x(title)));
}
#endif

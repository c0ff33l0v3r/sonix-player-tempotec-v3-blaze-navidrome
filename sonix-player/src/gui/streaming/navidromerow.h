#ifndef NAVIDROMEROW_H
#define NAVIDROMEROW_H

#include "lvgl/lvgl.h"
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

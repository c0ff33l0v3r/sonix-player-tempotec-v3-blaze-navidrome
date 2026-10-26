#ifndef PLAYMARK_H
#define PLAYMARK_H

#include "theme.h"
#include "uiscale.h"

// Match Music's inset, vertically centered current-track pill.
static inline lv_obj_t *playmark_create(lv_obj_t *row) {
	lv_obj_t *mark = lv_obj_create(row);
	lv_obj_remove_style_all(mark);
	lv_obj_set_ignore_layout(mark, true);
	lv_obj_set_size(mark, ui_px(6), ui_px(52));
	lv_obj_align(mark, LV_ALIGN_LEFT_MID, ui_px(4) - lv_obj_get_style_pad_left(row, 0), 0);
	lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(mark, theme()->accent, 0);
	lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(mark, false);
	lv_obj_set_clickable(mark, false);
	lv_obj_set_hidden(mark, true);
	return mark;
}

static inline void playmark_show(lv_obj_t *mark, bool visible) {
	lv_obj_set_style_bg_color(mark, theme()->accent, 0);
	lv_obj_set_hidden(mark, !visible);
}

#endif

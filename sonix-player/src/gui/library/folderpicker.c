#include "folderpicker.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/uiscale.h"
#include "src/system/core/lang.h"

static lv_obj_t *corner(lv_obj_t *parent, gui_config_t *cfg, const lv_image_dsc_t *icon, int slot, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(parent);
	lv_obj_set_size(button, ui_px(56), ui_px(56));
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * ui_px(56), cfg->padding + cfg->top_bar_height);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_set_event_bubble(button, true);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *glyph = lv_image_create(button);
	lv_image_set_src(glyph, icon);
	lv_obj_add_style(glyph, &theme_style_icon, 0);
	lv_obj_center(glyph);
	return button;
}
folderpicker_t folderpicker_create(lv_obj_t *parent, gui_config_t *cfg, const char *title, const char *use, lv_event_cb_t confirm, lv_event_cb_t create, lv_event_cb_t cancel) {
	folderpicker_t view = {0};
	view.root = lv_obj_create(parent);
	lv_obj_remove_style_all(view.root);
	lv_obj_add_style(view.root, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(view.root, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(view.root, 0, 0);
	lv_obj_set_size(view.root, cfg->screen_width, cfg->screen_height);
	lv_obj_set_scrollable(view.root, false);
	switcher_attach_back_gesture(view.root);
	view.title = settingsrow_title(view.root, cfg, title);
	settingsrow_title_corner_slots(view.title, cfg, !!create + !!cancel);
	if (cancel)
		corner(view.root, cfg, &icon_close, 0, cancel);
	if (create)
		view.new_folder = corner(view.root, cfg, &icon_folder_new, !!cancel, create);
	view.use = lv_btn_create(view.root);
	lv_obj_set_size(view.use, cfg->screen_width - 2 * cfg->padding, ui_px(64));
	lv_obj_align(view.use, LV_ALIGN_BOTTOM_MID, 0, -cfg->padding);
	lv_obj_set_style_radius(view.use, ui_px(14), 0);
	lv_obj_set_style_bg_color(view.use, theme()->accent, 0);
	lv_obj_set_style_border_width(view.use, 0, 0);
	lv_obj_set_style_shadow_width(view.use, 0, 0);
	lv_obj_set_event_bubble(view.use, true);
	lv_obj_add_event_cb(view.use, confirm, LV_EVENT_CLICKED, NULL);
	view.use_label = lv_label_create(view.use);
	lv_label_set_text(view.use_label, tr(use));
	lv_obj_set_style_text_color(view.use_label, lv_color_white(), 0);
	lv_obj_set_style_text_font(view.use_label, &font_ui_24, 0);
	lv_obj_set_width(view.use_label, lv_pct(100));
	lv_label_set_long_mode(view.use_label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_align(view.use_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_center(view.use_label);
	int top = settingsrow_content_top(cfg);
	view.list = lv_obj_create(view.root);
	lv_obj_remove_style_all(view.list);
	lv_obj_set_size(view.list, cfg->screen_width, cfg->screen_height - top - ui_px(64) - 2 * cfg->padding);
	lv_obj_set_pos(view.list, 0, top);
	lv_obj_set_style_pad_hor(view.list, cfg->padding, 0);
	lv_obj_set_style_pad_gap(view.list, ui_px(8), 0);
	lv_obj_set_event_bubble(view.list, true);
	lv_obj_set_scroll_dir(view.list, LV_DIR_VER);
	lv_obj_set_flex_flow(view.list, LV_FLEX_FLOW_COLUMN);
	return view;
}
lv_obj_t *folderpicker_row(lv_obj_t *list, const char *name, lv_event_cb_t click, void *user) {
	lv_obj_t *row = lv_btn_create(list);
	lv_obj_set_size(row, lv_pct(100), ui_px(76));
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, ui_px(12), 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, ui_px(16), 0);
	lv_obj_set_style_pad_column(row, ui_px(14), 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(row, true);
	lv_obj_add_event_cb(row, click, LV_EVENT_CLICKED, user);
	lv_obj_t *glyph = lv_image_create(row);
	lv_image_set_src(glyph, &icon_folder);
	lv_obj_add_style(glyph, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor(glyph, theme()->accent, 0);
	lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text(label, name);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(label, 1);
	lv_obj_set_height(label, ui_px(32));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	return row;
}
lv_obj_t *folderpicker_name_label(lv_obj_t *row) { return lv_obj_get_child(row, 1); }

// Production popover and native check glyph, using a deterministic test font.
#include "src/gui/shell/popover.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
int32_t ui_panel_width;
lv_font_t font_ui_24, font_ui_14;
extern const lv_font_t lv_font_unscii_8;
lv_style_t theme_style_card, theme_style_card_pressed, theme_style_text;
static theme_palette_t palette;
const theme_palette_t *theme(void) { return &palette; }
const char *tr(const char *key) { return key; }
static int chosen = -1;
static void choose(void *value) { chosen = (int)(intptr_t)value; }
static lv_indev_data_t touch;
static void read_touch(lv_indev_t *input, lv_indev_data_t *data) {
	(void)input;
	*data = touch;
}
int main(int argc, char **argv) {
	assert(argc == 2);
	ui_panel_width = atoi(argv[1]);
	lv_init();
	font_ui_24 = font_ui_14 = lv_font_unscii_8;
	int height = ui_panel_width == 320 ? 480 : 800;
	lv_display_t *display = lv_display_create(ui_panel_width, height);
	lv_obj_t *anchor = lv_button_create(lv_screen_active());
	lv_obj_set_size(anchor, ui_px(56), ui_px(56));
	lv_obj_align(anchor, LV_ALIGN_TOP_RIGHT, -8, 20);
	lv_style_init(&theme_style_card);
	lv_style_init(&theme_style_card_pressed);
	lv_style_init(&theme_style_text);
	popover_init();
	lv_obj_update_layout(lv_screen_active());
	popover_item_t items[9];
	for (int i = 0; i < 9; i++)
		items[i] = (popover_item_t){"An intentionally long translated album artist sorting option which wraps across several lines", choose, (void *)(intptr_t)i, i == 8};
	popover_show(anchor, items, 9);
	lv_obj_update_layout(veil);
	assert(items_count == 9 && lv_obj_get_child_count(card) == 17 && !lv_obj_is_hidden(veil));
	lv_area_t bounds;
	lv_obj_get_coords(card, &bounds);
	assert(bounds.x1 >= 0 && bounds.x2 < ui_panel_width && bounds.y1 >= 0 && bounds.y2 < height && bounds.y2 > bounds.y1);
	if (ui_panel_width == 320)
		assert(lv_obj_get_scroll_bottom(card) > 0);
	lv_obj_t *last = lv_obj_get_child(card, 16);
	lv_obj_scroll_to_view(last, LV_ANIM_OFF);
	lv_obj_update_layout(veil);
	lv_area_t last_bounds, check, label;
	lv_obj_get_coords(last, &last_bounds);
	lv_obj_get_coords(lv_obj_get_child(last, 0), &label);
	lv_obj_get_coords(lv_obj_get_child(last, 1), &check);
	assert(icon_check.header.w > 1 && label.x2 < check.x1 && check.x2 <= last_bounds.x2);
	assert(last_bounds.y1 >= bounds.y1 && last_bounds.y2 <= bounds.y2);
	lv_indev_t *pointer = lv_indev_create();
	lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
	lv_indev_set_display(pointer, display);
	lv_indev_set_read_cb(pointer, read_touch);
	touch.point = (lv_point_t){last_bounds.x1 + ui_px(16), last_bounds.y1 + ui_px(16)};
	touch.state = LV_INDEV_STATE_PRESSED;
	lv_indev_read(pointer);
	touch.state = LV_INDEV_STATE_RELEASED;
	lv_indev_read(pointer);
	assert(chosen == 8 && lv_obj_is_hidden(veil));
	popover_show(anchor, items, 4);
	lv_obj_update_layout(veil);
	assert(items_count == 4 && lv_obj_get_scroll_y(card) == 0);
	lv_indev_delete(pointer);
	lv_display_delete(display);
	lv_deinit();
	printf("native nine-choice popover %dx%d: long labels, check alignment, bounded card, final-choice scroll/pointer selection and reopen reset passed\n", ui_panel_width, height);
}

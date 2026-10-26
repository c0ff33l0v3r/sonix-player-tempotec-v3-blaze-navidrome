// Production coordinator calls routed through the actual LVGL busy-toast veil.
#define NAVIDROME_PREPARATION_UI
#include "navidrome_coordinatorcheck.c"

#define toast_init probe_toast_init
#define toast_success probe_toast_success
#define toast_error probe_toast_error
#define toast_plain probe_toast_plain
#define toast_glyph probe_toast_glyph
#define toast_busy probe_toast_busy
#define toast_busy_end probe_toast_busy_end
#define toast_busy_dismissable probe_toast_busy_dismissable
#define toast_busy_showing probe_toast_busy_showing
#include "src/gui/shell/toast.c"
#include "src/gui/shell/spinner.c"

void animpark_watch(lv_obj_t *obj, animpark_cb_t park, animpark_cb_t unpark) { (void)obj; (void)park; (void)unpark; }
static unsigned preparation_veils, preparation_dismissals;
void preparation_ui_init(void) {
	font_ui_24 = lv_font_unscii_8;
	gui_config_t config = {.screen_width = ui_panel_width, .screen_height = ui_panel_width == 320 ? 480 : 800};
	probe_toast_init(&config);
}
void preparation_busy_begin(const char *key) {
	probe_toast_busy_dismissable(key);
	assert(!lv_obj_is_hidden(veil) && lv_obj_get_style_bg_opa(veil, 0) == LV_OPA_60);
	lv_draw_buf_t *frame = lv_snapshot_take(veil, LV_COLOR_FORMAT_ARGB8888);
	assert(frame && frame->data[3] > 128 && frame->data[3] < 180);
	lv_draw_buf_destroy(frame);
	preparation_veils++;
}
void preparation_busy_end(void) {
	preparation_dismissals++;
	probe_toast_busy_end();
}
void preparation_ui_check(void) {
	if (getenv("SONIX_EXPECT_PREPARATION_FLASH")) {
		assert(preparation_veils && preparation_dismissals);
		puts("baseline reproduced: production preparation shows and removes a real 60%-black full-screen LVGL veil");
	} else {
		assert(!preparation_veils && !preparation_dismissals && lv_obj_is_hidden(veil));
		puts("real LVGL preparation: delayed/ready/failed/skipped/paused coordinator paths never show a veil or dismiss unrelated feedback: passed");
	}
}

#include "src/gui/shell/doubletap.h"

#include "src/gui/shell/switcher.h"
#include "src/system/core/config.h"
#include "src/system/device/power.h"
#include "src/gui/shell/uiscale.h"

#define DOUBLE_TAP_MS 400 // between the two taps
#define TAP_MAX_MS 300	  // a press held longer is not a tap
#define TAP_SLOP_PX ui_px(20)	  // movement within a tap
#define PAIR_SLOP_PX ui_px(60)	  // distance between the two taps

static void doubletap_cb(lv_event_t *e) {
	static lv_point_t down;
	static uint32_t down_at;
	static lv_point_t last;
	static uint32_t last_at; // 0 = no first tap pending

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}
	lv_point_t p;
	lv_indev_get_point(indev, &p);

	if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
		down = p;
		down_at = lv_tick_get();
		return;
	}

	bool tap = LV_ABS(p.x - down.x) < TAP_SLOP_PX && LV_ABS(p.y - down.y) < TAP_SLOP_PX &&
			   lv_tick_elaps(down_at) < TAP_MAX_MS && !switcher_back_drag_active();
	if (!tap || !config_get_bool("screen", "double_tap_sleep", false)) {
		last_at = 0;
		return;
	}
	if (last_at && lv_tick_elaps(last_at) < DOUBLE_TAP_MS && LV_ABS(p.x - last.x) < PAIR_SLOP_PX &&
		LV_ABS(p.y - last.y) < PAIR_SLOP_PX) {
		last_at = 0; // a third tap starts a new pair
		power_screen_off();
		return;
	}
	last = p;
	last_at = lv_tick_get();
}

void doubletap_sleep_attach(lv_obj_t *obj) {
	lv_obj_add_event_cb(obj, doubletap_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(obj, doubletap_cb, LV_EVENT_RELEASED, NULL);
}

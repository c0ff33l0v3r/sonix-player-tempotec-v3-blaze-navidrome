#ifndef NAVIDROMEOFFLINECOLOR_H
#define NAVIDROMEOFFLINECOLOR_H
#include "lvgl/lvgl.h"
#include "src/system/streaming/navidromeoffline.h"

static inline lv_color_t nd_offline_color(nd_off_state_t state, lv_color_t background) {
	bool light = (background.red * 299 + background.green * 587 + background.blue * 114) / 1000 > 150;
	return lv_color_hex(state == ND_OFF_COMPLETE ? (light ? 0x147a32 : 0x58cf78) : (light ? 0x666666 : 0xaaaaaa));
}
#endif

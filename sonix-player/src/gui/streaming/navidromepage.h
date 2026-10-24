#ifndef NAVIDROMEPAGE_H
#define NAVIDROMEPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Navidrome category menu, opened from Streaming.
extern lv_obj_t *navidrome_screen;

void navidromepage_init(gui_config_t *cfg);

#endif /* NAVIDROMEPAGE_H */

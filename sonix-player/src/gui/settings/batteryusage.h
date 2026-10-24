#ifndef BATTERYUSAGE_H
#define BATTERYUSAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The "Battery usage" page under Power: the last 24 hours of battery level and
// of screen on / standby / sleep as bars, and under them what used the battery
// and for how long (batterystats.c). Read on every open, like the processes
// page, so nothing runs behind it while nobody looks.
extern lv_obj_t *batteryusage_screen;

void batteryusage_init(gui_config_t *cfg);

#endif /* BATTERYUSAGE_H */

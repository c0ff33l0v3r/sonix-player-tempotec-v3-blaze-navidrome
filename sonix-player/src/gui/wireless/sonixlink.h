#ifndef GUI_SONIXLINK_H
#define GUI_SONIXLINK_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The SonixLink page: one switch, and the address the app should be pointed at
// when it cannot find the player by itself. The service lives in
// src/system/sonixlink.c; this is the page and the pump that carries the
// player's state out to it and the app's commands back in.

extern lv_obj_t *sonixlink_screen;

void sonixlink_page_init(gui_config_t *cfg);

// The pump that carries state and commands sleeps while the service is off.
// Call after sonixlink_set_enabled() from anywhere but this page, so a switch
// turned on elsewhere wakes it.
void sonixlink_pump_pace(void);

#endif /* GUI_SONIXLINK_H */

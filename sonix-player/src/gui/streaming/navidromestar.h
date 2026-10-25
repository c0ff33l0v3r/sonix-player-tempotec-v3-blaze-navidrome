#ifndef NAVIDROMESTAR_H
#define NAVIDROMESTAR_H
#include "src/gui/shell/gui.h"

// Return true when the represented source is Navidrome. Other sources retain
// their existing controls. Call only from the GUI thread.
bool navidromestar_player_update(lv_obj_t *button, lv_obj_t *icon, const char *reference, lv_color_t ink);
bool navidromestar_player_toggle(const char *reference, void (*refresh)(void));
#endif

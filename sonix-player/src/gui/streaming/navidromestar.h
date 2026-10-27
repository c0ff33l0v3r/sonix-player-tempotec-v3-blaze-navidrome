#ifndef NAVIDROMESTAR_H
#define NAVIDROMESTAR_H
#include "src/gui/shell/gui.h"
#include <stdint.h>

// Return true when the represented source is Navidrome. Other sources retain
// their existing controls. Call only from the GUI thread.
bool navidromestar_player_update(lv_obj_t *button, lv_obj_t *icon, const char *reference, lv_color_t ink);
bool navidromestar_player_toggle(const char *reference, void (*refresh)(void));
// Optional lifetime for persistent overlays; the predicate runs only on the GUI
// thread. A retired visit suppresses its feedback, not service publication.
typedef struct {
	bool (*current)(uint64_t visit);
	uint64_t visit;
} navidromestar_owner_t;
bool navidromestar_player_toggle_owned(const char *reference, void (*refresh)(void), navidromestar_owner_t owner);
#endif

#ifndef DOUBLETAP_H
#define DOUBLETAP_H

#include "lvgl/lvgl.h"

// Two quick taps on `obj` put the screen to sleep, while Settings > Screen >
// Double tap to sleep ([screen] double_tap_sleep) is on. A tap that
// moved (a drag, a swipe) does not count, so the surface keeps its own gestures.
// The surfaces: the screensaver, the status bar and the Now Playing artwork.
void doubletap_sleep_attach(lv_obj_t *obj);

#endif /* DOUBLETAP_H */

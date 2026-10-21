#ifndef UISCALE_H
#define UISCALE_H

#include <stdint.h>

// The interface is written in pixels of a panel 480 px wide, the R1's and the
// R3 Pro II's. ui_px() turns such a size into pixels of the panel this runs on,
// rounding half away from zero so -ui_px(n) == ui_px(-n). At 480 it returns n
// itself: those models draw exactly as they did before sizes went through it.
#define UI_REFERENCE_WIDTH 480

// Set once by main() from the panel the interface is laid out at, before the
// fonts are opened and before any widget exists.
extern int32_t ui_panel_width;

static inline int32_t ui_px(int32_t n) {
	int32_t half = n < 0 ? -UI_REFERENCE_WIDTH / 2 : UI_REFERENCE_WIDTH / 2;
	return (n * ui_panel_width + half) / UI_REFERENCE_WIDTH;
}

#endif

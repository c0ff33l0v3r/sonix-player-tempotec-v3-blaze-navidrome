#ifndef LOWBATTERY_H
#define LOWBATTERY_H

#include "src/gui/shell/gui.h"

// The battery running out, on battery power only.
//
// At 5 % a notice says so, once per discharge. At 1 % the player switches
// itself off, after a ten second countdown on screen that a charger plugged in
// calls off. Left alone, the cell sags under load until the system resets, and
// the device boots straight back into the same empty cell, again and again; a
// shutdown through power_shutdown() leaves it off until the power key or a
// cable.
//
// The level is read every five seconds on battery, also with the screen dark,
// and the countdown turns the screen on. On the cable the check sleeps.

void lowbattery_init(gui_config_t *cfg);

// VBUS came or went: a sleeping check starts again, and looks at once. Called
// from power.c's tick, which already watches the cable.
void lowbattery_cable_changed(void);

#endif /* LOWBATTERY_H */

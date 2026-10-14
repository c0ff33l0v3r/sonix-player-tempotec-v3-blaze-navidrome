#ifndef BOOTANIM_H
#define BOOTANIM_H

#include <stdint.h>

// TempoTec's boot animation, played over the logo.jpeg the kernel stage left on
// screen. The frames are TempoTec's and are not in the repository: the firmware
// packer copies them out of the stock rootfs into
// /usr/resource/sonix/boot_animation/0.png .. N.png before it deletes the stock
// interface. Frames that are not there are not an error, nothing is drawn.
//
// Drawn straight into the first framebuffer page at the panel's own pixels
// (the frames are 240x60, laid exactly over the mark in logo.jpeg), once and
// before the interface exists, so it is the one thing in startup that waits
// for something other than work. A single pass of 125 frames, about five
// seconds.
//
// fb is the mapped framebuffer, RGB565, panel_w x panel_h with a tight stride.
void bootanim_play(uint16_t *fb, int panel_w, int panel_h);

#endif // BOOTANIM_H

#ifndef BOOTLOGO_H
#define BOOTLOGO_H

#include <stdbool.h>
#include <stdint.h>

// The picture shown while the player boots: which one, and the user's own.
//
// It is drawn by `/etc/init.d/S11jpeg_display_shell`, long before the player
// runs and before any filesystem holding a configuration is mounted. The
// choice therefore lives in raw NAND, in a block of /dev/mtd5 that holds
// nothing else, as 13 bytes at 0x20000:
//
//     theme:T,aA,cC
//
//   T  1 light, 2 dark
//   A  the accent, 0-5, the index into theme.c's ACCENT_COLORS
//   C  1 when BOOTLOGO_CUSTOM_FILE is the logo to show
//
// The first seven bytes are the stock firmware's own marker, which its script
// reads as "theme:1" or "theme:2". The script shows
// /etc/boot-logo/<light|dark>-theme-<colour>.jpeg, or the custom file when C
// is 1; with C at 1 it also mounts userdata early so the file is there in time.
//
// The marker is written with the stock tools, `flash_erase` on the block and
// `nandwrite` of one page, on a thread of its own, and only when it differs
// from what is on the flash: NAND has a finite number of erase cycles.
//
// Not on every player. The V3 Blaze's own logo is logo.jpeg, TempoTec's, and
// its script is TempoTec's too: it knows "theme:1" and "theme:2" (HiBy's logo1
// and logo2, left behind and sized for other panels) and nothing of accents or
// a custom file. There /dev/mtd5 is the "ota" partition, and its first block
// the bootloader's boot selector ("ota:kernel"). A model with one_boot_logo set
// writes only to clear a stock "theme:1"/"theme:2" -- with "theme:0,a0,c0",
// which that script matches neither way -- and never otherwise, a marker that
// cannot be read included. Its custom logo is drawn by the player instead
// (bootlogo_draw_custom).

// The user's own logo, on the userdata partition.
#ifndef BOOTLOGO_CUSTOM_FILE // a host test puts it somewhere writable
#define BOOTLOGO_CUSTOM_FILE "/usr/data/bootlogo.jpeg"
#endif

// Brings the marker in line with the theme, the accent and whether a custom
// logo exists. Returns at once; the flash work happens on a worker. A no-op on
// a device without /dev/mtd5 and on the host build.
void bootlogo_follow(bool dark, int accent);

// Whether a custom logo is set.
bool bootlogo_has_custom(void);

// Makes `image_path` (a JPEG or a PNG) the boot logo: scaled to cover
// `width` x `height`, cropped to it around the centre, and saved as a baseline
// JPEG at BOOTLOGO_CUSTOM_FILE. Blocks for the decode and the encode, so it is
// called off the UI thread. False when the picture cannot be read or the file
// cannot be written; the previous logo is then left as it was.
bool bootlogo_make_custom(const char *image_path, int width, int height);

// The panel's own size, out of /dev/fb0, into `width` and `height`; left as
// they are when there is no framebuffer (the host). The interface may be laid
// out larger and shrunk on the way out (the Blaze: 480x720 on a 320x480
// panel), and a logo is drawn before any of that, at the panel's size.
void bootlogo_panel_size(int *width, int *height);

// On a one-logo player (the Blaze), with a custom logo set: draws it into the
// RGB565 framebuffer page `fb` (panel_w x panel_h, no padding), in place of the
// boot animation, whose mark is aligned on /etc/logo.jpeg. The stock script
// cannot show it -- it runs before userdata is mounted -- so this is where it
// first appears. False when there is none or it cannot be decoded; the caller
// then plays the animation as usual.
bool bootlogo_draw_custom(uint16_t *fb, int panel_w, int panel_h);

// Back to the theme's logo: deletes the custom file.
void bootlogo_clear_custom(void);

// Draws the boot logo now, the one S11jpeg_display_shell would pick for this
// theme and accent: the custom file when there is one, otherwise the theme's
// own, otherwise /etc/logo.jpeg. Blocks while cmd_jpeg_display runs. False
// when nothing could be shown.
bool bootlogo_show_now(bool dark, int accent);

#endif /* BOOTLOGO_H */

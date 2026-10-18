// Host self-check for the custom boot logo (src/system/device/bootlogo.c).
//
// Makes a large JPEG and a PNG of a known picture, turns each into the boot
// logo with bootlogo_make_custom(), and checks that what lands on disk is a
// 320x480 baseline JPEG, cropped around the centre, and that
// bootlogo_draw_custom() puts it into an RGB565 page the way the boot does.
//
//   make host   (for the image objects), then, on one line:
//   gcc -O2 -Wall -I. -DBOOTLOGO_CUSTOM_FILE='"/tmp/bootlogo_selftest.jpeg"' tools/bootlogo_selftest.c
//       src/system/device/bootlogo.c build_host/system/image/{art_shrink,jpeg_scaled,png_scaled,png_write,stb_image_impl}.o
//       -lpthread -lm -ldl -o /tmp/bootlogo_selftest && /tmp/bootlogo_selftest [dir]
//   (the source pictures go in dir, default $TMPDIR or /tmp)

#include "src/system/device/bootlogo.h"
#include "src/system/device/sysinfo.h"
#include "src/system/image/art_shrink.h"
#include "src/system/image/png_write.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOGO_W 320
#define LOGO_H 480

// What bootlogo.c needs from the rest of the player: a one-logo model (the
// Blaze), and a thread name that does nothing here.
static const sysinfo_model_t blaze = {.one_boot_logo = true};
const sysinfo_model_t *sysinfo_model(void) { return &blaze; }
void thread_be_background(const char *name) { (void)name; }

// Left half red, right half blue, with a green square in the middle: a cover
// crop keeps the centre, so the green must end up in the middle of the logo.
static uint8_t pixel(int x, int y, int w, int h, int c) {
	int cx = w / 2, cy = h / 2, r = (w < h ? w : h) / 8;
	if (x > cx - r && x < cx + r && y > cy - r && y < cy + r) {
		return c == 1 ? 220 : 20;
	}
	return x < cx ? (c == 0 ? 220 : 20) : (c == 2 ? 220 : 20);
}

static void make_jpeg(const char *path, int w, int h) {
	uint8_t *rgb = malloc((size_t)w * h * 3);
	assert(rgb);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			for (int c = 0; c < 3; c++) {
				rgb[((size_t)y * w + x) * 3 + c] = pixel(x, y, w, h, c);
			}
		}
	}
	size_t len = 0;
	uint8_t *jpeg = art_encode_jpeg(rgb, w, h, 90, &len);
	assert(jpeg && len > 0);
	FILE *f = fopen(path, "wb");
	assert(f && fwrite(jpeg, 1, len, f) == len);
	fclose(f);
	free(jpeg);
	free(rgb);
}

static void make_png(const char *path, int w, int h) {
	uint16_t *px = malloc((size_t)w * h * 2);
	assert(px);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			px[(size_t)y * w + x] = (uint16_t)(((pixel(x, y, w, h, 0) & 0xF8) << 8) |
											   ((pixel(x, y, w, h, 1) & 0xFC) << 3) | (pixel(x, y, w, h, 2) >> 3));
		}
	}
	assert(png_write_rgb565(path, px, w, h));
	free(px);
}

// The frame header of the JPEG at BOOTLOGO_CUSTOM_FILE: baseline (SOF0), and
// its size. Fails on a progressive (SOF2) file.
static void check_baseline(int *w, int *h) {
	FILE *f = fopen(BOOTLOGO_CUSTOM_FILE, "rb");
	assert(f);
	static uint8_t buf[1 << 20];
	size_t n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	assert(n > 4 && buf[0] == 0xFF && buf[1] == 0xD8);
	for (size_t i = 2; i + 9 < n;) {
		assert(buf[i] == 0xFF);
		uint8_t marker = buf[i + 1];
		size_t seg = (size_t)buf[i + 2] << 8 | buf[i + 3];
		assert(marker != 0xC2); // progressive
		if (marker == 0xC0) {
			*h = buf[i + 5] << 8 | buf[i + 6];
			*w = buf[i + 7] << 8 | buf[i + 8];
			return;
		}
		i += 2 + seg;
	}
	assert(!"no SOF0 in the logo");
}

static void check_logo(const char *source) {
	unlink(BOOTLOGO_CUSTOM_FILE);
	assert(bootlogo_make_custom(source, LOGO_W, LOGO_H));
	assert(bootlogo_has_custom());

	int w = 0, h = 0;
	check_baseline(&w, &h);
	assert(w == LOGO_W && h == LOGO_H);

	// As the boot draws it: RGB565, the panel's size.
	static uint16_t page[LOGO_W * LOGO_H];
	memset(page, 0, sizeof(page));
	assert(bootlogo_draw_custom(page, LOGO_W, LOGO_H));
	uint16_t mid = page[(LOGO_H / 2) * LOGO_W + LOGO_W / 2];
	int r = (mid >> 11) << 3, g = ((mid >> 5) & 0x3F) << 2, b = (mid & 0x1F) << 3;
	printf("%s -> %dx%d baseline, centre %d,%d,%d\n", source, w, h, r, g, b);
	assert(g > 150 && r < 80 && b < 80); // the green square survived the crop
}

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : (getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	char jpeg[512], png[512];
	snprintf(jpeg, sizeof(jpeg), "%s/bootlogo_selftest_src.jpg", dir);
	snprintf(png, sizeof(png), "%s/bootlogo_selftest_src.png", dir);

	make_jpeg(jpeg, 4000, 3000); // a camera picture, landscape: cropped to portrait
	check_logo(jpeg);

	make_png(png, 600, 900);
	check_logo(png);

	// Not a picture: refused, and the logo already there is kept.
	assert(!bootlogo_make_custom("/etc/hostname", LOGO_W, LOGO_H));
	assert(bootlogo_has_custom());

	bootlogo_clear_custom();
	assert(!bootlogo_has_custom());
	uint16_t one[4];
	assert(!bootlogo_draw_custom(one, 2, 2)); // nothing set: the animation plays

	unlink(jpeg);
	unlink(png);
	printf("ok\n");
	return 0;
}

// sonix_charge: the charging screen of a player that is "off" on its cable.
//
// The stock player shows a battery and nothing else when the cable lights the
// device, and again at a short press of the power button (a press while it is
// lit turns it off); the long press starts it for real. A player that is up to show that and has restored the
// last track, opened the DAC and loaded every service is not off, so this is
// a program of its own: the framebuffer, the LED, the power key and two sysfs
// files, and nothing that makes a sound.
//
// sonix_player.sh starts it through sonix_launch in place of the player when
// the cable lit the device, and the player execs it at the end of a power-off
// with the cable in. sonix_launch reboots when it exits, so the long press
// only has to say "start" (the PMIC byte below) and leave.
//
// The picture is the stock one: layout of hiby_charge.view on the 320 x 480
// panel, the battery's outline from its own PNG, the green fill (it falls with
// the level) and the percentage in white.

#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "battery_alpha.h"
#include "font_alpha.h"

#define FB_DEV "/dev/fb0"
#define BLANK_NODE "/sys/class/graphics/fb0/blank"
#define BACKLIGHT_NODE "/sys/class/backlight/backlight_pwm0/brightness"
#define LED_NODE "/sys/class/leds/sgm31324-leds/led_pattern"
#define PATTERN_RED 6 // unless the LED page chose another colour for charging
#define PATTERN_NONE "5"
#define PATTERN_BLUE "1"
#define RELEASE_WAIT_MS 3000 // after the long press, the reboot waits this long for the key to come up
#define CAPACITY_NODE "/sys/class/power_supply/battery/capacity"
#define SUPPLY_DIR "/sys/class/power_supply"
#define LOG_FILE "/mnt/sd_0/.local/charge.log"
#define LOG_FALLBACK "/usr/data/charge.log" // the card is not always mounted at this point
#define LOG_MAX 20000
#define KEYS_DEV "/dev/input/event0"
#define CONFIG_FILE "/usr/data/device_config.ini"

#define SHOWN_MS 10000	  // how long the battery stays lit
#define LONG_PRESS_MS 1000 // the power button held: start the player
#define POLL_MS 500
#define CABLE_GONE_POLLS 12 // consecutive reads without the cable: six seconds
#define CABLE_GRACE_MS 20000 // the supplies are not trusted this soon after the boot

// The stock layout, 320 x 480: outline at (107,160), fill 74 x 84 at (123,203),
// the percentage centred under it at y 325.
#define PANEL_W 320
#define PANEL_H 480
#define FRAME_X 107
#define FRAME_Y 160
#define FILL_X 123
#define FILL_BOTTOM 287
#define FILL_W 74
#define FILL_H 84
#define TEXT_Y 325

static int fb_fd = -1;
static uint8_t *fb_mem;
static struct fb_var_screeninfo var;
static size_t page_size;
static int xres, yres;

static void write_str(const char *path, const char *value) {
	FILE *f = fopen(path, "w");
	if (f) {
		fputs(value, f);
		fclose(f);
	}
}

static int read_int(const char *path, int fallback) {
	FILE *f = fopen(path, "r");
	int v = fallback;
	if (f) {
		if (fscanf(f, "%d", &v) != 1) {
			v = fallback;
		}
		fclose(f);
	}
	return v;
}

// Any supply that is not the battery and says it is online: the cable, whether
// it is a computer's (usb) or a wall adapter's (ac), and whatever else the
// charger driver calls it. The battery's own nodes have no "online".
static bool cable_in(void) {
	bool in = false;
	DIR *dir = opendir(SUPPLY_DIR);
	if (!dir) {
		return true; // cannot tell: not a reason to power off
	}
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}
		char path[300];
		snprintf(path, sizeof(path), SUPPLY_DIR "/%s/online", de->d_name);
		if (read_int(path, 0) == 1) {
			in = true;
		}
	}
	closedir(dir);
	return in;
}

// One line on the card, when there is one, for the day the screen does
// something unexpected: what was seen and why it left.
static void note(const char *what) {
	const char *path = access("/mnt/sd_0/.local", W_OK) == 0 ? LOG_FILE : LOG_FALLBACK;
	struct stat st;
	FILE *f = fopen(path, stat(path, &st) == 0 && st.st_size > LOG_MAX ? "w" : "a");
	if (!f) {
		return;
	}
	struct timespec up;
	clock_gettime(CLOCK_BOOTTIME, &up);
	fprintf(f, "up %lds sonix_charge: %s (cable %d, capacity %d)\n", (long)up.tv_sec, what, cable_in(),
			read_int(CAPACITY_NODE, -1));
	fclose(f);
}

static long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// A positive number from the player's settings ([section] key = value), or
// `value` when it is not there.
static int configured_int(const char *section, const char *key, int value) {
	FILE *f = fopen(CONFIG_FILE, "r");
	if (!f) {
		return value;
	}
	char line[256];
	size_t section_len = strlen(section), key_len = strlen(key);
	bool in_section = false;
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '[') {
			in_section = strncmp(line + 1, section, section_len) == 0 && line[section_len + 1] == ']';
		} else if (in_section && strncmp(line, key, key_len) == 0 && strchr(" =", line[key_len])) {
			char *eq = strchr(line, '=');
			if (eq && atoi(eq + 1) > 0) {
				value = atoi(eq + 1);
			}
		}
	}
	fclose(f);
	return value;
}

static uint16_t rgb565(int r, int g, int b) { return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }

static void put(uint16_t *page, int x, int y, uint16_t c) {
	if (x >= 0 && y >= 0 && x < xres && y < yres) {
		page[(size_t)y * xres + x] = c;
	}
}

static void rect(uint16_t *page, int x, int y, int w, int h, uint16_t c) {
	for (int j = 0; j < h; j++) {
		for (int i = 0; i < w; i++) {
			put(page, x + i, y + j, c);
		}
	}
}

static void draw_page(uint16_t *page, int percent, int ox, int oy) {
	memset(page, 0, page_size);

	// The outline: one tint, the PNG's alpha over black.
	for (int y = 0; y < BATTERY_H; y++) {
		for (int x = 0; x < BATTERY_W; x++) {
			int a = battery_alpha[y * BATTERY_W + x];
			if (a) {
				put(page, ox + FRAME_X + x, oy + FRAME_Y + y, rgb565(190 * a / 255, 175 * a / 255, 175 * a / 255));
			}
		}
	}

	// The fill, green, from the bottom up as the level falls.
	int h = FILL_H * percent / 100;
	if (percent > 0 && h < 2) {
		h = 2;
	}
	rect(page, ox + FILL_X, oy + FILL_BOTTOM - h, FILL_W, h, rgb565(1, 219, 17));

	// The percentage, in the interface's own font, centred under the battery.
	char text[8];
	snprintf(text, sizeof(text), "%d%%", percent);
	int total = 0;
	for (const char *c = text; *c; c++) {
		total += glyphs[*c == '%' ? 10 : *c - '0'].advance;
	}
	int pen = ox + FRAME_X + (BATTERY_W - total) / 2;
	int baseline = oy + TEXT_Y + glyphs[0].top;
	for (const char *c = text; *c; c++) {
		const glyph_t *g = &glyphs[*c == '%' ? 10 : *c - '0'];
		for (int row = 0; row < g->h; row++) {
			for (int col = 0; col < g->w; col++) {
				int a = glyph_alpha[g->offset + row * g->w + col];
				if (a) {
					put(page, pen + g->left + col, baseline - g->top + row, rgb565(a, a, a));
				}
			}
		}
		pen += g->advance;
	}
}

static bool fb_open(void) {
	fb_fd = open(FB_DEV, O_RDWR);
	if (fb_fd < 0 || ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) < 0) {
		return false;
	}
	struct fb_fix_screeninfo fix;
	if (var.bits_per_pixel != 16 || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) < 0 ||
		fix.line_length != var.xres * 2) {
		return false;
	}
	// Two pages, as the player asks for them, so the driver is in the mode it
	// expects.
	var.yres_virtual = var.yres * 2;
	var.xres_virtual = var.xres;
	var.yoffset = 0;
	ioctl(fb_fd, FBIOPUT_VSCREENINFO, &var);
	ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix);
	xres = (int)var.xres;
	yres = (int)var.yres;
	page_size = (size_t)fix.line_length * var.yres;
	if (fix.smem_len < page_size * 2) {
		return false;
	}
	fb_mem = mmap(NULL, page_size * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	return fb_mem != MAP_FAILED;
}

static bool screen_on;
static int brightness;

static void paint(int percent) {
	int ox = (xres - PANEL_W) / 2, oy = (yres - PANEL_H) / 2;
	draw_page((uint16_t *)fb_mem, percent, ox, oy);
	memcpy(fb_mem + page_size, fb_mem, page_size); // both pages alike: whichever is scanned out
}

static void light(int percent) {
	if (!screen_on) {
		write_str(BLANK_NODE, "0");
		// The unblank can reset the mode; put it back and pan, as the player
		// does on every wake.
		var.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
		ioctl(fb_fd, FBIOPUT_VSCREENINFO, &var);
	}
	paint(percent);
	var.yoffset = 0;
	ioctl(fb_fd, FBIOPAN_DISPLAY, &var);
	if (!screen_on) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%d", brightness);
		write_str(BACKLIGHT_NODE, buf);
		screen_on = true;
	}
}

static void dark(void) {
	// 1, not 0: from 0 this backlight does not always come back on the next
	// unblank (the player's wake had the same trouble), and the blank puts it out anyway.
	write_str(BACKLIGHT_NODE, "1");
	write_str(BLANK_NODE, "4"); // FB_BLANK_POWERDOWN
	screen_on = false;
}

int main(void) {
	if (!fb_open()) {
		fprintf(stderr, "sonix_charge: no usable framebuffer\n");
		// Nothing to show on, but the LED and the buttons still do their job.
	}
	// [screen] brightness, the level the user chose, and the charging colour
	// from the LED page (state 6, LED_STATE_CHARGING in the player's led.h).
	brightness = configured_int("screen", "brightness", 40);
	int charging_pattern = configured_int("led", "colour_6", PATTERN_RED);
	if (charging_pattern > 16) {
		charging_pattern = PATTERN_RED;
	}
	char pattern_str[8];
	snprintf(pattern_str, sizeof(pattern_str), "%d", charging_pattern);
	write_str(LED_NODE, pattern_str);
	note("started");

	int keys = open(KEYS_DEV, O_RDONLY | O_NONBLOCK);
	int percent = read_int(CAPACITY_NODE, 0);
	int shown_percent = percent;
	if (fb_mem && fb_mem != MAP_FAILED) {
		light(percent);
	}
	long started_at = now_ms();
	long shown_until = started_at + SHOWN_MS;
	long held_since = 0;
	int cable_gone = 0;
	bool started = false;

	for (;;) {
		struct pollfd pfd = {.fd = keys, .events = POLLIN};
		int wait = POLL_MS;
		if (held_since) {
			long left = held_since + LONG_PRESS_MS - now_ms();
			wait = left < 0 ? 0 : (left < POLL_MS ? (int)left : POLL_MS);
		}
		if (keys >= 0 && poll(&pfd, 1, wait) > 0) {
			struct input_event ev;
			while (read(keys, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
				if (ev.type != EV_KEY || ev.code != KEY_POWER) {
					continue;
				}
				if (ev.value == 1) {
					held_since = now_ms();
				} else if (ev.value == 0 && held_since) {
					// A short press: dark if lit, like the player's own screen
					// off; otherwise the battery again, for a while.
					held_since = 0;
					if (screen_on) {
						note("screen off by the key");
						dark();
						continue;
					}
					note("screen on by the key");
					percent = read_int(CAPACITY_NODE, percent);
					shown_percent = percent;
					light(percent);
					shown_until = now_ms() + SHOWN_MS;
				}
			}
		} else if (keys < 0) {
			usleep((useconds_t)wait * 1000);
		}

		long now = now_ms();
		if (held_since && now - held_since >= LONG_PRESS_MS && !started) {
			// Start for real: the PMIC byte says this is not a charge-only
			// start any more, and sonix_launch reboots when this exits.
			started = true;
			note("power held: starting the player");
			int rc = system("i2cset -f -y 0 0x34 0x07 0x01");
			(void)rc;
			// The LED says "let go", and the reboot waits for it: a key still
			// held through the reboot reaches the PMIC's 6 s power-off (REG27h)
			// and the player has to be switched on again.
			write_str(LED_NODE, PATTERN_BLUE);
			long until = now_ms() + RELEASE_WAIT_MS;
			while (keys >= 0 && now_ms() < until) {
				struct pollfd rel = {.fd = keys, .events = POLLIN};
				if (poll(&rel, 1, (int)(until - now_ms())) <= 0) {
					break;
				}
				struct input_event ev;
				bool released = false;
				while (read(keys, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
					released |= ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 0;
				}
				if (released) {
					break;
				}
			}
			return 0;
		}

		percent = read_int(CAPACITY_NODE, percent);
		if (screen_on && percent != shown_percent) {
			shown_percent = percent;
			light(percent);
		}
		if (screen_on && now >= shown_until) {
			note("screen off");
			dark();
		}

		// The cable pulled: off, as the stock player. Read a few times in a
		// row, the supply can blink.
		if (!cable_in()) {
			if (cable_gone == 0) {
				note("cable not seen");
			}
			// Counted from the boot (now_ms is CLOCK_MONOTONIC), not from this start:
			// exec'd by the player's power-off the supplies are long settled.
			if (now < CABLE_GRACE_MS) {
				cable_gone = 1;
			} else if (++cable_gone >= CABLE_GONE_POLLS) {
				note("cable gone: power off");
				write_str(LED_NODE, PATTERN_NONE);
				dark();
				int rc = system("poweroff");
				(void)rc;
				sleep(30);
			}
		} else {
			if (cable_gone) {
				note("cable seen again");
			}
			cable_gone = 0;
		}
	}
}

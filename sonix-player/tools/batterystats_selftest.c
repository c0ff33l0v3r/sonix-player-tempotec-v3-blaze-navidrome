// Host self-check for the battery usage counts (src/system/device/batterystats.c).
//
// Drives the counter on a fake clock and a fake gauge: time split at the hour
// boundaries, a suspend spread over the hours it covered, a drop shared out in
// the model's proportions, a gauge bouncing back a percent, and the cable.
//
//   gcc -O2 -Wall -DHOST_BUILD -I. -Ilvgl tools/batterystats_selftest.c -lm -o /tmp/batterystats_selftest
//   /tmp/batterystats_selftest

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint64_t fake_mono = 1000, fake_boot = 1000;
static int64_t fake_wall = 1792771200000LL; // an exact hour: 497992 * 3600 s

static int fake_clock_gettime(clockid_t id, struct timespec *ts) {
	uint64_t ms = id == CLOCK_MONOTONIC ? fake_mono : id == CLOCK_BOOTTIME ? fake_boot : (uint64_t)fake_wall;
	ts->tv_sec = (time_t)(ms / 1000);
	ts->tv_nsec = (long)(ms % 1000) * 1000000;
	return 0;
}
#define clock_gettime fake_clock_gettime
#include "src/system/device/batterystats.c"

static int fake_source = -1, fake_level = 80;
static bool fake_cable, fake_bt_out;

bool btreceiver_is_streaming(void) { return fake_source == BS_BT_RECEIVER; }
void usbdac_get_state(usbdac_state_t *out) { memset(out, 0, sizeof(*out)); }
bool usbdac_is_active(void) { return false; }
bool usbdac_charging_enabled(void) { return true; }
bool radio_is_playing(void) { return false; }
void airplay_get_state(airplay_state_t *out) { memset(out, 0, sizeof(*out)); }
audio_status_t audio_get_status(void) { return fake_source == BS_LOCAL ? AUDIO_STATUS_PLAYING : AUDIO_STATUS_STOPPED; }
bool dlna_owns_playback(void) { return false; }
bool audio_output_is_bluetooth(void) { return fake_bt_out; }
void audio_get_current_file(char *out, size_t size) { snprintf(out, size, "/mnt/sd/a.flac"); }
bool qobuzcache_owns(const char *path) { return (void)path, false; }
bool tidalcache_owns(const char *path) { return (void)path, false; }
bool podcastcache_owns(const char *path) { return (void)path, false; }
bool usb_vbus_present(void) { return fake_cable; }
char *read_battery_percent() {
	static char text[8];
	snprintf(text, sizeof(text), "%d", fake_level);
	return text;
}

// `seconds` awake, one tick a second.
static void awake(int seconds, bool screen_on) {
	for (int i = 0; i < seconds; i++) {
		fake_mono += 1000;
		fake_boot += 1000;
		fake_wall += 1000;
		batterystats_tick(screen_on);
	}
}

static void suspended(int seconds) {
	fake_boot += (uint64_t)seconds * 1000;
	fake_wall += (int64_t)seconds * 1000;
}

static float total_used(int item) {
	float sum = 0;
	for (int ago = 0; ago < BS_HOURS; ago++) {
		const batterystats_hour_t *h = batterystats_hour(ago);
		if (h) {
			sum += item < 0 ? 0 : h->used[item] + h->pending[item];
			for (int k = 0; item < 0 && k < BS_ITEM_COUNT; k++) {
				sum += h->used[k] + h->pending[k];
			}
		}
	}
	return sum;
}

int main(void) {
	batterystats_init();
	awake(1, true); // the first tick only takes the clocks

	// 30 min, screen on, a track from the card; the level falls 80 -> 77.
	fake_source = BS_LOCAL;
	for (int i = 0; i < 3; i++) {
		awake(600, true);
		fake_level--;
	}
	awake(1, true);
	assert(s_ref_level == 77);
	// What the gauge lost, all of it (no pending left after the last drop but a second's worth).
	assert(fabsf(total_used(-1) - 3.0f) < 0.01f);
	// Shared 8.7 : 5.0 between the music and the screen, as the model says.
	assert(fabsf(total_used(BS_LOCAL) / total_used(BS_SCREEN) - (RATE_AWAKE + RATE_PLAY[BS_LOCAL]) / RATE_SCREEN) < 0.01f);

	// Paused, screen dark up to minute 59, then 2 h suspended: a minute in
	// this hour, all of the next, 59 min of the one after.
	fake_source = -1;
	awake(29 * 60 - 2, false);
	suspended(2 * 3600);
	awake(1, false);
	const batterystats_hour_t *now = batterystats_hour(0), *mid = batterystats_hour(1), *first = batterystats_hour(2);
	assert(first && mid && now);
	assert(first->ms[BS_SLEEP] == 60 * 1000);
	assert(mid->ms[BS_SLEEP] == BS_HOUR_MS);
	assert(now->ms[BS_SLEEP] == 59 * 60 * 1000);
	// Every second of the hour accounted for once, less the one the first tick took the clocks in.
	assert(first->ms[BS_SCREEN] + first->ms[BS_STANDBY] + first->ms[BS_SLEEP] == BS_HOUR_MS - 1000);
	assert(now->ms[BS_SLEEP] + now->ms[BS_STANDBY] == (uint32_t)(fake_wall % BS_HOUR_MS));

	// The gauge bounces 77 -> 76 -> 77 -> 76: one percent, not two.
	float settled = 0;
	for (int k = 0; k < BS_ITEM_COUNT; k++) {
		for (int ago = 0; ago < BS_HOURS; ago++) {
			const batterystats_hour_t *h = batterystats_hour(ago);
			settled += h ? h->used[k] : 0;
		}
	}
	fake_level = 76;
	awake(1, false);
	fake_level = 77;
	awake(1, false);
	fake_level = 76;
	awake(1, false);
	float after = 0;
	for (int k = 0; k < BS_ITEM_COUNT; k++) {
		for (int ago = 0; ago < BS_HOURS; ago++) {
			const batterystats_hour_t *h = batterystats_hour(ago);
			after += h ? h->used[k] : 0;
		}
	}
	assert(fabsf(after - settled - 1.0f) < 0.01f);

	// On the cable: nothing charged to anyone, and the climb to 90 is no drop.
	float plugged = total_used(-1);
	fake_cable = true;
	fake_level = 90;
	awake(600, true);
	assert(fabsf(total_used(-1) - plugged) < 0.001f);
	assert(batterystats_hour(0)->cable);
	fake_cable = false;
	awake(1, true);
	assert(s_ref_level == 90 && s_ref_fresh);

	// A track from the card to Bluetooth headphones: both items count the time.
	uint32_t local_before = batterystats_hour(0)->ms[BS_LOCAL];
	fake_source = BS_LOCAL;
	fake_bt_out = true;
	awake(60, false);
	assert(batterystats_hour(0)->ms[BS_BT_OUT] == 60 * 1000);
	assert(batterystats_hour(0)->ms[BS_LOCAL] - local_before == 60 * 1000);

	printf("batterystats selftest: ok\n");
	return 0;
}

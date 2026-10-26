// Host self-check for the battery usage counts (src/system/device/batterystats.c).
//
// Drives the counter on a fake clock and a fake gauge: time split at the hour
// boundaries, a suspend spread over the hours it covered, a drop shared out in
// the model's proportions, a gauge bouncing back a percent, and the cable.
//
// Links the real Navidrome ownership helper, discarding unrelated transfer code.
//   gcc -O2 -Wall -DHOST_BUILD -I. -Ilvgl -ffunction-sections -fdata-sections tools/batterystats_selftest.c src/system/streaming/navidromeaudio.c -Wl,--gc-sections -lm -o /tmp/batterystats_selftest
//   /tmp/batterystats_selftest

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Deterministic publication failures, without relying on host permissions.
static enum { IO_OK, IO_OPEN, IO_WRITE, IO_SYNC, IO_CLOSE, IO_RENAME } io_fault;
static int test_open(const char *path, int flags, ...) {
	va_list args;
	va_start(args, flags);
	int mode = va_arg(args, int);
	va_end(args);
	if (io_fault == IO_OPEN) {
		errno = EROFS;
		return -1;
	}
	return open(path, flags, mode);
}
static ssize_t test_write(int fd, const void *data, size_t size) {
	if (io_fault == IO_WRITE) {
		errno = ENOSPC;
		return write(fd, data, size / 2);
	}
	return write(fd, data, size);
}
static int test_fsync(int fd) {
	if (io_fault == IO_SYNC) {
		errno = EIO;
		return -1;
	}
	return fsync(fd);
}
static int test_close(int fd) {
	int result = close(fd);
	if (io_fault == IO_CLOSE) {
		errno = EIO;
		return -1;
	}
	return result;
}
static int test_rename(const char *from, const char *to) {
	if (io_fault == IO_RENAME) {
		errno = EIO;
		return -1;
	}
	return rename(from, to);
}

static uint64_t fake_mono = 1000, fake_boot = 1000;
static int64_t fake_wall = 1792771200000LL; // an exact hour: 497992 * 3600 s

static int fake_clock_gettime(clockid_t id, struct timespec *ts) {
	uint64_t ms = id == CLOCK_MONOTONIC ? fake_mono : id == CLOCK_BOOTTIME ? fake_boot : (uint64_t)fake_wall;
	ts->tv_sec = (time_t)(ms / 1000);
	ts->tv_nsec = (long)(ms % 1000) * 1000000;
	return 0;
}
#define clock_gettime fake_clock_gettime
#define open test_open
#define write test_write
#define fsync test_fsync
#define close test_close
#define rename test_rename
#include "src/system/device/batterystats.c"
#undef clock_gettime
#undef open
#undef write
#undef fsync
#undef close
#undef rename

static int fake_source = -1, fake_level = 80;
static int fake_status = -1;
static const char *fake_path;
static bool fake_cable, fake_bt_out;

bool btreceiver_is_streaming(void) { return fake_source == BS_BT_RECEIVER; }
void usbdac_get_state(usbdac_state_t *out) {
	memset(out, 0, sizeof(*out));
	out->streaming = fake_source == BS_USB_DAC;
}
bool usbdac_is_active(void) { return fake_source == BS_USB_DAC; }
bool usbdac_charging_enabled(void) { return true; }
bool radio_is_playing(void) { return fake_source == BS_RADIO; }
void airplay_get_state(airplay_state_t *out) {
	memset(out, 0, sizeof(*out));
	out->playing = fake_source == BS_AIRPLAY;
}
audio_status_t audio_get_status(void) { return fake_status >= 0 ? (audio_status_t)fake_status : fake_source >= 0 ? AUDIO_STATUS_PLAYING : AUDIO_STATUS_STOPPED; }
bool dlna_owns_playback(void) { return fake_source == BS_DLNA; }
bool audio_output_is_bluetooth(void) { return fake_bt_out; }
void audio_get_current_file(char *out, size_t size) {
	const char *path = fake_path ? fake_path : fake_source == BS_NAVIDROME ? "/mnt/sd/.local/navidrome/.cache/audio/scope/song.flac" : "/mnt/sd/a.flac";
	snprintf(out, size, "%s", path);
}
bool qobuzcache_owns(const char *path) { return (void)path, fake_source == BS_QOBUZ; }
bool tidalcache_owns(const char *path) { return (void)path, fake_source == BS_TIDAL; }
bool podcastcache_owns(const char *path) { return (void)path, fake_source == BS_PODCAST; }
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

static void existing_accounting(void) {
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
}

// Simulate a new process; battery initialization itself is deliberately once-only.
static void reset_process(void) {
	memset(&s_stats, 0, sizeof(s_stats));
	memset(s_interval_ms, 0, sizeof(s_interval_ms));
	s_path = NULL;
	s_last_mono = s_last_boot = s_saved_mono = 0;
	s_ref_level = -1;
	s_ref_fresh = false;
	fake_mono = fake_boot = 1000;
	fake_wall = 1792771200000LL;
	fake_source = fake_status = -1;
	fake_level = 80;
	fake_path = NULL;
	fake_cable = fake_bt_out = false;
	io_fault = IO_OK;
}

static void navidrome_accounting(void) {
	reset_process();
	batterystats_init();
	awake(1, true);
	track_level(fake_level, false); // begin this allocation case at a known gauge boundary
	fake_source = BS_NAVIDROME;
	assert(playing_source() == BS_NAVIDROME);
	assert(!nd_audio_owns(NULL) && !nd_audio_owns("ndq:1:0"));
	assert(!nd_audio_owns("/mnt/sd/Music/Navidrome/song.flac"));
	// One physical permanent file retains the audible source's accounting.
	const char *permanent = "/mnt/sd/Music/Navidrome/song.flac";
	fake_path = permanent;
	nd_audio_set_source(permanent);
	assert(playing_source() == BS_NAVIDROME);
	nd_audio_set_source(NULL);
	fake_source = BS_LOCAL;
	assert(playing_source() == BS_LOCAL);
	fake_source = BS_NAVIDROME;
	fake_path = NULL;

	// The same real ownership rule covers growing, complete and legacy files;
	// it does not inspect completion metadata, the card or connection state.
	const char *paths[] = {
		"/mnt/sd/.local/navidrome/.cache/audio/scope/song.flac",
		"/mnt/sd/.local/navidrome/.cache/audio/scope/song.mp3",
		"/mnt/sd/.local/navidrome/audio/scope/song.flac",
	};
	for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		fake_path = paths[i];
		assert(playing_source() == BS_NAVIDROME);
	}
	fake_path = paths[0];

	// Existing sources retain precedence even with a stale Navidrome file path.
	const int sources[] = {BS_BT_RECEIVER, BS_USB_DAC, BS_RADIO, BS_AIRPLAY, BS_DLNA, BS_QOBUZ, BS_TIDAL, BS_PODCAST};
	for (unsigned i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
		fake_source = sources[i];
		assert(playing_source() == sources[i]);
	}
	fake_source = BS_NAVIDROME;
	awake(600, true);
	fake_level--;
	awake(1, true);
	assert(fabsf(total_used(-1) - 1.0f) < 0.001f);
	assert(total_used(BS_LOCAL) == 0);
	assert(fabsf(total_used(BS_NAVIDROME) / total_used(BS_SCREEN) - 11.0f / 5.0f) < 0.001f);
	assert(RATE_PLAY[BS_NAVIDROME] == RATE_PLAY[BS_QOBUZ]);
	assert(RATE_PLAY[BS_NAVIDROME] == RATE_PLAY[BS_TIDAL]);
	assert(!strcmp(ITEM_NAMES[BS_NAVIDROME], "navidrome"));

	uint32_t before = batterystats_hour(0)->ms[BS_NAVIDROME];
	float used = total_used(-1);
	fake_bt_out = true;
	awake(60, false);
	assert(batterystats_hour(0)->ms[BS_NAVIDROME] == before + 60000);
	assert(batterystats_hour(0)->ms[BS_BT_OUT] == 60000);
	assert(fabsf(total_used(-1) - used - 13.0f / 60.0f) < 0.001f);

	// Plugging in settles pending weights but adds no new battery charges.
	fake_cable = true;
	used = total_used(-1);
	awake(60, true);
	assert(batterystats_hour(0)->ms[BS_NAVIDROME] == before + 120000);
	assert(fabsf(total_used(-1) - used) < 0.001f);

	// Paused/stopped/preparation/background-only intervals do not listen.
	before = batterystats_hour(0)->ms[BS_NAVIDROME];
	fake_status = AUDIO_STATUS_PAUSED;
	awake(60, true);
	fake_status = AUDIO_STATUS_STOPPED;
	awake(60, false);
	assert(batterystats_hour(0)->ms[BS_NAVIDROME] == before);
	fake_status = -1;
	fake_source = BS_LOCAL;
	fake_path = "/mnt/sd/Music/song.flac";
	awake(60, false);
	assert(batterystats_hour(0)->ms[BS_NAVIDROME] == before);
	assert(batterystats_hour(0)->ms[BS_LOCAL] == 60000);
	fake_source = BS_NAVIDROME;
	fake_path = NULL;
	awake(60, false);
	assert(batterystats_hour(0)->ms[BS_NAVIDROME] == before + 60000);
}

// Independently describe the original on-disk fixture, without using the new
// reader's legacy typedef or mapping table to manufacture its expected values.
typedef struct {
	int32_t hour;
	uint32_t ms[13];
	float used[13], pending[13];
	int8_t level;
	bool cable;
} fixture_hour_v1_t;
typedef struct {
	uint32_t magic, version;
	int64_t since_ms;
	fixture_hour_v1_t hours[24];
} fixture_v1_t;

static void write_fixture(const char *path, const void *data, size_t size) {
	FILE *f = fopen(path, "wb");
	assert(f);
	assert(fwrite(data, 1, size, f) == size);
	assert(fclose(f) == 0);
}

static void assert_file(const char *path, const void *expected, size_t size) {
	unsigned char actual[sizeof(stats_file_t)];
	assert(size <= sizeof(actual));
	FILE *f = fopen(path, "rb");
	assert(f && fread(actual, 1, size, f) == size);
	assert(fgetc(f) == EOF && !ferror(f));
	assert(!memcmp(actual, expected, size));
	assert(fclose(f) == 0);
}

static void persistence(void) {
	char directory[] = "/tmp/sonix-batterystats-XXXXXX";
	assert(mkdtemp(directory));
	char path[192], tmp[200];
	snprintf(path, sizeof(path), "%s/history.bin", directory);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	assert(setenv("SONIX_BATTERY_STATS", path, 1) == 0);
	fixture_v1_t old = {.magic = 0x53425431u, .version = 1, .since_ms = 1792771200000LL - 23 * 3600000LL};
	for (int i = 0; i < 24; i++) {
		fixture_hour_v1_t *h = &old.hours[i];
		h->hour = 497992 - (497992 - i) % 24; // each hour occupies its real ring slot
		h->level = 40 + i;
		h->cable = i % 2;
		for (int k = 0; k < 13; k++) {
			h->ms[k] = 1000 * (i + 1) + k;
			h->used[k] = (i * 13 + k) * 0.25f;
			h->pending[k] = (i * 13 + k + 1) * 0.125f;
		}
	}
	write_fixture(path, &old, sizeof(old));
	reset_process();
	batterystats_init();
	assert_file(path, &old, sizeof(old)); // init did not force an upgrade write
	assert(s_stats.version == 2 && batterystats_since_ms() == old.since_ms);
	const int destinations[] = {BS_LOCAL, BS_BT_RECEIVER, BS_USB_DAC, BS_RADIO, BS_QOBUZ, BS_TIDAL, BS_PODCAST, BS_AIRPLAY, BS_DLNA, BS_SCREEN, BS_STANDBY, BS_SLEEP, BS_BT_OUT};
	for (int i = 0; i < 24; i++) {
		const fixture_hour_v1_t *from = &old.hours[i];
		const batterystats_hour_t *to = &s_stats.hours[i];
		assert(to->hour == from->hour && to->level == from->level && to->cable == from->cable);
		for (int k = 0; k < 13; k++) {
			int item = destinations[k];
			assert(to->ms[item] == from->ms[k]);
			assert(to->used[item] == from->used[k] + from->pending[k]);
			assert(to->pending[item] == 0);
		}
		assert(to->ms[BS_NAVIDROME] == 0 && to->used[BS_NAVIDROME] == 0 && to->pending[BS_NAVIDROME] == 0);
	}

	// Failed upgrade publication leaves every byte of version 1 available.
	stats_file_t migrated = s_stats;
	for (int fault = IO_OPEN; fault <= IO_RENAME; fault++) {
		io_fault = fault;
		batterystats_save();
		assert_file(path, &old, sizeof(old));
		assert(access(tmp, F_OK) != 0);
		assert(!memcmp(&s_stats, &migrated, sizeof(migrated)));
	}
	io_fault = IO_OK;
	batterystats_save();
	assert_file(path, &migrated, sizeof(migrated));
	reset_process();
	batterystats_init();
	assert(!memcmp(&s_stats, &migrated, sizeof(migrated))); // pending settled just once

	// Current-format real sampled Navidrome time survives a subsequent restart.
	fake_source = BS_NAVIDROME;
	awake(1, false);
	awake(60, false);
	batterystats_save();
	stats_file_t saved = s_stats;
	assert_file(path, &saved, sizeof(saved));
	reset_process();
	batterystats_init();
	for (int i = 0; i < 24; i++) {
		for (int k = 0; k < BS_ITEM_COUNT; k++) {
			assert(s_stats.hours[i].ms[k] == saved.hours[i].ms[k]);
			assert(s_stats.hours[i].used[k] == saved.hours[i].used[k] + saved.hours[i].pending[k]);
			assert(s_stats.hours[i].pending[k] == 0);
		}
	}
	assert(batterystats_hour(0)->ms[BS_NAVIDROME] == 60000);

	// Current-file save failures also retain the previous successful recording.
	for (int fault = IO_OPEN; fault <= IO_RENAME; fault++) {
		io_fault = fault;
		batterystats_save();
		assert_file(path, &saved, sizeof(saved));
	}
	io_fault = IO_OK;
	stats_file_t empty = {.magic = STATS_MAGIC, .version = 2};
	for (int version = 1; version <= 2; version++) {
		const void *fixture = version == 1 ? (const void *)&old : (const void *)&saved;
		size_t size = version == 1 ? sizeof(old) : sizeof(saved);
		const size_t lengths[] = {0, 1, 7, 8, 16, size - 1};
		for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
			write_fixture(path, fixture, lengths[i]);
			reset_process();
			batterystats_init();
			assert(!memcmp(&s_stats, &empty, sizeof(empty)));
		}
	}
	saved.version = 99;
	write_fixture(path, &saved, sizeof(saved));
	reset_process();
	batterystats_init();
	assert(!memcmp(&s_stats, &empty, sizeof(empty)));
	saved.version = 2;
	saved.magic = 0;
	write_fixture(path, &saved, sizeof(saved));
	reset_process();
	batterystats_init();
	assert(!memcmp(&s_stats, &empty, sizeof(empty)));
	assert(unlink(path) == 0);
	reset_process();
	batterystats_init();
	assert(!memcmp(&s_stats, &empty, sizeof(empty)));

	batterystats_save();
	assert(access(path, F_OK) == 0);
	batterystats_forget();
	batterystats_save();
	assert(access(path, F_OK) != 0); // reset must never recreate the file
	assert(rmdir(directory) == 0);
	assert(unsetenv("SONIX_BATTERY_STATS") == 0);
}

int main(void) {
	// Never honor an inherited runtime statistics path in this synthetic check.
	assert(unsetenv("SONIX_BATTERY_STATS") == 0);
	existing_accounting();
	navidrome_accounting();
	persistence();
	printf("batterystats selftest: ok (existing accounting, Navidrome, migration/publication)\n");
	return 0;
}

// The shared PCM processors used after Navidrome hands the original to the decoder.
#include "src/system/audio/eq.h"
#include "src/system/audio/replaygain.h"
#include "src/system/core/config.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static long gain_mode, play_mode;
const char *config_get(const char *s, const char *k, const char *fallback) {
	(void)s;
	(void)k;
	return fallback;
}
long config_get_int(const char *s, const char *k, long fallback) {
	if (!strcmp(s, "audio") && !strcmp(k, "replaygain"))
		return gain_mode;
	if (!strcmp(s, "player") && !strcmp(k, "playback_mode"))
		return play_mode;
	return fallback;
}
bool config_get_bool(const char *s, const char *k, bool fallback) { return config_get_int(s, k, fallback) != 0; }
void config_set_int(const char *s, const char *k, long value) {
	if (!strcmp(s, "audio") && !strcmp(k, "replaygain"))
		gain_mode = value;
}
void config_set_bool(const char *s, const char *k, bool value) { config_set_int(s, k, value); }
void config_set(const char *s, const char *k, const char *v) {
	(void)s;
	(void)k;
	(void)v;
}
bool config_save(void) { return true; }
#define FRAMES 512
static short original[FRAMES * 2], samples[FRAMES * 2];
static int32_t wide[FRAMES * 2], original_wide[FRAMES * 2];
static void copy(void) {
	memcpy(samples, original, sizeof(samples));
	memcpy(wide, original_wide, sizeof(wide));
}
static void changed_filters(void) {
	copy();
	eq_process(samples, FRAMES, 2, 44100);
	eq_process_s32(wide, FRAMES, 2, 44100);
	assert(memcmp(samples, original, sizeof(samples)) && memcmp(wide, original_wide, sizeof(wide)));
}
int main(void) {
	for (int i = 0; i < FRAMES; i++) {
		original[i * 2] = (short)(6000 * sin(i * .14));
		original[i * 2 + 1] = (short)(3000 * cos(i * .07));
	}
	for (int i = 0; i < FRAMES * 2; i++)
		original_wide[i] = (int32_t)original[i] * 32768;
	eq_init();
	eq_set_enabled(false);
	mseb_set_enabled(false);
	peq_set_enabled(false);
	copy();
	eq_process(samples, FRAMES, 2, 44100);
	assert(!memcmp(samples, original, sizeof(samples)));
	eq_set_band(5, 6);
	eq_set_enabled(true);
	changed_filters();
	eq_set_enabled(false);
	mseb_set_value(0, 60);
	mseb_set_enabled(true);
	changed_filters();
	mseb_set_enabled(false);
	peq_band_t band = {.on = true, .type = PEQ_TYPE_PEAK, .freq = 1000, .gain_tenths = 60, .q_cent = 100};
	peq_set_band(0, &band);
	peq_set_enabled(true);
	changed_filters();
	peq_set_enabled(false);
	soundfield_set_enabled(true);
	soundfield_set_width(150);
	copy();
	soundfield_process(samples, FRAMES, 2);
	soundfield_process_s32(wide, FRAMES, 2);
	assert(memcmp(samples, original, sizeof(samples)) && memcmp(wide, original_wide, sizeof(wide)));
	crossfeed_set_enabled(true);
	copy();
	crossfeed_process(samples, FRAMES, 2, 44100);
	crossfeed_process_s32(wide, FRAMES, 2, 44100);
	assert(memcmp(samples, original, sizeof(samples)) && memcmp(wide, original_wide, sizeof(wide)));
	mono_set_enabled(true);
	copy();
	mono_process(samples, FRAMES, 2);
	mono_process_s32(wide, FRAMES, 2);
	for (int i = 0; i < FRAMES; i++)
		assert(samples[i * 2] == samples[i * 2 + 1] && wide[i * 2] == wide[i * 2 + 1]);
	balance_set_enabled(true);
	balance_set_value(60);
	copy();
	balance_process(samples, FRAMES, 2);
	balance_process_s32(wide, FRAMES, 2);
	assert(memcmp(samples, original, sizeof(samples)) && memcmp(wide, original_wide, sizeof(wide)));
	song_metadata_t tagged = {.has_track_gain = true, .track_gain_db = -6, .track_peak = .5, .has_album_gain = true, .album_gain_db = -12, .album_peak = .5}, untagged = {0};
	replaygain_set_mode(REPLAYGAIN_TRACK);
	replaygain_load(&tagged);
	assert(replaygain_active() && replaygain_current_db() == -6);
	short frame[] = {10000, -10000};
	replaygain_process(frame, 1, 2);
	assert(frame[0] > 4900 && frame[0] < 5100);
	replaygain_load(&untagged);
	assert(!replaygain_active());
	frame[0] = 10000;
	replaygain_process(frame, 1, 1);
	assert(frame[0] == 10000);
	replaygain_set_mode(REPLAYGAIN_ALBUM);
	replaygain_load(&tagged);
	assert(replaygain_current_db() == -12);
	puts("Shared 16/32-bit PCM: EQ, PEQ, MSEB, soundfield, crossfeed, mono, balance and tagged/untagged ReplayGain change/bypass behavior passed; device routing, DSD and gapless timing remain separate acceptance.");
	return 0;
}

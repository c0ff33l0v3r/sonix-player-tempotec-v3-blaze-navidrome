// Exercise production open/write/hold/take/teardown with fake ALSA handles.
// No decoder, GUI, server or physical output is used by this fixture.
#ifndef HOST_GAPLESS_AUDIO_SOURCE
#define HOST_GAPLESS_AUDIO_SOURCE "src/system/audio/audio.c"
#endif
#include HOST_GAPLESS_AUDIO_SOURCE
#include <assert.h>

typedef struct {
	char name[160];
	snd_pcm_state_t state;
	bool closed;
	int16_t queued[128];
	size_t samples;
} fake_pcm_t;
static fake_pcm_t fake[16];
static int opens, closes, drops, starts, prepares, polls, route = 4;
static int delay_mode, prepare_error, open_error, params_error;
static bool fail_named, reject_writes;
static long virtual_us = 1000000;
static int64_t played_samples;
static fake_pcm_t *handle(snd_pcm_t *pcm) {
	fake_pcm_t *f = (fake_pcm_t *)pcm;
	assert(f >= fake && f < fake + 16 && !f->closed);
	return f;
}
int clock_gettime(clockid_t clock, struct timespec *ts) {
	(void)clock;
	ts->tv_sec = virtual_us / 1000000;
	ts->tv_nsec = virtual_us % 1000000 * 1000;
	return 0;
}
int usleep(useconds_t us) {
	virtual_us += us;
	return 0;
}
int pthread_setschedparam(pthread_t thread, int policy, const struct sched_param *param) { return EPERM; }
long config_get_int(const char *section, const char *key, long fallback) { return fallback; }
int alsa_output_key(void) { return route; }
void alsa_wait_output_reinit(void) {}
void auto_set_output(void) {}
int get_dac_dop(void) { return 0; }
bool alsa_mute_ramp(int rate) { return false; }
void alsa_suspend_restore(void) {}
bool bluetooth_audio_active(void) { return true; }
unsigned bluetooth_sink_rate(void) { return 0; }
const char *snd_pcm_name(snd_pcm_t *pcm) { return handle(pcm)->name; }
snd_pcm_state_t snd_pcm_state(snd_pcm_t *pcm) { return handle(pcm)->state; }
int snd_pcm_open(snd_pcm_t **out, const char *name, snd_pcm_stream_t stream, int mode) {
	assert(stream == SND_PCM_STREAM_PLAYBACK);
	if (open_error || (fail_named && strcmp(name, "default")))
		return -EINVAL;
	assert(opens < 16);
	fake_pcm_t *f = &fake[opens++];
	*f = (fake_pcm_t){.state = SND_PCM_STATE_PREPARED};
#ifdef HOST_GAPLESS_TARGET
	snprintf(f->name, sizeof(f->name), "%s", name);
#else
	snprintf(f->name, sizeof(f->name), "%s", !strncmp(name, "bluealsa", 8) ? "bluealsa:fixture" : "fixture plugin description");
#endif
	*out = (snd_pcm_t *)f;
	return 0;
}
int snd_pcm_close(snd_pcm_t *pcm) {
	handle(pcm)->closed = true;
	closes++;
	return 0;
}
int snd_pcm_start(snd_pcm_t *pcm) {
	handle(pcm)->state = SND_PCM_STATE_RUNNING;
	starts++;
	return 0;
}
int snd_pcm_prepare(snd_pcm_t *pcm) {
	prepares++;
	handle(pcm)->state = SND_PCM_STATE_PREPARED;
	return prepare_error;
}
int snd_pcm_drop(snd_pcm_t *pcm) {
	handle(pcm)->samples = 0;
	drops++;
	return 0;
}
int snd_pcm_drain(snd_pcm_t *pcm) {
#ifdef HOST_GAPLESS_TARGET
	fake_pcm_t *f = handle(pcm);
	played_samples += f->samples;
	f->samples = 0;
	return 0;
#else
	assert(!"host must not enter the blocking ALSA drain");
	return -EIO;
#endif
}
int snd_pcm_delay(snd_pcm_t *pcm, snd_pcm_sframes_t *frames) {
	fake_pcm_t *f = handle(pcm);
	polls++;
	if (delay_mode == 4)
		return -EIO;
	*frames = delay_mode == 1 ? 1024 : delay_mode == 2 ? (polls % 2 ? 3763 : 4000) : delay_mode == 3 ? 100000 - polls : (snd_pcm_sframes_t)(f->samples / 2);
	if (!delay_mode && f->samples) {
		played_samples += f->samples;
		f->samples = 0;
	}
	return 0;
}
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t frames) {
	fake_pcm_t *f = handle(pcm);
	if (reject_writes)
		return -EIO;
	assert(f->samples + frames * 2 <= 128);
	memcpy(f->queued + f->samples, buffer, frames * 2 * sizeof(int16_t));
	f->samples += frames * 2;
	return (snd_pcm_sframes_t)frames;
}
int snd_pcm_get_params(snd_pcm_t *pcm, snd_pcm_uframes_t *buffer, snd_pcm_uframes_t *period) {
	handle(pcm);
	*buffer = 32768;
	*period = 4096;
	return 0;
}
int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *p) {
	handle(pcm);
	return 0;
}
int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, snd_pcm_access_t a) { return 0; }
int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, snd_pcm_format_t f) { return 0; }
int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned c) { return 0; }
int snd_pcm_hw_params_set_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned r, int d) { return 0; }
int snd_pcm_hw_params_set_periods_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned *v, int *d) { return 0; }
int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, snd_pcm_uframes_t *v, int *d) { return 0; }
int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *p) { return params_error; }
int snd_pcm_hw_params_current(snd_pcm_t *pcm, snd_pcm_hw_params_t *p) { return 0; }
int snd_pcm_hw_params_get_rate(const snd_pcm_hw_params_t *p, unsigned *v, int *d) {
	*v = 44100;
	return 0;
}
int snd_pcm_hw_params_get_channels(const snd_pcm_hw_params_t *p, unsigned *v) {
	*v = 2;
	return 0;
}
int snd_pcm_hw_params_get_format(const snd_pcm_hw_params_t *p, snd_pcm_format_t *v) {
	*v = SND_PCM_FORMAT_S16_LE;
	return 0;
}
int snd_pcm_hw_params_get_period_size(const snd_pcm_hw_params_t *p, snd_pcm_uframes_t *v, int *d) {
	*v = 4096;
	return 0;
}
int snd_pcm_hw_params_get_buffer_size(const snd_pcm_hw_params_t *p, snd_pcm_uframes_t *v) {
	*v = 32768;
	return 0;
}
int snd_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *p) { return -ENOSYS; }
int snd_pcm_dump(snd_pcm_t *pcm, snd_output_t *out) { return -ENOSYS; }

static snd_pcm_t *open_output(void) {
	snd_pcm_uframes_t period = 0;
	snd_pcm_t *pcm = open_pcm_device(2, 44100, 16, &period);
	assert(pcm && period == 4096);
	return pcm;
}
static void reset(void) {
	assert(!gapless_holding());
	for (int i = 0; i < opens; i++)
		assert(fake[i].closed);
	memset(fake, 0, sizeof(fake));
	opens = closes = drops = starts = prepares = polls = 0;
	delay_mode = prepare_error = open_error = params_error = 0;
	fail_named = reject_writes = false;
	played_samples = 0;
	route = 4;
	audio_set_output_device(NULL);
	playback_thread = pthread_self();
	gapless_enabled = true;
	audio_command = AUDIO_CMD_PLAY;
	play_request = false;
	bt_converted_pcm = NULL;
	follow_pcm = NULL;
}
static void write_frames(snd_pcm_t **pcm, const int16_t *samples, size_t count) {
	snd_pcm_uframes_t period = 4096;
	assert(pcm_write_recover(pcm, samples, count / 2, 2, 44100, 16, &period) == (snd_pcm_sframes_t)(count / 2));
}
static void compatible(void) {
	reset();
	snd_pcm_t *pcm = open_output();
	const int16_t blocks[][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}};
	for (int i = 0; i < 3; i++) {
		write_frames(&pcm, blocks[i], 4);
		if (i == 2)
			break;
#ifndef HOST_GAPLESS_BASELINE
		assert(gapless_can_hold(pcm, true, false, false));
#endif
		gapless_hold(pcm, 2, 44100, 16, 4096, false);
		virtual_us += 2000;
		snd_pcm_uframes_t period = 0;
		assert(gapless_take(2, 44100, 16, &period) == pcm && period == 4096);
		assert(opens == 1 && !closes && !drops && !polls);
	}
	assert(handle(pcm)->samples == 12 && !memcmp(handle(pcm)->queued, blocks, sizeof(blocks)));
	gapless_hold(pcm, 2, 44100, 16, 4096, false);
	gapless_release();
	assert(played_samples == 12 && closes == 1 && !gapless_holding());
#ifdef HOST_GAPLESS_TARGET
	assert(!drops);
#else
	assert(drops == 1);
#endif
	gapless_release();
	assert(closes == 1);
	puts("Compatible output: two production write/hold/take joins preserve all ordered frames without drain/reopen.");
}
static void mismatches(void) {
	for (int i = 0; i < 5; i++) {
		reset();
		snd_pcm_t *pcm = open_output();
		gapless_hold(pcm, 2, 44100, 16, 4096, false);
		if (i == 3)
			audio_set_output_device("fixture-other-output");
		if (i == 4)
			route = 6;
		assert(!gapless_take(i == 0 ? 1 : 2, i == 1 ? 48000 : 44100, i == 2 ? 32 : 16, NULL));
		assert(closes == 1 && !gapless_holding());
	}
	reset();
	audio_set_output_device("fixture-unavailable-output");
	fail_named = true;
	snd_pcm_t *pcm = open_output();
	gapless_hold(pcm, 2, 44100, 16, 4096, false);
	assert(!gapless_take(2, 44100, 16, NULL)); // the requested output still differs from the actual fallback
	reset();
	audio_set_output_device("fixture-unavailable-output");
	fail_named = true;
	pcm = open_output();
	audio_set_output_device(NULL);
	gapless_hold(pcm, 2, 44100, 16, 4096, false);
	assert(gapless_take(2, 44100, 16, NULL) == pcm); // same actual fallback, even with a descriptive plugin name
	snd_pcm_close(pcm);
	puts("Format/device/route changes reject reuse; actual successful fallback identity remains authoritative.");
}
static void *external_open(void *arg) {
	snd_pcm_t *pcm = open_output();
	snd_pcm_close(pcm);
	return NULL;
}
static void external_identity(void) {
	reset();
	snd_pcm_t *pcm = open_output();
	gapless_hold(pcm, 2, 44100, 16, 4096, false);
	pthread_t thread;
	assert(!pthread_create(&thread, NULL, external_open, NULL));
	assert(!pthread_join(thread, NULL));
	assert(gapless_take(2, 44100, 16, NULL) == pcm);
	snd_pcm_close(pcm);
	puts("Another thread's successful open cannot replace playback PCM provenance.");
}
#ifndef HOST_GAPLESS_BASELINE
static void natural_policy(void) {
	reset();
	snd_pcm_t *pcm = open_output();
	const int16_t tail[] = {1, 2, 3, 4};
	write_frames(&pcm, tail, 4);
	assert(gapless_can_hold(pcm, true, false, false));
	audio_set_gapless(false);
	assert(!gapless_can_hold(pcm, true, false, false));
	assert(!gapless_holding());
	pcm_drain_bounded(pcm);
	snd_pcm_close(pcm);
	assert(played_samples == 4 && closes == 1);
	reset();
	pcm = open_output();
	assert(!gapless_can_hold(NULL, true, false, false));
	assert(!gapless_can_hold(pcm, false, false, false));
	assert(!gapless_can_hold(pcm, true, true, false));
	assert(!gapless_can_hold(pcm, true, false, true));
	snd_pcm_close(pcm);
	puts("Shared WAV/decoder eligibility: Gapless off drains the valid tail; pause/skip/DoP cannot claim a natural hold.");
}
#endif
static void transport_and_failures(void) {
	reset();
	snd_pcm_t *pcm = open_output();
	assert(!skip_hold_prepare(pcm, false, false));
	play_request = true;
	assert(!skip_hold_prepare(pcm, true, false)); // DoP exclusion
	assert(!skip_hold_prepare(pcm, false, true)); // paused exclusion
	assert(skip_hold_prepare(pcm, false, false));
	assert(drops == 1 && prepares == 1);
	gapless_hold(pcm, 2, 44100, 16, 4096, true);
	assert(gapless_take(2, 44100, 16, NULL) == pcm);
	play_request = false;
	audio_stop();
	assert(!play_request && audio_command == AUDIO_CMD_STOP && !audio_take_completion());
	gapless_hold(pcm, 2, 44100, 16, 4096, true);
	gapless_release();
	assert(closes == 1 && !polls); // empty skip output never drains
	reset();
	pcm = open_output();
	play_request = true;
	prepare_error = -EIO;
	assert(!skip_hold_prepare(pcm, false, false));
	snd_pcm_close(pcm);
	reset();
	open_error = 1;
	snd_pcm_uframes_t period;
	assert(!open_pcm_device(2, 44100, 16, &period) && !opens && !closes);
	reset();
	params_error = -EINVAL;
	assert(!open_pcm_device(2, 44100, 16, &period) && opens == 1 && closes == 1);
	reset();
	pcm = open_output();
	reject_writes = true;
	open_error = 1;
	assert(pcm_write_recover(&pcm, (int16_t[]){1, 2}, 1, 2, 44100, 16, &period) == -EIO);
	assert(!pcm && closes == 1 && !gapless_holding());
	puts("Skip/Stop/pause/DoP and open/prepare/write failures preserve intent and single-owner cleanup.");
}
static void bluetooth(void) {
	reset();
	audio_set_output_device("bluealsa:fixture");
	snd_pcm_t *pcm = open_output();
	assert(!skip_hold_prepare(pcm, false, false));
#ifndef HOST_GAPLESS_BASELINE
	assert(!gapless_can_hold(pcm, true, false, false));
#endif
	gapless_hold(pcm, 2, 44100, 16, 4096, false);
	assert(!gapless_take(2, 44100, 16, NULL)); // not RUNNING
	reset();
	audio_set_output_device("bluealsa:fixture");
	pcm = open_output();
	handle(pcm)->state = SND_PCM_STATE_RUNNING;
	delay_mode = 1; // 1024 frames is less than the 120-ms Bluetooth reserve
#ifndef HOST_GAPLESS_BASELINE
	assert(!gapless_can_hold(pcm, true, false, false));
#endif
	gapless_hold(pcm, 2, 44100, 16, 4096, false);
	assert(!gapless_take(2, 44100, 16, NULL));
	puts("Bluetooth stopped/low-queue output remains ineligible for reuse.");
}
static void drains_and_pacing(void) {
	for (int mode = 0; mode <= 4; mode++) {
		reset();
		snd_pcm_t *pcm = open_output();
		delay_mode = mode;
#ifdef HOST_GAPLESS_TARGET
		// Real target hardware keeps its existing drain; bounded polling is
		// tested on the target Bluetooth path instead.
		snprintf(handle(pcm)->name, sizeof(handle(pcm)->name), "bluealsa:fixture");
#endif
		gapless_hold(pcm, 2, 44100, 16, 4096, false);
		long before = virtual_us;
		assert(!audio_play("/fixture/newer.flac"));
		gapless_release();
		assert(closes == 1 && drops == 1 && virtual_us - before <= DRAIN_TIMEOUT_MS * 1000);
		assert(play_request && audio_command == AUDIO_CMD_PLAY && !strcmp(current_filepath, "/fixture/newer.flac"));
		if (mode == 1)
			assert(virtual_us - before <= (DRAIN_STALL_MS + 20) * 1000);
		if (mode == 2 || mode == 3)
			assert(virtual_us - before == DRAIN_TIMEOUT_MS * 1000);
	}
	long before = virtual_us;
	host_pace(4410, 44100);
	host_pace(0, 44100);
#ifdef HOST_GAPLESS_TARGET
	assert(virtual_us == before);
#else
	assert(virtual_us - before == 100000);
#endif
	puts("Missing-next/retired-output drains remain finite with a newer request retained; existing host pacing stays bounded.");
}
int main(void) {
	compatible();
	mismatches();
	external_identity();
#ifndef HOST_GAPLESS_BASELINE
	natural_policy();
#endif
	transport_and_failures();
	bluetooth();
	drains_and_pacing();
	puts("Production audio handoff checks passed; no actual audio device opened.");
	return 0;
}

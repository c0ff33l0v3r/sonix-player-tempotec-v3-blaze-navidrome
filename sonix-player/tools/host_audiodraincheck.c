// Compile the production audio module and exercise only its end-of-track drain.
// The fake PCM fails immediately if the blocking drain path is entered.
#include "src/system/audio/audio.c"
#include <assert.h>

static int mode, delay_calls, drops, waits;
static bool delay_error;
const char *snd_pcm_name(snd_pcm_t *pcm) {
	(void)pcm;
	return "default";
}
int snd_pcm_drain(snd_pcm_t *pcm) {
	(void)pcm;
	assert(!"host must not enter an unbounded ALSA drain");
	return -1;
}
int snd_pcm_delay(snd_pcm_t *pcm, snd_pcm_sframes_t *frames) {
	(void)pcm;
	delay_calls++;
	if (delay_error)
		return -EIO;
	*frames = mode == 0 ? (delay_calls < 4 ? 4 - delay_calls : 0) : mode == 1 ? 1024 : 100000 - delay_calls;
	return 0;
}
int snd_pcm_drop(snd_pcm_t *pcm) {
	(void)pcm;
	drops++;
	return 0;
}
int usleep(useconds_t usec) {
	assert(usec == 20000);
	waits++;
	return 0;
}
static void reset(int scenario) {
	mode = scenario;
	delay_calls = drops = waits = 0;
	delay_error = false;
}
int main(void) {
	snd_pcm_t *pcm = (snd_pcm_t *)(uintptr_t)1;
	reset(0);
	pcm_drain_bounded(NULL);
	assert(!delay_calls && !drops);
	pcm_drain_bounded(pcm);
	assert(delay_calls == 4 && drops == 1 && waits == 3);
	track_completed = true;
	assert(audio_take_completion() && !audio_take_completion());
	assert(!audio_play("/fixture/next.flac"));
	reset(1);
	pcm_drain_bounded(pcm);
	assert(drops == 1 && waits * 20 <= DRAIN_STALL_MS + 20);
	assert(play_request && audio_command == AUDIO_CMD_PLAY && playback_status == AUDIO_STATUS_PLAYING && !strcmp(current_filepath, "/fixture/next.flac"));
	reset(2);
	pcm_drain_bounded(pcm);
	assert(drops == 1 && waits * 20 <= DRAIN_TIMEOUT_MS);
	reset(0);
	delay_error = true;
	pcm_drain_bounded(pcm);
	assert(drops == 1 && !waits);
	puts("Host end-of-track drain: advancing, stalled, deadline, failed PCM and pending next-track preservation return without blocking; no audio device opened.");
	return 0;
}

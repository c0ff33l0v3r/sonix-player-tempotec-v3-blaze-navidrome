// Loaded locally by the artwork fixture, with hidden symbols and -Bsymbolic:
// the fake clock/ALSA functions must never interpose the HTTP/LVGL process.
// Exercises production pcm_write_recover with the existing inert PCM model.
#define main unused_gapless_main
#include "host_gaplesscheck.c"
#undef main
static snd_pcm_t *art_pcm;
static unsigned art_steps;
__attribute__((visibility("default"))) void art_pcm_begin(void) {
	reset();
	art_pcm = open_output();
	art_steps = 0;
}
__attribute__((visibility("default"))) void art_pcm_step(void) {
	int16_t samples[4];
	for (int i = 0; i < 4; i++) samples[i] = (int16_t)(art_steps * 4 + i);
	write_frames(&art_pcm, samples, 4);
	assert(handle(art_pcm)->samples == 4 && !memcmp(handle(art_pcm)->queued, samples, sizeof(samples)));
	snd_pcm_sframes_t frames;
	assert(!snd_pcm_delay(art_pcm, &frames) && frames == 2);
	assert(!handle(art_pcm)->samples);
	art_steps++;
}
__attribute__((visibility("default"))) void art_pcm_end(void) {
	assert(art_steps && played_samples == (int64_t)art_steps * 4);
	assert(!trace_counts[D_UNDERRUN] && !drops);
	assert(!snd_pcm_close(art_pcm));
	puts("production PCM continuity: ordered samples retained during artwork admission/cancellation; no audio device");
}

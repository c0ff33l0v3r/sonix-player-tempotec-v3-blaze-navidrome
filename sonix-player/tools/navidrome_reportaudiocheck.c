#define main original_drain_main
#include "host_audiodraincheck.c"
#undef main
int main(void) {
	audio_observation_t s;
	assert(!pthread_mutex_lock(&audio_mutex));
	assert(!audio_observe(&s)); // never waits behind decoder/output work
	pthread_mutex_unlock(&audio_mutex);
	audio_play("/fixture/a.flac");
	assert(audio_observe(&s));
	unsigned first = s.serial, discontinuity = s.discontinuity;
	assert(!strcmp(s.path, "/fixture/a.flac"));
	audio_seek(0);
	assert(audio_observe(&s) && s.serial == first && s.discontinuity != discontinuity);
	audio_pause();
	audio_resume();
	assert(audio_observe(&s) && s.serial == first);
	// The real suspend-resume rebuild is a continuation of this listen.
	pthread_mutex_lock(&audio_mutex);
	restart_fresh_pending = true;
	restart_fresh_pos = 12;
	snprintf(restart_fresh_path, sizeof(restart_fresh_path), "/fixture/a.flac");
	pthread_mutex_unlock(&audio_mutex);
	audio_resume();
	assert(audio_observe(&s) && s.serial == first && s.position == 12);
	audio_play("/fixture/a.flac");
	assert(audio_observe(&s) && s.serial != first);
	first = s.serial;
	audio_stop();
	assert(audio_observe(&s) && s.serial == first && s.status == AUDIO_STATUS_STOPPED);
	audio_play_paused("/fixture/a.flac", 0);
	assert(audio_observe(&s) && s.serial != first && s.status == AUDIO_STATUS_PAUSED);
	puts("real audio observation: try-lock, same-file repeat, pause/seek/stop and suspend continuation: passed; no output device opened");
	return 0;
}

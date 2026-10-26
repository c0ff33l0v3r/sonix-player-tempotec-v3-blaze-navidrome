#include "navidromereports.h"
#include "src/system/lastfm/lastfm.h"
#include <math.h>
#include <string.h>

void nd_listen_start(nd_listen_tracker_t *t, const nd_listen_t *e, const audio_observation_t *s, double now, double duration) {
	memset(t, 0, sizeof(*t));
	t->event = *e;
	t->audio_serial = s->serial;
	t->discontinuity = s->discontinuity;
	t->last_tick = now;
	t->position = s->position;
	t->duration = duration;
	t->active = true;
	t->playing = s->status == AUDIO_STATUS_PLAYING;
	t->begun = t->playing;
	if (!t->begun)
		t->event.when = 0;
}
bool nd_listen_sample(nd_listen_tracker_t *t, const audio_observation_t *s, double now, int64_t wall, nd_listen_t *out) {
	if (!t->active || s->serial != t->audio_serial) {
		t->active = false;
		return false;
	}
	double step = now - t->last_tick, moved = s->position - t->position;
	bool playing = s->status == AUDIO_STATUS_PLAYING;
	if (playing && !t->begun) {
		t->begun = true;
		t->event.boot = now;
		t->event.when = wall;
	}
	// Completion can supply the last advancing sample; pause/seek discontinuities
	// cannot. A nonadvancing decoder (including buffering) contributes nothing.
	if (t->playing && (playing || s->status == AUDIO_STATUS_STOPPED) && t->discontinuity == s->discontinuity && step > 0 && step < 5 && isfinite(moved) && moved > 0 && moved <= step * s->rate + .25)
		t->listened += fmin(step, moved / fmax(s->rate, .01));
	t->last_tick = now;
	t->position = s->position;
	t->discontinuity = s->discontinuity;
	t->playing = playing;
	if (isfinite(s->duration) && s->duration > 0)
		t->duration = s->duration;
	if (t->begun && !t->event.when && wall > 0 && now >= t->event.boot) {
		double elapsed = now - t->event.boot;
		if (elapsed <= wall / 1000.0)
			t->event.when = wall - (int64_t)(elapsed * 1000);
	}
	// Undated qualified events retain their same-process boot start in the
	// bounded worker queue; a later valid clock can date even an ended session.
	bool qualified = !t->qualified && lastfm_play_counts(t->duration, t->listened);
	if (qualified) {
		t->qualified = true;
		*out = t->event;
	}
	if (s->status == AUDIO_STATUS_STOPPED)
		t->active = false;
	return qualified;
}

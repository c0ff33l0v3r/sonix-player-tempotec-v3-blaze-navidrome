#include "navidromelyrics.h"
#include "navidromecrypto.h"
#include "navidromeplay.h"
#include "src/gui/shell/gui.h"
#include "src/system/playback/playlist.h"
#include "streamturn.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	nd_lyric_result_t result;
	nd_lyric_done_cb done;
	void *user;
	char *text;
	int choice;
	unsigned token;
	bool durable;
} lyric_job_t;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static pthread_t thread;
static bool started, stopped, saving;
static unsigned serial;
static lyric_job_t *pending, *delivery;

bool nd_lyrics_same(const nd_lyric_target_t *a, const nd_lyric_target_t *b) { return a->queue == b->queue && a->context.profile_epoch == b->context.profile_epoch && a->context.card_epoch == b->context.card_epoch && !strcmp(a->library.scope, b->library.scope) && !strcmp(a->id, b->id) && !strcmp(a->reference, b->reference); }
bool nd_lyrics_target(const char *ref, nd_lyric_target_t *out) {
	memset(out, 0, sizeof(*out));
	nd_playrow_t track;
	if (!ref || !*ref || (!navidromeplay_reference(ref) && !nd_audio_source(ref)) || (!navidromeplay_track(ref, &track, &out->library) && !navidromeplay_lookup(ref, &track, &out->library)))
		return false;
	nd_context_t c;
	if (!navidrome_context(&c, false))
		return false;
	out->context.profile_epoch = c.profile_epoch;
	out->context.card_epoch = c.card_epoch;
	out->context.playback_network_epoch = c.playback_network_epoch;
	nd_wipe(&c, sizeof(c));
	if (out->library.card_epoch != out->context.card_epoch || !track.row.id[0])
		return false;
	snprintf(out->id, sizeof(out->id), "%s", track.row.id);
	if (!playlist_current_path(out->reference, sizeof(out->reference)))
		return false;
	out->queue = playlist_identity();
	snprintf(out->title, sizeof(out->title), "%.*s", 255, track.row.title);
	snprintf(out->artist, sizeof(out->artist), "%.*s", 255, track.row.artist);
	snprintf(out->album, sizeof(out->album), "%.*s", 255, track.album);
	out->duration = track.row.duration;
	return true;
}
static void release(lyric_job_t *j) {
	if (!j)
		return;
	nd_lyrics_free(&j->result.set);
	lrclib_free(j->result.records, j->result.count);
	free(j->text);
	free(j);
}
static bool current(void *user) {
	lyric_job_t *j = user;
	pthread_mutex_lock(&mutex);
	bool ok = !stopped && (j->durable || j->token == serial);
	pthread_mutex_unlock(&mutex);
	return ok && navidrome_playback_current(&j->result.target.context, false);
}
static bool network(void *user) {
	bool idle;
	streamturn_observe(&idle);
	lyric_job_t *j = user;
	return idle && !navidromeplay_preparing() && current(j) && (j->result.action == ND_LYRIC_SEARCH || navidrome_playback_current(&j->result.target.context, true));
}
static bool storage_current(void *user) { return !navidromeplay_preparing() && current(user); }
static const char *store(lyric_job_t *j, nd_lyric_action_t action) {
	nd_lyric_target_t *t = &j->result.target;
	char path[1200];
	if (navidromeplay_preparing())
		return "nd_busy";
	if (!current(j) || !nd_lyrics_path(&t->library, path) || (action != ND_LYRIC_LOCAL && !t->library.writable) || !navidrome_playback_begin(&t->context))
		return "nd_lyrics_storage";
	const char *error = nd_lyrics_store(path, t->library.scope, t->id, action, &j->result.set, j->text, j->choice, storage_current, j);
	navidrome_cache_end();
	return error;
}
static void process(lyric_job_t *j) {
	nd_lyric_result_t *r = &j->result;
	if (!current(j)) {
		r->error = "nd_sync_cancelled";
		return;
	}
	if (r->action == ND_LYRIC_SEARCH) {
		lrclib_result_t status = lrclib_search_cancel(r->target.title, r->target.artist, r->target.album, r->target.duration, network, j, &r->records, &r->count);
		r->error = status == LRCLIB_OK ? NULL : status == LRCLIB_INSTRUMENTAL ? "lyrics_instrumental" : status == LRCLIB_NOT_FOUND ? "lyrics_not_found" : status == LRCLIB_NO_TAGS ? "lyrics_no_tags" : status == LRCLIB_LIMIT ? "nd_lyrics_limit" : "lyrics_download_failed";
		return;
	}
	if (r->action == ND_LYRIC_APPLY || r->action == ND_LYRIC_SELECT) {
		r->error = store(j, r->action);
		return;
	}
	if (r->action == ND_LYRIC_SERVER) {
		r->error = store(j, ND_LYRIC_SERVER);
		if (!r->error)
			return;
		nd_lyrics_free(&r->set);
	} else {
		r->error = store(j, ND_LYRIC_LOCAL);
		if (r->action == ND_LYRIC_LOCAL || (r->set.count && r->action != ND_LYRIC_REFRESH))
			return;
		nd_lyrics_free(&r->set);
	}
	nd_context_t secret;
	if (!network(j) || !navidrome_context(&secret, true)) {
		r->error = navidrome_playback_current(&r->target.context, true) ? "nd_busy" : "nd_lyrics_offline";
		return;
	}
	// A reconnect can retire network work while preserving its local source.
	if (secret.profile_epoch != r->target.context.profile_epoch || secret.card_epoch != r->target.context.card_epoch || secret.playback_network_epoch != r->target.context.playback_network_epoch) {
		nd_wipe(&secret, sizeof(secret));
		r->error = "nd_lyrics_offline";
		return;
	}
	nd_document_t doc = {0};
	int status = 0;
	r->error = nd_api_lyrics(secret.host, secret.user, secret.password, r->target.id, network, j, &doc, &status);
	nd_wipe(&secret, sizeof(secret));
	if (r->error) {
		if (status == 404 || status == 501 || !strcmp(r->error, "nd_api_error"))
			r->error = "nd_lyrics_unsupported";
	} else
		r->error = nd_lyrics_decode(&doc, &r->set);
	nd_api_free(&doc);
	if (!r->error && current(j)) {
		const char *error = store(j, r->action == ND_LYRIC_SERVER ? ND_LYRIC_SERVER : ND_LYRIC_VIEW);
		if (error) {
			if (r->action == ND_LYRIC_SERVER) {
				nd_lyrics_free(&r->set);
				r->error = error;
			} else
				r->set.unsaved = true;
		}
	}
}
static void deliver(void *unused) {
	(void)unused;
	pthread_mutex_lock(&mutex);
	lyric_job_t *j = delivery;
	delivery = NULL;
	bool valid = j && !stopped && j->token == serial;
	pthread_mutex_unlock(&mutex);
	if (j) {
		if (valid && navidrome_playback_current(&j->result.target.context, false) && j->done)
			j->done(&j->result, j->user);
		release(j);
	}
}
void nd_lyrics_poll(void) { deliver(NULL); }
static void *run(void *unused) {
	(void)unused;
	for (;;) {
		pthread_mutex_lock(&mutex);
		while (!pending && !stopped)
			pthread_cond_wait(&ready, &mutex);
		if (stopped) {
			pthread_mutex_unlock(&mutex);
			return NULL;
		}
		lyric_job_t *j = pending;
		pending = NULL;
		saving = j->durable;
		pthread_mutex_unlock(&mutex);
		process(j);
		pthread_mutex_lock(&mutex);
		saving = false;
		if (stopped || j->token != serial) {
			pthread_mutex_unlock(&mutex);
			release(j);
			continue;
		}
		lyric_job_t *old = delivery;
		delivery = j;
		pthread_mutex_unlock(&mutex);
		release(old);
		(void)gui_post(deliver, NULL);
	}
}
unsigned nd_lyrics_submit(const nd_lyric_target_t *target, nd_lyric_action_t action, const char *text, int choice, nd_lyric_done_cb done, void *user) {
	if (!target || !target->id[0] || (text && strnlen(text, ND_LYRIC_TEXT + 1) > ND_LYRIC_TEXT))
		return 0;
	lyric_job_t *j = calloc(1, sizeof(*j));
	if (!j)
		return 0;
	j->result.target = *target;
	j->result.action = action;
	j->done = done;
	j->user = user;
	j->choice = choice;
	j->durable = action == ND_LYRIC_APPLY || action == ND_LYRIC_SERVER || action == ND_LYRIC_SELECT;
	if (text && !(j->text = strdup(text))) {
		release(j);
		return 0;
	}
	pthread_mutex_lock(&mutex);
	if (stopped && !started)
		stopped = false;
	if (stopped || (pending && pending->durable) || (saving && j->durable)) {
		pthread_mutex_unlock(&mutex);
		release(j);
		return 0;
	}
	if (!started) {
		pthread_attr_t attr;
		int rc = pthread_attr_init(&attr);
		bool initialized = !rc;
		if (!rc)
			rc = pthread_attr_setstacksize(&attr, 256 * 1024);
		if (!rc)
			rc = pthread_create(&thread, &attr, run, NULL);
		if (initialized)
			pthread_attr_destroy(&attr);
		if (rc) {
			pthread_mutex_unlock(&mutex);
			release(j);
			return 0;
		}
		started = true;
		navidrome_set_lyrics_shutdown(nd_lyrics_shutdown);
	}
	lyric_job_t *old = pending;
	pending = j;
	j->token = ++serial;
	unsigned token = j->token;
	pthread_cond_signal(&ready);
	pthread_mutex_unlock(&mutex);
	release(old);
	return token;
}
void nd_lyrics_cancel(unsigned token) {
	pthread_mutex_lock(&mutex);
	if (token && token == serial)
		serial++;
	pthread_cond_signal(&ready);
	pthread_mutex_unlock(&mutex);
}
void nd_lyrics_shutdown(void) {
	pthread_mutex_lock(&mutex);
	stopped = true;
	serial++;
	bool join = started;
	started = false;
	pthread_cond_signal(&ready);
	pthread_mutex_unlock(&mutex);
	if (join)
		pthread_join(thread, NULL);
	pthread_mutex_lock(&mutex);
	lyric_job_t *p = pending, *d = delivery;
	pending = delivery = NULL;
	pthread_mutex_unlock(&mutex);
	release(p);
	release(d);
}

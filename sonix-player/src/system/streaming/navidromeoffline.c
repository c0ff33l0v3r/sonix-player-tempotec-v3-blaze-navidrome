#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "navidromeoffline.h"
#include "navidromeaudio.h"
#include "navidromedownloadstore.h"
#include "navidromeplay.h"
#include "navidromeplayliststore.h"
#include "src/gui/shell/gui.h"
#include "streamturn.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>

#define OFF_CACHE 128
#define OFF_STACK (256 * 1024)
typedef struct {
	nd_off_request_t request;
	nd_context_t context; // epoch fields only; no credentials
	unsigned token;
	nd_off_done_cb done;
	void *user;
	bool pinned;
	struct {
		int offset, total, hits, misses;
		bool finished;
	} rows[ND_WINDOW];
	struct {
		char id[ND_ID_MAX];
		int state;
	} cache[OFF_CACHE];
	unsigned next;
} off_job_t;
static pthread_mutex_t off_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t off_ready = PTHREAD_COND_INITIALIZER;
static pthread_t off_thread;
static bool off_started, off_stop, off_active;
static unsigned off_serial;
static off_job_t *off_pending;
// Fixed delivery mailbox; a GUI queue rejection never leaks or loses results.
static struct {
	unsigned token;
	nd_off_state_t state[ND_WINDOW];
	bool ready[ND_WINDOW];
	nd_context_t context;
	nd_off_done_cb done;
	void *user;
} off_delivery;

static void off_free(off_job_t *j) {
	if (!j)
		return;
	if (j->pinned)
		nd_store_release_source(&j->request.library);
	free(j);
}
static bool off_current(void *value) {
	off_job_t *j = value;
	pthread_mutex_lock(&off_lock);
	bool ok = !off_stop && j->token == off_serial;
	pthread_mutex_unlock(&off_lock);
	return ok && navidrome_playback_scope(&j->context, &j->request.library);
}
static bool off_io_current(void *value) {
	bool idle;
	streamturn_observe(&idle);
	return idle && !navidromeplay_preparing() && off_current(value);
}
static void off_deliver(void *unused) {
	(void)unused;
	pthread_mutex_lock(&off_lock);
	unsigned token = off_delivery.token;
	nd_off_done_cb done = off_delivery.done;
	void *user = off_delivery.user;
	nd_context_t context = off_delivery.context;
	nd_off_state_t state[ND_WINDOW];
	bool ready[ND_WINDOW];
	memcpy(state, off_delivery.state, sizeof(state));
	memcpy(ready, off_delivery.ready, sizeof(ready));
	memset(off_delivery.ready, 0, sizeof(off_delivery.ready));
	bool valid = !off_stop && token == off_serial;
	pthread_mutex_unlock(&off_lock);
	if (valid && done && navidrome_playback_current(&context, false))
		for (int i = 0; i < ND_WINDOW; i++)
			if (ready[i])
				done(token, i, state[i], user);
}
void nd_offline_poll(void) { off_deliver(NULL); }
static void off_publish(off_job_t *j, int row, nd_off_state_t state) {
	bool post = false;
	pthread_mutex_lock(&off_lock);
	if (!off_stop && j->token == off_serial) {
		bool queued = false;
		for (int i = 0; i < ND_WINDOW; i++)
			queued |= off_delivery.ready[i];
		off_delivery.token = j->token;
		off_delivery.done = j->done;
		off_delivery.user = j->user;
		off_delivery.context = j->context;
		off_delivery.state[row] = state;
		off_delivery.ready[row] = true;
		post = !queued;
	}
	pthread_mutex_unlock(&off_lock);
	if (post)
		(void)gui_post(off_deliver, NULL);
}

// All handles/leases are scoped to this finite membership slice.
static bool off_members(off_job_t *j, int row, nd_row_t rows[ND_WINDOW], int *count, int *total) {
	const nd_off_request_t *q = &j->request;
	const nd_off_item_t *item = &q->items[row];
	if (!off_current(j) || !navidrome_playback_begin(&j->context))
		return false;
	bool ok = false;
	if (q->kind == ND_OFF_PLAYLIST) {
		nd_playlist_store_t store = {0};
		char path[1200];
		nd_playlist_entry_t entries[ND_WINDOW];
		ok = nd_pl_path(&q->library, path) && nd_pl_open(&store, path, q->library.scope, false) && nd_pl_play_window(&store, item->playlist, item->revision, j->rows[row].offset, entries, count, total);
		if (ok)
			for (int i = 0; i < *count; i++)
				rows[i] = entries[i].track;
		nd_pl_close(&store);
	} else if (q->kind == ND_OFF_ALBUM && q->library.ready) {
		nd_store_t store = {0};
		nd_playrow_t album;
		nd_query_t query = {.kind = ND_TRACKS, .filter_kind = ND_ALBUMS, .favorites = q->library.favorites};
		memcpy(query.filter, item->id, sizeof(query.filter));
		ok = nd_store_open_source(&store, q->library.path, q->library.scope, false, false, q->library.favorites) && nd_store_metadata(&store, q->library.generation, ND_ALBUMS, item->id, &album) && !album.row.details && nd_store_query(&store, q->library.generation, &query, j->rows[row].offset, rows, count, total);
		nd_store_close(&store);
	}
	navidrome_cache_end();
	return ok && off_current(j);
}

static int off_track(off_job_t *j, const char *id) {
	for (int i = 0; i < OFF_CACHE; i++)
		if (j->cache[i].id[0] && !strcmp(j->cache[i].id, id))
			return j->cache[i].state;
	if (!off_io_current(j) || !navidrome_playback_begin(&j->context))
		return -1;
	nd_dl_store_t store = {0};
	char root[ND_DL_PATH], path[1200];
	struct stat st;
	int result = -1;
	if (nd_dl_paths(&j->request.library, root, path)) {
		if (lstat(path, &st) && errno == ENOENT)
			result = 0;
		else if (nd_dl_open(&store, &j->request.library, false))
			result = nd_dl_probe(&store, id, nd_audio_valid, off_io_current, j);
	}
	nd_dl_close(&store);
	navidrome_cache_end();
	if (result >= 0 && off_io_current(j)) {
		unsigned slot = j->next++ % OFF_CACHE;
		snprintf(j->cache[slot].id, sizeof(j->cache[slot].id), "%s", id);
		j->cache[slot].state = result;
	}
	return result;
}

static void off_step(off_job_t *j, int row) {
	nd_row_t rows[ND_WINDOW];
	int count = 0, total = 0;
	nd_off_state_t state = ND_OFF_UNKNOWN;
	if (j->request.kind == ND_OFF_TRACK) {
		int found = j->request.items[row].id[0] ? off_track(j, j->request.items[row].id) : -1;
		if (found < 0 && off_current(j) && !off_io_current(j))
			return; // preparation interrupted validation; retain settled UI and retry later
		state = found < 0 ? ND_OFF_UNKNOWN : found ? ND_OFF_COMPLETE : ND_OFF_NONE;
	} else if (off_members(j, row, rows, &count, &total) && (!j->rows[row].offset || j->rows[row].total == total)) {
		j->rows[row].total = total;
		int hits = j->rows[row].hits, misses = j->rows[row].misses;
		for (int i = 0; i < count; i++) {
			int found = off_track(j, rows[i].id);
			if (found < 0) {
				if (off_current(j) && !off_io_current(j)) {
					j->rows[row].hits = hits;
					j->rows[row].misses = misses;
					return;
				}
				goto finish;
			}
			if (found)
				j->rows[row].hits++;
			else
				j->rows[row].misses++;
		}
		j->rows[row].offset += count;
		if (count)
			return; // terminal empty window revalidates expected membership
		if (j->rows[row].offset == total)
			state = !j->rows[row].hits ? ND_OFF_NONE : j->rows[row].misses ? ND_OFF_PARTIAL : ND_OFF_COMPLETE;
	}
finish:
	j->rows[row].finished = true;
	off_publish(j, row, state);
}
static void *off_run(void *unused) {
	(void)unused;
	(void)setpriority(PRIO_PROCESS, 0, 10);
	for (;;) {
		pthread_mutex_lock(&off_lock);
		while (!off_stop && !off_pending)
			pthread_cond_wait(&off_ready, &off_lock);
		off_job_t *j = off_pending;
		off_pending = NULL;
		bool stop = off_stop;
		pthread_mutex_unlock(&off_lock);
		if (stop) {
			off_free(j);
			return NULL;
		}
		bool remaining = true;
		while (remaining && off_current(j)) {
			remaining = false;
			for (int i = 0; i < j->request.count && off_current(j); i++) {
				if (j->rows[i].finished)
					continue;
				bool idle;
				streamturn_observe(&idle);
				if (idle && !navidromeplay_preparing())
					off_step(j, i);
				remaining |= !j->rows[i].finished;
			}
			if (remaining) {
				// No leases/handles held while yielding. Cancellation is checked every 10 ms.
				struct timespec pause = {.tv_nsec = 10000000};
				nanosleep(&pause, NULL);
			}
		}
		pthread_mutex_lock(&off_lock);
		if (j->token == off_serial)
			off_active = false;
		pthread_mutex_unlock(&off_lock);
		off_free(j);
	}
}
unsigned nd_offline_submit(const nd_off_request_t *q, nd_off_done_cb done, void *user) {
	if (!q || q->count < 0 || q->count > ND_WINDOW || q->kind < ND_OFF_TRACK || q->kind > ND_OFF_PLAYLIST)
		return 0;
	nd_context_t secret = {0};
	if (!navidrome_context(&secret, false)) {
		nd_wipe(&secret, sizeof(secret));
		return 0;
	}
	off_job_t *j = calloc(1, sizeof(*j));
	if (j) {
		j->request = *q;
		j->context.profile_epoch = secret.profile_epoch;
		j->context.card_epoch = secret.card_epoch;
		j->done = done;
		j->user = user;
	}
	nd_wipe(&secret, sizeof(secret));
	if (!j)
		return 0;
	if (!navidrome_playback_scope(&j->context, &q->library)) {
		off_free(j);
		return 0;
	}
	if (q->kind == ND_OFF_ALBUM && q->library.ready) {
		j->pinned = navidrome_playback_pin_source(&q->library);
		if (!j->pinned) {
			off_free(j);
			return 0;
		}
	}
	pthread_mutex_lock(&off_lock);
	if (!off_started) {
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, OFF_STACK);
		off_stop = false;
		off_started = pthread_create(&off_thread, &attr, off_run, NULL) == 0;
		pthread_attr_destroy(&attr);
	}
	if (!off_started || off_stop) {
		pthread_mutex_unlock(&off_lock);
		off_free(j);
		return 0;
	}
	off_job_t *old = off_pending;
	j->token = ++off_serial;
	off_pending = j;
	off_active = true;
	memset(&off_delivery, 0, sizeof(off_delivery));
	pthread_cond_signal(&off_ready);
	unsigned token = j->token;
	pthread_mutex_unlock(&off_lock);
	off_free(old);
	return token;
}
void nd_offline_cancel(unsigned token) {
	pthread_mutex_lock(&off_lock);
	if (token && token == off_serial) {
		off_serial++;
		off_active = false;
	}
	pthread_cond_signal(&off_ready);
	pthread_mutex_unlock(&off_lock);
}
bool nd_offline_busy(unsigned token) {
	pthread_mutex_lock(&off_lock);
	bool busy = token && token == off_serial && off_active;
	pthread_mutex_unlock(&off_lock);
	return busy;
}
void nd_offline_shutdown(void) {
	pthread_mutex_lock(&off_lock);
	off_stop = true;
	off_serial++;
	bool join = off_started;
	pthread_cond_signal(&off_ready);
	pthread_mutex_unlock(&off_lock);
	if (join)
		pthread_join(off_thread, NULL);
	pthread_mutex_lock(&off_lock);
	off_started = off_active = false;
	memset(&off_delivery, 0, sizeof(off_delivery));
	pthread_mutex_unlock(&off_lock);
}

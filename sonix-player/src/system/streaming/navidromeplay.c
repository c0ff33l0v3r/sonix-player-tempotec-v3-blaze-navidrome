#include "navidromeplay.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/nowplaying/trackmenu.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/toast.h"
#include "src/gui/streaming/navidromeart.h"
#include "src/system/core/utils.h"
#include "src/system/decode/growfile.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/streamturn.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct source {
	unsigned refs, token;
	nd_context_t context;
	nd_library_t library;
	nd_query_t query;
	char selected[ND_ID_MAX];
	int64_t *handles;
	int count, start;
	int window_start, window_count;
	nd_playrow_t window[ND_WINDOW * 2];
	lv_obj_t *origin;
} source_t;
typedef struct {
	source_t *source;
	unsigned serial;
	int entry;
	bool initial, foreground;
	char ref[80];
} job_t;
typedef struct {
	job_t job;
	char path[512];
	nd_playrow_t row;
	song_metadata_t metadata;
	const char *error;
} result_t;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER, meta_wake = PTHREAD_COND_INITIALIZER;
static pthread_t worker, meta_worker;
static bool started, quitting, pending, running, waiting, desired_play = true, invalidated;
static unsigned serial;
static bool turn_ready;
static bool turn_wait_reader;
static bool preparation_toast;
static job_t next_job;
static result_t *fallback_result;
static source_t *queue_source, *meta_source;
static int meta_entry;
static char current_path[512], current_ref[80], ahead_refs[2][80], ahead_paths[2][512];
static nd_playrow_t current_track;
static unsigned prefetch_revision;
static char failed_refs[4][80];
static unsigned failed_next;
static lv_timer_t *timer;

static void retain(source_t *s) { __sync_add_and_fetch(&s->refs, 1); }
static void release(source_t *s) {
	if (s && __sync_sub_and_fetch(&s->refs, 1) == 0) {
		nd_store_release(s->library.scope, s->library.generation);
		free(s->handles);
		nd_wipe(&s->context, sizeof(s->context));
		free(s);
	}
}
bool navidromeplay_reference(const char *path) { return path && !strncmp(path, "ndq:", 4); }
static bool source_path(void *user, size_t entry, char *out, size_t cap) {
	source_t *s = user;
	return entry < (size_t)s->count && snprintf(out, cap, "ndq:%u:%zu", s->token, entry) < (int)cap;
}
static void display_text(char *out, size_t cap, const char *text) {
	size_t n = strlen(text);
	if (n >= cap) {
		n = cap - 1;
		while (n && ((unsigned char)text[n] & 0xc0) == 0x80)
			n--;
	}
	memcpy(out, text, n);
	out[n] = 0;
}
static void metadata(const nd_playrow_t *row, song_metadata_t *out) {
	display_text(out->title, sizeof(out->title), row->row.title);
	display_text(out->artist, sizeof(out->artist), row->row.artist);
	display_text(out->album, sizeof(out->album), row->album);
	out->track_number = row->row.track;
	out->disc_number = row->row.disc;
	out->has_tags = true;
}
static bool source_metadata(void *user, size_t entry, song_metadata_t *out) {
	source_t *s = user;
	pthread_mutex_lock(&lock);
	bool found = entry >= (size_t)s->window_start && entry < (size_t)(s->window_start + s->window_count);
	if (found) {
		memset(out, 0, sizeof(*out));
		metadata(&s->window[entry - (size_t)s->window_start], out);
	} else if (!quitting) {
		if (meta_source != s) {
			release(meta_source);
			meta_source = s;
			retain(s);
		}
		meta_entry = (int)entry;
		pthread_cond_signal(&meta_wake);
	}
	pthread_mutex_unlock(&lock);
	return found;
}
static void source_close(void *user) {
	source_t *s = user;
	if (queue_source == s)
		queue_source = NULL;
	release(s);
}
static bool current(void *user) {
	job_t *job = user;
	pthread_mutex_lock(&lock);
	bool valid = !quitting && job->serial == serial;
	pthread_mutex_unlock(&lock);
	return valid && navidrome_playback_current(&job->source->context, false);
}
static void protections(void) {
	const char *paths[3] = {current_path, ahead_paths[0], ahead_paths[1]};
	nd_audio_protect(paths);
}
static void result_done(void *user) {
	result_t *r = user;
	job_t *job = &r->job;
	source_t *s = job->source;
	if (!current(job))
		goto done;
	if (!job->initial && queue_source != s)
		goto done;
	if (r->error) {
		if (job->ref[0] && strcmp(r->error, "nd_audio_cancelled"))
			snprintf(failed_refs[failed_next++ % 4], sizeof(failed_refs[0]), "%s", job->ref);
		if (job->foreground) {
			toast_busy_end();
			preparation_toast = false;
			pthread_mutex_lock(&lock);
			waiting = false;
			pthread_mutex_unlock(&lock);
			device_state_t state;
			device_state_get(&state);
			if (nd_audio_owns(state.current_file))
				audio_stop();
			if (strcmp(r->error, "nd_audio_cancelled"))
				gui_notify_popup(r->error);
		}
		goto done;
	}
	if (!job->foreground) {
		for (int i = 0; i < 2; i++)
			if (!strcmp(ahead_refs[i], job->ref))
				snprintf(ahead_paths[i], sizeof(ahead_paths[i]), "%s", r->path);
		protections();
		goto done;
	}
	toast_busy_end();
	preparation_toast = false;
	if (job->initial) {
		playlist_source_t source = {.user = s, .count = s->count, .path = source_path, .metadata = source_metadata, .close = source_close};
		retain(s);
		if (!playlist_load_source(&source, s->start)) {
			release(s);
			navidromeplay_stop();
			gui_notify_popup("nd_worker_error");
			goto done;
		}
		queue_source = s;
		memset(failed_refs, 0, sizeof(failed_refs));
		memset(ahead_refs, 0, sizeof(ahead_refs));
		memset(ahead_paths, 0, sizeof(ahead_paths));
	}
	if (queue_source != s)
		goto done;
	char ref[80];
	if (!playlist_current_path(ref, sizeof(ref)) || strcmp(ref, job->ref))
		goto done;
	snprintf(current_path, sizeof(current_path), "%s", r->path);
	snprintf(current_ref, sizeof(current_ref), "%s", job->ref);
	current_track = r->row;
	protections();
	song_metadata_t meta = r->metadata;
	metadata(&r->row, &meta);
	pthread_mutex_lock(&lock);
	bool paused = !desired_play;
	waiting = false;
	pthread_mutex_unlock(&lock);
	device_state_play_prepared(r->path, &meta, paused, r->row.row.duration);
	player_refresh_now_playing();
	if (job->initial && lv_obj_is_valid(s->origin) && lv_screen_active() == s->origin)
		player_sheet_open(true);
done:
	release(s);
	free(r);
}
static void deliver_result(result_t *r) {
	if (gui_post(result_done, r))
		return;
	pthread_mutex_lock(&lock);
	result_t *old = fallback_result;
	fallback_result = r;
	pthread_mutex_unlock(&lock);
	if (old) {
		release(old->job.source);
		free(old);
	}
}
static void ready(const char *path, void *user) {
	job_t *job = user;
	result_t *r = calloc(1, sizeof(*r));
	if (!r)
		return;
	r->job = *job;
	retain(job->source);
	snprintf(r->path, sizeof(r->path), "%s", path);
	source_t *s = job->source;
	if (navidrome_playback_begin(&s->context)) {
		nd_store_t store;
		if (nd_store_open(&store, s->library.path, s->library.scope, false, false))
			nd_store_play_row(&store, s->library.generation, s->handles[job->entry], &r->row);
		nd_store_close(&store);
		metadata_read(path, &r->metadata);
		navidrome_cache_end();
	}
	if (!r->row.row.id[0])
		r->error = "nd_cache_error";
	deliver_result(r);
}
static bool read_row(source_t *s, int entry, nd_playrow_t *row) {
	if (entry < 0 || entry >= s->count || !navidrome_playback_begin(&s->context))
		return false;
	nd_store_t store;
	bool ok = nd_store_open(&store, s->library.path, s->library.scope, false, false) && nd_store_play_row(&store, s->library.generation, s->handles[entry], row);
	nd_store_close(&store);
	navidrome_cache_end();
	return ok;
}
static void yield_turn(void *user) {
	result_t *r = user;
	if (current(&r->job)) {
		device_state_t state;
		device_state_get(&state);
		bool stop_reader = growfile_is_growing(state.current_file) || nd_audio_failed(state.current_file);
		if (stop_reader)
			audio_stop();
		const char *keep[3] = {stop_reader ? "" : current_path, ahead_paths[0], ahead_paths[1]};
		nd_audio_protect(keep);
		pthread_mutex_lock(&lock);
		turn_wait_reader = stop_reader;
		pthread_mutex_unlock(&lock);
		pthread_mutex_lock(&lock);
		turn_ready = true;
		pthread_cond_broadcast(&wake);
		pthread_mutex_unlock(&lock);
	}
	release(r->job.source);
	free(r);
}
static bool request_turn(job_t *job) {
	result_t *r = calloc(1, sizeof(*r));
	if (!r)
		return false;
	r->job = *job;
	retain(job->source);
	pthread_mutex_lock(&lock);
	turn_ready = false;
	pthread_mutex_unlock(&lock);
	if (!gui_post(yield_turn, r)) {
		release(job->source);
		free(r);
		return false;
	}
	pthread_mutex_lock(&lock);
	while (!turn_ready && !quitting && job->serial == serial)
		pthread_cond_wait(&wake, &lock);
	bool ok = turn_ready && !quitting && job->serial == serial;
	bool wait_reader = turn_wait_reader;
	pthread_mutex_unlock(&lock);
	// Closing the old decoder can wait on ALSA; do that wait on this worker,
	// outside every service/card lease, before replacing an incomplete file.
	for (int waited = 0; ok && wait_reader && audio_playback_context_active() && waited < 3000; waited += 10) {
		usleep(10000);
		ok = current(job);
	}
	if (wait_reader && audio_playback_context_active())
		ok = false;
	if (ok)
		streamturn_abandon_all();
	return ok;
}
static void *work(void *unused) {
	(void)unused;
	thread_be_background("navidrome playback");
	for (;;) {
		pthread_mutex_lock(&lock);
		while (!pending && !quitting)
			pthread_cond_wait(&wake, &lock);
		if (quitting) {
			pthread_mutex_unlock(&lock);
			break;
		}
		job_t job = next_job;
		pending = false;
		running = true;
		pthread_mutex_unlock(&lock);
		source_t *s = job.source;
		const char *error = NULL;
		if (job.initial) {
			if (!navidrome_playback_begin(&s->context))
				error = "nd_audio_cancelled";
			else {
				nd_store_t store;
				bool ok = nd_store_open(&store, s->library.path, s->library.scope, false, false) && nd_store_play_index(&store, s->library.generation, &s->query, s->selected, &s->handles, &s->count, &s->start);
				nd_store_close(&store);
				navidrome_cache_end();
				if (!ok)
					error = "nd_cache_error";
			}
			job.entry = s->start;
			source_path(s, (size_t)job.entry, job.ref, sizeof(job.ref));
		}
		nd_playrow_t row;
		if (!error && (!current(&job) || !read_row(s, job.entry, &row)))
			error = "nd_audio_cancelled";
		if (!error) {
			nd_context_t context;
			if (!navidrome_context(&context, false) || context.profile_epoch != s->context.profile_epoch || context.card_epoch != s->context.card_epoch)
				error = "nd_audio_cancelled";
			else {
				bool cached = nd_audio_cached(&s->library, &context, row.row.id);
				if (!cached && (!navidrome_playback_current(&context, true) || !s->library.writable))
					error = s->library.writable ? "nd_reconnect_required" : "nd_cache_readonly";
				if (!error && !cached && job.foreground && !request_turn(&job))
					error = current(&job) ? "nd_audio_unavailable" : "nd_audio_cancelled";
				if (!error)
					error = nd_audio_fetch(&s->library, &context, &row, job.foreground, current, ready, &job);
			}
			nd_wipe(&context, sizeof(context));
		}
		if (error) {
			result_t *r = calloc(1, sizeof(*r));
			if (r) {
				r->job = job;
				r->error = error;
				retain(s);
				deliver_result(r);
			}
		}
		pthread_mutex_lock(&lock);
		running = false;
		bool more = pending;
		pthread_mutex_unlock(&lock);
		nd_audio_set_network_wanted(more);
		release(s);
	}
	return NULL;
}
static void *metadata_work(void *unused) {
	(void)unused;
	thread_be_background("navidrome queue metadata");
	for (;;) {
		pthread_mutex_lock(&lock);
		while (!meta_source && !quitting)
			pthread_cond_wait(&meta_wake, &lock);
		if (quitting) {
			pthread_mutex_unlock(&lock);
			break;
		}
		source_t *s = meta_source;
		int first = meta_entry > ND_WINDOW / 2 ? meta_entry - ND_WINDOW / 2 : 0;
		meta_source = NULL;
		pthread_mutex_unlock(&lock);
		nd_playrow_t *rows = calloc(ND_WINDOW * 2, sizeof(*rows));
		int count = 0;
		while (rows && count < ND_WINDOW * 2 && first + count < s->count && read_row(s, first + count, &rows[count]))
			count++;
		if (rows && navidrome_playback_current(&s->context, false)) {
			pthread_mutex_lock(&lock);
			memcpy(s->window, rows, (size_t)count * sizeof(*rows));
			s->window_start = first;
			s->window_count = count;
			pthread_mutex_unlock(&lock);
		}
		free(rows);
		release(s);
	}
	return NULL;
}
static void invalidate(void) {
	pthread_mutex_lock(&lock);
	serial++;
	invalidated = true;
	waiting = false;
	pthread_cond_broadcast(&wake);
	pthread_mutex_unlock(&lock);
	nd_audio_cancel();
}
void navidromeplay_stop(void) {
	pthread_mutex_lock(&lock);
	serial++;
	waiting = false;
	desired_play = false;
	pthread_cond_broadcast(&wake);
	pthread_mutex_unlock(&lock);
	nd_audio_cancel();
	nd_audio_set_network_wanted(false);
}
void navidromeplay_other(const char *path) {
	if (!nd_audio_owns(path) && !navidromeplay_reference(path))
		navidromeplay_stop();
}
static bool submit(source_t *s, int entry, bool initial, bool foreground, const char *ref) {
	if (foreground) {
		device_state_t state;
		device_state_get(&state);
		if (nd_audio_owns(state.current_file) && growfile_is_growing(state.current_file))
			audio_stop();
		nd_audio_cancel();
	}
	pthread_mutex_lock(&lock);
	if (!started || quitting) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	nd_audio_set_network_wanted(true);
	if (pending)
		release(next_job.source);
	if (foreground) {
		serial++;
		waiting = true;
		desired_play = true;
	}
	next_job = (job_t){.source = s, .serial = serial, .entry = entry, .initial = initial, .foreground = foreground};
	if (ref)
		snprintf(next_job.ref, sizeof(next_job.ref), "%s", ref);
	retain(s);
	pending = true;
	pthread_cond_broadcast(&wake);
	pthread_mutex_unlock(&lock);
	if (foreground && timer)
		lv_timer_resume(timer);
	if (foreground) {
		toast_busy_dismissable("nd_audio_preparing");
		preparation_toast = true;
	}
	return true;
}
bool navidromeplay_select(const nd_library_t *lib, const nd_query_t *query, const char *id, void *origin) {
	if (!started || !id || strlen(id) >= ND_ID_MAX || !navidrome_playback_pin(lib))
		return false;
	source_t *s = calloc(1, sizeof(*s));
	if (!s) {
		nd_store_release(lib->scope, lib->generation);
		return false;
	}
	s->refs = 1;
	s->library = *lib;
	s->query = *query;
	s->origin = origin;
	snprintf(s->selected, sizeof(s->selected), "%s", id);
	bool ok = navidrome_context(&s->context, false) && s->context.epoch == lib->epoch && s->context.card_epoch == lib->card_epoch;
	pthread_mutex_lock(&lock);
	s->token = serial + 1;
	pthread_mutex_unlock(&lock);
	if (ok)
		ok = submit(s, 0, true, true, NULL);
	release(s);
	return ok;
}
bool navidromeplay_prepare(const char *ref) {
	if (!navidromeplay_reference(ref))
		return true;
	// Entering here is an explicit retry or an already approved automatic
	// transition. A failed prefetch is blocked separately by auto().
	for (int i = 0; i < 4; i++)
		if (!strcmp(ref, failed_refs[i]))
			failed_refs[i][0] = 0;
	source_t *s = queue_source;
	unsigned token;
	size_t entry;
	char extra;
	if (s && sscanf(ref, "ndq:%u:%zu%c", &token, &entry, &extra) == 2 && token == s->token && entry < (size_t)s->count)
		submit(s, (int)entry, false, true, ref);
	return false;
}
bool navidromeplay_auto(const char *ref) {
	if (!navidromeplay_reference(ref))
		return true;
	for (int i = 0; i < 4; i++)
		if (!strcmp(ref, failed_refs[i])) {
			gui_notify_popup("nd_audio_unavailable");
			return false;
		}
	return true;
}
bool navidromeplay_toggle_pending(bool *playing) {
	pthread_mutex_lock(&lock);
	bool handled = waiting;
	if (handled) {
		desired_play = !desired_play;
		*playing = desired_play;
	}
	pthread_mutex_unlock(&lock);
	return handled;
}
bool navidromeplay_preparing(void) {
	pthread_mutex_lock(&lock);
	bool preparing = waiting;
	pthread_mutex_unlock(&lock);
	return preparing;
}
bool navidromeplay_next_ready(char *path, size_t cap) {
	char ref[80];
	if (!queue_source || !playlist_peek_auto(ref, sizeof(ref)))
		return false;
	for (int i = 0; i < 2; i++) {
		if (strcmp(ref, ahead_refs[i]) || !ahead_paths[i][0] || growfile_is_growing(ahead_paths[i]) || nd_audio_failed(ahead_paths[i]))
			continue;
		return snprintf(path, cap, "%s", ahead_paths[i]) < (int)cap;
	}
	return false;
}
bool navidromeplay_track(const char *path, nd_playrow_t *track, nd_library_t *library) {
	if (!path || !current_path[0] || strcmp(path, current_path) || !queue_source)
		return false;
	if (track)
		*track = current_track;
	if (library)
		*library = queue_source->library;
	return true;
}
void navidromeplay_tick(void) {
	if (!started)
		return;
	if (preparation_toast && !navidromeplay_preparing()) {
		toast_busy_end();
		preparation_toast = false;
	}
	pthread_mutex_lock(&lock);
	result_t *result = fallback_result;
	fallback_result = NULL;
	pthread_mutex_unlock(&lock);
	if (result)
		result_done(result);
	trackmenu_source_metadata_ready(NULL);
	pthread_mutex_lock(&lock);
	bool revoked = invalidated;
	invalidated = false;
	bool busy = pending || running;
	pthread_mutex_unlock(&lock);
	if (busy || queue_source)
		navidrome_poll_network();
	(void)revoked;
	if (queue_source && !navidrome_playback_current(&queue_source->context, false)) {
		device_state_t state;
		device_state_get(&state);
		if (nd_audio_owns(state.current_file))
			device_state_clear_prepared();
		if (playlist_has_source())
			playlist_clear();
		current_path[0] = current_ref[0] = 0;
		memset(&current_track, 0, sizeof(current_track));
		memset(ahead_refs, 0, sizeof(ahead_refs));
		memset(ahead_paths, 0, sizeof(ahead_paths));
		protections();
		nd_audio_set_network_wanted(false);
		player_refresh_now_playing();
		toast_busy_end();
		return;
	}
	if (!queue_source || busy) {
		if (!queue_source && !busy && timer)
			lv_timer_pause(timer);
		return;
	}
	device_state_t state;
	device_state_get(&state);
	if (!nd_audio_owns(state.current_file) || state.status != AUDIO_STATUS_PLAYING)
		return;
	navidrome_poll_network();
	if (prefetch_revision != playlist_revision()) {
		prefetch_revision = playlist_revision();
		memset(ahead_refs, 0, sizeof(ahead_refs));
	}
	int count = playlist_count(), at = playlist_current_index();
	for (int i = 0; i < 2; i++) {
		char ref[80];
		int index = at + i + 1;
		if (i == 0) {
			if (!playlist_peek_auto(ref, sizeof(ref)))
				continue;
		} else {
			if (playlist_get_mode() == PLAYBACK_MODE_REPEAT_ONE)
				continue;
			if (index >= count && playlist_get_mode() == PLAYBACK_MODE_REPEAT_ALL)
				index %= count;
			if (!playlist_path_at(index, ref, sizeof(ref)))
				continue;
		}
		if (!strcmp(ref, current_ref) || !strcmp(ref, ahead_refs[i]))
			continue;
		bool failed = false;
		for (int j = 0; j < 4; j++)
			failed |= !strcmp(ref, failed_refs[j]);
		if (failed)
			continue;
		snprintf(ahead_refs[i], sizeof(ahead_refs[i]), "%s", ref);
		ahead_paths[i][0] = 0;
		unsigned token;
		size_t entry;
		char extra;
		if (sscanf(ref, "ndq:%u:%zu%c", &token, &entry, &extra) == 2 && token == queue_source->token && entry < (size_t)queue_source->count) {
			submit(queue_source, (int)entry, false, false, ref);
			break;
		}
	}
}
static void tick(lv_timer_t *unused) {
	(void)unused;
	navidromeplay_tick();
}
void navidromeplay_init(void) {
	if (started)
		return;
	if (pthread_create(&worker, NULL, work, NULL))
		return;
	if (pthread_create(&meta_worker, NULL, metadata_work, NULL)) {
		pthread_mutex_lock(&lock);
		quitting = true;
		pthread_cond_signal(&wake);
		pthread_mutex_unlock(&lock);
		pthread_join(worker, NULL);
		return;
	}
	started = true;
	timer = lv_timer_create(tick, 250, NULL);
	lv_timer_pause(timer);
	navidrome_set_playback_invalidator(invalidate);
	navidrome_set_playback_network_stop(nd_audio_cancel);
}
void navidromeplay_shutdown(void) {
	if (!started)
		return;
	navidrome_set_playback_invalidator(NULL);
	navidrome_set_playback_network_stop(NULL);
	pthread_mutex_lock(&lock);
	quitting = true;
	serial++;
	pthread_cond_broadcast(&wake);
	pthread_cond_broadcast(&meta_wake);
	pthread_mutex_unlock(&lock);
	nd_audio_cancel();
	pthread_join(worker, NULL);
	pthread_join(meta_worker, NULL);
	if (pending)
		release(next_job.source);
	release(meta_source);
	meta_source = NULL;
	pending = false;
	if (fallback_result) {
		release(fallback_result->job.source);
		free(fallback_result);
		fallback_result = NULL;
	}
	if (playlist_has_source())
		playlist_clear();
	nd_audio_protect(NULL);
	nd_audio_set_network_wanted(false);
	if (timer)
		lv_timer_delete(timer);
	timer = NULL;
	started = false;
}

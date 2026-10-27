#include "navidromeplay.h"
#include "navidromeplaycatalog.h"
#include "navidromeplayliststore.h"
#include "navidromereports.h"
#include "navidromesort.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/nowplaying/trackmenu.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/toast.h"
#include "src/gui/streaming/navidromeart.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/decode/growfile.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/streamturn.h"
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct source {
	unsigned refs, token, queue_owners;
	nd_context_t context;
	nd_library_t library, album_library;
	bool album_origin, album_pinned;
	nd_order_t album_order;
	const char *album_error;
	nd_query_t query;
	char selected[ND_ID_MAX];
	int64_t *handles;
	nd_playcatalog_t *catalog;
	nd_playlist_capture_t saved;
	unsigned playlist_origin_serial, addition_origin_serial;
	bool capture;
	bool saved_shortcut, saved_shuffle;
	unsigned saved_random;
	int album_action; // 1 shuffle, 2 ordered play, 3 random single-track enqueue.
	unsigned album_intent, album_random;
	bool album_committed;
	playback_mode_t album_mode;
	int64_t selection;
	nd_kind_t enqueue_kind;
	int count, start;
	unsigned window_clock;
	int window_entry[ND_WINDOW * 2];
	unsigned window_age[ND_WINDOW * 2];
	bool window_loading[ND_WINDOW * 2];
	struct metadata_result *window_result[ND_WINDOW * 2];
	bool window_pending[ND_WINDOW * 2], window_valid[ND_WINDOW * 2], window_failed[ND_WINDOW * 2];
	nd_playrow_t window[ND_WINDOW * 2];
	lv_obj_t *origin;
} source_t;
typedef struct metadata_result {
	source_t *source;
	struct metadata_result *next;
	int slot, entry;
	nd_playrow_t row;
	bool ok;
} metadata_result_t;
static metadata_result_t *metadata_fallback;
static unsigned metadata_outstanding;
#define METADATA_PENDING_MAX 64
typedef struct {
	source_t *source;
	unsigned serial, queue_identity;
	int entry;
	bool initial, foreground, enqueue, chain, backward, automatic;
	const char *failure;
	char ref[80];
} job_t;
typedef struct {
	job_t job;
	char path[512];
	nd_playrow_t row;
	song_metadata_t metadata;
	const char *error;
	bool metadata_only, chain_fallback;
} result_t;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER, meta_wake = PTHREAD_COND_INITIALIZER;
static pthread_t worker, meta_worker;
static bool adding;
static bool started, quitting, pending, running, waiting, desired_play = true, invalidated;
static unsigned serial, album_intent;
static bool turn_ready;
static bool turn_wait_reader;
static job_t next_job, addition_job;
static bool addition_pending;
static sqlite3 *addition_db; // Only sqlite3_interrupt() may touch this from another thread.
static result_t *fallback_result, *addition_fallback;
static source_t *addition_failed;
#define SOURCE_CONTEXTS 8
static source_t *queue_source, *album_owner, *sources[SOURCE_CONTEXTS];
static unsigned album_queue_identity;
static char album_first_ref[80];
static source_t *meta_sources[SOURCE_CONTEXTS + 1];
static unsigned next_token;
static unsigned playlist_origin_serial;
static void *playlist_origin;
static unsigned addition_origin_serial;
static void *addition_origin;
static int meta_count;
static bool installed(source_t *s) {
	for (int i = 0; i < SOURCE_CONTEXTS; i++)
		if (sources[i] == s) return true;
	return false;
}
static bool install(source_t *s) {
	if (installed(s)) return true;
	for (int i = 0; i < SOURCE_CONTEXTS; i++)
		if (!sources[i]) { sources[i] = s; return true; }
	return false;
}
static source_t *reference_source(const char *ref, int *entry) {
	unsigned token;
	size_t ordinal;
	char extra;
	if (!ref || sscanf(ref, "ndq:%u:%zu%c", &token, &ordinal, &extra) != 2) return NULL;
	for (int i = 0; i < SOURCE_CONTEXTS; i++)
		if (sources[i] && sources[i]->token == token && ordinal < (size_t)sources[i]->count) {
			*entry = (int)ordinal;
			return sources[i];
		}
	return NULL;
}

static char current_path[512], current_ref[80], ahead_refs[2][80], ahead_paths[2][512];
static nd_playrow_t current_track;
typedef struct {
	source_t *source;
	nd_playrow_t row;
	int64_t generation;
} annotation_t;
static annotation_t *annotation_pending, *annotation_fallback;
static int64_t annotation_revision;
static unsigned prefetch_revision;
static char failed_refs[4][80];
static const char *failed_reasons[4];
static unsigned failed_next;
static lv_timer_t *timer;

static void retain(source_t *s) { __sync_add_and_fetch(&s->refs, 1); }
static void release(source_t *s) {
	if (s && __sync_sub_and_fetch(&s->refs, 1) == 0) {
		if (s->capture)
			nd_playcatalog_free(s->catalog);
		else
			nd_store_release_source(&s->library);
		if (s->album_pinned) nd_store_release_source(&s->album_library);
		free(s->handles);
		nd_wipe(&s->context, sizeof(s->context));
		free(s);
	}
}
// Album progression belongs to the queue origin, including when its final
// explicit addition comes from a different provider or local Music.
static void album_origin(source_t *s) {
	if (s && !s->album_origin) s = NULL;
	if (s) retain(s);
	source_t *old = album_owner;
	album_owner = s;
	album_queue_identity = playlist_identity();
	album_first_ref[0] = 0;
	if (s) playlist_path_at(0, album_first_ref, sizeof(album_first_ref));
	release(old);
}
// Called under lock. Every requested source remains retained until all its slots resolve.
static void metadata_schedule(source_t *s) {
	for (int i = 0; i < meta_count; i++)
		if (meta_sources[i] == s) return;
	if (meta_count < SOURCE_CONTEXTS + 1) {
		meta_sources[meta_count++] = s;
		retain(s);
		pthread_cond_signal(&meta_wake);
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
	int victim = -1;
	for (int i = 0; i < ND_WINDOW * 2; i++) {
		if (s->window_age[i] && s->window_entry[i] == (int)entry) {
			s->window_age[i] = ++s->window_clock;
			bool ready = s->window_valid[i] || s->window_failed[i];
			if (ready) {
				memset(out, 0, sizeof(*out));
				if (s->window_valid[i])
					metadata(&s->window[i], out);
				else
					snprintf(out->title, sizeof(out->title), "%s", tr("nd_audio_unavailable"));
			}
			pthread_mutex_unlock(&lock);
			return ready;
		}
		if (!s->window_pending[i] && !s->window_loading[i] && (victim < 0 || s->window_age[i] < s->window_age[victim]))
			victim = i;
	}
	if (!quitting && victim >= 0 && metadata_outstanding < METADATA_PENDING_MAX) {
		bool scheduled = false;
		for (int i = 0; i < meta_count; i++) scheduled |= meta_sources[i] == s;
		if (!scheduled && meta_count >= SOURCE_CONTEXTS + 1) { pthread_mutex_unlock(&lock); return false; }
		metadata_result_t *r = calloc(1, sizeof(*r));
		if (!r) {
			s->window_entry[victim] = (int)entry;
			s->window_age[victim] = ++s->window_clock;
			s->window_valid[victim] = false;
			s->window_failed[victim] = true;
			memset(out,0,sizeof(*out));
			snprintf(out->title,sizeof(out->title),"%s",tr("nd_audio_unavailable"));
			pthread_mutex_unlock(&lock);
			return true;
		}
		r->source = s;
		r->slot = victim;
		r->entry = (int)entry;
		retain(s);
		s->window_result[victim] = r;
		metadata_outstanding++;
		s->window_entry[victim] = (int)entry;
		s->window_age[victim] = ++s->window_clock;
		s->window_pending[victim] = true;
		s->window_valid[victim] = s->window_failed[victim] = false;
		metadata_schedule(s);
	}
	pthread_mutex_unlock(&lock);
	return false;
}
static void source_close(void *user) {
	source_t *s = user;
	if (s->queue_owners) s->queue_owners--;
	if (!s->queue_owners) {
		if (queue_source == s) queue_source = NULL;
		for (int i = 0; i < SOURCE_CONTEXTS; i++)
			if (sources[i] == s) sources[i] = NULL;
	}
	release(s);
}
static bool current(void *user) {
	job_t *job = user;
	pthread_mutex_lock(&lock);
	bool valid = !quitting && (job->enqueue || job->serial == serial) && (!(job->initial || job->enqueue) || !job->source->album_action || job->source->album_committed || job->source->album_intent == album_intent);
	pthread_mutex_unlock(&lock);
	return valid && navidrome_playback_current(&job->source->context, false);
}
static void protections(void) {
	const char *paths[3] = {current_path, ahead_paths[0], ahead_paths[1]};
	nd_audio_protect(paths);
}
static bool addition_feedback(source_t *s) {
	return !s->saved.playlist || !s->origin ||
		(s->addition_origin_serial == addition_origin_serial &&
		lv_obj_is_valid(s->origin) && lv_screen_active() == s->origin);
}
static void addition_error(source_t *s, const char *error) {
	if (addition_feedback(s)) gui_notify_popup(error);
}
static void result_done(void *user) {
	result_t *r = user;
	job_t *job = &r->job;
	source_t *s = job->source;
	if (!current(job))
		goto done;
	if (job->chain) {
		if (r->chain_fallback) {
			pthread_mutex_lock(&lock); waiting = false; pthread_mutex_unlock(&lock);
			char ref[512];
			bool stepped = job->backward ? playlist_prev(ref,sizeof(ref)) : job->automatic ? playlist_advance_auto(ref,sizeof(ref)) : playlist_next(ref,sizeof(ref));
			if (stepped) device_state_play_queue_index(playlist_current_index());
			goto done;
		}
		if (r->error) {
			navidromeplay_stop();
			audio_stop();
			gui_notify_popup(r->error);
			goto done;
		}
		playlist_source_t provider = {.user = s, .count = s->count, .path = source_path, .metadata = source_metadata, .close = source_close};
		retain(s); s->queue_owners++;
		if (!playlist_load_source(&provider, s->start)) { source_close(s); gui_notify_popup("nd_worker_error"); goto done; }
		install(s);
		album_origin(s);
		queue_source = s;
		char ref[80];
		source_path(s, (size_t)s->start, ref, sizeof(ref));
		navidromeplay_prepare(ref);
		goto done;
	}
	if (job->enqueue) {
		if (job->queue_identity != playlist_identity()) { addition_error(s, "nd_queue_changed"); goto done; }
		if (r->error) { addition_error(s, r->error); goto done; }
		source_t *owner = s;
		int first = 0;
		for (int i = 0; i < SOURCE_CONTEXTS; i++) {
			source_t *candidate = sources[i];
			if (!s->capture && candidate && !candidate->capture && candidate->library.generation == s->library.generation &&
				candidate->library.card_epoch == s->library.card_epoch && candidate->library.favorites == s->library.favorites &&
				!strcmp(candidate->library.path, s->library.path) && !strcmp(candidate->library.scope, s->library.scope)) {
				owner = candidate; break;
			}
		}
		if (owner != s) {
			first = owner->count;
			if (s->count > INT_MAX - first || (size_t)(first + s->count) > SIZE_MAX / sizeof(*s->handles)) { addition_error(s, "nd_queue_limit"); goto done; }
			int64_t *grown = malloc((size_t)(first + s->count) * sizeof(*grown));
			if (!grown) { addition_error(s, "nd_worker_error"); goto done; }
			pthread_mutex_lock(&lock);
			memcpy(grown, owner->handles, (size_t)first * sizeof(*grown));
			memcpy(grown + first, s->handles, (size_t)s->count * sizeof(*grown));
			free(owner->handles);
			owner->handles = grown;
			owner->count += s->count;
			pthread_mutex_unlock(&lock);
		}
		if (!install(owner)) { addition_error(s, "nd_queue_limit"); goto done; }
		bool empty = playlist_count() == 0;
		playlist_source_t provider = {.user = owner, .first = (size_t)first, .count = s->count, .path = source_path, .metadata = source_metadata, .close = source_close};
		retain(owner); owner->queue_owners++;
		if (!playlist_enqueue_source(&provider)) {
			if (owner != s) { pthread_mutex_lock(&lock); owner->count = first; pthread_mutex_unlock(&lock); }
			source_close(owner);
			addition_error(s, "nd_queue_limit");
			goto done;
		}
		s->album_committed = true;
		if (empty) {
			queue_source = owner;
			current_track = r->row;
			source_path(owner, (size_t)first, current_ref, sizeof(current_ref));
			current_path[0] = 0;
			song_metadata_t meta = {0};
			metadata(&r->row, &meta);
			device_state_represent_source(current_ref, &meta, r->row.row.duration);
			player_refresh_now_playing();
		}
		if (addition_feedback(s)) toast_success(s->album_action == 3 ? "medialist_random_track_queued" : s->count == 1 ? "added_to_the_queue" : "queue_tracks_added");
		goto done;
	}
	if (!job->initial && (!installed(s) || (job->foreground && queue_source != s)))
		goto done;
	if (r->metadata_only && !job->initial) {
		if (!strcmp(current_ref, job->ref) && r->row.row.id[0]) {
			current_track = r->row;
			song_metadata_t meta = {0};
			metadata(&r->row, &meta);
			device_state_represent_source(job->ref, &meta, r->row.row.duration);
			player_refresh_now_playing();
		}
		goto done;
	}
	if (r->error) {
		bool represented = !job->initial || (installed(s) && queue_source == s && !strcmp(current_ref, job->ref));
		if (represented && job->foreground && !strcmp(current_ref, job->ref) && r->row.row.id[0]) {
			current_track = r->row;
			current_path[0] = 0;
			song_metadata_t meta = {0};
			metadata(&r->row, &meta);
			device_state_represent_source(job->ref, &meta, r->row.row.duration);
			player_refresh_now_playing();
		}
		if (job->ref[0] && strcmp(r->error, "nd_audio_cancelled")) {
			unsigned slot = failed_next++ % 4;
			snprintf(failed_refs[slot], sizeof(failed_refs[0]), "%s", job->ref);
			failed_reasons[slot] = r->error;
		}
		if (job->foreground) {
			pthread_mutex_lock(&lock);
			waiting = false;
			pthread_mutex_unlock(&lock);
			device_state_t state;
			device_state_get(&state);
			if (represented && nd_audio_source(state.current_file))
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
	if (job->initial) {
		playlist_source_t source = {.user = s, .count = s->count, .path = source_path, .metadata = source_metadata, .close = source_close};
		retain(s); s->queue_owners++;
		if (!((s->album_action || s->saved_shortcut) ? playlist_load_source_mode(&source, s->start, s->album_mode) : playlist_load_source(&source, s->start))) {
			source_close(s);
			navidromeplay_stop();
			gui_notify_popup("nd_worker_error");
			goto done;
		}
		s->album_committed = true;
		install(s); // Replacement has released all prior providers.
		album_origin(s);
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
	annotation_revision = -1;
	protections();
	song_metadata_t meta = r->metadata;
	metadata(&r->row, &meta);
	pthread_mutex_lock(&lock);
	bool paused = !desired_play;
	waiting = false;
	pthread_mutex_unlock(&lock);
	nd_audio_set_source(r->path);
	device_state_play_prepared(r->path, &meta, paused, r->row.row.duration);
	nd_reports_start(&s->context, &s->library, r->row.row.id, r->path, r->row.row.duration);
	player_refresh_now_playing();
	if (job->initial && (!s->saved.playlist || s->playlist_origin_serial == playlist_origin_serial) &&
		config_get_bool("player", "open_player_on_tap", true) && lv_obj_is_valid(s->origin) && lv_screen_active() == s->origin)
		player_sheet_open(true);
done:
	if (job->enqueue) { pthread_mutex_lock(&lock); adding = false; pthread_mutex_unlock(&lock); }
	release(s);
	free(r);
}
static void deliver_result(result_t *r) {
	if (gui_post(result_done, r))
		return;
	pthread_mutex_lock(&lock);
	result_t **slot = r->job.enqueue ? &addition_fallback : &fallback_result;
	result_t *old = *slot;
	*slot = r;
	pthread_mutex_unlock(&lock);
	if (old) {
		release(old->job.source);
		free(old);
	}
}
// Called with a short playback storage lease, never on the GUI thread.
static void cached_favorite(source_t *s, nd_playrow_t *row) {
	if (!row->row.id[0])
		return;
	if (!s->capture) {
		row->row.favorite = false;
		row->row.favorite_known = false;
	}
	nd_library_t favorites;
	navidrome_favorites_snapshot(&favorites);
	if (!favorites.ready || favorites.card_epoch != s->context.card_epoch || strcmp(favorites.scope, s->library.scope))
		return;
	nd_store_t store = {0};
	if (nd_store_open_source(&store, favorites.path, favorites.scope, false, false, true)) {
		bool starred;
		bool known = nd_store_annotation(&store, favorites.generation, ND_TRACKS, row->row.id, &starred);
		if (known || !s->capture) {
			row->row.favorite_known = known;
			row->row.favorite = known && starred;
		}
	}
	nd_store_close(&store);
}

static bool read_row(source_t *s, int entry, nd_playrow_t *row);
static void ready(const char *path, void *user) {
	job_t *job = user;
	result_t *r = calloc(1, sizeof(*r));
	if (!r)
		return;
	r->job = *job;
	retain(job->source);
	snprintf(r->path, sizeof(r->path), "%s", path);
	source_t *s = job->source;
	if (read_row(s, job->entry, &r->row) && navidrome_playback_begin(&s->context)) {
		metadata_read(path, &r->metadata);
		navidrome_cache_end();
	}
	if (!r->row.row.id[0])
		r->error = "nd_cache_error";
	deliver_result(r);
}
static bool read_row(source_t *s, int entry, nd_playrow_t *row) {
	int64_t handle = 0;
	pthread_mutex_lock(&lock);
	bool valid = entry >= 0 && entry < s->count;
	if (valid && !s->capture) handle = s->handles[entry];
	pthread_mutex_unlock(&lock);
	if (!valid || !navidrome_playback_begin(&s->context)) return false;

	nd_store_t store = {0};
	bool ok = s->capture ? nd_playcatalog_read(s->catalog, entry, row) : nd_store_open_source(&store, s->library.path, s->library.scope, false, false, s->library.favorites) && nd_store_play_row(&store, s->library.generation, handle, row);
	nd_store_close(&store);
	if (ok)
		cached_favorite(s, row);
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
static bool album_pick(source_t *s, nd_store_t *store) {
	nd_row_t rows[ND_WINDOW];
	nd_playrow_t album;
	int count = 0, total = 0;
	if (!nd_store_metadata(store, s->library.generation, ND_ALBUMS, s->query.filter, &album) || album.row.details || !nd_store_query(store, s->library.generation, &s->query, 0, rows, &count, &total) || total <= 0)
		return false;
	int ordinal = s->album_action == 2 ? 0 : (int)(s->album_random % (unsigned)total);
	if (ordinal && !nd_store_query(store, s->library.generation, &s->query, ordinal, rows, &count, &total))
		return false;
	if (!count)
		return false;
	memcpy(s->selected, rows[0].id, sizeof(s->selected));
	return true;
}

static int addition_cancelled(void *user) {
	source_t *s = user;
	pthread_mutex_lock(&lock);
	bool stop = quitting;
	pthread_mutex_unlock(&lock);
	return stop || !navidrome_playback_current(&s->context,false);
}
static bool build_addition(source_t *s) {
	if (!navidrome_playback_begin(&s->context)) return false;
	nd_store_t store = {0};
	nd_playlist_store_t selections = {0};
	bool ok = nd_store_open_source(&store, s->library.path, s->library.scope, false, false, s->library.favorites);
	pthread_mutex_lock(&lock);
	addition_db = ok ? store.db : NULL;
	pthread_mutex_unlock(&lock);
	ok = ok && !addition_cancelled(s);
	if (ok && s->album_action == 3)
		ok = album_pick(s, &store);
	if (ok && !s->selection)
		ok = nd_store_queue_add(&store, s->library.generation, s->enqueue_kind, s->selected);
	else if (ok) {
		char path[1200];
		sqlite3_stmt *st = NULL;
		ok = nd_pl_path(&s->library, path) && nd_pl_open(&selections, path, s->library.scope, false) &&
			nd_pl_prepare(&selections, &st, "SELECT kind,track FROM selection WHERE session=? ORDER BY ordinal");
		if (ok) {
			sqlite3_bind_int64(st, 1, s->selection);
			int rc;
			while ((rc = sqlite3_step(st)) == SQLITE_ROW && ok)
				ok = !addition_cancelled(s) && nd_store_queue_add(&store, s->library.generation, sqlite3_column_int(st, 0), (const char *)sqlite3_column_text(st, 1));
			ok = ok && rc == SQLITE_DONE;
			sqlite3_finalize(st);
		}
	}
	if (ok) ok = !addition_cancelled(s) && nd_store_queue_finish(&store, &s->handles, &s->count);
	nd_pl_close(&selections);
	pthread_mutex_lock(&lock);
	addition_db = NULL;
	pthread_mutex_unlock(&lock);
	nd_store_close(&store);
	navidrome_cache_end();
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

		if (job.chain) {
			nd_store_t store = {0};
			bool lease = navidrome_playback_begin(&s->context);
			bool ok = lease && nd_store_open(&store, s->library.path, s->library.scope, false, false);
			char neighbor[ND_ID_MAX];
			bool no_neighbor = false;
			if (ok) {
				ok = nd_store_album_beside_order(&store, s->library.generation, s->selected, !job.backward, s->album_order, neighbor);
				no_neighbor = !ok && !store.error;
			}
			if (ok) {
				snprintf(s->query.filter, sizeof(s->query.filter), "%s", neighbor);
				ok = nd_store_queue_add(&store, s->library.generation, ND_ALBUMS, neighbor) && nd_store_queue_finish(&store, &s->handles, &s->count);
			}
			const char *chain_error = store.error;
			nd_store_close(&store);
			if (lease) navidrome_cache_end();
			s->start = job.backward ? s->count - 1 : 0;
			result_t *r = calloc(1, sizeof(*r));
			if (r) {
				r->job = job;
				retain(s);
				if (no_neighbor) r->chain_fallback = true;
				else if (!ok || !read_row(s, s->start, &r->row)) r->error = chain_error ? chain_error : s->count ? "nd_audio_unavailable" : "nd_album_empty";
				deliver_result(r);
			}
			pthread_mutex_lock(&lock);
			running = false;
			pthread_mutex_unlock(&lock);
			release(s);
			continue;
		}
		if (job.initial) {
			if (s->capture) {
				s->catalog = s->saved.playlist ?
					nd_playcatalog_playlist(&s->library, &s->context, &s->saved, current, &job, &error) :
					nd_playcatalog_capture(&s->library, &s->context, &s->query, s->selected, current, &job, &error);
				if (s->catalog) {
					s->count = nd_playcatalog_count(s->catalog);
					s->start = nd_playcatalog_start(s->catalog);
					if (s->saved_shortcut && s->saved_shuffle && s->count > 0)
						s->start = (int)(s->saved_random % (unsigned)s->count);
				}
			} else if (!navidrome_playback_begin(&s->context))
				error = "nd_audio_cancelled";
			else {
				nd_store_t store;
				bool ok = nd_store_open_source(&store, s->library.path, s->library.scope, false, false, s->library.favorites) &&
                    (!s->album_action || album_pick(s, &store)) &&
                    nd_store_play_index(&store, s->library.generation, &s->query, s->selected, &s->handles, &s->count, &s->start);
				nd_store_close(&store);
				navidrome_cache_end();
				if (!ok)
					error = "nd_cache_error";
			}
			job.entry = s->start;
			source_path(s, (size_t)job.entry, job.ref, sizeof(job.ref));
		}
		nd_playrow_t row = {0};
		if (!error && (!current(&job) || !read_row(s, job.entry, &row)))
			error = "nd_audio_cancelled";
		if (!error && job.foreground && !job.initial) {
			result_t *shown = calloc(1, sizeof(*shown));
			if (shown) {
				shown->job = job;
				shown->row = row;
				shown->metadata_only = true;
				retain(s);
				deliver_result(shown);
			}
		}
		if (!error && job.failure) error = job.failure;
		if (!error) {
			nd_context_t context;
			if (!navidrome_context(&context, false) || context.profile_epoch != s->context.profile_epoch || context.card_epoch != s->context.card_epoch)
				error = "nd_audio_cancelled";
			else {
				bool cached = nd_audio_cached(&s->library, &context, row.row.id);
				if (!cached && (!navidrome_playback_current(&context, true) || !s->library.writable))
					error = s->library.writable ? "nd_track_reconnect" : "nd_cache_readonly";
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
				r->row = row;
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
static void annotation_done(void *data) {
	annotation_t *a = data;
	nd_library_t favorites;
	navidrome_favorites_snapshot(&favorites);
	if (queue_source == a->source && favorites.generation == a->generation && navidrome_playback_current(&a->source->context, false) && !strcmp(current_track.row.id, a->row.row.id)) {
		current_track.row.favorite = a->row.row.favorite;
		current_track.row.favorite_known = a->row.row.favorite_known;
		player_refresh_favorite();
	}
	release(a->source);
	free(a);
}
static void metadata_done(void *data) {
	metadata_result_t *r = data;
	source_t *s = r->source;
	pthread_mutex_lock(&lock);
	if (s->window_result[r->slot] == r && s->window_entry[r->slot] == r->entry) {
		s->window_result[r->slot] = NULL;
		s->window_loading[r->slot] = false;
		s->window[r->slot] = r->row;
		s->window_valid[r->slot] = r->ok && !quitting;
		s->window_failed[r->slot] = !s->window_valid[r->slot];
	}
	if (metadata_outstanding) metadata_outstanding--;
	pthread_mutex_unlock(&lock);
	char ref[80];
	if (r->ok && queue_source==s && !current_path[0] && source_path(s,(size_t)r->entry,ref,sizeof(ref)) && !strcmp(current_ref,ref) && navidrome_playback_current(&s->context,false)) {
		nd_row_t previous = current_track.row;
		current_track=r->row;
		if (previous.favorite_known && !strcmp(previous.id,current_track.row.id)) {
			current_track.row.favorite=previous.favorite;
			current_track.row.favorite_known=true;
		}
		song_metadata_t meta={0};metadata(&r->row,&meta);
		device_state_represent_source(ref,&meta,r->row.row.duration);
		player_refresh_now_playing();
	}
	release(s);
	free(r);
}
static void *metadata_work(void *unused) {
	(void)unused;
	thread_be_background("navidrome queue metadata");
	for (;;) {
		pthread_mutex_lock(&lock);
		while (!meta_count && !annotation_pending && !addition_pending && !quitting)
			pthread_cond_wait(&meta_wake, &lock);
		if (quitting) {
			pthread_mutex_unlock(&lock);
			break;
		}
		if (annotation_pending) {
			annotation_t *a = annotation_pending;
			annotation_pending = NULL;
			pthread_mutex_unlock(&lock);
			if (navidrome_playback_begin(&a->source->context)) {
				cached_favorite(a->source, &a->row);
				navidrome_cache_end();
			}
			if (!gui_post(annotation_done, a)) {
				pthread_mutex_lock(&lock);
				annotation_t *old = annotation_fallback;
				annotation_fallback = a;
				pthread_mutex_unlock(&lock);
				if (old) {
					release(old->source);
					free(old);
				}
			}
			continue;
		}
		if (addition_pending) {
			job_t job = addition_job;
			addition_pending = false;
			pthread_mutex_unlock(&lock);
			source_t *s = job.source;
			result_t *r = calloc(1,sizeof(*r));
			if (!r) {
				// Transfer the worker reference to a fixed GUI error slot.
				pthread_mutex_lock(&lock); addition_failed = s; pthread_mutex_unlock(&lock);
				continue;
			}
			const char *error = NULL;
			bool ok;
			if (s->capture) {
				s->catalog = nd_playcatalog_playlist(&s->library, &s->context, &s->saved, current, &job, &error);
				ok = s->catalog != NULL;
				if (ok) { s->count = nd_playcatalog_count(s->catalog); s->start = 0; }
			} else ok = build_addition(s);
			r->job = job;
			retain(s);
			if (!ok || !read_row(s,0,&r->row)) r->error = error ? error : "nd_audio_unavailable";
			deliver_result(r);
			release(s);
			continue;
		}
		source_t *s = meta_sources[0];
		int slot = -1;
		for (int i = 0; i < ND_WINDOW * 2; i++)
			if (s->window_pending[i]) {
				slot = i;
				break;
			}
		if (slot < 0) {
			memmove(meta_sources, meta_sources + 1, (size_t)--meta_count * sizeof(*meta_sources));
			pthread_mutex_unlock(&lock);
			release(s);
			continue;
		}
		metadata_result_t *r = s->window_result[slot];
		s->window_pending[slot] = false;
		s->window_loading[slot] = true;
		pthread_mutex_unlock(&lock);
		r->ok = read_row(s, r->entry, &r->row);
		if (!gui_post(metadata_done, r)) {
			pthread_mutex_lock(&lock);
			r->next = metadata_fallback;
			metadata_fallback = r; // At most METADATA_PENDING_MAX owned results.
			pthread_mutex_unlock(&lock);
		}

	}
	return NULL;
}
static void invalidate(void) {
	pthread_mutex_lock(&lock);
	if (addition_db) sqlite3_interrupt(addition_db);
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
void navidromeplay_cache_stop(void) {
	navidromeplay_stop();
	device_state_t state;
	device_state_get(&state);
	if (nd_audio_source(state.current_file)) {
		audio_stop();
		if (current_ref[0] && current_track.row.id[0]) {
			song_metadata_t meta = {0};
			metadata(&current_track, &meta);
			device_state_represent_source(current_ref, &meta, current_track.row.duration);
		}
		current_path[0] = 0;
		player_refresh_now_playing();
	}
	memset(ahead_refs, 0, sizeof(ahead_refs));
	memset(ahead_paths, 0, sizeof(ahead_paths));
	memset(failed_refs, 0, sizeof(failed_refs));
	nd_audio_protect(NULL);
}
bool navidromeplay_cache_reader_active(void) {
	char path[512];
	audio_get_current_file(path, sizeof(path));
	return nd_audio_source(path) && audio_playback_context_active();
}
void navidromeplay_other(const char *path) {
	if (!nd_audio_source(path) && !navidromeplay_reference(path)) {
		navidromeplay_stop();
		queue_source = NULL;
		current_ref[0] = current_path[0] = 0;
		memset(&current_track, 0, sizeof(current_track));
	}
}
static bool submit(source_t *s, int entry, bool initial, bool foreground, const char *ref, const char *failure) {
	if (foreground) {
		nd_reports_defer();
		device_state_t state;
		device_state_get(&state);
		if (nd_audio_source(state.current_file) && growfile_is_growing(state.current_file))
			audio_stop();
		nd_audio_cancel();
	}
	pthread_mutex_lock(&lock);
	if (!started || quitting) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	nd_audio_set_network_wanted(true);
	if (pending) {
		if (next_job.enqueue) adding = false;
		release(next_job.source);
	}
	if (foreground) {
		serial++;
		waiting = true;
		desired_play = true;
	}
	next_job = (job_t){.source = s, .serial = serial, .entry = entry, .initial = initial, .foreground = foreground, .failure = failure};
	if (ref)
		snprintf(next_job.ref, sizeof(next_job.ref), "%s", ref);
	retain(s);
	pending = true;
	pthread_cond_broadcast(&wake);
	pthread_mutex_unlock(&lock);
	if (foreground && timer)
		lv_timer_resume(timer);

	return true;
}
bool navidromeplay_chain_boundary(bool forward, bool automatic) {
	if (album_owner && (album_queue_identity != playlist_identity() || !playlist_has_source())) album_origin(NULL);
	source_t *old = album_owner;
	if (!old || !old->album_origin || !config_get_bool("player", "album_chaining", false)) return false;
	if (!old->album_pinned) { gui_notify_popup(old->album_error ? old->album_error : "nd_album_order_required"); return false; }
	if (!forward) {
		char ref[80];
		if (!playlist_current_path(ref,sizeof(ref)) || strcmp(ref,album_first_ref)) return false;
	}
	source_t *s = calloc(1, sizeof(*s));
	if (!s || !nd_store_retain_source(&old->album_library)) { free(s); return false; }
	s->refs = 1;
	s->context = old->context;
	s->library = old->album_library;
	s->album_library = old->album_library;
	s->album_origin = true;
	s->album_order = old->album_order;
	s->album_pinned = nd_store_retain_source(&s->album_library);
	s->query.kind = ND_TRACKS;
	s->query.filter_kind = ND_ALBUMS;
	snprintf(s->selected, sizeof(s->selected), "%s", old->query.filter);
	pthread_mutex_lock(&lock);
	if (pending) { if (next_job.enqueue) adding = false; release(next_job.source); }
	s->token = ++next_token;
	serial++;
	next_job = (job_t){.source = s, .serial = serial, .chain = true, .backward = !forward, .automatic = automatic};
	retain(s);
	pending = true;
	waiting = true;
	desired_play = true;
	pthread_cond_broadcast(&wake);
	pthread_mutex_unlock(&lock);
	nd_audio_cancel();
	audio_stop();
	if (timer) lv_timer_resume(timer);
	release(s);
	return true;
}
bool navidromeplay_chain(bool forward) { return navidromeplay_chain_boundary(forward,false); }
bool navidromeplay_enqueuing(void) {
	pthread_mutex_lock(&lock);
	bool busy = adding;
	pthread_mutex_unlock(&lock);
	return busy;
}
bool navidromeplay_enqueue(const nd_library_t *lib, nd_kind_t kind, const char *id, int64_t session) {
	if (!started || (!session && (!id || !*id || strlen(id) >= ND_ID_MAX)) || !navidrome_playback_pin_source(lib)) return false;
	source_t *s = calloc(1, sizeof(*s));
	if (!s) { nd_store_release_source(lib); return false; }
	s->refs = 1;
	s->library = *lib;
	s->enqueue_kind = kind;
	s->selection = session;
	if (id) snprintf(s->selected, sizeof(s->selected), "%s", id);
	bool ok = navidrome_context(&s->context, false) && s->context.card_epoch == lib->card_epoch && s->context.epoch == lib->epoch;
	pthread_mutex_lock(&lock);
	ok = ok && !quitting && !adding;
	if (ok) {
		s->token = ++next_token;
		adding = true;
		addition_job = (job_t){.source = s, .serial = serial, .queue_identity = playlist_identity(), .enqueue = true};
		retain(s);
		addition_pending = true;
		pthread_cond_signal(&meta_wake);
	}
	pthread_mutex_unlock(&lock);
	release(s);
	if (ok && timer) lv_timer_resume(timer);
	return ok;
}
bool navidromeplay_occurrence_matches(const nd_library_t *lib, int64_t playlist, int64_t occurrence, const char *track) {
	int entry;
	source_t *s = reference_source(current_ref, &entry);
	if (s && s->saved.playlist == playlist && current_track.saved_occurrence > 0) {
		return lib && lib->card_epoch == s->context.card_epoch && !strcmp(lib->scope, s->library.scope) && navidrome_playback_current(&s->context, false) && occurrence == current_track.saved_occurrence;
	}
	return navidromeplay_matches(lib, ND_TRACKS, track);
}

bool navidromeplay_matches(const nd_library_t *lib, nd_kind_t kind, const char *id) {
	return queue_source && current_track.row.id[0] && lib && id &&
		lib->card_epoch == queue_source->library.card_epoch && !strcmp(lib->scope, queue_source->library.scope) &&
		navidrome_playback_current(&queue_source->context, false) &&
		!strcmp(id, kind == ND_TRACKS ? current_track.row.id : kind == ND_ALBUMS ? current_track.album_id : "");
}
void navidromeplay_retry_metadata(const char *ref) {
	int entry;
	source_t *s = reference_source(ref, &entry);
	if (!s) return;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < ND_WINDOW * 2; i++)
		if (s->window_age[i] && s->window_entry[i] == entry && s->window_failed[i]) {
			s->window_age[i] = 0;
			s->window_failed[i] = false;
		}
	pthread_mutex_unlock(&lock);
}
bool navidromeplay_lookup(const char *ref, nd_playrow_t *row, nd_library_t *lib) {
	int entry;
	source_t *s = reference_source(ref, &entry);
	if (!s || !navidrome_playback_current(&s->context, false)) return false;
	song_metadata_t ignored;
	source_metadata(s, (size_t)entry, &ignored);
	bool ok = false;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < ND_WINDOW * 2; i++)
		if (s->window_age[i] && s->window_entry[i] == entry && s->window_valid[i]) {
			*row = s->window[i]; *lib = s->library; ok = true; break;
		}
	pthread_mutex_unlock(&lock);
	return ok;
}
static void capture_album_order(source_t *s) {
	if (s->query.kind != ND_TRACKS || s->query.filter_kind != ND_ALBUMS || !s->query.filter[0]) return;
	s->album_origin = true;
	s->album_order = nd_sort_preference(ND_ALBUMS, false);
	s->album_error = "nd_album_order_required";
	navidrome_library_snapshot(&s->album_library);
	if (s->album_library.ready && s->album_library.card_epoch == s->library.card_epoch && !strcmp(s->album_library.scope, s->library.scope)) {
		s->album_pinned = navidrome_playback_pin_source(&s->album_library);
		if (!s->album_pinned) s->album_error = "nd_queue_limit";
	}
}

bool navidromeplay_select(const nd_library_t *lib, const nd_query_t *query, const char *id, void *origin) {
	if (!started || !id || strlen(id) >= ND_ID_MAX || !navidrome_playback_pin_source(lib))
		return false;
	source_t *s = calloc(1, sizeof(*s));
	if (!s) {
		nd_store_release_source(lib);
		return false;
	}
	s->refs = 1;
	s->library = *lib;
	s->query = *query;
	capture_album_order(s);
	s->origin = origin;
	snprintf(s->selected, sizeof(s->selected), "%s", id);
	bool ok = navidrome_context(&s->context, false) && s->context.epoch == lib->epoch && s->context.card_epoch == lib->card_epoch;
	pthread_mutex_lock(&lock);
	s->token = ++next_token;
	pthread_mutex_unlock(&lock);
	if (ok)
		ok = submit(s, 0, true, true, NULL, NULL);
	release(s);
	return ok;
}
void navidromeplay_playlist_leave(void *origin) {
	if (addition_origin == origin) {
		addition_origin_serial++;
		addition_origin = NULL;
	}
	if (playlist_origin == origin) {
		playlist_origin_serial++;
		playlist_origin = NULL;
	}
}
bool navidromeplay_playlist(const nd_library_t *lib, int64_t playlist, int64_t revision,
		int ordinal, int64_t occurrence, const char *track, void *origin) {
	if (!started || playlist <= 0 || revision <= 0 || ordinal < 0 || occurrence <= 0 ||
		!track || !*track || strlen(track) >= ND_ID_MAX || !*lib->path || !*lib->scope)
		return false;
	source_t *s = calloc(1, sizeof(*s));
	if (!s) return false;
	s->refs = 1;
	s->capture = true;
	s->library = *lib;
	s->saved = (nd_playlist_capture_t){.playlist = playlist, .revision = revision, .ordinal = ordinal, .occurrence = occurrence};
	snprintf(s->saved.track, sizeof(s->saved.track), "%s", track);
	s->origin = origin;
	playlist_origin = origin;
	s->playlist_origin_serial = ++playlist_origin_serial;
	bool ok = navidrome_context(&s->context, false) && s->context.card_epoch == lib->card_epoch && s->context.epoch == lib->epoch;
	pthread_mutex_lock(&lock);
	s->token = ++next_token;
	pthread_mutex_unlock(&lock);
	if (ok) ok = submit(s, 0, true, true, NULL, NULL);
	release(s);
	return ok;
}
static source_t *saved_source(const nd_library_t *lib, const nd_playlist_capture_t *request) {
	if (!started || !lib || !request || request->playlist <= 0 || request->revision <= 0 || request->count <= 0 || !lib->path[0] || !lib->scope[0] || ((request->whole == request->single) && !request->view) || ((!request->whole || request->single) && (request->ordinal < 0 || request->occurrence <= 0 || !request->track[0] || strnlen(request->track, ND_ID_MAX) == ND_ID_MAX)))
		return NULL;
	source_t *s = calloc(1, sizeof(*s));
	if (!s) return NULL;
	s->refs = 1; s->capture = true; s->library = *lib; s->saved = *request;
	bool ok = navidrome_context(&s->context, false) && s->context.card_epoch == lib->card_epoch && s->context.epoch == lib->epoch;
	if (!ok) { release(s); return NULL; }
	pthread_mutex_lock(&lock); s->token = ++next_token; pthread_mutex_unlock(&lock);
	return s;
}
bool navidromeplay_playlist_view(const nd_library_t *lib, const nd_playlist_capture_t *request, bool shortcut, bool shuffle, void *origin) {
	if (!request || !request->view || request->single || request->whole != shortcut)
		return false;
	source_t *s = saved_source(lib, request);
	if (!s)
		return false;
	s->saved_shortcut = shortcut;
	s->saved_shuffle = shuffle;
	if (shortcut) {
		s->saved_random = lv_rand(0, 0x7fffffff);
		s->album_mode = shuffle ? (config_get_bool("player", "endless_shuffle", false) ? PLAYBACK_MODE_SHUFFLE_REPEAT : PLAYBACK_MODE_SHUFFLE) : PLAYBACK_MODE_NORMAL;
	}
	s->origin = origin;
	playlist_origin = origin;
	s->playlist_origin_serial = ++playlist_origin_serial;
	bool ok = submit(s, 0, true, true, NULL, NULL);
	release(s);
	return ok;
}

bool navidromeplay_playlist_shortcut(const nd_library_t *lib, int64_t playlist,
		int64_t revision, int count, bool shuffle, void *origin) {
	nd_playlist_capture_t request = {.playlist = playlist, .revision = revision, .count = count, .whole = true};
	source_t *s = saved_source(lib, &request);
	if (!s) return false;
	s->saved_shortcut = true; s->saved_shuffle = shuffle;
	s->saved_random = lv_rand(0, 0x7fffffff);
	s->album_mode = shuffle ? (config_get_bool("player", "endless_shuffle", false) ?
		PLAYBACK_MODE_SHUFFLE_REPEAT : PLAYBACK_MODE_SHUFFLE) : PLAYBACK_MODE_NORMAL;
	s->origin = origin;
	playlist_origin = origin;
	s->playlist_origin_serial = ++playlist_origin_serial;
	bool ok = submit(s, 0, true, true, NULL, NULL);
	release(s);
	return ok;
}
bool navidromeplay_playlist_enqueue(const nd_library_t *lib, const nd_playlist_capture_t *request, void *origin) {
	if (!request)
		return false;
	nd_playlist_capture_t stored = *request;
	stored.view = false;
	source_t *s = saved_source(lib, &stored);
	if (!s) return false;
	pthread_mutex_lock(&lock);
	bool ok = !quitting && !adding;
	if (ok) {
		s->origin = origin;
		addition_origin = origin;
		s->addition_origin_serial = ++addition_origin_serial;
		adding = true;
		addition_job = (job_t){.source = s, .serial = serial, .queue_identity = playlist_identity(), .enqueue = true};
		retain(s); addition_pending = true; pthread_cond_signal(&meta_wake);
	}
	pthread_mutex_unlock(&lock);
	release(s);
	if (ok && timer) lv_timer_resume(timer);
	return ok;
}
bool navidromeplay_search(const nd_library_t *lib, const nd_context_t *identity, const nd_query_t *query, const char *id, void *origin) {
	if (!started || !id || !*id || strlen(id) >= ND_ID_MAX || query->kind != ND_TRACKS || !*lib->path || !*lib->scope)
		return false;
	source_t *s = calloc(1, sizeof(*s));
	if (!s)
		return false;
	s->refs = 1;
	s->capture = true;
	s->library = *lib;
	s->query = *query;
	s->origin = origin;
	snprintf(s->selected, sizeof(s->selected), "%s", id);
	bool ok = navidrome_context(&s->context, false) && s->context.profile_epoch == identity->profile_epoch && s->context.card_epoch == lib->card_epoch;
	pthread_mutex_lock(&lock);
	s->token = ++next_token;
	pthread_mutex_unlock(&lock);
	if (ok)
		ok = submit(s, 0, true, true, NULL, NULL);
	release(s);
	return ok;
}
static bool prepare_selection(const char *ref, const char *failure) {
	if (!navidromeplay_reference(ref))
		return true;
	// Entering here is an explicit retry or an already approved automatic
	// transition. A failed prefetch is blocked separately by auto().
	for (int i = 0; i < 4; i++)
		if (!strcmp(ref, failed_refs[i]))
			failed_refs[i][0] = 0;
	int entry;
	source_t *s = reference_source(ref, &entry);
	if (s) {
		queue_source = s;
		snprintf(current_ref, sizeof(current_ref), "%s", ref);
		current_path[0] = 0;
		memset(&current_track, 0, sizeof(current_track));
		song_metadata_t meta = {0};
		snprintf(meta.title, sizeof(meta.title), "%s", tr("nd_working"));
		nd_playrow_t shown;
		nd_library_t library;
		double duration=0;
		navidromeplay_retry_metadata(ref);
		if (navidromeplay_lookup(ref,&shown,&library)) {
			current_track=shown;metadata(&shown,&meta);duration=shown.row.duration;
		}
		device_state_represent_source(ref, &meta, duration);
		submit(s, entry, false, true, ref, failure);
		player_refresh_now_playing();
	}
	return false;
}
bool navidromeplay_prepare(const char *ref) { return prepare_selection(ref, NULL); }
void navidromeplay_failed_selection(const char *ref) {
	const char *error = "nd_audio_unavailable";
	for (int i = 0; i < 4; i++) if (!strcmp(ref, failed_refs[i]) && failed_reasons[i]) error = failed_reasons[i];
	int entry;
	source_t *s = reference_source(ref, &entry);
	if (s && !navidrome_playback_current(&s->context, true)) error = "nd_track_reconnect";
	prepare_selection(ref, error);
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
	if (!path || !current_track.row.id[0] || (strcmp(path, current_path) && strcmp(path, current_ref)) || !queue_source || !navidrome_playback_current(&queue_source->context, false))
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
	if (album_owner && (album_queue_identity != playlist_identity() || !playlist_has_source())) album_origin(NULL);

	pthread_mutex_lock(&lock);
	result_t *result = fallback_result;
	fallback_result = NULL;
	result_t *addition = addition_fallback;
	addition_fallback = NULL;
	source_t *failed_addition = addition_failed;
	addition_failed = NULL;
	if (failed_addition) adding = false;
	metadata_result_t *titles = metadata_fallback;
	metadata_fallback = NULL;
	annotation_t *annotation = annotation_fallback;
	annotation_fallback = NULL;
	pthread_mutex_unlock(&lock);
	if (failed_addition) {
		if (navidrome_playback_current(&failed_addition->context, false)) addition_error(failed_addition, "nd_worker_error");
		release(failed_addition);
	}
	while (titles) {
		metadata_result_t *next = titles->next;
		metadata_done(titles);
		titles = next;
	}
	if (result)
		result_done(result);
	if (addition)
		result_done(addition);
	if (annotation)
		annotation_done(annotation);
	if (queue_source && current_track.row.id[0]) {
		nd_library_t favorites;
		navidrome_favorites_snapshot(&favorites);
		if (favorites.generation != annotation_revision) {
			annotation_t *a = calloc(1, sizeof(*a));
			if (a) {
				a->source = queue_source;
				retain(a->source);
				a->row = current_track;
				a->generation = favorites.generation;
				annotation_revision = favorites.generation;
				pthread_mutex_lock(&lock);
				annotation_t *old = annotation_pending;
				annotation_pending = a;
				pthread_cond_signal(&meta_wake);
				pthread_mutex_unlock(&lock);
				if (old) {
					release(old->source);
					free(old);
				}
			}
		}
	}
	trackmenu_source_metadata_ready(NULL);
	pthread_mutex_lock(&lock);
	bool revoked = invalidated;
	invalidated = false;
	bool busy = pending || running || adding;
	pthread_mutex_unlock(&lock);
	if (busy || queue_source)
		navidrome_poll_network();
	(void)revoked;
	bool stale = false;
	for (int i = 0; i < SOURCE_CONTEXTS; i++)
		if (sources[i] && !navidrome_playback_current(&sources[i]->context, false)) stale = true;
	if (stale) {
		album_origin(NULL);
		device_state_t state;
		device_state_get(&state);
		if (nd_audio_source(state.current_file))
			device_state_clear_prepared();
		playlist_drop_sources();
		current_path[0] = current_ref[0] = 0;
		memset(&current_track, 0, sizeof(current_track));
		memset(ahead_refs, 0, sizeof(ahead_refs));
		memset(ahead_paths, 0, sizeof(ahead_paths));
		protections();
		nd_audio_set_network_wanted(false);
		player_refresh_now_playing();
		return;
	}
	if (!queue_source || busy) {
		bool owners = false;
		for (int i = 0; i < SOURCE_CONTEXTS; i++) owners |= sources[i] != NULL;
		if (!owners && !queue_source && !busy && timer)
			lv_timer_pause(timer);
		return;
	}
	device_state_t state;
	device_state_get(&state);
	if (!nd_audio_source(state.current_file) || state.status != AUDIO_STATUS_PLAYING)
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
		int entry;
		source_t *s = reference_source(ref, &entry);
		if (s) {
			submit(s, entry, false, false, ref, NULL);
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
	nd_reports_init();
	timer = lv_timer_create(tick, 250, NULL);
	lv_timer_pause(timer);
	navidrome_set_playback_invalidator(invalidate);
	navidrome_set_playback_network_stop(nd_audio_cancel);
}
void navidromeplay_shutdown(void) {
	if (!started)
		return;
	nd_reports_shutdown();
	navidrome_set_playback_invalidator(NULL);
	navidrome_set_playback_network_stop(NULL);
	pthread_mutex_lock(&lock);
	quitting = true;
	if (addition_db) sqlite3_interrupt(addition_db);
	serial++;
	pthread_cond_broadcast(&wake);
	pthread_cond_broadcast(&meta_wake);
	pthread_mutex_unlock(&lock);
	nd_audio_cancel();
	pthread_join(worker, NULL);
	pthread_join(meta_worker, NULL);
	if (pending) {
		if (next_job.enqueue) adding = false;
		release(next_job.source);
	}
	for (int i = 0; i < meta_count; i++) {
		source_t *s = meta_sources[i];
		for (int slot = 0; slot < ND_WINDOW * 2; slot++) if (s->window_pending[slot] && s->window_result[slot]) {
			s->window_pending[slot] = false;
			metadata_done(s->window_result[slot]);
		}
	}
	while (metadata_fallback) {
		metadata_result_t *r = metadata_fallback;
		metadata_fallback = r->next;
		metadata_done(r);
	}
	for (int i = 0; i < meta_count; i++) release(meta_sources[i]);
	meta_count = 0;
	if (addition_pending) { release(addition_job.source); addition_pending=false; }
	if (addition_fallback) { release(addition_fallback->job.source); free(addition_fallback); addition_fallback=NULL; }
	if (addition_failed) { release(addition_failed); addition_failed = NULL; }
	adding=false;
	if (annotation_pending) {
		release(annotation_pending->source);
		free(annotation_pending);
		annotation_pending = NULL;
	}
	if (annotation_fallback) {
		release(annotation_fallback->source);
		free(annotation_fallback);
		annotation_fallback = NULL;
	}
	pending = false;
	if (fallback_result) {
		release(fallback_result->job.source);
		free(fallback_result);
		fallback_result = NULL;
	}
	if (playlist_has_source())
		playlist_clear();
	album_origin(NULL);
	nd_audio_protect(NULL);
	nd_audio_set_network_wanted(false);
	if (timer)
		lv_timer_delete(timer);
	timer = NULL;
	started = false;
}

// Album metadata is chosen on the existing worker. The enum deliberately
// matches the presentation's Shuffle/Play/Queue order without a GUI dependency.
bool navidromeplay_album(const nd_library_t *lib, const nd_query_t *query, int action, void *origin) {
	if (!started || !lib || !query || action < 0 || action > 2 || query->kind != ND_TRACKS || query->filter_kind != ND_ALBUMS || !*query->filter || !navidrome_playback_pin_source(lib))
		return false;
	source_t *s = calloc(1, sizeof(*s));
	if (!s) {
		nd_store_release_source(lib);
		return false;
	}
	s->refs = 1;
	s->library = *lib;
	s->query = *query;
	s->origin = origin;
	if (action != 2)
		capture_album_order(s);
	s->album_action = action + 1;
	s->album_mode = action == 0 ? config_get_bool("player", "endless_shuffle", false) ? PLAYBACK_MODE_SHUFFLE_REPEAT : PLAYBACK_MODE_SHUFFLE : PLAYBACK_MODE_NORMAL;
	s->album_random = lv_rand(0, 0x7fffffff);
	s->enqueue_kind = ND_TRACKS;
	bool ok = navidrome_context(&s->context, false) && s->context.epoch == lib->epoch && s->context.card_epoch == lib->card_epoch;
	pthread_mutex_lock(&lock);
	if (ok && !quitting) {
		source_t *old = adding ? addition_job.source : (pending || running) ? next_job.source : NULL;
		if (old && !old->album_committed && old->album_action == s->album_action && old->context.profile_epoch == s->context.profile_epoch && !strcmp(old->query.filter, s->query.filter)) {
			pthread_mutex_unlock(&lock);
			release(s);
			return true;
		}
		s->token = ++next_token;
		if (action == 2) {
			ok = !adding;
			if (ok) {
				s->album_intent = ++album_intent;
				adding = true;
				addition_job = (job_t){.source = s, .serial = serial, .queue_identity = playlist_identity(), .enqueue = true};
				retain(s);
				addition_pending = true;
				pthread_cond_signal(&meta_wake);
			}
		} else
			s->album_intent = ++album_intent;
	} else
		ok = false;
	pthread_mutex_unlock(&lock);
	if (ok && action != 2)
		ok = submit(s, 0, true, true, NULL, NULL);
	release(s);
	if (ok && timer)
		lv_timer_resume(timer);
	return ok;
}

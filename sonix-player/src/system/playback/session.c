#define _GNU_SOURCE 1
#include "session.h"
#include "device_state.h"
#include "sessionmusic.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/power.h"
#include "src/system/streaming/navidromeplay.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/tidalcache.h"
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct job {
	session_header_t header;
	unsigned intent, serial, queue_revision, card_epoch, request;
	int runtime_selected;
	bool load, position_only, legacy_adoption, flush;
	unsigned barrier;
	playlist_checkpoint_t *capture;
	session_music_t *music;
	char root[1024];
	char file[512];
} job_t;
typedef struct result {
	job_t *job;
	session_header_t header;
	int state; // 0 legacy, 1 ready/pending, -1 bad state, -2 save failure
	uint32_t *order;
	char **local;
	session_row_t selected;
	session_identity_t file_identity;
	session_music_t *music;
	char path[1300];
	unsigned profile, card;
} result_t;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static job_t *pending;
static result_t *fallback;
static bool started, working, quit, installing, blocked, loading, enabled, adopting, flushing;
static unsigned intent, serial, identity, revision, observed_service, observed_card, published_intent;
static unsigned published_queue_revision;
static int tracked_owner;
static unsigned tracked_card;
static char tracked_scope[33];
static unsigned saved_ms;
static audio_status_t saved_status;
static double saved_position;
static char saved_file[512], journal[1300];
static session_header_t active, committed;
static unsigned request_sequence, latest_delivery, committed_revision, observed_revision;
static int published_selected, committed_selected, observed_selected;
static bool write_failed;
static char published_file[512], committed_root[1024];
static unsigned committed_serial, committed_intent, committed_card;
static char committed_file[512];
static bool dirty, suspended_local;
static unsigned allocation_failed, other_pending;
static unsigned retry_ms;
static bool (*legacy)(void);
static lv_timer_t *session_timer;
static void tick(lv_timer_t *unused);
static void session_notice(void *unused) {
	(void)unused;
	tick(session_timer);
}
void playback_session_service_changed(void) {
	if (started && enabled) {
		if (blocked && !suspended_local && !loading)
			observed_service = observed_card = UINT_MAX;
		gui_post_signal(session_notice, NULL);
	}
}
static unsigned now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}
static bool current(const job_t *j) { return j->serial == __atomic_load_n(&serial, __ATOMIC_ACQUIRE); }
static void free_job(job_t *j) {
	if (j) {
		playlist_checkpoint_free(j->capture);
		session_music_free(j->music);
		free(j);
	}
}
static void free_result(result_t *r) {
	if (!r)
		return;
	for (int i = 0; r->local && i < r->header.count; i++)
		free(r->local[i]);
	free(r->local);
	free(r->order);
	session_music_free(r->music);
	free_job(r->job);
	free(r);
}
static void submit(job_t *j) {
	pthread_mutex_lock(&mutex);
	if (quit || !current(j) || (j->flush && (j->barrier != request_sequence || observed_revision != j->queue_revision || observed_selected != j->runtime_selected))) {
		pthread_mutex_unlock(&mutex);
		free_job(j);
		return;
	}
	job_t *old = pending;
	j->request = ++request_sequence;
	pending = j;
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&mutex);
	free_job(old);
}
static void pending_view(const char *reason) {
	song_metadata_t m = {0};
	snprintf(m.title, sizeof(m.title), "%s", tr(reason));
	m.has_tags = true;
	device_state_restore_representation("ndq:0:0", &m, active.position, 0);
	player_refresh_restored();
}
static void delivered(void *user) {
	result_t *r = user;
	job_t *j = r->job;
	if (!started || !current(j) || !enabled || (int32_t)(j->request - latest_delivery) < 0) {
		free_result(r);
		return;
	}
	latest_delivery = j->request;
	loading = false;
	if (r->state == -2) {
		toast_plain(tr("session_save_error"));
		dirty = true;
		retry_ms = now_ms();
		free_result(r);
		return;
	}
	if (!j->load) {
		active = r->header;
		published_intent = j->intent;
		published_queue_revision = j->queue_revision;
		published_selected = j->runtime_selected;
		snprintf(published_file, sizeof(published_file), "%s", j->file);
		dirty = false;
		free_result(r);
		return;
	}
	if (r->state == 0) {
		installing = true;
		bool restored = legacy && legacy();
		installing = false;
		if (restored) {
			adopting = true;
			playback_session_note();
			adopting = false;
			if (config_get_bool("music", "nowplaying_at_boot", false))
				player_sheet_open(false);
		}
		free_result(r);
		return;
	}
	active = r->header;
	if (active.owner == SESSION_NONE && r->state >= 0) {
		blocked = false;
		free_result(r);
		return;
	}
	navidrome_snapshot_t service;
	navidrome_snapshot(&service);
	nd_library_t lib;
	navidrome_library_snapshot(&lib);
	nd_context_t context;
	bool context_ok = navidrome_context(&context, false);
	bool available = r->state == 1 && r->header.ready && r->local && r->card == lib.card_epoch;
	if (active.owner == SESSION_MUSIC)
		available = r->state == 1 && r->music && r->card == lib.card_epoch;
	if (active.owner == SESSION_NAVIDROME)
		available = available && context_ok && service.unlocked && !service.invalid && r->profile == context.profile_epoch && !strcmp(active.scope, lib.scope);
	nd_wipe(&context, sizeof(context));
	installing = true;
	bool restored = false;
	if (available && active.owner == SESSION_NAVIDROME) {
		restored = navidromeplay_restore(r->path, &active, r->order, r->local, &r->selected, &r->file_identity);
		if (restored)
			r->local = NULL;
	} else if (available && active.owner == SESSION_MUSIC) {
		session_music_t *m = r->music;
		if (m->query.valid) {
			library_index_t *ix = library_index_open(m->query.kind, m->query.filter, m->query.value, m->query.order, m->query.desc);
			if (ix && library_index_count(ix) > 0) {
				player_restore_index(ix, m->current, (const char *const *)m->paths, m->slots, m->count, m->selected, active.position);
				restored = true;
			} else
				library_index_close(ix);
		} else if (m->count > 0) {
			player_restore_list((const char *const *)m->paths, m->count, m->current, m->custom, m->selected, active.position);
			restored = true;
		}
		if (restored && m->album[0]) {
			playlist_set_album(m->album);
			playlist_note_album_restored();
		}
	}
	installing = false;
	blocked = !restored;
	tracked_owner = active.owner;
	tracked_card = lib.card_epoch;
	snprintf(tracked_scope, sizeof(tracked_scope), "%s", active.scope);
	if (restored) {
		published_queue_revision = playlist_revision();
		identity = playlist_identity();
		revision = playlist_revision();
		device_state_t state;
		device_state_get(&state);
		snprintf(saved_file, sizeof(saved_file), "%s", state.current_file);
		saved_position = active.position;
		saved_status = AUDIO_STATUS_PAUSED;
		saved_ms = now_ms();
		pthread_mutex_lock(&mutex);
		committed = active;
		committed_serial = j->serial;
		committed_intent = j->intent;
		committed_card = lib.card_epoch;
		snprintf(committed_root, sizeof(committed_root), "%s", j->root);
		committed_revision = observed_revision = revision;
		committed_selected = observed_selected = published_selected = playlist_current_index();
		snprintf(published_file, sizeof(published_file), "%s", state.current_file);
		snprintf(committed_file, sizeof(committed_file), "%s", state.current_file);
		pthread_mutex_unlock(&mutex);
	} else {
		const char *reason = "session_restore_unavailable";
		if (active.owner == SESSION_NAVIDROME) {
			if (!service.configured)
				reason = "nd_not_configured";
			else if (service.invalid)
				reason = "nd_vault_error";
			else if (!service.unlocked)
				reason = "nd_unlock_required";
		}
		pending_view(reason);
	}
	if (config_get_bool("music", "nowplaying_at_boot", false))
		player_sheet_open(false);
	free_result(r);
}
static void post(result_t *r) {
	if (!current(r->job)) {
		free_result(r);
		return;
	}
	if (gui_post(delivered, r))
		return;
	pthread_mutex_lock(&mutex);
	result_t *old = fallback;
	fallback = r;
	pthread_mutex_unlock(&mutex);
	free_result(old);
	gui_post_signal(session_notice, NULL);
}
typedef struct {
	job_t *job;
	nd_context_t context;
	bool held;
} lease_t;
static bool renew_lease(void *user) {
	lease_t *lease = user;
	if (lease->held)
		navidrome_cache_end();
	lease->held = false;
	if (!current(lease->job))
		return false;
	usleep((nd_audio_network_wanted() || navidromeplay_preparing()) ? 10000 : 1000);
	lease->held = navidrome_playback_begin(&lease->context);
	return lease->held;
}
static bool load_payload(result_t *r) {
	job_t *j = r->job;
	char card[33];
	if (!r->header.ready || !session_card(j->root, false, card) || strcmp(card, r->header.card))
		return false;
	navidrome_snapshot_t service;
	navidrome_snapshot(&service);
	nd_library_t lib;
	navidrome_library_snapshot(&lib);
	r->card = lib.card_epoch;
	r->profile = 0;
	if (r->header.owner == SESSION_MUSIC) {
		bool held = navidrome_session_card_begin(r->card, j->root);
		bool ok = held && session_card(j->root, false, card) && !strcmp(card, r->header.card);
		if (ok)
			r->music = session_music_read(&r->header, j->root);
		if (held)
			navidrome_cache_end();
		return ok && r->music != NULL;
	}
	if (!service.unlocked || service.invalid || strcmp(lib.scope, r->header.scope))
		return false;
	nd_context_t context;
	bool root_lease = navidrome_session_card_begin(r->card, j->root);
	bool lease = root_lease && navidrome_context(&context, false) && navidrome_playback_begin(&context);
	if (root_lease)
		navidrome_cache_end();
	if (lease)
		r->profile = context.profile_epoch;
	if (!lease) {
		nd_wipe(&context, sizeof(context));
		return false;
	}
	lease_t io = {.job = j, .context = context, .held = true};
	sqlite3 *db = NULL;
	bool ok = session_card(j->root, false, card) && !strcmp(card, r->header.card) && session_path(j->root, r->header.key, r->path, sizeof(r->path)) && session_file_identity(r->path, &r->file_identity) && session_payload_open_scoped(r->path, &r->header, &db, &r->order, renew_lease, &io);
	if (ok) {
		r->local = calloc((size_t)r->header.count, sizeof(*r->local));
		ok = r->local != NULL;
	}
	int service_rows = 0;
	for (int i = 0; ok && i < r->header.count; i++) {
		if (i && i % 128 == 0) {
			sqlite3_close(db);
			db = NULL;
			ok = renew_lease(&io) && sqlite3_open_v2(r->path, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOFOLLOW, NULL) == SQLITE_OK;
			if (!ok)
				break;
		}
		session_row_t row;
		ok = current(j) && session_payload_row(db, i, &row);
		if (!ok)
			break;
		if (row.local[0]) {
			char path[1536];
			int n = snprintf(path, sizeof(path), "%s/%s", j->root, row.local);
			ok = n > 0 && n < 512;
			if (ok) {
				r->local[i] = strdup(path);
				ok = r->local[i] != NULL;
			}
		} else
			service_rows++;
		if ((uint32_t)i == r->order[r->header.selected])
			r->selected = row;
	}
	if (db)
		sqlite3_close(db);
	if (ok)
		ok = session_file_matches(r->path, &r->file_identity);
	if (io.held)
		navidrome_cache_end();
	nd_wipe(&io.context, sizeof(io.context));
	nd_wipe(&context, sizeof(context));
	return ok && service_rows > 0;
}
static bool read_current(void *user, int canonical, session_row_t *row) {
	job_t *j = user;
	return current(j) && playlist_checkpoint_read(j->capture, canonical, row);
}
static void *run(void *unused) {
	(void)unused;
	thread_be_background("session writer");
	session_header_t durable = {0};
	int initial = session_journal_read(journal, &durable);
	int64_t base = initial == 1 ? durable.activation : 0;
	unsigned durable_queue_revision = UINT_MAX;
	for (;;) {
		pthread_mutex_lock(&mutex);
		while (!pending && !quit)
			pthread_cond_wait(&wake, &mutex);
		if (quit && !pending) {
			pthread_mutex_unlock(&mutex);
			return NULL;
		}
		job_t *j = pending;
		pending = NULL;
		working = true;
		pthread_mutex_unlock(&mutex);
		int journal_state = 1;
		if (!j->load) {
			session_header_t observed;
			journal_state = session_journal_read(journal, &observed);
			if (journal_state == 1) {
				if (observed.activation != durable.activation || observed.revision != durable.revision || strcmp(observed.key, durable.key))
					durable_queue_revision = UINT_MAX;
				durable = observed;
			}
		}
		result_t *r = calloc(1, sizeof(*r));
		if (!r) {
			__atomic_store_n(&allocation_failed, 1, __ATOMIC_RELEASE);
			gui_post_signal(session_notice, NULL);
			pthread_mutex_lock(&mutex);
			write_failed = true;
			pthread_mutex_unlock(&mutex);
			if (current(j) && base <= INT64_MAX - (int64_t)j->intent) {
				session_header_t fence = j->header;
				fence.activation = base + (int64_t)j->intent;
				if (fence.activation <= 0)
					fence.activation = 1;
				fence.ready = 0;
				if ((fence.activation != durable.activation || fence.owner != durable.owner) && session_journal_write(journal, &fence))
					durable = fence;
			}
			free_job(j);
			pthread_mutex_lock(&mutex);
			working = false;
			pthread_cond_broadcast(&wake);
			pthread_mutex_unlock(&mutex);
			continue;
		}
		r->job = j;
		r->header = j->header;
		if (j->load) {
			int found = session_journal_read(journal, &r->header);
			r->state = found;
			if (found == 1 && r->header.owner == SESSION_NONE) {
				bool held = navidrome_session_card_begin(j->card_epoch, j->root);
				if (held) {
					session_payload_budget(j->root, NULL, navidromeplay_session_pinned, false);
					navidrome_cache_end();
				}
			}
			if (found == 1 && r->header.owner != SESSION_NONE && !load_payload(r))
				r->state = -1;
		} else if (current(j) && journal_state >= 0) {
			session_header_t h = j->header;
			bool ok = base <= INT64_MAX - (int64_t)j->intent && durable.revision < INT64_MAX;
			h.activation = ok ? base + (int64_t)j->intent : 0;
			if (!j->position_only && durable.ready && h.owner == SESSION_NAVIDROME && durable.owner == SESSION_NAVIDROME && h.activation == durable.activation && j->queue_revision == durable_queue_revision && !strcmp(h.scope, durable.scope)) {
				j->position_only = true;
				h.revision = durable.revision;
			}
			if (!j->position_only)
				h.revision = durable.revision < INT64_MAX ? durable.revision + 1 : 0;
			if (h.activation <= 0)
				h.activation = 1;
			if (ok && !(j->legacy_adoption && journal_state == 0 && h.owner == SESSION_MUSIC) && (h.activation != durable.activation || h.owner != durable.owner)) {
				session_header_t fence = h;
				fence.ready = 0;
				ok = session_journal_write(journal, &fence);
				if (ok)
					durable = fence;
			}
			if (ok && h.owner == SESSION_NONE) {
				h.ready = 0;
				ok = current(j) && session_journal_write(journal, &h);
			} else if (ok && j->position_only) {
				if (durable.ready && durable.activation == h.activation && durable.revision == h.revision) {
					snprintf(h.key, sizeof(h.key), "%s", durable.key);
					snprintf(h.card, sizeof(h.card), "%s", durable.card);
					h.ready = 1;
					ok = current(j) && session_journal_write(journal, &h);
				} else
					ok = false;
			} else if (ok) {
				pthread_mutex_lock(&mutex);
				struct timespec until;
				clock_gettime(CLOCK_REALTIME, &until);
				until.tv_sec += 1;
				until.tv_nsec += 500000000L;
				if (until.tv_nsec >= 1000000000L) {
					until.tv_sec++;
					until.tv_nsec -= 1000000000L;
				}
				while (!pending && !flushing && !quit && current(j))
					if (pthread_cond_timedwait(&wake, &mutex, &until) != 0)
						break;
				bool superseded = pending || !current(j);
				pthread_mutex_unlock(&mutex);
				if (superseded) {
					free_result(r);
					pthread_mutex_lock(&mutex);
					working = false;
					pthread_cond_broadcast(&wake);
					pthread_mutex_unlock(&mutex);
					continue;
				}
				bool card_lease = navidrome_session_card_begin(j->card_epoch, j->root);
				ok = card_lease && session_card(j->root, true, h.card);
				if (card_lease)
					navidrome_cache_end();
				h.ready = 1;
				if (ok && h.owner == SESSION_MUSIC) {
					h.key[0] = 0;
					bool held = navidrome_session_card_begin(j->card_epoch, j->root);
					if (held)
						session_music_prune(&durable, j->root);
					ok = held && session_music_write(&h, j->music, j->root);
					if (held)
						navidrome_cache_end();
				} else if (ok) {
					char path[1300];
					lease_t io = {.job = j};
					nd_library_t binding;
					navidrome_library_snapshot(&binding);
					ok = binding.card_epoch == j->card_epoch && !strcmp(binding.scope, h.scope) && navidrome_context(&io.context, false) && navidrome_playback_begin(&io.context);
					io.held = ok;
					if (ok)
						ok = session_payload_budget(j->root, durable.key, navidromeplay_session_pinned, true) && session_random_id(h.key) && session_path(j->root, h.key, path, sizeof(path)) && session_payload_write_scoped(path, &h, playlist_checkpoint_order(j->capture), read_current, j, renew_lease, &io);
					if (io.held)
						navidrome_cache_end();
					nd_wipe(&io.context, sizeof(io.context));
					if (ok && !current(j))
						ok = false;
				}
				if (ok)
					ok = current(j) && session_journal_write(journal, &h);
			}
			if (ok) {
				durable = h;
				durable_queue_revision = j->queue_revision;
				pthread_mutex_lock(&mutex);
				committed = h;
				committed_serial = j->serial;
				committed_intent = j->intent;
				committed_card = j->card_epoch;
				snprintf(committed_root, sizeof(committed_root), "%s", j->root);
				committed_revision = j->queue_revision;
				committed_selected = j->runtime_selected;
				snprintf(committed_file, sizeof(committed_file), "%s", j->file);
				pthread_mutex_unlock(&mutex);
				bool held = navidrome_session_card_begin(j->card_epoch, j->root);
				char card[33];
				if (held && session_card(j->root, false, card) && (!h.card[0] || !strcmp(card, h.card))) {
					session_payload_budget(j->root, h.owner == SESSION_NAVIDROME ? h.key : NULL, navidromeplay_session_pinned, false);
					session_music_prune(&h, j->root);
				}
				if (held)
					navidrome_cache_end();
				r->header = h;
				r->state = 1;
			} else
				r->state = -2;
		}
		if (!j->load && journal_state < 0)
			r->state = -2;
		pthread_mutex_lock(&mutex);
		if (!j->load && current(j))
			write_failed = r->state == -2;
		pthread_mutex_unlock(&mutex);
		post(r);
		pthread_mutex_lock(&mutex);
		working = false;
		pthread_cond_broadcast(&wake);
		pthread_mutex_unlock(&mutex);
	}
}
static job_t *new_job(void) {
	job_t *j = calloc(1, sizeof(*j));
	if (!j) {
		__atomic_store_n(&allocation_failed, 1, __ATOMIC_RELEASE);
		gui_post_signal(session_notice, NULL);
		return NULL;
	}
	j->intent = intent;
	j->queue_revision = revision;
	nd_library_t binding;
	navidrome_library_snapshot(&binding);
	j->card_epoch = binding.card_epoch;
	j->serial = __atomic_load_n(&serial, __ATOMIC_ACQUIRE);
	snprintf(j->root, sizeof(j->root), "%s", playlist_card_root());
	return j;
}
static void request_load(void) {
	if (session_timer)
		lv_timer_resume(session_timer);
	if (loading)
		return;
	job_t *j = new_job();
	if (!j)
		return;
	j->load = true;
	loading = true;
	navidrome_snapshot_t snap;
	navidrome_snapshot(&snap);
	nd_library_t lib;
	navidrome_library_snapshot(&lib);
	observed_service = snap.revision;
	observed_card = lib.card_epoch;
	submit(j);
}
void playback_session_note(void) {
	if (session_timer && enabled)
		lv_timer_resume(session_timer);
	if (!started || !enabled || installing)
		return;
	__atomic_store_n(&other_pending, 0, __ATOMIC_RELEASE);
	device_state_t state;
	device_state_get(&state);
	bool different = identity != playlist_identity() || blocked;
	if (different) {
		intent++;
		__atomic_add_fetch(&serial, 1, __ATOMIC_RELEASE);
		identity = playlist_identity();
		blocked = false;
		suspended_local = false;
		loading = false;
	}
	job_t *j = new_job();
	if (!j)
		return;
	j->header = active;
	j->legacy_adoption = adopting;
	snprintf(j->file, sizeof(j->file), "%s", state.current_file);
	j->header.version = 1;
	j->header.owner = playlist_has_source() ? SESSION_NAVIDROME : SESSION_MUSIC;
	j->header.count = playlist_count();
	j->header.selected = playlist_current_index();
	j->runtime_selected = j->header.selected;
	j->header.position = state.progress_current_secs;
	j->header.mode = playlist_get_mode();
	j->header.revision = playlist_revision();
	j->queue_revision = playlist_revision();
	if (state.live || qobuzcache_owns(state.current_file) || tidalcache_owns(state.current_file) || j->header.count <= 0 || !state.current_file[0])
		j->header.owner = SESSION_NONE;
	tracked_owner = j->header.owner;
	tracked_card = j->card_epoch;
	if (tracked_owner == SESSION_NAVIDROME) {
		nd_library_t binding;
		navidrome_library_snapshot(&binding);
		tracked_card = binding.card_epoch;
		j->card_epoch = binding.card_epoch;
		snprintf(tracked_scope, sizeof(tracked_scope), "%s", binding.scope);
	}
	if (j->header.owner == SESSION_NAVIDROME && active.ready && active.owner == SESSION_NAVIDROME && published_intent == intent && published_queue_revision == playlist_revision()) {
		j->position_only = true;
		j->header.revision = active.revision;
	} else if (j->header.owner == SESSION_NAVIDROME) {
		nd_library_t lib;
		navidrome_library_snapshot(&lib);
		snprintf(j->header.scope, sizeof(j->header.scope), "%s", lib.scope);
		j->capture = playlist_checkpoint_capture();

	} else if (j->header.owner == SESSION_MUSIC) {
		j->music = session_music_capture(state.current_file, state.progress_current_secs);

		j->header.scope[0] = 0;
	}
	revision = playlist_revision();
	pthread_mutex_lock(&mutex);
	observed_revision = revision;
	observed_selected = j->runtime_selected;
	pthread_mutex_unlock(&mutex);
	saved_ms = now_ms();
	saved_status = state.status;
	saved_position = state.progress_current_secs;
	snprintf(saved_file, sizeof(saved_file), "%s", state.current_file);
	submit(j);
}
static void position_note(const device_state_t *state) {
	pthread_mutex_lock(&mutex);
	bool failed = write_failed;
	pthread_mutex_unlock(&mutex);
	if (dirty || failed)
		return;
	if (active.owner == SESSION_MUSIC && (published_selected != playlist_current_index() || strcmp(state->current_file, published_file)))
		return;
	if (!active.ready || published_intent != intent || published_queue_revision != revision || identity != playlist_identity() || strcmp(state->current_file, saved_file))
		return;
	job_t *j = new_job();
	if (!j)
		return;
	j->header = active;
	j->runtime_selected = playlist_current_index();
	if (j->header.owner == SESSION_NAVIDROME)
		j->header.selected = j->runtime_selected;
	j->header.position = state->progress_current_secs;
	j->position_only = true;
	pthread_mutex_lock(&mutex);
	observed_revision = j->queue_revision;
	observed_selected = j->runtime_selected;
	pthread_mutex_unlock(&mutex);
	snprintf(j->file, sizeof(j->file), "%s", state->current_file);
	saved_position = state->progress_current_secs;
	saved_ms = now_ms();
	saved_status = state->status;
	submit(j);
}
void playback_session_settings(void) {
	if (!started)
		return;
	bool on = config_get_int("player", "remember_track", 0) != 0;
	if (on == enabled)
		return;
	enabled = on;
	if (session_timer) {
		if (on)
			lv_timer_resume(session_timer);
		else
			lv_timer_pause(session_timer);
	}
	__atomic_add_fetch(&serial, 1, __ATOMIC_RELEASE);
	intent++;
	loading = blocked = false;
	if (on) {
		identity = 0;
		playback_session_note();
	} else {
		job_t *j = new_job();
		if (j) {
			j->header = (session_header_t){.version = 1, .owner = SESSION_NONE};
			submit(j);
		}
	}
}
void playback_session_forget(void) {
	if (!started)
		return;
	// Only an active Navidrome restore is retired; unrelated Music stays active.
	if (tracked_owner != SESSION_NAVIDROME)
		return;
	__atomic_add_fetch(&serial, 1, __ATOMIC_RELEASE);
	intent++;
	tracked_owner = SESSION_NONE;
	blocked = loading = false;
	job_t *j = new_job();
	if (j) {
		j->header = (session_header_t){.version = 1, .owner = SESSION_NONE};
		submit(j);
	}
}
void playback_session_suspend(void) {
	if (started && tracked_owner == SESSION_NAVIDROME) {
		device_state_t state;
		device_state_get(&state);
		suspended_local = state.current_file[0] && !navidromeplay_reference(state.current_file) && !nd_audio_source(state.current_file) && state.status != AUDIO_STATUS_STOPPED;
		blocked = true;
		loading = false;
		__atomic_add_fetch(&serial, 1, __ATOMIC_RELEASE);
	}
}
bool playback_session_blocked(void) { return started && blocked; }
static void other_delivered(void *unused) {
	(void)unused;
	if (!__atomic_exchange_n(&other_pending, 0, __ATOMIC_ACQ_REL) || !started)
		return;
	loading = blocked = suspended_local = false;
	tracked_owner = SESSION_NONE;
	intent++;
	identity = playlist_identity();
	revision = playlist_revision();
	job_t *j = new_job();
	if (j) {
		j->header = (session_header_t){.version = 1, .owner = SESSION_NONE};
		submit(j);
	}
}
void playback_session_other_started(void) {
	if (!started)
		return;
	navidromeplay_stop();
	__atomic_add_fetch(&serial, 1, __ATOMIC_RELEASE);
	if (!enabled)
		return;
	__atomic_store_n(&other_pending, 1, __ATOMIC_RELEASE);
	gui_post_signal(other_delivered, NULL); // retained flag, coalesced doorbell delivery
}
static void session_tick_body(lv_timer_t *unused) {
	(void)unused;
	other_delivered(NULL);
	pthread_mutex_lock(&mutex);
	result_t *r = fallback;
	fallback = NULL;
	pthread_mutex_unlock(&mutex);
	if (r)
		delivered(r);
	if (__atomic_exchange_n(&allocation_failed, 0, __ATOMIC_ACQ_REL)) {
		dirty = true;
		retry_ms = now_ms();
		toast_plain(tr("session_save_error"));
	}
	playback_session_settings();
	if (!enabled)
		return;
	navidrome_snapshot_t service;
	navidrome_snapshot(&service);
	nd_library_t lib;
	navidrome_library_snapshot(&lib);
	if (!blocked && tracked_owner == SESSION_MUSIC && lib.card_epoch != tracked_card) {
		blocked = true;
		loading = false;
		suspended_local = false;
		__atomic_add_fetch(&serial, 1, __ATOMIC_RELEASE);
	}
	if (!blocked && tracked_owner == SESSION_NAVIDROME && (!service.unlocked || service.invalid || lib.card_epoch != tracked_card || strcmp(lib.scope, tracked_scope)))
		playback_session_suspend();
	if (blocked && !suspended_local && (service.revision != observed_service || lib.card_epoch != observed_card))
		request_load();

	if (dirty && !blocked && !loading && identity == playlist_identity() && now_ms() - retry_ms >= 10000) {
		dirty = false;
		playback_session_note();
		return;
	}
	if (!blocked && !loading && tracked_owner != SESSION_NONE && playlist_count() == 0) {
		playback_session_note();
		return;
	}
	if (blocked || loading || identity != playlist_identity())
		return;
	if (revision != playlist_revision()) {
		playback_session_note();
		return;
	}
	device_state_t state;
	device_state_get(&state);
	if (state.live && tracked_owner != SESSION_NONE) {
		playback_session_note();
		return;
	}
	unsigned period = power_screen_is_on() ? 10000u : 30000u;
	if (state.status != saved_status || (state.status != AUDIO_STATUS_PLAYING && fabs(state.progress_current_secs - saved_position) >= 1) || (state.status == AUDIO_STATUS_PLAYING && now_ms() - saved_ms >= period))
		position_note(&state);
}
static void tick(lv_timer_t *unused) {
	if (!started)
		return;
	session_tick_body(unused);
	pthread_mutex_lock(&mutex);
	bool outstanding = pending || working || fallback;
	pthread_mutex_unlock(&mutex);
	bool observing = enabled && !blocked && !loading && tracked_owner != SESSION_NONE;
	if (session_timer) {
		if (outstanding || observing || dirty || __atomic_load_n(&allocation_failed, __ATOMIC_ACQUIRE))
			lv_timer_resume(session_timer);
		else
			lv_timer_pause(session_timer);
	}
}
void playback_session_flush(void) {
	if (!started)
		return;
	pthread_mutex_lock(&mutex);
	flushing = true;
	pthread_cond_signal(&wake);
	while (pending || working)
		pthread_cond_wait(&wake, &mutex);
	session_header_t last = committed;
	unsigned last_revision = committed_revision;
	int last_selected = committed_selected;
	unsigned last_serial = committed_serial, last_intent = committed_intent, last_card = committed_card, barrier = request_sequence;
	char root[1024];
	snprintf(root, sizeof(root), "%s", committed_root);
	bool coherent = !write_failed && observed_revision == committed_revision && observed_selected == committed_selected;
	flushing = false;
	char file[512];
	snprintf(file, sizeof(file), "%s", committed_file);
	pthread_mutex_unlock(&mutex);
	device_state_t state;
	device_state_get(&state);
	if (enabled && coherent && last.ready && file[0] && !strcmp(file, state.current_file) && isfinite(state.progress_current_secs) && state.progress_current_secs >= 0) {
		job_t *j = calloc(1, sizeof(*j));
		if (j) {
			j->flush = true;
			j->barrier = barrier;
			j->serial = last_serial;
			j->intent = last_intent;
			j->card_epoch = last_card;
			snprintf(j->root, sizeof(j->root), "%s", root);
			j->header = last;
			j->queue_revision = last_revision;
			j->runtime_selected = last_selected;
			j->header.position = state.progress_current_secs;
			j->position_only = true;
			snprintf(j->file, sizeof(j->file), "%s", file);
			submit(j);
			pthread_mutex_lock(&mutex);
			while (pending || working)
				pthread_cond_wait(&wake, &mutex);
			pthread_mutex_unlock(&mutex);
		}
	}
}
static void shutdown_session(void) {
	if (!started)
		return;
	playback_session_flush();
	pthread_mutex_lock(&mutex);
	quit = true;
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&mutex);
	pthread_join(worker, NULL);
	started = false;
	if (session_timer)
		lv_timer_pause(session_timer);
}
void playback_session_reset(void) {
	if (!started)
		return;
	__atomic_add_fetch(&serial, 1, __ATOMIC_RELEASE);
	enabled = false;
	blocked = loading = false;
	pthread_mutex_lock(&mutex);
	quit = true;
	job_t *old = pending;
	pending = NULL;
	pthread_cond_broadcast(&wake);
	pthread_mutex_unlock(&mutex);
	free_job(old);
	pthread_join(worker, NULL);
	started = false;
	if (session_timer)
		lv_timer_pause(session_timer);
	pthread_mutex_lock(&mutex);
	result_t *r = fallback;
	fallback = NULL;
	pthread_mutex_unlock(&mutex);
	free_result(r);
	// Reset owns this journal even when firmware recovery is unavailable.
	// The writer is joined and deliveries are retired before files disappear.
	static const char *const suffixes[] = {"", ".new", ".lock"};
	for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
		char path[1500];
		if (snprintf(path, sizeof(path), "%s%s", journal, suffixes[i]) < (int)sizeof(path))
			unlink(path);
	}
}
void playback_session_init(bool (*legacy_restore)(void)) {
	if (started)
		return;
	legacy = legacy_restore;
	quit = false;
	const char *config = config_path();
	if (config && *config) {
		const char *slash = strrchr(config, '/');
		if (slash)
			snprintf(journal, sizeof(journal), "%.*s/playback-session.db", (int)(slash - config), config);
		else
			snprintf(journal, sizeof(journal), "playback-session.db");
	} else
		snprintf(journal, sizeof(journal), "playback-session.db");
	enabled = config_get_int("player", "remember_track", 0) != 0;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	bool ok = pthread_create(&worker, &attr, run, NULL) == 0;
	pthread_attr_destroy(&attr);
	if (!ok)
		return;
	started = true;
	atexit(shutdown_session);
	if (!session_timer)
		session_timer = lv_timer_create(tick, 1000, NULL);
	lv_timer_pause(session_timer);
	if (enabled)
		request_load();
}

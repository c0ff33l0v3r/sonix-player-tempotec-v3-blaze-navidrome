#define _GNU_SOURCE
#include "navidromedownloads.h"
#include "navidromeaudio.h"
#include "navidromedownloadfile.h"
#include "navidromeplayliststore.h"
#include "navidromeplaycatalog.h"
#include "src/gui/shell/gui.h"
#include "src/system/core/md5.h"
#include "src/system/core/sha256.h"
#include "src/system/core/utils.h"
#include "src/system/library/library.h"
#include "src/system/library/metadata.h"
#include "streamturn.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_t manager_thread, transfer_thread;
static bool started, stopping, pending, armed, network_wanted;
static unsigned serial;
static unsigned admissions;
static nd_dl_request_t command;
static nd_context_t command_context, active_context;
static nd_library_t active_library;
static bool saved_pending;
static nd_library_t saved_library;
// A read-only card cannot persist retirement. Keep a bounded same-lifetime
// deny list so an observed stale association cannot become a hit again.
static struct {
	unsigned profile, card;
	char scope[33], publication[ND_DL_PATH];
	int64_t key;
} retired[32];
static unsigned retired_next;
static nd_dl_view_t view;
static struct {
	bool pending, ok;
	int added, skipped;
	char error[64];
	nd_dl_captured_cb callback;
	void *user;
	unsigned profile, card;
} delivery;
static nd_dl_captured_cb command_callback;
static void *command_user;
typedef struct {
	nd_context_t context;
	nd_library_t library;
	unsigned serial;
	bool network;
	struct {
		char id[ND_ID_MAX], artist[ND_TEXT_MAX];
		int multi;
		bool complete;
	} albums[ND_WINDOW];
	unsigned album_next;
} run_t;
static void reconcile(run_t *r);
static bool capture_pin(const nd_dl_request_t *q) {
	return q->action == NDDL_CAPTURE && !q->captured && !q->direct && !q->remote_album && (!q->playlist || q->library.ready);
}

static bool current(void *data) {
	run_t *r = data;
	pthread_mutex_lock(&mutex);
	bool ok = !stopping && armed && serial == r->serial;
	pthread_mutex_unlock(&mutex);
	return ok && navidrome_playback_current(&r->context, r->network);
}
static bool commit_sql(void *data) { return sqlite3_exec(data, "COMMIT", NULL, NULL, NULL) == SQLITE_OK; }
static bool context_current(void *data) {
	run_t *r = data;
	pthread_mutex_lock(&mutex);
	bool alive = !stopping;
	pthread_mutex_unlock(&mutex);
	return alive && navidrome_playback_current(&r->context, false);
}
static bool open_store(const run_t *r, nd_dl_store_t *s, bool write) {
	memset(s, 0, sizeof(*s));
	if (write && !r->library.writable)
		return false;
	if (!navidrome_playback_begin(&r->context))
		return false;
	if (nd_dl_open(s, &r->library, write))
		return true;
	nd_dl_close(s);
	navidrome_cache_end();
	return false;
}
static void close_store(nd_dl_store_t *s) {
	nd_dl_close(s);
	navidrome_cache_end();
}
static void accept_repaired_mapping(const run_t *r, const nd_dl_job_t *j) {
	pthread_mutex_lock(&mutex);
	for (size_t i = 0; i < sizeof(retired) / sizeof(*retired); i++)
		if (retired[i].profile == r->context.profile_epoch && retired[i].card == r->context.card_epoch && retired[i].key == j->key && !strcmp(retired[i].scope, r->library.scope))
			memset(&retired[i], 0, sizeof(retired[i]));
	pthread_mutex_unlock(&mutex);
}
static bool save_job(run_t *r, nd_dl_job_t *j) {
	nd_dl_store_t s;
	if (!open_store(r, &s, true))
		return false;
	nd_dl_job_t stored;
	bool ok = nd_dl_sql(&s, "BEGIN IMMEDIATE") && nd_dl_get(&s, j->key, &stored);
	// A late transfer result must not undo an explicit manager Pause/Cancel.
	bool suppressed = ok && j->state != NDDL_COMPLETE && (stored.state == NDDL_CANCELLED || stored.state == NDDL_PAUSED);
	// An old resolver may finish after this same job was repaired. Its
	// publication nonce cannot retire the replacement or an in-progress retry.
	if (ok && j->state == NDDL_FAILED && !strcmp(j->error, "nd_dl_changed"))
		suppressed |= stored.state != NDDL_COMPLETE || strcmp(stored.partial, j->partial) || strcmp(stored.digest, j->digest);
	if (ok && !suppressed)
		ok = nd_dl_update(&s, j);
	if (ok)
		ok = navidrome_playback_action(&r->context, false, commit_sql, s.db);
	close_store(&s);
	if (ok && j->state == NDDL_COMPLETE) {
		accept_repaired_mapping(r, j);
		nd_dl_availability_changed();
	}
	return ok;
}
static void deliver(void *unused) {
	(void)unused;
	pthread_mutex_lock(&mutex);
	if (!delivery.pending) {
		pthread_mutex_unlock(&mutex);
		return;
	}
	nd_dl_captured_cb callback = delivery.callback;
	void *user = delivery.user;
	bool ok = delivery.ok;
	int added = delivery.added, skipped = delivery.skipped;
	char error[64];
	snprintf(error, sizeof(error), "%s", delivery.error);
	delivery.pending = false;
	unsigned profile = delivery.profile, card = delivery.card;
	pthread_mutex_unlock(&mutex);
	nd_context_t context;
	bool eligible = navidrome_context(&context, false) && context.profile_epoch == profile && context.card_epoch == card;
	nd_wipe(&context, sizeof(context));
	if (!eligible) {
		ok = false;
		snprintf(error, sizeof(error), "nd_dl_unavailable");
	}
	if (callback)
		callback(ok, error[0] ? error : NULL, added, skipped, user);
}
void navidromedownloads_poll(void) { deliver(NULL); }
void navidromedownloads_view(nd_dl_view_t *out) {
	pthread_mutex_lock(&mutex);
	*out = view;
	out->armed = armed;
	pthread_mutex_unlock(&mutex);
}
void navidromedownloads_cancel(void) {
	pthread_mutex_lock(&mutex);
	serial++;
	armed = network_wanted = false;
	nd_wipe(&active_context, sizeof(active_context));
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&mutex);
}
static void preempt(void) {
	pthread_mutex_lock(&mutex);
	serial++;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&mutex);
}
bool navidromedownloads_network_wanted(void) {
	pthread_mutex_lock(&mutex);
	bool wanted = network_wanted;
	pthread_mutex_unlock(&mutex);
	return wanted;
}
bool navidromedownloads_take_saved(nd_library_t *library) {
	pthread_mutex_lock(&mutex);
	bool ready = saved_pending && !armed;
	if (ready) {
		*library = saved_library;
		saved_pending = false;
	}
	pthread_mutex_unlock(&mutex);
	return ready;
}
static bool mapping_valid(const char *root, nd_dl_job_t *j, char path[ND_DL_PATH], run_t *r) {
	return nd_dl_mapping_valid(root, j, path, nd_audio_valid, context_current, r);
}

bool nd_download_resolve(const nd_library_t *lib, const nd_context_t *ctx, const char *id, char path[ND_DL_PATH]) {
	if (!navidrome_playback_scope(ctx, lib))
		return false;
	run_t r = {.context = *ctx, .library = *lib};
	nd_dl_store_t s;
	nd_dl_job_t j;
	if (!open_store(&r, &s, false))
		return false;
	bool found = nd_dl_find(&s, id, &j);
	close_store(&s);
	if (!found || !j.path[0] || !j.digest[0])
		return false;
	pthread_mutex_lock(&mutex);
	bool denied = false;
	for (size_t i = 0; i < 32; i++)
		denied |= retired[i].profile == ctx->profile_epoch && retired[i].card == ctx->card_epoch && retired[i].key == j.key && !strcmp(retired[i].scope, lib->scope) && !strcmp(retired[i].publication, j.partial);
	pthread_mutex_unlock(&mutex);
	if (denied)
		return false;
	if (!navidrome_playback_begin(ctx))
		return false;
	char root[ND_DL_PATH], db[1200];
	bool ok = nd_dl_paths(lib, root, db) && mapping_valid(root, &j, path, &r);
	navidrome_cache_end();
	if (!ok) {
		nd_dl_availability_changed();
		pthread_mutex_lock(&mutex);
		unsigned slot = retired_next++ % 32;
		retired[slot].profile = ctx->profile_epoch;
		retired[slot].card = ctx->card_epoch;
		retired[slot].key = j.key;
		snprintf(retired[slot].scope, sizeof(retired[slot].scope), "%s", lib->scope);
		snprintf(retired[slot].publication, sizeof(retired[slot].publication), "%s", j.partial);
		pthread_mutex_unlock(&mutex);
		j.state = NDDL_FAILED;
		snprintf(j.error, sizeof(j.error), "nd_dl_changed");
		if (lib->writable)
			save_job(&r, &j);
	}
	return ok;
}

// Revalidate every historical completion that could suppress a deliberate
// submission. A stale older row must not survive retiring only the newest one.
static bool validate_completions(nd_dl_store_t *store, const char *id, run_t *r) {
	nd_dl_job_t completed;
	char path[ND_DL_PATH];
	while (nd_dl_find(store, id, &completed)) {
		if (mapping_valid(store->root, &completed, path, r))
			return true;
		if (!context_current(r))
			return false;
		completed.state = NDDL_FAILED;
		snprintf(completed.error, sizeof(completed.error), "nd_dl_changed");
		if (!nd_dl_update(store, &completed))
			return false;
	}
	return context_current(r);
}
static bool append_track(nd_dl_store_t *dest, nd_store_t *source, const nd_library_t *lib, const char *id, int64_t batch, const char *folder, int *added, int *skipped, run_t *r) {
	nd_playrow_t row, album;
	if (!nd_store_metadata(source, lib->generation, ND_TRACKS, id, &row))
		return false;
	int multi = -1;
	bool complete = false, cached = false;
	char album_artist[ND_TEXT_MAX] = "";
	for (int i = 0; i < ND_WINDOW; i++)
		if (row.album_id[0] && !strcmp(r->albums[i].id, row.album_id)) {
			cached = true;
			complete = r->albums[i].complete;
			multi = r->albums[i].multi;
			snprintf(album_artist, sizeof(album_artist), "%s", r->albums[i].artist);
			break;
		}
	if (!cached)
		complete = nd_store_metadata(source, lib->generation, ND_ALBUMS, row.album_id, &album) && !album.row.details;
	if (complete && !cached) {
		snprintf(album_artist, sizeof(album_artist), "%s", album.row.artist);
		// metadata() established the same-generation connection-local browse view.
		sqlite3_stmt *st = NULL;
		if (sqlite3_prepare_v2(source->db, "SELECT coalesce(max(disc),1)>1 FROM browse_entity WHERE gen=? AND kind=1 AND album=?", -1, &st, NULL) != SQLITE_OK)
			return false;
		sqlite3_bind_int64(st, 1, lib->generation);
		nd_dl_bind(st, 2, row.album_id);
		bool ok = sqlite3_step(st) == SQLITE_ROW;
		if (ok)
			multi = sqlite3_column_int(st, 0);
		sqlite3_finalize(st);
		if (!ok)
			return false;
	}
	if (!cached) {
		unsigned slot = r->album_next++ % ND_WINDOW;
		snprintf(r->albums[slot].id, sizeof(r->albums[slot].id), "%s", row.album_id);
		snprintf(r->albums[slot].artist, sizeof(r->albums[slot].artist), "%s", album_artist);
		r->albums[slot].complete = complete;
		r->albums[slot].multi = multi;
	}
	int before = *added;
	if (!validate_completions(dest, id, r))
		return false;
	if (!nd_dl_insert(dest, batch, folder, &row, multi, added, skipped))
		return false;
	if (*added > before && complete) {
		nd_dl_job_t job;
		if (!nd_dl_get(dest, sqlite3_last_insert_rowid(dest->db), &job))
			return false;
		snprintf(job.album_artist, sizeof(job.album_artist), "%s", album_artist);
		if (!nd_dl_update(dest, &job))
			return false;
	}
	return true;
}
static bool append_entity(nd_dl_store_t *dest, nd_store_t *source, const nd_library_t *lib, nd_kind_t kind, const char *id, int64_t batch, const char *folder, int *added, int *skipped, run_t *r) {
	if (kind == ND_TRACKS)
		return append_track(dest, source, lib, id, batch, folder, added, skipped, r);
	nd_playrow_t album;
	if (kind != ND_ALBUMS || !nd_store_metadata(source, lib->generation, ND_ALBUMS, id, &album) || album.row.details)
		return false;
	nd_query_t q = {.kind = ND_TRACKS, .filter_kind = ND_ALBUMS, .favorites = lib->favorites};
	snprintf(q.filter, sizeof(q.filter), "%s", id);
	int offset = 0, count = 0, total = 0;
	do {
		nd_row_t rows[ND_WINDOW];
		if (!nd_store_query(source, lib->generation, &q, offset, rows, &count, &total))
			return false;
		if (total <= 0)
			return false;
		for (int i = 0; i < count; i++)
			if (!context_current(r) || !append_track(dest, source, lib, rows[i].id, batch, folder, added, skipped, r))
				return false;
		offset += count;
	} while (count && offset < total);
	return true;
}
static bool append_playlist(nd_dl_store_t *dest, nd_store_t *source, bool opened,
		const nd_dl_request_t *q, int64_t batch, int *added, int *skipped, run_t *r) {
	char path[1200];
	if (!nd_pl_path(&q->library, path)) return false;
	int offset = 0, expected = -1;
	for (;;) {
		nd_playlist_store_t saved = {0};
		nd_playlist_entry_t entries[ND_WINDOW];
		int count = 0, total = 0;
		bool ok = context_current(r) && nd_pl_open(&saved, path, q->library.scope, false) &&
			nd_pl_play_window(&saved, q->playlist, q->playlist_revision, offset, entries, &count, &total);
		nd_pl_close(&saved);
		if (!ok || !total || (expected >= 0 && total != expected)) return false;
		expected = total;
		if (!count) return true; // Final empty window revalidates the complete revision.
		for (int i = 0; i < count; i++) {
			nd_playrow_t row;
			if (!context_current(r)) return false;
			if (opened && nd_store_metadata(source, q->library.generation, ND_TRACKS, entries[i].track.id, &row)) {
				if (!append_track(dest, source, &q->library, entries[i].track.id, batch, q->destination, added, skipped, r)) return false;
			} else {
				row = (nd_playrow_t){.row = entries[i].track};
				if (!validate_completions(dest, row.row.id, r) ||
					!nd_dl_insert(dest, batch, q->destination, &row, -1, added, skipped)) return false;
			}
		}
		offset += count;
	}
}
static bool capture_rows(run_t *r, const nd_dl_request_t *q, nd_playcatalog_t *remote, int *added, int *skipped) {
	if (!nd_dl_destination(q->destination))
		return false;
	nd_dl_store_t dest;
	if (!open_store(r, &dest, true))
		return false;
	int dir = nd_dl_directory(dest.root, q->destination, true);
	if (dir < 0) {
		close_store(&dest);
		return false;
	}
	close(dir);
	nd_store_t source = {0};
	bool opened = nd_store_open_source(&source, q->library.path, q->library.scope, false, false, q->library.favorites);
	bool ok = (opened || q->direct || q->playlist || remote || q->captured) && nd_dl_sql(&dest, "BEGIN IMMEDIATE");
	sqlite3_stmt *st = NULL;
	if (ok)
		ok = nd_dl_prepare(&dest, &st, "INSERT INTO batch(destination,created) VALUES(?,strftime('%s','now'))");
	if (ok) {
		nd_dl_bind(st, 1, q->destination);
		ok = sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	st = NULL;
	int64_t batch = sqlite3_last_insert_rowid(dest.db);
	if (ok && q->captured) {
		ok = nd_owned_current(q->captured) && q->captured->identity.profile_epoch == r->context.profile_epoch && q->captured->identity.card_epoch == r->context.card_epoch && !strcmp(q->captured->library.scope, q->library.scope);
		for (int first = 0; ok && first < q->captured->count;) {
			nd_playrow_t rows[ND_WINDOW];
			int count = nd_owned_read(q->captured, first, rows, ND_WINDOW);
			ok = count > 0;
			for (int i = 0; ok && i < count; i++) {
				if (q->captured->local && opened)
					ok = context_current(r) && append_track(&dest, &source, &q->library, rows[i].row.id, batch, q->destination, added, skipped, r);
				else {
					int multi = q->captured->album_multi ? q->captured->album_multi(q->captured, rows[i].album_id) : -1;
					int before = *added;
					ok = context_current(r) && validate_completions(&dest, rows[i].row.id, r) && nd_dl_insert(&dest, batch, q->destination, &rows[i], multi, added, skipped);
					if (ok && *added > before && multi >= 0) {
						nd_dl_job_t job;
						ok = nd_dl_get(&dest, sqlite3_last_insert_rowid(dest.db), &job);
						if (ok) {
							snprintf(job.album_artist, sizeof(job.album_artist), "%s", rows[i].album_artist);
							ok = nd_dl_update(&dest, &job);
						}
					}
				}
			}
			if (count > 0)
				first += count;
		}
	} else if (ok && remote) {
		int total = nd_playcatalog_count(remote);
		ok = total > 0;
		for (int i = 0; ok && i < total; i++) {
			nd_playrow_t row;
			ok = context_current(r) && nd_playcatalog_read(remote, i, &row) &&
				validate_completions(&dest, row.row.id, r) &&
				nd_dl_insert(&dest, batch, q->destination, &row, -1, added, skipped);
		}
	} else if (ok && q->playlist)
		ok = append_playlist(&dest, &source, opened, q, batch, added, skipped, r);
	else if (ok && q->direct) {
		nd_playrow_t known;
		if (!q->track.row.id[0] || !q->track.album_id[0] || strcmp(q->id, q->track.row.id))
			ok = false;
		else if (opened && nd_store_metadata(&source, q->library.generation, ND_TRACKS, q->id, &known))
			ok = append_track(&dest, &source, &q->library, q->id, batch, q->destination, added, skipped, r);
		else {
			ok = validate_completions(&dest, q->id, r);
			if (ok)
				ok = nd_dl_insert(&dest, batch, q->destination, &q->track, -1, added, skipped);
		}
	} else if (ok && q->session) {
		nd_playlist_store_t selections = {0};
		char path[1200];
		ok = nd_pl_path(&q->library, path) && nd_pl_open(&selections, path, q->library.scope, false) && nd_pl_prepare(&selections, &st, "SELECT kind,track FROM selection WHERE session=? ORDER BY ordinal");
		if (ok)
			sqlite3_bind_int64(st, 1, q->session);
		int rc = SQLITE_DONE;
		while (ok && (rc = sqlite3_step(st)) == SQLITE_ROW) {
			const char *id = (const char *)sqlite3_column_text(st, 1);
			ok = id && navidrome_playback_current(&r->context, false) && append_entity(&dest, &source, &q->library, sqlite3_column_int(st, 0), id, batch, q->destination, added, skipped, r);
		}
		if (ok)
			ok = rc == SQLITE_DONE;
		sqlite3_finalize(st);
		st = NULL;
		nd_pl_close(&selections);
	} else if (ok)
		ok = append_entity(&dest, &source, &q->library, q->kind, q->id, batch, q->destination, added, skipped, r);
	if (ok)
		ok = nd_dl_prepare(&dest, &st, "UPDATE batch SET skipped=?,admitted=? WHERE id=?");
	if (ok) {
		sqlite3_bind_int(st, 1, *skipped);
		sqlite3_bind_int(st, 2, *added);
		sqlite3_bind_int64(st, 3, batch);
		ok = sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	if (ok && !*added && !*skipped)
		ok = false;
	if (ok)
		ok = navidrome_playback_action(&r->context, false, commit_sql, dest.db);
	nd_store_close(&source);
	close_store(&dest);
	return ok;
}

static bool capture(run_t *r, const nd_dl_request_t *q, int *added, int *skipped) {
	nd_playcatalog_t *remote = NULL;
	if (q->remote_album) {
		// Fetch/validate complete membership before acquiring a store/SQL lease.
		if (!q->library.writable || !nd_dl_destination(q->destination))
			return false;
		nd_query_t album = {.kind = ND_TRACKS, .filter_kind = ND_ALBUMS};
		snprintf(album.filter, sizeof(album.filter), "%s", q->id);
		const char *error = NULL;
		remote = nd_playcatalog_capture(&q->library, &r->context, &album, NULL, context_current, r, &error);
		if (!remote)
			return false;
	}
	bool ok = capture_rows(r, q, remote, added, skipped);
	nd_playcatalog_free(remote);
	return ok;
}

static void refresh_view(run_t *r, const nd_dl_request_t *q, const char *error) {
	nd_dl_view_t *out = calloc(1, sizeof(*out));
	if (!out)
		return;
	out->token = q->token;
	out->offset = q->offset;
	out->filter = q->filter;
	out->profile_epoch = r->context.profile_epoch;
	out->card_epoch = r->context.card_epoch;
	snprintf(out->scope, sizeof(out->scope), "%s", r->library.scope);
	if (error)
		snprintf(out->error, sizeof(out->error), "%s", error);
	nd_dl_store_t s;
	if (open_store(r, &s, false)) {
		if (!nd_dl_window_filter(&s, q->offset, q->filter, out->jobs, &out->count, &out->total, out->counts))
			snprintf(out->error, sizeof(out->error), "nd_dl_store_error");
		sqlite3_stmt *st = NULL;
		if (nd_dl_prepare(&s, &st, s.history ? "SELECT count(*),coalesce(sum(skipped),0) FROM batch WHERE NOT EXISTS(SELECT 1 FROM history_hidden WHERE kind=1 AND key=batch.id)" : "SELECT count(*),coalesce(sum(skipped),0) FROM batch") && sqlite3_step(st) == SQLITE_ROW) {
			out->batches = sqlite3_column_int(st, 0);
			int64_t skips = sqlite3_column_int64(st, 1);
			out->skipped = skips > INT32_MAX ? INT32_MAX : (int)skips;
		}
		sqlite3_finalize(st);
		close_store(&s);
	} else {
		char root[512], path[1200];
		if (navidrome_playback_begin(&r->context)) {
			if (nd_dl_paths(&r->library, root, path) && access(path, F_OK) == 0)
				snprintf(out->error, sizeof(out->error), "nd_dl_store_error");
			navidrome_cache_end();
		}
	}
	if (q->action == NDDL_FOLDERS || q->action == NDDL_MKDIR || q->action == NDDL_PREPARE_FOLDER) {
		if (!r->library.writable)
			snprintf(out->error, sizeof(out->error), "nd_dl_readonly");
		snprintf(out->folder, sizeof(out->folder), "%s", q->folder);
		if (!nd_dl_destination(q->folder) || !navidrome_playback_begin(&r->context))
			snprintf(out->error, sizeof(out->error), "nd_dl_path_error");
		else {
			char root[512], path[1200];
			int fd = nd_dl_paths(&r->library, root, path) ? nd_dl_directory(root, q->folder, q->action == NDDL_PREPARE_FOLDER && r->library.writable) : -1;
			DIR *dir = fd >= 0 ? fdopendir(fd) : NULL;
			if (!dir) {
				if (fd >= 0)
					close(fd);
				if (!out->error[0])
					snprintf(out->error, sizeof(out->error), "nd_dl_path_error");
			} else {
				struct dirent *entry;
				while ((entry = readdir(dir))) {
					struct stat st;
					if (entry->d_name[0] == '.' || strlen(entry->d_name) >= ND_DL_DEST || fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISDIR(st.st_mode))
						continue;
					if (!q->folder[0] && (!strcasecmp(entry->d_name, "Podcast") || !strcasecmp(entry->d_name, "Audiobooks")))
						continue;
					if (out->folder_total++ >= q->offset && out->folder_count < ND_WINDOW)
						memcpy(out->folders[out->folder_count++], entry->d_name, strlen(entry->d_name) + 1);
				}
				closedir(dir);
			}
			navidrome_cache_end();
		}
	}
	pthread_mutex_lock(&mutex);
	out->busy = true; // Admission stays closed until the manager finishes delivery.
	out->active_key = view.active_key;
	out->bytes = view.bytes;
	out->length = view.length;
	out->revision = view.revision + 1;
	view = *out;
	pthread_mutex_unlock(&mutex);
	free(out);
}
static void *manage(void *unused) {
	(void)unused;
	thread_be_background("navidrome download jobs");
	char loaded_scope[33] = "";
	unsigned loaded_card = 0, loaded_profile = 0;
	for (;;) {
		pthread_mutex_lock(&mutex);
		while (!pending && !stopping)
			pthread_cond_wait(&changed, &mutex);
		if (stopping) {
			pthread_mutex_unlock(&mutex);
			break;
		}
		nd_dl_request_t q = command;
		command.captured = NULL;
		nd_context_t owned_context = command_context;
		nd_wipe(&command_context, sizeof(command_context));
		nd_dl_captured_cb callback = command_callback;
		void *user = command_user;
		pending = false;
		pthread_mutex_unlock(&mutex);
		run_t r = {.library = q.library, .context = owned_context};
		nd_wipe(&owned_context, sizeof(owned_context));
		const char *error = NULL;
		int added = 0, skipped = 0;
		bool available = context_current(&r) && r.context.card_epoch == q.library.card_epoch;
		if (!available)
			error = "nd_dl_unavailable";
		if (available && (strcmp(loaded_scope, q.library.scope) || loaded_card != r.context.card_epoch || loaded_profile != r.context.profile_epoch)) {
			reconcile(&r);
			nd_dl_store_t s;
			if (open_store(&r, &s, true)) {
				bool recovered = nd_dl_sql(&s, "UPDATE job SET state=5 WHERE state IN(0,1,2,8)");
				close_store(&s);
				if (!recovered)
					error = "nd_dl_store_error";
			}
			snprintf(loaded_scope, sizeof(loaded_scope), "%s", q.library.scope);
			loaded_card = r.context.card_epoch;
			loaded_profile = r.context.profile_epoch;
		}
		if (!error && q.action == NDDL_CAPTURE && !capture(&r, &q, &added, &skipped))
			error = "nd_dl_capture_error";
		if (!error && q.action >= NDDL_PAUSE && q.action <= NDDL_RETRY) {
			if (q.action == NDDL_PAUSE || q.action == NDDL_CANCEL)
				reconcile(&r);
			nd_dl_store_t s;
			if (!open_store(&r, &s, true))
				error = "nd_dl_store_error";
			else {
				const char *sql = q.action == NDDL_PAUSE ? "UPDATE job SET state=5 WHERE state IN(0,1,2,8)" : q.action == NDDL_CANCEL ? "UPDATE job SET state=7 WHERE state NOT IN(3,4)" : q.action == NDDL_RETRY ? "UPDATE job SET state=0,error='' WHERE state=6 AND key=(SELECT max(newer.key) FROM job newer WHERE newer.track=job.track) AND NOT EXISTS(SELECT 1 FROM history_hidden WHERE kind=0 AND key=job.key)" : "UPDATE job SET state=0,error='' WHERE state=5";
				if (!nd_dl_sql(&s, "BEGIN IMMEDIATE") || !nd_dl_sql(&s, sql) || !navidrome_playback_action(&r.context, false, commit_sql, s.db))
					error = "nd_dl_store_error";
				close_store(&s);
			}
		}
		if (!error && q.action == NDDL_CLEAR_HISTORY) {
			nd_dl_store_t s;
			if (!r.library.writable)
				error = "nd_dl_readonly";
			else if (!open_store(&r, &s, true))
				error = "nd_dl_store_error";
			else {
				if (!nd_dl_sql(&s, "BEGIN IMMEDIATE") || !nd_dl_clear_history(&s) || !navidrome_playback_action(&r.context, false, commit_sql, s.db))
					error = "nd_dl_store_error";
				close_store(&s);
			}
		}
		if (!error && q.action == NDDL_MKDIR) {
			if (!r.library.writable)
				error = "nd_dl_readonly";
		}
		if (!error && q.action == NDDL_MKDIR) {
			char safe[96], relative[512], root[512], db[1200];
			nd_dl_safe_name(q.name, "", safe);
			bool ok = safe[0] && !strcmp(safe, q.name) && snprintf(relative, sizeof(relative), "%s%s%s", q.folder, q.folder[0] ? "/" : "", safe) < (int)sizeof(relative) && nd_dl_destination(relative);
			if (ok && navidrome_playback_begin(&r.context)) {
				int dir = nd_dl_paths(&r.library, root, db) ? nd_dl_directory(root, relative, true) : -1;
				ok = dir >= 0;
				if (dir >= 0)
					close(dir);
				navidrome_cache_end();
			} else
				ok = false;
			if (!ok)
				error = "nd_dl_path_error";
			else {
				snprintf(q.folder, sizeof(q.folder), "%s", relative);
				q.offset = 0;
			}
		}
		if (available)
			refresh_view(&r, &q, error);
		else {
			pthread_mutex_lock(&mutex);
			memset(&view, 0, sizeof(view));
			view.token = q.token;
			snprintf(view.error, sizeof(view.error), "%s", error);
			pthread_mutex_unlock(&mutex);
		}
		if (capture_pin(&q))
			nd_store_release_source(&q.library);
		nd_owned_release(q.captured);
		q.captured = NULL;
		pthread_mutex_lock(&mutex);
		view.busy = false;
		if (!error && (q.action == NDDL_CAPTURE || q.action == NDDL_RESUME || q.action == NDDL_RETRY)) {
			active_library = q.library;
			active_context = r.context;
			admissions++;
			armed = true;
			pthread_cond_broadcast(&changed);
		}
		if (callback) {
			delivery.pending = true;
			delivery.callback = callback;
			delivery.user = user;
			delivery.profile = r.context.profile_epoch;
			delivery.card = r.context.card_epoch;
			delivery.ok = !error;
			delivery.added = error ? 0 : added;
			delivery.skipped = error ? 0 : skipped;
			snprintf(delivery.error, sizeof(delivery.error), "%s", error ? error : "");
		}
		pthread_mutex_unlock(&mutex);
		if (callback)
			gui_post(deliver, NULL);
		nd_wipe(&r.context, sizeof(r.context));
	}
	return NULL;
}

static const char *enrich(run_t *r, nd_dl_job_t *j, const char *cached) {
	// A saved playlist may contain exact IDs absent from any synced library.
	// Resolve structure here, outside every card/SQL lease and only on transfer.
	if (!j->source.album_id[0]) {
		if (!navidrome_playback_current(&r->context, true)) return "nd_dl_album_context";
		char encoded[ND_ID_MAX * 3 + 1], query[ND_ID_MAX * 3 + 8];
		http_url_encode(j->source.row.id, encoded, sizeof(encoded));
		snprintf(query, sizeof(query), "id=%s", encoded);
		bool old = r->network;
		r->network = true;
		pthread_mutex_lock(&mutex);
		network_wanted = true;
		pthread_mutex_unlock(&mutex);
		nd_document_t doc;
		const char *error = nd_api_finite_json(r->context.host, r->context.user, r->context.password, "getSong", query, ND_API_BYTES, current, r, &doc);
		r->network = old;
		if (error) return error;
		nd_playrow_t row;
		int song = json_get(&doc.doc, doc.response, "song");
		bool valid = nd_api_field_unique(&doc.doc, doc.response, "song", true) &&
			nd_metadata_row(&doc.doc, song, ND_TRACKS, &row) && !strcmp(row.row.id, j->source.row.id) && row.album_id[0];
		if (valid) j->source = row;
		nd_api_free(&doc);
		if (!valid) return "nd_response_error";
	}
	nd_dl_store_t store;
	if (open_store(r, &store, false)) {
		sqlite3_stmt *st = NULL;
		if (nd_dl_prepare(&store, &st, "SELECT artist,multi FROM placement WHERE album=? AND destination=?")) {
			nd_dl_bind(st, 1, j->source.album_id);
			nd_dl_bind(st, 2, j->destination);
			if (sqlite3_step(st) == SQLITE_ROW) {
				const char *credit = (const char *)sqlite3_column_text(st, 0);
				snprintf(j->album_artist, sizeof(j->album_artist), "%s", credit ? credit : "");
				j->multi = sqlite3_column_int(st, 1);
			}
		}
		sqlite3_finalize(st);
		close_store(&store);
	}
	if (cached && navidrome_playback_begin(&r->context)) {
		song_metadata_t tags;
		metadata_read(cached, &tags);
		if (!j->album_artist[0])
			snprintf(j->album_artist, sizeof(j->album_artist), "%s", tags.album_artist);
		navidrome_cache_end();
	}
	if ((!j->album_artist[0] || j->multi < 0) && navidrome_playback_current(&r->context, true)) {
		char encoded[ND_ID_MAX * 3 + 1], query[ND_ID_MAX * 3 + 8];
		http_url_encode(j->source.album_id, encoded, sizeof(encoded));
		snprintf(query, sizeof(query), "id=%s", encoded);
		nd_document_t doc;
		bool old = r->network;
		r->network = true;
		pthread_mutex_lock(&mutex);
		network_wanted = true;
		pthread_mutex_unlock(&mutex);
		const char *error = nd_api_finite_json(r->context.host, r->context.user, r->context.password, "getAlbum", query, ND_API_BYTES, current, r, &doc);
		r->network = old;
		if (error)
			return error;
		int album = json_get(&doc.doc, doc.response, "album"), songs = json_get(&doc.doc, album, "song");
		char id[ND_ID_MAX];
		bool valid = nd_pl_field(&doc.doc, album, "id", id, sizeof(id), true, false) && !strcmp(id, j->source.album_id);
		if (valid && !j->album_artist[0]) {
			json_obj_str(&doc.doc, album, "displayAlbumArtist", j->album_artist, sizeof(j->album_artist));
			if (!j->album_artist[0])
				json_obj_str(&doc.doc, album, "artist", j->album_artist, sizeof(j->album_artist));
		}
		if (valid && songs >= 0 && doc.doc.toks[songs].type == JSON_ARRAY) {
			j->multi = 0;
			bool contains = false;
			for (int i = 0; valid && i < json_len(&doc.doc, songs); i++) {
				int child = json_at(&doc.doc, songs, i), disc = json_get(&doc.doc, child, "discNumber");
				char song_id[ND_ID_MAX];
				long number = disc < 0 ? 1 : json_long(&doc.doc, disc, -1);
				valid = nd_pl_field(&doc.doc, child, "id", song_id, sizeof(song_id), true, false) && number >= 0 && number <= INT32_MAX && (disc < 0 || doc.doc.toks[disc].type == JSON_NUMBER);
				contains |= valid && !strcmp(song_id, j->source.row.id);
				if (number > 1)
					j->multi = 1;
			}
			valid = valid && contains;
			int count = json_get(&doc.doc, album, "songCount");
			if (count >= 0)
				valid = valid && json_long(&doc.doc, count, -1) == json_len(&doc.doc, songs);
		}
		nd_api_free(&doc);
		if (!valid)
			return "nd_response_error";
	}
	if (j->multi < 0)
		return "nd_dl_album_context";
	if (!j->album_artist[0])
		snprintf(j->album_artist, sizeof(j->album_artist), "Unknown Album Artist");
	return NULL;
}

static int case_name(int directory, const char *name, char actual[256]) {
	int fd = dup(directory);
	DIR *dir = fd >= 0 ? fdopendir(fd) : NULL;
	if (!dir) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	rewinddir(dir);
	struct dirent *entry;
	int found = 0;
	while ((entry = readdir(dir))) {
		if (!strcasecmp(entry->d_name, name)) {
			memcpy(actual, entry->d_name, strlen(entry->d_name) + 1);
			found = 1;
			break;
		}
	}
	closedir(dir);
	return found;
}
static bool place(run_t *r, nd_dl_job_t *j, const char *extension) {
	nd_dl_store_t s;
	if (!open_store(r, &s, true))
		return false;
	char album_dir[ND_DL_PATH] = "";
	sqlite3_stmt *st = NULL;
	bool ok = nd_dl_sql(&s, "BEGIN IMMEDIATE") && nd_dl_prepare(&s, &st, "SELECT path,artist,multi FROM placement WHERE album=? AND destination=?");
	if (ok) {
		nd_dl_bind(st, 1, j->source.album_id);
		nd_dl_bind(st, 2, j->destination);
		if (sqlite3_step(st) == SQLITE_ROW) {
			const char *p = (const char *)sqlite3_column_text(st, 0), *a = (const char *)sqlite3_column_text(st, 1);
			snprintf(album_dir, sizeof(album_dir), "%s", p ? p : "");
			snprintf(j->album_artist, sizeof(j->album_artist), "%s", a ? a : "");
			j->multi = sqlite3_column_int(st, 2);
		}
	}
	sqlite3_finalize(st);
	st = NULL;
	if (ok && !album_dir[0]) {
		char artist[96], album[96], hash[33];
		nd_dl_safe_name(j->album_artist, "Unknown Album Artist", artist);
		// Root downloads still belong to Music, even when an artist happens
		// to have the name of one of Music's excluded top-level trees.
		if (!j->destination[0] && (!strcasecmp(artist, "Podcast") || !strcasecmp(artist, "Audiobooks"))) {
			char visible[96];
			snprintf(visible, sizeof(visible), "_%.80s", artist);
			snprintf(artist, sizeof(artist), "%s", visible);
		}
		nd_dl_safe_name(j->source.album, "Unknown Album", album);
		char identity[ND_ID_MAX + 40];
		snprintf(identity, sizeof(identity), "%s:%s", r->library.scope, j->source.album_id);
		md5_hex(identity, strlen(identity), hash);
		int destination = nd_dl_directory(s.root, j->destination, false);
		char actual[256], artist_path[512];
		int matched = destination >= 0 ? case_name(destination, artist, actual) : -1;
		if (matched > 0 && strlen(actual) < sizeof(artist))
			memcpy(artist, actual, strlen(actual) + 1);
		if (matched < 0 || (matched > 0 && strlen(actual) >= sizeof(artist)))
			ok = false;
		if (destination >= 0)
			close(destination);
		int n = snprintf(artist_path, sizeof(artist_path), "%s%s%s", j->destination, j->destination[0] ? "/" : "", artist);
		int parent = ok && n > 0 && n < (int)sizeof(artist_path) ? nd_dl_directory(s.root, artist_path, true) : -1;
		if (parent < 0)
			ok = false;
		for (int attempt = 0; ok && attempt < 3; attempt++) {
			char candidate[128];
			if (attempt)
				snprintf(candidate, sizeof(candidate), "%s [%.*s]", album, attempt == 1 ? 12 : 32, hash);
			else
				snprintf(candidate, sizeof(candidate), "%s", album);
			n = snprintf(album_dir, sizeof(album_dir), "%s/%s", artist_path, candidate);
			if (n < 0 || n >= (int)sizeof(album_dir)) {
				ok = false;
				break;
			}
			matched = case_name(parent, candidate, actual);
			if (matched == 0 && !mkdirat(parent, candidate, 0755))
				break;
			if (matched < 0 || (matched == 0 && errno != EEXIST))
				ok = false;
			album_dir[0] = 0;
		}
		if (parent >= 0)
			close(parent);
		if (!album_dir[0])
			ok = false;
		if (ok)
			ok = nd_dl_prepare(&s, &st, "INSERT INTO placement VALUES(?,?,?,?,?)");
		if (ok) {
			nd_dl_bind(st, 1, j->source.album_id);
			nd_dl_bind(st, 2, j->destination);
			nd_dl_bind(st, 3, j->album_artist);
			nd_dl_bind(st, 4, album_dir);
			sqlite3_bind_int(st, 5, j->multi);
			ok = sqlite3_step(st) == SQLITE_DONE;
		}
		sqlite3_finalize(st);
		st = NULL;
	}
	char dirpath[ND_DL_PATH], title[96], leaf[160], hash[33];
	nd_dl_safe_name(j->source.row.title, "Track", title);
	md5_hex(j->source.row.id, strlen(j->source.row.id), hash);
	if (ok) {
		int n = j->multi ? snprintf(dirpath, sizeof(dirpath), "%s/Disc %d", album_dir, j->source.row.disc > 0 ? j->source.row.disc : 1) : snprintf(dirpath, sizeof(dirpath), "%s", album_dir);
		ok = n > 0 && n < (int)sizeof(dirpath);
	}
	int directory = ok ? nd_dl_directory(s.root, dirpath, true) : -1;
	if (directory < 0)
		ok = false;
	if (ok) {
		for (int attempt = 0; attempt < 3; attempt++) {
			if (attempt)
				snprintf(leaf, sizeof(leaf), "%s [%.*s]%s", title, attempt == 1 ? 12 : 32, hash, extension);
			else
				snprintf(leaf, sizeof(leaf), "%s%s", title, extension);
			// Case-insensitive conflicts are relevant even on a case-sensitive host.
			int copy = dup(directory);
			DIR *entries = copy >= 0 ? fdopendir(copy) : NULL;
			bool collision = !entries;
			struct dirent *entry;
			if (entries) {
				rewinddir(entries);
				while ((entry = readdir(entries)))
					if (!strcasecmp(entry->d_name, leaf)) {
						collision = true;
						break;
					}
				closedir(entries);
			} else if (copy >= 0)
				close(copy);
			if (!collision)
				break;
			leaf[0] = 0;
		}
		if (!leaf[0])
			ok = false;
	}
	uint8_t random[8];
	char nonce[17];
	if (ok)
		ok = nd_random(random, sizeof(random));
	if (ok)
		for (int i = 0; i < 8; i++)
			snprintf(nonce + 2 * i, 3, "%02x", random[i]);
	if (ok)
		ok = snprintf(j->path, sizeof(j->path), "%s/%s", dirpath, leaf) < (int)sizeof(j->path) && snprintf(j->partial, sizeof(j->partial), "%s/.nd-%lld-%s%s", dirpath, (long long)j->key, nonce, extension) < (int)sizeof(j->partial) && strlen(s.root) + 1 + strlen(j->partial) < ND_DL_PATH && strlen(s.root) + 1 + strlen(j->path) < ND_DL_PATH;
	if (ok) {
		snprintf(j->extension, sizeof(j->extension), "%s", extension);
		j->state = NDDL_RUNNING;
		j->bytes = 0;
		j->digest[0] = 0;
		ok = nd_dl_update(&s, j) && navidrome_playback_action(&r->context, false, commit_sql, s.db);
	}
	if (directory >= 0)
		close(directory);
	close_store(&s);
	return ok;
}
static bool room(int directory, int incoming) {
	struct statvfs fs;
	return !fstatvfs(directory, &fs) && fs.f_frsize && (uint64_t)fs.f_bavail >= (uint64_t)(16 * 1024 * 1024 + incoming) / fs.f_frsize + 1;
}
static double seconds(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}
typedef struct {
	int directory;
	const char *part, *leaf;
	nd_download_publish_result_t result;
	unsigned serial;
} publication_t;
static bool publish(void *data) {
	publication_t *p = data;
	pthread_mutex_lock(&mutex);
	if (!stopping && armed && serial == p->serial)
		p->result = nd_download_file_publish(p->directory, p->part, p->leaf);
	pthread_mutex_unlock(&mutex);
	return p->result == ND_DOWNLOAD_PUBLISHED;
}

static const char *probe_publication(run_t *r) {
	// Probe the actual selected filesystem before fetching a potentially large
	// original. Both probe names live beneath the hidden feature directory.
	if (!navidrome_playback_begin(&r->context))
		return "nd_audio_cancelled";
	char root[512], db[1200], part[80], leaf[80], nonce[33];
	uint8_t random[16];
	int dir = nd_dl_paths(&r->library, root, db) ? nd_dl_directory(root, ".local/navidrome", false) : -1;
	const char *error = "nd_dl_storage";
	if (dir >= 0 && nd_random(random, sizeof(random))) {
		for (int i = 0; i < 16; i++)
			snprintf(nonce + 2 * i, 3, "%02x", random[i]);
		snprintf(part, sizeof(part), ".publish-%s", nonce);
		snprintf(leaf, sizeof(leaf), "publish-%s", nonce);
		int fd = openat(dir, part, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
		if (fd >= 0) {
			bool closed = close(fd) == 0;
			nd_download_publish_result_t result = closed ? nd_download_file_publish(dir, part, leaf) : ND_DOWNLOAD_PUBLISH_ERROR;
			if (result == ND_DOWNLOAD_PUBLISHED) {
				unlinkat(dir, leaf, 0);
				error = NULL;
			} else {
				unlinkat(dir, part, 0);
				error = result == ND_DOWNLOAD_PUBLISH_UNSUPPORTED ? "nd_dl_atomic_unsupported" : "nd_dl_storage";
			}
		}
	}
	if (dir >= 0)
		close(dir);
	navidrome_cache_end();
	return error;
}
static void remove_partial(run_t *r, const nd_dl_job_t *j) {
	if (!j->partial[0] || j->inode <= 0 || j->state == NDDL_INTENT || !navidrome_playback_begin(&r->context))
		return;
	char root[512], db[1200], relative[512], prefix[48];
	snprintf(relative, sizeof(relative), "%s", j->partial);
	snprintf(prefix, sizeof(prefix), ".nd-%lld-", (long long)j->key);
	char *leaf = strrchr(relative, '/');
	if (leaf) {
		*leaf++ = 0;
		int dir = nd_dl_paths(&r->library, root, db) ? nd_dl_directory(root, relative, false) : -1;
		struct stat st;
		if (dir >= 0 && !strncmp(leaf, prefix, strlen(prefix)) && !fstatat(dir, leaf, &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode) && (int64_t)st.st_ino == j->inode)
			unlinkat(dir, leaf, 0);
		if (dir >= 0)
			close(dir);
	}
	navidrome_cache_end();
}

static const char *transfer(run_t *r, nd_dl_job_t *j) {
	if (!nd_dl_destination(j->destination))
		return "nd_dl_path_error";
	remove_partial(r, j);
	j->state = NDDL_WAITING;
	if (!save_job(r, j))
		return "nd_dl_store_error";
	const char *probe = probe_publication(r);
	if (probe)
		return probe;
	if (!streamturn_acquire_background(current, r))
		return "nd_audio_cancelled";
	char cached[512] = "", root[512], db[1200];
	bool copying = nd_audio_copy_acquire(&r->library, &r->context, j->source.row.id, cached);
	const char *error = enrich(r, j, copying ? cached : NULL);
	http_session_t *stream = NULL;
	if (!error && !copying) {
		r->network = true;
		if (!current(r))
			error = "nd_reconnect_required";
		else {
			pthread_mutex_lock(&mutex);
			network_wanted = true;
			pthread_mutex_unlock(&mutex);
			stream = http_session_new(current, r);
			if (!stream || !http_session_binary(stream, ND_DL_MAX_BYTES, 1800))
				error = "nd_dl_unavailable";
			else
				error = nd_api_download(r->context.host, r->context.user, r->context.password, j->source.row.id, stream);
		}
		const char *type = stream ? http_session_content_type(stream) : "";
		if (!error && (strcasestr(type, "text/") || strcasestr(type, "json") || strcasestr(type, "xml")))
			error = "nd_audio_invalid";
		if (!error)
			j->total = http_session_length(stream);
	}
	unsigned char buffer[65536];
	sha256_ctx incoming;
	sha256_init(&incoming);
	bool placed = false;
	int64_t source_offset = 0;
	double deadline = seconds() + 1800, last_saved = seconds();
	char relative[512] = "", part[160] = "", leaf[160] = "";
	while (!error) {
		if (!current(r)) {
			error = "nd_audio_cancelled";
			break;
		}
		if (seconds() > deadline) {
			error = "nd_network_error";
			break;
		}
		int n = -1;
		if (copying) {
			if (navidrome_playback_begin(&r->context)) {
				int in = open(cached, O_RDONLY | O_NOFOLLOW);
				struct stat st;
				bool valid = in >= 0 && !fstat(in, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= ND_DL_MAX_BYTES && lseek(in, source_offset, SEEK_SET) == source_offset;
				if (valid) {
					j->total = st.st_size;
					n = read(in, buffer, sizeof(buffer));
				}
				if (in >= 0)
					close(in);
				navidrome_cache_end();
			}
		} else {
			n = http_session_read(stream, buffer, sizeof(buffer));
			if (!placed && n > 0)
				while (n < 64) {
					int more = http_session_read(stream, buffer + n, sizeof(buffer) - n);
					if (more < 0) {
						n = -1;
						break;
					}
					if (!more)
						break;
					n += more;
				}
		}
		if (!current(r)) {
			error = "nd_audio_cancelled";
			break;
		}
		if (n < 0) {
			error = copying ? "nd_dl_changed" : "nd_network_error";
			const char *reason = stream ? http_session_error(stream) : NULL;
			if (reason && (!strcmp(reason, "body_limit") || !strcmp(reason, "decoded_limit")))
				error = "nd_dl_size_limit";
			break;
		}
		if (!n)
			break;
		if (j->bytes > ND_DL_MAX_BYTES - n) {
			error = "nd_dl_size_limit";
			break;
		}
		if (!placed) {
			const char *extension = nd_audio_extension(buffer, (size_t)n);
			if (!extension) {
				error = "nd_audio_invalid";
				break;
			}
			if (!place(r, j, extension)) {
				error = "nd_dl_path_error";
				break;
			}
			snprintf(relative, sizeof(relative), "%s", j->path);
			char *slash = strrchr(relative, '/');
			if (!slash) {
				error = "nd_dl_path_error";
				break;
			}
			snprintf(leaf, sizeof(leaf), "%s", slash + 1);
			*slash = 0;
			const char *p = strrchr(j->partial, '/');
			p = p ? p + 1 : j->partial;
			if (strlen(p) >= sizeof(part)) {
				error = "nd_dl_path_error";
				break;
			}
			memcpy(part, p, strlen(p) + 1);
			placed = true;
		}
		if (!navidrome_playback_begin(&r->context)) {
			error = "nd_audio_cancelled";
			break;
		}
		int directory = nd_dl_paths(&r->library, root, db) ? nd_dl_directory(root, relative, false) : -1;
		int out = directory >= 0 ? openat(directory, part, O_WRONLY | O_NOFOLLOW | (j->bytes ? O_APPEND : O_CREAT | O_EXCL), 0600) : -1;
		struct stat st;
		bool ok = out >= 0 && !fstat(out, &st) && S_ISREG(st.st_mode) && st.st_size == j->bytes && (!j->bytes || (int64_t)st.st_ino == j->inode) && room(directory, n);
		if (ok) {
			j->inode = st.st_ino;
			ok = nd_audio_write_original(out, buffer, (size_t)n);
		}
		if (out >= 0 && close(out))
			ok = false;
		if (directory >= 0)
			close(directory);
		navidrome_cache_end();
		if (!ok) {
			error = "nd_dl_storage";
			break;
		}
		j->bytes += n;
		sha256_update(&incoming, buffer, (size_t)n);
		source_offset += n;
		pthread_mutex_lock(&mutex);
		view.active_key = j->key;
		view.bytes = j->bytes;
		view.length = j->total;
		view.revision++;
		pthread_mutex_unlock(&mutex);
		if (j->bytes == n || seconds() - last_saved >= 1) {
			if (!save_job(r, j)) {
				error = "nd_dl_store_error";
				break;
			}
			last_saved = seconds();
		}
	}
	http_session_free(stream);
	nd_audio_copy_release();
	if (error && !current(r))
		error = "nd_audio_cancelled";
	if (!error && (!placed || j->bytes <= 0 || (j->total && j->bytes != j->total)))
		error = "nd_audio_invalid";
	if (!error && !current(r))
		error = "nd_audio_cancelled";
	char transferred[65];
	if (!error)
		sha256_final_hex(&incoming, transferred);
	if (!error) {
		j->state = NDDL_VALIDATING;
		if (!save_job(r, j))
			error = "nd_dl_store_error";
		if (!error && navidrome_playback_begin(&r->context)) {
			int directory = nd_dl_directory(root, relative, false);
			char path[512];
			int n = snprintf(path, sizeof(path), "%s/%s", root, j->partial);
			struct stat st;
			int fd = directory >= 0 ? openat(directory, part, O_WRONLY | O_NOFOLLOW) : -1;
			bool ok = fd >= 0 && !fsync(fd);
			if (fd >= 0 && close(fd))
				ok = false;
			if (!ok)
				error = "nd_dl_storage";
			if (ok)
				ok = n > 0 && n < (int)sizeof(path) && nd_audio_valid(path) && nd_dl_fingerprint(directory, part, j->digest, &st, current, r) && st.st_size == j->bytes && !strcmp(j->digest, transferred);
			if (directory >= 0)
				close(directory);
			navidrome_cache_end();
			if (!ok && !error)
				error = "nd_audio_invalid";
		} else if (!error)
			error = "nd_audio_cancelled";
	}
	if (!error) {
		j->state = NDDL_INTENT;
		if (!save_job(r, j))
			error = "nd_dl_store_error";
	}
	if (!error && navidrome_playback_begin(&r->context)) {
		int directory = nd_dl_directory(root, relative, false);
		publication_t p = {.directory = directory, .part = part, .leaf = leaf, .serial = r->serial, .result = ND_DOWNLOAD_PUBLISH_ERROR};
		bool ok = directory >= 0 && current(r) && navidrome_playback_action(&r->context, r->network, publish, &p);
		if (!ok)
			error = p.result == ND_DOWNLOAD_PUBLISH_UNSUPPORTED ? "nd_dl_atomic_unsupported" : p.result == ND_DOWNLOAD_PUBLISH_CONFLICT ? "nd_dl_conflict" : "nd_dl_storage";
		struct stat st;
		if (ok && !fstatat(directory, leaf, &st, AT_SYMLINK_NOFOLLOW)) {
			j->mtime = st.st_mtime;
			j->inode = st.st_ino;
		}
		if (directory >= 0) {
			if (ok)
				fsync(directory);
			close(directory);
		}
		navidrome_cache_end();
		if (ok) {
			j->state = NDDL_COMPLETE;
			j->error[0] = 0;
			if (!save_job(r, j)) {
				j->state = NDDL_INTENT;
				error = "nd_dl_registration_pending";
			} else {
				pthread_mutex_lock(&mutex);
				saved_pending = true;
				saved_library = r->library;
				pthread_mutex_unlock(&mutex);
			}
		}
	} else if (!error)
		error = "nd_audio_cancelled";
	pthread_mutex_lock(&mutex);
	network_wanted = false;
	view.active_key = 0;
	pthread_mutex_unlock(&mutex);
	streamturn_release();
	return error;
}

static void reconcile(run_t *r) {
	nd_dl_store_t s;
	if (!open_store(r, &s, true))
		return;
	sqlite3_stmt *st = NULL;
	int64_t after = 0;
	while (nd_dl_prepare(&s, &st, "SELECT key FROM job WHERE state=3 AND key>? ORDER BY key LIMIT 1")) {
		sqlite3_bind_int64(st, 1, after);
		bool found = sqlite3_step(st) == SQLITE_ROW;
		if (found)
			after = sqlite3_column_int64(st, 0);
		sqlite3_finalize(st);
		st = NULL;
		if (!found)
			break;
		nd_dl_job_t j;
		if (!nd_dl_get(&s, after, &j))
			break;
		char relative[512];
		snprintf(relative, sizeof(relative), "%s", j.path);
		char *leaf = strrchr(relative, '/');
		if (!leaf)
			continue;
		*leaf++ = 0;
		int dir = nd_dl_directory(s.root, relative, false);
		char digest[65];
		struct stat observed;
		bool ok = dir >= 0 && nd_dl_fingerprint(dir, leaf, digest, &observed, context_current, r) && j.digest[0] && !strcmp(digest, j.digest) && j.inode == (int64_t)observed.st_ino;
		if (dir >= 0)
			close(dir);
		if (ok) {
			j.state = NDDL_COMPLETE;
			j.bytes = observed.st_size;
			j.mtime = observed.st_mtime;
			j.inode = observed.st_ino;
			j.error[0] = 0;
		} else {
			j.state = NDDL_PAUSED;
			snprintf(j.error, sizeof(j.error), "nd_dl_registration_pending");
		}
		if (!nd_dl_sql(&s, "BEGIN IMMEDIATE") || !nd_dl_update(&s, &j) || !navidrome_playback_action(&r->context, false, commit_sql, s.db))
			break;
		if (j.state == NDDL_COMPLETE)
			accept_repaired_mapping(r, &j);
	}
	close_store(&s);
}
static void *download(void *unused) {
	(void)unused;
	thread_be_background("navidrome originals");
	for (;;) {
		pthread_mutex_lock(&mutex);
		while (!armed && !stopping)
			pthread_cond_wait(&changed, &mutex);
		if (stopping) {
			pthread_mutex_unlock(&mutex);
			break;
		}
		run_t r = {.library = active_library, .context = active_context, .serial = serial};
		unsigned admission = admissions;
		pthread_mutex_unlock(&mutex);
		if (!context_current(&r) || r.context.card_epoch != r.library.card_epoch) {
			navidromedownloads_cancel();
			continue;
		}
		reconcile(&r);
		nd_dl_store_t s;
		nd_dl_job_t j;
		bool found = false;
		if (open_store(&r, &s, true)) {
			found = nd_dl_next(&s, &j);
			close_store(&s);
		}
		if (!found) {
			pthread_mutex_lock(&mutex);
			if (r.serial == serial && admission == admissions) {
				armed = false;
				nd_wipe(&active_context, sizeof(active_context));
			}
			pthread_mutex_unlock(&mutex);
			nd_wipe(&r.context, sizeof(r.context));
			continue;
		}
		const char *error = transfer(&r, &j);
		if (error) {
			remove_partial(&r, &j);
			bool cancelled = !strcmp(error, "nd_audio_cancelled");
			bool eligible = navidrome_playback_current(&r.context, r.network);
			pthread_mutex_lock(&mutex);
			bool resume = armed && !stopping && eligible;
			pthread_mutex_unlock(&mutex);
			if (cancelled && !resume)
				navidromedownloads_cancel();
			if (j.state != NDDL_INTENT)
				j.state = cancelled ? (resume ? NDDL_QUEUED : NDDL_PAUSED) : NDDL_FAILED;
			snprintf(j.error, sizeof(j.error), "%s", error);
			save_job(&r, &j);
			bool systemic = !strcmp(error, "nd_network_error") || !strcmp(error, "nd_auth_error") || !strcmp(error, "nd_reconnect_required") || !strcmp(error, "nd_dl_storage") || !strcmp(error, "nd_dl_atomic_unsupported") || !strcmp(error, "nd_dl_store_error");
			if (systemic) {
				if (r.network && (!strcmp(error, "nd_network_error") || !strcmp(error, "nd_auth_error")))
					navidrome_network_failed(&r.context);
				pthread_mutex_lock(&mutex);
				armed = false;
				nd_wipe(&active_context, sizeof(active_context));
				pthread_mutex_unlock(&mutex);
				if (open_store(&r, &s, true)) {
					nd_dl_sql(&s, "UPDATE job SET state=5 WHERE state IN(0,1,2,8)");
					close_store(&s);
				}
			}
			// The next background acquire is cancellable; never wait for a whole
			// playback transfer here with an uncancellable idle/join operation.
		}
		nd_wipe(&r.context, sizeof(r.context));
	}
	return NULL;
}
static bool start(void) {
	pthread_mutex_lock(&mutex);
	bool running = started;
	pthread_mutex_unlock(&mutex);
	if (running)
		return true;
	if (!streamturn_register_abandon(preempt) || !streamturn_set_background_cancel(preempt))
		return false;
	navidrome_set_download_hooks(navidromedownloads_cancel, navidromedownloads_shutdown);
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 512 * 1024);
	pthread_mutex_lock(&mutex);
	stopping = false;
	bool ok = pthread_create(&manager_thread, &attr, manage, NULL) == 0;
	if (ok && pthread_create(&transfer_thread, &attr, download, NULL)) {
		stopping = true;
		pthread_cond_broadcast(&changed);
		pthread_mutex_unlock(&mutex);
		pthread_join(manager_thread, NULL);
		pthread_attr_destroy(&attr);
		return false;
	}
	started = ok;
	pthread_mutex_unlock(&mutex);
	pthread_attr_destroy(&attr);
	return ok;
}
void navidromedownloads_init(void) {
	nd_audio_set_original_resolver(nd_download_resolve);
	navidrome_set_download_hooks(navidromedownloads_cancel, navidromedownloads_shutdown);
}
bool navidromedownloads_request(const nd_dl_request_t *q, nd_dl_captured_cb callback, void *user) {
	if (!q || (q->remote_album && (q->action != NDDL_CAPTURE || q->kind != ND_ALBUMS || !q->id[0] || q->direct || q->session || q->playlist || !q->profile_epoch)) || q->playlist < 0 || (q->playlist && (q->playlist_revision <= 0 || q->direct || q->session || q->action != NDDL_CAPTURE)) || !start())
		return false;
	nd_context_t context;
	bool eligible = navidrome_context(&context, false) && navidrome_playback_scope(&context, &q->library) && (!q->profile_epoch || q->profile_epoch == context.profile_epoch);
	if (!eligible) {
		nd_wipe(&context, sizeof(context));
		return false;
	}
	if (capture_pin(q) && !navidrome_playback_pin_source(&q->library)) {
		nd_wipe(&context, sizeof(context));
		return false;
	}
	pthread_mutex_lock(&mutex);
	bool ok = !pending && !view.busy && !delivery.pending && !stopping;
	if (ok) {
		if (q->action == NDDL_PAUSE || q->action == NDDL_CANCEL) {
			serial++;
			armed = network_wanted = false;
		}
		command = *q;
		command.captured = nd_owned_retain(q->captured);
		command_context = context;
		command_callback = callback;
		command_user = user;
		pending = true;
		view.busy = true;
		pthread_cond_broadcast(&changed);
	}
	pthread_mutex_unlock(&mutex);
	nd_wipe(&context, sizeof(context));
	if (!ok && capture_pin(q))
		nd_store_release_source(&q->library);
	return ok;
}
void navidromedownloads_shutdown(void) {
	navidromedownloads_cancel();
	pthread_mutex_lock(&mutex);
	bool join = started;
	nd_owned_rows_t *captured = pending ? command.captured : NULL;
	bool release_pin = pending && capture_pin(&command);
	command.captured = NULL;
	nd_library_t pinned = command.library;
	pending = false;
	started = false;
	stopping = true;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&mutex);
	nd_owned_release(captured);
	if (release_pin)
		nd_store_release_source(&pinned);
	if (join) {
		pthread_join(manager_thread, NULL);
		pthread_join(transfer_thread, NULL);
	}
	pthread_mutex_lock(&mutex);
	nd_wipe(&active_context, sizeof(active_context));
	nd_wipe(&command_context, sizeof(command_context));
	view.busy = false;
	delivery.pending = false;
	pthread_mutex_unlock(&mutex);
}

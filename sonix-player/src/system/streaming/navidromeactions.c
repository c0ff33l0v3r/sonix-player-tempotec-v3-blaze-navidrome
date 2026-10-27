#include "navidromeactions.h"
#include "navidromeplaycatalog.h"
#include "src/gui/shell/gui.h"
#include "src/system/core/utils.h"
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ACTION_MEMORY 64
#ifndef ND_ACTION_PAGES
#define ND_ACTION_PAGES 7680 // 30 MiB plus bounded one-row rollback journal.
#endif
typedef struct action_rows {
	nd_owned_rows_t owned;
	nd_playrow_t *memory;
	int ordinals[ACTION_MEMORY];
	bool selection, pinned, complete_albums;
	char path[1200];
	dev_t device;
	ino_t inode;
	struct action_rows *next, *retired_next;
} action_rows_t;
typedef struct {
	nd_action_request_t request;
	nd_context_t context;
	nd_action_result_t *result;
	void (*done)(nd_action_result_t *, void *);
	void *user;
	bool pinned;
} action_job_t;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static action_rows_t *live, *retired;
static action_job_t *pending, *deferred;
static bool started, busy;
static void *work(void *unused);
static bool start(void) {
	pthread_mutex_lock(&mutex);
	if (!started) {
		pthread_t thread;
		pthread_attr_t attr;
		int error = pthread_attr_init(&attr);
		if (!error) {
			error = pthread_attr_setstacksize(&attr, 512 * 1024);
			if (!error)
				error = pthread_create(&thread, &attr, work, NULL);
			pthread_attr_destroy(&attr);
		}
		if (!error) {
			pthread_detach(thread);
			started = true;
		}
	}
	bool ok = started;
	pthread_mutex_unlock(&mutex);
	return ok;
}
static void retire(nd_owned_rows_t *owned) {
	action_rows_t *rows = (action_rows_t *)owned;
	pthread_mutex_lock(&mutex);
	rows->retired_next = retired;
	retired = rows;
	pthread_cond_signal(&ready);
	pthread_mutex_unlock(&mutex);
}
static bool file_current(const action_rows_t *rows) {
	struct stat st;
	return !lstat(rows->path, &st) && S_ISREG(st.st_mode) && st.st_dev == rows->device && st.st_ino == rows->inode;
}
static void cleanup_orphans(const char *path) {
	DIR *dir = opendir(path);
	struct dirent *entry;
	const size_t prefix = sizeof(".search-action-") - 1;
	while (dir && (entry = readdir(dir))) {
		if (strlen(entry->d_name) != prefix + 6 || strncmp(entry->d_name, ".search-action-", prefix))
			continue;
		char file[1500];
		snprintf(file, sizeof(file), "%s/%s", path, entry->d_name);
		pthread_mutex_lock(&mutex);
		bool held = false;
		for (action_rows_t *p = live; p; p = p->next)
			if (!strcmp(p->path, file)) {
				held = true;
				break;
			}
		if (!held) {
			struct stat st;
			if (!lstat(file, &st) && S_ISREG(st.st_mode)) {
				unlink(file);
				char journal[1510];
				snprintf(journal, sizeof(journal), "%s-journal", file);
				if (!lstat(journal, &st) && S_ISREG(st.st_mode))
					unlink(journal);
			}
		}
		pthread_mutex_unlock(&mutex);
	}
	if (dir)
		closedir(dir);
}
static bool open_rows(const action_rows_t *rows, sqlite3 **db, bool write) {
	if (!file_current(rows) || sqlite3_open_v2(rows->path, db, write ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
		return false;
	char sql[120];
	snprintf(sql, sizeof(sql), "PRAGMA cache_size=-64;%s", write ? "" : "PRAGMA query_only=ON;");
	if (sqlite3_exec(*db, sql, NULL, NULL, NULL) != SQLITE_OK)
		return false;
	if (write) {
		snprintf(sql, sizeof(sql), "PRAGMA max_page_count=%d", ND_ACTION_PAGES);
		if (sqlite3_exec(*db, sql, NULL, NULL, NULL) != SQLITE_OK)
			return false;
	}
	return true;
}
static void close_rows(sqlite3 *db) {
	if (db)
		sqlite3_close(db);
}
static void decode(sqlite3_stmt *st, nd_playrow_t *row) {
	memset(row, 0, sizeof(*row));
	char *values[] = {row->row.id, row->row.title, row->row.artist, row->row.cover, row->album_id, row->album, row->artist_id, row->album_artist, row->genre};
	const size_t caps[] = {ND_ID_MAX, ND_TEXT_MAX, ND_TEXT_MAX, ND_ID_MAX, ND_ID_MAX, ND_TEXT_MAX, ND_ID_MAX, ND_TEXT_MAX, ND_TEXT_MAX};
	for (int i = 0; i < 9; i++) {
		const char *value = (const char *)sqlite3_column_text(st, i);
		snprintf(values[i], caps[i], "%s", value ? value : "");
	}
	row->row.duration = sqlite3_column_int(st, 9);
	row->row.disc = sqlite3_column_int(st, 10);
	row->row.track = sqlite3_column_int(st, 11);
	row->row.favorite = sqlite3_column_int(st, 12);
	row->row.favorite_known = sqlite3_column_int(st, 13);
	row->year = sqlite3_column_int(st, 14);
}
static bool put(sqlite3 *db, int ordinal, const nd_playrow_t *row) {
	sqlite3_stmt *st = NULL;
	bool ok = sqlite3_prepare_v2(db, "INSERT INTO item VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK;
	if (ok) {
		sqlite3_bind_int(st, 1, ordinal);
		const char *values[] = {row->row.id, row->row.title, row->row.artist, row->row.cover, row->album_id, row->album, row->artist_id, row->album_artist, row->genre};
		for (int i = 0; i < 9; i++)
			sqlite3_bind_text(st, i + 2, values[i], -1, SQLITE_TRANSIENT);
		const int numbers[] = {row->row.duration, row->row.disc, row->row.track, row->row.favorite, row->row.favorite_known, row->year};
		for (int i = 0; i < 6; i++)
			sqlite3_bind_int(st, i + 11, numbers[i]);
		ok = sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	return ok;
}
static int read_rows(const nd_owned_rows_t *owned, int first, nd_playrow_t *out, int limit) {
	const action_rows_t *rows = (const action_rows_t *)owned;
	if (first >= owned->count)
		return 0;
	int count = LV_MIN(limit, owned->count - first);
	if (rows->memory) {
		memcpy(out, rows->memory + first, (size_t)count * sizeof(*out));
		return count;
	}
	if (!navidrome_playback_begin(&owned->identity))
		return -1;
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	bool ok = open_rows(rows, &db, false) && sqlite3_prepare_v2(db, "SELECT id,title,artist,cover,album_id,album,artist_id,album_artist,genre,duration,disc,track,favorite,known,year FROM item ORDER BY ordinal LIMIT ? OFFSET ?", -1, &st, NULL) == SQLITE_OK;
	int found = 0, rc = SQLITE_DONE;
	if (ok) {
		sqlite3_bind_int(st, 1, count);
		sqlite3_bind_int(st, 2, first);
		while ((rc = sqlite3_step(st)) == SQLITE_ROW && found < count)
			decode(st, &out[found++]);
	}
	ok = ok && rc == SQLITE_DONE && found == count;
	sqlite3_finalize(st);
	close_rows(db);
	navidrome_cache_end();
	return ok ? found : -1;
}
static int album_multi(const nd_owned_rows_t *owned, const char *id) {
	const action_rows_t *rows = (const action_rows_t *)owned;
	if (!rows->complete_albums || !id || !id[0] || !nd_owned_current(owned))
		return -1;
	if (rows->memory) {
		int multi = -1;
		for (int i = 0; i < owned->count; i++)
			if (!strcmp(rows->memory[i].album_id, id))
				multi = LV_MAX(multi, rows->memory[i].row.disc > 1);
		return multi;
	}
	if (!navidrome_playback_begin(&owned->identity))
		return -1;
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	int multi = -1;
	if (open_rows(rows, &db, false) && sqlite3_prepare_v2(db, "SELECT max(disc)>1 FROM item WHERE album_id=?", -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
			multi = sqlite3_column_int(st, 0);
	}
	sqlite3_finalize(st);
	close_rows(db);
	navidrome_cache_end();
	return multi;
}
static action_rows_t *create_rows(const nd_action_request_t *q, const nd_context_t *identity, bool selection) {
	action_rows_t *rows = calloc(1, sizeof(*rows));
	if (!rows)
		return NULL;
	rows->memory = calloc(ACTION_MEMORY, sizeof(*rows->memory));
	if (!rows->memory) {
		free(rows);
		return NULL;
	}
	rows->owned = (nd_owned_rows_t){.refs = 1, .local = !q->remote, .identity = *identity, .library = q->library, .read = read_rows, .album_multi = album_multi, .destroy = retire};
	nd_wipe(rows->owned.identity.password, sizeof(rows->owned.identity.password));
	rows->selection = selection;
	rows->complete_albums = !selection && q->expand && q->kind == ND_ALBUMS;
	if (!q->remote && q->library.ready) {
		rows->pinned = nd_store_retain_source(&q->library);
		if (!rows->pinned) {
			free(rows->memory);
			free(rows);
			return NULL;
		}
	}
	return rows;
}
static bool spill(action_rows_t *rows) {
	if (!rows->owned.library.writable || !navidrome_playback_begin(&rows->owned.identity))
		return false;
	char path[1200];
	snprintf(path, sizeof(path), "%s", rows->owned.library.path);
	char *slash = strrchr(path, '/');
	bool ok = slash != NULL;
	if (ok) {
		*slash = 0;
		for (char *p = path + 1; *p; p++)
			if (*p == '/') {
				*p = 0;
				mkdir(path, 0700);
				*p = '/';
			}
		mkdir(path, 0700);
		cleanup_orphans(path);
		ok = snprintf(rows->path, sizeof(rows->path), "%s/.search-action-XXXXXX", path) < (int)sizeof(rows->path);
	}
	int fd = ok ? mkstemp(rows->path) : -1;
	struct stat st;
	ok = fd >= 0 && !fstat(fd, &st);
	if (fd >= 0)
		close(fd);
	if (ok) {
		rows->device = st.st_dev;
		rows->inode = st.st_ino;
	}
	sqlite3 *db = NULL;
	if (ok)
		ok = open_rows(rows, &db, true);
	char sql[850];
	snprintf(sql, sizeof(sql), "PRAGMA page_size=4096;PRAGMA max_page_count=%d;PRAGMA journal_mode=DELETE;PRAGMA journal_size_limit=1048576;CREATE TABLE item(ordinal INTEGER PRIMARY KEY,id TEXT,title TEXT,artist TEXT,cover TEXT,album_id TEXT,album TEXT,artist_id TEXT,album_artist TEXT,genre TEXT,duration INT,disc INT,track INT,favorite INT,known INT,year INT);CREATE %s INDEX item_id ON item(id);BEGIN", ND_ACTION_PAGES, rows->selection ? "UNIQUE" : "");
	if (ok)
		ok = sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
	if (ok && rows->complete_albums)
		ok = sqlite3_exec(db, "CREATE INDEX item_album ON item(album_id,disc)", NULL, NULL, NULL) == SQLITE_OK;
	for (int i = 0; ok && i < rows->owned.count; i++)
		ok = put(db, rows->ordinals[i], &rows->memory[i]);
	if (db)
		ok = sqlite3_exec(db, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK && ok;
	close_rows(db);
	if (ok) {
		free(rows->memory);
		rows->memory = NULL;
		pthread_mutex_lock(&mutex);
		rows->next = live;
		live = rows;
		pthread_mutex_unlock(&mutex);
	} else {
		if (file_current(rows))
			unlink(rows->path);
		rows->path[0] = 0;
	}
	navidrome_cache_end();
	return ok;
}
static bool contains(const action_rows_t *rows, const char *id) {
	if (rows->memory) {
		for (int i = 0; i < rows->owned.count; i++)
			if (!strcmp(rows->memory[i].row.id, id))
				return true;
		return false;
	}
	if (!navidrome_playback_begin(&rows->owned.identity))
		return false;
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	bool found = open_rows(rows, &db, false) && sqlite3_prepare_v2(db, "SELECT 1 FROM item WHERE id=?", -1, &st, NULL) == SQLITE_OK;
	if (found) {
		sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
		found = sqlite3_step(st) == SQLITE_ROW;
	}
	sqlite3_finalize(st);
	close_rows(db);
	navidrome_cache_end();
	return found;
}
static bool valid_row(const nd_playrow_t *row) {
	const char *values[] = {row->row.id, row->row.title, row->row.artist, row->row.cover, row->album_id, row->album, row->artist_id, row->album_artist, row->genre};
	const size_t caps[] = {ND_ID_MAX, ND_TEXT_MAX, ND_TEXT_MAX, ND_ID_MAX, ND_ID_MAX, ND_TEXT_MAX, ND_ID_MAX, ND_TEXT_MAX, ND_TEXT_MAX};
	if (!row->row.id[0])
		return false;
	for (int i = 0; i < 9; i++)
		if (!memchr(values[i], 0, caps[i]))
			return false;
	return true;
}
static bool change(action_rows_t *rows, const nd_playrow_t *row, int ordinal, bool toggle) {
	if (!valid_row(row) || ordinal < 0 || rows->owned.count == INT_MAX || !nd_owned_current(&rows->owned))
		return false;
	if (rows->memory) {
		for (int i = 0; toggle && i < rows->owned.count; i++)
			if (!strcmp(rows->memory[i].row.id, row->row.id)) {
				memmove(rows->memory + i, rows->memory + i + 1, (size_t)(rows->owned.count - i - 1) * sizeof(*row));
				memmove(rows->ordinals + i, rows->ordinals + i + 1, (size_t)(rows->owned.count - i - 1) * sizeof(int));
				rows->owned.count--;
				return true;
			}
		for (int j = 0; j < rows->owned.count; j++)
			if (rows->ordinals[j] == ordinal)
				return false;
		if (rows->owned.count < ACTION_MEMORY) {
			int i = rows->owned.count;
			while (i && rows->ordinals[i - 1] > ordinal) {
				rows->memory[i] = rows->memory[i - 1];
				rows->ordinals[i] = rows->ordinals[i - 1];
				i--;
			}
			if ((i && rows->ordinals[i - 1] == ordinal) || (i < rows->owned.count && rows->ordinals[i] == ordinal))
				return false;
			rows->memory[i] = *row;
			rows->ordinals[i] = ordinal;
			rows->owned.count++;
			return true;
		}
		if (!spill(rows))
			return false;
	}
	if (!navidrome_playback_begin(&rows->owned.identity))
		return false;
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	bool ok = open_rows(rows, &db, true) && sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK;
	int removed = 0;
	if (ok && toggle) {
		ok = sqlite3_prepare_v2(db, "DELETE FROM item WHERE id=?", -1, &st, NULL) == SQLITE_OK;
		if (ok) {
			sqlite3_bind_text(st, 1, row->row.id, -1, SQLITE_TRANSIENT);
			ok = sqlite3_step(st) == SQLITE_DONE;
			removed = sqlite3_changes(db);
		}
		sqlite3_finalize(st);
	}
	if (ok && !removed)
		ok = put(db, ordinal, row);
	if (db)
		ok = sqlite3_exec(db, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK && ok;
	close_rows(db);
	navidrome_cache_end();
	if (ok)
		rows->owned.count += removed ? -1 : 1;
	return ok;
}
static bool job_current(void *value) { return navidrome_playback_current(value, false); }
static bool network_current(void *value) { return job_current(value) && navidrome_search_current(value, true); }
static bool local_album(action_rows_t *out, const nd_action_request_t *q, const nd_context_t *identity, const char *id) {
	if (!q->library.ready || !nd_store_retain_source(&q->library))
		return false;
	nd_query_t query = {.kind = ND_TRACKS, .filter_kind = ND_ALBUMS};
	snprintf(query.filter, sizeof(query.filter), "%.*s", ND_ID_MAX - 1, id);
	bool ok = true;
	int first = 0, total = 0;
	do {
		if (!navidrome_playback_begin(identity)) {
			ok = false;
			break;
		}
		nd_store_t store = {0};
		nd_row_t window[ND_WINDOW];
		nd_playrow_t album;
		int count = 0;
		ok = nd_store_open_source(&store, q->library.path, q->library.scope, false, false, q->library.favorites) && nd_store_metadata(&store, q->library.generation, ND_ALBUMS, id, &album) && !album.row.details && nd_store_query(&store, q->library.generation, &query, first, window, &count, &total) && total > 0;
		for (int i = 0; ok && i < count; i++) {
			nd_playrow_t row;
			ok = nd_store_metadata(&store, q->library.generation, ND_TRACKS, window[i].id, &row);
			if (ok) {
				snprintf(row.album_artist, sizeof(row.album_artist), "%s", album.album_artist);
				ok = change(out, &row, out->owned.count, false);
			}
		}
		nd_store_close(&store);
		navidrome_cache_end();
		first += count;
		if (!count && first < total)
			ok = false;
	} while (ok && first < total);
	nd_store_release_source(&q->library);
	return ok;
}
void nd_action_execute(const nd_action_request_t *q, const nd_context_t *identity, nd_action_result_t *result) {
	memset(result, 0, sizeof(*result));
	result->token = q->token;
	if (!start() || !job_current((void *)identity) || q->library.card_epoch != identity->card_epoch || q->kind < ND_ALBUMS || q->kind > ND_ARTISTS || q->count < 0 || q->count > ND_WINDOW || (q->selection && !nd_owned_current(q->selection))) {
		result->error = "nd_pl_stale";
		return;
	}
	if (q->op == ND_ACTION_TOGGLE || q->op == ND_ACTION_MARKS) {
		if (q->selection && (q->selection->read != read_rows || !((action_rows_t *)q->selection)->selection)) {
			result->error = "nd_pl_stale";
			return;
		}
		action_rows_t *rows = (action_rows_t *)q->selection;
		if (!rows && q->op == ND_ACTION_TOGGLE)
			rows = create_rows(q, identity, true);
		else
			nd_owned_retain(q->selection);
		result->selection = rows ? &rows->owned : NULL;
		if (q->op == ND_ACTION_TOGGLE) {
			nd_playrow_t row = q->row;
			if (!q->remote && q->library.ready && navidrome_playback_begin(identity)) {
				nd_store_t store = {0};
				nd_playrow_t richer;
				if (nd_store_open_source(&store, q->library.path, q->library.scope, false, false, q->library.favorites) && nd_store_metadata(&store, q->library.generation, q->kind, row.row.id, &richer))
					row = richer;
				nd_store_close(&store);
				navidrome_cache_end();
			}
			if (!rows || !change(rows, &row, q->ordinal, true))
				result->error = q->library.writable ? "nd_pl_storage_error" : "nd_cache_readonly";
		}
		if (rows) {
			result->selected = rows->owned.count;
			for (int i = 0; i < q->count; i++)
				result->marks[i] = contains(rows, q->ids[i]);
		}
		return;
	}
	action_rows_t *out = create_rows(q, identity, false);
	if (!out) {
		result->error = "nd_json_memory";
		return;
	}
	result->capture = &out->owned;
	int total = q->selection ? q->selection->count : 1;
	for (int first = 0; first < total && !result->error;) {
		nd_playrow_t window[ND_WINDOW];
		int count = q->selection ? nd_owned_read(q->selection, first, window, ND_WINDOW) : 1;
		if (!q->selection)
			window[0] = q->row;
		if (count <= 0) {
			result->error = "nd_pl_stale";
			break;
		}
		for (int i = 0; i < count && !result->error; i++) {
			if (!valid_row(&window[i])) {
				result->error = "nd_response_error";
				break;
			}
			if (!job_current((void *)identity)) {
				result->error = "nd_audio_cancelled";
				break;
			}
			if (!q->expand || q->kind == ND_TRACKS) {
				if (!q->remote && q->library.ready && navidrome_playback_begin(identity)) {
					nd_store_t store = {0};
					nd_playrow_t richer;
					if (nd_store_open_source(&store, q->library.path, q->library.scope, false, false, q->library.favorites) && nd_store_metadata(&store, q->library.generation, q->kind, window[i].row.id, &richer))
						window[i] = richer;
					nd_store_close(&store);
					navidrome_cache_end();
				}
				if (!change(out, &window[i], out->owned.count, false))
					result->error = q->library.writable ? "nd_search_child_limit" : "nd_cache_readonly";
			} else if (q->kind == ND_ALBUMS) {
				nd_owned_rows_t *retained_album = NULL;
				for (int k = 0; k < 2; k++)
					if (q->albums[k] && nd_owned_current(q->albums[k]) && !strcmp(q->album_ids[k], window[i].row.id))
						retained_album = q->albums[k];
				if (retained_album) {
					for (int j = 0; j < retained_album->count && !result->error;) {
						nd_playrow_t tracks[ND_WINDOW];
						int n = nd_owned_read(retained_album, j, tracks, ND_WINDOW);
						if (n <= 0) {
							result->error = "nd_cache_storage";
							break;
						}
						for (int k = 0; k < n && !result->error; k++)
							if (!change(out, &tracks[k], out->owned.count, false))
								result->error = "nd_search_child_limit";
						j += n;
					}
				} else if (!q->remote) {
					if (!local_album(out, q, identity, window[i].row.id))
						result->error = "nd_pl_stale";
				} else {
					nd_query_t query = {.kind = ND_TRACKS, .filter_kind = ND_ALBUMS};
					memcpy(query.filter, window[i].row.id, sizeof(query.filter));
					nd_playcatalog_t *album = NULL;
					if (!network_current((void *)identity))
						result->error = "nd_reconnect_required";
					else
						album = nd_playcatalog_capture(&q->library, identity, &query, NULL, network_current, (void *)identity, &result->error);
					for (int j = 0; album && j < nd_playcatalog_count(album) && !result->error; j++) {
						nd_playrow_t row;
						if (!nd_playcatalog_read(album, j, &row) || !change(out, &row, out->owned.count, false))
							result->error = "nd_search_child_limit";
					}
					bool captured_album = album != NULL;
					nd_playcatalog_free(album);
					if (!captured_album && result->error && job_current((void *)identity) && q->library.ready && local_album(out, q, identity, window[i].row.id))
						result->error = NULL;
				}
			} else
				result->error = "nd_pl_stale";
		}
		first += count;
	}
	if (!out->owned.count && !result->error)
		result->error = "nd_item_removed";
}
void nd_action_result_free(nd_action_result_t *result) {
	if (!result)
		return;
	nd_owned_release(result->selection);
	nd_owned_release(result->capture);
	free(result);
}
static void deliver(void *value) {
	action_job_t *job = value;
	if (job->done)
		job->done(job->result, job->user);
	else
		nd_action_result_free(job->result);
	free(job);
	pthread_mutex_lock(&mutex);
	busy = false;
	pthread_mutex_unlock(&mutex);
}
void navidrome_actions_poll(void) {
	pthread_mutex_lock(&mutex);
	action_job_t *job = deferred;
	deferred = NULL;
	pthread_mutex_unlock(&mutex);
	if (job)
		deliver(job);
}
bool navidrome_actions_busy(void) {
	pthread_mutex_lock(&mutex);
	bool b = busy;
	pthread_mutex_unlock(&mutex);
	return b;
}
static void clean(action_rows_t *rows) {
	if (*rows->path && navidrome_playback_begin(&rows->owned.identity)) {
		if (file_current(rows))
			unlink(rows->path);
		navidrome_cache_end();
	}
	pthread_mutex_lock(&mutex);
	action_rows_t **at = &live;
	while (*at && *at != rows)
		at = &(*at)->next;
	if (*at)
		*at = rows->next;
	pthread_mutex_unlock(&mutex);
	if (rows->pinned)
		nd_store_release_source(&rows->owned.library);
	free(rows->memory);
	free(rows);
}
static void *work(void *unused) {
	(void)unused;
	thread_be_background("navidrome search actions");
	for (;;) {
		pthread_mutex_lock(&mutex);
		while (!pending && !retired)
			pthread_cond_wait(&ready, &mutex);
		action_rows_t *garbage = retired;
		retired = NULL;
		action_job_t *job = pending;
		pending = NULL;
		pthread_mutex_unlock(&mutex);
		while (garbage) {
			action_rows_t *next = garbage->retired_next;
			clean(garbage);
			garbage = next;
		}
		if (!job)
			continue;
		nd_action_execute(&job->request, &job->context, job->result);
		nd_owned_release(job->request.selection);
		for (int k = 0; k < 2; k++)
			nd_owned_release(job->request.albums[k]);
		if (job->pinned)
			nd_store_release_source(&job->request.library);
		nd_wipe(&job->context, sizeof(job->context));
		if (!gui_post(deliver, job)) {
			pthread_mutex_lock(&mutex);
			deferred = job;
			pthread_mutex_unlock(&mutex);
		}
	}
	return NULL;
}
bool navidrome_actions_submit(const nd_action_request_t *q, void (*done)(nd_action_result_t *, void *), void *user) {
	if (!q || !start())
		return false;
	action_job_t *job = calloc(1, sizeof(*job));
	if (!job)
		return false;
	job->result = calloc(1, sizeof(*job->result));
	if (!job->result) {
		free(job);
		return false;
	}
	bool ok = navidrome_context(&job->context, false) && q->library.card_epoch == job->context.card_epoch && (!q->profile_epoch || q->profile_epoch == job->context.profile_epoch);
	if (ok && q->selection)
		ok = nd_owned_current(q->selection) && q->selection->identity.profile_epoch == job->context.profile_epoch && q->selection->identity.card_epoch == job->context.card_epoch;
	if (ok && !q->remote && q->library.ready) {
		job->pinned = navidrome_playback_pin_source(&q->library);
		ok = job->pinned;
	}
	pthread_mutex_lock(&mutex);
	ok = ok && !busy;
	if (ok) {
		job->request = *q;
		job->request.selection = nd_owned_retain(q->selection);
		for (int k = 0; k < 2; k++)
			job->request.albums[k] = nd_owned_retain(q->albums[k]);
		job->done = done;
		job->user = user;
		busy = true;
		pending = job;
		pthread_cond_signal(&ready);
	}
	pthread_mutex_unlock(&mutex);
	if (!ok) {
		if (job->pinned)
			nd_store_release_source(&q->library);
		free(job->result);
		nd_wipe(job, sizeof(*job));
		free(job);
	}
	return ok;
}

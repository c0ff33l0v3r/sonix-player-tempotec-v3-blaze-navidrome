#include "navidromeplaycatalog.h"
#include "navidromeplayliststore.h"
#include "src/system/core/utils.h"
#include "src/system/library/library.h"
#include "src/system/net/http.h"
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MEMORY_ROWS 64
#ifndef ND_PLAYCATALOG_PAGES
#define ND_PLAYCATALOG_PAGES 8192 // 32 MiB, including indexes; no WAL/sidecar growth.
#endif
struct nd_playcatalog {
	nd_context_t identity;
	nd_library_t library;
	nd_playrow_t *rows;
	int count, start;
	bool album, playlist;
	int64_t occurrences[MEMORY_ROWS];
	char path[1200];
	dev_t device;
	ino_t inode;
	struct nd_playcatalog *live_next, *retired_next;
};
static pthread_mutex_t catalogs_lock = PTHREAD_MUTEX_INITIALIZER, create_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cleanup_wake = PTHREAD_COND_INITIALIZER;
static nd_playcatalog_t *live, *retired;
static bool cleanup_started;

static void remove_file(nd_playcatalog_t *c) {
	if (!navidrome_playback_begin(&c->identity))
		return;
	struct stat st;
	if (!lstat(c->path, &st) && S_ISREG(st.st_mode) && st.st_dev == c->device && st.st_ino == c->inode)
		unlink(c->path);
	navidrome_cache_end();
}
static void *cleanup_work(void *unused) {
	(void)unused;
	thread_be_background("navidrome queue cleanup");
	for (;;) {
		pthread_mutex_lock(&catalogs_lock);
		while (!retired)
			pthread_cond_wait(&cleanup_wake, &catalogs_lock);
		nd_playcatalog_t *c = retired;
		retired = c->retired_next;
		pthread_mutex_unlock(&catalogs_lock);
		remove_file(c);
		free(c);
	}
	return NULL;
}
void nd_playcatalog_free(nd_playcatalog_t *c) {
	if (!c)
		return;
	free(c->rows);
	if (!*c->path) {
		free(c);
		return;
	}
	pthread_mutex_lock(&catalogs_lock);
	nd_playcatalog_t **at = &live;
	while (*at && *at != c)
		at = &(*at)->live_next;
	if (*at)
		*at = c->live_next;
	c->retired_next = retired;
	retired = c;
	pthread_cond_signal(&cleanup_wake);
	pthread_mutex_unlock(&catalogs_lock);
}
static bool file_current(const nd_playcatalog_t *c) {
	struct stat st;
	return !lstat(c->path, &st) && S_ISREG(st.st_mode) && st.st_dev == c->device && st.st_ino == c->inode;
}
static void cleanup_orphans(const char *dir) {
	DIR *d = opendir(dir);
	struct dirent *entry;
	while (d && (entry = readdir(d))) {
		if (strlen(entry->d_name) != 17 || strncmp(entry->d_name, ".playqueue-", 11))
			continue;
		char path[1500];
		snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
		pthread_mutex_lock(&catalogs_lock);
		bool used = false;
		for (nd_playcatalog_t *c = live; c; c = c->live_next)
			used |= !strcmp(c->path, path);
		pthread_mutex_unlock(&catalogs_lock);
		struct stat st;
		if (!used && !lstat(path, &st) && S_ISREG(st.st_mode))
			unlink(path);
	}
	if (d)
		closedir(d);
}
static bool create_file(nd_playcatalog_t *c) {
	if (!c->library.writable)
		return false;
	pthread_mutex_lock(&create_lock);
	pthread_mutex_lock(&catalogs_lock);
	if (!cleanup_started) {
		pthread_t worker;
		if (!pthread_create(&worker, NULL, cleanup_work, NULL)) {
			pthread_detach(worker);
			cleanup_started = true;
		}
	}
	pthread_mutex_unlock(&catalogs_lock);
	char dir[1100];
	snprintf(dir, sizeof(dir), "%s", c->library.path);
	char *slash = strrchr(dir, '/');
	bool ok = cleanup_started && slash;
	if (ok) {
		*slash = 0;
		for (char *p = dir + 1; *p; p++)
			if (*p == '/') {
				*p = 0;
				mkdir(dir, 0700);
				*p = '/';
			}
		mkdir(dir, 0700);
		cleanup_orphans(dir);
		snprintf(c->path, sizeof(c->path), "%s/.playqueue-XXXXXX", dir);
		int fd = mkstemp(c->path);
		struct stat st;
		ok = fd >= 0 && !fstat(fd, &st);
		if (fd >= 0)
			close(fd);
		if (ok) {
			c->device = st.st_dev;
			c->inode = st.st_ino;
		}
		pthread_mutex_lock(&catalogs_lock);
		c->live_next = live;
		live = c;
		pthread_mutex_unlock(&catalogs_lock);
	}
	pthread_mutex_unlock(&create_lock);
	return ok;
}
static const char *columns = "id,title,artist,cover,duration,disc,track,album_id,album,artist_id,favorite,known";
static bool put_occurrence(sqlite3 *db, int pos, const nd_playrow_t *row, int64_t occurrence) {
	sqlite3_stmt *st = NULL;
	bool ok = sqlite3_prepare_v2(db, "INSERT INTO track VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK;
	if (ok) {
		sqlite3_bind_int(st, 1, pos);
		const char *values[] = {row->row.id, row->row.title, row->row.artist, row->row.cover};
		for (int i = 0; i < 4; i++)
			sqlite3_bind_text(st, i + 2, values[i], -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(st, 6, row->row.duration);
		sqlite3_bind_int(st, 7, row->row.disc);
		sqlite3_bind_int(st, 8, row->row.track);
		sqlite3_bind_text(st, 9, row->album_id, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 10, row->album, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 11, row->artist_id, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(st, 12, row->row.favorite);
		sqlite3_bind_int(st, 13, row->row.favorite_known);
		char key[LIBRARY_SORT_KEY_MAX];
		library_sort_key_ex(row->row.title, key, sizeof(key), true);
		sqlite3_bind_text(st, 14, key, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(st, 15, occurrence);
		ok = sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	return ok;
}
static bool put(sqlite3 *db, int pos, const nd_playrow_t *row) { return put_occurrence(db, pos, row, 0); }
static bool open_write(nd_playcatalog_t *c, sqlite3 **db) {
	bool fresh = !*c->path;
	if (fresh && !create_file(c))
		return false;
	if (!file_current(c) || sqlite3_open_v2(c->path, db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK)
		return false;
	char sql[800];
	// Playlist staging is private and discarded on any failure: no rollback
	// journal is needed. Fixed-window appends keep its pager working set bounded.
	snprintf(sql, sizeof(sql), "PRAGMA page_size=4096; PRAGMA max_page_count=%d; PRAGMA cache_size=-64; PRAGMA journal_mode=%s;%s%s%s BEGIN;",
		ND_PLAYCATALOG_PAGES, c->playlist ? "OFF" : "MEMORY",
		fresh ? "CREATE TABLE track(pos INTEGER PRIMARY KEY,id TEXT " : "",
		fresh ? (c->playlist ? "" : "UNIQUE") : "",
		fresh ? ",title TEXT,artist TEXT,cover TEXT,duration INT,disc INT,track INT,album_id TEXT,album TEXT,artist_id TEXT,favorite INT,known INT,key TEXT,occurrence INTEGER);CREATE INDEX ordered ON track(disc,track,key,id);" : "");
	if (sqlite3_exec(*db, sql, NULL, NULL, NULL) != SQLITE_OK)
		return false;
	for (int i = 0; fresh && i < c->count; i++)
		if (!put_occurrence(*db, i, &c->rows[i], c->occurrences[i]))
			return false;
	return true;
}
static bool import_page(nd_playcatalog_t *c, const nd_document_t *doc, int array, const char *album_id, const char *album_name, bool (*current)(void *), void *user, const char **error) {
	int count = json_len(&doc->doc, array);
	if (count > INT_MAX - c->count) {
		*error = "nd_search_child_limit";
		return false;
	}
	bool disk = *c->path || c->count + count > MEMORY_ROWS;
	bool lease = !disk || navidrome_playback_begin(&c->identity);
	sqlite3 *db = NULL;
	bool ok = lease && (!disk || open_write(c, &db));
	for (int i = 0; ok && i < count; i++) {
		nd_playrow_t row;
		if (!current(user) || !nd_metadata_album_track(&doc->doc, json_at(&doc->doc, array, i), album_id, &row)) {
			*error = current(user) ? "nd_response_error" : "nd_audio_cancelled";
			ok = false;
			break;
		}
		if (!*row.album && album_name)
			snprintf(row.album, sizeof(row.album), "%s", album_name);
		if (disk) {
			ok = put(db, c->count, &row);
			if (!ok && sqlite3_errcode(db) == SQLITE_CONSTRAINT)
				*error = "nd_sync_inconsistent";
		} else {
			for (int j = 0; j < c->count; j++)
				if (!strcmp(c->rows[j].row.id, row.row.id)) {
					*error = "nd_sync_inconsistent";
					ok = false;
				}
			if (ok)
				c->rows[c->count] = row;
		}
		if (ok)
			c->count++;
	}
	if (db) {
		ok = sqlite3_exec(db, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK && ok;
		sqlite3_close(db);
	}
	if (disk && lease)
		navidrome_cache_end();
	if (ok && disk) {
		free(c->rows);
		c->rows = NULL;
	}
	if (!ok && !*error)
		*error = c->library.writable ? "nd_search_child_limit" : "nd_cache_readonly";
	return ok;
}
static int album_order(const void *a, const void *b) {
	const nd_playrow_t *x = a, *y = b;
	if (x->row.disc != y->row.disc)
		return (x->row.disc > y->row.disc) - (x->row.disc < y->row.disc);
	if (x->row.track != y->row.track)
		return (x->row.track > y->row.track) - (x->row.track < y->row.track);
	char kx[LIBRARY_SORT_KEY_MAX], ky[LIBRARY_SORT_KEY_MAX];
	library_sort_key_ex(x->row.title, kx, sizeof(kx), true);
	library_sort_key_ex(y->row.title, ky, sizeof(ky), true);
	int n = strcmp(kx, ky);
	return n ? n : strcmp(x->row.id, y->row.id);
}
bool nd_playcatalog_read(const nd_playcatalog_t *c, int index, nd_playrow_t *row) {
	if (index < 0 || index >= c->count || !navidrome_playback_current(&c->identity, false))
		return false;
	if (c->rows) {
		*row = c->rows[index];
		return true;
	}
	if (!navidrome_playback_begin(&c->identity))
		return false;
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	char sql[300];
	snprintf(sql, sizeof(sql), "SELECT %s FROM track %s", columns, c->album ? "ORDER BY disc,track,key,id LIMIT 1 OFFSET ?" : "WHERE pos=?");
	bool ok = file_current(c) && sqlite3_open_v2(c->path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK && sqlite3_exec(db, "PRAGMA cache_size=-64", NULL, NULL, NULL) == SQLITE_OK && sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK;
	if (ok) {
		sqlite3_bind_int(st, 1, index);
		ok = sqlite3_step(st) == SQLITE_ROW;
		if (ok) {
			memset(row, 0, sizeof(*row));
			char *fields[] = {row->row.id, row->row.title, row->row.artist, row->row.cover, row->album_id, row->album, row->artist_id};
			size_t caps[] = {sizeof(row->row.id), sizeof(row->row.title), sizeof(row->row.artist), sizeof(row->row.cover), sizeof(row->album_id), sizeof(row->album), sizeof(row->artist_id)};
			int cols[] = {0, 1, 2, 3, 7, 8, 9};
			for (int i = 0; i < 7; i++) {
				const char *v = (const char *)sqlite3_column_text(st, cols[i]);
				snprintf(fields[i], caps[i], "%s", v ? v : "");
			}
			row->row.duration = sqlite3_column_int(st, 4);
			row->row.disc = sqlite3_column_int(st, 5);
			row->row.track = sqlite3_column_int(st, 6);
			row->row.favorite = sqlite3_column_int(st, 10);
			row->row.favorite_known = sqlite3_column_int(st, 11);
		}
	}
	sqlite3_finalize(st);
	if (db)
		sqlite3_close(db);
	navidrome_cache_end();
	return ok;
}
int nd_playcatalog_count(const nd_playcatalog_t *c) { return c->count; }
int nd_playcatalog_start(const nd_playcatalog_t *c) { return c->start; }
nd_playcatalog_t *nd_playcatalog_playlist(const nd_library_t *lib, const nd_context_t *identity,
		const nd_playlist_capture_t *request, bool (*current)(void *), void *user, const char **error) {
	*error = NULL;
	if (request->playlist <= 0 || request->revision <= 0 || request->count < 0 ||
		(request->whole && request->single) || (!request->whole &&
		(request->occurrence <= 0 || request->ordinal < 0 || !*request->track))) {
		*error = "nd_pl_stale";
		return NULL;
	}
	nd_playcatalog_t *c = calloc(1, sizeof(*c));
	if (!c) { *error = "nd_json_memory"; return NULL; }
	c->identity = *identity;
	nd_wipe(c->identity.password, sizeof(c->identity.password));
	c->library = *lib;
	c->playlist = true;
	c->start = request->whole ? 0 : -1;
	c->rows = calloc(request->single ? 1 : MEMORY_ROWS, sizeof(*c->rows));
	if (!c->rows) *error = "nd_json_memory";
	char path[1200];
	if (!*error && !nd_pl_path(lib, path)) *error = "nd_pl_storage_error";
	if (!*error && request->single) {
		if (!current(user) || !navidrome_playback_begin(identity)) *error = "nd_audio_cancelled";
		else {
			nd_playlist_store_t store = {0};
			nd_playlist_entry_t entry;
			int total = 0;
			bool ok = nd_pl_open(&store, path, lib->scope, false) &&
				nd_pl_play_entry(&store, request->playlist, request->revision, request->ordinal,
					request->occurrence, request->track, &entry, &total);
			if (ok && request->count && total != request->count) { ok = false; store.error = "nd_pl_stale"; }
			if (ok) {
				c->rows[0].row = entry.track;
				c->occurrences[0] = entry.occurrence;
				c->count = 1; c->start = 0;
			} else *error = store.error ? store.error : "nd_pl_storage_error";
			nd_pl_close(&store);
			navidrome_cache_end();
		}
		if (!*error && !current(user)) *error = "nd_audio_cancelled";
		if (*error) { nd_playcatalog_free(c); return NULL; }
		return c;
	}
	int expected = -1;
	while (!*error) {
		if (!current(user) || !navidrome_playback_begin(identity)) {
			*error = "nd_audio_cancelled";
			break;
		}
		nd_playlist_store_t store = {0};
		nd_playlist_entry_t entries[ND_WINDOW];
		int count = 0, total = 0;
		bool ok = nd_pl_open(&store, path, lib->scope, false) &&
			nd_pl_play_window(&store, request->playlist, request->revision, c->count, entries, &count, &total);
		if (!ok) *error = store.error ? store.error : "nd_pl_storage_error";
		nd_pl_close(&store);
		if (ok && (total == 0 || (!request->whole && request->ordinal >= total) ||
			(request->count && total != request->count) || (expected >= 0 && total != expected))) {
			*error = "nd_pl_stale";
			ok = false;
		}
		expected = total;
		bool disk = *c->path || count > MEMORY_ROWS - c->count;
		sqlite3 *db = NULL;
		if (ok && disk && !lib->writable) { *error = "nd_cache_readonly"; ok = false; }
		if (ok && disk) ok = open_write(c, &db);
		for (int i = 0; ok && i < count; i++) {
			if (!current(user)) { *error = "nd_audio_cancelled"; ok = false; break; }
			nd_playlist_entry_t *entry = &entries[i];
			if (!request->whole && c->count == request->ordinal) {
				ok = entry->occurrence == request->occurrence && !strcmp(entry->track.id, request->track);
				if (!ok) { *error = "nd_pl_stale"; break; }
				c->start = c->count;
			}
			nd_playrow_t row = {.row = entry->track};
			if (disk)
				ok = put_occurrence(db, c->count, &row, entry->occurrence);
			else {
				c->rows[c->count] = row;
				c->occurrences[c->count] = entry->occurrence;
			}
			if (ok) c->count++;
		}
		if (db) {
			if (!ok && !*error && sqlite3_errcode(db) == SQLITE_FULL) *error = "nd_pl_play_limit";
			ok = sqlite3_exec(db, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK && ok;
			if (!ok && !*error && sqlite3_errcode(db) == SQLITE_FULL) *error = "nd_pl_play_limit";
			sqlite3_close(db);
		}
		if (ok && disk) { free(c->rows); c->rows = NULL; }
		navidrome_cache_end();
		if (!ok && !*error) *error = "nd_pl_storage_error";
		// An extra zero-row window rechecks the revision after the final copy.
		if (!count || *error) break;
	}
	if (!*error && (!current(user) || c->start < 0)) *error = "nd_audio_cancelled";
	if (*error) { nd_playcatalog_free(c); return NULL; }
	return c;
}
nd_playcatalog_t *nd_playcatalog_capture(const nd_library_t *lib, const nd_context_t *identity, const nd_query_t *q, const char *selected, bool (*current)(void *), void *user, const char **error) {
	*error = NULL;
	nd_playcatalog_t *c = calloc(1, sizeof(*c));
	if (!c) {
		*error = "nd_json_memory";
		return NULL;
	}
	c->identity = *identity;
	nd_wipe(c->identity.password, sizeof(c->identity.password));
	c->library = *lib;
	c->rows = calloc(MEMORY_ROWS, sizeof(*c->rows));
	c->start = -1;
	c->album = *q->filter != 0;
	if (!c->rows)
		*error = "nd_json_memory";
	char encoded[3 * 256 + 1], query[3 * 256 + 120];
	http_url_encode(c->album ? q->filter : q->search, encoded, sizeof(encoded));
	int offset = 0, amount = 1000;
	while (!*error) {
		nd_context_t ctx;
		if (!current(user)) {
			*error = "nd_audio_cancelled";
			break;
		}
		if (!navidrome_context(&ctx, true)) {
			*error = "nd_reconnect_required";
			break;
		}
		if (ctx.profile_epoch != identity->profile_epoch || ctx.card_epoch != identity->card_epoch) {
			nd_wipe(&ctx, sizeof(ctx));
			*error = "nd_audio_cancelled";
			break;
		}
		if (c->album)
			snprintf(query, sizeof(query), "id=%s", encoded);
		else
			snprintf(query, sizeof(query), "query=%s&songCount=%d&songOffset=%d&albumCount=0&artistCount=0", encoded, amount, offset);
		nd_document_t doc;
		*error = nd_api_finite_json(ctx.host, ctx.user, ctx.password, c->album ? "getAlbum" : "search3", query, ND_API_BYTES, current, user, &doc);
		if (*error && current(user) && (!strcmp(*error, "nd_network_error") || !strcmp(*error, "nd_tls_error") || !strcmp(*error, "nd_auth_error")))
			navidrome_search_failed(&ctx, *error);
		nd_wipe(&ctx, sizeof(ctx));
		if (*error && !c->album && !strcmp(*error, "nd_page_large") && amount > 1) {
			amount = (amount + 1) / 2;
			*error = NULL;
			continue;
		}
		if (*error)
			break;
		int obj = json_get(&doc.doc, doc.response, c->album ? "album" : "searchResult3");
		int array = json_get(&doc.doc, obj, "song");
		int n = json_len(&doc.doc, array);
		char parent[ND_ID_MAX] = "", title[ND_TEXT_MAX] = "";
		if (!nd_api_field_unique(&doc.doc, doc.response, c->album ? "album" : "searchResult3", true) || !nd_api_field_unique(&doc.doc, obj, "song", false) || obj < 0 || doc.doc.toks[obj].type != JSON_OBJECT || (array >= 0 && doc.doc.toks[array].type != JSON_ARRAY) || (!c->album && n > amount))
			*error = "nd_response_error";
		if (c->album) {
			nd_playrow_t album;
			if (!nd_metadata_row(&doc.doc, obj, ND_ALBUMS, &album) || strcmp(album.row.id, q->filter))
				*error = "nd_response_error";
			else {
				snprintf(parent, sizeof(parent), "%s", album.row.id);
				snprintf(title, sizeof(title), "%s", album.row.title);
			}
		}
		if (!*error)
			import_page(c, &doc, array, c->album ? parent : NULL, c->album ? title : NULL, current, user, error);
		nd_api_free(&doc);
		if (*error || c->album || n < amount)
			break;
		if (offset > INT_MAX - n) {
			*error = "nd_search_child_limit";
			break;
		}
		offset += n;
	}
	if (!*error && c->album && c->rows)
		qsort(c->rows, c->count, sizeof(*c->rows), album_order);
	// Locate the selected exact ID using the unique disk index, not N opens.
	if (!*error && *c->path) {
		bool lease = navidrome_playback_begin(&c->identity);
		sqlite3 *db = NULL;
		sqlite3_stmt *st = NULL;
		const char *sql = c->album ? "SELECT (SELECT count(*) FROM track a WHERE (a.disc,a.track,a.key,a.id)<(s.disc,s.track,s.key,s.id)) FROM track s WHERE s.id=?" : "SELECT pos FROM track WHERE id=?";
		bool ok = lease && current(user) && file_current(c) && sqlite3_open_v2(c->path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK && sqlite3_exec(db, "PRAGMA cache_size=-64", NULL, NULL, NULL) == SQLITE_OK && sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK;
		if (ok) {
			sqlite3_bind_text(st, 1, selected, -1, SQLITE_TRANSIENT);
			if (sqlite3_step(st) == SQLITE_ROW)
				c->start = sqlite3_column_int(st, 0);
		}
		sqlite3_finalize(st);
		if (db)
			sqlite3_close(db);
		if (lease)
			navidrome_cache_end();
		if (!ok)
			*error = "nd_audio_cancelled";
	}
	for (int i = 0; !*error && c->rows && i < c->count; i++) {
		nd_playrow_t row;
		if (!current(user) || !nd_playcatalog_read(c, i, &row)) {
			*error = "nd_audio_cancelled";
			break;
		}
		if (!strcmp(row.row.id, selected)) {
			c->start = i;
			break;
		}
	}
	if (!*error && c->start < 0)
		*error = "nd_item_removed";
	if (*error) {
		nd_playcatalog_free(c);
		return NULL;
	}
	return c;
}

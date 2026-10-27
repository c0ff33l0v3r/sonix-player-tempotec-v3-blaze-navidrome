#include "sessionmusic.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void session_music_free(session_music_t *m) {
	if (!m)
		return;
	for (int i = 0; i < m->count; i++)
		free(m->paths ? m->paths[i] : NULL);
	free(m->paths);
	free(m->slots);
	free(m);
}
session_music_t *session_music_capture(const char *selected, double position) {
	if (playlist_has_source() || playlist_count() <= 0 || !selected || !*selected)
		return NULL;
	session_music_t *m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	snprintf(m->selected, sizeof(m->selected), "%s", selected);
	m->position = position;
	m->custom = playlist_is_custom();
	snprintf(m->album, sizeof(m->album), "%s", playlist_album());
	bool query = playlist_library_spec(&m->query);
	m->current = query ? (int)playlist_current_entry() : playlist_current_index();
	m->count = query ? playlist_appended_count() : playlist_count();
	m->paths = calloc((size_t)m->count + 1, sizeof(*m->paths));
	m->slots = calloc((size_t)m->count + 1, sizeof(*m->slots));
	if (!m->paths || !m->slots)
		goto fail;
	if (query)
		playlist_appended_slots(m->slots, m->count);
	for (int i = 0; i < m->count; i++) {
		char path[512];
		if (!(query ? playlist_appended_at(i, path, sizeof(path)) : playlist_path_at(i, path, sizeof(path))))
			goto fail;
		m->paths[i] = strdup(path);
		if (!m->paths[i])
			goto fail;
	}
	return m;
fail:
	session_music_free(m);
	return NULL;
}
static bool exec(sqlite3 *db, const char *sql) { return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK; }
static bool local_path(const char *root, const char *path) {
	size_t n = strlen(root);
	while (n && root[n - 1] == '/')
		n--;
	return n && strlen(path) > n && !strncmp(root, path, n) && path[n] == '/' && session_relative_path(path + n + 1);
}
static bool database(const char *root, bool write, sqlite3 **db) {
	char path[1200];
	if (!library_session_db_path(path, sizeof(path)))
		return false;
	size_t n = strlen(root);
	while (n && root[n - 1] == '/')
		n--;
	if (!n || strncmp(path, root, n) || path[n] != '/')
		return false;
	if (sqlite3_open_v2(path, db, (write ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY) | SQLITE_OPEN_NOFOLLOW, NULL) != SQLITE_OK)
		return false;
	sqlite3_busy_timeout(*db, 250);
	return exec(*db, "PRAGMA trusted_schema=OFF; PRAGMA cache_size=-64; PRAGMA mmap_size=0;");
}
bool session_music_write(const session_header_t *h, const session_music_t *m, const char *root) {
	size_t bytes = 1024;
	for (int i = 0; m && i < m->count; i++) {
		if (!m->paths || !m->paths[i] || bytes > SESSION_BYTES - strlen(m->paths[i]) - 64)
			return false;
		bytes += strlen(m->paths[i]) + 64;
	}
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	bool ok = m && isfinite(m->position) && m->position >= 0 && database(root, true, &db) &&
			  exec(db, "BEGIN IMMEDIATE; CREATE TABLE IF NOT EXISTS SESSION_STATE(a INTEGER,r INTEGER,kind INT,filter INT,ord INT,desc INT,value TEXT,current INT,custom INT,album TEXT,path TEXT,position REAL,count INT,PRIMARY KEY(a,r));"
					   "CREATE TABLE IF NOT EXISTS SESSION_QUEUE(a INTEGER,r INTEGER,idx INT,path TEXT,slot INT,PRIMARY KEY(a,r,idx));");
	if (ok)
		ok = sqlite3_prepare_v2(db, "INSERT INTO SESSION_STATE VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK;
	if (ok) {
		sqlite3_bind_int64(st, 1, h->activation);
		sqlite3_bind_int64(st, 2, h->revision);
		sqlite3_bind_int(st, 3, m->query.valid ? (int)m->query.kind : -1);
		sqlite3_bind_int(st, 4, m->query.filter);
		sqlite3_bind_int(st, 5, m->query.order);
		sqlite3_bind_int(st, 6, m->query.desc);
		sqlite3_bind_text(st, 7, m->query.value, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(st, 8, m->current);
		sqlite3_bind_int(st, 9, m->custom);
		sqlite3_bind_text(st, 10, m->album, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 11, m->selected, -1, SQLITE_TRANSIENT);
		sqlite3_bind_double(st, 12, m->position);
		sqlite3_bind_int(st, 13, m->count);
		ok = sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	st = NULL;
	if (ok)
		ok = sqlite3_prepare_v2(db, "INSERT INTO SESSION_QUEUE VALUES(?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK;
	for (int i = 0; ok && i < m->count; i++) {
		sqlite3_bind_int64(st, 1, h->activation);
		sqlite3_bind_int64(st, 2, h->revision);
		sqlite3_bind_int(st, 3, i);
		sqlite3_bind_text(st, 4, m->paths[i], -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(st, 5, m->slots[i]);
		ok = sqlite3_step(st) == SQLITE_DONE;
		sqlite3_reset(st);
	}
	sqlite3_finalize(st);
	if (ok)
		ok = exec(db, "COMMIT;");
	if (db) {
		if (!ok)
			exec(db, "ROLLBACK;");
		sqlite3_close(db);
	}
	return ok;
}
static bool text(sqlite3_stmt *st, int c, char *out, size_t size) {
	if (sqlite3_column_type(st, c) != SQLITE_TEXT)
		return false;
	const char *s = (const char *)sqlite3_column_text(st, c);
	int n = sqlite3_column_bytes(st, c);
	if (!s || n < 0 || (size_t)n >= size || strlen(s) != (size_t)n)
		return false;
	memcpy(out, s, (size_t)n + 1);
	return true;
}
session_music_t *session_music_read(const session_header_t *h, const char *root) {
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	session_music_t *m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	bool ok = database(root, false, &db) && sqlite3_prepare_v2(db, "SELECT kind,filter,ord,desc,value,current,custom,album,path,position,count FROM SESSION_STATE WHERE a=? AND r=?", -1, &st, NULL) == SQLITE_OK;
	if (ok) {
		sqlite3_bind_int64(st, 1, h->activation);
		sqlite3_bind_int64(st, 2, h->revision);
		ok = sqlite3_step(st) == SQLITE_ROW;
	}
	if (ok) {
		m->query.valid = sqlite3_column_int(st, 0) >= 0;
		m->query.kind = sqlite3_column_int(st, 0);
		m->query.filter = sqlite3_column_int(st, 1);
		m->query.order = sqlite3_column_int(st, 2);
		m->query.desc = sqlite3_column_int(st, 3) != 0;
		ok = text(st, 4, m->query.value, sizeof(m->query.value)) && text(st, 7, m->album, sizeof(m->album)) && text(st, 8, m->selected, sizeof(m->selected));
		m->current = sqlite3_column_int(st, 5);
		m->custom = sqlite3_column_int(st, 6) != 0;
		m->position = h->position;
		m->count = sqlite3_column_int(st, 10);
		ok = ok && m->count >= 0 && m->count <= SESSION_BYTES / 8 && m->current >= 0 && isfinite(m->position) && m->position >= 0 && local_path(root, m->selected) && access(m->selected, R_OK) == 0 && (!m->query.valid || (m->query.kind <= LIBRARY_LIST_MOST_PLAYED && m->query.filter >= 0 && m->query.filter <= LIBRARY_FILTER_YEAR && m->query.order >= 0 && m->query.order <= LIBRARY_ORDER_YEAR_DESC)) && (m->query.valid || (m->count > 0 && m->current < m->count));
	}
	sqlite3_finalize(st);
	st = NULL;
	if (ok) {
		m->paths = calloc((size_t)m->count + 1, sizeof(*m->paths));
		m->slots = calloc((size_t)m->count + 1, sizeof(*m->slots));
		ok = m->paths && m->slots;
	}
	if (ok)
		ok = sqlite3_prepare_v2(db, "SELECT idx,path,slot FROM SESSION_QUEUE WHERE a=? AND r=? ORDER BY idx", -1, &st, NULL) == SQLITE_OK;
	if (ok) {
		sqlite3_bind_int64(st, 1, h->activation);
		sqlite3_bind_int64(st, 2, h->revision);
	}
	for (int i = 0; ok && i < m->count; i++) {
		char path[512];
		ok = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == i && text(st, 1, path, sizeof(path)) && local_path(root, path);
		if (ok) {
			m->paths[i] = strdup(path);
			m->slots[i] = sqlite3_column_int(st, 2);
			ok = m->paths[i] != NULL;
		}
	}
	if (ok)
		ok = sqlite3_step(st) == SQLITE_DONE;
	if (ok && !m->query.valid)
		ok = !strcmp(m->paths[m->current], m->selected);
	sqlite3_finalize(st);
	if (db)
		sqlite3_close(db);
	if (!ok) {
		session_music_free(m);
		return NULL;
	}
	return m;
}

void session_music_prune(const session_header_t *keep, const char *root) {
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	if (!database(root, true, &db)) {
		if (db)
			sqlite3_close(db);
		return;
	}
	const char *tables[] = {"SESSION_QUEUE", "SESSION_STATE"};
	for (int i = 0; i < 2; i++) {
		char sql[180];
		snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE NOT(a=? AND r=?)", tables[i]);
		if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
			sqlite3_bind_int64(st, 1, keep && keep->owner == SESSION_MUSIC && keep->ready ? keep->activation : -1);
			sqlite3_bind_int64(st, 2, keep && keep->owner == SESSION_MUSIC && keep->ready ? keep->revision : -1);
			sqlite3_step(st);
		}
		sqlite3_finalize(st);
		st = NULL;
	}
	sqlite3_close(db);
}

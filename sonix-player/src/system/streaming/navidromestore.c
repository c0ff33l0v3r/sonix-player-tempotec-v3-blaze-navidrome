#include "navidromestore.h"
#include "src/system/library/library.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool sql(nd_store_t *s, const char *text) {
	int rc = sqlite3_exec(s->db, text, NULL, NULL, NULL);
	if (rc != SQLITE_OK)
		s->error = "nd_cache_error";
	return rc == SQLITE_OK;
}

static bool prepare(nd_store_t *s, sqlite3_stmt **stmt, const char *text) {
	if (sqlite3_prepare_v2(s->db, text, -1, stmt, NULL) == SQLITE_OK)
		return true;
	s->error = "nd_cache_error";
	return false;
}

static void bind(sqlite3_stmt *st, int col, const char *text) { sqlite3_bind_text(st, col, text, -1, SQLITE_TRANSIENT); }

bool nd_store_open(nd_store_t *s, const char *path, const char *scope, bool write, bool rebuild) {
	memset(s, 0, sizeof(*s));
	snprintf(s->scope, sizeof(s->scope), "%s", scope);
	int flags = write ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READONLY;
	if (sqlite3_open_v2(path, &s->db, flags, NULL) != SQLITE_OK) {
		s->error = "nd_cache_error";
		nd_store_close(s);
		return false;
	}
	sqlite3_busy_timeout(s->db, 1000);
	if (!sql(s, "PRAGMA cache_size=-256; PRAGMA temp_store=FILE; PRAGMA foreign_keys=ON;"))
		return false;
	sqlite3_stmt *st = NULL;
	if (!prepare(s, &st, "PRAGMA user_version"))
		return false;
	int version = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
	sqlite3_finalize(st);
	if ((version != 1 && (!write || version != 0)) && !rebuild) {
		s->error = "nd_cache_error";
		return false;
	}
	if (write && version != 1) {
		if (!sql(s, "BEGIN; DROP TABLE IF EXISTS active; DROP TABLE IF EXISTS rel;"
					"DROP TABLE IF EXISTS entity; DROP TABLE IF EXISTS snapshot;"
					"CREATE TABLE snapshot(id INTEGER PRIMARY KEY,scope TEXT NOT NULL,complete INT NOT NULL DEFAULT 0,finished INT);"
					"CREATE TABLE active(scope TEXT PRIMARY KEY,gen INT NOT NULL);"
					"CREATE TABLE entity(gen INT REFERENCES snapshot(id) ON DELETE CASCADE,kind INT,id TEXT,title TEXT,artist TEXT,"
					"album TEXT,cover TEXT,duration INT,disc INT,track INT,sortkey TEXT,PRIMARY KEY(gen,kind,id));"
					"CREATE TABLE rel(gen INT REFERENCES snapshot(id) ON DELETE CASCADE,album TEXT,kind INT,id TEXT,PRIMARY KEY(gen,album,kind,id));"
					"CREATE INDEX names ON entity(gen,kind,sortkey,id);"
					"CREATE INDEX album_tracks ON entity(gen,kind,album,disc,track,id);"
					"CREATE INDEX memberships ON rel(gen,kind,id,album); PRAGMA user_version=1; COMMIT;")) {
			sql(s, "ROLLBACK");
			return false;
		}
	}
	return true;
}

void nd_store_close(nd_store_t *s) {
	if (s->db)
		sqlite3_close(s->db);
	s->db = NULL;
}

bool nd_store_status(nd_store_t *s, nd_library_t *out) {
	sqlite3_stmt *st = NULL;
	if (!prepare(s, &st, "SELECT s.id,s.finished FROM active a JOIN snapshot s ON s.id=a.gen WHERE a.scope=? AND s.scope=? AND s.complete=1"))
		return false;
	bind(st, 1, s->scope);
	bind(st, 2, s->scope);
	int rc = sqlite3_step(st);
	out->ready = rc == SQLITE_ROW;
	out->generation = out->ready ? sqlite3_column_int64(st, 0) : 0;
	out->finished = out->ready ? (time_t)sqlite3_column_int64(st, 1) : 0;
	sqlite3_finalize(st);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE)
		return false;
	memset(out->counts, 0, sizeof(out->counts));
	if (!out->ready)
		return true;
	if (!prepare(s, &st, "SELECT kind,count(*) FROM entity WHERE gen=? GROUP BY kind"))
		return false;
	sqlite3_bind_int64(st, 1, out->generation);
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		int kind = sqlite3_column_int(st, 0);
		if (kind >= 0 && kind < 4)
			out->counts[kind] = sqlite3_column_int(st, 1);
	}
	sqlite3_finalize(st);
	return rc == SQLITE_DONE;
}

bool nd_store_begin(nd_store_t *s) {
	sqlite3_stmt *st = NULL;
	if (!prepare(s, &st, "INSERT INTO snapshot(scope) VALUES(?)"))
		return false;
	bind(st, 1, s->scope);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	if (ok)
		s->generation = sqlite3_last_insert_rowid(s->db);
	return ok;
}

// Reject identity truncation and escaped NULs rather than collapsing entities.
// Keys/reasons here are caller-owned constants, never metadata or server text.
static bool field_error(const char *key, const char *reason) {
	fprintf(stderr, "navidrome: sync field=%s reason=%s\n", key, reason);
	return false;
}

static bool string_field(const json_doc_t *d, int obj, const char *key, char *out, size_t cap, bool required, bool display) {
	int t = json_get(d, obj, key);
	out[0] = 0;
	if (t < 0 || d->toks[t].type == JSON_NULL)
		return !required || field_error(key, "missing_required");
	if (d->toks[t].type != JSON_STRING)
		return field_error(key, "wrong_type");
	if (!display && (size_t)(d->toks[t].end - d->toks[t].start) >= cap)
		return field_error(key, "field_limit");
	for (int i = d->toks[t].start; i + 6 <= d->toks[t].end; i++)
		if (!memcmp(d->text + i, "\\u0000", 6))
			return field_error(key, "escaped_nul");
	// Decode a little past the display boundary so that both raw UTF-8 and
	// escaped code points can be shortened to the same complete prefix.
	char text[ND_TEXT_MAX + 4];
	if (!json_str(d, t, display ? text : out, display ? sizeof(text) : cap))
		return field_error(key, "invalid_string");
	if (display) {
		size_t n = strlen(text);
		if (n >= cap) {
			n = cap - 1;
			while (n && ((unsigned char)text[n] & 0xc0) == 0x80)
				n--;
		}
		memcpy(out, text, n);
		out[n] = 0;
	}
	return !required || *out || field_error(key, "empty_required");
}

static bool field(const json_doc_t *d, int obj, const char *key, char *out, size_t cap, bool required) { return string_field(d, obj, key, out, cap, required, false); }

static bool label(const json_doc_t *d, int obj, const char *key, char out[ND_TEXT_MAX]) { return string_field(d, obj, key, out, ND_TEXT_MAX, false, true); }

static bool relation(nd_store_t *s, const char *album, nd_kind_t kind, const char *id) {
	if (!*album || !*id)
		return true;
	sqlite3_stmt *st = NULL;
	if (!prepare(s, &st, "INSERT OR IGNORE INTO rel VALUES(?,?,?,?)"))
		return false;
	sqlite3_bind_int64(st, 1, s->generation);
	bind(st, 2, album);
	sqlite3_bind_int(st, 3, kind);
	bind(st, 4, id);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	return ok;
}

static bool genre(nd_store_t *s, const char *album, const char *name) {
	if (!*name)
		return true;
	if (strlen(name) >= ND_ID_MAX)
		return false;
	sqlite3_stmt *st = NULL;
	if (!prepare(s, &st, "INSERT OR IGNORE INTO entity(gen,kind,id,title,artist,album,cover,duration,disc,track,sortkey) VALUES(?,3,?,?,'','','',0,0,0,?)"))
		return false;
	sqlite3_bind_int64(st, 1, s->generation);
	bind(st, 2, name);
	bind(st, 3, name);
	char key[LIBRARY_SORT_KEY_MAX];
	library_sort_key_ex(name, key, sizeof(key), true);
	bind(st, 4, key);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	return ok && relation(s, album, ND_GENRES, name);
}

static bool memberships(nd_store_t *s, const json_doc_t *d, int obj, const char *album) {
	char value[ND_ID_MAX];
	const char *arrays[] = {"artists", "albumArtists", "genres"};
	bool structured_artists = false, structured_genres = false;
	for (int a = 0; a < 3; a++) {
		int arr = json_get(d, obj, arrays[a]);
		if (arr < 0 || d->toks[arr].type == JSON_NULL)
			continue;
		if (d->toks[arr].type != JSON_ARRAY)
			return field_error(arrays[a], "wrong_type");
		if (a == 0)
			structured_artists = json_len(d, arr) > 0;
		if (a == 2)
			structured_genres = json_len(d, arr) > 0;
		for (int i = 0; i < json_len(d, arr); i++) {
			int item = json_at(d, arr, i);
			if (!field(d, item, a == 2 ? "name" : "id", value, sizeof(value), true))
				return false;
			if (!(a == 2 ? genre(s, album, value) : relation(s, album, ND_ARTISTS, value)))
				return false;
		}
	}
	// OpenSubsonic relationships are authoritative. The legacy primary ID or
	// joined genre label need not identify any of the structured entities.
	if (!structured_artists && (!field(d, obj, "artistId", value, sizeof(value), false) || !relation(s, album, ND_ARTISTS, value)))
		return false;
	if (structured_genres)
		return true;
	if (!field(d, obj, "genre", value, sizeof(value), false))
		return false;
	return genre(s, album, value);
}

bool nd_store_import(nd_store_t *s, nd_kind_t kind, const nd_document_t *document, int *count) {
	const json_doc_t *d = &document->doc;
	int result = json_get(d, document->response, kind == ND_GENRES ? "genres" : "searchResult3");
	if (result < 0 || d->toks[result].type != JSON_OBJECT)
		return field_error(kind == ND_GENRES ? "genres" : "searchResult3", "missing_object");
	static const char *keys[] = {"album", "song", "artist", "genre"};
	int arr = json_get(d, result, keys[kind]);
	if (arr >= 0 && d->toks[arr].type != JSON_ARRAY)
		return field_error(keys[kind], "wrong_type");
	*count = json_len(d, arr);
	if (!sql(s, "BEGIN"))
		return false;
	bool ok = true;
	sqlite3_stmt *st = NULL;
	if (!prepare(s, &st, "INSERT INTO entity VALUES(?,?,?,?,?,?,?,?,?,?,?)"))
		ok = false;
	for (int i = 0; ok && i < *count; i++) {
		int obj = json_at(d, arr, i);
		nd_row_t row = {0};
		char album[ND_ID_MAX] = "";
		if (kind == ND_GENRES) {
			ok = field(d, obj, "value", row.title, ND_ID_MAX, true) && genre(s, "", row.title);
			continue;
		}
		ok = field(d, obj, "id", row.id, sizeof(row.id), true) && label(d, obj, kind == ND_TRACKS ? "title" : "name", row.title) && label(d, obj, "artist", row.artist) && field(d, obj, "coverArt", row.cover, sizeof(row.cover), false);
		if (kind == ND_TRACKS)
			ok = ok && field(d, obj, "albumId", album, sizeof(album), true);
		else if (kind == ND_ALBUMS)
			snprintf(album, sizeof(album), "%s", row.id);
		if (!ok)
			break;
		if (!row.title[0])
			snprintf(row.title, sizeof(row.title), "%s", row.id);
		long duration = json_obj_long(d, obj, "duration", 0), disc = json_obj_long(d, obj, "discNumber", 0), track = json_obj_long(d, obj, "track", 0);
		if (duration < 0 || duration > INT_MAX || disc < 0 || disc > INT_MAX || track < 0 || track > INT_MAX) {
			field_error("duration/discNumber/track", "out_of_range");
			ok = false;
			break;
		}
		sqlite3_reset(st);
		sqlite3_clear_bindings(st);
		sqlite3_bind_int64(st, 1, s->generation);
		sqlite3_bind_int(st, 2, kind);
		bind(st, 3, row.id);
		bind(st, 4, row.title);
		bind(st, 5, row.artist);
		bind(st, 6, album);
		bind(st, 7, row.cover);
		sqlite3_bind_int(st, 8, duration);
		sqlite3_bind_int(st, 9, disc);
		sqlite3_bind_int(st, 10, track);
		char key[LIBRARY_SORT_KEY_MAX];
		library_sort_key_ex(row.title, key, sizeof(key), true);
		bind(st, 11, key);
		int rc = sqlite3_step(st);
		ok = rc == SQLITE_DONE && (kind == ND_ARTISTS || memberships(s, d, obj, album));
		if (rc != SQLITE_DONE) {
			fprintf(stderr, "navidrome: sync insert kind=%d item=%d reason=%s code=%d\n", kind, i, rc == SQLITE_CONSTRAINT ? "duplicate_entity" : "database_error", rc);
			s->error = rc == SQLITE_CONSTRAINT ? "nd_sync_inconsistent" : "nd_cache_error";
		}
	}
	sqlite3_finalize(st);
	if (ok && sql(s, "COMMIT"))
		return true;
	sql(s, "ROLLBACK");
	if (!s->error)
		s->error = "nd_sync_inconsistent";
	return false;
}

bool nd_store_validate(nd_store_t *s) {
	sqlite3_stmt *st = NULL;
	if (!prepare(s, &st,
				 "SELECT 1,1 FROM entity t WHERE t.gen=? AND t.kind=1 AND NOT EXISTS(SELECT 1 FROM entity a WHERE a.gen=t.gen AND a.kind=0 AND a.id=t.album)"
				 " UNION ALL SELECT CASE WHEN NOT EXISTS(SELECT 1 FROM entity a WHERE a.gen=r.gen AND a.kind=0 AND a.id=r.album) THEN 2 ELSE 3 END,r.kind FROM rel r WHERE r.gen=? AND (NOT EXISTS(SELECT 1 FROM entity a WHERE a.gen=r.gen AND a.kind=0 AND a.id=r.album) OR NOT EXISTS(SELECT 1 FROM entity e WHERE e.gen=r.gen AND e.kind=r.kind AND e.id=r.id)) LIMIT 1"))
		return false;
	sqlite3_bind_int64(st, 1, s->generation);
	sqlite3_bind_int64(st, 2, s->generation);
	int rc = sqlite3_step(st);
	bool ok = rc == SQLITE_DONE;
	if (rc == SQLITE_ROW) {
		int reason = sqlite3_column_int(st, 0);
		fprintf(stderr, "navidrome: sync validation reason=%s kind=%d\n", reason == 1 ? "missing_track_album" : reason == 2 ? "missing_relation_album" : "missing_relation_entity", sqlite3_column_int(st, 1));
	} else if (!ok)
		fprintf(stderr, "navidrome: sync validation reason=database_error code=%d\n", rc);
	sqlite3_finalize(st);
	if (!ok)
		s->error = rc == SQLITE_ROW ? "nd_sync_inconsistent" : "nd_cache_error";
	return ok;
}

bool nd_store_publish(nd_store_t *s) {
	if (!sql(s, "BEGIN IMMEDIATE"))
		return false;
	sqlite3_stmt *st = NULL;
	bool ok = prepare(s, &st, "UPDATE snapshot SET complete=1,finished=? WHERE id=? AND scope=?");
	if (ok) {
		sqlite3_bind_int64(st, 1, time(NULL));
		sqlite3_bind_int64(st, 2, s->generation);
		bind(st, 3, s->scope);
		ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(s->db) == 1;
	}
	sqlite3_finalize(st);
	st = NULL;
	if (ok)
		ok = prepare(s, &st, "INSERT OR REPLACE INTO active VALUES(?,?)");
	if (ok) {
		bind(st, 1, s->scope);
		sqlite3_bind_int64(st, 2, s->generation);
		ok = sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	if (ok && sql(s, "COMMIT"))
		return true;
	sql(s, "ROLLBACK");
	return false;
}

void nd_store_cleanup(nd_store_t *s) {
	// Readers use a read transaction, so SQLite protects any existing window.
	sql(s, "DELETE FROM snapshot WHERE id NOT IN (SELECT gen FROM active)");
}

bool nd_store_contains(nd_store_t *s, int64_t gen, nd_kind_t kind, const char *id) {
	sqlite3_stmt *st = NULL;
	if (!prepare(s, &st, "SELECT 1 FROM entity e JOIN snapshot s ON s.id=e.gen WHERE e.gen=? AND e.kind=? AND e.id=? AND s.scope=? AND s.complete=1"))
		return false;
	sqlite3_bind_int64(st, 1, gen);
	sqlite3_bind_int(st, 2, kind);
	bind(st, 3, id);
	bind(st, 4, s->scope);
	bool found = sqlite3_step(st) == SQLITE_ROW;
	sqlite3_finalize(st);
	return found;
}

bool nd_store_query(nd_store_t *s, int64_t gen, const nd_query_t *q, int offset, nd_row_t rows[ND_WINDOW], int *count, int *total) {
	*count = *total = 0;
	if (offset < 0 || !sql(s, "BEGIN"))
		return false;
	char where[768], text[1200];
	snprintf(where, sizeof(where), " FROM entity e JOIN snapshot s ON s.id=e.gen WHERE e.gen=?1 AND e.kind=?2 AND s.scope=?3 AND s.complete=1%s", !q->filter[0] ? "" : q->kind == ND_TRACKS ? " AND e.album=?4" : " AND EXISTS(SELECT 1 FROM rel r WHERE r.gen=e.gen AND r.album=e.id AND r.kind=?5 AND r.id=?4)");
	sqlite3_stmt *st = NULL;
	bool ok = true;
	for (int pass = 0; ok && pass < 2; pass++) {
		snprintf(text, sizeof(text), "%s%s%s", pass ? "SELECT e.id,e.title,e.artist,e.cover,e.duration,e.disc,e.track" : "SELECT count(*)", where, pass ? q->kind == ND_TRACKS && *q->filter ? " ORDER BY e.disc,e.track,e.sortkey,e.id LIMIT ?6 OFFSET ?7" : " ORDER BY e.sortkey,e.id LIMIT ?6 OFFSET ?7" : "");
		ok = prepare(s, &st, text);
		if (!ok)
			break;
		sqlite3_bind_int64(st, 1, gen);
		sqlite3_bind_int(st, 2, q->kind);
		bind(st, 3, s->scope);
		if (*q->filter) {
			bind(st, 4, q->filter);
			if (q->kind != ND_TRACKS)
				sqlite3_bind_int(st, 5, q->filter_kind);
		}
		if (pass) {
			sqlite3_bind_int(st, 6, ND_WINDOW);
			sqlite3_bind_int(st, 7, offset);
		}
		int rc;
		while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
			if (!pass) {
				*total = sqlite3_column_int(st, 0);
				continue;
			}
			if (*count >= ND_WINDOW) {
				ok = false;
				break;
			}
			nd_row_t *row = &rows[(*count)++];
			memset(row, 0, sizeof(*row));
			char *fields[] = {row->id, row->title, row->artist, row->cover};
			size_t caps[] = {sizeof(row->id), sizeof(row->title), sizeof(row->artist), sizeof(row->cover)};
			for (int c = 0; c < 4; c++) {
				const unsigned char *value = sqlite3_column_text(st, c);
				snprintf(fields[c], caps[c], "%s", value ? (const char *)value : "");
			}
			row->duration = sqlite3_column_int(st, 4);
			row->disc = sqlite3_column_int(st, 5);
			row->track = sqlite3_column_int(st, 6);
		}
		ok = ok && rc == SQLITE_DONE;
		sqlite3_finalize(st);
		st = NULL;
	}
	bool ended = sql(s, ok ? "COMMIT" : "ROLLBACK");
	return ok && ended;
}

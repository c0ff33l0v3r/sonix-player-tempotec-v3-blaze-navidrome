#include "navidromeplayliststore.h"
#include "src/system/core/sha256.h"
#include "src/system/library/library.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool playlist_sort_open(nd_playlist_store_t *s, bool write);
static bool playlist_view_read(nd_playlist_store_t *s, nd_playlist_t *p);
static bool playlist_sorted_rows(nd_playlist_store_t *s, int64_t id, int offset, int64_t session, nd_pl_sort_t sort, bool view, nd_playlist_entry_t *out, int limit, int *count);

bool nd_pl_sql(nd_playlist_store_t *s, const char *sql) {
	if (sqlite3_exec(s->db, sql, NULL, NULL, NULL) == SQLITE_OK)
		return true;
	s->error = "nd_pl_storage_error";
	return false;
}
bool nd_pl_prepare(nd_playlist_store_t *s, sqlite3_stmt **st, const char *sql) {
	if (sqlite3_prepare_v2(s->db, sql, -1, st, NULL) == SQLITE_OK)
		return true;
	s->error = "nd_pl_storage_error";
	return false;
}
void nd_pl_bind(sqlite3_stmt *st, int index, const char *value) { sqlite3_bind_text(st, index, value ? value : "", -1, SQLITE_TRANSIENT); }
static void text(sqlite3_stmt *st, int index, char *out, size_t size) {
	const char *value = (const char *)sqlite3_column_text(st, index);
	snprintf(out, size, "%s", value ? value : "");
}
bool nd_pl_path(const nd_library_t *lib, char out[1200]) {
	out[0] = 0;
	if (strlen(lib->scope) != 32 || strspn(lib->scope, "0123456789abcdef") != 32)
		return false;
	const char *slash = strrchr(lib->path, '/');
	if (!slash)
		return false;
	// User playlists belong beside the disposable cache, including when the
	// metadata reader is still using a read-only legacy library.db path.
	size_t parent = (size_t)(slash - lib->path);
	const char suffix[] = "/.cache";
	if (parent >= sizeof(suffix) - 1 && !memcmp(lib->path + parent - (sizeof(suffix) - 1), suffix, sizeof(suffix) - 1))
		parent -= sizeof(suffix) - 1;
	int n = snprintf(out, 1200, "%.*s/playlists-%s.db", (int)parent, lib->path, lib->scope);
	return n > 0 && n < 1200;
}
bool nd_pl_open(nd_playlist_store_t *s, const char *path, const char *scope, bool write) {
	memset(s, 0, sizeof(*s));
	s->error = "nd_pl_storage_error";
	if (!path || !*path || !scope || strlen(scope) != 32)
		return false;
	if (write) {
		char dir[1200];
		if (strlen(path) >= sizeof(dir))
			return false;
		snprintf(dir, sizeof(dir), "%s", path);
		for (char *p = dir + 1; *p; p++)
			if (*p == '/') {
				*p = 0;
				if (mkdir(dir, 0700) && errno != EEXIST)
					return false;
				*p = '/';
			}
	}
	snprintf(s->path, sizeof(s->path), "%s", path);
	snprintf(s->scope, sizeof(s->scope), "%s", scope);
	int flags = write ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READONLY;
	if (sqlite3_open_v2(path, &s->db, flags, NULL) != SQLITE_OK)
		return false;
	sqlite3_busy_timeout(s->db, 1000);
	if (!nd_pl_sql(s, "PRAGMA foreign_keys=ON; PRAGMA cache_size=-256; PRAGMA temp_store=FILE; PRAGMA synchronous=FULL;"))
		return false;
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st, "PRAGMA user_version"))
		return false;
	int version = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
	sqlite3_finalize(st);
	if (version == 0 && write) {
		// Only a genuinely empty store can be initialized. Never rebuild user data.
		if (!nd_pl_prepare(s, &st, "SELECT count(*) FROM sqlite_master WHERE type='table'"))
			return false;
		bool empty = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 0;
		sqlite3_finalize(st);
		if (!empty || !nd_pl_sql(s, "BEGIN IMMEDIATE; CREATE TABLE identity(scope TEXT NOT NULL);"
									"CREATE TABLE playlist(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL,rev INT NOT NULL DEFAULT 1,"
									"remote TEXT UNIQUE,baseline TEXT NOT NULL DEFAULT '',synced INT DEFAULT 0,pending INT DEFAULT 0,desired TEXT DEFAULT '',readonly INT DEFAULT 0,changed TEXT DEFAULT '',operation INTEGER DEFAULT 0,intent_name TEXT DEFAULT '');"
									"CREATE TABLE entry(id INTEGER PRIMARY KEY AUTOINCREMENT,p INT NOT NULL REFERENCES playlist(id) ON DELETE CASCADE,"
									"ordinal INTEGER NOT NULL,track TEXT NOT NULL,title TEXT,artist TEXT,cover TEXT,duration INT,UNIQUE(p,ordinal));"
									"CREATE INDEX membership ON entry(p,track);"
									"CREATE TABLE selection(session INTEGER,ordinal INTEGER,kind INT,track TEXT,occurrence INTEGER,PRIMARY KEY(session,ordinal));"
									"CREATE TABLE catalogue(ordinal INTEGER PRIMARY KEY,remote TEXT UNIQUE,name TEXT,readonly INT);"
									"PRAGMA user_version=1;"))
			return false;
		if (!nd_pl_prepare(s, &st, "INSERT INTO identity VALUES(?)"))
			return false;
		nd_pl_bind(st, 1, scope);
		bool ok = sqlite3_step(st) == SQLITE_DONE;
		sqlite3_finalize(st);
		if (!ok || !nd_pl_sql(s, "COMMIT"))
			return false;
	} else if (version != 1)
		return false;
	if (!nd_pl_prepare(s, &st, "SELECT scope FROM identity"))
		return false;
	bool ok = sqlite3_step(st) == SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(st, 0), scope) && sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	if (ok)
		ok = playlist_sort_open(s, write);
	if (ok)
		s->error = NULL;
	return ok;
}
void nd_pl_close(nd_playlist_store_t *s) {
	if (s->db) {
		sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
		sqlite3_close(s->db);
	}
	s->db = NULL;
}
static void hash_field(sha256_ctx *hash, const char *value) {
	uint64_t n = strlen(value);
	unsigned char length[8];
	for (int i = 0; i < 8; i++)
		length[7 - i] = (unsigned char)(n >> (i * 8));
	sha256_update(hash, length, sizeof(length));
	sha256_update(hash, value, (size_t)n);
}
bool nd_pl_fingerprint(nd_playlist_store_t *s, int64_t id, char out[ND_PL_HASH]) {
	sha256_ctx hash;
	sha256_init(&hash);
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st, "SELECT name FROM playlist WHERE id=?"))
		return false;
	sqlite3_bind_int64(st, 1, id);
	bool ok = sqlite3_step(st) == SQLITE_ROW;
	if (ok)
		hash_field(&hash, (const char *)sqlite3_column_text(st, 0));
	sqlite3_finalize(st);
	if (!ok || !nd_pl_prepare(s, &st, "SELECT track FROM entry WHERE p=? ORDER BY ordinal"))
		return false;
	sqlite3_bind_int64(st, 1, id);
	int rc;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW)
		hash_field(&hash, (const char *)sqlite3_column_text(st, 0));
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return false;
	sha256_final_hex(&hash, out);
	return true;
}
bool nd_pl_get(nd_playlist_store_t *s, int64_t id, nd_playlist_t *out) {
	memset(out, 0, sizeof(*out));
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st, "SELECT id,rev,name,remote,baseline,synced,pending,desired,readonly,changed,operation,intent_name,(SELECT count(*) FROM entry WHERE p=playlist.id) FROM playlist WHERE id=?"))
		return false;
	sqlite3_bind_int64(st, 1, id);
	bool ok = sqlite3_step(st) == SQLITE_ROW;
	if (ok) {
		out->id = sqlite3_column_int64(st, 0);
		out->revision = sqlite3_column_int64(st, 1);
		text(st, 2, out->name, sizeof(out->name));
		text(st, 3, out->remote, sizeof(out->remote));
		text(st, 4, out->baseline, sizeof(out->baseline));
		out->synced = sqlite3_column_int64(st, 5);
		out->pending = sqlite3_column_int(st, 6);
		text(st, 7, out->desired, sizeof(out->desired));
		out->readonly = sqlite3_column_int(st, 8);
		text(st, 9, out->changed, sizeof(out->changed));
		out->operation = sqlite3_column_int64(st, 10);
		text(st, 11, out->intent_name, sizeof(out->intent_name));
		out->count = sqlite3_column_int(st, 12);
	}
	sqlite3_finalize(st);
	char hash[ND_PL_HASH];
	if (ok)
		ok = nd_pl_fingerprint(s, id, hash);
	if (ok)
		out->dirty = *out->remote && strcmp(hash, out->baseline);
	if (ok)
		ok = playlist_view_read(s, out);
	if (!ok && !s->error)
		s->error = "nd_pl_stale";
	return ok;
}
bool nd_pl_list(nd_playlist_store_t *s, int offset, nd_playlist_t out[ND_WINDOW], int *count, int *total) {
	*count = *total = 0;
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st, "SELECT count(*) FROM playlist WHERE id>0"))
		return false;
	if (sqlite3_step(st) != SQLITE_ROW) {
		sqlite3_finalize(st);
		return false;
	}
	*total = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	if (!nd_pl_prepare(s, &st, "SELECT id FROM playlist WHERE id>0 ORDER BY name COLLATE NOCASE,id LIMIT 16 OFFSET ?"))
		return false;
	sqlite3_bind_int(st, 1, offset);
	int rc;
	bool ok = true;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW && ok)
		ok = nd_pl_get(s, sqlite3_column_int64(st, 0), &out[(*count)++]);
	sqlite3_finalize(st);
	return ok && rc == SQLITE_DONE;
}

bool nd_pl_entries(nd_playlist_store_t *s, int64_t id, int offset, int64_t session, nd_playlist_entry_t out[ND_WINDOW], int *count) { return playlist_sorted_rows(s, id, offset, session, ND_PL_ORIGINAL, false, out, ND_WINDOW, count); }

static bool play_rows(nd_playlist_store_t *s, int64_t id, int64_t revision, int first,
		nd_playlist_entry_t *out, int limit, int *count, int *total) {
	*count = *total = 0;
	if (id <= 0 || revision <= 0 || first < 0 || !nd_pl_sql(s, "BEGIN"))
		return false;
	sqlite3_stmt *st = NULL;
	bool ok = nd_pl_prepare(s, &st, "SELECT rev,(SELECT count(*) FROM entry WHERE p=playlist.id) FROM playlist WHERE id=?");
	if (ok) {
		sqlite3_bind_int64(st, 1, id);
		ok = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int64(st, 0) == revision;
		if (!ok)
			s->error = "nd_pl_stale";
		else {
			int64_t size = sqlite3_column_int64(st, 1);
			ok = size >= 0 && size <= INT_MAX && first <= size;
			if (ok) *total = (int)size;
		}
	}
	sqlite3_finalize(st);
	if (ok)
		ok = playlist_sorted_rows(s, id, first, 0, ND_PL_ORIGINAL, false, out, limit, count) && *count == (*total - first < limit ? *total - first : limit);
	if (ok) ok = nd_pl_sql(s, "COMMIT");
	if (!ok) sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
	if (!ok && !s->error) s->error = "nd_pl_storage_error";
	return ok;
}

bool nd_pl_play_window(nd_playlist_store_t *s, int64_t id, int64_t revision, int first,
		nd_playlist_entry_t out[ND_WINDOW], int *count, int *total) {
	return play_rows(s, id, revision, first, out, ND_WINDOW, count, total);
}
bool nd_pl_play_entry(nd_playlist_store_t *s, int64_t id, int64_t revision, int ordinal,
		int64_t occurrence, const char *track, nd_playlist_entry_t *out, int *total) {
	int count;
	bool ok = track && *track && occurrence > 0 &&
		play_rows(s, id, revision, ordinal, out, 1, &count, total) && count == 1 &&
		out->occurrence == occurrence && !strcmp(out->track.id, track);
	if (!ok && !s->error) s->error = "nd_pl_stale";
	return ok;
}

bool nd_pl_name_valid(const char *name) {
	if (!name || !*name || strlen(name) >= ND_PL_NAME)
		return false;
	int characters = 0;
	bool ink = false;
	for (const unsigned char *p = (const unsigned char *)name; *p;) {
		unsigned cp;
		int n;
		if (*p < 0x80) {
			cp = *p;
			n = 1;
		} else if (*p >= 0xc2 && *p <= 0xdf) {
			cp = *p & 31;
			n = 2;
		} else if (*p >= 0xe0 && *p <= 0xef) {
			cp = *p & 15;
			n = 3;
		} else if (*p >= 0xf0 && *p <= 0xf4) {
			cp = *p & 7;
			n = 4;
		} else
			return false;
		for (int i = 1; i < n; i++) {
			if ((p[i] & 0xc0) != 0x80)
				return false;
			cp = (cp << 6) | (p[i] & 63);
		}
		if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || cp < 32 || cp == 127)
			return false;
		bool space = cp == 32 || cp == 0xa0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 || cp == 0x202f || cp == 0x205f || cp == 0x3000;
		ink |= !space;
		if (++characters > 200)
			return false;
		p += n;
	}
	return ink;
}
bool nd_pl_create(nd_playlist_store_t *s, const char *name, int64_t *id) {
	if (!nd_pl_name_valid(name)) {
		s->error = "nd_pl_invalid_name";
		return false;
	}
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st, "INSERT INTO playlist(name) VALUES(?)"))
		return false;
	nd_pl_bind(st, 1, name);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	if (ok)
		*id = sqlite3_last_insert_rowid(s->db);
	return ok;
}
bool nd_pl_begin(nd_playlist_store_t *s, int64_t id, int64_t rev) {
	if (!nd_pl_sql(s, "BEGIN IMMEDIATE"))
		return false;
	nd_playlist_t p;
	if (!nd_pl_get(s, id, &p) || p.revision != rev) {
		s->error = "nd_pl_stale";
		nd_pl_sql(s, "ROLLBACK");
		return false;
	}
	return true;
}
bool nd_pl_move(nd_playlist_store_t *s, int64_t id, int64_t revision,
				int from, int to, int64_t occurrence, const char *track, bool *changed) {
	*changed = false;
	if (!nd_pl_begin(s, id, revision))
		return false;
	sqlite3_stmt *st = NULL;
	nd_playlist_t p;
	bool ok = nd_pl_get(s, id, &p) && from >= 0 && to >= 0 && from < p.count && to < p.count && track;
	if (ok) ok = nd_pl_prepare(s, &st, "SELECT id,track FROM entry WHERE p=? ORDER BY ordinal LIMIT 1 OFFSET ?");
	if (ok) {
		sqlite3_bind_int64(st, 1, id);
		sqlite3_bind_int(st, 2, from);
		ok = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int64(st, 0) == occurrence &&
			!strcmp((const char *)sqlite3_column_text(st, 1), track);
	}
	sqlite3_finalize(st);
	st = NULL;
	if (!ok) s->error = "nd_pl_stale";
	if (ok && from != to) {
		// temp_store=FILE and the small SQLite cache bound mapping memory. All
		// temporary ordinals lie below the existing minimum, avoiding UNIQUE
		// collisions without deleting/recreating stable occurrence identities.
		int64_t low = 0;
		ok = nd_pl_prepare(s, &st, "SELECT min(ordinal) FROM entry WHERE p=?");
		if (ok) {
			sqlite3_bind_int64(st, 1, id);
			ok = sqlite3_step(st) == SQLITE_ROW;
			if (ok) low = sqlite3_column_int64(st, 0);
		}
		sqlite3_finalize(st);
		st = NULL;
		if (ok && low < INT64_MIN + (int64_t)p.count) {
			s->error = "nd_pl_storage_error";
			ok = false;
		}
		if (ok) ok = nd_pl_sql(s, "PRAGMA temp.cache_size=-256; CREATE TEMP TABLE move_positions(id INTEGER PRIMARY KEY,stage INTEGER,target INTEGER)");
		if (ok) ok = nd_pl_prepare(s, &st,
			"INSERT INTO move_positions SELECT id,?2-n,CASE WHEN n=?3 THEN ?4 WHEN ?3<?4 AND n>?3 AND n<=?4 THEN n-1 WHEN ?3>?4 AND n>=?4 AND n<?3 THEN n+1 ELSE n END "
			"FROM (SELECT id,row_number() OVER(ORDER BY ordinal)-1 AS n FROM entry WHERE p=?1)");
		if (ok) {
			sqlite3_bind_int64(st, 1, id);
			sqlite3_bind_int64(st, 2, (low < 0 ? low : 0) - 1);
			sqlite3_bind_int(st, 3, from);
			sqlite3_bind_int(st, 4, to);
			ok = sqlite3_step(st) == SQLITE_DONE;
		}
		sqlite3_finalize(st);
		st = NULL;
		const char *updates[] = {
			"UPDATE entry SET ordinal=(SELECT stage FROM move_positions WHERE id=entry.id) WHERE p=?",
			"UPDATE entry SET ordinal=(SELECT target FROM move_positions WHERE id=entry.id) WHERE p=?"};
		for (unsigned i = 0; ok && i < 2; i++) {
			ok = nd_pl_prepare(s, &st, updates[i]);
			if (ok) {
				sqlite3_bind_int64(st, 1, id);
				ok = sqlite3_step(st) == SQLITE_DONE;
			}
			sqlite3_finalize(st);
			st = NULL;
		}
		if (ok) ok = nd_pl_sql(s, "DROP TABLE move_positions") && nd_pl_finish(s, id, true);
		*changed = ok;
	}
	if (!ok) {
		sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
		if (!s->error) s->error = "nd_pl_storage_error";
	}
	return ok;
}
bool nd_pl_finish(nd_playlist_store_t *s, int64_t id, bool changed) {
	sqlite3_stmt *st = NULL;
	bool ok = nd_pl_prepare(s, &st, "UPDATE playlist SET rev=rev+? WHERE id=?");
	if (ok) {
		sqlite3_bind_int(st, 1, changed);
		sqlite3_bind_int64(st, 2, id);
		ok = sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	return ok;
}
bool nd_pl_edit(nd_playlist_store_t *s, int64_t id, int64_t rev, const char *name, bool remove) {
	if (!remove && !nd_pl_name_valid(name)) {
		s->error = "nd_pl_invalid_name";
		return false;
	}
	if (!nd_pl_begin(s, id, rev))
		return false;
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st, remove ? "DELETE FROM playlist WHERE id=?" : "UPDATE playlist SET rev=rev+(name<>?2),name=?2 WHERE id=?1"))
		return false;
	sqlite3_bind_int64(st, 1, id);
	if (!remove)
		nd_pl_bind(st, 2, name);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	return ok;
}
bool nd_pl_append(nd_playlist_store_t *s, int64_t id, const nd_row_t *r, bool dedupe, int *added, int *skipped) {
	if (!*r->id || strlen(r->id) >= ND_ID_MAX)
		return false;
	sqlite3_stmt *st = NULL;
	if (dedupe) {
		if (!nd_pl_prepare(s, &st, "SELECT 1 FROM entry WHERE p=? AND track=?"))
			return false;
		sqlite3_bind_int64(st, 1, id);
		nd_pl_bind(st, 2, r->id);
		int rc = sqlite3_step(st);
		sqlite3_finalize(st);
		if (rc == SQLITE_ROW) {
			(*skipped)++;
			return true;
		}
		if (rc != SQLITE_DONE)
			return false;
	}
	if (!nd_pl_prepare(s, &st, "INSERT INTO entry(p,ordinal,track,title,artist,cover,duration) VALUES(?1,(SELECT coalesce(max(ordinal),-1)+1 FROM entry WHERE p=?1),?2,?3,?4,?5,?6)"))
		return false;
	sqlite3_bind_int64(st, 1, id);
	nd_pl_bind(st, 2, r->id);
	nd_pl_bind(st, 3, r->title);
	nd_pl_bind(st, 4, r->artist);
	nd_pl_bind(st, 5, r->cover);
	sqlite3_bind_int(st, 6, r->duration);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	if (ok) {
		nd_playrow_t info = {.row = *r};
		ok = nd_pl_sort_info(s, sqlite3_last_insert_rowid(s->db), &info);
		if (ok)
			(*added)++;
	}
	return ok;
}
bool nd_pl_copy(nd_playlist_store_t *s, int64_t id, const char *name, int64_t *copy) {
	nd_playlist_t p;
	if (!nd_pl_get(s, id, &p) || !nd_pl_sql(s, "BEGIN IMMEDIATE") || !nd_pl_create(s, name && *name ? name : p.name, copy))
		return false;
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st, "INSERT INTO entry(p,ordinal,track,title,artist,cover,duration) SELECT ?,ordinal,track,title,artist,cover,duration FROM entry WHERE p=? ORDER BY ordinal"))
		return false;
	sqlite3_bind_int64(st, 1, *copy);
	sqlite3_bind_int64(st, 2, id);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	return ok && nd_pl_sort_copy(s, id, *copy);
}
bool nd_pl_baseline(nd_playlist_store_t *s, int64_t id, const char *remote, const char *hash, bool readonly, const char *changed) {
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st, "UPDATE playlist SET remote=?,baseline=?,synced=?,pending=0,desired='',intent_name='',readonly=?,changed=? WHERE id=?"))
		return false;
	nd_pl_bind(st, 1, remote);
	nd_pl_bind(st, 2, hash);
	sqlite3_bind_int64(st, 3, time(NULL));
	sqlite3_bind_int(st, 4, readonly);
	nd_pl_bind(st, 5, changed);
	sqlite3_bind_int64(st, 6, id);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	return ok;
}
bool nd_pl_field(const json_doc_t *d, int obj, const char *key, char *out, size_t cap, bool required, bool display) {
	out[0] = 0;
	int t = json_get(d, obj, key);
	if (t < 0 || d->toks[t].type == JSON_NULL)
		return !required;
	if (d->toks[t].type != JSON_STRING)
		return false;
	for (int i = d->toks[t].start; i + 6 <= d->toks[t].end; i++)
		if (!memcmp(d->text + i, "\\u0000", 6))
			return false;
	if (display) {
		char decoded[ND_PL_NAME + 4];
		if (cap > ND_PL_NAME || !json_str(d, t, decoded, sizeof(decoded)))
			return false;
		size_t n = strlen(decoded);
		if (n >= cap) {
			n = cap - 1;
			while (n && ((unsigned char)decoded[n] & 0xc0) == 0x80)
				n--;
		}
		memcpy(out, decoded, n);
		out[n] = 0;
	} else {
		char decoded[ND_PL_NAME + 4];
		if (cap > ND_PL_NAME || !json_str(d, t, decoded, sizeof(decoded)) || strlen(decoded) >= cap)
			return false;
		memcpy(out, decoded, strlen(decoded) + 1);
	}
	return !required || *out;
}
bool nd_pl_decode(nd_playlist_store_t *s, const nd_document_t *doc, int64_t *id, nd_playlist_t *remote) {
	const json_doc_t *d = &doc->doc;
	int obj = json_get(d, doc->response, "playlist");
	memset(remote, 0, sizeof(*remote));
	if (obj < 0 || d->toks[obj].type != JSON_OBJECT || !nd_pl_field(d, obj, "id", remote->remote, sizeof(remote->remote), true, false) || !nd_pl_field(d, obj, "name", remote->name, sizeof(remote->name), true, false) || !nd_pl_name_valid(remote->name))
		return false;
	remote->readonly = json_obj_bool(d, obj, "readonly", false);
	if (!nd_pl_field(d, obj, "changed", remote->changed, sizeof(remote->changed), false, false))
		return false;
	int entries = json_get(d, obj, "entry");
	if (entries >= 0 && d->toks[entries].type != JSON_ARRAY)
		return false;
	long expected = json_obj_long(d, obj, "songCount", -1);
	if (expected < 0 || expected != json_len(d, entries))
		return false;
	if (!nd_pl_create(s, remote->name, id))
		return false;
	// Negative IDs are private candidates; normal lists never expose them.
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st, "UPDATE playlist SET id=-id,readonly=?2,changed=?3 WHERE id=?1"))
		return false;
	sqlite3_bind_int64(st, 1, *id);
	sqlite3_bind_int(st, 2, remote->readonly);
	nd_pl_bind(st, 3, remote->changed);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	*id = -*id;
	int added = 0, skipped = 0;
	for (int t = entries + 1; entries >= 0 && t < d->count && d->toks[t].start < d->toks[entries].end && ok; t++) {
		if (d->toks[t].parent != entries)
			continue;
		nd_row_t row = {0};
		ok = d->toks[t].type == JSON_OBJECT && nd_pl_field(d, t, "id", row.id, sizeof(row.id), true, false) && nd_pl_field(d, t, "title", row.title, sizeof(row.title), false, true) && nd_pl_field(d, t, "artist", row.artist, sizeof(row.artist), false, true) && nd_pl_field(d, t, "coverArt", row.cover, sizeof(row.cover), false, false);
		if (!*row.title)
			snprintf(row.title, sizeof(row.title), "%s", row.id);
		row.duration = json_obj_long(d, t, "duration", 0);
		if (ok) {
			nd_playrow_t info = {.row = row};
			nd_pl_field(d, t, "albumId", info.album_id, sizeof(info.album_id), false, false);
			nd_pl_field(d, t, "album", info.album, sizeof(info.album), false, true);
			if (!nd_pl_field(d, t, "displayAlbumArtist", info.album_artist, sizeof(info.album_artist), false, true))
				info.album_artist[0] = 0;
			if (!info.album_artist[0] && !nd_pl_field(d, t, "albumArtist", info.album_artist, sizeof(info.album_artist), false, true))
				info.album_artist[0] = 0;
			long disc = json_obj_long(d, t, "discNumber", 0), number = json_obj_long(d, t, "track", 0);
			info.row.disc = disc > 0 && disc <= INT_MAX ? (int)disc : 0;
			info.row.track = number > 0 && number <= INT_MAX ? (int)number : 0;
			ok = nd_pl_append(s, *id, &info.row, false, &added, &skipped) && nd_pl_sort_info(s, sqlite3_last_insert_rowid(s->db), &info);
		}
	}
	remote->count = added;
	return ok && added == expected && nd_pl_fingerprint(s, *id, remote->baseline);
}

// Worker-only presentation state. None of these operations edits saved order.
static void playlist_sort_key(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
	const char *value = argc ? (const char *)sqlite3_value_text(argv[0]) : NULL;
	char key[LIBRARY_SORT_KEY_MAX];
	library_sort_key_ex(value ? value : "", key, sizeof(key), true);
	sqlite3_result_text(ctx, key, -1, SQLITE_TRANSIENT);
}

static bool playlist_sort_open(nd_playlist_store_t *s, bool write) {
	if (sqlite3_create_function(s->db, "nd_sort_key", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, playlist_sort_key, NULL, NULL) != SQLITE_OK)
		return false;
	if (write && !nd_pl_sql(s, "BEGIN IMMEDIATE;"
							   "CREATE TABLE IF NOT EXISTS playlist_view(id INTEGER PRIMARY KEY REFERENCES playlist(id) ON DELETE CASCADE,"
							   "sort INT NOT NULL DEFAULT 0,version INT NOT NULL DEFAULT 0,metadata INT NOT NULL DEFAULT 0);"
							   "CREATE TABLE IF NOT EXISTS entry_sort(entry INTEGER PRIMARY KEY REFERENCES entry(id) ON DELETE CASCADE,"
							   "track TEXT NOT NULL,album_id TEXT,album TEXT,album_artist TEXT,disc INT,number INT);"
							   "CREATE TABLE IF NOT EXISTS entry_view(entry INTEGER PRIMARY KEY REFERENCES entry(id) ON DELETE CASCADE,"
							   "p INTEGER NOT NULL REFERENCES playlist(id) ON DELETE CASCADE,track TEXT NOT NULL,position INTEGER NOT NULL);"
							   "CREATE INDEX IF NOT EXISTS entry_view_order ON entry_view(p,position,entry);COMMIT;")) {
		sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
		return false;
	}
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN ('playlist_view','entry_sort')"))
		return false;
	s->sorting = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 2;
	sqlite3_finalize(st);
	if (!nd_pl_prepare(s, &st, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='entry_view'"))
		return false;
	s->local_order = s->sorting && sqlite3_step(st) == SQLITE_ROW;
	sqlite3_finalize(st);
	return true;
}

static bool playlist_view_read(nd_playlist_store_t *s, nd_playlist_t *p) {
	p->sort = ND_PL_ORIGINAL;
	p->view_revision = p->metadata_revision = 0;
	if (!s->sorting)
		return true;
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st, "SELECT sort,version,metadata FROM playlist_view WHERE id=?"))
		return false;
	sqlite3_bind_int64(st, 1, p->id);
	int rc = sqlite3_step(st);
	if (rc == SQLITE_ROW) {
		int sort = sqlite3_column_int(st, 0);
		p->sort = sort >= ND_PL_ORIGINAL && sort <= ND_PL_ARTIST_DESC ? sort : ND_PL_ORIGINAL;
		p->view_revision = sqlite3_column_int64(st, 1);
		p->metadata_revision = sqlite3_column_int64(st, 2);
	}
	sqlite3_finalize(st);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE)
		return false;
	if (s->local_order) {
		if (!nd_pl_prepare(s, &st, "SELECT 1 FROM entry_view v JOIN entry e ON e.id=v.entry AND e.track=v.track AND e.p=v.p WHERE e.p=? LIMIT 1"))
			return false;
		sqlite3_bind_int64(st, 1, p->id);
		p->manual = sqlite3_step(st) == SQLITE_ROW;
		sqlite3_finalize(st);
	}
	return true;
}

bool nd_pl_sort_set(nd_playlist_store_t *s, const nd_playlist_t *expected, nd_pl_sort_t sort) {
	if (!s->sorting || sort < ND_PL_ORIGINAL || sort > ND_PL_ARTIST_DESC || !nd_pl_begin(s, expected->id, expected->revision))
		return false;
	nd_playlist_t current;
	if (!nd_pl_get(s, expected->id, &current) || current.pending || current.view_revision != expected->view_revision || current.metadata_revision != expected->metadata_revision) {
		s->error = "nd_pl_stale";
		return false;
	}
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st,
					   "INSERT INTO playlist_view(id,sort,version) VALUES(?,?,1) "
					   "ON CONFLICT(id) DO UPDATE SET sort=excluded.sort,version=version+(sort<>excluded.sort) WHERE version<9223372036854775807"))
		return false;
	sqlite3_bind_int64(st, 1, expected->id);
	sqlite3_bind_int(st, 2, sort);
	bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(s->db) == 1;
	sqlite3_finalize(st);
	return ok;
}

bool nd_pl_sort_info(nd_playlist_store_t *s, int64_t occurrence, const nd_playrow_t *info) {
	sqlite3_int64 last_entry = sqlite3_last_insert_rowid(s->db);
	if (!s->sorting)
		return true;
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st,
					   "INSERT INTO entry_sort(entry,track,album_id,album,album_artist,disc,number) "
					   "SELECT id,track,?2,?3,?4,?5,?6 FROM entry WHERE id=?1 AND track=?7 "
					   "ON CONFLICT(entry) DO UPDATE SET track=excluded.track,"
					   "album_id=coalesce(nullif(excluded.album_id,''),album_id),album=coalesce(nullif(excluded.album,''),album),"
					   "album_artist=coalesce(nullif(excluded.album_artist,''),album_artist),"
					   "disc=CASE WHEN excluded.disc>0 THEN excluded.disc ELSE disc END,number=CASE WHEN excluded.number>0 THEN excluded.number ELSE number END "
					   "WHERE track IS NOT excluded.track OR album_id IS NOT coalesce(nullif(excluded.album_id,''),album_id) "
					   "OR album IS NOT coalesce(nullif(excluded.album,''),album) "
					   "OR album_artist IS NOT coalesce(nullif(excluded.album_artist,''),album_artist) "
					   "OR disc IS NOT CASE WHEN excluded.disc>0 THEN excluded.disc ELSE disc END "
					   "OR number IS NOT CASE WHEN excluded.number>0 THEN excluded.number ELSE number END"))
		return false;
	sqlite3_bind_int64(st, 1, occurrence);
	nd_pl_bind(st, 2, info->album_id);
	nd_pl_bind(st, 3, info->album);
	nd_pl_bind(st, 4, info->album_artist);
	sqlite3_bind_int(st, 5, info->row.disc);
	sqlite3_bind_int(st, 6, info->row.track);
	nd_pl_bind(st, 7, info->row.id);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	int changed = sqlite3_changes(s->db);
	sqlite3_finalize(st);
	if (ok && changed) {
		if (!nd_pl_prepare(s, &st,
						   "INSERT INTO playlist_view(id,metadata) SELECT p,1 FROM entry WHERE id=? "
						   "ON CONFLICT(id) DO UPDATE SET metadata=metadata+1 WHERE metadata<9223372036854775807"))
			return false;
		sqlite3_bind_int64(st, 1, occurrence);
		ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(s->db) == 1;
		sqlite3_finalize(st);
	}
	// Preserve the entry insert identity consumed by existing append callers.
	sqlite3_set_last_insert_rowid(s->db, last_entry);
	return ok;
}

bool nd_pl_sort_copy(nd_playlist_store_t *s, int64_t from, int64_t to) {
	if (!s->sorting)
		return true;
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st,
					   "INSERT INTO entry_sort(entry,track,album_id,album,album_artist,disc,number) "
					   "SELECT d.id,d.track,m.album_id,m.album,m.album_artist,m.disc,m.number FROM entry d "
					   "JOIN entry e ON e.p=?1 AND e.ordinal=d.ordinal AND e.track=d.track "
					   "JOIN entry_sort m ON m.entry=e.id AND m.track=e.track WHERE d.p=?2"))
		return false;
	sqlite3_bind_int64(st, 1, from);
	sqlite3_bind_int64(st, 2, to);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	return ok;
}

static void playlist_order(nd_pl_sort_t sort, char out[1024]) {
	if (sort == ND_PL_ORIGINAL) {
		snprintf(out, 1024, "e.ordinal,e.id");
		return;
	}
	const char *primary = sort <= ND_PL_TITLE_DESC ? "e.title" : sort <= ND_PL_ALBUM_DESC ? "m.album" : sort <= ND_PL_ALBUM_ARTIST_DESC ? "m.album_artist" : "e.artist";
	const char *secondary = sort <= ND_PL_TITLE_DESC ? ""
													 : "(coalesce(m.album,'')=''),nd_sort_key(m.album),(coalesce(m.album_id,'')=''),coalesce(m.album_id,''),"
													   "CASE WHEN m.disc>0 THEN m.disc ELSE 1 END,(coalesce(m.number,0)<=0),coalesce(m.number,0),nd_sort_key(e.title),";
	snprintf(out, 1024, "(coalesce(%s,'')=''),nd_sort_key(%s) %s,%se.ordinal,e.id", primary, primary, sort % 2 == 0 ? "DESC" : "ASC", secondary);
}

static bool playlist_sorted_rows(nd_playlist_store_t *s, int64_t id, int offset, int64_t session, nd_pl_sort_t sort, bool view, nd_playlist_entry_t *out, int limit, int *count) {
	*count = 0;
	char order[1024], query[2400];
	playlist_order(sort, order);
	if (view && s->local_order && sort == ND_PL_ORIGINAL)
		snprintf(order, sizeof(order), "(v.entry IS NULL),v.position,e.ordinal,e.id");
	const char *join = s->sorting ? "LEFT JOIN entry_sort m ON m.entry=e.id AND m.track=e.track" : "LEFT JOIN (SELECT 0 AS entry,'' AS album_id,'' AS album,'' AS album_artist,0 AS disc,0 AS number WHERE 0) m ON m.entry=e.id";
	snprintf(query, sizeof(query),
			 "WITH saved AS (SELECT *,row_number() OVER(ORDER BY ordinal,id)-1 AS position FROM entry WHERE p=?1) "
			 "SELECT e.id,e.track,e.title,e.artist,e.cover,e.duration,"
			 "EXISTS(SELECT 1 FROM selection WHERE session=?2 AND occurrence=e.id),e.position,e.ordinal,"
			 "m.album_id,m.album,m.album_artist,coalesce(m.disc,0),coalesce(m.number,0) "
			 "FROM saved e %s %s ORDER BY %s LIMIT ?4 OFFSET ?3",
			 join, view && s->local_order ? "LEFT JOIN entry_view v ON v.entry=e.id AND v.track=e.track AND v.p=e.p" : "", order);
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st, query))
		return false;
	sqlite3_bind_int64(st, 1, id);
	sqlite3_bind_int64(st, 2, session);
	sqlite3_bind_int(st, 3, offset);
	sqlite3_bind_int(st, 4, limit);
	int rc;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		nd_playlist_entry_t *e = &out[(*count)++];
		memset(e, 0, sizeof(*e));
		e->occurrence = sqlite3_column_int64(st, 0);
		text(st, 1, e->track.id, sizeof(e->track.id));
		text(st, 2, e->track.title, sizeof(e->track.title));
		text(st, 3, e->track.artist, sizeof(e->track.artist));
		text(st, 4, e->track.cover, sizeof(e->track.cover));
		e->track.duration = sqlite3_column_int(st, 5);
		e->selected = sqlite3_column_int(st, 6);
		e->position = sqlite3_column_int(st, 7);
		e->ordinal = sqlite3_column_int(st, 8);
		text(st, 9, e->album_id, sizeof(e->album_id));
		text(st, 10, e->album, sizeof(e->album));
		text(st, 11, e->album_artist, sizeof(e->album_artist));
		e->track.disc = sqlite3_column_int(st, 12);
		e->track.track = sqlite3_column_int(st, 13);
	}
	sqlite3_finalize(st);
	return rc == SQLITE_DONE;
}

bool nd_pl_view_window(nd_playlist_store_t *s, const nd_playlist_t *expected, int first, int64_t session, nd_playlist_entry_t out[ND_WINDOW], int *count, int *total) {
	*count = *total = 0;
	if (first < 0 || !nd_pl_sql(s, "BEGIN"))
		return false;
	nd_playlist_t current = {.id = expected->id};
	sqlite3_stmt *st = NULL;
	bool ok = nd_pl_prepare(s, &st, "SELECT rev,(SELECT count(*) FROM entry WHERE p=playlist.id) FROM playlist WHERE id=?");
	if (ok) {
		sqlite3_bind_int64(st, 1, expected->id);
		ok = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int64(st, 0) == expected->revision;
		if (ok) {
			sqlite3_int64 size = sqlite3_column_int64(st, 1);
			ok = size >= 0 && size <= INT_MAX;
			if (ok)
				*total = (int)size;
		}
	}
	sqlite3_finalize(st);
	if (ok)
		ok = playlist_view_read(s, &current) && current.sort == expected->sort && current.view_revision == expected->view_revision && current.metadata_revision == expected->metadata_revision;
	if (!ok)
		s->error = "nd_pl_stale";
	if (ok)
		ok = playlist_sorted_rows(s, expected->id, first, session, expected->sort, true, out, ND_WINDOW, count);
	if (ok)
		ok = nd_pl_sql(s, "COMMIT");
	if (!ok)
		sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
	return ok;
}

void nd_pl_entry_info(const nd_playlist_entry_t *entry, nd_playrow_t *row) {
	memset(row, 0, sizeof(*row));
	row->row = entry->track;
	row->saved_occurrence = entry->occurrence;
	snprintf(row->album_id, sizeof(row->album_id), "%s", entry->album_id);
	snprintf(row->album, sizeof(row->album), "%s", entry->album);
	snprintf(row->album_artist, sizeof(row->album_artist), "%s", entry->album_artist);
}

// Rearrangement is presentation state. Canonical entry ordinals remain the
// definition used by fingerprinting, Publish, Upload and non-view consumers.
bool nd_pl_view_move(nd_playlist_store_t *s, const nd_playlist_t *expected, int from, int to, int64_t occurrence, const char *track, bool *changed) {
	*changed = false;
	if (!s->local_order || !nd_pl_begin(s, expected->id, expected->revision))
		return false;
	nd_playlist_t p;
	bool ok = nd_pl_get(s, expected->id, &p) && p.sort == ND_PL_ORIGINAL && !p.pending && p.view_revision == expected->view_revision && p.metadata_revision == expected->metadata_revision && from >= 0 && to >= 0 && from < p.count && to < p.count && track;
	sqlite3_stmt *st = NULL;
	if (ok)
		ok = nd_pl_prepare(s, &st,
						   "SELECT e.id,e.track FROM entry e LEFT JOIN entry_view v ON v.entry=e.id AND v.track=e.track AND v.p=e.p "
						   "WHERE e.p=? ORDER BY (v.entry IS NULL),v.position,e.ordinal,e.id LIMIT 1 OFFSET ?");
	if (ok) {
		sqlite3_bind_int64(st, 1, p.id);
		sqlite3_bind_int(st, 2, from);
		ok = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int64(st, 0) == occurrence && !strcmp((const char *)sqlite3_column_text(st, 1), track);
	}
	sqlite3_finalize(st);
	if (!ok) {
		s->error = "nd_pl_stale";
		return false;
	}
	if (from == to)
		return true;
	ok = nd_pl_sql(s, "PRAGMA temp.cache_size=-256; CREATE TEMP TABLE IF NOT EXISTS view_positions(entry INTEGER PRIMARY KEY,position INTEGER); DELETE FROM view_positions");
	if (ok)
		ok = nd_pl_prepare(s, &st,
						   "INSERT INTO view_positions SELECT id,CASE WHEN n=?2 THEN ?3 WHEN ?2<?3 AND n>?2 AND n<=?3 THEN n-1 "
						   "WHEN ?2>?3 AND n>=?3 AND n<?2 THEN n+1 ELSE n END FROM "
						   "(SELECT e.id,row_number() OVER(ORDER BY (v.entry IS NULL),v.position,e.ordinal,e.id)-1 n FROM entry e "
						   "LEFT JOIN entry_view v ON v.entry=e.id AND v.track=e.track AND v.p=e.p WHERE e.p=?1)");
	if (ok) {
		sqlite3_bind_int64(st, 1, p.id);
		sqlite3_bind_int(st, 2, from);
		sqlite3_bind_int(st, 3, to);
		ok = sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	st = NULL;
	if (ok)
		ok = nd_pl_prepare(s, &st,
						   "INSERT INTO entry_view(entry,p,track,position) "
						   "SELECT e.id,e.p,e.track,m.position FROM entry e JOIN view_positions m ON m.entry=e.id WHERE e.p=? "
						   "ON CONFLICT(entry) DO UPDATE SET p=excluded.p,track=excluded.track,position=excluded.position");
	if (ok) {
		sqlite3_bind_int64(st, 1, p.id);
		ok = sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	st = NULL;
	if (ok)
		ok = nd_pl_prepare(s, &st,
						   "INSERT INTO playlist_view(id,version) VALUES(?,1) "
						   "ON CONFLICT(id) DO UPDATE SET version=version+1 WHERE version<9223372036854775807");
	if (ok) {
		sqlite3_bind_int64(st, 1, p.id);
		ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(s->db) == 1;
	}
	sqlite3_finalize(st);
	*changed = ok;
	return ok;
}
bool nd_pl_view_reset(nd_playlist_store_t *s, int64_t id) {
	if (!s->local_order)
		return true;
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st, "DELETE FROM entry_view WHERE p=?"))
		return false;
	sqlite3_bind_int64(st, 1, id);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	int changed = sqlite3_changes(s->db);
	sqlite3_finalize(st);
	st = NULL;
	if (ok && changed) {
		ok = nd_pl_prepare(s, &st, "UPDATE playlist_view SET version=version+1 WHERE id=? AND version<9223372036854775807");
		if (ok) {
			sqlite3_bind_int64(st, 1, id);
			ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(s->db) == 1;
		}
		sqlite3_finalize(st);
	}
	return ok;
}

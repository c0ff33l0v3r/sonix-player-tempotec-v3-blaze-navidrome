#include "navidromeplayliststore.h"
#include "src/system/core/sha256.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
bool nd_pl_entries(nd_playlist_store_t *s, int64_t id, int offset, int64_t session, nd_playlist_entry_t out[ND_WINDOW], int *count) {
	*count = 0;
	sqlite3_stmt *st = NULL;
	if (!nd_pl_prepare(s, &st, "SELECT id,track,title,artist,cover,duration,EXISTS(SELECT 1 FROM selection WHERE session=? AND occurrence=entry.id) FROM entry WHERE p=? ORDER BY ordinal LIMIT 16 OFFSET ?"))
		return false;
	sqlite3_bind_int64(st, 1, session);
	sqlite3_bind_int64(st, 2, id);
	sqlite3_bind_int(st, 3, offset);
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
	}
	sqlite3_finalize(st);
	return rc == SQLITE_DONE;
}
bool nd_pl_play_window(nd_playlist_store_t *s, int64_t id, int64_t revision, int first,
					   nd_playlist_entry_t out[ND_WINDOW], int *count, int *total) {
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
	st = NULL;
	if (ok) ok = nd_pl_prepare(s, &st, "SELECT id,track,title,artist,cover,duration FROM entry WHERE p=? ORDER BY ordinal LIMIT 16 OFFSET ?");
	if (ok) {
		sqlite3_bind_int64(st, 1, id);
		sqlite3_bind_int(st, 2, first);
		int rc;
		while ((rc = sqlite3_step(st)) == SQLITE_ROW && ok) {
			nd_playlist_entry_t *e = &out[(*count)++];
			memset(e, 0, sizeof(*e));
			e->occurrence = sqlite3_column_int64(st, 0);
			const char *track = (const char *)sqlite3_column_text(st, 1);
			int bytes = sqlite3_column_bytes(st, 1);
			ok = e->occurrence > 0 && track && bytes > 0 && (size_t)bytes < sizeof(e->track.id) && strlen(track) == (size_t)bytes;
			if (!ok) break;
			memcpy(e->track.id, track, (size_t)bytes + 1);
			text(st, 2, e->track.title, sizeof(e->track.title));
			text(st, 3, e->track.artist, sizeof(e->track.artist));
			text(st, 4, e->track.cover, sizeof(e->track.cover));
			e->track.duration = sqlite3_column_int(st, 5);
		}
		ok = ok && rc == SQLITE_DONE && *count == (*total - first < ND_WINDOW ? *total - first : ND_WINDOW);
	}
	sqlite3_finalize(st);
	if (ok) ok = nd_pl_sql(s, "COMMIT");
	if (!ok) sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
	if (!ok && !s->error) s->error = "nd_pl_storage_error";
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
	if (ok)
		(*added)++;
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
	return ok;
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
		if (ok)
			ok = nd_pl_append(s, *id, &row, false, &added, &skipped);
	}
	remote->count = added;
	return ok && added == expected && nd_pl_fingerprint(s, *id, remote->baseline);
}

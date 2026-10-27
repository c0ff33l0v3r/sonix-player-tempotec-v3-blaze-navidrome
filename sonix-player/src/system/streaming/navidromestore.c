#include "navidromestore.h"
#include "navidromebatch.h"
#include "navidromedebug.h"
#include "src/system/library/library.h"
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void album_display_fields(const json_doc_t *d, int obj, nd_playrow_t *out);
static bool album_display_put(nd_store_t *s, int64_t handle, int year, const char *genre, const char *credit);
static void album_display_read(nd_store_t *s, int64_t handle, nd_playrow_t *out);

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

// Presentation only: retain every imported credit for exact-ID lookups.
// The memberships index and album primary key keep this bounded on disk.
#define ARTIST_ALBUM_SQL                                                                                               \
	"EXISTS(SELECT 1 FROM rel r JOIN entity a ON a.gen=r.gen AND a.kind=0 AND a.id=r.album "                              \
	"WHERE r.gen=e.gen AND r.kind=2 AND r.id=e.id)"

static const char *artist_filter(nd_store_t *s, nd_kind_t kind) { return !s->favorites && kind == ND_ARTISTS ? " AND " ARTIST_ALBUM_SQL : ""; }

static void browse_name_key(sqlite3_context *ctx, int count, sqlite3_value **values) {
	const char *name = count ? (const char *)sqlite3_value_text(values[0]) : NULL;
	char key[LIBRARY_SORT_KEY_MAX];
	if (name && *name)
		library_sort_key_ex(name, key, sizeof(key), true);
	else
		key[0] = 0;
	sqlite3_result_text(ctx, key, -1, SQLITE_TRANSIENT);
}

// Per-connection views bind one immutable effective revision. Physical base rows
// and changed rows keep distinct stable handles, including for pinned queues.
static bool read_view(nd_store_t *s, int64_t gen) {
	if (s->view == gen)
		return true;
	char *text = NULL;
	bool overlay = false;
	if (s->favorites && s->local) {
		sqlite3_stmt *st = NULL;
		if (!prepare(s, &st, "SELECT 1 FROM local_view WHERE gen=?"))
			return false;
		sqlite3_bind_int64(st, 1, gen);
		overlay = sqlite3_step(st) == SQLITE_ROW;
		sqlite3_finalize(st);
	}
	if (overlay) {
		text = sqlite3_mprintf("DROP VIEW IF EXISTS temp.browse_entity; DROP VIEW IF EXISTS temp.browse_song; DROP VIEW IF EXISTS temp.browse_album;"
							   "CREATE TEMP VIEW browse_entity AS WITH binding AS (SELECT %lld AS gen,coalesce((SELECT base FROM local_view WHERE gen=%lld),%lld) AS base),"
							   "changes AS (SELECT c.* FROM local_change c,binding b WHERE c.scope=%Q AND c.seq>b.base AND c.seq<=b.gen AND NOT EXISTS(SELECT 1 FROM local_change n WHERE n.scope=c.scope AND n.kind=c.kind AND n.id=c.id AND n.seq>c.seq AND n.seq<=b.gen)) "
							   "SELECT e.rowid AS rowid,b.gen AS gen,e.kind,e.id,e.title,e.artist,e.album,e.cover,e.duration,e.disc,e.track,e.sortkey,"
							   "CASE WHEN e.kind=0 THEN 1 ELSE coalesce(f.starred,0) END AS starred,0 AS details "
							   "FROM entity e,binding b LEFT JOIN favorite_song f ON f.gen=b.base AND f.id=e.id AND e.kind=1 WHERE e.gen=b.base AND NOT EXISTS(SELECT 1 FROM changes c WHERE c.kind=e.kind AND c.id=e.id) "
							   "UNION ALL SELECT -c.key,b.gen,c.kind,c.id,c.title,c.artist,c.album,c.cover,c.duration,c.disc,c.track,c.sortkey,c.starred,c.details FROM changes c,binding b WHERE c.kind=1 OR (c.kind=0 AND c.starred=1);"
							   "CREATE TEMP VIEW browse_song AS SELECT gen,id,starred FROM browse_entity WHERE kind=1;"
							   "CREATE TEMP VIEW browse_album AS SELECT gen,id FROM browse_entity WHERE kind=0;",
							   (long long)gen, (long long)gen, (long long)gen, s->scope);
	} else {
		text = sqlite3_mprintf("DROP VIEW IF EXISTS temp.browse_entity; DROP VIEW IF EXISTS temp.browse_song; DROP VIEW IF EXISTS temp.browse_album;"
							   "CREATE TEMP VIEW browse_entity AS SELECT e.rowid AS rowid,e.*,0 AS details,%s AS starred FROM entity e;"
							   "CREATE TEMP VIEW browse_song AS %s; CREATE TEMP VIEW browse_album AS %s;",
							   s->favorites ? "CASE WHEN e.kind=0 THEN 1 ELSE coalesce((SELECT starred FROM favorite_song f WHERE f.gen=e.gen AND f.id=e.id),0) END" : "0", s->favorites ? "SELECT gen,id,starred FROM favorite_song" : "SELECT 0 AS gen,'' AS id,0 AS starred WHERE 0", s->favorites ? "SELECT gen,id FROM favorite_album" : "SELECT 0 AS gen,'' AS id WHERE 0");
	}
	bool ok = text && sql(s, text);
	sqlite3_free(text);
	if (ok && s->display) {
		ok = sql(s, "DROP VIEW IF EXISTS temp.browse_year; CREATE TEMP VIEW browse_year AS "
					"SELECT e.rowid AS handle,CASE WHEN a.year BETWEEN 1 AND 9999 THEN a.year ELSE 0 END AS year "
					"FROM entity e JOIN album_display a ON a.gen=e.gen AND a.handle=e.rowid");
		if (ok && s->local)
			ok = sql(s, "DROP VIEW temp.browse_year; CREATE TEMP VIEW browse_year AS "
						"SELECT e.rowid AS handle,CASE WHEN a.year BETWEEN 1 AND 9999 THEN a.year ELSE 0 END AS year "
						"FROM entity e JOIN album_display a ON a.gen=e.gen AND a.handle=e.rowid "
						"UNION ALL SELECT -c.key,CASE WHEN a.year BETWEEN 1 AND 9999 THEN a.year ELSE 0 END "
						"FROM local_change c JOIN album_display a ON a.gen=c.seq AND a.handle=-c.key");
	}
	if (ok) {
		sqlite3_stmt *credit = NULL;
		ok = prepare(s, &credit, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='album_credit'");
		bool retained = ok && sqlite3_step(credit) == SQLITE_ROW;
		sqlite3_finalize(credit);
		if (ok)
			ok = sqlite3_create_function(s->db, "nd_name_key", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, browse_name_key, NULL, NULL) == SQLITE_OK;
		if (ok)
			ok = sql(s, "DROP VIEW IF EXISTS temp.browse_credit; CREATE TEMP VIEW browse_credit AS SELECT 0 AS handle,'' AS credit WHERE 0");
		if (ok && retained)
			ok = sql(s, "DROP VIEW temp.browse_credit; CREATE TEMP VIEW browse_credit AS "
						"SELECT e.rowid AS handle,c.credit FROM entity e JOIN album_credit c ON c.gen=e.gen AND c.handle=e.rowid");
		if (ok && retained && s->local)
			ok = sql(s, "DROP VIEW temp.browse_credit; CREATE TEMP VIEW browse_credit AS "
						"SELECT e.rowid AS handle,c.credit FROM entity e JOIN album_credit c ON c.gen=e.gen AND c.handle=e.rowid "
						"UNION ALL SELECT -e.key,c.credit FROM local_change e JOIN album_credit c ON c.gen=e.seq AND c.handle=-e.key");
		if (ok)
			ok = sql(s, "DROP VIEW IF EXISTS temp.browse_track_order; CREATE TEMP VIEW browse_track_order AS "
						"SELECT t.rowid AS handle,coalesce(a.sortkey,'') AS albumkey,coalesce(a.id,'') AS albumid,"
						"nd_name_key(coalesce(nullif(c.credit,''),a.artist,'')) AS creditkey,nd_name_key(t.artist) AS artistkey "
						"FROM browse_entity t LEFT JOIN browse_entity a ON a.gen=t.gen AND a.kind=0 AND a.id=t.album "
						"LEFT JOIN browse_credit c ON c.handle=a.rowid WHERE t.kind=1");
	}
	if (ok)
		s->view = gen;
	return ok;
}

bool nd_store_open(nd_store_t *s, const char *path, const char *scope, bool write, bool rebuild) { return nd_store_open_source(s, path, scope, write, rebuild, false); }

bool nd_store_open_source(nd_store_t *s, const char *path, const char *scope, bool write, bool rebuild, bool favorites) {
	memset(s, 0, sizeof(*s));
	s->favorites = favorites;
	snprintf(s->path, sizeof(s->path), "%s", path);
	s->view = -1;
	int expected_version = favorites ? 3 : 1;
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
	bool legacy = favorites && version == 2;
	if ((version != expected_version && (!write || version != 0)) && !legacy && !rebuild) {
		s->error = "nd_cache_error";
		return false;
	}
	if (write && version != expected_version && !legacy) {
		if (!sql(s, "BEGIN; DROP TABLE IF EXISTS album_credit; DROP TABLE IF EXISTS track_navigation; DROP TABLE IF EXISTS album_display; DROP TABLE IF EXISTS local_view; DROP TABLE IF EXISTS local_change; DROP TABLE IF EXISTS artist_state; DROP TABLE IF EXISTS favorite_song; DROP TABLE IF EXISTS favorite_album; DROP TABLE IF EXISTS seen; DROP TABLE IF EXISTS active; DROP TABLE IF EXISTS rel;"
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
		if (favorites && !sql(s, "BEGIN; CREATE TABLE favorite_album(gen INT REFERENCES snapshot(id) ON DELETE CASCADE,id TEXT,expected INT,PRIMARY KEY(gen,id));"
								 "CREATE TABLE favorite_song(gen INT REFERENCES snapshot(id) ON DELETE CASCADE,id TEXT,starred INT,PRIMARY KEY(gen,id));"
								 "CREATE TABLE seen(gen INT REFERENCES snapshot(id) ON DELETE CASCADE,id TEXT,PRIMARY KEY(gen,id));"
								 "CREATE INDEX favorite_flags ON favorite_song(gen,starred,id); PRAGMA user_version=2; COMMIT;"))
			return false;
	}
	if (favorites && write && version != 3 &&
		!sql(s, "BEGIN IMMEDIATE; CREATE TABLE IF NOT EXISTS local_view(gen INTEGER PRIMARY KEY REFERENCES snapshot(id) ON DELETE CASCADE,base INTEGER NOT NULL);"
				"CREATE TABLE IF NOT EXISTS local_change(key INTEGER PRIMARY KEY,seq INTEGER,scope TEXT,kind INT,id TEXT,title TEXT,artist TEXT,album TEXT,cover TEXT,duration INT,disc INT,track INT,sortkey TEXT,starred INT,details INT,UNIQUE(seq,kind,id));"
				"CREATE INDEX IF NOT EXISTS local_identity ON local_change(scope,kind,id,seq);"
				"CREATE TABLE IF NOT EXISTS artist_state(scope TEXT,id TEXT,starred INT,PRIMARY KEY(scope,id)); PRAGMA user_version=3; COMMIT;")) {
		sql(s, "ROLLBACK");
		return false;
	}
	// Optional metadata must not make old/read-only snapshots unusable.
	if (write && !sql(s, "CREATE TABLE IF NOT EXISTS album_credit(gen INT,handle INT,credit TEXT,PRIMARY KEY(gen,handle));"))
		return false;
	if (write && !sql(s, "CREATE TABLE IF NOT EXISTS track_navigation(gen INTEGER,id TEXT,artist TEXT,PRIMARY KEY(gen,id));"))
		return false;
	if (!prepare(s, &st, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='track_navigation'"))
		return false;
	s->navigation = sqlite3_step(st) == SQLITE_ROW;
	sqlite3_finalize(st);
	if (write && !sql(s, "CREATE TABLE IF NOT EXISTS album_display(gen INT NOT NULL,handle INT NOT NULL,year INT,genre TEXT,PRIMARY KEY(gen,handle));"))
		return false;
	if (!prepare(s, &st, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='album_display'"))
		return false;
	s->display = sqlite3_step(st) == SQLITE_ROW;
	sqlite3_finalize(st);
	if (favorites) {
		if (!prepare(s, &st, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='local_view'"))
			return false;
		s->local = sqlite3_step(st) == SQLITE_ROW;
		sqlite3_finalize(st);
		if (version == 3 && !s->local)
			return false;
	}
	return true;
}

void nd_store_close(nd_store_t *s) {
	sqlite3_finalize(s->insert);
	s->insert = NULL;
	sqlite3_finalize(s->relationship);
	s->relationship = NULL;
	sqlite3_finalize(s->genre);
	s->genre = NULL;
	sqlite3_finalize(s->favorite_album);
	sqlite3_finalize(s->favorite_seen);
	sqlite3_finalize(s->favorite_member);
	sqlite3_finalize(s->favorite_song);
	s->favorite_album = s->favorite_seen = s->favorite_member = s->favorite_song = NULL;
	sqlite3_finalize(s->local_insert);
	sqlite3_finalize(s->local_artist);
	sqlite3_finalize(s->annotation);
	s->local_insert = s->local_artist = s->annotation = NULL;
	if (s->db)
		sqlite3_close(s->db);
	s->db = NULL;
}

bool nd_store_status(nd_store_t *s, nd_library_t *out) {
	out->base = 0;
	out->favorites = s->favorites;
	out->corrupt = false;
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
	out->base = out->generation;
	if (s->local) {
		if (!prepare(s, &st, "SELECT base FROM local_view WHERE gen=?"))
			return false;
		sqlite3_bind_int64(st, 1, out->generation);
		if (sqlite3_step(st) == SQLITE_ROW)
			out->base = sqlite3_column_int64(st, 0);
		sqlite3_finalize(st);
	}
	if (!read_view(s, out->generation))
		return false;
	if (!prepare(s, &st, s->favorites ? "SELECT kind,count(*) FROM browse_entity e WHERE gen=? AND (kind=0 OR e.starred=1) GROUP BY kind" : "SELECT e.kind,count(*) FROM browse_entity e WHERE e.gen=? AND (e.kind<>2 OR " ARTIST_ALBUM_SQL ") GROUP BY e.kind"))
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
	nd_log("navidrome: sync field=%s reason=%s\n", key, reason);
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
	if (!s->relationship && !prepare(s, &s->relationship, "INSERT OR IGNORE INTO rel VALUES(?,?,?,?)"))
		return false;
	sqlite3_stmt *st = s->relationship;
	sqlite3_reset(st);
	sqlite3_clear_bindings(st);
	sqlite3_bind_int64(st, 1, s->generation);
	bind(st, 2, album);
	sqlite3_bind_int(st, 3, kind);
	bind(st, 4, id);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_reset(st);
	sqlite3_clear_bindings(st);
	return ok;
}

static bool genre(nd_store_t *s, const char *album, const char *name) {
	if (!*name)
		return true;
	if (strlen(name) >= ND_ID_MAX)
		return false;
	if (!s->genre && !prepare(s, &s->genre, "INSERT OR IGNORE INTO entity(gen,kind,id,title,artist,album,cover,duration,disc,track,sortkey) VALUES(?,3,?,?,'','','',0,0,0,?)"))
		return false;
	sqlite3_stmt *st = s->genre;
	sqlite3_reset(st);
	sqlite3_clear_bindings(st);
	sqlite3_bind_int64(st, 1, s->generation);
	bind(st, 2, name);
	bind(st, 3, name);
	char key[LIBRARY_SORT_KEY_MAX];
	library_sort_key_ex(name, key, sizeof(key), true);
	bind(st, 4, key);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_reset(st);
	sqlite3_clear_bindings(st);
	return ok && relation(s, album, ND_GENRES, name);
}

static void primary_artist(const json_doc_t *d, int obj, char out[ND_ID_MAX]) {
	out[0] = 0;
	if (!nd_api_field_unique(d, obj, "artistId", false) || !field(d, obj, "artistId", out, ND_ID_MAX, false)) {
		out[0] = 0;
		return;
	}
	int arr = json_get(d, obj, "artists");
	if (!nd_api_field_unique(d, obj, "artists", false) || (arr >= 0 && d->toks[arr].type != JSON_NULL && d->toks[arr].type != JSON_ARRAY)) {
		out[0] = 0;
		return;
	}
	if (arr < 0 || d->toks[arr].type == JSON_NULL || !json_len(d, arr))
		return;
	bool matches = false;
	for (int i = 0; i < json_len(d, arr); i++) {
		char id[ND_ID_MAX];
		if (field(d, json_at(d, arr, i), "id", id, sizeof(id), true) && !strcmp(id, out))
			matches = true;
	}
	if (!matches)
		out[0] = 0;
}

static bool navigation_put(nd_store_t *s, const char *id, const char *artist) {
	if (!s->navigation)
		return true;
	sqlite3_stmt *st = NULL;
	if (!prepare(s, &st, "INSERT OR REPLACE INTO track_navigation VALUES(?,?,?)"))
		return false;
	sqlite3_bind_int64(st, 1, s->generation);
	bind(st, 2, id);
	bind(st, 3, artist);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	return ok;
}

static void navigation_get(nd_store_t *s, int64_t gen, const char *id, char out[ND_ID_MAX]) {
	if (!s->navigation)
		return;
	sqlite3_stmt *st = NULL;
	const char *query = s->local ?
		"SELECT artist FROM track_navigation WHERE id=?1 AND gen<=?2 AND gen>=coalesce((SELECT base FROM local_view WHERE gen=?2),?2) ORDER BY gen DESC LIMIT 1" :
		"SELECT artist FROM track_navigation WHERE id=?1 AND gen=?2";
	if (!prepare(s, &st, query))
		return;
	bind(st, 1, id);
	sqlite3_bind_int64(st, 2, gen);
	if (sqlite3_step(st) == SQLITE_ROW) {
		const char *value = (const char *)sqlite3_column_text(st, 0);
		snprintf(out, ND_ID_MAX, "%s", value ? value : "");
	}
	sqlite3_finalize(st);
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

// Subsonic's starred value is an ISO-8601 timestamp, not a boolean or rating.
static bool star_timestamp(const char *s) {
	size_t n = strlen(s);
	if (n < 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':')
		return false;
	for (int i = 0; i < 19; i++)
		if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 && (s[i] < '0' || s[i] > '9'))
			return false;
	int month = (s[5] - '0') * 10 + s[6] - '0', day = (s[8] - '0') * 10 + s[9] - '0';
	if (month < 1 || month > 12 || day < 1 || day > 31 || s[11] > '2' || (s[11] == '2' && s[12] > '3') || s[14] > '5' || s[17] > '5')
		return false;
	size_t i = 19;
	if (s[i] == '.') {
		size_t start = ++i;
		while (s[i] >= '0' && s[i] <= '9')
			i++;
		if (i == start)
			return false;
	}
	if (s[i] == 'Z')
		return i + 1 == n;
	return n == i + 6 && (s[i] == '+' || s[i] == '-') && s[i + 1] >= '0' && s[i + 1] <= '2' && s[i + 2] >= '0' && s[i + 2] <= '9' && (s[i + 1] != '2' || s[i + 2] <= '3') && s[i + 3] == ':' && s[i + 4] >= '0' && s[i + 4] <= '5' && s[i + 5] >= '0' && s[i + 5] <= '9';
}

bool nd_metadata_starred(const json_doc_t *d, int obj, bool *out) {
	bool starred = false;
	int token = json_get(d, obj, "starred");
	if (token >= 0) {
		char stamp[96];
		if (d->toks[token].type != JSON_STRING || !field(d, obj, "starred", stamp, sizeof(stamp), true) || !star_timestamp(stamp))
			return field_error("starred", "invalid_timestamp");
		starred = true;
	}
	*out = starred;
	return true;
}

static bool favorite_count(const json_doc_t *d, int obj, int *out) {
	int count = json_get(d, obj, "songCount");
	if (count >= 0) {
		if (d->toks[count].type != JSON_NUMBER)
			return field_error("songCount", "wrong_type");
		for (int i = d->toks[count].start; i < d->toks[count].end; i++)
			if (d->text[i] < '0' || d->text[i] > '9')
				return field_error("songCount", "invalid_integer");
	}
	long long expected = count < 0 ? -1 : json_llong(d, count, -2);
	if ((count >= 0 && expected < 0) || expected > INT_MAX)
		return field_error("songCount", "out_of_range");
	*out = (int)expected;
	return true;
}

// Reused statements keep Favorites imports as bounded page transactions.
static bool favorite_values(nd_store_t *s, nd_kind_t kind, const char *id, const char *album, bool starred, int expected, bool *retain) {
	sqlite3_stmt **slot = kind == ND_ALBUMS ? &s->favorite_album : &s->favorite_seen;
	if (!*slot && !prepare(s, slot, kind == ND_ALBUMS ? "INSERT INTO favorite_album VALUES(?,?,?)" : "INSERT INTO seen VALUES(?,?)"))
		return false;
	sqlite3_stmt *st = *slot;
	sqlite3_bind_int64(st, 1, s->generation);
	bind(st, 2, id);
	if (kind == ND_ALBUMS)
		sqlite3_bind_int(st, 3, expected);
	int rc = sqlite3_step(st);
	sqlite3_reset(st);
	sqlite3_clear_bindings(st);
	if (rc != SQLITE_DONE) {
		s->error = rc == SQLITE_CONSTRAINT ? "nd_sync_inconsistent" : "nd_cache_error";
		return false;
	}
	if (kind == ND_ALBUMS)
		return true;
	if (!s->favorite_member && !prepare(s, &s->favorite_member, "SELECT 1 FROM favorite_album WHERE gen=? AND id=?"))
		return false;
	st = s->favorite_member;
	sqlite3_bind_int64(st, 1, s->generation);
	bind(st, 2, album);
	rc = sqlite3_step(st);
	*retain = starred || rc == SQLITE_ROW;
	sqlite3_reset(st);
	sqlite3_clear_bindings(st);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE)
		return false;
	if (!*retain)
		return true;
	if (!s->favorite_song && !prepare(s, &s->favorite_song, "INSERT INTO favorite_song VALUES(?,?,?)"))
		return false;
	st = s->favorite_song;
	sqlite3_bind_int64(st, 1, s->generation);
	bind(st, 2, id);
	sqlite3_bind_int(st, 3, starred);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_reset(st);
	sqlite3_clear_bindings(st);
	return ok;
}

static bool favorite_record(nd_store_t *s, const json_doc_t *d, int obj, nd_kind_t kind, const char *id, const char *album, bool *retain) {
	bool starred = false;
	int expected = -1;
	return nd_metadata_starred(d, obj, &starred) && (kind != ND_ALBUMS || favorite_count(d, obj, &expected)) && favorite_values(s, kind, id, album, starred, expected, retain);
}

bool nd_store_import(nd_store_t *s, nd_kind_t kind, const nd_document_t *document, int *count) {
	if (kind < ND_ALBUMS || kind > ND_GENRES || (s->favorites && kind > ND_TRACKS))
		return false;
	const json_doc_t *d = &document->doc;
	const char *container = s->favorites && kind == ND_ALBUMS ? "albumList2" : kind == ND_GENRES ? "genres" : "searchResult3";
	int result = json_get(d, document->response, container);
	if (result < 0 || d->toks[result].type != JSON_OBJECT)
		return field_error(container, "missing_object");
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
		if (obj < 0 || d->toks[obj].type != JSON_OBJECT) {
			ok = false;
			break;
		}
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
		bool retain = true;
		if (s->favorites && !favorite_record(s, d, obj, kind, row.id, album, &retain)) {
			ok = false;
			break;
		}
		if (!retain)
			continue;
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
		int64_t album_handle = sqlite3_last_insert_rowid(s->db);
		ok = rc == SQLITE_DONE && (s->favorites || kind == ND_ARTISTS || memberships(s, d, obj, album));
		if (ok && kind == ND_ALBUMS) {
			nd_playrow_t display = {0};
			album_display_fields(d, obj, &display);
			ok = album_display_put(s, album_handle, display.year, display.genre, display.album_artist);
		}
		if (ok && kind == ND_TRACKS) {
			char artist[ND_ID_MAX];
			primary_artist(d, obj, artist);
			ok = navigation_put(s, row.id, artist);
		}
		if (rc != SQLITE_DONE) {
			nd_log("navidrome: sync insert kind=%d item=%d reason=%s code=%d\n", kind, i, rc == SQLITE_CONSTRAINT ? "duplicate_entity" : "database_error", rc);
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
	if (s->favorites) {
		sqlite3_stmt *st = NULL;
		if (!prepare(s, &st, "SELECT 1 FROM favorite_album a WHERE gen=? AND expected>=0 AND expected<>(SELECT count(*) FROM entity e WHERE e.gen=a.gen AND e.kind=1 AND e.album=a.id) LIMIT 1"))
			return false;
		sqlite3_bind_int64(st, 1, s->generation);
		int rc = sqlite3_step(st);
		sqlite3_finalize(st);
		if (rc != SQLITE_DONE)
			s->error = rc == SQLITE_ROW ? "nd_sync_inconsistent" : "nd_cache_error";
		return rc == SQLITE_DONE;
	}
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
		nd_log("navidrome: sync validation reason=%s kind=%d\n", reason == 1 ? "missing_track_album" : reason == 2 ? "missing_relation_album" : "missing_relation_entity", sqlite3_column_int(st, 1));
	} else if (!ok)
		nd_log("navidrome: sync validation reason=database_error code=%d\n", rc);
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
	// Normally service serialization excludes newer local writers. Preserve a
	// later valid observation even if a candidate was captured before it.
	if (ok && s->local) {
		st = NULL;
		ok = prepare(s, &st, "SELECT 1 FROM local_change WHERE scope=? AND seq>? LIMIT 1");
		if (ok) {
			bind(st, 1, s->scope);
			sqlite3_bind_int64(st, 2, s->generation);
		}
		int rc = ok ? sqlite3_step(st) : SQLITE_ERROR;
		bool newer = rc == SQLITE_ROW;
		ok = ok && (rc == SQLITE_ROW || rc == SQLITE_DONE);
		sqlite3_finalize(st);
		st = NULL;
		if (ok && newer) {
			int64_t base = s->generation;
			ok = prepare(s, &st, "INSERT INTO snapshot(scope,complete,finished) VALUES(?,1,?)");
			if (ok) {
				bind(st, 1, s->scope);
				sqlite3_bind_int64(st, 2, time(NULL));
				ok = sqlite3_step(st) == SQLITE_DONE;
			}
			sqlite3_finalize(st);
			st = NULL;
			int64_t revision = sqlite3_last_insert_rowid(s->db);
			if (ok)
				ok = prepare(s, &st, "INSERT INTO local_view VALUES(?,?)");
			if (ok) {
				sqlite3_bind_int64(st, 1, revision);
				sqlite3_bind_int64(st, 2, base);
				ok = sqlite3_step(st) == SQLITE_DONE;
			}
			sqlite3_finalize(st);
			st = NULL;
			if (ok)
				ok = prepare(s, &st, "UPDATE active SET gen=? WHERE scope=?");
			if (ok) {
				sqlite3_bind_int64(st, 1, revision);
				bind(st, 2, s->scope);
				ok = sqlite3_step(st) == SQLITE_DONE;
			}
			sqlite3_finalize(st);
			st = NULL;
		}
	}
	if (ok && sql(s, "COMMIT"))
		return true;
	sql(s, "ROLLBACK");
	return false;
}

#define PIN_CONTEXTS 9
static pthread_mutex_t pin_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
	char scope[33], path[1024];
	int64_t generation;
	unsigned users;
} pins[PIN_CONTEXTS];
static bool retain_context(const char *scope, int64_t generation, const char *path) {
	pthread_mutex_lock(&pin_lock);
	int free_slot = -1;
	bool ok = false;
	for (int i = 0; i < PIN_CONTEXTS; i++) {
		if (!pins[i].users)
			free_slot = i;
		else if (pins[i].generation == generation && !strcmp(pins[i].scope, scope) && !strcmp(pins[i].path, path)) {
			pins[i].users++;
			ok = true;
			break;
		}
	}
	if (!ok && free_slot >= 0) {
		pins[free_slot].generation = generation;
		snprintf(pins[free_slot].scope, sizeof(pins[free_slot].scope), "%s", scope);
		snprintf(pins[free_slot].path, sizeof(pins[free_slot].path), "%s", path);
		pins[free_slot].users = 1;
		ok = true;
	}
	pthread_mutex_unlock(&pin_lock);
	return ok;
}
static void release_context(const char *scope, int64_t generation, const char *path) {
	pthread_mutex_lock(&pin_lock);
	for (int i = 0; i < PIN_CONTEXTS; i++)
		if (pins[i].users && pins[i].generation == generation && !strcmp(pins[i].scope, scope) && !strcmp(pins[i].path, path)) {
			pins[i].users--;
			break;
		}
	pthread_mutex_unlock(&pin_lock);
}
bool nd_store_retain(const char *scope, int64_t generation) { return retain_context(scope, generation, ""); }
void nd_store_release(const char *scope, int64_t generation) { release_context(scope, generation, ""); }
bool nd_store_retain_source(const nd_library_t *library) { return retain_context(library->scope, library->generation, library->path); }
void nd_store_release_source(const nd_library_t *library) { release_context(library->scope, library->generation, library->path); }

void nd_store_cleanup(nd_store_t *s) {
	// The pin mutex serializes admission with deletion, not with playback I/O.
	pthread_mutex_lock(&pin_lock);
	sqlite3_stmt *st = NULL;
	char text[4096];
	size_t used = snprintf(text, sizeof(text), "DELETE FROM snapshot WHERE id NOT IN(SELECT gen FROM active)");
	const char *path = s->path; // Match the authoritative spelling captured by queue admission.
	for (int i = 0; i < PIN_CONTEXTS; i++)
		used += snprintf(text + used, sizeof(text) - used, " AND NOT(id=?%d AND scope=?%d)", i * 2 + 1, i * 2 + 2);
	if (s->local) {
		used += snprintf(text + used, sizeof(text) - used, " AND id NOT IN(SELECT base FROM local_view WHERE gen IN(SELECT gen FROM active)");
		for (int i = 0; i < PIN_CONTEXTS; i++)
			used += snprintf(text + used, sizeof(text) - used, " OR gen=?%d", i * 2 + 1);
		snprintf(text + used, sizeof(text) - used, ")");
	}
	if (prepare(s, &st, text)) {
		for (int i = 0; i < PIN_CONTEXTS; i++) {
			bool matches = pins[i].users && (!*pins[i].path || (path && !strcmp(path, pins[i].path)));
			sqlite3_bind_int64(st, 1 + 2 * i, matches ? pins[i].generation : -1);
			bind(st, 2 + 2 * i, pins[i].scope);
		}
		if (sqlite3_step(st) != SQLITE_DONE)
			s->error = "nd_cache_error";
		sqlite3_finalize(st);
	}
	pthread_mutex_unlock(&pin_lock);
	if (s->favorites)
		sql(s, "DELETE FROM seen");
	if (sqlite3_db_readonly(s->db, "main") == 0)
		sql(s, s->local ? "DELETE FROM album_display WHERE (handle>0 AND NOT EXISTS(SELECT 1 FROM entity e WHERE e.gen=album_display.gen AND e.rowid=album_display.handle)) OR (handle<0 AND NOT EXISTS(SELECT 1 FROM local_change c WHERE c.seq=album_display.gen AND c.key=-album_display.handle))" : "DELETE FROM album_display WHERE NOT EXISTS(SELECT 1 FROM entity e WHERE e.gen=album_display.gen AND e.rowid=album_display.handle)");
	if (sqlite3_db_readonly(s->db, "main") == 0)
		sqlite3_exec(s->db, "DELETE FROM album_credit WHERE NOT EXISTS(SELECT 1 FROM album_display a WHERE a.gen=album_credit.gen AND a.handle=album_credit.handle)", NULL, NULL, NULL);
	if (s->navigation)
		sql(s, s->local ? "DELETE FROM track_navigation WHERE gen NOT IN(SELECT id FROM snapshot) AND gen NOT IN(SELECT seq FROM local_change)" : "DELETE FROM track_navigation WHERE gen NOT IN(SELECT id FROM snapshot)");
	if (s->local)
		sql(s, "DELETE FROM local_change WHERE seq<(SELECT coalesce(min(gen),9223372036854775807) FROM local_view) AND EXISTS(SELECT 1 FROM local_change n WHERE n.scope=local_change.scope AND n.kind=local_change.kind AND n.id=local_change.id AND n.seq>local_change.seq AND n.seq<=(SELECT coalesce(min(gen),9223372036854775807) FROM local_view))");
	if (s->local)
		sql(s, "DELETE FROM local_change WHERE seq<=coalesce((SELECT min(base) FROM local_view),(SELECT min(gen) FROM active))");
}

bool nd_store_contains(nd_store_t *s, int64_t gen, nd_kind_t kind, const char *id) {
	if (!read_view(s, gen))
		return false;
	sqlite3_stmt *st = NULL;
	if (!prepare(s, &st, "SELECT 1 FROM browse_entity e JOIN snapshot s ON s.id=e.gen WHERE e.gen=? AND e.kind=? AND e.id=? AND s.scope=? AND s.complete=1"))
		return false;
	sqlite3_bind_int64(st, 1, gen);
	sqlite3_bind_int(st, 2, kind);
	bind(st, 3, id);
	bind(st, 4, s->scope);
	bool found = sqlite3_step(st) == SQLITE_ROW;
	sqlite3_finalize(st);
	return found;
}

bool nd_store_artist_browsable(nd_store_t *s, int64_t gen, const char *id) {
	if (s->favorites || !read_view(s, gen))
		return false;
	sqlite3_stmt *st = NULL;
	if (!prepare(s, &st, "SELECT 1 FROM browse_entity e JOIN snapshot s ON s.id=e.gen WHERE e.gen=?1 AND e.kind=2 AND e.id=?2 AND s.scope=?3 AND s.complete=1 AND " ARTIST_ALBUM_SQL))
		return false;
	sqlite3_bind_int64(st, 1, gen);
	bind(st, 2, id);
	bind(st, 3, s->scope);
	bool found = sqlite3_step(st) == SQLITE_ROW;
	sqlite3_finalize(st);
	return found;
}

// All complete-query consumers use the same validated order. Album tracks
// never inherit a list preference, and absent optional years are all unknown.
static bool grouped_tracks(const nd_query_t *q) { return q->kind == ND_TRACKS && !q->filter[0] && q->order >= ND_ORDER_ALBUM && q->order <= ND_ORDER_ARTIST_DESC; }
static const char *query_sort_join(const nd_query_t *q) { return grouped_tracks(q) ? " LEFT JOIN browse_track_order o ON o.handle=e.rowid " : " "; }
static const char *query_order(nd_store_t *s, const nd_query_t *q, char order[768]) {
	if (grouped_tracks(q)) {
		const char *primary = q->order <= ND_ORDER_ALBUM_DESC ? "o.albumkey" : q->order <= ND_ORDER_ALBUM_ARTIST_DESC ? "o.creditkey" : "o.artistkey";
		snprintf(order, 768,
				 "(coalesce(%s,'')=''),%s %s,(coalesce(o.albumkey,'')=''),o.albumkey,"
				 "(coalesce(o.albumid,'')=''),o.albumid,CASE WHEN e.disc>0 THEN e.disc ELSE 1 END,"
				 "(e.track<=0),e.track,e.sortkey,e.id",
				 primary, primary, q->order % 2 ? "DESC" : "ASC");
		return order;
	}
	if (q->kind == ND_TRACKS && q->filter[0])
		return "e.disc,e.track,e.sortkey,e.id";
	if (q->order == ND_ORDER_NAME_DESC)
		return "e.sortkey DESC,e.id DESC";
	if (q->kind == ND_ALBUMS && s->display && (q->order == ND_ORDER_YEAR || q->order == ND_ORDER_YEAR_DESC))
		return q->order == ND_ORDER_YEAR ? "coalesce((SELECT year FROM browse_year WHERE handle=e.rowid),0)<=0,"
										   "(SELECT year FROM browse_year WHERE handle=e.rowid),e.sortkey,e.id"
										 : "coalesce((SELECT year FROM browse_year WHERE handle=e.rowid),0)<=0,"
										   "(SELECT year FROM browse_year WHERE handle=e.rowid) DESC,e.sortkey,e.id";
	return "e.sortkey,e.id";
}

bool nd_store_letters(nd_store_t *s, int64_t gen, const nd_query_t *q, int first[ND_LETTER_BUCKETS]) {
	if (!read_view(s, gen))
		return false;
	_Static_assert(ND_LETTER_BUCKETS == LIBRARY_INDEX_BUCKETS, "letter buckets must agree");
	for (int i = 0; i < ND_LETTER_BUCKETS; i++)
		first[i] = -1;
	if (grouped_tracks(q) || (q->kind == ND_TRACKS && *q->filter) || (q->kind == ND_ALBUMS && (q->order == ND_ORDER_YEAR || q->order == ND_ORDER_YEAR_DESC)))
		return true;
	const char *filter = !*q->filter ? (s->favorites && q->kind == ND_TRACKS ? " AND e.starred=1" : "") : " AND EXISTS(SELECT 1 FROM rel r WHERE r.gen=e.gen AND r.album=e.id AND r.kind=?5 AND r.id=?4)";
	char text[1024], order[768];
	snprintf(text, sizeof(text), "SELECT e.sortkey FROM browse_entity e JOIN snapshot s ON s.id=e.gen WHERE e.gen=?1 AND e.kind=?2 AND s.scope=?3 AND s.complete=1%s%s ORDER BY %s", filter, artist_filter(s, q->kind), query_order(s, q, order));
	sqlite3_stmt *st = NULL;
	if (!prepare(s, &st, text))
		return false;
	sqlite3_bind_int64(st, 1, gen);
	sqlite3_bind_int(st, 2, q->kind);
	bind(st, 3, s->scope);
	if (*q->filter) {
		bind(st, 4, q->filter);
		sqlite3_bind_int(st, 5, q->filter_kind);
	}
	int rc, position = 0;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		const char *key = (const char *)sqlite3_column_text(st, 0);
		int bucket = library_index_slot(library_index_letter_of_key(key));
		if (first[bucket] < 0)
			first[bucket] = position;
		if (position == INT_MAX)
			break;
		position++;
	}
	sqlite3_finalize(st);
	return rc == SQLITE_DONE;
}

static void search_fold(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
	const char *text = argc ? (const char *)sqlite3_value_text(argv[0]) : "";
	char folded[ND_TEXT_MAX];
	library_fold_text(text, folded, sizeof(folded));
	sqlite3_result_text(ctx, folded, -1, SQLITE_TRANSIENT);
}

bool nd_store_query(nd_store_t *s, int64_t gen, const nd_query_t *q, int offset, nd_row_t rows[ND_WINDOW], int *count, int *total) {
	if (!read_view(s, gen))
		return false;
	*count = *total = 0;
	if (offset < 0 || q->kind < ND_ALBUMS || q->kind > ND_GENRES || q->favorites != s->favorites || (s->favorites && q->kind > ND_TRACKS) || !sql(s, "BEGIN"))
		return false;
	char where[1024], text[2200], order[768], pattern[2 * 256 + 3];
	if (*q->search && (!library_search_pattern(q->search, pattern, sizeof(pattern)) || sqlite3_create_function(s->db, "nd_fold", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, search_fold, NULL, NULL) != SQLITE_OK)) {
		sql(s, "ROLLBACK");
		return false;
	}
	snprintf(where, sizeof(where), " FROM browse_entity e JOIN snapshot s ON s.id=e.gen %s WHERE e.gen=?1 AND e.kind=?2 AND s.scope=?3 AND s.complete=1%s", query_sort_join(q), !q->filter[0] ? "" : q->kind == ND_TRACKS ? " AND e.album=?4" : " AND EXISTS(SELECT 1 FROM rel r WHERE r.gen=e.gen AND r.album=e.id AND r.kind=?5 AND r.id=?4)");
	if (s->favorites && q->kind == ND_TRACKS && !*q->filter)
		strcat(where, " AND e.starred=1");
	strcat(where, artist_filter(s, q->kind));
	if (*q->search)
		strcat(where, " AND nd_fold(e.title) LIKE ?8 ESCAPE '\\'");
	sqlite3_stmt *st = NULL;
	bool ok = true;
	for (int pass = 0; ok && pass < 2; pass++) {
		if (pass)
			snprintf(text, sizeof(text), "SELECT e.id,e.title,e.artist,e.cover,e.duration,e.disc,e.track,e.details%s ORDER BY %s LIMIT ?6 OFFSET ?7", where, query_order(s, q, order));
		else
			snprintf(text, sizeof(text), "SELECT count(*)%s", where);
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
		if (*q->search)
			bind(st, 8, pattern);
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
			row->details = sqlite3_column_int(st, 7);
		}
		ok = ok && rc == SQLITE_DONE;
		sqlite3_finalize(st);
		st = NULL;
	}
	bool ended = sql(s, ok ? "COMMIT" : "ROLLBACK");
	if (ok && ended && s->favorites)
		for (int i = 0; i < *count; i++)
			rows[i].favorite_known = nd_store_annotation(s, gen, q->kind, rows[i].id, &rows[i].favorite);
	return ok && ended;
}

bool nd_store_play_index(nd_store_t *s, int64_t gen, const nd_query_t *q, const char *selected, int64_t **handles, int *count, int *start) {
	if (!read_view(s, gen))
		return false;
	*handles = NULL;
	*count = 0;
	*start = -1;
	if (q->kind != ND_TRACKS || q->favorites != s->favorites || !sql(s, "BEGIN"))
		return false;
	char text[1800], order[768], pattern[515];
	if (*q->search && (!library_search_pattern(q->search, pattern, sizeof(pattern)) || sqlite3_create_function(s->db, "nd_fold", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, search_fold, NULL, NULL) != SQLITE_OK)) {
		sql(s, "ROLLBACK");
		return false;
	}
	snprintf(text, sizeof(text),
			 "SELECT e.rowid,e.id FROM browse_entity e JOIN snapshot s ON s.id=e.gen %s "
			 "WHERE e.gen=?1 AND e.kind=1 AND s.scope=?2 AND s.complete=1 %s %s ORDER BY %s",
			 query_sort_join(q),
			 q->filter[0]	? "AND e.album=?3"
			 : s->favorites ? "AND e.starred=1"
							: "",
			 *q->search ? "AND nd_fold(e.title) LIKE ?4 ESCAPE '\\'" : "", query_order(s, q, order));
	sqlite3_stmt *st = NULL;
	bool ok = prepare(s, &st, text);
	int64_t *entries = NULL;
	size_t used = 0, cap = 0;
	if (ok) {
		sqlite3_bind_int64(st, 1, gen);
		bind(st, 2, s->scope);
		if (q->filter[0])
			bind(st, 3, q->filter);
		if (*q->search)
			bind(st, 4, pattern);
		int rc;
		while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
			if (used == cap) {
				size_t more = cap ? cap * 2 : 128;
				if (more > INT_MAX || more > SIZE_MAX / sizeof(*entries)) {
					ok = false;
					break;
				}
				int64_t *grown = realloc(entries, more * sizeof(*entries));
				if (!grown) {
					ok = false;
					break;
				}
				entries = grown;
				cap = more;
			}
			entries[used] = sqlite3_column_int64(st, 0);
			const char *id = (const char *)sqlite3_column_text(st, 1);
			if (id && !strcmp(id, selected))
				*start = (int)used;
			used++;
		}
		ok = ok && rc == SQLITE_DONE && used && *start >= 0;
		sqlite3_finalize(st);
	}
	bool ended = sql(s, ok ? "COMMIT" : "ROLLBACK");
	if (!ok || !ended) {
		free(entries);
		return false;
	}
	int64_t *compact = realloc(entries, used * sizeof(*entries));
	if (compact)
		entries = compact;
	*handles = entries;
	*count = (int)used;
	return true;
}

bool nd_store_album_beside(nd_store_t *s, int64_t gen, const char *id, bool forward, char next[ND_ID_MAX]) { return nd_store_album_beside_order(s, gen, id, forward, ND_ORDER_NAME, next); }

bool nd_store_album_beside_order(nd_store_t *s, int64_t gen, const char *id, bool forward, nd_order_t order, char next[ND_ID_MAX]) {
	if (!read_view(s, gen) || !nd_store_contains(s, gen, ND_ALBUMS, id)) return false;
	nd_query_t q = {.kind = ND_ALBUMS, .order = order};
	char query[1400], order_sql[768];
	snprintf(query, sizeof(query),
			 "WITH ranked AS (SELECT e.id,row_number() OVER(ORDER BY %s) AS n "
			 "FROM browse_entity e WHERE e.gen=?1 AND e.kind=0),here AS (SELECT n FROM ranked WHERE id=?2) "
			 "SELECT ranked.id FROM ranked,here WHERE ranked.id<>?2 "
			 "ORDER BY CASE WHEN ranked.n %s here.n THEN 0 ELSE 1 END,ranked.n %s LIMIT 1",
			 query_order(s, &q, order_sql), forward ? ">" : "<", forward ? "ASC" : "DESC");
	sqlite3_stmt *st = NULL;
	if (!prepare(s, &st, query)) return false;
	sqlite3_bind_int64(st, 1, gen);
	bind(st, 2, id);
	bool ok = sqlite3_step(st) == SQLITE_ROW;
	if (ok) snprintf(next, ND_ID_MAX, "%s", sqlite3_column_text(st, 0));
	sqlite3_finalize(st);
	return ok;
}

bool nd_store_queue_add(nd_store_t *s, int64_t gen, nd_kind_t kind, const char *id) {
	if ((kind != ND_TRACKS && kind != ND_ALBUMS) || !id || !*id || !read_view(s, gen) ||
		!sql(s, "CREATE TEMP TABLE IF NOT EXISTS queue_add(ordinal INTEGER PRIMARY KEY,handle INTEGER UNIQUE)"))
		return false;
	sqlite3_stmt *st = NULL;
	const char *query = kind == ND_TRACKS
		? "INSERT OR IGNORE INTO queue_add(handle) SELECT rowid FROM browse_entity WHERE gen=? AND kind=1 AND id=?"
		: "INSERT OR IGNORE INTO queue_add(handle) SELECT rowid FROM browse_entity WHERE gen=? AND kind=1 AND album=? ORDER BY disc,track,sortkey,id";
	if (!prepare(s, &st, query)) return false;
	sqlite3_bind_int64(st, 1, gen);
	bind(st, 2, id);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	return ok;
}
bool nd_store_queue_finish(nd_store_t *s, int64_t **handles, int *count) {
	*handles = NULL;
	*count = 0;
	sqlite3_stmt *st = NULL;
	if (!prepare(s, &st, "SELECT count(*) FROM queue_add")) return false;
	bool ok = sqlite3_step(st) == SQLITE_ROW;
	sqlite3_int64 n = ok ? sqlite3_column_int64(st, 0) : 0;
	sqlite3_finalize(st);
	if (!ok || n <= 0 || n > INT_MAX || (uint64_t)n > SIZE_MAX / sizeof(int64_t)) return false;
	int64_t *items = malloc((size_t)n * sizeof(*items));
	if (!items || !prepare(s, &st, "SELECT handle FROM queue_add ORDER BY ordinal")) { free(items); return false; }
	int used = 0, rc;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW && used < n)
		items[used++] = sqlite3_column_int64(st, 0);
	ok = rc == SQLITE_DONE && used == n;
	sqlite3_finalize(st);
	if (!ok) { free(items); return false; }
	*handles = items;
	*count = used;
	return true;
}

bool nd_store_favorite(nd_store_t *s, int64_t gen, nd_kind_t kind, const char *id) {
	if (!read_view(s, gen))
		return false;
	if (!s->favorites || (kind != ND_ALBUMS && kind != ND_TRACKS))
		return false;
	sqlite3_stmt *st = NULL;
	if (!prepare(s, &st, kind == ND_ALBUMS ? "SELECT 1 FROM browse_album f JOIN snapshot s ON s.id=f.gen WHERE f.gen=? AND f.id=? AND s.scope=? AND s.complete=1" : "SELECT 1 FROM browse_song f JOIN snapshot s ON s.id=f.gen WHERE f.gen=? AND f.id=? AND f.starred=1 AND s.scope=? AND s.complete=1"))
		return false;
	sqlite3_bind_int64(st, 1, gen);
	bind(st, 2, id);
	bind(st, 3, s->scope);
	bool favorite = sqlite3_step(st) == SQLITE_ROW;
	sqlite3_finalize(st);
	return favorite;
}

bool nd_store_play_row(nd_store_t *s, int64_t gen, int64_t handle, nd_playrow_t *out) {
	if (!read_view(s, gen))
		return false;
	memset(out, 0, sizeof(*out));
	sqlite3_stmt *st = NULL;
	if (!prepare(s, &st,
				 "SELECT e.id,e.title,e.artist,e.cover,e.duration,e.disc,e.track,e.album,a.title "
				 "FROM browse_entity e JOIN snapshot s ON s.id=e.gen LEFT JOIN browse_entity a ON a.gen=e.gen AND a.kind=0 AND a.id=e.album "
				 "WHERE e.rowid=?1 AND e.gen=?2 AND e.kind=1 AND s.scope=?3 AND s.complete=1"))
		return false;
	sqlite3_bind_int64(st, 1, handle);
	sqlite3_bind_int64(st, 2, gen);
	bind(st, 3, s->scope);
	bool ok = sqlite3_step(st) == SQLITE_ROW;
	if (ok) {
		char *fields[] = {out->row.id, out->row.title, out->row.artist, out->row.cover, out->album_id, out->album};
		size_t caps[] = {sizeof(out->row.id), sizeof(out->row.title), sizeof(out->row.artist), sizeof(out->row.cover), sizeof(out->album_id), sizeof(out->album)};
		int columns[] = {0, 1, 2, 3, 7, 8};
		for (int i = 0; i < 6; i++) {
			const char *value = (const char *)sqlite3_column_text(st, columns[i]);
			snprintf(fields[i], caps[i], "%s", value ? value : "");
		}
		out->row.duration = sqlite3_column_int(st, 4);
		out->row.disc = sqlite3_column_int(st, 5);
		out->row.track = sqlite3_column_int(st, 6);
	}
	sqlite3_finalize(st);
	if (ok) {
		if (out->album_id[0]) {
			nd_playrow_t album;
			if (nd_store_metadata(s, gen, ND_ALBUMS, out->album_id, &album))
				snprintf(out->album_artist, sizeof(out->album_artist), "%s", album.album_artist);
		}
		navigation_get(s, gen, out->row.id, out->artist_id);
		out->row.favorite_known = nd_store_annotation(s, gen, ND_TRACKS, out->row.id, &out->row.favorite);
	}
	return ok;
}

// Packed extracted metadata, not response bodies. Offsets survive ownership transfer.
bool nd_batch_init(nd_batch_t *b, nd_kind_t kind) {
	memset(b, 0, sizeof(*b));
	b->kind = kind;
	b->used = 4;
	b->arena = calloc(1, ND_BATCH_ARENA);
	if (!b->arena)
		b->error = "nd_json_memory";
	return b->arena != NULL;
}
void nd_batch_free(nd_batch_t *b) {
	free(b->arena);
	memset(b, 0, sizeof(*b));
}
static uint32_t batch_alloc(nd_batch_t *b, size_t size) {
	size_t at = (b->used + 3) & ~(size_t)3;
	if (at > ND_BATCH_ARENA || size > ND_BATCH_ARENA - at) {
		b->error = "nd_page_large";
		return 0;
	}
	b->used = at + size;
	return at;
}
static uint32_t batch_string(nd_batch_t *b, const char *text) {
	if (!*text)
		return 0;
	size_t size = strlen(text) + 1;
	uint32_t at = batch_alloc(b, size);
	if (at)
		memcpy(b->arena + at, text, size);
	return at;
}
static bool batch_unique(const json_doc_t *d, int obj, const char *name) {
	int found = 0;
	for (int i = obj + 1; i < d->count && d->toks[i].start < d->toks[obj].end; i++) {
		if (d->toks[i].parent != obj)
			continue;
		char key[64];
		if (json_str(d, i, key, sizeof(key)) && !strcmp(key, name) && ++found > 1)
			return false;
	}
	return true;
}
static bool batch_relation(nd_batch_t *b, nd_batch_row_t *row, int kind, const char *id) {
	if (!*id)
		return true;
	uint32_t at = batch_alloc(b, sizeof(nd_batch_relation_t));
	if (!at)
		return false;
	nd_batch_relation_t *r = (void *)(b->arena + at);
	r->next = row->relations;
	r->kind = kind;
	r->id = batch_string(b, id);
	row->relations = at;
	return !b->error;
}
bool nd_batch_add(nd_batch_t *b, const json_doc_t *d, int obj) {
	if (obj < 0 || d->toks[obj].type != JSON_OBJECT)
		return false;
	static const char *unique[] = {"id", "albumId", "artistId", "artists", "albumArtists", "genres", "genre", "title", "name", "value", "artist", "coverArt", "duration", "discNumber", "track"};
	for (size_t i = 0; i < sizeof(unique) / sizeof(*unique); i++)
		if (!batch_unique(d, obj, unique[i]))
			return false;
	nd_row_t row = {0};
	char album[ND_ID_MAX] = "";
	bool ok;
	if (b->kind == ND_GENRES) {
		ok = field(d, obj, "value", row.id, sizeof(row.id), true);
		snprintf(row.title, sizeof(row.title), "%s", row.id);
	} else {
		ok = field(d, obj, "id", row.id, sizeof(row.id), true) && label(d, obj, b->kind == ND_TRACKS ? "title" : "name", row.title) && label(d, obj, "artist", row.artist) && field(d, obj, "coverArt", row.cover, sizeof(row.cover), false);
		if (b->kind == ND_TRACKS)
			ok = ok && field(d, obj, "albumId", album, sizeof(album), true);
		else if (b->kind == ND_ALBUMS)
			snprintf(album, sizeof(album), "%s", row.id);
	}
	if (!ok)
		return false;
	if (!*row.title)
		snprintf(row.title, sizeof(row.title), "%s", row.id);
	long duration = json_obj_long(d, obj, "duration", 0), disc = json_obj_long(d, obj, "discNumber", 0), track = json_obj_long(d, obj, "track", 0);
	if (duration < 0 || duration > INT_MAX || disc < 0 || disc > INT_MAX || track < 0 || track > INT_MAX)
		return false;
	uint32_t at = batch_alloc(b, sizeof(nd_batch_row_t));
	if (!at)
		return false;
	nd_batch_row_t *r = (void *)(b->arena + at);
	memset(r, 0, sizeof(*r));
	r->id = batch_string(b, row.id);
	r->title = batch_string(b, row.title);
	r->artist = batch_string(b, row.artist);
	r->album = batch_string(b, album);
	r->cover = batch_string(b, row.cover);
	if (b->kind == ND_TRACKS) {
		char artist[ND_ID_MAX];
		primary_artist(d, obj, artist);
		r->primary_artist = batch_string(b, artist);
	}
	r->duration = duration;
	r->disc = disc;
	r->track = track;
	r->expected = -1;
	if (b->kind == ND_ALBUMS) {
		nd_playrow_t display = {0};
		album_display_fields(d, obj, &display);
		r->year = display.year;
		r->genre = batch_string(b, display.genre);
		r->album_artist = batch_string(b, display.album_artist);
	}
	if (b->favorites && (!batch_unique(d, obj, "starred") || !batch_unique(d, obj, "songCount") || !nd_metadata_starred(d, obj, &r->starred) || (b->kind == ND_ALBUMS && !favorite_count(d, obj, &r->expected))))
		return false;
	if (b->error)
		return false;
	if (!b->favorites && b->kind != ND_ARTISTS && b->kind != ND_GENRES) {
		bool structured_artists = false, structured_genres = false;
		const char *keys[] = {"artists", "albumArtists", "genres"};
		for (int a = 0; a < 3; a++) {
			int arr = json_get(d, obj, keys[a]);
			if (arr < 0 || d->toks[arr].type == JSON_NULL)
				continue;
			if (d->toks[arr].type != JSON_ARRAY)
				return false;
			if (!a)
				structured_artists = json_len(d, arr) > 0;
			if (a == 2)
				structured_genres = json_len(d, arr) > 0;
			// Direct-child iteration is linear even for metadata-heavy arrays.
			for (int t = arr + 1; t < d->count && d->toks[t].start < d->toks[arr].end; t++) {
				if (d->toks[t].parent != arr)
					continue;
				char id[ND_ID_MAX];
				const char *key = a == 2 ? "name" : "id";
				if (d->toks[t].type != JSON_OBJECT || !batch_unique(d, t, key) || !field(d, t, key, id, sizeof(id), true) || !batch_relation(b, r, a == 2 ? ND_GENRES : ND_ARTISTS, id))
					return false;
			}
		}
		char value[ND_ID_MAX];
		if (!structured_artists && (!field(d, obj, "artistId", value, sizeof(value), false) || !batch_relation(b, r, ND_ARTISTS, value)))
			return false;
		if (!structured_genres && (!field(d, obj, "genre", value, sizeof(value), false) || !batch_relation(b, r, ND_GENRES, value)))
			return false;
	}
	if (b->last)
		((nd_batch_row_t *)(void *)(b->arena + b->last))->next = at;
	else
		b->first = at;
	b->last = at;
	b->count++;
	return !b->error;
}
bool nd_store_import_batch(nd_store_t *s, const nd_batch_t *b) {
	if (s->favorites != b->favorites || (s->favorites && b->kind != ND_TRACKS && b->kind != ND_ALBUMS))
		return false;
	if (!s->insert && !prepare(s, &s->insert, "INSERT INTO entity VALUES(?,?,?,?,?,?,?,?,?,?,?)"))
		return false;
	if (!sql(s, "BEGIN"))
		return false;
	bool ok = true;
	int item = 0;
	for (uint32_t at = b->first; at && ok; item++) {
		const nd_batch_row_t *r = (const void *)(b->arena + at);
		const char *id = (const char *)b->arena + r->id;
		const char *album = (const char *)b->arena + r->album;
		bool retain = true;
		if (s->favorites) {
			ok = favorite_values(s, b->kind, id, album, r->starred, r->expected, &retain);
			if (!ok)
				break;
			if (!retain) {
				at = r->next;
				continue;
			}
		}
		if (b->kind == ND_GENRES)
			ok = genre(s, "", id);
		else {
			sqlite3_stmt *st = s->insert;
			sqlite3_reset(st);
			sqlite3_clear_bindings(st);
			sqlite3_bind_int64(st, 1, s->generation);
			sqlite3_bind_int(st, 2, b->kind);
			bind(st, 3, id);
			bind(st, 4, (const char *)b->arena + r->title);
			bind(st, 5, (const char *)b->arena + r->artist);
			bind(st, 6, album);
			bind(st, 7, (const char *)b->arena + r->cover);
			sqlite3_bind_int(st, 8, r->duration);
			sqlite3_bind_int(st, 9, r->disc);
			sqlite3_bind_int(st, 10, r->track);
			char key[LIBRARY_SORT_KEY_MAX];
			library_sort_key_ex((const char *)b->arena + r->title, key, sizeof(key), true);
			bind(st, 11, key);
			int rc = sqlite3_step(st);
			ok = rc == SQLITE_DONE;
			sqlite3_reset(st);
			sqlite3_clear_bindings(st);
			if (!ok) {
				nd_log("navidrome: sync insert kind=%d item=%d reason=%s code=%d\n", b->kind, item, rc == SQLITE_CONSTRAINT ? "duplicate_entity" : "database_error", rc);
				s->error = rc == SQLITE_CONSTRAINT ? "nd_sync_inconsistent" : "nd_cache_error";
			}
			if (ok && b->kind == ND_ALBUMS)
				ok = album_display_put(s, sqlite3_last_insert_rowid(s->db), r->year, (const char *)b->arena + r->genre, (const char *)b->arena + r->album_artist);
			if (ok && b->kind == ND_TRACKS)
				ok = navigation_put(s, id, (const char *)b->arena + r->primary_artist);
			for (uint32_t rel = r->relations; rel && ok;) {
				const nd_batch_relation_t *v = (const void *)(b->arena + rel);
				const char *name = (const char *)b->arena + v->id;
				ok = v->kind == ND_GENRES ? genre(s, album, name) : relation(s, album, v->kind, name);
				rel = v->next;
			}
		}
		at = r->next;
	}
	if (ok && sql(s, "COMMIT"))
		return true;
	sql(s, "ROLLBACK");
	if (!s->error)
		s->error = "nd_cache_error";
	return false;
}

bool nd_store_track(nd_store_t *s, int64_t generation, const char *id, nd_row_t *out) {
	if (!read_view(s, generation))
		return false;
	sqlite3_stmt *st = NULL;
	if (!prepare(s, &st, "SELECT e.id,e.title,e.artist,e.cover,e.duration,e.disc,e.track FROM browse_entity e JOIN snapshot s ON s.id=e.gen WHERE e.gen=? AND e.kind=? AND e.id=? AND s.scope=? AND s.complete=1"))
		return false;
	sqlite3_bind_int64(st, 1, generation);
	sqlite3_bind_int(st, 2, ND_TRACKS);
	bind(st, 3, id);
	bind(st, 4, s->scope);
	bool ok = sqlite3_step(st) == SQLITE_ROW;
	memset(out, 0, sizeof(*out));
	if (ok) {
		char *fields[] = {out->id, out->title, out->artist, out->cover};
		size_t caps[] = {sizeof(out->id), sizeof(out->title), sizeof(out->artist), sizeof(out->cover)};
		for (int i = 0; i < 4; i++) {
			const char *value = (const char *)sqlite3_column_text(st, i);
			snprintf(fields[i], caps[i], "%s", value ? value : "");
		}
		out->duration = sqlite3_column_int(st, 4);
		out->disc = sqlite3_column_int(st, 5);
		out->track = sqlite3_column_int(st, 6);
	}
	sqlite3_finalize(st);
	return ok;
}

bool nd_store_metadata(nd_store_t *s, int64_t gen, nd_kind_t kind, const char *id, nd_playrow_t *out) {
	memset(out, 0, sizeof(*out));
	if (!read_view(s, gen))
		return false;
	sqlite3_stmt *st = NULL;
	if (!prepare(s, &st, "SELECT e.rowid FROM browse_entity e JOIN snapshot s ON s.id=e.gen WHERE e.gen=? AND e.kind=? AND e.id=? AND s.scope=? AND s.complete=1"))
		return false;
	sqlite3_bind_int64(st, 1, gen);
	sqlite3_bind_int(st, 2, kind);
	bind(st, 3, id);
	bind(st, 4, s->scope);
	bool ok = sqlite3_step(st) == SQLITE_ROW;
	int64_t handle = ok ? sqlite3_column_int64(st, 0) : 0;
	sqlite3_finalize(st);
	if (!ok)
		return false;
	if (kind == ND_TRACKS)
		return nd_store_play_row(s, gen, handle, out);
	if (!prepare(s, &st, "SELECT id,title,artist,cover,details FROM browse_entity WHERE gen=? AND kind=? AND id=?"))
		return false;
	sqlite3_bind_int64(st, 1, gen);
	sqlite3_bind_int(st, 2, kind);
	bind(st, 3, id);
	ok = sqlite3_step(st) == SQLITE_ROW;
	if (ok) {
		char *fields[] = {out->row.id, out->row.title, out->row.artist, out->row.cover};
		size_t caps[] = {sizeof(out->row.id), sizeof(out->row.title), sizeof(out->row.artist), sizeof(out->row.cover)};
		for (int i = 0; i < 4; i++) {
			const char *t = (const char *)sqlite3_column_text(st, i);
			snprintf(fields[i], caps[i], "%s", t ? t : "");
		}
		out->row.details = sqlite3_column_int(st, 4);
	}
	sqlite3_finalize(st);
	if (ok && kind == ND_ALBUMS)
		album_display_read(s, handle, out);
	return ok;
}

bool nd_store_annotation(nd_store_t *s, int64_t gen, nd_kind_t kind, const char *id, bool *starred) {
	if (!s->favorites)
		return false;
	if (kind == ND_ARTISTS) {
		if (!s->local)
			return false;
		sqlite3_stmt *st = NULL;
		if (!prepare(s, &st, "SELECT starred FROM artist_state WHERE scope=? AND id=?"))
			return false;
		bind(st, 1, s->scope);
		bind(st, 2, id);
		bool ok = sqlite3_step(st) == SQLITE_ROW;
		if (ok)
			*starred = sqlite3_column_int(st, 0) != 0;
		sqlite3_finalize(st);
		return ok;
	}
	if (!s->annotation) {
		const char *query = s->local ? "WITH b AS (SELECT coalesce((SELECT base FROM local_view WHERE gen=?4),?4) AS base FROM snapshot WHERE id=?4 AND scope=?1 AND complete=1) SELECT starred FROM (SELECT c.starred,c.seq AS rank FROM local_change c,b WHERE c.scope=?1 AND c.kind=?2 AND c.id=?3 AND c.seq>b.base AND c.seq<=?4 UNION ALL SELECT f.starred,-1 FROM favorite_song f,b WHERE ?2=1 AND f.gen=b.base AND f.id=?3 UNION ALL SELECT 1,-1 FROM favorite_album a,b WHERE ?2=0 AND a.gen=b.base AND a.id=?3) ORDER BY rank DESC LIMIT 1" : "WITH b AS (SELECT id AS base FROM snapshot WHERE id=?4 AND scope=?1 AND complete=1) SELECT f.starred FROM favorite_song f,b WHERE ?2=1 AND f.gen=b.base AND f.id=?3 UNION ALL SELECT 1 FROM favorite_album a,b WHERE ?2=0 AND a.gen=b.base AND a.id=?3 LIMIT 1";
		if (!prepare(s, &s->annotation, query))
			return false;
	}
	sqlite3_stmt *st = s->annotation;
	sqlite3_reset(st);
	sqlite3_clear_bindings(st);
	bind(st, 1, s->scope);
	sqlite3_bind_int(st, 2, kind);
	bind(st, 3, id);
	sqlite3_bind_int64(st, 4, gen);
	bool ok = sqlite3_step(st) == SQLITE_ROW;
	if (ok)
		*starred = sqlite3_column_int(st, 0) != 0;
	sqlite3_reset(st);
	sqlite3_clear_bindings(st);
	return ok;
}

bool nd_store_local_begin(nd_store_t *s) {
	if (!s->favorites || !s->local || !sql(s, "BEGIN IMMEDIATE"))
		return false;
	nd_library_t previous = {0};
	if (!nd_store_status(s, &previous) || !nd_store_begin(s))
		return false;
	sqlite3_stmt *st = NULL;
	if (!prepare(s, &st, "INSERT INTO local_view VALUES(?,?)"))
		return false;
	sqlite3_bind_int64(st, 1, s->generation);
	sqlite3_bind_int64(st, 2, previous.base);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	if (!ok || !prepare(s, &st, "UPDATE snapshot SET complete=1,finished=? WHERE id=?"))
		return false;
	sqlite3_bind_int64(st, 1, previous.finished);
	sqlite3_bind_int64(st, 2, s->generation);
	ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	return ok;
}

bool nd_store_local_put(nd_store_t *s, nd_kind_t kind, const nd_playrow_t *row, bool starred, int details) {
	sqlite3_stmt *st = NULL;
	if (kind == ND_ARTISTS) {
		if (!s->local_artist && !prepare(s, &s->local_artist, "INSERT OR REPLACE INTO artist_state VALUES(?,?,?)"))
			return false;
		st = s->local_artist;
		sqlite3_reset(st);
		sqlite3_clear_bindings(st);
		bind(st, 1, s->scope);
		bind(st, 2, row->row.id);
		sqlite3_bind_int(st, 3, starred);
	} else {
		if (!s->local_insert && !prepare(s, &s->local_insert, "INSERT INTO local_change(seq,scope,kind,id,title,artist,album,cover,duration,disc,track,sortkey,starred,details) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)"))
			return false;
		st = s->local_insert;
		sqlite3_reset(st);
		sqlite3_clear_bindings(st);
		sqlite3_bind_int64(st, 1, s->generation);
		bind(st, 2, s->scope);
		sqlite3_bind_int(st, 3, kind);
		bind(st, 4, row->row.id);
		bind(st, 5, row->row.title);
		bind(st, 6, row->row.artist);
		bind(st, 7, row->album_id);
		bind(st, 8, row->row.cover);
		sqlite3_bind_int(st, 9, row->row.duration);
		sqlite3_bind_int(st, 10, row->row.disc);
		sqlite3_bind_int(st, 11, row->row.track);
		char key[LIBRARY_SORT_KEY_MAX];
		library_sort_key_ex(row->row.title, key, sizeof(key), true);
		bind(st, 12, key);
		sqlite3_bind_int(st, 13, starred);
		sqlite3_bind_int(st, 14, details);
	}
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_reset(st);
	sqlite3_clear_bindings(st);
	if (ok && kind == ND_ALBUMS)
		ok = album_display_put(s, -sqlite3_last_insert_rowid(s->db), row->year, row->genre, row->album_artist);
	if (ok && kind == ND_TRACKS)
		ok = navigation_put(s, row->row.id, row->artist_id);
	return ok;
}

bool nd_store_local_activate(nd_store_t *s) {
	sqlite3_stmt *st = NULL;
	if (!prepare(s, &st, "INSERT OR REPLACE INTO active VALUES(?,?)"))
		return false;
	bind(st, 1, s->scope);
	sqlite3_bind_int64(st, 2, s->generation);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	return ok;
}

static bool metadata_row(const json_doc_t *d, int obj, nd_kind_t kind, const char *parent_album, nd_playrow_t *out) {
	memset(out, 0, sizeof(*out));
	const char *names[] = {"id", "albumId", "album", "artistId", "title", "name", "artist", "coverArt", "starred", "songCount", "duration", "discNumber", "track"};
	for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++)
		if (!nd_api_field_unique(d, obj, names[i], false))
			return false;
	if (!field(d, obj, "id", out->row.id, sizeof(out->row.id), true) || !label(d, obj, kind == ND_TRACKS ? "title" : "name", out->row.title) || !label(d, obj, "artist", out->row.artist) || !field(d, obj, "coverArt", out->row.cover, sizeof(out->row.cover), false))
		return false;
	if (kind == ND_TRACKS && !field(d, obj, "albumId", out->album_id, sizeof(out->album_id), !parent_album))
		return false;
	if (kind == ND_TRACKS && parent_album) {
		if (*out->album_id && strcmp(out->album_id, parent_album))
			return false;
		snprintf(out->album_id, sizeof(out->album_id), "%s", parent_album);
	}
	if (!label(d, obj, "album", out->album) || !field(d, obj, "artistId", out->artist_id, sizeof(out->artist_id), false))
		return false;
	if (kind == ND_ALBUMS)
		album_display_fields(d, obj, out);
	if (kind == ND_TRACKS)
		primary_artist(d, obj, out->artist_id);
	long duration = json_obj_long(d, obj, "duration", 0), disc = json_obj_long(d, obj, "discNumber", 0), track = json_obj_long(d, obj, "track", 0);
	if (duration < 0 || duration > INT_MAX || disc < 0 || disc > INT_MAX || track < 0 || track > INT_MAX)
		return false;
	out->row.duration = duration;
	out->row.disc = disc;
	out->row.track = track;
	if (!out->row.title[0])
		snprintf(out->row.title, sizeof(out->row.title), "%s", out->row.id);
	if (kind == ND_TRACKS) {
		const char *credits[] = {"displayAlbumArtist", "albumArtist"};
		for (int i = 0; i < 2 && !out->album_artist[0]; i++) {
			int token = json_get(d, obj, credits[i]);
			if (token >= 0 && d->toks[token].type == JSON_STRING && !label(d, obj, credits[i], out->album_artist))
				out->album_artist[0] = 0;
		}
	}
	out->row.favorite_known = true;
	return nd_metadata_starred(d, obj, &out->row.favorite);
}
bool nd_metadata_row(const json_doc_t *d, int obj, nd_kind_t kind, nd_playrow_t *out) { return metadata_row(d, obj, kind, NULL, out); }
bool nd_metadata_album_track(const json_doc_t *d, int obj, const char *album, nd_playrow_t *out) { return metadata_row(d, obj, ND_TRACKS, album, out); }

// Optional album presentation data is keyed by the physical entity/local-change
// owner, not by a mutable logical view. Old/read-only stores need no table.
static void album_display_fields(const json_doc_t *d, int obj, nd_playrow_t *out) {
	const char *credit[] = {"displayAlbumArtist", "albumArtist", "artist"};
	for (int i = 0; i < 3 && !out->album_artist[0]; i++) {
		int field_token = json_get(d, obj, credit[i]);
		if (field_token >= 0 && d->toks[field_token].type == JSON_STRING && !label(d, obj, credit[i], out->album_artist))
			out->album_artist[0] = 0;
	}
	int t = json_get(d, obj, "year");
	bool integer = t >= 0 && d->toks[t].type == JSON_NUMBER && d->toks[t].end - d->toks[t].start <= 4;
	for (int i = integer ? d->toks[t].start : 0; integer && i < d->toks[t].end; i++)
		if (d->text[i] < '0' || d->text[i] > '9')
			integer = false;
	long year = integer ? json_obj_long(d, obj, "year", 0) : 0;
	out->year = year > 0 && year <= 9999 ? (int)year : 0;
	t = json_get(d, obj, "genre");
	if (t >= 0 && d->toks[t].type == JSON_STRING && !label(d, obj, "genre", out->genre))
		out->genre[0] = 0;
	if (!out->genre[0]) {
		int genres = json_get(d, obj, "genres");
		int first = json_at(d, genres, 0);
		int name = first >= 0 ? json_get(d, first, "name") : -1;
		if (name >= 0 && d->toks[name].type == JSON_STRING && !label(d, first, "name", out->genre))
			out->genre[0] = 0;
	}
}

static bool album_display_put(nd_store_t *s, int64_t handle, int year, const char *genre_text, const char *credit) {
	sqlite3_stmt *st = NULL;
	if (!prepare(s, &st, "INSERT OR REPLACE INTO album_display(gen,handle,year,genre) VALUES(?,?,?,?)"))
		return false;
	sqlite3_bind_int64(st, 1, s->generation);
	sqlite3_bind_int64(st, 2, handle);
	sqlite3_bind_int(st, 3, year);
	bind(st, 4, genre_text);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	if (ok) {
		ok = prepare(s, &st, "INSERT OR REPLACE INTO album_credit(gen,handle,credit) VALUES(?,?,?)");
		if (ok) {
			sqlite3_bind_int64(st, 1, s->generation);
			sqlite3_bind_int64(st, 2, handle);
			bind(st, 3, credit);
			ok = sqlite3_step(st) == SQLITE_DONE;
		}
		sqlite3_finalize(st);
	}
	return ok;
}

static void album_display_read(nd_store_t *s, int64_t handle, nd_playrow_t *out) {
	sqlite3_stmt *st = NULL;
	const char *credit = handle < 0 ? "SELECT a.credit FROM album_credit a JOIN local_change c ON c.seq=a.gen AND c.key=-a.handle WHERE a.handle=?" : "SELECT a.credit FROM album_credit a JOIN entity e ON e.gen=a.gen AND e.rowid=a.handle WHERE a.handle=?";
	if (sqlite3_prepare_v2(s->db, credit, -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(st, 1, handle);
		if (sqlite3_step(st) == SQLITE_ROW) {
			const char *value = (const char *)sqlite3_column_text(st, 0);
			snprintf(out->album_artist, sizeof(out->album_artist), "%s", value ? value : "");
		}
	}
	sqlite3_finalize(st);
	st = NULL;
	if (!out->album_artist[0])
		snprintf(out->album_artist, sizeof(out->album_artist), "%s", out->row.artist);
	// Absence is a supported legacy format, not a cache error.
	if (sqlite3_prepare_v2(s->db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='album_display'", -1, &st, NULL) != SQLITE_OK)
		return;
	bool exists = sqlite3_step(st) == SQLITE_ROW;
	sqlite3_finalize(st);
	st = NULL;
	if (exists) {
		const char *text = handle < 0 ? "SELECT a.year,a.genre FROM album_display a JOIN local_change c ON c.seq=a.gen AND c.key=-a.handle WHERE a.handle=?" : "SELECT a.year,a.genre FROM album_display a JOIN entity e ON e.gen=a.gen AND e.rowid=a.handle WHERE a.handle=?";
		if (sqlite3_prepare_v2(s->db, text, -1, &st, NULL) == SQLITE_OK) {
			sqlite3_bind_int64(st, 1, handle);
			if (sqlite3_step(st) == SQLITE_ROW) {
				out->year = sqlite3_column_int(st, 0);
				const char *g = (const char *)sqlite3_column_text(st, 1);
				snprintf(out->genre, sizeof(out->genre), "%s", g ? g : "");
			}
		}
		sqlite3_finalize(st);
		st = NULL;
	}
	if (!out->genre[0] && sqlite3_prepare_v2(s->db, "SELECT r.id FROM rel r JOIN entity e ON e.gen=r.gen AND e.id=r.album AND e.kind=0 WHERE e.rowid=? AND r.kind=3 ORDER BY r.id LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(st, 1, handle);
		if (sqlite3_step(st) == SQLITE_ROW)
			snprintf(out->genre, sizeof(out->genre), "%s", sqlite3_column_text(st, 0));
	}
	sqlite3_finalize(st);
}

bool nd_store_album_window(nd_store_t *s, int64_t gen, const nd_query_t *q, int offset, int pitch, int disc_pitch, nd_row_t rows[ND_WINDOW], int *count, int *total, nd_album_window_t *layout) {
	memset(layout, 0, sizeof(*layout));
	if (q->favorites != s->favorites || q->kind != ND_TRACKS || q->filter_kind != ND_ALBUMS || !*q->filter || offset < 0 || pitch <= 0 || disc_pitch <= 0 || !read_view(s, gen) || !nd_store_metadata(s, gen, ND_ALBUMS, q->filter, &layout->info))
		return false;
	// CTEs keep compact ordering/positions in SQLite's bounded file-backed
	// temporary store. Only the final visible band materializes display fields.
	const char *prefix = "WITH t AS (SELECT e.rowid AS handle,CASE WHEN e.disc>0 THEN e.disc ELSE 1 END AS disc,"
						 "row_number() OVER(ORDER BY e.disc,e.track,e.sortkey,e.id)-1 AS ordinal,"
						 "dense_rank() OVER(ORDER BY CASE WHEN e.disc>0 THEN e.disc ELSE 1 END)-1 AS dr FROM browse_entity e "
						 "JOIN snapshot s ON s.id=e.gen WHERE e.gen=?1 AND e.kind=1 AND e.album=?2 AND s.scope=?3 AND s.complete=1),"
						 "d AS (SELECT disc,min(ordinal) AS ordinal,dr FROM t GROUP BY disc),"
						 "v AS (SELECT handle,ordinal,disc,0 AS heading,ordinal*?4+CASE WHEN (SELECT count(*) FROM d)>1 THEN (dr+1)*?5 ELSE 0 END AS y FROM t "
						 "UNION ALL SELECT 0,ordinal,disc,1,ordinal*?4+dr*?5 FROM d WHERE (SELECT count(*) FROM d)>1) ";
	char text[1800];
	sqlite3_stmt *st = NULL;
	bool ok = true;
	*count = *total = 0;
	for (int pass = 0; ok && pass < 2; pass++) {
		snprintf(text, sizeof(text), "%s%s", prefix, pass ? "SELECT v.ordinal,v.disc,v.heading,v.y,e.id,e.title,e.artist,e.cover,e.duration,e.track,e.details FROM v LEFT JOIN browse_entity e ON e.gen=?1 AND e.rowid=v.handle WHERE v.y+CASE WHEN v.heading THEN ?5 ELSE ?4 END>?6 ORDER BY v.y LIMIT 16" : "SELECT (SELECT count(*) FROM t),coalesce(max(y+CASE WHEN heading THEN ?5 ELSE ?4 END),0) FROM v");
		ok = prepare(s, &st, text);
		if (!ok)
			break;
		sqlite3_bind_int64(st, 1, gen);
		bind(st, 2, q->filter);
		bind(st, 3, s->scope);
		sqlite3_bind_int(st, 4, pitch);
		sqlite3_bind_int(st, 5, disc_pitch);
		sqlite3_bind_int(st, 6, layout->offset);
		int rc;
		while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
			if (!pass) {
				int64_t extent = sqlite3_column_int64(st, 1);
				if (extent < 0 || extent > INT_MAX) {
					ok = false;
					break;
				}
				*total = sqlite3_column_int(st, 0);
				layout->extent = (int)extent;
				layout->offset = offset < layout->extent ? offset : layout->extent > pitch ? layout->extent - pitch : 0;
				continue;
			}
			int i = (*count)++;
			nd_row_t *row = &rows[i];
			memset(row, 0, sizeof(*row));
			layout->ordinal[i] = sqlite3_column_int(st, 0);
			row->disc = sqlite3_column_int(st, 1);
			layout->heading[i] = sqlite3_column_int(st, 2) != 0;
			layout->y[i] = sqlite3_column_int(st, 3);
			if (layout->heading[i])
				continue;
			char *fields[] = {row->id, row->title, row->artist, row->cover};
			size_t caps[] = {sizeof(row->id), sizeof(row->title), sizeof(row->artist), sizeof(row->cover)};
			for (int c = 0; c < 4; c++) {
				const char *value = (const char *)sqlite3_column_text(st, c + 4);
				snprintf(fields[c], caps[c], "%s", value ? value : "");
			}
			row->duration = sqlite3_column_int(st, 8);
			row->track = sqlite3_column_int(st, 9);
			row->details = sqlite3_column_int(st, 10);
		}
		ok = ok && rc == SQLITE_DONE;
		sqlite3_finalize(st);
		st = NULL;
	}
	if (ok && s->favorites)
		for (int i = 0; i < *count; i++)
			if (!layout->heading[i])
				rows[i].favorite_known = nd_store_annotation(s, gen, ND_TRACKS, rows[i].id, &rows[i].favorite);
	return ok;
}

bool nd_store_album_position(nd_store_t *s, int64_t gen, const nd_query_t *q, const char *id, int pitch, int disc_pitch, bool alternate, int *position) {
	if (q->favorites != s->favorites || q->kind != ND_TRACKS || q->filter_kind != ND_ALBUMS || !*q->filter || !read_view(s, gen) || pitch <= 0 || disc_pitch <= 0)
		return false;
	sqlite3_stmt *st = NULL;
	const char *text = "WITH t AS (SELECT e.id,row_number() OVER(ORDER BY e.disc,e.track,e.sortkey,e.id)-1 AS ordinal,dense_rank() OVER(ORDER BY CASE WHEN e.disc>0 THEN e.disc ELSE 1 END) AS dr FROM browse_entity e JOIN snapshot s ON s.id=e.gen WHERE e.gen=? AND e.kind=1 AND e.album=? AND s.scope=? AND s.complete=1) SELECT ordinal,dr,(SELECT max(dr) FROM t) FROM t WHERE id=?";
	if (!prepare(s, &st, text))
		return false;
	sqlite3_bind_int64(st, 1, gen);
	bind(st, 2, q->filter);
	bind(st, 3, s->scope);
	bind(st, 4, id);
	bool ok = sqlite3_step(st) == SQLITE_ROW;
	if (ok) {
		int64_t y = sqlite3_column_int64(st, 0) * pitch;
		if (alternate && sqlite3_column_int(st, 2) > 1)
			y += sqlite3_column_int64(st, 1) * disc_pitch;
		ok = y >= 0 && y <= INT_MAX;
		if (ok)
			*position = (int)y;
	}
	sqlite3_finalize(st);
	return ok;
}

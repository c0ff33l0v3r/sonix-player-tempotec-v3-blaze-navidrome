#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "navidromelyrics.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool nd_lyrics_path(const nd_library_t *lib, char out[1200]) {
	if (strlen(lib->scope) != 32 || strspn(lib->scope, "0123456789abcdef") != 32)
		return false;
	const char *slash = strrchr(lib->path, '/');
	if (!slash)
		return false;
	size_t n = slash - lib->path;
	if (n >= 7 && !memcmp(lib->path + n - 7, "/.cache", 7))
		n -= 7;
	return snprintf(out, 1200, "%.*s/lyrics-%s.db", (int)n, lib->path, lib->scope) < 1200;
}
static bool safe(const char *path, bool write) {
	if (!path || !*path || strlen(path) >= 1200)
		return false;
	char copy[1200];
	strcpy(copy, path);
	char *part = copy;
	for (char *p = copy;; p++) {
		if (*p && *p != '/')
			continue;
		char saved = *p;
		*p = 0;
		if (!strcmp(part, ".."))
			return false;
		if (saved && *copy) {
			struct stat st;
			if (lstat(copy, &st)) {
				if (!write || errno != ENOENT || mkdir(copy, 0700))
					return false;
			} else if (!S_ISDIR(st.st_mode) || (write && !(st.st_mode & 0222)))
				return false;
		}
		*p = saved;
		part = p + 1;
		if (!saved)
			break;
	}
	const char *suffix[] = {"", "-journal", "-wal", "-shm"};
	uint64_t total = 0;
	for (int i = 0; i < 4; i++) {
		char leaf[1220];
		struct stat st;
		snprintf(leaf, sizeof(leaf), "%s%s", path, suffix[i]);
		if (lstat(leaf, &st)) {
			if (errno != ENOENT)
				return false;
		} else {
			if (!S_ISREG(st.st_mode) || (i >= 2 && st.st_size) || (!i && write && !(st.st_mode & 0222)))
				return false;
			total += st.st_size;
		}
	}
	return total <= 16 * 1024 * 1024;
}
static bool sql(sqlite3 *db, const char *s) { return sqlite3_exec(db, s, NULL, NULL, NULL) == SQLITE_OK; }
static int64_t number(sqlite3 *db, const char *s) {
	sqlite3_stmt *st = NULL;
	int64_t n = -1;
	if (sqlite3_prepare_v2(db, s, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
		n = sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	return n;
}
static void put32(unsigned char *p, uint32_t n) {
	for (int i = 0; i < 4; i++)
		p[i] = (unsigned char)(n >> (8 * i));
}
static uint32_t get32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static unsigned char *pack(const lyrics_t *l, size_t *size) {
	if (l->count < 1 || l->count > ND_LYRIC_LINES)
		return NULL;
	size_t n = 4, text = 0;
	for (int i = 0; i < l->count; i++) {
		size_t len = strlen(l->lines[i].text) + 1;
		if (len > ND_LYRIC_TEXT - text)
			return NULL;
		text += len;
		n += 8 + len;
	}
	unsigned char *b = malloc(n), *p = b;
	if (!b)
		return NULL;
	put32(p, l->count);
	p += 4;
	for (int i = 0; i < l->count; i++) {
		size_t len = strlen(l->lines[i].text) + 1;
		put32(p, (uint32_t)l->lines[i].ms);
		put32(p + 4, len);
		p += 8;
		memcpy(p, l->lines[i].text, len);
		p += len;
	}
	*size = n;
	return b;
}
static bool unpack(const void *data, int size, bool synced, lyrics_t *l) {
	if (!data || size < 4 || size > ND_LYRIC_TEXT + ND_LYRIC_LINES * 8 + 4)
		return false;
	const unsigned char *p = data, *end = p + size;
	uint32_t count = get32(p);
	p += 4;
	if (!count || count > ND_LYRIC_LINES)
		return false;
	l->lines = calloc(count, sizeof(*l->lines));
	l->arena = malloc(ND_LYRIC_TEXT);
	if (!l->lines || !l->arena)
		goto fail;
	size_t used = 0;
	for (uint32_t i = 0; i < count; i++) {
		if (end - p < 8)
			goto fail;
		int32_t ms = (int32_t)get32(p);
		uint32_t len = get32(p + 4);
		p += 8;
		if (!len || len > (size_t)(end - p) || len > ND_LYRIC_TEXT - used || p[len - 1] || memchr(p, 0, len - 1) || (synced && (ms < 0 || (i && ms < l->lines[i - 1].ms))) || (!synced && ms != -1))
			goto fail;
		memcpy(l->arena + used, p, len);
		l->lines[i] = (lyrics_line_t){.ms = ms, .text = l->arena + used};
		used += len;
		p += len;
	}
	if (p != end)
		goto fail;
	l->count = count;
	l->synced = synced;
	return true;
fail:
	lyrics_free(l);
	return false;
}
static bool load(sqlite3 *db, const char *id, nd_lyric_set_t *set) {
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(db, "SELECT kind,n,sync,lang,body FROM words WHERE track=? ORDER BY kind DESC,n", -1, &st, NULL) != SQLITE_OK)
		return false;
	sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
	bool ok = true;
	int rc;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		if (set->count && set->override)
			break;
		if (set->count == ND_LYRIC_VERSIONS || sqlite3_column_int(st, 0) < 0 || sqlite3_column_int(st, 0) > 1 || sqlite3_column_int(st, 1) != set->count || sqlite3_column_int(st, 2) < 0 || sqlite3_column_int(st, 2) > 1) {
			ok = false;
			break;
		}
		nd_lyric_doc_t *v = &set->versions[set->count];
		const char *lang = (const char *)sqlite3_column_text(st, 3);
		if (!lang || strlen(lang) >= sizeof(v->language) || !unpack(sqlite3_column_blob(st, 4), sqlite3_column_bytes(st, 4), sqlite3_column_int(st, 2), &v->lyrics)) {
			ok = false;
			break;
		}
		strcpy(v->language, lang);
		set->override = sqlite3_column_int(st, 0) == 1;
		set->count++;
	}
	if (rc != SQLITE_DONE && rc != SQLITE_ROW)
		ok = false;
	sqlite3_finalize(st);
	if (ok && !set->override && set->count) {
		if (sqlite3_prepare_v2(db, "SELECT choice FROM tracks WHERE id=?", -1, &st, NULL) != SQLITE_OK)
			ok = false;
		else {
			sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
			if (sqlite3_step(st) == SQLITE_ROW)
				set->selected = sqlite3_column_int(st, 0);
			if (set->selected < 0 || set->selected >= set->count)
				ok = false;
		}
		sqlite3_finalize(st);
	}
	if (!ok)
		nd_lyrics_free(set);
	return ok;
}
static bool remove_kind(sqlite3 *db, const char *id, int kind) {
	sqlite3_stmt *st = NULL;
	bool ok = sqlite3_prepare_v2(db, "DELETE FROM words WHERE track=? AND kind=?", -1, &st, NULL) == SQLITE_OK;
	if (ok) {
		sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(st, 2, kind);
		ok = sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	return ok;
}
static bool save_doc(sqlite3 *db, const char *id, int kind, int n, const nd_lyric_doc_t *v) {
	size_t size;
	unsigned char *blob = pack(&v->lyrics, &size);
	if (!blob)
		return false;
	sqlite3_stmt *st = NULL;
	bool ok = sqlite3_prepare_v2(db, "INSERT INTO words VALUES(?,?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK;
	if (ok) {
		sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(st, 2, kind);
		sqlite3_bind_int(st, 3, n);
		sqlite3_bind_int(st, 4, v->lyrics.synced);
		sqlite3_bind_text(st, 5, v->language, -1, SQLITE_TRANSIENT);
		sqlite3_bind_blob(st, 6, blob, (int)size, SQLITE_TRANSIENT);
		ok = sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	free(blob);
	return ok;
}
const char *nd_lyrics_store(const char *path, const char *scope, const char *id, nd_lyric_action_t action, nd_lyric_set_t *set, const char *text, int choice, bool (*current)(void *), void *user) {
	bool write = action != ND_LYRIC_LOCAL;
	const char *error = "nd_lyrics_storage";
	if (!scope || strlen(scope) != 32 || strspn(scope, "0123456789abcdef") != 32 || !id || !*id || strlen(id) >= ND_ID_MAX || !safe(path, write))
		return error;
	struct stat info;
	if (!write && lstat(path, &info) && errno == ENOENT)
		return NULL;
	if (write) {
		int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
		if (fd >= 0)
			close(fd);
		else if (errno != EEXIST)
			return error;
	}
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	if (sqlite3_open_v2(path, &db, (write ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY) | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK)
		goto done;
	sqlite3_busy_timeout(db, 0);
	sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 512 * 1024);
	if (number(db, "PRAGMA page_count") > 2000)
		goto done;
	int64_t version = number(db, "PRAGMA user_version");
	bool fresh = version == 0 && number(db, "SELECT count(*) FROM sqlite_master") == 0;
	if ((!fresh && version != 1) || (fresh && !write))
		goto done;
	if (!sql(db, "PRAGMA foreign_keys=ON; PRAGMA cache_size=-128; PRAGMA mmap_size=0; PRAGMA temp_store=MEMORY;"))
		goto done;
	if (write && (!sql(db, "PRAGMA page_size=4096; PRAGMA max_page_count=2000; PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;") || number(db, "PRAGMA page_size") != 4096 || number(db, "PRAGMA page_count") > 2000))
		goto done;
	if (fresh) {
		if (!sql(db, "BEGIN IMMEDIATE; CREATE TABLE identity(scope TEXT NOT NULL); CREATE TABLE tracks(id TEXT PRIMARY KEY,choice INT NOT NULL DEFAULT 0,used INTEGER NOT NULL); CREATE TABLE words(track TEXT REFERENCES tracks(id) ON DELETE CASCADE,kind INT,n INT,sync INT,lang TEXT,body BLOB,PRIMARY KEY(track,kind,n)); PRAGMA user_version=1;"))
			goto done;
		if (sqlite3_prepare_v2(db, "INSERT INTO identity VALUES(?)", -1, &st, NULL) != SQLITE_OK)
			goto done;
		sqlite3_bind_text(st, 1, scope, -1, SQLITE_TRANSIENT);
		bool ok = sqlite3_step(st) == SQLITE_DONE;
		sqlite3_finalize(st);
		st = NULL;
		if (!ok || (current && !current(user)) || !sql(db, "COMMIT"))
			goto done;
	}
	if (sqlite3_prepare_v2(db, "SELECT scope FROM identity", -1, &st, NULL) != SQLITE_OK)
		goto done;
	bool matches = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_TEXT && !strcmp((const char *)sqlite3_column_text(st, 0), scope) && sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	st = NULL;
	if (!matches)
		goto done;
	if (!write) {
		error = load(db, id, set) ? NULL : error;
		goto done;
	}
	nd_lyric_doc_t override = {0};
	if (action == ND_LYRIC_APPLY && !lyrics_parse_text(text, &override.lyrics)) {
		error = "nd_lyrics_limit";
		goto done;
	}
	bool ok = false;
	for (int attempt = 0; attempt < 33; attempt++) {
		if (current && !current(user))
			break;
		if (!sql(db, "BEGIN IMMEDIATE"))
			break;
		if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO tracks VALUES(?,0,0)", -1, &st, NULL) != SQLITE_OK)
			break;
		sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
		ok = sqlite3_step(st) == SQLITE_DONE;
		sqlite3_finalize(st);
		st = NULL;
		if (ok && action == ND_LYRIC_APPLY)
			ok = remove_kind(db, id, 1) && save_doc(db, id, 1, 0, &override);
		if (ok && (action == ND_LYRIC_REFRESH || action == ND_LYRIC_SERVER || action == ND_LYRIC_VIEW)) {
			if (set->count) {
				ok = remove_kind(db, id, 0);
				for (int i = 0; ok && i < set->count; i++)
					ok = save_doc(db, id, 0, i, &set->versions[i]);
			}
			if (ok && action == ND_LYRIC_SERVER) {
				if (sqlite3_prepare_v2(db, "SELECT count(*) FROM words WHERE track=? AND kind=0", -1, &st, NULL) != SQLITE_OK)
					ok = false;
				else {
					sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
					ok = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) > 0;
				}
				sqlite3_finalize(st);
				st = NULL;
				if (ok)
					ok = remove_kind(db, id, 1);
			}
		}
		if (ok && (action == ND_LYRIC_SELECT || set->count)) {
			if (choice < 0 || choice >= ND_LYRIC_VERSIONS)
				ok = false;
			else if (sqlite3_prepare_v2(db, "UPDATE tracks SET choice=? WHERE id=? AND ?<(SELECT count(*) FROM words WHERE track=? AND kind=0)", -1, &st, NULL) != SQLITE_OK)
				ok = false;
			else {
				sqlite3_bind_int(st, 1, choice);
				sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
				sqlite3_bind_int(st, 3, choice);
				sqlite3_bind_text(st, 4, id, -1, SQLITE_TRANSIENT);
				ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
			}
			sqlite3_finalize(st);
			st = NULL;
		}
		if (ok && sqlite3_prepare_v2(db, "UPDATE tracks SET used=(SELECT coalesce(max(used),0)+1 FROM tracks) WHERE id=?", -1, &st, NULL) == SQLITE_OK) {
			sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
			ok = sqlite3_step(st) == SQLITE_DONE;
			sqlite3_finalize(st);
			st = NULL;
		} else
			ok = false;
		int failure_code = sqlite3_errcode(db);
		bool capacity = number(db, "SELECT count(*) FROM tracks") > 1024 || failure_code == SQLITE_FULL;
		if (ok && !capacity && (!current || current(user)) && sql(db, "COMMIT"))
			break;
		capacity |= sqlite3_errcode(db) == SQLITE_FULL;
		ok = false;
		sql(db, "ROLLBACK");
		if (!capacity || (current && !current(user)))
			break;
		// Bounded reclamation never touches a track carrying an applied override.
		if (sqlite3_prepare_v2(db, "DELETE FROM tracks WHERE id=(SELECT id FROM tracks WHERE id<>? AND NOT EXISTS(SELECT 1 FROM words WHERE track=id AND kind=1) ORDER BY used LIMIT 1)", -1, &st, NULL) != SQLITE_OK)
			break;
		sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
		bool evicted = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) > 0;
		sqlite3_finalize(st);
		st = NULL;
		if (!evicted)
			break;
	}
	lyrics_free(&override.lyrics);
	if (ok) {
		nd_lyrics_free(set);
		error = load(db, id, set) ? NULL : "nd_lyrics_storage";
	}
done:
	sqlite3_finalize(st);
	if (db) {
		sql(db, "ROLLBACK");
		sqlite3_close(db);
	}
	return error;
}

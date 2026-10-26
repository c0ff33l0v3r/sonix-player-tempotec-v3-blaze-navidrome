#include "navidromereportstore.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool sql(nd_report_store_t *s, const char *q) { return sqlite3_exec(s->db, q, NULL, NULL, NULL) == SQLITE_OK; }
static int64_t integer(nd_report_store_t *s, const char *q) {
	sqlite3_stmt *st = NULL;
	int64_t n = -1;
	if (sqlite3_prepare_v2(s->db, q, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
		n = sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	return n;
}
// Fixed-depth trusted service path, also reject symlink sidecars and traversal.
static bool path_safe(const char *path, bool create) {
	char copy[1200];
	if (!path || !*path || strlen(path) >= sizeof(copy))
		return false;
	snprintf(copy, sizeof(copy), "%s", path);
	char *component = copy;
	for (char *p = copy;; p++) {
		if (*p && *p != '/')
			continue;
		char saved = *p;
		*p = 0;
		if (!strcmp(component, ".."))
			return false;
		if (saved && *copy) {
			struct stat st;
			if (lstat(copy, &st)) {
				if (!create || errno != ENOENT || mkdir(copy, 0700))
					return false;
			} else if (!S_ISDIR(st.st_mode))
				return false;
		}
		*p = saved;
		component = p + 1;
		if (!saved)
			break;
	}
	const char *suffixes[] = {"", "-journal", "-wal", "-shm"};
	uint64_t total = 0;
	for (unsigned i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
		char leaf[1220];
		struct stat st;
		snprintf(leaf, sizeof(leaf), "%s%s", path, suffixes[i]);
		if (lstat(leaf, &st)) {
			if (errno != ENOENT)
				return false;
		} else {
			if (!S_ISREG(st.st_mode) || (i >= 2 && st.st_size))
				return false;
			if (!i && !(st.st_mode & 0222))
				return false;
			total += st.st_size;
		}
	}
	return total <= 2 * 1024 * 1024;
}
bool nd_report_store_open(nd_report_store_t *s, const char *path, const char *scope, bool create) {
	memset(s, 0, sizeof(*s));
	if (!scope || strlen(scope) != 32 || strspn(scope, "0123456789abcdef") != 32 || !path_safe(path, create))
		return false;
	if (create) {
		int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
		if (fd >= 0)
			close(fd);
		else if (errno != EEXIST)
			return false;
	}
	if (sqlite3_open_v2(path, &s->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK)
		return false;
	sqlite3_busy_timeout(s->db, 0);
	int64_t version = integer(s, "PRAGMA user_version");
	bool fresh = version == 0 && integer(s, "SELECT count(*) FROM sqlite_master") == 0;
	if ((!fresh && version != 1) || integer(s, "PRAGMA page_count") > 128)
		return false;
	// 128 4-KiB pages = 512 KiB DB. A rollback journal of every DB page plus
	// headers is < 1 MiB; temp storage is bounded memory, no WAL or temp copies.
	if (!sql(s, "PRAGMA page_size=4096; PRAGMA max_page_count=128; PRAGMA cache_size=-64;"
				"PRAGMA mmap_size=0; PRAGMA temp_store=MEMORY; PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;"))
		return false;
	if (integer(s, "PRAGMA page_size") != 4096)
		return false;
	if (fresh) {
		if (!create || !sql(s, "BEGIN IMMEDIATE; CREATE TABLE identity(scope TEXT NOT NULL);"
							   "CREATE TABLE listens(seq INTEGER PRIMARY KEY, key TEXT UNIQUE NOT NULL, track TEXT NOT NULL,"
							   "time INTEGER NOT NULL, attempted INTEGER NOT NULL DEFAULT 0); PRAGMA user_version=1;"))
			return false;
		sqlite3_stmt *st = NULL;
		bool ok = sqlite3_prepare_v2(s->db, "INSERT INTO identity VALUES(?)", -1, &st, NULL) == SQLITE_OK;
		if (ok) {
			sqlite3_bind_text(st, 1, scope, -1, SQLITE_TRANSIENT);
			ok = sqlite3_step(st) == SQLITE_DONE;
		}
		sqlite3_finalize(st);
		if (!ok || !sql(s, "COMMIT"))
			return false;
	}
	sqlite3_stmt *st = NULL;
	bool ok = sqlite3_prepare_v2(s->db, "SELECT scope FROM identity", -1, &st, NULL) == SQLITE_OK;
	if (ok)
		ok = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_TEXT && !strcmp((const char *)sqlite3_column_text(st, 0), scope) && sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	return ok;
}
void nd_report_store_close(nd_report_store_t *s) {
	if (s->db) {
		sql(s, "ROLLBACK");
		sqlite3_close(s->db);
	}
	s->db = NULL;
}
bool nd_report_store_ready(nd_report_store_t *s, int64_t now) {
	sqlite3_stmt *st = NULL;
	bool ok = sqlite3_prepare_v2(s->db, "SELECT 1 FROM listens WHERE attempted=0 AND time>=? AND time<=? LIMIT 1", -1, &st, NULL) == SQLITE_OK;
	if (ok) {
		sqlite3_bind_int64(st, 1, now - ND_LISTEN_AGE);
		sqlite3_bind_int64(st, 2, now + 300000);
		ok = sqlite3_step(st) == SQLITE_ROW;
	}
	sqlite3_finalize(st);
	return ok;
}
static bool expire(nd_report_store_t *s, int64_t now) {
	sqlite3_stmt *st = NULL;
	bool ok = sqlite3_prepare_v2(s->db, "DELETE FROM listens WHERE attempted!=0 OR time<? OR time>?", -1, &st, NULL) == SQLITE_OK;
	if (ok) {
		sqlite3_bind_int64(st, 1, now - ND_LISTEN_AGE);
		sqlite3_bind_int64(st, 2, now + 300000);
		ok = sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	return ok;
}
bool nd_report_store_put(nd_report_store_t *s, const nd_listen_t *e, const char *key, int64_t now) {
	if (e->when <= 0 || e->when < now - ND_LISTEN_AGE || e->when > now + 300000 || !e->id[0] || strnlen(e->id, ND_ID_MAX) >= ND_ID_MAX || strlen(key) > 64 || !sql(s, "BEGIN IMMEDIATE"))
		return false;
	bool ok = expire(s, now);
	int64_t count = integer(s, "SELECT count(*) FROM listens");
	if (ok && count >= ND_LISTEN_ROWS)
		ok = sql(s, "DELETE FROM listens WHERE seq=(SELECT seq FROM listens ORDER BY seq LIMIT 1)");
	sqlite3_stmt *st = NULL;
	if (ok)
		ok = count >= 0 && sqlite3_prepare_v2(s->db, "INSERT OR IGNORE INTO listens(key,track,time) VALUES(?,?,?)", -1, &st, NULL) == SQLITE_OK;
	if (ok) {
		sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 2, e->id, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(st, 3, e->when);
		ok = sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	if (ok)
		ok = sql(s, "COMMIT");
	if (!ok)
		sql(s, "ROLLBACK");
	return ok;
}
int64_t nd_report_store_take(nd_report_store_t *s, int64_t now, nd_listen_t *e) {
	if (now <= 0 || !sql(s, "BEGIN IMMEDIATE"))
		return 0;
	bool ok = expire(s, now);
	sqlite3_stmt *st = NULL;
	int64_t seq = 0;
	if (ok)
		ok = sqlite3_prepare_v2(s->db, "SELECT seq,track,time FROM listens WHERE attempted=0 ORDER BY seq LIMIT 1", -1, &st, NULL) == SQLITE_OK;
	if (ok && sqlite3_step(st) == SQLITE_ROW) {
		const char *id = (const char *)sqlite3_column_text(st, 1);
		if (id && sqlite3_column_bytes(st, 1) > 0 && sqlite3_column_bytes(st, 1) < ND_ID_MAX && strlen(id) == (size_t)sqlite3_column_bytes(st, 1)) {
			seq = sqlite3_column_int64(st, 0);
			snprintf(e->id, sizeof(e->id), "%s", id);
			e->when = sqlite3_column_int64(st, 2);
		}
	}
	sqlite3_finalize(st);
	st = NULL;
	if (ok && seq) {
		ok = sqlite3_prepare_v2(s->db, "UPDATE listens SET attempted=1 WHERE seq=?", -1, &st, NULL) == SQLITE_OK;
		if (ok) {
			sqlite3_bind_int64(st, 1, seq);
			ok = sqlite3_step(st) == SQLITE_DONE;
		}
		sqlite3_finalize(st);
	}
	if (ok)
		ok = sql(s, "COMMIT");
	if (!ok)
		sql(s, "ROLLBACK");
	return ok ? seq : 0;
}

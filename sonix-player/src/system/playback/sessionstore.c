#define _GNU_SOURCE 1
#include "sessionstore.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static bool hex_id(const char *s) {
	if (!s || strlen(s) != 32)
		return false;
	for (int i = 0; i < 32; i++)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
			return false;
	return true;
}
bool session_random_id(char out[33]) {
	unsigned char bytes[16];
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	bool ok = fd >= 0 && read(fd, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes);
	if (fd >= 0)
		close(fd);
	if (!ok)
		return false;
	const char *digits = "0123456789abcdef";
	for (int i = 0; i < 16; i++) {
		out[i * 2] = digits[bytes[i] >> 4];
		out[i * 2 + 1] = digits[bytes[i] & 15];
	}
	out[32] = 0;
	return true;
}
static bool safe_parent(const char *path) {
	char copy[1500];
	if (!path || snprintf(copy, sizeof(copy), "%s", path) >= (int)sizeof(copy))
		return false;
	for (char *p = copy + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			struct stat st;
			bool ok = lstat(copy, &st) == 0 && S_ISDIR(st.st_mode);
			*p = '/';
			if (!ok)
				return false;
		}
	return true;
}
static bool regular(const char *path) {
	struct stat st;
	return !lstat(path, &st) && S_ISREG(st.st_mode) && st.st_nlink == 1;
}
bool session_file_identity(const char *path, session_identity_t *id) {
	struct stat st;
	if (!safe_parent(path) || !regular(path) || lstat(path, &st))
		return false;
	*id = (session_identity_t){.device = st.st_dev, .inode = st.st_ino, .bytes = st.st_size, .seconds = st.st_mtim.tv_sec, .nanoseconds = st.st_mtim.tv_nsec};
	return true;
}
bool session_file_matches(const char *path, const session_identity_t *id) {
	session_identity_t current;
	return session_file_identity(path, &current) && current.device == id->device && current.inode == id->inode && current.bytes == id->bytes && current.seconds == id->seconds && current.nanoseconds == id->nanoseconds;
}
static bool sync_parent(const char *path) {
	char dir[1400];
	if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir))
		return false;
	char *slash = strrchr(dir, '/');
	if (slash)
		*slash = 0;
	else
		strcpy(dir, ".");
	int fd = open(*dir ? dir : "/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	bool ok = fd >= 0 && fsync(fd) == 0;
	if (fd >= 0)
		close(fd);
	return ok;
}
static bool directory(const char *path, bool create) {
	struct stat st;
	if (lstat(path, &st))
		return create && errno == ENOENT && mkdir(path, 0700) == 0;
	return S_ISDIR(st.st_mode);
}
static bool session_dir(const char *root, bool create, char out[1200]) {
	if (!root || !*root || snprintf(out, 1200, "%s/.local", root) >= 1200 || !safe_parent(out) || !directory(out, create))
		return false;
	size_t len = strlen(out);
	if (len + 11 >= 1200)
		return false;
	strcpy(out + len, "/navidrome");
	return directory(out, create);
}
bool session_card(const char *root, bool create, char out[33]) {
	char dir[1200], path[1300];
	if (!session_dir(root, create, dir))
		return false;
	snprintf(path, sizeof(path), "%s/session-card", dir);
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	char value[34] = {0};
	if (fd >= 0) {
		bool ok = read(fd, value, sizeof(value)) == 32 && regular(path);
		close(fd);
		if (ok && hex_id(value)) {
			memcpy(out, value, 33);
			return true;
		}
		return false;
	}
	if (!create || errno != ENOENT || !session_random_id(value))
		return false;
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return false;
	bool ok = write(fd, value, 32) == 32 && fsync(fd) == 0;
	if (close(fd))
		ok = false;
	if (ok)
		ok = sync_parent(path);
	if (ok)
		memcpy(out, value, 33);
	return ok;
}
bool session_path(const char *root, const char *key, char *out, size_t size) {
	if (!root || !*root || !hex_id(key))
		return false;
	int n = snprintf(out, size, "%s/.local/navidrome/session-%s.db", root, key);
	return n > 0 && (size_t)n < size;
}
bool session_relative_path(const char *path) {
	if (!path || !*path || *path == '/' || strchr(path, ':') || strchr(path, '\\'))
		return false;
	const char *p = path;
	while (*p) {
		const char *end = strchr(p, '/');
		size_t n = end ? (size_t)(end - p) : strlen(p);
		if (!n || (n == 1 && *p == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
			return false;
		if (!end)
			break;
		p = end + 1;
		if (!*p)
			return false;
	}
	return true;
}
static bool header_valid(const session_header_t *h, bool payload) {
	if (h->version != SESSION_FORMAT || h->owner < SESSION_NONE || h->owner > SESSION_NAVIDROME || h->ready < 0 || h->ready > 1 || h->activation <= 0 || h->revision < 0 || !isfinite(h->position) || h->position < 0)
		return false;
	if (h->owner == SESSION_NONE)
		return !payload && !h->ready;
	if (h->ready && (h->mode < 0 || h->mode > 4 || h->count <= 0 || h->count > SESSION_BYTES / 8 || h->selected < 0 || h->selected >= h->count || !hex_id(h->card)))
		return false;
	if (h->ready && h->owner == SESSION_NAVIDROME && (!hex_id(h->scope) || !hex_id(h->key)))
		return false;
	return !payload || (h->ready && hex_id(h->key));
}
static bool sql(sqlite3 *db, const char *s) { return sqlite3_exec(db, s, NULL, NULL, NULL) == SQLITE_OK; }
static bool open_db(const char *path, bool write, sqlite3 **db) {
	if (!safe_parent(path) || !regular(path))
		return false;
	int flags = (write ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY) | SQLITE_OPEN_NOFOLLOW;
	if (sqlite3_open_v2(path, db, flags, NULL) != SQLITE_OK)
		return false;
	sqlite3_limit(*db, SQLITE_LIMIT_LENGTH, 8192);
	return sql(*db, "PRAGMA trusted_schema=OFF; PRAGMA cache_size=-64; PRAGMA mmap_size=0;");
}
static bool text_col(sqlite3_stmt *st, int c, char *out, size_t size) {
	if (sqlite3_column_type(st, c) != SQLITE_TEXT)
		return false;
	const char *v = (const char *)sqlite3_column_text(st, c);
	int n = sqlite3_column_bytes(st, c);
	if (!v || n < 0 || (size_t)n >= size || strlen(v) != (size_t)n)
		return false;
	memcpy(out, v, (size_t)n + 1);
	return true;
}
static bool get_header(sqlite3 *db, session_header_t *h) {
	sqlite3_stmt *st = NULL;
	memset(h, 0, sizeof(*h));
	bool ok = sqlite3_prepare_v2(db, "SELECT version,owner,ready,activation,revision,key,card,scope,mode,count,selected,position FROM header WHERE id=1", -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW;
	if (ok) {
		const int bounded[] = {0, 1, 2, 8, 9, 10};
		for (size_t i = 0; i < sizeof(bounded) / sizeof(bounded[0]); i++) {
			sqlite3_int64 value = sqlite3_column_int64(st, bounded[i]);
			if (sqlite3_column_type(st, bounded[i]) != SQLITE_INTEGER || value < INT32_MIN || value > INT32_MAX)
				ok = false;
		}
		if (sqlite3_column_type(st, 3) != SQLITE_INTEGER || sqlite3_column_type(st, 4) != SQLITE_INTEGER)
			ok = false;
	}
	if (ok) {
		h->version = sqlite3_column_int(st, 0);
		h->owner = sqlite3_column_int(st, 1);
		h->ready = sqlite3_column_int(st, 2);
		h->activation = sqlite3_column_int64(st, 3);
		h->revision = sqlite3_column_int64(st, 4);
		ok = text_col(st, 5, h->key, sizeof(h->key)) && text_col(st, 6, h->card, sizeof(h->card)) && text_col(st, 7, h->scope, sizeof(h->scope));
		h->mode = sqlite3_column_int(st, 8);
		h->count = sqlite3_column_int(st, 9);
		h->selected = sqlite3_column_int(st, 10);
		h->position = sqlite3_column_double(st, 11);
		ok = ok && (sqlite3_column_type(st, 11) == SQLITE_INTEGER || sqlite3_column_type(st, 11) == SQLITE_FLOAT) && header_valid(h, false) && sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	return ok;
}
static bool put_header(sqlite3 *db, const session_header_t *h) {
	sqlite3_stmt *st = NULL;
	bool ok = sql(db, "CREATE TABLE IF NOT EXISTS header(id INTEGER PRIMARY KEY CHECK(id=1),version INT,owner INT,ready INT,activation INTEGER,revision INTEGER,key TEXT,card TEXT,scope TEXT,mode INT,count INT,selected INT,position REAL);") && sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO header VALUES(1,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK;
	if (ok) {
		sqlite3_bind_int(st, 1, h->version);
		sqlite3_bind_int(st, 2, h->owner);
		sqlite3_bind_int(st, 3, h->ready);
		sqlite3_bind_int64(st, 4, h->activation);
		sqlite3_bind_int64(st, 5, h->revision);
		sqlite3_bind_text(st, 6, h->key, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 7, h->card, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 8, h->scope, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(st, 9, h->mode);
		sqlite3_bind_int(st, 10, h->count);
		sqlite3_bind_int(st, 11, h->selected);
		sqlite3_bind_double(st, 12, h->position);
		ok = sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	return ok;
}
int session_journal_read(const char *path, session_header_t *out) {
	struct stat st;
	if (lstat(path, &st))
		return errno == ENOENT ? 0 : -1;
	if (st.st_size > 31 * 4096)
		return -1;
	sqlite3 *db = NULL;
	bool ok = open_db(path, false, &db) && get_header(db, out);
	if (db)
		sqlite3_close(db);
	db = NULL;
	// A power loss may leave a hot rollback journal. Recover it on the worker,
	// without recreating schema or interpreting an unknown version as empty.
	if (!ok) {
		ok = open_db(path, true, &db) && get_header(db, out);
		if (db)
			sqlite3_close(db);
	}
	return ok ? 1 : -1;
}
bool session_journal_write(const char *path, const session_header_t *h) {
	if (!safe_parent(path) || !header_valid(h, false))
		return false;
	char lock_path[1500], temp[1500];
	if (snprintf(lock_path, sizeof(lock_path), "%s.lock", path) >= (int)sizeof(lock_path) || snprintf(temp, sizeof(temp), "%s.new", path) >= (int)sizeof(temp))
		return false;
	struct stat existing;
	if (!lstat(path, &existing) && (!S_ISREG(existing.st_mode) || existing.st_nlink != 1))
		return false;
	int lock_fd = open(lock_path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	struct stat lock_st;
	if (lock_fd < 0)
		return false;
	bool ok = !fstat(lock_fd, &lock_st) && S_ISREG(lock_st.st_mode) && lock_st.st_nlink == 1 && flock(lock_fd, LOCK_EX | LOCK_NB) == 0;
	struct stat st;
	session_header_t previous;
	if (ok && !lstat(path, &st)) {
		ok = session_journal_read(path, &previous) == 1;
		if (ok)
			ok = h->activation > previous.activation || (h->activation == previous.activation && h->revision >= previous.revision);
	} else if (ok && errno != ENOENT)
		ok = false;
	// Only this feature's private, never-published staging file is replaceable.
	if (ok && !lstat(temp, &st))
		ok = S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_uid == geteuid() && unlink(temp) == 0;
	else if (ok && errno != ENOENT)
		ok = false;
	int fd = -1;
	bool created = false;
	sqlite3 *db = NULL;
	if (ok) {
		fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		ok = fd >= 0;
		created = ok;
		if (fd >= 0)
			close(fd);
	}
	if (ok)
		ok = open_db(temp, true, &db) && sql(db, "PRAGMA page_size=4096; PRAGMA max_page_count=31; PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; BEGIN;") && put_header(db, h) && sql(db, "COMMIT;");
	if (db)
		sqlite3_close(db);
	if (ok) {
		fd = open(temp, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		ok = fd >= 0 && fsync(fd) == 0;
		if (fd >= 0)
			close(fd);
	}
	if (ok)
		ok = rename(temp, path) == 0;
	if (ok)
		ok = sync_parent(path);
	// After rename an uncertain directory sync never deletes the visible record.
	if (!ok && created && regular(temp))
		unlink(temp);
	close(lock_fd);
	return ok;
}
static bool row_valid(const session_row_t *r) { return r->track.saved_occurrence >= 0 && (r->local[0] ? session_relative_path(r->local) : r->track.row.id[0] != 0); }
static bool put_row(sqlite3_stmt *st, int canonical, int dealt, const session_row_t *r) {
	if (!row_valid(r))
		return false;
	const char *v[] = {r->local, r->track.row.id, r->track.row.title, r->track.row.artist, r->track.row.cover, r->track.album_id, r->track.album, r->track.artist_id, r->track.genre};
	sqlite3_bind_int(st, 1, canonical);
	sqlite3_bind_int(st, 2, dealt);
	for (int i = 0; i < 9; i++)
		sqlite3_bind_text(st, i + 3, v[i], -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(st, 12, r->track.row.duration);
	sqlite3_bind_int(st, 13, r->track.row.disc);
	sqlite3_bind_int(st, 14, r->track.row.track);
	sqlite3_bind_int(st, 15, r->track.year);
	sqlite3_bind_int(st, 16, r->track.row.favorite);
	sqlite3_bind_int(st, 17, r->track.row.favorite_known);
	sqlite3_bind_text(st, 18, r->track.album_artist, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 19, r->track.saved_occurrence);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_reset(st);
	sqlite3_clear_bindings(st);
	return ok;
}
static bool same_file(const char *path, const struct stat *identity) {
	struct stat st;
	return regular(path) && !lstat(path, &st) && st.st_dev == identity->st_dev && st.st_ino == identity->st_ino;
}
static bool reopen(const char *path, bool write, sqlite3 **db, const struct stat *identity, bool (*renew)(void *), void *lease) {
	if (*db) {
		sqlite3_close(*db);
		*db = NULL;
	}
	bool ok = (!renew || renew(lease)) && same_file(path, identity);
	if (ok && !write) {
		struct stat st;
		ok = !lstat(path, &st) && st.st_size == identity->st_size && st.st_mtim.tv_sec == identity->st_mtim.tv_sec && st.st_mtim.tv_nsec == identity->st_mtim.tv_nsec;
	}
	return ok && open_db(path, write, db);
}
bool session_payload_write(const char *path, const session_header_t *h, const uint32_t *order, session_read_cb read_row, void *user) { return session_payload_write_scoped(path, h, order, read_row, user, NULL, NULL); }
bool session_payload_write_scoped(const char *path, const session_header_t *h, const uint32_t *order, session_read_cb read_row, void *user, bool (*renew)(void *), void *lease) {
	if (!safe_parent(path) || !header_valid(h, true) || !order || !read_row || (size_t)h->count > SIZE_MAX / sizeof(uint32_t))
		return false;
	uint32_t *inverse = malloc((size_t)h->count * sizeof(*inverse));
	if (!inverse)
		return false;
	for (int i = 0; i < h->count; i++)
		inverse[i] = UINT32_MAX;
	for (int i = 0; i < h->count; i++) {
		if (order[i] >= (uint32_t)h->count || inverse[order[i]] != UINT32_MAX) {
			free(inverse);
			return false;
		}
		inverse[order[i]] = (uint32_t)i;
	}
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	struct stat identity = {0};
	if (fd < 0) {
		free(inverse);
		return false;
	}
	bool ok = fstat(fd, &identity) == 0;
	bool retired = false;
	close(fd);
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	ok = ok && open_db(path, true, &db);
	for (int start = 0; ok && start < h->count; start += 128) {
		if (start) {
			ok = reopen(path, true, &db, &identity, renew, lease);
			if (!ok)
				retired = true;
		}
		if (ok)
			ok = sql(db, "PRAGMA page_size=4096; PRAGMA max_page_count=8192; PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; BEGIN;");
		if (ok && !start)
			ok = put_header(db, h) && sql(db, "CREATE TABLE entry(canonical INTEGER PRIMARY KEY,dealt INT UNIQUE,local TEXT,id TEXT,title TEXT,artist TEXT,cover TEXT,album_id TEXT,album TEXT,artist_id TEXT,genre TEXT,duration INT,disc INT,track INT,year INT,favorite INT,known INT,album_artist TEXT,saved_occurrence INT);");
		if (ok)
			ok = sqlite3_prepare_v2(db, "INSERT INTO entry VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK;
		int end = h->count - start > 128 ? start + 128 : h->count;
		for (int i = start; ok && i < end; i++) {
			session_row_t row = {0};
			ok = read_row(user, i, &row) && put_row(st, i, (int)inverse[i], &row);
		}
		sqlite3_finalize(st);
		st = NULL;
		if (ok)
			ok = sql(db, "COMMIT;");
	}
	free(inverse);
	if (db)
		sqlite3_close(db);
	fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (ok)
		ok = fd >= 0 && fsync(fd) == 0;
	if (fd >= 0)
		close(fd);
	struct stat info;
	if (ok)
		ok = same_file(path, &identity) && !stat(path, &info) && info.st_size <= SESSION_BYTES && sync_parent(path);
	if (!ok && !retired && same_file(path, &identity))
		unlink(path);
	return ok;
}
bool session_payload_row(sqlite3 *db, int canonical, session_row_t *r) {
	sqlite3_stmt *st = NULL;
	memset(r, 0, sizeof(*r));
	int rc = sqlite3_prepare_v2(db, "SELECT local,id,title,artist,cover,album_id,album,artist_id,genre,duration,disc,track,year,favorite,known,album_artist,saved_occurrence FROM entry WHERE canonical=?", -1, &st, NULL);
	bool extended = rc == SQLITE_OK;
	// Earlier format-1 snapshots predate the additive sorting credit fields.
	if (rc == SQLITE_ERROR) {
		sqlite3_finalize(st);
		st = NULL;
		rc = sqlite3_prepare_v2(db, "SELECT local,id,title,artist,cover,album_id,album,artist_id,genre,duration,disc,track,year,favorite,known FROM entry WHERE canonical=?", -1, &st, NULL);
	}
	bool ok = rc == SQLITE_OK;
	if (ok) {
		sqlite3_bind_int(st, 1, canonical);
		ok = sqlite3_step(st) == SQLITE_ROW;
	}
	if (ok) {
		char *v[] = {r->local, r->track.row.id, r->track.row.title, r->track.row.artist, r->track.row.cover, r->track.album_id, r->track.album, r->track.artist_id, r->track.genre};
		size_t sizes[] = {sizeof(r->local), sizeof(r->track.row.id), sizeof(r->track.row.title), sizeof(r->track.row.artist), sizeof(r->track.row.cover), sizeof(r->track.album_id), sizeof(r->track.album), sizeof(r->track.artist_id), sizeof(r->track.genre)};
		for (int i = 0; ok && i < 9; i++)
			ok = text_col(st, i, v[i], sizes[i]);
		r->track.row.duration = sqlite3_column_int(st, 9);
		r->track.row.disc = sqlite3_column_int(st, 10);
		r->track.row.track = sqlite3_column_int(st, 11);
		r->track.year = sqlite3_column_int(st, 12);
		r->track.row.favorite = sqlite3_column_int(st, 13) != 0;
		r->track.row.favorite_known = sqlite3_column_int(st, 14) != 0;
		for (int i = 9; ok && i <= 14; i++) {
			sqlite3_int64 value = sqlite3_column_int64(st, i);
			ok = sqlite3_column_type(st, i) == SQLITE_INTEGER && value >= INT32_MIN && value <= INT32_MAX;
			if (i >= 13)
				ok = ok && (value == 0 || value == 1);
		}
		if (ok && extended) {
			ok = text_col(st, 15, r->track.album_artist, sizeof(r->track.album_artist)) && sqlite3_column_type(st, 16) == SQLITE_INTEGER && sqlite3_column_int64(st, 16) >= 0;
			r->track.saved_occurrence = sqlite3_column_int64(st, 16);
		}
		ok = ok && row_valid(r) && sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	return ok;
}
bool session_payload_open(const char *path, const session_header_t *expected, sqlite3 **db, uint32_t **order) { return session_payload_open_scoped(path, expected, db, order, NULL, NULL); }
bool session_payload_open_scoped(const char *path, const session_header_t *expected, sqlite3 **db, uint32_t **order, bool (*renew)(void *), void *lease) {
	*db = NULL;
	*order = NULL;
	struct stat identity;
	session_header_t got;
	if (!header_valid(expected, true))
		return false;
	if (!regular(path) || lstat(path, &identity) || identity.st_size > SESSION_BYTES || !open_db(path, false, db) || !get_header(*db, &got))
		goto fail;
	if (!header_valid(&got, true) || got.activation != expected->activation || got.revision != expected->revision || got.owner != expected->owner || got.count != expected->count || got.mode != expected->mode || strcmp(got.key, expected->key) || strcmp(got.card, expected->card) || strcmp(got.scope, expected->scope))
		goto fail;
	if ((size_t)got.count > SIZE_MAX / sizeof(uint32_t))
		goto fail;
	sqlite3_stmt *st = NULL;
	bool ok = sqlite3_prepare_v2(*db, "SELECT count(*) FROM entry", -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int64(st, 0) == got.count;
	sqlite3_finalize(st);
	st = NULL;
	if (!ok)
		goto fail;
	*order = malloc((size_t)got.count * sizeof(**order));
	unsigned char *seen = calloc((size_t)got.count, 1);
	ok = *order && seen;
	for (int start = 0; ok && start < got.count; start += 128) {
		if (start)
			ok = reopen(path, false, db, &identity, renew, lease);
		if (ok)
			ok = sqlite3_prepare_v2(*db, "SELECT canonical,dealt FROM entry WHERE dealt>=? ORDER BY dealt LIMIT 128", -1, &st, NULL) == SQLITE_OK;
		if (ok)
			sqlite3_bind_int(st, 1, start);
		int end = got.count - start > 128 ? start + 128 : got.count;
		for (int i = start; ok && i < end; i++) {
			ok = sqlite3_step(st) == SQLITE_ROW;
			if (!ok)
				break;
			sqlite3_int64 c = sqlite3_column_int64(st, 0), d = sqlite3_column_int64(st, 1);
			ok = c >= 0 && c < got.count && d == i && !seen[c];
			if (ok) {
				seen[c] = 1;
				(*order)[i] = (uint32_t)c;
			}
		}
		sqlite3_finalize(st);
		st = NULL;
	}
	free(seen);
	for (int start = 0; ok && start < got.count; start += 128) {
		ok = reopen(path, false, db, &identity, renew, lease);
		int end = got.count - start > 128 ? start + 128 : got.count;
		for (int i = start; ok && i < end; i++) {
			session_row_t row;
			ok = session_payload_row(*db, i, &row);
		}
	}
	if (ok)
		return true;
fail:
	free(*order);
	*order = NULL;
	if (*db)
		sqlite3_close(*db);
	*db = NULL;
	return false;
}
bool session_payload_budget(const char *root, const char *keep, bool (*pinned)(const char *), bool reserve) {
	char dir[1200];
	if (!session_dir(root, true, dir))
		return false;
	DIR *d = opendir(dir);
	if (!d)
		return false;
	uint64_t bytes = 0;
	bool ok = true;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strlen(e->d_name) != 43 || strncmp(e->d_name, "session-", 8) || strcmp(e->d_name + 40, ".db"))
			continue;
		char key[33];
		memcpy(key, e->d_name + 8, 32);
		key[32] = 0;
		if (!hex_id(key))
			continue;
		char path[1500];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		struct stat st;
		if (lstat(path, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
			ok = false;
			continue;
		}
		if ((keep && strcmp(key, keep) == 0) || (pinned && pinned(path))) {
			bytes += (uint64_t)st.st_size;
			continue;
		}
		if (unlink(path))
			ok = false;
	}
	closedir(d);
	return ok && bytes + (reserve ? SESSION_BYTES : 0) <= 2ull * SESSION_BYTES;
}

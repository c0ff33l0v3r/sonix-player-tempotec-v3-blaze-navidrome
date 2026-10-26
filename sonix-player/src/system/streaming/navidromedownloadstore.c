#define _GNU_SOURCE
#include "navidromedownloadstore.h"
#include "src/system/core/sha256.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <stdatomic.h>

static atomic_uint availability_revision = 1;
unsigned nd_dl_availability_revision(void) { return atomic_load(&availability_revision); }
void nd_dl_availability_changed(void) { atomic_fetch_add(&availability_revision, 1); }

void nd_dl_bind(sqlite3_stmt *s, int at, const char *text) { sqlite3_bind_text(s, at, text ? text : "", -1, SQLITE_TRANSIENT); }
bool nd_dl_sql(nd_dl_store_t *s, const char *sql) {
	if (sqlite3_exec(s->db, sql, NULL, NULL, NULL) == SQLITE_OK)
		return true;
	s->error = "nd_dl_store_error";
	return false;
}
bool nd_dl_prepare(nd_dl_store_t *s, sqlite3_stmt **st, const char *sql) {
	if (sqlite3_prepare_v2(s->db, sql, -1, st, NULL) == SQLITE_OK)
		return true;
	s->error = "nd_dl_store_error";
	return false;
}
static void column(sqlite3_stmt *s, int at, char *out, size_t cap) {
	const char *v = (const char *)sqlite3_column_text(s, at);
	snprintf(out, cap, "%s", v ? v : "");
}
bool nd_dl_paths(const nd_library_t *lib, char root[ND_DL_PATH], char path[1200]) {
	if (!lib || strlen(lib->scope) != 32 || strspn(lib->scope, "0123456789abcdef") != 32)
		return false;
	const char *slash = strrchr(lib->path, '/');
	if (!slash)
		return false;
	size_t parent = slash - lib->path;
	if (parent >= 7 && !memcmp(lib->path + parent - 7, "/.cache", 7))
		parent -= 7;
	const char tail[] = "/.local/navidrome";
	if (parent < sizeof(tail) - 1 || memcmp(lib->path + parent - sizeof(tail) + 1, tail, sizeof(tail) - 1))
		return false;
	size_t n = parent - (sizeof(tail) - 1);
	if (!n || n >= ND_DL_PATH)
		return false;
	memcpy(root, lib->path, n);
	root[n] = 0;
	return snprintf(path, 1200, "%.*s/downloads-%s.db", (int)parent, lib->path, lib->scope) < 1200;
}
bool nd_dl_open(nd_dl_store_t *s, const nd_library_t *lib, bool write) {
	memset(s, 0, sizeof(*s));
	s->error = "nd_dl_store_error";
	if (!nd_dl_paths(lib, s->root, s->path))
		return false;
	int directory = nd_dl_directory(s->root, ".local/navidrome", write);
	if (directory < 0)
		return false;
	close(directory);
	int flags = (write ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READONLY) | SQLITE_OPEN_NOFOLLOW;
	if (sqlite3_open_v2(s->path, &s->db, flags, NULL) != SQLITE_OK)
		return false;
	sqlite3_busy_timeout(s->db, 1000);
	if (!nd_dl_sql(s, "PRAGMA foreign_keys=ON; PRAGMA cache_size=-256; PRAGMA temp_store=FILE; PRAGMA synchronous=FULL"))
		return false;
	sqlite3_stmt *st = NULL;
	if (!nd_dl_prepare(s, &st, "PRAGMA user_version"))
		return false;
	int version = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
	sqlite3_finalize(st);
	if (version == 0 && write) {
		// Never adopt an unrelated version-zero database or rebuild corruption.
		if (!nd_dl_prepare(s, &st, "SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'"))
			return false;
		bool empty = sqlite3_step(st) == SQLITE_ROW && !sqlite3_column_int(st, 0);
		sqlite3_finalize(st);
		if (!empty || !nd_dl_sql(s, "BEGIN IMMEDIATE; CREATE TABLE binding(scope TEXT PRIMARY KEY);"
									"CREATE TABLE batch(id INTEGER PRIMARY KEY,destination TEXT NOT NULL,created INT NOT NULL,skipped INT NOT NULL DEFAULT 0,admitted INT NOT NULL DEFAULT 0);"
									"CREATE TABLE job(key INTEGER PRIMARY KEY,batch INT REFERENCES batch(id),track TEXT NOT NULL,albumid TEXT,title TEXT,artist TEXT,album TEXT,cover TEXT,disc INT,number INT,multi INT,state INT NOT NULL,"
									"destination TEXT,albumartist TEXT DEFAULT '',path TEXT DEFAULT '',partial TEXT DEFAULT '',digest TEXT DEFAULT '',extension TEXT DEFAULT '',error TEXT DEFAULT '',bytes INT DEFAULT 0,total INT DEFAULT 0,mtime INT DEFAULT 0,inode INT DEFAULT 0);"
									"CREATE INDEX job_identity ON job(track,state); CREATE INDEX job_schedule ON job(state,key);"
									"CREATE TABLE placement(album TEXT,destination TEXT,artist TEXT,path TEXT,multi INT,PRIMARY KEY(album,destination)); PRAGMA user_version=1;"))
			return false;
		if (!nd_dl_prepare(s, &st, "INSERT INTO binding VALUES(?)"))
			return false;
		nd_dl_bind(st, 1, lib->scope);
		bool ok = sqlite3_step(st) == SQLITE_DONE;
		sqlite3_finalize(st);
		if (!ok || !nd_dl_sql(s, "COMMIT"))
			return false;
	} else if (version != 1)
		return false;
	if (!nd_dl_prepare(s, &st, "SELECT scope FROM binding"))
		return false;
	bool ok = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0) && !strcmp((const char *)sqlite3_column_text(st, 0), lib->scope) && sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	if (!ok || !nd_dl_prepare(s, &st, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='history_hidden'"))
		return false;
	int rc = sqlite3_step(st);
	s->history = rc == SQLITE_ROW;
	ok = rc == SQLITE_ROW || rc == SQLITE_DONE;
	sqlite3_finalize(st);
	if (ok && write && !s->history) {
		ok = nd_dl_sql(s, "CREATE TABLE history_hidden(kind INT NOT NULL,key INTEGER NOT NULL,PRIMARY KEY(kind,key))");
		s->history = ok;
	}
	if (ok)
		s->error = NULL;
	return ok;
}
void nd_dl_close(nd_dl_store_t *s) {
	if (s->db) {
		sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
		sqlite3_close(s->db);
	}
	s->db = NULL;
}
static const char *columns = "key,batch,track,albumid,title,artist,album,cover,disc,number,multi,state,destination,albumartist,path,partial,digest,extension,error,bytes,total,mtime,inode";
static void decode(sqlite3_stmt *st, nd_dl_job_t *j) {
	memset(j, 0, sizeof(*j));
	j->key = sqlite3_column_int64(st, 0);
	j->batch = sqlite3_column_int64(st, 1);
	column(st, 2, j->source.row.id, sizeof(j->source.row.id));
	column(st, 3, j->source.album_id, sizeof(j->source.album_id));
	column(st, 4, j->source.row.title, sizeof(j->source.row.title));
	column(st, 5, j->source.row.artist, sizeof(j->source.row.artist));
	column(st, 6, j->source.album, sizeof(j->source.album));
	column(st, 7, j->source.row.cover, sizeof(j->source.row.cover));
	j->source.row.disc = sqlite3_column_int(st, 8);
	j->source.row.track = sqlite3_column_int(st, 9);
	j->multi = sqlite3_column_int(st, 10);
	j->state = sqlite3_column_int(st, 11);
	column(st, 12, j->destination, sizeof(j->destination));
	column(st, 13, j->album_artist, sizeof(j->album_artist));
	column(st, 14, j->path, sizeof(j->path));
	column(st, 15, j->partial, sizeof(j->partial));
	column(st, 16, j->digest, sizeof(j->digest));
	column(st, 17, j->extension, sizeof(j->extension));
	column(st, 18, j->error, sizeof(j->error));
	j->bytes = sqlite3_column_int64(st, 19);
	j->total = sqlite3_column_int64(st, 20);
	j->mtime = sqlite3_column_int64(st, 21);
	j->inode = sqlite3_column_int64(st, 22);
}
static bool query_one(nd_dl_store_t *s, const char *where, int64_t key, const char *id, nd_dl_job_t *j) {
	char sql[900];
	snprintf(sql, sizeof(sql), "SELECT %s FROM job WHERE %s", columns, where);
	sqlite3_stmt *st = NULL;
	if (!nd_dl_prepare(s, &st, sql))
		return false;
	if (id)
		nd_dl_bind(st, 1, id);
	else if (key)
		sqlite3_bind_int64(st, 1, key);
	bool found = sqlite3_step(st) == SQLITE_ROW;
	if (found)
		decode(st, j);
	sqlite3_finalize(st);
	return found;
}
bool nd_dl_get(nd_dl_store_t *s, int64_t key, nd_dl_job_t *j) { return query_one(s, "key=?", key, NULL, j); }
bool nd_dl_next(nd_dl_store_t *s, nd_dl_job_t *j) { return query_one(s, "state=0 ORDER BY key LIMIT 1", 0, NULL, j); }
bool nd_dl_find(nd_dl_store_t *s, const char *id, nd_dl_job_t *j) { return query_one(s, "track=? AND state=4 ORDER BY key DESC LIMIT 1", 0, id, j); }

bool nd_dl_mapping_valid(const char *root, const nd_dl_job_t *j, char path[ND_DL_PATH], bool (*media)(const char *), bool (*current)(void *), void *user) {
	const char *suffix = strrchr(j->path, '.');
	size_t prefix = strlen(j->destination);
	if (!nd_dl_destination(j->destination) || j->path[0] == '.' || strstr(j->path, "/.") || strlen(j->digest) != 64 || strspn(j->digest, "0123456789abcdef") != 64 || !suffix || !j->extension[0] || strcmp(suffix, j->extension) || (prefix && (strncmp(j->path, j->destination, prefix) || j->path[prefix] != '/')))
		return false;
	char relative[ND_DL_PATH];
	snprintf(relative, sizeof(relative), "%s", j->path);
	char *leaf = strrchr(relative, '/');
	if (!leaf || j->bytes <= 0 || (current && !current(user)))
		return false;
	*leaf++ = 0;
	int dir = nd_dl_directory(root, relative, false);
	struct stat st;
	bool ok = dir >= 0 && !fstatat(dir, leaf, &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode) && st.st_size == j->bytes;
	if (ok && (st.st_mtime != j->mtime || (int64_t)st.st_ino != j->inode)) {
		char digest[65];
		ok = nd_dl_fingerprint(dir, leaf, digest, &st, current, user) && !strcmp(digest, j->digest);
	}
	if (ok)
		ok = snprintf(path, ND_DL_PATH, "%s/%s", root, j->path) < ND_DL_PATH && media(path);
	if (dir >= 0)
		close(dir);
	return ok && (!current || current(user));
}

int nd_dl_probe(nd_dl_store_t *s, const char *id, bool (*media)(const char *), bool (*current)(void *), void *user) {
	char sql[900];
	snprintf(sql, sizeof(sql), "SELECT %s FROM job WHERE track=? AND state=4 ORDER BY key DESC", columns);
	sqlite3_stmt *st = NULL;
	if (!nd_dl_prepare(s, &st, sql))
		return -1;
	nd_dl_bind(st, 1, id);
	int result = 0, rc;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		if (current && !current(user)) { result = -1; break; }
		nd_dl_job_t job;
		char path[ND_DL_PATH];
		decode(st, &job);
		errno = 0;
		if (nd_dl_mapping_valid(s->root, &job, path, media, current, user)) { result = 1; break; }
		if (errno == EACCES || errno == EIO || errno == EMFILE || errno == ENFILE || errno == ENOMEM)
			result = -1;
	}
	if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || (current && !current(user)))
		result = -1;
	sqlite3_finalize(st);
	return result;
}
bool nd_dl_insert(nd_dl_store_t *s, int64_t batch, const char *dest, const nd_playrow_t *r, int multi, int *added, int *skipped) {
	sqlite3_stmt *st = NULL;
	if (!nd_dl_prepare(s, &st, "SELECT 1 FROM job WHERE track=? AND state NOT IN(6,7) LIMIT 1"))
		return false;
	nd_dl_bind(st, 1, r->row.id);
	bool duplicate = sqlite3_step(st) == SQLITE_ROW;
	sqlite3_finalize(st);
	if (duplicate) {
		if (*skipped == INT32_MAX)
			return false;
		(*skipped)++;
		return true;
	}
	if (!nd_dl_prepare(s, &st, "INSERT INTO job(batch,track,albumid,title,artist,album,cover,disc,number,multi,state,destination) VALUES(?,?,?,?,?,?,?,?,?,?,0,?)"))
		return false;
	sqlite3_bind_int64(st, 1, batch);
	nd_dl_bind(st, 2, r->row.id);
	nd_dl_bind(st, 3, r->album_id);
	nd_dl_bind(st, 4, r->row.title);
	nd_dl_bind(st, 5, r->row.artist);
	nd_dl_bind(st, 6, r->album);
	nd_dl_bind(st, 7, r->row.cover);
	sqlite3_bind_int(st, 8, r->row.disc > 0 ? r->row.disc : 1);
	sqlite3_bind_int(st, 9, r->row.track);
	sqlite3_bind_int(st, 10, multi);
	nd_dl_bind(st, 11, dest);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	if (ok && *added == INT32_MAX)
		return false;
	if (ok)
		(*added)++;
	return ok;
}
bool nd_dl_update(nd_dl_store_t *s, const nd_dl_job_t *j) {
	sqlite3_stmt *st = NULL;
	if (!nd_dl_prepare(s, &st, "UPDATE job SET state=?,albumartist=?,path=?,partial=?,digest=?,extension=?,error=?,bytes=?,total=?,mtime=?,inode=?,multi=?,albumid=?,title=?,artist=?,album=?,cover=?,disc=?,number=? WHERE key=? AND track=?"))
		return false;
	sqlite3_bind_int(st, 1, j->state);
	nd_dl_bind(st, 2, j->album_artist);
	nd_dl_bind(st, 3, j->path);
	nd_dl_bind(st, 4, j->partial);
	nd_dl_bind(st, 5, j->digest);
	nd_dl_bind(st, 6, j->extension);
	nd_dl_bind(st, 7, j->error);
	sqlite3_bind_int64(st, 8, j->bytes);
	sqlite3_bind_int64(st, 9, j->total);
	sqlite3_bind_int64(st, 10, j->mtime);
	sqlite3_bind_int64(st, 11, j->inode);
	sqlite3_bind_int(st, 12, j->multi);
	nd_dl_bind(st, 13, j->source.album_id);
	nd_dl_bind(st, 14, j->source.row.title);
	nd_dl_bind(st, 15, j->source.row.artist);
	nd_dl_bind(st, 16, j->source.album);
	nd_dl_bind(st, 17, j->source.row.cover);
	sqlite3_bind_int(st, 18, j->source.row.disc);
	sqlite3_bind_int(st, 19, j->source.row.track);
	sqlite3_bind_int64(st, 20, j->key);
	nd_dl_bind(st, 21, j->source.row.id);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	return ok;
}
const char *nd_dl_history_visible(const nd_dl_store_t *s) { return s->history ? "NOT EXISTS(SELECT 1 FROM history_hidden WHERE kind=0 AND key=job.key)" : "1"; }
bool nd_dl_clear_history(nd_dl_store_t *s) {
	return s->history && nd_dl_sql(s, "INSERT OR IGNORE INTO history_hidden SELECT 0,key FROM job WHERE state IN(4,6,7);"
									  "INSERT OR IGNORE INTO history_hidden SELECT 1,id FROM batch WHERE NOT EXISTS(SELECT 1 FROM job WHERE job.batch=batch.id AND state NOT IN(4,6,7));");
}
bool nd_dl_window(nd_dl_store_t *s, int offset, nd_dl_job_t jobs[ND_WINDOW], int *count, int *total, int counts[9]) { return nd_dl_window_filter(s, offset, NDDL_ALL, jobs, count, total, counts); }
bool nd_dl_window_filter(nd_dl_store_t *s, int offset, nd_dl_filter_t filter, nd_dl_job_t jobs[ND_WINDOW], int *count, int *total, int counts[9]) {
	unsigned success = 1U << NDDL_COMPLETE, failed = (1U << NDDL_FAILED) | (1U << NDDL_CANCELLED);
	unsigned all = (1U << (NDDL_WAITING + 1)) - 1;
	unsigned mask = filter == NDDL_ALL ? all : filter == NDDL_PENDING ? all & ~(success | failed) : filter == NDDL_SUCCESS ? success : filter == NDDL_FAILURE ? failed : 0;
	if (!mask)
		return false;
	*count = *total = 0;
	memset(counts, 0, 9 * sizeof(int));
	sqlite3_stmt *st = NULL;
	char sql[1200];
	snprintf(sql, sizeof(sql), "SELECT state,count(*) FROM job WHERE %s GROUP BY state", nd_dl_history_visible(s));
	if (!nd_dl_prepare(s, &st, sql))
		return false;
	int rc;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		int state = sqlite3_column_int(st, 0);
		if (state < 0 || state > 8) {
			sqlite3_finalize(st);
			return false;
		}
		counts[state] = sqlite3_column_int(st, 1);
		if (counts[state] > INT32_MAX - *total) {
			sqlite3_finalize(st);
			return false;
		}
		if (mask & (1U << state))
			*total += counts[state];
	}
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return false;
	snprintf(sql, sizeof(sql), "SELECT %s FROM job WHERE (? & (1 << state)) != 0 AND %s ORDER BY key DESC LIMIT ? OFFSET ?", columns, nd_dl_history_visible(s));
	if (!nd_dl_prepare(s, &st, sql))
		return false;
	sqlite3_bind_int(st, 1, mask);
	sqlite3_bind_int(st, 2, ND_WINDOW);
	sqlite3_bind_int(st, 3, offset < 0 ? 0 : offset);
	while ((rc = sqlite3_step(st)) == SQLITE_ROW && *count < ND_WINDOW)
		decode(st, &jobs[(*count)++]);
	sqlite3_finalize(st);
	return rc == SQLITE_DONE;
}
bool nd_dl_destination(const char *p) {
	if (!p || strlen(p) >= ND_DL_DEST || p[0] == '/' || strchr(p, '\\'))
		return false;
	if (!*p)
		return true;
	char text[ND_DL_DEST];
	snprintf(text, sizeof(text), "%s", p);
	char *save = NULL, *component = strtok_r(text, "/", &save);
	int i = 0;
	if (p[strlen(p) - 1] == '/' || strstr(p, "//"))
		return false;
	while (component) {
		if (component[0] == '.' || (!i && (!strcasecmp(component, "Podcast") || !strcasecmp(component, "Audiobooks"))))
			return false;
		for (const unsigned char *c = (unsigned char *)component; *c; c++)
			if (*c < 32 || strchr(":*?\"<>|", *c))
				return false;
		if (component[strlen(component) - 1] == ' ' || component[strlen(component) - 1] == '.')
			return false;
		i++;
		component = strtok_r(NULL, "/", &save);
	}
	return i > 0;
}
int nd_dl_directory(const char *root, const char *relative, bool create) {
	if (!root || !relative || strlen(relative) >= ND_DL_PATH || relative[0] == '/') {
		errno = EINVAL;
		return -1;
	}
	int fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	char copy[ND_DL_PATH];
	snprintf(copy, sizeof(copy), "%s", relative);
	char *save = NULL;
	for (char *p = strtok_r(copy, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
		if (!strcmp(p, ".") || !strcmp(p, "..") || strchr(p, '\\')) {
			close(fd);
			errno = EINVAL;
			return -1;
		}
		if (create && mkdirat(fd, p, 0755) && errno != EEXIST) {
			close(fd);
			return -1;
		}
		int next = openat(fd, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
		close(fd);
		fd = next;
		if (fd < 0)
			return -1;
	}
	return fd;
}
void nd_dl_safe_name(const char *text, const char *fallback, char out[96]) {
	size_t n = 0;
	for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p;) {
		size_t width = *p < 0x80 ? 1 : *p >= 0xc2 && *p <= 0xdf ? 2 : *p >= 0xe0 && *p <= 0xef ? 3 : *p >= 0xf0 && *p <= 0xf4 ? 4 : 0;
		bool valid = width && strnlen((const char *)p, width) == width;
		for (size_t i = 1; valid && i < width; i++)
			valid = (p[i] & 0xc0) == 0x80;
		if (valid && width == 3)
			valid = (*p != 0xe0 || p[1] >= 0xa0) && (*p != 0xed || p[1] < 0xa0);
		if (valid && width == 4)
			valid = (*p != 0xf0 || p[1] >= 0x90) && (*p != 0xf4 || p[1] < 0x90);
		if (!valid || *p < 32 || *p == 127 || strchr("/\\:*?\"<>|", *p)) {
			p++;
			continue;
		}
		if (n + width > 80)
			break;
		memcpy(out + n, p, width);
		n += width;
		p += width;
	}
	out[n] = 0;
	while (n && (out[n - 1] == '.' || out[n - 1] == ' '))
		out[--n] = 0;
	size_t start = 0;
	while (out[start] == '.' || out[start] == ' ')
		start++;
	if (start)
		memmove(out, out + start, strlen(out + start) + 1);
	char prefix[96];
	snprintf(prefix, sizeof(prefix), "%s", out);
	char *dot = strchr(prefix, '.');
	if (dot)
		*dot = 0;
	bool reserved = !strcasecmp(prefix, "CON") || !strcasecmp(prefix, "PRN") || !strcasecmp(prefix, "AUX") || !strcasecmp(prefix, "NUL") || ((strncasecmp(prefix, "COM", 3) == 0 || strncasecmp(prefix, "LPT", 3) == 0) && strlen(prefix) == 4 && prefix[3] >= '1' && prefix[3] <= '9');
	if (!out[0])
		snprintf(out, 96, "%s", fallback);
	else if (reserved) {
		snprintf(prefix, sizeof(prefix), "_%s", out);
		snprintf(out, 96, "%s", prefix);
	}
}
bool nd_dl_fingerprint(int dir, const char *leaf, char out[65], struct stat *observed, bool (*current)(void *), void *user) {
	if (!leaf || strchr(leaf, '/'))
		return false;
	int fd = openat(dir, leaf, O_RDONLY | O_NOFOLLOW);
	struct stat st;
	bool ok = fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= ND_DL_MAX_BYTES;
	sha256_ctx hash;
	sha256_init(&hash);
	unsigned char bytes[32768];
	int64_t count = 0;
	while (ok && (!current || current(user))) {
		ssize_t n = read(fd, bytes, sizeof(bytes));
		if (!n)
			break;
		if (n < 0 || count > ND_DL_MAX_BYTES - n) {
			ok = false;
			break;
		}
		sha256_update(&hash, bytes, (size_t)n);
		count += n;
	}
	struct stat after;
	ok = ok && (!current || current(user)) && count == st.st_size && !fstat(fd, &after) && after.st_size == st.st_size && after.st_mtime == st.st_mtime && after.st_ino == st.st_ino;
	if (fd >= 0 && close(fd))
		ok = false;
	if (ok) {
		sha256_final_hex(&hash, out);
		if (observed)
			*observed = after;
	}
	return ok;
}

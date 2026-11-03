#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "filepublication.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FP_APPLICATION_ID 1397116977 /* SFP1 */

static int64_t milliseconds(void) {
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t))
		return -1;
	return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int fp_admission_begin(bool write) {
	char path[96];
	snprintf(path, sizeof(path), "/tmp/sonix-file-publication-%lu.lock", (unsigned long)geteuid());
	int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
	struct stat st;
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 || (st.st_mode & 077)) {
		close(fd);
		errno = EACCES;
		return -1;
	}
	int64_t start = milliseconds();
	for (;;) {
		if (!flock(fd, (write ? LOCK_EX : LOCK_SH) | LOCK_NB))
			return fd;
		if ((errno != EWOULDBLOCK && errno != EINTR) || start < 0 || milliseconds() - start >= 200)
			break;
		struct timespec delay = {.tv_nsec = 5000000};
		nanosleep(&delay, NULL);
	}
	close(fd);
	errno = EBUSY;
	return -1;
}

void fp_admission_end(int admission) {
	if (admission >= 0)
		close(admission);
}

static bool hex(const char *s, size_t n) { return s && strlen(s) == n && strspn(s, "0123456789abcdef") == n; }
static bool name_valid(const char *s) { return s && s[0] && strlen(s) < FP_NAME && strcmp(s, ".") && strcmp(s, "..") && !strchr(s, '/') && !strchr(s, '\\'); }
static bool record_valid(const fp_record_t *r) { return hex(r->operation, 32) && hex(r->digest, 64) && name_valid(r->part) && r->part[0] == '.' && strncmp(r->part, FP_JOURNAL, sizeof(FP_JOURNAL) - 1) && name_valid(r->final) && r->final[0] != '.' && r->bytes > 0 && r->bytes <= INT32_MAX && r->source_device >= 0 && r->source_inode > 0 && r->phase >= FP_INTENT && r->phase <= FP_COMPLETE && r->device >= 0 && r->inode >= 0 && (r->phase == FP_INTENT ? r->device == 0 && r->inode == 0 : r->inode > 0); }
static bool sql(fp_guard_t *g, const char *text) { return sqlite3_exec(g->db, text, NULL, NULL, NULL) == SQLITE_OK; }
static bool integer(sqlite3 *db, const char *query, int64_t *result) {
	sqlite3_stmt *st = NULL;
	bool ok = sqlite3_prepare_v2(db, query, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW;
	if (ok) {
		*result = sqlite3_column_int64(st, 0);
		ok = sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	return ok;
}

static bool configure(fp_guard_t *g) {
	sqlite3_busy_timeout(g->db, 100);
	sqlite3_limit(g->db, SQLITE_LIMIT_LENGTH, 8192);
	sqlite3_limit(g->db, SQLITE_LIMIT_SQL_LENGTH, 8192);
	return sql(g, "PRAGMA trusted_schema=OFF; PRAGMA cache_size=-64; PRAGMA temp_store=MEMORY");
}

static bool delete_journal(sqlite3 *db) {
	sqlite3_stmt *st = NULL;
	bool ok = sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW;
	if (ok) {
		const char *mode = (const char *)sqlite3_column_text(st, 0);
		ok = mode && !strcmp(mode, "delete") && sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	return ok;
}

static bool artifacts_valid(int directory) {
	const char *names[] = {FP_JOURNAL, FP_JOURNAL "-journal", FP_JOURNAL "-wal", FP_JOURNAL "-shm"};
	for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++) {
		struct stat st;
		if (fstatat(directory, names[i], &st, AT_SYMLINK_NOFOLLOW)) {
			if (errno == ENOENT)
				continue;
			return false;
		}
		if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size > FP_DATABASE_LIMIT || st.st_size < 0)
			return false;
		// This format never uses WAL: unexpected WAL state is not repaired.
		if (i >= 2)
			return false;
	}
	return true;
}

static const char *columns = "operation,part,final,digest,bytes,source_device,source_inode,device,inode,phase";
static bool text_column(sqlite3_stmt *st, int col, char *out, size_t size) {
	const unsigned char *text = sqlite3_column_text(st, col);
	int bytes = sqlite3_column_bytes(st, col);
	if (sqlite3_column_type(st, col) != SQLITE_TEXT || !text || bytes <= 0 || (size_t)bytes >= size || memchr(text, 0, (size_t)bytes))
		return false;
	memcpy(out, text, (size_t)bytes);
	out[bytes] = 0;
	return true;
}
static bool decode(sqlite3_stmt *st, fp_record_t *r) {
	memset(r, 0, sizeof(*r));
	if (!text_column(st, 0, r->operation, sizeof(r->operation)) || !text_column(st, 1, r->part, sizeof(r->part)) || !text_column(st, 2, r->final, sizeof(r->final)) || !text_column(st, 3, r->digest, sizeof(r->digest)))
		return false;
	for (int i = 4; i < 10; i++)
		if (sqlite3_column_type(st, i) != SQLITE_INTEGER)
			return false;
	r->bytes = sqlite3_column_int64(st, 4);
	r->source_device = sqlite3_column_int64(st, 5);
	r->source_inode = sqlite3_column_int64(st, 6);
	r->device = sqlite3_column_int64(st, 7);
	r->inode = sqlite3_column_int64(st, 8);
	int64_t phase = sqlite3_column_int64(st, 9);
	if (phase < FP_INTENT || phase > FP_COMPLETE)
		return false;
	r->phase = (fp_phase_t)phase;
	return record_valid(r);
}

static bool guard_open(fp_guard_t *g, int directory, bool write, bool create, int admission) {
	*g = (fp_guard_t){.directory = -1, .admission = -1, .writable = write};
	if ((create && !write) || directory < 0)
		return false;
	g->admission = admission >= 0 ? fcntl(admission, F_DUPFD_CLOEXEC, 0) : fp_admission_begin(write);
	if (g->admission < 0)
		return false;
	g->directory = fcntl(directory, F_DUPFD_CLOEXEC, 0);
	if (g->directory < 0 || !artifacts_valid(g->directory))
		return false;
	struct stat before;
	bool absent = fstatat(g->directory, FP_JOURNAL, &before, AT_SYMLINK_NOFOLLOW) != 0;
	if (absent && errno != ENOENT)
		return false;
	if (absent) {
		struct stat sidecar;
		if (!fstatat(g->directory, FP_JOURNAL "-journal", &sidecar, AT_SYMLINK_NOFOLLOW) || errno != ENOENT)
			return false;
		if (!create) {
			g->valid = true;
			return true;
		}
		int fd = openat(g->directory, FP_JOURNAL, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (fd < 0)
			return false;
		bool ok = !fstat(fd, &before);
		if (close(fd))
			ok = false;
		if (!ok)
			return false;
	}
	// SQLite's NOFOLLOW rejects symlinks in parent components too, including
	// /proc/self/fd. Resolve the pinned directory to its physical path while
	// admission excludes cooperating rename/export, then verify its identity.
	char descriptor[64], path[PATH_MAX];
	snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", g->directory);
	ssize_t length = readlink(descriptor, path, sizeof(path) - sizeof(FP_JOURNAL) - 1);
	if (length <= 0 || (size_t)length >= sizeof(path) - sizeof(FP_JOURNAL) - 1)
		return false;
	path[length] = 0;
	struct stat pinned, resolved;
	if (fstat(g->directory, &pinned) || stat(path, &resolved) || pinned.st_dev != resolved.st_dev || pinned.st_ino != resolved.st_ino)
		return false;
	memcpy(path + length, "/" FP_JOURNAL, sizeof("/" FP_JOURNAL));
	// Inspect existing metadata read-only before granting write access. In
	// particular, do not recover a foreign hot journal or change its mode.
	int flags = (write && absent ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY) | SQLITE_OPEN_NOFOLLOW;
	if (sqlite3_open_v2(path, &g->db, flags, NULL) != SQLITE_OK)
		return false;
	if (!configure(g) || !delete_journal(g->db))
		return false;
	if (absent) {
		if (!sql(g, "PRAGMA synchronous=FULL; PRAGMA journal_mode=DELETE; PRAGMA max_page_count=512; PRAGMA page_size=4096; BEGIN IMMEDIATE;"
					"CREATE TABLE publication(operation TEXT PRIMARY KEY,part TEXT NOT NULL,"
					"final TEXT NOT NULL UNIQUE COLLATE NOCASE,digest TEXT NOT NULL,bytes INTEGER NOT NULL,"
					"source_device INTEGER NOT NULL,source_inode INTEGER NOT NULL,device INTEGER NOT NULL,"
					"inode INTEGER NOT NULL,phase INTEGER NOT NULL CHECK(phase BETWEEN 0 AND 2));"
					"PRAGMA application_id=1397116977; PRAGMA user_version=1; COMMIT") ||
			fsync(g->directory))
			return false;
	}
	int64_t version, app, pagesize, count, objects;
	if (!integer(g->db, "PRAGMA user_version", &version) || version != 1 || !integer(g->db, "PRAGMA application_id", &app) || app != FP_APPLICATION_ID || !integer(g->db, "PRAGMA page_size", &pagesize) || pagesize != 4096 || !integer(g->db, "SELECT count(*) FROM publication", &count) || count > FP_RECORD_LIMIT || !integer(g->db, "SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'", &objects) || objects != 1)
		return false;
	struct stat after;
	if (fstatat(g->directory, FP_JOURNAL, &after, AT_SYMLINK_NOFOLLOW) || before.st_dev != after.st_dev || before.st_ino != after.st_ino || !artifacts_valid(g->directory))
		return false;
	// Validate every bounded record, not only the requested name. A malformed
	// record cannot be mistaken for absence and expose an uncertain directory.
	char query[256];
	snprintf(query, sizeof(query), "SELECT %s FROM publication LIMIT %d", columns, FP_RECORD_LIMIT + 1);
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(g->db, query, -1, &st, NULL) != SQLITE_OK)
		return false;
	int rc;
	bool valid = true;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		fp_record_t r;
		if (!decode(st, &r)) {
			valid = false;
			break;
		}
	}
	sqlite3_finalize(st);
	g->valid = valid && rc == SQLITE_DONE;
	if (g->valid && write && !absent) {
		sqlite3_close(g->db);
		g->db = NULL;
		g->valid = sqlite3_open_v2(path, &g->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOFOLLOW, NULL) == SQLITE_OK && configure(g);
		if (g->valid)
			g->valid = !fstatat(g->directory, FP_JOURNAL, &after, AT_SYMLINK_NOFOLLOW) && before.st_dev == after.st_dev && before.st_ino == after.st_ino;
	}
	if (g->valid && write)
		g->valid = sql(g, "PRAGMA synchronous=FULL; PRAGMA max_page_count=512");
	return g->valid;
}

bool fp_guard_open(fp_guard_t *g, int directory, bool write, bool create) { return guard_open(g, directory, write, create, -1); }
bool fp_guard_open_locked(fp_guard_t *g, int directory, bool write, bool create, int admission) {
	if (admission < 0) {
		*g = (fp_guard_t){.directory = -1, .admission = -1};
		return false;
	}
	return guard_open(g, directory, write, create, admission);
}

void fp_guard_close(fp_guard_t *g) {
	if (g->db) {
		sqlite3_exec(g->db, "ROLLBACK", NULL, NULL, NULL);
		sqlite3_close(g->db);
	}
	if (g->directory >= 0)
		close(g->directory);
	fp_admission_end(g->admission);
	*g = (fp_guard_t){.directory = -1, .admission = -1};
}

static bool internal_name(const char *name);

fp_visibility_t fp_guard_lookup(fp_guard_t *g, const char *name) {
	if (!g->valid || !name || !name[0] || strchr(name, '/') || strchr(name, '\\'))
		return FP_UNKNOWN;
	if (internal_name(name))
		return FP_PENDING;
	if (!g->db)
		return FP_SAFE;
	char query[256];
	snprintf(query, sizeof(query), "SELECT %s FROM publication WHERE phase<>2", columns);
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(g->db, query, -1, &st, NULL) != SQLITE_OK)
		return FP_UNKNOWN;
	struct stat named;
	bool exists = !fstatat(g->directory, name, &named, AT_SYMLINK_NOFOLLOW);
	fp_visibility_t result = FP_SAFE;
	if (!exists && errno != ENOENT)
		result = FP_UNKNOWN;
	int rc;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		fp_record_t r;
		if (!decode(st, &r)) {
			result = FP_UNKNOWN;
			break;
		}
		if (!strcasecmp(name, r.final) || !strcasecmp(name, r.part) || (exists && (((int64_t)named.st_dev == r.device && (int64_t)named.st_ino == r.inode) || ((int64_t)named.st_dev == r.source_device && (int64_t)named.st_ino == r.source_inode))))
			result = FP_PENDING;
		// Name aliases must also be hidden in the crash interval between
		// exclusive creation and ownership persistence. Present-day identity
		// is sufficient for exclusion, never for adopting/deleting the object.
		if (exists) {
			struct stat protected;
			const char *paths[] = {r.final, r.part};
			for (unsigned i = 0; i < sizeof(paths) / sizeof(*paths); i++) {
				if (!fstatat(g->directory, paths[i], &protected, AT_SYMLINK_NOFOLLOW)) {
					if (protected.st_dev == named.st_dev && protected.st_ino == named.st_ino)
						result = FP_PENDING;
				} else if (errno != ENOENT) {
					result = FP_UNKNOWN;
					break;
				}
			}
			if (result == FP_UNKNOWN)
				break;
		}
	}
	if (rc != SQLITE_DONE)
		result = FP_UNKNOWN;
	sqlite3_finalize(st);
	return result;
}

int fp_guard_pending(fp_guard_t *g) {
	if (!g->valid)
		return -1;
	if (!g->db)
		return 0;
	int64_t pending;
	if (!integer(g->db, "SELECT EXISTS(SELECT 1 FROM publication WHERE phase<>2)", &pending))
		return -1;
	return pending ? 1 : 0;
}

bool fp_guard_get(fp_guard_t *g, const char *operation, fp_record_t *r) {
	if (!g->valid || !g->db || !hex(operation, 32))
		return false;
	char query[256];
	snprintf(query, sizeof(query), "SELECT %s FROM publication WHERE operation=?", columns);
	sqlite3_stmt *st = NULL;
	bool ok = sqlite3_prepare_v2(g->db, query, -1, &st, NULL) == SQLITE_OK;
	if (ok) {
		sqlite3_bind_text(st, 1, operation, -1, SQLITE_TRANSIENT);
		ok = sqlite3_step(st) == SQLITE_ROW && decode(st, r) && sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	return ok;
}

bool fp_guard_final(fp_guard_t *g, const char *name, fp_record_t *r) {
	if (!g->valid || !g->db || !name)
		return false;
	char query[256];
	snprintf(query, sizeof(query), "SELECT %s FROM publication WHERE final=? COLLATE NOCASE", columns);
	sqlite3_stmt *st = NULL;
	bool ok = sqlite3_prepare_v2(g->db, query, -1, &st, NULL) == SQLITE_OK;
	if (ok) {
		sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
		ok = sqlite3_step(st) == SQLITE_ROW && decode(st, r) && sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	return ok;
}
int fp_guard_part_pending(fp_guard_t *g, const char *name) {
	if (!g->valid || !name)
		return -1;
	if (!g->db)
		return 0;
	sqlite3_stmt *st = NULL;
	int result = -1;
	if (sqlite3_prepare_v2(g->db, "SELECT EXISTS(SELECT 1 FROM publication WHERE phase<>2 AND part=? COLLATE NOCASE)", -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(st) == SQLITE_ROW)
			result = sqlite3_column_int(st, 0) ? 1 : 0;
	}
	sqlite3_finalize(st);
	return result;
}

static bool finish(fp_guard_t *g, sqlite3_stmt *st, bool ok) {
	if (ok)
		ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(g->db) == 1;
	sqlite3_finalize(st);
	if (ok)
		ok = !fsync(g->directory);
#ifdef FILEPUBLICATION_TEST
	extern bool fp_test_commit(bool after);
	if (ok)
		ok = fp_test_commit(false);
#endif
	if (ok)
		ok = sql(g, "COMMIT");
#ifdef FILEPUBLICATION_TEST
	if (ok)
		ok = fp_test_commit(true);
#endif
	if (!ok) {
		sql(g, "ROLLBACK");
		// An uncertain transaction invalidates this handle, never a retry that
		// silently interprets a failed lookup as no protection.
		g->valid = false;
	}
	return ok;
}

bool fp_guard_begin(fp_guard_t *g, const fp_record_t *r) {
	if (!g->valid || !g->writable || !g->db || !record_valid(r) || r->phase != FP_INTENT)
		return false;
	int64_t count;
	if (!integer(g->db, "SELECT count(*) FROM publication", &count) || count >= FP_RECORD_LIMIT || !sql(g, "BEGIN IMMEDIATE"))
		return false;
	sqlite3_stmt *st = NULL;
	bool ok = sqlite3_prepare_v2(g->db, "INSERT INTO publication VALUES(?,?,?,?,?,?,?,0,0,0)", -1, &st, NULL) == SQLITE_OK;
	if (ok) {
		const char *values[] = {r->operation, r->part, r->final, r->digest};
		for (int i = 0; i < 4; i++)
			sqlite3_bind_text(st, i + 1, values[i], -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(st, 5, r->bytes);
		sqlite3_bind_int64(st, 6, r->source_device);
		sqlite3_bind_int64(st, 7, r->source_inode);
	}
	return finish(g, st, ok);
}

static bool transition(fp_guard_t *g, const char *operation, const char *query, int64_t device, int64_t inode) {
	if (!g->valid || !g->writable || !g->db || !hex(operation, 32) || !sql(g, "BEGIN IMMEDIATE"))
		return false;
	sqlite3_stmt *st = NULL;
	bool ok = sqlite3_prepare_v2(g->db, query, -1, &st, NULL) == SQLITE_OK;
	if (ok) {
		sqlite3_bind_text(st, 1, operation, -1, SQLITE_TRANSIENT);
		if (sqlite3_bind_parameter_count(st) == 3) {
			sqlite3_bind_int64(st, 2, device);
			sqlite3_bind_int64(st, 3, inode);
		}
	}
	return finish(g, st, ok);
}
bool fp_guard_owned(fp_guard_t *g, const char *operation, int64_t device, int64_t inode) { return device >= 0 && inode > 0 && transition(g, operation, "UPDATE publication SET phase=1,device=?2,inode=?3 WHERE operation=?1 AND phase=0", device, inode); }
bool fp_guard_complete(fp_guard_t *g, const char *operation) { return transition(g, operation, "UPDATE publication SET phase=2 WHERE operation=? AND phase=1", 0, 0); }
bool fp_guard_retire_complete(fp_guard_t *g, const char *operation) { return transition(g, operation, "DELETE FROM publication WHERE operation=? AND phase=2", 0, 0); }
bool fp_guard_failed_create(fp_guard_t *g, const char *operation) { return transition(g, operation, "DELETE FROM publication WHERE operation=? AND phase=0", 0, 0); }
bool fp_guard_retire_absent(fp_guard_t *g, const char *operation) {
	fp_record_t r;
	struct stat st;
	return fp_guard_get(g, operation, &r) && r.phase != FP_COMPLETE && fstatat(g->directory, r.final, &st, AT_SYMLINK_NOFOLLOW) && errno == ENOENT && transition(g, operation, "DELETE FROM publication WHERE operation=? AND phase<>2", 0, 0);
}

static bool internal_name(const char *name) {
	if (!strncasecmp(name, FP_JOURNAL, sizeof(FP_JOURNAL) - 1))
		return true;
	// Network staging is hidden before it has a validated digest/journal.
	// Recognize only the generated nonce form, not arbitrary dotfiles.
	if (strncasecmp(name, ".nd-", 4))
		return false;
	const char *p = name + 4;
	if (*p < '0' || *p > '9')
		return false;
	while (*p >= '0' && *p <= '9')
		p++;
	return *p++ == '-' && strspn(p, "0123456789abcdefABCDEF") == 16 && p[16] == '.';
}

static bool path_safe(const char *path, int admission, bool missing) {
	if (!path || !path[0] || strlen(path) >= PATH_MAX)
		return false;
	char resolved[PATH_MAX], parent[PATH_MAX];
	// Resolve the parent but not the final component so even a destination
	// that does not exist yet is checked against durable pre-create intent.
	snprintf(parent, sizeof(parent), "%s", path);
	char *slash = strrchr(parent, '/');
	const char *leaf = slash ? slash + 1 : path;
	char name[NAME_MAX + 1];
	if (!leaf[0] || strlen(leaf) > NAME_MAX || !strcmp(leaf, ".") || !strcmp(leaf, ".."))
		return false;
	memcpy(name, leaf, strlen(leaf) + 1);
	if (internal_name(name))
		return false;
	if (slash) {
		if (slash == parent)
			slash[1] = 0;
		else
			*slash = 0;
	} else
		strcpy(parent, ".");
	if (!realpath(parent, resolved))
		return false;
	int dir = open(resolved, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (dir < 0)
		return false;
	fp_guard_t g;
	bool safe = fp_guard_open_locked(&g, dir, false, false, admission) && fp_guard_lookup(&g, name) == FP_SAFE;
	struct stat st;
	if (safe && fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW))
		safe = missing && errno == ENOENT;
	else if (safe && S_ISLNK(st.st_mode))
		safe = false; // caller can resolve aliases for reads; never mutate through one
	fp_guard_close(&g);
	close(dir);
	return safe;
}

int fp_read_begin(const char *path) {
	int admission = fp_admission_begin(false);
	if (admission < 0)
		return -1;
	char resolved[PATH_MAX];
	struct stat st;
	if (path && realpath(path, resolved) && !stat(resolved, &st) && S_ISREG(st.st_mode) && path_safe(resolved, admission, false))
		return admission;
	fp_admission_end(admission);
	errno = EACCES;
	return -1;
}

static bool tree_safe(const char *path, int admission, unsigned depth, unsigned *budget) {
	if (!*budget || depth > 32 || !path_safe(path, admission, true))
		return false;
	(*budget)--;
	struct stat st;
	if (lstat(path, &st))
		return errno == ENOENT;
	if (!S_ISDIR(st.st_mode))
		return S_ISREG(st.st_mode);
	DIR *dir = opendir(path);
	if (!dir)
		return false;
	fp_guard_t guard;
	bool settled = fp_guard_open_locked(&guard, dirfd(dir), false, false, admission) && fp_guard_pending(&guard) == 0;
	fp_guard_close(&guard);
	if (!settled) {
		closedir(dir);
		return false;
	}
	bool ok = true;
	struct dirent *entry;
	while ((entry = readdir(dir))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		// A settled journal can travel with its directory. Direct operations
		// on metadata remain forbidden; no pending row is discarded here.
		if (!strncasecmp(entry->d_name, FP_JOURNAL, sizeof(FP_JOURNAL) - 1))
			continue;
		char child[PATH_MAX];
		int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
		if (n < 0 || n >= (int)sizeof(child) || !tree_safe(child, admission, depth + 1, budget)) {
			ok = false;
			break;
		}
	}
	closedir(dir);
	return ok;
}

int fp_mutation_begin_locked(const char *source, const char *target, bool recursive, int owned) {
	if (!source && !target)
		return -1;
	int admission = owned >= 0 ? fcntl(owned, F_DUPFD_CLOEXEC, 0) : -1;
	if (admission < 0)
		return -1;
	unsigned budget = 100000;
	const char *paths[] = {source, target};
	for (unsigned i = 0; i < sizeof(paths) / sizeof(*paths); i++) {
		if (paths[i] && !(recursive ? tree_safe(paths[i], admission, 0, &budget) : path_safe(paths[i], admission, true))) {
			fp_admission_end(admission);
			errno = EACCES;
			return -1;
		}
	}
	return admission;
}

int fp_mutation_begin(const char *source, const char *target, bool recursive) {
	int admission = fp_admission_begin(true);
	if (admission < 0)
		return -1;
	int result = fp_mutation_begin_locked(source, target, recursive, admission);
	fp_admission_end(admission);
	return result;
}

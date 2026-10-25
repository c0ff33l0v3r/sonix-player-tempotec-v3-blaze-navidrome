#include "navidromelibrary.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static bool binding(const char *vault, char out[1200]) {
	int n = snprintf(out, 1200, "%s.cache", vault);
	return *vault && n > 0 && n < 1200;
}

static bool line(FILE *f, char *out, size_t size) {
	if (!fgets(out, size, f))
		return false;
	size_t n = strlen(out);
	if (!n || out[n - 1] != '\n')
		return false;
	out[n - 1] = 0;
	return true;
}

static bool scope_load(const char *vault, const char *host, const char *user, char out[33]) {
	char path[1200], temp[1210], old_host[ND_HOST_MAX + 2], old_user[ND_USER_MAX + 2], version[32], scope[34];
	if (!binding(vault, path))
		return false;
	FILE *f = fopen(path, "r");
	bool found = f && line(f, version, sizeof(version)) && !strcmp(version, "SONIX-ND-CACHE-1") && line(f, old_host, sizeof(old_host)) && !strcmp(old_host, host) && line(f, old_user, sizeof(old_user)) && !strcmp(old_user, user) && line(f, scope, sizeof(scope)) && strlen(scope) == 32;
	if (f)
		fclose(f);
	if (found && strspn(scope, "0123456789abcdef") == 32) {
		memcpy(out, scope, 33);
		return true;
	}
	uint8_t random[16];
	if (!nd_random(random, sizeof(random)))
		return false;
	for (int i = 0; i < 16; i++)
		snprintf(out + i * 2, 3, "%02x", random[i]);
	snprintf(temp, sizeof(temp), "%s.tmp", path);
	int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return false;
	f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		unlink(temp);
		return false;
	}
	bool ok = fprintf(f, "SONIX-ND-CACHE-1\n%s\n%s\n%s\n", host, user, out) > 0 && fflush(f) == 0 && fsync(fd) == 0;
	if (fclose(f) != 0)
		ok = false;
	if (ok)
		ok = rename(temp, path) == 0 && nd_vault_sync(vault);
	if (!ok)
		unlink(temp);
	return ok;
}

static bool directory(const char *path) {
	struct stat st;
	return (!mkdir(path, 0700) || errno == EEXIST) && !lstat(path, &st) && S_ISDIR(st.st_mode);
}

static bool copy_invalid_database(const char *old, const char *temp) {
	int in = open(old, O_RDONLY | O_NOFOLLOW), out = open(temp, O_WRONLY | O_TRUNC | O_NOFOLLOW);
	bool ok = in >= 0 && out >= 0;
	char buffer[16384];
	while (ok) {
		ssize_t n = read(in, buffer, sizeof(buffer));
		if (!n)
			break;
		if (n < 0) {
			ok = false;
			break;
		}
		ssize_t at = 0;
		while (at < n) {
			ssize_t wrote = write(out, buffer + at, (size_t)(n - at));
			if (wrote <= 0) {
				ok = false;
				break;
			}
			at += wrote;
		}
	}
	if (in >= 0 && close(in))
		ok = false;
	if (out >= 0 && close(out))
		ok = false;
	return ok;
}

// The service calls this only before admitting readers on this card. SQLite's
// backup API includes committed WAL data and leaves the old DB intact on failure.
static bool migrate_database(const char *old, const char *path) {
	char temp[1100];
	if (snprintf(temp, sizeof(temp), "%s.migrating", path) >= (int)sizeof(temp))
		return false;
	struct stat st;
	if (!lstat(temp, &st) && (!S_ISREG(st.st_mode) || unlink(temp)))
		return false;
	int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (fd < 0)
		return false;
	bool ok = close(fd) == 0;
	sqlite3 *source = NULL, *target = NULL;
	if (ok)
		ok = sqlite3_open_v2(old, &source, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOFOLLOW, NULL) == SQLITE_OK;
	if (ok)
		ok = sqlite3_open_v2(temp, &target, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOFOLLOW, NULL) == SQLITE_OK;
	if (ok)
		ok = sqlite3_exec(source, "PRAGMA cache_size=-256", NULL, NULL, NULL) == SQLITE_OK && sqlite3_exec(target, "PRAGMA cache_size=-256", NULL, NULL, NULL) == SQLITE_OK;
	if (ok) {
		sqlite3_backup *backup = sqlite3_backup_init(target, "main", source, "main");
		ok = backup && sqlite3_backup_step(backup, -1) == SQLITE_DONE;
		if (backup && sqlite3_backup_finish(backup) != SQLITE_OK)
			ok = false;
	}
	int source_error = source ? sqlite3_errcode(source) : SQLITE_OK;
	int target_error = target ? sqlite3_errcode(target) : SQLITE_OK;
	bool invalid = source_error == SQLITE_CORRUPT || source_error == SQLITE_NOTADB || target_error == SQLITE_CORRUPT || target_error == SQLITE_NOTADB;
	if (target && sqlite3_close(target) != SQLITE_OK)
		ok = false;
	if (source && sqlite3_close(source) != SQLITE_OK)
		ok = false;
	// Preserve unreadable bytes in the new location so explicit Sync can still
	// perform its existing corrupt-store recovery. Never raw-copy a busy valid DB.
	if (!ok && invalid)
		ok = copy_invalid_database(old, temp);
	fd = ok ? open(temp, O_RDONLY | O_NOFOLLOW) : -1;
	if (ok)
		ok = fd >= 0 && fsync(fd) == 0;
	if (fd >= 0 && close(fd))
		ok = false;
	if (ok)
		ok = rename(temp, path) == 0 && nd_vault_sync(path);
	if (!ok) {
		unlink(temp);
		return false;
	}
	// Remove the retired database only after the new durable copy is published.
	// Failed cleanup leaves recoverable legacy data, never replaces new data.
	if (unlink(old) && errno != ENOENT)
		return false;
	const char *sidecars[] = {"-journal", "-wal", "-shm"};
	for (int i = 0; i < 3; i++) {
		char leaf[1100];
		if (snprintf(leaf, sizeof(leaf), "%s%s", old, sidecars[i]) >= (int)sizeof(leaf))
			continue;
		if (unlink(leaf) && errno != ENOENT)
			return false;
	}
	return true;
}

static bool migrate_cache(const char *base, const char *path) {
	char dir[1100], old[1100], dest[1100];
	if (snprintf(dir, sizeof(dir), "%s/.cache", base) >= (int)sizeof(dir) || !directory(dir))
		return false;
	if (snprintf(old, sizeof(old), "%s/library.db", base) >= (int)sizeof(old))
		return false;
	struct stat st;
	if (lstat(old, &st) == 0 && access(path, F_OK) != 0) {
		if (!S_ISREG(st.st_mode) || !migrate_database(old, path))
			return false;
	}
	const char *names[] = {"art", "audio", "library.db.invalid"};
	for (int i = 0; i < 3; i++) {
		if (snprintf(old, sizeof(old), "%s/%s", base, names[i]) >= (int)sizeof(old) || snprintf(dest, sizeof(dest), "%s/%s", dir, names[i]) >= (int)sizeof(dest))
			return false;
		if (lstat(old, &st)) {
			if (errno != ENOENT)
				return false;
			continue;
		}
		if ((i < 2 && !S_ISDIR(st.st_mode)) || (i == 2 && !S_ISREG(st.st_mode)))
			return false;
		struct stat target;
		if (!lstat(dest, &target))
			continue; // never overwrite an existing new cache
		if (errno != ENOENT || rename(old, dest))
			return false;
	}
	return nd_vault_sync(path);
}

void nd_cache_load(const char *root, const char *vault, const char *host, const char *user, bool migrate_legacy, nd_library_t *out) {
	memset(out, 0, sizeof(*out));
	out->error = "nd_cache_storage";
	if (!*host || !scope_load(vault, host, user, out->scope)) {
		out->error = "nd_cache_error";
		return;
	}
	struct stat st;
	if (!root || !*root || stat(root, &st) || !S_ISDIR(st.st_mode))
		return;
	int n = snprintf(out->path, sizeof(out->path), "%s/" ND_CACHE_RELATIVE "/library.db", root);
	if (n < 0 || (size_t)n >= sizeof(out->path)) {
		out->path[0] = 0;
		return;
	}
	struct statvfs fs;
	out->writable = access(root, W_OK) == 0 && (st.st_mode & 0222) && statvfs(root, &fs) == 0 && !(fs.f_flag & ST_RDONLY);
	char base[1024], legacy[1100];
	if (snprintf(base, sizeof(base), "%s/.local/navidrome", root) >= (int)sizeof(base) || snprintf(legacy, sizeof(legacy), "%s/library.db", base) >= (int)sizeof(legacy))
		return;
	// Never relocate a path already captured by an in-session queue. A writable
	// reinsertion/restart retries migration; read-only old cards remain readable.
	bool moved = true;
	if (out->writable && migrate_legacy && !lstat(base, &st))
		moved = S_ISDIR(st.st_mode) && migrate_cache(base, out->path);
	if (access(out->path, F_OK) && !access(legacy, F_OK)) {
		if (snprintf(out->path, sizeof(out->path), "%s", legacy) >= (int)sizeof(out->path))
			return;
		out->writable = false;
	} else if (!moved)
		out->writable = false;
	out->error = !moved ? "nd_cache_storage" : out->writable ? NULL : "nd_cache_readonly";
	if (access(out->path, F_OK))
		return;
	nd_store_t store;
	if (nd_store_open(&store, out->path, out->scope, false, false) && nd_store_status(&store, out)) {
		nd_store_close(&store);
		return;
	}
	nd_store_close(&store);
	out->ready = false;
	out->error = "nd_cache_error";
}

bool nd_cache_open_sync(const nd_library_t *lib, nd_store_t *store) {
	char dir[1024];
	snprintf(dir, sizeof(dir), "%s", lib->path);
	char *slash = strrchr(dir, '/');
	if (!slash || !lib->writable)
		return false;
	*slash = 0;
	for (char *p = dir + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			mkdir(dir, 0700);
			*p = '/';
		}
	if (mkdir(dir, 0700) != 0 && errno != EEXIST)
		return false;
	if (nd_store_open(store, lib->path, lib->scope, true, false) && (!lib->error || strcmp(lib->error, "nd_cache_error")))
		return true;
	nd_store_close(store);
	// Rebuilding disposable invalid storage is authorized only by explicit Sync.
	// Keep it aside until a new complete snapshot exists; never touch a readable
	// valid snapshot merely because a normal write was denied.
	if (!lib->error || strcmp(lib->error, "nd_cache_error"))
		return false;
	char backup[1050];
	snprintf(backup, sizeof(backup), "%s.invalid", lib->path);
	if (rename(lib->path, backup) != 0)
		return false;
	return nd_store_open(store, lib->path, lib->scope, true, false);
}

bool nd_cache_card_same(const char *root, uint64_t device, uint64_t inode) {
	struct stat st;
	return root && *root && stat(root, &st) == 0 && (uint64_t)st.st_dev == device && (uint64_t)st.st_ino == inode;
}

bool nd_cache_retire_scope(const char *vault) {
	char path[1200];
	bool ok = binding(vault, path);
	if (ok)
		ok = unlink(path) == 0 || errno == ENOENT;
	if (ok)
		ok = nd_vault_sync(vault);
	return ok;
}

bool nd_cache_retire(const char *root, const char *vault) {
	bool ok = nd_cache_retire_scope(vault);
	char path[1200];
	// All names here are generated by this feature; do not follow directories
	// supplied by server metadata. Scope retirement remains effective with no card.
	if (root && *root) {
		const char *dirs[] = {ND_CACHE_RELATIVE "/art", ND_CACHE_RELATIVE, ".local/navidrome/art", ".local/navidrome"};
		for (int i = 0; i < 4; i++) {
			char dir[1100];
			if (snprintf(dir, sizeof(dir), "%s/%s", root, dirs[i]) >= (int)sizeof(dir))
				continue;
			DIR *d = opendir(dir);
			if (!d)
				continue;
			struct dirent *entry;
			while ((entry = readdir(d))) {
				if (entry->d_name[0] == '.')
					continue;
				if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) < (int)sizeof(path)) {
					struct stat st;
					if (lstat(path, &st) == 0 && !S_ISDIR(st.st_mode) && unlink(path) && errno != ENOENT)
						ok = false;
				}
			}
			closedir(d);
		}
	}
	return ok;
}

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "webcgi.h"
#include "src/system/library/filepublication.h"
#include "src/system/library/filepublicationcard.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_PATH 1024
#define MAX_LIST 4096
#define CHUNK 65536
#define MAX_BODY INT32_MAX
static const char *operation;
static uint64_t card_epoch, transfer_session;
static char card_root[512];
static const char *const operations[] = {"sonix-list", "list", "download", "upload", "create", "move", "delete"};

static int hexval(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}
static bool query_value(const char *query, const char *key, char *out, size_t size) {
	size_t key_len = strlen(key);
	for (const char *at = query; at && *at;) {
		const char *end = strchr(at, '&');
		if (!end)
			end = at + strlen(at);
		if ((size_t)(end - at) > key_len && !memcmp(at, key, key_len) && at[key_len] == '=') {
			size_t used = 0;
			for (const char *p = at + key_len + 1; p < end; p++) {
				unsigned char c = *p;
				if (c == '%') {
					if (end - p < 3 || hexval(p[1]) < 0 || hexval(p[2]) < 0)
						return false;
					c = (hexval(p[1]) << 4) | hexval(p[2]);
					p += 2;
				} else if (c == '+')
					c = ' ';
				if (!c || c < 32 || c == 127 || used + 1 >= size)
					return false;
				out[used++] = c;
			}
			out[used] = 0;
			return true;
		}
		at = *end ? end + 1 : NULL;
	}
	return false;
}
static void json_string(const char *s) {
	putchar('"');
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '"' || *p == '\\') {
			putchar('\\');
			putchar(*p);
		} else if (*p < 32)
			printf("\\u%04x", *p);
		else
			putchar(*p);
	}
	putchar('"');
}
static int fail(int status, const char *reason) {
	printf("Status: %d Request Failed\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n\r\n{\"error\":", status);
	json_string(reason);
	puts("}");
	return 1;
}
static void headers(const char *type) { printf("Content-Type: %s\r\nCache-Control: no-store\r\nX-Sonix-Card: %llu\r\n", type, (unsigned long long)card_epoch); }
static bool unsigned_number(const char *text, uint64_t *number) {
	if (!text || !*text)
		return false;
	uint64_t n = 0;
	for (; *text; text++) {
		if (*text < '0' || *text > '9' || n > (UINT64_MAX - (*text - '0')) / 10)
			return false;
		n = n * 10 + (*text - '0');
	}
	*number = n;
	return true;
}

// Resolve the browser's stock virtual card paths to the selected physical card.
// Every component is opened without following links, under the card lifetime
// and publication admission. Other stock roots are accepted only when they
// identify this same card. No additional bind mount can pin an exported card.
static bool resolve(const char *logical, char physical[MAX_PATH], bool missing_leaf) {
	static const char *const roots[] = {"/data/mnt/sd_0", "/data/mnt/sd_1", "/data/mnt/udisk_0", "/data/mnt/udisk_1"};
	const char *relative = NULL;
	struct stat base;
	if (stat(card_root, &base))
		return false;
	for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
		size_t len = strlen(roots[i]);
		if (strncmp(logical, roots[i], len) || (logical[len] && logical[len] != '/'))
			continue;
		if (i) {
			struct stat alias;
			if (stat(roots[i], &alias) || alias.st_dev != base.st_dev || alias.st_ino != base.st_ino)
				return false;
		}
		relative = logical + len;
		break;
	}
	if (!relative || strlen(relative) + strlen(card_root) + 1 >= MAX_PATH)
		return false;
	int dir = open(card_root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (dir < 0)
		return false;
	snprintf(physical, MAX_PATH, "%s", card_root);
	bool ok = true;
	for (const char *p = relative; *p && ok;) {
		while (*p == '/')
			p++;
		if (!*p)
			break;
		const char *end = strchr(p, '/');
		if (!end)
			end = p + strlen(p);
		size_t len = end - p;
		char name[256];
		if (!len || len >= sizeof(name)) {
			ok = false;
			break;
		}
		memcpy(name, p, len);
		name[len] = 0;
		if (!strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '\\')) {
			ok = false;
			break;
		}
		const char *next = end;
		while (*next == '/')
			next++;
		bool last = !*next;
		struct stat st;
		if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW)) {
			ok = last && missing_leaf && errno == ENOENT;
		} else {
			ok = !S_ISLNK(st.st_mode) && st.st_dev == base.st_dev && (last || S_ISDIR(st.st_mode));
			if (ok && !last) {
				int child = openat(dir, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
				close(dir);
				dir = child;
				ok = dir >= 0;
			}
		}
		if (ok) {
			strcat(physical, "/");
			strcat(physical, name);
		}
		p = next;
	}
	if (dir >= 0)
		close(dir);
	return ok;
}

// Collect a bounded listing, then release every card descriptor before writing
// to the CGI pipe. A slow or disconnected browser must never pin the card.
typedef struct {
	char name[256];
	int64_t size, modified;
	bool directory;
} listing_row_t;
static int listing(const char *logical, bool hidden) {
	int lease = fp_transfer_begin(card_epoch, transfer_session);
	int admission = lease >= 0 ? fp_admission_begin(false) : -1;
	char physical[MAX_PATH];
	DIR *dir = admission >= 0 && resolve(logical, physical, false) ? opendir(physical) : NULL;
	fp_guard_t guard = {.directory = -1, .admission = -1};
	bool ok = dir && fp_guard_open_locked(&guard, dirfd(dir), false, false, admission);
	listing_row_t *rows = ok ? calloc(MAX_LIST, sizeof(*rows)) : NULL;
	ok = ok && rows;
	size_t count = 0;
	struct dirent *entry;
	while (ok && (entry = readdir(dir))) {
		const char *name = entry->d_name;
		if (!strcmp(name, ".") || !strcmp(name, "..") || (!hidden && name[0] == '.'))
			continue;
		if (fp_guard_lookup(&guard, name) != FP_SAFE)
			continue;
		struct stat st;
		if (fstatat(dirfd(dir), name, &st, AT_SYMLINK_NOFOLLOW)) {
			ok = false;
			break;
		}
		if (S_ISLNK(st.st_mode) || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)))
			continue;
		if (count == MAX_LIST || strlen(name) >= sizeof(rows[0].name)) {
			ok = false;
			break;
		}
		strcpy(rows[count].name, name);
		rows[count].size = st.st_size;
		rows[count].modified = st.st_mtime;
		rows[count++].directory = S_ISDIR(st.st_mode);
	}
	fp_guard_close(&guard);
	if (dir)
		closedir(dir);
	fp_admission_end(admission);
	fp_card_end(lease);
	if (!ok) {
		free(rows);
		return fail(409, "directory unavailable or too large");
	}
	headers("application/json; charset=utf-8");
	fputs("\r\n[", stdout);
	for (size_t i = 0; i < count; i++) {
		char full[MAX_PATH + 258];
		snprintf(full, sizeof(full), "%s%s%s%s", logical, logical[strlen(logical) - 1] == '/' ? "" : "/", rows[i].name, rows[i].directory ? "/" : "");
		if (i)
			putchar(',');
		fputs("{\"path\":", stdout);
		json_string(full);
		fputs(",\"name\":", stdout);
		json_string(rows[i].name);
		printf(",\"ctime\":\"%lld\"", (long long)rows[i].modified);
		if (!rows[i].directory)
			printf(",\"size\":%lld", (long long)rows[i].size);
		putchar('}');
	}
	puts("]");
	free(rows);
	return ferror(stdout) ? 1 : 0;
}
static bool same_file(const struct stat *a, const struct stat *b) { return S_ISREG(b->st_mode) && a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size && a->st_mtime == b->st_mtime && a->st_ctime == b->st_ctime; }
static int download(const char *logical) {
	unsigned char buffer[CHUNK];
	struct stat identity = {0};
	off_t offset = 0;
	bool started = false;
	for (;;) {
		int lease = fp_transfer_begin(card_epoch, transfer_session);
		int admission = lease >= 0 ? fp_admission_begin(false) : -1;
		char physical[MAX_PATH];
		int read_guard = admission >= 0 && resolve(logical, physical, false) ? fp_read_begin(physical) : -1;
		int fd = read_guard >= 0 ? open(physical, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC) : -1;
		struct stat st;
		bool ok = fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && (!started || same_file(&identity, &st));
		ssize_t got = -1;
		if (ok) {
			if (!started)
				identity = st;
			do {
				got = pread(fd, buffer, sizeof(buffer), offset);
			} while (got < 0 && errno == EINTR);
		}
		if (fd >= 0 && close(fd))
			ok = false;
		fp_admission_end(read_guard);
		fp_admission_end(admission);
		fp_card_end(lease);
		if (!ok || got < 0)
			return started ? 1 : fail(409, "file unavailable");
		if (!started) {
			headers("application/octet-stream");
			printf("Content-Disposition: attachment\r\nContent-Length: %lld\r\n\r\n", (long long)identity.st_size);
			started = true;
		}
		if (!got)
			return offset == identity.st_size && !ferror(stdout) ? 0 : 1;
		if (fwrite(buffer, 1, got, stdout) != (size_t)got || fflush(stdout))
			return 1;
		offset += got;
	}
}
static bool remove_tree(const char *path, unsigned depth, size_t *budget) {
	if (depth > 32 || !*budget)
		return false;
	(*budget)--;
	struct stat st;
	if (lstat(path, &st))
		return false;
	if (!S_ISDIR(st.st_mode))
		return !unlink(path);
	DIR *dir = opendir(path);
	if (!dir)
		return false;
	bool ok = true;
	struct dirent *de;
	while (ok && (de = readdir(dir))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		char child[MAX_PATH];
		ok = snprintf(child, sizeof(child), "%s/%s", path, de->d_name) < (int)sizeof(child) && remove_tree(child, depth + 1, budget);
	}
	closedir(dir);
	return ok && !rmdir(path);
}
static int mutation(const char *logical, const char *target) {
	int lease = fp_transfer_begin(card_epoch, transfer_session);
	int admission = lease >= 0 ? fp_admission_begin(true) : -1;
	char source[MAX_PATH], destination[MAX_PATH];
	bool create = !strcmp(operation, "create"), move = !strcmp(operation, "move");
	bool ok = admission >= 0 && resolve(logical, source, create) && strcmp(source, card_root) && (!move || (resolve(target, destination, true) && strcmp(destination, card_root)));
	// Reuse the already-owned cross-process lock to avoid acquiring an
	// exclusive flock recursively. The guard preflight remains under it.
	int guard = ok ? fp_mutation_begin_locked(source, move ? destination : NULL, true, admission) : -1;
	ok = guard >= 0;
	if (ok && create) {
		struct stat st;
		ok = !mkdir(source, 0777) || (errno == EEXIST && !lstat(source, &st) && S_ISDIR(st.st_mode));
	} else if (ok && move) {
		struct stat st;
		ok = lstat(destination, &st) && errno == ENOENT && !rename(source, destination);
	} else if (ok) {
		size_t budget = 100000;
		ok = remove_tree(source, 0, &budget);
	}
	fp_admission_end(guard);
	fp_admission_end(admission);
	fp_card_end(lease);
	if (!ok)
		return fail(409, "file protected or operation unavailable");
	headers("application/json");
	puts("\r\n{}");
	return 0;
}
static bool write_all(int fd, const unsigned char *bytes, size_t count) {
	while (count) {
		ssize_t n = write(fd, bytes, count);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return false;
		bytes += n;
		count -= n;
	}
	return true;
}
static int upload(const char *logical, uint64_t length) {
	unsigned char buffer[CHUNK];
	uint64_t offset = 0;
	struct stat identity = {0};
	bool started = false;
	do {
		size_t wanted = length - offset > sizeof(buffer) ? sizeof(buffer) : (size_t)(length - offset);
		// Complete the network read before opening any card file or lease.
		size_t got = 0;
		while (got < wanted) {
			ssize_t n = read(STDIN_FILENO, buffer + got, wanted - got);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				return fail(400, "incomplete upload");
			got += n;
		}
		int lease = fp_transfer_begin(card_epoch, transfer_session);
		int admission = lease >= 0 ? fp_admission_begin(true) : -1;
		char physical[MAX_PATH];
		bool resolved = admission >= 0 && resolve(logical, physical, !started) && strcmp(physical, card_root);
		int guard = resolved ? fp_mutation_begin_locked(NULL, physical, false, admission) : -1;
		int fd = guard >= 0 ? open(physical, O_WRONLY | (started ? 0 : O_CREAT) | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0666) : -1;
		struct stat st;
		bool ok = fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && (!started || same_file(&identity, &st));
		if (ok && !started)
			ok = !ftruncate(fd, 0);
		if (ok)
			ok = lseek(fd, (off_t)offset, SEEK_SET) == (off_t)offset && write_all(fd, buffer, got);
		if (ok && offset + got == length)
			ok = !fsync(fd);
		if (ok)
			ok = !fstat(fd, &identity);
		if (fd >= 0 && close(fd))
			ok = false;
		fp_admission_end(guard);
		fp_admission_end(admission);
		fp_card_end(lease);
		if (!ok)
			return fail(409, "file protected or storage changed");
		started = true;
		offset += got;
	} while (offset < length);
	headers("application/json");
	puts("\r\n{}");
	return 0;
}

bool webcgi_should_run(int argc, char **argv) {
	if (!getenv("GATEWAY_INTERFACE"))
		return false;
	const char *name = argc > 0 && argv[0] ? argv[0] : "";
	const char *slash = strrchr(name, '/');
	if (slash)
		name = slash + 1;
	for (unsigned i = 0; i < sizeof(operations) / sizeof(operations[0]); i++) {
		if (!strcmp(name, operations[i])) {
			operation = operations[i];
			return true;
		}
	}
	return false;
}
int webcgi_main(void) {
	if (!operation)
		return fail(400, "invalid endpoint");
	prctl(PR_SET_NAME, "sonix-transfer", 0, 0, 0);
	const char *query = getenv("QUERY_STRING");
	const char *method = getenv("REQUEST_METHOD");
	if (!query)
		query = "";
	bool list = !strcmp(operation, "list") || !strcmp(operation, "sonix-list");
	bool get = list || !strcmp(operation, "download");
	if (!method || strcmp(method, get ? "GET" : "POST"))
		return fail(405, "method unavailable");
	uint64_t length = 0;
	char body[8192];
	bool sending = !strcmp(operation, "upload");
	if (!get) {
		if (!unsigned_number(getenv("CONTENT_LENGTH"), &length) || length > MAX_BODY)
			return fail(413, "body too large");
		const char *type = getenv("CONTENT_TYPE");
		if (!type || strcmp(type, sending ? "application/octet-stream" : "application/x-www-form-urlencoded"))
			return fail(415, "unsupported upload format");
		if (!sending) {
			if (length >= sizeof(body))
				return fail(413, "body too large");
			size_t used = 0;
			while (used < length) {
				ssize_t n = read(STDIN_FILENO, body + used, length - used);
				if (n < 0 && errno == EINTR)
					continue;
				if (n <= 0)
					return fail(400, "incomplete request");
				used += n;
			}
			body[used] = 0;
			query = body;
		}
	}
	char logical[MAX_PATH], target[MAX_PATH], requested[32], hidden[8];
	bool move = !strcmp(operation, "move");
	if (!query_value(query, move ? "oldPath" : "path", logical, sizeof(logical)) || !logical[0] || (move && !query_value(query, "newPath", target, sizeof(target))))
		return fail(400, "missing path");
	if (!fp_transfer_snapshot(&card_epoch, &transfer_session, card_root))
		return fail(409, "card unavailable");
	uint64_t expected;
	if (!list && (!query_value(query, "epoch", requested, sizeof(requested)) || !unsigned_number(requested, &expected) || expected != card_epoch))
		return fail(409, "card changed; refresh directory");
	if (list)
		return listing(logical, query_value(query, "hidden", hidden, sizeof(hidden)) && !strcmp(hidden, "1"));
	if (get)
		return download(logical);
	if (sending)
		return upload(logical, length);
	return mutation(logical, move ? target : NULL);
}

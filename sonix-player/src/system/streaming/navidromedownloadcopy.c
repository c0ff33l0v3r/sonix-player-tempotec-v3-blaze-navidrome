#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "navidromedownloadcopy.h"
#include "src/system/core/sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define COPY_CHUNK 65536
#define COPY_RESERVE (16 * 1024 * 1024 + 2 * FP_DATABASE_LIMIT)
static bool current(nd_download_copy_t *c) {
	if (!c->current || c->current(c->user))
		return true;
	errno = ECANCELED;
	return false;
}
static bool room(int directory, int64_t bytes) {
	struct statvfs fs;
	if (bytes < 0 || bytes > INT32_MAX || fstatvfs(directory, &fs) || !fs.f_frsize)
		return false;
	uint64_t need = (uint64_t)bytes + COPY_RESERVE;
	if ((uint64_t)fs.f_bavail >= need / fs.f_frsize + (need % fs.f_frsize != 0))
		return true;
	errno = ENOSPC;
	return false;
}
static bool fingerprint(nd_download_copy_t *c, int fd, struct stat *observed) {
	struct stat before, after;
	if (fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_size != c->record.bytes || lseek(fd, 0, SEEK_SET) != 0)
		return false;
	unsigned char bytes[COPY_CHUNK];
	sha256_ctx hash;
	sha256_init(&hash);
	int64_t total = 0;
	while (current(c)) {
		ssize_t n = read(fd, bytes, sizeof(bytes));
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 || n > c->record.bytes - total)
			return false;
		if (!n)
			break;
		sha256_update(&hash, bytes, (size_t)n);
		total += n;
	}
	char digest[65];
	sha256_final_hex(&hash, digest);
	if (!current(c) || total != c->record.bytes || strcmp(digest, c->record.digest) || fstat(fd, &after) || after.st_dev != before.st_dev || after.st_ino != before.st_ino || after.st_size != before.st_size || after.st_mtime != before.st_mtime)
		return false;
	*observed = after;
	return true;
}
typedef struct {
	nd_download_copy_t *copy;
	struct stat expected;
} seal_t;
static bool seal(void *user) {
	seal_t *s = user;
	nd_download_copy_t *c = s->copy;
	fp_guard_t g;
	fp_record_t r;
	struct stat named;
	bool ok = fp_guard_open(&g, c->directory, true, false) && current(c) && fp_guard_get(&g, c->record.operation, &r) && r.phase == FP_OWNED && r.device == (int64_t)s->expected.st_dev && r.inode == (int64_t)s->expected.st_ino && !fstatat(c->directory, r.final, &named, AT_SYMLINK_NOFOLLOW) && S_ISREG(named.st_mode) && named.st_nlink == 1 && named.st_dev == s->expected.st_dev && named.st_ino == s->expected.st_ino && named.st_size == r.bytes && named.st_mtime == s->expected.st_mtime && fp_guard_complete(&g, r.operation);
	fp_guard_close(&g);
	return ok;
}
nd_download_publish_result_t nd_download_copy(nd_download_copy_t *c) {
	if (!c || c->directory < 0 || c->record.bytes <= 0 || c->record.bytes > INT32_MAX || !current(c))
		return ND_DOWNLOAD_PUBLISH_ERROR;
	if (!memchr(c->record.part, 0, sizeof(c->record.part)) || !memchr(c->record.final, 0, sizeof(c->record.final)) || !memchr(c->record.digest, 0, sizeof(c->record.digest)) || !memchr(c->record.operation, 0, sizeof(c->record.operation)) || c->record.part[0] != '.' || strchr(c->record.part, '/') || strchr(c->record.part, '\\') || !c->record.final[0] || c->record.final[0] == '.' || strchr(c->record.final, '/') || strchr(c->record.final, '\\'))
		return ND_DOWNLOAD_PUBLISH_ERROR;
	int source = openat(c->directory, c->record.part, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	struct stat staged;
	bool ok = source >= 0 && fingerprint(c, source, &staged) && room(c->directory, c->record.bytes);
	int output = -1;
	nd_download_publish_result_t result = ND_DOWNLOAD_PUBLISH_ERROR;
	if (ok) {
		c->record.source_device = staged.st_dev;
		c->record.source_inode = staged.st_ino;
		c->record.device = c->record.inode = 0;
		c->record.phase = FP_INTENT;
		fp_guard_t g;
		ok = fp_guard_open(&g, c->directory, true, true) && current(c) && fp_guard_begin(&g, &c->record);
		if (ok) {
			output = nd_download_file_create(c->directory, c->record.final);
			int create_error = errno;
			if (output < 0) {
				bool certain = create_error == EEXIST || create_error == ENOTEMPTY || create_error == EACCES || create_error == EPERM || create_error == EROFS;
				if (certain && fp_guard_failed_create(&g, c->record.operation) && (create_error == EEXIST || create_error == ENOTEMPTY))
					result = ND_DOWNLOAD_PUBLISH_CONFLICT;
				ok = false;
				errno = create_error;
			} else {
				struct stat made;
				ok = !fstat(output, &made) && nd_download_file_owned(c->directory, c->record.final, output) && fp_guard_owned(&g, c->record.operation, made.st_dev, made.st_ino);
			}
		}
		fp_guard_close(&g);
	}
	if (ok)
		ok = lseek(source, 0, SEEK_SET) == 0;
	unsigned char bytes[COPY_CHUNK];
	int64_t copied = 0;
	while (ok && copied < c->record.bytes) {
		if (!current(c)) {
			ok = false;
			break;
		}
		ssize_t n = read(source, bytes, sizeof(bytes));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0 || n > c->record.bytes - copied || !room(c->directory, n)) {
			ok = false;
			break;
		}
		ssize_t at = 0;
		while (at < n && current(c)) {
			ssize_t wrote = write(output, bytes + at, (size_t)(n - at));
			if (wrote < 0 && errno == EINTR)
				continue;
			if (wrote <= 0) {
				ok = false;
				break;
			}
			at += wrote;
		}
		copied += at;
		ok = ok && at == n;
	}
	struct stat expected;
	if (ok)
		ok = current(c) && !fsync(output) && !fstat(output, &expected) && nd_download_file_owned(c->directory, c->record.final, output);
	if (output >= 0 && close(output))
		ok = false;
	if (source >= 0 && close(source))
		ok = false;
	if (ok) {
		// Read-only reopen after successful close. Compare against the live
		// descriptor's identity, never persisted inode-only recovery evidence.
		int check = openat(c->directory, c->record.final, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
		struct stat named, verified;
		ok = check >= 0 && !fstat(check, &named) && named.st_dev == expected.st_dev && named.st_ino == expected.st_ino && nd_download_file_owned(c->directory, c->record.final, check) && fingerprint(c, check, &verified);
		if (check >= 0 && close(check))
			ok = false;
		if (ok) {
			seal_t s = {.copy = c, .expected = verified};
			ok = c->commit ? c->commit(seal, &s, c->user) : seal(&s);
		}
	}
	return ok ? ND_DOWNLOAD_PUBLISHED : result;
}

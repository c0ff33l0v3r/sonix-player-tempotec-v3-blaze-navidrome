#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "src/system/core/sha256.h"
#include "src/system/library/filepublication.h"
#include "src/system/streaming/navidromedownloadcopy.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fault;
static int copy_fault;
static bool copy_output(int fd) {
	char descriptor[64], path[4096];
	snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
	ssize_t n = readlink(descriptor, path, sizeof(path) - 1);
	if (n <= 0)
		return false;
	path[n] = 0;
	const char *leaf = strrchr(path, '/');
	return leaf && !strncmp(leaf + 1, "copy-", 5);
}
ssize_t __real_write(int fd, const void *buffer, size_t size);
int __real_fsync(int fd);
int __real_close(int fd);
ssize_t __wrap_write(int fd, const void *buffer, size_t size) {
	if (copy_fault == 1 && copy_output(fd)) {
		errno = ENOSPC;
		return -1;
	}
	return __real_write(fd, buffer, size);
}
int __wrap_fsync(int fd) {
	if (copy_fault == 2 && copy_output(fd)) {
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}
int __wrap_close(int fd) {
	bool fail = copy_fault == 3 && copy_output(fd);
	int result = __real_close(fd);
	if (fail) {
		errno = EIO;
		return -1;
	}
	return result;
}
bool fp_test_commit(bool after) {
	if (fault == (after ? 2 : 1)) {
		fault = 0;
		return false;
	}
	return true;
}
static fp_record_t record(unsigned n) {
	fp_record_t r = {.bytes = 42, .source_device = 1, .source_inode = 2};
	snprintf(r.operation, sizeof(r.operation), "%032x", n);
	snprintf(r.part, sizeof(r.part), ".nd-%u.part", n);
	snprintf(r.final, sizeof(r.final), "Track-%u.flac", n);
	memset(r.digest, 'a', 64);
	return r;
}
static int folder(int root, const char *name) {
	assert(!mkdirat(root, name, 0700));
	int dir = openat(root, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	assert(dir >= 0);
	return dir;
}
static void file(int dir, const char *name, const char *text) {
	int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	assert(fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text) && !close(fd));
}
static void reopen(fp_guard_t *g, int dir, bool write) {
	fp_guard_close(g);
	assert(fp_guard_open(g, dir, write, false));
}
typedef struct {
	int directory;
	const char *final;
	bool checked, cancel;
} copy_state_t;
static bool copy_current(void *user) {
	copy_state_t *s = user;
	struct stat st;
	if (!s->checked && !fstatat(s->directory, s->final, &st, 0) && st.st_size > 0) {
		s->checked = true;
		fp_guard_t g;
		assert(fp_guard_open(&g, s->directory, false, false));
		assert(fp_guard_lookup(&g, s->final) == FP_PENDING);
		assert(fp_guard_lookup(&g, "unrelated.flac") == FP_SAFE);
		fp_guard_close(&g);
	}
	return !s->cancel || !s->checked;
}
static bool reject_seal(bool (*seal)(void *), void *data, void *user) {
	(void)seal;
	(void)data;
	(void)user;
	return false;
}
static bool die_after_create(void *user) {
	nd_download_copy_t *c = user;
	struct stat st;
	if (!fstatat(c->directory, c->record.final, &st, 0))
		_exit(77);
	return true;
}
static void copy_checks(int root) {
	int dir = folder(root, "copy");
	nd_download_copy_t c = {.directory = dir, .record = record(1000)};
	strcpy(c.record.part, ".source.part");
	unsigned char bytes[65536];
	for (unsigned i = 0; i < sizeof(bytes); i++)
		bytes[i] = (unsigned char)(i * 7);
	int fd = openat(dir, c.record.part, O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(fd >= 0);
	sha256_ctx hash;
	sha256_init(&hash);
	for (int i = 0; i < 3; i++) {
		assert(write(fd, bytes, sizeof(bytes)) == sizeof(bytes));
		sha256_update(&hash, bytes, sizeof(bytes));
	}
	assert(!fsync(fd) && !close(fd));
	sha256_final_hex(&hash, c.record.digest);
	c.record.bytes = 3 * sizeof(bytes);
	for (int attempt = 0; attempt < 6; attempt++) {
		snprintf(c.record.operation, sizeof(c.record.operation), "%032x", 1000 + attempt);
		snprintf(c.record.final, sizeof(c.record.final), "copy-%d.flac", attempt);
		copy_state_t state = {.directory = dir, .final = c.record.final, .cancel = attempt == 4};
		c.current = copy_current;
		c.user = &state;
		c.commit = attempt == 5 ? reject_seal : NULL;
		copy_fault = attempt <= 3 ? attempt : 0;
		nd_download_publish_result_t result = nd_download_copy(&c);
		copy_fault = 0;
		assert(result == (attempt == 0 ? ND_DOWNLOAD_PUBLISHED : ND_DOWNLOAD_PUBLISH_ERROR));
		fp_guard_t g;
		assert(fp_guard_open(&g, dir, false, false));
		assert(fp_guard_lookup(&g, c.record.final) == (attempt == 0 ? FP_SAFE : FP_PENDING));
		assert(fp_guard_lookup(&g, "unrelated.flac") == FP_SAFE);
		fp_guard_close(&g);
		struct stat st;
		assert(!fstatat(dir, c.record.part, &st, 0) && st.st_size == c.record.bytes);
		assert(!fstatat(dir, c.record.final, &st, 0));
		if (attempt == 0)
			assert(st.st_size == c.record.bytes);
	}
	puts("PASS actual bounded copy, hidden-until-complete observation, write/fsync/close failures and cancellation/fence rejection");
	c.current = NULL;
	c.commit = NULL;
	c.user = NULL;
	strcpy(c.record.operation, "11111111111111111111111111111111");
	strcpy(c.record.final, "existing.flac");
	file(dir, c.record.final, "USER");
	assert(nd_download_copy(&c) == ND_DOWNLOAD_PUBLISH_CONFLICT);
	fd = openat(dir, c.record.final, O_RDONLY);
	char user[4];
	assert(fd >= 0 && read(fd, user, sizeof(user)) == 4 && !memcmp(user, "USER", 4) && !close(fd));
	fp_guard_t g;
	assert(fp_guard_open(&g, dir, false, false));
	assert(fp_guard_lookup(&g, c.record.final) == FP_SAFE);
	fp_guard_close(&g);
	strcpy(c.record.operation, "22222222222222222222222222222222");
	strcpy(c.record.final, "invalid.flac");
	c.record.digest[0] = c.record.digest[0] == 'a' ? 'b' : 'a';
	assert(nd_download_copy(&c) == ND_DOWNLOAD_PUBLISH_ERROR);
	assert(faccessat(dir, c.record.final, F_OK, 0) && errno == ENOENT);
	// Restore the expected digest and race independent processes, all with
	// separate SQLite handles and the same staged original/final filename.
	sha256_init(&hash);
	for (int i = 0; i < 3; i++)
		sha256_update(&hash, bytes, sizeof(bytes));
	sha256_final_hex(&hash, c.record.digest);
	strcpy(c.record.final, "race-copy.flac");
	pid_t children[8];
	for (unsigned i = 0; i < 8; i++) {
		children[i] = fork();
		assert(children[i] >= 0);
		if (!children[i]) {
			snprintf(c.record.operation, sizeof(c.record.operation), "%032x", 2000 + i);
			_exit(nd_download_copy(&c) == ND_DOWNLOAD_PUBLISHED ? 10 : 11);
		}
	}
	unsigned winners = 0;
	for (unsigned i = 0; i < 8; i++) {
		int status;
		assert(waitpid(children[i], &status, 0) == children[i] && WIFEXITED(status));
		assert(WEXITSTATUS(status) == 10 || WEXITSTATUS(status) == 11);
		winners += WEXITSTATUS(status) == 10;
	}
	assert(winners == 1);
	strcpy(c.record.operation, "33333333333333333333333333333333");
	strcpy(c.record.final, "crashed.flac");
	c.current = die_after_create;
	c.user = &c;
	pid_t child = fork();
	assert(child >= 0);
	if (!child) {
		nd_download_copy(&c);
		_exit(1);
	}
	int status;
	assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
	assert(fp_guard_open(&g, dir, false, false));
	assert(fp_guard_lookup(&g, "crashed.flac") == FP_PENDING);
	fp_guard_close(&g);
	c.current = NULL;
	c.user = NULL;
	strcpy(c.record.operation, "44444444444444444444444444444444");
	strcpy(c.record.final, "retried.flac");
	assert(nd_download_copy(&c) == ND_DOWNLOAD_PUBLISHED);
	assert(fp_guard_open(&g, dir, false, false));
	assert(fp_guard_lookup(&g, "crashed.flac") == FP_PENDING);
	assert(fp_guard_lookup(&g, "retried.flac") == FP_SAFE);
	fp_guard_close(&g);
	assert(!close(dir));
	puts("PASS copy collisions preserve unrelated content and bad source digest creates no final file");
	puts("PASS eight process publishers have one winner; killed writer stays protected and a fresh copy reuses staging");
}
int main(int argc, char **argv) {
	assert(argc == 2);
	int root = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	assert(root >= 0);
	int dir = folder(root, "ordinary");
	fp_guard_t g;
	assert(fp_guard_open(&g, dir, false, false));
	assert(fp_guard_lookup(&g, "ordinary.flac") == FP_SAFE);
	assert(faccessat(dir, FP_JOURNAL, F_OK, 0) && errno == ENOENT);
	fp_guard_close(&g);
	puts("PASS absent read-only guard creates no card metadata");

	assert(fp_guard_open(&g, dir, true, true));
	fp_record_t r = record(1), loaded;
	assert(fp_guard_begin(&g, &r));
	assert(fp_guard_lookup(&g, r.final) == FP_PENDING);
	assert(fp_guard_lookup(&g, "TRACK-1.FLAC") == FP_PENDING);
	assert(fp_guard_lookup(&g, r.part) == FP_PENDING);
	assert(fp_guard_lookup(&g, "unrelated.flac") == FP_SAFE);
	reopen(&g, dir, false);
	assert(fp_guard_get(&g, r.operation, &loaded) && loaded.phase == FP_INTENT);
	assert(fp_guard_lookup(&g, r.final) == FP_PENDING);
	assert(!fp_guard_begin(&g, &r));
	reopen(&g, dir, true);
	assert(!fp_guard_complete(&g, r.operation));
	assert(fp_guard_lookup(&g, r.final) == FP_UNKNOWN);
	reopen(&g, dir, true);
	assert(fp_guard_owned(&g, r.operation, 3, 4));
	reopen(&g, dir, false);
	assert(fp_guard_get(&g, r.operation, &loaded) && loaded.phase == FP_OWNED);
	assert(loaded.device == 3 && loaded.inode == 4);
	assert(fp_guard_lookup(&g, r.final) == FP_PENDING);
	reopen(&g, dir, true);
	assert(fp_guard_complete(&g, r.operation));
	reopen(&g, dir, false);
	assert(fp_guard_lookup(&g, r.final) == FP_SAFE);
	reopen(&g, dir, true);
	assert(fp_guard_retire_complete(&g, r.operation));
	assert(!fp_guard_get(&g, r.operation, &loaded));
	fp_guard_close(&g);
	puts("PASS durable intent/ownership/completion, read-only restart and unrelated-file isolation");

	assert(fp_guard_open(&g, dir, true, false));
	r = record(2);
	assert(fp_guard_begin(&g, &r));
	file(dir, r.final, "ambiguous existing audio");
	assert(!linkat(dir, r.final, dir, "alias.flac", 0));
	assert(fp_guard_lookup(&g, "alias.flac") == FP_PENDING);
	assert(!unlinkat(dir, "alias.flac", 0));
	assert(!fp_guard_retire_absent(&g, r.operation));
	fp_record_t collision = record(3);
	snprintf(collision.final, sizeof(collision.final), "TRACK-2.FLAC");
	assert(!fp_guard_begin(&g, &collision));
	reopen(&g, dir, true);
	assert(fp_guard_get(&g, r.operation, &loaded));
	assert(!fp_guard_get(&g, collision.operation, &loaded));
	assert(!fp_guard_owned(&g, collision.operation, 3, 4));
	reopen(&g, dir, true);
	assert(fp_guard_lookup(&g, r.final) == FP_PENDING);
	assert(!unlinkat(dir, r.final, 0)); // explicit fixture action, not automatic recovery
	assert(fp_guard_retire_absent(&g, r.operation));
	fp_guard_close(&g);
	puts("PASS stale operation/case collision cannot release another guard; ambiguous files preserved");

	assert(fp_guard_open(&g, dir, true, false));
	r = record(4);
	fault = 1;
	assert(!fp_guard_begin(&g, &r));
	reopen(&g, dir, false);
	assert(!fp_guard_get(&g, r.operation, &loaded));
	reopen(&g, dir, true);
	fault = 2;
	assert(!fp_guard_begin(&g, &r));
	assert(fp_guard_lookup(&g, r.final) == FP_UNKNOWN);
	reopen(&g, dir, false);
	assert(fp_guard_get(&g, r.operation, &loaded));
	assert(fp_guard_lookup(&g, r.final) == FP_PENDING);
	fp_guard_close(&g);
	puts("PASS rolled-back and uncertain committed intent preserve conservative visibility");

	int bounded = folder(root, "bounded");
	assert(fp_guard_open(&g, bounded, true, true));
	for (unsigned i = 0; i < FP_RECORD_LIMIT; i++) {
		r = record(i + 100);
		assert(fp_guard_begin(&g, &r));
	}
	r = record(999);
	assert(!fp_guard_begin(&g, &r));
	reopen(&g, bounded, false);
	r = record(100);
	assert(fp_guard_lookup(&g, r.final) == FP_PENDING);
	assert(fp_guard_lookup(&g, "unrelated") == FP_SAFE);
	fp_guard_close(&g);
	struct stat st;
	assert(!fstatat(bounded, FP_JOURNAL, &st, 0) && st.st_size <= FP_DATABASE_LIMIT);
	assert(!close(bounded));
	puts("PASS finite record/page bounds retain unresolved protection at capacity");

	const char *bad[] = {"empty", "corrupt", "symlink", "sidecar", "version", "malformed"};
	for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
		int b = folder(root, bad[i]);
		if (i < 2)
			file(b, FP_JOURNAL, i == 0 ? "" : "foreign data");
		else if (i == 2)
			assert(!symlinkat("../ordinary/" FP_JOURNAL, b, FP_JOURNAL));
		else if (i == 3)
			file(b, FP_JOURNAL "-journal", "orphan");
		else {
			assert(fp_guard_open(&g, b, true, true));
			if (i == 4)
				assert(sqlite3_exec(g.db, "PRAGMA user_version=99", NULL, NULL, NULL) == SQLITE_OK);
			else {
				r = record(9);
				assert(fp_guard_begin(&g, &r));
				assert(sqlite3_exec(g.db, "UPDATE publication SET digest='invalid'", NULL, NULL, NULL) == SQLITE_OK);
			}
			fp_guard_close(&g);
		}
		assert(!fp_guard_open(&g, b, false, false));
		assert(fp_guard_lookup(&g, "audio.flac") == FP_UNKNOWN);
		fp_guard_close(&g);
		assert(!fp_guard_open(&g, b, true, true));
		fp_guard_close(&g);
		assert(!close(b));
	}
	puts("PASS foreign/corrupt/unknown/symlink/orphan protection fails closed without rebuilding");

	char pending_path[4096], ordinary_path[4096], folder_path[4096], other_path[4096];
	snprintf(folder_path, sizeof(folder_path), "%s/ordinary", argv[1]);
	snprintf(pending_path, sizeof(pending_path), "%s/ordinary/Track-4.flac", argv[1]);
	snprintf(ordinary_path, sizeof(ordinary_path), "%s/ordinary/ordinary.flac", argv[1]);
	snprintf(other_path, sizeof(other_path), "%s/ordinary/moved.flac", argv[1]);
	file(dir, "Track-4.flac", "partial");
	file(dir, "ordinary.flac", "complete unrelated audio");
	int admitted = fp_read_begin(ordinary_path);
	assert(admitted >= 0);
	fp_admission_end(admitted);
	assert(fp_read_begin(pending_path) < 0);
	assert(fp_mutation_begin(pending_path, NULL, false) < 0);
	assert(fp_mutation_begin(folder_path, NULL, true) < 0);
	admitted = fp_mutation_begin(ordinary_path, other_path, false);
	assert(admitted >= 0);
	assert(!rename(ordinary_path, other_path));
	fp_admission_end(admitted);
	assert(!symlinkat("Track-4.flac", dir, "alias.flac"));
	snprintf(pending_path, sizeof(pending_path), "%s/ordinary/alias.flac", argv[1]);
	assert(fp_read_begin(pending_path) < 0);
	assert(!unlinkat(dir, "alias.flac", 0));
	assert(fp_guard_open(&g, dir, true, false));
	r = record(4);
	assert(fp_guard_owned(&g, r.operation, 3, 4));
	assert(fp_guard_complete(&g, r.operation));
	fp_guard_close(&g);
	admitted = fp_mutation_begin(folder_path, NULL, true);
	assert(admitted >= 0); // retained settled journal does not block directory operations
	fp_admission_end(admitted);
	assert(fp_guard_open(&g, dir, false, false));
	assert(fp_guard_lookup(&g, ".ND-123-0123456789ABCDEF.FLAC") == FP_PENDING);
	assert(fp_guard_lookup(&g, ".SONIX-DOWNLOAD-PUBLICATION.DB") == FP_PENDING);
	fp_guard_close(&g);
	file(dir, ".nd-123-0123456789abcdef.flac", "network partial");
	assert(fp_mutation_begin(folder_path, NULL, true) < 0);
	admitted = fp_read_begin(other_path);
	assert(admitted >= 0);
	fp_admission_end(admitted);
	puts("PASS per-path read/mutation admission, aliases, ancestors and network staging isolation");
	copy_checks(root);

	int sync[2];
	assert(!pipe(sync));
	pid_t child = fork();
	assert(child >= 0);
	if (!child) {
		close(sync[0]);
		int lock = fp_admission_begin(true);
		assert(lock >= 0);
		assert(write(sync[1], "x", 1) == 1);
		pause();
		_exit(1);
	}
	close(sync[1]);
	char byte;
	assert(read(sync[0], &byte, 1) == 1);
	assert(fp_admission_begin(true) < 0 && errno == EBUSY);
	assert(!kill(child, SIGKILL));
	int status;
	assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
	int lock = fp_admission_begin(true);
	assert(lock >= 0);
	fp_admission_end(lock);
	assert(!close(sync[0]));
	assert(!close(dir) && !close(root));
	puts("PASS finite cross-process admission and automatic release on handler death");
	puts("11 journal/copy/admission groups passed; no consumer or target acceptance claim");
	return 0;
}

// Native publication and unsupported-filesystem spike; opens no card or network.
#define _GNU_SOURCE
#include "src/system/streaming/navidromedownloadfile.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int injected_error;
int nd_download_publish_test_call(int directory, const char *part, const char *final) {
	if (injected_error) {
		errno = injected_error;
		return -1;
	}
#ifdef SYS_renameat2
	return syscall(SYS_renameat2, directory, part, directory, final, 1U);
#else
	errno = ENOSYS;
	return -1;
#endif
}

static void create(int directory, const char *name, char value) {
	int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	assert(fd >= 0);
	assert(write(fd, &value, 1) == 1);
	assert(!fsync(fd));
	assert(!close(fd));
}
static void contents(int directory, const char *name, char expected) {
	int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW);
	char bytes[2];
	assert(fd >= 0);
	assert(read(fd, bytes, sizeof(bytes)) == 1 && bytes[0] == expected);
	assert(!close(fd));
}
typedef struct {
	int directory, index;
	pthread_barrier_t *barrier;
	nd_download_publish_result_t result;
} race_t;
static void *race(void *data) {
	race_t *job = data;
	char part[32];
	snprintf(part, sizeof(part), ".race-%d.part", job->index);
	create(job->directory, part, (char)('A' + job->index));
	int rc = pthread_barrier_wait(job->barrier);
	assert(!rc || rc == PTHREAD_BARRIER_SERIAL_THREAD);
	job->result = nd_download_file_publish(job->directory, part, "race.flac");
	return NULL;
}
int main(int argc, char **argv) {
	assert(argc == 2);
	int directory = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	assert(directory >= 0);
	create(directory, ".success.part", 's');
	assert(nd_download_file_publish(directory, ".success.part", "success.flac") == ND_DOWNLOAD_PUBLISHED);
	contents(directory, "success.flac", 's');
	assert(faccessat(directory, ".success.part", F_OK, AT_SYMLINK_NOFOLLOW) == -1 && errno == ENOENT);
	puts("PASS native complete-file publication");

	create(directory, ".conflict.part", 'n');
	create(directory, "existing.flac", 'o');
	assert(nd_download_file_publish(directory, ".conflict.part", "existing.flac") == ND_DOWNLOAD_PUBLISH_CONFLICT);
	contents(directory, ".conflict.part", 'n');
	contents(directory, "existing.flac", 'o');
	puts("PASS existing destination preserves both files");

	assert(!symlinkat("existing.flac", directory, "link.flac"));
	assert(nd_download_file_publish(directory, ".conflict.part", "link.flac") == ND_DOWNLOAD_PUBLISH_CONFLICT);
	struct stat st;
	assert(!fstatat(directory, "link.flac", &st, AT_SYMLINK_NOFOLLOW) && S_ISLNK(st.st_mode));
	contents(directory, "existing.flac", 'o');
	puts("PASS destination symlink is not replaced or followed");

	assert(!symlinkat("existing.flac", directory, ".link.part"));
	assert(nd_download_file_publish(directory, ".link.part", "invalid.flac") == ND_DOWNLOAD_PUBLISH_ERROR);
	assert(nd_download_file_publish(directory, "../escape", "invalid.flac") == ND_DOWNLOAD_PUBLISH_ERROR);
	assert(nd_download_file_publish(directory, ".conflict.part", "../escape") == ND_DOWNLOAD_PUBLISH_ERROR);
	assert(nd_download_file_publish(directory, ".conflict.part", ".hidden.flac") == ND_DOWNLOAD_PUBLISH_ERROR);
	puts("PASS invalid publication inputs rejected");

	int unsupported[] = {ENOSYS, EINVAL, EOPNOTSUPP};
	for (size_t i = 0; i < sizeof(unsupported) / sizeof(*unsupported); i++) {
		injected_error = unsupported[i];
		assert(nd_download_file_publish(directory, ".conflict.part", "unsupported.flac") == ND_DOWNLOAD_PUBLISH_UNSUPPORTED);
		assert(faccessat(directory, "unsupported.flac", F_OK, AT_SYMLINK_NOFOLLOW) == -1 && errno == ENOENT);
		contents(directory, ".conflict.part", 'n');
	}
	injected_error = EIO;
	assert(nd_download_file_publish(directory, ".conflict.part", "io.flac") == ND_DOWNLOAD_PUBLISH_ERROR);
	assert(faccessat(directory, "io.flac", F_OK, AT_SYMLINK_NOFOLLOW) == -1 && errno == ENOENT);
	contents(directory, ".conflict.part", 'n');
	injected_error = 0;
	puts("PASS unsupported and I/O failures never use a replacing fallback");

	enum { CONTENDERS = 16 };
	pthread_barrier_t barrier;
	assert(!pthread_barrier_init(&barrier, NULL, CONTENDERS));
	pthread_t threads[CONTENDERS];
	race_t jobs[CONTENDERS];
	for (int i = 0; i < CONTENDERS; i++) {
		jobs[i] = (race_t){.directory = directory, .index = i, .barrier = &barrier};
		assert(!pthread_create(&threads[i], NULL, race, &jobs[i]));
	}
	int winners = 0;
	for (int i = 0; i < CONTENDERS; i++) {
		assert(!pthread_join(threads[i], NULL));
		if (jobs[i].result == ND_DOWNLOAD_PUBLISHED) {
			winners++;
			contents(directory, "race.flac", (char)('A' + i));
		} else {
			assert(jobs[i].result == ND_DOWNLOAD_PUBLISH_CONFLICT);
			char part[32];
			snprintf(part, sizeof(part), ".race-%d.part", i);
			contents(directory, part, (char)('A' + i));
		}
	}
	assert(winners == 1);
	assert(!pthread_barrier_destroy(&barrier));
	assert(!close(directory));
	puts("PASS 16 concurrent publishers: one winner, 15 unchanged partials");
	puts("6 publication checks passed; no target-filesystem claim");
	return 0;
}

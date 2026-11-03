#define _GNU_SOURCE
#include "src/system/streaming/navidromedownloadfile.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
	assert(argc == 2);
	int dir = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	assert(dir >= 0);
	int fd = openat(dir, ".no-syscall.part", O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(fd >= 0 && write(fd, "x", 1) == 1 && !close(fd));
	assert(nd_download_file_publish(dir, ".no-syscall.part", "no-syscall.flac") == ND_DOWNLOAD_PUBLISH_UNSUPPORTED);
	assert(errno == ENOSYS);
	assert(!faccessat(dir, ".no-syscall.part", F_OK, 0));
	assert(faccessat(dir, "no-syscall.flac", F_OK, 0) == -1 && errno == ENOENT);
	assert(!close(dir));
	puts("PASS compiled absent-syscall path preserves staging without publication");
	return 0;
}

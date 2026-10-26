#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "navidromedownloadfile.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

// Use the syscall, not glibc's renameat2 symbol: the target libc predates it.
// A supported syscall alone is insufficient; the mounted filesystem must also
// implement RENAME_NOREPLACE. In particular, stock Linux 4.4 VFAT does not.
static int publish_call(int directory, const char *part, const char *final) {
#ifdef NAVIDROME_DOWNLOAD_PUBLISH_TEST
	extern int nd_download_publish_test_call(int, const char *, const char *);
	return nd_download_publish_test_call(directory, part, final);
#elif defined(SYS_renameat2)
	return syscall(SYS_renameat2, directory, part, directory, final, 1U);
#else
	(void)directory;
	(void)part;
	(void) final;
	errno = ENOSYS;
	return -1;
#endif
}

static bool basename_valid(const char *name) { return name && name[0] && strcmp(name, ".") && strcmp(name, "..") && !strchr(name, '/') && !strchr(name, '\\'); }

nd_download_publish_result_t nd_download_file_publish(int directory, const char *part, const char *final) {
	if (!basename_valid(part) || !basename_valid(final) || !strcmp(part, final) || part[0] != '.' || final[0] == '.') {
		errno = EINVAL;
		return ND_DOWNLOAD_PUBLISH_ERROR;
	}
	struct stat st;
	if (fstatat(directory, part, &st, AT_SYMLINK_NOFOLLOW))
		return ND_DOWNLOAD_PUBLISH_ERROR;
	if (!S_ISREG(st.st_mode)) {
		errno = EINVAL;
		return ND_DOWNLOAD_PUBLISH_ERROR;
	}
	if (!publish_call(directory, part, final))
		return ND_DOWNLOAD_PUBLISHED;
	if (errno == EEXIST || errno == ENOTEMPTY)
		return ND_DOWNLOAD_PUBLISH_CONFLICT;
	if (errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP)
		return ND_DOWNLOAD_PUBLISH_UNSUPPORTED;
	return ND_DOWNLOAD_PUBLISH_ERROR;
}

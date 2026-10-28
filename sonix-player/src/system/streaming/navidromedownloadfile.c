#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "navidromedownloadfile.h"
#include "src/system/core/diagnostic.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/vfs.h>
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
	DIAGNOSTIC_SCOPE(D_DOWNLOAD);
	_diagnostic_scope.outcome = D_FAILED;
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
	// Inspect the already-owned directory on this worker; never probe by path
	// or issue another publication attempt for diagnostics.
	struct statfs fs = {0};
	bool measured = diagnostic_enabled();
	bool fs_ok = measured && !fstatfs(directory, &fs);
	int kernel = -1;
	if (measured) {
		struct utsname system;
		unsigned major, minor, patch;
		if (!uname(&system) && sscanf(system.release, "%u.%u.%u", &major, &minor, &patch) == 3 && major < 256 && minor < 256 && patch < 256)
			kernel = (int)((major << 16) | (minor << 8) | patch);
	}
#ifdef HOST_BUILD
	const int build = 1;
#else
	const int build = 2;
#endif
	int result = publish_call(directory, part, final);
	int publish_errno = result ? errno : 0;
#ifdef SYS_renameat2
	const int available = 1;
#else
	const int available = 0;
#endif
	if (measured)
		DIAG(diagnostic_current(), D_PUBLISH, result ? D_FAILED : D_OK, DV(D_SYSCALL, available), DV(D_STRATEGY, 1), DV(D_ERRNO, publish_errno), DV(D_FS_MAGIC, fs_ok ? (int64_t)fs.f_type : -1), DV(D_KERNEL, kernel), DV(D_BUILD, build));
	errno = publish_errno;
	if (!result) {
		_diagnostic_scope.outcome = D_OK;
		return ND_DOWNLOAD_PUBLISHED;
	}
	if (errno == EEXIST || errno == ENOTEMPTY)
		return ND_DOWNLOAD_PUBLISH_CONFLICT;
	if (errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP)
		return ND_DOWNLOAD_PUBLISH_UNSUPPORTED;
	return ND_DOWNLOAD_PUBLISH_ERROR;
}

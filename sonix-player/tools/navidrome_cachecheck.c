#include "src/system/streaming/navidromecache.c"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *fail_name;
static bool cancel_after_unlink, cancelled;
bool navidrome_playback_current(const nd_context_t *context, bool network) {
	(void)context;
	(void)network;
	return !cancelled;
}
int __real_unlinkat(int fd, const char *path, int flags);
int __wrap_unlinkat(int fd, const char *path, int flags) {
	if (fail_name && !strcmp(path, fail_name)) {
		errno = EACCES;
		return -1;
	}
	int result = __real_unlinkat(fd, path, flags);
	if (!result && cancel_after_unlink)
		cancelled = true;
	return result;
}
int main(int argc, char **argv) {
	if (argc < 3)
		return 2;
	fail_name = argc > 3 ? argv[3] : NULL;
	nd_cache_usage_t usage = {0};
	int mode = atoi(argv[2]);
	cancel_after_unlink = mode == 2;
	nd_context_t context = {0};
	bool ok = mode == 2 ? scan(argv[1], true, &usage, &context) : nd_cache_scan(argv[1], mode != 0, &usage);
	printf("%d %d %llu %llu\n", ok, usage.ready, (unsigned long long)usage.audio, (unsigned long long)usage.art);
	return 0;
}

// Full service sync against synthetic loopback metadata. Never uses real config.
#include "src/system/streaming/navidrome.h"
#include <assert.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static char settings[1200];
const char *config_get(const char *section, const char *key, const char *fallback) {
	if (!strcmp(section, "network") && !strcmp(key, "ca_bundle") && getenv("SONIX_TEST_CA"))
		return getenv("SONIX_TEST_CA");
	return fallback;
}
const char *config_path(void) { return settings; }
const char *tr(const char *key) { return key; }
bool gui_post(void (*cb)(void *), void *user) {
	cb(user);
	return true;
}
void thread_be_background(const char *name) { (void)name; }
static double seconds(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}
static void wait_idle(void) {
	double deadline = seconds() + 90;
	for (;;) {
		navidrome_snapshot_t s;
		navidrome_snapshot(&s);
		nd_library_t lib;
		navidrome_library_snapshot(&lib);
		if (!s.busy && !lib.loading)
			return;
		assert(seconds() < deadline);
		usleep(1000);
	}
}
static int descriptors(void) {
	DIR *dir = opendir("/proc/self/fd");
	if (!dir)
		return -1;
	int count = 0;
	struct dirent *entry;
	while ((entry = readdir(dir)))
		if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
			count++;
	closedir(dir);
	return count;
}
int main(int argc, char **argv) {
	assert(argc >= 5);
	signal(SIGPIPE, SIG_IGN);
	snprintf(settings, sizeof(settings), "%s/config.ini", argv[2]);
	navidrome_init();
	navidrome_set_root(argv[2]);
	assert(navidrome_save(argv[1], "fixture user", "synthetic-server-password", NULL));
	wait_idle();
	navidrome_snapshot_t s;
	navidrome_snapshot(&s);
	assert(s.state == ND_CONNECTED);
	if (argc > 6 && !strcmp(argv[6], "lock")) {
		assert(navidrome_encrypt("vault1"));
		wait_idle();
	}
	int fd_before = descriptors();
	double start = seconds();
	assert(navidrome_sync());
	if (argc > 6) {
		usleep(30000);
		double cancelling = seconds();
		if (!strcmp(argv[6], "card"))
			navidrome_set_root(NULL);
		else if (!strcmp(argv[6], "forget"))
			assert(navidrome_forget());
		else if (!strcmp(argv[6], "shutdown"))
			navidrome_shutdown();
		else if (!strcmp(argv[6], "lock"))
			navidrome_lock();
		else if (!strcmp(argv[6], "network"))
			navidrome_test_online(0);
		else if (!strcmp(argv[6], "reset"))
			navidrome_reset();
		else if (!strcmp(argv[6], "profile")) {
			assert(navidrome_forget());
			wait_idle();
			assert(navidrome_save(argv[1], "fixture user", "synthetic-server-password", NULL));
		} else
			navidrome_sync_cancel();
		wait_idle();
		assert(seconds() - cancelling < 2.0);
	} else
		wait_idle();
	nd_library_t lib;
	navidrome_library_snapshot(&lib);
	if (argc > 6 && strcmp(argv[6], "cancel")) {
		assert(!lib.ready);
	} else if (argc > 5) {
		assert(lib.error && !strcmp(lib.error, argv[5]));
		assert(!lib.ready);
	} else {
		if (lib.error)
			fprintf(stderr, "fixture sync result=%s\n", lib.error);
		assert(lib.ready && !lib.error);
		assert(lib.counts[ND_TRACKS] == atoi(argv[3]));
		assert(lib.counts[ND_ALBUMS] == atoi(argv[4]));
	}
	navidrome_shutdown();
	if (argc > 6)
		assert(seconds() - start < 2.0);
	struct rusage usage;
	getrusage(RUSAGE_SELF, &usage);
	int fd_after = descriptors();
	if (fd_before >= 0 && fd_after > fd_before)
		fprintf(stderr, "fixture descriptors before=%d after=%d\n", fd_before, fd_after);
	assert(fd_before < 0 || fd_after <= fd_before);
	printf("{\"seconds\":%.6f,\"rss_kib\":%ld,\"sqlite_peak_bytes\":%lld,\"fd_before\":%d,\"fd_after\":%d,\"tracks\":%d,\"albums\":%d}\n", seconds() - start, usage.ru_maxrss, (long long)sqlite3_memory_highwater(0), fd_before, fd_after, lib.counts[ND_TRACKS], lib.counts[ND_ALBUMS]);
	return 0;
}

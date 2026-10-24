#define _GNU_SOURCE 1
#include "src/system/core/logging.h"
#include "src/system/streaming/navidromedebug.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static atomic_int hold_write, write_blocked, fail_write;
static atomic_int tapped;
static atomic_long naming_clock;
time_t __real_time(time_t *out);
time_t __wrap_time(time_t *out) {
	time_t value = atomic_load(&naming_clock);
	if (!value)
		return __real_time(out);
	if (out)
		*out = value;
	return value;
}
const char *tr(const char *key) { return key; }
void thread_be_background(const char *name) { (void)name; }
void thread_be_realtime(const char *name, int priority) {
	(void)name;
	(void)priority;
}
static void tap(const char *line, size_t size) {
	(void)line;
	(void)size;
	atomic_fetch_add(&tapped, 1);
}
ssize_t __real_write(int, const void *, size_t);
ssize_t __wrap_write(int fd, const void *data, size_t size) {
	if (fd > 2 && atomic_load(&hold_write)) {
		atomic_store(&write_blocked, 1);
		while (atomic_load(&hold_write))
			usleep(1000);
	}
	if (fd > 2 && atomic_exchange(&fail_write, 0)) {
		errno = ENOSPC;
		return -1;
	}
	return __real_write(fd, data, size);
}

static nd_debug_state_t wait_state(bool recording) {
	nd_debug_state_t s;
	for (int i = 0; i < 5000; i++) {
		nd_debug_snapshot(&s);
		if (!s.pending && s.recording == recording)
			return s;
		usleep(1000);
	}
	assert(!"debug writer timeout");
	return s;
}

static void no_handles(const char *root) {
	DIR *dir = opendir("/proc/self/fd");
	assert(dir);
	struct dirent *entry;
	while ((entry = readdir(dir))) {
		char path[512], target[1500];
		snprintf(path, sizeof(path), "/proc/self/fd/%s", entry->d_name);
		ssize_t n = readlink(path, target, sizeof(target) - 1);
		if (n < 0)
			continue;
		target[n] = 0;
		assert(!strstr(target, root));
	}
	closedir(dir);
}

static unsigned inspect(const char *root, bool *dropped) {
	char folder[1200];
	snprintf(folder, sizeof(folder), "%s/.local", root);
	DIR *dir = opendir(folder);
	assert(dir);
	struct dirent *entry;
	unsigned count = 0;
	while ((entry = readdir(dir))) {
		if (strncmp(entry->d_name, "navidrome-debug-", 16) || !strcmp(entry->d_name, "navidrome-debug-user.log"))
			continue;
		char path[1500], text[5000];
		snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
		struct stat st;
		assert(!stat(path, &st) && st.st_size <= 4096 && (st.st_mode & 0777) == 0600);
		FILE *f = fopen(path, "r");
		assert(f);
		size_t n = fread(text, 1, sizeof(text) - 1, f);
		text[n] = 0;
		fclose(f);
		assert(!strstr(text, "unrelated canary"));
		if (strstr(text, "dropped_lines="))
			*dropped = true;
		count++;
	}
	closedir(dir);
	assert(count <= 5);
	return count;
}

int main(void) {
	alarm(30);
	logging_init();
	logging_set_line_tap(tap); // The hook normally owned by Bluetooth.
	char root[] = "/tmp/sonix-nd-debug-XXXXXX";
	assert(mkdtemp(root));
	atomic_store(&naming_clock, 1700000000);
	nd_debug_enable(true);
	nd_debug_state_t s = wait_state(false);
	assert(s.enabled && s.error);
	nd_debug_card(root);
	s = wait_state(true);
	nd_log("navidrome: fixture status=200 bytes=12\n");
	printf("general unrelated canary\n");
	logging_flush();
	assert(atomic_load(&tapped) >= 2);
	char first[1200];
	snprintf(first, sizeof(first), "%s", s.path);
	nd_debug_line("unrelated canary");
	atomic_store(&hold_write, 1);
	nd_debug_line("navidrome: fixture status=200 bytes=12");
	while (!atomic_load(&write_blocked))
		usleep(1000);
	for (int i = 0; i < 1000; i++)
		nd_debug_line("navidrome: fixture status=200 bytes=12");
	atomic_store(&hold_write, 0);
	nd_debug_enable(false);
	wait_state(false);
	no_handles(root);
	bool dropped = false;
	assert(inspect(root, &dropped) == 1 && dropped);
	nd_debug_enable(true);
	s = wait_state(true);
	assert(strcmp(first, s.path)); // Same-second sessions never overwrite.
	nd_debug_enable(false);
	wait_state(false);
	atomic_store(&naming_clock, 1600000000); // A clock correction moves backward.
	nd_debug_enable(true);
	s = wait_state(true);
	assert(strcmp(first, s.path));
	char foreign[1500];
	snprintf(foreign, sizeof(foreign), "%s/.local/navidrome-debug-user.log", root);
	FILE *f = fopen(foreign, "w");
	assert(f && fputs("owned by user", f) >= 0 && !fclose(f));
	for (int i = 0; i < 1200; i++) {
		nd_debug_line("navidrome: fixture status=200 bytes=12");
		if (i % 8 == 0)
			usleep(1000);
	}
	nd_debug_enable(false);
	wait_state(false);
	assert(inspect(root, &dropped) == 5 && !access(foreign, F_OK));
	nd_debug_enable(true);
	wait_state(true);
	atomic_store(&fail_write, 1);
	nd_debug_line("navidrome: fixture status=200 bytes=12");
	s = wait_state(false);
	assert(s.enabled && s.error);
	nd_debug_card(NULL);
	no_handles(root);
	assert(!chmod(root, 0500));
	nd_debug_card(root);
	s = wait_state(false);
	assert(s.enabled && s.error);
	assert(!chmod(root, 0700));
	nd_debug_card(root);
	wait_state(true);
	nd_debug_card(NULL);
	no_handles(root);
	nd_debug_enable(false);
	wait_state(false);
	nd_debug_shutdown();
	char folder[1200];
	snprintf(folder, sizeof(folder), "%s/.local", root);
	DIR *dir = opendir(folder);
	struct dirent *entry;
	while ((entry = readdir(dir))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		char path[1500];
		snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
		assert(!unlink(path));
	}
	closedir(dir);
	assert(!rmdir(folder) && !rmdir(root));
	puts("Debug queue/overflow, collision-safe sessions, bounded retention, full/read-only/absent storage and handle release: passed");
	logging_flush();
}

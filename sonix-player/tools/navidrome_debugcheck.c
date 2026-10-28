#define _GNU_SOURCE
#include "src/system/core/config.h"
#include "src/system/core/diagnostic.h"
#include "src/system/core/logging.h"
#include "src/system/streaming/navidromedebug.h"
#include "src/system/streaming/navidromedownloadfile.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

const char *tr(const char *key) { return key; }
void thread_be_background(const char *name) { (void)name; }
void thread_be_realtime(const char *name, int priority) {
	(void)name;
	(void)priority;
}
static atomic_bool hold, entered;
static atomic_int write_fault;
static atomic_bool readonly_card;
FILE *__real_fopen(const char *, const char *);
FILE *__wrap_fopen(const char *path, const char *mode) {
	if (atomic_load(&readonly_card) && strstr(path, "sonix_player.log") && (strchr(mode, 'w') || strchr(mode, 'a'))) {
		errno = EROFS;
		return NULL;
	}
	return __real_fopen(path, mode);
}
static atomic_long wall_shift;
int __real_clock_gettime(clockid_t, struct timespec *);
int __wrap_clock_gettime(clockid_t id, struct timespec *value) {
	int result = __real_clock_gettime(id, value);
	if (!result && id == CLOCK_REALTIME)
		value->tv_sec += atomic_load(&wall_shift);
	return result;
}
ssize_t __real_write(int, const void *, size_t);
ssize_t __wrap_write(int fd, const void *data, size_t n) {
	if ((fd == 1 || fd == 2) && atomic_load(&hold)) {
		atomic_store(&entered, true);
		while (atomic_load(&hold))
			usleep(1000);
	}
	if ((fd == 1 || fd == 2) && atomic_load(&write_fault)) {
		errno = ENOSPC;
		return atomic_load(&write_fault) == 1 ? -1 : 0;
	}
	return __real_write(fd, data, n);
}
static int publication_errno, publication_calls;
int nd_download_publish_test_call(int directory, const char *part, const char *final) {
	(void)directory;
	(void)part;
	(void) final;
	publication_calls++;
	errno = publication_errno;
	return publication_errno ? -1 : 0;
}
static void *concurrent_producer(void *arg) {
	uintptr_t index = (uintptr_t)arg;
	diagnostic_t parent = diagnostic_begin(D_PLAY, 0);
	diagnostic_t previous = logging_diagnostic_attach(parent);
	diagnostic_t child = diagnostic_begin(D_ART, diagnostic_current().id);
	assert(child.parent == parent.id && child.id != parent.id);
	for (int i = 0; i < 1000; i++) {
		assert(diagnostic_current().id == parent.id);
		DIAG(child, D_PROGRESS, D_PENDING, DV(D_COUNT, i), DV(D_KIND, index));
	}
	DIAG(child, D_END, D_OK);
	logging_diagnostic_attach(previous);
	assert(diagnostic_current().id == previous.id);
	return NULL;
}
static char *read_file(const char *path) {
	FILE *f = fopen(path, "rb");
	assert(f);
	assert(!fseek(f, 0, SEEK_END));
	long n = ftell(f);
	rewind(f);
	char *s = calloc((size_t)n + 1, 1);
	assert(s);
	assert(fread(s, 1, (size_t)n, f) == (size_t)n);
	fclose(f);
	return s;
}
static void migration(const char *path) {
	for (int old = 0; old < 2; old++)
		for (int general = 0; general < 2; general++) {
			unlink(path);
			config_init(path);
			config_set_bool("navidrome", "debug", old);
			config_set_bool("system", "log_to_sd", general);
			assert(logging_migrate_navidrome());
			assert(config_init(path));
			assert(config_get_bool("system", "log_to_sd", false) == (old || general));
			assert(!config_get_bool("navidrome", "debug", true));
			config_set_bool("system", "log_to_sd", false);
			assert(config_save());
			assert(logging_migrate_navidrome());
			assert(config_init(path));
			assert(!config_get_bool("system", "log_to_sd", true));
		}
	config_init("/dev/null/impossible.ini");
	assert(!logging_migrate_navidrome());
	assert(!logging_migrate_navidrome()); // a failed marker is not durable success
	assert(config_init(path));
}
int main(int argc, char **argv) {
	assert(argc == 3);
	int report = dup(STDOUT_FILENO);
	assert(report >= 0);
	char root[1024], path[1200], config[1200], old[1300];
	snprintf(root, sizeof(root), "%s", argv[1]);
	snprintf(config, sizeof(config), "%s/settings.ini", root);
	migration(config);
	bool explicit = !strcmp(argv[2], "explicit");
	if (explicit) {
		snprintf(path, sizeof(path), "%s/explicit.log", root);
		setenv("SONIX_LOG", path, 1);
	} else {
		unsetenv("SONIX_LOG");
		snprintf(path, sizeof(path), "%s/.local/sonix_player.log", root);
	}
	char legacy[1200];
	snprintf(legacy, sizeof(legacy), "%s/navidrome-debug-legacy.log", root);
	FILE *legacy_file = fopen(legacy, "we");
	assert(legacy_file);
	fputs("legacy sentinel", legacy_file);
	fclose(legacy_file);
	logging_init();
	config_set_bool("system", "log_to_sd", true);
	if (!explicit)
		logging_attach_sd(root);
	assert(diagnostic_enabled());
	diagnostic_t a = diagnostic_begin(D_PLAY, 0);
	logging_flush();
	diagnostic_t b = diagnostic_begin(D_ART, a.id);
	logging_flush();
	DIAG(a, D_RESPONSE, D_OK, DV(D_STATUS, 200));
	logging_flush();
	DIAG(a, D_DECODE, D_FAILED, DV(D_ERRNO, EINVAL));
	logging_flush();
	DIAG(b, D_PRIORITY_WAIT, D_PENDING, DV(D_WAIT_US, 12000));
	logging_flush();
	DIAG(b, D_END, D_CANCELLED);
	logging_flush();
	DIAG(a, D_END, D_FAILED);
	logging_flush();
	nd_log("navidrome: fixed legacy marker\n");
	logging_flush();
	char *text = read_file(path);
	assert(strstr(text, "component=play event=decode result=failed"));
	assert(strstr(text, "component=art event=end result=cancelled"));
	assert(strstr(text, "fixed legacy marker"));
	assert(!strstr(strstr(text, "fixed legacy marker") + 1, "fixed legacy marker"));
	for (char *line = text; line && *line;) {
		char *end = strchr(line, '\n');
		if (!end)
			break;
		if (!strncmp(line, "navidrome-perf ", 15))
			assert(end - line + 1 <= 768);
		line = end + 1;
	}
	free(text);
	if (explicit) {
		int dir = open(root, O_RDONLY | O_DIRECTORY);
		assert(dir >= 0);
		int part = openat(dir, ".PRIVATE_MEDIA_CANARY", O_CREAT | O_WRONLY, 0600);
		assert(part >= 0);
		close(part);
		const int errors[] = {EOPNOTSUPP, EEXIST, EIO, 0};
		const nd_download_publish_result_t expected[] = {ND_DOWNLOAD_PUBLISH_UNSUPPORTED, ND_DOWNLOAD_PUBLISH_CONFLICT, ND_DOWNLOAD_PUBLISH_ERROR, ND_DOWNLOAD_PUBLISHED};
		// Hold only the real writer, so production emission is deterministic while
		// actual publication classification/filesystem inspection executes normally.
		atomic_store(&entered, false);
		atomic_store(&hold, true);
		nd_log("navidrome: publication fixture boundary\n");
		for (int i = 0; i < 1000 && !atomic_load(&entered); i++)
			usleep(1000);
		assert(atomic_load(&entered));
		for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
			publication_errno = errors[i];
			assert(nd_download_file_publish(dir, ".PRIVATE_MEDIA_CANARY", "PRIVATE_TITLE_CANARY") == expected[i]);
			assert(errno == errors[i]);
		}
		assert(publication_calls == 4);
		atomic_store(&hold, false);
		logging_flush();
		char *record = read_file(path);
		assert(strstr(record, "component=download event=publish result=ok"));
		assert(strstr(record, "component=download event=publish result=failed"));
		for (int i = 0; i < 3; i++) {
			char value[40];
			snprintf(value, sizeof(value), "errno=%d ", errors[i]);
			assert(strstr(record, value));
		}
		assert(strstr(record, "fs_magic=") && strstr(record, "kernel="));
		assert(!strstr(record, "PRIVATE_MEDIA_CANARY") && !strstr(record, "PRIVATE_TITLE_CANARY"));
		free(record);
		unlinkat(dir, ".PRIVATE_MEDIA_CANARY", 0);
		close(dir);
	}
	// Wall-clock correction cannot make producer elapsed time go backwards.
	uint64_t before_clock = logging_diagnostic_clock();
	atomic_store(&wall_shift, -86400);
	DIAG(a, D_PROGRESS, D_PENDING);
	atomic_store(&wall_shift, 86400);
	assert(logging_diagnostic_clock() >= before_clock);
	atomic_store(&wall_shift, 0);
	// Concurrent callers retain thread-local parentage; recurring summaries stay
	// bounded even when callers update counters much faster than the writer.
	pthread_t producers[8];
	for (uintptr_t i = 0; i < 8; i++)
		assert(!pthread_create(&producers[i], NULL, concurrent_producer, (void *)i));
	for (int i = 0; i < 8; i++)
		assert(!pthread_join(producers[i], NULL));
	logging_flush();
	usleep(1100000);
	if (explicit) {
		atomic_store(&entered, false);
		atomic_store(&hold, true);
		nd_log("navidrome: bounded registry boundary\n");
		for (int i = 0; i < 1000 && !atomic_load(&entered); i++)
			usleep(1000);
		assert(atomic_load(&entered));
		diagnostic_t summaries[160];
		for (int i = 0; i < 160; i++) {
			summaries[i] = diagnostic_begin(D_BROWSE, 0);
			DIAG(summaries[i], D_PROGRESS, D_PENDING, DV(D_COUNT, i));
		}
		// Exhaust the ordinary shared queue too, independently of typed rate caps.
		for (int i = 0; i < 2000; i++)
			nd_log("navidrome: synthetic queue saturation 012345678901234567890123456789\n");
		DIAG(a, D_END, D_FAILED);
		atomic_store(&hold, false);
		logging_flush();
		usleep(1100000);
		DIAG(a, D_END, D_FAILED);
		logging_flush();
		char *record = read_file(path);
		bool context_loss = false;
		for (char *at = record; (at = strstr(at, " context=")); at++)
			if (strtoul(at + 9, NULL, 10))
				context_loss = true;
		assert(context_loss);
		free(record);
	}
	// A held writer cannot delay the audio producer; rate/queue losses are visible.
	atomic_store(&hold, true);
	diagnostic_t flood = diagnostic_begin(D_AUDIO, 0);
	for (int i = 0; i < 1000 && !atomic_load(&entered); i++)
		usleep(1000);
	assert(atomic_load(&entered));
	uint64_t start = logging_diagnostic_clock();
	for (int i = 0; i < 20000; i++)
		DIAG(flood, D_UNDERRUN, D_FAILED, DV(D_ERRNO, EPIPE));
	assert(logging_diagnostic_clock() - start < 1000000);
	atomic_store(&hold, false);
	logging_flush();
	usleep(1100000);
	DIAG(flood, D_END, D_FAILED);
	logging_flush();
	text = read_file(path);
	assert(strstr(text, "event=loss"));
	free(text);
	if (!explicit) {
		// Both a full destination and an anomalous zero-byte writer must drain the
		// bounded queue without spinning, and preserve enabled/unavailable status.
		for (int fault = 1; fault <= 2; fault++) {
			atomic_store(&write_fault, fault);
			nd_log("navidrome: storage fault fixture\n");
			logging_flush();
			assert(logging_to_sd());
			assert(!strcmp(logging_path(), "log_card_missing"));
			atomic_store(&write_fault, 0);
			nd_log("navidrome: storage recovered fixture\n");
			logging_flush();
			assert(!strcmp(logging_path(), path));
		}
		// Live rotation, without a reopen, and a bounded two-file footprint.
		for (int j = 0; j < 15; j++) {
			for (int i = 0; i < 100; i++)
				nd_log("navidrome: rotation fixture 012345678901234567890123456789012345678901234567890123456789\n");
			logging_flush();
		}
		snprintf(old, sizeof(old), "%s.old", path);
		struct stat st;
		assert(!stat(old, &st) && st.st_size <= 4096);
		assert(!stat(path, &st) && st.st_size <= 4096);
		// No feature file writer. Capture pauses during USB; old contexts cannot
		// masquerade as newly timed operations on resume.
		logging_suspend_for_usb();
		assert(!diagnostic_enabled());
		DIAG(a, D_PROGRESS, D_PENDING);
		logging_resume_after_usb();
		assert(diagnostic_enabled());
		DIAG(a, D_END, D_SUPERSEDED);
		logging_flush();
		text = read_file(path);
		assert(strstr(text, "elapsed_us=-1"));
		free(text);
		logging_suspend_for_usb();
		atomic_store(&readonly_card, true);
		logging_resume_after_usb();
		assert(logging_to_sd() && !strcmp(logging_path(), "log_card_missing"));
		atomic_store(&readonly_card, false);
		logging_attach_sd(root);
		assert(diagnostic_enabled());
		// A replacement root is a fresh capture; retired diagnostic records must
		// never cross over, and opening an unavailable destination keeps preference.
		char replacement[1200], replacement_log[1300];
		snprintf(replacement, sizeof(replacement), "%s/replacement", root);
		assert(!mkdir(replacement, 0700));
		logging_suspend_for_usb();
		DIAG(a, D_PROGRESS, D_PENDING, DV(D_REASON, 999123));
		logging_attach_sd(replacement);
		assert(diagnostic_enabled());
		DIAG(a, D_END, D_SUPERSEDED);
		logging_flush();
		snprintf(replacement_log, sizeof(replacement_log), "%s/.local/sonix_player.log", replacement);
		text = read_file(replacement_log);
		assert(!strstr(text, "reason=999123"));
		assert(strstr(text, "elapsed_us=-1"));
		free(text);
		logging_suspend_for_usb();
		logging_attach_sd("/dev/null/not-a-card");
		assert(logging_to_sd() && !strcmp(logging_path(), "log_card_missing"));
		logging_attach_sd(replacement);
		assert(diagnostic_enabled());
		logging_set_to_sd(false);
		assert(!diagnostic_enabled());
		DIAG(a, D_END, D_FAILED);
		logging_flush();
	}
	if (explicit) {
		char *record = read_file(path);
		uint64_t current_second = 0;
		size_t bytes = 0;
		for (char *line = record; line && *line;) {
			char *end = strchr(line, '\n');
			if (!end)
				break;
			*end = 0;
			if (!strncmp(line, "navidrome-perf ", 15)) {
				char *time = strstr(line, "mono_us=");
				assert(time);
				uint64_t second = strtoull(time + 8, NULL, 10) / 1000000;
				if (second != current_second) {
					current_second = second;
					bytes = 0;
				}
				bytes += end - line + 1;
				assert(bytes <= 32768);
			}
			line = end + 1;
		}
		free(record);
	}
	char *legacy_text = read_file(legacy);
	assert(!strcmp(legacy_text, "legacy sentinel"));
	free(legacy_text);
	dprintf(report, "consolidated diagnostics %s: migration, correlation, bounded producer, loss and lifecycle passed\n", argv[2]);
	close(report);
	return 0;
}

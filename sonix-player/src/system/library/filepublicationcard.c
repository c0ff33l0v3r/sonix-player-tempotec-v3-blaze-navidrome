#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "filepublicationcard.h"
#include "filepublication.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
	uint64_t magic, epoch, device, inode, transfer;
	unsigned active;
	char root[512];
} card_state_t;
#define CARD_MAGIC UINT64_C(0x534f4e4958434152)
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t change_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t cached_epoch;
static bool registered, cached_active;
#ifndef FP_CARD_CONTROL
#define FP_CARD_CONTROL "/tmp/sonix-publication-card"
#endif

static int control(const char *suffix, bool create) {
	char path[128];
	if (snprintf(path, sizeof(path), FP_CARD_CONTROL "-%lu.%s", (unsigned long)geteuid(), suffix) >= (int)sizeof(path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	int fd = open(path, (create ? O_RDWR | O_CREAT : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
	struct stat st;
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 || (st.st_mode & 077)) {
		close(fd);
		errno = EACCES;
		return -1;
	}
	return fd;
}
static bool state_read(card_state_t *s) {
	int fd = control("state", false);
	if (fd < 0)
		return false;
	struct stat st;
	bool ok = !fstat(fd, &st) && st.st_size == sizeof(*s) && pread(fd, s, sizeof(*s), 0) == sizeof(*s);
	close(fd);
	return ok && s->magic == CARD_MAGIC && s->epoch && s->active <= 1 && memchr(s->root, 0, sizeof(s->root));
}
static bool state_write(const card_state_t *s) {
	int fd = control("state", true);
	if (fd < 0)
		return false;
	bool ok = pwrite(fd, s, sizeof(*s), 0) == sizeof(*s) && !ftruncate(fd, sizeof(*s));
	if (!ok)
		ftruncate(fd, 0); // missing/invalid state always refuses new CGI I/O
	if (close(fd))
		ok = false;
	return ok;
}
uint64_t fp_card_epoch(void) {
	pthread_mutex_lock(&cache_lock);
	uint64_t epoch = cached_epoch;
	pthread_mutex_unlock(&cache_lock);
	return epoch;
}
bool fp_card_snapshot(uint64_t *epoch, char root[512]) {
	int admission = fp_admission_begin(false);
	if (admission < 0)
		return false;
	card_state_t s;
	bool ok = state_read(&s) && s.active && s.root[0];
	if (ok) {
		*epoch = s.epoch;
		memcpy(root, s.root, sizeof(s.root));
	}
	fp_admission_end(admission);
	return ok;
}
bool fp_card_current(uint64_t epoch) {
	pthread_mutex_lock(&cache_lock);
	bool known = registered;
	bool current = epoch == cached_epoch && cached_active;
	pthread_mutex_unlock(&cache_lock);
	if (known)
		return current;
	uint64_t found;
	char root[512];
	return fp_card_snapshot(&found, root) && found == epoch;
}
int fp_card_begin(uint64_t epoch) {
	int lease = control("lease", true);
	if (lease < 0)
		return -1;
	if (flock(lease, LOCK_SH | LOCK_NB)) {
		close(lease);
		return -1;
	}
	int admission = fp_admission_begin(false);
	card_state_t s;
	struct stat st;
	bool ok = admission >= 0 && state_read(&s) && s.active && s.epoch == epoch && !stat(s.root, &st) && S_ISDIR(st.st_mode) && (uint64_t)st.st_dev == s.device && (uint64_t)st.st_ino == s.inode;
	fp_admission_end(admission);
	if (ok)
		return lease;
	close(lease);
	errno = ESTALE;
	return -1;
}
int fp_card_local_begin(uint64_t epoch) {
	pthread_mutex_lock(&cache_lock);
	bool known = registered;
	pthread_mutex_unlock(&cache_lock);
	return known ? fp_card_begin(epoch) : -2;
}
bool fp_card_local_current(uint64_t epoch) {
	pthread_mutex_lock(&cache_lock);
	bool current = !registered || (cached_active && cached_epoch == epoch);
	pthread_mutex_unlock(&cache_lock);
	return current;
}
void fp_card_end(int lease) {
	if (lease >= 0)
		close(lease);
}

bool fp_transfer_set_enabled(bool enabled) {
	pthread_mutex_lock(&change_lock);
	int admission = fp_admission_begin(true);
	card_state_t s;
	bool ok = admission >= 0 && state_read(&s);
	if (ok) {
		s.transfer = 0;
		if (enabled) {
			int random = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
			ok = random >= 0 && read(random, &s.transfer, sizeof(s.transfer)) == sizeof(s.transfer) && s.transfer;
			if (random >= 0)
				close(random);
		}
		if (ok)
			ok = state_write(&s);
	}
	fp_admission_end(admission);
	pthread_mutex_unlock(&change_lock);
	return ok;
}
bool fp_transfer_snapshot(uint64_t *epoch, uint64_t *session, char root[512]) {
	int admission = fp_admission_begin(false);
	card_state_t s;
	bool ok = admission >= 0 && state_read(&s) && s.active && s.transfer;
	if (ok) {
		*epoch = s.epoch;
		*session = s.transfer;
		memcpy(root, s.root, sizeof(s.root));
	}
	fp_admission_end(admission);
	return ok;
}
int fp_transfer_begin(uint64_t epoch, uint64_t session) {
	int lease = fp_card_begin(epoch);
	if (lease < 0)
		return -1;
	int admission = fp_admission_begin(false);
	card_state_t s;
	bool ok = admission >= 0 && state_read(&s) && s.transfer && s.transfer == session;
	fp_admission_end(admission);
	if (ok)
		return lease;
	fp_card_end(lease);
	errno = ESTALE;
	return -1;
}
bool fp_card_drain(void) {
	int lease = control("lease", true);
	if (lease < 0)
		return false;
	int result;
	do {
		result = flock(lease, LOCK_EX);
	} while (result && errno == EINTR);
	close(lease);
	return !result;
}

bool fp_card_set_root(const char *root) {
	pthread_mutex_lock(&change_lock);
	// Invalidate application workers before waiting for any filesystem lock.
	// No caller may unmount/export when this function returns false.
	pthread_mutex_lock(&cache_lock);
	registered = true;
	cached_active = false;
	pthread_mutex_unlock(&cache_lock);
	int admission;
	do {
		admission = fp_admission_begin(true);
		if (admission < 0 && (errno == EWOULDBLOCK || errno == EAGAIN || errno == EBUSY || errno == EINTR)) {
			struct timespec pause = {0, 5000000};
			nanosleep(&pause, NULL);
		} else
			break;
	} while (true);
	card_state_t s = {.magic = CARD_MAGIC};
	card_state_t old;
	if (admission >= 0 && state_read(&old)) {
		s.epoch = old.epoch + 1;
		s.transfer = old.transfer;
	}
	bool seeded = true;
	if (!s.epoch) {
		int random = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
		seeded = random >= 0 && read(random, &s.epoch, sizeof(s.epoch)) == sizeof(s.epoch) && s.epoch;
		if (random >= 0)
			close(random);
	}
	pthread_mutex_lock(&cache_lock);
	registered = true;
	cached_active = false;
	cached_epoch = s.epoch;
	pthread_mutex_unlock(&cache_lock);
	bool ok = admission >= 0 && seeded && state_write(&s);
	fp_admission_end(admission);
	int lease = ok ? control("lease", true) : -1;
	if (lease >= 0) {
		int result;
		do {
			result = flock(lease, LOCK_EX);
		} while (result && errno == EINTR);
		ok = result == 0;
	} else
		ok = false;
	if (ok && root && root[0]) {
		char *physical = realpath(root, NULL);
		struct stat st;
		ok = physical && strlen(physical) < sizeof(s.root) && !stat(physical, &st) && S_ISDIR(st.st_mode);
		if (ok) {
			snprintf(s.root, sizeof(s.root), "%s", physical);
			s.device = st.st_dev;
			s.inode = st.st_ino;
			s.active = 1;
		}
		free(physical);
	}
	if (ok) {
		admission = fp_admission_begin(true);
		ok = admission >= 0 && state_write(&s);
		fp_admission_end(admission);
	}
	pthread_mutex_lock(&cache_lock);
	cached_active = ok && s.active;
	pthread_mutex_unlock(&cache_lock);
	fp_card_end(lease);
	pthread_mutex_unlock(&change_lock);
	return ok;
}

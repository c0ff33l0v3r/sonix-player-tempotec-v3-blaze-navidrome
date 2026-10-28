#include "streamturn.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool busy;
static atomic_uint demand, demand_epoch;
static _Atomic(void (*)(void)) observer;
void streamturn_set_observer(void (*changed)(void)) { atomic_store(&observer, changed); }
static void observe_change(void) {
	void (*changed)(void) = atomic_load(&observer);
	if (changed)
		changed();
}
static void demand_begin(void) {
	atomic_fetch_add(&demand, 1);
	atomic_fetch_add(&demand_epoch, 1);
	observe_change();
}
static void demand_end(void) {
	atomic_fetch_sub(&demand, 1);
	atomic_fetch_add(&demand_epoch, 1);
	observe_change();
}
unsigned streamturn_observe(bool *idle) {
	*idle = atomic_load(&demand) == 0;
	return atomic_load(&demand_epoch);
}
static unsigned playback_waiters;
static bool background_busy;
static void (*background_cancel)(void);

bool streamturn_set_background_cancel(void (*cancel)(void)) {
	pthread_mutex_lock(&lock);
	bool ok = !background_cancel || background_cancel == cancel;
	if (ok) background_cancel = cancel;
	pthread_mutex_unlock(&lock);
	return ok;
}

void streamturn_acquire(void) {
	demand_begin();
	pthread_mutex_lock(&lock);
	playback_waiters++;
	void (*cancel)(void) = background_busy ? background_cancel : NULL;
	pthread_mutex_unlock(&lock);
	if (cancel) cancel();
	pthread_mutex_lock(&lock);
	while (busy) {
		pthread_cond_wait(&changed, &lock);
	}
	busy = true;
	playback_waiters--;
	pthread_mutex_unlock(&lock);
}

bool streamturn_acquire_current(bool (*current)(void *), void *user) {
	demand_begin();
	pthread_mutex_lock(&lock);
	playback_waiters++;
	void (*cancel)(void) = background_busy ? background_cancel : NULL;
	pthread_mutex_unlock(&lock);
	if (cancel) cancel();
	for (;;) {
		if (!current(user)) {
			pthread_mutex_lock(&lock);
			playback_waiters--;
			pthread_cond_broadcast(&changed);
			pthread_mutex_unlock(&lock);
			demand_end();
			return false;
		}
		pthread_mutex_lock(&lock);
		if (!busy) {
			busy = true;
			playback_waiters--;
			pthread_mutex_unlock(&lock);
			return true;
		}
		struct timespec deadline;
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_nsec += 100000000;
		if (deadline.tv_nsec >= 1000000000) {
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000;
		}
		pthread_cond_timedwait(&changed, &lock, &deadline);
		pthread_mutex_unlock(&lock);
	}
}

bool streamturn_acquire_background(bool (*current)(void *), void *user) {
	demand_begin();
	while (current(user)) {
		pthread_mutex_lock(&lock);
		if (!busy && !playback_waiters) {
			busy = true;
			background_busy = true;
			pthread_mutex_unlock(&lock);
			return true;
		}
		struct timespec deadline;
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_nsec += 100000000;
		if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
		pthread_cond_timedwait(&changed, &lock, &deadline);
		pthread_mutex_unlock(&lock);
	}
	demand_end();
	return false;
}

void streamturn_release(void) {
	pthread_mutex_lock(&lock);
	busy = false;
	background_busy = false;
	// Broadcast, not signal: a waiter may be another download or somebody
	// parked in streamturn_wait_idle(), and waking just one leaves the other
	// stuck until someone else comes through.
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&lock);
	demand_end();
}

// Three registrants -- Qobuz, Tidal, podcasts -- plus margin.
//
// The table has its own lock: registration comes from the main thread at
// startup, from the card supervisor thread (inserting a card redoes set_root)
// and from the UI thread, while the readers are the pages' workers. Without it
// the count can become visible before the function it counts -- one preemption
// between two instructions -- and the call lands on a null pointer.
#define ABANDON_MAX 8
static pthread_mutex_t abandon_lock = PTHREAD_MUTEX_INITIALIZER;
static streamturn_abandon_fn abandon_fns[ABANDON_MAX];
static int abandon_count;

bool streamturn_register_abandon(streamturn_abandon_fn fn) {
	if (!fn) {
		return false;
	}
	pthread_mutex_lock(&abandon_lock);
	bool known = false;
	for (int i = 0; i < abandon_count && !known; i++) {
		known = abandon_fns[i] == fn;
	}
	bool ok = known || abandon_count < ABANDON_MAX;
	if (!known && ok) {
		abandon_fns[abandon_count] = fn;
		abandon_count++;
	}
	pthread_mutex_unlock(&abandon_lock);
	return ok;
}

void streamturn_abandon_all(void) {
	// The table is copied and the calls made outside the lock: an abandon
	// closes a socket under a blocked recv(), and must not do that while
	// holding off anyone trying to register.
	streamturn_abandon_fn copy[ABANDON_MAX];
	int count;

	pthread_mutex_lock(&abandon_lock);
	count = abandon_count;
	for (int i = 0; i < count; i++) {
		copy[i] = abandon_fns[i];
	}
	pthread_mutex_unlock(&abandon_lock);

	for (int i = 0; i < count; i++) {
		copy[i]();
	}
}

void streamturn_wait_idle(void) {
	pthread_mutex_lock(&lock);
	while (busy) {
		pthread_cond_wait(&changed, &lock);
	}
	pthread_mutex_unlock(&lock);
}

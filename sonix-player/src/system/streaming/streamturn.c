#include "streamturn.h"
#include "src/system/core/diagnostic.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool busy;
static atomic_uint demand, demand_epoch, foreground, foreground_epoch;
static atomic_uint headroom_token, headroom_ms_at, artwork_epoch;
static atomic_bool artwork_buffered;
static void observe_change(void);
static unsigned monotonic_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned)((uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
unsigned streamturn_playback_token(void) { return atomic_load(&foreground) == 1 ? atomic_load(&foreground_epoch) : 0; }
void streamturn_artwork_urgent(void) {
	atomic_store(&headroom_ms_at, 0);
	if (atomic_exchange(&artwork_buffered, false)) {
		atomic_fetch_add(&artwork_epoch, 1);
		observe_change();
	}
}
void streamturn_artwork_headroom(unsigned token, int milliseconds) {
	if (!token || token != streamturn_playback_token()) return;
	unsigned now = monotonic_ms(), previous = atomic_load(&headroom_ms_at);
	bool was = atomic_load(&artwork_buffered);
	bool fresh = previous && now - previous <= 500 && token == atomic_load(&headroom_token);
	bool eligible = milliseconds >= 8000 || (fresh && was && milliseconds > 4000);
	atomic_store(&headroom_token, token);
	atomic_store(&artwork_buffered, eligible);
	atomic_store(&headroom_ms_at, now);
	if (eligible != was || (eligible && !fresh)) {
		atomic_fetch_add(&artwork_epoch, 1);
		observe_change();
	}
}
unsigned streamturn_observe_artwork(bool player, bool *eligible) {
	unsigned token = atomic_load(&foreground_epoch), sampled = atomic_load(&headroom_ms_at);
	*eligible = atomic_load(&foreground) == 0 || (player && atomic_load(&foreground) == 1 &&
		atomic_load(&headroom_token) == token && atomic_load(&artwork_buffered) && sampled && monotonic_ms() - sampled <= 500);
	return token + atomic_load(&artwork_epoch);
}
static _Atomic(void (*)(void)) observer, artwork_observer;
void streamturn_set_artwork_observer(void (*changed)(void)) { atomic_store(&artwork_observer, changed); }
unsigned streamturn_observe_playback(bool *idle) {
	*idle = atomic_load(&foreground) == 0;
	return atomic_load(&foreground_epoch);
}
void streamturn_set_observer(void (*changed)(void)) { atomic_store(&observer, changed); }
static void observe_change(void) {
	void (*changed)(void) = atomic_load(&observer);
	if (changed)
		changed();
	changed = atomic_load(&artwork_observer);
	if (changed)
		changed();
}
static void demand_begin(bool background) {
	if (!background) {
		streamturn_artwork_urgent();
		atomic_fetch_add(&foreground, 1);
		atomic_fetch_add(&foreground_epoch, 1);
	}
	atomic_fetch_add(&demand, 1);
	atomic_fetch_add(&demand_epoch, 1);
	observe_change();
}
static void demand_end(bool background) {
	if (!background) {
		atomic_fetch_sub(&foreground, 1);
		atomic_fetch_add(&foreground_epoch, 1);
	}
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
	if (ok)
		background_cancel = cancel;
	pthread_mutex_unlock(&lock);
	return ok;
}

void streamturn_acquire(void) {
	demand_begin(false);
	pthread_mutex_lock(&lock);
	playback_waiters++;
	void (*cancel)(void) = background_busy ? background_cancel : NULL;
	pthread_mutex_unlock(&lock);
	if (cancel)
		cancel();
	pthread_mutex_lock(&lock);
	while (busy) {
		pthread_cond_wait(&changed, &lock);
	}
	busy = true;
	playback_waiters--;
	pthread_mutex_unlock(&lock);
}

bool streamturn_acquire_current(bool (*current)(void *), void *user) {
	DIAGNOSTIC_SCOPE(D_AUDIO);
	demand_begin(false);
	pthread_mutex_lock(&lock);
	playback_waiters++;
	void (*cancel)(void) = background_busy ? background_cancel : NULL;
	pthread_mutex_unlock(&lock);
	if (cancel)
		cancel();
	for (;;) {
		if (!current(user)) {
			pthread_mutex_lock(&lock);
			playback_waiters--;
			pthread_cond_broadcast(&changed);
			pthread_mutex_unlock(&lock);
			demand_end(false);
			DIAGNOSTIC_RETURN_BOOL(false);
		}
		pthread_mutex_lock(&lock);
		if (!busy) {
			busy = true;
			playback_waiters--;
			pthread_mutex_unlock(&lock);
			DIAGNOSTIC_RETURN_BOOL(true);
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
	demand_begin(true);
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
		if (deadline.tv_nsec >= 1000000000) {
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000;
		}
		pthread_cond_timedwait(&changed, &lock, &deadline);
		pthread_mutex_unlock(&lock);
	}
	demand_end(true);
	return false;
}

void streamturn_release(void) {
	pthread_mutex_lock(&lock);
	bool background = background_busy;
	busy = false;
	background_busy = false;
	// Broadcast, not signal: a waiter may be another download or somebody
	// parked in streamturn_wait_idle(), and waking just one leaves the other
	// stuck until someone else comes through.
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&lock);
	demand_end(background);
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

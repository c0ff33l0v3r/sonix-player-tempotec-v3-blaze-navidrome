#include "workactivity.h"
#include <stdatomic.h>

// Three independent 10-bit counts and a suspend-admission bit. One native-word
// CAS keeps policy reads bounded and avoids 64-bit atomics on MIPS32.
#define COUNT_MASK 1023u
#define SUSPENDING (1u << 31)
static atomic_uint activity;

bool work_activity_hold(unsigned mask) {
	if (!mask || (mask & ~7u))
		return false;
	unsigned before = atomic_load(&activity);
	for (;;) {
		if (before & SUSPENDING)
			return false;
		unsigned delta = 0;
		for (unsigned i = 0; i < 3; i++) {
			if (!(mask & (1u << i)))
				continue;
			if (((before >> (i * 10)) & COUNT_MASK) == COUNT_MASK)
				return false;
			delta += 1u << (i * 10);
		}
		if (atomic_compare_exchange_weak(&activity, &before, before + delta))
			return true;
	}
}

void work_activity_release(unsigned mask) {
	unsigned delta = 0;
	for (unsigned i = 0; i < 3; i++)
		if (mask & (1u << i))
			delta += 1u << (i * 10);
	atomic_fetch_sub(&activity, delta);
}

bool work_activity_begin(work_activity_t *owner, unsigned mask) {
	if (!owner || owner->mask || !work_activity_hold(mask))
		return false;
	owner->mask = mask;
	return true;
}

void work_activity_end(work_activity_t *owner) {
	if (owner && owner->mask) {
		work_activity_release(owner->mask);
		owner->mask = 0;
	}
}

bool work_activity_network(void) { return (atomic_load(&activity) & COUNT_MASK) != 0; }
bool work_activity_busy(void) { return (atomic_load(&activity) & ~SUSPENDING) != 0; }
bool work_activity_suspend_begin(void) {
	unsigned idle = 0;
	return atomic_compare_exchange_strong(&activity, &idle, SUSPENDING);
}
void work_activity_suspend_end(void) { atomic_fetch_and(&activity, ~SUSPENDING); }

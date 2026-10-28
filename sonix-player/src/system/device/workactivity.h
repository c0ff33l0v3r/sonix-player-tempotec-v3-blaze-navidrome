#ifndef WORKACTIVITY_H
#define WORKACTIVITY_H

#include <stdbool.h>

enum { WORK_NETWORK = 1, WORK_STORAGE = 2, WORK_JOB = 4 };
// Process-local ownership only. Do not copy a live token; end it after I/O has
// quiesced, not when cancellation is merely requested. Zero initialization is safe.
typedef struct {
	unsigned mask;
} work_activity_t;
bool work_activity_begin(work_activity_t *owner, unsigned mask);
void work_activity_end(work_activity_t *owner);
// For existing balanced I/O lease APIs whose token is maintained by their owner.
bool work_activity_hold(unsigned mask);
void work_activity_release(unsigned mask);
bool work_activity_network(void);
bool work_activity_busy(void);
// Atomic admission fence. No new hold succeeds until end after resume/failure.
bool work_activity_suspend_begin(void);
void work_activity_suspend_end(void);

#endif

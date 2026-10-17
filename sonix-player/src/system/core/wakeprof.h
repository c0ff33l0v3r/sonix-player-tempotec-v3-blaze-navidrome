#ifndef WAKEPROF_H
#define WAKEPROF_H

#include <stdbool.h>

// What woke the interface loop: a census, one line a minute in the log.
//
// The loop sleeps in poll() on the gui_post doorbell until the next timer is
// due, and every return is a wakeup the battery pays for. Each pass is put down
// to what started it:
//
//   t <addr>          an lv_timer whose time came (the real callback, from
//                     timerprof.c's trampoline); every one that ran is counted
//   p <addr> <thread> a gui_post() item, by the function posted and the name of
//                     the thread that posted it
//   idle              poll() timed out and nothing was due: the loop's own cap
//   fd-empty          the doorbell rang and the queue was already empty
//
// A pass woken by the doorbell is put down to the posts it drained only; the
// timers that happened to fall due in it rode along. A pass woken by the
// timeout is put down to every timer that ran in it, so with several timers on
// one beat the counts add up to more than the passes.
//
// The line reads "wakeprof: N passes in 60 s (timeout A, fd B): " and then the
// twelve commonest causes with their counts. Addresses are as the program sees
// them; the load base is printed once at start, for a position-independent
// build (the simulator) where addr2line wants the address less the base.
//
// Only when <card>/.local/wakeprof exists at start, or SONIX_WAKEPROF is set
// on the simulator. Otherwise every hook is one test of wakeprof_on.

extern bool wakeprof_on;

void wakeprof_init(void);

// The main loop: once after each pass of lv_timer_handler(), and once after
// each poll(), with whether the doorbell is what returned it.
void wakeprof_pass_end(void);
void wakeprof_woke(bool by_doorbell);

// From timerprof.c and gui.c's drain, on the interface thread.
void wakeprof_note_timer(void *cb);
void wakeprof_note_post(void *cb, const char *thread);

#endif /* WAKEPROF_H */

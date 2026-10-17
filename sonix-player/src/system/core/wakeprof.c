#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "wakeprof.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TRIGGER "/mnt/sd_0/.local/wakeprof"
#define PERIOD_MS 60000
#define TOP 12

bool wakeprof_on;

enum { K_TIMER, K_POST, K_IDLE, K_FD_EMPTY };

typedef struct {
	int kind;
	void *cb;
	char thread[16];
	unsigned count;
} cause_t;

// What one pass ran, until wakeprof_pass_end() files it. A pass that runs more
// than this many things keeps the first ones; the rest are rare enough.
#define PASS_MAX 24
static cause_t pass[PASS_MAX];
static int pass_n;

// The minute's tally. Full, a new cause is not counted: 128 different timers
// and posters in a minute is far beyond what this player has.
#define CAUSES_MAX 128
static cause_t causes[CAUSES_MAX];
static int causes_n;

static unsigned passes, by_timeout, by_doorbell;
static bool woke_by_doorbell;
static unsigned long long period_start_ms;

static unsigned long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}

void wakeprof_init(void) {
	wakeprof_on = getenv("SONIX_WAKEPROF") || access(TRIGGER, F_OK) == 0;
	if (!wakeprof_on) {
		return;
	}
	Dl_info info;
	void *base = dladdr((void *)&wakeprof_init, &info) ? info.dli_fbase : NULL;
	printf("wakeprof: on, a census of the interface loop's wakeups every %d s (load base %p)\n", PERIOD_MS / 1000,
		   base);
	period_start_ms = now_ms();
}

static void pass_add(int kind, void *cb, const char *thread) {
	if (pass_n >= PASS_MAX) {
		return;
	}
	cause_t *c = &pass[pass_n++];
	c->kind = kind;
	c->cb = cb;
	snprintf(c->thread, sizeof(c->thread), "%s", thread ? thread : "");
}

void wakeprof_note_timer(void *cb) { pass_add(K_TIMER, cb, NULL); }

void wakeprof_note_post(void *cb, const char *thread) { pass_add(K_POST, cb, thread); }

static void tally(int kind, void *cb, const char *thread) {
	for (int i = 0; i < causes_n; i++) {
		cause_t *c = &causes[i];
		if (c->kind == kind && c->cb == cb && strcmp(c->thread, thread) == 0) {
			c->count++;
			return;
		}
	}
	if (causes_n < CAUSES_MAX) {
		cause_t *c = &causes[causes_n++];
		c->kind = kind;
		c->cb = cb;
		snprintf(c->thread, sizeof(c->thread), "%s", thread);
		c->count = 1;
	}
}

static int by_count(const void *a, const void *b) {
	unsigned x = ((const cause_t *)a)->count, y = ((const cause_t *)b)->count;
	return x < y ? 1 : x > y ? -1 : 0;
}

static void report(void) {
	qsort(causes, (size_t)causes_n, sizeof(causes[0]), by_count);
	char line[1024];
	int len = snprintf(line, sizeof(line), "wakeprof: %u passes in %d s (timeout %u, fd %u):", passes,
					   PERIOD_MS / 1000, by_timeout, by_doorbell);
	for (int i = 0; i < causes_n && i < TOP && len > 0 && len < (int)sizeof(line); i++) {
		const cause_t *c = &causes[i];
		switch (c->kind) {
		case K_TIMER:
			len += snprintf(line + len, sizeof(line) - (size_t)len, " t %p=%u", c->cb, c->count);
			break;
		case K_POST:
			len += snprintf(line + len, sizeof(line) - (size_t)len, " p %p %s=%u", c->cb,
							c->thread[0] ? c->thread : "?", c->count);
			break;
		case K_IDLE:
			len += snprintf(line + len, sizeof(line) - (size_t)len, " idle=%u", c->count);
			break;
		default:
			len += snprintf(line + len, sizeof(line) - (size_t)len, " fd-empty=%u", c->count);
			break;
		}
	}
	printf("%s\n", line);
	causes_n = 0;
	passes = by_timeout = by_doorbell = 0;
}

void wakeprof_pass_end(void) {
	if (!wakeprof_on) {
		return;
	}
	passes++;
	bool any = false;
	if (woke_by_doorbell) {
		by_doorbell++;
		for (int i = 0; i < pass_n; i++) {
			if (pass[i].kind == K_POST) {
				tally(K_POST, pass[i].cb, pass[i].thread);
				any = true;
			}
		}
		if (!any) {
			tally(K_FD_EMPTY, NULL, "");
		}
	} else {
		by_timeout++;
		for (int i = 0; i < pass_n; i++) {
			tally(pass[i].kind, pass[i].cb, pass[i].thread);
			any = true;
		}
		if (!any) {
			tally(K_IDLE, NULL, "");
		}
	}
	pass_n = 0;
	unsigned long long now = now_ms();
	if (now - period_start_ms >= PERIOD_MS) {
		report();
		period_start_ms = now;
	}
}

void wakeprof_woke(bool by_doorbell_now) { woke_by_doorbell = by_doorbell_now; }

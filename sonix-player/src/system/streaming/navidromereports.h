#ifndef NAVIDROMEREPORTS_H
#define NAVIDROMEREPORTS_H
#include "navidrome.h"
#include "src/system/audio/audio.h"

// GUI-owned observation only. Never waits for the reporter worker or performs I/O.
void nd_reports_init(void);
void nd_reports_wake(void) __attribute__((weak)); // bounded notification, no worker-held lock wait
void nd_reports_start(const nd_context_t *context, const nd_library_t *library, const char *id, const char *path, double duration);
void nd_reports_defer(void);	// advisory demand; atomic, callable from any thread
void nd_reports_shutdown(void); // process teardown only; never flushes the network

typedef struct {
	char scope[33], id[ND_ID_MAX];
	unsigned profile, card, session;
	int64_t when;
	double boot;
} nd_listen_t;
typedef struct {
	nd_listen_t event;
	unsigned audio_serial, discontinuity;
	double last_tick, position, listened, duration;
	bool active, playing, qualified, begun;
} nd_listen_tracker_t;
// Pure tracker boundary, also exercised with deterministic clocks by focused checks.
void nd_listen_start(nd_listen_tracker_t *tracker, const nd_listen_t *event, const audio_observation_t *sample, double now, double duration);
bool nd_listen_sample(nd_listen_tracker_t *tracker, const audio_observation_t *sample, double now, int64_t wall, nd_listen_t *qualified);
#ifdef NAVIDROME_REPORT_TEST
void nd_reports_test_clock(unsigned milliseconds);
void nd_reports_test_clock_silent(unsigned milliseconds); // advance deadlines without notifying
void nd_reports_test_undated(bool undated);
void nd_reports_test_observe(void);
unsigned nd_reports_test_drops(void);
unsigned nd_reports_test_wakes(void);
unsigned nd_reports_test_opens(void);
#endif
#endif

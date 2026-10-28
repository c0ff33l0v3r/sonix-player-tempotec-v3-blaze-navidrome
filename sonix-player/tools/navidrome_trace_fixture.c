// Optional bounded event recorder for production-path focused fixtures.
// Link with NAVIDROME_CHECK_FLAGS=tools/navidrome_trace_fixture.c. This checks
// emission sites; debugcheck/networkcheck separately exercise the real writer.
#define SONIX_DIAGNOSTIC_IMPLEMENTATION
#include "src/system/core/diagnostic.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static atomic_uint next_id;
static atomic_uint accepted_audio[64], accepted_decoder_failures;
static atomic_uint counts[D_COMPONENTS][D_EVENTS][D_OUTCOMES];
static atomic_uint duration_max[D_COMPONENTS][D_EVENTS];
static __thread diagnostic_t context;
bool logging_diagnostic_enabled(void) { return true; }
uint64_t logging_diagnostic_clock(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
diagnostic_t logging_diagnostic_current(void) { return context; }
diagnostic_t logging_diagnostic_attach(diagnostic_t c) {
	diagnostic_t old = context;
	context = c;
	return old;
}
void logging_diagnostic_event(diagnostic_t c, diagnostic_event_t e, diagnostic_outcome_t o, const diagnostic_value_t *v, size_t n) {
	if ((unsigned)c.component >= D_COMPONENTS || (unsigned)e >= D_EVENTS || (unsigned)o >= D_OUTCOMES)
		abort();
	atomic_fetch_add(&counts[c.component][e][o], 1);
	if (c.component == D_AUDIO && e == D_RESPONSE && o == D_OK && c.id)
		atomic_store(&accepted_audio[c.id % 64], c.id);
	if (c.component == D_AUDIO && e == D_DECODE && o == D_FAILED && c.parent && atomic_load(&accepted_audio[c.parent % 64]) == c.parent)
		atomic_fetch_add(&accepted_decoder_failures, 1);
	for (size_t i = 0; i < n; i++) {
		if ((v[i].key != D_DURATION_US && v[i].key != D_COMMIT_US && v[i].key != D_DELIVERY_US && v[i].key != D_WAIT_US && v[i].key != D_BLANK_US) || v[i].value < 0)
			continue;
		unsigned value = v[i].value > UINT32_MAX ? UINT32_MAX : (unsigned)v[i].value;
		unsigned previous = atomic_load(&duration_max[c.component][e]);
		while (value > previous && !atomic_compare_exchange_weak(&duration_max[c.component][e], &previous, value)) {
		}
	}
}
diagnostic_t logging_diagnostic_begin(diagnostic_component_t c, uint32_t parent) {
	diagnostic_t result = {.id = atomic_fetch_add(&next_id, 1) + 1, .parent = parent, .capture = 1, .component = c, .start_us = logging_diagnostic_clock()};
	logging_diagnostic_event(result, D_BEGIN, D_PENDING, NULL, 0);
	return result;
}
__attribute__((destructor)) static void report(void) {
	const char *path = getenv("SONIX_TRACE_REPORT");
	if (!path)
		return;
	FILE *f = fopen(path, "ae");
	if (!f)
		abort();
	for (unsigned c = 0; c < D_COMPONENTS; c++)
		for (unsigned e = 0; e < D_EVENTS; e++)
			for (unsigned o = 0; o < D_OUTCOMES; o++) {
				unsigned n = atomic_load(&counts[c][e][o]);
				if (n)
					fprintf(f, "{\"component\":%u,\"event\":%u,\"outcome\":%u,\"count\":%u,\"duration_max_us\":%u,\"accepted_parent_decode_failures\":%u}\n", c, e, o, n, atomic_load(&duration_max[c][e]), atomic_load(&accepted_decoder_failures));
			}
	if (fclose(f))
		abort();
}

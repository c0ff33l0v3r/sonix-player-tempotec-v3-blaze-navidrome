#ifndef NAVIDROMEDEBUG_H
#define NAVIDROMEDEBUG_H

#include <stdbool.h>

typedef struct {
	bool enabled, recording, pending;
	char path[1200];
	const char *error; // localization key, never storage/server text
} nd_debug_state_t;

void nd_debug_enable(bool enabled); // nonblocking; preference is owned by settings
void nd_debug_snapshot(nd_debug_state_t *out);
void nd_debug_card(const char *root); // storage-thread barrier: closes retired card handles
void nd_debug_shutdown(void);
void nd_debug_line(const char *line); // fixed, already-redacted diagnostics only
void nd_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif

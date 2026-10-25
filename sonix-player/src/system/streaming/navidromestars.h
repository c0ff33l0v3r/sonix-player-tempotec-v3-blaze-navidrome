#ifndef NAVIDROMESTARS_H
#define NAVIDROMESTARS_H
#include "navidrome.h"

typedef enum { ND_STAR_UNKNOWN, ND_STAR_CLEAR, ND_STAR_SET, ND_STAR_PENDING, ND_STAR_UNCERTAIN } nd_star_state_t;
typedef enum { ND_STAR_ADD, ND_STAR_REMOVE, ND_STAR_TOGGLE, ND_STAR_CHECK, ND_STAR_SELECTED, ND_STAR_RETRY } nd_star_action_t;
typedef struct {
	nd_star_action_t action;
	nd_kind_t kind;
	char id[ND_ID_MAX];
	int64_t session;
	bool bound_profile;
	unsigned profile_epoch;
	nd_playrow_t metadata; // Owned optional represented row for missing-cache retry.
	nd_library_t library;  // Optional captured browser/selection source; not required for a playing track.
} nd_star_request_t;
typedef struct {
	nd_star_request_t request;
	nd_context_t context; // Epochs only; credentials are wiped before delivery.
	int completed, failed, total;
	bool uncertain, cache_failed;
	int saved;
	const char *error;
} nd_star_result_t;

// Nonblocking, GUI-facing state and one-operation service admission.
nd_star_state_t navidrome_star_state(nd_kind_t kind, const char *id);
bool navidrome_star_busy(void);
bool navidrome_star_retry(nd_kind_t kind, const char *id);
bool navidrome_star_loading(nd_kind_t kind, const char *id);
// Include credential/profile lifetime as well as local metadata/card ownership.
bool navidrome_star_current(const nd_context_t *context);
bool navidrome_star_submit(const nd_star_request_t *request, void (*done)(nd_star_result_t *, void *), void *user);
// GUI timer fallback for a rejected gui_post; never repeats server work.
void navidrome_star_poll(void);
// Blocking worker boundary for protected HTTP/service fixtures.
void nd_star_execute(const nd_star_request_t *request, const nd_context_t *context, nd_star_result_t *result);
#endif

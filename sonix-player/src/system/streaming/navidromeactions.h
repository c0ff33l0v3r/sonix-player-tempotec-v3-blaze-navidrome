#ifndef NAVIDROMEACTIONS_H
#define NAVIDROMEACTIONS_H
#include "navidromeowned.h"

typedef enum { ND_ACTION_TOGGLE, ND_ACTION_MARKS, ND_ACTION_CAPTURE } nd_action_op_t;
typedef struct {
	nd_action_op_t op;
	nd_owned_rows_t *albums[2];
	char album_ids[2][ND_ID_MAX];
	nd_owned_rows_t *selection; // Borrowed on submit; service retains it.
	nd_library_t library;
	nd_kind_t kind;
	bool remote, expand;
	nd_playrow_t row;
	int ordinal, count;
	char ids[ND_WINDOW][ND_ID_MAX];
	unsigned token, profile_epoch;
} nd_action_request_t;
typedef struct {
	nd_owned_rows_t *selection, *capture;
	int selected;
	bool marks[ND_WINDOW];
	unsigned token;
	const char *error;
} nd_action_result_t;
// One active action, no unbounded pending queue. All I/O runs on its worker.
bool navidrome_actions_submit(const nd_action_request_t *request, void (*done)(nd_action_result_t *, void *), void *user);
bool navidrome_actions_busy(void);
void navidrome_actions_poll(void); // GUI fallback for a rejected post.
void nd_action_result_free(nd_action_result_t *result);
// Worker-only synchronous boundary, also used by native fixtures.
void nd_action_execute(const nd_action_request_t *request, const nd_context_t *identity, nd_action_result_t *result);
#endif

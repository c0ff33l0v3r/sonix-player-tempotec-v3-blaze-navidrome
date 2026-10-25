#ifndef NAVIDROMEPLAYLISTS_H
#define NAVIDROMEPLAYLISTS_H
#include "navidrome.h"
#include "navidromeplayliststore.h"
typedef enum { NDPL_LIST, NDPL_ENTRIES, NDPL_CREATE, NDPL_RENAME, NDPL_DELETE, NDPL_COPY, NDPL_SELECT, NDPL_SELECT_ONLY, NDPL_CLEAR, NDPL_ADD, NDPL_REMOVE, NDPL_CATALOGUE, NDPL_CATALOGUE_WINDOW, NDPL_IMPORT, NDPL_PULL, NDPL_PULL_ACCEPT, NDPL_UPLOAD, NDPL_PUBLISH, NDPL_VERIFY, NDPL_RECOVER, NDPL_RECREATE } nd_pl_action_t;
typedef struct {
	nd_pl_action_t action;
	int64_t id, revision, session, occurrence, candidate, generation;
	int first, ordinal;
	nd_kind_t kind;
	char name[ND_PL_NAME], remote[ND_ID_MAX], track[ND_ID_MAX], checked[ND_PL_HASH];
	bool confirmed, favorites;
} nd_pl_request_t;
typedef struct {
	nd_pl_request_t request;
	nd_playlist_t playlist, playlists[ND_WINDOW];
	nd_playlist_entry_t entries[ND_WINDOW];
	int count, total, selected, added, skipped;
	bool confirm, conflict;
	char checked[ND_PL_HASH];
	int64_t candidate;
	const char *error;
	nd_context_t context; // callback contains no password; generation checks only
} nd_pl_result_t;
// Nonblocking one-operation boundary. Caller owns the returned result and frees it.
bool navidrome_playlists_submit(const nd_pl_request_t *request, void (*done)(nd_pl_result_t *, void *), void *user);
bool navidrome_playlists_busy(void);
// Blocking worker boundary for focused fixtures; caller supplies coherent captured context.
void nd_pl_execute(const nd_pl_request_t *request, const nd_context_t *context, const nd_library_t *library, nd_pl_result_t *out);
#endif

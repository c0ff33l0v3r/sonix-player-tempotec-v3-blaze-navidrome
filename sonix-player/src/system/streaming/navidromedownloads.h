#ifndef NAVIDROMEDOWNLOADS_H
#define NAVIDROMEDOWNLOADS_H
#include "navidrome.h"
#include "navidromedownloadstore.h"

typedef enum { NDDL_VIEW, NDDL_CAPTURE, NDDL_PAUSE, NDDL_RESUME, NDDL_CANCEL, NDDL_RETRY, NDDL_FOLDERS, NDDL_MKDIR, NDDL_PREPARE_FOLDER, NDDL_CLEAR_HISTORY } nd_dl_action_t;
typedef struct {
	nd_dl_action_t action;
	nd_library_t library;
	nd_kind_t kind;
	nd_dl_filter_t filter;
	bool remote_album; // Explicit exact-ID Discovery album; no Library snapshot pin.
	bool direct; // Current Navidrome track: owned metadata, no source snapshot pin.
	nd_playrow_t track;
	char id[ND_ID_MAX], destination[ND_DL_DEST], folder[ND_DL_PATH], name[96];
	int64_t session;
	int64_t playlist, playlist_revision; // Complete saved copy, independent of playback.
	int offset;
	unsigned token;
	unsigned profile_epoch; // Optional GUI source binding, required by Now Playing.
} nd_dl_request_t;
typedef struct {
	unsigned token, revision;
	nd_dl_filter_t filter;
	bool busy, armed;
	char error[64], scope[33];
	unsigned profile_epoch, card_epoch;
	nd_dl_job_t jobs[ND_WINDOW];
	int count, total, counts[9], offset, skipped, batches;
	int64_t active_key, bytes, length;
	char folders[ND_WINDOW][ND_DL_DEST], folder[ND_DL_PATH];
	int folder_count, folder_total;
} nd_dl_view_t;
typedef void (*nd_dl_captured_cb)(bool ok, const char *error, int added, int skipped, void *user);
// GUI thread admission; all filesystem/SQL work stays on owned workers.
bool navidromedownloads_request(const nd_dl_request_t *request, nd_dl_captured_cb callback, void *user);
void navidromedownloads_init(void); // after service initialization; no threads/I/O
void navidromedownloads_poll(void); // GUI fallback delivery only
void navidromedownloads_view(nd_dl_view_t *out);
void navidromedownloads_cancel(void); // service mutex-safe: wake only
void navidromedownloads_shutdown(void);
bool navidromedownloads_network_wanted(void);
// GUI-thread discovery consumes one coalesced notification after jobs settle.
bool navidromedownloads_take_saved(nd_library_t *library);
// Worker lookup; its own short leases; no GUI SQL or whole-card file matching.
bool nd_download_resolve(const nd_library_t *library, const nd_context_t *context, const char *id, char path[ND_DL_PATH]);
#endif

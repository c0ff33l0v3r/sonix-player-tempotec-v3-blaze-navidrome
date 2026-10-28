#ifndef NAVIDROMEAPI_H
#define NAVIDROMEAPI_H

#include "src/system/core/json.h"
#include "src/system/net/http.h"

#define ND_API_BYTES (256 * 1024)
#define ND_API_TOKENS 16384
typedef struct {
	char *body;
	json_doc_t doc;
	int response;
} nd_document_t;

// Finite protected JSON boundary. method/query are built by internal callers;
// query values must already be URL encoded. Success transfers ownership to out.
const char *nd_api_json(const char *host, const char *user, const char *password, const char *method, const char *query, size_t limit, nd_document_t *out);
const char *nd_api_artwork(const char *host, const char *user, const char *password, const char *id, int pixels, char **body, size_t *size);
// Shared artwork source: finite 2 MiB, cancellable, no redirects.
const char *nd_api_artwork_source(const char *host, const char *user, const char *password, const char *id, int pixels, bool (*current)(void *), void *arg, char **body, size_t *size);
// Playlist-only larger bounded response and optional form POST; legacy callers retain limits.
const char *nd_api_playlist(const char *host, const char *user, const char *password, const char *method, const char *query, const char *form, nd_document_t *out);
const char *nd_api_json_current(const char *host, const char *user, const char *password, const char *method, const char *query, size_t limit, bool (*current)(void *), void *arg, nd_document_t *out);
const char *nd_api_playlist_current(const char *host, const char *user, const char *password, const char *method, const char *query, const char *form, bool (*current)(void *), void *arg, nd_document_t *out);
void nd_api_free(nd_document_t *doc);
const char *nd_api_stream(const char *host, const char *user, const char *password, const char *id, http_session_t *stream);
const char *nd_api_download(const char *host, const char *user, const char *password, const char *id, http_session_t *session);
// Strict opt-in identity/control fields reject duplicate keys and escaped NULs.
bool nd_api_field_unique(const json_doc_t *document, int object, const char *name, bool required);
// Opt-in finite, cancellable protected JSON for Favorites; legacy callers retain behavior.
const char *nd_api_finite_json(const char *host, const char *user, const char *password, const char *method, const char *query, size_t limit, bool (*current)(void *), void *arg, nd_document_t *out);
const char *nd_api_control(const char *host, const char *user, const char *password, bool (*current)(void *), void *arg, nd_document_t *out);
const char *nd_api_report(const char *host, const char *user, const char *password,
		const char *method, const char *query, bool (*current)(void *), void *arg,
		nd_document_t *out, int *status);

const char *nd_api_lyrics(const char *host, const char *user, const char *password, const char *id, bool (*current)(void *), void *arg, nd_document_t *out, int *status);

#endif

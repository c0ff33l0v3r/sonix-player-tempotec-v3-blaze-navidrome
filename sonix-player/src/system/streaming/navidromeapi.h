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

// Blocking, protected JSON boundary. method/query are built by internal callers;
// query values must already be URL encoded. Success transfers ownership to out.
const char *nd_api_json(const char *host, const char *user, const char *password, const char *method, const char *query, size_t limit, nd_document_t *out);
const char *nd_api_artwork(const char *host, const char *user, const char *password, const char *id, int pixels, char **body, size_t *size);
void nd_api_free(nd_document_t *doc);
const char *nd_api_stream(const char *host, const char *user, const char *password, const char *id, http_stream_t *stream);

#endif

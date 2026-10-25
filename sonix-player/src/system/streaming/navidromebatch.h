#ifndef NAVIDROMEBATCH_H
#define NAVIDROMEBATCH_H
#include "navidromestore.h"
#include "src/system/net/http.h"
#define ND_BATCH_ARENA (1024u * 1024u)
#define ND_BATCH_ITEMS 1000
#define ND_BATCH_SLOT_BYTES (2u * 1024u * 1024u)
typedef struct {
	uint32_t next, id, title, artist, album, cover, relations;
	int duration, disc, track;
} nd_batch_row_t;
typedef struct {
	uint32_t next, id;
	int kind;
} nd_batch_relation_t;
typedef struct nd_batch {
	unsigned char *arena;
	size_t used;
	uint32_t first, last;
	int count;
	nd_kind_t kind;
	const char *error;
	double parse_seconds;
	size_t buffer_peak;
} nd_batch_t;
bool nd_batch_init(nd_batch_t *batch, nd_kind_t kind);
void nd_batch_free(nd_batch_t *batch);
bool nd_batch_add(nd_batch_t *batch, const json_doc_t *doc, int obj);
const char *nd_batch_read(nd_batch_t *batch, http_session_t *session, int requested);
const char *nd_api_page(http_session_t *session, const char *host, const char *user, const char *password, nd_kind_t kind, int offset, int requested, bool gzip, bool reuse, nd_batch_t *batch);
#endif

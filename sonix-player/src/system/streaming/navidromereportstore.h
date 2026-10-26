#ifndef NAVIDROMEREPORTSTORE_H
#define NAVIDROMEREPORTSTORE_H
#include "navidromereports.h"

#define ND_LISTEN_ROWS 1024
#define ND_LISTEN_AGE (30LL * 86400 * 1000)
typedef struct {
	sqlite3 *db;
} nd_report_store_t;
// Worker only, within a short card lease. Caller always closes, even on failure.
bool nd_report_store_open(nd_report_store_t *store, const char *path, const char *scope, bool create);
void nd_report_store_close(nd_report_store_t *store);
bool nd_report_store_put(nd_report_store_t *store, const nd_listen_t *event, const char *key, int64_t now);
int64_t nd_report_store_take(nd_report_store_t *store, int64_t now, nd_listen_t *event);
bool nd_report_store_ready(nd_report_store_t *store, int64_t now);
#endif

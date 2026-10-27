#ifndef NAVIDROMEDISCOVERY_H
#define NAVIDROMEDISCOVERY_H
#include "navidromealbumreader.h"
#define ND_DISCOVERY_RANDOM_MAX 50
typedef enum { ND_DISCOVERY_NEWEST, ND_DISCOVERY_RECENT, ND_DISCOVERY_FREQUENT, ND_DISCOVERY_RANDOM, ND_DISCOVERY_MODES } nd_discovery_mode_t;
typedef struct nd_discovery nd_discovery_t;
// Worker-owned session. Results contain extracted display metadata only.
nd_discovery_t *nd_discovery_create(nd_discovery_mode_t mode);
void nd_discovery_free(nd_discovery_t *session);
void nd_discovery_cancel(nd_discovery_t *session, bool (*current)(void *), void *user);
bool nd_discovery_valid(nd_discovery_t *session);
void nd_discovery_read(nd_discovery_t *session, int first, nd_search_result_t *out);
void nd_discovery_album_open(nd_discovery_t *session, const char *id, nd_search_result_t *out);
void nd_discovery_album_read(nd_discovery_t *session, int offset, int pitch, int disc_pitch, bool alternate, nd_search_result_t *out);
bool nd_discovery_album_position(nd_discovery_t *session, const char *id, int pitch, int disc_pitch, bool alternate, int *position);
nd_album_membership_t *nd_discovery_album_membership(nd_discovery_t *session);
#endif

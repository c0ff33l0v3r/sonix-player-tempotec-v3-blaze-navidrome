#ifndef NAVIDROMEALBUMREADER_H
#define NAVIDROMEALBUMREADER_H
#include "navidromesearch.h"
#include "navidromemembership.h"
// Worker-only exact remote album reader. No Search query/local fallback policy.
typedef struct nd_search nd_album_reader_t;
nd_album_reader_t *nd_album_reader_create(void);
void nd_album_reader_cancel(nd_album_reader_t *reader, bool (*current)(void *), void *user);
void nd_album_reader_free(nd_album_reader_t *reader);
void nd_album_reader_open(nd_album_reader_t *reader, const char *id, nd_search_result_t *out);
void nd_album_reader_read(nd_album_reader_t *reader, int offset, int pitch, int disc_pitch, bool alternate, nd_search_result_t *out);
bool nd_album_reader_position(nd_album_reader_t *reader, const char *id, int pitch, int disc_pitch, bool alternate, int *position);
// Worker-only: an owned reference to already-loaded complete membership.
nd_album_membership_t *nd_album_reader_membership(nd_album_reader_t *reader);
#endif

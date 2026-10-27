#ifndef NAVIDROMEMEMBERSHIP_H
#define NAVIDROMEMEMBERSHIP_H
#include "navidrome.h"
#include <sys/types.h>

// Immutable complete remote album. Creation/reads/collection are worker-only.
// On success this owns memory and the exact staged path; the child may borrow
// them while holding its reference. No network or mutable reader is retained.
typedef struct nd_album_membership nd_album_membership_t;
nd_album_membership_t *nd_album_membership_create(const nd_library_t *library, const char *album, nd_row_t *memory, int count, const char *path, dev_t device, ino_t inode);
nd_album_membership_t *nd_album_membership_retain(nd_album_membership_t *members);
// GUI-safe: only retires ownership; storage cleanup runs in collect().
void nd_album_membership_release(nd_album_membership_t *members);
void nd_album_membership_collect(void);
void nd_album_membership_cleanup_waker(void (*wake)(void));
bool nd_album_membership_matches(const nd_album_membership_t *members, const nd_library_t *library, const char *album);
bool nd_album_membership_read(const nd_album_membership_t *members, int offset, nd_row_t rows[ND_WINDOW], int *count, int *total);
#endif

#ifndef NAVIDROMELIBRARY_H
#define NAVIDROMELIBRARY_H

#include "navidromestore.h"
#include "navidromevault.h"

#define ND_CACHE_RELATIVE ".local/navidrome/.cache"

// Blocking helpers, called by the serialized service worker only.
void nd_cache_load(const char *root, const char *vault, const char *host, const char *user, bool migrate_legacy, nd_library_t *out);
bool nd_cache_retire_scope(const char *vault);
bool nd_cache_retire(const char *root, const char *vault);
bool nd_cache_open_sync(const nd_library_t *library, nd_store_t *store);
bool nd_cache_card_same(const char *root, uint64_t device, uint64_t inode);

#endif

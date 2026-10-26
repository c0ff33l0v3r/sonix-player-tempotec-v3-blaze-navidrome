#ifndef NAVIDROMEART_H
#define NAVIDROMEART_H

#include "src/gui/nowplaying/cover.h"
#include "src/system/streaming/navidromestore.h"
#define ND_ART_PLAYER_SLOT ND_WINDOW
#define ND_ART_SEARCH_SLOT (ND_WINDOW + 1)
#define ND_ART_ALBUM_SLOT (ND_ART_SEARCH_SLOT + ND_WINDOW)
#define ND_ART_SEARCH_ALBUM_SLOT (ND_ART_ALBUM_SLOT + 1)
#define ND_ART_QUEUE_SLOT (ND_ART_SEARCH_ALBUM_SLOT + 1)
#define ND_ART_DOWNLOAD_SLOT (ND_ART_QUEUE_SLOT + 12)
#define ND_ART_SLOT_COUNT (ND_ART_DOWNLOAD_SLOT + ND_WINDOW)
// Reference pixels, matching Music medialist thumbnails; scale with ui_px().
#define ND_ART_THUMB_SIZE 72

// Browser, player, Search, album heads, Queue and Downloads own disjoint slots.
void navidromeart_request(int slot, const nd_library_t *library, const char *id, int size);
void navidromeart_release(int slot);
bool navidromeart_take(int slot, cover_image_t *out, bool *finished);
void navidromeart_request_player(const nd_library_t *library, const char *id, int size, int backdrop_w, int backdrop_h);
bool navidromeart_take_player(cover_image_t *out, cover_image_t *backdrop, bool *finished);

#endif

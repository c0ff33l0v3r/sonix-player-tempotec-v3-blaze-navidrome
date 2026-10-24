#ifndef NAVIDROMEART_H
#define NAVIDROMEART_H

#include "src/gui/nowplaying/cover.h"
#include "src/system/streaming/navidromestore.h"

void navidromeart_request(int slot, const nd_library_t *library, const char *id, int size);
void navidromeart_release(int slot);
bool navidromeart_take(int slot, cover_image_t *out, bool *finished);

#endif

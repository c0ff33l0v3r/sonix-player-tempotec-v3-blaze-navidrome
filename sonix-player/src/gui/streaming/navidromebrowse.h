#ifndef NAVIDROMEBROWSE_H
#define NAVIDROMEBROWSE_H

#include "src/gui/shell/gui.h"
#include "src/system/streaming/navidromestore.h"

void navidromebrowse_init(gui_config_t *cfg);
void navidromebrowse_open(nd_kind_t kind);
void navidromebrowse_open_favorites(nd_kind_t kind);
// Playlist Add tracks entry: taps select tracks; album taps still drill down.
void navidromebrowse_pick(nd_kind_t kind);
void navidromebrowse_refresh(void);
bool navidromebrowse_album(const char *id, const char *title);

#endif

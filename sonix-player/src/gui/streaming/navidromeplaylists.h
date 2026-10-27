#ifndef NAVIDROMEPLAYLISTPAGE_H
#define NAVIDROMEPLAYLISTPAGE_H
#include "src/gui/shell/gui.h"
#include <stdint.h>
void navidromeplaylists_init(gui_config_t *cfg);
void navidromeplaylists_open(void);
void navidromeplaylists_refresh(void);
bool navidromeplaylists_bind_current(const char *path);
bool navidromeplaylists_add_current(void);
void navidromeplaylists_pick_source(int64_t session, int64_t generation, lv_obj_t *origin, bool favorites);
void navidromeplaylists_pick(int64_t session, int64_t generation, lv_obj_t *origin);
#endif

#ifndef NAVIDROMEPLAYLISTPAGE_H
#define NAVIDROMEPLAYLISTPAGE_H
#include "src/gui/shell/gui.h"
#include <stdint.h>
void navidromeplaylists_init(gui_config_t *cfg);
void navidromeplaylists_open(void);
void navidromeplaylists_refresh(void);
void navidromeplaylists_pick(int64_t session, int64_t generation, lv_obj_t *origin);
#endif

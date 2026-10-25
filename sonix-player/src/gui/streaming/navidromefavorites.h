#ifndef NAVIDROMEFAVORITES_H
#define NAVIDROMEFAVORITES_H

#include "src/gui/shell/gui.h"

void navidromefavorites_init(gui_config_t *cfg);
void navidromefavorites_open(void);
lv_obj_t *navidromefavorites_page(void);
void navidromefavorites_update(void);

#endif

#ifndef NAVIDROMESETTINGS_H
#define NAVIDROMESETTINGS_H

#include "src/gui/shell/gui.h"
void navidromesettings_init(gui_config_t *cfg);
void navidromesettings_open(void);
void navidromesettings_refresh(void); // GUI thread, including worker notifications

#endif

#ifndef FOLDERPICKER_H
#define FOLDERPICKER_H
#include "lvgl/lvgl.h"
#include "src/gui/shell/gui.h"

// Shared File Explorer destination UI. Callers own navigation, storage work
// and result lifetime; constructing or updating this view never touches disk.
typedef struct {
	lv_obj_t *root, *title, *list, *use, *use_label, *new_folder;
} folderpicker_t;
folderpicker_t folderpicker_create(lv_obj_t *parent, gui_config_t *cfg, const char *title, const char *use, lv_event_cb_t confirm, lv_event_cb_t create, lv_event_cb_t cancel);
lv_obj_t *folderpicker_row(lv_obj_t *list, const char *name, lv_event_cb_t click, void *user);
lv_obj_t *folderpicker_name_label(lv_obj_t *row);
#endif

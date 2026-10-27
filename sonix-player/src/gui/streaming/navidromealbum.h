#ifndef NAVIDROMEALBUM_H
#define NAVIDROMEALBUM_H

#include "navidromeart.h"
#include "src/gui/shell/gui.h"

// Shared reference-height for disc headings, including space below the label.
#define ND_ALBUM_DISC_HEIGHT 64

typedef enum { ND_ALBUM_SHUFFLE, ND_ALBUM_PLAY, ND_ALBUM_QUEUE, ND_ALBUM_DOWNLOAD } nd_album_action_t;
typedef void (*nd_album_action_cb)(void *user, nd_album_action_t action);
typedef struct {
	lv_obj_t *screen, *list, *page_title, *status, *reason;
	lv_obj_t *head, *cover, *name, *artist, *meta, *buttons, *mask;
	lv_obj_t *controls[4];
	gui_config_t *cfg;
	nd_playrow_t info;
	nd_library_t library;
	cover_image_t image;
	nd_album_action_cb action;
	void *user;
	int slot, height, title_y;
	unsigned revision, art_network_epoch;
	bool active, art_bound, art_done, tinted;
	lv_color_t bg, ink, dim, card;
} nd_album_view_t;

bool navidromealbum_enabled(void);
void navidromealbum_set_enabled(bool enabled);
unsigned navidromealbum_revision(void);
void navidromealbum_init(nd_album_view_t *view, gui_config_t *cfg, lv_obj_t *screen, lv_obj_t *list, lv_obj_t *title, lv_obj_t *status, lv_obj_t *reason, int slot, nd_album_action_cb action, void *user);
// Bind on the GUI thread after accepting a coherent owned worker result.
void navidromealbum_bind(nd_album_view_t *view, bool active, const nd_playrow_t *info, const nd_library_t *library, bool controls, bool available);
void navidromealbum_release(nd_album_view_t *view);
void navidromealbum_tick(nd_album_view_t *view);
void navidromealbum_icon(nd_album_view_t *view, lv_obj_t *image);
void navidromealbum_row(nd_album_view_t *view, lv_obj_t *row, lv_obj_t *name, lv_obj_t *detail, lv_obj_t *number, lv_obj_t *image, const nd_row_t *data, bool heading);
#endif

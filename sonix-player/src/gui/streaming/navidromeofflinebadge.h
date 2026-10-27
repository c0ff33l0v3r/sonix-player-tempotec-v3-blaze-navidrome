#ifndef NAVIDROMEOFFLINEBADGE_H
#define NAVIDROMEOFFLINEBADGE_H
#include "lvgl/lvgl.h"
#include "navidromealbum.h"
#include "src/system/streaming/navidromeoffline.h"

typedef struct {
	lv_obj_t *screen, *icons[ND_WINDOW], *details[ND_WINDOW];
	int widths[ND_WINDOW];
	nd_off_state_t state[ND_WINDOW];
	nd_off_request_t request;
	unsigned token, revision;
	uint32_t settled;
	bool busy, bound;
	nd_album_view_t *album;
} nd_off_badges_t;
void navidromeofflinebadge_bind(nd_off_badges_t *view, lv_obj_t *screen, const nd_off_request_t *request, lv_obj_t *const details[ND_WINDOW]);
void navidromeofflinebadge_bind_album(nd_off_badges_t *view, lv_obj_t *screen, const nd_off_request_t *request, lv_obj_t *const details[ND_WINDOW], nd_album_view_t *album);
void navidromeofflinebadge_tick(nd_off_badges_t *view);
void navidromeofflinebadge_release(nd_off_badges_t *view);
#endif

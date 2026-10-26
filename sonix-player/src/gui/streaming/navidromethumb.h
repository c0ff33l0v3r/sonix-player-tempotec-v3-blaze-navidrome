#ifndef NAVIDROMETHUMB_H
#define NAVIDROMETHUMB_H
#include "navidromeart.h"

// A bounded row thumbnail. Only real covers fill the artwork frame; the
// placeholder remains a small centered glyph. Owns its decoded pixels/slot.
typedef struct {
	lv_obj_t *frame, *image;
	cover_image_t cover;
	int slot, size;
	bool bound, finished;
	unsigned profile, card;
	char scope[33], id[ND_ID_MAX];
} nd_thumb_t;
void navidromethumb_init(nd_thumb_t *thumb, lv_obj_t *row, int slot);
void navidromethumb_bind(nd_thumb_t *thumb, const nd_library_t *library, unsigned profile, const char *id);
void navidromethumb_tick(nd_thumb_t *thumb);
void navidromethumb_release(nd_thumb_t *thumb);
#endif

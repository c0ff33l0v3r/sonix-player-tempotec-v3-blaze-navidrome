#include "navidromethumb.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/uiscale.h"
#include <stdio.h>
#include <string.h>

static void placeholder(nd_thumb_t *thumb) {
	lv_image_set_src(thumb->image, &icon_music_note);
	lv_obj_set_size(thumb->image, ui_px(32), ui_px(32));
	lv_image_set_inner_align(thumb->image, LV_IMAGE_ALIGN_CONTAIN);
	lv_obj_add_style(thumb->image, &theme_style_icon, 0);
	lv_obj_center(thumb->image);
}
void navidromethumb_init(nd_thumb_t *thumb, lv_obj_t *row, int slot) {
	thumb->slot = slot;
	thumb->size = ui_px(ND_ART_THUMB_SIZE);
	thumb->finished = true;
	thumb->frame = lv_obj_create(row);
	lv_obj_remove_style_all(thumb->frame);
	lv_obj_set_size(thumb->frame, thumb->size, thumb->size);
	lv_obj_set_scrollable(thumb->frame, false);
	lv_obj_set_clickable(thumb->frame, false);
	lv_obj_align(thumb->frame, LV_ALIGN_LEFT_MID, 0, 0);
	thumb->image = lv_image_create(thumb->frame);
	placeholder(thumb);
}
void navidromethumb_release(nd_thumb_t *thumb) {
	if (thumb->bound)
		navidromeart_release(thumb->slot);
	// Detach the descriptor before freeing pixels still referenced by LVGL.
	placeholder(thumb);
	cover_free(&thumb->cover);
	thumb->bound = false;
	thumb->finished = true;
	thumb->id[0] = 0;
}
void navidromethumb_bind(nd_thumb_t *thumb, const nd_library_t *library, unsigned profile, const char *id) {
	if (!id)
		id = "";
	if (thumb->bound && thumb->profile == profile && thumb->card == library->card_epoch && !strcmp(thumb->scope, library->scope) && !strcmp(thumb->id, id))
		return;
	navidromethumb_release(thumb);
	thumb->profile = profile;
	thumb->card = library->card_epoch;
	snprintf(thumb->scope, sizeof(thumb->scope), "%s", library->scope);
	snprintf(thumb->id, sizeof(thumb->id), "%s", id);
	thumb->bound = true;
	if (*id) {
		thumb->finished = false;
		navidromeart_request(thumb->slot, library, id, thumb->size);
	}
}
void navidromethumb_tick(nd_thumb_t *thumb) {
	if (thumb->finished || !thumb->bound)
		return;
	if (navidromeart_take(thumb->slot, &thumb->cover, &thumb->finished)) {
		lv_obj_remove_style(thumb->image, &theme_style_icon, 0);
		lv_obj_set_size(thumb->image, thumb->size, thumb->size);
		lv_image_set_inner_align(thumb->image, LV_IMAGE_ALIGN_CONTAIN);
		lv_image_set_src(thumb->image, &thumb->cover.dsc);
		lv_obj_center(thumb->image);
		thumb->finished = true;
	}
}

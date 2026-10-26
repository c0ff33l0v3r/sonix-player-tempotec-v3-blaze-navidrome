#include "navidromeofflinebadge.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/uiscale.h"
#include "src/system/streaming/navidromedownloadstore.h"
#include <string.h>

#define OFF_REFRESH_MS 5000

static void badge_paint(nd_off_badges_t *v, int i) {
	if (!v->icons[i] || !v->details[i])
		return;
	bool shown = i < v->request.count && !lv_obj_is_hidden(v->details[i]) && (v->state[i] == ND_OFF_COMPLETE || v->state[i] == ND_OFF_PARTIAL);
	int size = ui_px(20), gap = ui_px(6);
	lv_obj_set_hidden(v->icons[i], !shown);
	lv_obj_set_width(v->details[i], LV_MAX(1, v->widths[i] - (shown ? size + gap : 0)));
	if (!shown)
		return;
	lv_obj_set_pos(v->icons[i], lv_obj_get_x(v->details[i]) + v->widths[i] - size, lv_obj_get_y(v->details[i]) + (lv_obj_get_height(v->details[i]) - size) / 2);
	lv_obj_t *bg = lv_obj_get_parent(v->details[i]);
	while (lv_obj_get_parent(bg) && lv_obj_get_style_bg_opa(bg, 0) < LV_OPA_COVER)
		bg = lv_obj_get_parent(bg);
	lv_color_t color = lv_obj_get_style_bg_color(bg, 0);
	bool light = (color.red * 299 + color.green * 587 + color.blue * 114) / 1000 > 150;
	lv_obj_set_style_image_recolor(v->icons[i], lv_color_hex(v->state[i] == ND_OFF_COMPLETE ? (light ? 0x147a32 : 0x58cf78) : (light ? 0x666666 : 0xaaaaaa)), 0);
}
static void badge_done(unsigned token, int row, nd_off_state_t state, void *user) {
	nd_off_badges_t *v = user;
	if (!v->bound || token != v->token || row < 0 || row >= v->request.count || lv_screen_active() != v->screen || v->revision != nd_dl_availability_revision())
		return;
	if (v->request.kind == ND_OFF_ALBUM) {
		nd_library_t current;
		if (v->request.library.favorites)
			navidrome_favorites_snapshot(&current);
		else
			navidrome_library_snapshot(&current);
		if (current.generation != v->request.library.generation || current.epoch != v->request.library.epoch)
			return;
	}
	v->state[row] = state;
	badge_paint(v, row);
}
void navidromeofflinebadge_release(nd_off_badges_t *v) {
	nd_offline_cancel(v->token);
	v->bound = v->busy = false;
	v->token = 0;
	for (int i = 0; i < ND_WINDOW; i++) {
		v->state[i] = ND_OFF_UNKNOWN;
		if (v->icons[i])
			lv_obj_set_hidden(v->icons[i], true);
		if (v->details[i] && v->widths[i])
			lv_obj_set_width(v->details[i], v->widths[i]);
	}
}
void navidromeofflinebadge_bind(nd_off_badges_t *v, lv_obj_t *screen, const nd_off_request_t *q, lv_obj_t *const details[ND_WINDOW]) {
	nd_offline_cancel(v->token);
	memset(v->state, 0, sizeof(v->state));
	v->screen = screen;
	v->request = *q;
	v->revision = nd_dl_availability_revision();
	v->bound = true;
	lv_obj_update_layout(screen);
	for (int i = 0; i < ND_WINDOW; i++) {
		v->details[i] = details[i];
		v->widths[i] = lv_obj_get_width(details[i]);
		if (!v->icons[i]) {
			v->icons[i] = lv_image_create(lv_obj_get_parent(details[i]));
			lv_image_set_src(v->icons[i], &icon_download);
			lv_obj_set_size(v->icons[i], ui_px(20), ui_px(20));
			lv_image_set_inner_align(v->icons[i], LV_IMAGE_ALIGN_CONTAIN);
			lv_obj_set_floating(v->icons[i], true);
			lv_obj_set_clickable(v->icons[i], false);
			lv_obj_set_scrollable(v->icons[i], false);
			lv_obj_set_style_image_recolor_opa(v->icons[i], LV_OPA_COVER, 0);
		}
		badge_paint(v, i);
	}
	v->token = nd_offline_submit(q, badge_done, v);
	v->busy = v->token != 0;
	v->settled = lv_tick_get();
}
void navidromeofflinebadge_tick(nd_off_badges_t *v) {
	if (!v->bound)
		return;
	if (lv_screen_active() != v->screen) {
		navidromeofflinebadge_release(v);
		return;
	}
	nd_context_t context;
	bool valid = navidrome_context(&context, false) && navidrome_playback_scope(&context, &v->request.library);
	nd_wipe(&context, sizeof(context));
	if (!valid) {
		navidromeofflinebadge_release(v);
		return;
	}
	nd_offline_poll();
	bool busy = nd_offline_busy(v->token);
	if (v->busy && !busy)
		v->settled = lv_tick_get();
	v->busy = busy;
	unsigned revision = nd_dl_availability_revision();
	if (revision != v->revision || (!busy && lv_tick_elaps(v->settled) >= OFF_REFRESH_MS)) {
		v->revision = revision;
		v->token = nd_offline_submit(&v->request, badge_done, v);
		v->busy = v->token != 0;
		v->settled = lv_tick_get();
		if (!v->token)
			for (int i = 0; i < v->request.count; i++)
				v->state[i] = ND_OFF_UNKNOWN;
	}
	for (int i = 0; i < v->request.count; i++)
		badge_paint(v, i);
}

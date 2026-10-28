#ifndef NAVIDROME_GUI_DIAGNOSTIC_H
#define NAVIDROME_GUI_DIAGNOSTIC_H
#include "lvgl/lvgl.h"
#include "src/system/core/diagnostic.h"
// GUI-owned, sampled only at existing scroll/bind boundaries. No timer, layout
// mutation, per-row log, or storage work. Count clipped rows, not pooled objects.
typedef struct {
	diagnostic_t context;
	uint64_t blank_since;
	int visible;
	bool observed;
} nd_view_diagnostic_t;
static inline void nd_view_retire(nd_view_diagnostic_t *v) {
	if (v->context.id)
		DIAG(v->context, D_END, D_SUPERSEDED, DV(D_BLANK_US, v->blank_since ? diagnostic_since(v->blank_since) : 0));
	*v = (nd_view_diagnostic_t){0};
}
static inline void nd_view_observe(nd_view_diagnostic_t *v, lv_obj_t *list, lv_obj_t *const *rows, int count, int total, bool pending) {
	if (!diagnostic_enabled() || !list || !lv_obj_is_visible(list))
		return;
	lv_area_t viewport;
	lv_obj_get_coords(list, &viewport);
	int visible = 0;
	for (int i = 0; i < count; i++) {
		if (!rows[i] || lv_obj_is_hidden(rows[i]))
			continue;
		lv_area_t row;
		lv_obj_get_coords(rows[i], &row);
		if (row.y2 >= viewport.y1 && row.y1 <= viewport.y2 && row.x2 >= viewport.x1 && row.x1 <= viewport.x2)
			visible++;
	}
	bool blank = !visible && (total > 0 || pending);
	if (!v->context.id)
		v->context = diagnostic_begin(D_BROWSE, diagnostic_current().id);
	if (blank && !v->blank_since)
		v->blank_since = diagnostic_clock();
	if (!v->observed || (!!visible != !!v->visible) || (!blank && v->blank_since)) {
		DIAG(v->context, D_VISIBLE, blank ? D_PENDING : D_OK, DV(D_VISIBLE_ROWS, visible), DV(D_TOTAL, total), DV(D_REASON, !visible && !blank ? 1 : 0), DV(D_BLANK_US, v->blank_since ? diagnostic_since(v->blank_since) : 0));
	}
	if (!blank)
		v->blank_since = 0;
	v->visible = visible;
	v->observed = true;
}
#endif

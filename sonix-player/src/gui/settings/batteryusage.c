#include "batteryusage.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/uiscale.h"
#include "src/system/core/lang.h"
#include "src/system/device/batterystats.h"

lv_obj_t *batteryusage_screen;

#define CHART_H ui_px(96)
#define SEGS 3 // the activity bars: screen on, standby, sleep, from the bottom up

// The list's names, in bs_item_t order. Brand names stand as they are; the
// rest reuse the keys of their own pages where there is one, so the words
// match -- sleep is what the Power page calls its standby.
static const char *const ITEM_LABELS[BS_ITEM_COUNT] = {
	"battery_usage_local",	"btreceiver_title",		 "dac_mode",		  "radio", "Qobuz", "Tidal", "podcasts",
	"AirPlay",				"DLNA",					 "battery_usage_screen", "battery_usage_standby",
	"power_standby_mem",	"battery_usage_bt_out",
};
static const bs_item_t SEG_ITEMS[SEGS] = {BS_SCREEN, BS_STANDBY, BS_SLEEP};

static lv_obj_t *summary;
static lv_obj_t *level_bars[BS_HOURS];
static lv_obj_t *activity_bars[BS_HOURS * SEGS];
static lv_obj_t *level_cols[BS_HOURS], *activity_cols[BS_HOURS];
static lv_obj_t *level_marks[BS_HOURS], *activity_marks[BS_HOURS];
static lv_obj_t *legend_dots[SEGS];
static lv_obj_t *list; // emptied and refilled on every open

// A bare box with no style of its own, for the rows and columns the charts are
// made of. Neither scrollable nor clickable, so a drag on a chart scrolls the
// page and the back gesture can start on it.
static lv_obj_t *box(lv_obj_t *parent, lv_flex_flow_t flow) {
	lv_obj_t *o = lv_obj_create(parent);
	lv_obj_remove_style_all(o);
	lv_obj_set_flex_flow(o, flow);
	lv_obj_set_scrollable(o, false);
	lv_obj_set_clickable(o, false);
	return o;
}

static lv_obj_t *card_create(lv_obj_t *parent, const char *title) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, ui_px(12), 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, ui_px(14), 0);
	lv_obj_set_style_pad_row(card, ui_px(8), 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_clickable(card, false);
	lv_obj_t *label = lv_label_create(card);
	lv_label_set_text(label, tr(title));
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	return card;
}

// One column per hour, oldest on the left, each a stack of `segs` bars packed
// against the bottom, and a row of hour marks under it. Flex packs from the
// top, so the stack is built top first: bar 0 is the last child, at the
// bottom. The marks are wider than a column and LVGL clips a child to its
// parent, so they sit loose in their row and refresh() puts each under its
// column.
static void chart_create(lv_obj_t *card, int segs, lv_obj_t **bars, lv_obj_t **cols_out, lv_obj_t **marks) {
	lv_obj_t *cols = box(card, LV_FLEX_FLOW_ROW);
	lv_obj_set_size(cols, lv_pct(100), CHART_H);
	lv_obj_set_style_pad_column(cols, ui_px(3), 0);
	lv_obj_t *row = box(card, LV_FLEX_FLOW_ROW);
	lv_obj_set_layout(row, LV_LAYOUT_NONE);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	for (int i = 0; i < BS_HOURS; i++) {
		lv_obj_t *col = box(cols, LV_FLEX_FLOW_COLUMN);
		cols_out[i] = col;
		lv_obj_set_height(col, lv_pct(100));
		lv_obj_set_flex_grow(col, 1);
		lv_obj_set_flex_align(col, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
		for (int s = segs - 1; s >= 0; s--) {
			lv_obj_t *bar = box(col, LV_FLEX_FLOW_ROW);
			lv_obj_set_size(bar, lv_pct(100), 0);
			lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
			lv_obj_set_style_radius(bar, ui_px(2), 0);
			bars[i * segs + s] = bar;
		}
		marks[i] = lv_label_create(row);
		lv_obj_add_style(marks[i], &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(marks[i], &font_ui_18, 0);
	}
}

static void paint(lv_obj_t *bar, int64_t value, int64_t full, lv_color_t color, lv_opa_t opa) {
	lv_obj_set_height(bar, (int32_t)(value * CHART_H / full));
	lv_obj_set_style_bg_color(bar, color, 0);
	lv_obj_set_style_bg_opa(bar, opa, 0);
}

static lv_color_t seg_color(int s) { return s == 2 ? theme()->text_secondary : theme()->accent; }
static const lv_opa_t SEG_OPA[SEGS] = {LV_OPA_COVER, LV_OPA_50, LV_OPA_40};

static void format_duration(uint64_t ms, char *out, size_t size) {
	unsigned minutes = (unsigned)(ms / 60000);
	if (minutes >= 60) {
		snprintf(out, size, tr("battery_usage_hm"), minutes / 60, minutes % 60);
	} else {
		snprintf(out, size, tr("battery_usage_m"), minutes);
	}
}

// Name and time on the left, the share of the battery on the right; no share
// for the time spent switched off. Same card as the processes page rows.
static void add_row(const char *name, uint64_t ms, float percent, bool has_percent) {
	lv_obj_t *card = lv_obj_create(list);
	lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, ui_px(12), 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_hor(card, ui_px(18), 0);
	lv_obj_set_style_pad_ver(card, ui_px(10), 0);
	lv_obj_set_style_pad_row(card, ui_px(2), 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_clickable(card, false);

	lv_obj_t *name_lbl = lv_label_create(card);
	lv_label_set_text(name_lbl, name);
	lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
	lv_obj_set_width(name_lbl, lv_pct(72));
	lv_obj_set_style_text_font(name_lbl, &font_ui_22, 0);

	char text[32];
	format_duration(ms, text, sizeof(text));
	lv_obj_t *time_lbl = lv_label_create(card);
	lv_label_set_text(time_lbl, text);
	lv_obj_add_style(time_lbl, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(time_lbl, &font_ui_20, 0);

	if (has_percent) {
		if (percent > 0 && percent < 1) {
			snprintf(text, sizeof(text), "<1%%");
		} else {
			snprintf(text, sizeof(text), "%d%%", (int)(percent + 0.5f));
		}
		lv_obj_t *pct_lbl = lv_label_create(card);
		lv_label_set_text(pct_lbl, text);
		lv_obj_set_style_text_font(pct_lbl, &font_ui_24, 0);
		lv_obj_set_ignore_layout(pct_lbl, true); // beside the column, not in it
		lv_obj_align(pct_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
	}
}

static float s_sort_used[BS_ITEM_COUNT];
static uint64_t s_sort_ms[BS_ITEM_COUNT];
static int by_use_desc(const void *a, const void *b) {
	int x = *(const int *)a, y = *(const int *)b;
	if (s_sort_used[x] != s_sort_used[y]) {
		return s_sort_used[x] < s_sort_used[y] ? 1 : -1;
	}
	return s_sort_ms[x] < s_sort_ms[y] ? 1 : s_sort_ms[x] > s_sort_ms[y] ? -1 : 0;
}

static void refresh(void) {
	lv_obj_update_layout(batteryusage_screen);
	int64_t now_ms = (int64_t)time(NULL) * 1000;
	int64_t since = batterystats_since_ms();
	float total_used = 0;
	uint64_t off_ms = 0;
	int last_level = -1;
	for (int k = 0; k < BS_ITEM_COUNT; k++) {
		s_sort_used[k] = 0;
		s_sort_ms[k] = 0;
	}

	for (int i = 0; i < BS_HOURS; i++) {
		int ago = BS_HOURS - 1 - i;
		const batterystats_hour_t *h = batterystats_hour(ago);
		int64_t start = (now_ms / BS_HOUR_MS - ago) * BS_HOUR_MS;

		time_t t = (time_t)(start / 1000);
		struct tm tm;
		localtime_r(&t, &tm);
		const char *mark = "";
		char mark_text[8];
		if (tm.tm_hour % 6 == 0) {
			snprintf(mark_text, sizeof(mark_text), "%02d", tm.tm_hour);
			mark = mark_text;
		}
		lv_label_set_text(level_marks[i], mark);
		lv_label_set_text(activity_marks[i], mark);
		lv_obj_set_x(level_marks[i], lv_obj_get_x(level_cols[i]));
		lv_obj_set_x(activity_marks[i], lv_obj_get_x(activity_cols[i]));

		// Nothing is read while suspended: an hour asleep shows the level it
		// went to sleep at. An hour switched off stays empty.
		int level = h ? h->level : -1;
		if (level < 0 && h) {
			level = last_level;
		}
		last_level = level;
		paint(level_bars[i], level > 0 ? level : 0, 100,
			  h && h->cable ? lv_palette_main(LV_PALETTE_GREEN) : theme()->accent, LV_OPA_COVER);

		int64_t recorded = 0;
		for (int s = 0; s < SEGS; s++) {
			uint32_t v = h ? h->ms[SEG_ITEMS[s]] : 0;
			recorded += v;
			paint(activity_bars[i * SEGS + s], v, BS_HOUR_MS, seg_color(s), SEG_OPA[s]);
		}
		if (h) {
			for (int k = 0; k < BS_ITEM_COUNT; k++) {
				float used = h->used[k] + h->pending[k];
				s_sort_used[k] += used;
				s_sort_ms[k] += h->ms[k];
				total_used += used;
			}
		}
		// Switched off: the part of the hour after the counting began that
		// nothing was recorded in. A minute of slack for the tick's own lag.
		if (since > 0) {
			int64_t from = start > since ? start : since;
			int64_t to = start + BS_HOUR_MS < now_ms ? start + BS_HOUR_MS : now_ms;
			if (to - from > recorded + 60000) {
				off_ms += (uint64_t)(to - from - recorded);
			}
		}
	}

	lv_label_set_text_fmt(summary, tr("battery_usage_summary"), (int)(total_used + 0.5f));
	for (int s = 0; s < SEGS; s++) {
		lv_obj_set_style_bg_color(legend_dots[s], seg_color(s), 0);
		lv_obj_set_style_bg_opa(legend_dots[s], SEG_OPA[s], 0);
	}

	lv_obj_clean(list);
	int order[BS_ITEM_COUNT];
	for (int k = 0; k < BS_ITEM_COUNT; k++) {
		order[k] = k;
	}
	qsort(order, BS_ITEM_COUNT, sizeof(order[0]), by_use_desc);
	for (int i = 0; i < BS_ITEM_COUNT; i++) {
		int k = order[i];
		if (s_sort_ms[k] >= 60000) {
			add_row(tr(ITEM_LABELS[k]), s_sort_ms[k], s_sort_used[k], true);
		}
	}
	if (off_ms >= 60000) {
		add_row(tr("battery_usage_off"), off_ms, 0, false);
	}
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	refresh();
}

void batteryusage_init(gui_config_t *cfg) {
	batteryusage_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(batteryusage_screen, cfg, "battery_usage");

	summary = lv_label_create(container);
	lv_obj_set_width(summary, lv_pct(100));
	lv_obj_add_style(summary, &theme_style_text, 0);
	lv_obj_set_style_text_font(summary, &font_ui_22, 0);

	lv_obj_t *level_card = card_create(container, "battery_usage_level");
	chart_create(level_card, 1, level_bars, level_cols, level_marks);

	lv_obj_t *activity_card = card_create(container, "battery_usage_activity");
	chart_create(activity_card, SEGS, activity_bars, activity_cols, activity_marks);
	lv_obj_t *legend = box(activity_card, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_size(legend, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_column(legend, ui_px(14), 0);
	lv_obj_set_style_pad_row(legend, ui_px(4), 0);
	for (int s = 0; s < SEGS; s++) {
		lv_obj_t *item = box(legend, LV_FLEX_FLOW_ROW);
		lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		lv_obj_set_style_pad_column(item, ui_px(6), 0);
		lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		legend_dots[s] = box(item, LV_FLEX_FLOW_ROW);
		lv_obj_set_size(legend_dots[s], ui_px(14), ui_px(14));
		lv_obj_set_style_radius(legend_dots[s], ui_px(3), 0);
		lv_obj_t *label = lv_label_create(item);
		lv_label_set_text(label, tr(ITEM_LABELS[SEG_ITEMS[s]]));
		lv_obj_add_style(label, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(label, &font_ui_18, 0);
	}

	list = box(container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_row(list, ui_px(8), 0);

	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr("battery_usage_note"));

	lv_obj_add_event_cb(batteryusage_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(batteryusage_screen);
}

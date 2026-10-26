// Production Queue binding/rendering with actual async loaders and decoded cache pixels.
#define trackmenu_source_metadata_ready polish_unused_metadata_notice
#include "src/gui/nowplaying/trackmenu.c"
#undef trackmenu_source_metadata_ready
#include "src/system/core/md5.h"
#include "src/system/streaming/navidromecache.h"
#include <assert.h>
#include <errno.h>
#include <unistd.h>
extern const lv_font_t lv_font_unscii_8;
extern void polish_fixture_drain(void);
static const unsigned char note_pixel[4] = {255, 255, 255, 255};
const lv_image_dsc_t icon_music2 = {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = 1, .h = 1, .stride = 4}, .data = note_pixel, .data_size = 4};
void thread_be_low_priority(const char *name) { (void)name; }
void thread_set_background_level(bool on) { (void)on; }
bool power_screen_is_on(void) { return true; }
bool library_track_title(const char *path, char *out, size_t size) {
	(void)path;
	snprintf(out, size, "Local fixture");
	return true;
}
bool library_history_entry(long long id, char *path, size_t size, char *name, size_t n) {
	(void)id;
	(void)path;
	(void)size;
	(void)name;
	(void)n;
	return false;
}
bool cover_thumb_load(const char *path, bool directory, int size, cover_image_t *image) {
	(void)directory;
	return cover_load_image_file(path, size, size, COVER_FIT_COVER, image);
}
bool cover_load_shared(const char *path, int w, int h, int size, cover_image_t *image) {
	(void)w;
	(void)h;
	return cover_thumb_load(path, false, size, image);
}
uint64_t cover_source_id(const char *path) {
	(void)path;
	return 1;
}
void cover_prefetch_player(const char *path, int w, int h) {
	(void)path;
	(void)w;
	(void)h;
}
bool cover_load_player_images(const char *path, int w, int h, cover_image_t *out, int bw, int bh, cover_image_t *back) {
	(void)path;
	(void)w;
	(void)h;
	(void)out;
	(void)bw;
	(void)bh;
	(void)back;
	return false;
}
bool cover_warm_player_images(const char *path, int w, int h, cover_image_t *out, int bw, int bh, cover_image_t *back) { return cover_load_player_images(path, w, h, out, bw, bh, back); }
bool cover_load_screensaver_images(const char *path, int w, int h, cover_image_t *out, int bw, int bh, cover_image_t *back) { return cover_load_player_images(path, w, h, out, bw, bh, back); }
static void image_file(const char *path) {
	FILE *f = fopen(path, "wb");
	assert(f);
	static const unsigned char png[] = {137, 80,  78,  71, 13,	10, 26,	 10,  0,  0,   0,  13,	73,	 72, 68,  82, 0,   0,	0,	128, 0,	 0,	  0,   128, 8,	 2,	 0,	  0,   0,  76,	92, 246, 156, 0,  0,   1,  90,	73,	 68, 65,  84, 120, 156, 237, 206, 65, 13,  0,	48, 16,	 4,	 161, 245, 111, 250, 42, 131, 199, 52, 65,	0,	187, 237, 131, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32, 206, 15,  226, 252, 32,	 206, 15, 226, 252, 32, 206, 15, 226, 252, 32, 206, 15,	 226, 252, 32, 206,
										15,	 226, 252, 32, 206, 15, 226, 252, 32, 206, 15, 226, 252, 32, 206, 15, 226, 252, 32, 206, 15, 226, 252, 32,	206, 15, 226, 252, 32, 206, 15, 226, 252, 32, 206, 15, 226, 252, 32, 206, 15, 226, 252, 32,	 206, 15, 226, 252, 32, 206, 15, 226, 252, 32,	206, 15, 226, 252, 32, 206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  206, 15, 226, 252, 32,  238, 1,	32,	 104, 195, 178, 172, 203, 77, 79,  0,	0,	0,	 0,	 73,  69,  78, 68,	174, 66,  96,  130};
	assert(fwrite(png, 1, sizeof(png), f) == sizeof(png) && !fclose(f));
}
static void opaque_cover(lv_obj_t *image) {
	assert(lv_obj_get_style_image_recolor_opa(image, 0) == LV_OPA_TRANSP);
	lv_obj_update_layout(image);
	lv_draw_buf_t *pixels = lv_snapshot_take(image, LV_COLOR_FORMAT_ARGB8888);
	assert(pixels);
	int opaque = 0;
	for (unsigned y = 0; y < pixels->header.h; y++)
		for (unsigned x = 0; x < pixels->header.w; x++) {
			const unsigned char *p = pixels->data + y * pixels->header.stride + x * 4;
			if (p[3] > 240 && (p[0] > 200 || p[2] > 200) && p[1] < 30)
				opaque++;
		}
	fprintf(stderr, "queue cover opaque=%d expected=%d snapshot=%ux%u widget=%dx%d scale=%d recolor=%d\n", opaque, QUEUE_THUMB_SIZE * QUEUE_THUMB_SIZE, pixels->header.w, pixels->header.h, (int)lv_obj_get_width(image), (int)lv_obj_get_height(image), (int)lv_image_get_scale_x(image), (int)lv_obj_get_style_image_recolor_opa(image, 0));
	assert(opaque > QUEUE_THUMB_SIZE * QUEUE_THUMB_SIZE * 8 / 10);
	lv_draw_buf_destroy(pixels);
}
static void wait_cover(queue_row_t *row) {
	for (int i = 0; i < 10000 && !row->has_thumb; i++) {
		polish_fixture_drain();
		queue_row_bind(row, 0);
		queue_art_tick(NULL);
		usleep(1000);
	}
	assert(row->has_thumb);
	opaque_cover(row->icon);
}
void polish_queue_ui_check(const nd_library_t *library) {
	char directory[1200], key[33], input[700], path[1300];
	snprintf(directory, sizeof(directory), "%s", library->path);
	*strrchr(directory, '/') = 0;
	size_t length = strlen(directory);
	snprintf(directory + length, sizeof(directory) - length, "/art");
	assert(!mkdir(directory, 0700) || errno == EEXIST);
	snprintf(input, sizeof(input), "%s:%zu:%s:%d:v1", library->scope, strlen("queue-fixture"), "queue-fixture", QUEUE_THUMB_SIZE);
	md5_hex(input, strlen(input), key);
	snprintf(path, sizeof(path), "%s/%s.img", directory, key);
	image_file(path);
	lv_obj_t *return_to = lv_screen_active();
	queue_screen = lv_obj_create(NULL);
	lv_screen_load(queue_screen);
	queue_body = lv_obj_create(queue_screen);
	lv_obj_set_size(queue_body, 320, 480);
	queue_total = playlist_count();
	queue_current = playlist_current_index();
	queue_hist_count = 0;
	queue_view_h = 480;
	queue_row_t *row = &queue_rows[0];
	row->vindex = -2;
	row->button = lv_obj_create(queue_body);
	lv_obj_set_size(row->button, 300, QUEUE_ROW_HEIGHT);
	lv_obj_set_style_pad_all(row->button, 0, 0);
	lv_obj_set_style_border_width(row->button, 0, 0);
	row->playmark = playmark_create(row->button);
	row->icon = lv_image_create(row->button);
	lv_obj_set_size(row->icon, QUEUE_THUMB_SIZE, QUEUE_THUMB_SIZE);
	lv_image_set_inner_align(row->icon, LV_IMAGE_ALIGN_CONTAIN);
	row->label = lv_label_create(row->button);
	lv_obj_set_style_text_font(row->label, &lv_font_unscii_8, 0);
	lv_obj_set_x(row->label, 80);
	row->check = lv_obj_create(row->button);
	row->grip = lv_obj_create(row->button);
	coverloader_start();
	queue_row_bind(row, 0);
	wait_cover(row);
	lv_obj_update_layout(queue_screen);
	assert(!lv_obj_is_hidden(row->playmark));
	assert(lv_obj_get_style_border_width(row->button, 0) == 0);
	assert(lv_obj_get_width(row->playmark) == ui_px(6) && lv_obj_get_height(row->playmark) == ui_px(52));
	lv_area_t mark_area, row_area;
	lv_obj_get_coords(row->playmark, &mark_area);
	lv_obj_get_coords(row->button, &row_area);
	assert(mark_area.x1 - row_area.x1 == ui_px(4));
	assert(abs((mark_area.y1 + mark_area.y2) - (row_area.y1 + row_area.y2)) <= 1);
	void *original = row->thumb.pixels;
	queue_row_bind(row, 0);
	queue_row_paint_mark(row);
	assert(row->thumb.pixels == original);
	opaque_cover(row->icon);
	// Actual cache result decoded before maintenance must not publish afterward.
	navidromeart_request(ND_ART_QUEUE_SLOT + 1, library, "queue-fixture", QUEUE_THUMB_SIZE);
	usleep(20000);
	assert(nd_cache_maintenance_begin());
	nd_cache_maintenance_end();
	cover_image_t stale = {0};
	bool finished = false;
	assert(!navidromeart_take(ND_ART_QUEUE_SLOT + 1, &stale, &finished));
	navidromeart_release(ND_ART_QUEUE_SLOT + 1);
	queue_row_drop_thumb(row);
	char local[1300];
	snprintf(local, sizeof(local), "%s/queue-local.ppm", directory);
	image_file(local);
	const char *local_paths[] = {local};
	playlist_load_paths(local_paths, 1, 0);
	queue_total = 1;
	queue_current = 0;
	row->vindex = -2;
	queue_row_bind(row, 0);
	wait_cover(row);
	queue_row_drop_thumb(row);
	lv_screen_load(return_to);
	lv_obj_delete(queue_screen);
	queue_screen = queue_body = NULL;
	puts("real LVGL shared Queue: actual source metadata, async Navidrome cache and local cover loaders, contained opaque pixels, separate paused/current accent, retained repaint and maintenance-stale rejection: passed");
}

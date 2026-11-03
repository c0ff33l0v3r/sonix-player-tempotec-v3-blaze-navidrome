// Real artwork workers/streamturn/HTTP/decoders and production LVGL thumbnails.
#define NAVIDROME_ART_SCHEDULER_CHECK
#define main artwork_legacy_main
#include "navidrome_artworkcheck.c"
#undef main
#define NAVIDROME_ART_REAL_WORKER
#define main player_art_mock_main
#include "navidrome_playerartuicheck.c"
#undef main
#include "src/gui/shell/theme.h"
#include "src/gui/streaming/navidromethumb.h"
#include <limits.h>
#include <poll.h>
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_changed = PTHREAD_COND_INITIALIZER;
static bool gate_open;
static void scheduler_worker_gate(void) {
	pthread_mutex_lock(&gate_lock);
	while (!gate_open)
		pthread_cond_wait(&gate_changed, &gate_lock);
	pthread_mutex_unlock(&gate_lock);
}
static void open_gate(void) {
	pthread_mutex_lock(&gate_lock);
	gate_open = true;
	pthread_cond_broadcast(&gate_changed);
	pthread_mutex_unlock(&gate_lock);
}
static bool decode_gated;
static atomic_int active_decoders, active_requests;
static unsigned decode_entered, decode_released;
static void scheduler_decode_gate(void) {
	assert(atomic_fetch_add(&active_decoders, 1) == 0);
	pthread_mutex_lock(&gate_lock);
	if (decode_gated) {
		unsigned serial = ++decode_entered;
		pthread_cond_broadcast(&gate_changed);
		while (decode_released < serial)
			pthread_cond_wait(&gate_changed, &gate_lock);
	}
	pthread_mutex_unlock(&gate_lock);
}
static void scheduler_decode_done(void) { assert(atomic_fetch_sub(&active_decoders, 1) == 1); }
const char *__real_nd_api_artwork_source(const char *, const char *, const char *, const char *, int, bool (*)(void *), void *, char **, size_t *);
const char *__wrap_nd_api_artwork_source(const char *host, const char *user, const char *password, const char *id, int pixels, bool (*current)(void *), void *arg, char **body, size_t *length) {
	assert(atomic_fetch_add(&active_requests, 1) == 0);
	const char *error = __real_nd_api_artwork_source(host, user, password, id, pixels, current, arg, body, length);
	assert(atomic_fetch_sub(&active_requests, 1) == 1);
	return error;
}
int __real_poll(struct pollfd *, nfds_t, int);
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
#ifndef NAVIDROME_ART_BASELINE
	if (count == 1 && fds[0].fd == atomic_load(&network_wake)) {
		pthread_mutex_lock(&art_lock);
		bool idle = !local_busy;
		for (int n = 0; n < ART_SLOTS; n++)
			idle &= network[n].state == NET_FREE;
		if (idle && streamturn_set_artwork_observer)
			assert(timeout == -1);
		pthread_mutex_unlock(&art_lock);
	}
#endif
	return __real_poll(fds, count, timeout);
}
static void await_decode(unsigned serial) {
	struct timespec end;
	clock_gettime(CLOCK_REALTIME, &end);
	end.tv_sec += 5;
	pthread_mutex_lock(&gate_lock);
	while (decode_entered < serial)
		assert(!pthread_cond_timedwait(&gate_changed, &gate_lock, &end));
	pthread_mutex_unlock(&gate_lock);
}
static void release_decode(unsigned serial) {
	pthread_mutex_lock(&gate_lock);
	decode_released = serial;
	pthread_cond_broadcast(&gate_changed);
	pthread_mutex_unlock(&gate_lock);
}
static atomic_int create_calls, fail_create_at;
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*entry)(void *), void *arg) {
	if (++create_calls == fail_create_at)
		return EAGAIN;
	return __real_pthread_create(thread, attr, entry, arg);
}
static atomic_uint observed_changes;
static void report_observer(void) { atomic_fetch_add(&observed_changes, 1); }

// Unrelated audiobook menu callbacks can be retained by sanitizer metadata.
void audiobookextras_add_bookmark(void) {}
void audiobookextras_open_current_bookmarks(void) {}
void audiobookextras_open_current_summary(void) {}
void chapters_open(void) {}
void gui_notify_popup(const char *text) { (void)text; }

static void scheduler_control(const char *command) {
	char url[1400], *body = NULL;
	size_t n = 0;
	int status = 0;
	snprintf(url, sizeof(url), "%s/control?%s", fixture.host, command);
	http_req_t q = {.require_complete_body = true};
	assert(http_request(url, &q, &body, &n, 1024, 8, &status) && status == 200);
	free(body);
}
static bool always(void *unused) {
	(void)unused;
	return true;
}
static void display_flush(lv_display_t *d, const lv_area_t *a, uint8_t *p) {
	(void)a;
	(void)p;
	lv_display_flush_ready(d);
}
static bool take_until(int slot, double seconds) {
	double until = now() + seconds;
	bool done = false;
	cover_image_t image = {0};
	while (now() < until) {
		if (navidromeart_take(slot, &image, &done)) {
			assert(image.pixels && image.dsc.header.w);
			cover_free(&image);
			return true;
		}
		if (done)
			return false;
		usleep(1000);
	}
	return false;
}
int main(int argc, char **argv) {
	assert(argc == 6);
	snprintf(fixture.host, sizeof(fixture.host), "%s", argv[1]);
	snprintf(root, sizeof(root), "%s", argv[2]);
	strcpy(fixture.user, "art-fixture-user");
	strcpy(fixture.password, "art-fixture-secret");
	fixture.epoch = fixture.profile_epoch = fixture.card_epoch = fixture.network_epoch = 1;
	const char *mode = argv[3];
	ui_panel_width = atoi(argv[4]);
	art_slot_t seed = fixture_bind("cached", 48, true);
	fetch("cached", 48, true, true);
	navidromeart_release(0);
	if (!strcmp(mode, "cached")) {
		open_gate();
		navidromeart_request(0, &seed.library, "held", 48);
		scheduler_control("wait=held");
		navidromeart_request_player(&seed.library, "cached", 48, 0, 0);
		bool ok = take_until(ND_ART_PLAYER_SLOT, 1.5);
		scheduler_control("release=held");
		assert(ok && "cached player must finish before held thumbnail response is released");
	} else if (!strcmp(mode, "priority")) {
		navidromeart_request(0, &seed.library, "row-first", 48);
		navidromeart_request_player(&seed.library, "player-first", 48, 0, 0);
		open_gate();
		assert(take_until(ND_ART_PLAYER_SLOT, 3));
		scheduler_control("assert_first=player-first");
		assert(take_until(0, 3));
	} else if (!strcmp(mode, "recovery")) {
		font_ui_14 = lv_font_unscii_8;
		lv_init();
		lv_style_init(&theme_style_icon);
		lv_display_t *d = lv_display_create(ui_panel_width, atoi(argv[5]));
		static uint32_t pixels[480 * 32];
		lv_display_set_buffers(d, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
		lv_display_set_flush_cb(d, display_flush);
		lv_obj_t *row = lv_obj_create(lv_screen_active());
		lv_obj_set_size(row, 200, 100);
		nd_thumb_t thumb = {0};
		navidromethumb_init(&thumb, row, ND_ART_DOWNLOAD_SLOT);
		open_gate();
		navidromethumb_bind(&thumb, &seed.library, 1, "interrupted");
		scheduler_control("wait=interrupted");
		assert(streamturn_acquire_current(always, NULL));
		scheduler_control("closed=interrupted");
		streamturn_release();
		scheduler_control("release=interrupted");
		double until = now() + 4;
		while (!thumb.finished && now() < until) {
			navidromethumb_tick(&thumb);
			usleep(1000);
		}
		assert(thumb.cover.pixels && "preempted visible thumbnail must recover without another bind");
		lv_obj_update_layout(row);
		assert(lv_image_get_src(thumb.image) == &thumb.cover.dsc);
		lv_draw_buf_t *frame = lv_snapshot_take(thumb.image, LV_COLOR_FORMAT_ARGB8888);
		assert(frame && frame->header.w);
		lv_draw_buf_destroy(frame);
		navidromethumb_release(&thumb);
		lv_obj_delete(row);
	} else if (!strcmp(mode, "preempt")) {
		open_gate();
		navidromeart_request(0, &seed.library, "held", 48);
		scheduler_control("wait=held");
		navidromeart_request_player(&seed.library, "player-first", 48, 0, 0);
		assert(take_until(ND_ART_PLAYER_SLOT, 3));
		scheduler_control("closed=held");
		scheduler_control("release=held");
		assert(take_until(0, 3));
	} else if (!strcmp(mode, "coalesce")) {
		open_gate();
		navidromeart_request(0, &seed.library, "held", 48);
		scheduler_control("wait=held");
		navidromeart_request(1, &seed.library, "held", 72);
		navidromeart_request_player(&seed.library, "held", 128, 0, 0);
		navidromeart_release(0);
		scheduler_control("release=held");
		assert(take_until(ND_ART_PLAYER_SLOT, 3));
		assert(take_until(1, 3));
	} else if (!strcmp(mode, "cached-pressure")) {
		assert(streamturn_acquire_current(always, NULL));
		open_gate();
		navidromeart_request_player(&seed.library, "cached", 48, 0, 0);
		assert(take_until(ND_ART_PLAYER_SLOT, 2));
		streamturn_release();
	} else if (!strcmp(mode, "background")) {
		assert(streamturn_acquire_background(always, NULL));
		open_gate();
		navidromeart_request(0, &seed.library, "background", 48);
		assert(take_until(0, 3));
		streamturn_release();
	} else if (!strcmp(mode, "stale")) {
		open_gate();
		navidromeart_request(0, &seed.library, "held", 48);
		scheduler_control("wait=held");
		navidromeart_request(0, &seed.library, "cached", 48);
		assert(take_until(0, 2));
		scheduler_control("release=held");
		assert(!take_until(0, .2));
	} else if (!strcmp(mode, "offline")) {
		online = false;
		seed.library.writable = false;
		open_gate();
		navidromeart_request_player_local(&seed.library, "cached", 48, 0, 0);
		assert(take_until(ND_ART_PLAYER_SLOT, 2));
		navidromeart_request(0, &seed.library, "offline-miss", 48);
		assert(!take_until(0, 2));
	} else if (!strcmp(mode, "quality")) {
		open_gate();
		navidromeart_request(0, &seed.library, "portrait", 48);
		assert(take_until(0, 3));
		navidromeart_request_player(&seed.library, "portrait", 320, 0, 0);
		assert(take_until(ND_ART_PLAYER_SLOT, 3));
	} else if (!strcmp(mode, "failure")) {
		open_gate();
		navidromeart_request(0, &seed.library, "bad", 48);
		assert(!take_until(0, 3));
		navidromeart_request(1, &seed.library, "bad", 72);
		assert(!take_until(1, 3));
	} else if (!strcmp(mode, "repeated")) {
		open_gate();
		navidromeart_request(0, &seed.library, "repeated", 48);
		for (int n = 0; n < 3; n++) {
			scheduler_control("wait=repeated");
			assert(streamturn_acquire_current(always, NULL));
			scheduler_control("closed=repeated");
			scheduler_control("reset=1");
			streamturn_release();
		}
		assert(take_until(0, 4));
	} else if (!strcmp(mode, "player-ui")) {
		font_ui_14 = lv_font_unscii_8;
		lv_init();
		lv_display_t *display = lv_display_create(ui_panel_width, atoi(argv[5]));
		player_screen = lv_obj_create(NULL);
		lv_screen_load(player_screen);
		cover_panel = lv_obj_create(player_screen);
		lv_obj_set_size(cover_panel, ui_panel_width, ui_panel_width);
		lv_obj_set_style_pad_all(cover_panel, 0, 0);
		lv_obj_set_style_border_width(cover_panel, 0, 0);
		player_menu = lv_obj_create(player_screen);
		lv_obj_set_size(player_menu, 64, 32);
		cover_img = lv_image_create(cover_panel);
		studio_cover = lv_image_create(cover_panel);
		cover_box_w = cover_box_h = studio_box_w = ui_panel_width;
		studio_box_h = atoi(argv[5]) - ui_px(210);
		backdrop_w = 64;
		backdrop_h = backdrop_studio_h = 32;
		progress_slider_timer = lv_timer_create(NULL, 1000, NULL);
		fixture_library = seed.library;
		strcpy(fixture_track.row.cover, "cached");
		open_gate();
		navidromeart_request(0, &seed.library, "held", 48);
		scheduler_control("wait=held");
		for (int mode = 0; mode < 2; mode++) {
			layout_studio_now = mode;
			navidrome_cover_key.bound = false;
			refresh_cover("ndq:real-art");
			double until = now() + 2;
			while (cover_request_outstanding && now() < until) {
				apply_cover_result();
				usleep(1000);
			}
			assert(current_cover.pixels && !cover_request_outstanding);
			assert(current_backdrop.pixels && album_tone);
			detailed_rendered(mode ? studio_cover : cover_img, &current_cover);
			cover_drop_current(true);
		}
		scheduler_control("release=held");
		navidromeart_release(ND_ART_PLAYER_SLOT);
		lv_display_delete(display);
		lv_deinit();
	} else if (!strcmp(mode, "fair")) {
		decode_gated = true;
		navidromeart_request_player(&seed.library, "cached", 48, 0, 0);
		open_gate();
		await_decode(1);
		navidromeart_request(0, &seed.library, "cached", 48);
		for (unsigned n = 1; n <= 12; n++) {
			navidromeart_request_player(&seed.library, "cached", 48, 0, 0);
			release_decode(n);
			await_decode(n + 1);
			pthread_mutex_lock(&art_lock);
			bool row_done = slots[0].done;
			pthread_mutex_unlock(&art_lock);
			if (row_done) {
				assert(take_until(0, 1));
				break;
			}
			assert(n < 12 && "ready row starved by sustained player requests");
		}
		pthread_mutex_lock(&gate_lock);
		decode_gated = false;
		decode_released = UINT_MAX;
		pthread_cond_broadcast(&gate_changed);
		pthread_mutex_unlock(&gate_lock);
	} else if (!strcmp(mode, "thread-fail-net") || !strcmp(mode, "thread-fail-local")) {
		create_calls = 0;
		fail_create_at = !strcmp(mode, "thread-fail-net") ? 1 : 2;
		open_gate();
		navidromeart_request(0, &seed.library, "thread-failure", 48);
		assert(!take_until(0, 2));
		fail_create_at = 0;
		navidromeart_request(1, &seed.library, "thread-recovery", 48);
		assert(take_until(1, 3));
	} else if (!strcmp(mode, "clear")) {
		open_gate();
		navidromeart_request(0, &seed.library, "held", 48);
		scheduler_control("wait=held");
		pthread_mutex_lock(&art_lock);
		maintenance++;
		pthread_mutex_unlock(&art_lock);
		assert(!take_until(0, 2));
		scheduler_control("closed=held");
		// A later demand cycle must not revive the unchanged retired binding.
		assert(streamturn_acquire_current(always, NULL));
		streamturn_release();
		scheduler_control("release=held");
		assert(!take_until(0, .2));
	} else if (!strcmp(mode, "observers")) {
		streamturn_set_observer(report_observer);
		open_gate();
		navidromeart_request(0, &seed.library, "cached", 48);
		assert(take_until(0, 2));
		assert(streamturn_acquire_background(always, NULL));
		bool idle;
		streamturn_observe(&idle);
		assert(!idle);
		streamturn_observe_playback(&idle);
		assert(idle);
		streamturn_release();
		assert(streamturn_acquire_current(always, NULL));
		streamturn_observe_playback(&idle);
		assert(!idle);
		streamturn_release();
		assert(atomic_load(&observed_changes) == 4);
		streamturn_set_observer(NULL);
	} else if (!strcmp(mode, "resources")) {
		int descriptors = fd_count();
		open_gate();
		for (int n = 0; n < 20; n++) {
			char id[32];
			snprintf(id, sizeof(id), "resource-%d", n);
			navidromeart_request(0, &seed.library, id, 48);
			assert(take_until(0, 3));
		}
		navidromeart_release(0);
		assert(fd_count() == descriptors + 1); // One process-lifetime eventfd.
#ifndef NAVIDROME_ART_BASELINE
		printf("slots_bytes=%zu source_table_bytes=%zu worker_stacks=%u encoded_ceiling=%u\n", sizeof(slots), sizeof(network), 2 * ART_STACK_BYTES, 2 * ART_BYTES);
#endif
	} else if (!strcmp(mode, "publication")) {
		publish_ok = false;
		open_gate();
		navidromeart_request(0, &seed.library, "unpublished", 48);
		assert(take_until(0, 3));
		navidromeart_request_player(&seed.library, "unpublished", 128, 0, 0);
		assert(take_until(ND_ART_PLAYER_SLOT, 3));
	} else if (!strcmp(mode, "allocation")) {
		fail_malloc = true;
		open_gate();
		navidromeart_request(0, &seed.library, "allocation-fail", 48);
		assert(!take_until(0, 3));
		navidromeart_request(1, &seed.library, "allocation-recover", 48);
		assert(take_until(1, 3));
	} else if (!strcmp(mode, "retire-lock") || !strcmp(mode, "retire-profile") || !strcmp(mode, "retire-card")) {
		open_gate();
		navidromeart_request(0, &seed.library, "held", 48);
		scheduler_control("wait=held");
		pthread_mutex_lock(&fixture_lock);
		if (!strcmp(mode, "retire-lock"))
			available = false;
		if (!strcmp(mode, "retire-profile"))
			fixture.profile_epoch++;
		if (!strcmp(mode, "retire-card"))
			fixture.card_epoch++;
		pthread_mutex_unlock(&fixture_lock);
		assert(!take_until(0, 2));
		scheduler_control("closed=held");
		scheduler_control("release=held");
	} else if (!strcmp(mode, "clear-decode")) {
		decode_gated = true;
		open_gate();
		navidromeart_request_player(&seed.library, "cached", 48, 0, 0);
		await_decode(1);
		maintenance++;
		assert(!take_until(ND_ART_PLAYER_SLOT, 1));
		release_decode(1);
	} else if (!strcmp(mode, "withheld")) {
		open_gate();
		navidromeart_request(0, &seed.library, "held", 48);
		scheduler_control("wait=held");
		scheduler_control("release=held");
		double until = now() + 3;
		bool done = false;
		while (!done && now() < until) {
			pthread_mutex_lock(&art_lock);
			done = slots[0].done;
			pthread_mutex_unlock(&art_lock);
			usleep(1000);
		}
		assert(done);
		maintenance++;
		assert(!take_until(0, 1));
	} else if (!strcmp(mode, "stale-decode")) {
		decode_gated = true;
		open_gate();
		navidromeart_request(0, &seed.library, "cached", 48);
		await_decode(1);
		navidromeart_request(0, &seed.library, "portrait", 48);
		release_decode(1);
		await_decode(2);
		cover_image_t old = {0};
		bool finished = false;
		assert(!navidromeart_take(0, &old, &finished) && !finished);
		pthread_mutex_lock(&gate_lock);
		decode_gated = false;
		decode_released = UINT_MAX;
		pthread_cond_broadcast(&gate_changed);
		pthread_mutex_unlock(&gate_lock);
		assert(take_until(0, 3));
#ifndef NAVIDROME_ART_BASELINE
	} else if (!strcmp(mode, "clear-handoff")) {
		open_gate();
		navidromeart_request(0, &seed.library, "held", 48);
		scheduler_control("wait=held");
		decode_gated = true;
		navidromeart_request_player(&seed.library, "cached", 48, 0, 0);
		await_decode(1);
		scheduler_control("release=held");
		struct timespec end;
		clock_gettime(CLOCK_REALTIME, &end);
		end.tv_sec += 5;
		pthread_mutex_lock(&art_lock);
		bool ready = false;
		while (!ready) {
			for (int n = 0; n < ART_SLOTS; n++)
				ready |= network[n].state == NET_READY;
			if (!ready)
				assert(!pthread_cond_timedwait(&wake, &art_lock, &end));
		}
		maintenance++;
		pthread_mutex_unlock(&art_lock);
		assert(!take_until(0, 1));
		assert(!take_until(ND_ART_PLAYER_SLOT, 1));
		release_decode(1);
#endif
	} else if (!strcmp(mode, "shutdown")) {
		open_gate();
		navidromeart_request(0, &seed.library, "held", 48);
		scheduler_control("wait=held");
		assert(fixture_art_shutdown);
		fixture_art_shutdown();
		scheduler_control("closed=held");
		scheduler_control("release=held");
		assert(!hot_source.bytes);
		assert(!take_until(0, 1));
		assert(streamturn_acquire_current(always, NULL));
		streamturn_release();
	} else
		assert(0);
	puts("real artwork scheduler/HTTP/consumer scenario passed");
	// Process-lifetime workers are terminated with this isolated fixture process.
	return 0;
}

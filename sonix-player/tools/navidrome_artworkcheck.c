// Production source cache, protected HTTP and production crop/decoder integration.
#include "src/gui/streaming/navidromeart.c"
#include <assert.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdatomic.h>
static nd_context_t fixture;
static bool available = true, online = true, publish_ok = true, invalidate_decode;
static unsigned maintenance = 1;
static int leases;
static double cancel_at;
static int cancel_kind, arm_after_decode;
static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static bool fail_malloc, fail_write;
static int partial_writes;
static double pressure_from, pressure_until, dns_until;
static atomic_int dns_calls;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t size) {
	if (fail_malloc) {
		fail_malloc = false;
		return NULL;
	}
	return __real_malloc(size);
}
ssize_t __real_write(int, const void *, size_t);
ssize_t __wrap_write(int fd, const void *data, size_t size) {
	if (fail_write && partial_writes++ == 0)
		return __real_write(fd, data, size > 7 ? 7 : size);
	if (fail_write) {
		errno = ENOSPC;
		return -1;
	}
	return __real_write(fd, data, size);
}

static char root[900];
void nd_log(const char *format, ...) {
	va_list ap;
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
}
const char *tr(const char *key) { return key; }
const char *config_get(const char *section, const char *key, const char *fallback) {
	(void)section;
	(void)key;
	return fallback;
}
void nd_debug_line(const char *line) { (void)line; }
unsigned streamturn_observe(bool *idle) {
	*idle = now() < pressure_from || now() >= pressure_until;
	return 0;
}
int __real_getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
int __wrap_getaddrinfo(const char *host, const char *port, const struct addrinfo *hints, struct addrinfo **out) {
	if (!strcmp(host, "art-dns.invalid")) {
		atomic_fetch_add(&dns_calls, 1);
		usleep(200000);
		return EAI_AGAIN;
	}
	return __real_getaddrinfo(host, port, hints, out);
}
static bool dns_current(void *arg) {
	(void)arg;
	return now() < dns_until;
}
void thread_be_background(const char *name) { (void)name; }
const char *navidrome_validate(const char *host, const char *user, const char *password, char normalized[ND_HOST_MAX]) {
	(void)user;
	(void)password;
	snprintf(normalized, ND_HOST_MAX, "%s", host);
	return NULL;
}
bool navidrome_context(nd_context_t *out, bool network) {
	*out = fixture;
	return available && (!network || online);
}
bool navidrome_context_current(const nd_context_t *c, bool network) {
	if (network && cancel_kind && now() >= cancel_at) {
		switch (cancel_kind) {
		case 1:
			available = false;
			break;
		case 2:
			fixture.card_epoch++;
			break;
		case 3:
			maintenance++;
			break;
		case 4:
			fixture.profile_epoch++;
			break;
		case 5:
			slots[0].id[0] = 0;
			break;
		case 6:
			strcpy(slots[0].id, "rebound");
			break;
		}
		cancel_kind = 0;
	}
	return available && c->epoch == fixture.epoch && c->profile_epoch == fixture.profile_epoch && c->card_epoch == fixture.card_epoch && (!network || online);
}
bool navidrome_playback_current(const nd_context_t *c, bool network) { return navidrome_context_current(c, network); }
bool navidrome_cache_begin(unsigned e, unsigned card) {
	bool ok = available && e == fixture.epoch && card == fixture.card_epoch;
	if (ok)
		leases++;
	return ok;
}
bool navidrome_playback_begin(const nd_context_t *c) { return navidrome_cache_begin(c->epoch, c->card_epoch); }
void navidrome_cache_end(void) {
	assert(leases > 0);
	leases--;
}
bool navidrome_context_publish(const nd_context_t *c, const char *temp, const char *path) { return publish_ok && navidrome_context_current(c, false) && !rename(temp, path); }
bool navidrome_playback_publish(const nd_context_t *c, const char *temp, const char *path) { return navidrome_context_publish(c, temp, path); }
unsigned nd_cache_epoch(void) { return maintenance; }
bool nd_cache_io_current(unsigned e) { return e == maintenance; }
bool nd_cache_io_begin(unsigned e) { return nd_cache_io_current(e); }
void nd_cache_io_end(void) {}
bool __real_cover_load_image_memory(const void *, size_t, int, int, cover_fit_t, cover_image_t *);
bool __wrap_cover_load_image_memory(const void *bytes, size_t length, int w, int h, cover_fit_t fit, cover_image_t *out) {
	bool ok = __real_cover_load_image_memory(bytes, length, w, h, fit, out);
	if (arm_after_decode) {
		cancel_kind = arm_after_decode;
		cancel_at = now() + .05;
		arm_after_decode = 0;
	}
	if (invalidate_decode) {
		maintenance++;
		invalidate_decode = false;
	}
	return ok;
}
static art_slot_t fixture_bind(const char *id, int size, bool writable) {
	art_slot_t job = {.serial = 1, .profile_epoch = fixture.profile_epoch, .maintenance_epoch = maintenance, .size = size};
	job.library.epoch = fixture.epoch;
	job.library.card_epoch = fixture.card_epoch;
	job.library.writable = writable;
	snprintf(job.library.scope, sizeof(job.library.scope), "%032d", 1);
	snprintf(job.library.path, sizeof(job.library.path), "%s/library.db", root);
	snprintf(job.id, sizeof(job.id), "%s", id);
	slots[0] = job;
	return job;
}
static cover_image_t run(const char *id, int size, bool writable, bool want) {
	art_slot_t job = fixture_bind(id, size, writable);
	cover_image_t out = {0};
	assert(load(&job, &out, NULL) == want);
	assert(!leases);
	if (want)
		assert(out.dsc.header.w == (unsigned)size && out.dsc.header.h == (unsigned)size);
	return out;
}
static void fetch(const char *id, int size, bool writable, bool want) {
	cover_image_t image = run(id, size, writable, want);
	cover_free(&image);
}
static void cold(void) {
	source_free(&hot_source);
	hot_path[0] = 0;
}
static void path_for(const char *id, int size, char path[1200]) {
	art_slot_t job = fixture_bind(id, 48, true);
	char key[33];
	cache_key(&job, size, key);
	snprintf(path, 1200, "%s/art/%s.img", root, key);
}
static int fd_count(void) {
	DIR *d = opendir("/proc/self/fd");
	assert(d);
	int n = 0;
	while (readdir(d))
		n++;
	closedir(d);
	return n;
}
static void control(const char *action) {
	char url[1400];
	snprintf(url, sizeof(url), "%s/control?%s", fixture.host, action);
	char *body = NULL;
	size_t length;
	assert(http_get(url, &body, &length, 10000, 8));
	free(body);
}
int main(int argc, char **argv) {
	assert(argc == 3);
	snprintf(root, sizeof(root), "%s", argv[2]);
	snprintf(fixture.host, sizeof(fixture.host), "%s", argv[1]);
	snprintf(fixture.user, sizeof(fixture.user), "art-fixture-user");
	snprintf(fixture.password, sizeof(fixture.password), "art-fixture-secret");
	fixture.epoch = fixture.card_epoch = fixture.profile_epoch = fixture.network_epoch = 1;
	int descriptors = fd_count();
	for (int kind = 0; kind < 5; kind++) {
		const char *id = (const char *[]){"square", "portrait", "landscape", "small", "panorama"}[kind];
		fetch(id, 48, true, true);
		int expected_state = kind == 3 ? ART_SMALL : kind == 4 ? ART_CAPPED : ART_BASELINE;
		assert(hot_source.state == expected_state);
		if (kind < 3)
			assert(LV_MIN(hot_source.w, hot_source.h) >= 512);
		for (int size = 200; size <= 480; size += 140)
			fetch(id, size, true, true);
		cold();
		online = false;
		fetch(id, 320, false, true);
		online = true;
	}
	// Pixel equality against the same source's production centered crop, plus
	// actual source-side baseline. PNG fixtures carry fine grid/text detail.
	for (int width = 320; width <= 480; width += 160) {
		cover_image_t actual = run("portrait", width, true, true), expected = {0};
		assert(cover_load_image_memory(hot_source.bytes, hot_source.length, width, width, COVER_FIT_COVER, &expected));
		assert(actual.dsc.data_size == expected.dsc.data_size && !memcmp(actual.pixels, expected.pixels, actual.dsc.data_size));
		cover_free(&actual);
		cover_free(&expected);
	}
	fetch("jpeg", 480, true, true);
	fetch("progressive", 480, true, true);
	fetch("retry", 200, true, true);
	assert(hot_source.state == ART_PENDING);
	fetch("retry", 480, true, true);
	assert(hot_source.state == ART_PENDING);
	cold();
	fixture.network_epoch++;
	control("recover=1");
	fetch("retry", 480, true, true);
	assert(hot_source.state == ART_BASELINE);
	fetch("bad", 48, true, false);
	fetch("bad", 480, true, false);
	fetch("oversize", 48, true, false);
	fetch("huge", 48, true, false);
	fetch("redirect", 48, true, false);
	fetch("truncated", 48, true, false);
	// A rejected publication still coalesces same-source demand in bounded RAM.
	publish_ok = false;
	fetch("no-publish", 48, true, true);
	fetch("no-publish", 480, true, true);
	publish_ok = true;
	// Best legacy art remains readable offline and upgrades lazily online.
	char path[1200], input[1200];
	path_for("legacy", 320, path);
	snprintf(input, sizeof(input), "%s/legacy.png", root);
	FILE *in = fopen(input, "rb"), *out = fopen(path, "wb");
	assert(in && out);
	char buffer[4096];
	size_t n;
	while ((n = fread(buffer, 1, sizeof(buffer), in)))
		assert(fwrite(buffer, 1, n, out) == n);
	fclose(in);
	fclose(out);
	online = false;
	cold();
	fetch("legacy", 480, false, true);
	assert(hot_source.w == 320);
	online = true;
	fetch("legacy", 480, true, true);
	assert(hot_source.state == ART_BASELINE);
	// Malformed envelopes are rejected, then legacy fallback or acquisition runs.
	path_for("square", 0, path);
	out = fopen(path, "r+b");
	assert(out);
	fputc('X', out);
	fclose(out);
	cold();
	online = false;
	fetch("square", 48, false, false);
	online = true;
	fetch("square", 48, true, true);
	// Fixed envelope validation, failed allocation/write and atomic replacement.
	art_slot_t square_job = fixture_bind("square", 48, true);
	path_for("square", 0, path);
	art_source_t saved = {0};
	assert(source_read(&square_job, &fixture, path, false, &saved));
	fail_malloc = true;
	art_source_t rejected = {0};
	assert(!source_read(&square_job, &fixture, path, false, &rejected));
	char art_dir[1100];
	snprintf(art_dir, sizeof(art_dir), "%s/art", root);
	partial_writes = 0;
	fail_write = true;
	assert(!source_publish(&square_job, &fixture, art_dir, path, &saved));
	fail_write = false;
	assert(source_read(&square_job, &fixture, path, false, &rejected));
	assert(rejected.length == saved.length && !memcmp(rejected.bytes, saved.bytes, saved.length));
	source_free(&rejected);
	for (int offset = 8; offset <= 28; offset += 4) {
		out = fopen(path, "r+b");
		assert(out);
		assert(!fseek(out, offset, SEEK_SET));
		unsigned char invalid[4] = {255, 255, 255, 255};
		assert(fwrite(invalid, 1, 4, out) == 4);
		fclose(out);
		assert(!source_read(&square_job, &fixture, path, false, &rejected));
		assert(source_publish(&square_job, &fixture, art_dir, path, &saved));
	}
	cover_image_t no_memory = {0};
	fail_malloc = true;
	// A refused fast-path allocation can fall back to the bounded decoder.
	assert(source_decode(&square_job, &fixture, &saved, &no_memory) && !fail_malloc);
	cover_free(&no_memory);
	// A genuinely adequate v1 encoding promotes without a server request.
	path_for("legacy-sharp", 512, path);
	out = fopen(path, "wb");
	assert(out && fwrite(saved.bytes, 1, saved.length, out) == saved.length);
	fclose(out);
	cold();
	fetch("legacy-sharp", 48, true, true);
	assert(hot_source.state == ART_BASELINE && access(path, F_OK));
	source_free(&saved);
	available = false;
	fetch("unavailable", 48, true, false);
	available = true;
	// Canceling the initiator preserves a separate same-source consumer.
	art_slot_t shared = fixture_bind("shared", 48, true);
	slots[1] = shared;
	slots[0].id[0] = 0;
	cover_image_t shared_image = {0};
	assert(load(&shared, &shared_image, NULL));
	cover_free(&shared_image);
	memset(&slots[1], 0, sizeof(slots[1]));
	// Stale decode and maintenance changes cannot deliver or publish pixels.
	invalidate_decode = true;
	fetch("stale", 48, true, false);
	fetch("stale", 48, true, true);
	art_slot_t job = fixture_bind("cancelled", 48, true);
	slots[0].id[0] = 0;
	cover_image_t image = {0};
	assert(!load(&job, &image, NULL));
	for (int phase = 1; phase <= 2; phase++) {
		for (int kind = 1; kind <= 6; kind++) {
			char id[32];
			snprintf(id, sizeof(id), "slow-%d-%d", phase, kind);
			cold();
			cancel_kind = phase == 1 ? kind : 0;
			arm_after_decode = phase == 2 ? kind : 0;
			cancel_at = now() + .05;
			fetch(id, 48, true, false);
			available = true;
			cancel_kind = arm_after_decode = 0;
			path_for(id, 0, path);
			assert(access(path, F_OK));
		}
	}
	// Artwork demand waits without owning the audio turn.
	pressure_until = now() + .05;
	double begin = now();
	fetch("priority", 48, true, true);
	assert(now() - begin >= .045);
	// Audio demand arriving during HTTP cancels promptly without poisoning
	// the source cooldown. The next visible demand can immediately recover.
	pressure_from = now() + .05;
	pressure_until = pressure_from + 1;
	fetch("slow-audio", 48, true, false);
	pressure_from = pressure_until = 0;
	fetch("slow-audio", 48, true, true);
	// A stuck artwork resolver gets only its own single helper quota. Reporting
	// and general finite HTTP still resolve while the abandoned helper drains.
	char *body = NULL;
	size_t length = 0;
	dns_until = now() + .03;
	assert(nd_api_artwork_source("http://art-dns.invalid", fixture.user, fixture.password, "dns", 512, dns_current, NULL, &body, &length));
	dns_until = now() + 1;
	assert(nd_api_artwork_source("http://art-dns.invalid", fixture.user, fixture.password, "dns", 512, dns_current, NULL, &body, &length));
	assert(atomic_load(&dns_calls) == 1);
	for (int report = 0; report < 2; report++) {
		http_session_t *session = http_session_new(NULL, NULL);
		assert(session);
		if (report)
			assert(http_session_report(session, 1024));
		char url[1400];
		snprintf(url, sizeof(url), "%s/control?recover=1", fixture.host);
		assert(http_session_get(session, url, false, false));
		char chunk[16];
		assert(http_session_read(session, chunk, sizeof(chunk)) == 2);
		assert(!http_session_read(session, chunk, sizeof(chunk)));
		http_session_free(session);
	}
	usleep(250000);
	// Eviction must count both formats and never select the replacement target.
	char dir[1100];
	snprintf(dir, sizeof(dir), "%s/art", root);
	for (int i = 0; i < 34; i++) {
		snprintf(path, sizeof(path), "%s/%032x.img", dir, i);
		int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
		assert(fd >= 0);
		assert(!ftruncate(fd, 1024 * 1024));
		close(fd);
	}
	fetch("evict", 48, true, true);
	assert(trim(dir, "/not-the-destination", 0));
	cold();
	assert(fd_count() == descriptors);
	// Reconstruction remains local-only even with an authenticated connection.
	art_slot_t cached_only = fixture_bind("restore-local-only", 48, true);
	cached_only.local_only = true;
	bool interrupted = false;
	assert(!load(&cached_only, &image, &interrupted));
	assert(!interrupted && !image.pixels);
	// Real serialized worker: a player miss interrupted during HTTP resumes
	// once after audio yields, without needing navigation or another binding.
	art_slot_t player = fixture_bind("slow-player-audio", 48, true);
	pressure_from = now() + .05;
	pressure_until = pressure_from + .4;
	navidromeart_request_player(&player.library, player.id, 48, 0, 0);
	bool finished = false;
	cover_image_t back = {0};
	begin = now();
	while (!finished && now() - begin < 5) {
		usleep(10000);
		if (navidromeart_take_player(&image, &back, &finished)) {
			assert(finished && image.pixels);
			break;
		}
	}
	assert(finished && image.pixels);
	cover_free(&image);
	cover_free(&back);
	navidromeart_release(ND_ART_PLAYER_SLOT);
	// All three production album-header slots must use the same decoded tone.
	art_slot_t album = fixture_bind("discovery-tone", 128, true);
	const int album_slots[] = {ND_ART_ALBUM_SLOT, ND_ART_SEARCH_ALBUM_SLOT, ND_ART_DISCOVERY_ALBUM_SLOT};
	uint32_t tone = 0;
	for (unsigned n = 0; n < sizeof(album_slots) / sizeof(album_slots[0]); n++) {
		int slot = album_slots[n];
		navidromeart_request(slot, &album.library, album.id, album.size);
		pthread_mutex_lock(&art_lock);
		assert(slots[slot].album);
		pthread_mutex_unlock(&art_lock);
		finished = false;
		begin = now();
		while (!finished && now() - begin < 5) {
			usleep(10000);
			navidromeart_take(slot, &image, &finished);
		}
		assert(finished && image.pixels && image.has_tone);
		if (n)
			assert(image.tone == tone);
		tone = image.tone;
		cover_free(&image);
		navidromeart_release(slot);
	}
	puts("artwork: shared source, aspect baseline, native PNG/JPEG/progressive crops, finite requests/cooldown/recovery, legacy/offline/read-only, corruption/bounds/redirect/truncation, failed publication, stale decode and eviction: passed");
	return 0;
}

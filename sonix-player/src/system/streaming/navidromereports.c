#include "navidromereports.h"
#include "lvgl/lvgl.h"
#include "navidromedebug.h"
#include "navidromereportstore.h"
#include "src/system/device/clock.h"
#include "streamturn.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

typedef struct {
	nd_listen_t event;
	double position, rate;
	unsigned discontinuity;
	audio_status_t status;
} presence_t;
static pthread_mutex_t mailbox = PTHREAD_MUTEX_INITIALIZER;
static nd_listen_t ingress[32];
static unsigned head, count;
static presence_t latest;
static atomic_uint demand_epoch, drops;
static atomic_bool stopping;
static pthread_t worker;
static bool started;
static lv_timer_t *timer;
static atomic_int wake_fd = -1;
static atomic_uint wake_users;
static void close_doorbell(void) {
	int fd = atomic_exchange(&wake_fd, -1);
	// Teardown only: prevent a notifier writing to a recycled descriptor.
	while (atomic_load(&wake_users))
		sched_yield();
	if (fd >= 0)
		close(fd);
}
void nd_reports_wake(void) {
	atomic_fetch_add(&wake_users, 1);
	int fd = atomic_load(&wake_fd);
	if (fd >= 0) {
		uint64_t one = 1;
		ssize_t ignored = write(fd, &one, sizeof(one));
		(void)ignored; // EAGAIN means an unread notification already exists.
	}
	atomic_fetch_sub(&wake_users, 1);
}
static void report_wait(double delay) {
	int milliseconds = delay < 0 ? -1 : delay >= (INT_MAX - 1) / 1000.0 ? INT_MAX : (int)(delay * 1000) + 1;
	struct pollfd fd = {.fd = wake_fd, .events = POLLIN};
	int rc;
	do {
		rc = poll(&fd, 1, milliseconds);
	} while (rc < 0 && errno == EINTR && !atomic_load(&stopping));
	if (rc > 0) {
		uint64_t notices;
		ssize_t ignored = read(wake_fd, &notices, sizeof(notices));
		(void)ignored;
	}
}
// The tracker and path are GUI-owned; worker only sees copied mailboxes.
static nd_listen_tracker_t tracker;
static char tracked_path[512];
static unsigned session;
static void observe(void);
#ifdef NAVIDROME_REPORT_TEST
static atomic_uint test_clock, test_wakes, test_opens;
unsigned nd_reports_test_wakes(void) { return atomic_load(&test_wakes); }
unsigned nd_reports_test_opens(void) { return atomic_load(&test_opens); }
static atomic_bool test_undated;
void nd_reports_test_undated(bool undated) {
	atomic_store(&test_undated, undated);
	nd_reports_wake();
}
void nd_reports_test_clock_silent(unsigned milliseconds) { atomic_store(&test_clock, milliseconds); }
void nd_reports_test_clock(unsigned milliseconds) {
	nd_reports_test_clock_silent(milliseconds);
	nd_reports_wake();
}
unsigned nd_reports_test_drops(void) { return atomic_load(&drops); }
void nd_reports_test_observe(void) { observe(); }
#endif

static double seconds(void) {
#ifdef NAVIDROME_REPORT_TEST
	return atomic_load(&test_clock) / 1000.0;
#else
	struct timespec t;
	clock_gettime(CLOCK_BOOTTIME, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
#endif
}
static int64_t wall_time(void) {
#ifdef NAVIDROME_REPORT_TEST
	return atomic_load(&test_undated) ? 0 : 1700000000000LL + atomic_load(&test_clock);
#else
	int64_t now = time(NULL);
	return now >= 1577836800 ? now * 1000 : 0; // same validated-clock baseline as Last.fm
#endif
}
void nd_reports_defer(void) {
	atomic_fetch_add(&demand_epoch, 1);
	nd_reports_wake();
}
static void observe(void) {
	audio_observation_t sample;
	if (!tracker.active || !audio_observe(&sample))
		return;
	nd_listen_t qualified;
	bool ready = nd_listen_sample(&tracker, &sample, seconds(), wall_time(), &qualified);
	if (strcmp(sample.path, tracked_path))
		tracker.active = false;
	if (pthread_mutex_trylock(&mailbox)) {
		if (ready)
			atomic_fetch_add(&drops, 1);
		return;
	}
	if (ready && !strcmp(sample.path, tracked_path)) {
		if (count < 32) {
			ingress[(head + count) % 32] = qualified;
			count++;
		} else
			atomic_fetch_add(&drops, 1);
	}
	if (latest.event.session != tracker.event.session || latest.status != sample.status || latest.discontinuity != sample.discontinuity)
		nd_reports_defer();
	latest = (presence_t){.event = tracker.event, .position = sample.position, .rate = sample.rate, .discontinuity = sample.discontinuity, .status = tracker.active ? sample.status : AUDIO_STATUS_STOPPED};
	pthread_mutex_unlock(&mailbox);
	nd_reports_wake();
	if (!tracker.active && timer)
		lv_timer_pause(timer);
}
void nd_reports_start(const nd_context_t *c, const nd_library_t *lib, const char *id, const char *path, double duration) {
	nd_reports_defer();
	observe(); // finalize the prior observation before rebinding identity
	tracker.active = false;
	if (!started || !id || !*id || strnlen(id, ND_ID_MAX) >= ND_ID_MAX || strlen(lib->scope) != 32)
		return;
	audio_observation_t sample;
	if (!audio_observe(&sample) || strcmp(sample.path, path))
		return;
	nd_listen_t e = {.profile = c->profile_epoch, .card = c->card_epoch, .session = ++session, .boot = seconds(), .when = wall_time()};
	snprintf(e.scope, sizeof(e.scope), "%s", lib->scope);
	snprintf(e.id, sizeof(e.id), "%s", id);
	snprintf(tracked_path, sizeof(tracked_path), "%s", path);
	nd_listen_start(&tracker, &e, &sample, e.boot, duration);
	if (timer)
		lv_timer_resume(timer);
	observe();
}
static void tick(lv_timer_t *unused) {
	(void)unused;
	observe();
}

typedef struct {
	nd_report_context_t context;
	unsigned demand, stream;
} request_t;
static bool current(void *user) {
	request_t *r = user;
	bool idle;
	unsigned epoch = streamturn_observe(&idle);
	return !atomic_load(&stopping) && idle && epoch == r->stream && atomic_load(&demand_epoch) == r->demand && navidrome_report_current(&r->context, true, false);
}
static bool matches(const nd_listen_t *e, const nd_report_context_t *c) { return e->profile == c->profile && e->card == c->card && !strcmp(e->scope, c->scope); }
static const char *request(const nd_context_t *secret, request_t *r, const char *method, const char *query, nd_document_t *doc, int *status, double *last_request) {
	const char *error = nd_api_report(secret->host, secret->user, secret->password, method, query, current, r, doc, status);
	*last_request = seconds();
	// Only bounded fixed classifications. No IDs, scope, user, history or text.
	nd_log("navidrome: report status=%d result=%s drops=%u\n", *status, error ? !strcmp(error, "nd_sync_cancelled") ? "cancelled" : "failed" : "ack", atomic_load(&drops));
	return error;
}
static bool extensions(const nd_document_t *d) {
	int a = json_get(&d->doc, d->response, "openSubsonicExtensions");
	if (!nd_api_field_unique(&d->doc, d->response, "openSubsonicExtensions", true) || a < 0 || d->doc.toks[a].type != JSON_ARRAY || json_len(&d->doc, a) > 128)
		return false;
	for (int i = 0; i < json_len(&d->doc, a); i++) {
		int item = json_at(&d->doc, a, i);
		char name[64];
		if (!nd_api_field_unique(&d->doc, item, "name", true) || !nd_api_field_unique(&d->doc, item, "versions", true) || !json_str(&d->doc, json_get(&d->doc, item, "name"), name, sizeof(name)))
			return false;
		if (strcmp(name, "playbackReport"))
			continue;
		int versions = json_get(&d->doc, item, "versions");
		if (versions < 0 || d->doc.toks[versions].type != JSON_ARRAY)
			return false;
		for (int v = 0; v < json_len(&d->doc, versions); v++) {
			int n = json_at(&d->doc, versions, v);
			if (d->doc.toks[n].type == JSON_NUMBER && d->doc.toks[n].end - d->doc.toks[n].start == 1 && d->doc.text[d->doc.toks[n].start] == '1')
				return true;
		}
	}
	return false;
}
static bool persist(const nd_report_context_t *c, nd_listen_t *e, const char *key, int64_t now) {
	if (!navidrome_report_current(c, false, true))
		return false;
	nd_report_store_t store;
#ifdef NAVIDROME_REPORT_TEST
	atomic_fetch_add(&test_opens, 1);
#endif
	bool opened = nd_report_store_open(&store, c->path, c->scope, true);
	if (opened) {
		// Even a failed COMMIT is terminal for the volatile copy: it could
		// already be durable. Never create a second delivery route.
		if (!nd_report_store_put(&store, e, key, now))
			atomic_fetch_add(&drops, 1);
	}
	nd_report_store_close(&store);
	navidrome_cache_end();
	return opened;
}
static bool take(const nd_report_context_t *c, int64_t now, nd_listen_t *e) {
	if (!c->writable || !navidrome_report_current(c, false, true))
		return false;
	nd_report_store_t store;
#ifdef NAVIDROME_REPORT_TEST
	atomic_fetch_add(&test_opens, 1);
#endif
	bool ok = nd_report_store_open(&store, c->path, c->scope, false);
	if (ok)
		ok = nd_report_store_take(&store, now, e) > 0;
	nd_report_store_close(&store);
	navidrome_cache_end();
	return ok;
}
static bool history_ready(const nd_report_context_t *c, int64_t now) {
	if (!c->writable || !navidrome_report_current(c, false, true))
		return false;
	nd_report_store_t store;
#ifdef NAVIDROME_REPORT_TEST
	atomic_fetch_add(&test_opens, 1);
#endif
	bool ok = nd_report_store_open(&store, c->path, c->scope, false);
	if (ok)
		ok = nd_report_store_ready(&store, now);
	nd_report_store_close(&store);
	navidrome_cache_end();
	return ok;
}
static void *work(void *unused) {
	(void)unused;
	// Linux nice is per-thread; failure is harmless and never prompts.
	(void)setpriority(PRIO_PROCESS, 0, 10);
	unsigned char nonce[8];
	if (!nd_random(nonce, sizeof(nonce)))
		return NULL;
	char boot_key[17];
	for (unsigned i = 0; i < sizeof(nonce); i++)
		snprintf(boot_key + 2 * i, 3, "%02x", nonce[i]);
	nd_listen_t pending[32];
	unsigned pending_count = 0;
	nd_report_context_t previous = {0};
	double quiet = seconds(), last_request = -60, last_presence = -60;
	unsigned stream = 0, demand = 0, connection = 0, announced = 0;
	bool circuit = false, storage_failed = false, had_store = false, checked_store = false, need_state = false;
	int mode = 0; // unknown, legacy, modern
	presence_t sent = {0};
	double delay = 0;
	while (!atomic_load(&stopping)) {
		report_wait(delay);
#ifdef NAVIDROME_REPORT_TEST
		atomic_fetch_add(&test_wakes, 1);
#endif
		if (atomic_load(&stopping))
			break;
		nd_report_context_t c;
		nd_context_t secret;
		bool available = navidrome_report_context(&c, &secret);
		bool idle;
		unsigned next_stream = streamturn_observe(&idle), next_demand = atomic_load(&demand_epoch);
		double now = seconds();
		if (!idle || c.busy || c.busy != previous.busy || next_stream != stream || next_demand != demand)
			quiet = now;
		stream = next_stream;
		demand = next_demand;
		if (!available || c.profile != previous.profile || c.card != previous.card || strcmp(c.scope, previous.scope)) {
			pending_count = 0;
			storage_failed = false;
			had_store = false;
			checked_store = false;
			announced = 0;
			need_state = false;
			sent = (presence_t){0};
			quiet = now;
		}
		previous = c;
		if (c.connection != connection) {
			connection = c.connection;
			circuit = false;
			mode = 0;
			announced = 0;
			need_state = false;
			sent = (presence_t){0};
		}
		presence_t p;
		pthread_mutex_lock(&mailbox);
		p = latest;
		while (count) {
			nd_listen_t e = ingress[head];
			head = (head + 1) % 32;
			count--;
			if (available && matches(&e, &c) && pending_count < 32)
				pending[pending_count++] = e;
			else
				atomic_fetch_add(&drops, 1);
		}
		pthread_mutex_unlock(&mailbox);
		int64_t wall = wall_time();
		if (available && idle && !c.busy && now - quiet >= 2 && !atomic_load(&stopping)) {
			if (!checked_store && wall > 0) {
				had_store = history_ready(&c, wall);
				checked_store = true;
			}
			if (pending_count && wall > 0) {
				nd_listen_t e = pending[0];
				if (!e.when && now >= e.boot && now - e.boot < wall / 1000.0) {
					e.when = wall - (int64_t)((now - e.boot) * 1000);
					pending[0] = e;
				}
				char key[64];
				snprintf(key, sizeof(key), "%s-%u", boot_key, e.session);
				bool consumed = e.when <= 0 || e.when < wall - ND_LISTEN_AGE || e.when > wall + 300000;
				if (!consumed && c.writable && !storage_failed) {
					consumed = persist(&c, &e, key, wall);
					if (consumed)
						had_store = true;
					else
						storage_failed = true;
				}
				if (consumed) {
					memmove(pending, pending + 1, --pending_count * sizeof(*pending));
				}
			}
			request_t r = {.context = c, .demand = demand, .stream = stream};
			bool eligible_presence = matches(&p.event, &c) && p.event.id[0];
			if (c.connected && !circuit && wall > 0 && now - last_request >= 5 && (pending_count || had_store || eligible_presence) && current(&r)) {
				nd_document_t doc;
				int status;
				const char *error = NULL;
				if (!mode) {
					error = request(&secret, &r, "getOpenSubsonicExtensions", NULL, &doc, &status, &last_request);
					mode = !error && extensions(&doc) ? 2 : 1;
					if (!error)
						nd_api_free(&doc);
					// Absent/invalid capabilities use legacy. Transport/auth failures
					// suppress only reporting until an explicit connection succeeds.
					if (error && strcmp(error, "nd_response_error") && status != 404 && status != 501 && strcmp(error, "nd_api_error") && strcmp(error, "nd_sync_cancelled"))
						circuit = true;
					if (error && (status == 401 || status == 403 || status >= 500) && status != 501)
						circuit = true;
				} else {
					nd_listen_t e = {0};
					bool event = false;
					if (had_store && !storage_failed && c.writable) {
						event = take(&c, wall, &e);
						if (!event)
							had_store = false;
					}
					if (!event && pending_count) {
						e = pending[0];
						event = true;
						memmove(pending, pending + 1, --pending_count * sizeof(*pending));
					}
					char encoded[ND_ID_MAX * 3 + 1], query[ND_ID_MAX * 3 + 256];
					const char *method = "scrobble";
					bool send = false, start = false, outgoing = false;
					presence_t target = p;
					if (event) {
						http_url_encode(e.id, encoded, sizeof(encoded));
						snprintf(query, sizeof(query), "id=%s&submission=true&time=%lld", encoded, (long long)e.when);
						send = true;
					} else if (eligible_presence) {
						bool changed = need_state || p.event.session != announced || p.status != sent.status || p.discontinuity != sent.discontinuity || p.rate != sent.rate || now - last_presence >= 60;
						if (mode == 2 && announced && p.event.session != announced) {
							target = sent;
							target.status = AUDIO_STATUS_STOPPED;
							outgoing = true;
						}
						if (mode == 2 && changed && (p.status != AUDIO_STATUS_STOPPED || announced)) {
							method = "reportPlayback";
							start = !outgoing && p.event.session != announced;
							http_url_encode(target.event.id, encoded, sizeof(encoded));
							const char *state = start ? "starting" : target.status == AUDIO_STATUS_PLAYING ? "playing" : target.status == AUDIO_STATUS_PAUSED ? "paused" : "stopped";
							snprintf(query, sizeof(query), "mediaId=%s&mediaType=song&positionMs=%.0f&state=%s&playbackRate=%.6f&ignoreScrobble=true", encoded, fmax(0, target.position) * 1000, state, target.rate);
							send = true;
						} else if (mode == 1 && p.status == AUDIO_STATUS_PLAYING && p.event.session != announced) {
							http_url_encode(p.event.id, encoded, sizeof(encoded));
							snprintf(query, sizeof(query), "id=%s&submission=false", encoded);
							send = true;
						}
					}
					// Revalidate AFTER the no-replay transaction and before HTTP.
					if (send && current(&r)) {
						error = request(&secret, &r, method, query, &doc, &status, &last_request);
						if (!error)
							nd_api_free(&doc);
						if (!event && !error) {
							announced = target.status == AUDIO_STATUS_STOPPED ? 0 : target.event.session;
							need_state = start;
							sent = target;
							last_presence = now;
						}
						if (error && mode == 2 && (status == 404 || status == 501)) {
							mode = 1;
							announced = 0;
						} else if (error && strcmp(error, "nd_sync_cancelled"))
							circuit = true;
					}
				}
			}
		}
		// Sleep until useful work changes or its next actual deadline. Offline
		// history and an empty/ineligible session do not create polling traffic.
		delay = -1;
		bool presence = matches(&p.event, &c) && p.event.id[0];
		bool presence_due = presence && (p.status != AUDIO_STATUS_STOPPED || announced) && (mode == 0 || (mode == 1 ? p.status == AUDIO_STATUS_PLAYING && p.event.session != announced : need_state || p.event.session != announced || p.status != sent.status || p.discontinuity != sent.discontinuity || p.rate != sent.rate || now - last_presence >= 60));
		bool storage_due = available && (!checked_store || (pending_count && ((!pending[0].when || pending[0].when < wall - ND_LISTEN_AGE || pending[0].when > wall + 300000) || (c.writable && !storage_failed))));
		bool network_due = available && c.connected && !circuit && ((had_store && c.writable && !storage_failed) || pending_count || presence_due);
		if ((storage_due || network_due) && idle && !c.busy) {
			if (wall <= 0)
				delay = 30; // clock eligibility recheck, not a 100-ms idle poll
			else if (now - quiet < 2)
				delay = 2 - (now - quiet);
			else if (storage_due)
				delay = 0;
			else
				delay = now - last_request < 5 ? 5 - (now - last_request) : 0;
		} else if (available && c.connected && !circuit && mode == 2 && presence && announced && idle && !c.busy) {
			delay = last_presence + 60 - now;
			if (delay < 0)
				delay = 0;
		}
		if (available && pending_count && wall > 0 && pending[0].when > 0) {
			double expiry = (pending[0].when + ND_LISTEN_AGE - wall) / 1000.0;
			if (expiry > 0 && (delay < 0 || expiry < delay))
				delay = expiry;
		}
		nd_wipe(&secret, sizeof(secret));
	}
	nd_wipe(pending, sizeof(pending));
	return NULL;
}
void nd_reports_init(void) {
	if (started)
		return;
	atomic_store(&stopping, false);
	atomic_store(&wake_fd, eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
	if (wake_fd < 0)
		return;
	pthread_attr_t attr;
	if (pthread_attr_init(&attr)) {
		close_doorbell();
		return;
	}
	int error = pthread_attr_setstacksize(&attr, 256 * 1024);
	if (!error)
		error = pthread_create(&worker, &attr, work, NULL);
	pthread_attr_destroy(&attr);
	if (error) {
		close_doorbell();
		return;
	}
	started = true;
	if (streamturn_set_observer)
		streamturn_set_observer(nd_reports_wake);
	timer = lv_timer_create(tick, 1000, NULL);
	lv_timer_pause(timer);
}
void nd_reports_shutdown(void) {
	if (!started)
		return;
	atomic_store(&stopping, true);
	if (streamturn_set_observer)
		streamturn_set_observer(NULL);
	nd_reports_wake();
	if (timer) {
		lv_timer_delete(timer);
		timer = NULL;
	}
	pthread_join(worker, NULL); // atexit only: no GUI action waits for delivery
	close_doorbell();
	started = false;
	tracker.active = false;
	pthread_mutex_lock(&mailbox);
	count = head = 0;
	memset(&latest, 0, sizeof(latest));
	pthread_mutex_unlock(&mailbox);
}

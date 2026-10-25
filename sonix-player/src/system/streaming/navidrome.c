#include "navidrome.h"
#include "navidromeapi.h"
#include "navidromebatch.h"
#include "navidromedebug.h"
#include "navidromelibrary.h"
#include "src/gui/shell/gui.h"
#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/md5.h"
#include "src/system/net/http.h"
#include "src/system/net/wifi.h"
#include <stdatomic.h>
#include <time.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef enum { JOB_SAVE, JOB_UNLOCK, JOB_CHANGE, JOB_CONNECT, JOB_FORGET, JOB_CACHE, JOB_SYNC, JOB_ENCRYPT, JOB_PLAIN } job_kind_t;
typedef struct {
	job_kind_t kind;
	unsigned generation, card_epoch, local_epoch;
	bool offer, authenticate;
	char root[1024];
	char host[ND_HOST_MAX], user[ND_USER_MAX], password[ND_SECRET_MAX];
	char passphrase[ND_SECRET_MAX], replacement[ND_SECRET_MAX];
} job_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t io_drained = PTHREAD_COND_INITIALIZER;
static unsigned cache_users, sync_owners, external_users;
static pthread_t worker;
static struct {
	bool initialized, started, stop, pending, busy, configured, invalid, unlocked, checking, connected;
	unsigned generation, revision, local_epoch, card_epoch, offer, profile_epoch, playback_network_epoch;
	bool cache_pending, sync_cancel;
	char root[1024];
	nd_library_t library;
	char path[1024], password[ND_SECRET_MAX];
	uint8_t key[32];
	nd_record_t record;
	job_t job;
	const char *error;
	void (*listener)(void);
	void (*invalidate_playback)(void);
	void (*stop_playback_network)(void);
} g;

static void changed(void *unused) {
	(void)unused;
	pthread_mutex_lock(&lock);
	void (*cb)(void) = g.listener;
	pthread_mutex_unlock(&lock);
	if (cb)
		cb();
}

static void notify(void) { gui_post(changed, NULL); }

#ifdef NAVIDROME_TEST
static int test_online = -1;
void navidrome_test_online(int online) { test_online = online; }
#endif

static bool online(void) {
#ifdef NAVIDROME_TEST
	if (test_online >= 0)
		return test_online != 0;
#endif
#ifdef HOST_BUILD
	return true; // Same host-network boundary as Tidal/Qobuz.
#else
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0];
#endif
}

const char *navidrome_validate(const char *host, const char *user, const char *password, char normalized[ND_HOST_MAX]) {
	if (!host || !user || !password || !*host || !*user || !*password)
		return "nd_fields_required";
	if (strlen(host) >= ND_HOST_MAX || strlen(user) >= ND_USER_MAX || strlen(password) >= ND_SECRET_MAX)
		return "nd_input_long";
	const char *p;
	if (!strncmp(host, "http://", 7))
		p = host + 7;
	else if (!strncmp(host, "https://", 8))
		p = host + 8;
	else
		return "nd_invalid_host";
	for (const unsigned char *c = (const unsigned char *)host; *c; c++)
		if (*c <= 32 || *c == 127 || *c == '?' || *c == '#' || *c == '@' || *c == '\\')
			return "nd_invalid_host";
	for (const unsigned char *c = (const unsigned char *)user; *c; c++)
		if (*c < 32 || *c == 127)
			return "nd_fields_required";
	const char *end = strchr(p, '/');
	if (!end)
		end = p + strlen(p);
	const char *colon = memchr(p, ':', (size_t)(end - p));
	const char *name_end = colon ? colon : end;
	if (name_end == p || name_end - p >= 256)
		return "nd_invalid_host";
	for (const char *c = p; c < name_end; c++)
		if (!(isalnum((unsigned char)*c) || *c == '.' || *c == '-'))
			return "nd_invalid_host";
	if (colon) {
		unsigned port = 0;
		if (colon + 1 == end || end - colon > 6)
			return "nd_invalid_host";
		for (const char *c = colon + 1; c < end; c++) {
			if (*c < '0' || *c > '9')
				return "nd_invalid_host";
			port = port * 10 + (unsigned)(*c - '0');
		}
		if (!port || port > 65535)
			return "nd_invalid_host";
	}
	snprintf(normalized, ND_HOST_MAX, "%s", host);
	size_t n = strlen(normalized);
	while (n && normalized[n - 1] == '/')
		normalized[--n] = 0;
	return NULL;
}

const char *navidrome_ping(const char *host, const char *user, const char *password) {
	nd_document_t doc;
	const char *error = nd_api_json(host, user, password, "ping", NULL, 16384, &doc);
	if (!error)
		nd_api_free(&doc);
	return error;
}

static bool current(const job_t *job) { return !g.stop && g.generation == job->generation; }

// Expensive file writes/fsync do not hold the snapshot mutex. Only the rename
// and in-memory adoption serialize with Lock/Forget and generation changes.
static int commit(const job_t *job, const nd_record_t *r, const uint8_t key[32], const char *password) {
	if (!nd_vault_stage(g.path, r)) {
		nd_log("navidrome: vault stage failed errno=%d\n", errno);
		return 0;
	}
	pthread_mutex_lock(&lock);
	bool applied = current(job) && nd_vault_commit(g.path);
	if (applied) {
		if (strcmp(g.record.host, r->host) || strcmp(g.record.user, r->user) || strcmp(g.password, password)) {
			g.profile_epoch++;
			if (g.invalidate_playback)
				g.invalidate_playback();
		}
		if (strcmp(g.record.host, r->host) || strcmp(g.record.user, r->user)) {
			g.local_epoch++;
			g.offer = 0;
			memset(&g.library, 0, sizeof(g.library));
		}
		g.record = *r;
		memcpy(g.key, key, 32);
		snprintf(g.password, sizeof(g.password), "%s", password);
		g.configured = g.unlocked = true;
		g.invalid = false;
		g.revision++;
	}
	pthread_mutex_unlock(&lock);
	if (!applied) {
		nd_log("navidrome: vault commit failed_or_cancelled generation=%u\n", job->generation);
		nd_vault_discard(g.path);
		return 0;
	}
	bool synced = nd_vault_sync(g.path);
	nd_log("navidrome: vault committed generation=%u synced=%d\n", job->generation, synced);
	return synced ? 1 : 2;
}

static void cache_refresh(const job_t *job, const nd_record_t *record) {
	pthread_mutex_lock(&lock);
	bool allowed = current(job) && g.unlocked && job->card_epoch == g.card_epoch;
	unsigned epoch = g.local_epoch;
	bool migrate_legacy = !g.library.path[0];
	pthread_mutex_unlock(&lock);
	if (!allowed) {
		pthread_mutex_lock(&lock);
		if (job->card_epoch == g.card_epoch)
			g.library.loading = false;
		pthread_mutex_unlock(&lock);
		return;
	}
	nd_library_t result;
	if (!navidrome_cache_begin(epoch, job->card_epoch))
		return;
	nd_cache_load(job->root, g.path, record->host, record->user, migrate_legacy, &result);
	navidrome_cache_end();
	pthread_mutex_lock(&lock);
	if (current(job) && g.unlocked && job->card_epoch == g.card_epoch) {
		if (g.library.generation != result.generation || strcmp(g.library.scope, result.scope) || strcmp(g.library.path, result.path))
			g.local_epoch++;
		g.library = result;
	}
	pthread_mutex_unlock(&lock);
	notify();
}

static bool sync_current(const job_t *job) {
	navidrome_poll_network();
	pthread_mutex_lock(&lock);
	bool valid = current(job) && g.unlocked && g.connected && job->card_epoch == g.card_epoch && !g.sync_cancel;
	pthread_mutex_unlock(&lock);
	return valid;
}

static bool sync_control_current(void *arg) { return sync_current(arg); }

static const char *scan_status(const job_t *job, const nd_record_t *record, const char *password, char marker[128]) {
	nd_document_t doc;
	const char *error = nd_api_control(record->host, record->user, password, sync_control_current, (void *)job, &doc);
	if (error)
		return error;
	int obj = json_get(&doc.doc, doc.response, "scanStatus");
	if (obj < 0 || doc.doc.toks[obj].type != JSON_OBJECT)
		error = "nd_response_error";
	else if (json_obj_bool(&doc.doc, obj, "scanning", false)) {
		nd_log("navidrome: sync scan reason=server_scanning\n");
		error = "nd_sync_inconsistent";
	}
	json_obj_str(&doc.doc, obj, "lastScan", marker, 128);
	// Some versions supply a numeric scan marker instead of an ISO timestamp.
	if (!*marker) {
		long long value = json_llong(&doc.doc, json_get(&doc.doc, obj, "lastScan"), 0);
		if (value)
			snprintf(marker, 128, "%lld", value);
	}
	nd_api_free(&doc);
	return error;
}

// Fixed slots own every fetching/ready/importing batch; no full-library queue.
typedef struct sync_pool sync_pool_t;
typedef struct {
	sync_pool_t *pool;
	pthread_t thread;
	http_session_t *session;
	int state, offset, amount;
	nd_kind_t kind;
	unsigned serial;
	nd_batch_t batch;
	const char *error;
} sync_slot_t;
struct sync_pool {
	pthread_mutex_t mutex;
	pthread_cond_t ready;
	atomic_bool stop;
	atomic_uint serial;
	int workers;
	sync_slot_t slots[2];
	const job_t *job;
	const nd_record_t *record;
	const char *password;
	bool gzip, reuse;
	size_t requests, reductions, discarded, encoded, decoded, opens, reuses, buffer_peak, commits;
	size_t accepted_pages[4], accepted_rows;
	double import_seconds, wait_seconds, parse_seconds, transfer_seconds, connect_seconds;
};
static double sync_now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}
static bool sync_slot_current(void *arg) {
	sync_slot_t *slot = arg;
	return !atomic_load(&slot->pool->stop) && slot->serial == atomic_load(&slot->pool->serial) && sync_current(slot->pool->job);
}
static void *sync_fetch(void *arg) {
	sync_slot_t *slot = arg;
	sync_pool_t *p = slot->pool;
	pthread_mutex_lock(&p->mutex);
	for (;;) {
		while (!atomic_load(&p->stop) && slot->state != 1)
			pthread_cond_wait(&p->ready, &p->mutex);
		if (atomic_load(&p->stop))
			break;
		pthread_mutex_unlock(&p->mutex);
		slot->error = nd_batch_init(&slot->batch, slot->kind) ? nd_api_page(slot->session, p->record->host, p->record->user, p->password, slot->kind, slot->offset, slot->amount, p->gzip, p->reuse, &slot->batch) : "nd_json_memory";
		pthread_mutex_lock(&p->mutex);
		slot->state = 2;
		pthread_cond_broadcast(&p->ready);
	}
	pthread_mutex_unlock(&p->mutex);
	return NULL;
}
static bool sync_pool_start(sync_pool_t *p, const job_t *job, const nd_record_t *record, const char *password) {
	memset(p, 0, sizeof(*p));
	p->job = job;
	p->record = record;
	p->password = password;
	p->gzip = p->reuse = true;
	pthread_mutex_init(&p->mutex, NULL);
	pthread_condattr_t cond;
	pthread_condattr_init(&cond);
	pthread_condattr_setclock(&cond, CLOCK_MONOTONIC);
	pthread_cond_init(&p->ready, &cond);
	pthread_condattr_destroy(&cond);
	atomic_init(&p->stop, false);
	atomic_init(&p->serial, 1);
	int wanted = 2;
#ifdef NAVIDROME_TEST
	const char *setting = getenv("SONIX_SYNC_SLOTS");
	if (setting && !strcmp(setting, "1"))
		wanted = 1;
	setting = getenv("SONIX_SYNC_GZIP");
	if (setting && !strcmp(setting, "0"))
		p->gzip = false;
	setting = getenv("SONIX_SYNC_REUSE");
	if (setting && !strcmp(setting, "0"))
		p->reuse = false;
#endif
	pthread_attr_t attr;
	if (pthread_attr_init(&attr))
		return false;
	if (pthread_attr_setstacksize(&attr, 256 * 1024)) {
		pthread_attr_destroy(&attr);
		return false;
	}
	for (int i = 0; i < wanted; i++) {
		sync_slot_t *slot = &p->slots[i];
		slot->pool = p;
		slot->serial = 1;
		slot->session = http_session_new(sync_slot_current, slot);
		bool inject = false;
#ifdef NAVIDROME_TEST
		const char *failure = getenv("SONIX_SYNC_FAIL_WORKER");
		inject = failure && atoi(failure) == i + 1;
#endif
		if (!slot->session || inject || pthread_create(&slot->thread, &attr, sync_fetch, slot)) {
			http_session_free(slot->session);
			slot->session = NULL;
			break;
		}
		p->workers++;
	}
	pthread_attr_destroy(&attr);
	return p->workers > 0;
}
static void sync_collect(sync_pool_t *p, sync_slot_t *slot) {
	http_session_stats_t stats;
	http_session_stats(slot->session, &stats);
	p->requests++;
	p->encoded += stats.encoded;
	p->decoded += stats.decoded;
	p->opens += stats.opens;
	p->reuses += stats.reuses;
	p->transfer_seconds += stats.transfer_seconds;
	p->connect_seconds += stats.connect_seconds;
	p->parse_seconds += slot->batch.parse_seconds;
	if (slot->batch.buffer_peak > p->buffer_peak)
		p->buffer_peak = slot->batch.buffer_peak;
}
static void sync_pool_wait(sync_pool_t *p) {
	struct timespec until;
	clock_gettime(CLOCK_MONOTONIC, &until);
	until.tv_nsec += 100000000;
	if (until.tv_nsec >= 1000000000) {
		until.tv_sec++;
		until.tv_nsec -= 1000000000;
	}
	pthread_cond_timedwait(&p->ready, &p->mutex, &until);
}
static const char *sync_retire(sync_pool_t *p) {
	atomic_fetch_add(&p->serial, 1);
	pthread_mutex_lock(&p->mutex);
	for (;;) {
		bool running = false;
		for (int i = 0; i < p->workers; i++)
			running |= p->slots[i].state == 1;
		if (!running)
			break;
		sync_pool_wait(p);
	}
	const char *error = NULL;
	for (int i = 0; i < p->workers; i++) {
		sync_slot_t *slot = &p->slots[i];
		if (slot->state) {
			sync_collect(p, slot);
			p->discarded++;
			if (slot->error && strcmp(slot->error, "nd_sync_cancelled") && strcmp(slot->error, "nd_page_large"))
				error = slot->error;
			nd_batch_free(&slot->batch);
			slot->state = 0;
		}
	}
	pthread_mutex_unlock(&p->mutex);
	return error;
}
static void sync_pool_end(sync_pool_t *p) {
	sync_retire(p);
	atomic_store(&p->stop, true);
	pthread_mutex_lock(&p->mutex);
	pthread_cond_broadcast(&p->ready);
	pthread_mutex_unlock(&p->mutex);
	for (int i = 0; i < p->workers; i++) {
		pthread_join(p->slots[i].thread, NULL);
		http_session_free(p->slots[i].session);
	}
	pthread_cond_destroy(&p->ready);
	pthread_mutex_destroy(&p->mutex);
}
static void sync_dispatch(sync_pool_t *p, sync_slot_t *slot, nd_kind_t kind, int offset, int amount) {
	slot->kind = kind;
	slot->offset = offset;
	slot->amount = amount;
	slot->serial = atomic_load(&p->serial);
	slot->error = NULL;
	slot->state = 1;
	pthread_cond_broadcast(&p->ready);
}
static void sync_run(const job_t *job, const nd_record_t *record, const char *password) {
	double start = sync_now(), validation_seconds = 0, publish_seconds = 0, cleanup_seconds = 0;
	pthread_mutex_lock(&lock);
	nd_library_t result = g.library;
	sync_owners++;
	pthread_mutex_unlock(&lock);
	struct stat card = {0};
	const char *error = stat(job->root, &card) == 0 ? NULL : "nd_cache_storage";
	nd_store_t store = {0};
	char before[128] = "", after[128] = "";
	if (!error && !sync_current(job))
		error = "nd_sync_cancelled";
	if (!error)
		error = scan_status(job, record, password, before);
	if (!error && navidrome_cache_begin(job->local_epoch, job->card_epoch)) {
		if (!nd_cache_open_sync(&result, &store) || !nd_store_begin(&store))
			error = "nd_cache_error";
		navidrome_cache_end();
	} else if (!error)
		error = "nd_sync_cancelled";
	sync_pool_t pool;
	bool initialized = false;
	if (!error) {
		initialized = true;
		if (!sync_pool_start(&pool, job, record, password))
			error = "nd_worker_error";
	}
	for (int kind = 0; !error && kind < 4; kind++) {
		int offset = 0, next = 0, amount = ND_BATCH_ITEMS;
		bool bootstrap = true;
#ifdef NAVIDROME_TEST
		const char *size = getenv("SONIX_SYNC_PAGE");
		if (size && atoi(size) > 0 && atoi(size) <= ND_BATCH_ITEMS)
			amount = atoi(size);
#endif
		for (;;) {
			if (!sync_current(job) || !nd_cache_card_same(job->root, card.st_dev, card.st_ino)) {
				error = "nd_sync_cancelled";
				break;
			}
			pthread_mutex_lock(&pool.mutex);
			int active = 0;
			for (int i = 0; i < pool.workers; i++)
				active += pool.slots[i].state != 0;
			for (int i = 0; i < pool.workers && (!bootstrap || !active); i++) {
				if (pool.slots[i].state)
					continue;
				if (next > INT_MAX - amount) {
					error = "nd_sync_inconsistent";
					break;
				}
				sync_dispatch(&pool, &pool.slots[i], kind, next, amount);
				active++;
				next += amount;
				if (bootstrap || kind == ND_GENRES)
					break;
			}
			sync_slot_t *slot = NULL;
			double waiting = sync_now();
			while (!error && !slot) {
				for (int i = 0; i < pool.workers; i++) {
					sync_slot_t *s = &pool.slots[i];
					if (s->state != 2)
						continue;
					if (s->error && strcmp(s->error, "nd_page_large") && strcmp(s->error, "nd_sync_cancelled")) {
						error = s->error;
						break;
					}
					if (s->offset == offset)
						slot = s;
				}
				if (!slot && !error) {
					sync_pool_wait(&pool);
					if (!sync_current(job))
						error = "nd_sync_cancelled";
				}
			}
			pool.wait_seconds += sync_now() - waiting;
			if (error) {
				pthread_mutex_unlock(&pool.mutex);
				break;
			}
			sync_collect(&pool, slot);
			if (slot->error) {
				error = slot->error;
				nd_batch_free(&slot->batch);
				slot->state = 0;
				pthread_mutex_unlock(&pool.mutex);
				if (strcmp(error, "nd_page_large") || amount == 1 || kind == ND_GENRES)
					break;
				const char *retired = sync_retire(&pool);
				if (retired) {
					error = retired;
					break;
				}
				error = NULL;
				amount = (amount + 1) / 2;
				pool.reductions++;
				next = offset;
				bootstrap = true;
				continue;
			}
			slot->state = 3;
			pthread_mutex_unlock(&pool.mutex);
			int count = slot->batch.count;
			double importing = sync_now();
			if (!sync_current(job) || !navidrome_cache_begin(job->local_epoch, job->card_epoch))
				error = "nd_sync_cancelled";
			else {
				if (!nd_store_import_batch(&store, &slot->batch))
					error = store.error ? store.error : "nd_cache_error";
				navidrome_cache_end();
				if (!error) {
					pool.commits++;
					pool.accepted_pages[kind]++;
					pool.accepted_rows += count;
				}
			}
			pool.import_seconds += sync_now() - importing;
			pthread_mutex_lock(&pool.mutex);
			nd_batch_free(&slot->batch);
			slot->state = 0;
			pthread_mutex_unlock(&pool.mutex);
			if (error)
				break;
			if (offset > INT_MAX - count) {
				error = "nd_sync_inconsistent";
				break;
			}
			offset += count;
			pthread_mutex_lock(&lock);
			if (current(job) && job->card_epoch == g.card_epoch) {
				g.library.stage = kind;
				g.library.fetched = offset;
			}
			pthread_mutex_unlock(&lock);
			notify();
			bootstrap = false;
			if (kind == ND_GENRES || count < amount) {
				error = sync_retire(&pool);
				nd_log("navidrome: sync stage kind=%d fetched=%d\n", kind, offset);
				break;
			}
		}
	}
	if (initialized)
		sync_pool_end(&pool);
	if (!error)
		error = scan_status(job, record, password, after);
	if (!error && strcmp(before, after)) {
		nd_log("navidrome: sync scan reason=marker_changed\n");
		error = "nd_sync_inconsistent";
	}
	bool leased = navidrome_cache_begin(job->local_epoch, job->card_epoch);
	if (!leased && !error)
		error = "nd_sync_cancelled";
	if (!error) {
		double phase = sync_now();
		if (!nd_store_validate(&store))
			error = store.error ? store.error : "nd_sync_inconsistent";
		validation_seconds = sync_now() - phase;
	}
	bool valid = sync_current(job) && nd_cache_card_same(job->root, card.st_dev, card.st_ino);
	pthread_mutex_lock(&lock);
	if (!error && (!valid || !current(job) || job->card_epoch != g.card_epoch || g.sync_cancel))
		error = "nd_sync_cancelled";
	double phase = sync_now();
	bool published = !error && nd_store_publish(&store);
	publish_seconds = sync_now() - phase;
	if (!error && !published)
		error = "nd_cache_error";
	if (published)
		g.local_epoch++;
	pthread_mutex_unlock(&lock);
	if (published && !nd_store_status(&store, &result))
		error = "nd_cache_error";
	phase = sync_now();
	if (leased && store.db)
		nd_store_cleanup(&store);
	nd_store_close(&store);
	cleanup_seconds = sync_now() - phase;
	if (leased)
		navidrome_cache_end();
	pthread_mutex_lock(&lock);
	sync_owners--;
	pthread_cond_broadcast(&io_drained);
	if (current(job)) {
		if (job->card_epoch == g.card_epoch) {
			if (published)
				g.library = result;
			g.library.syncing = false;
			g.library.error = error;
		}
		if (error && (!strcmp(error, "nd_auth_error") || !strcmp(error, "nd_network_error") || !strcmp(error, "nd_tls_error"))) {
			g.connected = false;
			g.error = error;
		}
		g.busy = false;
	}
	pthread_mutex_unlock(&lock);
	if (initialized) {
		nd_log("navidrome: sync metrics requests=%zu reductions=%zu discarded=%zu commits=%zu opens=%zu reuses=%zu encoded_bytes=%zu decoded_bytes=%zu slot_peak=%d buffer_peak=%zu album_pages=%zu track_pages=%zu artist_pages=%zu genre_pages=%zu accepted_rows=%zu\n", pool.requests, pool.reductions, pool.discarded, pool.commits, pool.opens, pool.reuses, pool.encoded, pool.decoded, pool.workers, pool.buffer_peak * pool.workers, pool.accepted_pages[0], pool.accepted_pages[1], pool.accepted_pages[2], pool.accepted_pages[3], pool.accepted_rows);
		nd_log("navidrome: sync timing elapsed_ms=%.3f connect_worker_ms=%.3f transfer_decode_worker_ms=%.3f parse_worker_ms=%.3f wait_ms=%.3f import_ms=%.3f validation_ms=%.3f publication_ms=%.3f cleanup_ms=%.3f tls_separate=unavailable commit_separate=unavailable\n", (sync_now() - start) * 1000, pool.connect_seconds * 1000, pool.transfer_seconds * 1000, pool.parse_seconds * 1000, pool.wait_seconds * 1000, pool.import_seconds * 1000, validation_seconds * 1000, publish_seconds * 1000, cleanup_seconds * 1000);
	}
	nd_log("navidrome: sync finish published=%d result=%s\n", published, error ? error : "ok");
}

static void *work(void *unused) {
	(void)unused;
	for (;;) {
		job_t job;
		nd_record_t r;
		uint8_t key[32];
		char password[ND_SECRET_MAX];
		pthread_mutex_lock(&lock);
		while (!g.pending && !g.cache_pending && !g.stop)
			pthread_cond_wait(&ready, &lock);
		if (g.stop) {
			pthread_mutex_unlock(&lock);
			break;
		}
		if (g.pending)
			job = g.job;
		else {
			memset(&job, 0, sizeof(job));
			job.kind = JOB_CACHE;
			job.generation = g.generation;
		}
		job.card_epoch = g.card_epoch;
		job.local_epoch = g.local_epoch;
		snprintf(job.root, sizeof(job.root), "%s", g.root);
		g.cache_pending = false;
		nd_wipe(&g.job, sizeof(g.job));
		g.pending = false;
		r = g.record;
		memcpy(key, g.key, sizeof(key));
		memcpy(password, g.password, sizeof(password));
		bool configured = g.configured;
		pthread_mutex_unlock(&lock);
		if (job.kind == JOB_CACHE || job.kind == JOB_SYNC) {
			if (job.kind == JOB_CACHE)
				cache_refresh(&job, &r);
			else
				sync_run(&job, &r, password);
			nd_wipe(&job, sizeof(job));
			nd_wipe(&r, sizeof(r));
			nd_wipe(key, sizeof(key));
			nd_wipe(password, sizeof(password));
			notify();
			continue;
		}
		static const char *operations[] = {"save", "unlock", "change_unlock_password", "reconnect", "forget", "cache", "sync", "encrypt", "unencrypt"};
		nd_log("navidrome: operation start kind=%s generation=%u configured=%d\n", operations[job.kind], job.generation, configured);
		const char *error = NULL;
		bool check = job.kind == JOB_SAVE || job.kind == JOB_UNLOCK || job.kind == JOB_CONNECT;
		if (job.kind == JOB_UNLOCK || job.kind == JOB_CHANGE || job.authenticate) {
			// Re-read before authenticating: startup metadata is only a public
			// snapshot, and a file changed since startup must not bypass GCM.
			if (nd_vault_load(g.path, &r) != 1 || r.plain)
				error = "nd_vault_error";
			else if (!nd_derive(job.passphrase, r.salt, r.iterations, key))
				error = "nd_crypto_error";
			else if (!nd_vault_open(&r, key, password))
				error = "nd_unlock_error";
		}
		nd_wipe(job.passphrase, sizeof(job.passphrase));
		if (!error && job.kind == JOB_SAVE) {
			bool same = configured && !strcmp(r.host, job.host) && !strcmp(r.user, job.user);
			r.authenticated = same && r.authenticated;
			snprintf(r.host, sizeof(r.host), "%s", job.host);
			snprintf(r.user, sizeof(r.user), "%s", job.user);
			snprintf(password, sizeof(password), "%s", job.password);
			if (!configured) {
				if (*job.replacement) {
					if (!nd_vault_prepare(&r, job.replacement, key))
						error = "nd_crypto_error";
				} else
					nd_vault_plain(&r, password);
			}
		}
		if (!error && (job.kind == JOB_CHANGE || job.kind == JOB_ENCRYPT) && !nd_vault_prepare(&r, job.replacement, key))
			error = "nd_crypto_error";
		if (!error && job.kind == JOB_PLAIN) {
			nd_vault_plain(&r, password);
			nd_wipe(key, sizeof(key));
		}
		nd_wipe(job.replacement, sizeof(job.replacement));
		nd_wipe(job.password, sizeof(job.password));
		if (!error && (job.kind == JOB_SAVE || job.kind == JOB_CHANGE || job.kind == JOB_ENCRYPT || job.kind == JOB_PLAIN)) {
			r.version = 2;
			if (!nd_vault_seal(&r, key, password))
				error = "nd_crypto_error";
			else {
				int result = commit(&job, &r, key, password);
				if (result != 1)
					error = result == 2 ? "nd_durability_error" : "nd_save_error";
			}
		}
		if (!error && job.kind == JOB_FORGET && !nd_cache_retire_scope(g.path))
			error = "nd_remove_error";
		pthread_mutex_lock(&lock);
		bool valid = current(&job);
		if (valid && !error && job.kind == JOB_FORGET) {
			bool removed = nd_vault_remove(g.path);
			if (!removed)
				error = "nd_remove_error";
			if (removed || (access(g.path, F_OK) != 0 && errno == ENOENT)) {
				nd_wipe(&g.record, sizeof(g.record));
				g.configured = g.invalid = false;
				g.revision++;
			}
		}
		if (valid && !error && job.kind == JOB_UNLOCK) {
			g.record = r;
			memcpy(g.key, key, sizeof(key));
			memcpy(g.password, password, sizeof(password));
			g.unlocked = true;
			g.revision++;
		}
		if (valid && !error && check) {
			g.checking = true;
			g.connected = false;
		}
		pthread_mutex_unlock(&lock);
		if (valid && !error && job.kind == JOB_FORGET && !nd_vault_sync(g.path))
			error = "nd_remove_error";
		// A failed removal must not leave plain mode asking for an unlock
		// password it never had. Reload the surviving record on this worker.
		if (valid && error && job.kind == JOB_FORGET && r.plain) {
			nd_record_t surviving;
			if (nd_vault_load(g.path, &surviving) == 1 && surviving.plain && nd_vault_open(&surviving, NULL, password)) {
				pthread_mutex_lock(&lock);
				if (current(&job)) {
					g.record = surviving;
					memcpy(g.password, password, sizeof(g.password));
					g.unlocked = true;
					g.revision++;
				}
				pthread_mutex_unlock(&lock);
			}
			nd_wipe(&surviving, sizeof(surviving));
		}
		if (valid && !error && job.kind == JOB_FORGET) {
			if (!nd_cache_retire(job.root, g.path)) {
				pthread_mutex_lock(&lock);
				g.library.error = "nd_cache_error";
				pthread_mutex_unlock(&lock);
			}
		}
		if (valid && !error && job.kind != JOB_FORGET)
			cache_refresh(&job, &r);
		if (valid && !error && check) {
			notify();
			error = online() ? navidrome_ping(r.host, r.user, password) : "nd_wifi_error";
			// Recheck Wi-Fi even if the user left the menu during this request.
			if (!error && !online())
				error = "nd_wifi_error";
			if (!error && !r.authenticated) {
				r.authenticated = true;
				r.version = 2;
				if (!nd_vault_seal(&r, key, password))
					error = "nd_history_error";
				else {
					if (commit(&job, &r, key, password) != 1)
						error = "nd_history_error";
				}
			}
		}
		pthread_mutex_lock(&lock);
		bool accepted = current(&job);
		if (accepted) {
			g.error = error;
			g.busy = g.checking = false;
			if (check) {
				g.connected = !error || !strcmp(error, "nd_history_error");
				if (g.connected && job.offer)
					g.offer = job.generation;
			}
		}
		pthread_mutex_unlock(&lock);
		nd_log("navidrome: operation finish kind=%s generation=%u accepted=%d result=%s\n", operations[job.kind], job.generation, accepted, error ? error : "ok");
		nd_wipe(&job, sizeof(job));
		nd_wipe(&r, sizeof(r));
		nd_wipe(key, sizeof(key));
		nd_wipe(password, sizeof(password));
		notify();
	}
	return NULL;
}

void navidrome_init(void) {
	pthread_mutex_lock(&lock);
	if (!g.initialized) {
		g.initialized = true;
		if (!nd_vault_path(config_path(), g.path, sizeof(g.path)))
			g.error = "nd_save_error";
		else {
			int result = nd_vault_load(g.path, &g.record);
			g.configured = result != 0;
			g.invalid = result < 0;
			if (result == 1 && g.record.plain)
				g.unlocked = nd_vault_open(&g.record, NULL, g.password);
			if (g.invalid)
				g.error = "nd_vault_error";
		}
		nd_log("navidrome: initialized storage_path_available=%d configured=%d invalid=%d result=%s\n", g.path[0] != 0, g.configured, g.invalid, g.error ? g.error : "ok");
	}
	pthread_mutex_unlock(&lock);
}

void navidrome_set_listener(void (*cb)(void)) {
	pthread_mutex_lock(&lock);
	g.listener = cb;
	pthread_mutex_unlock(&lock);
}

void navidrome_snapshot(navidrome_snapshot_t *out) {
	pthread_mutex_lock(&lock);
	memset(out, 0, sizeof(*out));
	snprintf(out->host, sizeof(out->host), "%s", g.record.host);
	snprintf(out->user, sizeof(out->user), "%s", g.record.user);
	out->configured = g.configured;
	out->encrypted = g.configured && !g.record.plain;
	out->unlocked = g.unlocked;
	out->authenticated = g.unlocked && g.record.authenticated;
	out->invalid = g.invalid;
	out->busy = g.busy || external_users;
	out->revision = g.revision;
	out->error = g.error;
	out->state = !g.configured ? ND_UNCONFIGURED : !g.unlocked ? ND_LOCKED : g.checking ? ND_CONNECTING : g.connected ? ND_CONNECTED : ND_DISCONNECTED;
	pthread_mutex_unlock(&lock);
}

bool navidrome_password(char out[ND_SECRET_MAX]) {
	pthread_mutex_lock(&lock);
	nd_wipe(out, ND_SECRET_MAX);
	bool ok = g.unlocked;
	if (ok)
		memcpy(out, g.password, ND_SECRET_MAX);
	pthread_mutex_unlock(&lock);
	return ok;
}

// lock must be held. One pending slot, with no secret-bearing backlog.
static bool submit(job_t *job) {
	if (g.busy || (external_users && job->kind != JOB_FORGET) || g.stop || !g.initialized)
		return false;
	if (!g.started) {
		if (pthread_create(&worker, NULL, work, NULL)) {
			g.error = "nd_worker_error";
			return false;
		}
		g.started = true;
	}
	g.busy = true;
	g.error = NULL;
	if (job->kind == JOB_SAVE || job->kind == JOB_CONNECT || job->kind == JOB_UNLOCK)
		g.offer = 0;
	job->generation = ++g.generation;
	g.job = *job;
	g.pending = true;
	if (job->kind == JOB_CONNECT || job->kind == JOB_SAVE || job->kind == JOB_UNLOCK)
		g.connected = false;
	pthread_cond_signal(&ready);
	return true;
}

static bool save(const char *host, const char *user, const char *password, const char *passphrase, bool offer) {
	job_t job = {.kind = JOB_SAVE, .offer = offer};
	const char *error = navidrome_validate(host, user, password, job.host);
	pthread_mutex_lock(&lock);
	if (!error && ((!g.configured && passphrase && !nd_passphrase_valid(passphrase)) || (g.configured && (!g.unlocked || g.invalid))))
		error = g.configured ? "nd_unlock_required" : "nd_passphrase_short";
	bool ok = false;
	if (error)
		g.error = error;
	else {
		snprintf(job.user, sizeof(job.user), "%s", user);
		snprintf(job.password, sizeof(job.password), "%s", password);
		if (passphrase)
			snprintf(job.replacement, sizeof(job.replacement), "%s", passphrase);
		ok = submit(&job);
	}
	pthread_mutex_unlock(&lock);
	nd_wipe(&job, sizeof(job));
	notify();
	return ok;
}

bool navidrome_save(const char *host, const char *user, const char *password, const char *passphrase) { return save(host, user, password, passphrase, true); }
bool navidrome_update_password(const char *host, const char *user, const char *password) { return save(host, user, password, NULL, false); }

bool navidrome_unlock(const char *passphrase) {
	job_t job = {.kind = JOB_UNLOCK};
	pthread_mutex_lock(&lock);
	bool ok = false;
	if (g.configured && !g.record.plain && !g.invalid && passphrase && *passphrase && strlen(passphrase) < ND_SECRET_MAX) {
		snprintf(job.passphrase, sizeof(job.passphrase), "%s", passphrase);
		ok = submit(&job);
	} else
		g.error = g.invalid ? "nd_vault_error" : "nd_unlock_error";
	pthread_mutex_unlock(&lock);
	nd_wipe(&job, sizeof(job));
	notify();
	return ok;
}

bool navidrome_change_passphrase(const char *current_passphrase, const char *replacement) {
	job_t job = {.kind = JOB_CHANGE};
	pthread_mutex_lock(&lock);
	bool ok = false;
	if (!g.unlocked || g.record.plain)
		g.error = "nd_unlock_required";
	else if (!current_passphrase || !*current_passphrase || strlen(current_passphrase) >= ND_SECRET_MAX || !nd_passphrase_valid(replacement))
		g.error = "nd_passphrase_short";
	else {
		snprintf(job.passphrase, sizeof(job.passphrase), "%s", current_passphrase);
		snprintf(job.replacement, sizeof(job.replacement), "%s", replacement);
		ok = submit(&job);
	}
	pthread_mutex_unlock(&lock);
	nd_wipe(&job, sizeof(job));
	notify();
	return ok;
}

bool navidrome_encrypt(const char *replacement) {
	job_t job = {.kind = JOB_ENCRYPT};
	pthread_mutex_lock(&lock);
	bool ok = false;
	if (!g.configured || !g.unlocked || !g.record.plain || g.invalid)
		g.error = "nd_unlock_required";
	else if (!nd_passphrase_valid(replacement))
		g.error = "nd_passphrase_short";
	else {
		snprintf(job.replacement, sizeof(job.replacement), "%s", replacement);
		ok = submit(&job);
	}
	pthread_mutex_unlock(&lock);
	nd_wipe(&job, sizeof(job));
	notify();
	return ok;
}

bool navidrome_unencrypt(const char *passphrase) {
	job_t job = {.kind = JOB_PLAIN};
	pthread_mutex_lock(&lock);
	bool ok = false;
	if (!g.configured || g.record.plain || g.invalid)
		g.error = "nd_vault_error";
	else if (!g.unlocked && (!passphrase || !*passphrase || strlen(passphrase) >= ND_SECRET_MAX))
		g.error = "nd_unlock_required";
	else {
		job.authenticate = !g.unlocked;
		if (job.authenticate)
			snprintf(job.passphrase, sizeof(job.passphrase), "%s", passphrase);
		ok = submit(&job);
	}
	pthread_mutex_unlock(&lock);
	nd_wipe(&job, sizeof(job));
	notify();
	return ok;
}

bool navidrome_reconnect(void) {
	job_t job = {.kind = JOB_CONNECT, .offer = true};
	pthread_mutex_lock(&lock);
	bool ok = g.unlocked && !g.invalid && submit(&job);
	pthread_mutex_unlock(&lock);
	notify();
	return ok;
}

static void clear_secrets(void) {
	g.profile_epoch++;
	if (g.invalidate_playback)
		g.invalidate_playback();
	g.generation++;
	g.local_epoch++;
	g.offer = 0;
	g.library.syncing = false;
	g.unlocked = g.connected = g.checking = g.busy = g.pending = false;
	nd_wipe(&g.job, sizeof(g.job));
	nd_wipe(g.key, sizeof(g.key));
	nd_wipe(g.password, sizeof(g.password));
	if (g.record.plain)
		nd_wipe(g.record.cipher, sizeof(g.record.cipher));
	g.revision++;
}

void navidrome_lock(void) {
	pthread_mutex_lock(&lock);
	if (g.record.plain) {
		pthread_mutex_unlock(&lock);
		return;
	}
	clear_secrets();
	g.error = NULL;
	pthread_mutex_unlock(&lock);
	notify();
}

bool navidrome_forget(void) {
	job_t job = {.kind = JOB_FORGET};
	pthread_mutex_lock(&lock);
	clear_secrets();
	bool ok = submit(&job);
	pthread_mutex_unlock(&lock);
	notify();
	return ok;
}

void navidrome_poll_network(void) {
	if (online())
		return;
	pthread_mutex_lock(&lock);
	if (g.connected || g.checking) {
		g.playback_network_epoch++;
		if (g.stop_playback_network)
			g.stop_playback_network();
		g.generation++;
		g.connected = g.checking = g.busy = false;
		g.library.syncing = false;
		g.error = "nd_wifi_error";
	}
	pthread_mutex_unlock(&lock);
}

void navidrome_reset(void) {
	// Reset terminates the service before removing files, so a cancelled
	// worker cannot recreate even a staging file after reset has finished.
	navidrome_shutdown();
	pthread_mutex_lock(&lock);
	clear_secrets();
	if (nd_vault_remove(g.path)) {
		nd_vault_sync(g.path);
		nd_cache_retire(g.root, g.path);
		nd_wipe(&g.record, sizeof(g.record));
		g.configured = g.invalid = false;
		g.error = NULL;
	} else
		g.error = "nd_remove_error";
	pthread_mutex_unlock(&lock);
}

void navidrome_shutdown(void) {
	pthread_mutex_lock(&lock);
	clear_secrets();
	g.stop = true;
	pthread_cond_signal(&ready);
	bool join = g.started;
	g.started = false;
	g.listener = NULL;
	pthread_mutex_unlock(&lock);
	if (join)
		pthread_join(worker, NULL);
	pthread_mutex_lock(&lock);
	while (cache_users || sync_owners || external_users)
		pthread_cond_wait(&io_drained, &lock);
	pthread_mutex_unlock(&lock);
	nd_debug_shutdown();
}

const char *navidrome_feedback(const navidrome_snapshot_t *s) {
	if (s->invalid)
		return "nd_vault_error";
	if (s->state == ND_UNCONFIGURED)
		return "nd_not_configured";
	if (s->state == ND_LOCKED)
		return "nd_unlock_required";
	if (s->state == ND_CONNECTING)
		return "nd_connecting";
	if (s->busy)
		return "nd_working";
	if (s->error)
		return s->error;
	return s->state == ND_CONNECTED ? "nd_connected" : "nd_reconnect_required";
}

void navidrome_set_root(const char *root) {
	nd_debug_card(root);
	pthread_mutex_lock(&lock);
	if (root && strlen(root) < sizeof(g.root))
		snprintf(g.root, sizeof(g.root), "%s", root);
	else
		g.root[0] = 0;
	g.card_epoch++;
	g.local_epoch++;
	if (g.invalidate_playback)
		g.invalidate_playback();
	// Card events run on the storage worker. Leases only cover local file I/O,
	// never network waits, and no new old-card lease can enter after this epoch.
	while (cache_users || sync_owners)
		pthread_cond_wait(&io_drained, &lock);
	memset(&g.library, 0, sizeof(g.library));
	g.cache_pending = true;
	g.library.loading = true;
	if (!g.started && g.initialized && !g.stop) {
		if (pthread_create(&worker, NULL, work, NULL) == 0)
			g.started = true;
		else {
			g.cache_pending = false;
			g.library.loading = false;
			g.library.error = "nd_worker_error";
		}
	}
	pthread_cond_signal(&ready);
	pthread_mutex_unlock(&lock);
	notify();
}

void navidrome_library_snapshot(nd_library_t *out) {
	pthread_mutex_lock(&lock);
	*out = g.library;
	out->epoch = g.local_epoch;
	out->card_epoch = g.card_epoch;
	out->offer = g.offer;
	if (!g.unlocked) {
		out->ready = false;
		out->offer = 0;
		out->scope[0] = out->path[0] = 0;
		memset(out->counts, 0, sizeof(out->counts));
		out->finished = 0;
	}
	pthread_mutex_unlock(&lock);
}

bool navidrome_sync(void) {
	navidrome_poll_network();
	job_t job = {.kind = JOB_SYNC};
	pthread_mutex_lock(&lock);
	bool ok = g.unlocked && g.connected && g.library.writable && *g.library.path && *g.library.scope && submit(&job);
	if (ok) {
		g.offer = 0;
		g.sync_cancel = false;
		g.library.syncing = true;
		g.library.stage = g.library.fetched = 0;
		g.library.error = NULL;
	}
	pthread_mutex_unlock(&lock);
	notify();
	return ok;
}

void navidrome_sync_cancel(void) {
	pthread_mutex_lock(&lock);
	g.sync_cancel = true;
	pthread_mutex_unlock(&lock);
	notify();
}

void navidrome_sync_offer_consume(unsigned offer) {
	pthread_mutex_lock(&lock);
	if (g.offer == offer)
		g.offer = 0;
	pthread_mutex_unlock(&lock);
}

bool navidrome_context(nd_context_t *out, bool network) {
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&lock);
	bool ok = g.unlocked && !g.invalid && (!network || g.connected);
	if (ok) {
		snprintf(out->host, sizeof(out->host), "%s", g.record.host);
		snprintf(out->user, sizeof(out->user), "%s", g.record.user);
		memcpy(out->password, g.password, sizeof(out->password));
		out->epoch = g.local_epoch;
		out->card_epoch = g.card_epoch;
		out->network_epoch = g.generation;
		out->profile_epoch = g.profile_epoch;
		out->playback_network_epoch = g.playback_network_epoch;
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

bool navidrome_context_current(const nd_context_t *context, bool network) {
	pthread_mutex_lock(&lock);
	bool ok = !g.stop && g.unlocked && context->epoch == g.local_epoch && context->card_epoch == g.card_epoch && (!network || (g.connected && context->network_epoch == g.generation));
	pthread_mutex_unlock(&lock);
	return ok;
}

bool navidrome_context_publish(const nd_context_t *context, const char *temp, const char *path) {
	pthread_mutex_lock(&lock);
	bool ok = g.unlocked && g.connected && context->epoch == g.local_epoch && context->card_epoch == g.card_epoch && context->network_epoch == g.generation && rename(temp, path) == 0;
	pthread_mutex_unlock(&lock);
	return ok;
}

bool navidrome_cache_begin(unsigned epoch, unsigned card_epoch) {
	pthread_mutex_lock(&lock);
	bool ok = g.unlocked && !g.stop && epoch == g.local_epoch && card_epoch == g.card_epoch;
	if (ok)
		cache_users++;
	pthread_mutex_unlock(&lock);
	return ok;
}

void navidrome_cache_end(void) {
	pthread_mutex_lock(&lock);
	if (cache_users)
		cache_users--;
	pthread_cond_broadcast(&io_drained);
	pthread_mutex_unlock(&lock);
}

static bool playback_current(const nd_context_t *c, bool network) { return !g.stop && g.unlocked && !g.invalid && c->profile_epoch == g.profile_epoch && c->card_epoch == g.card_epoch && (!network || (g.connected && c->playback_network_epoch == g.playback_network_epoch)); }

bool navidrome_playback_current(const nd_context_t *c, bool network) {
	pthread_mutex_lock(&lock);
	bool ok = playback_current(c, network);
	pthread_mutex_unlock(&lock);
	return ok;
}

bool navidrome_playback_begin(const nd_context_t *c) {
	pthread_mutex_lock(&lock);
	bool ok = playback_current(c, false);
	if (ok)
		cache_users++;
	pthread_mutex_unlock(&lock);
	return ok;
}

bool navidrome_playback_publish(const nd_context_t *c, const char *temp, const char *path) {
	pthread_mutex_lock(&lock);
	bool ok = playback_current(c, true) && rename(temp, path) == 0;
	pthread_mutex_unlock(&lock);
	return ok;
}

bool navidrome_playback_pin(const nd_library_t *library) {
	pthread_mutex_lock(&lock);
	bool ok = !g.stop && g.unlocked && library->ready && library->epoch == g.local_epoch && library->card_epoch == g.card_epoch && library->generation == g.library.generation && !strcmp(library->scope, g.library.scope) && nd_store_retain(library->scope, library->generation);
	pthread_mutex_unlock(&lock);
	return ok;
}

void navidrome_network_failed(const nd_context_t *c) {
	pthread_mutex_lock(&lock);
	if (playback_current(c, true)) {
		g.playback_network_epoch++;
		g.generation++;
		g.connected = false;
		g.error = "nd_network_error";
		g.revision++;
	}
	pthread_mutex_unlock(&lock);
	notify();
}

void navidrome_set_playback_invalidator(void (*invalidate)(void)) {
	pthread_mutex_lock(&lock);
	g.invalidate_playback = invalidate;
	pthread_mutex_unlock(&lock);
}

void navidrome_set_playback_network_stop(void (*cancel)(void)) {
	pthread_mutex_lock(&lock);
	g.stop_playback_network = cancel;
	pthread_mutex_unlock(&lock);
}

bool navidrome_external_begin(const nd_context_t *context, bool network) {
	pthread_mutex_lock(&lock);
	bool ok = !g.stop && !g.busy && !external_users && g.unlocked && context->epoch == g.local_epoch && context->card_epoch == g.card_epoch && (!network || (g.connected && context->network_epoch == g.generation));
	if (ok)
		external_users++;
	pthread_mutex_unlock(&lock);
	return ok;
}
void navidrome_external_end(void) {
	pthread_mutex_lock(&lock);
	if (external_users)
		external_users--;
	pthread_cond_broadcast(&io_drained);
	pthread_mutex_unlock(&lock);
}
bool navidrome_context_commit(const nd_context_t *context, sqlite3 *db) {
	pthread_mutex_lock(&lock);
	bool ok = !g.stop && g.unlocked && context->epoch == g.local_epoch && context->card_epoch == g.card_epoch;
	if (ok)
		ok = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
	pthread_mutex_unlock(&lock);
	return ok;
}

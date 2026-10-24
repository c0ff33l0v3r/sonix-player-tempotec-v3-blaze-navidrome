#include "navidrome.h"
#include "navidromeapi.h"
#include "navidromedebug.h"
#include "navidromelibrary.h"
#include "src/gui/shell/gui.h"
#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/md5.h"
#include "src/system/net/http.h"
#include "src/system/net/wifi.h"

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
static unsigned cache_users;
static pthread_t worker;
static struct {
	bool initialized, started, stop, pending, busy, configured, invalid, unlocked, checking, connected;
	unsigned generation, revision, local_epoch, card_epoch, offer;
	bool cache_pending, sync_cancel;
	char root[1024];
	nd_library_t library;
	char path[1024], password[ND_SECRET_MAX];
	uint8_t key[32];
	nd_record_t record;
	job_t job;
	const char *error;
	void (*listener)(void);
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
	nd_cache_load(job->root, g.path, record->host, record->user, &result);
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

static const char *scan_status(const nd_record_t *record, const char *password, char marker[128]) {
	nd_document_t doc;
	const char *error = nd_api_json(record->host, record->user, password, "getScanStatus", NULL, 16384, &doc);
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

static void sync_run(const job_t *job, const nd_record_t *record, const char *password) {
	pthread_mutex_lock(&lock);
	nd_library_t result = g.library;
	pthread_mutex_unlock(&lock);
	struct stat card = {0};
	const char *error = stat(job->root, &card) == 0 ? NULL : "nd_cache_storage";
	nd_store_t store = {0};
	char before[128] = "", after[128] = "";
	if (!error && !sync_current(job))
		error = "nd_sync_cancelled";
	if (!error)
		error = scan_status(record, password, before);
	int64_t candidate = 0;
	if (!error) {
		if (!navidrome_cache_begin(job->local_epoch, job->card_epoch))
			error = "nd_sync_cancelled";
		else {
			if (!nd_cache_open_sync(&result, &store) || !nd_store_begin(&store))
				error = "nd_cache_error";
			candidate = store.generation;
			nd_store_close(&store);
			navidrome_cache_end();
		}
	}
	for (int kind = 0; !error && kind < 4; kind++) {
		int offset = 0;
		int amount = 50;
		for (;;) {
			if (!sync_current(job) || !nd_cache_card_same(job->root, card.st_dev, card.st_ino)) {
				error = "nd_sync_cancelled";
				break;
			}
			char query[256];
			int count = 0;
			nd_document_t doc;
			for (;;) {
				if (kind != ND_GENRES)
					snprintf(query, sizeof(query), "query=&songCount=%d&songOffset=%d&albumCount=%d&albumOffset=%d&artistCount=%d&artistOffset=%d", kind == ND_TRACKS ? amount : 0, offset, kind == ND_ALBUMS ? amount : 0, offset, kind == ND_ARTISTS ? amount : 0, offset);
				error = nd_api_json(record->host, record->user, password, kind == ND_GENRES ? "getGenres" : "search3", kind == ND_GENRES ? NULL : query, ND_API_BYTES, &doc);
				if (error)
					nd_log("navidrome: sync request kind=%d offset=%d count=%d result=%s\n", kind, offset, amount, error);
				if (!error || strcmp(error, "nd_page_large") || amount == 1 || kind == ND_GENRES || !sync_current(job))
					break;
				amount = (amount + 1) / 2;
			}
			if (error)
				break;
			if (!sync_current(job))
				error = "nd_sync_cancelled";
			else if (!navidrome_cache_begin(job->local_epoch, job->card_epoch))
				error = "nd_sync_cancelled";
			else {
				if (!nd_store_open(&store, result.path, result.scope, true, false))
					error = "nd_cache_error";
				else {
					store.generation = candidate;
					if (!nd_store_import(&store, kind, &doc, &count))
						error = store.error ? store.error : "nd_response_error";
				}
				nd_store_close(&store);
				navidrome_cache_end();
			}
			nd_api_free(&doc);
			if (error) {
				nd_log("navidrome: sync page kind=%d offset=%d limit=%d result=%s\n", kind, offset, amount, error);
				break;
			}
			if ((kind != ND_GENRES && count > amount) || offset > INT_MAX - count) {
				nd_log("navidrome: sync page kind=%d offset=%d count=%d limit=%d reason=invalid_count\n", kind, offset, count, amount);
				error = "nd_sync_inconsistent";
				break;
			}
			offset += count;
			pthread_mutex_lock(&lock);
			if (current(job)) {
				g.library.stage = kind;
				g.library.fetched = offset;
			}
			pthread_mutex_unlock(&lock);
			notify();
			if (kind == ND_GENRES || count < amount) {
				nd_log("navidrome: sync stage kind=%d fetched=%d\n", kind, offset);
				break;
			}
		}
	}
	if (!error)
		error = scan_status(record, password, after);
	if (!error && strcmp(before, after)) {
		nd_log("navidrome: sync scan reason=marker_changed\n");
		error = "nd_sync_inconsistent";
	}
	bool leased = navidrome_cache_begin(job->local_epoch, job->card_epoch);
	if (!leased && !error)
		error = "nd_sync_cancelled";
	if (leased) {
		if (!nd_store_open(&store, result.path, result.scope, true, false)) {
			if (!error)
				error = "nd_cache_error";
		} else {
			store.generation = candidate;
			if (!error && !nd_store_validate(&store))
				error = store.error ? store.error : "nd_sync_inconsistent";
		}
	}
	bool valid = sync_current(job) && nd_cache_card_same(job->root, card.st_dev, card.st_ino);
	pthread_mutex_lock(&lock);
	valid = valid && current(job) && job->card_epoch == g.card_epoch && !g.sync_cancel;
	if (!error && !valid)
		error = "nd_sync_cancelled";
	// The small final transaction is serialized with invalidation. Network,
	// enumeration, relationship checks and page writes never hold this mutex.
	bool published = !error && nd_store_publish(&store);
	if (!error && !published)
		error = "nd_cache_error";
	if (published)
		g.local_epoch++;
	pthread_mutex_unlock(&lock);
	if (published && !nd_store_status(&store, &result))
		error = "nd_cache_error";
	if (store.db)
		nd_store_cleanup(&store);
	nd_store_close(&store);
	if (leased)
		navidrome_cache_end();
	pthread_mutex_lock(&lock);
	if (current(job)) {
		if (published && job->card_epoch == g.card_epoch)
			g.library = result;
		g.library.syncing = false;
		g.library.error = error;
		if (error && (!strcmp(error, "nd_auth_error") || !strcmp(error, "nd_network_error") || !strcmp(error, "nd_tls_error"))) {
			g.connected = false;
			g.error = error;
		}
		g.busy = false;
	}
	pthread_mutex_unlock(&lock);
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
	out->busy = g.busy;
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
	if (g.busy || g.stop || !g.initialized)
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
	while (cache_users)
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
	// Card events run on the storage worker. Leases only cover local file I/O,
	// never network waits, and no new old-card lease can enter after this epoch.
	while (cache_users)
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
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

bool navidrome_context_current(const nd_context_t *context, bool network) {
	pthread_mutex_lock(&lock);
	bool ok = g.unlocked && context->epoch == g.local_epoch && context->card_epoch == g.card_epoch && (!network || (g.connected && context->network_epoch == g.generation));
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

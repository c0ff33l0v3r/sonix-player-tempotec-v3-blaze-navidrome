#include "navidrome.h"
#include "src/gui/shell/gui.h"
#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/md5.h"
#include "src/system/net/http.h"
#include "src/system/net/wifi.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef enum { JOB_SAVE, JOB_UNLOCK, JOB_CHANGE, JOB_CONNECT, JOB_FORGET } job_kind_t;
typedef struct {
	job_kind_t kind;
	unsigned generation;
	char host[ND_HOST_MAX], user[ND_USER_MAX], password[ND_SECRET_MAX];
	char passphrase[ND_SECRET_MAX], replacement[ND_SECRET_MAX];
} job_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static struct {
	bool initialized, started, stop, pending, busy, configured, invalid, unlocked, checking, connected;
	unsigned generation, revision;
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
	char base[ND_HOST_MAX];
	const char *error = navidrome_validate(host, user, password, base);
	if (error)
		return error;
	uint8_t random[16];
	char salt[33], token[MD5_HEX_LEN], signed_text[ND_SECRET_MAX + 33], encoded[ND_USER_MAX * 3 + 1], url[1536];
	if (!nd_random(random, sizeof(random)))
		return "nd_crypto_error";
	for (size_t i = 0; i < sizeof(random); i++)
		snprintf(salt + 2 * i, 3, "%02x", random[i]);
	snprintf(signed_text, sizeof(signed_text), "%s%s", password, salt);
	md5_hex(signed_text, strlen(signed_text), token);
	nd_wipe(signed_text, sizeof(signed_text));
	http_url_encode(user, encoded, sizeof(encoded));
	int n = snprintf(url, sizeof(url), "%s/rest/ping.view?u=%s&t=%s&s=%s&v=1.16.1&c=SonixPlayer&f=json", base, encoded, token, salt);
	nd_wipe(token, sizeof(token));
	if (n < 0 || (size_t)n >= sizeof(url))
		return "nd_input_long";
	char *body = NULL;
	size_t size = 0;
	int status = 0;
	http_req_t request = {.want_error_body = true, .sensitive_url = true, .require_complete_body = true};
	fprintf(stderr, "navidrome: ping start scheme=%s timeout_seconds=8 body_limit=16384\n", !strncmp(base, "https://", 8) ? "https" : "http");
	bool ok = http_request(url, &request, &body, &size, 16384, 8, &status);
	bool diagnostic_error = http_last_error() != NULL;
	fprintf(stderr, "navidrome: ping response http_ok=%d status=%d bytes=%zu transport_diagnostic=%d\n", ok, status, size, diagnostic_error);
	http_clear_request_metadata();
	nd_wipe(url, sizeof(url));
	if (!ok) {
		if (body)
			nd_wipe(body, size);
		free(body);
		fprintf(stderr, "navidrome: ping rejected reason=%s\n", status >= 200 && status < 300 ? "incomplete_or_oversized_http_body" : "http_request_failed");
		if (status >= 200 && status < 300)
			return "nd_response_error";
		if (status >= 400)
			return "nd_api_error";
		return diagnostic_error && !strncmp(base, "https://", 8) ? "nd_tls_error" : "nd_network_error";
	}
	error = "nd_response_error";
	// Only classify response bytes. Never log server text, which may echo
	// credentials, query parameters, cookies, or a login form's hidden inputs.
	size_t first = 0;
	while (first < size && isspace((unsigned char)body[first]))
		first++;
	const char *kind = first == size ? "empty" : body[first] == '{' || body[first] == '[' ? "json" : body[first] == '<' ? "markup" : size - first >= 2 && (unsigned char)body[first] == 0x1f && (unsigned char)body[first + 1] == 0x8b ? "gzip" : "other";
	fprintf(stderr, "navidrome: ping body kind=%s\n", kind);
	const char *reason = !size ? "empty_body" : size >= 16384 ? "body_limit" : memchr(body, 0, size) ? "embedded_nul" : "malformed_json";
	json_doc_t doc;
	if (size && size < 16384 && !memchr(body, 0, size) && json_parse(body, &doc)) {
		int response = json_get(&doc, json_root(&doc), "subsonic-response");
		char result[32];
		int status_token = json_get(&doc, response, "status");
		bool valid_status = json_str(&doc, status_token, result, sizeof(result));
		reason = response < 0 ? "missing_subsonic_wrapper" : !valid_status ? "missing_or_invalid_status" : "unknown_status";
		if (valid_status) {
			// C strings cannot represent an embedded JSON NUL. In particular,
			// "ok\u0000..." must not authenticate as the exact status "ok".
			for (int i = doc.toks[status_token].start; i + 6 <= doc.toks[status_token].end; i++)
				if (!memcmp(doc.text + i, "\\u0000", 6))
					valid_status = false;
			if (!valid_status)
				reason = "status_contains_nul";
		}
		if (valid_status) {
			if (status >= 200 && status < 300 && !strcmp(result, "ok")) {
				error = NULL;
				reason = "subsonic_ok";
			} else if (!strcmp(result, "failed")) {
				int failure = json_get(&doc, response, "error");
				long code = json_obj_long(&doc, failure, "code", 0);
				error = code == 40 ? "nd_auth_error" : "nd_api_error";
				reason = "subsonic_failed";
				fprintf(stderr, "navidrome: ping API error_code=%ld\n", code);
			}
		}
		json_free(&doc);
	}
	fprintf(stderr, "navidrome: ping result=%s reason=%s\n", error ? error : "connected", reason);
	nd_wipe(body, size);
	free(body);
	return error;
}

static bool current(const job_t *job) { return !g.stop && g.generation == job->generation; }

// Expensive file writes/fsync do not hold the snapshot mutex. Only the rename
// and in-memory adoption serialize with Lock/Forget and generation changes.
static int commit(const job_t *job, const nd_record_t *r, const uint8_t key[32], const char *password) {
	if (!nd_vault_stage(g.path, r)) {
		fprintf(stderr, "navidrome: vault stage failed errno=%d\n", errno);
		return 0;
	}
	pthread_mutex_lock(&lock);
	bool applied = current(job) && nd_vault_commit(g.path);
	if (applied) {
		g.record = *r;
		memcpy(g.key, key, 32);
		snprintf(g.password, sizeof(g.password), "%s", password);
		g.configured = g.unlocked = true;
		g.invalid = false;
		g.revision++;
	}
	pthread_mutex_unlock(&lock);
	if (!applied) {
		fprintf(stderr, "navidrome: vault commit failed_or_cancelled generation=%u\n", job->generation);
		nd_vault_discard(g.path);
		return 0;
	}
	bool synced = nd_vault_sync(g.path);
	fprintf(stderr, "navidrome: vault committed generation=%u synced=%d\n", job->generation, synced);
	return synced ? 1 : 2;
}

static void *work(void *unused) {
	(void)unused;
	for (;;) {
		job_t job;
		nd_record_t r;
		uint8_t key[32];
		char password[ND_SECRET_MAX];
		pthread_mutex_lock(&lock);
		while (!g.pending && !g.stop)
			pthread_cond_wait(&ready, &lock);
		if (g.stop) {
			pthread_mutex_unlock(&lock);
			break;
		}
		job = g.job;
		nd_wipe(&g.job, sizeof(g.job));
		g.pending = false;
		r = g.record;
		memcpy(key, g.key, sizeof(key));
		memcpy(password, g.password, sizeof(password));
		bool configured = g.configured;
		pthread_mutex_unlock(&lock);
		static const char *operations[] = {"save", "unlock", "change_unlock_password", "reconnect", "forget"};
		fprintf(stderr, "navidrome: operation start kind=%s generation=%u configured=%d\n", operations[job.kind], job.generation, configured);
		const char *error = NULL;
		bool check = job.kind == JOB_SAVE || job.kind == JOB_UNLOCK || job.kind == JOB_CONNECT;
		if (job.kind == JOB_UNLOCK || job.kind == JOB_CHANGE) {
			// Re-read before authenticating: startup metadata is only a public
			// snapshot, and a file changed since startup must not bypass GCM.
			if (nd_vault_load(g.path, &r) != 1)
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
			if (!configured && !nd_vault_prepare(&r, job.replacement, key))
				error = "nd_crypto_error";
		}
		if (!error && job.kind == JOB_CHANGE && !nd_vault_prepare(&r, job.replacement, key))
			error = "nd_crypto_error";
		nd_wipe(job.replacement, sizeof(job.replacement));
		nd_wipe(job.password, sizeof(job.password));
		if (!error && (job.kind == JOB_SAVE || job.kind == JOB_CHANGE)) {
			if (!nd_vault_seal(&r, key, password))
				error = "nd_crypto_error";
			else {
				int result = commit(&job, &r, key, password);
				if (result != 1)
					error = result == 2 ? "nd_durability_error" : "nd_save_error";
			}
		}
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
		if (valid && !error && check) {
			notify();
			error = online() ? navidrome_ping(r.host, r.user, password) : "nd_wifi_error";
			// Recheck Wi-Fi even if the user left the menu during this request.
			if (!error && !online())
				error = "nd_wifi_error";
			if (!error && !r.authenticated) {
				r.authenticated = true;
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
			if (check)
				g.connected = !error || !strcmp(error, "nd_history_error");
		}
		pthread_mutex_unlock(&lock);
		fprintf(stderr, "navidrome: operation finish kind=%s generation=%u accepted=%d result=%s\n", operations[job.kind], job.generation, accepted, error ? error : "ok");
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
			if (g.invalid)
				g.error = "nd_vault_error";
		}
		fprintf(stderr, "navidrome: initialized storage_path_available=%d configured=%d invalid=%d result=%s\n", g.path[0] != 0, g.configured, g.invalid, g.error ? g.error : "ok");
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
	job->generation = ++g.generation;
	g.job = *job;
	g.pending = true;
	if (job->kind == JOB_CONNECT || job->kind == JOB_SAVE || job->kind == JOB_UNLOCK)
		g.connected = false;
	pthread_cond_signal(&ready);
	return true;
}

bool navidrome_save(const char *host, const char *user, const char *password, const char *passphrase) {
	job_t job = {.kind = JOB_SAVE};
	const char *error = navidrome_validate(host, user, password, job.host);
	pthread_mutex_lock(&lock);
	if (!error && ((!g.configured && !nd_passphrase_valid(passphrase)) || (g.configured && (!g.unlocked || g.invalid))))
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

bool navidrome_unlock(const char *passphrase) {
	job_t job = {.kind = JOB_UNLOCK};
	pthread_mutex_lock(&lock);
	bool ok = false;
	if (g.configured && !g.invalid && passphrase && *passphrase && strlen(passphrase) < ND_SECRET_MAX) {
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
	if (!g.unlocked)
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

bool navidrome_reconnect(void) {
	job_t job = {.kind = JOB_CONNECT};
	pthread_mutex_lock(&lock);
	bool ok = g.unlocked && !g.invalid && submit(&job);
	pthread_mutex_unlock(&lock);
	notify();
	return ok;
}

static void clear_secrets(void) {
	g.generation++;
	g.unlocked = g.connected = g.checking = g.busy = g.pending = false;
	nd_wipe(&g.job, sizeof(g.job));
	nd_wipe(g.key, sizeof(g.key));
	nd_wipe(g.password, sizeof(g.password));
	g.revision++;
}

void navidrome_lock(void) {
	pthread_mutex_lock(&lock);
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

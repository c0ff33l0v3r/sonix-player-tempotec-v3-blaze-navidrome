// Focused checks, independent of the full player build. Run with
// tools/navidrome_selftest.sh. All credentials/files are synthetic and temporary.
#define _GNU_SOURCE 1
#include "src/system/core/md5.h"
#include "src/system/net/http.h"
#include "src/system/streaming/navidrome.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char config_file[1024];
const char *config_path(void) { return config_file[0] ? config_file : NULL; }
// No LVGL objects are created. The service still crosses the real gui_post
// boundary; state assertions read mutex-protected snapshots instead of widgets.
bool gui_post(void (*cb)(void *), void *data) {
	(void)cb;
	(void)data;
	return true;
}

static atomic_int mode, calls, blocked, release_request;
static char previous_salt[33];
static const char *server_password = "synthetic-server-password";
static const char *executable;
static atomic_int fail_rename, fail_sync, fail_remove;
static atomic_int sync_blocked, release_sync;
static char remove_path[1024];

int __real_rename(const char *, const char *);
int __wrap_rename(const char *from, const char *to) {
	if (atomic_exchange(&fail_rename, 0)) {
		errno = EIO;
		return -1;
	}
	return __real_rename(from, to);
}
int __real_fsync(int);
int __wrap_fsync(int fd) {
	struct stat st;
	assert(fstat(fd, &st) == 0);
	int mode = atomic_load(&fail_sync);
	if (mode == 3) {
		atomic_store(&sync_blocked, 1);
		while (!atomic_load(&release_sync))
			usleep(1000);
		atomic_store(&fail_sync, 0);
	}
	if (mode == 1 || (mode == 2 && S_ISDIR(st.st_mode))) {
		atomic_store(&fail_sync, 0);
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}
int __real_unlink(const char *);
int __wrap_unlink(const char *path) {
	if (atomic_load(&fail_remove) && !strcmp(path, remove_path)) {
		atomic_store(&fail_remove, 0);
		errno = EACCES;
		return -1;
	}
	return __real_unlink(path);
}

const char *http_last_error(void) { return atomic_load(&mode) == 7 ? "synthetic TLS failure" : NULL; }
void http_clear_request_metadata(void) {}
bool http_file_open(http_stream_t *stream, const char *url, int timeout) {
	(void)stream;
	(void)url;
	(void)timeout;
	assert(!"service self-test must not request audio");
	return false;
}
char *http_url_encode(const char *in, char *out, size_t size) {
	static const char h[] = "0123456789ABCDEF";
	size_t n = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
		bool safe = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-._~", *p);
		assert(n + (safe ? 1 : 3) < size);
		if (safe)
			out[n++] = (char)*p;
		else {
			out[n++] = '%';
			out[n++] = h[*p >> 4];
			out[n++] = h[*p & 15];
		}
	}
	out[n] = 0;
	return out;
}

bool http_request(const char *url, const http_req_t *req, char **out, size_t *size, size_t limit, int timeout, int *status) {
	assert(req->extra_headers == NULL && req->want_error_body && req->sensitive_url && req->require_complete_body && limit == 16384 && timeout > 0);
	assert(strstr(url, "/base/rest/ping.view?u=fixture%20user&"));
	assert(strstr(url, "&v=1.16.1&c=SonixPlayer&f=json"));
	assert(!strstr(url, server_password));
	const char *s = strstr(url, "&s=");
	const char *t = strstr(url, "&t=");
	assert(s && t);
	char salt[33], token[33], joined[ND_SECRET_MAX + 33];
	memcpy(salt, s + 3, 32);
	salt[32] = 0;
	assert(strcmp(salt, previous_salt));
	memcpy(previous_salt, salt, sizeof(salt));
	snprintf(joined, sizeof(joined), "%s%s", server_password, salt);
	md5_hex(joined, strlen(joined), token);
	bool valid_password = !memcmp(t + 3, token, 32);
	nd_wipe(joined, sizeof(joined));
	atomic_fetch_add(&calls, 1);
	int fixture = atomic_load(&mode);
	if (fixture == 8) {
		atomic_store(&blocked, 1);
		while (!atomic_load(&release_request))
			usleep(1000);
	}
	if (fixture == 6 || fixture == 7)
		return false;
	const char *body = "{\"subsonic-response\":{\"status\":\"ok\"}}";
	if (!valid_password || fixture == 1)
		body = "{\"subsonic-response\":{\"status\":\"failed\",\"error\":{\"code\":40}}}";
	if (fixture == 2)
		body = "<html>login</html>";
	if (fixture == 3)
		body = "{\"unrelated\":true}";
	if (fixture == 4)
		body = "{\"subsonic-response\":";
	if (fixture == 9)
		body = "{\"subsonic-response\":{\"status\":\"ok\\u0000invalid\"}}";
	*status = 200;
	if (fixture == 5) {
		*out = malloc(limit + 1);
		memset(*out, ' ', limit);
		(*out)[limit] = 0;
		memcpy(*out, body, strlen(body));
		*size = limit;
	} else {
		*out = strdup(body);
		*size = strlen(body);
	}
	return true;
}

static navidrome_snapshot_t wait_idle(void) {
	navidrome_snapshot_t s;
	for (int i = 0; i < 10000; i++) {
		navidrome_snapshot(&s);
		if (!s.busy)
			return s;
		usleep(1000);
	}
	assert(!"worker did not finish");
	return s;
}

static void crypto_checks(void) {
	uint8_t key[32] = {0}, nonce[12] = {0}, plain[16] = {0}, cipher[16], tag[16], decoded[16];
	const uint8_t expected_cipher[] = {0xce, 0xa7, 0x40, 0x3d, 0x4d, 0x60, 0x6b, 0x6e, 0x07, 0x4e, 0xc5, 0xd3, 0xba, 0xf3, 0x9d, 0x18};
	const uint8_t expected_tag[] = {0xd0, 0xd1, 0xc8, 0xa7, 0x99, 0x99, 0x6b, 0xf0, 0x26, 0x5b, 0x98, 0xb5, 0xd4, 0x8a, 0xb9, 0x19};
	assert(nd_encrypt(key, nonce, "", 0, plain, 16, cipher, tag));
	assert(!memcmp(cipher, expected_cipher, 16) && !memcmp(tag, expected_tag, 16));
	assert(nd_decrypt(key, nonce, "", 0, cipher, 16, tag, decoded));
	assert(!memcmp(decoded, plain, 16));
	tag[0] ^= 1;
	memset(decoded, 0xff, 16);
	assert(!nd_decrypt(key, nonce, "", 0, cipher, 16, tag, decoded));
	assert(!memcmp(decoded, plain, 16));
	uint8_t salt[16];
	for (int i = 0; i < 16; i++)
		salt[i] = (uint8_t)i;
	const uint8_t derived[] = {0xe1, 0xb0, 0x8f, 0x92, 0xbe, 0x81, 0x74, 0xd9, 0xf4, 0x42, 0xd9, 0x5d, 0x89, 0xaa, 0x4c, 0xcd, 0xc3, 0x11, 0x23, 0x1a, 0x4d, 0x70, 0xd0, 0xb8, 0x54, 0xd1, 0x54, 0x8d, 0xe8, 0xfa, 0xbd, 0xfd};
	assert(nd_derive("password", salt, 1, key) && !memcmp(key, derived, 32));
	assert(!nd_passphrase_valid(NULL) && !nd_passphrase_valid(""));
	assert(!nd_passphrase_valid("short") && nd_passphrase_valid("vault1"));
	assert(!nd_passphrase_valid("あいうえお") && nd_passphrase_valid("あいうえおか"));
	assert(nd_passphrase_valid("twelve chars!"));
}

static void vault_checks(const char *dir) {
	char path[1024], tmp[1030], decoded[ND_SECRET_MAX];
	assert(nd_vault_path(NULL, path, sizeof(path)) && !strcmp(path, ND_VAULT_FILE));
	assert(nd_vault_path("", path, sizeof(path)) && !strcmp(path, ND_VAULT_FILE));
	assert(!nd_vault_path(NULL, path, 1));
	assert(nd_vault_path("/usr/data/device_config.ini", path, sizeof(path)) && !strcmp(path, "/usr/data/" ND_VAULT_FILE));
	assert(nd_vault_path("/device_config.ini", path, sizeof(path)) && !strcmp(path, "/" ND_VAULT_FILE));
	assert(nd_vault_path("device_config.ini", path, sizeof(path)) && !strcmp(path, ND_VAULT_FILE));
	snprintf(path, sizeof(path), "%s/vault.json", dir);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	nd_record_t r = {0}, loaded;
	uint8_t key[32], wrong[32];
	snprintf(r.host, sizeof(r.host), "http://fixture/base");
	snprintf(r.user, sizeof(r.user), "fixture user");
	assert(nd_vault_prepare(&r, "synthetic-vault-passphrase", key));
	nd_record_t fresh = {0};
	uint8_t fresh_key[32];
	assert(nd_vault_prepare(&fresh, "synthetic-vault-passphrase", fresh_key));
	assert(memcmp(fresh.salt, r.salt, 16));
	nd_wipe(fresh_key, sizeof(fresh_key));
	assert(nd_vault_seal(&r, key, " leading/trailing秘密 \n"));
	assert(nd_vault_save(path, &r) == 1);
	struct stat st;
	assert(!stat(path, &st) && (st.st_mode & 0777) == 0600);
	assert(nd_vault_load(path, &loaded) == 1 && nd_vault_open(&loaded, key, decoded));
	assert(!strcmp(decoded, " leading/trailing秘密 \n"));
	assert(nd_derive("wrong passphrase", r.salt, r.iterations, wrong));
	assert(!nd_vault_open(&loaded, wrong, decoded));
	loaded.host[7] ^= 1;
	assert(!nd_vault_open(&loaded, key, decoded));
	loaded = r;
	loaded.user[0] ^= 1;
	assert(!nd_vault_open(&loaded, key, decoded));
	loaded = r;
	loaded.authenticated = true;
	assert(!nd_vault_open(&loaded, key, decoded));
	loaded = r;
	loaded.cipher[0] ^= 1;
	assert(!nd_vault_open(&loaded, key, decoded));
	uint8_t old_nonce[12];
	memcpy(old_nonce, r.nonce, 12);
	assert(nd_vault_seal(&r, key, "new synthetic password") && memcmp(old_nonce, r.nonce, 12));
	assert(!mkdir(tmp, 0700)); // Own temp path cannot be created: preserve committed file.
	assert(nd_vault_save(path, &r) == 0);
	assert(!rmdir(tmp));
	atomic_store(&fail_rename, 1);
	assert(nd_vault_save(path, &r) == 0);
	atomic_store(&fail_sync, 1);
	assert(nd_vault_save(path, &r) == 0);
	assert(nd_vault_load(path, &loaded) == 1 && nd_vault_open(&loaded, key, decoded));
	assert(!strcmp(decoded, " leading/trailing秘密 \n"));
	FILE *f = fopen(path, "r");
	assert(f);
	char bytes[8192];
	size_t n = fread(bytes, 1, sizeof(bytes) - 1, f);
	bytes[n] = 0;
	fclose(f);
	assert(!strstr(bytes, "synthetic-vault-passphrase") && !strstr(bytes, "leading/trailing"));
	char *iterations = strstr(bytes, "600000");
	assert(iterations);
	memcpy(iterations, "999999", 6);
	f = fopen(path, "w");
	assert(f);
	fputs(bytes, f);
	fclose(f);
	assert(nd_vault_load(path, &loaded) == 1 && !nd_vault_open(&loaded, key, decoded));
	iterations = strstr(bytes, "999999");
	assert(iterations);
	memcpy(iterations, "9999999", 7);
	f = fopen(path, "w");
	assert(f);
	fputs(bytes, f);
	fclose(f);
	assert(nd_vault_load(path, &loaded) == -1);
	f = fopen(path, "w");
	assert(f);
	fputs("{\"version\":999}", f);
	fclose(f);
	assert(nd_vault_load(path, &loaded) == -1);
	assert(nd_vault_remove(path) && nd_vault_load(path, &loaded) == 0);
	assert(nd_vault_remove(path));
	memset(r.user, '"', ND_USER_MAX - 1);
	r.user[ND_USER_MAX - 1] = 0;
	assert(nd_vault_seal(&r, key, "quoted metadata round trip"));
	atomic_store(&fail_sync, 2);
	assert(nd_vault_save(path, &r) == 2);
	assert(nd_vault_load(path, &loaded) == 1 && !strcmp(loaded.user, r.user) && nd_vault_open(&loaded, key, decoded));
	assert(nd_vault_remove(path));
	// Legacy encrypted files keep the original associated-data contract.
	r.version = 1;
	assert(nd_vault_seal(&r, key, "legacy synthetic password"));
	assert(nd_vault_save(path, &r) == 1);
	assert(nd_vault_load(path, &loaded) == 1 && loaded.version == 1 && !loaded.plain);
	assert(nd_vault_open(&loaded, key, decoded) && !strcmp(decoded, "legacy synthetic password"));
	assert(nd_vault_plain(&r, " plain \\\"秘密\n"));
	assert(nd_vault_save(path, &r) == 1);
	assert(nd_vault_load(path, &loaded) == 1 && loaded.plain && loaded.version == 2);
	assert(nd_vault_open(&loaded, NULL, decoded) && !strcmp(decoded, " plain \\\"秘密\n"));
	assert(!stat(path, &st) && (st.st_mode & 0777) == 0600);
	const char *invalid[] = {"{\"version\":2,\"mode\":\"other\",\"host\":\"x\",\"username\":\"u\",\"authenticated\":false,\"password\":\"p\"}", "{\"version\":2,\"mode\":\"plain\",\"host\":\"x\",\"username\":\"u\",\"authenticated\":false,\"ciphertext\":\"aa\"}", "{\"version\":2,\"mode\":\"plain\",\"host\":\"x\",\"username\":\"u\",\"authenticated\":false,\"password\":\"p\",\"ciphertext\":\"aa\"}"};
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		f = fopen(path, "w");
		assert(f && fputs(invalid[i], f) >= 0 && !fclose(f));
		assert(nd_vault_load(path, &loaded) == -1);
	}
	assert(nd_vault_remove(path));
	nd_wipe(key, sizeof(key));
	nd_wipe(wrong, sizeof(wrong));
	nd_wipe(decoded, sizeof(decoded));
}

static void service_checks(const char *dir) {
	snprintf(config_file, sizeof(config_file), "%s/device_config.ini", dir);
	navidrome_init();
	navidrome_snapshot_t s;
	navidrome_snapshot(&s);
	assert(!s.configured && !s.busy);
	char base[ND_HOST_MAX], secret[ND_SECRET_MAX];
	assert(!navidrome_validate("http://fixture:4533/base/", "fixture user", server_password, base));
	assert(!strcmp(base, "http://fixture:4533/base"));
	const char *bad[] = {"fixture", "http://[::1]/base", "http://fixture:0", "http://fixture:1x", "http://a@b/", "http://a/?x=1", "http://a/#x", "file://a", "http://a/\n"};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		assert(navidrome_validate(bad[i], "u", "p", base));
	assert(!navidrome_save("http://fixture/base", "fixture user", server_password, "short"));
	navidrome_snapshot(&s);
	assert(!s.configured && !s.busy && !strcmp(s.error, "nd_passphrase_short") && atomic_load(&calls) == 0);
	atomic_store(&mode, 1);
	assert(navidrome_save("http://fixture/base", "fixture user", server_password, "synthetic-vault-passphrase"));
	s = wait_idle();
	assert(s.configured && s.unlocked && !s.authenticated && s.state == ND_DISCONNECTED);
	atomic_store(&mode, 0);
	assert(navidrome_reconnect());
	s = wait_idle();
	assert(s.configured && s.unlocked && s.authenticated && s.state == ND_CONNECTED);
	assert(navidrome_password(secret) && !strcmp(secret, server_password));
	nd_wipe(secret, sizeof(secret));
	atomic_store(&fail_rename, 1);
	assert(navidrome_save("http://fixture/base", "fixture user", "unsaved replacement", NULL));
	s = wait_idle();
	assert(s.error && !strcmp(s.error, "nd_save_error"));
	assert(navidrome_password(secret) && !strcmp(secret, server_password));
	atomic_store(&fail_sync, 3);
	assert(navidrome_save("http://fixture/base", "fixture user", "cancelled replacement", NULL));
	for (int i = 0; i < 5000 && !atomic_load(&sync_blocked); i++)
		usleep(1000);
	assert(atomic_load(&sync_blocked));
	assert(!navidrome_reconnect()); // No second request can overwrite active work.
	navidrome_snapshot(&s);
	assert(s.busy);
	navidrome_lock();
	navidrome_snapshot(&s);
	assert(s.state == ND_LOCKED);
	atomic_store(&release_sync, 1);
	assert(navidrome_unlock("synthetic-vault-passphrase"));
	s = wait_idle();
	assert(s.state == ND_CONNECTED);
	assert(navidrome_password(secret) && !strcmp(secret, server_password));
	for (int i = 1; i <= 7; i++) {
		atomic_store(&mode, i);
		assert(navidrome_reconnect());
		s = wait_idle();
		assert(s.state == ND_DISCONNECTED && s.error);
	}
	atomic_store(&mode, 9);
	assert(navidrome_reconnect());
	s = wait_idle();
	assert(s.state == ND_DISCONNECTED && !strcmp(s.error, "nd_response_error"));
	atomic_store(&mode, 0);
	atomic_store(&mode, 1);
	assert(navidrome_save("http://fixture/other/base", "fixture user", server_password, NULL));
	s = wait_idle();
	assert(!s.authenticated && s.state == ND_DISCONNECTED && !strcmp(s.host, "http://fixture/other/base"));
	atomic_store(&mode, 0);
	assert(navidrome_save("http://fixture/base", "fixture user", server_password, NULL));
	s = wait_idle();
	assert(s.authenticated && s.state == ND_CONNECTED);
	assert(navidrome_save("http://fixture/base", "fixture user", "wrong synthetic password", NULL));
	s = wait_idle();
	assert(s.configured && s.authenticated && s.state == ND_DISCONNECTED && !strcmp(s.error, "nd_auth_error"));
	assert(navidrome_password(secret) && !strcmp(secret, "wrong synthetic password"));
	assert(navidrome_save("http://fixture/base", "fixture user", server_password, NULL));
	s = wait_idle();
	assert(s.state == ND_CONNECTED);
	assert(!navidrome_change_passphrase("synthetic-vault-passphrase", "short"));
	navidrome_snapshot(&s);
	assert(s.unlocked && !s.busy && !strcmp(s.error, "nd_passphrase_short"));
	atomic_store(&fail_rename, 1);
	assert(navidrome_change_passphrase("synthetic-vault-passphrase", "unsaved-vault-passphrase"));
	s = wait_idle();
	assert(s.error && !strcmp(s.error, "nd_save_error") && s.unlocked && s.state == ND_CONNECTED);
	assert(navidrome_change_passphrase("synthetic-vault-passphrase", "vault2"));
	s = wait_idle();
	assert(s.unlocked && s.state == ND_CONNECTED && !s.error);
	pid_t child = fork();
	assert(child >= 0);
	if (!child) {
		execl(executable, executable, "restart", config_file, (char *)NULL);
		_exit(127);
	}
	int status;
	assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	navidrome_lock();
	navidrome_snapshot(&s);
	assert(s.state == ND_LOCKED && !navidrome_password(secret));
	char vault_path[1024];
	assert(nd_vault_path(config_file, vault_path, sizeof(vault_path)));
	nd_record_t original, tampered;
	assert(nd_vault_load(vault_path, &original) == 1);
	tampered = original;
	tampered.user[0] ^= 1;
	assert(nd_vault_save(vault_path, &tampered) == 1);
	int before_tamper = atomic_load(&calls);
	assert(navidrome_unlock("vault2"));
	s = wait_idle();
	assert(!s.unlocked && s.error && atomic_load(&calls) == before_tamper);
	assert(nd_vault_save(vault_path, &original) == 1);
	assert(navidrome_unlock("synthetic-vault-passphrase"));
	s = wait_idle();
	assert(!s.unlocked && s.error);
	assert(navidrome_unlock("vault2"));
	s = wait_idle();
	assert(s.state == ND_CONNECTED);
	assert(nd_vault_path(config_file, remove_path, sizeof(remove_path)));
	atomic_store(&fail_remove, 1);
	assert(navidrome_forget());
	s = wait_idle();
	assert(s.configured && !s.unlocked && s.error && !strcmp(s.error, "nd_remove_error"));
	assert(navidrome_unlock("vault2"));
	s = wait_idle();
	assert(s.state == ND_CONNECTED);
	navidrome_test_online(0);
	navidrome_poll_network();
	navidrome_snapshot(&s);
	assert(s.state == ND_DISCONNECTED);
	int requests = atomic_load(&calls);
	navidrome_test_online(1);
	navidrome_poll_network();
	usleep(20000);
	navidrome_snapshot(&s);
	assert(s.state == ND_DISCONNECTED && atomic_load(&calls) == requests);
	atomic_store(&mode, 8);
	assert(navidrome_reconnect());
	for (int i = 0; i < 5000 && !atomic_load(&blocked); i++)
		usleep(1000);
	assert(atomic_load(&blocked));
	navidrome_lock();
	assert(navidrome_forget());
	atomic_store(&release_request, 1);
	s = wait_idle();
	assert(!s.configured && !s.unlocked && s.state == ND_UNCONFIGURED);
	char path[1024];
	assert(nd_vault_path(config_file, path, sizeof(path)));
	assert(access(path, F_OK) && errno == ENOENT);
	atomic_store(&mode, 0);
	assert(navidrome_save("http://fixture/base", "fixture user", server_password, "vault1"));
	s = wait_idle();
	assert(s.configured && s.state == ND_CONNECTED);
	int before_conversion = atomic_load(&calls);
	unsigned offer_before;
	nd_library_t library;
	navidrome_library_snapshot(&library);
	offer_before = library.offer;
	atomic_store(&fail_rename, 1);
	assert(navidrome_unencrypt(NULL));
	s = wait_idle();
	assert(s.encrypted && s.unlocked && s.error && !strcmp(s.error, "nd_save_error"));
	assert(navidrome_unencrypt(NULL));
	s = wait_idle();
	assert(!s.encrypted && s.unlocked && s.authenticated && s.state == ND_CONNECTED);
	assert(atomic_load(&calls) == before_conversion);
	navidrome_library_snapshot(&library);
	assert(library.offer == offer_before);
	child = fork();
	assert(child >= 0);
	if (!child) {
		execl(executable, executable, "plain-restart", config_file, (char *)NULL);
		_exit(127);
	}
	assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	atomic_store(&fail_rename, 1);
	assert(navidrome_encrypt("vault1"));
	s = wait_idle();
	assert(!s.encrypted && s.unlocked && s.error);
	assert(navidrome_encrypt("vault1"));
	s = wait_idle();
	assert(s.encrypted && s.unlocked && s.state == ND_CONNECTED && atomic_load(&calls) == before_conversion);
	navidrome_lock();
	assert(navidrome_unencrypt("wrong unlock password"));
	s = wait_idle();
	assert(s.encrypted && !s.unlocked && s.error && !strcmp(s.error, "nd_unlock_error"));
	assert(navidrome_unencrypt("vault1"));
	s = wait_idle();
	assert(!s.encrypted && s.unlocked && s.state == ND_DISCONNECTED && atomic_load(&calls) == before_conversion);
	assert(navidrome_password(secret) && !strcmp(secret, server_password));
	atomic_store(&fail_remove, 1);
	assert(navidrome_forget());
	s = wait_idle();
	assert(s.configured && !s.encrypted && s.unlocked && s.error && !strcmp(s.error, "nd_remove_error"));
	atomic_store(&sync_blocked, 0);
	atomic_store(&release_sync, 0);
	atomic_store(&fail_sync, 3);
	assert(navidrome_encrypt("vault1"));
	for (int i = 0; i < 10000 && !atomic_load(&sync_blocked); i++)
		usleep(1000);
	assert(atomic_load(&sync_blocked));
	assert(navidrome_forget());
	atomic_store(&release_sync, 1);
	s = wait_idle();
	assert(!s.configured && !s.unlocked && access(path, F_OK));
	assert(navidrome_save("http://fixture/base", "fixture user", server_password, NULL));
	s = wait_idle();
	assert(s.configured && !s.encrypted && s.unlocked && s.state == ND_CONNECTED);
	navidrome_reset();
	navidrome_snapshot(&s);
	assert(!s.configured && !s.unlocked && access(path, F_OK));
	navidrome_shutdown();
	nd_wipe(secret, sizeof(secret));
}

int main(int argc, char **argv) {
	alarm(60); // A serialization regression must fail rather than hang forever.
	executable = argv[0];
	if (argc == 3 && !strcmp(argv[1], "plain-restart")) {
		snprintf(config_file, sizeof(config_file), "%s", argv[2]);
		navidrome_init();
		navidrome_snapshot_t s;
		navidrome_snapshot(&s);
		assert(s.configured && s.unlocked && !s.encrypted && s.authenticated && s.state == ND_DISCONNECTED);
		assert(atomic_load(&calls) == 0);
		char password[ND_SECRET_MAX];
		assert(navidrome_password(password) && !strcmp(password, server_password));
		nd_wipe(password, sizeof(password));
		navidrome_shutdown();
		return 0;
	}
	if (argc == 3 && (!strcmp(argv[1], "default-path") || !strcmp(argv[1], "default-restart"))) {
		assert(!chdir(argv[2]));
		assert(!config_path()); // A simulator launched without SONIX_CONFIG.
		navidrome_init();
		navidrome_snapshot_t s;
		navidrome_snapshot(&s);
		assert(!s.error && !s.unlocked);
		if (!strcmp(argv[1], "default-restart")) {
			assert(s.configured && s.state == ND_LOCKED && !atomic_load(&calls));
			assert(navidrome_unlock("vault1"));
		} else {
			assert(!s.configured);
			assert(navidrome_save("http://fixture/base", "fixture user", server_password, "vault1"));
		}
		s = wait_idle();
		assert(s.state == ND_CONNECTED && !s.error);
		if (!strcmp(argv[1], "default-path")) {
			pid_t child = fork();
			assert(child >= 0);
			if (!child) {
				execl(executable, executable, "default-restart", argv[2], (char *)NULL);
				_exit(127);
			}
			int status;
			assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
			assert(navidrome_forget());
			s = wait_idle();
			assert(!s.configured && access(ND_VAULT_FILE, F_OK));
		}
		navidrome_shutdown();
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "restart")) {
		snprintf(config_file, sizeof(config_file), "%s", argv[2]);
		navidrome_init();
		navidrome_snapshot_t s;
		navidrome_snapshot(&s);
		assert(s.configured && !s.unlocked && !s.busy && s.state == ND_LOCKED && atomic_load(&calls) == 0);
		assert(navidrome_unlock("vault2"));
		s = wait_idle();
		assert(s.state == ND_CONNECTED && s.authenticated);
		navidrome_shutdown();
		return 0;
	}
	char dir[] = "/tmp/sonix-navidrome-check-XXXXXX";
	assert(mkdtemp(dir));
	pid_t child = fork();
	assert(child >= 0);
	if (!child) {
		execl(executable, executable, "default-path", dir, (char *)NULL);
		_exit(127);
	}
	int status;
	assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	puts("simulator default storage/restart: passed");
	crypto_checks();
	puts("crypto vectors/authentication: passed");
	vault_checks(dir);
	puts("vault integrity/persistence failures: passed");
	service_checks(dir);
	puts("service protocol/state/late-result checks: passed");
	assert(!rmdir(dir));
	return 0;
}

#include "src/system/device/workactivity.h"
#include "src/system/net/tls.h"
#include "src/system/streaming/navidrome.h"
#include "src/system/streaming/navidromeapi.h"
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
enum { NORMAL, DNS_FAIL, DNS_STALL, CONNECT_STALL, TLS_STALL, TLS_FAIL, HEADER_STALL, BODY_STALL, POST_STALL };
static int phase;
static atomic_bool entered, alive = true, release_dns;
static atomic_int dns_active;
static bool body_phase, partial_post;
const char *tr(const char *key) { return key; }
const char *config_get(const char *section, const char *key, const char *fallback) {
	(void)section;
	(void)key;
	return fallback;
}
void nd_log(const char *format, ...) { (void)format; }
void nd_debug_line(const char *line) { (void)line; }
const char *navidrome_validate(const char *host, const char *user, const char *password, char out[ND_HOST_MAX]) {
	assert(*user && *password);
	snprintf(out, ND_HOST_MAX, "%s", host);
	return NULL;
}
int __real_getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
int __wrap_getaddrinfo(const char *host, const char *port, const struct addrinfo *hints, struct addrinfo **out) {
	if (phase == DNS_FAIL)
		return EAI_AGAIN;
	if (phase == DNS_STALL) {
		atomic_fetch_add(&dns_active, 1);
		atomic_store(&entered, true);
		while (!atomic_load(&release_dns))
			usleep(100);
		atomic_fetch_sub(&dns_active, 1);
		return EAI_AGAIN;
	}
	return __real_getaddrinfo(host, port, hints, out);
}
int __real_connect(int, const struct sockaddr *, socklen_t);
int __wrap_connect(int fd, const struct sockaddr *address, socklen_t size) {
	if (phase == CONNECT_STALL) {
		atomic_store(&entered, true);
		errno = EINPROGRESS;
		return -1;
	}
	return __real_connect(fd, address, size);
}
int __real_poll(struct pollfd *, nfds_t, int);
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
	if (phase == CONNECT_STALL || (phase == POST_STALL && partial_post)) {
		atomic_store(&entered, true);
		usleep(10000);
		return 0;
	}
	if (phase == HEADER_STALL || (phase == BODY_STALL && body_phase))
		atomic_store(&entered, true);
	return __real_poll(fds, count, timeout);
}
ssize_t __real_send(int, const void *, size_t, int);
ssize_t __wrap_send(int fd, const void *data, size_t size, int flags) {
	if (phase == POST_STALL && ((const char *)data)[0] != 'P') {
		if (!partial_post) {
			partial_post = true;
			return __real_send(fd, data, size > 4 ? 4 : size, flags);
		}
		errno = EAGAIN;
		return -1;
	}
	return __real_send(fd, data, size, flags);
}
tls_conn_t *__real_tls_client_wait(int, const char *, bool (*)(void *, bool), void *, char *, size_t);
tls_conn_t *__wrap_tls_client_wait(int fd, const char *host, bool (*wait)(void *, bool), void *user, char *error, size_t size) {
	if (phase == TLS_FAIL)
		return NULL;
	if (phase == TLS_STALL) {
		atomic_store(&entered, true);
		while (wait(user, false)) {
		}
		return NULL;
	}
	return __real_tls_client_wait(fd, host, wait, user, error, size);
}
static bool current(void *unused) {
	(void)unused;
	return atomic_load(&alive);
}
static void *cancel(void *unused) {
	(void)unused;
	while (!atomic_load(&entered))
		usleep(100);
	usleep(20000);
	atomic_store(&alive, false);
	return NULL;
}
static double seconds(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}
int main(int argc, char **argv) {
	assert(argc == 3);
	alarm(8);
	const char *mode = argv[1], *host = argv[2];
	if (strstr(mode, "dns"))
		phase = strstr(mode, "cancel") ? DNS_STALL : DNS_FAIL;
	else if (strstr(mode, "connect"))
		phase = CONNECT_STALL;
	else if (strstr(mode, "tls"))
		phase = strstr(mode, "cancel") ? TLS_STALL : TLS_FAIL;
	else if (strstr(mode, "header"))
		phase = HEADER_STALL;
	else if (strstr(mode, "body"))
		phase = BODY_STALL;
	else if (strstr(mode, "post-write"))
		phase = POST_STALL;
	bool cancelling = !strncmp(mode, "cancel-", 7);
	pthread_t thread;
	if (cancelling)
		assert(!pthread_create(&thread, NULL, cancel, NULL));
	double start = seconds();
	const char *error = NULL;
	if (phase == BODY_STALL || !strcmp(mode, "long-body")) {
		http_session_t *session = http_session_new(current, NULL);
		assert(session && http_session_audio(session, 1048576));
		assert(!nd_api_stream(host, "fixture user", "synthetic-password", "track", session));
		body_phase = true;
		char chunk[64];
		int n;
		size_t received = 0;
		while ((n = http_session_read(session, chunk, sizeof(chunk))) > 0)
			received += n;
		if (cancelling)
			assert(n < 0 && !strcmp(http_session_error(session), "cancelled"));
		else
			assert(n == 0 && received == 8 && seconds() - start > .3);
		http_session_free(session);
	} else if (phase == POST_STALL || !strncmp(mode, "post-", 5)) {
		nd_document_t doc;
		error = nd_api_playlist_current(host, "fixture user", "synthetic-password", "createPlaylist", NULL, "songId=synthetic", current, NULL, &doc);
		if (!error) {
			char method[16];
			json_obj_str(&doc.doc, doc.response, "method", method, sizeof(method));
			assert(!strcmp(method, !strcmp(mode, "post-303") || !strcmp(mode, "post-302") || !strcmp(mode, "post-301") ? "GET" : "POST"));
			nd_api_free(&doc);
		}
	} else {
		nd_document_t doc;
		error = nd_api_json_current(host, "fixture user", "synthetic-password", "ping", NULL, 16384, current, NULL, &doc);
		if (!error)
			nd_api_free(&doc);
	}
	if (cancelling) {
		pthread_join(thread, NULL);
		if (phase != BODY_STALL)
			assert(error && !strcmp(error, "nd_sync_cancelled"));
		assert(seconds() - start < .25);
		atomic_store(&release_dns, true);
		while (atomic_load(&dns_active))
			usleep(100);
	} else if (phase == DNS_FAIL)
		assert(error && !strcmp(error, "nd_network_error"));
	else if (phase == TLS_FAIL)
		assert(error && !strcmp(error, "nd_tls_error"));
	else if (!strcmp(mode, "deadline-header"))
		assert(error && !strcmp(error, "nd_network_error") && seconds() - start < 1);
	else
		assert(!error);
	assert(!work_activity_busy());
	printf("finite transport %s: passed (%.1f ms)\n", mode, (seconds() - start) * 1000);
}

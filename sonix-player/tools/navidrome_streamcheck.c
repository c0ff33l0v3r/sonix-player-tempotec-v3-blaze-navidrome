#include "src/system/device/workactivity.h"
#include "src/system/net/http.h"
#include "src/system/streaming/navidrome.h"
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
void nd_log(const char *format, ...) {
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}
const char *tr(const char *key) { return key; }
const char *config_get(const char *section, const char *key, const char *fallback) {
	(void)section;
	(void)key;
	return fallback;
}

static atomic_bool alive = true;
static bool current(void *unused) {
	(void)unused;
	return atomic_load(&alive);
}
static void *wake_reader(void *user) {
	usleep(100000);
	if (user)
		http_stream_wake(user);
	else
		atomic_store(&alive, false);
	return NULL;
}

int main(int argc, char **argv) {
	assert(argc == 3);
	http_stream_t stream = {.fd = -1};
	bool legacy = !strcmp(argv[2], "legacy");
	http_session_t *session = legacy ? NULL : http_session_new(current, NULL);
	if (!legacy)
		assert(session && http_session_audio(session, 1024 * 1024));
	const char *error = legacy ? (http_stream_open(&stream, argv[1], 8) ? NULL : "failed") : nd_api_stream(argv[1], "fixture user", "synthetic-server-password", "opaque:/track ?+", session);
	if (!strcmp(argv[2], "openfail")) {
		assert(error);
		http_session_free(session);
		assert(!work_activity_busy());
		return 0;
	}
	assert(!error);
	char buf[37];
	if (!strcmp(argv[2], "stall")) {
		pthread_t thread;
		assert(!pthread_create(&thread, NULL, wake_reader, legacy ? &stream : NULL));
		assert((legacy ? http_stream_read(&stream, buf, sizeof(buf)) : http_session_read(session, buf, sizeof(buf))) < 0);
		pthread_join(thread, NULL);
		if (legacy)
			http_stream_close(&stream);
		else
			http_session_free(session);
		assert(!work_activity_busy());
		return 0;
	}
	long received = 0;
	int n;
	while ((n = legacy ? http_stream_read(&stream, buf, sizeof(buf)) : http_session_read(session, buf, sizeof(buf))) > 0) {
		for (int i = 0; i < n; i++)
			assert((unsigned char)buf[i] == (unsigned char)((received + i) % 251));
		received += n;
	}
	if (!strcmp(argv[2], "cut"))
		assert(n == -1 && (legacy ? stream.failed : http_session_error(session) != NULL));
	else
		assert(n == 0 && received == 2048);
	if (legacy)
		http_stream_close(&stream);
	else
		http_session_free(session);
	assert(!work_activity_busy());
	assert(!*http_last_final_url() && !http_last_error());
	puts("protected finite stream: passed");
	return 0;
}

// Real HTTP transport checks driven by navidrome_networkcheck.py.
#include "src/system/net/http.h"
#include "src/system/core/logging.h"
#include "src/system/streaming/navidrome.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool gui_post(void (*cb)(void *), void *user) {
	(void)cb;
	(void)user;
	return true;
}
const char *tr(const char *key) { return key; }
void thread_be_background(const char *name) { (void)name; }

int main(int argc, char **argv) {
	assert(argc == 3);
	bool file_log = getenv("SONIX_LOG") != NULL;
	if (file_log)
		logging_init();
	if (!strcmp(argv[2], "legacy")) {
		char *body = NULL;
		size_t size = 0;
		assert(http_get(argv[1], &body, &size, 16384, 8));
		assert(size > 0 && size <= 16384);
		free(body);
		puts("legacy prefix default: passed");
		return 0;
	}
	const char *error = navidrome_ping(argv[1], "fixture user", "synthetic-server-password");
	assert(!strcmp(error ? error : "ok", argv[2]));
	assert(!*http_last_final_url() && !http_last_error());
	puts("real HTTP fixture: passed");
	if (file_log)
		logging_flush();
	return 0;
}

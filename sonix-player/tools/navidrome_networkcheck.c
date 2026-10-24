// Real HTTP transport checks driven by navidrome_networkcheck.py.
#include "src/system/core/logging.h"
#include "src/system/net/http.h"
#include "src/system/streaming/navidrome.h"
#include "src/system/streaming/navidromedebug.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
	if (getenv("ND_DEBUG_CARD")) {
		nd_debug_enable(true);
		nd_debug_card(getenv("ND_DEBUG_CARD"));
	}
	if (!strcmp(argv[2], "legacy")) {
		char *body = NULL;
		size_t size = 0;
		assert(http_get(argv[1], &body, &size, 16384, 8));
		assert(size > 0 && size <= 16384);
		free(body);
		puts("legacy prefix default: passed");
		return 0;
	}
	char password[ND_SECRET_MAX] = "synthetic-server-password";
	if (getenv("ND_DEBUG_CARD")) {
		nd_record_t record = {0};
		uint8_t key[32] = {0};
		if (getenv("ND_TEST_PLAIN"))
			assert(nd_vault_plain(&record, password));
		else {
			assert(nd_vault_prepare(&record, "fixture-unlock-password", key));
			assert(nd_vault_seal(&record, key, password));
		}
		assert(nd_vault_open(&record, key, password));
		nd_wipe(&record, sizeof(record));
		nd_wipe(key, sizeof(key));
	}
	const char *error = navidrome_ping(argv[1], "fixture user", password);
	nd_wipe(password, sizeof(password));
	assert(!strcmp(error ? error : "ok", argv[2]));
	assert(!*http_last_final_url() && !http_last_error());
	if (getenv("ND_DEBUG_CARD")) {
		// A simultaneous shared-client caller has no Navidrome subscription.
		const char *path = strchr(strstr(argv[1], "://") + 3, '/');
		char url[512], *body = NULL;
		snprintf(url, sizeof(url), "%.*s/legacy/rest/ping.view", (int)(path - argv[1]), argv[1]);
		http_req_t request = {.sensitive_url = true};
		assert(http_request(url, &request, &body, NULL, 16384, 8, NULL));
		free(body);
		http_clear_request_metadata();
	}
	puts("real HTTP fixture: passed");
	if (file_log)
		logging_flush();
	nd_debug_shutdown();
	return 0;
}

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "src/system/library/filepublication.h"
#include "src/system/net/webcgi.h"
#include "src/system/net/wifitransfer.c"
#include <assert.h>
#include <fcntl.h>

// Only page translation is stubbed. No server, mount, audio or power action is
// executed. The production document-root builder and CGI handlers are linked.
const char *tr(const char *key) { return key; }
const char *lang_current(void) { return "en"; }
int main(int argc, char **argv) {
	if (webcgi_should_run(argc, argv))
		return webcgi_main();
	assert(argc >= 2);
	if (!strcmp(argv[1], "root")) {
		assert(argc == 3 && fp_card_set_root(argv[2]));
		printf("%llu\n", (unsigned long long)fp_card_epoch());
	} else if (!strcmp(argv[1], "enable")) {
		assert(fp_transfer_set_enabled(true));
	} else if (!strcmp(argv[1], "disable")) {
		assert(fp_transfer_set_enabled(false));
		assert(fp_card_drain());
	} else if (!strcmp(argv[1], "pending")) {
		assert(argc == 3);
		int directory = open(argv[2], O_RDONLY | O_DIRECTORY);
		assert(directory >= 0);
		fp_guard_t g;
		assert(fp_guard_open(&g, directory, true, true));
		fp_record_t r = {.operation = "123456789abcdef0123456789abcdef0", .part = ".nd-1-0123456789abcdef.flac", .final = "pending.flac", .bytes = 1024, .source_device = 1, .source_inode = 2};
		memset(r.digest, 'a', 64);
		assert(fp_guard_begin(&g, &r));
		fp_guard_close(&g);
		close(directory);
	} else if (!strcmp(argv[1], "webroot")) {
		const char *root = prepare_web_root();
		assert(root);
		char config[128];
		snprintf(config, sizeof(config), "%s/test.conf", root);
		assert(write_config(root, 4399, config, "/tmp/not-used.pid"));
		puts(root);
	} else {
		assert(!strcmp(argv[1], "cleanup") && argc == 3 && !strncmp(argv[2], "/tmp/sonix-web-", 15));
		assert(strlen(argv[2]) < sizeof(prepared_root));
		strcpy(prepared_root, argv[2]);
		discard_web_root();
	}
	return 0;
}

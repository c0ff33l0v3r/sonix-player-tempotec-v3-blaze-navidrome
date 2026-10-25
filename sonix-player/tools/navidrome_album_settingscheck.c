#include "src/gui/streaming/navidromealbum.h"
#include "src/system/core/config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
	char dir[] = "/tmp/sonix-album-settings-XXXXXX";
	assert(mkdtemp(dir));
	char path[256];
	snprintf(path, sizeof(path), "%s/settings.ini", dir);
	config_init(path); // A missing first-run file uses defaults.
	assert(navidromealbum_enabled());
	config_set_bool("library", "album_head", false);
	unsigned revision = navidromealbum_revision();
	navidromealbum_set_enabled(false);
	assert(!navidromealbum_enabled() && navidromealbum_revision() != revision);
	assert(!config_get_bool("library", "album_head", true));
	assert(config_init(path));
	assert(!navidromealbum_enabled());
	config_set_bool("library", "album_head", true);
	navidromealbum_set_enabled(true);
	assert(config_get_bool("library", "album_head", false));
	assert(config_init(path) && navidromealbum_enabled());
	assert(unlink(path) == 0 && rmdir(dir) == 0);
	puts("album preference: default-on, persisted off/on and independent Music setting: passed");
}

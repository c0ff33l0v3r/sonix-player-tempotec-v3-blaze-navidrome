// Real service/vault cleanup, with only synthetic transport and selected cards.
#define main unused_service_fixture_main
#include "navidrome_selftest.c"
#undef main
#include "src/system/streaming/navidromelibrary.h"
static atomic_bool retire_entered, release_retire, card_done;
static bool stall_retire;
bool __real_nd_cache_retire(const char *, const char *);
bool __wrap_nd_cache_retire(const char *root, const char *vault) {
	if (stall_retire && root) {
		atomic_store(&retire_entered, true);
		while (!atomic_load(&release_retire))
			usleep(100);
	}
	return __real_nd_cache_retire(root, vault);
}
static void *replace_card(void *root) {
	navidrome_set_root(root);
	atomic_store(&card_done, true);
	return NULL;
}
static void settled(void) {
	for (int i = 0;; i++) {
		nd_library_t lib;
		navidrome_library_snapshot(&lib);
		if (!lib.loading && !work_activity_busy())
			return;
		assert(i < 20000);
		usleep(100);
	}
}
static void sentinel(const char *root, char path[1400]) {
	const char *parts[] = {".local", ".local/navidrome", ".local/navidrome/.cache"};
	for (int i = 0; i < 3; i++) {
		snprintf(path, 1400, "%s/%s", root, parts[i]);
		assert(!mkdir(path, 0700) || errno == EEXIST);
	}
	snprintf(path, 1400, "%s/.local/navidrome/.cache/owned.fixture", root);
	FILE *file = fopen(path, "w");
	assert(file);
	fputs("synthetic", file);
	fclose(file);
}
int main(int argc, char **argv) {
	assert(argc == 3);
	alarm(15);
	bool after = !strcmp(argv[1], "after");
	snprintf(config_file, sizeof(config_file), "%s/config.ini", argv[2]);
	navidrome_init();
	assert(navidrome_save("http://fixture/base", "fixture user", server_password, NULL));
	assert(wait_idle().state == ND_CONNECTED);
	settled();
	char oldroot[1200], newroot[1200], oldfile[1400], newfile[1400];
	snprintf(oldroot, sizeof(oldroot), "%s/old", argv[2]);
	assert(!mkdir(oldroot, 0700));
	snprintf(newroot, sizeof(newroot), "%s/new", argv[2]);
	assert(!mkdir(newroot, 0700));
	navidrome_set_root(oldroot);
	settled();
	sentinel(oldroot, oldfile);
	sentinel(newroot, newfile);
	nd_library_t old;
	navidrome_library_snapshot(&old);
	if (after)
		stall_retire = true;
	else
		atomic_store(&fail_sync, 3);
	assert(navidrome_forget());
	for (int i = 0; !(after ? atomic_load(&retire_entered) : atomic_load(&sync_blocked)); i++) {
		assert(i < 10000);
		usleep(100);
	}
	pthread_t card;
	assert(!pthread_create(&card, NULL, replace_card, newroot));
	for (int i = 0;; i++) {
		nd_library_t now;
		navidrome_library_snapshot(&now);
		if (now.card_epoch != old.card_epoch)
			break;
		assert(i < 10000);
		usleep(100);
	}
	navidrome_snapshot_t snapshot;
	navidrome_snapshot(&snapshot);
	assert(!snapshot.unlocked && !access(newfile, F_OK));
	if (after)
		assert(!atomic_load(&card_done));
	else {
		pthread_join(card, NULL);
		assert(atomic_load(&card_done));
	}
	atomic_store(&release_retire, true);
	atomic_store(&release_sync, 1);
	if (after)
		pthread_join(card, NULL);
	wait_idle();
	settled();
	assert(!access(newfile, F_OK));
	assert((access(oldfile, F_OK) != 0) == after);
	navidrome_shutdown();
	assert(!work_activity_busy());
	puts(after ? "Forget after cleanup admission: GUI reads remain responsive, card replacement drains old cleanup, replacement data survives." : "Forget before cleanup admission: card replacement revokes old admission; neither card is accidentally deleted.");
	return 0;
}

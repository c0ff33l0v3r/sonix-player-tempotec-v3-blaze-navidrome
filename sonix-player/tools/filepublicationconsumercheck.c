#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "src/gui/library/filespage.c"
#include <assert.h>

// This fixture targets production listing and file mutations, not junk-name
// policy or LVGL painting. Unused GUI functions are discarded by the linker.
bool playlist_is_junk_name(const char *name) {
	(void)name;
	return false;
}

static void create_file(const char *path, const char *bytes) {
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(fd >= 0 && write(fd, bytes, strlen(bytes)) == (ssize_t)strlen(bytes));
	assert(!close(fd));
}
static bool listed(const folder_job_t *job, const char *name) {
	for (int i = 0; i < job->count; i++)
		if (!strcmp(job->entries[i].name, name))
			return true;
	return false;
}
int main(int argc, char **argv) {
	assert(argc == 2);
	char album[640], pending[640], ordinary[640], copied[640], moved[640];
	assert(snprintf(album, sizeof(album), "%s/album", argv[1]) < (int)sizeof(album));
	assert(!mkdir(album, 0700));
	snprintf(pending, sizeof(pending), "%s/album/pending.flac", argv[1]);
	snprintf(ordinary, sizeof(ordinary), "%s/album/ordinary.flac", argv[1]);
	snprintf(copied, sizeof(copied), "%s/album/copied.flac", argv[1]);
	snprintf(moved, sizeof(moved), "%s/moved", argv[1]);
	create_file(pending, "partial");
	create_file(ordinary, "ordinary complete file");
	int directory = open(album, O_RDONLY | O_DIRECTORY);
	assert(directory >= 0);
	fp_guard_t g;
	assert(fp_guard_open(&g, directory, true, true));
	fp_record_t r = {.operation = "123456789abcdef0123456789abcdef0", .part = ".nd-1-123456789abcdef0.flac", .final = "pending.flac", .bytes = 42, .source_device = 1, .source_inode = 2};
	memset(r.digest, 'a', 64);
	assert(fp_guard_begin(&g, &r));
	fp_guard_close(&g);
	folder_job_t *job = calloc(1, sizeof(*job));
	assert(job);
	snprintf(job->path, sizeof(job->path), "%s", album);
	read_folder(job);
	assert(!job->failed && listed(job, "ordinary.flac") && !listed(job, "pending.flac") && !listed(job, FP_JOURNAL));
	folder_free(job);
	assert(!copy_file(pending, copied));
	assert(!copy_file(ordinary, pending));
	assert(!guarded_rename(album, moved, true));
	assert(!remove_tree(album));
	assert(!access(pending, F_OK) && !access(ordinary, F_OK));
	assert(copy_file(ordinary, copied));
	assert(remove_tree(copied));
	assert(guarded_rename(ordinary, copied, true));
	puts("PASS production file-manager listing and copy/move/delete reject pending files while unrelated files remain usable");
	assert(fp_guard_open(&g, directory, true, false));
	assert(sqlite3_exec(g.db, "PRAGMA user_version=99", NULL, NULL, NULL) == SQLITE_OK);
	fp_guard_close(&g);
	job = calloc(1, sizeof(*job));
	assert(job);
	snprintf(job->path, sizeof(job->path), "%s", album);
	read_folder(job);
	assert(job->failed && !job->count);
	folder_free(job);
	assert(!remove_tree(album));
	assert(!close(directory));
	puts("PASS production file manager preserves unknown journal and affected directory");
	return 0;
}

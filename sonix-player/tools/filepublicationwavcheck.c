#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "src/system/audio/audio.c"
#include <assert.h>

int main(int argc, char **argv) {
	assert(argc == 2);
	char path[1024];
	snprintf(path, sizeof(path), "%s/track.wav", argv[1]);
	unsigned char wav[48] = {'R', 'I', 'F', 'F', 40, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0, 0x44, 0xac, 0, 0, 0x10, 0xb1, 2, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 4, 0, 0, 0, 0, 0, 0, 0};
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(fd >= 0 && write(fd, wav, sizeof(wav)) == sizeof(wav) && !close(fd));
	wav_info_t info;
	FILE *file = NULL;
	assert(parse_wav(path, &info, &file) == WAV_OK && file);
	assert(!fclose(file));
	int dir = open(argv[1], O_RDONLY | O_DIRECTORY);
	assert(dir >= 0);
	fp_guard_t g;
	assert(fp_guard_open(&g, dir, true, true));
	fp_record_t r = {.operation = "123456789abcdef0123456789abcdef0", .part = ".nd-1-123456789abcdef0.wav", .final = "track.wav", .bytes = sizeof(wav), .source_device = 1, .source_inode = 2};
	memset(r.digest, 'a', 64);
	assert(fp_guard_begin(&g, &r));
	fp_guard_close(&g);
	file = NULL;
	assert(parse_wav(path, &info, &file) == WAV_BAD && !file);
	assert(fp_guard_open(&g, dir, true, false));
	assert(fp_guard_owned(&g, r.operation, 1, 3));
	assert(fp_guard_complete(&g, r.operation));
	fp_guard_close(&g);
	assert(parse_wav(path, &info, &file) == WAV_OK && file);
	assert(!fclose(file) && !close(dir));
	puts("PASS production native WAV admission rejects an otherwise playable pending path and admits completed local audio");
	return 0;
}

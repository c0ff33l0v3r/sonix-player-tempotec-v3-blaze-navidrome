// Bounded gzip wrapper around the player's one shared raw DEFLATE inflater.
#include "gzip.h"
#include "src/gb/miniz/miniz.h"
#include "src/system/image/miniz/miniz_tinfl.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct gzip_reader {
	tinfl_decompressor inflater;
	unsigned char input[8192], dictionary[32768];
	size_t pos, end, output, available;
	uint32_t crc, size;
	int (*read)(void *, void *, size_t);
	void *user;
	bool member, finished, end_deflate, failed, eof;
};
static int fill(gzip_reader_t *g) {
	if (g->pos < g->end)
		return 1;
	int n = g->read(g->user, g->input, sizeof(g->input));
	if (n < 0) {
		g->failed = true;
		return -1;
	}
	g->pos = 0;
	g->end = n;
	g->eof = n == 0;
	return n ? 1 : 0;
}
static int byte(gzip_reader_t *g) { return fill(g) == 1 ? g->input[g->pos++] : -1; }
static bool header(gzip_reader_t *g) {
	int first = byte(g);
	if (first < 0) {
		if (g->member && !g->failed) {
			g->finished = true;
			return true;
		}
		return false;
	}
	unsigned char h[10];
	h[0] = first;
	for (size_t i = 1; i < sizeof(h); i++) {
		int c = byte(g);
		if (c < 0)
			return false;
		h[i] = c;
	}
	if (h[0] != 31 || h[1] != 139 || h[2] != 8 || (h[3] & 0xe0))
		return false;
	uint32_t crc = mz_crc32(0, h, sizeof(h));
	unsigned used = 10;
	if (h[3] & 4) {
		unsigned char length[2];
		for (int i = 0; i < 2; i++) {
			int c = byte(g);
			if (c < 0)
				return false;
			length[i] = c;
		}
		crc = mz_crc32(crc, length, 2);
		unsigned n = length[0] | ((unsigned)length[1] << 8);
		if (n > 8192 - used - 2)
			return false;
		used += n + 2;
		while (n--) {
			int c = byte(g);
			if (c < 0)
				return false;
			unsigned char b = c;
			crc = mz_crc32(crc, &b, 1);
		}
	}
	for (int flag = 8; flag <= 16; flag *= 2) {
		if (!(h[3] & flag))
			continue;
		int c;
		do {
			if (++used > 8192 || (c = byte(g)) < 0)
				return false;
			unsigned char b = c;
			crc = mz_crc32(crc, &b, 1);
		} while (c);
	}
	if (h[3] & 2) {
		int a = byte(g), b = byte(g);
		if (a < 0 || b < 0 || (unsigned)(a | (b << 8)) != (crc & 0xffff))
			return false;
	}
	tinfl_init(&g->inflater);
	g->crc = g->size = 0;
	g->output = g->available = 0;
	g->end_deflate = false;
	g->member = true;
	return true;
}
gzip_reader_t *gzip_reader_new(int (*read)(void *, void *, size_t), void *user) {
	gzip_reader_t *g = calloc(1, sizeof(*g));
	if (g) {
		g->read = read;
		g->user = user;
	}
	return g;
}
size_t gzip_reader_bytes(void) { return sizeof(gzip_reader_t); }
void gzip_reader_free(gzip_reader_t *g) { free(g); }
int gzip_reader_read(gzip_reader_t *g, void *out, size_t size) {
	if (g->failed)
		return -1;
	for (;;) {
		if (g->available) {
			size_t n = g->available < size ? g->available : size;
			memcpy(out, g->dictionary + g->output, n);
			g->output += n;
			g->available -= n;
			if (g->output == sizeof(g->dictionary))
				g->output = 0;
			return n;
		}
		if (g->finished)
			return 0;
		if (!g->member || g->end_deflate) {
			if (g->end_deflate) {
				uint32_t trailer[2] = {0};
				for (int i = 0; i < 8; i++) {
					int c = byte(g);
					if (c < 0)
						goto bad;
					trailer[i / 4] |= (uint32_t)c << ((i % 4) * 8);
				}
				if (trailer[0] != g->crc || trailer[1] != g->size)
					goto bad;
			}
			if (!header(g))
				goto bad;
			if (g->finished)
				return 0;
		}
		if (fill(g) < 0)
			goto bad;
		size_t input = g->end - g->pos, produced = sizeof(g->dictionary) - g->output;
		tinfl_status status = tinfl_decompress(&g->inflater, g->input + g->pos, &input, g->dictionary, g->dictionary + g->output, &produced, g->eof ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
		g->pos += input;
		if (status < 0 || (!input && !produced && g->eof))
			goto bad;
		g->crc = mz_crc32(g->crc, g->dictionary + g->output, produced);
		g->size += produced;
		g->available = produced;
		g->end_deflate = status == TINFL_STATUS_DONE;
	}
bad:
	g->failed = true;
	return -1;
}

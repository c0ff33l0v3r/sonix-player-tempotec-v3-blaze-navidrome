// Focused artwork tests use the real stb decoder with a small cover wrapper,
// avoiding the unrelated local-file metadata and player cover pipeline.
#include "src/gui/nowplaying/cover.h"
#include "src/system/image/stb_image_decl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool cover_load_image_memory(const void *data, size_t size, int width, int height, cover_fit_t fit, cover_image_t *out) {
	(void)width;
	(void)height;
	(void)fit;
	int w, h, channels;
	unsigned char *pixels = stbi_load_from_memory(data, (int)size, &w, &h, &channels, 4);
	if (!pixels)
		return false;
	memset(out, 0, sizeof(*out));
	out->pixels = pixels;
	out->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	out->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
	out->dsc.header.w = w;
	out->dsc.header.h = h;
	out->dsc.header.stride = w * 4;
	out->dsc.data = pixels;
	out->dsc.data_size = w * h * 4;
	return true;
}

bool cover_load_image_file(const char *path, int width, int height, cover_fit_t fit, cover_image_t *out) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	unsigned char *data = malloc(256 * 1024);
	if (!data) {
		fclose(f);
		return false;
	}
	size_t length = fread(data, 1, 256 * 1024, f);
	bool ok = !ferror(f) && cover_load_image_memory(data, length, width, height, fit, out);
	fclose(f);
	free(data);
	return ok;
}

void cover_free(cover_image_t *image) {
	stbi_image_free(image->pixels);
	memset(image, 0, sizeof(*image));
}

// This fixture validates cache decoding/ownership, not blur rendering.
bool cover_blur_fill(const cover_image_t *src, int width, int height, cover_image_t *out) {
	(void)src;
	(void)width;
	(void)height;
	memset(out, 0, sizeof(*out));
	return false;
}

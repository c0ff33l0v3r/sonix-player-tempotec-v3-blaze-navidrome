#include "screensaverpics.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "src/system/library/albumart.h"
#include "src/system/library/jpeginfo.h"

#define IMAGES_DIR_NAME "Screensaver"

// How many times a pick may land on a file that turns out to be unusable
// before it gives up. Names are sampled without opening anything, so an
// unlucky draw costs one header read and not a folder walk.
#define PICK_TRIES 8

static char card_root[512];

void screensaverpics_set_root(const char *root) {
	snprintf(card_root, sizeof(card_root), "%s", root ? root : "");
}

const char *screensaverpics_dir(void) {
	static char dir[512];
	if (!card_root[0] || (size_t)snprintf(dir, sizeof(dir), "%s/%s", card_root, IMAGES_DIR_NAME) >= sizeof(dir)) {
		dir[0] = '\0';
	}
	return dir;
}

// .jpg, .jpeg or .png, either case.
static bool has_image_extension(const char *name) {
	const char *dot = strrchr(name, '.');
	return dot && (strcasecmp(dot, ".png") == 0 || jpeg_info_has_extension(name));
}

// A picture beyond this on either side is a file that was not meant for a
// screen. Under it the scaled decoders take any size: a JPEG comes out at a
// half, quarter or eighth of its own and a PNG is shrunk scanline by scanline.
#define MAX_SIDE 16384

// PNG's size from its IHDR: the signature, then the first chunk, whose first
// eight bytes are width and height, big-endian.
static bool png_size(const char *path, int *w, int *h) {
	unsigned char head[24];
	FILE *f = fopen(path, "rb");
	size_t got = f ? fread(head, 1, sizeof(head), f) : 0;
	if (f) {
		fclose(f);
	}
	if (got != sizeof(head) || memcmp(head, "\x89PNG\r\n\x1a\n", 8) != 0) {
		return false;
	}
	*w = (head[16] << 24) | (head[17] << 16) | (head[18] << 8) | head[19];
	*h = (head[20] << 24) | (head[21] << 16) | (head[22] << 8) | head[23];
	return true;
}

bool screensaverpics_usable(const char *dir, const char *name) {
	if (!dir || !dir[0] || !has_image_extension(name)) {
		return false;
	}
	char path[PATH_MAX];
	if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, name) >= sizeof(path)) {
		return false;
	}
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > ALBUMART_MAX_BYTES) {
		return false;
	}
	int w, h;
	jpeg_info_t info;
	if (jpeg_info_read(path, &info)) {
		w = info.width;
		h = info.height;
	} else if (!png_size(path, &w, &h)) {
		return false;
	}
	return w > 0 && h > 0 && w <= MAX_SIDE && h <= MAX_SIDE;
}

bool screensaverpics_any(void) {
	const char *dir = screensaverpics_dir();
	DIR *d = dir[0] ? opendir(dir) : NULL;
	if (!d) {
		return false;
	}
	bool found = false;
	struct dirent *e;
	while (!found && (e = readdir(d)) != NULL) {
		found = screensaverpics_usable(dir, e->d_name);
	}
	closedir(d);
	return found;
}

// The random sequence only has to vary, not to be unpredictable, so the clock
// is seed enough. Seeded here as well as in the shuffle so neither depends on
// the other having run.
static void seed_once(void) {
	static bool seeded;
	if (!seeded) {
		seeded = true;
		srand((unsigned)time(NULL));
	}
}

// One name out of the folder, uniformly, in a single pass and without keeping
// the list: the nth candidate replaces the one held with probability 1/n. A
// folder of a thousand wallpapers costs one readdir and one string copy.
//
// `avoid` and the `skip` list are left out of the draw. Leaving out what has
// already been tried is what makes the retry below make progress: without it a
// folder half full of rejects can draw the same reject eight times and come
// away with nothing while good pictures sat there the whole time.
static bool sample_name(const char *dir, const char *avoid, char skip[][NAME_MAX + 1], int skip_count, char *out,
						size_t out_size) {
	DIR *d = opendir(dir);
	if (!d) {
		return false;
	}
	seed_once();
	int seen = 0;
	out[0] = '\0';
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (!has_image_extension(e->d_name)) {
			continue;
		}
		if (avoid && avoid[0] && strcmp(e->d_name, avoid) == 0) {
			continue;
		}
		bool skipped = false;
		for (int i = 0; i < skip_count && !skipped; i++) {
			skipped = strcmp(skip[i], e->d_name) == 0;
		}
		if (skipped) {
			continue;
		}
		seen++;
		if (rand() % seen == 0) {
			snprintf(out, out_size, "%s", e->d_name);
		}
	}
	closedir(d);
	return out[0] != '\0';
}

// dir + "/" + name, refused rather than truncated: a long mount point and a
// long file name together might not fit, and a cut path is a file that does
// not exist.
static bool join_path(char *out, size_t out_size, const char *dir, const char *name) {
	if ((size_t)snprintf(out, out_size, "%s/%s", dir, name) >= out_size) {
		out[0] = '\0';
		return false;
	}
	return true;
}

bool screensaverpics_pick(const char *current, char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return false;
	}
	out[0] = '\0';

	const char *dir = screensaverpics_dir();
	if (!dir[0]) {
		return false;
	}

	// `current` may be a whole path: what matters is the name inside the
	// folder, which is what readdir hands back.
	const char *shown = NULL;
	if (current && current[0]) {
		const char *slash = strrchr(current, '/');
		shown = slash ? slash + 1 : current;
	}

	char tried[PICK_TRIES][NAME_MAX + 1];
	int tries = 0;
	while (tries < PICK_TRIES) {
		char name[NAME_MAX + 1];
		if (!sample_name(dir, shown, tried, tries, name, sizeof(name))) {
			break; // nothing left in the folder that has not been tried
		}
		snprintf(tried[tries], sizeof(tried[tries]), "%s", name);
		tries++;

		if (screensaverpics_usable(dir, name) && join_path(out, out_size, dir, name)) {
			return true;
		}
	}

	// Nothing else would do. The picture already up is the last resort rather
	// than the first: with one picture in the folder "a different one" is a
	// request that cannot be met, and seeing it again beats seeing black.
	if (shown && screensaverpics_usable(dir, shown) && join_path(out, out_size, dir, shown)) {
		return true;
	}

	out[0] = '\0';
	return false;
}

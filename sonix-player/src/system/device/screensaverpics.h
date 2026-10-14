#ifndef SCREENSAVERPICS_H
#define SCREENSAVERPICS_H

#include <stdbool.h>
#include <stddef.h>

// The Screensaver folder: which of its files this screen can show, and one of
// them at random.
//
// A folder in the root of the card, beside Playlist, holding the pictures the
// screensaver puts up instead of the record's artwork. Nearly anything goes:
//
//   * JPEG (.jpg, .jpeg), baseline or progressive, and PNG;
//   * any size up to 16384 on a side and 8 MB of file. The cover pipeline
//     decodes JPEGs at a half, quarter or eighth and PNGs a scanline at a time,
//     so a large picture does not cost large memory;
//   * portrait or landscape: the picture is scaled until it covers the screen
//     and the overflow is cropped evenly from both sides (COVER_FIT_COVER).
//
// What is passed over, in silence, is a file that is not a picture of those
// kinds, is too big, or turns out not to decode (a PNG too large for the memory
// free at the time). With several in the folder the others still come up.
//
// Nothing here decodes anything: the rules are decided from the file headers
// and the file's size, which is what lets a folder be judged at every wake.

// Where the card is mounted. Called at startup and again when a card is
// attached; passing NULL or "" leaves the folder unreachable, which is the
// right answer with no card in.
void screensaverpics_set_root(const char *card_root);

// The folder itself, or "" when there is no card.
const char *screensaverpics_dir(void);

// Whether one named file in the folder is a picture that can be shown.
bool screensaverpics_usable(const char *dir, const char *name);

// Whether the folder holds at least one. Stops at the first, so the answer
// costs one readable file rather than a whole folder.
bool screensaverpics_any(void);

// One picture at random, as a full path. `current` is the one already on
// screen (a path or a bare name, either will do) and is avoided while there is
// anything else to choose. False when nothing usable turned up, leaving `out`
// empty.
bool screensaverpics_pick(const char *current, char *out, size_t out_size);

#endif /* SCREENSAVERPICS_H */

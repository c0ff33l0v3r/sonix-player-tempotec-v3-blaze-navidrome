#include "filespage.h"
#include "folderpicker.h"
#include "src/system/device/workactivity.h"
#include "src/system/library/filepublication.h"
#include "src/system/library/filepublicationcard.h"
#include <errno.h>
#include <fcntl.h>

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/confirm.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/library/imageview.h"
#include "src/gui/library/textview.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/system.h"
#include "src/system/library/library.h"
#include "src/system/playback/playlist.h"
#include "src/gui/shell/uiscale.h"

lv_obj_t *filespage_screen;

// The tap a gesture ends with is not a tap. LVGL sends CLICKED to whatever the
// finger was on when it lifted, and for a swipe-back that is the row the swipe
// began on, so a tap handler has to ignore a click that closed a drag. Same
// guard the browser, the media lists and the grid pages use.
static bool gesture_not_tap(void) {
	return player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active();
}

// How deep the page will follow a path, and how many entries it will list from
// one folder. Both are limits against a pathological card rather than against
// any real one: a folder with more than this in it is already unusable as a
// list. What the cap leaves out goes in the log, because a file manager that
// drops entries without saying so is worse than one with a limit.
#define MAX_ENTRIES 2000
#define PATH_MAX_LEN 640

// The listing is built a screenful at a time, for the reason the chapter list
// gives in ebookreader.c: every row is a card with a glyph and two labels, and
// LVGL measures all of them before it can draw the first. What the reader is
// waiting for is the first six.
//
// It matters more here than there, because a row also costs a stat() on the
// card to find out how big the file is -- and a stat on a slow card, two
// thousand times, is a folder that takes seconds to open. Built this way, it is
// a stat for each row that is actually on screen.
#define ROWS_FIRST ui_px(14)
#define ROWS_MORE ui_px(14)

#define ROW_HEIGHT ui_px(76)

typedef struct {
	char *name;	 // in the pool
	bool dir;
	int64_t bytes;
} entry_t;

static char root_path[PATH_MAX_LEN];  // the card, which is as far up as this goes
static char path[PATH_MAX_LEN];		  // the folder on screen

static entry_t *entries;
static int entry_count;
static int entries_built;  // how many rows are on the list
static int skipped;		   // entries the cap left out

typedef struct {
	char path[PATH_MAX_LEN];
	unsigned visit;
	uint64_t card_epoch;
	entry_t *entries;
	int count, skipped;
	bool failed;
} folder_job_t;
static pthread_mutex_t folder_lock = PTHREAD_MUTEX_INITIALIZER;
static folder_job_t *folder_pending, *folder_result;
static bool folder_busy;
static unsigned folder_visit;
static uint64_t entries_epoch, op_epoch;
static void folder_deliver(void *unused);
static void folder_free(folder_job_t *job) {
	if (!job)
		return;
	for (int i = 0; i < job->count; i++)
		free(job->entries[i].name);
	free(job->entries);
	free(job);
}

// Set while rebuild() is pulling the list apart and putting it back.
//
// Emptying a scrolling list changes how much there is to scroll, and LVGL says
// so with an LV_EVENT_SCROLL -- delivered from inside lv_obj_clean(), while the
// entries still belong to the folder being left. Taken as "scrolled near the
// end, build some more", that would append rows of the outgoing folder to the
// list of the incoming one.
static bool rebuilding;

static lv_obj_t *file_list;
static lv_obj_t *empty_label;
static lv_obj_t *title_label;


// The name dialog, in the shape the playlist page uses: a full-screen layer
// with the field at the top and the shared keyboard at the bottom.
static lv_obj_t *name_layer;
static lv_obj_t *name_heading;
static lv_obj_t *name_field;
static keyboard_t *name_keyboard;

// What the dialog is naming: a folder being created, or the entry below.
static bool naming_new_folder;
static char acting_on[NAME_MAX + 1];

// ---------------------------------------------------------------------------
// Moving and copying
//
// Both need a second thing the rest of the page does not: somewhere to put it.
// That is the picker layer -- the same walk as the page itself, folders only,
// with the button that says "here" at the bottom.
//
// And both can take a while. A copy is bounded by how big the file is, which on
// a music card is half a gigabyte often enough, and a move across the card is a
// rename and instant -- until it is not, and then it is a copy. So the work
// happens on a thread with the modal up, and the page hears about it through
// gui_post(): the alternative is an interface frozen for a minute, which on a
// single core is what doing it here would mean.
// ---------------------------------------------------------------------------

typedef enum {
	OP_NONE = 0,
	OP_MOVE,
	OP_COPY,
	OP_DELETE,
	OP_RENAME,
	OP_MKDIR,
} op_t;

static op_t pending_op;
static char op_source[PATH_MAX_LEN + NAME_MAX + 2];	 // what is being moved or copied
static char op_target[PATH_MAX_LEN + NAME_MAX + 2];	 // where it is going, name and all
static bool op_ok, op_complete;
static work_activity_t op_activity;

// How far along it is, in bytes rather than in files: a folder holding one
// album and a hundred cover thumbnails would otherwise stand at 1% and then
// jump to the end.
//
// The total is counted by the worker and not by op_start(), because a move
// within the card is a rename and has no length worth counting -- it is only
// when the rename fails and the move becomes a copy that there is anything to
// measure. A total of zero means "no bar", which is exactly right for both the
// rename and the moment before the walk has finished.
static pthread_mutex_t op_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t op_total_bytes;
static uint64_t op_done_bytes;
static lv_timer_t *op_progress_timer;

static lv_obj_t *pick_layer;
static lv_obj_t *pick_list;
static lv_obj_t *pick_title;
static lv_obj_t *pick_here_btn;
static lv_obj_t *pick_here_label;
static char pick_path[PATH_MAX_LEN];

static void rebuild(void);
static void op_finished(void *user);

// ---------------------------------------------------------------------------
// what a file is
// ---------------------------------------------------------------------------

// The glyph for a name, by its extension. The same families the transfer page
// draws in a browser, so the card looks the same from both ends.
static const char *ext_of(const char *name) {
	const char *dot = strrchr(name, '.');
	return dot ? dot + 1 : "";
}

static bool ext_in(const char *ext, const char *const *list) {
	for (int i = 0; list[i]; i++) {
		if (strcasecmp(ext, list[i]) == 0) {
			return true;
		}
	}
	return false;
}

static const char *const EXT_AUDIO[] = {"iso", "dff", "dsf", "dts", "ape", "flac", "aif", "aiff", "wav",
										"m4a", "aac", "mp2", "mp3", "ogg", "oga", "wma", "opus", "dsd",
										"wv",  "mpc", "tak", "tta", NULL};
static const char *const EXT_AUDIOBOOK[] = {"m4b", "aa", "aax", NULL};
static const char *const EXT_PLAYLIST[] = {"m3u", "m3u8", "cue", NULL};
static const char *const EXT_IMAGE[] = {"bmp", "png", "jpg", "jpeg", "gif", "webp", NULL};
static const char *const EXT_TEXT[] = {"lrc", "txt", "t", "ini", "nfo", NULL};
static const char *const EXT_FIRMWARE[] = {"upt", NULL};
static const char *const EXT_GAME[] = {"gb", "gbc", "sav", NULL};
static const char *const EXT_BOOK[] = {"epub", NULL};

static const lv_image_dsc_t *glyph_for(const entry_t *entry) {
	if (entry->dir) {
		return &icon_folder;
	}
	const char *ext = ext_of(entry->name);
	if (ext_in(ext, EXT_AUDIO)) {
		return &icon_files_music;
	}
	if (ext_in(ext, EXT_AUDIOBOOK)) {
		return &icon_files_audiobook;
	}
	if (ext_in(ext, EXT_PLAYLIST)) {
		return &icon_files_playlist;
	}
	if (ext_in(ext, EXT_IMAGE)) {
		return &icon_files_image;
	}
	if (ext_in(ext, EXT_TEXT)) {
		return &icon_files_text;
	}
	if (ext_in(ext, EXT_FIRMWARE)) {
		return &icon_files_update;
	}
	if (ext_in(ext, EXT_GAME)) {
		return &icon_files_game;
	}
	if (ext_in(ext, EXT_BOOK)) {
		return &icon_files_book;
	}
	return &icon_file;
}

// A name with any of the extensions above is a file and there is nothing to ask
// the card about. Only names that could go either way are worth a stat -- which
// on exFAT, where readdir fills in no type at all, is what keeps opening a
// folder of eight hundred tracks from being eight hundred stats.
static bool ext_is_known(const char *name) {
	const char *ext = ext_of(name);
	return ext[0] && (ext_in(ext, EXT_AUDIO) || ext_in(ext, EXT_AUDIOBOOK) || ext_in(ext, EXT_PLAYLIST) ||
					  ext_in(ext, EXT_IMAGE) || ext_in(ext, EXT_TEXT) || ext_in(ext, EXT_FIRMWARE) ||
					  ext_in(ext, EXT_GAME) || ext_in(ext, EXT_BOOK));
}

// The player's own way of putting a size on screen, the same one the transfer
// page uses.
static void human_size(long long bytes, char *out, size_t size) {
	if (bytes >= 1073741824LL) {
		snprintf(out, size, "%.2f GB", (double)bytes / 1073741824.0);
	} else if (bytes >= 1048576LL) {
		snprintf(out, size, "%.1f MB", (double)bytes / 1048576.0);
	} else if (bytes >= 1024LL) {
		snprintf(out, size, "%lld KB", bytes / 1024);
	} else {
		snprintf(out, size, "%lld B", bytes);
	}
}

// Joins the folder on screen with one of its entries. False when the result
// would not fit, and then it is a refusal and not a shortened path: everything
// here acts on what this builds, and a truncated path names a different file --
// which for the delete below would be a different file deleted.
static bool child_path(const char *name, char *out, size_t size) {
	int wrote = snprintf(out, size, "%s/%s", path, name);
	if (wrote < 0 || (size_t)wrote >= size) {
		out[0] = '\0';
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// reading a folder
// ---------------------------------------------------------------------------

static void entries_free(void) {
	for (int i = 0; i < entry_count; i++) {
		free(entries[i].name);
	}
	free(entries);
	entries = NULL;
	entry_count = 0;
	entries_built = 0;
	skipped = 0;
}

// Folders first, then by name, the way every list on this device is ordered.
static int entry_cmp(const void *a, const void *b) {
	const entry_t *x = a, *y = b;
	if (x->dir != y->dir) {
		return x->dir ? -1 : 1;
	}
	return strcasecmp(x->name, y->name);
}

static void read_folder_contents(folder_job_t *job) {

	DIR *dir = opendir(job->path);
	if (!dir) {
		job->failed = true;
		return;
	}

	job->entries = calloc(MAX_ENTRIES, sizeof(*job->entries));
	if (!job->entries) {
		job->failed = true;
		closedir(dir);
		return;
	}

	fp_guard_t protection;
	if (!fp_guard_open(&protection, dirfd(dir), false, false)) {
		fp_guard_close(&protection);
		closedir(dir);
		job->failed = true;
		return;
	}
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
			continue;
		}
		// Unlike the music browser, a dot file is shown: a file manager that
		// hides files is not one, and .local is exactly the kind of folder
		// somebody opens this page to look inside. What stays out is the junk a
		// desktop leaves behind, which is not the user's and never was.
		if (playlist_is_junk_name(de->d_name) || fp_guard_lookup(&protection, de->d_name) != FP_SAFE) {
			continue;
		}

		if (job->count >= MAX_ENTRIES) {
			job->skipped++;
			continue;
		}

		bool is_dir;
		if (de->d_type == DT_DIR) {
			is_dir = true;
		} else if (de->d_type == DT_REG || de->d_type == DT_LNK) {
			is_dir = false;
		} else if (ext_is_known(de->d_name)) {
			is_dir = false;
		} else {
			char full[PATH_MAX_LEN + NAME_MAX + 2];
			struct stat st;
			is_dir = (snprintf(full, sizeof(full), "%s/%s", job->path, de->d_name) < (int)sizeof(full)) && stat(full, &st) == 0 && S_ISDIR(st.st_mode);
		}

		struct stat info;
		if (fstatat(dirfd(dir), de->d_name, &info, AT_SYMLINK_NOFOLLOW))
			continue;
		if (S_ISLNK(info.st_mode)) {
			char full[PATH_MAX_LEN + NAME_MAX + 2];
			int n = snprintf(full, sizeof(full), "%s/%s", job->path, de->d_name);
			int admission = n > 0 && n < (int)sizeof(full) ? fp_read_begin(full) : -1;
			if (admission < 0)
				continue;
			fp_admission_end(admission);
		}
		job->entries[job->count].bytes = info.st_size;
		job->entries[job->count].name = strdup(de->d_name);
		if (!job->entries[job->count].name) {
			break;
		}
		job->entries[job->count].dir = is_dir;
		job->count++;
	}
	fp_guard_close(&protection);
	closedir(dir);

	if (job->skipped > 0) {
		fprintf(stderr, "files: listing limit reached, %d entries omitted\n", job->skipped);
	}

	qsort(job->entries, (size_t)job->count, sizeof(*job->entries), entry_cmp);
}

static void read_folder(folder_job_t *job) {
	work_activity_t activity = {0};
	if (!work_activity_begin(&activity, WORK_STORAGE)) {
		job->failed = true;
		return;
	}
	int lease = fp_card_local_begin(job->card_epoch);
	if (lease != -1)
		read_folder_contents(job);
	else
		job->failed = true;
	fp_card_end(lease);
	work_activity_end(&activity);
}

static void *folder_worker(void *unused) {
	(void)unused;
	thread_be_background("file listing");
	for (;;) {
		pthread_mutex_lock(&folder_lock);
		folder_job_t *job = folder_pending;
		folder_pending = NULL;
		if (!job)
			folder_busy = false;
		pthread_mutex_unlock(&folder_lock);
		if (!job)
			return NULL;
		read_folder(job);
		pthread_mutex_lock(&folder_lock);
		folder_free(folder_result);
		folder_result = job;
		pthread_mutex_unlock(&folder_lock);
		gui_post_signal(folder_deliver, NULL);
	}
}

// ---------------------------------------------------------------------------
// the actions
// ---------------------------------------------------------------------------

// Deletes a folder and everything in it. Written out rather than shelling out
// to `rm -rf`: there is no shell on the device worth trusting with a path the
// user typed, and this way a name with a quote in it is just a name.
static bool remove_tree_unprotected(const char *target) {
	struct stat st;
	if (lstat(target, &st) != 0) {
		return false;
	}
	if (!S_ISDIR(st.st_mode)) {
		return unlink(target) == 0;
	}

	DIR *dir = opendir(target);
	if (!dir) {
		return false;
	}
	bool ok = true;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
			continue;
		}
		char child[PATH_MAX_LEN + NAME_MAX + 2];
		snprintf(child, sizeof(child), "%s/%s", target, de->d_name);
		if (!remove_tree_unprotected(child)) {
			ok = false;
		}
	}
	closedir(dir);
	return rmdir(target) == 0 && ok;
}

static bool remove_tree(const char *target) {
	int admission = fp_mutation_begin(target, NULL, true);
	if (admission < 0)
		return false;
	bool ok = remove_tree_unprotected(target);
	fp_admission_end(admission);
	return ok;
}
static bool guarded_rename(const char *from, const char *to, bool absent) {
	int admission = fp_mutation_begin(from, to, true);
	if (admission < 0)
		return false;
	struct stat st;
	bool available = !absent || (lstat(to, &st) != 0 && errno == ENOENT);
	bool ok = available && rename(from, to) == 0;
	int reason = available ? errno : EEXIST;
	fp_admission_end(admission);
	errno = reason;
	return ok;
}
static bool guarded_mkdir(const char *target) {
	int admission = fp_mutation_begin(NULL, target, false);
	if (admission < 0)
		return false;
	bool ok = mkdir(target, 0777) == 0;
	fp_admission_end(admission);
	return ok;
}

// ---------------------------------------------------------------------------
// copying
// ---------------------------------------------------------------------------

#define COPY_CHUNK (64 * 1024)

// What the whole job amounts to. lstat, not stat: a symlink counts as the few
// bytes of its own name and not as whatever it points at, which is also how it
// will be copied.
static uint64_t tree_bytes(const char *target) {
	struct stat st;
	if (lstat(target, &st) != 0) {
		return 0;
	}
	if (!S_ISDIR(st.st_mode)) {
		return (uint64_t)st.st_size;
	}

	DIR *dir = opendir(target);
	if (!dir) {
		return 0;
	}
	uint64_t total = 0;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
			continue;
		}
		char child[PATH_MAX_LEN + NAME_MAX + 2];
		int wrote = snprintf(child, sizeof(child), "%s/%s", target, de->d_name);
		if (wrote < 0 || (size_t)wrote >= sizeof(child)) {
			continue; // too deep to name: copy_tree will not copy it either
		}
		total += tree_bytes(child);
	}
	closedir(dir);
	return total;
}

static void op_advance(size_t bytes) {
	pthread_mutex_lock(&op_lock);
	op_done_bytes += bytes;
	pthread_mutex_unlock(&op_lock);
}

static bool copy_file(const char *from, const char *to) {
	int admission = fp_mutation_begin(from, to, false);
	if (admission < 0)
		return false;
	int source = open(from, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
	struct stat source_st;
	if (source < 0 || fstat(source, &source_st) || !S_ISREG(source_st.st_mode)) {
		if (source >= 0)
			close(source);
		fp_admission_end(admission);
		return false;
	}
	FILE *in = fdopen(source, "rb");
	int destination = in ? open(to, O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0666) : -1;
	struct stat target_st;
	bool target_ok = destination >= 0 && !fstat(destination, &target_st) && S_ISREG(target_st.st_mode) && (source_st.st_dev != target_st.st_dev || source_st.st_ino != target_st.st_ino) && !ftruncate(destination, 0);
	FILE *out = target_ok ? fdopen(destination, "wb") : NULL;
	fp_admission_end(admission);
	if (!in || !out) {
		if (in)
			fclose(in);
		else
			close(source);
		if (destination >= 0 && !out)
			close(destination);
		return false;
	}

	char *buffer = malloc(COPY_CHUNK);
	if (!buffer) {
		fclose(in);
		fclose(out);
		return false;
	}

	bool ok = true;
	size_t got;
	while ((got = fread(buffer, 1, COPY_CHUNK, in)) > 0) {
		if (fwrite(buffer, 1, got, out) != got) {
			ok = false;
			break;
		}
		op_advance(got);
	}
	if (ferror(in)) {
		ok = false;
	}

	free(buffer);
	fclose(in);
	if (fclose(out) != 0) {
		ok = false;
	}
	// Preserve a failed ordinary copy: without a live ownership check,
	// unlinking its pathname could delete another operation's replacement.
	return ok;
}

static bool copy_tree(const char *from, const char *to) {
	struct stat st;
	if (lstat(from, &st) != 0) {
		return false;
	}
	if (!S_ISDIR(st.st_mode)) {
		return copy_file(from, to);
	}

	if (!guarded_mkdir(to)) {
		return false;
	}

	DIR *dir = opendir(from);
	if (!dir) {
		return false;
	}
	bool ok = true;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
			continue;
		}
		if (!strncasecmp(de->d_name, FP_JOURNAL, sizeof(FP_JOURNAL) - 1))
			continue;
		char child_from[PATH_MAX_LEN + NAME_MAX + 2];
		char child_to[PATH_MAX_LEN + NAME_MAX + 2];
		int a = snprintf(child_from, sizeof(child_from), "%s/%s", from, de->d_name);
		int b = snprintf(child_to, sizeof(child_to), "%s/%s", to, de->d_name);
		if (a < 0 || (size_t)a >= sizeof(child_from) || b < 0 || (size_t)b >= sizeof(child_to)) {
			ok = false; // too deep to name, so too deep to copy
			continue;
		}
		if (!copy_tree(child_from, child_to)) {
			ok = false;
		}
	}
	closedir(dir);
	return ok;
}

// ---------------------------------------------------------------------------
// the thread that does it, and the way back
// ---------------------------------------------------------------------------

// Reads the two counters and moves the bar. It runs on the interface thread and
// the worker never touches LVGL, which is the whole reason for the counters:
// the alternative is a gui_post() per chunk, and a 64 KB chunk of a gigabyte is
// sixteen thousand of them.
#define OP_PROGRESS_MS 200

static void op_progress_cb(lv_timer_t *timer) {
	(void)timer;

	pthread_mutex_lock(&op_lock);
	uint64_t total = op_total_bytes;
	uint64_t done = op_done_bytes;
	bool complete = op_complete;
	pthread_mutex_unlock(&op_lock);
	if (complete) {
		op_finished(NULL);
		return;
	}

	// Nothing to show until the walk has a total: a rename never gets one, and
	// a bar at nought over a job that has already finished is a lie.
	gui_modal_progress(total > 0 ? (int)((done * 100) / total) : -1);
}

static void op_progress_stop(void) {
	if (op_progress_timer) {
		lv_timer_delete(op_progress_timer);
		op_progress_timer = NULL;
	}
}

static void op_finished(void *user) {
	(void)user;
	pthread_mutex_lock(&op_lock);
	bool complete = op_complete;
	op_complete = false;
	bool ok = op_ok;
	pthread_mutex_unlock(&op_lock);
	if (!complete)
		return;
	op_progress_stop();
	gui_modal_hide();
	if (ok) {
		if (pending_op == OP_MOVE)
			toast_success("files_moved");
		else if (pending_op == OP_COPY)
			toast_success("files_copied");
		else if (pending_op == OP_DELETE)
			toast_success("files_deleted");
	} else {
		const char *reason = pending_op == OP_MOVE ? "files_could_not_move_it" : pending_op == OP_COPY ? "files_could_not_copy_it" : pending_op == OP_DELETE ? "files_could_not_delete_it" : pending_op == OP_RENAME ? "files_could_not_rename_it" : "files_create_failed";
		gui_notify_popup(reason);
	}
	pending_op = OP_NONE;
	storage_card_files_recheck();
	rebuild();
}

static void *op_worker(void *arg) {
	(void)arg;
	thread_be_background("filesop");
	bool ok = false;
	int lease = fp_card_local_begin(op_epoch);
	if (lease == -1)
		goto done;
	if (pending_op == OP_DELETE) {
		ok = remove_tree(op_source);
		const char *gone[] = {op_source};
		if (ok)
			library_forget_deleted(gone, 1);
	} else if (pending_op == OP_MKDIR) {
		ok = guarded_mkdir(op_target);
	} else if (pending_op == OP_RENAME) {
		ok = guarded_rename(op_source, op_target, true);
	} else {
		int admission = fp_mutation_begin(op_source, op_target, true);
		bool eligible = admission >= 0;
		fp_admission_end(admission);
		if (eligible && pending_op == OP_MOVE) {
			ok = guarded_rename(op_source, op_target, false);
			eligible = !ok && errno == EXDEV;
		}
		if (eligible && !ok) {
			uint64_t total = tree_bytes(op_source);
			pthread_mutex_lock(&op_lock);
			op_total_bytes = total;
			pthread_mutex_unlock(&op_lock);
			ok = copy_tree(op_source, op_target);
			if (ok && pending_op == OP_MOVE)
				ok = remove_tree(op_source);
		}
	}
done:
	fp_card_end(lease);
	work_activity_end(&op_activity);
	pthread_mutex_lock(&op_lock);
	op_ok = ok;
	op_complete = true;
	pthread_mutex_unlock(&op_lock);
	gui_post_signal(op_finished, NULL);
	return NULL;
}

static void op_start(void) {
	op_epoch = entries_epoch;
	pthread_mutex_lock(&op_lock);
	op_total_bytes = 0;
	op_done_bytes = 0;
	op_complete = false;
	pthread_mutex_unlock(&op_lock);

	gui_modal_show(NULL, theme()->accent, pending_op == OP_MOVE ? "moving" : pending_op == OP_COPY ? "copying" : "loading", NULL);

	pthread_t thread;
	if (!work_activity_begin(&op_activity, WORK_JOB | WORK_STORAGE) || pthread_create(&thread, NULL, op_worker, NULL) != 0) {
		work_activity_end(&op_activity);
		pthread_mutex_lock(&op_lock);
		op_ok = false;
		op_complete = true;
		pthread_mutex_unlock(&op_lock);
		op_finished(NULL);
		return;
	}
	pthread_detach(thread);

	op_progress_stop();
	op_progress_timer = lv_timer_create(op_progress_cb, OP_PROGRESS_MS, NULL);
}

static void delete_confirmed(void *user) {
	(void)user;
	if (!acting_on[0] || pending_op != OP_NONE || !child_path(acting_on, op_source, sizeof(op_source)))
		return;
	acting_on[0] = 0;
	pending_op = OP_DELETE;
	op_start();
}

static void menu_delete_action(void *user) {
	(void)user;
	if (!acting_on[0]) {
		return;
	}
	// The name goes in the question, because on a list of forty rows the one the
	// menu belongs to is not obvious by the time the dialog is up. Formatted
	// here and passed as the title: confirm_show() puts its message through
	// tr(), and a file name is not a translation key.
	char question[NAME_MAX + 64];
	snprintf(question, sizeof(question), tr("files_delete_x_question"), acting_on);
	confirm_show(question, NULL, "delete", delete_confirmed, NULL);
}

static void name_layer_show(const char *heading, const char *initial) {
	lv_label_set_text(name_heading, tr(heading));
	lv_textarea_set_text(name_field, initial ? initial : "");
	keyboard_reset(name_keyboard);
	keyboard_set_field(name_keyboard, name_field);
	lv_obj_set_hidden(name_layer, false);
	lv_obj_move_foreground(name_layer);
}

static void name_layer_hide(void) {
	lv_obj_set_hidden(name_layer, true);
	acting_on[0] = '\0';
	naming_new_folder = false;
}

// ---------------------------------------------------------------------------
// choosing where
// ---------------------------------------------------------------------------

static void pick_rebuild(void);
static void pick_name_free(lv_event_t *e) { free(lv_event_get_user_data(e)); }

static void pick_row_cb(lv_event_t *e) {
	if (gesture_not_tap()) {
		return;
	}
	const char *name = lv_event_get_user_data(e);
	char next[PATH_MAX_LEN];
	int wrote = snprintf(next, sizeof(next), "%s/%s", pick_path, name);
	if (wrote < 0 || (size_t)wrote >= sizeof(next)) {
		gui_notify_popup("files_too_deep");
		return;
	}
	snprintf(pick_path, sizeof(pick_path), "%s", next);
	pick_rebuild();
	lv_obj_scroll_to_y(pick_list, 0, LV_ANIM_OFF);
}

// Folders only: nothing else can be a destination, and a list of every track on
// the card would bury the three folders that can.
static void pick_rebuild(void) {
	lv_obj_clean(pick_list);

	const char *slash = strrchr(pick_path, '/');
	lv_label_set_text(pick_title, strcmp(pick_path, root_path) == 0 ? tr("file_explorer") : (slash ? slash + 1 : pick_path));
	lv_label_set_text(pick_here_label, tr(pending_op == OP_MOVE ? "files_move_here" : "files_copy_here"));

	DIR *dir = opendir(pick_path);
	if (!dir) {
		return;
	}

	// Names first, then rows, so they come out in order without a second walk.
	char (*names)[NAME_MAX + 1] = calloc(MAX_ENTRIES, NAME_MAX + 1);
	int count = 0;
	if (names) {
		struct dirent *de;
		while ((de = readdir(dir)) != NULL && count < MAX_ENTRIES) {
			if (de->d_name[0] == '.' || playlist_is_junk_name(de->d_name)) {
				continue;
			}
			char full[PATH_MAX_LEN + NAME_MAX + 2];
			int wrote = snprintf(full, sizeof(full), "%s/%s", pick_path, de->d_name);
			if (wrote < 0 || (size_t)wrote >= sizeof(full)) {
				continue;
			}
			struct stat st;
			if (de->d_type != DT_DIR && !(de->d_type == DT_UNKNOWN && stat(full, &st) == 0 && S_ISDIR(st.st_mode))) {
				continue;
			}
			snprintf(names[count], NAME_MAX + 1, "%s", de->d_name);
			count++;
		}
	}
	closedir(dir);

	if (!names) {
		return;
	}

	for (int i = 0; i < count - 1; i++) {
		for (int j = i + 1; j < count; j++) {
			if (strcasecmp(names[i], names[j]) > 0) {
				char swap[NAME_MAX + 1];
				snprintf(swap, sizeof(swap), "%s", names[i]);
				snprintf(names[i], NAME_MAX + 1, "%s", names[j]);
				snprintf(names[j], NAME_MAX + 1, "%s", swap);
			}
		}
	}

	for (int i = 0; i < count; i++) {
		// A row owns its callback name until it is rebuilt or destroyed.
		char *kept = strdup(names[i]);
		if (!kept)
			break;
		lv_obj_t *row = folderpicker_row(pick_list, names[i], pick_row_cb, kept);
		lv_obj_add_event_cb(row, pick_name_free, LV_EVENT_DELETE, kept);
	}
	free(names);
}

static void pick_hide(void) {
	lv_obj_set_hidden(pick_layer, true);
	pending_op = OP_NONE;
	acting_on[0] = '\0';
}

static void pick_cancel_cb(lv_event_t *e) {
	(void)e;
	pick_hide();
}

// True when `inner` is `outer` or lies under it. What it is for: a folder
// cannot be moved or copied into itself or into one of its own children --
// a move would make it unreachable, a copy would not end.
static bool path_is_within(const char *inner, const char *outer) {
	size_t n = strlen(outer);
	if (strncmp(inner, outer, n) != 0) {
		return false;
	}
	return inner[n] == '\0' || inner[n] == '/';
}

static void pick_here_cb(lv_event_t *e) {
	(void)e;
	if (pending_op == OP_NONE || !acting_on[0]) {
		return;
	}

	char source[PATH_MAX_LEN + NAME_MAX + 2];
	char target[PATH_MAX_LEN + NAME_MAX + 2];
	int wrote = snprintf(target, sizeof(target), "%s/%s", pick_path, acting_on);
	if (!child_path(acting_on, source, sizeof(source)) || wrote < 0 || (size_t)wrote >= sizeof(target)) {
		gui_notify_popup("files_too_deep");
		return;
	}

	if (strcmp(pick_path, path) == 0) {
		gui_notify_popup("files_it_is_already_there");
		return;
	}
	if (path_is_within(pick_path, source)) {
		gui_notify_popup("files_move_into_itself");
		return;
	}
	struct stat st;
	if (lstat(target, &st) == 0) {
		gui_notify_popup("files_name_taken");
		return;
	}

	snprintf(op_source, sizeof(op_source), "%s", source);
	snprintf(op_target, sizeof(op_target), "%s", target);

	lv_obj_set_hidden(pick_layer, true);
	acting_on[0] = '\0';
	op_start();
}

static void pick_show(op_t op) {
	pending_op = op;
	snprintf(pick_path, sizeof(pick_path), "%s", root_path);
	pick_rebuild();
	lv_obj_scroll_to_y(pick_list, 0, LV_ANIM_OFF);
	lv_obj_set_hidden(pick_layer, false);
	lv_obj_move_foreground(pick_layer);
}

static void menu_move_action(void *user) {
	(void)user;
	if (acting_on[0]) {
		pick_show(OP_MOVE);
	}
}

static void menu_copy_action(void *user) {
	(void)user;
	if (acting_on[0]) {
		pick_show(OP_COPY);
	}
}

static void menu_rename_action(void *user) {
	(void)user;
	if (!acting_on[0]) {
		return;
	}
	naming_new_folder = false;
	name_layer_show("rename", acting_on);
}

static void newdir_btn_cb(lv_event_t *e) {
	(void)e;
	acting_on[0] = '\0';
	naming_new_folder = true;
	name_layer_show("files_new_folder", NULL);
}

// A name that would step outside the folder on screen, or overwrite the parent,
// is not a name: everything here joins `path` with a slash, and a '/' in the
// middle of that is a path and not a file name.
static bool name_is_usable(const char *name) {
	return name[0] && strcmp(name, ".") != 0 && strcmp(name, "..") != 0 && strchr(name, '/') == NULL;
}

static void name_accept_cb(lv_event_t *e) {
	(void)e;

	const char *name = lv_textarea_get_text(name_field);
	if (!name_is_usable(name)) {
		gui_notify_popup("files_name_invalid");
		return;
	}

	char target[PATH_MAX_LEN + NAME_MAX + 2];
	if (!child_path(name, target, sizeof(target))) {
		gui_notify_popup("files_name_invalid");
		return;
	}

	if (pending_op != OP_NONE)
		return;
	snprintf(op_target, sizeof(op_target), "%s", target);
	if (!naming_new_folder && !child_path(acting_on, op_source, sizeof(op_source)))
		return;
	pending_op = naming_new_folder ? OP_MKDIR : OP_RENAME;
	name_layer_hide();
	op_start();
}

static void name_cancel_cb(lv_event_t *e) {
	(void)e;
	name_layer_hide();
}

// ---------------------------------------------------------------------------
// the rows
// ---------------------------------------------------------------------------

static void entry_menu_cb(lv_event_t *e) {
	if (gesture_not_tap()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= entry_count) {
		return;
	}
	snprintf(acting_on, sizeof(acting_on), "%s", entries[index].name);

	static const popover_item_t items[] = {
		{"rename", menu_rename_action, NULL, false},
		{"files_move_to", menu_move_action, NULL, false},
		{"files_copy_to", menu_copy_action, NULL, false},
		{"delete", menu_delete_action, NULL, false},
	};
	popover_show(lv_event_get_target(e), items, 4);
}

static void row_clicked_cb(lv_event_t *e) {
	if (gesture_not_tap()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= entry_count) {
		return;
	}

	// A text file opens to be read and a picture to be looked at. Any other
	// file has nothing to open into, and the tap does nothing: the menu is what
	// the three dots are for, and a row that opens a menu when pressed anywhere
	// is a row that opens one every time a scroll ends on it.
	if (!entries[index].dir) {
		char file[PATH_MAX_LEN];
		const char *ext = ext_of(entries[index].name);
		if (ext_in(ext, EXT_TEXT) && child_path(entries[index].name, file, sizeof(file)) && !textview_open(file)) {
			gui_notify_popup("files_text_unreadable");
		} else if (ext_in(ext, EXT_IMAGE) && child_path(entries[index].name, file, sizeof(file))) {
			imageview_open(file);
		}
		return;
	}

	char next[PATH_MAX_LEN];
	if (!child_path(entries[index].name, next, sizeof(next))) {
		gui_notify_popup("files_too_deep");
		return;
	}
	snprintf(path, sizeof(path), "%s", next);
	rebuild();
	lv_obj_scroll_to_y(file_list, 0, LV_ANIM_OFF);
}

static void build_row(int index) {
	const entry_t *entry = &entries[index];

	lv_obj_t *row = lv_btn_create(file_list);
	lv_obj_set_size(row, lv_pct(100), ROW_HEIGHT);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, ui_px(12), 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, ui_px(16), 0);
	lv_obj_set_style_pad_column(row, ui_px(14), 0);
	lv_obj_set_event_bubble(row, true);
	lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *glyph = lv_image_create(row);
	lv_image_set_src(glyph, glyph_for(entry));
	lv_obj_add_style(glyph, &theme_style_icon, 0);
	// A folder in the accent, everything else in the quiet text colour: the one
	// thing worth telling apart at a glance is what can be entered.
	lv_obj_set_style_image_recolor(glyph, entry->dir ? theme()->accent : theme()->text_secondary, 0);
	lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);

	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text(label, entry->name);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(label, 1);
	// One line, then dots. Without a height the label wraps instead, and a long
	// name then grows past the fixed row it lives in.
	lv_obj_set_height(label, ui_px(32));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);

	if (!entry->dir) {
		char text[24];
		human_size((long long)entry->bytes, text, sizeof(text));
		lv_obj_t *size_label = lv_label_create(row);
		lv_label_set_text(size_label, text);
		lv_obj_add_style(size_label, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(size_label, &font_ui_18, 0);
	}

	lv_obj_t *dots = lv_btn_create(row);
	lv_obj_set_size(dots, ui_px(44), ui_px(44));
	lv_obj_set_style_bg_opa(dots, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(dots, 0, 0);
	lv_obj_set_style_shadow_width(dots, 0, 0);
	lv_obj_set_style_pad_all(dots, 0, 0);
	lv_obj_add_event_cb(dots, entry_menu_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	lv_obj_t *dots_glyph = lv_image_create(dots);
	lv_image_set_src(dots_glyph, &icon_ellipsis_vertical);
	lv_obj_add_style(dots_glyph, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(dots_glyph, LV_OPA_60, 0);
	lv_obj_center(dots_glyph);
}

static void rows_add(int how_many) {
	int end = entries_built + how_many;
	if (end > entry_count) {
		end = entry_count;
	}
	for (; entries_built < end; entries_built++) {
		build_row(entries_built);
	}
}

// Keeps going until there is something below the fold, so a short first batch
// on a tall list still leaves a scroll to ask for the rest with.
static void rows_fill(void) {
	for (int guard = 0; guard < 4 && entries_built < entry_count; guard++) {
		lv_obj_update_layout(file_list);
		if (lv_obj_get_scroll_bottom(file_list) > 0) {
			return;
		}
		rows_add(ROWS_MORE);
	}
}

static void list_scrolled_cb(lv_event_t *e) {
	(void)e;
	if (rebuilding || entries_built >= entry_count) {
		return;
	}
	if (lv_obj_get_scroll_bottom(file_list) < lv_obj_get_height(file_list)) {
		rows_add(ROWS_MORE);
	}
}

// ---------------------------------------------------------------------------

// The heading is the folder's own name, and the card's is the card's label
// rather than the last piece of a mount point nobody chose.
static void title_refresh(void) {
	if (strcmp(path, root_path) == 0) {
		lv_label_set_text(title_label, tr("file_explorer"));
		return;
	}
	const char *slash = strrchr(path, '/');
	lv_label_set_text(title_label, slash ? slash + 1 : path);
}

static void folder_deliver(void *unused) {
	(void)unused;
	pthread_mutex_lock(&folder_lock);
	folder_job_t *job = folder_result;
	folder_result = NULL;
	pthread_mutex_unlock(&folder_lock);
	if (!job)
		return;
	if (!fp_card_local_current(job->card_epoch) || job->visit != folder_visit || strcmp(job->path, path) || !lv_obj_is_valid(file_list) || lv_screen_active() != filespage_screen) {
		folder_free(job);
		return;
	}
	rebuilding = true;
	lv_obj_clean(file_list);
	empty_label = NULL;
	entries_free();
	entries_epoch = job->card_epoch;
	entries = job->entries;
	entry_count = job->count;
	skipped = job->skipped;
	job->entries = NULL;
	job->count = 0;
	if (!entry_count) {
		empty_label = lv_label_create(file_list);
		lv_label_set_text(empty_label, tr(job->failed ? "files_text_unreadable" : "files_this_folder_is_empty"));
		lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_width(empty_label, lv_pct(100));
		lv_obj_add_style(empty_label, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(empty_label, &font_ui_22, 0);
		lv_obj_set_style_pad_top(empty_label, ui_px(80), 0);
	} else {
		rows_add(ROWS_FIRST);
	}
	rebuilding = false;
	if (entry_count)
		rows_fill();
	folder_free(job);
}
static void folder_retired(lv_event_t *e) {
	(void)e;
	folder_visit++;
	pthread_mutex_lock(&folder_lock);
	folder_free(folder_pending);
	folder_pending = NULL;
	folder_free(folder_result);
	folder_result = NULL;
	pthread_mutex_unlock(&folder_lock);
}
static void rebuild(void) {
	rebuilding = true;
	lv_obj_clean(file_list);
	entries_free();
	title_refresh();
	empty_label = lv_label_create(file_list);
	lv_label_set_text(empty_label, tr("loading"));
	folder_visit++;
	folder_job_t *job = calloc(1, sizeof(*job));
	bool started = false;
	if (job) {
		job->visit = folder_visit;
		job->card_epoch = fp_card_epoch();
		snprintf(job->path, sizeof(job->path), "%s", path);
		pthread_mutex_lock(&folder_lock);
		folder_free(folder_pending);
		folder_pending = job;
		started = folder_busy;
		if (!started) {
			pthread_t worker;
			started = pthread_create(&worker, NULL, folder_worker, NULL) == 0;
			if (started) {
				folder_busy = true;
				pthread_detach(worker);
			} else {
				folder_free(folder_pending);
				folder_pending = NULL;
			}
		}
		pthread_mutex_unlock(&folder_lock);
	}
	if (!started)
		lv_label_set_text(empty_label, tr("files_text_unreadable"));
	rebuilding = false;
}

// The chevron goes up a folder while there is one to go up to, and off the page
// from the card's root -- which is the same thing the music browser does, and
// the only way up that does not need a second control.
static bool back_guard(void) {
	if (!name_layer || !lv_obj_is_hidden(name_layer)) {
		name_layer_hide();
		return true;
	}
	if (pick_layer && !lv_obj_is_hidden(pick_layer)) {
		// Inside the picker the chevron walks back up it, and closes it from the
		// card's root -- the same rule as the page underneath.
		if (strcmp(pick_path, root_path) != 0) {
			char *slash = strrchr(pick_path, '/');
			if (slash && slash != pick_path) {
				*slash = '\0';
				pick_rebuild();
				lv_obj_scroll_to_y(pick_list, 0, LV_ANIM_OFF);
				return true;
			}
		}
		pick_hide();
		return true;
	}
	if (strcmp(path, root_path) == 0) {
		return false;
	}

	char *slash = strrchr(path, '/');
	if (!slash || slash == path) {
		return false;
	}
	*slash = '\0';
	rebuild();
	lv_obj_scroll_to_y(file_list, 0, LV_ANIM_OFF);
	return true;
}

static bool back_guard_peek(void) {
	return (name_layer && !lv_obj_is_hidden(name_layer)) ||
		   (pick_layer && !lv_obj_is_hidden(pick_layer)) || strcmp(path, root_path) != 0;
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	rebuild();
}

void filespage_open(void) {
	snprintf(path, sizeof(path), "%s", root_path);
	switch_screen(filespage_screen);
}

void filespage_init(gui_config_t *cfg) {
	snprintf(root_path, sizeof(root_path), "%s", cfg->sd_root_path ? cfg->sd_root_path : "/");
	snprintf(path, sizeof(path), "%s", root_path);

	lv_obj_add_style(filespage_screen, &theme_style_screen, 0);
	title_label = settingsrow_title(filespage_screen, cfg, "file_explorer");
	settingsrow_title_corner_slots(title_label, cfg, 1);

	// The one thing on this page that is about the folder rather than about an
	// entry in it, so it is a corner button and not a row of the menu.
	lv_obj_t *newdir_btn = lv_btn_create(filespage_screen);
	lv_obj_set_size(newdir_btn, ui_px(56), ui_px(56));
	lv_obj_align(newdir_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_set_style_bg_opa(newdir_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(newdir_btn, 0, 0);
	lv_obj_set_style_shadow_width(newdir_btn, 0, 0);
	lv_obj_set_style_pad_all(newdir_btn, 0, 0);
	lv_obj_add_event_cb(newdir_btn, newdir_btn_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *newdir_glyph = lv_image_create(newdir_btn);
	lv_image_set_src(newdir_glyph, &icon_folder_new);
	lv_obj_add_style(newdir_glyph, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(newdir_glyph, LV_OPA_COVER, 0);
	lv_obj_center(newdir_glyph);

	int content_top = settingsrow_content_top(cfg);

	file_list = lv_obj_create(filespage_screen);
	lv_obj_set_size(file_list, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(file_list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(file_list, 0, 0);
	lv_obj_set_style_border_width(file_list, 0, 0);
	lv_obj_set_style_radius(file_list, 0, 0);
	lv_obj_set_style_pad_hor(file_list, cfg->padding, 0);
	lv_obj_set_style_pad_ver(file_list, 0, 0);
	lv_obj_set_style_pad_gap(file_list, ui_px(8), 0);
	lv_obj_set_scroll_dir(file_list, LV_DIR_VER);
	lv_obj_set_flex_flow(file_list, LV_FLEX_FLOW_COLUMN);
	// Set here and not in the row builder: an empty folder creates no rows, and
	// the page still has to answer the swipe back.
	lv_obj_set_event_bubble(file_list, true);
	lv_obj_add_event_cb(file_list, list_scrolled_cb, LV_EVENT_SCROLL, NULL);

	// --- the naming dialog, in the shape the playlist page uses
	name_layer = lv_obj_create(filespage_screen);
	lv_obj_set_size(name_layer, cfg->screen_width, cfg->screen_height);
	lv_obj_align(name_layer, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(name_layer, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(name_layer, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(name_layer, 0, 0);
	lv_obj_set_style_radius(name_layer, 0, 0);
	lv_obj_set_style_pad_all(name_layer, 0, 0);
	lv_obj_set_scrollable(name_layer, false);
	lv_obj_set_hidden(name_layer, true);

	name_heading = lv_label_create(name_layer);
	lv_label_set_text(name_heading, tr("files_new_folder"));
	lv_obj_add_style(name_heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(name_heading, &font_ui_24, 0);
	lv_obj_align(name_heading, LV_ALIGN_TOP_LEFT, cfg->padding + ui_px(56) + ui_px(14), cfg->padding + cfg->top_bar_height + ui_px(10));

	lv_obj_t *cancel = lv_btn_create(name_layer);
	lv_obj_set_size(cancel, ui_px(56), ui_px(56));
	lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_set_style_pad_all(cancel, 0, 0);
	lv_obj_add_event_cb(cancel, name_cancel_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *cancel_icon = lv_image_create(cancel);
	lv_image_set_src(cancel_icon, &icon_close);
	lv_obj_add_style(cancel_icon, &theme_style_icon, 0);
	lv_obj_center(cancel_icon);

	name_field = lv_textarea_create(name_layer);
	lv_textarea_set_one_line(name_field, true);
	lv_textarea_set_max_length(name_field, NAME_MAX);
	lv_textarea_set_placeholder_text(name_field, tr("name"));
	lv_obj_set_size(name_field, cfg->screen_width - 2 * cfg->padding, ui_px(62));
	lv_obj_set_scrollbar_mode(name_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(name_field, LV_ALIGN_TOP_LEFT, cfg->padding, cfg->padding + cfg->top_bar_height + ui_px(60));
	lv_obj_add_style(name_field, &theme_style_card, 0);
	lv_obj_set_style_radius(name_field, ui_px(12), 0);
	lv_obj_set_style_border_width(name_field, 0, 0);
	lv_obj_set_style_shadow_width(name_field, 0, 0);
	lv_obj_set_style_pad_all(name_field, ui_px(14), 0);
	lv_obj_set_style_text_font(name_field, &font_ui_24, 0);
	keyboard_style_caret(name_field);

	name_keyboard = keyboard_create(name_layer, cfg->screen_width, ui_px(316), name_field, NULL, "ok", name_accept_cb, NULL);

	// --- the destination picker: the same page, folders only, with the button
	// that says "here" pinned to the bottom.
	folderpicker_t destination = folderpicker_create(filespage_screen, cfg,
		"file_explorer", "files_move_here", pick_here_cb, NULL, pick_cancel_cb);
	pick_layer = destination.root;
	pick_title = destination.title;
	pick_list = destination.list;
	pick_here_btn = destination.use;
	pick_here_label = destination.use_label;
	lv_obj_set_hidden(pick_layer, true);

	lv_obj_add_event_cb(filespage_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(filespage_screen, folder_retired, LV_EVENT_SCREEN_UNLOAD_START, NULL);
	lv_obj_add_event_cb(filespage_screen, folder_retired, LV_EVENT_DELETE, NULL);
	switcher_attach_back_gesture(file_list);
	switcher_set_back_guard(filespage_screen, back_guard);
	switcher_set_back_guard_peek(filespage_screen, back_guard_peek);
}

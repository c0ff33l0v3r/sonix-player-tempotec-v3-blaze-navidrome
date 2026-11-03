#ifndef FILEPUBLICATION_H
#define FILEPUBLICATION_H

#include "src/system/db/sqlite3.h"
#include <stdbool.h>
#include <stdint.h>

#define FP_JOURNAL ".sonix-download-publication.db"
#define FP_NAME 160
#define FP_RECORD_LIMIT 128
#define FP_DATABASE_LIMIT (2 * 1024 * 1024)

typedef enum { FP_SAFE, FP_PENDING, FP_UNKNOWN } fp_visibility_t;
typedef enum { FP_INTENT, FP_OWNED, FP_COMPLETE } fp_phase_t;
typedef struct {
	char operation[33], part[FP_NAME], final[FP_NAME], digest[65];
	int64_t bytes, source_device, source_inode, device, inode;
	fp_phase_t phase;
} fp_record_t;

typedef struct {
	sqlite3 *db;
	int directory, admission;
	bool writable, valid;
} fp_guard_t;

// Worker-only, bounded cross-process admission. All cooperating mutations and
// publication intent creation must use this same lock. Never unlink the lock
// file or hold it over a network wait, audio copy/hash, or GUI operation.
// Lock ownership is a descriptor, so process death releases it automatically.
int fp_admission_begin(bool write);
void fp_admission_end(int admission);

// The caller pins the physical directory for this entire operation and owns
// its card lease. Open takes admission; close always releases it, even after a
// failed open. An absent journal is valid for read-only checks and creates no
// card files. Failed/foreign/corrupt/unknown journals are never rebuilt.
bool fp_guard_open(fp_guard_t *guard, int directory, bool write, bool create);
// Same operation under admission already held by this thread/process. The
// guard duplicates it; closing the guard does not release the caller's lease.
bool fp_guard_open_locked(fp_guard_t *guard, int directory, bool write, bool create, int admission);
void fp_guard_close(fp_guard_t *guard);
fp_visibility_t fp_guard_lookup(fp_guard_t *guard, const char *name);
// -1 unknown, 0 no unresolved records, 1 unresolved records.
int fp_guard_pending(fp_guard_t *guard);
bool fp_guard_get(fp_guard_t *guard, const char *operation, fp_record_t *record);
bool fp_guard_final(fp_guard_t *guard, const char *name, fp_record_t *record);
// -1 unknown, 0 unreferenced by pending operations, 1 still needed by one.
int fp_guard_part_pending(fp_guard_t *guard, const char *name);

// Durable metadata primitives, not a full publication protocol. begin precedes
// O_EXCL creation. owned persists the newly created descriptor's identity.
// complete may ONLY be called after that live operation has verified the full
// digest/size, fsync, successful close and current physical pathname identity.
// A recorded inode is never authority to resume/adopt an output after remount.
bool fp_guard_begin(fp_guard_t *guard, const fp_record_t *record);
bool fp_guard_owned(fp_guard_t *guard, const char *operation, int64_t device, int64_t inode);
bool fp_guard_complete(fp_guard_t *guard, const char *operation);
bool fp_guard_retire_complete(fp_guard_t *guard, const char *operation);
// Only after a confirmed absent final path; never clears a possibly-created
// output after an uncertain O_EXCL/commit result.
bool fp_guard_retire_absent(fp_guard_t *guard, const char *operation);
// Only for a certain failed exclusive-create outcome, before any output
// descriptor was obtained. Removes this operation's INTENT, never an owner.
bool fp_guard_failed_create(fp_guard_t *guard, const char *operation);

// Read admission for a physical path, including symlink aliases. Hold the
// returned descriptor through the actual file open, then release it using
// fp_admission_end. Failure never means an absent guard. Worker-only.
int fp_read_begin(const char *path);

// Worker-only mutation admission for a source/target and optional descendant
// trees. The caller must retain the returned descriptor through mutation and
// must not wait for transport or the GUI while holding it. Reads/writes of
// unrelated files remain eligible while protected final copying is active.
int fp_mutation_begin(const char *source, const char *target, bool recursive);
// Same preflight under an exclusive admission already owned by the caller.
// Returns a duplicate lease; close it with fp_admission_end as usual.
int fp_mutation_begin_locked(const char *source, const char *target, bool recursive, int admission);

#endif

#ifndef NAVIDROMEDOWNLOADCOPY_H
#define NAVIDROMEDOWNLOADCOPY_H
#include "navidromedownloadfile.h"
#include "src/system/library/filepublication.h"

typedef struct {
	int directory;
	fp_record_t record;
	bool (*current)(void *user);
	// Optional generation/publication fence around the short durable seal.
	bool (*commit)(bool (*seal)(void *), void *seal_data, void *user);
	void *user;
} nd_download_copy_t;

// Worker only, after media validation and durable account intent. Caller owns
// the physical directory/card lease and WORK_JOB/WORK_STORAGE throughout. No
// network or GUI calls; checks current at bounded copy/hash boundaries. Retains
// source and any pending output on error. Success means durable local COMPLETE,
// not account registration. Caller settles registration and source cleanup.
nd_download_publish_result_t nd_download_copy(nd_download_copy_t *copy);
#endif

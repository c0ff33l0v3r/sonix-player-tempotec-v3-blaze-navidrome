#define _GNU_SOURCE
#include "src/system/library/filepublicationcard.h"
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static atomic_bool finished;
static void *detach(void *unused) {
	(void)unused;
	assert(fp_card_set_root(NULL));
	atomic_store(&finished, true);
	return NULL;
}
int main(int argc, char **argv) {
	assert(argc == 2 || argc == 3);
	if (argc == 3) {
		uint64_t epoch;
		char root[512];
		assert(fp_card_snapshot(&epoch, root) && epoch == strtoull(argv[2], NULL, 10));
		int lease = fp_card_begin(epoch);
		assert(lease >= 0);
		assert(!strcmp(root, argv[1]));
		fp_card_end(lease);
		return 0;
	}
	assert(fp_card_local_begin(0) == -2);
	assert(fp_card_set_root(argv[1]));
	uint64_t epoch = fp_card_epoch();
	assert(epoch && fp_card_current(epoch));
	pid_t child = fork();
	assert(child >= 0);
	if (!child) {
		char number[32];
		snprintf(number, sizeof(number), "%llu", (unsigned long long)epoch);
		execl(argv[0], argv[0], argv[1], number, NULL);
		_exit(127);
	}
	int status;
	assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
	int lease = fp_card_begin(epoch);
	assert(lease >= 0);
	pthread_t worker;
	assert(!pthread_create(&worker, NULL, detach, NULL));
	for (int i = 0; i < 1000 && fp_card_current(epoch); i++)
		usleep(1000);
	assert(!fp_card_current(epoch));
	assert(!atomic_load(&finished));
	assert(fp_card_begin(epoch) < 0);
	fp_card_end(lease);
	pthread_join(worker, NULL);
	assert(atomic_load(&finished));
	uint64_t inactive = fp_card_epoch();
	assert(inactive != epoch && fp_card_begin(inactive) < 0);
	assert(fp_card_set_root(argv[1]));
	uint64_t replacement = fp_card_epoch();
	assert(replacement != inactive && replacement != epoch && fp_card_begin(epoch) < 0);
	lease = fp_card_begin(replacement);
	assert(lease >= 0);
	fp_card_end(lease);
	puts("PASS card lifetime: separate CGI, invalidate before drain, stale replacement rejection");
	int ready[2];
	assert(!pipe(ready));
	child = fork();
	assert(child >= 0);
	if (!child) {
		close(ready[0]);
		assert(fp_card_begin(replacement) >= 0);
		assert(write(ready[1], "x", 1) == 1);
		for (;;)
			pause();
	}
	close(ready[1]);
	char byte;
	assert(read(ready[0], &byte, 1) == 1);
	close(ready[0]);
	atomic_store(&finished, false);
	assert(!pthread_create(&worker, NULL, detach, NULL));
	for (int i = 0; i < 1000 && fp_card_current(replacement); i++)
		usleep(1000);
	assert(!fp_card_current(replacement) && !atomic_load(&finished));
	assert(!kill(child, SIGKILL));
	assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
	pthread_join(worker, NULL);
	assert(atomic_load(&finished));
	puts("PASS card lifetime: killed handler releases lifetime lease");
	return 0;
}

// Production GUI bridge only; unused page construction is discarded at link.
#include "../src/gui/shell/gui.c"
#include <assert.h>
#include <poll.h>
bool wakeprof_on;
void wakeprof_note_post(void *cb, const char *thread) {
	(void)cb;
	(void)thread;
}
static unsigned ordinary, noticed;
static void normal(void *unused) {
	(void)unused;
	ordinary++;
}
static void notice(void *unused) {
	(void)unused;
	noticed++;
}
static void *notify(void *unused) {
	(void)unused;
	for (int i = 0; i < 10000; i++)
		assert(gui_post_signal(notice, NULL));
	return NULL;
}
int main(void) {
	post_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	assert(post_wake_fd >= 0);
	for (int i = 0; i < GUI_POST_MAX; i++)
		assert(gui_post(normal, NULL));
	assert(!gui_post(normal, NULL));
	pthread_t thread;
	assert(!pthread_create(&thread, NULL, notify, NULL));
	pthread_join(thread, NULL);
	struct pollfd fd = {.fd = post_wake_fd, .events = POLLIN};
	assert(poll(&fd, 1, 0) == 1);
	post_drain_cb(NULL);
	assert(ordinary == GUI_POST_MAX && noticed == 1 && !post_count);
	assert(poll(&fd, 1, 0) == 0);
	for (int i = 0; i < 100; i++)
		post_drain_cb(NULL);
	assert(noticed == 1); // no timer/poll-driven repeated completion callback
	close(post_wake_fd);
	puts("Production GUI overflow recovery: 10000 worker notices coalesce, survive a full post queue and leave the doorbell idle.");
}

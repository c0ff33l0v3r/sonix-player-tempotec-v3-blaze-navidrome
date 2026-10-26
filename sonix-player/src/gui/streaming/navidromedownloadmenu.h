#ifndef NAVIDROMEDOWNLOADMENU_H
#define NAVIDROMEDOWNLOADMENU_H
#include <stdbool.h>
// GUI thread. Captures the current exact source; ordinary Music returns false.
bool navidromedownloadmenu_bind(const char *path);
void navidromedownloadmenu_activate(void *unused);
#endif

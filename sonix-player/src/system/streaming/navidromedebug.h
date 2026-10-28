#ifndef NAVIDROMEDEBUG_H
#define NAVIDROMEDEBUG_H
#include "src/system/core/diagnostic.h"
// Fixed, already-redacted diagnostics use the general logger only.
void nd_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
#endif

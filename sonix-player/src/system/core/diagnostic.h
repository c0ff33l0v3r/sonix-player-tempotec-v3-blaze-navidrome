#ifndef SONIX_DIAGNOSTIC_H
#define SONIX_DIAGNOSTIC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Only local counters and numeric measurements cross this interface. No strings
// from media, configuration or a server can enter a diagnostic record.
typedef enum { D_SERVICE, D_SYNC, D_STORE, D_PLAY, D_AUDIO, D_ART, D_BROWSE, D_PLAYLIST, D_SEARCH, D_DISCOVERY, D_DOWNLOAD, D_OFFLINE, D_STAR, D_ACTION, D_LYRICS, D_REPORT, D_SESSION, D_HTTP, D_GUI, D_POWER, D_CACHE, D_VAULT, D_COMPONENTS } diagnostic_component_t;
typedef enum { D_BEGIN, D_END, D_SNAPSHOT, D_QUEUED, D_WORK, D_ADMISSION, D_REQUEST, D_RESPONSE, D_PARSE, D_PREPARE, D_EXECUTE, D_COMMIT, D_VALIDATE, D_PUBLISH, D_CLEANUP, D_DELIVER, D_BIND, D_VISIBLE, D_CACHE_HIT, D_CACHE_MISS, D_PRIORITY_WAIT, D_RETRY, D_RETIRED, D_DECODE, D_PCM, D_FIRST_WRITE, D_UNDERRUN, D_PAUSE, D_STOP, D_INTENT, D_PROGRESS, D_PROBE, D_FLUSH, D_REGISTER, D_RECOVERY, D_RELEASE, D_QUERY, D_DISPLAY, D_EVENTS } diagnostic_event_t;
typedef enum { D_UNKNOWN, D_OK, D_FAILED, D_CANCELLED, D_SUPERSEDED, D_PENDING, D_UNAVAILABLE, D_UNCERTAIN, D_OUTCOMES } diagnostic_outcome_t;
typedef enum { D_KIND, D_SERIAL, D_COUNT, D_OFFSET, D_LIMIT, D_BYTES, D_DURATION_US, D_WAIT_US, D_PREPARE_US, D_EXECUTE_US, D_COMMIT_US, D_STATUS, D_ERRNO, D_SOURCE, D_FLAGS, D_VISIBLE_ROWS, D_TOTAL, D_RETRIES, D_WIDTH, D_HEIGHT, D_TARGET, D_STRATEGY, D_FS_MAGIC, D_SYSCALL, D_BUFFERED, D_RATE, D_BITS, D_REASON, D_QUEUE_US, D_DELIVERY_US, D_BLANK_US, D_ENTITY_US, D_RELATION_US, D_KERNEL, D_BUILD, D_FIELDS } diagnostic_field_t;
typedef struct {
	diagnostic_field_t key;
	int64_t value;
} diagnostic_value_t;
typedef struct {
	uint32_t id, parent, capture;
	diagnostic_component_t component;
	uint64_t start_us;
} diagnostic_t;

// Small headless tools can omit the logger. Production links logging.o directly;
// diagnostic tests link it too, rather than substituting an event mock.
#ifndef SONIX_DIAGNOSTIC_IMPLEMENTATION
#define D_OPTIONAL __attribute__((weak))
#define D_AVAILABLE(fn) ((fn) != NULL)
#else
#define D_OPTIONAL
#define D_AVAILABLE(fn) true
#endif
bool logging_diagnostic_enabled(void) D_OPTIONAL;
diagnostic_t logging_diagnostic_begin(diagnostic_component_t, uint32_t) D_OPTIONAL;
void logging_diagnostic_event(diagnostic_t, diagnostic_event_t, diagnostic_outcome_t, const diagnostic_value_t *, size_t) D_OPTIONAL;
uint64_t logging_diagnostic_clock(void) D_OPTIONAL;
static inline bool diagnostic_enabled(void) { return D_AVAILABLE(logging_diagnostic_enabled) && logging_diagnostic_enabled(); }
static inline uint64_t diagnostic_clock(void) { return diagnostic_enabled() && D_AVAILABLE(logging_diagnostic_clock) ? logging_diagnostic_clock() : 0; }
static inline int64_t diagnostic_since(uint64_t start) {
	uint64_t now = diagnostic_clock();
	return start && now >= start ? (int64_t)(now - start) : -1;
}
static inline diagnostic_t diagnostic_begin(diagnostic_component_t c, uint32_t parent) { return D_AVAILABLE(logging_diagnostic_begin) ? logging_diagnostic_begin(c, parent) : (diagnostic_t){0}; }
static inline void diagnostic_emit(diagnostic_t c, diagnostic_event_t e, diagnostic_outcome_t o, const diagnostic_value_t *v, size_t n) {
	if (D_AVAILABLE(logging_diagnostic_event))
		logging_diagnostic_event(c, e, o, v, n);
}
#define DV(key, value) ((diagnostic_value_t){(key), (int64_t)(value)})
#define DIAG(ctx, event, outcome, ...) \
	do { \
		if (diagnostic_enabled()) { \
			const diagnostic_value_t _dv[] = {DV(D_FIELDS, 0), ##__VA_ARGS__}; \
			diagnostic_emit((ctx), (event), (outcome), _dv + 1, sizeof(_dv) / sizeof(_dv[0]) - 1); \
		} \
	} while (0)

// Thread-local nesting only supplies synchronous parentage. Async owners must
// copy diagnostic_t with their jobs and attach it on the destination thread.
diagnostic_t logging_diagnostic_current(void) D_OPTIONAL;
diagnostic_t logging_diagnostic_attach(diagnostic_t) D_OPTIONAL;
static inline diagnostic_t diagnostic_current(void) { return D_AVAILABLE(logging_diagnostic_current) ? logging_diagnostic_current() : (diagnostic_t){0}; }
// Classify local application error keys; never pass their text to the sink.
static inline diagnostic_outcome_t diagnostic_error_outcome(const char *error) {
	if (!error)
		return D_OK;
	if (!strcmp(error, "nd_sync_cancelled") || !strcmp(error, "nd_audio_cancelled") || !strcmp(error, "nd_favorites_cancelled"))
		return D_CANCELLED;
	if (!strcmp(error, "nd_pl_stale") || !strcmp(error, "nd_favorites_stale") || !strcmp(error, "nd_queue_changed"))
		return D_SUPERSEDED;
	if (!strcmp(error, "nd_pl_verify_needed"))
		return D_UNCERTAIN;
	return D_FAILED;
}
typedef struct {
	diagnostic_t context, previous;
	diagnostic_outcome_t outcome;
	const char *const *error;
	bool owned;
} diagnostic_scope_t;
static inline diagnostic_scope_t diagnostic_scope(diagnostic_component_t c) {
	diagnostic_scope_t s = {.previous = diagnostic_current(), .outcome = D_UNKNOWN, .owned = true};
	s.context = diagnostic_begin(c, s.previous.id);
	if (D_AVAILABLE(logging_diagnostic_attach))
		logging_diagnostic_attach(s.context);
	return s;
}
static inline diagnostic_scope_t diagnostic_attach(diagnostic_t c) {
	diagnostic_scope_t s = {.context = c, .previous = diagnostic_current()};
	if (D_AVAILABLE(logging_diagnostic_attach))
		logging_diagnostic_attach(c);
	return s;
}
static inline void diagnostic_scope_end(diagnostic_scope_t *s) {
	if (s->owned)
		DIAG(s->context, D_END, s->error ? diagnostic_error_outcome(*s->error) : s->outcome);
	if (D_AVAILABLE(logging_diagnostic_attach))
		logging_diagnostic_attach(s->previous);
}
#define DIAGNOSTIC_SCOPE(component) diagnostic_scope_t _diagnostic_scope __attribute__((cleanup(diagnostic_scope_end))) = diagnostic_scope(component)
#define DIAGNOSTIC_ATTACH(context) diagnostic_scope_t _diagnostic_scope __attribute__((cleanup(diagnostic_scope_end))) = diagnostic_attach(context)
#define DIAGNOSTIC_RETURN_BOOL(expr) \
	do { \
		__auto_type _diagnostic_value = (expr); \
		_diagnostic_scope.outcome = _diagnostic_value ? D_OK : D_FAILED; \
		return _diagnostic_value; \
	} while (0)
#define DIAGNOSTIC_RETURN_ERROR(expr) \
	do { \
		const char *_diagnostic_value = (expr); \
		_diagnostic_scope.outcome = diagnostic_error_outcome(_diagnostic_value); \
		return _diagnostic_value; \
	} while (0)
#endif

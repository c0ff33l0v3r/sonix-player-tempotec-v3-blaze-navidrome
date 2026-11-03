#ifndef FILEPUBLICATIONCARD_H
#define FILEPUBLICATIONCARD_H
#include <stdbool.h>
#include <stdint.h>

// Storage-worker lifecycle fence. Invalidates jobs first, drains open local
// operations across CGI processes, then publishes the new physical card root.
// Call before the corresponding Navidrome root update/unmount or root attach.
bool fp_card_set_root(const char *root);
// GUI-safe process-local generation snapshot; no filesystem access.
uint64_t fp_card_epoch(void);
// Worker-only IPC snapshot for CGI. Never returns authentication material.
bool fp_card_snapshot(uint64_t *epoch, char root[512]);
// Cross-process local-I/O lease. Do not retain over network waits. The returned
// descriptor must outlive every descriptor opened under it, then be closed.
int fp_card_begin(uint64_t epoch);
void fp_card_end(int lease);
bool fp_card_current(uint64_t epoch);
// Application-only adapter: unregistered standalone consumers need no selected
// card lifetime. Production registers storage before opening its GUI pages.
int fp_card_local_begin(uint64_t epoch);
// GUI-safe validity test; unregistered isolated consumers remain usable.
bool fp_card_local_current(uint64_t epoch);
// Wi-Fi lifecycle is independent of card/local publication. Disabling refuses
// subsequent CGI chunks; draining waits only for already-admitted local I/O.
bool fp_transfer_set_enabled(bool enabled);
bool fp_transfer_snapshot(uint64_t *epoch, uint64_t *session, char root[512]);
int fp_transfer_begin(uint64_t epoch, uint64_t session);
bool fp_card_drain(void);
#endif

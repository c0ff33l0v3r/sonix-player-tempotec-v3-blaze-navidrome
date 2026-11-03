#ifndef WEBCGI_H
#define WEBCGI_H

#include <stdbool.h>

// The player also serves guarded Wi-Fi CGI endpoints without opening a GUI.
// Listing supplies a card epoch; mutations/downloads require that epoch. File
// descriptors and cross-process admission are released before transport waits.
// Stock binaries/static card routes are excluded from the private web root.

// True when this process was started by a web server rather than by the user.
// Call it first thing in main(); everything else in the player assumes it is
// the player.
bool webcgi_should_run(int argc, char **argv);

// Answers the request on stdout and returns the process exit code. Only
// meaningful when webcgi_should_run() said yes.
int webcgi_main(void);

#endif /* WEBCGI_H */

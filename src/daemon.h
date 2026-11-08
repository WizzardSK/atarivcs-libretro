/* A stand-in for AtariOS's bundle bridge daemon (daemon.c) */
#ifndef ATARIVCS_DAEMON_H
#define ATARIVCS_DAEMON_H

#include <stdbool.h>

/* Serves the socket in a runtime folder of its own; store and name are the
 * bundle's StoreID and Name. log gets a line per event. */
bool atarivcs_daemon_start(const char *store, const char *name, void (*log)(const char *));
/* That folder, for the game's XDG_RUNTIME_DIR; NULL when not started */
const char *atarivcs_daemon_runtime_dir(void);
void atarivcs_daemon_stop(void);

#endif

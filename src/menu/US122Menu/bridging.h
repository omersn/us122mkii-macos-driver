/* Bridging header: exposes the C shim to the Swift app. */
#ifndef US122MENU_BRIDGING_H
#define US122MENU_BRIDGING_H

int us122_shm_open_rw(const char *name);

#endif

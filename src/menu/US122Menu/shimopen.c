/* shimopen.c — non-variadic wrapper around shm_open.
 *
 * Swift cannot call C variadic functions, and Darwin declares shm_open as
 * variadic (the optional mode arg), so Swift marks the whole symbol
 * unavailable. This wrapper exposes a fixed-signature entry point Swift can
 * call. We only ever OPEN an existing segment (no O_CREAT), so no mode arg. */
#include <sys/mman.h>
#include <fcntl.h>

int us122_shm_open_rw(const char *name){
    return shm_open(name, O_RDWR);
}

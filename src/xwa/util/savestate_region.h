#ifndef XWA_UTIL_SAVESTATE_REGION_H
#define XWA_UTIL_SAVESTATE_REGION_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* In-mission save states (xwa_runtime/runtime/flight_savestate.c): a module
 * that keeps simulation state in file-static variables hands each of them to
 * the save state through one of these, by address and size. The save state
 * copies the bytes as they are (same process, same mission). */
typedef void (*XwaSaveStateRegionFn)(void* ctx, void* addr, size_t size, const char* name);

#ifdef __cplusplus
}
#endif

#endif

#ifndef XWA_REMASTER_VR_KTX2_DECODE_H
#define XWA_REMASTER_VR_KTX2_DECODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A KTX2 image (Basis Universal UASTC / ETC1S, Zstandard supercompressed or
 * not) as RGBA8, rows top to bottom; free() it. NULL when it is not one. */
uint8_t* XwaKtx2_DecodeRgba(const void* data, size_t size, int* width, int* height);

#ifdef __cplusplus
}
#endif

#endif

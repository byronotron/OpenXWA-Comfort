#ifndef XWA_REMASTER_OPT_MESH_H
#define XWA_REMASTER_OPT_MESH_H

#include "aeron/asset/flight_model.h"
#include "aeron/vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool XwaRemasterOptMesh_Init(AeronVfs* vfs, char* error, size_t error_size);

bool XwaRemasterOptMesh_Build(AeronVfs* vfs, const char* basename,
							  float smooth_angle_degrees, float emissive_strength,
							  AeronFlightModel* out, char* error, size_t error_size);

/* XwaRemasterOptMesh_Build in two parts: the file read (main thread: the
 * VFS is not thread-safe) and the conversion (any thread). */
bool XwaRemasterOptMesh_Read(AeronVfs* vfs, const char* basename, char* path, size_t path_size,
							 uint8_t** out_bytes, size_t* out_size);
bool XwaRemasterOptMesh_BuildFromMemory(const uint8_t* bytes, size_t size, const char* path,
										float smooth_angle_degrees, float emissive_strength,
										AeronFlightModel* out, char* error, size_t error_size);

#endif

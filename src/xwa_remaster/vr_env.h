#ifndef XWA_REMASTER_VR_ENV_H
#define XWA_REMASTER_VR_ENV_H

/*
 * VR environment for the 2D screens (concourse, briefings, menus, films):
 * the game's own carrier hangar (Hangar.opt and its props, laid out like
 * the in-mission hangar) built from the installed game data, drawn in
 * stereo around the flat virtual screen.
 *
 * Space: XWA world axes (x right, y forward, z up) in world units, with
 * the hangar's origin (the classic launch reference) at 0.
 */

#include "aeron/render.h"
#include "aeron/scene/scene3d.h"

#ifdef __cplusplus
extern "C" {
#endif

/* XWA_XR_ENVIRONMENT (default on). */
int XwaVrEnv_Enabled(void);

/* Loads the environment a piece per call (no open pass on `cmd`) so menus
 * never stall. Returns 1 once the hangar itself can be drawn. */
int XwaVrEnv_Prepare(AeronCommandBuffer* cmd);

/* The seated viewer: head position (world units) and world -> eye rows
 * (engine eye space: x right, y down, z forward). */
void XwaVrEnv_View(float pos[3], float rows[9]);

/* Submits the loaded meshes as scene instances. */
void XwaVrEnv_Submit(AeronScene3D* scene);

/* Frees the GPU meshes (the flight scene loads its own hangar). */
void XwaVrEnv_Release(void);

#ifdef __cplusplus
}
#endif

#endif

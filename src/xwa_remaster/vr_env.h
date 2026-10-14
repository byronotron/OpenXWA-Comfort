#ifndef XWA_REMASTER_VR_ENV_H
#define XWA_REMASTER_VR_ENV_H

/*
 * VR environment for the 2D screens (concourse, briefings, menus, films),
 * built from the installed game data and drawn in stereo around the flat
 * virtual screen. XWA_XR_ENVIRONMENT selects it:
 *   endor (default) — a still of the Battle of Endor after the box art:
 *                     game craft, the Death Star II and Endor sprites, the
 *                     logo above the screen (vr/logo.rgba, shipped in the
 *                     APK assets);
 *   hangar          — the carrier hangar (Hangar.opt and its props, laid
 *                     out like the in-mission hangar);
 *   0               — off.
 *
 * Space: XWA world axes (x right, y forward, z up) in world units; the
 * hangar's origin is the classic launch reference, the Endor scene's the
 * viewer.
 */

#include "aeron/render.h"
#include "aeron/scene/scene3d.h"

#ifdef __cplusplus
extern "C" {
#endif

/* XWA_XR_ENVIRONMENT (default on). */
int XwaVrEnv_Enabled(void);

/* The space scene (lit by a sun, not the hangar lights) is selected. */
int XwaVrEnv_IsSpace(void);

/* Loads the environment a piece per call (no open pass on `cmd`) so menus
 * never stall. Returns 1 once the hangar itself can be drawn. */
int XwaVrEnv_Prepare(AeronCommandBuffer* cmd);

/* The seated viewer: head position (world units) and world -> eye rows
 * (engine eye space: x right, y down, z forward). */
void XwaVrEnv_View(float pos[3], float rows[9]);

/* Submits the loaded meshes as scene instances. */
void XwaVrEnv_Submit(AeronScene3D* scene);

/* The screen frame as a compositor layer right over the virtual screen
 * (the picture fills its opening, just behind it). Call after the
 * backdrop is committed; `brightness` (0..1) dims it with the scene.
 * Returns 0 when the frame has no layer (it is then drawn in the scene). */
int XwaVrEnv_CommitFrameLayer(AeronCommandBuffer* cmd, float brightness);

/* The flat rectangle the compositor layers cover in the menus (the frame,
 * or the bare screen without one), facing the viewer: centre and size in
 * metres, LOCAL space (x right, y up, -z ahead). */
int XwaVrEnv_LayerRect(float centre[3], float size[2]);

/* Frees the GPU meshes (the flight scene loads its own hangar). */
void XwaVrEnv_Release(void);

#ifdef __cplusplus
}
#endif

#endif

#ifndef XWA_REMASTER_VR_HUD_OCCLUSION_H
#define XWA_REMASTER_VR_HUD_OCCLUSION_H

/*
 * The world HUD (reticle, target brackets, arrows) is a compositor quad far
 * out in front, so the compositor draws it over everything, the cockpit
 * included. This cuts the cockpit's outline, seen from the eyes, out of the
 * world HUD image, so the reticle disappears behind struts and the dash
 * like a real reflector sight would. Only the cockpit occludes it, never
 * the ships being aimed at. XWA_XR_HUD_OCCLUDE=0 turns it off.
 */

#include "aeron/render.h"
#include "aeron/scene/mesh.h"
#include "aeron/scene/scene3d.h"
#include "aeron/xr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Frame start: forget last frame's cockpit. */
void XwaVrHudOcclusion_Reset(void);

/* The cockpit drawn this frame: its mesh, model matrix (row-major, mesh
 * metres -> scene units), mesh table (may be NULL), the base camera's
 * world -> eye rows and position, and scene units per LOCAL metre. */
void XwaVrHudOcclusion_SetCockpit(const AeronSceneMesh* mesh, const float model[16], const AeronSceneMeshTable* table,
								  const float camera_rows[9], const float camera_pos[3], float units_per_m);

/* Builds this frame's mask for the world HUD quad (LOCAL space) seen by
 * the two eyes, for a target_w x target_h HUD image. No pass may be open on
 * `cmd`. Returns 1 when there is something to cut. */
int XwaVrHudOcclusion_Prepare(AeronCommandBuffer* cmd, const AeronXrView views[2], const AeronXrHudQuad* quad,
							  int target_w, int target_h);

/* Erases the prepared mask from the HUD image (inside its pass). */
void XwaVrHudOcclusion_Render(AeronCommandBuffer* cmd, AeronRenderPass* pass, AeronRenderTarget* target);

void XwaVrHudOcclusion_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

#ifndef XWA_REMASTER_VR_COCKPIT_HUD_H
#define XWA_REMASTER_VR_COCKPIT_HUD_H

/*
 * VR cockpit HUD: the HUD's instruments as separate holographic panels
 * placed on the cockpit (radar pods beside the canopy, targeting computer
 * on the dash, MFDs on the side consoles, ...) instead of one sheet in
 * front of the eyes. The HUD is still drawn as usual into the head HUD
 * target; each panel is a rectangle of that target shown as its own
 * OpenXR quad (Aeron_XrStereoCommitPanels). Reticle, brackets and target
 * arrows stay on the far world quad.
 */

#include "aeron/render.h"
#include "aeron/xr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Finds the panel rectangles in this frame's prepared HUD (call after
 * XwaRemasterHud_PrepareFrame for a target_w x target_h HUD) and fills
 * `out` with their cockpit placements. With `cmd_separate` the targeting
 * computer was drawn to the aux HUD target (XwaRemasterHud_SetCmdSplit). `world_scale` is XWA_XR_WORLD_SCALE:
 * panels move and scale with the cockpit. Returns the panel count. */
int XwaVrCockpitHud_Build(int target_w, int target_h, float world_scale, int cmd_separate, AeronXrPanel* out,
						  int max_out);

/* Holographic backplates for the panels found by the last Build, drawn
 * into the head HUD target under the HUD. Prepare outside a render pass;
 * Render inside the HUD pass, before the HUD itself. */
void XwaVrCockpitHud_PrepareHolo(AeronCommandBuffer* cmd, int target_w, int target_h);
void XwaVrCockpitHud_RenderHolo(AeronCommandBuffer* cmd, AeronRenderPass* pass, AeronRenderTarget* target, int aux);

void XwaVrCockpitHud_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

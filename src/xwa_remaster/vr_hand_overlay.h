#ifndef XWA_REMASTER_VR_HAND_OVERLAY_H
#define XWA_REMASTER_VR_HAND_OVERLAY_H

/*
 * Hands in front of the menu screen and the flight HUD. The virtual
 * screen and its frame, and in flight the HUD panels, the helmet HUD and
 * the world HUD (reticle), are compositor layers over the 3D view, so they
 * covered the gloves / controllers reaching in front of them. The eye
 * images are shown a second time over those layers, only in the hands'
 * box (Aeron_XrStereoCommitEyeOverlayRect), with an alpha that is 1 only
 * on the hands: their outline seen from each eye, built here.
 */

#include "aeron/render.h"
#include "aeron/scene/draw_list2d.h"
#include "aeron/xr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* In flight the layers to come out in front of are the HUD's (cockpit
 * panels, the helmet HUD, the world HUD with the reticle), given here
 * before Prepare, in LOCAL space (position, orientation, size of each
 * quad). The hands stay behind a panel they reach through. Without this
 * call the menu screen and its frame are the layers. Used by one Prepare. */
void XwaVrHandOverlay_SetLayers(const AeronXrPanel* quads, int count);

/* After the gloves were submitted for this frame, with no pass open on
 * `cmd`: builds each eye's alpha mask (eye images w x h). Returns 1 when a
 * glove is in view of either eye over a layer (then the overlay is worth
 * showing). */
int XwaVrHandOverlay_Prepare(AeronCommandBuffer* cmd, const AeronXrView views[2], int w, int h);

/* Eye `eye`'s alpha list, to draw into the eye image after its colour
 * (it writes only the alpha, inside Rect); NULL when there is none. */
AeronDrawList2D* XwaVrHandOverlay_List(int eye);

/* The part of each eye image the overlay needs ({ x, y, w, h }: the hands'
 * box), for Aeron_XrStereoCommitEyeOverlayRect. */
void XwaVrHandOverlay_Rects(int rect[2][4]);

void XwaVrHandOverlay_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

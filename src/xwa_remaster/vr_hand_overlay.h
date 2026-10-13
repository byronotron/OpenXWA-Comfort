#ifndef XWA_REMASTER_VR_HAND_OVERLAY_H
#define XWA_REMASTER_VR_HAND_OVERLAY_H

/*
 * Hands in front of the menu screen. The virtual screen and its frame are
 * compositor layers over the 3D view, so they covered the gloves reaching
 * in front of them. The eye images are shown a second time over those
 * layers (Aeron_XrStereoCommitEyeOverlay) with an alpha that is 1 only
 * on the gloves: their outline seen from each eye, built here.
 */

#include "aeron/render.h"
#include "aeron/scene/draw_list2d.h"
#include "aeron/xr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* After the gloves were submitted for this frame, with no pass open on
 * `cmd`: builds each eye's alpha mask (eye images w x h). Returns 1 when a
 * glove is in view of either eye (then the overlay is worth showing). */
int XwaVrHandOverlay_Prepare(AeronCommandBuffer* cmd, const AeronXrView views[2], int w, int h);

/* Eye `eye`'s alpha list, to draw into the eye image after its colour;
 * NULL when there is none this frame. */
AeronDrawList2D* XwaVrHandOverlay_List(int eye);

void XwaVrHandOverlay_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

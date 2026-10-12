#ifndef XWA_REMASTER_VR_MENU_BEAM_H
#define XWA_REMASTER_VR_MENU_BEAM_H

/*
 * VR menus: a laser beam from the right hand (the glove's index fingertip)
 * to where its aim ray meets the virtual screen, so the cursor is easy to
 * follow. The beam is a thin quad layer drawn over the screen.
 */

#include "aeron/render.h"
#include "aeron/xr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Draws the beam into `target` (the stereo frame's console target, free in
 * menus) and fills `out` with its quad. Returns 1 when the beam is shown;
 * commit it with Aeron_XrStereoCommitConsole. */
int XwaVrMenuBeam_Frame(AeronCommandBuffer* cmd, AeronRenderTarget* target, int width, int height,
						const AeronXrView views[2], AeronXrPanel* out);

void XwaVrMenuBeam_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

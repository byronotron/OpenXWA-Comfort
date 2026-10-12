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

/* Draws the beam strips into `target` (the stereo frame's beam target)
 * and fills `out` with the beam's quad. Returns 1 when the beam is shown;
 * commit it with Aeron_XrStereoCommitBeams. */
int XwaVrMenuBeam_Frame(AeronCommandBuffer* cmd, AeronRenderTarget* target, int width, int height,
						const AeronXrView views[2], AeronXrPanel* out);

/* Shared beam pieces: the strips (bright and faint) drawn into a beam
 * target (at least 96 px wide), and a beam's quad from `start` to `end`
 * (LOCAL space) facing the eyes. */
int XwaVrBeam_DrawStrips(AeronCommandBuffer* cmd, AeronRenderTarget* target, int width, int height);
int XwaVrBeam_Pose(const float start[3], const float end[3], const AeronXrView views[2], int faint, int height,
				   AeronXrPanel* out);

void XwaVrMenuBeam_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

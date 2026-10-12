#ifndef XWA_REMASTER_VR_GLOVES_H
#define XWA_REMASTER_VR_GLOVES_H

/*
 * VR pilot gloves: two dark grey flight gloves that follow the Touch
 * controllers, posed from the trigger, grip and the controllers' finger
 * touch sensors (index off the trigger = pointing). The right glove wears
 * the family crest pin on the back of the hand above the thumb.
 *
 * The gloves are built from rigid pieces (palm, cuff, finger segments and
 * joints) drawn as scene mesh instances with the cockpit, so they are lit
 * and seen in stereo like it.
 */

#include "aeron/render.h"
#include "aeron/scene/scene3d.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Updates the hand poses and submits the glove instances. `crows` are the
 * ship camera's world->eye rows, `cam_pos` its position and `units` the
 * scene units per metre of the player's space (world scale applied).
 * `cmd` must have no open pass (meshes are created on first use). */
void XwaVrGloves_Submit(AeronScene3D* scene, AeronCommandBuffer* cmd, const float crows[9], const float cam_pos[3],
						float units);

/* Hides the gloves this frame (not in a stereo cockpit view). */
void XwaVrGloves_Idle(void);

/* The pose of the last submitted frame, in OpenXR LOCAL space (metres). */
typedef struct XwaVrGloveCapsule {
	float a[3], b[3];
	float radius;
} XwaVrGloveCapsule;

/* Index fingertip of hand 0 (left) / 1 (right). Returns 0 when the hand is
 * not shown. `pointing` is set when the index finger is off the trigger. */
int XwaVrGloves_IndexTip(int hand, float out[3], int* pointing);

/* The hand's shape as capsules (palm, fingers, cuff). Returns the count. */
int XwaVrGloves_Capsules(int hand, XwaVrGloveCapsule* out, int max_out);

void XwaVrGloves_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

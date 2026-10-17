#ifndef XWA_REMASTER_VR_CONTROLLERS_H
#define XWA_REMASTER_VR_CONTROLLERS_H

/*
 * VR controller models (VR Options > Hands: Controllers), shown in place of
 * the pilot gloves. Where the headset's runtime has its own model of the
 * controller (Quest: XR_FB_render_model; SteamVR / Pico 4 Ultra:
 * XR_EXT_interaction_render_model, with moving buttons) that one is drawn;
 * otherwise a generic controller of our own. They are scene meshes like
 * the gloves (lit with the cockpit, seen in stereo).
 *
 * The labels beside them (vr_lap_console.c) point at the controls this
 * module locates on the model (XwaVrControllers_View).
 */

#include "aeron/render.h"
#include "aeron/scene/scene3d.h"
#include "xwa_remaster/vr_gloves.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The controls a label can point at. */
enum {
	XWA_VR_CTL_TRIGGER,
	XWA_VR_CTL_GRIP,
	XWA_VR_CTL_UPPER, /* B / Y */
	XWA_VR_CTL_LOWER, /* A / X */
	XWA_VR_CTL_STICK,
	XWA_VR_CTL_MENU,
	XWA_VR_CTL_THUMBREST,
	XWA_VR_CTL_COUNT
};

/* Where a shown controller's parts are this frame, OpenXR LOCAL space. */
typedef struct XwaVrControllerView {
	float anchor[XWA_VR_CTL_COUNT][3]; /* on each control */
	float face_center[3];              /* the middle of the button face */
	float face_normal[3];              /* the way the button face looks */
	float outline[8][3];               /* points around the body */
	int outline_count;
	int runtime_model; /* the headset's own model is shown */
} XwaVrControllerView;

void XwaVrControllers_Submit(AeronScene3D* scene, AeronCommandBuffer* cmd, const float crows[9],
							 const float cam_pos[3], float units);
void XwaVrControllers_Idle(void);
/* Hand 0 (left) / 1 (right). Returns 0 when it is not shown. */
int XwaVrControllers_View(int hand, XwaVrControllerView* out);
/* For the panels' cut-outs and the flight stick (as the gloves' ones):
 * the body as capsules (the last one is the handle, where the palm is),
 * and a mesh in LOCAL space. */
int XwaVrControllers_Capsules(int hand, XwaVrGloveCapsule* out, int max_out);
int XwaVrControllers_Mesh(int hand, const float** local_xyz, const float** local_normals, int* vertex_count,
						  const uint16_t** indices, int* index_count);
void XwaVrControllers_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

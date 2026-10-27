#ifndef XWA_VR_PILOT_SPACE_H
#define XWA_VR_PILOT_SPACE_H

#include "aeron/aeron.h"
#include "aeron/xr.h"

/* Translation of the simulated pilot in cockpit LOCAL axes (metres).
 * Tracking poses remain untouched; consumers work on translated copies.
 * Set/read only on the render thread. */
void XwaVrPilotSpace_Set(const float local_m[3]);
void XwaVrPilotSpace_Reset(void);
const float* XwaVrPilotSpace_Offset(void);
void XwaVrPilotSpace_ToCockpit(float p[3]);
void XwaVrPilotSpace_ToTracking(float p[3]);
void XwaVrPilotSpace_Views(const AeronXrView in[2], AeronXrView out[2]);
void XwaVrPilotSpace_Panels(AeronXrPanel* panels, int count);
void XwaVrPilotSpace_CacheLayer(const AeronXrPanel* presented,int cockpit_fixed,
	AeronXrPanel* cached,uint8_t* cockpit_flag);
void XwaVrPilotSpace_LayerInCockpit(const AeronXrPanel* cached,int cockpit_fixed,AeronXrPanel* out);
int XwaVrPilotSpace_GetHand(int hand, AeronXrHand* out);
int XwaVrPilotSpace_LocateRenderModel(int hand, float position[3], float orientation[4],
	AeronXrRenderModelNode* nodes, uint32_t capacity);

/* Engine view axes are right/down/forward; OpenXR LOCAL is right/up/back. */
static inline void XwaVrPilotSpace_WorldToLocal(const float rows[9], const float world_m[3], float out[3]) {
	for(int a=0;a<3;a++) {
		float v=rows[a*3]*world_m[0]+rows[a*3+1]*world_m[1]+rows[a*3+2]*world_m[2];
		out[a]=a ? -v : v;
	}
}

#endif

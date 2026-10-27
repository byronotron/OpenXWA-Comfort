#include "xwa_remaster/vr_pilot_space.h"
#include <math.h>
#include <string.h>

static float s_offset[3];

void XwaVrPilotSpace_Reset(void) { memset(s_offset,0,sizeof s_offset); }
void XwaVrPilotSpace_Set(const float local_m[3]) {
	for(int a=0;a<3;a++) if(!isfinite(local_m[a])) { XwaVrPilotSpace_Reset(); return; }
	memcpy(s_offset,local_m,sizeof s_offset);
}
const float* XwaVrPilotSpace_Offset(void) { return s_offset; }
void XwaVrPilotSpace_ToCockpit(float p[3]) {
	for(int a=0;a<3;a++) if(s_offset[a]!=0) p[a]+=s_offset[a];
}
void XwaVrPilotSpace_ToTracking(float p[3]) {
	for(int a=0;a<3;a++) if(s_offset[a]!=0) p[a]-=s_offset[a];
}
void XwaVrPilotSpace_Views(const AeronXrView in[2], AeronXrView out[2]) {
	memcpy(out,in,2*sizeof *out);
	for(int e=0;e<2;e++) XwaVrPilotSpace_ToCockpit(out[e].position);
}
void XwaVrPilotSpace_Panels(AeronXrPanel* panels,int count) {
	for(int i=0;i<count;i++) XwaVrPilotSpace_ToTracking(panels[i].position);
}
void XwaVrPilotSpace_CacheLayer(const AeronXrPanel* presented,int cockpit_fixed,
	AeronXrPanel* cached,uint8_t* cockpit_flag) {
	*cached=*presented; *cockpit_flag=(uint8_t)(cockpit_fixed!=0);
	if(cockpit_fixed) XwaVrPilotSpace_ToCockpit(cached->position);
}
void XwaVrPilotSpace_LayerInCockpit(const AeronXrPanel* cached,int cockpit_fixed,AeronXrPanel* out) {
	*out=*cached;
	if(!cockpit_fixed) XwaVrPilotSpace_ToCockpit(out->position);
}
int XwaVrPilotSpace_GetHand(int hand,AeronXrHand* out) {
	if(!Aeron_XrGetHand(hand,out)) return 0;
	XwaVrPilotSpace_ToCockpit(out->position);
	if(out->grip_valid) XwaVrPilotSpace_ToCockpit(out->grip_position);
	return 1;
}
int XwaVrPilotSpace_LocateRenderModel(int hand,float position[3],float orientation[4],
	AeronXrRenderModelNode* nodes,uint32_t capacity) {
	if(!Aeron_XrLocateRenderModel(hand,position,orientation,nodes,capacity)) return 0;
	XwaVrPilotSpace_ToCockpit(position);
	return 1;
}

#ifndef XWA_VR_PILOT_MOTION_H
#define XWA_VR_PILOT_MOTION_H

/* Render-only exterior viewpoint experiments. No tracked pose or simulation writes. */
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct XwaVrPilotMotion {
	int have, centered, have_velocity;
	uint64_t identity, frame_us, sample_us;
	uint32_t stamp, sample_stamp;
	double ship[3];
	float reference_seat[3], pivot[3], velocity[3], acceleration[3];
	float inertia_m[3], target_m[3], offset_world[3];
	float units_per_m;
} XwaVrPilotMotion;

static inline void XwaVrPilotMotion_Reset(XwaVrPilotMotion* s) { memset(s,0,sizeof *s); }

static inline void XwaVrPilotMotion_Limit(float v[3], float limit) {
	float n=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
	if(n>limit && n>0) for(int a=0;a<3;a++) v[a]*=limit/n;
}

static inline void XwaVrPilotMotion_Update(XwaVrPilotMotion* s, const int32_t ship[3],
	const float seat[3], uint32_t stamp, uint64_t now, uint64_t identity,
	int eligible, int centered, float inertia_limit_m, float units_per_m) {
	if(!eligible || (!centered && inertia_limit_m<=0) || !isfinite(units_per_m) || units_per_m<=0) {
		XwaVrPilotMotion_Reset(s); return;
	}
	for(int a=0;a<3;a++) if(!isfinite(seat[a])) { XwaVrPilotMotion_Reset(s); return; }
	if(!s->have || s->identity!=identity || s->centered!=centered || units_per_m!=s->units_per_m || now<=s->frame_us ||
		now-s->frame_us>250000 || (uint32_t)(stamp-s->stamp)>250) {
		XwaVrPilotMotion_Reset(s);
		s->have=1; s->identity=identity; s->centered=centered;
		s->units_per_m=units_per_m;
		s->frame_us=s->sample_us=now; s->stamp=s->sample_stamp=stamp;
		for(int a=0;a<3;a++) { s->ship[a]=ship[a]; s->reference_seat[a]=seat[a]; }
		return;
	}
	float frame_dt=(float)(now-s->frame_us)*0.000001f;
	s->frame_us=now;
	for(int a=0;a<3;a++) s->pivot[a]=centered ? s->reference_seat[a]-seat[a] : 0;
	if(stamp!=s->stamp) {
		float window=(float)(now-s->sample_us)*0.000001f;
		/* Differentiate physical motion on the simulation clock. Host-frame
		 * arrival jitter/held snapshots must not create a false acceleration. */
		float dt=(uint32_t)(stamp-s->sample_stamp)*0.001f, v[3];
		s->stamp=stamp;
		/* Quantized ship positions need a short velocity window: differentiating
		 * individual 72/90 Hz integer steps would manufacture acceleration. */
		float distance2=0;
		for(int a=0;a<3;a++) {
			float delta=(float)(((double)ship[a]-s->ship[a])/units_per_m);
			distance2+=delta*delta;
			v[a]=dt>0 ? delta/dt : 0;
		}
		if(distance2>250*250) {
			/* Teleport: re-prime at the new seat, with no carry-over impulse. */
			XwaVrPilotMotion_Reset(s);
			XwaVrPilotMotion_Update(s,ship,seat,stamp,now,identity,eligible,centered,inertia_limit_m,units_per_m);
			return;
		}
		if(window>=0.08f) {
			if(dt>0 && dt<=0.25f && inertia_limit_m>0) {
				if(s->have_velocity) {
					float weight=1-expf(-dt/0.12f);
					for(int a=0;a<3;a++) {
						float acceleration=(v[a]-s->velocity[a])/dt;
						s->acceleration[a]+=(acceleration-s->acceleration[a])*weight;
						s->target_m[a]=-s->acceleration[a]*0.002f;
					}
					XwaVrPilotMotion_Limit(s->target_m,inertia_limit_m);
				} else memset(s->target_m,0,sizeof s->target_m);
				memcpy(s->velocity,v,sizeof v); s->have_velocity=1;
			} else {
				s->have_velocity=0;
				memset(s->acceleration,0,sizeof s->acceleration);
				memset(s->target_m,0,sizeof s->target_m);
			}
			for(int a=0;a<3;a++) s->ship[a]=ship[a];
			s->sample_us=now; s->sample_stamp=stamp;
		}
	} else if(now-s->sample_us>100000) {
		memset(s->target_m,0,sizeof s->target_m);
		memset(s->acceleration,0,sizeof s->acceleration);
		s->have_velocity=0;
	}
	if(inertia_limit_m<=0) {
		memset(s->inertia_m,0,sizeof s->inertia_m);
		memset(s->target_m,0,sizeof s->target_m);
		s->have_velocity=0;
	} else {
		XwaVrPilotMotion_Limit(s->target_m,inertia_limit_m);
		float target2=0,current2=0;
		for(int a=0;a<3;a++) { target2+=s->target_m[a]*s->target_m[a]; current2+=s->inertia_m[a]*s->inertia_m[a]; }
		float weight=1-expf(-frame_dt/(target2>current2 ? 0.18f : 0.45f));
		for(int a=0;a<3;a++) s->inertia_m[a]+=(s->target_m[a]-s->inertia_m[a])*weight;
		XwaVrPilotMotion_Limit(s->inertia_m,inertia_limit_m);
	}
	for(int a=0;a<3;a++) s->offset_world[a]=s->pivot[a]+s->inertia_m[a]*units_per_m;
}

#endif

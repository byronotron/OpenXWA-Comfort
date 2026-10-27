#include <assert.h>
#include <stdio.h>
#include "xwa_remaster/vr_pilot_motion.h"

static float length(const float v[3]) { return sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); }
static void near_value(float a,float b,float tol) { assert(fabsf(a-b)<tol); }

static void test_pivot(void) {
	/* A long nose seat rotates around a stationary CG in the original game.
	 * The centered view keeps the initial eye point fixed, for all axes. */
	for(int axis=0;axis<3;axis++) {
		XwaVrPilotMotion s={0}; int32_t ship[3]={1000000000,-1000000000,1800000000};
		float seat[3]={0}; seat[(axis+1)%3]=400;
		float first[3]; memcpy(first,seat,sizeof first);
		for(int i=0;i<=360;i++) {
			float a=i*0.01745329252f;
			seat[(axis+1)%3]=400*cosf(a); seat[(axis+2)%3]=400*sinf(a);
			int32_t copy[3]; memcpy(copy,ship,sizeof copy);
			XwaVrPilotMotion_Update(&s,ship,seat,i*10,1000000+i*10000,7,1,1,0,40.96f);
			for(int j=0;j<3;j++) near_value(seat[j]+s.offset_world[j],first[j],0.0001f);
			assert(memcmp(copy,ship,sizeof copy)==0);
		}
		/* Changing seat/ship primes a new reference, not an old correction. */
		XwaVrPilotMotion_Update(&s,ship,seat,3610,4610000,8,1,1,0,40.96f);
		assert(length(s.offset_world)==0);
		XwaVrPilotMotion_Update(&s,ship,seat,3620,4620000,8,1,0,0,40.96f);
		assert(!s.have && length(s.offset_world)==0);
	}
}

static void test_inertia(void) {
	const float units=40.96f, caps[3]={0.01f,0.02f,0.035f};
	for(int axis=0;axis<3;axis++) for(int level=0;level<3;level++) {
		XwaVrPilotMotion s={0}; float seat[3]={0}; int32_t ship[3]={0};
		for(int i=0;i<=300;i++) {
			float t=i*0.01f;
			ship[axis]=(int32_t)lroundf(0.5f*30*t*t*units);
			XwaVrPilotMotion_Update(&s,ship,seat,i*10,1000000+i*10000,1,1,0,caps[level],units);
			assert(length(s.inertia_m)<=caps[level]+0.000001f);
			assert(length(s.pivot)==0);
		}
		assert(s.inertia_m[axis]<-caps[level]*0.95f); /* acceleration pushes view back */
		for(int i=301;i<=700;i++) XwaVrPilotMotion_Update(&s,ship,seat,3000,1000000+i*10000,1,1,0,caps[level],units);
		assert(length(s.inertia_m)<0.00002f); /* pause releases, no held g-force */
		XwaVrPilotMotion_Update(&s,ship,seat,3010,8010000,2,1,0,caps[level],units);
		assert(length(s.offset_world)==0);
	}
	/* Diagonal acceleration is bounded by total displacement, not per axis. */
	XwaVrPilotMotion s={0}; int32_t ship[3]={0}; float seat[3]={0};
	for(int i=0;i<=300;i++) {
		float t=i*0.01f;
		for(int a=0;a<3;a++) ship[a]=(int32_t)lroundf(0.5f*30*t*t*units);
		XwaVrPilotMotion_Update(&s,ship,seat,i*10,1000000+i*10000,1,1,0,0.035f,units);
		assert(length(s.inertia_m)<=0.035001f);
	}
	for(int a=0;a<3;a++) assert(s.inertia_m[a]<0);
}

static void test_constant_velocity_and_reset(void) {
	XwaVrPilotMotion s={0}; int32_t ship[3]={0}; float seat[3]={120,30,-40};
	uint64_t now=1000000; double time=0;
	for(int i=0;i<=1500;i++) {
		int step=(i%3==0) ? 13000 : 15000;
		now+=step; time+=step*0.000001;
		ship[0]=(int32_t)llround(42*time*40.96);
		XwaVrPilotMotion_Update(&s,ship,seat,(uint32_t)llround(time*1000),now,1,1,1,0.035f,40.96f);
		if(i>200) assert(length(s.inertia_m)<0.002f); /* no quantization-induced lean */
	}
	XwaVrPilotMotion held={0};
	for(int i=0;i<1500;i++) {
		/* 30 Hz simulation snapshots displayed at 72 Hz must not invent g-forces. */
		uint64_t host=(uint64_t)i*13889;
		uint32_t sim=(uint32_t)((host/33333)*33333/1000);
		int32_t position[3]={(int32_t)llround(42*(double)sim*0.001*40.96),0,0};
		XwaVrPilotMotion_Update(&held,position,seat,sim,1000000+host,1,1,0,0.035f,40.96f);
		if(i>200) assert(length(held.inertia_m)<0.002f);
	}
	/* Teleport, focus loss, clock jump, invalid seat and Off clear every offset. */
	ship[1]+=100000;
	XwaVrPilotMotion_Update(&s,ship,seat,(uint32_t)llround(time*1000)+20,now+20000,1,1,1,0.035f,40.96f);
	assert(length(s.offset_world)==0);
	XwaVrPilotMotion_Update(&s,ship,seat,1,now+30000,1,1,1,0.035f,40.96f);
	assert(length(s.offset_world)==0);
	XwaVrPilotMotion_Update(&s,ship,seat,2,now+40000,1,0,1,0.035f,40.96f);
	assert(!s.have);
	seat[0]=NAN;
	XwaVrPilotMotion_Update(&s,ship,seat,3,now+50000,1,1,1,0.035f,40.96f);
	assert(!s.have);
	seat[0]=0;
	XwaVrPilotMotion_Update(&s,ship,seat,4,now+60000,1,1,0,0,40.96f);
	assert(!s.have && length(s.offset_world)==0);
}

int main(void) {
	test_pivot(); test_inertia(); test_constant_velocity_and_reset();
	puts("PASS: pilot-centered rotation on all axes; large world coordinates; immutable inputs; bounded opposing inertia; diagonal cap; pause/reset/teleport; quantized constant-speed stability; Off zero offsets");
}

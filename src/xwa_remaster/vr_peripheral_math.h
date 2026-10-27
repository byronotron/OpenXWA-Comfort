#ifndef XWA_VR_PERIPHERAL_MATH_H
#define XWA_VR_PERIPHERAL_MATH_H

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct XwaVrMaskMotion {
	int have;
	uint64_t identity, frame_us, sample_us;
	uint32_t stamp;
	float q[4]; /* normalized ship-body quaternion, w x y z; never an eye pose */
	float speed_dps, activation;
} XwaVrMaskMotion;

static inline float XwaVrMask_Clamp(float v, float lo, float hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

static inline float XwaVrMask_Parse(const char* text, float fallback, float lo, float hi) {
	if (!text || !text[0]) return fallback;
	char* end;
	float v = strtof(text, &end);
	return end == text || *end || !isfinite(v) ? fallback : XwaVrMask_Clamp(v, lo, hi);
}

static inline float XwaVrMask_Response(float speed, float sensitivity) {
	/* Normal flight turns must activate the mask, not only hard maneuvers. */
	float x = XwaVrMask_Clamp((speed * sensitivity - 5.0f) / 50.0f, 0.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

static inline int XwaVrMask_Normalize(const float input[4], float q[4]) {
	float n = 0;
	for (int i = 0; i < 4; i++) n += input[i] * input[i];
	if (!isfinite(n) || n < 0.000001f) return 0;
	n = 1.0f / sqrtf(n);
	for (int i = 0; i < 4; i++) q[i] = input[i] * n;
	return 1;
}

static inline void XwaVrMask_UpdateMotion(XwaVrMaskMotion* s, const float ship_q[4],
	uint32_t stamp, uint64_t now, uint64_t identity, int eligible, float sensitivity) {
	float q[4];
	if (!eligible || !XwaVrMask_Normalize(ship_q, q)) {
		memset(s, 0, sizeof *s);
		return;
	}
	if (!s->have || s->identity != identity || now <= s->frame_us || now - s->frame_us > 250000 ||
		(uint32_t)(stamp - s->stamp) > 250) {
		memset(s, 0, sizeof *s);
		s->have = 1; s->identity = identity; s->frame_us = s->sample_us = now; s->stamp = stamp;
		memcpy(s->q, q, sizeof q);
		return;
	}
	const float frame_dt = (float)(now - s->frame_us) * 0.000001f;
	s->frame_us = now;
	if (stamp != s->stamp) {
		/* Relative quaternion vector is stable even for identical/negated poses;
		 * acos(trace) would amplify Q15 rounding into false stationary motion. */
		const float* p = s->q;
		float v[3] = {
			p[0]*q[1] - p[1]*q[0] - p[2]*q[3] + p[3]*q[2],
			p[0]*q[2] + p[1]*q[3] - p[2]*q[0] - p[3]*q[1],
			p[0]*q[3] - p[1]*q[2] + p[2]*q[1] - p[3]*q[0] };
		float w = p[0]*q[0] + p[1]*q[1] + p[2]*q[2] + p[3]*q[3];
		float degrees = 114.591559026f * atan2f(sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]), fabsf(w));
		const float dt = (float)(now - s->sample_us) * 0.000001f;
		if (dt <= 0 || dt > 0.25f || degrees > 45.0f) {
			s->speed_dps = s->activation = 0; /* discontinuity, not a turn */
		} else {
			s->speed_dps = degrees / dt; /* actual displayed motion, including slow motion */
		}
		memcpy(s->q, q, sizeof q);
		s->sample_us = now; s->stamp = stamp;
	} else if (now - s->sample_us > 100000) {
		s->speed_dps = 0; /* held/paused simulation must release the mask */
	}
	float target = XwaVrMask_Response(s->speed_dps, sensitivity);
	float tau = target > s->activation ? 0.12f : 0.45f;
	s->activation += (target - s->activation) * (1.0f - expf(-frame_dt / tau));
	s->activation = XwaVrMask_Clamp(s->activation, 0, 1);
}

/* OpenXR quaternion (x y z w) -> columns mapping eye directions into LOCAL. */
static inline void XwaVrMask_XrMatrix(const float q[4], float r[9]) {
	float x=q[0], y=q[1], z=q[2], w=q[3];
	r[0]=1-2*(y*y+z*z); r[1]=2*(x*y-w*z); r[2]=2*(x*z+w*y);
	r[3]=2*(x*y+w*z); r[4]=1-2*(x*x+z*z); r[5]=2*(y*z-w*x);
	r[6]=2*(x*z-w*y); r[7]=2*(y*z+w*x); r[8]=1-2*(x*x+y*y);
}

/* Relative eye -> shared cyclopean-head axes, including canted eye views.
 * Global head turns cancel here; they never enter the motion detector. */
static inline void XwaVrMask_EyeAxes(const float left[4], const float right[4], float axes[2][9]) {
	float dot=0, head[4], h[9], eye[9];
	for (int i=0;i<4;i++) dot += left[i]*right[i];
	float n=0;
	for (int i=0;i<4;i++) { head[i]=left[i]+(dot<0 ? -right[i] : right[i]); n+=head[i]*head[i]; }
	if (n < 0.000001f || !isfinite(n)) { head[0]=head[1]=head[2]=0; head[3]=1; }
	else { n=1/sqrtf(n); for(int i=0;i<4;i++) head[i]*=n; }
	XwaVrMask_XrMatrix(head,h);
	for(int e=0;e<2;e++) {
		XwaVrMask_XrMatrix(e ? right : left,eye);
		for(int i=0;i<3;i++) for(int j=0;j<3;j++)
			axes[e][i*3+j]=h[i]*eye[j]+h[3+i]*eye[3+j]+h[6+i]*eye[6+j];
	}
}

static inline float XwaVrMask_AngularAlpha(float tx, float ty, const float eye_to_head[9], float strength) {
	float x=eye_to_head[0]*tx+eye_to_head[1]*ty-eye_to_head[2];
	float y=eye_to_head[3]*tx+eye_to_head[4]*ty-eye_to_head[5];
	float z=eye_to_head[6]*tx+eye_to_head[7]*ty-eye_to_head[8];
	float angle=atan2f(sqrtf(x*x+y*y),-z);
	/* Medium (1) preserves the previous High exactly. Above 1, narrow the
	 * aperture rather than trying to increase opacity beyond fully black. */
	angle *= fmaxf(strength,1.0f);
	/* Clear central 40-degree cone; feather to full coverage at 35 degrees.
	 * The old 35..55 degree band only dimmed the extreme edges in dark space. */
	float a=XwaVrMask_Clamp((angle-0.3490658504f)/0.2617993878f,0,1);
	return a*a*(3-2*a);
}

#endif

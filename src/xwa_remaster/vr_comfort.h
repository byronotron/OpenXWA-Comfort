#ifndef XWA_REMASTER_VR_COMFORT_H
#define XWA_REMASTER_VR_COMFORT_H

/* Render-only comfort policy. Never writes the simulation or headset pose. */
#include "xwa_runtime/snapshot/snapshot.h"
#include <math.h>
#include <stdlib.h>

static inline float XwaVrComfort_ParseShake(const char* value) {
	if (!value || !value[0]) {
		return 0.25f;
	}
	char* end;
	const float scale = strtof(value, &end);
	if (end == value || *end || !isfinite(scale)) {
		return 0.25f;
	}
	return scale < 0.0f ? 0.0f : (scale > 1.0f ? 1.0f : scale);
}

static inline int XwaVrComfort_InternalFlight(const XwaFlightCamera* cam) {
	return cam && cam->player_obj_idx >= 0 && cam->focus_obj_idx == cam->player_obj_idx &&
		   !cam->external && !cam->film_overlay && !cam->map_mode && !cam->in_hangar &&
		   !cam->hyperspace_phase;
}

static inline float XwaVrComfort_ShakeScale(const XwaFlightCamera* cam, int vr_active,
										 const char* stereo, const char* value) {
	return vr_active && !(stereo && stereo[0] == '0') && XwaVrComfort_InternalFlight(cam)
			   ? XwaVrComfort_ParseShake(value) : 1.0f;
}

static inline void XwaVrComfort_ApplyCameraPan(const XwaFlightCamera* cam, float scale, float pos[3]) {
	/* The classic camera adds the signed integer pan>>4, not pan/16. Keep
	 * its exact contribution so Original is identical and Off removes it. */
	for (int axis = 0; axis < 3; axis++) {
		pos[axis] -= (1.0f - scale) * cam->impact_pan_world[axis];
	}
}

#endif

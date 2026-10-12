#ifndef XWA_REMASTER_SKY_STARS_H
#define XWA_REMASTER_SKY_STARS_H

#include "aeron/scene/scene3d.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XwaRemasterSkyStars XwaRemasterSkyStars;

typedef struct XwaRemasterSkyStarsParams {
	float exposure;
	float brightness;
	float density;
	float grid_n;
	float core_radius_px;
	float feather_px;
	float pixel_pitch_px;
	float flare_strength;
	uint32_t game_time_ms;
} XwaRemasterSkyStarsParams;

XwaRemasterSkyStars* XwaRemasterSkyStars_Create(void);
void XwaRemasterSkyStars_Destroy(XwaRemasterSkyStars* stars);

/* Captures the current scene transform after AeronScene_Begin. The public
 * jittered view-projection keeps stars aligned with temporally jittered meshes. */
int XwaRemasterSkyStars_Prepare(XwaRemasterSkyStars* stars, const AeronScene3D* scene,
								const float world_to_cube[9], const XwaRemasterSkyStarsParams* params);

/* One star of the procedural field, for drawing it some other way than the
 * GPU scatter pass (VR draws them as sky billboards). */
typedef struct XwaRemasterSkyStarPoint {
	float dir_cube[3]; /* unit direction, cube-sampling basis */
	float color[3];    /* linear, exposure and brightness applied */
	int bright;        /* the classic draws a flare around this one */
} XwaRemasterSkyStarPoint;

/* CPU evaluation of the same field the shader draws (same hash, same
 * twinkle at params->game_time_ms). Only stars within the cone around
 * `cone_dir_cube` (cosine >= cos_min) are returned. Returns the number
 * written to `out`. */
int XwaRemasterSkyStars_Collect(const XwaRemasterSkyStarsParams* params, const float cone_dir_cube[3], float cos_min,
								XwaRemasterSkyStarPoint* out, int max_out);

/* Small soft round dot (premultiplied RGBA8) for drawing collected stars;
 * created on first use, NULL on failure. */
AeronTexture* XwaRemasterSkyStars_DotTexture(XwaRemasterSkyStars* stars);

/* AeronScene BEFORE_OPAQUE hook draw. */
void XwaRemasterSkyStars_Draw(AeronCommandBuffer* command_buffer, AeronRenderPass* render_pass, int rt_w,
							  int rt_h, void* user);

#ifdef __cplusplus
}
#endif

#endif /* XWA_REMASTER_SKY_STARS_H */

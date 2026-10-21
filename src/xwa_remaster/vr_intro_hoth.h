#ifndef XWA_REMASTER_VR_INTRO_HOTH_H
#define XWA_REMASTER_VR_INTRO_HOTH_H

/*
 * VR: the intro film's Hoth shot (after the crawl) as a scene in the game's
 * own models, part of the intro scene (vr_intro_crawl.h), on the film's
 * clock. The film turns from the crawl to Hoth, the Azzameen transport and
 * its two X-wings come away from the planet, the transport roars past the
 * camera and they run at the waiting Star Destroyers; here the viewer sits
 * still and turns their head:
 *
 *   ~50-53 s  Hoth fades in on the left (centre 70 deg left, 13 deg down,
 *             45 deg radius in view; RESDATA/PLANET.DAT group 6030).
 *   51 s on   the transport (FAMILYTRANSPORT) and the X-wings come in from
 *             in front of Hoth, unlit against the planet.
 *   63-66 s   the light comes up: the craft lit, and out of the dark ahead
 *             the two Star Destroyers (30 deg right at 1.6 km, 8 deg right
 *             at 2.6 km) as the ships turn towards them.
 *   65.7 s    the transport crosses 18 m in front, just below eye level;
 *             an X-wing passes overhead at 66.75 s.
 *   from 66 s turbolasers from the Star Destroyers, the X-wings' lasers.
 *   67.93 s   the film's cut to the X-wing cockpit: the screen is back at
 *             once and the scene holds at half brightness round it until
 *             the film ends.
 *
 * Positions are the viewer's LOCAL metres (x right, y up, -z ahead, the
 * screen at 0 degrees).
 */

#include "aeron/render.h"
#include "aeron/scene/scene3d.h"
#include "xwa_remaster/vr_env.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The film's cut back to its own pictures (frame 1019). */
#define XWA_VR_HOTH_CUT 67.93f
/* The scene's half brightness after the cut, reached over this. */
#define XWA_VR_HOTH_DIM_SECONDS 1.0f

/* Loads one piece per call (planet, glow and beam textures, the three
 * models); returns 1 once everything it can have is loaded. */
int XwaVrIntroHoth_Load(AeronCommandBuffer* cmd);
/* Nonzero when the scene can play (the models are there). */
int XwaVrIntroHoth_Ready(void);
void XwaVrIntroHoth_Release(void);

/* Queues the scene at film time `t`; `org`, `right`, `up`, `fwd` and `k`
 * map the viewer's LOCAL metres to the world (org + k * (x right + y up -
 * z fwd)). */
void XwaVrIntroHoth_Submit(AeronScene3D* scene, const float org[3], const float right[3], const float up[3],
						   const float fwd[3], float k, float t);
/* The scene's sun, Hoth's bounce and space's ambient at film time `t`, in
 * the world of the last Submit; 0 when nothing was submitted. */
int XwaVrIntroHoth_Lighting(float t, XwaVrEnvLighting* out);

#ifdef __cplusplus
}
#endif

#endif

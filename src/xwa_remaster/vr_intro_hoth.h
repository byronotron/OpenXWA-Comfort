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
 *   until 56.93 s the crawl's starfield alone.
 *   56.93-59.93 s  Hoth fades in on the left, ready for the first line
 *             spoken (centre 70 deg left, 13 deg down, 45 deg radius in
 *             view; RESDATA/PLANET.DAT group 6030); the transport
 *             (FAMILYTRANSPORT) and the X-wings come in from in front of
 *             Hoth, unlit against the planet.
 *   62.5-66 s the light comes up: the craft lit, and out of the dark the two
 *             Star Destroyers (55 deg right at 1.6 km, 33 deg right at
 *             2.6 km) as the ships turn towards them; far off, X-wings and
 *             TIE fighters wheel in two fights (upper right, upper left).
 *   65.7 s    the transport crosses 18 m in front, just below eye level;
 *             an X-wing passes overhead at 66.75 s.
 *   67.93 s   the film's cut to the X-wing cockpit: the screen is back at
 *             once and the scene holds at half brightness round it. The two
 *             X-wings join the upper right fight; three TIEs blow up there
 *             and in the other (83, 90.5, 95 s).
 *   75.5 s    the ion cannon: a glow on Hoth, the bolt across, a blue flash
 *             on the lead Star Destroyer's superstructure (77.1 s), its left
 *             shield generator blows up once (77.7 s) and is gone after, as
 *             the game shows a destroyed one; ion lightning wanders over its
 *             hull from the strike until the scene ends.
 *   98-101 s  the transport crosses the lead Star Destroyer's hull, then
 *             flies on beyond it and jumps to hyperspace at 109.3 s.
 *   111.67 s  the film's X-Wing Alliance logo card: the screen is put away
 *             and the menus' logo (vr/logo.rgba) shows in its place, fading
 *             as the film's does (in 111.67-112.0 s, out 116.4-118.45 s).
 *   120.93 s  the film's cut into the hyperspace tunnel: the screen is back
 *             and the scene round it becomes the game's hyperspace tunnel
 *             (flight.c), at half brightness, for 3 s only, fading to black
 *             in its last half second; black then until the film drops out
 *             of hyperspace (147-148 s), the startup films' stars from there
 *             until the film ends.
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
/* The film's logo card: in over 0.33 s, out from LOGO_OUT over LOGO_FADE. */
#define XWA_VR_HOTH_LOGO_IN 111.67f
#define XWA_VR_HOTH_LOGO_OUT 116.4f
#define XWA_VR_HOTH_LOGO_FADE 2.05f
/* The film's cut into its hyperspace tunnel (frame 1814); its tunnel fades
 * out over HYPER_FADE..HYPER_END. The scene's own tunnel lasts
 * TUNNEL_SECONDS (VR: the whole of it is too much), fading to black over
 * its last TUNNEL_FADE. */
#define XWA_VR_HOTH_HYPER 120.93f
#define XWA_VR_HOTH_TUNNEL_SECONDS 3.0f
#define XWA_VR_HOTH_TUNNEL_FADE 0.5f
#define XWA_VR_HOTH_HYPER_FADE 147.0f
#define XWA_VR_HOTH_HYPER_END 148.0f

/* Loads one piece per call (planet, sprites, logo, explosion, the four
 * models); returns 1 once everything it can have is loaded. */
int XwaVrIntroHoth_Load(AeronCommandBuffer* cmd);
/* Nonzero when the scene can play (the models are there). */
int XwaVrIntroHoth_Ready(void);
void XwaVrIntroHoth_Release(void);

/* Queues the scene at film time `t`; `org`, `right`, `up`, `fwd` and `k`
 * map the viewer's LOCAL metres to the world (org + k * (x right + y up -
 * z fwd)); the virtual screen is `screen_w` wide at `screen_dist` metres,
 * its centre `lift` up (the logo card takes its place). */
void XwaVrIntroHoth_Submit(AeronScene3D* scene, const float org[3], const float right[3], const float up[3],
						   const float fwd[3], float k, float t, float screen_dist, float screen_w, float lift);
/* The scene's sun, Hoth's bounce and space's ambient at film time `t`, in
 * the world of the last Submit; 0 when nothing was submitted. */
int XwaVrIntroHoth_Lighting(float t, XwaVrEnvLighting* out);

#ifdef __cplusplus
}
#endif

#endif

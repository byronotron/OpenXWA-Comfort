#ifndef XWA_REMASTER_VR_INTRO_CRAWL_H
#define XWA_REMASTER_VR_INTRO_CRAWL_H

/*
 * VR: the opening of the intro film (MOVIES/INTRO_FINAL.SNM) as a scene
 * round the viewer. The film plays on as always (its music and the clock
 * this follows); for its first 49 seconds the virtual screen is put away
 * and in its place the STAR WARS logo recedes into the stars and the text
 * crawls away on a huge tilted plane, in 3D, matching the film frame for
 * frame straight ahead (measured from the film: the logo's size each
 * frame, the plane's vanishing line and scroll, the lines' size, spacing
 * and justification, the colours and fades, its star field). Then the
 * film's Hoth shot plays on in the game's models (vr_intro_hoth.h) until
 * the film's cut at 67.93 s, where the screen comes back and the scene
 * holds at half brightness round it to the film's end. Without the Hoth
 * shot's models the screen comes back in the black after the crawl (49 s),
 * showing just the stars the scene had there.
 *
 * Needs, as APK assets (never in the repo): vr/starwars_logo.rgba (XWAL),
 * vr/intro_crawl.txt (the crawl's text, its lines as in the film; blank
 * lines part the paragraphs) and vr/LiberationSans-BoldItalic.ttf (SIL OFL
 * 1.1, rasterized here with stb_truetype); vr/intro_stars.rgba (the film's
 * star field, else the menus' stars show). Without the first three, or with
 * XWA_XR_INTRO_CRAWL=0 (VR Options: "Intro: 3D Crawl"), the film plays on
 * the screen as before. Skipping the film ends the scene with it.
 *
 * Tuning: XWA_XR_CRAWL_HEIGHT (metres between the eye and the crawl's
 * plane, 12), XWA_XR_CRAWL_LOGO_DISTANCE (metres to the logo when it
 * appears, 5), XWA_XR_CRAWL_TEXTURE (the text texture's width, 4096),
 * XWA_XR_CRAWL_TIME (seconds: hold the scene at that film time, tests).
 */

#include "aeron/render.h"
#include "aeron/scene/scene3d.h"
#include "xwa_remaster/vr_env.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every VR environment frame, before the scene is submitted: follows the
 * film's clock, loads the logo and rasterizes the crawl when the intro
 * starts, frees them after. */
void XwaVrIntroCrawl_Update(AeronCommandBuffer* cmd);

/* Nonzero while the scene plays (the film's first ~50 s). */
int XwaVrIntroCrawl_Active(void);

/* Queues the logo and the crawl; `view_pos` and `rows` are the seated
 * viewer's position and world -> eye rows (XwaVrEnv_View). */
void XwaVrIntroCrawl_Submit(AeronScene3D* scene, const float view_pos[3], const float rows[9]);

/* The virtual screen is kept out of view (until the handover). */
int XwaVrIntroCrawl_ScreenHidden(void);
/* The screen's brightness: 0 while hidden, else 1. */
float XwaVrIntroCrawl_ScreenFade(void);
/* How much the scene's own look applies (1 while it plays, easing to 0
 * over the handover): full eye brightness instead of the films' dimming. */
float XwaVrIntroCrawl_Level(void);
/* The menus' stars' brightness: 0 while the film's own are drawn (or in
 * its black opening), easing back over the handover. */
float XwaVrIntroCrawl_StarLevel(void);
/* The scene's displayed brightness on top of its own look: 1, easing to
 * half after the Hoth shot's cut (vr_intro_hoth.h) until the film ends. */
float XwaVrIntroCrawl_Brightness(void);
/* The Hoth shot's lights while it plays; 0 otherwise. */
int XwaVrIntroCrawl_Lighting(XwaVrEnvLighting* out);

#ifdef __cplusplus
}
#endif

#endif

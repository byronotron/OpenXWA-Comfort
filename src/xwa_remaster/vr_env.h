#ifndef XWA_REMASTER_VR_ENV_H
#define XWA_REMASTER_VR_ENV_H

/*
 * VR environment for the 2D screens (concourse, briefings, menus, films),
 * built from the installed game data and drawn in stereo around the flat
 * virtual screen. XWA_XR_ENVIRONMENT selects it:
 *   random (default) — endor, xwing, tiefighter, xvt or battle, picked at
 *                     each start;
 *   battle          — a live battle cast from the last mission flown (its
 *                     player craft and wingman, fighters, capital ships,
 *                     station, planets and sun), or rebels against the
 *                     Empire before any: fighters dogfight round the screen,
 *                     jump in from hyperspace and burst when shot down,
 *                     capital ships trade turbolaser fire on the flanks
 *                     (the mission is remembered in vr_last_mission.txt;
 *                     XWA_XR_BATTLE_MISSION names one instead);
 *   endor           — a still of the Battle of Endor after the box art:
 *                     game craft, the Death Star II and Endor sprites, the
 *                     logo above the screen (vr/logo.rgba, shipped in the
 *                     APK assets);
 *   xwing           — the X-Wing box art: its three X-wings and three TIE
 *                     fighters at their attitudes on the cover, a burst,
 *                     their fire and the purple nebula, spread around the
 *                     screen so none hides behind it;
 *   tiefighter      — the TIE Fighter box art: its Interceptors, TIE fighters,
 *                     bomber and the X-wing breaking up in the explosion,
 *                     at their cover attitudes around the screen, before
 *                     Vader's helmet (vr/vader.rgba, APK assets only).
 *                     Played: the five seconds of fighting before the
 *                     cover's moment, which then holds while the helmet
 *                     fades in (XWA_XR_VADER_FADE_SECONDS, default 2.5);
 *   xvt             — the X-Wing vs. TIE Fighter box art: its X-wings, TIE
 *                     Interceptor and TIE fighters round the explosion and
 *                     its pillar of light, at their cover attitudes, before
 *                     the TIE pilot and the rebel pilot (vr/xvt_tie_pilot.rgba,
 *                     vr/xvt_rebel_pilot.rgba, APK assets only);
 *   hangar          — the carrier hangar (Hangar.opt and its props, laid
 *                     out like the in-mission hangar);
 *   0               — off.
 *
 * Space: XWA world axes (x right, y forward, z up) in world units; the
 * hangar's origin is the classic launch reference, a space scene's the
 * viewer.
 */

#include "aeron/render.h"
#include "aeron/scene/scene3d.h"

#ifdef __cplusplus
extern "C" {
#endif

/* XWA_XR_ENVIRONMENT (default on). */
int XwaVrEnv_Enabled(void);

/* A space scene (lit by a sun, not the hangar lights) is selected. */
int XwaVrEnv_IsSpace(void);

/* A space scene's lights: a key (sun) and a fill light (world directions
 * toward the light, linear colour times intensity) and the ambient cube's
 * sides, top and bottom colours. */
typedef struct XwaVrEnvLighting {
	float key_dir[3], key_color[3];
	float fill_dir[3], fill_color[3];
	float ambient_sides[3], ambient_top[3], ambient_bottom[3];
} XwaVrEnvLighting;

/* Fills `out` for the selected space scene; 0 when it is not one. */
int XwaVrEnv_SpaceLighting(XwaVrEnvLighting* out);

/* Loads the environment a piece per call (no open pass on `cmd`) so menus
 * never stall. Returns 1 once the hangar itself can be drawn. */
int XwaVrEnv_Prepare(AeronCommandBuffer* cmd);

/* The seated viewer: head position (world units) and world -> eye rows
 * (engine eye space: x right, y down, z forward). */
void XwaVrEnv_View(float pos[3], float rows[9]);

/* Submits the loaded meshes as scene instances. */
void XwaVrEnv_Submit(AeronScene3D* scene);

/* The screen frame as a compositor layer right over the virtual screen
 * (the picture fills its opening, just behind it). Call after the
 * backdrop is committed; `brightness` (0..1) dims it with the scene.
 * Returns 0 when the frame has no layer (it is then drawn in the scene). */
int XwaVrEnv_CommitFrameLayer(AeronCommandBuffer* cmd, float brightness);

/* The flat rectangle the compositor layers cover in the menus (the frame,
 * or the bare screen without one), facing the viewer: centre and size in
 * metres, LOCAL space (x right, y up, -z ahead). */
int XwaVrEnv_LayerRect(float centre[3], float size[2]);

/* Call every frame before rendering: `bare` (the startup films) shows only
 * the stars round the screen, no scene, frame or logo. The scene keeps
 * loading meanwhile and also stays bare until every model, sprite and the
 * frame are loaded; then it fades in (XWA_XR_ENV_FADE_SECONDS, 1.5). */
void XwaVrEnv_SetBare(int bare);
int XwaVrEnv_Bare(void);
/* The fade in, 0 (bare) .. 1: the scene's lights scale with it. */
float XwaVrEnv_Fade(void);

/* Frees the GPU meshes (the flight scene loads its own hangar). */
void XwaVrEnv_Release(void);

#ifdef __cplusplus
}
#endif

#endif

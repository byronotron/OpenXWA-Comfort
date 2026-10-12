#ifndef XWA_REMASTER_VR_LAP_CONSOLE_H
#define XWA_REMASTER_VR_LAP_CONSOLE_H

/*
 * VR lap console: a holographic touch panel in front of the player's lap
 * with the commands that are not on a controller button (energy, targeting,
 * wingmen, ship). It sleeps (dim, not pressable) while the player looks
 * forward and wakes when they look down. Buttons are pressed by poking
 * them with the controller tip, or by pointing the controller at them and
 * pulling the trigger (the trigger does not fire while it points at the
 * console). Squeezing the grip at either end picks it up: it can be put
 * anywhere in the cockpit, turned any way, and stays there.
 *
 * Per host frame: BeginFrame, SetState (game state), then the flight
 * renderer calls Frame while it renders in stereo, then EndFrame. Pressed
 * buttons come out of PollAction.
 */

#include "aeron/render.h"
#include "aeron/xr.h"
#include "xwa_remaster/vr_console_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

void XwaVrConsole_BeginFrame(float dt_seconds);
void XwaVrConsole_SetState(const XwaVrConsoleState* state);

/* Handles the hands and draws the console into `target` (the stereo
 * frame's console target). Fills `out` with its placement and returns 1
 * when it should be shown. `views` are the stereo frame's eye views. */
int XwaVrConsole_Frame(AeronCommandBuffer* cmd, AeronRenderTarget* target, int width, int height,
					   const AeronXrView views[2], AeronXrPanel* out);

/* The button card: a reference of the controller layout, drawn into the
 * stereo frame's card target (`out` filled as for the console). It
 * appears at every mission start to the player's right, just out of view,
 * and can be picked up by its ends like the console. XWA_XR_BUTTON_CARD=0
 * turns it off. Call after Frame. */
int XwaVrConsole_CardFrame(AeronCommandBuffer* cmd, AeronRenderTarget* target, int width, int height,
						   const AeronXrView views[2], AeronXrPanel* out);

/* Releases the trigger when the console was not shown this frame. */
void XwaVrConsole_EndFrame(void);

/* After the player moved the console by its handles (grip at either end):
 * copies the new XWA_XR_CONSOLE_POSE value to `out` once, to be saved. */
int XwaVrConsole_TakePlacement(char* out, int size);

/* Pops a pressed console action. Returns 0 when none is queued. */
int XwaVrConsole_PollAction(XwaVrConsoleAction* action, int* arg);

void XwaVrConsole_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

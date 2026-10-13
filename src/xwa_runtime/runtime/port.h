#ifndef XWA_RUNTIME_PORT_H
#define XWA_RUNTIME_PORT_H

#include <stdint.h>

#include "xwa_remaster/vr_console_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

int XwaPort_Init(void);
void XwaPort_SetCommandLine(const char* commandLine);
void XwaPort_Tick(int32_t delta_us);
/* Select whether the compatibility renderer should produce a classic flight
 * frame. The port still runs the recovered render path for its view, audio, and
 * snapshot side effects; the DirectDraw/Direct3D shims discard only GPU work. */
void XwaPort_SetClassicFlightRenderingEnabled(int enabled);
/* Forces the last complete classic frame into a presentation whose normal
 * classic output was suppressed earlier in the host frame. */
void XwaPort_SubmitRetainedClassicFrame(void);
uint64_t XwaPort_GetClassicFlightFrameSerial(void);
/* Host-paused frame: the game tick is skipped (sim + snapshots
 * freeze); a required classic layer is re-submitted so CLASSIC/SPLIT
 * composition stays live under the shell's pause (Cmd+P). */
void XwaPort_PausedFrame(void);
/* Queues the original Scroll Lock flight action for keyboards without that key.
 * Returns nonzero when an active flight accepted the request. */
int XwaPort_QueueMouseLookToggle(void);

/* VR lap console bridge (see xwa_remaster/vr_lap_console.h). */
void XwaPort_GetVrConsoleState(XwaVrConsoleState* out);
void XwaPort_VrConsoleAction(XwaVrConsoleAction action, int arg);
/* 1 while flying the player's craft in VR (not loading, paused, hangar menu). */
int XwaPort_VrInFlight(void);
/* Shows `text` (up to 70 characters) in the in-flight message pane. */
void XwaPort_ShowFlightNote(const char* text);
/* Latched release of the flight mouse capture (Ctrl+Alt+M), so the pointer
 * returns to the OS while flight keeps running. Cleared by the hotkey, by a
 * click inside the window, or when the session leaves flight. */
void XwaPort_ToggleMouseCapture(void);
int XwaPort_IsMouseCaptureSuspended(void);
/* Nonzero once the window has held input focus at least once since
 * XwaPort_Init. The frontend/movie focus pause is gated on this so a launch
 * where the OS never handed focus back does not begin as a silent freeze. */
int XwaPort_EverHadFocus(void);
void XwaPort_Shutdown(void);
int XwaPort_ShouldQuit(void);
int XwaPort_GetExitCode(void);
uint64_t XwaPort_NextWakeDelayUs(void);

#ifdef __cplusplus
}
#endif

#endif

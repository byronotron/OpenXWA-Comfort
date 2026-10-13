#ifndef XWA_APP_VR_VOICE_H
#define XWA_APP_VR_VOICE_H

/*
 * VR voice commands: push-to-talk on the Touch controllers' thumb rests
 * (double-tap, then speak; or hold while speaking), recognized on the
 * headset (voice_core.c) and carried out as the lap console's actions, with
 * a confirmation line in the in-flight message pane.
 *
 * XWA_XR_VOICE: 0 off, doubletap (default), hold.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Once per host frame, before the flight tick. */
void XwaVrVoice_Update(void);

void XwaVrVoice_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

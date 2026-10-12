#ifndef XWA_RUNTIME_MODERN_VR_OPTIONS_SCREEN_H
#define XWA_RUNTIME_MODERN_VR_OPTIONS_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

/* VR settings screen (Video Options > OpenXWA VR Options). The settings are
 * the XWA_XR_* / XWA_POINT_LIGHTS / AERON_XR_RESOLUTION_SCALE environment
 * values that openxwa-quest.cfg sets at startup: a change takes effect by
 * updating the environment (the renderer reads them every frame) and is
 * written back to openxwa-quest.cfg when the screen is left. */

/* Nonzero when the screen is offered (VR presenting, or an Android build). */
int XwaModernVrOptions_Available(void);

/* Draws and updates the screen. Returns nonzero when the caller should
 * return to the parent video menu. */
int XwaModernVrOptionsScreen_Update(int menu_center_x, int* cursor_row);

/* Writes KEY=value into openxwa-quest.cfg (Android; elsewhere a no-op). */
void XwaModernVrOptions_SaveValue(const char* key, const char* value);

#ifdef __cplusplus
}
#endif

#endif

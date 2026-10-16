#ifndef XWA_RUNTIME_MODERN_VR_CREDITS_SCREEN_H
#define XWA_RUNTIME_MODERN_VR_CREDITS_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

/* The VR port's credits (Options > VR Port Credits), centred. Returns
 * nonzero when the caller should return to the options main menu. */
int XwaModernVrCreditsScreen_Update(int menu_center_x, int* cursor_row);

#ifdef __cplusplus
}
#endif

#endif

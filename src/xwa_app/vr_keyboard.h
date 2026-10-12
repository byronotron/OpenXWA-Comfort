#ifndef XWA_APP_VR_KEYBOARD_H
#define XWA_APP_VR_KEYBOARD_H

#include <stdint.h>

/*
 * On-screen keyboard for VR, where no physical keyboard is in reach (the
 * Quest standalone build above all). It appears on the virtual screen while
 * the original frontend shows an editable text field, such as the pilot
 * name on first launch, and types into it through the laser pointer.
 * XWA_VR_KEYBOARD=1 shows it on desktop too (testing), =0 disables it.
 *
 * Call XwaVrKeyboard_BeginFrame right after Aeron_BeginFrame (it consumes
 * clicks that land on the keyboard before the game sees them) and
 * XwaVrKeyboard_Submit after the game's frame is drawn, before Aeron_Present.
 */
void XwaVrKeyboard_BeginFrame(int32_t delta_us);
void XwaVrKeyboard_Submit(void);
void XwaVrKeyboard_Shutdown(void);

#endif

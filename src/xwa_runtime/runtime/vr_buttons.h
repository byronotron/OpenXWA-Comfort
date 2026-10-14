#ifndef XWA_RUNTIME_VR_BUTTONS_H
#define XWA_RUNTIME_VR_BUTTONS_H

/*
 * The VR (Quest Touch) controls and the labels every on-screen hint takes
 * from them: the one place to change a binding. The flight button table,
 * the lap console's keys, and the names below feed
 *
 *   - the game's own key hints (strings.txt), rewritten in port.c
 *     (XwaPort_ApplyVrStringHints),
 *   - mission radio messages ("Press 'T' to target ..."), rewritten as they
 *     are shown (XwaVrButtons_RewriteText),
 *   - the map legend and the map's exit hint (xwa_remaster/flight_map.c),
 *   - the controls card (xwa_remaster/vr_lap_console.c).
 *
 * The hangar menus run on Aeron's menu-keys mode (aeron/src/xr_input.c,
 * Aeron_XrApplyMenuKeys); its labels are the XWA_VR_MENU_* names below and
 * must follow that code.
 */

#include "aeron/aeron.h"
#include "xwa_remaster/vr_console_protocol.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Flight: a Touch button's plain and shifted (left grip held) game key. */
typedef struct XwaVrButtonBinding {
	uint16_t plain;
	uint16_t shifted;
} XwaVrButtonBinding;

const XwaVrButtonBinding* XwaVrButtons_Binding(AeronXrButton button);

/* B (plain) leaves the map instead of its flight action. */
#define XWA_VR_MAP_EXIT_BUTTON AERON_XR_BUTTON_B

/* Hangar menus (Aeron's menu-keys mode); the face buttons follow the
 * left-handed swap like every other name here. */
#define XWA_VR_MENU_MOVE "THUMBSTICK"
#define XWA_VR_MENU_SELECT XwaVrButtons_ButtonName(AERON_XR_BUTTON_A, 0) /* Enter (also the right trigger) */
#define XWA_VR_MENU_BACK XwaVrButtons_ButtonName(AERON_XR_BUTTON_B, 0)   /* Escape */
#define XWA_VR_MENU_SPACE XwaVrButtons_ButtonName(AERON_XR_BUTTON_X, 0)  /* Space */
/* Both triggers at once: land at a hangar bay, jump at a hyperspace buoy. */
#define XWA_VR_CHORD_LABEL "both triggers"
/* The left menu button: pause. */
#define XWA_VR_PAUSE_LABEL "menu button"
/* The right grip on its own: target what is in the sights. */
#define XWA_VR_RIGHT_GRIP_ACTION "Target in sights"

/* The lap console's (and voice commands') game key for `action`;
 * KEY_NONE for the ones that are not a single key. */
uint16_t XwaVrButtons_ConsoleKey(XwaVrConsoleAction action);

/* A button's name: "B", "left trigger", "right stick click", "flick
 * right" (right stick flick). `compact` gives the short form ("L-trigger",
 * "R-stick click") for radio messages. With left-handed controls
 * (Aeron_XrLeftHanded) it names the physical control that now sends it:
 * B is "Y", the left trigger "right trigger", and so on. */
const char* XwaVrButtons_ButtonName(AeronXrButton button, int compact);

/* The control that sends `key` in flight, e.g. "B" or "grip + B" ("grip+B"
 * when compact); "console" when only the lap console / voice has it; NULL
 * when nothing does. Static storage, valid until the next call. */
const char* XwaVrButtons_KeyLabel(uint16_t key, int compact);

/* The keyboard key's own name: "T", "Space", "Enter", "Shift+P". */
const char* XwaVrButtons_KeyName(uint16_t key);

/* What a key does in flight, for the controls card ("next target"); NULL
 * when it has no short description. */
const char* XwaVrButtons_KeyAction(uint16_t key);

/* Rewrites the keyboard references in a game text for the Touch
 * controllers ("Press 'T'" -> "Press B", "the space bar" -> "grip+X",
 * "(Shift-D)" -> "(grip+L-trigger)"). Keys no VR control sends are left as
 * they are. Returns 1 when something changed (out holds the result, always
 * terminated). */
int XwaVrButtons_RewriteText(const char* in, char* out, size_t size);

#ifdef __cplusplus
}
#endif

#endif

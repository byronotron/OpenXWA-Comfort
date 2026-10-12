#ifndef XWA_APP_SETUP_SCREEN_H
#define XWA_APP_SETUP_SCREEN_H

/*
 * In-game setup/error screen drawn with Aeron's UI toolkit inside the normal
 * presentation (and therefore inside the VR virtual screen). Used where OS
 * message boxes are invisible — the Meta Quest standalone build runs as an
 * immersive OpenXR app with no 2D window.
 */

#define XWA_SETUP_SCREEN_QUIT  0
#define XWA_SETUP_SCREEN_RETRY 1

/* Shows `title` and the word-wrapped `message` until the user picks a button.
 * `retry_label` NULL shows only "Quit". Returns XWA_SETUP_SCREEN_RETRY or
 * XWA_SETUP_SCREEN_QUIT (also on quit requests and when the screen cannot be
 * drawn). Requires Aeron_Init. */
int XwaSetupScreen_Run(const char* title, const char* message, const char* retry_label);

#endif

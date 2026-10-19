#ifndef XWA_RUNTIME_MODERN_MISSION_HINTS_SCREEN_H
#define XWA_RUNTIME_MODERN_MISSION_HINTS_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

/* In flight: nonzero when the current mission has debriefing hints (read
 * once per mission file and kept). */
int XwaModernMissionHintsScreen_Available(void);
/* The pause menu's Mission Hints page: the debriefing's hints only (no
 * results, statistics or the next-mission choices), scrollable, with Back.
 * Returns nonzero when the caller should return to the menu. */
int XwaModernMissionHintsScreen_Update(int menu_center_x, int* cursor_row);
/* Starts the page at its top (call when it is opened). */
void XwaModernMissionHintsScreen_Reset(void);

/* The pause menu's Review Briefing page: the briefing summary the loading
 * screen showed (flight_loading.c), scrollable, with Back. Available when
 * there was one. */
int XwaModernBriefingReviewScreen_Available(void);
int XwaModernBriefingReviewScreen_Update(int menu_center_x, int* cursor_row);
void XwaModernBriefingReviewScreen_Reset(void);

#ifdef __cplusplus
}
#endif

#endif

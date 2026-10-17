#include "xwa_runtime/config/modern_mission_hints_screen.h"

#include "xwa/assets/string_table.h"
#include "xwa/config/game_config.h"
#include "xwa/flight/flight.h"
#include "xwa/frontend/frontend_color.h"
#include "xwa/frontend/frontend_draw.h"
#include "xwa/frontend/frontend_input.h"
#include "xwa/frontend/frontend_scrollbar.h"
#include "xwa/frontend/frontend_text.h"
#include "xwa/frontend/mission_debrief.h"
#include "xwa_runtime/config/modern_options_menu.h"

#include <stdio.h>
#include <string.h>

/* Hint text box (640x480 menu space) and its line count before scrolling. */
enum {
	HINTS_LEFT = 70,
	HINTS_RIGHT = 570,
	HINTS_TOP = 92,
	HINTS_BOTTOM = 372,
	HINTS_VISIBLE_LINES = 17,
	HINTS_SCROLL_STEP = 3,
};

static struct {
	char file[128];
	int has;
	char text[4096];
	int scroll;
} s_hints;

int XwaModernMissionHintsScreen_Available(void) {
	if (!g_currentMissionFile[0]) {
		return 0;
	}
	if (strcmp(s_hints.file, g_currentMissionFile) != 0) {
		snprintf(s_hints.file, sizeof s_hints.file, "%s", g_currentMissionFile);
		s_hints.has = MissionDebrief_ReadFlightHints(s_hints.text, (int)sizeof s_hints.text);
		s_hints.scroll = 0;
	}
	return s_hints.has;
}

void XwaModernMissionHintsScreen_Reset(void) { s_hints.scroll = 0; }

int XwaModernMissionHintsScreen_Update(int menu_center_x, int* cursor_row) {
	XwaModernOptionsMenu menu;
	FrontendRect rect;
	int lines;
	int max_scroll;
	int key;
	int back;

	if (!cursor_row) {
		return 0;
	}
	if (!XwaModernMissionHintsScreen_Available()) {
		return 1;
	}
	/* Up / down (the keys or a thumbstick) scroll; read before the menu,
	 * whose single row would take them. */
	key = Config_GetMenuNavKey();
	if (key == 0x26 || key == 0x28) { /* VK_UP, VK_DOWN */
		Keyboard_FlushCharBuffer();
		s_hints.scroll += key == 0x26 ? -HINTS_SCROLL_STEP : HINTS_SCROLL_STEP;
	}

	XwaModernOptionsMenu_Begin(&menu, menu_center_x, 50, cursor_row, 1);
	XwaModernOptionsMenu_DrawTitle(&menu, "Mission Hints");

	/* Measure at full width (a first line past the end draws nothing); when
	 * it overflows, narrow the box for the scrollbar and measure again. */
	FrontendDraw_RectAssign(&rect, HINTS_LEFT, HINTS_TOP, HINTS_RIGHT, HINTS_BOTTOM);
	lines = FrontendText_DrawWrappedClipped(12, s_hints.text, &rect, g_colorLightBlue, 4, 4096) + 1;
	if (lines > HINTS_VISIBLE_LINES) {
		FrontendDraw_RectAssign(&rect, HINTS_LEFT, HINTS_TOP, HINTS_RIGHT - 20, HINTS_BOTTOM);
		lines = FrontendText_DrawWrappedClipped(12, s_hints.text, &rect, g_colorLightBlue, 4, 4096) + 1;
		max_scroll = lines - HINTS_VISIBLE_LINES;
		s_hints.scroll = s_hints.scroll < 0 ? 0 : s_hints.scroll > max_scroll ? max_scroll : s_hints.scroll;
		FrontendDraw_RectAssign(&rect, HINTS_RIGHT - 19, HINTS_TOP, HINTS_RIGHT, HINTS_BOTTOM);
		s_hints.scroll = FrontendScrollbar_Draw(&rect, s_hints.scroll, lines, 0, 5, g_colorNavy, 11);
		s_hints.scroll = s_hints.scroll < 0 ? 0 : s_hints.scroll > max_scroll ? max_scroll : s_hints.scroll;
		FrontendDraw_RectAssign(&rect, HINTS_LEFT, HINTS_TOP, HINTS_RIGHT - 20, HINTS_BOTTOM);
	} else {
		s_hints.scroll = 0;
	}
	/* Clipped to the box (a large bottom adjust: no reveal animation). */
	FrontendText_DrawWrappedClippedEx(12, s_hints.text, &rect, g_colorLightBlue, 4, s_hints.scroll, 4096);

	menu.y = HINTS_BOTTOM + 22;
	back = XwaModernOptionsMenu_DrawAction(&menu, FrontendString_Get(STR_BACK), 87, 0);
	back |= XwaModernOptionsMenu_TakeEscape(&menu);
	return back;
}

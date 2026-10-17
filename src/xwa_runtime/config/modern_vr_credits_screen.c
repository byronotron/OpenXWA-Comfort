#include "xwa_runtime/config/modern_vr_credits_screen.h"

#include "xwa/assets/string_table.h"
#include "xwa/frontend/frontend_color.h"
#include "xwa/frontend/frontend_draw.h"
#include "xwa/frontend/frontend_text.h"
#include "xwa_runtime/config/modern_options_menu.h"

#include <stddef.h>

typedef enum { CREDIT_GAP, CREDIT_HEADING, CREDIT_TEXT, CREDIT_NAME } CreditStyle;

static const struct {
	CreditStyle style;
	const char* text;
} k_credits[] = {
	{ CREDIT_TEXT, "This VR port is based on the open source project" },
	{ CREDIT_TEXT, "OpenXWA by Adrien Moulin" },
	{ CREDIT_GAP, NULL },
	{ CREDIT_HEADING, "VR Design, Code Prompting & QA" },
	{ CREDIT_NAME, "Neitey" },
	{ CREDIT_GAP, NULL },
	{ CREDIT_HEADING, "Voice Commands" },
	{ CREDIT_TEXT, "sherpa-onnx by k2-fsa (Next-gen Kaldi)" },
	{ CREDIT_TEXT, "Zipformer English model from k2-fsa icefall" },
	{ CREDIT_TEXT, "running on ONNX Runtime by Microsoft" },
	{ CREDIT_GAP, NULL },
	{ CREDIT_HEADING, "Disclaimer" },
	{ CREDIT_TEXT, "VR-specific code was generated using Claude AI." },
	{ CREDIT_TEXT, "Additional artwork was created using ChatGPT." },
};

int XwaModernVrCreditsScreen_Update(int menu_center_x, int* cursor_row) {
	XwaModernOptionsMenu menu;
	FrontendRect rect;
	int back;

	if (!cursor_row) {
		return 0;
	}
	XwaModernOptionsMenu_Begin(&menu, menu_center_x, 90, cursor_row, 1);
	XwaModernOptionsMenu_DrawTitle(&menu, "VR Port Credits");
	menu.y += 18;
	for (size_t i = 0; i < sizeof k_credits / sizeof k_credits[0]; ++i) {
		switch (k_credits[i].style) {
			case CREDIT_GAP:
				menu.y += 14;
				break;
			case CREDIT_HEADING:
				FrontendDraw_RectAssign(&rect, 0, menu.y, 639, menu.y + 15);
				FrontendText_DrawCentered(15, k_credits[i].text, &rect, g_colorLightBlue);
				menu.y += 21;
				break;
			case CREDIT_NAME:
				FrontendDraw_RectAssign(&rect, 0, menu.y, 639, menu.y + 15);
				FrontendText_DrawCentered(15, k_credits[i].text, &rect, g_colorPaleCyan);
				menu.y += 21;
				break;
			default:
				FrontendDraw_RectAssign(&rect, 0, menu.y, 639, menu.y + 12);
				FrontendText_DrawCentered(12, k_credits[i].text, &rect, g_colorPaleBlue);
				menu.y += 18;
				break;
		}
	}
	menu.y += 24;
	back = XwaModernOptionsMenu_DrawAction(&menu, FrontendString_Get(STR_BACK), 87, 0);
	back |= XwaModernOptionsMenu_TakeEscape(&menu);
	return back;
}

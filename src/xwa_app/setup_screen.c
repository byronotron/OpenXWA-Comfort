#include "setup_screen.h"

#include "aeron/aeron.h"
#include "aeron/log.h"
#include "aeron/render.h"
#include "aeron/scene/font_atlas.h"
#include "aeron/scene/ui.h"
#include "aeron/vfs.h"

#include <string.h>

#define XWA_SETUP_FONT_BASENAME "fonts/setup"
#define XWA_SETUP_FONT_MAX_SIZE (4u * 1024u * 1024u)
#define XWA_SETUP_WINDOW_WIDTH  1500.0f
/* Larger than the desktop default: the VR virtual screen is ~2 m wide. */
#define XWA_SETUP_UI_SCALE      1.6f

static int setup_load_font(AeronFontAtlas* atlas) {
	AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
	if (!cmd) {
		return 0;
	}
	if (!AeronFontAtlas_LoadVfs(atlas, cmd, Aeron_GetVfs(), AERON_VFS_ROOT_RESOURCE, XWA_SETUP_FONT_BASENAME,
								XWA_SETUP_FONT_MAX_SIZE)) {
		Aeron_CancelCommandBuffer(cmd);
		return 0;
	}
	if (!Aeron_SubmitCommandBuffer(cmd)) {
		AeronFontAtlas_Release(atlas);
		return 0;
	}
	return 1;
}

int XwaSetupScreen_Run(const char* title, const char* message, const char* retry_label) {
	AeronFontAtlas  atlas;
	AeronUiContext* ui;
	int             result = -1;

	memset(&atlas, 0, sizeof atlas);
	if (!setup_load_font(&atlas)) {
		Aeron_LogError("xwa.setup", "could not load the setup font '%s'", XWA_SETUP_FONT_BASENAME);
		return XWA_SETUP_SCREEN_QUIT;
	}
	ui = AeronUi_Create(NULL);
	if (!ui) {
		AeronFontAtlas_Release(&atlas);
		return XWA_SETUP_SCREEN_QUIT;
	}
	AeronUi_SetFonts(ui, &(AeronUiFontSet) { .regular = &atlas });
	{
		AeronUiTheme theme = *AeronUi_GetTheme(ui);
		theme.text_px *= XWA_SETUP_UI_SCALE;
		theme.title_px *= XWA_SETUP_UI_SCALE;
		theme.help_px *= XWA_SETUP_UI_SCALE;
		theme.row_height *= XWA_SETUP_UI_SCALE;
		theme.title_height *= XWA_SETUP_UI_SCALE;
		theme.window_pad *= XWA_SETUP_UI_SCALE;
		theme.item_spacing *= XWA_SETUP_UI_SCALE;
		theme.column_spacing *= XWA_SETUP_UI_SCALE;
		AeronUi_SetTheme(ui, &theme);
	}
	/* The VR laser pointer drives the mouse on the virtual screen. */
	Aeron_XrSetPointerEnabled(1);

	while (result < 0 && !Aeron_QuitRequested() && !Aeron_FatalErrorRequested()) {
		const int32_t delta_us = Aeron_BeginFrame();
		if (Aeron_FatalErrorRequested()) {
			break;
		}
		AeronUi_BeginFrame(ui, &(AeronUiFrameDesc) {
								   .input      = Aeron_InputSnapshot(),
								   .dt_seconds = (float)delta_us * 1e-6f,
							   });
		if (AeronUi_BeginWindow(ui, title,
								&(AeronUiWindowDesc) { .width_ref = XWA_SETUP_WINDOW_WIDTH, .centered = 1 })) {
			AeronUi_Help(ui, message);
			AeronUi_Spacer(ui, 16.0f);
			AeronUi_Separator(ui);
			if (retry_label) {
				int retry = 0;
				int quit  = 0;
				AeronUi_BeginColumns(ui, 2, NULL);
				retry = AeronUi_Button(ui, retry_label);
				AeronUi_NextColumn(ui);
				quit = AeronUi_Button(ui, "Quit");
				AeronUi_EndColumns(ui);
				/* A trigger pull can both click under the pointer and accept
				 * the focused button; prefer the harmless choice. */
				if (retry) {
					result = XWA_SETUP_SCREEN_RETRY;
				} else if (quit) {
					result = XWA_SETUP_SCREEN_QUIT;
				}
			} else if (AeronUi_Button(ui, "Quit")) {
				result = XWA_SETUP_SCREEN_QUIT;
			}
			AeronUi_EndWindow(ui);
		}
		/* Cancel (Esc / menu button / pad East) is ignored on purpose: the
		 * right grip maps to pad East and would quit by accident. */
		(void)AeronUi_EndFrame(ui);
		AeronUi_Submit(ui);
		if (!Aeron_Present()) {
			Aeron_RequestFatalRendererError("setup screen presentation");
		}
	}

	AeronUi_Destroy(ui);
	AeronFontAtlas_Release(&atlas);
	return result == XWA_SETUP_SCREEN_RETRY ? XWA_SETUP_SCREEN_RETRY : XWA_SETUP_SCREEN_QUIT;
}

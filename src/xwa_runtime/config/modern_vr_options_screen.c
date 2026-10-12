#include "xwa_runtime/config/modern_vr_options_screen.h"

#include "aeron/aeron.h"
#include "aeron/log.h"
#include "xwa/assets/string_table.h"
#include "xwa_runtime/config/modern_options_menu.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One setting: an environment key and its offered values. */
typedef struct VrSetting {
	const char* key;
	const char* label;
	const char* const* texts;   /* shown in the menu */
	const char* const* values;  /* written to the environment / cfg */
	int count;
	int fallback;               /* index used when the key is unset */
	int button_id;
} VrSetting;

enum {
	VR_HUD_LAYOUT,
	VR_HUD_SIZE,
	VR_DEPTH_BOOST,
	VR_WORLD_SCALE,
	VR_POINT_LIGHTS,
	VR_SHADOWS,
	VR_CONSOLE,
	VR_GLOVES,
	VR_ENVIRONMENT,
	VR_RESOLUTION,
	VR_COUNT
};

static const char* const k_layout_texts[] = { "Cockpit", "Helmet", "Fixed" };
static const char* const k_layout_values[] = { "cockpit", "helmet", "fixed" };
static const char* const k_size_texts[] = { "Small", "Medium", "Large", "Larger", "Largest" };
static const char* const k_size_values[] = { "0.45", "0.55", "0.65", "0.75", "0.85" };
static const char* const k_boost_texts[] = { "1x (real)", "1.5x", "2x", "3x", "4x", "5x", "6x", "8x", "10x" };
static const char* const k_boost_values[] = { "1.0", "1.5", "2", "3", "4", "5", "6", "8", "10" };
static const char* const k_scale_texts[] = { "0.60 (huge)", "0.70", "0.75", "0.80", "0.85",
											  "0.90",        "1.00 (real)", "1.10", "1.20" };
static const char* const k_scale_values[] = { "0.6", "0.7", "0.75", "0.8", "0.85", "0.9", "1.0", "1.1", "1.2" };
static const char* const k_toggle_texts[] = { "Off", "On" };
static const char* const k_toggle_values[] = { "0", "1" };
static const char* const k_res_texts[] = { "70%", "80%", "90%", "100%", "110%", "120%" };
static const char* const k_res_values[] = { "0.7", "0.8", "0.9", "1.0", "1.1", "1.2" };

static const VrSetting k_settings[VR_COUNT] = {
	{ "XWA_XR_HUD_LAYOUT", "HUD Layout", k_layout_texts, k_layout_values, 3, 0, 80 },
	{ "XWA_XR_HUD_SCALE", "Helmet HUD Size", k_size_texts, k_size_values, 5, 2, 81 },
	{ "XWA_XR_DEPTH_BOOST", "Depth Boost", k_boost_texts, k_boost_values, 9, 0, 82 },
	{ "XWA_XR_WORLD_SCALE", "World Scale", k_scale_texts, k_scale_values, 9, 6, 83 },
	{ "XWA_POINT_LIGHTS", "Dynamic Lights", k_toggle_texts, k_toggle_values, 2, 0, 84 },
	{ "XWA_XR_SHADOWS", "Cockpit Shadows", k_toggle_texts, k_toggle_values, 2, 0, 85 },
	{ "XWA_XR_CONSOLE", "Lap Console", k_toggle_texts, k_toggle_values, 2, 1, 88 },
	{ "XWA_XR_GLOVES", "Pilot Gloves", k_toggle_texts, k_toggle_values, 2, 1, 89 },
	{ "XWA_XR_ENVIRONMENT", "Menu Hangar", k_toggle_texts, k_toggle_values, 2, 1, 90 },
	{ "AERON_XR_RESOLUTION_SCALE", "Resolution (restart)", k_res_texts, k_res_values, 6, 2, 86 },
};

static struct {
	int loaded;
	uint8_t index[VR_COUNT];
	uint8_t dirty[VR_COUNT];
} g_vr;

/* Index of the offered value nearest to the environment's current value. */
static int vr_index_from_env(const VrSetting* setting) {
	const char* env = SDL_getenv(setting->key);
	int best = setting->fallback;
	if (!env || !env[0]) {
		return best;
	}
	if (setting->values == k_layout_values) {
		for (int i = 0; i < setting->count; i++) {
			if (SDL_strcasecmp(env, setting->values[i]) == 0) {
				return i;
			}
		}
		return best;
	}
	{
		const double v = atof(env);
		double best_d = 1e30;
		for (int i = 0; i < setting->count; i++) {
			const double d = fabs(atof(setting->values[i]) - v);
			if (d < best_d) {
				best_d = d;
				best = i;
			}
		}
	}
	return best;
}

static void vr_load(void) {
	if (g_vr.loaded) {
		return;
	}
	for (int i = 0; i < VR_COUNT; i++) {
		g_vr.index[i] = (uint8_t)vr_index_from_env(&k_settings[i]);
		g_vr.dirty[i] = 0;
	}
	g_vr.loaded = 1;
}

int XwaModernVrOptions_Available(void) {
#if defined(SDL_PLATFORM_ANDROID)
	return 1;
#else
	return Aeron_XrIsPresenting();
#endif
}

/* Writes the changed settings into openxwa-quest.cfg: an active KEY= line
 * is replaced, else the template's commented #KEY= line is activated, else
 * the line is appended. Other lines are kept as they are. */
static void vr_persist(void) {
#if defined(SDL_PLATFORM_ANDROID)
	const char* external = SDL_GetAndroidExternalStoragePath();
	char path[1024];
	char* text = NULL;
	size_t length = 0;
	FILE* f;
	int any = 0;
	int has_active[VR_COUNT] = { 0 };
	int written[VR_COUNT] = { 0 };

	for (int i = 0; i < VR_COUNT; i++) {
		any |= g_vr.dirty[i];
	}
	if (!any || !external || !external[0]) {
		return;
	}
	snprintf(path, sizeof path, "%s/openxwa-quest.cfg", external);
	f = fopen(path, "rb");
	if (f) {
		long size;
		fseek(f, 0, SEEK_END);
		size = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (size > 0 && size < 256 * 1024) {
			text = (char*)malloc((size_t)size + 1);
			if (text) {
				length = fread(text, 1, (size_t)size, f);
				text[length] = '\0';
			}
		}
		fclose(f);
	}

	/* Lines as [start, end) ranges; a key matches "KEY=" after optional
	 * whitespace and (for the commented form) one '#'. */
	for (int pass = 0; pass < 2; pass++) {
		FILE* out = NULL;
		const char* p = text ? text : "";
		if (pass == 1) {
			out = fopen(path, "wb");
			if (!out) {
				Aeron_LogError("xwa.config", "cannot write %s", path);
				free(text);
				return;
			}
		}
		while (*p) {
			const char* end = strchr(p, '\n');
			const char* next = end ? end + 1 : p + strlen(p);
			const char* q = p;
			int commented = 0;
			int match = -1;
			while (*q == ' ' || *q == '\t') {
				q++;
			}
			if (*q == '#') {
				commented = 1;
				q++;
			}
			for (int i = 0; i < VR_COUNT; i++) {
				const size_t n = strlen(k_settings[i].key);
				if (g_vr.dirty[i] && strncmp(q, k_settings[i].key, n) == 0 && q[n] == '=') {
					match = i;
					break;
				}
			}
			if (pass == 0) {
				if (match >= 0 && !commented) {
					has_active[match] = 1;
				}
			} else if (match >= 0 && !written[match] && (!commented || !has_active[match])) {
				fprintf(out, "%s=%s\n", k_settings[match].key,
						k_settings[match].values[g_vr.index[match]]);
				written[match] = 1;
			} else if (match >= 0 && !commented) {
				/* a later duplicate of a key already written: drop it */
			} else {
				fwrite(p, 1, (size_t)(next - p), out);
				if (!end) {
					fputc('\n', out);
				}
			}
			p = next;
		}
		if (pass == 1) {
			for (int i = 0; i < VR_COUNT; i++) {
				if (g_vr.dirty[i] && !written[i]) {
					fprintf(out, "%s=%s\n", k_settings[i].key, k_settings[i].values[g_vr.index[i]]);
				}
			}
			fclose(out);
		}
	}
	free(text);
	Aeron_LogInfo("xwa.config", "VR options saved to %s", path);
#endif
	memset(g_vr.dirty, 0, sizeof g_vr.dirty);
}

int XwaModernVrOptionsScreen_Update(int menu_center_x, int* cursor_row) {
	XwaModernOptionsMenu menu;
	int back;

	if (!cursor_row) {
		return 0;
	}
	vr_load();
	XwaModernOptionsMenu_Begin(&menu, menu_center_x, 150, cursor_row, VR_COUNT + 1);
	XwaModernOptionsMenu_DrawTitle(&menu, "OpenXWA VR Options");

	for (int i = 0; i < VR_COUNT; i++) {
		const VrSetting* setting = &k_settings[i];
		/* The helmet HUD size only applies to the helmet layout. */
		const int disabled = i == VR_HUD_SIZE && g_vr.index[VR_HUD_LAYOUT] != 1;
		uint8_t value = g_vr.index[i];
		if (XwaModernOptionsMenu_DrawCycleU8(&menu, &value, setting->label, setting->texts, setting->count,
											 setting->button_id, disabled) &&
			value != g_vr.index[i]) {
			g_vr.index[i] = value;
			g_vr.dirty[i] = 1;
			SDL_setenv_unsafe(setting->key, setting->values[value], 1);
			Aeron_LogInfo("xwa.config", "VR option %s=%s", setting->key, setting->values[value]);
		}
	}

	back = XwaModernOptionsMenu_DrawAction(&menu, FrontendString_Get(STR_BACK), 87, 0);
	back |= XwaModernOptionsMenu_TakeEscape(&menu);
	if (!back) {
		return 0;
	}
	vr_persist();
	return 1;
}

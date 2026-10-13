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
	VR_FLIGHT_STICK,
	VR_STICK_TWIST,
	VR_INVERT_PITCH,
	VR_VOICE,
	VR_BUTTON_CARD,
	VR_ENVIRONMENT,
	VR_RESOLUTION,
	VR_REFRESH,
	VR_GPU_OPT,
	VR_FOVEATION,
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
static const char* const k_voice_texts[] = { "Off", "Double-tap Thumb Rest", "Hold Thumb Rest" };
static const char* const k_voice_values[] = { "0", "doubletap", "hold" };
static const char* const k_res_texts[] = { "70%", "80%", "90%", "100%", "110%", "120%" };
static const char* const k_res_values[] = { "0.7", "0.8", "0.9", "1.0", "1.1", "1.2" };
static const char* const k_fov_texts[] = { "Off", "Low", "Medium", "High" };
static const char* const k_fov_values[] = { "0", "low", "medium", "high" };
static const char* const k_env_texts[] = { "Off", "Battle of Endor", "Hangar" };
static const char* const k_env_values[] = { "0", "endor", "hangar" };
static const char* const k_refresh_texts[] = { "Headset Setting", "72 Hz", "80 Hz", "90 Hz", "120 Hz" };
static const char* const k_refresh_values[] = { "0", "72", "80", "90", "120" };

static const VrSetting k_settings[VR_COUNT] = {
	{ "XWA_XR_HUD_LAYOUT", "HUD Layout", k_layout_texts, k_layout_values, 3, 0, 80 },
	{ "XWA_XR_HUD_SCALE", "Helmet HUD Size", k_size_texts, k_size_values, 5, 2, 81 },
	{ "XWA_XR_DEPTH_BOOST", "Depth Boost", k_boost_texts, k_boost_values, 9, 0, 82 },
	{ "XWA_XR_WORLD_SCALE", "World Scale", k_scale_texts, k_scale_values, 9, 6, 83 },
	{ "XWA_POINT_LIGHTS", "Dynamic Lights", k_toggle_texts, k_toggle_values, 2, 0, 84 },
	{ "XWA_XR_SHADOWS", "Cockpit Shadows", k_toggle_texts, k_toggle_values, 2, 0, 85 },
	{ "XWA_XR_CONSOLE", "Lap Console", k_toggle_texts, k_toggle_values, 2, 1, 88 },
	{ "XWA_XR_GLOVES", "Pilot Gloves", k_toggle_texts, k_toggle_values, 2, 1, 89 },
	{ "XWA_XR_FLIGHT_STICK", "Flight Stick", k_toggle_texts, k_toggle_values, 2, 1, 94 },
	{ "XWA_XR_STICK_TWIST", "Stick Twist Rolls", k_toggle_texts, k_toggle_values, 2, 0, 96 },
	{ "AERON_XR_INVERT_PITCH", "Invert Thumbstick Pitch", k_toggle_texts, k_toggle_values, 2, 0, 97 },
	{ "XWA_XR_VOICE", "Voice Commands", k_voice_texts, k_voice_values, 3, 1, 98 },
	{ "XWA_XR_BUTTON_CARD", "Button Card", k_toggle_texts, k_toggle_values, 2, 1, 92 },
	{ "XWA_XR_ENVIRONMENT", "Menu Scene", k_env_texts, k_env_values, 3, 1, 90 },
	{ "AERON_XR_RESOLUTION_SCALE", "Resolution (restart)", k_res_texts, k_res_values, 6, 2, 86 },
	{ "AERON_XR_REFRESH_RATE", "Refresh Rate", k_refresh_texts, k_refresh_values, 5, 0, 99 },
	{ "XWA_XR_GPU_OPT", "GPU Optimizations", k_toggle_texts, k_toggle_values, 2, 1, 79 },
	{ "XWA_XR_FOVEATION", "Foveation (restart)", k_fov_texts, k_fov_values, 4, 1, 78 },
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
	if (setting->values == k_layout_values || setting->values == k_voice_values || setting->values == k_fov_values ||
		setting->values == k_env_values) {
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

/* Writes KEY=value lines into openxwa-quest.cfg: an active KEY= line is
 * replaced, else the template's commented #KEY= line is activated, else the
 * line is appended. Other lines are kept as they are. */
static void vr_write_cfg(const char* const* keys, const char* const* values, int count) {
#if defined(SDL_PLATFORM_ANDROID)
	const char* external = SDL_GetAndroidExternalStoragePath();
	char path[1024];
	char* text = NULL;
	size_t length = 0;
	FILE* f;
	int has_active[32] = { 0 };
	int written[32] = { 0 };

	if (count <= 0 || count > 32 || !external || !external[0]) {
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
			for (int i = 0; i < count; i++) {
				const size_t n = strlen(keys[i]);
				if (strncmp(q, keys[i], n) == 0 && q[n] == '=') {
					match = i;
					break;
				}
			}
			if (pass == 0) {
				if (match >= 0 && !commented) {
					has_active[match] = 1;
				}
			} else if (match >= 0 && !written[match] && (!commented || !has_active[match])) {
				fprintf(out, "%s=%s\n", keys[match], values[match]);
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
			for (int i = 0; i < count; i++) {
				if (!written[i]) {
					fprintf(out, "%s=%s\n", keys[i], values[i]);
				}
			}
			fclose(out);
		}
	}
	free(text);
	Aeron_LogInfo("xwa.config", "VR settings saved to %s", path);
#else
	(void)keys;
	(void)values;
	(void)count;
#endif
}

void XwaModernVrOptions_SaveValue(const char* key, const char* value) {
	if (key && value) {
		vr_write_cfg(&key, &value, 1);
	}
}

static void vr_persist(void) {
	const char* keys[VR_COUNT];
	const char* values[VR_COUNT];
	int n = 0;
	for (int i = 0; i < VR_COUNT; i++) {
		if (g_vr.dirty[i]) {
			keys[n] = k_settings[i].key;
			values[n] = k_settings[i].values[g_vr.index[i]];
			n++;
		}
	}
	vr_write_cfg(keys, values, n);
	memset(g_vr.dirty, 0, sizeof g_vr.dirty);
}

/* Clears every cockpit's hand-placed HUD layout (XWA_XR_HUD_POSE_*). */
extern char** environ;
static void vr_reset_hud_layouts(void) {
	static const char prefix[] = "XWA_XR_HUD_POSE_";
	char keys[16][96];
	const char* key_ptrs[16];
	const char* empty[16];
	int n = 0;
	for (char** e = environ; e && *e && n < 16; ++e) {
		const char* eq = strchr(*e, '=');
		if (eq && strncmp(*e, prefix, sizeof prefix - 1) == 0 && (size_t)(eq - *e) < sizeof keys[0]) {
			snprintf(keys[n], sizeof keys[n], "%.*s", (int)(eq - *e), *e);
			n++;
		}
	}
	for (int i = 0; i < n; ++i) {
		SDL_setenv_unsafe(keys[i], "", 1);
		key_ptrs[i] = keys[i];
		empty[i] = "";
	}
	vr_write_cfg(key_ptrs, empty, n);
	Aeron_LogInfo("xwa.config", "VR HUD panels back at their default spots (%d cockpit layouts cleared)", n);
}

int XwaModernVrOptionsScreen_Update(int menu_center_x, int* cursor_row) {
	XwaModernOptionsMenu menu;
	int back;

	if (!cursor_row) {
		return 0;
	}
	vr_load();
	XwaModernOptionsMenu_Begin(&menu, menu_center_x, 44, cursor_row, VR_COUNT + 4);
	XwaModernOptionsMenu_DrawTitle(&menu, "OpenXWA VR Options");

	for (int i = 0; i < VR_COUNT; i++) {
		const VrSetting* setting = &k_settings[i];
		/* The helmet HUD size only applies to the helmet layout. */
		const int disabled = (i == VR_HUD_SIZE && g_vr.index[VR_HUD_LAYOUT] != 1) ||
							 (i == VR_STICK_TWIST && g_vr.index[VR_FLIGHT_STICK] == 0);
		uint8_t value = g_vr.index[i];
		if (XwaModernOptionsMenu_DrawCycleU8(&menu, &value, setting->label, setting->texts, setting->count,
											 setting->button_id, disabled) &&
			value != g_vr.index[i]) {
			g_vr.index[i] = value;
			g_vr.dirty[i] = 1;
			SDL_setenv_unsafe(setting->key, setting->values[value], 1);
			if (i == VR_GPU_OPT) {
				/* A/B switch: also the headset's GPU/CPU clock floor (live). */
				SDL_setenv_unsafe("AERON_XR_PERF_LEVEL", value ? "high" : "0", 1);
			}
			Aeron_LogInfo("xwa.config", "VR option %s=%s", setting->key, setting->values[value]);
			/* Saved at once: the "(restart)" options are often changed right
			 * before quitting, without backing out of this screen. */
			XwaModernVrOptions_SaveValue(setting->key, setting->values[value]);
		}
		menu.y -= 2; /* 18 px rows: the list has outgrown 20 */
	}

	if (XwaModernOptionsMenu_DrawAction(&menu, "Reset HUD Panel Positions", 93, 0)) {
		vr_reset_hud_layouts();
	}
	menu.y -= 2;
	if (XwaModernOptionsMenu_DrawAction(&menu, "Reset Lap Console Position", 91, 0)) {
		SDL_setenv_unsafe("XWA_XR_CONSOLE_POSE", "", 1);
		XwaModernVrOptions_SaveValue("XWA_XR_CONSOLE_POSE", "");
		Aeron_LogInfo("xwa.config", "lap console back at its default spot");
	}
	menu.y -= 2;
	if (XwaModernOptionsMenu_DrawAction(&menu, "Reset Flight Stick Position", 95, 0)) {
		SDL_setenv_unsafe("XWA_XR_STICK_POS", "", 1);
		XwaModernVrOptions_SaveValue("XWA_XR_STICK_POS", "");
		Aeron_LogInfo("xwa.config", "flight stick back at its default spot");
	}
	menu.y -= 2;
	back = XwaModernOptionsMenu_DrawAction(&menu, FrontendString_Get(STR_BACK), 87, 0);
	back |= XwaModernOptionsMenu_TakeEscape(&menu);
	if (!back) {
		return 0;
	}
	vr_persist();
	return 1;
}

#include "xwa_runtime/config/modern_vr_options_screen.h"

#include "aeron/aeron.h"
#include "aeron/log.h"
#include "xwa/assets/string_table.h"
#include "xwa_runtime/config/modern_options_menu.h"

#include <SDL3/SDL.h>

#include <errno.h>
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
	int page;                   /* 0 main, 1 Performance / GPU */
} VrSetting;

enum {
	VR_HUD_LAYOUT,
	VR_HUD_SIZE,
	VR_DEPTH_BOOST,
	VR_WORLD_SCALE,
	VR_POINT_LIGHTS,
	VR_SHADOWS,
	VR_CONSOLE,
	VR_WHEEL,
	VR_WHEEL_SLOWMO,
	VR_HANDS,
	VR_CONTROLLER_LABELS,
	VR_FLIGHT_STICK,
	VR_STICK_TWIST,
	VR_INVERT_PITCH,
	VR_HANDED,
	VR_VOICE,
	VR_BUTTON_CARD,
	VR_ENVIRONMENT,
	VR_SHIP_3D,
	VR_RESOLUTION,
	VR_REFRESH,
	VR_GPU_OPT,
	VR_FOVEATION,
	VR_SHADING,
	VR_DIRECT,
	VR_BLOOM,
	VR_ASTC,
	VR_GPU_TIMING,
	VR_COUNT
};

static const char* const k_layout_texts[] = { "Cockpit", "Helmet", "Fixed" };
static const char* const k_layout_values[] = { "cockpit", "helmet", "fixed" };
static const char* const k_size_texts[] = { "Small", "Medium", "Large", "Larger", "Largest" };
static const char* const k_size_values[] = { "0.45", "0.55", "0.65", "0.75", "0.85" };
static const char* const k_boost_texts[] = { "1x (real)", "1.5x", "2x (Recommended)", "3x", "4x", "5x", "6x", "8x", "10x" };
static const char* const k_boost_values[] = { "1.0", "1.5", "2", "3", "4", "5", "6", "8", "10" };
static const char* const k_scale_texts[] = { "0.60 (huge)", "0.70", "0.75 (Recommended)", "0.80", "0.85",
											  "0.90",        "1.00 (real)", "1.10", "1.20" };
static const char* const k_scale_values[] = { "0.6", "0.7", "0.75", "0.8", "0.85", "0.9", "1.0", "1.1", "1.2" };
static const char* const k_toggle_texts[] = { "Off", "On" };
static const char* const k_toggle_values[] = { "0", "1" };
static const char* const k_hand_texts[] = { "Right-handed", "Left-handed" };
static const char* const k_hands_texts[] = { "Pilot Gloves", "Controllers", "Off" };
static const char* const k_hands_values[] = { "gloves", "controllers", "off" };
static const char* const k_voice_texts[] = { "Off", "Double-tap Thumb Rest", "Hold Thumb Rest" };
static const char* const k_voice_values[] = { "0", "doubletap", "hold" };
static const char* const k_res_texts[] = { "70%", "80%", "90%", "100%", "110%", "120%" };
static const char* const k_res_values[] = { "0.7", "0.8", "0.9", "1.0", "1.1", "1.2" };
static const char* const k_fov_texts[] = { "Off", "Low", "Medium", "High" };
static const char* const k_fov_values[] = { "0", "low", "medium", "high" };
static const char* const k_env_texts[] = { "Off",           "Random",
											"XW: Alliance Box Art", "X-Wing Box Art",
											"TIE Fighter Box Art", "X-Wing vs. TIE Box Art", "Live Battle" };
static const char* const k_env_values[] = { "0", "random", "endor", "xwing", "tiefighter", "xvt", "battle" };
static const char* const k_depth_texts[] = { "Off (flat)", "Subtle", "Normal", "Strong" };
static const char* const k_depth_values[] = { "0", "0.5", "1", "1.6" };
static const char* const k_refresh_texts[] = { "Headset Setting", "72 Hz", "80 Hz", "90 Hz", "120 Hz" };
static const char* const k_refresh_values[] = { "0", "72", "80", "90", "120" };
static const char* const k_shading_texts[] = { "Full", "Lite (faster)" };
static const char* const k_shading_values[] = { "full", "lite" };
static const char* const k_bloom_texts[] = { "Shared (both eyes)", "Per Eye", "Off" };
static const char* const k_bloom_values[] = { "1", "eye", "0" };

/* Page 1 holds what costs or saves GPU time: each night-shift
 * optimization can be switched off there on its own to find a regression. */
static const VrSetting k_settings[VR_COUNT] = {
	{ "XWA_XR_HUD_LAYOUT", "HUD Layout", k_layout_texts, k_layout_values, 3, 0, 80, 0 },
	{ "XWA_XR_HUD_SCALE", "Helmet HUD Size", k_size_texts, k_size_values, 5, 2, 81, 0 },
	{ "XWA_XR_DEPTH_BOOST", "Depth Boost", k_boost_texts, k_boost_values, 9, 0, 82, 0 },
	{ "XWA_XR_WORLD_SCALE", "World Scale", k_scale_texts, k_scale_values, 9, 6, 83, 0 },
	{ "XWA_POINT_LIGHTS", "Dynamic Lights", k_toggle_texts, k_toggle_values, 2, 0, 84, 1 },
	{ "XWA_XR_SHADOWS", "Cockpit Shadows", k_toggle_texts, k_toggle_values, 2, 0, 85, 1 },
	{ "XWA_XR_CONSOLE", "Lap Console", k_toggle_texts, k_toggle_values, 2, 1, 88, 0 },
	/* Held open with the grip of the hand that has the left controls. */
	{ "XWA_XR_WHEEL", "Radial Menu (grip)", k_toggle_texts, k_toggle_values, 2, 1, 67, 0 },
	/* The game at 45 % speed while the wheel is open (single player). */
	{ "XWA_XR_WHEEL_SLOWMO", "Wheel Slow Motion", k_toggle_texts, k_toggle_values, 2, 1, 70, 0 },
	/* What your hands are: the gloves or the controllers (with labels of
	 * what their buttons do when you look at them). */
	{ "XWA_XR_HANDS", "Hands", k_hands_texts, k_hands_values, 3, 1, 89, 0 },
	{ "XWA_XR_CONTROLLER_LABELS", "Controller Button Labels", k_toggle_texts, k_toggle_values, 2, 1, 66, 0 },
	{ "XWA_XR_FLIGHT_STICK", "Flight Stick", k_toggle_texts, k_toggle_values, 2, 1, 94, 0 },
	{ "XWA_XR_STICK_TWIST", "Stick Twist Rolls", k_toggle_texts, k_toggle_values, 2, 0, 96, 0 },
	{ "AERON_XR_INVERT_PITCH", "Invert Thumbstick Pitch", k_toggle_texts, k_toggle_values, 2, 0, 97, 0 },
	/* Mirrors the controllers' game roles (the flight stick stays right). */
	{ "AERON_XR_LEFT_HANDED", "Controls", k_hand_texts, k_toggle_values, 2, 0, 75, 0 },
	{ "XWA_XR_VOICE", "Voice Commands", k_voice_texts, k_voice_values, 3, 1, 98, 0 },
	{ "XWA_XR_BUTTON_CARD", "Button Card", k_toggle_texts, k_toggle_values, 2, 1, 92, 0 },
	{ "XWA_XR_ENVIRONMENT", "Menu Scene", k_env_texts, k_env_values, 7, 4, 90, 0 }, /* TIE Fighter box art */
	/* Ship previews (mission setup, tech room) in stereo on the menu screen. */
	{ "XWA_XR_SHIP_3D", "3D Ship Previews", k_depth_texts, k_depth_values, 4, 2, 68, 0 },
	{ "AERON_XR_RESOLUTION_SCALE", "Resolution (restart)", k_res_texts, k_res_values, 6, 4, 86, 1 },
	{ "AERON_XR_REFRESH_RATE", "Refresh Rate", k_refresh_texts, k_refresh_values, 5, 0, 99, 1 },
	/* Near-first draw order and the GPU clock floor. */
	{ "XWA_XR_GPU_OPT", "GPU Optimizations", k_toggle_texts, k_toggle_values, 2, 1, 79, 1 },
	{ "XWA_XR_FOVEATION", "Foveation (restart)", k_fov_texts, k_fov_values, 4, 1, 78, 1 },
	{ "XWA_XR_SHADING", "Ship Shading (restart)", k_shading_texts, k_shading_values, 2, 1, 77, 1 },
	{ "XWA_XR_DIRECT_OUTPUT", "Direct Output (no bloom)", k_toggle_texts, k_toggle_values, 2, 0, 76, 1 },
	/* One bloom computed from the left eye for both (fewer passes). */
	{ "XWA_XR_BLOOM", "Bloom", k_bloom_texts, k_bloom_values, 3, 0, 74, 1 },
	/* Ship textures as ASTC: read when a ship's textures load. */
	{ "XWA_XR_ASTC", "Compressed Textures (restart)", k_toggle_texts, k_toggle_values, 2, 1, 73, 1 },
	/* The shim's per-pass GPU timestamps (the log's "GPU per frame"). */
	{ "XWA_XR_GPU_TIMING", "GPU Timing in Log (restart)", k_toggle_texts, k_toggle_values, 2, 1, 72, 1 },
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
		setting->values == k_env_values || setting->values == k_shading_values || setting->values == k_bloom_values ||
		setting->values == k_hands_values) {
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
	char tmp_path[1100];
	snprintf(tmp_path, sizeof tmp_path, "%s.new", path);
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
			/* Into a new file, then over the old one: a cfg copied in over USB
			 * may not be writable in place by the app, but the folder is. */
			out = fopen(tmp_path, "wb");
			if (!out) {
				Aeron_LogError("xwa.config", "cannot write %s: %s", tmp_path, strerror(errno));
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
			if (fclose(out) != 0) {
				Aeron_LogError("xwa.config", "cannot finish %s: %s", tmp_path, strerror(errno));
				remove(tmp_path);
				free(text);
				return;
			}
			if (rename(tmp_path, path) != 0) {
				const int rename_errno = errno;
				/* Some storage refuses to replace: remove the old file first. */
				if (remove(path) != 0 || rename(tmp_path, path) != 0) {
					Aeron_LogError("xwa.config", "cannot replace %s: %s / %s", path, strerror(rename_errno),
								   strerror(errno));
					remove(tmp_path);
					free(text);
					return;
				}
			}
		}
	}
	free(text);
	/* Read it back: a setting that does not stick should say so. */
	{
		FILE* check = fopen(path, "rb");
		char line[2048];
		int found[32] = { 0 };
		while (check && fgets(line, sizeof line, check)) {
			for (int i = 0; i < count; i++) {
				const size_t n = strlen(keys[i]);
				if (strncmp(line, keys[i], n) == 0 && line[n] == '=' &&
					strncmp(line + n + 1, values[i], strlen(values[i])) == 0) {
					found[i] = 1;
				}
			}
		}
		if (check) {
			fclose(check);
		}
		for (int i = 0; i < count; i++) {
			if (found[i]) {
				Aeron_LogInfo("xwa.config", "VR setting saved: %s=%s (%s)", keys[i], values[i], path);
			} else {
				Aeron_LogError("xwa.config", "VR setting NOT saved: %s=%s (%s)", keys[i], values[i], path);
			}
		}
	}
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

/* "Restore VR Defaults" (main menu): the recommended settings, tuned on a
 * Quest 3. A VR option not listed gets its built-in default; the resolution
 * stays within each headset's pixel budget (the device tier clamps it). */
static const struct {
	const char* key;
	const char* value;
} k_recommended[] = {
	{ "XWA_XR_WORLD_SCALE", "0.75" },
	{ "XWA_POINT_LIGHTS", "0" },
	{ "XWA_XR_DEPTH_BOOST", "2" },
	{ "XWA_XR_FOVEATION", "high" },
	{ "AERON_XR_RESOLUTION_SCALE", "1.1" },
	{ "XWA_XR_FLIGHT_STICK", "0" },
	{ "AERON_XR_REFRESH_RATE", "72" },
	{ "XWA_XR_HUD_LAYOUT", "cockpit" },
	{ "XWA_XR_HUD_SCALE", "0.55" },
	{ "XWA_XR_SHADING", "lite" },
	{ "XWA_XR_DIRECT_OUTPUT", "0" },
	{ "XWA_XR_ENVIRONMENT", "tiefighter" },
	{ "XWA_XR_VOICE", "doubletap" },
	{ "XWA_XR_GPU_OPT", "1" },
	{ "XWA_XR_GPU_TIMING", "0" },
	{ "XWA_XR_ASTC", "1" },
	{ "XWA_XR_BLOOM", "1" },
	{ "XWA_XR_SHIP_3D", "0.5" },
	{ "AERON_XR_INVERT_PITCH", "0" },
	{ "XWA_XR_HANDS", "controllers" },
};

/* Hand-placed spots that come with them (LOCAL space, metres; HUD panels
 * per cockpit). Every other cockpit's HUD, and the flight stick, go back
 * to their default spots. */
static const struct {
	const char* key;
	const char* value;
} k_recommended_places[] = {
	{ "XWA_XR_CONSOLE_POSE", "0.0120,-0.5343,-0.5958,-0.48705,-0.00045,0.00323,0.87337" },
	{ "XWA_XR_STICK_POS", "" },
	{ "XWA_XR_HUD_POSE_XWINGCOCKPIT", "pod_l=-0.4130,-0.0378,-0.5794,-0.02721,0.27131,0.02440,0.96180;"
									  "pod_r=0.4016,0.0013,-0.5742,0.01347,-0.32619,0.00733,0.94518" },
	{ "XWA_XR_HUD_POSE_CORELLIANTRANSPORT2COCKPIT",
	  "pod_l=-0.3468,0.0794,-0.6747,-0.00307,0.22331,-0.00042,0.97474;"
	  "pod_r=0.4555,0.0481,-0.7340,0.01256,-0.27346,0.01234,0.96172" },
	{ "XWA_XR_HUD_POSE_CORELLIANTRANSPORTGUNNER",
	  "pod_l=-0.4119,-0.0053,-0.6071,0.01244,0.33782,-0.01006,0.94107;"
	  "pod_r=0.4409,0.0234,-0.5578,0.03452,-0.34751,0.01515,0.93692" },
};

void XwaModernVrOptions_RestoreDefaults(void) {
	const char* keys[VR_COUNT];
	const char* values[VR_COUNT];
	for (int i = 0; i < VR_COUNT; i++) {
		keys[i] = k_settings[i].key;
		values[i] = k_settings[i].values[k_settings[i].fallback];
		for (size_t r = 0; r < sizeof k_recommended / sizeof k_recommended[0]; r++) {
			if (strcmp(k_recommended[r].key, keys[i]) == 0) {
				values[i] = k_recommended[r].value;
			}
		}
		SDL_setenv_unsafe(keys[i], values[i], 1);
	}
	SDL_setenv_unsafe("AERON_XR_PERF_LEVEL", strcmp(SDL_getenv("XWA_XR_GPU_OPT"), "0") ? "high" : "0", 1);
	vr_write_cfg(keys, values, VR_COUNT);
	vr_reset_hud_layouts();
	const char* place_keys[sizeof k_recommended_places / sizeof k_recommended_places[0]];
	const char* place_values[sizeof k_recommended_places / sizeof k_recommended_places[0]];
	const int places = (int)(sizeof k_recommended_places / sizeof k_recommended_places[0]);
	for (int i = 0; i < places; i++) {
		place_keys[i] = k_recommended_places[i].key;
		place_values[i] = k_recommended_places[i].value;
		SDL_setenv_unsafe(place_keys[i], place_values[i], 1);
	}
	vr_write_cfg(place_keys, place_values, places);
	g_vr.loaded = 0; /* the VR Options screen reads them again */
	Aeron_LogInfo("xwa.config", "VR settings restored to the recommended defaults");
}

/* One cycle row per setting of `page`. */
static void vr_draw_settings(XwaModernOptionsMenu* menu, int page) {
	for (int i = 0; i < VR_COUNT; i++) {
		const VrSetting* setting = &k_settings[i];
		if (setting->page != page) {
			continue;
		}
		/* The helmet HUD size only applies to the helmet layout. */
		const int disabled = (i == VR_HUD_SIZE && g_vr.index[VR_HUD_LAYOUT] != 1) ||
							 (i == VR_STICK_TWIST && g_vr.index[VR_FLIGHT_STICK] == 0) ||
							 (i == VR_WHEEL_SLOWMO && g_vr.index[VR_WHEEL] == 0);
		uint8_t value = g_vr.index[i];
		if (XwaModernOptionsMenu_DrawCycleU8(menu, &value, setting->label, setting->texts, setting->count,
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
		menu->y -= 4; /* 16 px rows */
	}
}

static int vr_page_rows(int page) {
	int n = 0;
	for (int i = 0; i < VR_COUNT; i++) {
		n += k_settings[i].page == page;
	}
	return n;
}

int XwaModernVrOptionsScreen_Update(int menu_center_x, int* cursor_row) {
	static int page;
	XwaModernOptionsMenu menu;
	int back;

	if (!cursor_row) {
		return 0;
	}
	vr_load();
	if (page == 1) {
		/* Performance / GPU: every optimization on its own switch. */
		XwaModernOptionsMenu_Begin(&menu, menu_center_x, 38, cursor_row, vr_page_rows(1) + 1);
		XwaModernOptionsMenu_DrawTitle(&menu, "VR Performance / GPU");
		vr_draw_settings(&menu, 1);
		back = XwaModernOptionsMenu_DrawAction(&menu, FrontendString_Get(STR_BACK), 87, 0);
		back |= XwaModernOptionsMenu_TakeEscape(&menu);
		if (back) {
			page = 0;
			*cursor_row = vr_page_rows(0) + 3; /* on "Performance / GPU..." */
		}
		return 0;
	}
	XwaModernOptionsMenu_Begin(&menu, menu_center_x, 38, cursor_row, vr_page_rows(0) + 5);
	XwaModernOptionsMenu_DrawTitle(&menu, "VR Options");
	vr_draw_settings(&menu, 0);

	if (XwaModernOptionsMenu_DrawAction(&menu, "Reset HUD Panel Positions", 93, 0)) {
		vr_reset_hud_layouts();
	}
	menu.y -= 4;
	if (XwaModernOptionsMenu_DrawAction(&menu, "Reset Lap Console Position", 91, 0)) {
		SDL_setenv_unsafe("XWA_XR_CONSOLE_POSE", "", 1);
		XwaModernVrOptions_SaveValue("XWA_XR_CONSOLE_POSE", "");
		Aeron_LogInfo("xwa.config", "lap console back at its default spot");
	}
	menu.y -= 4;
	if (XwaModernOptionsMenu_DrawAction(&menu, "Reset Flight Stick Position", 95, 0)) {
		SDL_setenv_unsafe("XWA_XR_STICK_POS", "", 1);
		XwaModernVrOptions_SaveValue("XWA_XR_STICK_POS", "");
		Aeron_LogInfo("xwa.config", "flight stick back at its default spot");
	}
	menu.y -= 4;
	if (XwaModernOptionsMenu_DrawAction(&menu, "Performance / GPU...", 71, 0)) {
		page = 1;
		*cursor_row = 0;
		return 0;
	}
	menu.y -= 4;
	back = XwaModernOptionsMenu_DrawAction(&menu, FrontendString_Get(STR_BACK), 87, 0);
	back |= XwaModernOptionsMenu_TakeEscape(&menu);
	if (!back) {
		return 0;
	}
	vr_persist();
	return 1;
}

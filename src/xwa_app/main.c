#include "aeron/main.h"
#include "aeron/aeron.h"
#include "crash_report.h"
#include "host_config.h"
#include "setup.h"
#include "setup_screen.h"
#include "vr_keyboard.h"
#include "vr_voice.h"
#include "window_icon.h"
#include "xwa/config/game_config.h"
#include "xwa_remaster/vr_cockpit_hud.h"
#include "xwa_remaster/vr_lap_console.h"
#include "xwa_remaster/vr_flight_stick.h"
#include "xwa_runtime/config/modern_vr_options_screen.h"
#include "xwa_remaster/xwa_remaster.h"
#include "xwa_runtime/config/modern_video_options.h"
#include "xwa_runtime/input/controller_mapping.h"
#include "xwa_runtime/input/mouse_flight.h"
#include "xwa_runtime/runtime/port.h"
#include "xwa_runtime/runtime/presentation.h"
#include "xwa_runtime/timing/modern_flight_timing.h"

#include <SDL3/SDL_hints.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* OS message boxes are invisible inside an immersive VR app, so the Quest
 * build (and desktop runs with XWA_INGAME_SETUP=1, for testing) shows setup
 * problems and fatal errors on an in-game screen instead. */
static int use_ingame_setup(void) {
#if defined(SDL_PLATFORM_ANDROID)
	return 1;
#else
	const char* value = getenv("XWA_INGAME_SETUP");
	return value && value[0] == '1';
#endif
}

static void ingame_fatal_error(const char* title, const char* message) {
	/* Once a fatal error was requested the frame loop is shutting down and
	 * cannot draw anymore; the log keeps the message. */
	if (!Aeron_FatalErrorRequested()) {
		(void)XwaSetupScreen_Run(title, message, NULL);
	}
}

enum {
	XWA_SETUP_CHOOSE_FOLDER = 1,
	XWA_SETUP_TRY_AGAIN = 2,
	XWA_SETUP_QUIT = 3,
};

static uint64_t presentation_interval_us(void) {
	double rate = Aeron_PresentationRate();
	if (!(rate >= 1.0 && rate <= 1000.0))
		rate = 60.0;
	return (uint64_t)(1000000.0 / rate + 0.5);
}

static void apply_modern_video_options(const XwaModernVideoOptions* options) {
	if (!options) {
		return;
	}
	Aeron_SetFullscreen(options->window_mode == XWA_MODERN_WINDOW_MODE_FULLSCREEN);
	XwaRemaster_SetVideoOptions(options);
}

static int persist_modern_video_options(const XwaModernVideoOptions* options, char* error,
										size_t error_size) {
	return XwaHostConfig_SaveVideoOptions(Aeron_GetVfs(), options, error, error_size);
}

/* Platform-conventional fullscreen toggle chord: Cmd+Ctrl+F on macOS,
 * Alt+Enter on Windows, Alt+Enter or F11 elsewhere. */
static int fullscreen_hotkey_pressed(const AeronInputSnapshot* in) {
	if (!in) {
		return 0;
	}
#if defined(__APPLE__)
	return (in->key_down[AERON_KEY_LGUI] || in->key_down[AERON_KEY_RGUI]) &&
		   (in->key_down[AERON_KEY_LCTRL] || in->key_down[AERON_KEY_RCTRL]) &&
		   in->key_pressed[AERON_KEY_A + ('f' - 'a')];
#else
	const int alt = in->key_down[AERON_KEY_LALT] || in->key_down[AERON_KEY_RALT];
	const int enter = in->key_pressed[AERON_KEY_RETURN] || in->key_pressed[AERON_KEY_KP_ENTER];
#if defined(_WIN32)
	return alt && enter;
#else
	return (alt && enter) || in->key_pressed[AERON_KEY_F11];
#endif
#endif
}

/* Window-mode changes route through the video options module so the apply
 * callback, the options screen, and config persistence stay in agreement. */
static void set_window_mode_option(XwaModernWindowMode mode) {
	XwaModernVideoOptions options;
	XwaModernVideoOptions_Get(&options);
	if (options.window_mode != mode) {
		options.window_mode = mode;
		XwaModernVideoOptions_Set(&options);
	}
}

static void apply_modern_input_options(const XwaModernInputOptions* options) {
	XwaMouseFlight_SetOptions(options);
	if (options) {
		XwaControllerMapping_SetOptions(&options->controller);
		Config_ApplyModernInputOptions(options);
	}
}

static int persist_modern_input_options(const XwaModernInputOptions* options, char* error,
										size_t error_size) {
	return XwaHostConfig_SaveInputOptions(Aeron_GetVfs(), options, error, error_size);
}

static int show_first_launch_prompt(int* cancelled, char* error, size_t error_size) {
	static const AeronMessageBoxButton buttons[] = {
		{ XWA_SETUP_CHOOSE_FOLDER, "Choose Game Folder", 1, 0 },
		{ XWA_SETUP_QUIT, "Quit", 0, 1 },
	};
	const AeronMessageBoxOptions options = {
		.kind = AERON_MESSAGE_BOX_INFORMATION,
		.title = "Set up OpenXWA",
		.message = "OpenXWA requires data files from an original X-Wing Alliance installation.\n\n"
				   "Select an installed game folder (for example a GOG or Steam install) containing:\n\n"
				   "    FLIGHTMODELS\n"
				   "    FRONTRES\n"
				   "    MISSIONS\n"
				   "    MOVIES\n"
				   "    RESDATA\n"
				   "    WAVE\n\n"
				   "The selected location will be validated and remembered.",
		.buttons = buttons,
		.button_count = sizeof buttons / sizeof buttons[0],
	};
	int selected_button = -1;

	if (!Aeron_ShowMessageBox(&options, &selected_button)) {
		snprintf(error, error_size, "Could not show the first-launch setup prompt.");
		return 0;
	}
	if (selected_button != XWA_SETUP_CHOOSE_FOLDER) {
		*cancelled = 1;
		return 0;
	}
	return 1;
}

static int select_game_data(AeronVfs* vfs, char* selected, size_t selected_capacity, int* cancelled,
							char* error, size_t error_size) {
	const AeronFolderDialogOptions options = {
		.title = "Select your original X-Wing Alliance game directory",
		.accept_label = "Choose folder",
		.cancel_label = "Quit",
	};

	*cancelled = 0;
	for (;;) {
		AeronFolderDialog* dialog = Aeron_ShowOpenFolderDialog(&options);
		AeronFolderDialogStatus status;
		char candidate[XWA_HOST_CONFIG_PATH_CAPACITY];
		char dialog_error[256];
		char validation_error[512];

		if (!dialog) {
			snprintf(error, error_size, "Could not open the folder picker.");
			return 0;
		}
		do {
			Aeron_BeginFrame();
			status = Aeron_PollFolderDialog(dialog, candidate, sizeof candidate, dialog_error,
											sizeof dialog_error);
			if (!Aeron_Present()) {
				snprintf(error, error_size, "Renderer failure while displaying the folder picker: %s",
						 Aeron_RenderLastError());
				Aeron_DestroyFolderDialog(dialog);
				return 0;
			}
			if (status == AERON_FOLDER_DIALOG_WAITING) {
				Aeron_WaitForNextFrame(16667);
			}
		} while (status == AERON_FOLDER_DIALOG_WAITING);
		Aeron_DestroyFolderDialog(dialog);

		if (Aeron_QuitRequested()) {
			*cancelled = 1;
			return 0;
		}
		if (status == AERON_FOLDER_DIALOG_CANCELLED) {
			*cancelled = 1;
			return 0;
		}
		if (status == AERON_FOLDER_DIALOG_ERROR) {
			snprintf(error, error_size, "Folder picker failed: %s",
					 dialog_error[0] ? dialog_error : "unknown platform error");
			return 0;
		}
		if (XwaSetup_ValidateGameData(vfs, candidate, selected, selected_capacity, validation_error,
									  sizeof validation_error)) {
			return 1;
		}
		{
			static const AeronMessageBoxButton buttons[] = {
				{ XWA_SETUP_CHOOSE_FOLDER, "Choose Another Folder", 1, 0 },
				{ XWA_SETUP_QUIT, "Quit", 0, 1 },
			};
			char message[1536];
			int selected_button = -1;
			const AeronMessageBoxOptions message_options = {
				.kind = AERON_MESSAGE_BOX_WARNING,
				.title = "Invalid X-Wing Alliance game directory",
				.message = message,
				.buttons = buttons,
				.button_count = sizeof buttons / sizeof buttons[0],
			};

			snprintf(message, sizeof message,
					 "The selected folder is not a complete X-Wing Alliance data directory.\n\n"
					 "Selected:\n%s\n\n"
					 "%s\n\n"
					 "Select an installed game folder containing FLIGHTMODELS, MISSIONS, and RESDATA.TXT.",
					 candidate, validation_error);
			if (!Aeron_ShowMessageBox(&message_options, &selected_button)) {
				snprintf(error, error_size, "Could not show the game-data validation result.");
				return 0;
			}
			if (selected_button != XWA_SETUP_CHOOSE_FOLDER) {
				*cancelled = 1;
				return 0;
			}
		}
	}
}

static int resolve_game_data(const XwaLaunchOptions* launch, const XwaHostConfig* host_config, char* selected,
							 size_t selected_capacity, int* cancelled, char* error, size_t error_size) {
	AeronVfs* vfs = Aeron_GetVfs();

	*cancelled = 0;
	if (launch->game_data_path[0]) {
		const int ok = XwaSetup_ValidateGameData(vfs, launch->game_data_path, selected, selected_capacity, error,
												 error_size);
		if (!ok && use_ingame_setup()) {
			char detail[512];
			snprintf(detail, sizeof detail, "%s", error);
			snprintf(error, error_size,
					 "The original X-Wing Alliance game files were not found (%s).\n\n"
					 "Copy the contents of your installed game folder (GOG or Steam) to:\n%s\n\n"
					 "From a PC with the headset connected by USB:\n"
					 "adb push \"<XWA folder>/.\" %s\n\n"
					 "Then choose Try Again.",
					 detail, launch->game_data_path, launch->game_data_path);
		}
		return ok;
	}
	if (host_config->game_data_path[0]) {
		if (XwaSetup_ValidateGameData(vfs, host_config->game_data_path, selected, selected_capacity, error,
									  error_size)) {
			return 1;
		}
		Aeron_LogWarn("xwa.config", "saved game-data path is no longer valid: %s", error);
		for (;;) {
			static const AeronMessageBoxButton buttons[] = {
				{ XWA_SETUP_TRY_AGAIN, "Try Again", 1, 0 },
				{ XWA_SETUP_CHOOSE_FOLDER, "Choose Another Folder", 0, 0 },
				{ XWA_SETUP_QUIT, "Quit", 0, 1 },
			};
			char message[1536];
			int selected_button = -1;
			const AeronMessageBoxOptions options = {
				.kind = AERON_MESSAGE_BOX_WARNING,
				.title = "Original X-Wing Alliance game data not found",
				.message = message,
				.buttons = buttons,
				.button_count = sizeof buttons / sizeof buttons[0],
			};

			snprintf(message, sizeof message,
					 "The previously configured game-data folder is no longer available or "
					 "does not contain the required files.\n\n"
					 "Configured folder:\n%s\n\n"
					 "%s\n\n"
					 "The folder may have moved, or its drive may be disconnected.",
					 host_config->game_data_path, error);
			if (!Aeron_ShowMessageBox(&options, &selected_button)) {
				snprintf(error, error_size, "Could not show the saved game-data error.");
				return 0;
			}
			if (selected_button == XWA_SETUP_TRY_AGAIN) {
				if (XwaSetup_ValidateGameData(vfs, host_config->game_data_path, selected, selected_capacity,
											  error, error_size)) {
					return 1;
				}
				continue;
			}
			if (selected_button == XWA_SETUP_CHOOSE_FOLDER) {
				return select_game_data(vfs, selected, selected_capacity, cancelled, error, error_size);
			}
			*cancelled = 1;
			return 0;
		}
	}
	if (!show_first_launch_prompt(cancelled, error, error_size)) {
		return 0;
	}
	return select_game_data(vfs, selected, selected_capacity, cancelled, error, error_size);
}

#if defined(SDL_PLATFORM_ANDROID)
#include <SDL3/SDL_system.h>
#include <sys/stat.h>

/* Quest has no folder picker inside an immersive app. The game data lives in
 * the app's external files directory, which needs no storage permission and
 * is reachable with `adb push` or a PC file manager:
 *   /sdcard/Android/data/<package>/files/XWA
 * The folder is created on first launch so it shows up for copying. */
/* Quest settings file: KEY=VALUE lines applied as environment variables
 * before startup (the headset has no way to set them otherwise). */
static void android_load_settings(void) {
	const char* external = SDL_GetAndroidExternalStoragePath();
	char path[1024];
	char line[2048]; /* HUD layouts are long lines */
	FILE* f;
	if (!external || !external[0]) {
		return;
	}
	if (!SDL_getenv("AERON_LOG_FILE")) {
		snprintf(path, sizeof path, "%s/openxwa.log", external);
		SDL_setenv_unsafe("AERON_LOG_FILE", path, 1);
	}
	/* Native crashes: fault + backtrace, kept until the next crash. */
	snprintf(path, sizeof path, "%s/openxwa-crash.txt", external);
	XwaCrashReport_Install(path);
	snprintf(path, sizeof path, "%s/openxwa-quest.cfg", external);
	f = fopen(path, "r");
	if (!f) {
		f = fopen(path, "w");
		if (f) {
			fputs("# OpenXWA Quest settings. Remove the leading '#' to change a value.\n"
				  "# Per-eye render resolution (lower = faster):\n"
				  "#AERON_XR_RESOLUTION_SCALE=0.9\n"
				  "# Size cap of the three HUD images, as a fraction of the recommended eye\n"
				  "# size (a higher eye resolution then costs no extra HUD work):\n"
				  "#AERON_XR_HUD_RESOLUTION=0.9\n"
				  "# Display refresh rate in Hz (0 = the headset's setting; 72 gives the\n"
				  "# most time per frame):\n"
				  "#AERON_XR_REFRESH_RATE=0\n"
				  "# Fixed foveated rendering of the 3D view: 0 (off), low, medium, high.\n"
				  "# The edges of the view are shaded in coarser blocks; HUD, panels and text\n"
				  "# are not affected (restart to change):\n"
				  "#XWA_XR_FOVEATION=low\n"
				  "# GPU/CPU clock floor requested from the headset: 0, low, high, boost:\n"
				  "#AERON_XR_PERF_LEVEL=high\n"
				  "# 0 = turn off the GPU optimizations (A/B test; also in the VR options):\n"
				  "#XWA_XR_GPU_OPT=1\n"
				  "# 0 = classic draw order (sky first) instead of near meshes first:\n"
				  "#XWA_XR_DRAW_ORDER=1\n"
				  "# 1 = dynamic lights from lasers/explosions (costs GPU time in combat):\n"
				  "#XWA_POINT_LIGHTS=0\n"
				  "# Directional shadows in the 3D cockpit (1 = on, costs GPU time):\n"
				  "#XWA_XR_SHADOWS=0\n"
				  "# 0 = render shadows again for the right eye (default: shared):\n"
				  "#XWA_XR_SHADOW_REUSE=1\n"
				  "# 0 = keep the game running while the headset is off / menu open:\n"
				  "#XWA_XR_AUTO_PAUSE=1\n"
				  "# 0 = everything on the flat virtual screen (no 3D cockpit):\n"
				  "#XWA_XR_STEREO=1\n"
				  "# Lower = cockpit and ships feel bigger (try 0.8):\n"
				  "#XWA_XR_WORLD_SCALE=1.0\n"
				  "# Stereo depth of the world outside the cockpit (1 = real, try 3-6;\n"
				  "# higher shows depth at combat range but makes ships look like models):\n"
				  "#XWA_XR_DEPTH_BOOST=1.0\n"
				  "# HUD: cockpit (panels on the cockpit), helmet (follows your head), fixed:\n"
				  "#XWA_XR_HUD_LAYOUT=cockpit\n"
				  "# Star dot size (1 = flat-screen stars magnified; default 0.33):\n"
				  "#XWA_XR_STAR_SIZE=0.33\n"
				  "# Lap console (touch panel below the view; look down to use it):\n"
				  "#XWA_XR_CONSOLE=1\n"
				  "#XWA_XR_CONSOLE_PITCH=-52\n"
				  "#XWA_XR_CONSOLE_DISTANCE=0.52\n"
				  "# Where you put it with the grip handles (Reset in the VR options):\n"
				  "#XWA_XR_CONSOLE_POSE=\n"
				  "# Controls reference card on your right at mission start (0 = off):\n"
				  "#XWA_XR_BUTTON_CARD=1\n"
				  "# HUD panels you move with the grip are kept per cockpit as\n"
				  "# XWA_XR_HUD_POSE_<COCKPIT>=... (Reset in the VR options).\n"
				  "# 1 = screen-space ambient occlusion in VR (darkens around the hands):\n"
				  "#XWA_XR_SSAO=0\n"
				  "# Pilot gloves on the controllers (0 = off); move them in the\n"
				  "# controller's frame, metres x,y,z (+x right, +y up, +z back):\n"
				  "#XWA_XR_GLOVES=1\n"
				  "#XWA_XR_GLOVE_OFFSET=0,0,0\n"
				  "# Glove size (1 = the model's own large hand):\n"
				  "#XWA_XR_GLOVE_SCALE=0.85\n"
				  "# Menus, briefings and films float in the carrier hangar (0 = off).\n"
				  "# View: x,y (hangar units, ~41 per metre), turn in degrees, eye\n"
				  "# height in metres; parked craft OPT name (0 = none):\n"
				  "#XWA_XR_ENVIRONMENT=1\n"
				  "#XWA_XR_ENV_VIEW=-500,-1150,0,1.7\n"
				  "#XWA_XR_ENV_CRAFT=Xwing\n"
				  "# Hangar brightness behind the screen (1 = full, 0.4 = 60 % darker):\n"
				  "#XWA_XR_ENV_BRIGHTNESS=0.4\n"
				  "# Menu laser pointer opacity (1 = solid):\n"
				  "#XWA_XR_BEAM_OPACITY=0.5\n"
				  "# Flight stick between your knees (0 = off): squeeze the right grip on\n"
				  "# it to fly, the left grip on it to move it. Full tilt in degrees;\n"
				  "# 1 = twisting it rolls; where you put it (Reset in the VR options):\n"
				  "#XWA_XR_FLIGHT_STICK=1\n"
				  "#XWA_XR_STICK_ANGLE=20\n"
				  "#XWA_XR_STICK_TWIST=0\n"
				  "#XWA_XR_STICK_POS=\n"
				  "# Left thumbstick: degrees either side of straight forward/back that\n"
				  "# move the throttle; everywhere else rolls:\n"
				  "#AERON_XR_THROTTLE_ZONE=30\n"
				  "# 1 = right thumbstick back pushes the nose down (inverted pitch):\n"
				  "#AERON_XR_INVERT_PITCH=0\n"
				  "# Voice commands: double-tap a thumb rest, then speak (doubletap), or\n"
				  "# speak while the thumb is on the rest (hold); 0 = off:\n"
				  "#XWA_XR_VOICE=doubletap\n"
				  "# Voice command list on your left at mission start (0 = off):\n"
				  "#XWA_XR_VOICE_CARD=1\n"
				  "# Helmet HUD distance in metres and size:\n"
				  "#XWA_XR_HUD_DISTANCE=3.0\n"
				  "#XWA_XR_HUD_SCALE=0.65\n"
				  "#AERON_XR_SCREEN_DISTANCE=1.6\n"
				  "#AERON_XR_SCREEN_WIDTH=2.0\n"
				  "#AERON_XR_THROTTLE_RATE=0.8\n"
				  "# Skip the Quest render defaults (SSAO/MSAA/... follow the in-game options):\n"
				  "#XWA_QUEST_PROFILE=0\n",
				  f);
			fclose(f);
		}
		return;
	}
	while (fgets(line, sizeof line, f)) {
		char* eq;
		char* end;
		char* key = line;
		while (*key == ' ' || *key == '\t') {
			key++;
		}
		if (*key == '#' || *key == '\0' || *key == '\n' || *key == '\r') {
			continue;
		}
		eq = strchr(key, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		end = eq + 1 + strcspn(eq + 1, "\r\n");
		*end = '\0';
		SDL_setenv_unsafe(key, eq + 1, 1);
	}
	fclose(f);
}

/* Foveation shim (packaging/quest/vkshim): its log, its state, and the
 * marker file that is present while a start with it has not proven itself. */
static char g_shim_guard[1024];
static int (*g_shim_take_log)(char* out, int size);
static int (*g_shim_foveating)(void);

static void shim_frame(void) {
	static int frames;
	if (g_shim_take_log) {
		char text[4096];
		if (g_shim_take_log(text, (int)sizeof text) > 0) {
			for (char* line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
				if (line[0] == '!') {
					Aeron_LogWarn("vkshim", "%s", line + 1);
				} else {
					Aeron_LogInfo("vkshim", "%s", line + 1);
				}
			}
		}
	}
	/* Proven: foveating for a while, or running for a minute. */
	if (g_shim_guard[0] && ((g_shim_foveating && g_shim_foveating() && ++frames > 300) || frames++ > 4000)) {
		remove(g_shim_guard);
		g_shim_guard[0] = '\0';
	}
}

/* Read before Aeron_Init: the XR swapchains are sized at session start. */
static void android_xr_defaults(void) {
	const char* profile = SDL_getenv("XWA_QUEST_PROFILE");
	if (profile && profile[0] == '0') {
		return;
	}
	if (!SDL_getenv("AERON_XR_RESOLUTION_SCALE")) {
		SDL_setenv_unsafe("AERON_XR_RESOLUTION_SCALE", "0.9", 1);
	}
	{
		/* Fixed foveated rendering: SDL loads Vulkan through the OpenXWA shim
		 * (packaging/quest/vkshim), which adds a fragment density map to the
		 * 3D scene's pass. Only when the shim is there to load. */
		const char* fov = SDL_getenv("XWA_XR_FOVEATION");
		const char* external = SDL_GetAndroidExternalStoragePath();
		FILE* guard = NULL;
		if (external && external[0]) {
			snprintf(g_shim_guard, sizeof g_shim_guard, "%s/openxwa-foveation-start", external);
			guard = fopen(g_shim_guard, "r");
		}
		if (guard) {
			/* The last start with the shim never got going: run without it and
			 * leave it off (the VR options turn it back on). */
			fclose(guard);
			remove(g_shim_guard);
			g_shim_guard[0] = '\0';
			SDL_setenv_unsafe("XWA_XR_FOVEATION", "0", 1);
			XwaModernVrOptions_SaveValue("XWA_XR_FOVEATION", "0");
			Aeron_LogWarn("xwa.config", "foveation: the last start with it failed, so it is now off");
		} else if (!(fov && (fov[0] == '0' || SDL_strcasecmp(fov, "off") == 0))) {
			void* shim = dlopen("libopenxwa_vkshim.so", RTLD_NOW | RTLD_LOCAL);
			if (shim && dlsym(shim, "vkGetInstanceProcAddr")) {
				SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, "libopenxwa_vkshim.so");
				g_shim_take_log = (int (*)(char*, int))dlsym(shim, "OpenXWA_VkShim_TakeLog");
				g_shim_foveating = (int (*)(void))dlsym(shim, "OpenXWA_VkShim_Foveating");
				if (g_shim_guard[0] && (guard = fopen(g_shim_guard, "w")) != NULL) {
					fclose(guard);
				}
				Aeron_LogInfo("xwa.config", "Vulkan through the foveation shim (XWA_XR_FOVEATION=%s)",
							  fov && fov[0] ? fov : "low");
			} else {
				g_shim_guard[0] = '\0';
				Aeron_LogWarn("xwa.config", "foveation shim not found: %s", dlerror());
			}
		} else {
			g_shim_guard[0] = '\0';
		}
	}
	{
		/* GPU Optimizations off (VR options A/B switch): no clock floor either. */
		const char* opt = SDL_getenv("XWA_XR_GPU_OPT");
		if (opt && opt[0] == '0' && !SDL_getenv("AERON_XR_PERF_LEVEL")) {
			SDL_setenv_unsafe("AERON_XR_PERF_LEVEL", "0", 1);
		}
	}
}

static void android_default_game_data(XwaLaunchOptions* launch) {
	const char* external = SDL_GetAndroidExternalStoragePath();
	char path[XWA_HOST_CONFIG_PATH_CAPACITY];
	if (launch->game_data_path[0] || !external || !external[0]) {
		return;
	}
	snprintf(path, sizeof path, "%s/XWA", external);
	mkdir(path, 0775);
	snprintf(launch->game_data_path, sizeof launch->game_data_path, "%s", path);
	Aeron_LogInfo("xwa.config", "Android game-data folder: %s", path);
}
#endif

/* Mobile/standalone render defaults. Applied to options the player has not
 * set explicitly, so the in-game Video Options still win. On by default on
 * Android; XWA_QUEST_PROFILE=1 previews it on PC VR. */
static void apply_quest_render_profile(XwaHostConfig* host) {
	const char* env = SDL_getenv("XWA_QUEST_PROFILE");
#if defined(SDL_PLATFORM_ANDROID)
	const int enabled = !(env && env[0] == '0');
#else
	const int enabled = env && env[0] == '1';
#endif
	unsigned int* mask = &host->video_options_override_mask;
	if (!enabled) {
		return;
	}
	if (!(*mask & XWA_MODERN_VIDEO_OVERRIDE_SSAO)) {
		host->video_options.ssao_quality = XWA_MODERN_SSAO_OFF;
		*mask |= XWA_MODERN_VIDEO_OVERRIDE_SSAO;
	}
	if (!(*mask & XWA_MODERN_VIDEO_OVERRIDE_FSR)) {
		host->video_options.fsr_upscaling = XWA_MODERN_FSR_OFF;
		*mask |= XWA_MODERN_VIDEO_OVERRIDE_FSR;
	}
	if (!(*mask & XWA_MODERN_VIDEO_OVERRIDE_MOTION_BLUR)) {
		host->video_options.motion_blur_quality = XWA_MODERN_MOTION_BLUR_OFF;
		*mask |= XWA_MODERN_VIDEO_OVERRIDE_MOTION_BLUR;
	}
	if (!(*mask & XWA_MODERN_VIDEO_OVERRIDE_MSAA)) {
		host->video_options.msaa = XWA_MODERN_MSAA_4X; /* cheap on tile-based mobile GPUs */
		*mask |= XWA_MODERN_VIDEO_OVERRIDE_MSAA;
	}
	if (!(*mask & XWA_MODERN_VIDEO_OVERRIDE_HDR)) {
		host->video_options.hdr_output = 0;
		*mask |= XWA_MODERN_VIDEO_OVERRIDE_HDR;
	}
	if (!SDL_getenv("XWA_XR_SHADOWS")) {
		SDL_setenv_unsafe("XWA_XR_SHADOWS", "0", 1);
	}
	/* Laser and explosion point lights are what saturates the Quest GPU in
	 * dogfights (each one is shaded per pixel, per eye). */
	if (!SDL_getenv("XWA_POINT_LIGHTS")) {
		SDL_setenv_unsafe("XWA_POINT_LIGHTS", "0", 1);
	}
	Aeron_LogInfo("xwa.config", "Quest render profile: SSAO off, FSR off, motion blur off, MSAA %s, HDR off, "
							   "stereo shadows %s, point lights %s",
				  host->video_options.msaa == XWA_MODERN_MSAA_4X ? "4x" : "user", SDL_getenv("XWA_XR_SHADOWS"),
				  SDL_getenv("XWA_POINT_LIGHTS"));
}

int main(int argc, char** argv) {
	AeronConfig config;
	XwaHostConfig host_config;
	XwaLaunchOptions launch;
	char selected_game_data[XWA_HOST_CONFIG_PATH_CAPACITY];
	char config_error[1024];
	XwaModernWindowMode initial_window_mode;
	int exit_code;
	int setup_cancelled;

	if (!XwaLaunchOptions_Parse(argc, argv, &launch, config_error, sizeof config_error)) {
		Aeron_LogError("xwa.config", "%s", config_error);
		return 2;
	}
#if defined(SDL_PLATFORM_ANDROID)
	android_load_settings();
	android_xr_defaults();
#endif
	memset(&config, 0, sizeof(config));
	config.org_name = "TotallyOpen";
	config.app_name = "OpenXWA";
	config.resource_root = launch.resource_root[0] ? launch.resource_root : NULL;
	config.resource_path = "resources";
	config.shader_path = "shaders";
	config.window_title = "OpenXWA";
	config.window_icon_bmp = xwa_window_icon_bmp;
	config.window_icon_bmp_size = sizeof(xwa_window_icon_bmp);
	/* Windowed size auto-fits the primary display. The logical frame starts
	 * at the 16:9 default and tracks the window aspect once the main loop
	 * runs (XwaPresentation_SyncToWindow). */
	config.logical_width = XWA_PRESENTATION_WIDTH;
	config.logical_height = XWA_PRESENTATION_HEIGHT;
	config.presentation_mode = AERON_PRESENTATION_ASPECT_FIT;
	config.clear_color_enabled = 1;
	config.clear_color_rgba[0] = 0.0f;
	config.clear_color_rgba[1] = 0.0f;
	config.clear_color_rgba[2] = 0.0f;
	config.clear_color_rgba[3] = 1.0f;
	config.xr_enabled = launch.xr_enabled;
#if defined(SDL_PLATFORM_ANDROID)
	/* Meta Quest standalone: always an OpenXR app. */
	config.xr_enabled = 1;
#endif

	if (!Aeron_Init(&config)) {
		return 1;
	}
	if (use_ingame_setup()) {
		Aeron_SetFatalErrorHandler(ingame_fatal_error);
	}
#if defined(SDL_PLATFORM_ANDROID)
	android_default_game_data(&launch);
#endif
	if (!XwaHostConfig_Load(Aeron_GetVfs(), &host_config, config_error, sizeof config_error)) {
		char message[1536];
		snprintf(message, sizeof message, "%s\nConfiguration path: %s/config.yaml", config_error,
				 Aeron_UserPath());
		Aeron_LogError("xwa.config", "%s", message);
		Aeron_FatalError("OpenXWA", message);
		Aeron_Shutdown();
		return 1;
	}
	apply_quest_render_profile(&host_config);
	initial_window_mode = XWA_MODERN_WINDOW_MODE_FULLSCREEN;
	if (host_config.video_options_override_mask & XWA_MODERN_VIDEO_OVERRIDE_WINDOW_MODE) {
		initial_window_mode = host_config.video_options.window_mode;
	}
	while (!resolve_game_data(&launch, &host_config, selected_game_data, sizeof selected_game_data,
							  &setup_cancelled, config_error, sizeof config_error)) {
		if (!setup_cancelled && use_ingame_setup() && launch.game_data_path[0]) {
			Aeron_LogWarn("xwa.config", "%s", config_error);
			const int choice = XwaSetupScreen_Run("X-Wing Alliance game files needed", config_error, "Try Again");
			if (choice == XWA_SETUP_SCREEN_RETRY) {
				continue;
			}
			/* FAILED falls through to the fatal-error path (logged, exit 1). */
			setup_cancelled = choice == XWA_SETUP_SCREEN_QUIT;
		}
		if (!setup_cancelled) {
			Aeron_LogError("xwa.config", "%s", config_error);
			Aeron_FatalError("OpenXWA", config_error);
		}
		Aeron_Shutdown();
		return setup_cancelled ? 0 : 1;
	}
	if (strcmp(host_config.game_data_path, selected_game_data) != 0 &&
		!XwaHostConfig_SaveGameDataPath(Aeron_GetVfs(), selected_game_data, config_error,
										sizeof config_error)) {
		Aeron_LogError("xwa.config", "%s", config_error);
		Aeron_FatalError("OpenXWA", config_error);
		Aeron_Shutdown();
		return 1;
	}
	Aeron_SetFullscreen(initial_window_mode == XWA_MODERN_WINDOW_MODE_FULLSCREEN);
	/* The setup dialogs above are separate OS windows; on some platforms
	 * (macOS) closing them does not re-key the game window, which would start
	 * the game inside its unfocused pause. Ask for input focus back. */
	Aeron_RaiseWindow();
	Aeron_LogInfo("xwa.config", "game data: %s", selected_game_data);
	Aeron_LogInfo("xwa.config", "resources: %s", Aeron_ResourceRoot());
	Aeron_LogInfo("xwa.config", "flight simulation step: %d ticks", host_config.flight_simulation_step_ticks);
	Aeron_LogInfo("xwa.config", "OPT smoothing angle: %.3g degrees",
				  (double)host_config.model_smooth_angle_degrees);
	Aeron_LogInfo("xwa.config", "OPT emissive strength: %.3g",
				  (double)host_config.model_opt_emissive_strength);
	Aeron_LogInfo("xwa.config", "OPT projectile emissive strength: %.3g",
				  (double)host_config.model_opt_projectile_emissive_strength);
	Aeron_LogInfo("xwa.config", "engine emissive strength: %.3g",
				  (double)host_config.model_engine_emissive_strength);
	Aeron_LogInfo("xwa.config", "force OPT models: %s", host_config.force_opt_models ? "yes" : "no");
	Aeron_LogInfo("xwa.config", "prefer original 2D assets: %s",
				  host_config.prefer_original_2d ? "yes" : "no");
	const XwaRemasterInitOptions remaster_options = {
		.opt_smooth_angle_degrees = host_config.model_smooth_angle_degrees,
		.opt_emissive_strength = host_config.model_opt_emissive_strength,
		.opt_projectile_emissive_strength = host_config.model_opt_projectile_emissive_strength,
		.engine_emissive_strength = host_config.model_engine_emissive_strength,
		.force_opt_models = host_config.force_opt_models,
		.prefer_original_2d = host_config.prefer_original_2d,
		.video_options = host_config.video_options,
		.video_options_override_mask = host_config.video_options_override_mask,
	};
	if (!XwaRemaster_Init(&remaster_options)) {
		Aeron_FatalError("OpenXWA",
						 "Required remaster resources could not be loaded. See the log for details.");
		XwaRemaster_Shutdown();
		Aeron_Shutdown();
		return 1;
	}
	{
		XwaModernVideoOptions effective_video_options;
		XwaRemaster_GetVideoOptions(&effective_video_options);
		effective_video_options.window_mode = initial_window_mode;
		XwaModernVideoOptions_Configure(&effective_video_options, apply_modern_video_options,
										persist_modern_video_options);
	}
	XwaModernInputOptions_Configure(&host_config.input_defaults, &host_config.input_options,
									apply_modern_input_options, persist_modern_input_options);
	apply_modern_input_options(&host_config.input_options);
	Aeron_LogInfo(
		"xwa.config", "mouse flight control: %s (%s mode, sensitivity %d, invert Y: %s)",
		host_config.input_options.mouse_flight_enabled ? "on" : "off",
		host_config.input_options.mouse_mode == XWA_MODERN_MOUSE_MODE_POSITION ? "position" : "rate",
		host_config.input_options.mouse_sensitivity, host_config.input_options.mouse_invert_y ? "yes" : "no");

	XwaPort_SetCommandLine(launch.game_command_line);
	XwaModernFlightTiming_Configure(host_config.flight_simulation_step_ticks);
	if (!XwaPort_Init()) {
		XwaRemaster_Shutdown();
		Aeron_Shutdown();
		return 1;
	}

	exit_code = 0;
	/* Cmd+P host pause (the TIE shell model): the game tick is skipped
	 * — sim and snapshots freeze — while the HD remaster keeps
	 * rendering the frozen snapshot every frame (debug UI and
	 * motion-blur inspection stay live). The toggle frame also
	 * skips the tick, so the P press edge never reaches the classic
	 * input path (edges pump inside the tick). */
	{
		int paused = 0;
		int last_fullscreen = Aeron_Fullscreen();
		/* VR: pause while the headset is off or the system menu is open
		 * (XWA_XR_AUTO_PAUSE=0 keeps the game running). */
		const char* xr_auto_pause_env = getenv("XWA_XR_AUTO_PAUSE");
		const int xr_auto_pause = !(xr_auto_pause_env && xr_auto_pause_env[0] == '0');
		int xr_paused = 0;
		while (!Aeron_QuitRequested() && !XwaPort_ShouldQuit()) {
			const int32_t delta_us = Aeron_BeginFrame();
#if defined(SDL_PLATFORM_ANDROID)
			shim_frame();
#endif
			/* Before the game reads input: the VR keyboard consumes its clicks. */
			XwaVrKeyboard_BeginFrame(delta_us);
			const int xr_unfocused = xr_auto_pause && Aeron_XrIsUnfocused();
			if (xr_unfocused != xr_paused) {
				xr_paused = xr_unfocused;
				Aeron_AudioSetPaused(paused || xr_paused);
				Aeron_LogInfo("xwa", "%s", xr_paused ? "paused (headset not focused)" : "resumed (headset focused)");
			}
			const AeronInputSnapshot* in = Aeron_InputSnapshot();
			int toggled = 0;
			if (in) {
				XwaPresentation_SyncToWindow(in->window_width, in->window_height);
			}
			/* Reconcile OS-initiated fullscreen transitions (e.g. the macOS
			 * green button) into the stored option. Edge-triggered on the
			 * observed state so an in-flight transition is never fought. */
			const int fullscreen = Aeron_Fullscreen();
			if (fullscreen != last_fullscreen) {
				last_fullscreen = fullscreen;
				set_window_mode_option(fullscreen ? XWA_MODERN_WINDOW_MODE_FULLSCREEN
												  : XWA_MODERN_WINDOW_MODE_WINDOWED);
			}
			/* The toggle frame skips the game tick like the other host
			 * chords so the Enter/F/F11 edge never reaches classic input. */
			if (fullscreen_hotkey_pressed(in)) {
				set_window_mode_option(fullscreen ? XWA_MODERN_WINDOW_MODE_WINDOWED
												  : XWA_MODERN_WINDOW_MODE_FULLSCREEN);
				toggled = 1;
			}
			const int gui_mod = in && (in->key_down[AERON_KEY_LGUI] || in->key_down[AERON_KEY_RGUI]);
			if (gui_mod && in->key_pressed[AERON_KEY_A + ('p' - 'a')]) {
				paused = !paused;
				toggled = 1;
				Aeron_AudioSetPaused(paused || xr_paused);
				Aeron_LogInfo("xwa", "%s", paused ? "paused" : "resumed");
			}
			/* Ctrl+Alt+L is a portable alias for the original Scroll Lock cockpit
			 * mouse-look action. Queue the original action so its gameplay gates remain
			 * authoritative, and skip this tick so the chord cannot leak into flight. */
			if (in && (in->key_down[AERON_KEY_LCTRL] || in->key_down[AERON_KEY_RCTRL]) &&
				(in->key_down[AERON_KEY_LALT] || in->key_down[AERON_KEY_RALT]) &&
				in->key_pressed[AERON_KEY_A + ('l' - 'a')] && XwaPort_QueueMouseLookToggle()) {
				toggled = 1;
			}
			/* Ctrl+Alt+M releases the flight mouse capture back to the OS and
			 * re-captures it. Handled here, and skipping the tick like the pause
			 * toggle, so the M edge never reaches the classic input path. */
			if (in && (in->key_down[AERON_KEY_LCTRL] || in->key_down[AERON_KEY_RCTRL]) &&
				(in->key_down[AERON_KEY_LALT] || in->key_down[AERON_KEY_RALT]) &&
				in->key_pressed[AERON_KEY_A + ('m' - 'a')]) {
				XwaPort_ToggleMouseCapture();
				toggled = 1;
			}
			XwaRemaster_BeginFrame(in);
			if (Aeron_QuitRequested() || XwaPort_ShouldQuit()) {
				break;
			}
			XwaVrConsole_BeginFrame((float)delta_us * 1e-6f);
			if (paused || toggled || xr_paused) {
				XwaPort_PausedFrame();
			} else {
				/* Lap console presses from the last frame become game actions. */
				XwaVrConsoleAction console_action;
				int console_arg;
				while (XwaVrConsole_PollAction(&console_action, &console_arg)) {
					XwaPort_VrConsoleAction(console_action, console_arg);
				}
				/* Voice commands (push-to-talk on the thumb rests). */
				XwaVrVoice_Update();
				XwaPort_Tick(delta_us);
			}
			{
				XwaVrConsoleState console_state;
				XwaPort_GetVrConsoleState(&console_state);
				XwaVrConsole_SetState(&console_state);
			}
			if (Aeron_QuitRequested() || XwaPort_ShouldQuit()) {
				break;
			}
			XwaRemaster_Frame(delta_us);
			XwaVrConsole_EndFrame();
			XwaVrStick_EndFrame();
			{
				/* The player put the lap console somewhere new: keep it there. */
				char placement[160];
				if (XwaVrConsole_TakePlacement(placement, (int)sizeof placement)) {
					XwaModernVrOptions_SaveValue("XWA_XR_CONSOLE_POSE", placement);
				}
				/* The flight stick moved. */
				char stick_pos[64];
				if (XwaVrStick_TakePlacement(stick_pos, (int)sizeof stick_pos)) {
					XwaModernVrOptions_SaveValue("XWA_XR_STICK_POS", stick_pos);
				}
				/* A HUD panel moved: this cockpit's layout. */
				char layout_key[96];
				char layout[1024];
				if (XwaVrCockpitHud_TakeLayout(layout_key, (int)sizeof layout_key, layout, (int)sizeof layout)) {
					XwaModernVrOptions_SaveValue(layout_key, layout);
				}
			}
			if (Aeron_QuitRequested() || XwaPort_ShouldQuit()) {
				break;
			}
			XwaVrKeyboard_Submit();
			if (!Aeron_Present()) {
				Aeron_RequestFatalRendererError("frame presentation");
				break;
			}
			uint64_t wake_us = presentation_interval_us();
			if (!paused && !xr_paused) {
				const uint64_t task_us = XwaPort_NextWakeDelayUs();
				if (task_us < wake_us)
					wake_us = task_us;
			}
			Aeron_WaitForNextFrame(wake_us);
		}
	}

	exit_code = Aeron_FatalErrorRequested() ? 1 : XwaPort_GetExitCode();
	XwaVrKeyboard_Shutdown();
	XwaVrVoice_Shutdown();
	XwaRemaster_Shutdown();
	XwaPort_Shutdown();
	Aeron_Shutdown();

	return exit_code;
}

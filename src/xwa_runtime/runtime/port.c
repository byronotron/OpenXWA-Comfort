#include "xwa_runtime/runtime/port.h"

#include "aeron/aeron.h"
#include "aeron/compat/ddraw.h"
#include "aeron/compat/host.h"
#include "xwa/assets/file_io.h"
#include "xwa/assets/linez.h"
#include "xwa/assets/opt_model.h"
#include "xwa/assets/sprite_resource.h"
#include "xwa/assets/string_table.h"
#include "xwa/config/game_config.h"
#include "xwa/config/pilot.h"
#include "xwa/flight/flight.h"
#include "xwa/flight/flight_display.h"
#include "xwa/flight/hangar.h"
#include "xwa/flight/hud/hud.h"
#include "xwa/flight/mission/mission.h"
#include "xwa/frontend/flight_loading.h"
#include "xwa/frontend/frontend_display.h"
#include "xwa/frontend/frontend_flight.h"
#include "xwa/frontend/frontend_input.h"
#include "xwa/frontend/frontend_resources.h"
#include "xwa/input/forcefeedback.h"
#include "xwa/render/renderer.h"
#include "xwa/util/memory.h"
#include "xwa/xwa_options.h"
#include "xwa_runtime/config/modern_controller_options_screen.h"
#include "xwa_runtime/config/modern_input_options.h"
#include "xwa_runtime/config/modern_video_options.h"
#include "xwa_runtime/input/controller_mapping.h"
#include "xwa_runtime/input/input_bridge.h"
#include "xwa_runtime/input/mouse_flight.h"
#include "xwa_runtime/input/rumble_provider.h"
#include "xwa_runtime/input/winmm_joystick_provider.h"
#include "xwa_runtime/runtime/flight_pause_task.h"
#include "xwa_runtime/runtime/flight_task.h"
#include "xwa_runtime/runtime/frontend_task.h"
#include "xwa_runtime/runtime/movie_task.h"
#include "xwa_runtime/runtime/presentation.h"
#include "xwa_runtime/snapshot/snapshot.h"
#include "xwa_runtime/timing/host_clock.h"

#include <stdio.h>
#include <string.h>

static int g_xwaPortInitialized;
static int g_xwaPortShouldQuit;
static int g_xwaPortHadFocus;
/* One-way latch: the window has held input focus at least once since init.
 * The frontend/movie focus pause only engages after this is set, so a launch
 * where the OS never handed focus back (setup dialogs, background start) does
 * not begin as a silent freeze. */
static int g_xwaPortEverHadFocus;
static int g_xwaPortClassicFlightRenderingEnabled = 1;
static char g_xwaPortCommandLine[1024];
/* Flight mouse capture. Relative mode is re-asserted on every flight tick, so a
 * manual release has to be latched here instead of applied once. */
static int g_xwaPortMouseCaptureSuspended;
/* Cached OS cursor visibility. Aeron_SetRelativeMouseMode hides the cursor on
 * every transition and never restores it, so visibility is driven explicitly
 * next to each capture decision; -1 forces the first call through. */
static int g_xwaPortHostCursorVisible = -1;

static AeronDx5Rect XwaPort_Dx5PresentationRect(void* context, int surfaceWidth, int surfaceHeight) {
	const XwaPresentationRect rect = XwaPresentation_ClassicSafeFrame();
	(void)context;
	(void)surfaceWidth;
	(void)surfaceHeight;
	return (AeronDx5Rect) { rect.x, rect.y, rect.width, rect.height };
}

static void XwaPort_Dx5Presented(void* context, int surfaceWidth, int surfaceHeight) {
	(void)context;
	XwaSnapshot_EmitSurfaceEvent(XWA_SURFACE_EVENT_PRESENT, 0, 0, surfaceWidth - 1, surfaceHeight - 1);
}

static const char XWA_STRING_DATA_TAG[] = "STRINGDATA";

enum {
	XWA_STRING_DATA_INITIAL_BYTES = 0xa410,
};

static void XwaPort_FreeStartupAssets(void) {
	if (g_stringDataHandle != 0) {
		Memory_FreeHandle(XWA_STRING_DATA_TAG, g_stringDataHandle);
		g_stringDataHandle = 0;
	}

	Linez_FreeDict();
	SpriteResource_FreeGroups();
}

static void XwaPort_ApplyClassicFlightRenderingPolicy(void) {
	const int suppress = XwaFlightTask_IsActive() && !FlightDisplay_IsFrontendModalActive() &&
						 !g_xwaPortClassicFlightRenderingEnabled;
	AeronDx5_SetClassicFlightRenderingSuppressed(suppress);
}

void XwaPort_SetClassicFlightRenderingEnabled(int enabled) {
	g_xwaPortClassicFlightRenderingEnabled = enabled ? 1 : 0;
}

void XwaPort_SubmitRetainedClassicFrame(void) { AeronDx5_ForceSubmitRetainedFrame(); }

uint64_t XwaPort_GetClassicFlightFrameSerial(void) { return AeronDx5_GetClassicFlightFrameSerial(); }

void XwaPort_SetCommandLine(const char* commandLine) {
	if (commandLine == 0) {
		commandLine = "";
	}

	snprintf(g_xwaPortCommandLine, sizeof(g_xwaPortCommandLine), "%s", commandLine);
	g_cmdLine = g_xwaPortCommandLine;
}

int XwaPort_Init(void) {
	AeronDx5_Configure(&(AeronDx5Config) {
		.presentation_rect = XwaPort_Dx5PresentationRect,
		.presented = XwaPort_Dx5Presented,
	});
	XwaWinmmJoystick_RegisterSource();
	XwaRumble_RegisterProvider();
	XwaTime_Reset();
	File_SetVfs(Aeron_GetVfs());
	if (SpriteResource_LoadCatalog("Resdata.txt") != 0) {
		Aeron_LogError("xwa.assets", "Failed to load required sprite catalog 'Resdata.txt'");
	}
	Linez_LoadDict("xwa.tab");
	/* Original _WinMain@16 gives the string loader a valid initial handle. Large
	   string tables replace this allocation after measuring the file. */
	g_stringDataHandle = Memory_AllocHandle(XWA_STRING_DATA_TAG, XWA_STRING_DATA_INITIAL_BYTES);
	if (g_stringDataHandle == 0) {
		Aeron_LogError("xwa.assets", "Failed to allocate initial game string data buffer");
		XwaPort_FreeStartupAssets();
		return 0;
	}
	if (StringTable_LoadGameStrings() != 0) {
		Aeron_LogError("xwa.assets", "Failed to load game string table 'strings.txt'");
		XwaPort_FreeStartupAssets();
		return 0;
	}
	/* Original _WinMain@16 render startup: allocate scene buffers, initialize the
	   D3D texture pool, then enable the frontend model-render path. */
	sw3d_InitSceneBuffers();
	D3DInfo_InitPool();
	Renderer_InitFrontendHardwareSettings();
	g_flightRenderToFrontend = 1;
	/* TODO: Reimplement remaining original WinMain @ 0x50A4A0 early init:
	   deusdbg command-line setup, nopilots/console flags. */
	XwaOptions_ParseCommandLine(g_cmdLine);
	if (!XwaFrontendTask_Init()) {
		return 0;
	}

	g_xwaPortInitialized = 1;
	g_xwaPortShouldQuit = 0;
	g_xwaPortHadFocus = 1;
	g_xwaPortEverHadFocus = 0;
	g_xwaPortClassicFlightRenderingEnabled = 1;
	return 1;
}

int XwaPort_EverHadFocus(void) { return g_xwaPortEverHadFocus; }

static void XwaPort_SetHostCursorVisible(int visible) {
	visible = visible != 0;
	if (g_xwaPortHostCursorVisible != visible) {
		g_xwaPortHostCursorVisible = visible;
		Aeron_SetHostCursorVisible(visible);
	}
}

int XwaPort_QueueMouseLookToggle(void) {
	if (!XwaFlightTask_IsActive() || FlightDisplay_IsFrontendModalActive()) {
		return 0;
	}

	if (g_injectedKeyCount >= (int)(sizeof g_injectedKeyStack / sizeof g_injectedKeyStack[0])) {
		Aeron_LogWarn("xwa", "could not queue cockpit mouse-look toggle: flight key stack is full");
		return 1;
	}

	g_injectedKeyStack[g_injectedKeyCount++] = KEY_SCROLL_LOCK;
	return 1;
}

/* VR controller buttons in flight. Each Touch button has a plain and a
 * shifted (left grip held) action; the right stick flicks only exist
 * shifted. Fire, target-in-sights (right grip), steering, roll and the
 * throttle lever stay on the virtual gamepad. */
typedef struct XwaVrButtonBinding {
	uint16_t plain;
	uint16_t shifted;
} XwaVrButtonBinding;

static const XwaVrButtonBinding g_xwaVrButtonBindings[AERON_XR_BUTTON_COUNT] = {
	[AERON_XR_BUTTON_A]            = { KEY_R, KEY_O },
	[AERON_XR_BUTTON_B]            = { KEY_T, KEY_Y },
	[AERON_XR_BUTTON_X]            = { KEY_C, KEY_SPACE },
	[AERON_XR_BUTTON_Y]            = { KEY_ENTER, KEY_SHIFT_P },
	[AERON_XR_BUTTON_LEFT_TRIGGER] = { KEY_W, KEY_SHIFT_D },
	[AERON_XR_BUTTON_LEFT_STICK]   = { KEY_X, KEY_SHIFT_R },
	[AERON_XR_BUTTON_RIGHT_STICK]  = { KEY_E, KEY_A },
	[AERON_XR_BUTTON_RIGHT_GRIP]   = { KEY_NONE, KEY_I },
	[AERON_XR_BUTTON_FLICK_UP]     = { KEY_NONE, KEY_APOSTROPHE },
	[AERON_XR_BUTTON_FLICK_DOWN]   = { KEY_NONE, KEY_SEMICOLON },
	[AERON_XR_BUTTON_FLICK_LEFT]   = { KEY_NONE, KEY_S },
	[AERON_XR_BUTTON_FLICK_RIGHT]  = { KEY_NONE, KEY_N },
};

static int g_xwaPortVrFlightButtons;

/* Queues a flight action key behind any already pending ones (the injected
 * stack pops from the top, so FIFO order means inserting at the bottom). */
static int XwaPort_QueueFlightActionKey(uint16_t key) {
	int i;
	if (key == KEY_NONE) {
		return 0;
	}
	if (g_injectedKeyCount >= (int)(sizeof g_injectedKeyStack / sizeof g_injectedKeyStack[0])) {
		return 0;
	}
	for (i = g_injectedKeyCount; i > 0; --i) {
		g_injectedKeyStack[i] = g_injectedKeyStack[i - 1];
	}
	g_injectedKeyStack[0] = key;
	++g_injectedKeyCount;
	return 1;
}

/* Short haptic tick on the VR controllers as press feedback. */
static void XwaPort_VrButtonTick(void) {
	const AeronInputSnapshot* input = Aeron_InputSnapshot();
	int                       i;
	if (!input) {
		return;
	}
	for (i = 0; i < AERON_CONTROLLER_MAX; ++i) {
		const AeronControllerSnapshot* pad = &input->controllers[i];
		if (pad->connected && strcmp(pad->path, "openxr:touch") == 0) {
			Aeron_RumbleController(pad->instance_id, 0x3000, 0x6000, 25);
			return;
		}
	}
}

static void XwaPort_UpdateVrFlightButtons(int enabled) {
	AeronXrButton button;
	int           shifted;
	enabled = enabled && Aeron_XrIsPresenting();
	if (enabled != g_xwaPortVrFlightButtons) {
		g_xwaPortVrFlightButtons = enabled;
		Aeron_XrSetFlightButtons(enabled);
	}
	if (!enabled) {
		return;
	}
	while (Aeron_XrPollButton(&button, &shifted)) {
		uint16_t key;
		if ((int)button < 0 || (int)button >= AERON_XR_BUTTON_COUNT) {
			continue;
		}
		key = shifted ? g_xwaVrButtonBindings[button].shifted : g_xwaVrButtonBindings[button].plain;
		if (XwaPort_QueueFlightActionKey(key)) {
			XwaPort_VrButtonTick();
		}
	}
}

/* VR lap console: live ship state for its buttons, and its presses as the
 * game's own key actions. */
static CraftData* XwaPort_VrConsoleCraft(void) {
	const int flight = XwaFlightTask_IsActive();
	PlayerData* player;
	uint16_t objIdx;
	if (!Aeron_XrIsPresenting() || !flight || g_inHangarReady || FlightDisplay_IsFrontendModalActive() ||
		XwaFlightPauseTask_IsActive()) {
		return NULL;
	}
	/* While a mission loads the flight task is already active but the object
	 * storage is not (or still holds the last mission's stale pointers):
	 * gate on the storage handle like the snapshot does. */
	if (!g_objectTable || g_objectTableHandle == 0 || g_localPlayer < 0 || g_localPlayer >= XWA_PLAYER_COUNT) {
		return NULL;
	}
	player = &g_players[g_localPlayer];
	objIdx = (uint16_t)player->objectIndex;
	if (objIdx == 0xffffu || objIdx >= g_objectTableSlotCount || player->mapCameraState || player->hyperspacePhase ||
		g_objectTable[objIdx].objectType == OBJ_None ||
		g_objectTable[objIdx].objectSignature != player->boundObjectSignature || g_objectTable[objIdx].mobj == NULL) {
		return NULL;
	}
	return g_objectTable[objIdx].mobj->pCraft;
}

void XwaPort_GetVrConsoleState(XwaVrConsoleState* out) {
	static int flight_was_active;
	static unsigned int mission_serial;
	const int flight_active = XwaFlightTask_IsActive();
	const CraftData* craft = XwaPort_VrConsoleCraft();
	const PlayerData* player = &g_players[g_localPlayer];
	uint16_t objectType;
	if (flight_active && !flight_was_active) {
		++mission_serial;
	}
	flight_was_active = flight_active;
	memset(out, 0, sizeof *out);
	out->mission_serial = mission_serial;
	if (!craft) {
		return;
	}
	objectType = g_objectTable[(uint16_t)player->objectIndex].objectType;
	out->valid = 1;
	out->laser_level = craft->laserRedirect % 5;
	out->shield_level = craft->shieldRedirect % 5;
	out->beam_level = craft->beamLevel % 5;
	out->has_shields = (craft->systemFlags & 1) != 0;
	out->has_beam = (craft->systemFlags & 0x100) != 0;
	out->shield_balance = craft->shieldDistribMode % 3;
	out->has_sfoils = objectType == OBJ_XWing || objectType == OBJ_BWing;
	out->sfoils_closed = (craft->sFoilState & 2) != 0;
	out->has_target = player->currentTargetObjectIdx != 0xffffu;
	for (int i = 0; i < 3; ++i) {
		const int slot = player->targetPresetSlot[i];
		out->memory_set[i] = slot >= 0 && (uint32_t)slot < g_objectTableSlotCount &&
							 g_objectTable[slot].objectType != OBJ_None;
	}
}

int XwaPort_VrInFlight(void) { return XwaPort_VrConsoleCraft() != NULL; }

/* A line of our own in the in-flight message pane (where "entering area"
 * reports appear), e.g. the voice command confirmations. Text in [brackets]
 * is highlighted like the game's own messages. */
void XwaPort_ShowFlightNote(const char* text) {
	static char note[72];
	if (!text || !text[0] || !XwaPort_VrConsoleCraft()) {
		return;
	}
	snprintf(note, sizeof note, "%s", text);
	g_msgSenderIff = 0;
	msg_addMessagePtr(0, note);
	msg_emitInFlightMessage(MSG_BLANK, g_localPlayer);
}

static void XwaPort_QueueKeyTimes(uint16_t key, int times) {
	for (int i = 0; i < times; ++i) {
		XwaPort_QueueFlightActionKey(key);
	}
}

void XwaPort_VrConsoleAction(XwaVrConsoleAction action, int arg) {
	static const uint16_t simple[XWA_VRC_COUNT] = {
		[XWA_VRC_LASERS_TO_SHIELDS] = KEY_APOSTROPHE,
		[XWA_VRC_SHIELDS_TO_LASERS] = KEY_SEMICOLON,
		[XWA_VRC_TARGET_NEAREST_ENEMY] = KEY_R,
		[XWA_VRC_TARGET_ATTACKER] = KEY_E,
		[XWA_VRC_MATCH_SPEED] = KEY_ENTER,
		[XWA_VRC_TARGET_FRIENDLY] = KEY_F1,
		[XWA_VRC_TARGET_NEUTRAL] = KEY_F2,
		[XWA_VRC_TARGET_ENEMY] = KEY_F3,
		[XWA_VRC_SUBSYSTEM_PREV] = KEY_LESS_THAN,
		[XWA_VRC_SUBSYSTEM_NEXT] = KEY_COMMA,
		[XWA_VRC_TARGET_NEWEST] = KEY_U,
		[XWA_VRC_TARGET_OBJECTIVE] = KEY_O,
		[XWA_VRC_TARGET_CLEAR] = KEY_ALT_C,
		[XWA_VRC_FIRE_LINK] = KEY_X,
		[XWA_VRC_CONVERGENCE] = KEY_Z,
		[XWA_VRC_SFOILS] = KEY_V,
		[XWA_VRC_MAP] = KEY_M,
		[XWA_VRC_DOCK] = KEY_SHIFT_D,
		[XWA_VRC_PICKUP] = KEY_SHIFT_P,
		[XWA_VRC_RELEASE] = KEY_SHIFT_R,
		[XWA_VRC_GUNNER] = KEY_G,
		[XWA_VRC_AUTO_GUNNER] = KEY_F,
		[XWA_VRC_COUNTERMEASURE] = KEY_C,
		[XWA_VRC_EJECT] = KEY_ALT_E,
		[XWA_VRC_WING_ATTACK] = KEY_SHIFT_A,
		[XWA_VRC_WING_COVER] = KEY_SHIFT_C,
		[XWA_VRC_WING_IGNORE] = KEY_SHIFT_I,
		[XWA_VRC_WING_EVADE] = KEY_SHIFT_E,
		[XWA_VRC_WING_WAIT] = KEY_SHIFT_W,
		[XWA_VRC_WING_GO_AHEAD] = KEY_SHIFT_G,
		[XWA_VRC_WING_HOME] = KEY_SHIFT_H,
		[XWA_VRC_WING_REPORT] = KEY_SHIFT_U,
		[XWA_VRC_WING_RESUPPLY] = KEY_SHIFT_B,
		[XWA_VRC_WING_REINFORCE] = KEY_SHIFT_S,
		[XWA_VRC_TARGET_NEXT] = KEY_T,
		[XWA_VRC_TARGET_PREV] = KEY_Y,
	};
	const CraftData* craft = XwaPort_VrConsoleCraft();
	if (!craft || (int)action <= XWA_VRC_NONE || (int)action >= XWA_VRC_COUNT) {
		return;
	}
	switch (action) {
		case XWA_VRC_LASER_LEVEL:
			/* The game cycles levels one key press at a time. */
			XwaPort_QueueKeyTimes(KEY_F9, ((arg - craft->laserRedirect) % 5 + 5) % 5);
			return;
		case XWA_VRC_SHIELD_LEVEL:
			XwaPort_QueueKeyTimes(KEY_F10, ((arg - craft->shieldRedirect) % 5 + 5) % 5);
			return;
		case XWA_VRC_BEAM_LEVEL:
			XwaPort_QueueKeyTimes(KEY_F8, ((arg - craft->beamLevel) % 5 + 5) % 5);
			return;
		case XWA_VRC_SHIELD_BALANCE:
			XwaPort_QueueKeyTimes(KEY_S, ((arg - craft->shieldDistribMode) % 3 + 3) % 3);
			return;
		case XWA_VRC_PRESET_RECALL:
			XwaPort_QueueFlightActionKey(arg ? KEY_F12 : KEY_F11);
			return;
		case XWA_VRC_PRESET_STORE:
			XwaPort_QueueFlightActionKey(arg ? KEY_SHIFT_F12 : KEY_SHIFT_F11);
			return;
		case XWA_VRC_MEMORY_RECALL:
			if (arg >= 0 && arg < 3) {
				XwaPort_QueueFlightActionKey((uint16_t)(KEY_F5 + arg));
			}
			return;
		case XWA_VRC_MEMORY_STORE:
			if (arg >= 0 && arg < 3) {
				XwaPort_QueueFlightActionKey((uint16_t)(KEY_SHIFT_F5 + arg));
			}
			return;
		default:
			XwaPort_QueueFlightActionKey(simple[action]);
			return;
	}
}

/* Picks the VR controller mode for this frame: keyboard-only menus in the
 * hangar, bindable buttons in flight, neither elsewhere (the laser pointer
 * handles the frontend). */
static void XwaPort_UpdateVrModes(void) {
	const int flight = XwaFlightTask_IsActive();
	const int modal  = flight && FlightDisplay_IsFrontendModalActive();
	if (!Aeron_XrIsPresenting()) {
		Aeron_XrSetMenuKeys(0);
		XwaPort_UpdateVrFlightButtons(0);
		return;
	}
	Aeron_XrSetMenuKeys(flight && g_inHangarReady && !modal);
	XwaPort_UpdateVrFlightButtons(flight && !g_inHangarReady && !modal && !XwaFlightPauseTask_IsActive());
}

void XwaPort_ToggleMouseCapture(void) {
	g_xwaPortMouseCaptureSuspended = !g_xwaPortMouseCaptureSuspended;
	Aeron_LogInfo("xwa", "%s", g_xwaPortMouseCaptureSuspended ? "mouse released to OS" : "mouse captured");
}

int XwaPort_IsMouseCaptureSuspended(void) { return g_xwaPortMouseCaptureSuspended; }

static void XwaPort_TickBody(int32_t delta_us) {
	const AeronInputSnapshot* input;
	int flightResult;

	if (!g_xwaPortInitialized) {
		return;
	}
	if (XwaControllerMapping_ConsumeSelectionChange()) {
		XwaModernControllerOptionsScreen_ResetCapture();
		Joystick_ReinitializeDevices();
		if (XwaFlightTask_IsActive()) {
			/* Flight normally detects a joystick only during initialization. */
			const uint16_t joystickActive = (uint16_t)Input_DetectActiveJoystick();
			g_joystickDetectResultWord = joystickActive;
			g_joystickEnabled = joystickActive != 0;
		}
		XwaControllerMapping_CopySelectedActions(g_gameConfig.joyButtons);
		ForceFeedback_Reconfigure();
	}
	XwaPort_ApplyClassicFlightRenderingPolicy();

	input = Aeron_InputSnapshot();
	if (input != 0 && input->has_focus) {
		g_xwaPortEverHadFocus = 1;
	}
	XwaPort_UpdateVrModes();

	XwaSnapshot_BeginTick();

	if (XwaMovieTask_IsActive()) {
		Aeron_SetRelativeMouseMode(0);
		XwaPort_SetHostCursorVisible(0);
		input = Aeron_InputSnapshot();
		XwaInputBridge_UpdateFrontend(input);
		XwaTime_AdvanceHostClock(delta_us);
		XwaSnapshot_SetSceneKind(XWA_SCENE_CUTSCENE);
		XwaMovieTask_Tick();
		XwaSnapshot_Commit();
		return;
	}

	if (XwaFlightTask_IsActive()) {
		const int wasPaused = XwaFlightPauseTask_IsActive();
		XwaSnapshot_SetSceneKind(XWA_SCENE_FLIGHT);
		input = Aeron_InputSnapshot();
		/* Capture this host frame's keyboard/mouse edges into the DirectInput shim so
		 * buffered key events are not lost on frames the fixed-step flight loop skips. */
		AeronCompat_Update(0);
		/* Same for mouse flight: fold this host frame's mouse deltas into its
		 * accumulator so motion on unsampled frames is not lost. */
		XwaMouseFlight_Pump();
		if (FlightDisplay_IsFrontendModalActive()) {
			Aeron_SetRelativeMouseMode(0);
			/* The modal draws the frontend software cursor; keep the OS one hidden
			 * even if the capture was released by hotkey before it opened. */
			XwaPort_SetHostCursorVisible(0);
			XwaInputBridge_UpdateFrontend(input);
			XwaTime_AdvanceHostClock(delta_us);
			XwaFrontendTask_ServiceFrameSystems();
			if (!FlightDisplay_PumpFrontendModal()) {
				XwaSnapshot_SetSceneKind(XWA_SCENE_FRONTEND_MODAL);
				FrontendDisplay_ClearPresentFrameReady();
				XwaSnapshot_Commit();
				return;
			}
		} else {
			/* A click inside the window re-captures a released pointer. The shim
			 * already dropped this frame's buttons -- capture was still off when
			 * AeronCompat_Update ran -- so the restoring click is not also delivered
			 * to the flight sim. */
			if (g_xwaPortMouseCaptureSuspended && input != 0 && input->has_focus &&
				input->mouse.inside_content && input->mouse.pressed_buttons != 0u) {
				g_xwaPortMouseCaptureSuspended = 0;
				Aeron_LogInfo("xwa", "mouse captured");
			}
			const int capture = input != 0 && input->has_focus && !g_xwaPortMouseCaptureSuspended;
			Aeron_SetRelativeMouseMode(capture);
			XwaPort_SetHostCursorVisible(!capture);
			/* Flight keyboard/mouse is served through the DirectInput compat shim,
			 * which samples the Aeron snapshot on demand from the recovered DInput_*
			 * calls during the tick -- no bridge feeding needed here. */
			XwaTime_AdvanceHostClock(delta_us);
		}

		XwaFlightTask_Tick();
		if (wasPaused) {
			/* Retain the complete paused view, including captured HUD draws.
			 * The resume frame also skips simulation and snapshot publication. */
			return;
		}
		if (XwaFlightTask_IsComplete()) {
			flightResult = XwaFlightTask_Shutdown();
			/* Shutdown ends the flight-only suppression scope. Frontend surface
			   initialization below must populate its GPU flip chain normally. */
			XwaPort_ApplyClassicFlightRenderingPolicy();
			FrontendFlight_CompleteLaunchSession(flightResult);
			/* The original launch callback returns 1 after flight result 2,
			   terminating the frontend main loop. */
			if (flightResult == 2) {
				g_xwaPortShouldQuit = 1;
			}
		} else if (FlightDisplay_IsFrontendModalActive()) {
			/* In-flight frontend modals emit the same fixed 640x480 draw stream
			   as the normal frontend. Route it through the HD frontend
			   reconstruction instead of presenting a flight-resolution surface. */
			XwaSnapshot_SetSceneKind(XWA_SCENE_FRONTEND_MODAL);
			FrontendDisplay_ClearPresentFrameReady();
		} else if (FlightLoading_AreFrontendSurfacesAttached()) {
			/* The loading screen is frontend 2D rendered into surfaces temporarily
			   attached to flight. Its snapshot already contains the sprite/glyph/PRESENT
			   records, so route this tick to the frontend reconstruction and do not expose
			   partially initialized flight state to the HD flight renderer. */
			XwaSnapshot_SetSceneKind(XWA_SCENE_LOADING);
			FrontendDisplay_ClearPresentFrameReady();
		} else {
			XwaSnapshot_CaptureFlight();
		}
		XwaSnapshot_Commit();
		return;
	}

	Aeron_SetRelativeMouseMode(0);
	/* The frontend draws its own software cursor and the capture release does not
	 * carry across a flight session. */
	XwaPort_SetHostCursorVisible(0);
	g_xwaPortMouseCaptureSuspended = 0;
	input = Aeron_InputSnapshot();
	XwaInputBridge_UpdateFrontend(input);
	if (input != 0 && !input->has_focus && g_xwaPortEverHadFocus) {
		g_xwaPortHadFocus = 0;
		XwaFrontendTask_Pause();
		XwaSnapshot_Commit();
		return;
	}

	if (!g_xwaPortHadFocus) {
		FrontendDisplay_SetReactivatedFlag(1);
	}
	g_xwaPortHadFocus = 1;
	XwaTime_AdvanceHostClock(delta_us);
	XwaSnapshot_SetSceneKind(XWA_SCENE_FRONTEND);
	XwaFrontendTask_Tick();
	if (FrontendFlight_HasPendingLaunch()) {
		FrontendFlight_BeginPendingLaunch();
	}
	XwaSnapshot_Commit();
}

void XwaPort_Tick(int32_t delta_us) {
	XwaMovieTask_ReapFinished();
	const int movieTick = XwaMovieTask_IsActive();
	XwaPort_TickBody(delta_us);
	if (movieTick || XwaMovieTask_IsActive()) {
		return;
	}
	/* Complete the compatibility renderer's host frame after all recovered
	 * presents have been coalesced and before remaster layers are submitted. */
	AeronDx5_EndFrame();
}

void XwaPort_PausedFrame(void) {
	XwaPort_ApplyClassicFlightRenderingPolicy();
	if (XwaFlightTask_IsActive()) {
		AeronCompat_Update(1);
	}
	XwaMovieTask_ReapFinished();
	if (XwaMovieTask_IsActive()) {
		XwaMovieTask_PausedFrame();
		return;
	}
	AeronDx5_EndFrame();
}

void XwaPort_Shutdown(void) {
	Aeron_SetRelativeMouseMode(0);
	XwaPort_SetHostCursorVisible(1);
	XwaModernVideoOptions_Flush();
	XwaModernInputOptions_Flush();
	/* TODO: Run recovered shutdown paths in original-compatible order. */
	if (XwaFlightTask_IsActive()) {
		XwaFlightTask_Shutdown();
	}
	XwaMovieTask_Shutdown();
	Pilot_Save(0);
	XwaFrontendTask_Shutdown();
	FlightDisplay_FreeSurfaces();
	FrontendDisplay_FreeSurfaces();
	AeronDx5_Shutdown();
	XwaPort_FreeStartupAssets();
	g_xwaPortInitialized = 0;
	g_xwaPortShouldQuit = 0;
	g_xwaPortHadFocus = 0;
	g_xwaPortEverHadFocus = 0;
	g_xwaPortClassicFlightRenderingEnabled = 1;
}

int XwaPort_ShouldQuit(void) {
	return g_xwaPortShouldQuit || File_HasFatalError() || XwaFrontendTask_ShouldQuit();
}

int XwaPort_GetExitCode(void) {
	if (File_HasFatalError()) {
		return File_GetFatalExitCode();
	}

	return 0;
}

uint64_t XwaPort_NextWakeDelayUs(void) {
	if (XwaMovieTask_IsActive()) {
		return XwaMovieTask_NextWakeDelayUs();
	}
	if (XwaFlightTask_IsActive()) {
		if (FlightDisplay_IsFrontendModalActive()) {
			return FlightDisplay_GetFrontendModalWakeDelayUs();
		}
		return XwaFlightTask_NextWakeDelayUs();
	}

	if (FrontendFlight_HasPendingLaunch()) {
		return 0;
	}

	return XwaFrontendTask_NextWakeDelayUs();
}

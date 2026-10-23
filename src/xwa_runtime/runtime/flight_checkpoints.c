#include "xwa_runtime/runtime/flight_checkpoints.h"

#include "aeron/aeron.h"
#include "aeron/log.h"

#include "xwa/audio/fsfx.h"
#include "xwa/flight/flight.h"
#include "xwa/flight/hangar.h"
#include "xwa/flight/hud/hud.h"
#include "xwa/flight/mission/mission.h"
#include "xwa/flight/object/object.h"
#include "xwa/flight/player/player.h"
#include "xwa/config/game_config.h"
#include "xwa/config/pilot.h"
#include "xwa/frontend/mission_setup.h"
#include "xwa/flight/flight_display.h"
#include "xwa_runtime/runtime/flight_pause_task.h"
#include "xwa_runtime/runtime/flight_savestate.h"
#include "xwa_runtime/runtime/flight_task.h"
#include "xwa_runtime/timing/host_clock.h"

#include <stdio.h>
#include <string.h>

enum {
	CP_OFFER_DELAY_US = 1500000,   /* the craft's last moments first */
	CP_OFFER_REPEAT_US = 30000000, /* the line written again now and then (its "ago") */
	CP_OFFER_HOLD_TICKS = 600,     /* the message pane's timer, kept above this */
	CP_OFFER_TIMEOUT_US = 45000000,
	CP_DEATH_MENU_DELAY_US = 2500000, /* the death camera first */
	/* The wreck gone (the mission's end pending, the world stopped): no
	 * longer than this on the still picture. */
	CP_DEATH_MENU_ENDED_US = 500000,
	CP_DEATH_MENU_OPEN_US = 2000000, /* still not open: the mission ends as it would */
	CP_DEATH_MENU_ARM_US = 600000,   /* presses that came with it are not choices */
};

static struct {
	unsigned serial;
	FlightSaveState* quick; /* the one save: made by hand, gone with the mission */
	int seen_destroyed;
	int offering;
	uint64_t offer_start_us;
	uint64_t offer_shown_us;
	int offer_answer; /* -1 none, 0 carry on, 1 retry */
	/* 0 none, 1 craft lost (death camera), 2 due but not open yet, 3 open */
	int death_menu;
	uint64_t death_us; /* the loss; in state 2 the first try to open */
	uint64_t shown_us; /* when it was first drawn (0: not yet) */
	int restarting;    /* Restart Mission chosen: the end check waits for it */
	XwaFlightCheckpointRequest request;
} s_cp = { .offer_answer = -1 };

static int cp_mission_seconds(void) {
	return g_missionElapsedClock.hours * 3600 + g_missionElapsedClock.minutes * 60 + g_missionElapsedClock.seconds;
}

/* A line in the in-flight message pane. */
static void cp_note(const char* text) {
	static char note[96];
	if (g_localPlayer < 0 || g_localPlayer >= XWA_PLAYER_COUNT) {
		return;
	}
	snprintf(note, sizeof note, "%s", text);
	/* Between two steps the side-effects gate is still closed from the
	 * last one (messages would be dropped). */
	const int suppressed = g_flightSimSideEffectsSuppressed;
	g_flightSimSideEffectsSuppressed = 0;
	g_msgSenderIff = 0;
	msg_addMessagePtr(0, note);
	msg_emitInFlightMessage(MSG_BLANK, g_localPlayer);
	g_flightSimSideEffectsSuppressed = suppressed;
}

void XwaFlightCheckpoints_Reset(void) {
	FlightSaveState_Free(s_cp.quick);
	s_cp.quick = NULL;
	s_cp.offering = 0;
	s_cp.offer_answer = -1;
	s_cp.death_menu = 0;
	s_cp.shown_us = 0;
	s_cp.restarting = 0;
	g_configDeathMenuActive = 0;
	s_cp.request = XWA_FLIGHT_CHECKPOINT_REQUEST_NONE;
	s_cp.seen_destroyed = g_localPlayerDestroyedCount;
}

static int cp_restore(FlightSaveState* state, const char* what) {
	char line[96];
	const int at = FlightSaveState_MissionSeconds(state);
	if (!FlightSaveState_Restore(state)) {
		return 0;
	}
	s_cp.offering = 0;
	s_cp.offer_answer = -1;
	s_cp.death_menu = 0;
	s_cp.shown_us = 0;
	g_configDeathMenuActive = 0;
	s_cp.seen_destroyed = g_localPlayerDestroyedCount;
	snprintf(line, sizeof line, "%s (%d:%02d)", what, at / 60, at % 60);
	cp_note(line);
	return 1;
}

/* A single-player mission flying (or in its hangar), no film, not the
 * proving grounds. */
static int cp_active(void) {
	return XwaFlightTask_IsActive() && g_flightPlayerCount == 1 && !g_provingGroundsModeActive &&
		   !g_filmPlaybackMode && !g_filmRecording;
}

int XwaFlightCheckpoints_Available(void) { return cp_active() && !g_inHangarReady; }

int XwaFlightCheckpoints_HasQuickSave(void) { return s_cp.quick && FlightSaveState_Fits(s_cp.quick); }

void XwaFlightCheckpoints_Request(XwaFlightCheckpointRequest request) {
	Aeron_LogInfo("xwa.checkpoint", "request %d", (int)request);
	s_cp.request = request;
}

int XwaFlightCheckpoints_HoldingMissionEnd(void) { return s_cp.offering || s_cp.death_menu; }

int XwaFlightCheckpoints_SkipMissionEndCheck(void) {
	return XwaFlightCheckpoints_HoldingMissionEnd() || (s_cp.restarting && g_flightExitRequest == 2);
}

int XwaFlightCheckpoints_DeathMenuWanted(void) { return cp_active() && s_cp.death_menu != 0; }

int XwaFlightCheckpoints_DeathMenuShown(void) {
	const uint64_t now_us = XwaTime_GetElapsedUs();
	if (!s_cp.death_menu) {
		return 1;
	}
	if (s_cp.death_menu != 3) {
		Aeron_LogInfo("xwa.checkpoint", "death menu open (%s)",
					  s_cp.death_menu == 1 ? "the pilot's menu key" : "due");
	}
	s_cp.death_menu = 3;
	g_configDeathMenuActive = 1;
	if (!s_cp.shown_us) {
		s_cp.shown_us = now_us;
	}
	return now_us - s_cp.shown_us >= CP_DEATH_MENU_ARM_US;
}

int XwaFlightCheckpoints_DeathMenuDue(void) {
	const uint64_t now_us = XwaTime_GetElapsedUs();
	if (!cp_active() || g_inHangarReady || XwaFlightPauseTask_IsActive() ||
		FlightDisplay_IsFrontendModalActive()) {
		return 0;
	}
	if (s_cp.death_menu == 1) {
		/* After the death camera; sooner once the wreck is gone and the
		 * mission's end is pending (the world stands still then). */
		const uint64_t since = now_us - s_cp.death_us;
		if (since < CP_DEATH_MENU_DELAY_US &&
			!(g_flightMissionEndPending && since >= CP_DEATH_MENU_ENDED_US)) {
			return 0;
		}
		s_cp.death_menu = 2;
		s_cp.death_us = now_us;
		Aeron_LogInfo("xwa.checkpoint", "craft lost: death menu");
	} else if (s_cp.death_menu == 2) {
		if (now_us - s_cp.death_us >= CP_DEATH_MENU_OPEN_US) {
			Aeron_LogWarn("xwa.checkpoint", "death menu did not open: the mission ends as it would");
			s_cp.death_menu = 0;
			g_configDeathMenuActive = 0;
			return 0;
		}
	} else {
		return 0;
	}
	g_configDeathMenuActive = 1;
	return 1;
}

/* The death menu's progress (between two steps, with the menu closed). */
static void cp_service_death_menu(void) {
	if (s_cp.death_menu == 1 && FlightSaveState_CanCapture()) {
		Aeron_LogInfo("xwa.checkpoint", "flying again: no death menu");
		s_cp.death_menu = 0; /* flying again (another craft of the flight group) */
		g_configDeathMenuActive = 0;
	} else if (s_cp.death_menu == 3 && !FlightDisplay_IsFrontendModalActive()) {
		/* Closed: Continue lets the mission end as it would; a restart or
		 * the quick save is the request handled next. */
		Aeron_LogInfo("xwa.checkpoint", "death menu closed");
		s_cp.death_menu = 0;
		s_cp.shown_us = 0;
		g_configDeathMenuActive = 0;
	}
}

int XwaFlightCheckpoints_Offering(void) {
	return s_cp.offering && XwaTime_GetElapsedUs() - s_cp.offer_start_us >= CP_OFFER_DELAY_US;
}

int XwaFlightCheckpoints_OfferAnswer(int retry) {
	if (!XwaFlightCheckpoints_Offering()) {
		return 0;
	}
	s_cp.offer_answer = retry ? 1 : 0;
	return 1;
}

static void cp_handle_request(void) {
	const XwaFlightCheckpointRequest request = s_cp.request;
	s_cp.request = XWA_FLIGHT_CHECKPOINT_REQUEST_NONE;
	switch (request) {
		case XWA_FLIGHT_CHECKPOINT_REQUEST_QUICK_SAVE: {
			FlightSaveState* state = FlightSaveState_Capture();
			if (!state) {
				cp_note("Can't save right now");
				return;
			}
			FlightSaveState_Free(s_cp.quick);
			s_cp.quick = state;
			{
				char line[64];
				const int at = FlightSaveState_MissionSeconds(state);
				snprintf(line, sizeof line, "Quick save made (%d:%02d)", at / 60, at % 60);
				cp_note(line);
			}
			return;
		}
		case XWA_FLIGHT_CHECKPOINT_REQUEST_RESTART:
			s_cp.death_menu = 0;
			s_cp.offering = 0;
			g_configDeathMenuActive = 0;
			if (!g_filmPlaybackMode) {
				Aeron_LogInfo("xwa.checkpoint", "restart mission");
				/* Not the end a lost craft's mission would take (its hangar). */
				s_cp.restarting = 1;
				g_flightExitRequest = 2;
				Hud_ClearFlightSurface();
			}
			return;
		case XWA_FLIGHT_CHECKPOINT_REQUEST_QUICK_LOAD:
			if (!XwaFlightCheckpoints_HasQuickSave() || !cp_restore(s_cp.quick, "Quick save loaded")) {
				cp_note("No quick save in this mission");
			}
			return;
		default:
			return;
	}
}

/* The offer while the craft is lost: answered by A / B in VR (through the
 * port's button loop), Enter / Y or N on the keyboard, a gamepad's A / B. */
static void cp_handle_offer(void) {
	const uint64_t now_us = XwaTime_GetElapsedUs();
	/* Flying again (another craft of the flight group): nothing to retry. */
	if (FlightSaveState_CanCapture()) {
		s_cp.offering = 0;
		s_cp.offer_answer = -1;
		return;
	}
	if (now_us - s_cp.offer_start_us < CP_OFFER_DELAY_US) {
		return;
	}
	if (s_cp.offer_answer < 0) {
		const AeronInputSnapshot* in = Aeron_InputSnapshot();
		if (in) {
			if (in->key_pressed[AERON_KEY_RETURN] || in->key_pressed[AERON_KEY_A + ('y' - 'a')]) {
				s_cp.offer_answer = 1;
			} else if (in->key_pressed[AERON_KEY_A + ('n' - 'a')]) {
				s_cp.offer_answer = 0;
			}
			for (int i = 0; s_cp.offer_answer < 0 && i < AERON_CONTROLLER_MAX; ++i) {
				const AeronControllerSnapshot* pad = &in->controllers[i];
				if (!pad->connected || strcmp(pad->path, "openxr:touch") == 0) {
					continue; /* VR: the A / B button loop answers */
				}
				if (pad->gamepad_pressed_buttons & (1u << AERON_GAMEPAD_BUTTON_SOUTH)) {
					s_cp.offer_answer = 1;
				} else if (pad->gamepad_pressed_buttons & (1u << AERON_GAMEPAD_BUTTON_EAST)) {
					s_cp.offer_answer = 0;
				}
			}
		}
	}
	if (s_cp.offer_answer == 1) {
		if (!XwaFlightCheckpoints_HasQuickSave() || !cp_restore(s_cp.quick, "Quick save loaded")) {
			s_cp.offering = 0; /* nothing to go back to: the mission ends as it would */
		}
		return;
	}
	if (s_cp.offer_answer == 0 || now_us - s_cp.offer_start_us >= CP_OFFER_TIMEOUT_US) {
		s_cp.offering = 0;
		s_cp.offer_answer = -1;
		return;
	}
	/* Shown until answered: the message pane's timer is kept from running
	 * out, and the line is put back if something else took its place. */
	if (s_cp.offer_shown_us && g_readyMessagePaneQueue[0].stateOrMessageId != 0xffff) {
		if (g_playerFlightTransientTimers[g_localPlayer].readyMessagePaneTimer < CP_OFFER_HOLD_TICKS) {
			g_playerFlightTransientTimers[g_localPlayer].readyMessagePaneTimer = CP_OFFER_HOLD_TICKS;
		}
	}
	if (!s_cp.offer_shown_us || g_readyMessagePaneQueue[0].stateOrMessageId == 0xffff ||
		now_us - s_cp.offer_shown_us >= CP_OFFER_REPEAT_US) {
		char line[96];
		const int ago = s_cp.quick ? cp_mission_seconds() - FlightSaveState_MissionSeconds(s_cp.quick) : 0;
		s_cp.offer_shown_us = now_us;
		/* At the front of the message pane (its queue does not move on
		 * while the HUD is off for the death camera). */
		Hud_ClearReadyMessageQueue();
		if (Aeron_XrIsPresenting()) {
			snprintf(line, sizeof line, "Load quick save (%d:%02d ago)? A: load  B: no", ago / 60, ago % 60);
		} else {
			snprintf(line, sizeof line, "Load quick save (%d:%02d ago)? Enter: load  N: no", ago / 60, ago % 60);
		}
		cp_note(line);
	}
}

void XwaFlightCheckpoints_ServiceFrame(void) {
	if (!cp_active()) {
		return;
	}
	if (s_cp.serial != XwaFlightTask_MissionSerial()) {
		/* Another mission (or the same one restarted). */
		XwaFlightCheckpoints_Reset();
		s_cp.serial = XwaFlightTask_MissionSerial();
	}
	if (g_inHangarReady) {
		s_cp.offering = 0;
		s_cp.death_menu = 0;
		g_configDeathMenuActive = 0;
		s_cp.seen_destroyed = g_localPlayerDestroyedCount;
		return;
	}
	cp_service_death_menu();
	if (s_cp.request != XWA_FLIGHT_CHECKPOINT_REQUEST_NONE) {
		cp_handle_request();
	}
	if (g_localPlayerDestroyedCount != s_cp.seen_destroyed) {
		s_cp.seen_destroyed = g_localPlayerDestroyedCount;
		/* The death menu (it offers the quick save too); the combat
		 * simulator keeps the original game's ending. */
		if (g_pilotData.missionDirectoryId != MISSION_DIRECTORY_SKIRMISH) {
			s_cp.death_menu = 1;
			s_cp.death_us = XwaTime_GetElapsedUs();
			s_cp.shown_us = 0;
			Aeron_LogInfo("xwa.checkpoint", "craft lost");
		}
	}
	if (s_cp.offering) {
		cp_handle_offer();
	}
}

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
#include "xwa_runtime/runtime/flight_savestate.h"
#include "xwa_runtime/runtime/flight_task.h"
#include "xwa_runtime/timing/host_clock.h"

#include <stdio.h>
#include <string.h>

enum {
	CP_AUTO_INTERVAL_S = 60,  /* an automatic checkpoint about once a minute */
	CP_AUTO_MIN_SPACING_S = 15,
	CP_GOAL_SETTLE_S = 2,     /* after a goal changes, once things settle */
	CP_RETRY_MIN_AGE_S = 12,  /* a retry goes back at least this far, if it can */
	CP_OFFER_DELAY_US = 1500000,  /* the craft's last moments first */
	CP_OFFER_REPEAT_US = 5000000, /* the message pane forgets */
	CP_OFFER_TIMEOUT_US = 45000000,
};

static struct {
	unsigned serial;
	FlightSaveState* autos[2]; /* [0] newest */
	FlightSaveState* quick;
	int last_auto_s;
	uint32_t goal_signature;
	int goal_changed_s; /* -1: none pending */
	int seen_destroyed;
	int offering;
	uint64_t offer_start_us;
	uint64_t offer_shown_us;
	int offer_answer; /* -1 none, 0 carry on, 1 retry */
	XwaFlightCheckpointRequest request;
} s_cp = { .goal_changed_s = -1, .offer_answer = -1 };

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

static void cp_free_all(void) {
	FlightSaveState_Free(s_cp.autos[0]);
	FlightSaveState_Free(s_cp.autos[1]);
	FlightSaveState_Free(s_cp.quick);
	s_cp.autos[0] = s_cp.autos[1] = s_cp.quick = NULL;
}

void XwaFlightCheckpoints_Reset(void) {
	cp_free_all();
	s_cp.last_auto_s = 0;
	s_cp.goal_signature = 0;
	s_cp.goal_changed_s = -1;
	s_cp.offering = 0;
	s_cp.offer_answer = -1;
	s_cp.request = XWA_FLIGHT_CHECKPOINT_REQUEST_NONE;
	s_cp.seen_destroyed = g_localPlayerDestroyedCount;
}

/* The goals' progress, to notice it change. */
static uint32_t cp_goal_signature(void) {
	const MissionFlightRuntimeState* m = &g_missionFlightRuntimeState;
	uint32_t h = 2166136261u;
	const uint8_t* parts[3] = { (const uint8_t*)m->teamGoalStatus, (const uint8_t*)m->teamGlobalGoalState,
								(const uint8_t*)m->globalGoalTriggerCounts };
	const size_t sizes[3] = { sizeof m->teamGoalStatus, sizeof m->teamGlobalGoalState,
							  sizeof m->globalGoalTriggerCounts };
	for (int p = 0; p < 3; ++p) {
		for (size_t i = 0; i < sizes[p]; ++i) {
			h = (h ^ parts[p][i]) * 16777619u;
		}
	}
	h = (h ^ m->globalPrimaryGoalStatus) * 16777619u;
	h = (h ^ m->globalBonusGoalStatus) * 16777619u;
	return h;
}

/* No immediate danger: the hull mostly whole, no missile locked on. */
static int cp_safe(void) {
	const PlayerData* player = &g_players[g_localPlayer];
	const uint32_t objIdx = (uint16_t)player->objectIndex;
	const MobileObject* mobj = g_objectTable[objIdx].mobj;
	const CraftData* craft = mobj ? mobj->pCraft : NULL;
	if (!craft || craft->hullMax <= 0) {
		return 0;
	}
	if ((int64_t)craft->hullDamage * 10 >= (int64_t)craft->hullMax * 6) {
		return 0;
	}
	return g_incomingMissileWarningState == 0;
}

static void cp_take_auto(int now_s) {
	FlightSaveState* state = FlightSaveState_Capture();
	if (!state) {
		return;
	}
	FlightSaveState_Free(s_cp.autos[1]);
	s_cp.autos[1] = s_cp.autos[0];
	s_cp.autos[0] = state;
	s_cp.last_auto_s = now_s;
	Aeron_LogInfo("xwa.checkpoint", "checkpoint at %d:%02d (%zu KB)", now_s / 60, now_s % 60,
				  FlightSaveState_Bytes(state) / 1024);
}

/* After going back to `restored`: the automatic checkpoints taken after it
 * are of a future that will not happen now. */
static void cp_drop_newer_than(const FlightSaveState* restored) {
	const int at = FlightSaveState_MissionSeconds(restored);
	for (int pass = 0; pass < 2; ++pass) {
		if (s_cp.autos[0] && s_cp.autos[0] != restored && FlightSaveState_MissionSeconds(s_cp.autos[0]) > at) {
			FlightSaveState_Free(s_cp.autos[0]);
			s_cp.autos[0] = s_cp.autos[1];
			s_cp.autos[1] = NULL;
		}
	}
}

static int cp_restore(FlightSaveState* state, const char* what) {
	char line[96];
	const int at = FlightSaveState_MissionSeconds(state);
	if (!FlightSaveState_Restore(state)) {
		return 0;
	}
	cp_drop_newer_than(state);
	s_cp.last_auto_s = at;
	s_cp.goal_signature = cp_goal_signature();
	s_cp.goal_changed_s = -1;
	s_cp.offering = 0;
	s_cp.offer_answer = -1;
	s_cp.seen_destroyed = g_localPlayerDestroyedCount;
	snprintf(line, sizeof line, "%s (%d:%02d)", what, at / 60, at % 60);
	cp_note(line);
	return 1;
}

/* The checkpoint a retry goes back to: the newest at least a little before
 * now (not the moment the craft was already lost), else the newest. */
static FlightSaveState* cp_retry_target(void) {
	FlightSaveState* candidates[3] = { s_cp.autos[0], s_cp.autos[1], s_cp.quick };
	FlightSaveState* newest = NULL;
	FlightSaveState* best = NULL;
	const int now_s = cp_mission_seconds();
	for (int i = 0; i < 3; ++i) {
		FlightSaveState* c = candidates[i];
		if (!c || !FlightSaveState_Fits(c)) {
			continue;
		}
		const int at = FlightSaveState_MissionSeconds(c);
		if (!newest || at > FlightSaveState_MissionSeconds(newest)) {
			newest = c;
		}
		if (now_s - at >= CP_RETRY_MIN_AGE_S && (!best || at > FlightSaveState_MissionSeconds(best))) {
			best = c;
		}
	}
	return best ? best : newest;
}

/* A single-player mission flying (or in its hangar), no film, not the
 * proving grounds. */
static int cp_active(void) {
	return XwaFlightTask_IsActive() && g_flightPlayerCount == 1 && !g_provingGroundsModeActive &&
		   !g_filmPlaybackMode && !g_filmRecording;
}

int XwaFlightCheckpoints_Available(void) { return cp_active() && !g_inHangarReady; }

int XwaFlightCheckpoints_HasQuickSave(void) { return s_cp.quick && FlightSaveState_Fits(s_cp.quick); }

int XwaFlightCheckpoints_HasCheckpoint(void) {
	return (s_cp.autos[0] && FlightSaveState_Fits(s_cp.autos[0])) ||
		   (s_cp.autos[1] && FlightSaveState_Fits(s_cp.autos[1]));
}

void XwaFlightCheckpoints_Request(XwaFlightCheckpointRequest request) {
	Aeron_LogInfo("xwa.checkpoint", "request %d", (int)request);
	s_cp.request = request;
}

int XwaFlightCheckpoints_HoldingMissionEnd(void) { return s_cp.offering; }

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
		case XWA_FLIGHT_CHECKPOINT_REQUEST_QUICK_LOAD:
			if (!XwaFlightCheckpoints_HasQuickSave() || !cp_restore(s_cp.quick, "Quick save loaded")) {
				cp_note("No quick save in this mission");
			}
			return;
		case XWA_FLIGHT_CHECKPOINT_REQUEST_RETRY: {
			FlightSaveState* target = s_cp.autos[0] && FlightSaveState_Fits(s_cp.autos[0]) ? s_cp.autos[0]
																						   : s_cp.autos[1];
			if (!target || !cp_restore(target, "Back at the checkpoint")) {
				cp_note("No checkpoint yet");
			}
			return;
		}
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
		FlightSaveState* target = cp_retry_target();
		if (!target || !cp_restore(target, "Retrying from the checkpoint")) {
			s_cp.offering = 0; /* nothing to go back to: the mission ends as it would */
		}
		return;
	}
	if (s_cp.offer_answer == 0 || now_us - s_cp.offer_start_us >= CP_OFFER_TIMEOUT_US) {
		s_cp.offering = 0;
		s_cp.offer_answer = -1;
		return;
	}
	if (!s_cp.offer_shown_us || now_us - s_cp.offer_shown_us >= CP_OFFER_REPEAT_US) {
		const FlightSaveState* target = cp_retry_target();
		char line[96];
		const int ago = target ? cp_mission_seconds() - FlightSaveState_MissionSeconds(target) : 0;
		s_cp.offer_shown_us = now_us;
		/* At the front of the message pane (its queue does not move on
		 * while the HUD is off for the death camera). */
		Hud_ClearReadyMessageQueue();
		if (Aeron_XrIsPresenting()) {
			snprintf(line, sizeof line, "Retry from checkpoint (%d:%02d ago)? A: retry  B: no", ago / 60, ago % 60);
		} else {
			snprintf(line, sizeof line, "Retry from checkpoint (%d:%02d ago)? Enter: retry  N: no", ago / 60,
					 ago % 60);
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
		s_cp.seen_destroyed = g_localPlayerDestroyedCount;
		return;
	}
	if (s_cp.request != XWA_FLIGHT_CHECKPOINT_REQUEST_NONE) {
		cp_handle_request();
	}
	if (g_localPlayerDestroyedCount != s_cp.seen_destroyed) {
		s_cp.seen_destroyed = g_localPlayerDestroyedCount;
		if (cp_retry_target()) {
			s_cp.offering = 1;
			s_cp.offer_start_us = XwaTime_GetElapsedUs();
			s_cp.offer_shown_us = 0;
			s_cp.offer_answer = -1;
		}
	}
	if (s_cp.offering) {
		cp_handle_offer();
		return;
	}

	/* Automatic checkpoints. */
	if (!FlightSaveState_CanCapture()) {
		return;
	}
	{
		const int now_s = cp_mission_seconds();
		const uint32_t goals = cp_goal_signature();
		int due;
		if (goals != s_cp.goal_signature) {
			s_cp.goal_signature = goals;
			if (s_cp.autos[0]) {
				s_cp.goal_changed_s = now_s;
			}
		}
		if (!s_cp.autos[0]) {
			due = 1; /* the mission's start, once flying */
		} else if (now_s - s_cp.last_auto_s < CP_AUTO_MIN_SPACING_S) {
			due = 0;
		} else {
			due = now_s - s_cp.last_auto_s >= CP_AUTO_INTERVAL_S ||
				  (s_cp.goal_changed_s >= 0 && now_s - s_cp.goal_changed_s >= CP_GOAL_SETTLE_S);
		}
		if (due && cp_safe()) {
			cp_take_auto(now_s);
			s_cp.goal_changed_s = -1;
		}
	}
}

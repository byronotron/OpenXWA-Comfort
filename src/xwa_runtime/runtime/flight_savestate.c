#include "xwa_runtime/runtime/flight_savestate.h"

#include "aeron/log.h"

#include "xwa/assets/model_mesh.h"
#include "xwa/audio/fsfx.h"
#include "xwa/audio/music.h"
#include "xwa/audio/sound.h"
#include "xwa/flight/ai/pai.h"
#include "xwa/flight/ai/pai_plan.h"
#include "xwa/flight/death_star.h"
#include "xwa/flight/fediskio.h"
#include "xwa/flight/film.h"
#include "xwa/flight/flight.h"
#include "xwa/flight/flight_display.h"
#include "xwa/flight/flight_light.h"
#include "xwa/flight/hangar.h"
#include "xwa/flight/hud/hud.h"
#include "xwa/flight/mission/mission.h"
#include "xwa/flight/object/collision.h"
#include "xwa/flight/object/debris.h"
#include "xwa/flight/object/object.h"
#include "xwa/flight/player/player.h"
#include "xwa/render/effects.h"
#include "xwa/render/renderer.h"
#include "xwa/render/renderer_internal.h"
#include "xwa/util/memory.h"
#include "xwa/util/random.h"
#include "xwa/util/savestate_region.h"
#include "xwa/util/time.h"
#include "xwa_runtime/runtime/flight_pause_task.h"
#include "xwa_runtime/runtime/flight_task.h"
#include "xwa_runtime/snapshot/snapshot.h"
#include "xwa_runtime/timing/modern_flight_timing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SAVESTATE_MAX_REGIONS = 256 };

typedef struct SaveStateRegion {
	void* addr;
	size_t size;
	size_t offset; /* into the snapshot's bytes */
	const char* name;
} SaveStateRegion;

struct FlightSaveState {
	unsigned mission_serial;
	void* object_table; /* the pools it was taken with */
	void* mobile_pool;
	void* craft_pool;
	int region_count;
	SaveStateRegion regions[SAVESTATE_MAX_REGIONS];
	uint8_t* bytes;
	size_t byte_count;
	/* Film state blocks (Death Star, hangar scene), as the film writes them. */
	uint8_t* film_blocks;
	size_t film_block_bytes;
	/* Time_GetFrameDelta's sub-tick carries (host ms since its last tick). */
	uint32_t frame_delta_carry_ms;
	uint32_t sim_step_carry_ms;
	uint8_t frame_delta_running, sim_step_running; /* the clocks had started */
	int mission_seconds;
	uint16_t player_object_type;
	uint8_t player_region;
};

/* ---- the regions --------------------------------------------------------- */

typedef struct RegionList {
	SaveStateRegion* regions;
	int count;
	size_t bytes;
	int overflow;
} RegionList;

static void savestate_add(void* ctx, void* addr, size_t size, const char* name) {
	RegionList* list = (RegionList*)ctx;
	if (!addr || size == 0) {
		return;
	}
	if (list->count >= SAVESTATE_MAX_REGIONS) {
		list->overflow = 1;
		return;
	}
	list->regions[list->count].addr = addr;
	list->regions[list->count].size = size;
	list->regions[list->count].offset = list->bytes;
	list->regions[list->count].name = name;
	list->bytes += size;
	++list->count;
}

#define SAVESTATE_ADD(list, var) savestate_add((list), (void*)&(var), sizeof(var), #var)
#define SAVESTATE_ADD_ARRAY(list, var) savestate_add((list), (void*)(var), sizeof(var), #var)

/* Everything the simulation keeps between frames, as it is now. */
static void savestate_collect(RegionList* list) {
	/* The object pools (where Mission_Init put them, for the whole mission). */
	savestate_add(list, g_objectTable, sizeof(ObjectRecord) * (size_t)g_objectTableSlotCount, "objects");
	savestate_add(list, g_mobileObjectPoolBase,
				  sizeof(MobileObject) * (size_t)(g_localDebrisObjectSlotsTotal + g_localEffectObjectSlotsTotal +
												  g_regionMainObjectSlotsTotal),
				  "mobile objects");
	savestate_add(list, g_craftDataPoolBase, sizeof(CraftData) * (size_t)g_craftObjectSlotsTotal, "craft");
	savestate_add(list, g_warheadGuidancePoolBase,
				  sizeof(WarheadGuidanceState) * (size_t)(g_projectileObjectSlotsTotal + 1), "warheads");
	savestate_add(list, g_mobileObjectCharDataPool,
				  sizeof(MobileObjectCharData) * (size_t)g_mobileObjectCharDataCount, "char data");
	SAVESTATE_ADD(list, g_nextObjectSignature);
	SAVESTATE_ADD(list, g_objScanStart);
	SAVESTATE_ADD(list, g_localDebrisRecycleSlotCursor);

	/* The mission: flight groups, goals, triggers, messages, clocks. */
	SAVESTATE_ADD_ARRAY(list, g_missionFlightGroups);
	SAVESTATE_ADD_ARRAY(list, g_missionFgStats);
	SAVESTATE_ADD(list, g_missionFlightRuntimeState);
	SAVESTATE_ADD_ARRAY(list, g_missionGlobalGoals);
	SAVESTATE_ADD_ARRAY(list, g_missionTeams);
	SAVESTATE_ADD_ARRAY(list, g_missionMessages);
	SAVESTATE_ADD_ARRAY(list, g_missionMessageTriggered);
	SAVESTATE_ADD_ARRAY(list, g_missionMessageDelayCountdown);
	SAVESTATE_ADD(list, g_missionElapsedClock);
	SAVESTATE_ADD(list, g_missionCountdownClock);
	SAVESTATE_ADD(list, g_missionTimeLimitActive);
	SAVESTATE_ADD(list, g_flightMissionEndPending);
	SAVESTATE_ADD(list, g_teamVictoryTimeLimitMinutes);
	SAVESTATE_ADD(list, g_teamVictoryTimeLimitStarted);
	SAVESTATE_ADD(list, g_missionConditionCurrentCount);
	SAVESTATE_ADD(list, g_missionConditionTotalCount);
	SAVESTATE_ADD(list, g_escapePodPilotFlightGroupIdx);
	SAVESTATE_ADD(list, g_currentFlightGroupIdx);
	SAVESTATE_ADD_ARRAY(list, g_missionGlobalUnitCraftCount);
	SAVESTATE_ADD(list, g_missionRegionHyperPoints);
	SAVESTATE_ADD(list, g_activeMissionRegionCount);
	SAVESTATE_ADD(list, g_maxConnectedPlayerCountThisMission);
	SAVESTATE_ADD(list, g_playerFlightGroupWaveMode);
	SAVESTATE_ADD(list, g_missionStateByte8053E6);

	/* The players. */
	SAVESTATE_ADD_ARRAY(list, g_players);

	/* The AI's plans (orders given in flight add plans). */
	SAVESTATE_ADD_ARRAY(list, g_planTable);
	SAVESTATE_ADD(list, g_planCount);
	SAVESTATE_ADD_ARRAY(list, g_planOrderData);
	SAVESTATE_ADD_ARRAY(list, g_planDataPtrs);
	SAVESTATE_ADD_ARRAY(list, g_builtinPlanIdByNameIndex);
	SAVESTATE_ADD(list, g_aiEscortCandidateFgIdx);

	/* The flight loop's clocks, timers and per-step state. */
	SAVESTATE_ADD(list, g_gameTime);
	SAVESTATE_ADD(list, g_inputTimestamp);
	SAVESTATE_ADD(list, g_serverTickTime);
	SAVESTATE_ADD(list, g_elapsedTicks);
	SAVESTATE_ADD(list, g_simStepScale);
	SAVESTATE_ADD(list, dtMs);
	SAVESTATE_ADD_ARRAY(list, g_flightGlobalCountdownTimers);
	SAVESTATE_ADD(list, g_fighterWarningLastScanTime);
	SAVESTATE_ADD(list, g_fighterWarningForwardCooldownUntil);
	SAVESTATE_ADD(list, g_fighterWarningTailCooldownUntil);
	SAVESTATE_ADD(list, g_fighterWarningPrevForwardThreatCount);
	SAVESTATE_ADD(list, g_fighterWarningPrevNearbyHostileCount);
	SAVESTATE_ADD(list, g_fighterWarningPrevTailAttackerCount);
	SAVESTATE_ADD(list, g_localBeamTargetObjIdx);
	SAVESTATE_ADD(list, g_hyperspaceFxPhaseLatch);
	SAVESTATE_ADD(list, g_throttleSmoothed);
	SAVESTATE_ADD_ARRAY(list, g_inputHistory);
	SAVESTATE_ADD_ARRAY(list, g_inputFrameCount);
	SAVESTATE_ADD(list, g_currentInputFrame);
	SAVESTATE_ADD_ARRAY(list, g_playerFlightTransientTimers);
	SAVESTATE_ADD(list, g_panelLastBeamThreatVoiceTime);
	SAVESTATE_ADD(list, g_panelLastLaserThreatVoiceTime);
	SAVESTATE_ADD(list, g_lastShieldDamageSide);
	/* The radio messages' log (its block lives for the mission). */
	if (g_messageLogHandle != 0) {
		void* log = Memory_LockHandle(g_messageLogHandle);
		Memory_UnlockHandle(g_messageLogHandle);
		savestate_add(list, log, 0x7d00u, "message log");
	}
	SAVESTATE_ADD(list, g_messageLogWriteIndex);
	SAVESTATE_ADD(list, g_messageLogTotalCount);
	SAVESTATE_ADD(list, g_messageLogWrapped);
	SAVESTATE_ADD(list, g_messageLogDrawTotalCount);

	/* Collision's carried-over scratch (the film keeps it too). */
	SAVESTATE_ADD(list, g_collisionSegmentStartWorldX);
	SAVESTATE_ADD(list, g_collisionSegmentStartWorldY);
	SAVESTATE_ADD(list, g_collisionSegmentStartWorldZ);
	SAVESTATE_ADD(list, g_collisionProbeWorldX);
	SAVESTATE_ADD(list, g_collisionProbeWorldY);
	SAVESTATE_ADD(list, g_collisionProbeWorldZ);
	SAVESTATE_ADD(list, g_collisionSweepStartX);
	SAVESTATE_ADD(list, g_collisionSweepStartY);
	SAVESTATE_ADD(list, g_collisionSweepStartZ);
	SAVESTATE_ADD(list, g_collisionSweepEndX);
	SAVESTATE_ADD(list, g_collisionSweepEndY);
	SAVESTATE_ADD(list, g_collisionSweepEndZ);
	SAVESTATE_ADD(list, g_collisionHitOffsetX);
	SAVESTATE_ADD(list, g_collisionHitOffsetY);
	SAVESTATE_ADD(list, g_collisionHitOffsetZ);
	SAVESTATE_ADD_ARRAY(list, g_hardpointOriginOffset);
	SAVESTATE_ADD(list, g_projOffsetY);

	/* Debris and the Death Star's tunnel lasers. */
	SAVESTATE_ADD(list, g_rubbleSpawnRemainingCount);
	SAVESTATE_ADD(list, g_rubbleSpawnVolumeBudget);
	SAVESTATE_ADD_ARRAY(list, g_deathStarTunnelLaserRegions);
	SAVESTATE_ADD(list, g_modelTextureOverrideNextSlot);

	/* The music's plan (the state playing is re-triggered after a restore). */
	SAVESTATE_ADD(list, g_selectedMusicState);
	SAVESTATE_ADD(list, g_setMusicState);
	SAVESTATE_ADD(list, g_musicCombatSeen);

	/* File-static state of other modules. */
	GameRand_SaveStateRegions(savestate_add, list);
	fsfx_SaveStateRegions(savestate_add, list);
	Flight_ModernSaveStateRegions(savestate_add, list);
	XwaModernFlightTiming_SaveStateRegions(savestate_add, list);
	XwaFlightTask_SaveStateRegions(savestate_add, list);
}

/* ---- capture / restore ----------------------------------------------------- */

int FlightSaveState_CanCapture(void) {
	const PlayerData* player;
	uint32_t objIdx;
	if (!XwaFlightTask_IsActive() || XwaFlightTask_IsComplete() || !g_objectTable || g_flightPlayerCount != 1 ||
		g_inHangarReady || g_provingGroundsModeActive || g_filmPlaybackMode || g_filmRecording ||
		XwaFlightPauseTask_IsActive() || FlightDisplay_IsFrontendModalActive() || g_flightMissionEndPending ||
		g_flightExitRequest) {
		return 0;
	}
	if (g_localPlayer < 0 || g_localPlayer >= XWA_PLAYER_COUNT) {
		return 0;
	}
	player = &g_players[g_localPlayer];
	objIdx = (uint16_t)player->objectIndex;
	if (player->connectedFlag != 1 || player->mapCameraState || objIdx >= g_objectTableSlotCount ||
		g_objectTable[objIdx].objectType == OBJ_None || g_objectTable[objIdx].genusId == GENUS_Explosion ||
		g_objectTable[objIdx].objectSignature != player->boundObjectSignature ||
		player->viewState.playerInputBlocked || player->hyperspacePhase != PLAYER_HYPERSPACE_PHASE_NONE) {
		return 0;
	}
	return 1;
}

FlightSaveState* FlightSaveState_Capture(void) {
	FlightSaveState* state;
	RegionList list;
	const uint32_t now = timeGetTime();

	if (!FlightSaveState_CanCapture()) {
		return NULL;
	}
	state = (FlightSaveState*)calloc(1, sizeof *state);
	if (!state) {
		return NULL;
	}
	memset(&list, 0, sizeof list);
	list.regions = state->regions;
	savestate_collect(&list);
	if (list.overflow) {
		Aeron_LogError("xwa.savestate", "too many regions");
		free(state);
		return NULL;
	}
	state->bytes = (uint8_t*)malloc(list.bytes);
	if (!state->bytes) {
		free(state);
		return NULL;
	}
	state->region_count = list.count;
	state->byte_count = list.bytes;
	for (int i = 0; i < list.count; ++i) {
		memcpy(state->bytes + state->regions[i].offset, state->regions[i].addr, state->regions[i].size);
	}

	/* The film's own state blocks (variable-sized, with allocations). */
	Film_BeginMemoryWrite();
	if (g_missionHeader.body.missionType == XWA_MISSION_TYPE_DEATH_STAR) {
		DeathStar_WriteFilmStateBlock();
	}
	Hangar_FilmWriteSceneObjectState();
	for (int i = 0; i < 32; ++i) {
		Film_WriteBytesBuffered(&g_modelTextureOverrideSlots[i].modelType,
								sizeof(g_modelTextureOverrideSlots[i].modelType));
	}
	state->film_blocks = Film_EndMemoryWrite(&state->film_block_bytes);
	if (!state->film_blocks) {
		FlightSaveState_Free(state);
		return NULL;
	}

	state->frame_delta_running = g_lastTickTime != 0;
	state->sim_step_running = g_simStepLastTickTime != 0;
	state->frame_delta_carry_ms = state->frame_delta_running ? now - g_lastTickTime : 0;
	state->sim_step_carry_ms = state->sim_step_running ? now - g_simStepLastTickTime : 0;
	state->mission_serial = XwaFlightTask_MissionSerial();
	state->object_table = g_objectTable;
	state->mobile_pool = g_mobileObjectPoolBase;
	state->craft_pool = g_craftDataPoolBase;
	state->mission_seconds = g_missionElapsedClock.hours * 3600 + g_missionElapsedClock.minutes * 60 +
							 g_missionElapsedClock.seconds;
	{
		const uint32_t objIdx = (uint16_t)g_players[g_localPlayer].objectIndex;
		state->player_object_type = (uint16_t)g_objectTable[objIdx].objectType;
		state->player_region = g_players[g_localPlayer].regionIndex;
	}
	return state;
}

int FlightSaveState_Fits(const FlightSaveState* state) {
	return state && XwaFlightTask_IsActive() && state->mission_serial == XwaFlightTask_MissionSerial() &&
		   state->object_table == g_objectTable && state->mobile_pool == g_mobileObjectPoolBase &&
		   state->craft_pool == g_craftDataPoolBase && g_flightPlayerCount == 1 && !g_inHangarReady &&
		   !g_filmPlaybackMode && !g_filmRecording && g_localPlayer >= 0 && g_localPlayer < XWA_PLAYER_COUNT &&
		   !g_players[g_localPlayer].mapCameraState &&
		   g_players[g_localPlayer].hyperspacePhase == PLAYER_HYPERSPACE_PHASE_NONE;
}

/* Sounds and voices in flight: stopped before the world is put back (the
 * effects' own bookkeeping is part of the snapshot, the queue of voices
 * waiting to speak is not). */
static void savestate_stop_sounds(void) {
	Sound_StopAllInstances();
	fsfx_ResetFlightSfxState(0);
}

/* Particle effects, trails and burn marks on the objects: dropped before
 * the world is put back (setting the effects up again draws on the
 * effects' dice, which the snapshot then puts back). */
static void savestate_reset_effects(void) {
	if (g_useHardware3D && g_objRenderState) {
		memset(g_objRenderState, 0, sizeof(ObjectRenderState) * (size_t)g_regionObjectSlotEnd);
		GlowMark_ShutdownFrameScalesAndPools();
		RenderBatch_FreeDataPoolsThunk();
		Particle_InitEffectTemplates();
		GlowMark_InitFrameScalesAndPools();
		RenderBatch_AllocMeshPassBatches();
		RenderScene_End3D();
	}
}

/* What only mirrors the world, after it is back. */
static void savestate_reset_mirrors(void) {
	/* The HUD's queued messages and panes. */
	Hud_ClearReadyMessageQueue();
	g_mfdLeftNeedsRedraw = 1;
	g_mfdRightNeedsRedraw = 1;
	/* The music plays the restored plan from its start. */
	g_currentMusicState = MUSIC_STATE_NONE;
	/* No input carried over. */
	g_actionKey = KEY_NONE;
	g_injectedKeyCount = 0;
	XwaSnapshot_NoteDiscontinuity();
}

void FlightSaveState_ResetMirrorsForTesting(void) {
	savestate_stop_sounds();
	savestate_reset_effects();
	savestate_reset_mirrors();
}

int FlightSaveState_Restore(const FlightSaveState* state) {
	const uint32_t now = timeGetTime();
	uint16_t typeBefore = 0;
	uint8_t regionBefore = 0;

	if (!FlightSaveState_Fits(state)) {
		return 0;
	}
	if (g_localPlayer >= 0 && g_localPlayer < XWA_PLAYER_COUNT) {
		const uint32_t objIdx = (uint16_t)g_players[g_localPlayer].objectIndex;
		/* The craft whose cockpit is loaded: the one flown now, or (the craft
		 * lost, its object a wreck now) taken to be the snapshot's. */
		typeBefore = state->player_object_type;
		if (objIdx < g_objectTableSlotCount && g_objectTable[objIdx].objectType != OBJ_None &&
			g_objectTable[objIdx].genusId != GENUS_Explosion && g_objectTable[objIdx].mobj &&
			g_objectTable[objIdx].mobj->pCraft) {
			typeBefore = (uint16_t)g_objectTable[objIdx].objectType;
		}
		regionBefore = g_players[g_localPlayer].regionIndex;
	}
	savestate_stop_sounds();
	savestate_reset_effects();
	for (int i = 0; i < state->region_count; ++i) {
		memcpy(state->regions[i].addr, state->bytes + state->regions[i].offset, state->regions[i].size);
	}
	Film_BeginMemoryRead(state->film_blocks, state->film_block_bytes);
	if (g_missionHeader.body.missionType == XWA_MISSION_TYPE_DEATH_STAR) {
		DeathStar_ReadFilmStateBlock();
	}
	Hangar_FilmReadSceneObjectState();
	for (int i = 0; i < 32; ++i) {
		Film_ReadBytes(&g_modelTextureOverrideSlots[i].modelType, sizeof(g_modelTextureOverrideSlots[i].modelType));
		if (g_modelTextureOverrideSlots[i].modelType != 0) {
			ModelMesh_AssignDebrisTexSlot((ObjectTypeId)g_modelTextureOverrideSlots[i].modelType, (uint16_t)i);
		}
	}
	if (!Film_EndMemoryRead()) {
		Aeron_LogWarn("xwa.savestate", "film state blocks did not read back exactly");
	}
	/* The host clock keeps going: the frame-delta clocks keep their sub-tick
	 * carry from the moment taken, measured from now. */
	g_lastTickTime = state->frame_delta_running ? now - state->frame_delta_carry_ms : 0;
	g_simStepLastTickTime = state->sim_step_running ? now - state->sim_step_carry_ms : 0;

	savestate_reset_mirrors();
	/* Taken in flight: no end or exit was under way then. */
	g_flightExitRequest = 0;
	g_flightReturnToFrontendRequested = 0;
	g_flightReturnToMissionSetupRequested = 0;
	if (state->player_object_type != typeBefore) {
		/* Another craft's cockpit than the one flown just now. */
		g_loadingModel = 1;
		FeDiskIo_LoadCockpitModel();
		FeDiskIo_LoadExteriorModel();
		g_loadingModel = 0;
	}
	if (state->player_region != regionBefore) {
		FlightLight_ClearDirectionalLights();
	}
	Aeron_LogInfo("xwa.savestate", "restored the mission at %d:%02d (%zu KB)", state->mission_seconds / 60,
				  state->mission_seconds % 60, (state->byte_count + state->film_block_bytes) / 1024);
	return 1;
}

void FlightSaveState_Free(FlightSaveState* state) {
	if (!state) {
		return;
	}
	free(state->bytes);
	free(state->film_blocks);
	free(state);
}

int FlightSaveState_MissionSeconds(const FlightSaveState* state) { return state ? state->mission_seconds : 0; }

size_t FlightSaveState_Bytes(const FlightSaveState* state) {
	return state ? state->byte_count + state->film_block_bytes : 0;
}

/* ---- testing ----------------------------------------------------------------- */

static uint64_t savestate_hash(const void* data, size_t size, uint64_t h) {
	const uint8_t* p = (const uint8_t*)data;
	for (size_t i = 0; i < size; ++i) {
		h ^= p[i];
		h *= 0x100000001b3ull;
	}
	return h;
}

int FlightSaveState_HashRegions(uint64_t* hashes, const char** names, int max) {
	SaveStateRegion regions[SAVESTATE_MAX_REGIONS];
	RegionList list;
	memset(&list, 0, sizeof list);
	list.regions = regions;
	if (!g_objectTable) {
		return 0;
	}
	savestate_collect(&list);
	for (int i = 0; i < list.count && i < max; ++i) {
		if (hashes) {
			hashes[i] = savestate_hash(regions[i].addr, regions[i].size, 0xcbf29ce484222325ull);
		}
		if (names) {
			names[i] = regions[i].name;
		}
	}
	return list.count;
}

void FlightSaveState_DumpForTesting(const FlightSaveState* state, const char* dir) {
	char path[512];
	for (int i = 0; state && i < state->region_count; ++i) {
		FILE* f;
		snprintf(path, sizeof path, "%s/%s.bin", dir, state->regions[i].name);
		for (char* c = path + strlen(dir) + 1; *c; ++c) {
			if (*c == ' ') {
				*c = '_';
			}
		}
		f = fopen(path, "wb");
		if (f) {
			fwrite(state->bytes + state->regions[i].offset, 1, state->regions[i].size, f);
			fclose(f);
		}
	}
}

void FlightSaveState_DumpLiveForTesting(const char* dir) {
	SaveStateRegion regions[SAVESTATE_MAX_REGIONS];
	RegionList list;
	char path[512];
	memset(&list, 0, sizeof list);
	list.regions = regions;
	savestate_collect(&list);
	for (int i = 0; i < list.count; ++i) {
		FILE* f;
		snprintf(path, sizeof path, "%s/%s.bin", dir, regions[i].name);
		for (char* c = path + strlen(dir) + 1; *c; ++c) {
			if (*c == ' ') {
				*c = '_';
			}
		}
		f = fopen(path, "wb");
		if (f) {
			fwrite(regions[i].addr, 1, regions[i].size, f);
			fclose(f);
		}
	}
}

uint64_t FlightSaveState_HashLive(void) {
	uint64_t hashes[SAVESTATE_MAX_REGIONS];
	const int n = FlightSaveState_HashRegions(hashes, NULL, SAVESTATE_MAX_REGIONS);
	return savestate_hash(hashes, sizeof(uint64_t) * (size_t)n, 0xcbf29ce484222325ull);
}

#include "xwa/frontend/frontend_net.h"

#include <string.h>

#include "xwa/config/game_config.h"
#include "xwa/frontend/frontend_image.h"
#include "xwa/frontend/frontend_mission_session.h"
#include "xwa/frontend/frontend_resources.h"
#include "xwa/frontend/frontend_scrollbar.h"
#include "xwa/frontend/mission_setup.h"
#include "xwa/frontend/net_transport.h"
#include "xwa/util/byte_order.h"
#ifdef XWA_MODERN
#include "xwa_runtime/net/netplay.h"
#include "xwa_runtime/net/netplay_game.h"
#include "xwa_runtime/net/netplay_screens.h"
#endif

// GLOBAL: XWA 0x9EAC40
MpRosterEntry g_mpRoster[8];
// GLOBAL: XWA 0x9EB8C0
int g_mpRosterReadyFlags[8];
// GLOBAL: XWA 0x9EAC20
int isHost;
// GLOBAL: XWA 0xABD7CC
int g_localPilotNetworkPlayerIndex;
// GLOBAL: XWA 0x783ED0
int g_frontendChatTeamOnly;
// GLOBAL: XWA 0x783FE0
int g_frontendNetPacketArg0;
// GLOBAL: XWA 0x783FE4
int g_frontendNetPacketArg1;
// GLOBAL: XWA 0x783FDC
int g_frontendNetPacketSenderPlayerId;
// GLOBAL: XWA 0x5AB8C0
FrontendNetGuid g_frontendNetXwaDirectPlayAppGuid;
// GLOBAL: XWA 0x9EB600
FrontendNetPacket g_frontendNetPacketScratch;

// FUNCTION: XWA 0x56DDB0
int FrontendNet_JoinGameScreen(int frameCounter) {
#ifdef XWA_MODERN
	/* Port: lists NetPlay games found on the network instead of DirectPlay sessions. */
	return XwaNetScreens_JoinGame(frameCounter);
#else
	(void)frameCounter;
	return 0;
#endif
}

// FUNCTION: XWA 0x5718F0
int FrontendNet_HostGameExit(int frameCounter) {
	(void)frameCounter;

	FrontImage_FreeResourceByName("background");
	Frontend_ResetScrollableControls();
	return 0;
}

// FUNCTION: XWA 0x571910
int FrontendNet_HostGameScreen(int frameCounter) {
#ifdef XWA_MODERN
	/* Port: the multiplayer screen (host or join). */
	return XwaNetScreens_MultiplayerSetup(frameCounter);
#else
	(void)frameCounter;
	return 0;
#endif
}

// FUNCTION: XWA 0x571090
int FrontendNet_IsTeamLocalPlayer(int team) {
	int slotIndex;

	for (slotIndex = 0; slotIndex < 16; ++slotIndex) {
		if (g_frontendMission->flightGroups[(int16_t)g_combatSimSlots[slotIndex].fgIndex].team == team &&
			g_combatSimSlots[slotIndex].ownerPlayerId != 0) {
			return g_combatSimSlots[slotIndex].ownerPlayerId == Net_GetLocalPlayerId();
		}
	}

	return 0;
}

/*
 * Frontend packet types (the leading dword of a packet), as the senders in
 * mission_setup.c, mission_briefing.c, mission_debrief.c and frontend_escape.c
 * build them:
 *
 *   'A' 'J' 'I' 'O'  host: game state (mission, roster, options, seed, team
 *                    goals, craft slots). 'J' also starts the briefing, 'I'
 *                    restarts from the debrief.
 *   'N'              host: skirmish slot names and file name.
 *   'Y'              host: game options.
 *   't'              host: the players it has accepted.
 *   'o' (111)        host: lobby summary; its records give the roster order.
 *   'P'              host keepalive.
 *   'C' 'L' 'H'      to the host: team goal, craft slot, slot name edits.
 *   'K'              a player is being dragged (dword: player id or 0).
 *   'Q' 'R'          ready / not ready.
 *   'G'              to the host: this player leaves.
 *   'F'              host quits.        '['  host boots the receiver.
 *   'S' 'f'          host returns everyone to the mission setup.
 *   '\'              host: skip-mission request.
 *   'e'              host: briefing countdown (dword ms).
 *   'p'              fly (to the players of a team).
 *   'u'              a player's rating changed (synthesized from a name change).
 *   'i'              connection lost (synthesized).
 *
 * The function returns the type of the packet it processed (or 1 for one
 * that needs nothing from the caller) and 0 when the queue is empty, with
 * g_frontendNetPacketSenderPlayerId / Arg0 / Arg1 describing it.
 * Reconstructed for the port from the senders and the callers.
 */

static int FrontendNet_RosterIndex(int playerId) {
	int rosterIdx;

	for (rosterIdx = 0; rosterIdx < 8; ++rosterIdx) {
		if (playerId != 0 && g_mpRoster[rosterIdx].playerId == playerId) {
			return rosterIdx;
		}
	}
	return -1;
}

static void FrontendNet_RosterSetName(MpRosterEntry* entry, int playerId) {
	NetPlayerInfo* player = Net_FindPlayer(playerId);

	if (player != NULL) {
		memset(entry->name, 0, sizeof(entry->name));
		strncpy(entry->name, player->sessionName, sizeof(entry->name) - 1);
	}
}

static void FrontendNet_RosterRemove(int playerId) {
	int rosterIdx = FrontendNet_RosterIndex(playerId);

	if (rosterIdx < 0) {
		return;
	}
	for (; rosterIdx < 7; ++rosterIdx) {
		g_mpRoster[rosterIdx] = g_mpRoster[rosterIdx + 1];
		g_mpRosterReadyFlags[rosterIdx] = g_mpRosterReadyFlags[rosterIdx + 1];
	}
	memset(&g_mpRoster[7], 0, sizeof(g_mpRoster[7]));
	g_mpRosterReadyFlags[7] = 0;
}

/* Host: a new player goes to the end of the roster (the order the lobby
 * summary sends to everyone). */
static void FrontendNet_RosterAppend(int playerId) {
	NetPlayerInfo* player;
	int rosterIdx;

	if (FrontendNet_RosterIndex(playerId) >= 0) {
		return;
	}
	/* Close the gaps, moving the ready flags along with their players. */
	for (rosterIdx = 0; rosterIdx < 8; ++rosterIdx) {
		if (g_mpRoster[rosterIdx].playerId == 0) {
			int next;
			for (next = rosterIdx + 1; next < 8 && g_mpRoster[next].playerId == 0; ++next) {
			}
			if (next == 8) {
				break;
			}
			g_mpRoster[rosterIdx] = g_mpRoster[next];
			g_mpRosterReadyFlags[rosterIdx] = g_mpRosterReadyFlags[next];
			memset(&g_mpRoster[next], 0, sizeof(g_mpRoster[next]));
			g_mpRosterReadyFlags[next] = 0;
		}
	}
	for (rosterIdx = 0; rosterIdx < 8; ++rosterIdx) {
		if (g_mpRoster[rosterIdx].playerId == 0) {
			memset(&g_mpRoster[rosterIdx], 0, sizeof(g_mpRoster[rosterIdx]));
			g_mpRoster[rosterIdx].playerId = playerId;
			player = Net_FindPlayer(playerId);
			g_mpRoster[rosterIdx].rating = player != NULL ? (uint8_t)player->playerName[0] - 1 : 0;
			FrontendNet_RosterSetName(&g_mpRoster[rosterIdx], playerId);
			g_mpRosterReadyFlags[rosterIdx] = 0;
			return;
		}
	}
}

/* Client: the roster in the host's order, from the lobby summary records. */
static void FrontendNet_RosterFromRecords(const uint8_t* records, int recordCount, int recordStride) {
	MpRosterEntry roster[8];
	int readyFlags[8];
	int rosterIdx;

	memset(roster, 0, sizeof(roster));
	memset(readyFlags, 0, sizeof(readyFlags));
	for (rosterIdx = 0; rosterIdx < recordCount && rosterIdx < 8; ++rosterIdx) {
		int playerId = ByteOrder_ReadI32Le(records + rosterIdx * recordStride);
		int oldIdx;

		if (playerId == 0) {
			continue;
		}
		roster[rosterIdx].playerId = playerId;
		roster[rosterIdx].rating = ByteOrder_ReadI32Le(records + rosterIdx * recordStride + 4);
		oldIdx = FrontendNet_RosterIndex(playerId);
		if (oldIdx >= 0) {
			memcpy(roster[rosterIdx].name, g_mpRoster[oldIdx].name, sizeof(roster[rosterIdx].name));
			readyFlags[rosterIdx] = g_mpRosterReadyFlags[oldIdx];
		}
		FrontendNet_RosterSetName(&roster[rosterIdx], playerId);
	}
	memcpy(g_mpRoster, roster, sizeof(g_mpRoster));
	memcpy(g_mpRosterReadyFlags, readyFlags, sizeof(g_mpRosterReadyFlags));
}

static void FrontendNet_SetLatency(int playerId, uint32_t latencyMs) {
	int slot;
	int freeSlot = -1;

	if (playerId == 0 || playerId == Net_GetLocalPlayerId() || latencyMs == 0) {
		return;
	}
	for (slot = 0; slot < 40; ++slot) {
		if (g_netPlayerConnectionStats[slot].playerId == playerId) {
			break;
		}
		if (freeSlot < 0 && g_netPlayerConnectionStats[slot].playerId == 0) {
			freeSlot = slot;
		}
	}
	if (slot == 40) {
		if (freeSlot < 0) {
			return;
		}
		slot = freeSlot;
	}
	/* The host measures its own links; others learn them from the host. */
	if (Net_IsHost() || g_netPlayerConnectionStats[slot].playerId == 0 || playerId != Net_GetHostPlayerId()) {
		g_netPlayerConnectionStats[slot].playerId = playerId;
		g_netPlayerConnectionStats[slot].latencyTotalMs = latencyMs;
		g_netPlayerConnectionStats[slot].latencySampleCount = 1;
	}
}

static void FrontendNet_ApplyGameOptions(const uint8_t* options) {
	g_gameConfig.difficulty = options[0];
	g_gameConfig.collisions = options[1];
	g_gameConfig.requirePassword = options[2];
	g_gameConfig.inProgressJoin = options[3];
	g_gameConfig.craftSelection = options[4];
	g_gameConfig.locatePlayers = options[5];
	g_gameConfig.lastTeamTimeLimit = options[6];
	g_gameConfig.eachTeamOwnRegion = options[7];
	g_gameConfig.environment = options[8];
	g_gameConfig.numberOfTeams = options[9];
	g_gameConfig.asyncFlag = options[10];
	g_gameConfig.laps = options[11];
	g_gameConfig.initialDistance = options[12];
	g_gameConfig.serverUpdateRate = options[13];
	g_gameConfig.maxPoints = options[14];
	g_gameConfig.goalType = options[15];
	g_gameConfig.timeLimit = options[16];
	g_gameConfig.randomSeed = (int)ByteOrder_ReadU32Le(options + 17);
}

/* 'A' 'J' 'I' 'O' from the host (see MissionSetup_BroadcastStatePacket). */
static void FrontendNet_ApplyStatePacket(const uint8_t* payload, int payloadSize) {
	int directoryId;
	int missionId;
	int recordIdx;
	int extra;
	int teamGoalsOffset;

	if (payloadSize < 37 + 96 + 21) {
		return;
	}
	memcpy(g_pilotData.multiplayerGameName, payload, 32);
	g_pilotData.multiplayerGameName[31] = '\0';
	directoryId = payload[32];
	missionId = ByteOrder_ReadU16Le(payload + 33);
	g_frontendNetPacketArg0 = directoryId != g_pilotData.missionDirectoryId;
	g_frontendNetPacketArg1 = 0;
	if (directoryId <
		(int)(sizeof(g_pilotData.missionDescriptionIds) / sizeof(g_pilotData.missionDescriptionIds[0]))) {
		g_frontendNetPacketArg1 = g_pilotData.missionDescriptionIds[directoryId] != missionId;
		g_pilotData.missionDirectoryId = directoryId;
		g_pilotData.missionDescriptionIds[directoryId] = missionId;
	}
	for (recordIdx = 0; recordIdx < 8; ++recordIdx) {
		const uint8_t* record = payload + 37 + 12 * recordIdx;
		int playerId = ByteOrder_ReadI32Le(record);
		int rosterIdx = FrontendNet_RosterIndex(playerId);

		if (playerId == 0) {
			continue;
		}
		if (rosterIdx >= 0) {
			g_mpRoster[rosterIdx].rating = ByteOrder_ReadI32Le(record + 4);
		}
		FrontendNet_SetLatency(playerId, ByteOrder_ReadU32Le(record + 8));
	}
	FrontendNet_ApplyGameOptions(payload + 37 + 96);
	/* The sender places the goals and slots after one extra 12-byte record
	 * per player it skipped as not ready. */
	extra = payloadSize - (37 + 96 + 21 + 10 + (int)sizeof(g_combatSimSlots));
	teamGoalsOffset = 37 + 96 + 21 + (extra > 0 && extra % 12 == 0 ? extra : 0);
	if (teamGoalsOffset + 10 + (int)sizeof(g_combatSimSlots) <= payloadSize) {
		memcpy(g_gameConfig.teamGoals, payload + teamGoalsOffset, sizeof(g_gameConfig.teamGoals));
		memcpy(g_combatSimSlots, payload + teamGoalsOffset + 10, sizeof(g_combatSimSlots));
	}
}

static void FrontendNet_HostBroadcastLobby(void) {
	MissionSetup_BroadcastStatePacket(0);
	MissionSetup_BroadcastSkirmishMetadata();
	MissionSetup_BroadcastReadyPlayerRosterPacket(0);
	MissionSetup_BroadcastLobbySelectionPacket();
}

/* A DirectPlay-style system message (sender 0). */
static int FrontendNet_ProcessSystemPacket(uint8_t* packet, int packetSize) {
	int opcode;
	int playerId;
	const char* shortName;
	const char* longName;
	int playerIdx;

	if (packetSize < 12) {
		return 1;
	}
	opcode = ByteOrder_ReadI32Le(packet);
	playerId = ByteOrder_ReadI32Le(packet + 8);
	if (packetSize >= 64) {
		/* names: short and long, each NUL terminated, within 28..63 */
		char* names = (char*)packet + 28;
		names[35] = '\0';
		shortName = names;
		longName = shortName + strlen(shortName) + 1;
		if (longName > names + 35) {
			longName = names + 35;
		}
	} else {
		shortName = "";
		longName = "";
	}
	g_frontendNetPacketSenderPlayerId = playerId;
	switch (opcode) {
		case 3: /* a player joined */
			if (Net_FindPlayer(playerId) == NULL && g_netPlayerCount < 32) {
				NetPlayerInfo* player = &g_netPlayers[g_netPlayerCount++];
				memset(player, 0, sizeof(*player));
				player->playerId = playerId;
				strncpy(player->playerName, longName, sizeof(player->playerName) - 1);
				strncpy(player->sessionName, shortName, sizeof(player->sessionName) - 1);
				player->readyFlag = 1;
			}
			if (Net_IsHost()) {
				FrontendNet_RosterAppend(playerId);
				FrontendNet_HostBroadcastLobby();
			}
			return 1;

		case 5: /* a player left */
			for (playerIdx = 1; playerIdx < g_netPlayerCount; ++playerIdx) {
				if (g_netPlayers[playerIdx].playerId == playerId) {
					memmove(&g_netPlayers[playerIdx], &g_netPlayers[playerIdx + 1],
							(size_t)(g_netPlayerCount - playerIdx - 1) * sizeof(g_netPlayers[0]));
					--g_netPlayerCount;
					memset(&g_netPlayers[g_netPlayerCount], 0, sizeof(g_netPlayers[0]));
					break;
				}
			}
			FrontendNet_RosterRemove(playerId);
			CombatSim_ClearDisconnectedSlotOwners();
			if (playerId == Net_GetHostPlayerId() && !Net_IsHost()) {
				return 'F';
			}
			if (Net_IsHost()) {
				FrontendNet_HostBroadcastLobby();
			}
			return 1;

		case 259: /* a player's names changed: the long name carries the rating */
		{
			NetPlayerInfo* player = Net_FindPlayer(playerId);
			if (player != NULL) {
				memset(player->playerName, 0, sizeof(player->playerName));
				memset(player->sessionName, 0, sizeof(player->sessionName));
				strncpy(player->playerName, longName, sizeof(player->playerName) - 1);
				strncpy(player->sessionName, shortName, sizeof(player->sessionName) - 1);
			}
			g_frontendNetPacketArg0 = (uint8_t)longName[0] - 1;
			return 'u';
		}

		case 0x31: /* the session is gone: the host closed it, or the link failed */
			return packet[4] == 1 ? 'F' : 'i';

		default:
			return 1;
	}
}

// FUNCTION: XWA 0x571DE0
int FrontendNet_ProcessNetworkPackets(void) {
	NetDirectPlayRuntimeState* st;
	NetQueuedPacket* queued;
	uint8_t packet[sizeof(((NetQueuedPacket*)0)->payload)];
	const uint8_t* payload;
	int packetSize;
	int payloadSize;
	int sender;
	int localId;
	int fromHost;
	uint32_t packetType;

	static int s_lostUnhandled;

	if (g_netDirectPlayInterface == NULL) {
		s_lostUnhandled = 0;
		return 0;
	}
	/* Only the mission setup acts on 'i'; elsewhere a lost session is a host quit. */
	if (s_lostUnhandled) {
		s_lostUnhandled = 0;
		g_frontendNetPacketSenderPlayerId = Net_GetHostPlayerId();
		return 'F';
	}
	Net_PumpIncomingPackets();
	st = &g_netDirectPlayRuntimeState;
	if (st->recvQueueCount <= 0) {
		return 0;
	}
	queued = &st->recvQueue[st->recvQueueReadIndex];
	sender = queued->directPlayId;
	packetSize = queued->payloadSize;
	if (packetSize > (int)sizeof(packet)) {
		packetSize = (int)sizeof(packet);
	}
	memset(packet, 0, sizeof(packet));
	memcpy(packet, queued->payload, (size_t)packetSize);
	--st->recvQueueCount;
	if (++st->recvQueueReadIndex >= 1024) {
		st->recvQueueReadIndex = 0;
	}

	g_frontendNetPacketArg0 = 0;
	g_frontendNetPacketArg1 = 0;
	g_frontendNetPacketSenderPlayerId = sender;
	if (sender == 0) {
		int result = FrontendNet_ProcessSystemPacket(packet, packetSize);
		s_lostUnhandled = result == 'i';
		return result;
	}
	if (packetSize < 4) {
		return 1;
	}
	packetType = ByteOrder_ReadU32Le(packet);
	payload = packet + 4;
	payloadSize = packetSize - 4;
	localId = Net_GetLocalPlayerId();
	fromHost = sender == Net_GetHostPlayerId();
	if (payloadSize >= 4) {
		g_frontendNetPacketArg0 = ByteOrder_ReadI32Le(payload);
	}

	switch (packetType) {
		case 'A':
		case 'O':
			/* The host already has the state it sent. */
			if (!fromHost || sender == localId) {
				return 1;
			}
			FrontendNet_ApplyStatePacket(payload, payloadSize);
			return (int)packetType;

		case 'J':
		case 'I':
			if (!fromHost) {
				return 1;
			}
			if (sender != localId) {
				FrontendNet_ApplyStatePacket(payload, payloadSize);
			} else {
				g_frontendNetPacketArg0 = 0;
				g_frontendNetPacketArg1 = 0;
				if (packetType == 'J' && payloadSize >= 37 + 96 + 21) {
					g_gameConfig.randomSeed = (int)ByteOrder_ReadU32Le(payload + 37 + 96 + 17);
				}
			}
#ifdef XWA_MODERN
			if (packetType == 'J' && Net_IsHost()) {
				NetPlay_SetLobbyOpen(0);
			}
#endif
			return (int)packetType;

		case 'N':
			if (!fromHost || sender == localId) {
				return 1;
			}
			if (payloadSize >= (int)(sizeof(g_combatSimSlotNames) + sizeof(g_combatSimSkirmishFileName))) {
				memcpy(g_combatSimSlotNames, payload, sizeof(g_combatSimSlotNames));
				memcpy(g_combatSimSkirmishFileName, payload + sizeof(g_combatSimSlotNames),
					   sizeof(g_combatSimSkirmishFileName));
				g_combatSimSkirmishFileName[sizeof(g_combatSimSkirmishFileName) - 1] = '\0';
			}
			return 'N';

		case 'Y':
			if (!fromHost || sender == localId) {
				return 1;
			}
			if (payloadSize >= 21) {
				FrontendNet_ApplyGameOptions(payload);
			}
			return 'Y';

		case 't':
			if (!fromHost || sender == localId) {
				return 1;
			}
			if (payloadSize >= 8) {
				int count = ByteOrder_ReadI32Le(payload + 4);
				int recordIdx;

				for (recordIdx = 0; recordIdx < count && 8 + 12 * recordIdx + 12 <= payloadSize;
					 ++recordIdx) {
					const uint8_t* record = payload + 8 + 12 * recordIdx;
					int playerId = ByteOrder_ReadI32Le(record);

					Net_MarkPlayerReadyNoLock(playerId);
					FrontendNet_SetLatency(playerId, ByteOrder_ReadU32Le(record + 8));
				}
			}
			return 't';

		case 111:
			if (!fromHost || sender == localId) {
				return 1;
			}
			if (payloadSize >= 52) {
				int recordCount = (payloadSize - 52) / 24;
				int listed = ByteOrder_ReadI32Le(payload + 48);
				int recordIdx;

				if (listed >= 0 && listed < recordCount) {
					recordCount = listed;
				}
				FrontendNet_RosterFromRecords(payload + 52, recordCount, 24);
				for (recordIdx = 0; recordIdx < recordCount; ++recordIdx) {
					const uint8_t* record = payload + 52 + 24 * recordIdx;
					FrontendNet_SetLatency(ByteOrder_ReadI32Le(record), ByteOrder_ReadU32Le(record + 8));
				}
			}
			return 111;

		case 'C':
			if (Net_IsHost() && payloadSize >= 8) {
				int team = ByteOrder_ReadI32Le(payload);
				if (team >= 0 && team < (int)sizeof(g_gameConfig.teamGoals)) {
					g_gameConfig.teamGoals[team] = (uint8_t)ByteOrder_ReadI32Le(payload + 4);
					MissionSetup_BroadcastStatePacket(0);
				}
			}
			return 'C';

		case 'L':
			if (Net_IsHost() && payloadSize >= 4 + (int)sizeof(CombatSimSlot)) {
				int slotIdx = ByteOrder_ReadI32Le(payload);
				if (slotIdx >= 0 && slotIdx < 16) {
					memcpy(&g_combatSimSlots[slotIdx], payload + 4, sizeof(CombatSimSlot));
					MissionSetup_BroadcastStatePacket(0);
				}
			}
			return 'L';

		case 'H':
			if (Net_IsHost() && payloadSize >= 4 + (int)sizeof(g_combatSimSlotNames[0])) {
				int slotIdx = ByteOrder_ReadI32Le(payload);
				if (slotIdx >= 0 && slotIdx < 16) {
					memcpy(g_combatSimSlotNames[slotIdx], payload + 4, sizeof(g_combatSimSlotNames[0]));
					g_combatSimSlotNames[slotIdx][sizeof(g_combatSimSlotNames[0]) - 1] = '\0';
					MissionSetup_BroadcastSkirmishMetadata();
				}
			}
			return 'H';

		case 'Q':
		case 'R': {
			int rosterIdx = FrontendNet_RosterIndex(sender);
			if (rosterIdx >= 0) {
				g_mpRosterReadyFlags[rosterIdx] = packetType == 'Q';
			}
		}
			return (int)packetType;

		case 'F':
		case '[':
		case 'S':
		case 'f':
		case '\\':
		case 'e':
			/* host commands; the host's own copy of 'F' is moot */
			if (!fromHost || (packetType == 'F' && sender == localId)) {
				return 1;
			}
			return (int)packetType;

		default:
			return packetType != 0 ? (int)packetType : 1;
	}
}

// FUNCTION: XWA 0x56F2D0
int FrontendNet_UpdateAndDrawPanel(int frameCounter) {
	(void)frameCounter;

	/* TODO: Reimplement FrontendNet_UpdateAndDrawPanel @ 0x56F2D0. */
	return 0;
}

// FUNCTION: XWA 0x57E850
int MpRoster_CompactActiveEntries(void) {
	unsigned int emptyIndex;
	unsigned int activeIndex;

	for (emptyIndex = 0; emptyIndex < 8; ++emptyIndex) {
		if (g_mpRoster[emptyIndex].playerId == 0) {
			for (activeIndex = emptyIndex + 1; activeIndex < 8; ++activeIndex) {
				if (g_mpRoster[activeIndex].playerId != 0) {
					g_mpRoster[emptyIndex] = g_mpRoster[activeIndex];
					memset(&g_mpRoster[activeIndex], 0, sizeof(g_mpRoster[activeIndex]));
					break;
				}
			}
		}
	}

	return 1;
}

// FUNCTION: XWA 0x57E7F0
int CombatSim_ClearDisconnectedSlotOwners(void) {
	int slotIdx;

	/* Craft slots of players no longer in the game go back to the AI. */
	for (slotIdx = 0; slotIdx < 16; ++slotIdx) {
		if (g_combatSimSlots[slotIdx].ownerPlayerId != 0 &&
			Net_FindPlayer(g_combatSimSlots[slotIdx].ownerPlayerId) == NULL) {
			g_combatSimSlots[slotIdx].ownerPlayerId = 0;
		}
		if (g_combatSimSlots[slotIdx].gunnerPlayerId != 0 &&
			Net_FindPlayer(g_combatSimSlots[slotIdx].gunnerPlayerId) == NULL) {
			g_combatSimSlots[slotIdx].gunnerPlayerId = 0;
		}
	}
	return 1;
}

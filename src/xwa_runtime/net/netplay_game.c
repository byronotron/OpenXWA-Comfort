#include "xwa_runtime/net/netplay_game.h"

#include "xwa_runtime/net/netplay.h"

#include "aeron/log.h"
#include "xwa/flight/net_session.h"
#include "xwa/frontend/net_transport.h"
#include "xwa/net/directplay_private.h"
#include "xwa/util/byte_order.h"

#include <string.h>

#define XN_LOG "xwa.netplay"

/* DirectPlay system message numbers the game understands. */
enum {
	XN_SYS_CREATE_PLAYER = 3,
	XN_SYS_DESTROY_PLAYER = 5,
	XN_SYS_SESSION_LOST = 0x31,
	XN_SYS_SET_PLAYER_NAME = 259,
};

static XwaDirectPlay4 s_frontendInterface;
static XwaDirectPlay4 s_flightInterface;
static int s_attached;
static int s_flightActive;

/* Remote flight packets are re-sequenced on arrival: NetPlay already keeps
 * them in order and complete, and the session's reorder logic only has to
 * see consecutive numbers per sender and class. */
typedef struct XnFlightSeq {
	int32_t id;
	uint8_t next[3];
} XnFlightSeq;

static XnFlightSeq s_flightSeq[NETPLAY_MAX_PLAYERS];

/* ------------------------------------------------------------ interfaces */

static uint32_t AERON_DXAPI xn_Release(XwaDirectPlay4* self) {
	(void)self;
	return 0;
}

static HRESULT AERON_DXAPI xn_Close(XwaDirectPlay4* self) {
	(void)self;
	return 0;
}

static HRESULT AERON_DXAPI xn_CreateGroup(XwaDirectPlay4* self, int32_t* outGroupId, void* name, void* data,
										  uint32_t dataSize, uint32_t flags) {
	(void)self;
	(void)name;
	(void)data;
	(void)dataSize;
	(void)flags;
	if (outGroupId) {
		*outGroupId = NetPlay_GroupId();
	}
	return 0;
}

static HRESULT AERON_DXAPI xn_DestroyPlayer(XwaDirectPlay4* self, int32_t playerId) {
	(void)self;
	(void)playerId;
	return 0;
}

static HRESULT AERON_DXAPI xn_GetCaps(XwaDirectPlay4* self, void* caps, uint32_t flags) {
	(void)self;
	(void)caps;
	(void)flags;
	return 0;
}

/* Length of a compact packet without the piggybacked copy of the previous one. */
static unsigned int xn_compact_core_length(const uint8_t* p, unsigned int size) {
	unsigned int type;
	unsigned int start;
	unsigned int h;

	if (size < 2) {
		return size;
	}
	h = ByteOrder_ReadU16Le(p);
	type = h & 0x7fu;
	start = ((h & 0x8000u) == 0 && (h & 0x80u)) ? 3u : 2u;
	if (type < 0x3cu || type >= 0x40u) {
		unsigned int end;
		if (size < start + 2u) {
			return size;
		}
		end = start + 2u + ByteOrder_ReadU16Le(p + start);
		return end <= size ? end : size;
	}
	return size;
}

static HRESULT AERON_DXAPI xn_SendFlight(XwaDirectPlay4* self, int32_t fromPlayerId, int32_t toPlayerId,
										 uint32_t flags, void* data, uint32_t dataSize) {
	(void)self;
	(void)fromPlayerId;
	(void)flags;
	dataSize = xn_compact_core_length((const uint8_t*)data, dataSize);
	return NetPlay_Send(NETPLAY_KIND_FLIGHT, toPlayerId, data, (int)dataSize) ? 0 : 1;
}

/* The frontend's own compact senders are not used by the game; expand and send. */
static HRESULT AERON_DXAPI xn_SendFrontend(XwaDirectPlay4* self, int32_t fromPlayerId, int32_t toPlayerId,
										   uint32_t flags, void* data, uint32_t dataSize) {
	uint8_t packet[NETPLAY_MAX_MESSAGE];
	const uint8_t* p = (const uint8_t*)data;
	unsigned int h;
	unsigned int start;
	unsigned int len;

	(void)self;
	(void)fromPlayerId;
	(void)flags;
	if (dataSize < 2) {
		return 1;
	}
	h = ByteOrder_ReadU16Le(p);
	start = ((h & 0x8000u) == 0 && (h & 0x80u)) ? 3u : 2u;
	if ((h & 0x7fu) < 0x3cu || (h & 0x7fu) >= 0x40u) {
		if (dataSize < start + 2u) {
			return 1;
		}
		len = ByteOrder_ReadU16Le(p + start);
		start += 2u;
	} else {
		len = dataSize - start;
	}
	if (start + len > dataSize || len + 4u > sizeof packet) {
		return 1;
	}
	ByteOrder_WriteU32Le(packet, h & 0x7fu);
	memcpy(packet + 4, p + start, len);
	return XwaNet_SendFrontend(toPlayerId, packet, len + 4u) ? 0 : 1;
}

static HRESULT AERON_DXAPI xn_SetPlayerName(XwaDirectPlay4* self, uint32_t playerId, XwaDirectPlayName* name,
											uint32_t flags) {
	(void)self;
	(void)playerId;
	(void)flags;
	if (name) {
		NetPlay_Rename(name->shortName, name->longName);
	}
	return 0;
}

static const XwaDirectPlay4Vtbl s_frontendVtbl = {
	.Release = xn_Release,
	.Close = xn_Close,
	.CreateGroup = xn_CreateGroup,
	.DestroyPlayer = xn_DestroyPlayer,
	.GetCaps = xn_GetCaps,
	.Send = xn_SendFrontend,
	.SetPlayerName = xn_SetPlayerName,
};

static const XwaDirectPlay4Vtbl s_flightVtbl = {
	.Release = xn_Release,
	.Close = xn_Close,
	.CreateGroup = xn_CreateGroup,
	.DestroyPlayer = xn_DestroyPlayer,
	.GetCaps = xn_GetCaps,
	.Send = xn_SendFlight,
	.SetPlayerName = xn_SetPlayerName,
};

void* XwaNet_FrontendInterface(void) {
	s_frontendInterface.lpVtbl = &s_frontendVtbl;
	return &s_frontendInterface;
}

void* XwaNet_FlightInterface(void) {
	s_flightInterface.lpVtbl = &s_flightVtbl;
	return &s_flightInterface;
}

/* ---------------------------------------------------------------- queues */

static void xn_runtime_push(int32_t from, const void* data, int len, int packetClass) {
	NetDirectPlayRuntimeState* st = &g_netDirectPlayRuntimeState;
	NetQueuedPacket* q;

	if (len > (int)sizeof q->payload) {
		Aeron_LogWarn(XN_LOG, "frontend packet of %d bytes truncated", len);
		len = (int)sizeof q->payload;
	}
	if (st->recvQueueCount >= 1024) {
		Aeron_LogWarn(XN_LOG, "frontend receive queue full; dropping a packet");
		return;
	}
	q = &st->recvQueue[st->recvQueueWriteIndex];
	memset(q, 0, offsetof(NetQueuedPacket, payload));
	q->directPlayId = from;
	q->payloadSize = len;
	q->packetClass = (uint8_t)packetClass;
	memcpy(q->payload, data, (size_t)len);
	if (len < (int)sizeof q->payload) {
		memset(q->payload + len, 0, sizeof q->payload - (size_t)len);
	}
	++st->recvQueueCount;
	if (++st->recvQueueWriteIndex >= 1024) {
		st->recvQueueWriteIndex = 0;
	}
}

/* Returns the queue entry, or NULL when the packet could not be queued. */
static NetQueuedPacket* xn_session_push(int32_t from, const void* data, int len, int packetClass,
										int sequenceByte) {
	NetQueuedPacket* q;

	if (len > (int)sizeof q->payload) {
		Aeron_LogWarn(XN_LOG, "flight packet of %d bytes dropped", len);
		return NULL;
	}
	if (g_netRecvQueueCount >= 1024) {
		Aeron_LogWarn(XN_LOG, "flight receive queue full; dropping a packet");
		return NULL;
	}
	q = &g_netSessionRecvQueue[g_netRecvQueueWriteIndex];
	memset(q, 0, sizeof *q);
	q->directPlayId = from;
	q->payloadSize = len;
	q->packetClass = (uint8_t)packetClass;
	q->sequenceByte = (uint8_t)sequenceByte;
	memcpy(q->payload, data, (size_t)len);
	++g_netRecvQueueCount;
	if (++g_netRecvQueueWriteIndex >= 1024) {
		g_netRecvQueueWriteIndex = 0;
	}
	return q;
}

/* A DirectPlay-style system message: dword opcode, dword 1 (a player),
 * dword id, and at +28 the short (pilot) and long (rating) names. */
static size_t xn_name_len(const char* name) {
	size_t n = 0;
	while (n < NETPLAY_NAME_LEN - 1 && name[n]) {
		++n;
	}
	return n;
}

static int xn_system_packet(uint8_t* out, int opcode, const NetPlayPlayer* p) {
	size_t shortLen;

	memset(out, 0, 64);
	ByteOrder_WriteU32Le(out, (uint32_t)opcode);
	ByteOrder_WriteU32Le(out + 4, 1u);
	ByteOrder_WriteU32Le(out + 8, p ? (uint32_t)p->id : 0u);
	if (p) {
		shortLen = xn_name_len(p->shortName);
		memcpy(out + 28, p->shortName, shortLen);
		memcpy(out + 28 + shortLen + 1, p->longName, xn_name_len(p->longName));
	}
	return 64;
}

/* The next sequence number for this sender and class; `take` advances it. */
static int xn_flight_seq(int32_t id, int packetClass, int take) {
	int free_slot = -1;

	for (int i = 0; i < NETPLAY_MAX_PLAYERS; ++i) {
		if (s_flightSeq[i].id == id) {
			return (take ? s_flightSeq[i].next[packetClass]++ : s_flightSeq[i].next[packetClass]) & 0x7f;
		}
		if (s_flightSeq[i].id == 0 && free_slot < 0) {
			free_slot = i;
		}
	}
	if (free_slot < 0) {
		return 0;
	}
	s_flightSeq[free_slot].id = id;
	memset(s_flightSeq[free_slot].next, 0, sizeof s_flightSeq[free_slot].next);
	return (take ? s_flightSeq[free_slot].next[packetClass]++ : 0) & 0x7f;
}

/* A compact flight packet from a peer, expanded into the session queue. */
static void xn_flight_receive(int32_t from, const uint8_t* p, int size) {
	uint8_t packet[sizeof(((NetQueuedPacket*)0)->payload)];
	unsigned int h;
	unsigned int type;
	unsigned int start;
	unsigned int len;
	int packetClass;

	if (size < 2) {
		return;
	}
	h = ByteOrder_ReadU16Le(p);
	type = h & 0x7fu;
	if ((h & 0x8080u) == 0x8080u) {
		packetClass = 2;
		start = 2;
	} else if (h & 0x8000u) {
		packetClass = 1;
		start = 2;
	} else if (h & 0x80u) {
		if (size < 3) {
			return;
		}
		packetClass = p[2] <= 2 ? p[2] : 1;
		start = 3;
	} else {
		packetClass = 0;
		start = 2;
	}
	if (type < 0x3cu || type >= 0x40u) {
		if ((unsigned int)size < start + 2u) {
			return;
		}
		len = ByteOrder_ReadU16Le(p + start);
		start += 2u;
	} else {
		len = (unsigned int)size - start;
	}
	if (start + len > (unsigned int)size || len + 4u > sizeof packet) {
		Aeron_LogWarn(XN_LOG, "malformed flight packet from %d", (int)from);
		return;
	}
	ByteOrder_WriteU32Le(packet, type);
	memcpy(packet + 4, p + start, len);
	if (xn_session_push(from, packet, (int)len + 4, packetClass, xn_flight_seq(from, packetClass, 0))) {
		xn_flight_seq(from, packetClass, 1);
	}
}

static void xn_update_latency(void) {
	for (int i = 0; i < g_netPlayerCount && i < 32; ++i) {
		int32_t id = g_netPlayers[i].playerId;
		uint32_t rtt;
		int slot = -1;

		if (id == 0 || id == g_netPlayers[0].playerId) {
			continue;
		}
		rtt = NetPlay_GetRttMs(id);
		if (rtt == 0) {
			continue;
		}
		for (int s = 0; s < 40; ++s) {
			if (g_netPlayerConnectionStats[s].playerId == id) {
				slot = s;
				break;
			}
			if (slot < 0 && g_netPlayerConnectionStats[s].playerId == 0) {
				slot = s;
			}
		}
		if (slot >= 0) {
			g_netPlayerConnectionStats[slot].playerId = id;
			g_netPlayerConnectionStats[slot].latencyTotalMs = rtt;
			g_netPlayerConnectionStats[slot].latencySampleCount = 1;
		}
	}
}

/* ------------------------------------------------------------------- API */

int XwaNet_IsAttached(void) { return s_attached; }

void XwaNet_AttachSession(void) {
	NetPlayPlayer players[NETPLAY_MAX_PLAYERS];
	int n = NetPlay_GetPlayers(players, NETPLAY_MAX_PLAYERS);

	memset(g_netPlayers, 0, sizeof g_netPlayers);
	for (int i = 0; i < n; ++i) {
		g_netPlayers[i].playerId = players[i].id;
		strncpy(g_netPlayers[i].playerName, players[i].longName, sizeof g_netPlayers[i].playerName - 1);
		strncpy(g_netPlayers[i].sessionName, players[i].shortName, sizeof g_netPlayers[i].sessionName - 1);
		g_netPlayers[i].readyFlag = 1;
	}
	g_netPlayerCount = n;
	g_netIsHost = NetPlay_IsHost();
	g_netDirectPlayRuntimeState.localPlayer = g_netPlayers[0];
	g_netDirectPlayRuntimeState.recvQueueReadIndex = 0;
	g_netDirectPlayRuntimeState.recvQueueWriteIndex = 0;
	g_netDirectPlayRuntimeState.recvQueueCount = 0;
	memset(g_netPlayerConnectionStats, 0, sizeof g_netPlayerConnectionStats);
	g_netDirectPlayInterface = XwaNet_FrontendInterface();
	s_attached = 1;
	s_flightActive = 0;
	Aeron_LogInfo(XN_LOG, "session attached: local player %d, %d players, %s", (int)g_netPlayers[0].playerId,
				  n, g_netIsHost ? "host" : "client");
}

void XwaNet_DetachSession(void) {
	if (s_flightActive) {
		XwaNet_EndFlightSession();
	}
	NetPlay_Shutdown();
	if (s_attached) {
		NetPlayerInfo local = g_netPlayers[0];
		memset(g_netPlayers, 0, sizeof g_netPlayers);
		g_netPlayers[0] = local;
		Aeron_LogInfo(XN_LOG, "session detached");
	}
	g_netPlayerCount = 0;
	g_netIsHost = 0;
	g_netDirectPlayInterface = NULL;
	g_netDirectPlayRuntimeState.recvQueueReadIndex = 0;
	g_netDirectPlayRuntimeState.recvQueueWriteIndex = 0;
	g_netDirectPlayRuntimeState.recvQueueCount = 0;
	s_attached = 0;
}

int XwaNet_SendFrontend(int toPlayerId, const void* packet, unsigned int size) {
	int32_t local;

	if (!s_attached) {
		return 1;
	}
	if (size > NETPLAY_MAX_MESSAGE) {
		Aeron_LogError(XN_LOG, "frontend packet of %u bytes not sent", size);
		return 0;
	}
	local = g_netPlayers[0].playerId;
	/* The host closes the lobby as it sends the players to the briefing, so
	 * nobody joins between that packet and the launch. */
	if (size >= 4 && ByteOrder_ReadU32Le(packet) == 'J' && NetPlay_IsHost()) {
		NetPlay_SetLobbyOpen(0);
	}
	/* DirectPlay delivered broadcasts to the sender as well in this game:
	 * the host leaves for the briefing on its own 'J', for example. */
	if (toPlayerId == 0 || toPlayerId == local) {
		xn_runtime_push(local, packet, (int)size, toPlayerId == 0 ? 0 : 1);
	}
	if (toPlayerId != local) {
		NetPlay_Send(NETPLAY_KIND_FRONTEND, toPlayerId, packet, (int)size);
	}
	return 1;
}

int XwaNet_Pump(void) {
	NetPlayMessage m;
	uint8_t sys[64];
	int count = 0;

	if (!s_attached) {
		return 0;
	}
	/* Flight packets stay with NetPlay while the session queue is nearly
	 * full, so none is ever lost (a gap would stall that sender). */
	while (NetPlay_Receive(&m, s_flightActive && g_netRecvQueueCount < 1024 - 16)) {
		NetPlayPlayer p;

		++count;
		memset(&p, 0, sizeof p);
		if (m.kind == NETPLAY_KIND_JOIN || m.kind == NETPLAY_KIND_LEAVE || m.kind == NETPLAY_KIND_RENAME) {
			memcpy(&p, m.data, m.len < (int)sizeof p ? (size_t)m.len : sizeof p);
		}
		if ((m.kind == NETPLAY_KIND_FRONTEND || m.kind == NETPLAY_KIND_FLIGHT) && m.from == 0) {
			continue; /* id 0 is reserved for system messages */
		}
		switch (m.kind) {
			case NETPLAY_KIND_FRONTEND:
				xn_runtime_push(m.from, m.data, m.len, m.route == NETPLAY_ROUTE_DIRECT ? 1 : 0);
				break;
			case NETPLAY_KIND_FLIGHT:
				if (s_flightActive) {
					xn_flight_receive(m.from, m.data, m.len);
				}
				break;
			case NETPLAY_KIND_JOIN:
				xn_runtime_push(0, sys, xn_system_packet(sys, XN_SYS_CREATE_PLAYER, &p), 0);
				break;
			case NETPLAY_KIND_LEAVE:
				xn_runtime_push(0, sys, xn_system_packet(sys, XN_SYS_DESTROY_PLAYER, &p), 0);
				if (s_flightActive) {
					xn_session_push(0, sys, xn_system_packet(sys, XN_SYS_DESTROY_PLAYER, &p), 0, 0);
				}
				break;
			case NETPLAY_KIND_RENAME:
				xn_runtime_push(0, sys, xn_system_packet(sys, XN_SYS_SET_PLAYER_NAME, &p), 0);
				if (s_flightActive) {
					xn_session_push(0, sys, xn_system_packet(sys, XN_SYS_SET_PLAYER_NAME, &p), 0, 0);
				}
				break;
			case NETPLAY_KIND_LOST:
				xn_system_packet(sys, XN_SYS_SESSION_LOST, NULL);
				sys[4] = m.len > 0 ? m.data[0] : 0; /* 1: the host closed the game */
				xn_runtime_push(0, sys, 64, 0);
				if (s_flightActive) {
					p.id = g_netSessionState.hostDplayId;
					xn_session_push(0, sys, xn_system_packet(sys, XN_SYS_DESTROY_PLAYER, &p), 0, 0);
				}
				break;
			default:
				break;
		}
	}
	xn_update_latency();
	return count;
}

int XwaNet_BeginFlightSession(int isHost) {
	int ids[NETPLAY_MAX_PLAYERS];
	int names[NETPLAY_MAX_PLAYERS];
	int n = 0;

	if (!s_attached) {
		return 0;
	}
	/* The session players in id order: the same order, and so the same
	 * player slots, on every machine. The host has the lowest id (slot 0). */
	for (int i = 0; i < g_netPlayerCount && i < 32; ++i) {
		if (g_netPlayers[i].playerId != 0 && g_netPlayers[i].readyFlag == 1 && n < NETPLAY_MAX_PLAYERS) {
			int j = n++;
			while (j > 0 && ids[j - 1] > g_netPlayers[i].playerId) {
				ids[j] = ids[j - 1];
				names[j] = names[j - 1];
				--j;
			}
			ids[j] = g_netPlayers[i].playerId;
			names[j] = i;
		}
	}

	memset(&g_netSessionState, 0, sizeof g_netSessionState);
	g_netSessionState.dplayInterface = XwaNet_FlightInterface();
	g_netSessionState.localPlayerId = isHost ? 1 : 0;
	g_netSessionState.localDplayId = g_netPlayers[0].playerId;
	g_netSessionState.unusedLocalDplayIdMirror = g_netPlayers[0].playerId;
	g_netSessionState.hostDplayId = NetPlay_HostId();
	g_netSessionState.groupDplayId = NetPlay_GroupId();
	g_netSessionState.playerCount = n;
	strncpy(g_netSessionState.localPlayerName, g_netPlayers[0].sessionName,
			sizeof g_netSessionState.localPlayerName - 1);
	for (int i = 0; i < n; ++i) {
		const NetPlayerInfo* src = &g_netPlayers[names[i]];
		g_netSessionState.players[i].dplayId = ids[i];
		g_netSessionState.players[i].activeFlag = 1;
		strncpy(g_netSessionState.players[i].playerName, src->sessionName,
				sizeof g_netSessionState.players[i].playerName - 1);
		strncpy(g_netSessionState.players[i].sessionName, src->playerName,
				sizeof g_netSessionState.players[i].sessionName - 1);
	}
	g_netSessionState.broadcastPendingPayload.pendingFlush = 1;
	g_netSessionState.broadcastPendingPayload.payload[0] = 57;
	g_netSessionState.broadcastPendingPayload.payloadLength = 1;
	g_netSessionState.groupPendingPayload.pendingFlush = 1;
	g_netSessionState.groupPendingPayload.payload[0] = 57;
	g_netSessionState.groupPendingPayload.payloadLength = 1;

	g_netRecvQueueReadIndex = 0;
	g_netRecvQueueWriteIndex = 0;
	g_netRecvQueueCount = 0;
	recvHistoryCount = 0;
	recvQueueHighWater = 0;
	g_netLastDeliveredRecvSequence = 0;
	g_netSessionFlightHandshakeActive = 1;
	memset(s_flightSeq, 0, sizeof s_flightSeq);

	NetPlay_BeginFlight();
	s_flightActive = 1;
	Aeron_LogInfo(XN_LOG, "flight session: %d players, local %d, host %d", n,
				  (int)g_netSessionState.localDplayId, (int)g_netSessionState.hostDplayId);
	return 1;
}

void XwaNet_EndFlightSession(void) {
	if (!s_flightActive) {
		return;
	}
	s_flightActive = 0;
	NetPlay_EndFlight();
	g_netSessionState.dplayInterface = NULL;
	g_netSessionFlightHandshakeActive = 0;
	g_netRecvQueueReadIndex = 0;
	g_netRecvQueueWriteIndex = 0;
	g_netRecvQueueCount = 0;
}

#ifndef XWA_RUNTIME_NET_NETPLAY_GAME_H
#define XWA_RUNTIME_NET_NETPLAY_GAME_H

/*
 * Port addition: connects the game's two network layers to NetPlay.
 *
 * The frontend layer (Net_*, the lobby/briefing/debrief) and the flight layer
 * (NetSession_*) each called DirectPlay through their own interface pointer.
 * Here both pointers are small DirectPlay stand-ins whose Send goes to
 * NetPlay, and the pump sorts what arrives into the game's two receive
 * queues, the way the original Receive loops did.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Attaches a NetPlay session that is already hosting or joined: fills the
 * frontend roster and installs the frontend interface. */
void XwaNet_AttachSession(void);
/* Leaves the session (if any) and clears the frontend network state. */
void XwaNet_DetachSession(void);
int XwaNet_IsAttached(void);

/* Moves received messages into the frontend and flight queues. */
int XwaNet_Pump(void);
/* Frontend send (an uncompressed frontend packet). 0 = everyone, the sender too. */
int XwaNet_SendFrontend(int toPlayerId, const void* packet, unsigned int size);

/* Flight session bracket: sets up the flight interface and roster. */
int XwaNet_BeginFlightSession(int isHost);
void XwaNet_EndFlightSession(void);

/* The two DirectPlay stand-ins. */
void* XwaNet_FrontendInterface(void);
void* XwaNet_FlightInterface(void);

#ifdef __cplusplus
}
#endif

#endif

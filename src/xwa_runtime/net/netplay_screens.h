#ifndef XWA_RUNTIME_NET_NETPLAY_SCREENS_H
#define XWA_RUNTIME_NET_NETPLAY_SCREENS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Port addition: frontend screens for NetPlay (see netplay_screens.c). */
int XwaNetScreens_MultiplayerSetup(int frameCounter);
int XwaNetScreens_JoinGame(int frameCounter);

#ifdef __cplusplus
}
#endif

#endif

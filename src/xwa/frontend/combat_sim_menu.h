#ifndef XWA_FRONTEND_COMBAT_SIM_MENU_H
#define XWA_FRONTEND_COMBAT_SIM_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

int CombatSimMenu_Update(int frameCounter);
int CombatSimMenu_Exit(int frameCounter);
int MultiplayerSetup_Update(int frameCounter);
#ifdef XWA_MODERN
int MultiplayerSetup_Exit(int frameCounter);
#endif

#ifdef __cplusplus
}
#endif

#endif

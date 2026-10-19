#ifndef XWA_FRONTEND_FLIGHT_LOADING_H
#define XWA_FRONTEND_FLIGHT_LOADING_H

#ifdef __cplusplus
extern "C" {
#endif

extern int g_unusedFlightLoadingReadyScreenFlag;
extern int g_flightLoadingReadyScreenCurrentTick;
extern int g_flightLoadingReadyScreenStartTick;
extern int g_frontendLaunchHumanPlayerCount;

int FlightLoading_DrawProgressScreen(int progressStep);
int FlightLoading_PulseAndDrawProgressScreen(int progressStep);
int FlightLoading_ShowInitialProgressScreen(int attachFrontendSurfaces);
int FlightLoading_GetReadyScreen(int frameCounter);
int FlightLoading_FreeReadyScreenResources(void);
int FlightLoading_AttachFrontendSurfaces(void);
int FlightLoading_DetachFrontendSurfaces(void);
int FlightLoading_AreFrontendSurfacesAttached(void);

#ifdef XWA_MODERN
extern const char* g_flightLoadingPrompt;
extern int g_flightLoadingPromptLevel;
/* The briefing summary the loading screen showed (plain text, "* " lines),
 * empty when it showed none; the pause menu's Review Briefing page. */
extern char g_flightLoadingBriefingSummary[4096];
#endif

#ifdef __cplusplus
}
#endif

#endif

#ifndef XWA_FRONTEND_FRONTEND_BOOTSTRAP_H
#define XWA_FRONTEND_FRONTEND_BOOTSTRAP_H

#ifdef __cplusplus
extern "C" {
#endif

int FrontendBootstrap_RunIntroAndEnterConcourse(int frameCounter);
int FrontendBootstrap_InitMode(void);
int FrontendBootstrap_LoadResources(int frameCounter);
int FrontendBootstrap_EnterConcourse(int frameCounter);
/* The startup films (logos, the opening crawl) are playing. */
int FrontendBootstrap_IntroPlaying(void);

#ifdef __cplusplus
}
#endif

#endif

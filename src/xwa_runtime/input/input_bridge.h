#ifndef XWA_RUNTIME_INPUT_BRIDGE_H
#define XWA_RUNTIME_INPUT_BRIDGE_H

#include "aeron/input.h"

#ifdef __cplusplus
extern "C" {
#endif

void XwaInputBridge_UpdateFrontendMouse(const AeronInputSnapshot* input);
void XwaInputBridge_UpdateFrontendKeyboard(const AeronInputSnapshot* input);
void XwaInputBridge_UpdateFrontend(const AeronInputSnapshot* input);
/* Queues one frontend character (printable ASCII, or 8/13/27 for
 * Backspace/Return/Escape), e.g. from the VR on-screen keyboard. */
void XwaInputBridge_InjectFrontendChar(unsigned char ch);

#ifdef __cplusplus
}
#endif

#endif

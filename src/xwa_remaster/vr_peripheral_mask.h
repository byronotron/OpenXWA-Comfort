#ifndef XWA_VR_PERIPHERAL_MASK_H
#define XWA_VR_PERIPHERAL_MASK_H
#include "aeron/render.h"
#include "aeron/xr.h"

void XwaVrPeripheralMask_Reset(void);
void XwaVrPeripheralMask_Update(const float ship_q[4], uint32_t stamp, uint64_t now,
	uint64_t identity, int eligible);
void XwaVrPeripheralMask_Prepare(AeronCommandBuffer* cmd, const AeronXrView views[2], AeronTextureFormat format);
int XwaVrPeripheralMask_Render(AeronCommandBuffer* cmd, AeronRenderTarget* const eyes[2], int w, int h);
void XwaVrPeripheralMask_Shutdown(void);
#endif

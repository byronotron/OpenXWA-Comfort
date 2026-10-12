#ifndef XWA_REMASTER_HUD_FIXED_H
#define XWA_REMASTER_HUD_FIXED_H

#include "xwa_remaster/hud.h"

#include "aeron/scene/draw_list2d.h"

int XwaRemasterHudFixed_Init(void);
void XwaRemasterHudFixed_Shutdown(void);
void XwaRemasterHudFixed_Build(const XwaSnapshot* snapshot, XwaHudProfileIndex profile,
							   uint32_t bundle_generation, const XwaRemasterFlightView* flight_view,
							   int target_w, int target_h);
void XwaRemasterHudFixed_PrepareDrawList(AeronCommandBuffer* cmd, int target_w, int target_h);
typedef struct XwaHudProjectedArrow {
	uint8_t visible;
	uint8_t behind;
	uint8_t padlock;
	uint8_t reserved;
	float center_x_ref;
	float center_y_ref;
	int16_t rotation_angle;
} XwaHudProjectedArrow;
int XwaRemasterHudFixed_ProjectTargetArrow(const XwaSnapshot* snapshot, XwaHudProfileIndex profile,
										   const XwaRemasterFlightView* flight_view, int target_w,
										   int target_h, XwaHudProjectedArrow* out);
const XwaHudProjectedArrow* XwaRemasterHudFixed_TargetArrow(void);
/* Bounds (x0, y0, x1, y1, target pixels) of everything the widget drew in
 * the last PrepareDrawList; 0 when it drew nothing. */
int XwaRemasterHudFixed_WidgetBounds(XwaHudWidgetId widget, float out[4]);
int XwaRemasterHudFixed_ReticleCenter(float* out_x_ref, float* out_y_ref);
void XwaRemasterHudFixed_Render(AeronCommandBuffer* cmd, AeronRenderPass* pass,
								AeronRenderTarget* target, int target_w, int target_h);
/* The world-split elements (see XwaRemasterHud_SetWorldSplit); nothing
 * when the split is off. */
void XwaRemasterHudFixed_RenderWorld(AeronCommandBuffer* cmd, AeronRenderPass* pass,
									 AeronRenderTarget* target, int target_w, int target_h);

#endif

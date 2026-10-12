#ifndef XWA_REMASTER_HUD_TEXT_H
#define XWA_REMASTER_HUD_TEXT_H

#include "xwa_remaster/hud.h"

typedef struct XwaHudMappedGlyph {
	uint16_t pane;
	uint16_t anchor;
	float x_ref, y_ref;
	float w_ref, h_ref;
} XwaHudMappedGlyph;

int XwaRemasterHudText_Init(void);
void XwaRemasterHudText_Shutdown(void);
void XwaRemasterHudText_Build(const XwaSnapshot* snapshot, XwaHudProfileIndex profile,
							  uint32_t bundle_generation);
/* Target-pixel bounds (x0, y0, x1, y1) of a visible text pane's classic
 * rectangle; 0 when the pane is not shown. */
/* Bounds of the glyphs the pane drew in the last PrepareDrawList; 0 when
 * it drew none. */
int XwaRemasterHudText_PaneGlyphBounds(uint16_t pane_id, float out[4]);
int XwaRemasterHudText_PaneBounds(uint16_t pane_id, int target_w, int target_h, float out[4]);
void XwaRemasterHudText_PrepareDrawList(AeronCommandBuffer* cmd, int target_w, int target_h);
void XwaRemasterHudText_RenderCmdGroup(AeronCommandBuffer* cmd, AeronRenderPass* pass, AeronRenderTarget* target,
									   int target_w, int target_h);
void XwaRemasterHudText_Render(AeronCommandBuffer* cmd, AeronRenderPass* pass,
							   AeronRenderTarget* target, int target_w, int target_h);
int XwaRemasterHudText_MapGlyph(const XwaHudPane* pane, const XwaHudGlyph* glyph, XwaHudProfileIndex profile,
								int screen_w, int screen_h, XwaHudMappedGlyph* out);
int XwaRemasterHudText_GlyphVisible(const XwaHudPane* pane, const XwaHudGlyph* glyph,
									XwaHudProfileIndex profile, int screen_w, int screen_h);

#endif

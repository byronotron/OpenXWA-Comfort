/* VR cockpit HUD panels; see vr_cockpit_hud.h. */

#include "xwa_remaster/vr_cockpit_hud.h"

#include "aeron/aeron.h"
#include "xwa_remaster/hud.h"
#include "xwa_remaster/hud_cmd.h"
#include "xwa_remaster/hud_fixed.h"
#include "xwa_remaster/hud_text.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum {
	CK_LEFT_POD,
	CK_RIGHT_POD,
	CK_MESSAGES,
	CK_MESSAGES_LOW,
	CK_COUNTERS,
	CK_SPEED,
	CK_CLOCK,
	CK_CHARGE,
	CK_CMD,
	CK_MFD_LEFT,
	CK_MFD_RIGHT,
	CK_COUNT
};

/* Placement relative to the seated eye point: yaw (+ right) and pitch
 * (+ up) in degrees, distance in metres, and the box (metres) the panel's
 * rectangle is fitted into, keeping its aspect. X-wing defaults. */
typedef struct CkSpec {
	const char* name;
	float yaw, pitch, distance;
	float box_w, box_h;
	/* > 0: fixed size, metres per classic HUD unit (1/480 of the screen
	 * height), instead of fitting the box: text keeps one size whatever the
	 * panel's width. */
	float m_per_unit;
} CkSpec;

static const CkSpec k_spec[CK_COUNT] = {
	[CK_LEFT_POD] = { "fore radar pod", -40.0f, 2.0f, 0.75f, 0.17f, 0.21f },
	[CK_RIGHT_POD] = { "aft radar pod", 40.0f, 2.0f, 0.75f, 0.17f, 0.21f },
	[CK_MESSAGES] = { "messages", 0.0f, -7.0f, 0.95f, 0.36f, 0.08f, 0.0036f },
	/* The flight-group line sits low on the flat screen (above the
	 * targeting computer): its own panel, so the two message areas don't
	 * join into one huge panel. */
	[CK_MESSAGES_LOW] = { "flight-group messages", 0.0f, -16.0f, 0.95f, 0.36f, 0.08f, 0.0027f },
	/* The top-bar readouts sit in a row on the targeting computer's top
	 * edge (placed by ck_place_row; the pose here is only a fallback). */
	[CK_COUNTERS] = { "counters", 0.0f, -21.0f, 0.62f, 0.12f, 0.03f },
	[CK_SPEED] = { "speed", -9.0f, -21.0f, 0.62f, 0.12f, 0.03f },
	[CK_CLOCK] = { "clock", 9.0f, -21.0f, 0.62f, 0.12f, 0.03f },
	[CK_CHARGE] = { "charge", 0.0f, -17.0f, 0.85f, 0.15f, 0.03f },
	[CK_CMD] = { "targeting computer", 0.0f, -30.0f, 0.62f, 0.20f, 0.16f },
	/* Beside the targeting computer, above the lap console. */
	[CK_MFD_LEFT] = { "left MFD", -29.0f, -25.0f, 0.62f, 0.17f, 0.13f },
	[CK_MFD_RIGHT] = { "right MFD", 29.0f, -25.0f, 0.62f, 0.17f, 0.13f },
};

static const XwaHudWidgetId k_widgets[CK_COUNT][5] = {
	[CK_LEFT_POD] = { XWA_HUD_WIDGET_FORE_RADAR_FRAME, XWA_HUD_WIDGET_FORE_RADAR_SCOPE, XWA_HUD_WIDGET_LEFT_POWER,
					  XWA_HUD_WIDGET_SHIELD_HULL, XWA_HUD_WIDGET_COUNT },
	[CK_RIGHT_POD] = { XWA_HUD_WIDGET_AFT_RADAR_FRAME, XWA_HUD_WIDGET_AFT_RADAR_SCOPE, XWA_HUD_WIDGET_RIGHT_POWER,
					   XWA_HUD_WIDGET_BEAM, XWA_HUD_WIDGET_COUNT },
	[CK_MESSAGES] = { XWA_HUD_WIDGET_COUNT },
	[CK_MESSAGES_LOW] = { XWA_HUD_WIDGET_COUNT },
	[CK_COUNTERS] = { XWA_HUD_WIDGET_COUNT },
	[CK_SPEED] = { XWA_HUD_WIDGET_COUNT },
	[CK_CLOCK] = { XWA_HUD_WIDGET_COUNT },
	/* The charge bars sit inside the targeting computer's frame; a separate
	 * panel would show the target behind them, so they stay part of it. */
	[CK_CHARGE] = { XWA_HUD_WIDGET_COUNT },
	[CK_CMD] = { XWA_HUD_WIDGET_CMD_FRAME, XWA_HUD_WIDGET_LASER_CHARGE, XWA_HUD_WIDGET_ION_CHARGE,
				 XWA_HUD_WIDGET_COUNT },
	[CK_MFD_LEFT] = { XWA_HUD_WIDGET_MFD_LEFT_FRAME, XWA_HUD_WIDGET_COUNT },
	[CK_MFD_RIGHT] = { XWA_HUD_WIDGET_MFD_RIGHT_FRAME, XWA_HUD_WIDGET_COUNT },
};

static const uint16_t k_panes[CK_COUNT][4] = {
	[CK_LEFT_POD] = { XWA_HUD_PANE_LEFT_SUBSYSTEM, 0 },
	[CK_RIGHT_POD] = { XWA_HUD_PANE_RIGHT_SUBSYSTEM, 0 },
	[CK_MESSAGES] = { XWA_HUD_PANE_MESSAGE_SYSTEM, XWA_HUD_PANE_MESSAGE_READY, 0 },
	[CK_MESSAGES_LOW] = { XWA_HUD_PANE_MESSAGE_FLIGHT_GROUP, 0 },
	[CK_COUNTERS] = { XWA_HUD_PANE_TOP_WEAPONS, XWA_HUD_PANE_TOP_COUNTERMEASURE, XWA_HUD_PANE_TOP_PROVING_STATUS, 0 },
	[CK_SPEED] = { XWA_HUD_PANE_TOP_SPEED, XWA_HUD_PANE_TOP_THROTTLE, 0 },
	[CK_CLOCK] = { XWA_HUD_PANE_TOP_CRAFT_NAME, XWA_HUD_PANE_TOP_CLOCK, 0 },
	[CK_CHARGE] = { 0 },
	[CK_CMD] = { XWA_HUD_PANE_CMD, 0 },
	[CK_MFD_LEFT] = { XWA_HUD_PANE_MFD_LEFT_TITLE, XWA_HUD_PANE_MFD_LEFT_BODY, 0 },
	[CK_MFD_RIGHT] = { XWA_HUD_PANE_MFD_RIGHT_TITLE, XWA_HUD_PANE_MFD_RIGHT_BODY, 0 },
};

static struct {
	int target_w, target_h;
	uint32_t layout_generation, bundle_generation;
	/* Grow-only union of what each panel showed since the last reset, so a
	 * panel keeps its size while parts of it come and go. */
	float latched[CK_COUNT][4];
	uint8_t latched_valid[CK_COUNT];
	/* This frame's final rectangles (after clipping) and visibility. */
	float rect[CK_COUNT][4];
	uint8_t visible[CK_COUNT];
	char logged[512];
} s_ck;

static void ck_union(float acc[4], int* valid, const float b[4]) {
	if (!*valid) {
		memcpy(acc, b, sizeof(float) * 4);
		*valid = 1;
		return;
	}
	acc[0] = fminf(acc[0], b[0]);
	acc[1] = fminf(acc[1], b[1]);
	acc[2] = fmaxf(acc[2], b[2]);
	acc[3] = fmaxf(acc[3], b[3]);
}

static void ck_pose(const CkSpec* spec, float world_scale, AeronXrPanel* out) {
	const float deg = 3.14159265f / 180.0f;
	const float yaw = spec->yaw * deg, pitch = spec->pitch * deg;
	const float d = spec->distance / world_scale;
	/* LOCAL space: +X right, +Y up, -Z forward. */
	const float dir[3] = { sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch) };
	out->position[0] = dir[0] * d;
	out->position[1] = dir[1] * d;
	out->position[2] = dir[2] * d;
	/* The quad's +Z faces the eye point; its X stays level. */
	const float z[3] = { -dir[0], -dir[1], -dir[2] };
	float x[3] = { z[2], 0.0f, -z[0] }; /* (0,1,0) x z */
	const float xl = sqrtf(x[0] * x[0] + x[2] * x[2]);
	x[0] /= xl;
	x[2] /= xl;
	const float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
	/* Rotation matrix with columns x, y, z -> quaternion. */
	const float m00 = x[0], m01 = y[0], m02 = z[0];
	const float m10 = x[1], m11 = y[1], m12 = z[1];
	const float m20 = x[2], m21 = y[2], m22 = z[2];
	const float trace = m00 + m11 + m22;
	float qx, qy, qz, qw;
	if (trace > 0.0f) {
		const float t = sqrtf(trace + 1.0f) * 2.0f;
		qw = 0.25f * t;
		qx = (m21 - m12) / t;
		qy = (m02 - m20) / t;
		qz = (m10 - m01) / t;
	} else if (m00 > m11 && m00 > m22) {
		const float t = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
		qw = (m21 - m12) / t;
		qx = 0.25f * t;
		qy = (m01 + m10) / t;
		qz = (m02 + m20) / t;
	} else if (m11 > m22) {
		const float t = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
		qw = (m02 - m20) / t;
		qx = (m01 + m10) / t;
		qy = 0.25f * t;
		qz = (m12 + m21) / t;
	} else {
		const float t = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
		qw = (m10 - m01) / t;
		qx = (m02 + m20) / t;
		qy = (m12 + m21) / t;
		qz = 0.25f * t;
	}
	out->orientation[0] = qx;
	out->orientation[1] = qy;
	out->orientation[2] = qz;
	out->orientation[3] = qw;
}

static void ck_rotate(const float q[4], const float v[3], float out[3]) {
	const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
	const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
	const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
	out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
	out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
	out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

/* Speed | counters | clock: one row standing on the targeting computer's
 * top edge, in its plane, bottoms aligned. One text scale for all three,
 * as large as fits ROW_W. */
#define CK_ROW_W_M 0.34f
#define CK_ROW_GAP_M 0.008f
#define CK_ROW_MAX_M_PER_UNIT 0.0026f

static void ck_place_row(AeronXrPanel* out, const int* index_of, float world_scale, int target_h) {
	const int is = index_of[CK_SPEED], ic = index_of[CK_COUNTERS], ik = index_of[CK_CLOCK];
	if (is < 0 && ic < 0 && ik < 0) {
		return;
	}
	/* Counters centred, speed to their left, clock to their right: the
	 * row is symmetric, so the wider side decides the scale. */
	const float cw_px = ic >= 0 ? (float)out[ic].rect[2] : 0.0f;
	const float side_px = fmaxf(is >= 0 ? (float)out[is].rect[2] : 0.0f, ik >= 0 ? (float)out[ik].rect[2] : 0.0f);
	const float m_per_px = fminf(CK_ROW_MAX_M_PER_UNIT * 480.0f / (float)target_h,
								 (CK_ROW_W_M - 2.0f * CK_ROW_GAP_M) / fmaxf(1.0f, cw_px + 2.0f * side_px));
	AeronXrPanel cmd;
	memset(&cmd, 0, sizeof cmd);
	if (index_of[CK_CMD] >= 0) {
		cmd = out[index_of[CK_CMD]];
	} else {
		ck_pose(&k_spec[CK_CMD], world_scale, &cmd);
		cmd.size[0] = k_spec[CK_CMD].box_w / world_scale;
		cmd.size[1] = k_spec[CK_CMD].box_h / world_scale;
	}
	static const float ux[3] = { 1.0f, 0.0f, 0.0f }, uy[3] = { 0.0f, 1.0f, 0.0f };
	float ax[3], ay[3];
	ck_rotate(cmd.orientation, ux, ax);
	ck_rotate(cmd.orientation, uy, ay);
	const float half_c = 0.5f * cw_px * m_per_px + (ic >= 0 ? CK_ROW_GAP_M : 0.5f * CK_ROW_GAP_M);
	const int ids[3] = { is, ic, ik };
	for (int k = 0; k < 3; ++k) {
		if (ids[k] < 0) {
			continue;
		}
		AeronXrPanel* panel = &out[ids[k]];
		const float w = (float)panel->rect[2] * m_per_px, h = (float)panel->rect[3] * m_per_px;
		const float cx = k == 1 ? 0.0f : k == 0 ? -(half_c + 0.5f * w) : half_c + 0.5f * w;
		const float off_x = cx / world_scale;
		const float off_y = 0.5f * cmd.size[1] + (CK_ROW_GAP_M * 0.5f + 0.5f * h) / world_scale;
		for (int a = 0; a < 3; ++a) {
			panel->position[a] = cmd.position[a] + ax[a] * off_x + ay[a] * off_y;
		}
		memcpy(panel->orientation, cmd.orientation, sizeof panel->orientation);
		panel->size[0] = w / world_scale;
		panel->size[1] = h / world_scale;
	}
}

int XwaVrCockpitHud_Build(int target_w, int target_h, float world_scale, int cmd_separate, AeronXrPanel* out,
						  int max_out) {
	const XwaHudPreparedDrawState* drawn = XwaRemasterHud_PreparedDrawState();
	int count = 0;
	memset(s_ck.visible, 0, sizeof s_ck.visible);
	if (!out || max_out <= 0 || target_w <= 0 || target_h <= 0 || !drawn || !drawn->valid) {
		return 0;
	}
	if (world_scale <= 0.0f) {
		world_scale = 1.0f;
	}
	if (s_ck.target_w != target_w || s_ck.target_h != target_h ||
		s_ck.layout_generation != drawn->layout_generation || s_ck.bundle_generation != drawn->bundle_generation) {
		memset(s_ck.latched_valid, 0, sizeof s_ck.latched_valid);
		s_ck.target_w = target_w;
		s_ck.target_h = target_h;
		s_ck.layout_generation = drawn->layout_generation;
		s_ck.bundle_generation = drawn->bundle_generation;
	}

	/* What each panel shows this frame. */
	for (int p = 0; p < CK_COUNT; ++p) {
		float acc[4];
		int valid = 0;
		for (int i = 0; i < 5 && k_widgets[p][i] != XWA_HUD_WIDGET_COUNT; ++i) {
			float b[4];
			if (XwaRemasterHudFixed_WidgetBounds(k_widgets[p][i], b)) {
				ck_union(acc, &valid, b);
			}
		}
		for (int i = 0; i < 4 && k_panes[p][i]; ++i) {
			float b[4];
			/* What the text actually drew: the classic pane rectangles are
			 * often larger (empty corners) or smaller (clipped text). */
			if (XwaRemasterHudText_PaneGlyphBounds(k_panes[p][i], b)) {
				ck_union(acc, &valid, b);
			}
		}
		if (p == CK_CMD) {
			float b[4];
			if (XwaRemasterHudCmd_ViewBounds(b)) {
				ck_union(acc, &valid, b);
			}
		}
		if (valid) {
			int latched = s_ck.latched_valid[p];
			ck_union(s_ck.latched[p], &latched, acc);
			s_ck.latched_valid[p] = 1;
			s_ck.visible[p] = 1;
		}
	}

	/* Final rectangles: the classic top bar and bottom panel are drawn as
	 * joined pieces, so cut neighbours apart where they overlap. */
	for (int p = 0; p < CK_COUNT; ++p) {
		memcpy(s_ck.rect[p], s_ck.latched[p], sizeof s_ck.rect[p]);
	}
	if (s_ck.latched_valid[CK_COUNTERS]) {
		float* c = s_ck.rect[CK_COUNTERS];
		const float pad = 0.15f * (c[3] - c[1]);
		c[0] -= pad;
		c[1] -= pad;
		c[2] += pad;
		c[3] += pad;
	}
	/* The MFD frames reach into the targeting computer's area (unless the
	 * targeting computer is drawn to a target of its own). */
	if (s_ck.latched_valid[CK_CMD] && !cmd_separate) {
		const float* cmd = s_ck.rect[CK_CMD];
		if (s_ck.latched_valid[CK_MFD_LEFT] && s_ck.rect[CK_MFD_LEFT][2] > cmd[0] &&
			s_ck.rect[CK_MFD_LEFT][0] < cmd[0]) {
			s_ck.rect[CK_MFD_LEFT][2] = cmd[0];
		}
		if (s_ck.latched_valid[CK_MFD_RIGHT] && s_ck.rect[CK_MFD_RIGHT][0] < cmd[2] &&
			s_ck.rect[CK_MFD_RIGHT][2] > cmd[2]) {
			s_ck.rect[CK_MFD_RIGHT][0] = cmd[2];
		}
	}

	char log[512];
	int log_len = 0;
	int index_of[CK_COUNT];
	for (int p = 0; p < CK_COUNT; ++p) {
		index_of[p] = -1;
	}
	for (int p = 0; p < CK_COUNT && count < max_out; ++p) {
		float* r = s_ck.rect[p];
		const float pad = 4.0f;
		r[0] = fmaxf(0.0f, floorf(r[0] - pad));
		r[1] = fmaxf(0.0f, floorf(r[1] - pad));
		r[2] = fminf((float)target_w, ceilf(r[2] + pad));
		r[3] = fminf((float)target_h, ceilf(r[3] + pad));
		const float w = r[2] - r[0], h = r[3] - r[1];
		if (!s_ck.visible[p] || w < 4.0f || h < 4.0f) {
			s_ck.visible[p] = 0;
			continue;
		}
		index_of[p] = count;
		AeronXrPanel* panel = &out[count++];
		memset(panel, 0, sizeof *panel);
		panel->rect[0] = (int)r[0];
		panel->rect[1] = (int)r[1];
		panel->rect[2] = (int)w;
		panel->rect[3] = (int)h;
		panel->source = p == CK_CMD && cmd_separate ? 1 : 0;
		const CkSpec* spec = &k_spec[p];
		float size_w = spec->box_w, size_h = spec->box_w * h / w;
		if (spec->m_per_unit > 0.0f) {
			const float m_per_px = spec->m_per_unit * 480.0f / (float)target_h;
			size_w = w * m_per_px;
			size_h = h * m_per_px;
		} else if (size_h > spec->box_h) {
			size_h = spec->box_h;
			size_w = spec->box_h * w / h;
		}
		panel->size[0] = size_w / world_scale;
		panel->size[1] = size_h / world_scale;
		ck_pose(spec, world_scale, panel);
		if (log_len < (int)sizeof log - 64) {
			log_len += snprintf(log + log_len, sizeof log - (size_t)log_len, "%s%s %dx%d@%d,%d", log_len ? "; " : "",
								spec->name, (int)w, (int)h, (int)r[0], (int)r[1]);
		}
	}
	ck_place_row(out, index_of, world_scale, target_h);
	if (log_len && strcmp(log, s_ck.logged) != 0) {
		memcpy(s_ck.logged, log, sizeof s_ck.logged);
		Aeron_LogInfo("xwa.remaster", "VR cockpit HUD panels (%dx%d HUD): %s", target_w, target_h, log);
	}
	return count;
}

void XwaVrCockpitHud_Shutdown(void) {
	memset(&s_ck, 0, sizeof s_ck);
}

/* VR lap console; see vr_lap_console.h. */

#include "xwa_remaster/vr_lap_console.h"

#include "aeron/aeron.h"
#include "aeron/log.h"
#include "aeron/scene/draw_list2d.h"
#include "aeron/scene/font_atlas.h"
#include "aeron/scene/text2d.h"
#include "aeron/vfs.h"
#include "xwa_remaster/vr_gloves.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Placement -------------------------------------------------------- */

/* Default spot: straight ahead, 52 degrees below eye level, 0.52 m from
 * the eyes (about 41 cm down and 32 cm forward: over the lap, within easy
 * reach), turned to face the eyes. 51 x 17 cm. */
#define VC_PITCH_DEG -52.0f
#define VC_DISTANCE_M 0.52f
#define VC_WIDTH_M 0.51f
#define VC_HEIGHT_M 0.17f

/* Look-at wake: awake when the gaze comes within WAKE degrees of the
 * console's centre, asleep again beyond SLEEP. */
#define VC_WAKE_DEG 24.0f
#define VC_SLEEP_DEG 32.0f
#define VC_SLEEP_ALPHA 0.22f

/* Grab zones: the left and right ends of the console, GRAB_X metres to
 * either side of each edge, a little above/below and in front/behind. */
#define VC_GRAB_X_M 0.06f
#define VC_GRAB_Y_M 0.05f
#define VC_GRAB_FRONT_M 0.09f
#define VC_GRAB_BACK_M 0.07f
#define VC_GRIP_ON 0.6f
#define VC_GRIP_OFF 0.35f

/* Poke: the controller tip is TIP metres ahead of the aim pose; a press is
 * PRESS metres through the surface, released when back above RELEASE.
 * Deeper than BLOCK (or coming from behind) is not a press. */
#define VC_TIP_M 0.02f
#define VC_HOVER_M 0.12f
#define VC_PRESS_M 0.006f
#define VC_RELEASE_M 0.002f
#define VC_BLOCK_M 0.08f
#define VC_RAY_MAX_M 1.5f

#define VC_HOLD_S 0.7f
#define VC_EJECT_HOLD_S 1.0f
#define VC_FLASH_S 0.15f

#define VC_MAX_BUTTONS 64
#define VC_MAX_LABELS 24
#define VC_ACTION_QUEUE 32

typedef struct VcButton {
	float x, y, w, h;
	const char* label;
	const char* sub; /* small state line, or NULL */
	XwaVrConsoleAction action;
	int arg;
	/* Holding the button (VC_HOLD_S) does hold_action instead; hold_only
	 * buttons do nothing on a tap (EJECT). */
	XwaVrConsoleAction hold_action;
	int hold_only;
	int enabled;
	int lit;
	int danger;
	int level; /* level cell: >0 when the cell is at or below the current level */
} VcButton;

typedef struct VcLabel {
	float x, y;
	const char* text;
	int size;
	int align;
	int section;
	int dim;
} VcLabel;

typedef struct VcHand {
	int poke_down;
	int blocked;
	int trigger_down;
	int press_button;
	float press_time;
	int hold_fired;
	int press_by_ray;
	int cursor;      /* 0 none, 1 poke hover, 2 ray */
	float cx, cy;    /* cursor in console pixels */
	float depth;     /* poke: distance in front of the surface, metres */
	int over_button; /* button under the cursor, -1 none */
	int captured;
} VcHand;

static struct {
	float dt;
	int drawn;
	XwaVrConsoleState state;
	int awake;
	float alpha;

	VcButton buttons[VC_MAX_BUTTONS];
	int button_count;
	VcLabel labels[VC_MAX_LABELS];
	int label_count;
	float flash[VC_MAX_BUTTONS];

	VcHand hands[2];

	XwaVrConsoleAction queue_action[VC_ACTION_QUEUE];
	int queue_arg[VC_ACTION_QUEUE];
	int queue_head, queue_count;

	AeronDrawList2D* list;
	AeronFontAtlas font;
	int font_state; /* 0 not tried, 1 loaded, -1 failed */
	float divider_y; /* horizontal rule above the stacked sections */
	/* This frame's eye point and placement, for cutting the gloves out. */
	float eye[3];
	AeronXrPanel pose;
	float ax[3], ay[3], az[3];
	int logged;
	/* Placement moved by hand (XWA_XR_CONSOLE_POSE), LOCAL space. */
	int custom;
	float cpos[3], cq[4];
	char pose_env[160];
	/* Grabbing: the hand, the side it holds, and the console relative to
	 * the hand. */
	int grab_hand; /* hand + 1, 0 = not held */
	int grab_side;
	float grab_pos[3], grab_q[4];
	int hover_side[2];
	float grip_prev[2];
	int placement_dirty;
	char placement[160];
} s_vc;

/* ---- Small math ------------------------------------------------------- */

static void vc_rotate(const float q[4], const float v[3], float out[3]) {
	const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
	const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
	const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
	out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
	out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
	out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

static float vc_dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void vc_qmul(const float a[4], const float b[4], float out[4]) {
	const float r[4] = {
		a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
		a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
		a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
		a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
	};
	memcpy(out, r, sizeof r);
}

static void vc_qnorm(float q[4]) {
	const float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (l > 1e-6f) {
		for (int k = 0; k < 4; ++k) {
			q[k] /= l;
		}
	} else {
		q[0] = q[1] = q[2] = 0.0f;
		q[3] = 1.0f;
	}
}

static float vc_env(const char* name, float fallback) {
	const char* v = getenv(name);
	return v && v[0] ? (float)atof(v) : fallback;
}

/* The hand-placed pose from XWA_XR_CONSOLE_POSE ("x,y,z,qx,qy,qz,qw";
 * empty = the default spot), read again whenever it changes. */
static void vc_read_pose_env(void) {
	const char* v = getenv("XWA_XR_CONSOLE_POSE");
	if (!v) {
		v = "";
	}
	if (strcmp(v, s_vc.pose_env) == 0) {
		return;
	}
	snprintf(s_vc.pose_env, sizeof s_vc.pose_env, "%s", v);
	float p[3], q[4];
	s_vc.custom = sscanf(v, "%f,%f,%f,%f,%f,%f,%f", &p[0], &p[1], &p[2], &q[0], &q[1], &q[2], &q[3]) == 7 &&
				  fabsf(p[0]) < 2.0f && fabsf(p[1]) < 2.0f && fabsf(p[2]) < 2.0f;
	if (s_vc.custom) {
		memcpy(s_vc.cpos, p, sizeof p);
		vc_qnorm(q);
		memcpy(s_vc.cq, q, sizeof q);
	}
}

/* Quad facing the LOCAL origin (the seated eye point), X kept level; or
 * where the player put it. */
static void vc_pose(AeronXrPanel* out, float axis_x[3], float axis_y[3], float axis_z[3]) {
	const float deg = 3.14159265f / 180.0f;
	if (s_vc.custom) {
		static const float ux[3] = { 1, 0, 0 }, uy[3] = { 0, 1, 0 }, uz[3] = { 0, 0, 1 };
		memcpy(out->position, s_vc.cpos, sizeof s_vc.cpos);
		memcpy(out->orientation, s_vc.cq, sizeof s_vc.cq);
		vc_rotate(s_vc.cq, ux, axis_x);
		vc_rotate(s_vc.cq, uy, axis_y);
		vc_rotate(s_vc.cq, uz, axis_z);
		out->size[0] = VC_WIDTH_M;
		out->size[1] = VC_HEIGHT_M;
		return;
	}
	const float pitch = vc_env("XWA_XR_CONSOLE_PITCH", VC_PITCH_DEG) * deg;
	const float d = vc_env("XWA_XR_CONSOLE_DISTANCE", VC_DISTANCE_M);
	const float dir[3] = { 0.0f, sinf(pitch), -cosf(pitch) };
	out->position[0] = dir[0] * d;
	out->position[1] = dir[1] * d;
	out->position[2] = dir[2] * d;
	const float z[3] = { -dir[0], -dir[1], -dir[2] };
	const float x[3] = { 1.0f, 0.0f, 0.0f };
	const float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
	memcpy(axis_x, x, sizeof x);
	memcpy(axis_y, y, sizeof y);
	memcpy(axis_z, z, sizeof z);
	/* Rotation about X only: columns x, y, z. */
	const float trace = x[0] + y[1] + z[2];
	const float t = sqrtf(trace + 1.0f) * 2.0f;
	out->orientation[3] = 0.25f * t;
	out->orientation[0] = (y[2] - z[1]) / t;
	out->orientation[1] = (z[0] - x[2]) / t;
	out->orientation[2] = (x[1] - y[0]) / t;
	out->size[0] = VC_WIDTH_M;
	out->size[1] = VC_HEIGHT_M;
}

/* ---- Actions ---------------------------------------------------------- */

static void vc_push(XwaVrConsoleAction action, int arg) {
	if (action == XWA_VRC_NONE || s_vc.queue_count >= VC_ACTION_QUEUE) {
		return;
	}
	const int i = (s_vc.queue_head + s_vc.queue_count) % VC_ACTION_QUEUE;
	s_vc.queue_action[i] = action;
	s_vc.queue_arg[i] = arg;
	s_vc.queue_count++;
}

int XwaVrConsole_PollAction(XwaVrConsoleAction* action, int* arg) {
	if (s_vc.queue_count <= 0) {
		return 0;
	}
	if (action) {
		*action = s_vc.queue_action[s_vc.queue_head];
	}
	if (arg) {
		*arg = s_vc.queue_arg[s_vc.queue_head];
	}
	s_vc.queue_head = (s_vc.queue_head + 1) % VC_ACTION_QUEUE;
	s_vc.queue_count--;
	return 1;
}

/* ---- Layout ----------------------------------------------------------- */

static VcButton* vc_button(float x, float y, float w, float h, const char* label, XwaVrConsoleAction action,
						   int arg) {
	if (s_vc.button_count >= VC_MAX_BUTTONS) {
		static VcButton spare;
		memset(&spare, 0, sizeof spare);
		return &spare;
	}
	VcButton* b = &s_vc.buttons[s_vc.button_count++];
	memset(b, 0, sizeof *b);
	b->x = x;
	b->y = y;
	b->w = w;
	b->h = h;
	b->label = label;
	b->action = action;
	b->arg = arg;
	b->enabled = 1;
	return b;
}

static void vc_label(float x, float y, const char* text, int size, int align, int section, int dim) {
	if (s_vc.label_count >= VC_MAX_LABELS) {
		return;
	}
	VcLabel* l = &s_vc.labels[s_vc.label_count++];
	l->x = x;
	l->y = y;
	l->text = text;
	l->size = size;
	l->align = align;
	l->section = section;
	l->dim = dim;
}

/* A row of equal buttons across [x, x + w). */
static void vc_row(float x, float y, float w, float h, int n, const char* const* labels,
				   const XwaVrConsoleAction* actions, VcButton** out) {
	const float gap = 10.0f;
	const float bw = (w - gap * (float)(n - 1)) / (float)n;
	for (int i = 0; i < n; ++i) {
		VcButton* b = vc_button(x + (bw + gap) * (float)i, y, bw, h, labels[i], actions[i], 0);
		if (out) {
			out[i] = b;
		}
	}
}

/* Five level cells (0 off .. 4 full) after a row label. */
static void vc_levels(float x, float y, float w, float h, const char* name, XwaVrConsoleAction action, int level,
					  int enabled) {
	static const char* const cells[5] = { "OFF", "LOW", "NRM", "HI", "MAX" };
	const float label_w = 128.0f;
	const float gap = 8.0f;
	const float cw = (w - label_w - gap * 4.0f) / 5.0f;
	vc_label(x, y + h * 0.5f - 12.0f, name, 22, 0, 0, !enabled);
	for (int i = 0; i < 5; ++i) {
		VcButton* b = vc_button(x + label_w + (cw + gap) * (float)i, y, cw, h, cells[i], action, i);
		b->enabled = enabled;
		b->level = enabled && level >= i && level > 0 ? 1 : 0;
		b->lit = enabled && level == i;
	}
}

#define VC_W 1536.0f
#define VC_H 512.0f

static void vc_layout(void) {
	const XwaVrConsoleState* st = &s_vc.state;
	VcButton* row[4];
	s_vc.button_count = 0;
	s_vc.label_count = 0;

	/* Energy */
	{
		const float x = 34.0f, w = 470.0f;
		vc_label(x, 22.0f, "ENERGY", 28, 0, 1, 0);
		vc_levels(x, 64.0f, w, 52.0f, "LASERS", XWA_VRC_LASER_LEVEL, st->laser_level, 1);
		vc_levels(x, 124.0f, w, 52.0f, "SHIELDS", XWA_VRC_SHIELD_LEVEL, st->shield_level, st->has_shields);
		vc_levels(x, 184.0f, w, 52.0f, "BEAM", XWA_VRC_BEAM_LEVEL, st->beam_level, st->has_beam);
		vc_label(x, 250.0f, "SHIELD BALANCE", 20, 0, 0, !st->has_shields);
		{
			static const char* const l[3] = { "FRONT", "EVEN", "REAR" };
			static const XwaVrConsoleAction a[3] = { XWA_VRC_SHIELD_BALANCE, XWA_VRC_SHIELD_BALANCE,
													 XWA_VRC_SHIELD_BALANCE };
			vc_row(x, 276.0f, w, 54.0f, 3, l, a, row);
			for (int i = 0; i < 3; ++i) {
				row[i]->arg = i;
				row[i]->enabled = st->has_shields;
				row[i]->lit = st->has_shields && st->shield_balance == i;
			}
		}
		{
			static const char* const l[2] = { "LASERS > SHIELDS", "SHIELDS > LASERS" };
			static const XwaVrConsoleAction a[2] = { XWA_VRC_LASERS_TO_SHIELDS, XWA_VRC_SHIELDS_TO_LASERS };
			vc_row(x, 340.0f, w, 54.0f, 2, l, a, row);
			row[0]->enabled = row[1]->enabled = st->has_shields;
		}
		{
			static const char* const l[2] = { "PRESET 1", "PRESET 2" };
			static const XwaVrConsoleAction a[2] = { XWA_VRC_PRESET_RECALL, XWA_VRC_PRESET_RECALL };
			vc_row(x, 404.0f, w, 54.0f, 2, l, a, row);
			for (int i = 0; i < 2; ++i) {
				row[i]->arg = i;
				row[i]->hold_action = XWA_VRC_PRESET_STORE;
				row[i]->sub = "hold = store";
			}
		}
	}

	/* Targeting and weapons */
	{
		const float x = 538.0f, w = 460.0f;
		vc_label(x, 22.0f, "TARGET COMPUTER", 28, 0, 1, 0);
		{
			static const char* const l[3] = { "LOCK NEAREST\nENEMY", "LOCK ENEMY\nATTACKING ME", "MATCH SPEED" };
			static const XwaVrConsoleAction a[3] = { XWA_VRC_TARGET_NEAREST_ENEMY, XWA_VRC_TARGET_ATTACKER,
													 XWA_VRC_MATCH_SPEED };
			vc_row(x, 62.0f, w, 66.0f, 3, l, a, row);
			row[2]->enabled = st->has_target;
		}
		{
			static const char* const l[3] = { "FRIENDLY", "NEUTRAL", "ENEMY" };
			static const XwaVrConsoleAction a[3] = { XWA_VRC_TARGET_FRIENDLY, XWA_VRC_TARGET_NEUTRAL,
													 XWA_VRC_TARGET_ENEMY };
			vc_row(x, 136.0f, w, 48.0f, 3, l, a, NULL);
		}
		{
			VcButton* prev = vc_button(x, 192.0f, 70.0f, 48.0f, "<", XWA_VRC_SUBSYSTEM_PREV, 0);
			VcButton* next = vc_button(x + w - 70.0f, 192.0f, 70.0f, 48.0f, ">", XWA_VRC_SUBSYSTEM_NEXT, 0);
			prev->enabled = next->enabled = st->has_target;
			vc_label(x + w * 0.5f, 202.0f, "SUBSYSTEM", 22, 1, 0, !st->has_target);
		}
		{
			vc_label(x, 251.0f, "STORE", 20, 0, 0, 0);
			vc_label(x, 273.0f, "TARGET", 20, 0, 0, 0);
			static const char* const l[3] = { "1", "2", "3" };
			static const XwaVrConsoleAction a[3] = { XWA_VRC_MEMORY_RECALL, XWA_VRC_MEMORY_RECALL,
													 XWA_VRC_MEMORY_RECALL };
			vc_row(x + 110.0f, 248.0f, w - 110.0f, 48.0f, 3, l, a, row);
			for (int i = 0; i < 3; ++i) {
				row[i]->arg = i;
				row[i]->hold_action = XWA_VRC_MEMORY_STORE;
				row[i]->lit = st->memory_set[i];
				row[i]->sub = st->memory_set[i] ? "set" : "hold = store";
			}
		}
		{
			static const char* const l[3] = { "NEWEST", "OBJECTIVE", "CLEAR" };
			static const XwaVrConsoleAction a[3] = { XWA_VRC_TARGET_NEWEST, XWA_VRC_TARGET_OBJECTIVE,
													 XWA_VRC_TARGET_CLEAR };
			vc_row(x, 304.0f, w, 48.0f, 3, l, a, row);
			row[2]->enabled = st->has_target;
		}
		/* Weapons: its own section under the targeting computer. */
		{
			s_vc.divider_y = 366.0f;
			vc_label(x, 376.0f, "WEAPONS", 28, 0, 1, 0);
			static const char* const l[3] = { "FIRE-LINK", "CONVERGE", "S-FOILS" };
			static const XwaVrConsoleAction a[3] = { XWA_VRC_FIRE_LINK, XWA_VRC_CONVERGENCE, XWA_VRC_SFOILS };
			vc_row(x, 412.0f, w, 58.0f, 3, l, a, row);
			row[2]->enabled = st->has_sfoils;
			row[2]->sub = !st->has_sfoils ? NULL : st->sfoils_closed ? "closed" : "open (attack)";
			row[2]->lit = st->has_sfoils && !st->sfoils_closed;
		}
	}

	/* Wingmen and ship */
	{
		const float x = 1032.0f, w = 470.0f;
		vc_label(x, 22.0f, "WINGMEN", 28, 0, 1, 0);
		{
			static const char* const l[2] = { "ATTACK MY TARGET", "COVER ME" };
			static const XwaVrConsoleAction a[2] = { XWA_VRC_WING_ATTACK, XWA_VRC_WING_COVER };
			vc_row(x, 64.0f, w, 66.0f, 2, l, a, row);
			row[0]->enabled = st->has_target;
		}
		{
			static const char* const l[4] = { "IGNORE", "EVADE", "WAIT", "GO AHEAD" };
			static const XwaVrConsoleAction a[4] = { XWA_VRC_WING_IGNORE, XWA_VRC_WING_EVADE, XWA_VRC_WING_WAIT,
													 XWA_VRC_WING_GO_AHEAD };
			vc_row(x, 140.0f, w, 52.0f, 4, l, a, row);
			row[0]->enabled = st->has_target;
		}
		{
			static const char* const l[4] = { "HOME", "REPORT", "RESUPPLY", "REINFORCE" };
			static const XwaVrConsoleAction a[4] = { XWA_VRC_WING_HOME, XWA_VRC_WING_REPORT, XWA_VRC_WING_RESUPPLY,
													 XWA_VRC_WING_REINFORCE };
			vc_row(x, 202.0f, w, 52.0f, 4, l, a, NULL);
		}
		{
			vc_label(x, 286.0f, "SHIP", 28, 0, 1, 0);
			static const char* const l[4] = { "MAP", "DOCK", "PICK UP", "RELEASE" };
			static const XwaVrConsoleAction a[4] = { XWA_VRC_MAP, XWA_VRC_DOCK, XWA_VRC_PICKUP, XWA_VRC_RELEASE };
			vc_row(x, 326.0f, w, 52.0f, 4, l, a, NULL);
		}
		{
			static const char* const l[4] = { "GUNNER", "AUTO-GUN", "FLARE", "EJECT" };
			static const XwaVrConsoleAction a[4] = { XWA_VRC_GUNNER, XWA_VRC_AUTO_GUNNER, XWA_VRC_COUNTERMEASURE,
													 XWA_VRC_NONE };
			vc_row(x, 388.0f, w, 52.0f, 4, l, a, row);
			row[3]->hold_action = XWA_VRC_EJECT;
			row[3]->hold_only = 1;
			row[3]->danger = 1;
			row[3]->sub = "hold";
		}
	}
}

static int vc_button_at(float px, float py) {
	for (int i = 0; i < s_vc.button_count; ++i) {
		const VcButton* b = &s_vc.buttons[i];
		if (px >= b->x && px < b->x + b->w && py >= b->y && py < b->y + b->h) {
			return i;
		}
	}
	return -1;
}

/* ---- Pressing --------------------------------------------------------- */

static void vc_press(int hand, int button, int by_ray) {
	VcHand* h = &s_vc.hands[hand];
	h->press_button = button;
	h->press_time = 0.0f;
	h->hold_fired = 0;
	h->press_by_ray = by_ray;
	if (button < 0) {
		return;
	}
	const VcButton* b = &s_vc.buttons[button];
	if (!b->enabled) {
		Aeron_XrHapticPulse(hand, 0.15f, 15);
		h->press_button = -1;
		return;
	}
	Aeron_XrHapticPulse(hand, 0.45f, 25);
	s_vc.flash[button] = VC_FLASH_S;
	if (b->hold_action == XWA_VRC_NONE) {
		vc_push(b->action, b->arg);
	}
}

static void vc_hold_tick(int hand) {
	VcHand* h = &s_vc.hands[hand];
	if (h->press_button < 0 || h->hold_fired) {
		return;
	}
	const VcButton* b = &s_vc.buttons[h->press_button];
	if (b->hold_action == XWA_VRC_NONE) {
		return;
	}
	h->press_time += s_vc.dt;
	if (h->press_time >= (b->hold_only ? VC_EJECT_HOLD_S : VC_HOLD_S)) {
		h->hold_fired = 1;
		Aeron_XrHapticPulse(hand, 0.9f, 60);
		s_vc.flash[h->press_button] = VC_FLASH_S * 2.0f;
		vc_push(b->hold_action, b->arg);
	}
}

static void vc_release(int hand) {
	VcHand* h = &s_vc.hands[hand];
	if (h->press_button >= 0) {
		const VcButton* b = &s_vc.buttons[h->press_button];
		if (b->hold_action != XWA_VRC_NONE && !h->hold_fired && !b->hold_only) {
			vc_push(b->action, b->arg); /* a tap */
		}
	}
	h->press_button = -1;
	h->hold_fired = 0;
}

static void vc_update_hand(int hand, const AeronXrPanel* pose, const float ax[3], const float ay[3],
						   const float az[3]) {
	VcHand* h = &s_vc.hands[hand];
	AeronXrHand xh;
	const int was_down = h->poke_down || h->trigger_down;
	h->cursor = 0;
	h->over_button = -1;
	if (!s_vc.awake || s_vc.grab_hand == hand + 1 || !Aeron_XrGetHand(hand, &xh)) {
		if (was_down) {
			vc_release(hand);
		}
		h->poke_down = h->trigger_down = 0;
		h->blocked = 0;
		h->captured = 0;
		return;
	}
	static const float minus_z[3] = { 0.0f, 0.0f, -1.0f };
	float fwd[3];
	vc_rotate(xh.orientation, minus_z, fwd);
	float tip[3] = { xh.position[0] + fwd[0] * VC_TIP_M, xh.position[1] + fwd[1] * VC_TIP_M,
					 xh.position[2] + fwd[2] * VC_TIP_M };
	/* With the gloves the glove's index fingertip pokes, and only while
	 * the finger is lifted off the trigger (pointing). */
	int can_poke = 1;
	{
		int pointing = 1;
		if (XwaVrGloves_IndexTip(hand, tip, &pointing)) {
			can_poke = pointing;
		}
	}
	const float rel[3] = { tip[0] - pose->position[0], tip[1] - pose->position[1], tip[2] - pose->position[2] };
	const float lx = vc_dot(rel, ax), ly = vc_dot(rel, ay), depth = vc_dot(rel, az);
	const float px = (lx / VC_WIDTH_M + 0.5f) * VC_W;
	const float py = (0.5f - ly / VC_HEIGHT_M) * VC_H;
	const int inside = px >= 0.0f && px < VC_W && py >= 0.0f && py < VC_H;

	/* Poke. */
	if (h->poke_down) {
		if (depth > -VC_RELEASE_M || !inside) {
			h->poke_down = 0;
			vc_release(hand);
		}
	} else if (inside && depth < -VC_PRESS_M && !h->blocked && !h->trigger_down && can_poke) {
		if (depth < -VC_BLOCK_M) {
			h->blocked = 1; /* arrived from behind or far too deep */
		} else {
			h->poke_down = 1;
			vc_press(hand, vc_button_at(px, py), 0);
		}
	}
	if (h->blocked && (depth > 0.0f || !inside)) {
		h->blocked = 0;
	}
	const int poking = can_poke && inside && depth < VC_HOVER_M && depth > -VC_BLOCK_M;
	if (poking) {
		h->cursor = 1;
		h->cx = px;
		h->cy = py;
		h->depth = depth;
	}

	/* Ray (when not poking). */
	int ray_hit = 0;
	if (!poking && !h->poke_down) {
		const float from[3] = { xh.position[0] - pose->position[0], xh.position[1] - pose->position[1],
								xh.position[2] - pose->position[2] };
		const float d0 = vc_dot(from, az);
		const float dn = vc_dot(fwd, az);
		if (d0 > 0.0f && dn < -1.0e-3f) {
			const float t = -d0 / dn;
			if (t > 0.0f && t < VC_RAY_MAX_M) {
				const float hit[3] = { from[0] + fwd[0] * t, from[1] + fwd[1] * t, from[2] + fwd[2] * t };
				const float rx = (vc_dot(hit, ax) / VC_WIDTH_M + 0.5f) * VC_W;
				const float ry = (0.5f - vc_dot(hit, ay) / VC_HEIGHT_M) * VC_H;
				if (rx >= 0.0f && rx < VC_W && ry >= 0.0f && ry < VC_H) {
					ray_hit = 1;
					h->cursor = 2;
					h->cx = rx;
					h->cy = ry;
				}
			}
		}
	}
	/* The trigger belongs to the console while the ray is on it (and until
	 * a pull that started there is released). */
	h->captured = ray_hit || h->trigger_down;
	if (h->trigger_down) {
		if (xh.trigger < 0.35f) {
			h->trigger_down = 0;
			vc_release(hand);
		}
	} else if (ray_hit && xh.trigger >= 0.6f) {
		h->trigger_down = 1;
		vc_press(hand, vc_button_at(h->cx, h->cy), 1);
	}
	if (h->cursor) {
		h->over_button = vc_button_at(h->cx, h->cy);
	}
	vc_hold_tick(hand);
}

/* ---- Grabbing --------------------------------------------------------- */

static void vc_grab_end(void) {
	if (!s_vc.grab_hand) {
		return;
	}
	Aeron_XrHapticPulse(s_vc.grab_hand - 1, 0.3f, 20);
	s_vc.grab_hand = 0;
	s_vc.grab_side = 0;
	snprintf(s_vc.placement, sizeof s_vc.placement, "%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f", s_vc.cpos[0],
			 s_vc.cpos[1], s_vc.cpos[2], s_vc.cq[0], s_vc.cq[1], s_vc.cq[2], s_vc.cq[3]);
	setenv("XWA_XR_CONSOLE_POSE", s_vc.placement, 1);
	snprintf(s_vc.pose_env, sizeof s_vc.pose_env, "%s", s_vc.placement);
	s_vc.placement_dirty = 1;
	Aeron_LogInfo("xwa.remaster", "lap console moved to %s", s_vc.placement);
}

/* Squeezing the grip at either end of the console picks it up; it follows
 * the hand freely (position and turn) until the grip is let go. */
static void vc_update_grab(const AeronXrPanel* pose, const float ax[3], const float ay[3], const float az[3]) {
	for (int hand = 0; hand < 2; ++hand) {
		AeronXrHand xh;
		s_vc.hover_side[hand] = 0;
		if (!Aeron_XrGetHand(hand, &xh)) {
			if (s_vc.grab_hand == hand + 1) {
				vc_grab_end();
			}
			s_vc.grip_prev[hand] = 0.0f;
			Aeron_XrCaptureGrip(hand, 0);
			continue;
		}
		const float rel[3] = { xh.position[0] - pose->position[0], xh.position[1] - pose->position[1],
							   xh.position[2] - pose->position[2] };
		const float lx = vc_dot(rel, ax), ly = vc_dot(rel, ay), depth = vc_dot(rel, az);
		const float edge = fabsf(lx) - 0.5f * VC_WIDTH_M;
		const int in_zone = edge > -VC_GRAB_X_M && edge < VC_GRAB_X_M && fabsf(ly) < 0.5f * VC_HEIGHT_M + VC_GRAB_Y_M &&
							depth < VC_GRAB_FRONT_M && depth > -VC_GRAB_BACK_M;
		if (in_zone) {
			s_vc.hover_side[hand] = lx < 0.0f ? -1 : 1;
		}
		if (s_vc.grab_hand == hand + 1) {
			if (xh.grip < VC_GRIP_OFF) {
				vc_grab_end();
			} else {
				/* console = hand * relative */
				float off[3];
				vc_rotate(xh.orientation, s_vc.grab_pos, off);
				for (int k = 0; k < 3; ++k) {
					s_vc.cpos[k] = xh.position[k] + off[k];
				}
				vc_qmul(xh.orientation, s_vc.grab_q, s_vc.cq);
				vc_qnorm(s_vc.cq);
				s_vc.custom = 1;
			}
		} else if (!s_vc.grab_hand && in_zone && xh.grip >= VC_GRIP_ON && s_vc.grip_prev[hand] < VC_GRIP_OFF) {
			/* relative = inverse(hand) * console */
			const float inv[4] = { -xh.orientation[0], -xh.orientation[1], -xh.orientation[2], xh.orientation[3] };
			const float to_console[3] = { -rel[0], -rel[1], -rel[2] };
			vc_rotate(inv, to_console, s_vc.grab_pos);
			vc_qmul(inv, pose->orientation, s_vc.grab_q);
			vc_qnorm(s_vc.grab_q);
			memcpy(s_vc.cpos, pose->position, sizeof s_vc.cpos);
			memcpy(s_vc.cq, pose->orientation, sizeof s_vc.cq);
			vc_qnorm(s_vc.cq);
			s_vc.custom = 1;
			s_vc.grab_hand = hand + 1;
			s_vc.grab_side = lx < 0.0f ? -1 : 1;
			Aeron_XrHapticPulse(hand, 0.6f, 30);
		}
		s_vc.grip_prev[hand] = xh.grip;
		/* The grip at a grab zone belongs to the console, not the game. */
		Aeron_XrCaptureGrip(hand, in_zone || s_vc.grab_hand == hand + 1);
	}
}

int XwaVrConsole_TakePlacement(char* out, int size) {
	if (!s_vc.placement_dirty || !out || size <= 0) {
		return 0;
	}
	snprintf(out, (size_t)size, "%s", s_vc.placement);
	s_vc.placement_dirty = 0;
	return 1;
}

/* ---- Drawing ---------------------------------------------------------- */

static void vc_pma(float out[4], float r, float g, float b, float a) {
	a *= s_vc.alpha;
	out[0] = r * a;
	out[1] = g * a;
	out[2] = b * a;
	out[3] = a;
}

static void vc_rrect(float x, float y, float w, float h, float radius, float border_px, float soft,
					 const float fill[4], const float border[4]) {
	AeronDrawList2DRRect r;
	memset(&r, 0, sizeof r);
	r.dst_x = x;
	r.dst_y = y;
	r.dst_w = w;
	r.dst_h = h;
	r.radius_px = radius;
	r.border_px = border_px;
	r.soft_px = soft;
	memcpy(r.fill_top, fill, sizeof r.fill_top);
	memcpy(r.fill_bottom, fill, sizeof r.fill_bottom);
	if (border) {
		memcpy(r.border, border, sizeof r.border);
	}
	AeronDrawList_AddRRect(s_vc.list, &r);
}

static void vc_text(float x, float y, const char* text, float size_px, int align, float r, float g, float b,
					float a) {
	if (s_vc.font_state != 1 || !text || !text[0]) {
		return;
	}
	AeronTextStyle style;
	memset(&style, 0, sizeof style);
	style.font = &s_vc.font;
	style.scale = size_px / (float)s_vc.font.cell_h;
	style.tint[0] = r;
	style.tint[1] = g;
	style.tint[2] = b;
	style.tint[3] = a * s_vc.alpha;
	style.filter = AERON_BLIT2D_FILTER_LINEAR;
	AeronText_Draw(s_vc.list, &style, x, y, (AeronTextAlign)align, text, -1, NULL);
}

/* Holo cyan from the console design. */
#define VC_CYAN 0.498f, 0.910f, 1.0f
#define VC_AMBER 1.0f, 0.710f, 0.278f

static void vc_draw_button(int i) {
	const VcButton* b = &s_vc.buttons[i];
	int hover = 0, pressed = 0;
	for (int h = 0; h < 2; ++h) {
		hover |= s_vc.hands[h].over_button == i;
		pressed |= s_vc.hands[h].press_button == i;
	}
	const float flash = s_vc.flash[i] > 0.0f ? 1.0f : 0.0f;
	float fill[4], border[4];
	if (!b->enabled) {
		vc_pma(fill, 0.42f, 0.48f, 0.50f, 0.05f);
		vc_pma(border, 0.42f, 0.48f, 0.50f, 0.35f);
	} else if (b->danger) {
		const float hold = pressed && b->hold_action != XWA_VRC_NONE
							   ? fminf(1.0f, s_vc.hands[0].press_button == i ? s_vc.hands[0].press_time / VC_EJECT_HOLD_S
																			  : s_vc.hands[1].press_time / VC_EJECT_HOLD_S)
							   : 0.0f;
		vc_pma(fill, VC_AMBER, 0.12f + 0.45f * hold + 0.1f * hover);
		vc_pma(border, VC_AMBER, 0.85f);
	} else {
		const float on = b->lit ? 0.30f : b->level ? 0.16f : 0.08f;
		vc_pma(fill, VC_CYAN, on + 0.10f * hover + 0.25f * (pressed || flash));
		vc_pma(border, VC_CYAN, b->lit ? 1.0f : 0.62f + 0.3f * hover);
	}
	if (b->enabled && (b->lit || hover || pressed)) {
		float glow[4];
		const float transparent[4] = { 0, 0, 0, 0 };
		vc_pma(glow, VC_CYAN, b->lit ? 0.45f : 0.3f);
		vc_rrect(b->x - 6.0f, b->y - 6.0f, b->w + 12.0f, b->h + 12.0f, 12.0f, 6.0f, 10.0f, transparent, glow);
	}
	vc_rrect(b->x, b->y, b->w, b->h, 7.0f, 2.0f, 1.0f, fill, border);

	const float tr = !b->enabled ? 0.45f : b->danger ? 1.0f : 0.85f;
	const float tg = !b->enabled ? 0.52f : b->danger ? 0.92f : 0.97f;
	const float tb = !b->enabled ? 0.55f : b->danger ? 0.80f : 1.0f;
	const float text_a = b->lit || pressed ? 1.0f : 0.9f;
	float size = b->h >= 60.0f ? 22.0f : 20.0f;
	/* "LINE ONE\nLINE TWO": two centred lines. */
	char line1[48], line2[48];
	const char* nl = strchr(b->label, '\n');
	line2[0] = '\0';
	if (nl) {
		snprintf(line1, sizeof line1, "%.*s", (int)(nl - b->label), b->label);
		snprintf(line2, sizeof line2, "%s", nl + 1);
	} else {
		snprintf(line1, sizeof line1, "%s", b->label);
	}
	if (s_vc.font_state == 1) {
		/* Shrink long labels to fit. */
		AeronTextStyle probe;
		memset(&probe, 0, sizeof probe);
		probe.font = &s_vc.font;
		probe.scale = size / (float)s_vc.font.cell_h;
		const float width = fmaxf(AeronText_MeasureWidth(&probe, line1, -1), AeronText_MeasureWidth(&probe, line2, -1));
		if (width > b->w - 14.0f && width > 0.0f) {
			size *= (b->w - 14.0f) / width;
		}
	}
	if (line2[0]) {
		vc_text(b->x + b->w * 0.5f, b->y + b->h * 0.5f - size * 1.05f, line1, size, 1, tr, tg, tb, text_a);
		vc_text(b->x + b->w * 0.5f, b->y + b->h * 0.5f + size * 0.02f, line2, size, 1, tr, tg, tb, text_a);
	} else if (b->sub) {
		vc_text(b->x + b->w * 0.5f, b->y + b->h * 0.5f - size * 0.95f, b->label, size, 1, tr, tg, tb, text_a);
		vc_text(b->x + b->w * 0.5f, b->y + b->h * 0.5f + 2.0f, b->sub, 15.0f, 1, tr, tg, tb, 0.65f);
	} else {
		vc_text(b->x + b->w * 0.5f, b->y + b->h * 0.5f - size * 0.55f, b->label, size, 1, tr, tg, tb, text_a);
	}
}

/* Point in LOCAL space -> console pixels, seen from the eye: the point
 * projected onto the console plane. Returns the scale (projected size per
 * metre at the point), or 0 when behind the eye. */
static float vc_project(const float p[3], float* px, float* py) {
	const float* c = s_vc.pose.position;
	const float ep[3] = { p[0] - s_vc.eye[0], p[1] - s_vc.eye[1], p[2] - s_vc.eye[2] };
	const float ec[3] = { c[0] - s_vc.eye[0], c[1] - s_vc.eye[1], c[2] - s_vc.eye[2] };
	const float dp = vc_dot(ep, s_vc.az), dc = vc_dot(ec, s_vc.az);
	if (dp > -1e-4f) {
		return 0.0f; /* not in front of the eye */
	}
	const float t = dc / dp;
	const float hit[3] = { ep[0] * t - ec[0], ep[1] * t - ec[1], ep[2] * t - ec[2] };
	*px = (vc_dot(hit, s_vc.ax) / VC_WIDTH_M + 0.5f) * VC_W;
	*py = (0.5f - vc_dot(hit, s_vc.ay) / VC_HEIGHT_M) * VC_H;
	return t;
}

static void vc_cut_capsule(const XwaVrGloveCapsule* cap) {
	static const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float a[3], b[3];
	memcpy(a, cap->a, sizeof a);
	memcpy(b, cap->b, sizeof b);
	/* Height above the console plane (towards the eye). */
	const float ra[3] = { a[0] - s_vc.pose.position[0], a[1] - s_vc.pose.position[1], a[2] - s_vc.pose.position[2] };
	const float rb[3] = { b[0] - s_vc.pose.position[0], b[1] - s_vc.pose.position[1], b[2] - s_vc.pose.position[2] };
	float ha = vc_dot(ra, s_vc.az) + cap->radius, hb = vc_dot(rb, s_vc.az) + cap->radius;
	if (ha <= 0.0f && hb <= 0.0f) {
		return; /* behind the console: the console covers it, correctly */
	}
	if (ha <= 0.0f || hb <= 0.0f) {
		/* Keep the part in front. */
		const float t = ha / (ha - hb);
		float m[3];
		for (int k = 0; k < 3; ++k) {
			m[k] = a[k] + (b[k] - a[k]) * t;
		}
		if (ha <= 0.0f) {
			memcpy(a, m, sizeof a);
		} else {
			memcpy(b, m, sizeof b);
		}
	}
	float x0, y0, x1, y1;
	const float sa = vc_project(a, &x0, &y0), sb = vc_project(b, &x1, &y1);
	if (sa <= 0.0f || sb <= 0.0f) {
		return;
	}
	const float px_per_m = VC_W / VC_WIDTH_M;
	const float r = cap->radius * 0.5f * (sa + sb) * px_per_m;
	/* Extend by the radius at both ends so the caps are covered. */
	float dx = x1 - x0, dy = y1 - y0;
	const float len = sqrtf(dx * dx + dy * dy);
	if (len > 1e-3f) {
		dx /= len;
		dy /= len;
	} else {
		dx = 1.0f;
		dy = 0.0f;
	}
	AeronDrawList_AddLine(s_vc.list, x0 - dx * r * 0.8f, y0 - dy * r * 0.8f, x1 + dx * r * 0.8f, y1 + dy * r * 0.8f,
						  2.0f * r, zero, AERON_BLIT2D_BLEND_NONE, NULL);
}

static void vc_draw(void) {
	const float transparent[4] = { 0, 0, 0, 0 };
	float fill[4], border[4], glow[4];
	/* Plate: faint tint, bright rim, soft halo. */
	vc_pma(glow, VC_CYAN, 0.25f);
	vc_rrect(4.0f, 4.0f, VC_W - 8.0f, VC_H - 8.0f, 30.0f, 8.0f, 14.0f, transparent, glow);
	vc_pma(fill, 0.157f, 0.549f, 0.745f, 0.10f);
	vc_pma(border, VC_CYAN, 0.6f);
	vc_rrect(10.0f, 10.0f, VC_W - 20.0f, VC_H - 20.0f, 26.0f, 2.0f, 1.0f, fill, border);
	/* Grab handles: thick frame bars at both ends, bright while a hand is
	 * at them or holds them. */
	for (int side = -1; side <= 1; side += 2) {
		int hot = s_vc.grab_side == side;
		for (int h = 0; h < 2; ++h) {
			hot |= s_vc.hover_side[h] == side;
		}
		const float bar_w = 16.0f;
		const float x = side < 0 ? 9.0f : VC_W - 9.0f - bar_w;
		float bar[4], rim[4];
		vc_pma(bar, VC_CYAN, s_vc.grab_side == side ? 0.85f : hot ? 0.6f : 0.32f);
		vc_pma(rim, VC_CYAN, hot ? 1.0f : 0.75f);
		vc_rrect(x, 52.0f, bar_w, VC_H - 104.0f, 8.0f, 2.0f, 1.0f, bar, rim);
		for (int r = -2; r <= 2; ++r) {
			float ridge[4];
			vc_pma(ridge, 0.0f, 0.0f, 0.0f, 0.35f);
			AeronDrawList_AddFill(s_vc.list, x + 3.0f, VC_H * 0.5f + (float)r * 22.0f - 1.5f, bar_w - 6.0f, 3.0f,
								  ridge, AERON_BLIT2D_BLEND_PMA, NULL);
		}
	}
	/* Scan lines, fixed to the panel. */
	for (float y = 14.0f; y < VC_H - 14.0f; y += 6.0f) {
		float line[4];
		vc_pma(line, VC_CYAN, 0.025f);
		AeronDrawList_AddFill(s_vc.list, 14.0f, y, VC_W - 28.0f, 2.0f, line, AERON_BLIT2D_BLEND_PMA, NULL);
	}
	/* Column dividers. */
	for (int i = 0; i < 2; ++i) {
		float line[4];
		vc_pma(line, VC_CYAN, 0.35f);
		const float x = i == 0 ? 520.0f : 1015.0f;
		AeronDrawList_AddFill(s_vc.list, x, 40.0f, 2.0f, VC_H - 80.0f, line, AERON_BLIT2D_BLEND_PMA, NULL);
	}
	/* Rules between the stacked sections (targeting / weapons, wingmen /
	 * ship). */
	{
		float line[4];
		vc_pma(line, VC_CYAN, 0.35f);
		AeronDrawList_AddFill(s_vc.list, 538.0f, s_vc.divider_y, 460.0f, 2.0f, line, AERON_BLIT2D_BLEND_PMA, NULL);
		AeronDrawList_AddFill(s_vc.list, 1032.0f, 274.0f, 470.0f, 2.0f, line, AERON_BLIT2D_BLEND_PMA, NULL);
	}
	for (int i = 0; i < s_vc.label_count; ++i) {
		const VcLabel* l = &s_vc.labels[i];
		if (l->section) {
			vc_text(l->x, l->y, l->text, (float)l->size, l->align, VC_CYAN, 1.0f);
		} else if (l->dim) {
			vc_text(l->x, l->y, l->text, (float)l->size, l->align, 0.42f, 0.48f, 0.50f, 0.8f);
		} else {
			vc_text(l->x, l->y, l->text, (float)l->size, l->align, 0.62f, 0.85f, 0.92f, 0.95f);
		}
	}
	for (int i = 0; i < s_vc.button_count; ++i) {
		vc_draw_button(i);
	}
	/* The console is a layer drawn over the 3D view: wherever a glove is
	 * between the eye and the console, cut the console away so the hand
	 * shows in front of it. */
	for (int h = 0; h < 2; ++h) {
		XwaVrGloveCapsule caps[24];
		const int n = XwaVrGloves_Capsules(h, caps, 24);
		for (int c = 0; c < n; ++c) {
			vc_cut_capsule(&caps[c]);
		}
	}
	/* Cursors: a ring that closes in as the tip nears the surface, or a
	 * dot where the ray lands. */
	for (int h = 0; h < 2; ++h) {
		const VcHand* hand = &s_vc.hands[h];
		if (!hand->cursor) {
			continue;
		}
		if (hand->cursor == 1) {
			const float r = 7.0f + fmaxf(0.0f, hand->depth) * 260.0f;
			float c[4];
			vc_pma(c, 1.0f, 1.0f, 1.0f, hand->poke_down ? 1.0f : 0.85f);
			vc_rrect(hand->cx - r, hand->cy - r, 2.0f * r, 2.0f * r, r, hand->poke_down ? r : 3.0f, 1.5f,
					 transparent, c);
		} else {
			float c[4], ring[4];
			vc_pma(c, 1.0f, 1.0f, 1.0f, 0.95f);
			vc_pma(ring, VC_CYAN, 0.9f);
			vc_rrect(hand->cx - 9.0f, hand->cy - 9.0f, 18.0f, 18.0f, 9.0f, 3.0f, 1.5f, c, ring);
		}
	}
}

/* ---- Frame ------------------------------------------------------------ */

static int vc_ensure(void);
static int vc_enabled(void);

void XwaVrConsole_BeginFrame(float dt_seconds) {
	s_vc.dt = dt_seconds > 0.0f && dt_seconds < 0.25f ? dt_seconds : 0.0f;
	s_vc.drawn = 0;
	/* Load the font here, between frames, rather than while the flight
	 * renderer is recording its command buffer. */
	if (s_vc.state.valid && s_vc.font_state == 0 && vc_enabled() && Aeron_XrIsPresenting()) {
		vc_ensure();
	}
}

void XwaVrConsole_SetState(const XwaVrConsoleState* state) {
	if (state) {
		s_vc.state = *state;
	} else {
		memset(&s_vc.state, 0, sizeof s_vc.state);
	}
}

static int vc_enabled(void) {
	const char* env = getenv("XWA_XR_CONSOLE");
	return !(env && env[0] == '0');
}

static int vc_ensure(void) {
	if (!s_vc.list) {
		s_vc.list = AeronDrawList_Create(0);
		if (!s_vc.list) {
			return 0;
		}
		for (int h = 0; h < 2; ++h) {
			s_vc.hands[h].press_button = -1;
		}
	}
	if (s_vc.font_state == 0) {
		AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
		s_vc.font_state = -1;
		if (cmd) {
			if (AeronFontAtlas_LoadVfs(&s_vc.font, cmd, Aeron_GetVfs(), AERON_VFS_ROOT_RESOURCE, "fonts/setup",
									   4u * 1024u * 1024u)) {
				if (Aeron_SubmitCommandBuffer(cmd)) {
					s_vc.font_state = 1;
				} else {
					AeronFontAtlas_Release(&s_vc.font);
				}
			} else {
				Aeron_CancelCommandBuffer(cmd);
			}
		}
		if (s_vc.font_state != 1) {
			Aeron_LogWarn("xwa.remaster", "lap console: font unavailable, labels are not drawn");
		}
	}
	return 1;
}

int XwaVrConsole_Frame(AeronCommandBuffer* cmd, AeronRenderTarget* target, int width, int height,
					   const AeronXrView views[2], AeronXrPanel* out) {
	if (!cmd || !target || width <= 0 || height <= 0 || !views || !out || !s_vc.state.valid || !vc_enabled() ||
		s_vc.font_state == 0 || !vc_ensure()) {
		return 0; /* the font loads in BeginFrame first */
	}
	s_vc.drawn = 1;
	if (!s_vc.logged) {
		Aeron_LogInfo("xwa.remaster", "lap console: %dx%d layer, %.0f deg down, %.2f m", width, height,
					  (double)vc_env("XWA_XR_CONSOLE_PITCH", VC_PITCH_DEG),
					  (double)vc_env("XWA_XR_CONSOLE_DISTANCE", VC_DISTANCE_M));
		s_vc.logged = 1;
	}

	memset(out, 0, sizeof *out);
	float ax[3], ay[3], az[3];
	if (!s_vc.grab_hand) {
		vc_read_pose_env();
	}
	vc_pose(out, ax, ay, az);
	vc_update_grab(out, ax, ay, az);
	vc_pose(out, ax, ay, az); /* moved by the grab */
	for (int k = 0; k < 3; ++k) {
		s_vc.eye[k] = 0.5f * (views[0].position[k] + views[1].position[k]);
	}

	/* Wake when the player looks at it (or reaches for its handles). */
	{
		static const float minus_z[3] = { 0.0f, 0.0f, -1.0f };
		float f[3];
		vc_rotate(views[0].orientation, minus_z, f);
		float to[3] = { out->position[0] - s_vc.eye[0], out->position[1] - s_vc.eye[1],
						out->position[2] - s_vc.eye[2] };
		const float len = sqrtf(vc_dot(to, to));
		const float c = len > 1e-4f ? vc_dot(f, to) / len : 1.0f;
		const float angle = acosf(fmaxf(-1.0f, fminf(1.0f, c))) * 180.0f / 3.14159265f;
		const int handled = s_vc.grab_hand || s_vc.hover_side[0] || s_vc.hover_side[1];
		if (handled) {
			s_vc.awake = 1;
		} else if (s_vc.awake && angle > VC_SLEEP_DEG) {
			s_vc.awake = 0;
		} else if (!s_vc.awake && angle < VC_WAKE_DEG) {
			s_vc.awake = 1;
		}
		const float goal = s_vc.awake ? 1.0f : VC_SLEEP_ALPHA;
		const float step = s_vc.dt / 0.2f;
		s_vc.alpha = s_vc.alpha < goal ? fminf(goal, s_vc.alpha + step) : fmaxf(goal, s_vc.alpha - step);
		if (s_vc.dt <= 0.0f) {
			s_vc.alpha = goal;
		}
	}

	out->rect[0] = 0;
	out->rect[1] = 0;
	out->rect[2] = width;
	out->rect[3] = height;

	s_vc.pose = *out;
	memcpy(s_vc.ax, ax, sizeof ax);
	memcpy(s_vc.ay, ay, sizeof ay);
	memcpy(s_vc.az, az, sizeof az);
	vc_layout();
	for (int i = 0; i < VC_MAX_BUTTONS; ++i) {
		if (s_vc.flash[i] > 0.0f) {
			s_vc.flash[i] -= s_vc.dt;
		}
	}
	for (int h = 0; h < 2; ++h) {
		vc_update_hand(h, out, ax, ay, az);
		Aeron_XrCaptureTrigger(h, s_vc.hands[h].captured);
	}

	AeronDrawList_Begin(s_vc.list, target, width, height, AERON_DRAWLIST2D_CLEAR, NULL);
	vc_draw();
	AeronDrawList_Render(s_vc.list, cmd);
	return 1;
}

void XwaVrConsole_EndFrame(void) {
	if (!s_vc.drawn) {
		for (int h = 0; h < 2; ++h) {
			if (s_vc.hands[h].captured || s_vc.hands[h].poke_down || s_vc.hands[h].trigger_down) {
				vc_release(h);
			}
			s_vc.hands[h].captured = 0;
			s_vc.hands[h].poke_down = 0;
			s_vc.hands[h].trigger_down = 0;
			s_vc.hands[h].cursor = 0;
			Aeron_XrCaptureTrigger(h, 0);
			Aeron_XrCaptureGrip(h, 0);
			s_vc.hover_side[h] = 0;
		}
		vc_grab_end();
		s_vc.awake = 0;
	}
}

void XwaVrConsole_Shutdown(void) {
	if (s_vc.list) {
		AeronDrawList_Destroy(s_vc.list);
	}
	if (s_vc.font_state == 1) {
		AeronFontAtlas_Release(&s_vc.font);
	}
	memset(&s_vc, 0, sizeof s_vc);
}

/* VR lap console; see vr_lap_console.h. */

#include "xwa_remaster/vr_lap_console.h"

#include "aeron/aeron.h"
#include "aeron/log.h"
#include "aeron/scene/draw_list2d.h"
#include "aeron/scene/font_atlas.h"
#include "aeron/scene/text2d.h"
#include "aeron/vfs.h"
#include "xwa_remaster/vr_gloves.h"
#include "xwa_remaster/vr_flight_stick.h"

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
#define VC_WAKE_DEG 32.0f
#define VC_SLEEP_DEG 40.0f
#define VC_SLEEP_ALPHA 0.22f

/* Grab zones: the left and right ends of the console, GRAB_X metres to
 * either side of each edge, a little above/below and in front/behind. */
#define VC_GRAB_X_M 0.06f
#define VC_GRAB_Y_M 0.05f
#define VC_GRAB_FRONT_M 0.09f
#define VC_GRAB_BACK_M 0.07f
/* Button card: a reference of the controller layout, 40 x 30 cm, that
 * appears at every mission start to the right, just out of view. */
#define VC_CARD_W_M 0.40f
#define VC_CARD_H_M 0.30f
#define VC_CARD_YAW_DEG 80.0f
#define VC_CARD_PITCH_DEG -12.0f
#define VC_CARD_DISTANCE_M 0.60f
#define VC_CARD_WAKE_DEG 30.0f
#define VC_CARD_SLEEP_DEG 38.0f

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
	int hyper; /* the HYPERSPACE button: pulses orange-red while the jump is ready */
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
	float ray_start[3], ray_end[3]; /* LOCAL, while the ray is on the console */
	int captured;
} VcHand;

/* A panel the player can pick up by its ends and put anywhere. */
typedef struct VcPlace {
	int custom; /* placed by hand (else the default spot) */
	float cpos[3], cq[4];
	int grab_hand; /* hand + 1, 0 = not held */
	int grab_side;
	float grab_pos[3], grab_q[4]; /* the panel relative to the holding hand */
	int hover_side[2];
	float grip_prev[2];
} VcPlace;

/* Reference cards beside the player: the button layout on the right, the
 * voice commands on the left. */
enum { VC_CARD_BUTTONS = 0, VC_CARD_VOICE = 1, VC_CARDS = 2 };
typedef struct VcCard {
	VcPlace place;
	AeronDrawList2D* list[2]; /* left eye (or both), right eye */
	int awake;
	float alpha;
	unsigned int mission;
	int drawn;
	int logged;
	uint64_t hash; /* what the layer's image shows */
	int hash_valid;
} VcCard;

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
	/* Placements moved by hand, LOCAL space: the lap console (saved as
	 * XWA_XR_CONSOLE_POSE) and the button card (back on the right at every
	 * mission start). */
	VcPlace lap;
	VcCard cards[VC_CARDS];
	char pose_env[160];
	int placement_dirty;
	char placement[160];
	/* The panel being drawn: its list and its size in metres and pixels
	 * (for the glove cut-outs), and how many cut-outs it got. */
	AeronDrawList2D* cur_list;
	const VcPlace* cur_place;
	float cur_w_m, cur_h_m, cur_w_px, cur_h_px;
	int cut_count;
	/* Drawing the panel's cached content (no cut-outs, no cursors), or the
	 * per-eye overlay drawn on top of it each frame. */
	int overlay_phase;
	/* The lap console's right-eye list and what its layer shows. */
	AeronDrawList2D* list_r;
	uint64_t hash;
	int hash_valid;
	/* Grips the panels own this frame (handles reached), applied in
	 * EndFrame. */
	int grip_capture[2];
	float clock; /* seconds, for pulsing */
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
	s_vc.lap.custom = sscanf(v, "%f,%f,%f,%f,%f,%f,%f", &p[0], &p[1], &p[2], &q[0], &q[1], &q[2], &q[3]) == 7 &&
					  fabsf(p[0]) < 2.0f && fabsf(p[1]) < 2.0f && fabsf(p[2]) < 2.0f;
	if (s_vc.lap.custom) {
		memcpy(s_vc.lap.cpos, p, sizeof p);
		vc_qnorm(q);
		memcpy(s_vc.lap.cq, q, sizeof q);
	}
}

/* Where the player put a panel: 1 with its pose and axes. */
static int vc_place_pose(const VcPlace* pl, float w, float h, AeronXrPanel* out, float axis_x[3], float axis_y[3],
						 float axis_z[3]) {
	static const float ux[3] = { 1, 0, 0 }, uy[3] = { 0, 1, 0 }, uz[3] = { 0, 0, 1 };
	if (!pl->custom) {
		return 0;
	}
	memcpy(out->position, pl->cpos, sizeof pl->cpos);
	memcpy(out->orientation, pl->cq, sizeof pl->cq);
	vc_rotate(pl->cq, ux, axis_x);
	vc_rotate(pl->cq, uy, axis_y);
	vc_rotate(pl->cq, uz, axis_z);
	out->size[0] = w;
	out->size[1] = h;
	return 1;
}

/* Rotation with columns x, y, z -> quaternion. */
static void vc_basis_quat(const float x[3], const float y[3], const float z[3], float q[4]) {
	const float trace = x[0] + y[1] + z[2];
	if (trace > 0.0f) {
		const float t = sqrtf(trace + 1.0f) * 2.0f;
		q[3] = 0.25f * t;
		q[0] = (y[2] - z[1]) / t;
		q[1] = (z[0] - x[2]) / t;
		q[2] = (x[1] - y[0]) / t;
	} else if (x[0] > y[1] && x[0] > z[2]) {
		const float t = sqrtf(1.0f + x[0] - y[1] - z[2]) * 2.0f;
		q[3] = (y[2] - z[1]) / t;
		q[0] = 0.25f * t;
		q[1] = (y[0] + x[1]) / t;
		q[2] = (z[0] + x[2]) / t;
	} else if (y[1] > z[2]) {
		const float t = sqrtf(1.0f + y[1] - x[0] - z[2]) * 2.0f;
		q[3] = (z[0] - x[2]) / t;
		q[0] = (y[0] + x[1]) / t;
		q[1] = 0.25f * t;
		q[2] = (z[1] + y[2]) / t;
	} else {
		const float t = sqrtf(1.0f + z[2] - x[0] - y[1]) * 2.0f;
		q[3] = (x[1] - y[0]) / t;
		q[0] = (z[0] + x[2]) / t;
		q[1] = (z[1] + y[2]) / t;
		q[2] = 0.25f * t;
	}
}

/* A quad `d` metres from the seated eye point at yaw (+ right) / pitch
 * (+ up) degrees, facing it, X kept level. */
static void vc_facing_pose(float yaw_deg, float pitch_deg, float d, float w, float h, AeronXrPanel* out,
						   float axis_x[3], float axis_y[3], float axis_z[3]) {
	const float deg = 3.14159265f / 180.0f;
	const float yaw = yaw_deg * deg, pitch = pitch_deg * deg;
	const float dir[3] = { sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch) };
	for (int k = 0; k < 3; ++k) {
		out->position[k] = dir[k] * d;
	}
	const float z[3] = { -dir[0], -dir[1], -dir[2] };
	float x[3] = { z[2], 0.0f, -z[0] };
	const float xl = sqrtf(x[0] * x[0] + x[2] * x[2]);
	x[0] /= xl;
	x[2] /= xl;
	const float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
	memcpy(axis_x, x, sizeof x);
	memcpy(axis_y, y, sizeof y);
	memcpy(axis_z, z, sizeof z);
	vc_basis_quat(x, y, z, out->orientation);
	out->size[0] = w;
	out->size[1] = h;
}

/* Quad facing the LOCAL origin (the seated eye point), X kept level; or
 * where the player put it. */
static void vc_pose(AeronXrPanel* out, float axis_x[3], float axis_y[3], float axis_z[3]) {
	const float deg = 3.14159265f / 180.0f;
	if (vc_place_pose(&s_vc.lap, VC_WIDTH_M, VC_HEIGHT_M, out, axis_x, axis_y, axis_z)) {
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
		/* Rows 404..458: the energy flow gauge (vc_draw_energy). */
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
			static const char* const l[4] = { "GUNNER", "AUTO-GUN", "FLARE", "HYPERSPACE" };
			static const XwaVrConsoleAction a[4] = { XWA_VRC_GUNNER, XWA_VRC_AUTO_GUNNER, XWA_VRC_COUNTERMEASURE,
													 XWA_VRC_HYPERSPACE };
			vc_row(x, 388.0f, w, 52.0f, 4, l, a, row);
			row[3]->hyper = 1;
			row[3]->sub = st->hyper_ready ? "ready" : "at a buoy";
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
	if (!s_vc.awake || s_vc.lap.grab_hand == hand + 1 || !Aeron_XrGetHand(hand, &xh)) {
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
					for (int k = 0; k < 3; ++k) {
						h->ray_start[k] = tip[k];
						h->ray_end[k] = pose->position[k] + hit[k];
					}
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

static void vc_grab_end(VcPlace* pl) {
	if (!pl->grab_hand) {
		return;
	}
	Aeron_XrHapticPulse(pl->grab_hand - 1, 0.3f, 20);
	pl->grab_hand = 0;
	pl->grab_side = 0;
	if (pl != &s_vc.lap) {
		return;
	}
	snprintf(s_vc.placement, sizeof s_vc.placement, "%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f", pl->cpos[0], pl->cpos[1],
			 pl->cpos[2], pl->cq[0], pl->cq[1], pl->cq[2], pl->cq[3]);
	setenv("XWA_XR_CONSOLE_POSE", s_vc.placement, 1);
	snprintf(s_vc.pose_env, sizeof s_vc.pose_env, "%s", s_vc.placement);
	s_vc.placement_dirty = 1;
	Aeron_LogInfo("xwa.remaster", "lap console moved to %s", s_vc.placement);
}

/* Whether `hand` holds a panel other than `pl`. */
static int vc_hand_holds_other(const VcPlace* pl, int hand) {
	if (pl != &s_vc.lap && s_vc.lap.grab_hand == hand + 1) {
		return 1;
	}
	for (int c = 0; c < VC_CARDS; ++c) {
		if (pl != &s_vc.cards[c].place && s_vc.cards[c].place.grab_hand == hand + 1) {
			return 1;
		}
	}
	return 0;
}

/* Squeezing the grip at either end of a panel (w x h metres) picks it up;
 * it follows the hand freely (position and turn) until the grip is let go.
 * A hand holding another panel does not pick this one up. */
static void vc_update_grab(VcPlace* pl, const AeronXrPanel* pose, const float ax[3], const float ay[3],
						   const float az[3], float w, float h) {
	for (int hand = 0; hand < 2; ++hand) {
		AeronXrHand xh;
		pl->hover_side[hand] = 0;
		if (!Aeron_XrGetHand(hand, &xh)) {
			if (pl->grab_hand == hand + 1) {
				vc_grab_end(pl);
			}
			pl->grip_prev[hand] = 0.0f;
			continue;
		}
		const float rel[3] = { xh.position[0] - pose->position[0], xh.position[1] - pose->position[1],
							   xh.position[2] - pose->position[2] };
		const float lx = vc_dot(rel, ax), ly = vc_dot(rel, ay), depth = vc_dot(rel, az);
		const float edge = fabsf(lx) - 0.5f * w;
		const int in_zone = edge > -VC_GRAB_X_M && edge < VC_GRAB_X_M && fabsf(ly) < 0.5f * h + VC_GRAB_Y_M &&
							depth < VC_GRAB_FRONT_M && depth > -VC_GRAB_BACK_M &&
							!vc_hand_holds_other(pl, hand);
		if (in_zone) {
			pl->hover_side[hand] = lx < 0.0f ? -1 : 1;
		}
		if (pl->grab_hand == hand + 1) {
			if (xh.grip < VC_GRIP_OFF) {
				vc_grab_end(pl);
			} else {
				/* panel = hand * relative */
				float off[3];
				vc_rotate(xh.orientation, pl->grab_pos, off);
				for (int k = 0; k < 3; ++k) {
					pl->cpos[k] = xh.position[k] + off[k];
				}
				vc_qmul(xh.orientation, pl->grab_q, pl->cq);
				vc_qnorm(pl->cq);
				pl->custom = 1;
			}
		} else if (!pl->grab_hand && in_zone && xh.grip >= VC_GRIP_ON && pl->grip_prev[hand] < VC_GRIP_OFF) {
			/* relative = inverse(hand) * panel */
			const float inv[4] = { -xh.orientation[0], -xh.orientation[1], -xh.orientation[2], xh.orientation[3] };
			const float to_panel[3] = { -rel[0], -rel[1], -rel[2] };
			vc_rotate(inv, to_panel, pl->grab_pos);
			vc_qmul(inv, pose->orientation, pl->grab_q);
			vc_qnorm(pl->grab_q);
			memcpy(pl->cpos, pose->position, sizeof pl->cpos);
			memcpy(pl->cq, pose->orientation, sizeof pl->cq);
			vc_qnorm(pl->cq);
			pl->custom = 1;
			pl->grab_hand = hand + 1;
			pl->grab_side = lx < 0.0f ? -1 : 1;
			Aeron_XrHapticPulse(hand, 0.6f, 30);
		}
		pl->grip_prev[hand] = xh.grip;
		/* The grip at a grab zone belongs to the panel, not the game. */
		if (in_zone || pl->grab_hand == hand + 1) {
			s_vc.grip_capture[hand] = 1;
		}
	}
}

void XwaVrConsole_CaptureGrip(int hand) {
	if (hand >= 0 && hand < 2) {
		s_vc.grip_capture[hand] = 1;
	}
}

int XwaVrConsole_RayBeam(int hand, float start[3], float end[3]) {
	if (hand < 0 || hand > 1 || !s_vc.drawn || !s_vc.awake || s_vc.hands[hand].cursor != 2) {
		return 0;
	}
	memcpy(start, s_vc.hands[hand].ray_start, sizeof s_vc.hands[hand].ray_start);
	memcpy(end, s_vc.hands[hand].ray_end, sizeof s_vc.hands[hand].ray_end);
	return 1;
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
	AeronDrawList_AddRRect(s_vc.cur_list, &r);
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
	AeronText_Draw(s_vc.cur_list, &style, x, y, (AeronTextAlign)align, text, -1, NULL);
}

/* Holo cyan from the console design. */
#define VC_CYAN 0.498f, 0.910f, 1.0f
#define VC_AMBER 1.0f, 0.710f, 0.278f
/* Hyperspace ready: orange-red. */
#define VC_HYPER 1.0f, 0.42f, 0.16f

static void vc_draw_button(int i) {
	const VcButton* b = &s_vc.buttons[i];
	int hover = 0, pressed = 0;
	for (int h = 0; h < 2; ++h) {
		hover |= s_vc.hands[h].over_button == i;
		pressed |= s_vc.hands[h].press_button == i;
	}
	const float flash = s_vc.flash[i] > 0.0f ? 1.0f : 0.0f;
	float fill[4], border[4];
	const int hyper_ready = b->hyper && s_vc.state.hyper_ready;
	/* Ready to jump: a slow orange-red pulse (about one per 1.2 s). */
	const float pulse = hyper_ready ? 0.5f + 0.5f * sinf(s_vc.clock * 2.0f * 3.14159265f / 1.2f) : 0.0f;
	if (!b->enabled) {
		vc_pma(fill, 0.42f, 0.48f, 0.50f, 0.05f);
		vc_pma(border, 0.42f, 0.48f, 0.50f, 0.35f);
	} else if (hyper_ready) {
		vc_pma(fill, VC_HYPER, 0.22f + 0.38f * pulse + 0.1f * hover + 0.2f * (pressed || flash));
		vc_pma(border, VC_HYPER, 0.75f + 0.25f * pulse);
	} else if (b->danger) {
		const float hold = pressed && b->hold_action != XWA_VRC_NONE
							   ? fminf(1.0f, s_vc.hands[0].press_button == i ? s_vc.hands[0].press_time / VC_EJECT_HOLD_S
																			  : s_vc.hands[1].press_time / VC_EJECT_HOLD_S)
							   : 0.0f;
		vc_pma(fill, VC_AMBER, 0.12f + 0.45f * hold + 0.1f * hover);
		vc_pma(border, VC_AMBER, 0.85f);
	} else {
		const float on = b->lit ? 0.30f : b->level ? 0.16f : 0.08f;
		vc_pma(fill, VC_CYAN, on + 0.22f * hover + 0.25f * (pressed || flash));
		vc_pma(border, hover ? 1.0f : 0.498f, hover ? 1.0f : 0.910f, 1.0f, b->lit || hover ? 1.0f : 0.62f);
	}
	if (hyper_ready) {
		float glow[4];
		const float transparent[4] = { 0, 0, 0, 0 };
		vc_pma(glow, VC_HYPER, 0.25f + 0.5f * pulse);
		vc_rrect(b->x - 8.0f, b->y - 8.0f, b->w + 16.0f, b->h + 16.0f, 13.0f, 8.0f, 14.0f, transparent, glow);
	} else if (b->enabled && (b->lit || hover || pressed)) {
		float glow[4];
		const float transparent[4] = { 0, 0, 0, 0 };
		vc_pma(glow, VC_CYAN, b->lit ? 0.45f : hover ? 0.55f : 0.3f);
		vc_rrect(b->x - 6.0f, b->y - 6.0f, b->w + 12.0f, b->h + 12.0f, 12.0f, 6.0f, 10.0f, transparent, glow);
	}
	vc_rrect(b->x, b->y, b->w, b->h, 7.0f, 2.0f, 1.0f, fill, border);

	const float tr = !b->enabled ? 0.45f : hyper_ready ? 1.0f : b->danger ? 1.0f : 0.85f;
	const float tg = !b->enabled ? 0.52f : hyper_ready ? 0.90f : b->danger ? 0.92f : 0.97f;
	const float tb = !b->enabled ? 0.55f : hyper_ready ? 0.78f : b->danger ? 0.80f : 1.0f;
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
	*px = (vc_dot(hit, s_vc.ax) / s_vc.cur_w_m + 0.5f) * s_vc.cur_w_px;
	*py = (0.5f - vc_dot(hit, s_vc.ay) / s_vc.cur_h_m) * s_vc.cur_h_px;
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
	const float px_per_m = s_vc.cur_w_px / s_vc.cur_w_m;
	/* A little inside the glove's outline, so no gap shows around it (the
	 * cut is made from each eye: the hole matches what that eye sees). */
	const float r = 0.9f * cap->radius * 0.5f * (sa + sb) * px_per_m;
	if (fmaxf(x0, x1) + r < 0.0f || fminf(x0, x1) - r > s_vc.cur_w_px || fmaxf(y0, y1) + r < 0.0f ||
		fminf(y0, y1) - r > s_vc.cur_h_px) {
		return; /* not over this panel */
	}
	s_vc.cut_count++;
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
	AeronDrawList_AddLine(s_vc.cur_list, x0 - dx * r * 0.8f, y0 - dy * r * 0.8f, x1 + dx * r * 0.8f, y1 + dy * r * 0.8f,
						  2.0f * r, zero, AERON_BLIT2D_BLEND_NONE, NULL);
}

/* Grab handles: thick frame bars at both ends of the current panel, bright
 * while a hand is at them or holds them. */
static void vc_draw_handles(const VcPlace* pl) {
	const float W = s_vc.cur_w_px, H = s_vc.cur_h_px;
	for (int side = -1; side <= 1; side += 2) {
		int hot = pl->grab_side == side;
		for (int h = 0; h < 2; ++h) {
			hot |= pl->hover_side[h] == side;
		}
		const float bar_w = 16.0f;
		const float x = side < 0 ? 9.0f : W - 9.0f - bar_w;
		float bar[4], rim[4];
		vc_pma(bar, VC_CYAN, pl->grab_side == side ? 0.85f : hot ? 0.6f : 0.32f);
		vc_pma(rim, VC_CYAN, hot ? 1.0f : 0.75f);
		vc_rrect(x, 52.0f, bar_w, H - 104.0f, 8.0f, 2.0f, 1.0f, bar, rim);
		for (int r = -2; r <= 2; ++r) {
			float ridge[4];
			vc_pma(ridge, 0.0f, 0.0f, 0.0f, 0.35f);
			AeronDrawList_AddFill(s_vc.cur_list, x + 3.0f, H * 0.5f + (float)r * 22.0f - 1.5f, bar_w - 6.0f, 3.0f,
								  ridge, AERON_BLIT2D_BLEND_PMA, NULL);
		}
	}
}

/* ---- Glove silhouettes ------------------------------------------------
 * The exact outline of each glove, as seen from the eye the list is drawn
 * for: its skinned triangles (the part in front of the panel) projected
 * onto the panel and rasterized into a small mask, which erases the panel
 * there. The hole then matches the rendered hand, with no gap around it.
 */
enum { VC_MASK = 320, VC_MASK_MAX_VERTS = 16384 };

static struct {
	AeronCommandBuffer* cmd; /* for the mask uploads */
	int eye;                 /* list being drawn: 0 left, 1 right */
	int masks;               /* masks drawn into the current panel */
	AeronTexture* tex[2][2]; /* [eye][hand] */
	uint8_t px[VC_MASK * VC_MASK * 4];
	float sx[VC_MASK_MAX_VERTS], sy[VC_MASK_MAX_VERTS], h[VC_MASK_MAX_VERTS];
	uint8_t facing[VC_MASK_MAX_VERTS];
} s_mask;

/* One triangle (mask texel space) into the mask, coverage at texel centres
 * (either winding). */
static void vc_mask_tri(int mw, int mh, float x0, float y0, float x1, float y1, float x2, float y2) {
	const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
	if (fabsf(area) < 1e-6f) {
		return;
	}
	if (area < 0.0f) {
		float t = x1;
		x1 = x2;
		x2 = t;
		t = y1;
		y1 = y2;
		y2 = t;
	}
	int ix0 = (int)fminf(x0, fminf(x1, x2)), iy0 = (int)fminf(y0, fminf(y1, y2));
	int ix1 = (int)fmaxf(x0, fmaxf(x1, x2)) + 1, iy1 = (int)fmaxf(y0, fmaxf(y1, y2)) + 1;
	ix0 = ix0 < 0 ? 0 : ix0;
	iy0 = iy0 < 0 ? 0 : iy0;
	ix1 = ix1 > mw ? mw : ix1;
	iy1 = iy1 > mh ? mh : iy1;
	if (ix0 >= ix1 || iy0 >= iy1) {
		return;
	}
	/* Edge functions E = a*x + b*y + c, stepped incrementally. */
	const float a0 = -(y1 - y0), b0 = x1 - x0, a1 = -(y2 - y1), b1 = x2 - x1, a2 = -(y0 - y2), b2 = x0 - x2;
	const float sx = (float)ix0 + 0.5f, sy = (float)iy0 + 0.5f;
	float r0 = a0 * (sx - x0) + b0 * (sy - y0);
	float r1 = a1 * (sx - x1) + b1 * (sy - y1);
	float r2 = a2 * (sx - x2) + b2 * (sy - y2);
	for (int y = iy0; y < iy1; ++y) {
		float e0 = r0, e1 = r1, e2 = r2;
		uint8_t* row = &s_mask.px[(y * mw) * 4 + 3];
		for (int x = ix0; x < ix1; ++x) {
			if (e0 >= 0.0f && e1 >= 0.0f && e2 >= 0.0f) {
				row[x * 4] = 255;
			}
			e0 += a0;
			e1 += a1;
			e2 += a2;
		}
		r0 += b0;
		r1 += b1;
		r2 += b2;
	}
}

/* Is any of the hand's capsules over the current panel, in front of it? */
static int vc_hand_near_panel(int hand) {
	XwaVrGloveCapsule caps[24];
	const int n = XwaVrGloves_Capsules(hand, caps, 24);
	for (int c = 0; c < n; ++c) {
		const float* p = s_vc.pose.position;
		for (int e = 0; e < 2; ++e) {
			const float* q = e ? caps[c].b : caps[c].a;
			const float r[3] = { q[0] - p[0], q[1] - p[1], q[2] - p[2] };
			const float m = caps[c].radius + 0.03f;
			if (vc_dot(r, s_vc.az) > -m && fabsf(vc_dot(r, s_vc.ax)) < 0.5f * s_vc.cur_w_m + 0.15f &&
				fabsf(vc_dot(r, s_vc.ay)) < 0.5f * s_vc.cur_h_m + 0.15f) {
				return 1;
			}
		}
	}
	return 0;
}

static int vc_cut_glove_silhouette(int hand) {
	const float* v;
	const float* nrm;
	const uint16_t* idx;
	int vn, in;
	if (!s_mask.cmd || !XwaVrGloves_Mesh(hand, &v, &nrm, &vn, &idx, &in) || vn > VC_MASK_MAX_VERTS) {
		return 0;
	}
	if (!vc_hand_near_panel(hand)) {
		return 1; /* nothing to cut */
	}
	/* Height above the panel (towards the viewer) and panel pixels. */
	const float* c = s_vc.pose.position;
	float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
	for (int i = 0; i < vn; ++i) {
		const float* p = &v[i * 3];
		const float r[3] = { p[0] - c[0], p[1] - c[1], p[2] - c[2] };
		const float to_eye[3] = { s_vc.eye[0] - p[0], s_vc.eye[1] - p[1], s_vc.eye[2] - p[2] };
		s_mask.h[i] = vc_dot(r, s_vc.az);
		s_mask.facing[i] = vc_dot(&nrm[i * 3], to_eye) > 0.0f;
		if (vc_project(p, &s_mask.sx[i], &s_mask.sy[i]) <= 0.0f) {
			s_mask.h[i] = -1.0f; /* behind the eye: treat as hidden */
			continue;
		}
		if (s_mask.h[i] > 0.0f) {
			bx0 = fminf(bx0, s_mask.sx[i]);
			by0 = fminf(by0, s_mask.sy[i]);
			bx1 = fmaxf(bx1, s_mask.sx[i]);
			by1 = fmaxf(by1, s_mask.sy[i]);
		}
	}
	bx0 = fmaxf(bx0, 0.0f);
	by0 = fmaxf(by0, 0.0f);
	bx1 = fminf(bx1, s_vc.cur_w_px);
	by1 = fminf(by1, s_vc.cur_h_px);
	if (bx1 <= bx0 || by1 <= by0) {
		return 1; /* in front of the panel, but not over it */
	}
	/* Texel size: at least two pixels (the bilinear edge stays under a
	 * millimetre), so the hand fits in the mask with a
	 * two-texel empty border (bilinear edges never sample past it). */
	const float bw = bx1 - bx0, bh = by1 - by0;
	const float scale = fmaxf(2.0f, fmaxf(bw, bh) / (float)(VC_MASK - 5));
	const int mw = (int)ceilf(bw / scale) + 4, mh = (int)ceilf(bh / scale) + 4;
	const float ox = bx0 - 2.0f * scale, oy = by0 - 2.0f * scale;
	memset(s_mask.px, 0, (size_t)mw * (size_t)mh * 4);
	for (int t = 0; t + 2 < in; t += 3) {
		const int k[3] = { idx[t], idx[t + 1], idx[t + 2] };
		if (k[0] >= vn || k[1] >= vn || k[2] >= vn) {
			continue;
		}
		const int front = (s_mask.h[k[0]] > 0.0f) + (s_mask.h[k[1]] > 0.0f) + (s_mask.h[k[2]] > 0.0f);
		/* The silhouette is the union of the faces turned towards the eye
		 * (the back faces cover the same area again). */
		if (front == 0 || !(s_mask.facing[k[0]] | s_mask.facing[k[1]] | s_mask.facing[k[2]])) {
			continue;
		}
		/* Clip to the part in front of the panel: the panel covers the rest. */
		float px[4], py[4];
		int n = 0;
		for (int e = 0; e < 3; ++e) {
			const int a = k[e], b = k[(e + 1) % 3];
			const float ha = s_mask.h[a], hb = s_mask.h[b];
			if (ha > 0.0f) {
				px[n] = s_mask.sx[a];
				py[n] = s_mask.sy[a];
				n++;
			}
			if ((ha > 0.0f) != (hb > 0.0f) && n < 4) {
				/* The crossing lies on the panel: its panel pixel is the
				 * point itself, whichever eye looks at it. */
				const float u = ha / (ha - hb);
				float q[3];
				for (int d = 0; d < 3; ++d) {
					q[d] = v[a * 3 + d] + (v[b * 3 + d] - v[a * 3 + d]) * u;
				}
				if (vc_project(q, &px[n], &py[n]) > 0.0f) {
					n++;
				}
			}
		}
		for (int j = 0; j < n; ++j) {
			px[j] = (px[j] - ox) / scale;
			py[j] = (py[j] - oy) / scale;
		}
		for (int j = 1; j + 1 < n; ++j) {
			vc_mask_tri(mw, mh, px[0], py[0], px[j], py[j], px[j + 1], py[j + 1]);
		}
	}
	AeronTexture** tex = &s_mask.tex[s_mask.eye][hand];
	if (!*tex) {
		*tex = Aeron_CreateTexture(&(AeronTextureDesc) {
			.width = VC_MASK,
			.height = VC_MASK,
			.mip_count = 1,
			.format = AERON_TEXTURE_FORMAT_RGBA8_UNORM,
			.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
			.debug_name = "xwa.vr_console.hand_mask",
		});
		if (!*tex) {
			return 0;
		}
	}
	/* Cycled: the same texture serves the next panel this frame. */
	if (!Aeron_UploadTextureDataCmd(s_mask.cmd, &(AeronTextureUploadDesc) {
			.texture = *tex,
			.width = mw,
			.height = mh,
			.pixels = s_mask.px,
			.pitch = mw * 4,
			.pixel_format = AERON_PIXEL_FORMAT_RGBA8888,
			.cycle = 1,
		})) {
		return 0;
	}
	AeronDrawList2DSprite sp;
	memset(&sp, 0, sizeof sp);
	sp.texture = *tex;
	sp.src_u0 = 1.0f / (float)VC_MASK;
	sp.src_v0 = 1.0f / (float)VC_MASK;
	sp.src_u1 = (float)(mw - 1) / (float)VC_MASK;
	sp.src_v1 = (float)(mh - 1) / (float)VC_MASK;
	sp.dst_x = ox + scale;
	sp.dst_y = oy + scale;
	sp.dst_w = (float)(mw - 2) * scale;
	sp.dst_h = (float)(mh - 2) * scale;
	sp.tint[0] = sp.tint[1] = sp.tint[2] = sp.tint[3] = 1.0f;
	sp.blend = AERON_BLIT2D_BLEND_ERASE;
	sp.filter = AERON_BLIT2D_FILTER_LINEAR;
	AeronDrawList_AddSprite(s_vc.cur_list, &sp);
	s_vc.cut_count++;
	s_mask.masks++;
	return 1;
}

/* The console is a layer drawn over the 3D view: wherever a glove is
 * between the eye and the current panel, cut the panel away so the hand
 * shows in front of it. */
static void vc_cut_gloves(void) {
	for (int h = 0; h < 2; ++h) {
		if (vc_cut_glove_silhouette(h)) {
			continue;
		}
		/* No mesh: the capsule approximation. */
		XwaVrGloveCapsule caps[24];
		const int n = XwaVrGloves_Capsules(h, caps, 24);
		for (int c = 0; c < n; ++c) {
			vc_cut_capsule(&caps[c]);
		}
	}
	/* The flight stick shows through too. */
	XwaVrGloveCapsule stick[4];
	const int n = XwaVrStick_Capsules(stick, 4);
	for (int c = 0; c < n; ++c) {
		vc_cut_capsule(&stick[c]);
	}
}

/* One bar of indicator lights: `lit` of `count` in colour `c`, with its
 * label and value above. `pulse` (0..1) dims the outlines (engine at 0). */
static void vc_energy_bar(float x, float y, float w, const char* label, const char* value, int count, int lit,
						  const float c[3], float pulse) {
	const float gap = 5.0f, h = 20.0f;
	const float cell = (w - gap * (float)(count - 1)) / (float)count;
	vc_text(x, y, label, 16.0f, 0, c[0], c[1], c[2], 0.95f);
	vc_text(x + w, y, value, 16.0f, 2, c[0], c[1], c[2], 0.95f);
	for (int i = 0; i < count; ++i) {
		float fill[4], rim[4];
		if (i < lit) {
			vc_pma(fill, c[0], c[1], c[2], 0.85f);
			vc_pma(rim, c[0], c[1], c[2], 1.0f);
		} else {
			vc_pma(fill, c[0], c[1], c[2], 0.06f);
			vc_pma(rim, c[0], c[1], c[2], 0.40f * (1.0f - pulse) + 0.85f * pulse);
		}
		vc_rrect(x + (float)i * (cell + gap), y + 22.0f, cell, h, 4.0f, 1.5f, 1.0f, fill, rim);
	}
}

/* Energy: the split the HUD shows beside the radars (laser and shield
 * pips, beam pips, reserve), as bars of lights. Engine is the reserve:
 * what lasers, shields and beam leave for speed. Lasers and shields have
 * 4 lights; the engine 8, or 12 on craft with a beam (the game counts 12
 * units there), which also get a 4-light beam bar. */
static void vc_draw_energy(float x, float y, float w) {
	const XwaVrConsoleState* st = &s_vc.state;
	const int lasers = st->laser_level;
	const int shields = st->has_shields ? st->shield_level : 0;
	const int beam = st->has_beam ? st->beam_level : 0;
	int engine = 8 - lasers;
	if (st->has_beam) {
		if (st->has_shields) {
			engine += 2 - shields;
		}
		engine += 2 - beam;
	} else {
		engine -= shields;
	}
	const int engine_lights = st->has_beam ? 12 : 8;
	engine = engine < 0 ? 0 : engine > engine_lights ? engine_lights : engine;
	static const float red[3] = { 1.0f, 0.36f, 0.28f };
	static const float yellow[3] = { 1.0f, 0.85f, 0.25f };
	static const float purple[3] = { 0.80f, 0.42f, 1.0f };
	static const float blue[3] = { 0.36f, 0.66f, 1.0f };
	/* Bars sized by their light count so all lights are the same width. */
	const int bars = st->has_beam ? 4 : 3;
	const float bar_gap = 20.0f;
	const int counts[4] = { 4, 4, st->has_beam ? 4 : engine_lights, engine_lights };
	int total = 0;
	for (int i = 0; i < bars; ++i) {
		total += counts[i];
	}
	const float unit = (w - bar_gap * (float)(bars - 1)) / (float)total;
	const float pulse = engine == 0 ? 0.5f + 0.5f * sinf(s_vc.clock * 5.0f) : 0.0f;
	char text[16];
	float cx = x;
	snprintf(text, sizeof text, "%d", lasers);
	vc_energy_bar(cx, y, unit * 4.0f, "LASERS", text, 4, lasers, red, 0.0f);
	cx += unit * 4.0f + bar_gap;
	if (st->has_shields) {
		snprintf(text, sizeof text, "%d", shields);
		vc_energy_bar(cx, y, unit * 4.0f, "SHIELDS", text, 4, shields, yellow, 0.0f);
	} else {
		const float dim[3] = { yellow[0] * 0.5f, yellow[1] * 0.5f, yellow[2] * 0.5f };
		vc_energy_bar(cx, y, unit * 4.0f, "SHIELDS", "NONE", 4, 0, dim, 0.0f);
	}
	cx += unit * 4.0f + bar_gap;
	if (st->has_beam) {
		snprintf(text, sizeof text, "%d", beam);
		vc_energy_bar(cx, y, unit * 4.0f, "BEAM", text, 4, beam, purple, 0.0f);
		cx += unit * 4.0f + bar_gap;
	}
	snprintf(text, sizeof text, "%d", engine);
	vc_energy_bar(cx, y, unit * (float)engine_lights, "ENGINE", text, engine_lights, engine, blue, pulse);
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
	vc_draw_handles(&s_vc.lap);
	vc_draw_energy(34.0f, 406.0f, 470.0f);
	/* Scan lines, fixed to the panel. */
	for (float y = 14.0f; y < VC_H - 14.0f; y += 6.0f) {
		float line[4];
		vc_pma(line, VC_CYAN, 0.025f);
		AeronDrawList_AddFill(s_vc.cur_list, 14.0f, y, VC_W - 28.0f, 2.0f, line, AERON_BLIT2D_BLEND_PMA, NULL);
	}
	/* Column dividers. */
	for (int i = 0; i < 2; ++i) {
		float line[4];
		vc_pma(line, VC_CYAN, 0.35f);
		const float x = i == 0 ? 520.0f : 1015.0f;
		AeronDrawList_AddFill(s_vc.cur_list, x, 40.0f, 2.0f, VC_H - 80.0f, line, AERON_BLIT2D_BLEND_PMA, NULL);
	}
	/* Rules between the stacked sections (targeting / weapons, wingmen /
	 * ship). */
	{
		float line[4];
		vc_pma(line, VC_CYAN, 0.35f);
		AeronDrawList_AddFill(s_vc.cur_list, 538.0f, s_vc.divider_y, 460.0f, 2.0f, line, AERON_BLIT2D_BLEND_PMA, NULL);
		AeronDrawList_AddFill(s_vc.cur_list, 1032.0f, 274.0f, 470.0f, 2.0f, line, AERON_BLIT2D_BLEND_PMA, NULL);
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
}

/* Per-eye overlay of the lap console: the glove cut-outs, then the cursors. */
static void vc_cut_gloves(void);
static void vc_console_overlay(void) {
	vc_cut_gloves();
	/* Cursors: a ring that closes in as the tip nears the surface, or a
	 * dot where the ray lands. */
	for (int h = 0; h < 2; ++h) {
		const VcHand* hand = &s_vc.hands[h];
		if (!hand->cursor) {
			continue;
		}
		if (hand->cursor == 1) {
			/* Touch: no pointer, the button under the finger lights up. */
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
static int vc_card_enabled(void);

void XwaVrConsole_BeginFrame(float dt_seconds) {
	s_vc.dt = dt_seconds > 0.0f && dt_seconds < 0.25f ? dt_seconds : 0.0f;
	s_vc.clock = fmodf(s_vc.clock + s_vc.dt, 1000.0f);
	s_vc.drawn = 0;
	for (int c = 0; c < VC_CARDS; ++c) {
		s_vc.cards[c].drawn = 0;
	}
	/* Load the font here, between frames, rather than while the flight
	 * renderer is recording its command buffer. */
	if (s_vc.state.valid && s_vc.font_state == 0 && (vc_enabled() || vc_card_enabled()) && Aeron_XrIsPresenting()) {
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

/* Whether a panel (pose, w x h metres) is anywhere near the field of view. */
static int vc_in_view(const AeronXrPanel* pose, float w, float h, const AeronXrView views[2]) {
	static const float minus_z[3] = { 0.0f, 0.0f, -1.0f };
	float f[3];
	vc_rotate(views[0].orientation, minus_z, f);
	float to[3];
	for (int k = 0; k < 3; ++k) {
		to[k] = pose->position[k] - 0.5f * (views[0].position[k] + views[1].position[k]);
	}
	const float len = sqrtf(vc_dot(to, to));
	if (len < 0.05f) {
		return 1;
	}
	const float angle = acosf(fmaxf(-1.0f, fminf(1.0f, vc_dot(f, to) / len)));
	const float radius = atanf(0.5f * sqrtf(w * w + h * h) / len);
	return angle - radius < 62.0f * 3.14159265f / 180.0f;
}

/* Records the current panel with `draw` (s_vc.cur_* set up), once per eye
 * when a hand or the stick is cut out of it (the holes must match what each
 * eye sees, else the panel shows a dark rim beside the glove like a
 * shadow), and redraws its layer only when that changed. Fills the quads
 * (`pose` in out[0]) and returns their count. */
/* A panel's own content (everything but the per-eye cut-outs and cursors),
 * kept in its own image and redrawn only when it changes: each frame the
 * layer is then one copy per eye plus the small overlay. */
typedef struct VcBase {
	AeronRenderTarget* rt;
	AeronDrawList2D* list;
	uint64_t hash;
	int valid, w, h;
	AeronTextureFormat format;
} VcBase;
static VcBase s_base[AERON_XR_PANEL_COUNT];

static int vc_render_panel(AeronCommandBuffer* cmd, int layer, AeronDrawList2D* lists[2], void (*draw)(void),
						   void (*overlay)(void), const AeronXrView views[2], AeronXrPanel out[2], int width,
						   int height, float w_m, float h_m, uint64_t* shown_hash, int* hash_valid) {
	const AeronXrPanel pose = out[0];
	if (!vc_in_view(&pose, w_m, h_m, views) || layer < 0 || layer >= AERON_XR_PANEL_COUNT) {
		return 0; /* nothing to draw or show; the layer keeps its image */
	}
	VcBase* base = &s_base[layer];
	if (!base->list && !(base->list = AeronDrawList_Create(0))) {
		return 0;
	}
	/* The content. */
	s_vc.overlay_phase = 0;
	s_vc.cur_list = base->list;
	memcpy(s_vc.eye, views[0].position, sizeof s_vc.eye);
	AeronDrawList_Begin(base->list, NULL, width, height, AERON_DRAWLIST2D_CLEAR, NULL);
	draw();
	const uint64_t base_hash = AeronDrawList_Hash(base->list);
	/* The content's own image, in the layer's format. Taking the layer's
	 * target acquires its next image, so only do that to draw into it. */
	static AeronTextureFormat s_panel_format = AERON_TEXTURE_FORMAT_UNKNOWN;
	AeronRenderTarget* target = NULL;
	if (s_panel_format == AERON_TEXTURE_FORMAT_UNKNOWN) {
		target = Aeron_XrStereoPanelTarget(layer, NULL, NULL);
		AeronTexture* target_tex = target ? Aeron_RenderTargetGetTexture(target) : NULL;
		if (!target_tex) {
			return 0;
		}
		s_panel_format = Aeron_TextureGetFormat(target_tex);
		*hash_valid = 0; /* acquired: must be drawn this frame */
	}
	const AeronTextureFormat format = s_panel_format;
	if (!base->rt || base->w != width || base->h != height || base->format != format) {
		if (base->rt) {
			Aeron_DestroyRenderTarget(base->rt);
		}
		base->rt = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
			.width = width,
			.height = height,
			.format = format,
			.debug_name = "xwa.vr_console.panel",
		});
		base->w = width;
		base->h = height;
		base->format = format;
		base->valid = 0;
		if (!base->rt) {
			return 0;
		}
	}
	/* Each eye: the content, then its overlay. */
	AeronDrawList2DSprite content;
	memset(&content, 0, sizeof content);
	content.texture = Aeron_RenderTargetGetTexture(base->rt);
	content.src_u1 = 1.0f;
	content.src_v1 = 1.0f;
	content.dst_w = (float)width;
	content.dst_h = (float)height;
	content.tint[0] = content.tint[1] = content.tint[2] = content.tint[3] = 1.0f;
	content.blend = AERON_BLIT2D_BLEND_NONE;
	content.filter = AERON_BLIT2D_FILTER_NEAREST;
	s_vc.overlay_phase = 1;
	s_vc.cut_count = 0;
	s_mask.cmd = cmd;
	s_mask.eye = 0;
	s_mask.masks = 0;
	s_vc.cur_list = lists[0];
	AeronDrawList_Begin(lists[0], NULL, width, height, AERON_DRAWLIST2D_CLEAR, NULL);
	AeronDrawList_AddSprite(lists[0], &content);
	overlay();
	const int stereo = s_vc.cut_count > 0;
	uint64_t hash = base_hash * 1099511628211ull ^ AeronDrawList_Hash(lists[0]);
	if (stereo) {
		s_vc.cur_list = lists[1];
		memcpy(s_vc.eye, views[1].position, sizeof s_vc.eye);
		s_mask.eye = 1;
		AeronDrawList_Begin(lists[1], NULL, width, height, AERON_DRAWLIST2D_LOAD, NULL);
		AeronDrawList_SetOrigin(lists[1], width, 0);
		AeronDrawList_AddSprite(lists[1], &content);
		overlay();
		hash = hash * 1099511628211ull ^ AeronDrawList_Hash(lists[1]);
	}
	s_vc.overlay_phase = 0;
	s_mask.cmd = NULL;
	if (s_mask.masks) {
		*hash_valid = 0; /* the masks' pixels are not in the hash: always redraw */
	}
	if (!*hash_valid || hash != *shown_hash) {
		if (!target && !(target = Aeron_XrStereoPanelTarget(layer, NULL, NULL))) {
			return 0;
		}
		if (!base->valid || base->hash != base_hash) {
			AeronDrawList_SetTarget(base->list, base->rt);
			AeronDrawList_Render(base->list, cmd);
			base->hash = base_hash;
			base->valid = 1;
		}
		/* One pass over the layer image: each eye's half is its content
		 * plus overlay (nothing is loaded from memory). */
		const int eyes = stereo ? 2 : 1;
		for (int e = 0; e < eyes; ++e) {
			AeronDrawList_SetTarget(lists[e], target);
			if (!AeronDrawList_Prepare(lists[e], cmd)) {
				return 0;
			}
		}
		AeronRenderPass* pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
			.color_target = target,
			.clear_color = 1,
			.command_buffer = cmd,
			.debug_label = "VR panel",
		});
		if (!pass) {
			return 0;
		}
		for (int e = 0; e < eyes; ++e) {
			AeronDrawList_RenderIntoPass(lists[e], cmd, pass, target);
		}
		Aeron_EndRenderPass(pass);
		*shown_hash = hash;
		*hash_valid = 1;
	}
	out[0].rect[0] = 0;
	out[0].rect[1] = 0;
	out[0].rect[2] = width;
	out[0].rect[3] = height;
	out[0].eye = stereo ? 1 : 0;
	if (!stereo) {
		return 1;
	}
	out[1] = out[0];
	out[1].rect[0] = width;
	out[1].eye = 2;
	return 2;
}

int XwaVrConsole_Frame(AeronCommandBuffer* cmd, int width, int height, const AeronXrView views[2],
					   AeronXrPanel out[2]) {
	if (!cmd || width <= 0 || height <= 0 || !views || !out || !s_vc.state.valid || !vc_enabled() ||
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
	if (!s_vc.lap.grab_hand) {
		vc_read_pose_env();
	}
	vc_pose(out, ax, ay, az);
	vc_update_grab(&s_vc.lap, out, ax, ay, az, VC_WIDTH_M, VC_HEIGHT_M);
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
		const int handled = s_vc.lap.grab_hand || s_vc.lap.hover_side[0] || s_vc.lap.hover_side[1];
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

	s_vc.cur_place = &s_vc.lap;
	s_vc.cur_w_m = VC_WIDTH_M;
	s_vc.cur_h_m = VC_HEIGHT_M;
	s_vc.cur_w_px = VC_W;
	s_vc.cur_h_px = VC_H;
	if (!s_vc.list_r) {
		s_vc.list_r = AeronDrawList_Create(0);
		if (!s_vc.list_r) {
			return 0;
		}
	}
	AeronDrawList2D* lists[2] = { s_vc.list, s_vc.list_r };
	return vc_render_panel(cmd, AERON_XR_PANEL_CONSOLE, lists, vc_draw, vc_console_overlay, views, out, width,
						   height, VC_WIDTH_M,
						   VC_HEIGHT_M, &s_vc.hash, &s_vc.hash_valid);
}


/* ---- Button card ------------------------------------------------------- */

/* The controller layout in flight: Quest Touch controllers in outline,
 * three-quarter view from above, each control labelled. Keep in step with
 * the VR button bindings in xwa_runtime/runtime/port.c and the Touch mapping
 * in Aeron's xr_input_map.h. "grip+" = with the left grip held. */

/* Outline paths of the LEFT controller, in card pixels around the top
 * face's centre (y down); the right one is its mirror image. */
static const float k_ctl_handle[][2] = {
	{ -88.0f, 66.0f },   { -104.0f, 128.0f }, { -116.0f, 198.0f }, { -120.0f, 254.0f }, { -110.0f, 293.0f },
	{ -86.0f, 309.0f },  { -60.0f, 298.0f },  { -44.0f, 256.0f },  { -24.0f, 190.0f },  { -2.0f, 128.0f },
	{ 18.0f, 88.0f },
};
static const float k_ctl_trigger[][2] = {
	{ 99.0f, 12.0f }, { 111.0f, 42.0f }, { 109.0f, 84.0f }, { 97.0f, 101.0f }, { 84.0f, 89.0f }, { 80.0f, 50.0f },
};
static const float k_ctl_grip[][2] = {
	{ -6.0f, 110.0f }, { 8.0f, 104.0f }, { 12.0f, 122.0f }, { 0.0f, 164.0f }, { -14.0f, 176.0f }, { -20.0f, 155.0f },
};
#define CTL_FACE_RX 104.0f
#define CTL_FACE_RY 68.0f
#define CTL_FACE_ROT_DEG -26.0f

/* Where each control sits on the left controller (mirrored on the right). */
enum { CTL_STICK, CTL_UPPER, CTL_LOWER, CTL_MENU, CTL_TRIGGER, CTL_GRIP, CTL_PARTS };
static const float k_ctl_part[CTL_PARTS][3] = {
	/* x, y, radius */
	[CTL_STICK] = { -30.0f, -20.0f, 25.0f }, [CTL_UPPER] = { 42.0f, -12.0f, 14.0f },
	[CTL_LOWER] = { 16.0f, 27.0f, 14.0f },   [CTL_MENU] = { -58.0f, 24.0f, 8.0f },
	[CTL_TRIGGER] = { 97.0f, 92.0f, 0.0f },  [CTL_GRIP] = { -4.0f, 140.0f, 0.0f },
};

typedef struct VcCardLabel {
	int part;
	const char* action;
	const char* more;
	float y; /* label row (card pixels) */
} VcCardLabel;

static const VcCardLabel k_card_left[] = {
	{ CTL_STICK, "Throttle (fwd/back) - roll", "click: fire link (X)   grip+: release", 96.0f },
	{ CTL_UPPER, "Y: match speed (Enter)", "grip+: pick up", 168.0f },
	{ CTL_LOWER, "X: countermeasure (C)", "grip+: confirm (Space)", 232.0f },
	{ CTL_MENU, "Pause / menu", NULL, 296.0f },
	{ CTL_GRIP, "Hold: grip+ layer", "on a panel or the stick: move it", 380.0f },
	{ CTL_TRIGGER, "Cycle warheads (W)", "grip+: dock / board", 452.0f },
};

static const VcCardLabel k_card_right[] = {
	{ CTL_STICK, "Yaw / pitch", "click: my attacker (E)   grip+: (A)", 96.0f },
	{ CTL_UPPER, "B: next target (T)", "grip+: previous target (Y)", 168.0f },
	{ CTL_LOWER, "A: nearest enemy (R)", "grip+: nearest objective (O)", 232.0f },
	{ CTL_MENU, "Meta: system menu", NULL, 296.0f },
	{ CTL_GRIP, "Target in sights", "on the flight stick: fly with it", 380.0f },
	{ CTL_TRIGGER, "Fire", NULL, 452.0f },
};

/* Smooth open/closed path through `pts` (Catmull-Rom), offset and
 * optionally mirrored. */
static void vc_card_path(const float (*pts)[2], int n, int closed, float ox, float oy, float sx, float width,
						 const float col[4]) {
	const int steps = 6;
	float px = 0.0f, py = 0.0f;
	int have = 0;
	const int segs = closed ? n : n - 1;
	for (int i = 0; i < segs; ++i) {
		const int i0 = closed ? (i - 1 + n) % n : (i > 0 ? i - 1 : 0);
		const int i1 = i;
		const int i2 = closed ? (i + 1) % n : i + 1;
		const int i3 = closed ? (i + 2) % n : (i + 2 < n ? i + 2 : n - 1);
		for (int s = 0; s <= steps; ++s) {
			if (have && s == 0) {
				continue;
			}
			const float t = (float)s / (float)steps, t2 = t * t, t3 = t2 * t;
			float p[2];
			for (int k = 0; k < 2; ++k) {
				p[k] = 0.5f * ((2.0f * pts[i1][k]) + (-pts[i0][k] + pts[i2][k]) * t +
							   (2.0f * pts[i0][k] - 5.0f * pts[i1][k] + 4.0f * pts[i2][k] - pts[i3][k]) * t2 +
							   (-pts[i0][k] + 3.0f * pts[i1][k] - 3.0f * pts[i2][k] + pts[i3][k]) * t3);
			}
			const float x = ox + sx * p[0], y = oy + p[1];
			if (have) {
				AeronDrawList_AddLine(s_vc.cur_list, px, py, x, y, width, col, AERON_BLIT2D_BLEND_PMA, NULL);
			}
			px = x;
			py = y;
			have = 1;
		}
	}
}

/* Outline of an ellipse arc (degrees in its own frame, rotated `rot`). */
static void vc_card_ellipse(float cx, float cy, float rx, float ry, float rot_deg, float a0, float a1, float width,
							const float col[4]) {
	const float rot = rot_deg * 3.14159265f / 180.0f;
	const float cr = cosf(rot), sr = sinf(rot);
	const int steps = (int)fmaxf(8.0f, fabsf(a1 - a0) / 6.0f);
	float px = 0.0f, py = 0.0f;
	for (int i = 0; i <= steps; ++i) {
		const float a = (a0 + (a1 - a0) * (float)i / (float)steps) * 3.14159265f / 180.0f;
		const float ex = rx * cosf(a), ey = ry * sinf(a);
		const float x = cx + ex * cr - ey * sr, y = cy + ex * sr + ey * cr;
		if (i > 0) {
			AeronDrawList_AddLine(s_vc.cur_list, px, py, x, y, width, col, AERON_BLIT2D_BLEND_PMA, NULL);
		}
		px = x;
		py = y;
	}
}

/* One controller: outline, its controls and their labels. `sx` -1 mirrors
 * it into the right controller; labels go to the outer side. */
static void vc_card_controller(float cx, float cy, float sx, const VcCardLabel* labels, int count, float text_x,
							   float mid_x) {
	float line[4], body[4], part[4], lead[4], trig[4], grip[4];
	vc_pma(body, 0.78f, 0.93f, 1.0f, 0.95f);
	vc_pma(line, 0.78f, 0.93f, 1.0f, 0.95f);
	vc_pma(part, VC_CYAN, 1.0f);
	vc_pma(lead, VC_CYAN, 0.5f);
	vc_pma(trig, VC_AMBER, 1.0f);
	vc_pma(grip, 0.66f, 0.56f, 1.0f, 1.0f);
	const float face_rot = CTL_FACE_ROT_DEG * sx;
	/* Handle first, then the face over it (the face hides the handle's top). */
	vc_card_path(k_ctl_handle, (int)(sizeof k_ctl_handle / sizeof k_ctl_handle[0]), 0, cx, cy, sx, 3.0f, body);
	vc_card_ellipse(cx + 5.0f * sx, cy + 15.0f, CTL_FACE_RX, CTL_FACE_RY, face_rot, sx > 0.0f ? -12.0f : 12.0f,
					sx > 0.0f ? 192.0f : 168.0f, 3.0f, body);
	vc_card_ellipse(cx, cy, CTL_FACE_RX, CTL_FACE_RY, face_rot, 0.0f, 360.0f, 3.0f, line);
	vc_card_path(k_ctl_trigger, (int)(sizeof k_ctl_trigger / sizeof k_ctl_trigger[0]), 1, cx, cy, sx, 2.5f, trig);
	vc_card_path(k_ctl_grip, (int)(sizeof k_ctl_grip / sizeof k_ctl_grip[0]), 1, cx, cy, sx, 2.5f, grip);
	/* Thumbstick (cap and well) and the face buttons. */
	{
		const float* s = k_ctl_part[CTL_STICK];
		vc_card_ellipse(cx + sx * s[0], cy + s[1], s[2], s[2] * 0.82f, 0.0f, 0.0f, 360.0f, 2.5f, part);
		vc_card_ellipse(cx + sx * s[0], cy + s[1] - 3.0f, s[2] * 0.62f, s[2] * 0.52f, 0.0f, 0.0f, 360.0f, 2.5f, part);
	}
	for (int p = CTL_UPPER; p <= CTL_MENU; ++p) {
		const float* b = k_ctl_part[p];
		vc_card_ellipse(cx + sx * b[0], cy + b[1], b[2], b[2] * 0.78f, 0.0f, 0.0f, 360.0f, 2.5f, part);
	}
	/* Labels with leader lines out to the side. */
	for (int i = 0; i < count; ++i) {
		const VcCardLabel* l = &labels[i];
		const float* pp = k_ctl_part[l->part];
		const float ex = cx + sx * pp[0], ey = cy + pp[1];
		/* Triggers: labelled below, toward the card's middle. */
		const int under = l->part == CTL_TRIGGER;
		const float tx = under ? mid_x - sx * 14.0f : text_x;
		const float ly = under ? l->y - 8.0f : l->y + 14.0f;
		const float lx = under ? ex + sx * 6.0f : text_x + (sx > 0.0f ? 10.0f : -10.0f);
		/* Start at the control's edge, toward the label. */
		const float r = l->part <= CTL_MENU ? pp[2] : 0.0f;
		float dx = lx - ex, dy = ly - ey;
		const float dl = sqrtf(dx * dx + dy * dy);
		dx /= dl > 0.0f ? dl : 1.0f;
		dy /= dl > 0.0f ? dl : 1.0f;
		const float* c = l->part == CTL_TRIGGER ? trig : l->part == CTL_GRIP ? grip : lead;
		float dot[4];
		memcpy(dot, c, sizeof dot);
		AeronDrawList_AddLine(s_vc.cur_list, ex + dx * (r + 3.0f), ey + dy * (r + 3.0f), lx, ly, 1.5f, lead,
							  AERON_BLIT2D_BLEND_PMA, NULL);
		vc_rrect(lx - 3.0f, ly - 3.0f, 6.0f, 6.0f, 3.0f, 0.0f, 0.0f, dot, dot);
		const int align = sx > 0.0f ? 2 : 0;
		const float* tc = l->part == CTL_TRIGGER ? (const float[3]) { VC_AMBER }
						  : l->part == CTL_GRIP  ? (const float[3]) { 0.80f, 0.72f, 1.0f }
												 : (const float[3]) { 0.85f, 0.97f, 1.0f };
		vc_text(tx, l->y, l->action, 20.0f, align, tc[0], tc[1], tc[2], 1.0f);
		if (l->more) {
			vc_text(tx, l->y + 24.0f, l->more, 15.0f, align, 0.62f, 0.85f, 0.92f, 0.9f);
		}
	}
}

static void vc_card_draw(void) {
	const float W = s_vc.cur_w_px, H = s_vc.cur_h_px;
	const float transparent[4] = { 0, 0, 0, 0 };
	float fill[4], border[4], glow[4];
	/* The lap console's plate. */
	vc_pma(glow, VC_CYAN, 0.25f);
	vc_rrect(4.0f, 4.0f, W - 8.0f, H - 8.0f, 30.0f, 8.0f, 14.0f, transparent, glow);
	vc_pma(fill, 0.157f, 0.549f, 0.745f, 0.14f);
	vc_pma(border, VC_CYAN, 0.6f);
	vc_rrect(10.0f, 10.0f, W - 20.0f, H - 20.0f, 26.0f, 2.0f, 1.0f, fill, border);
	vc_draw_handles(s_vc.cur_place);
	for (float y = 14.0f; y < H - 14.0f; y += 6.0f) {
		float line[4];
		vc_pma(line, VC_CYAN, 0.025f);
		AeronDrawList_AddFill(s_vc.cur_list, 14.0f, y, W - 28.0f, 2.0f, line, AERON_BLIT2D_BLEND_PMA, NULL);
	}
	vc_text(44.0f, 30.0f, "CONTROLS", 30.0f, 0, VC_CYAN, 1.0f);
	vc_text(W - 44.0f, 38.0f, "grip+ = while holding the LEFT grip", 18.0f, 2, 0.62f, 0.85f, 0.92f, 0.9f);
	const float cy = 222.0f;
	vc_card_controller(W * 0.5f - 148.0f, cy, 1.0f, k_card_left, (int)(sizeof k_card_left / sizeof k_card_left[0]),
					   236.0f, W * 0.5f);
	vc_card_controller(W * 0.5f + 148.0f, cy, -1.0f, k_card_right, (int)(sizeof k_card_right / sizeof k_card_right[0]),
					   W - 236.0f, W * 0.5f);
	vc_text(W * 0.5f - 148.0f - 80.0f, cy + 318.0f, "LEFT", 22.0f, 1, VC_CYAN, 0.9f);
	vc_text(W * 0.5f + 148.0f + 80.0f, cy + 318.0f, "RIGHT", 22.0f, 1, VC_CYAN, 0.9f);
	vc_text(W * 0.5f, H - 158.0f, "Flight stick (between your knees): right grip on it = fly, left grip = move it", 17.0f,
			1, 0.85f, 0.97f, 1.0f, 0.95f);
	vc_text(W * 0.5f, H - 132.0f, "Voice: double-tap a thumb rest, then say e.g. \"attack my target\", \"nearest enemy\"",
			17.0f, 1, 0.85f, 0.97f, 1.0f, 0.95f);
	vc_text(W * 0.5f, H - 104.0f, "Right stick flicks, grip+:  up  lasers > shields    down  shields > lasers", 17.0f, 1,
			0.62f, 0.85f, 0.92f, 0.9f);
	vc_text(W * 0.5f, H - 78.0f, "left  shield balance (S)    right  flagged targets (N)", 17.0f, 1, 0.62f, 0.85f,
			0.92f, 0.9f);
}

/* The voice command list (left card). Keep in step with the command table
 * in xwa_app/voice_core.c: one phrase per command; close wordings work too. */
static const char* const k_voice_wing[] = { "attack my target", "cover me",       "ignore my target",
											"evade",            "wait",           "go ahead",
											"return to base",   "report in",      "call resupply",
											"call reinforcements" };
static const char* const k_voice_target[] = { "nearest enemy",      "next target",      "previous target",
											  "target my attacker", "target objective", "newest target",
											  "next subsystem",     "clear target" };
static const char* const k_voice_ship[] = { "match speed",       "countermeasures",   "s-foils",
											"lasers to shields", "shields to lasers", "fire link",
											"convergence",       "map",               "dock",
											"pick up",           "release cargo",     "hyperspace" };

static void vc_voice_column(float x, const char* title, const char* const* lines, int count) {
	float rule[4];
	vc_text(x, 132.0f, title, 20.0f, 0, VC_CYAN, 0.95f);
	vc_pma(rule, VC_CYAN, 0.45f);
	AeronDrawList_AddFill(s_vc.cur_list, x, 160.0f, 270.0f, 2.0f, rule, AERON_BLIT2D_BLEND_PMA, NULL);
	for (int i = 0; i < count; ++i) {
		char text[64];
		snprintf(text, sizeof text, "\"%s\"", lines[i]);
		vc_text(x, 176.0f + 41.0f * (float)i, text, 21.0f, 0, 0.85f, 0.97f, 1.0f, 1.0f);
	}
}

static void vc_voice_draw(void) {
	const float W = s_vc.cur_w_px, H = s_vc.cur_h_px;
	const float transparent[4] = { 0, 0, 0, 0 };
	float fill[4], border[4], glow[4];
	vc_pma(glow, VC_CYAN, 0.25f);
	vc_rrect(4.0f, 4.0f, W - 8.0f, H - 8.0f, 30.0f, 8.0f, 14.0f, transparent, glow);
	vc_pma(fill, 0.157f, 0.549f, 0.745f, 0.14f);
	vc_pma(border, VC_CYAN, 0.6f);
	vc_rrect(10.0f, 10.0f, W - 20.0f, H - 20.0f, 26.0f, 2.0f, 1.0f, fill, border);
	vc_draw_handles(s_vc.cur_place);
	for (float y = 14.0f; y < H - 14.0f; y += 6.0f) {
		float line[4];
		vc_pma(line, VC_CYAN, 0.025f);
		AeronDrawList_AddFill(s_vc.cur_list, 14.0f, y, W - 28.0f, 2.0f, line, AERON_BLIT2D_BLEND_PMA, NULL);
	}
	vc_text(44.0f, 30.0f, "VOICE COMMANDS", 30.0f, 0, VC_CYAN, 1.0f);
	vc_text(W - 44.0f, 26.0f, "double-tap a thumb rest, then speak", 18.0f, 2, 0.85f, 0.97f, 1.0f, 0.95f);
	vc_text(W - 44.0f, 50.0f, "it listens until you pause (or tap again)", 16.0f, 2, 0.62f, 0.85f, 0.92f, 0.9f);
	vc_voice_column(44.0f, "WINGMEN", k_voice_wing, (int)(sizeof k_voice_wing / sizeof k_voice_wing[0]));
	vc_voice_column(366.0f, "TARGETING", k_voice_target, (int)(sizeof k_voice_target / sizeof k_voice_target[0]));
	vc_voice_column(688.0f, "SHIP", k_voice_ship, (int)(sizeof k_voice_ship / sizeof k_voice_ship[0]));
	vc_text(W * 0.5f, H - 84.0f, "Your own words work too: \"wingmen, go after my target\", \"shoot flares\"", 17.0f, 1,
			0.62f, 0.85f, 0.92f, 0.9f);
}

static int vc_card_enabled_kind(int card) {
	if (card == VC_CARD_VOICE) {
		/* The voice command list: when voice commands are on. */
		const char* voice = getenv("XWA_XR_VOICE");
		const char* env = getenv("XWA_XR_VOICE_CARD");
		if (voice && (voice[0] == '0' || voice[0] == 'o' || voice[0] == 'O')) { /* 0 / off */
			return 0;
		}
		return !(env && env[0] == '0');
	}
	const char* env = getenv("XWA_XR_BUTTON_CARD");
	return !(env && env[0] == '0');
}

static int vc_card_enabled(void) {
	return vc_card_enabled_kind(VC_CARD_BUTTONS) || vc_card_enabled_kind(VC_CARD_VOICE);
}

int XwaVrConsole_CardFrame(AeronCommandBuffer* cmd, int card, int width, int height, const AeronXrView views[2],
						   AeronXrPanel out[2]) {
	if (card < 0 || card >= VC_CARDS || !cmd || width <= 0 || height <= 0 || !views || !out || !s_vc.state.valid ||
		!vc_card_enabled_kind(card) || s_vc.font_state == 0 || !vc_ensure()) {
		return 0;
	}
	VcCard* cd = &s_vc.cards[card];
	for (int e = 0; e < 2; ++e) {
		if (!cd->list[e]) {
			cd->list[e] = AeronDrawList_Create(0);
			if (!cd->list[e]) {
				return 0;
			}
		}
	}
	/* Every mission starts with the cards back beside the player: buttons
	 * on the right, voice commands on the left. */
	if (s_vc.state.mission_serial != cd->mission) {
		vc_grab_end(&cd->place);
		cd->place.custom = 0;
		cd->mission = s_vc.state.mission_serial;
	}
	cd->drawn = 1;
	const float yaw = card == VC_CARD_VOICE ? vc_env("XWA_XR_VOICE_CARD_YAW", -VC_CARD_YAW_DEG)
											: vc_env("XWA_XR_CARD_YAW", VC_CARD_YAW_DEG);
	if (!cd->logged) {
		Aeron_LogInfo("xwa.remaster", "%s card: %dx%d layer, %.0f deg %s", card ? "voice command" : "button", width,
					  height, (double)fabsf(yaw), yaw < 0.0f ? "left" : "right");
		cd->logged = 1;
	}

	memset(out, 0, sizeof *out);
	float ax[3], ay[3], az[3];
	for (int pass = 0; pass < 2; ++pass) {
		if (!vc_place_pose(&cd->place, VC_CARD_W_M, VC_CARD_H_M, out, ax, ay, az)) {
			vc_facing_pose(yaw, VC_CARD_PITCH_DEG, VC_CARD_DISTANCE_M, VC_CARD_W_M, VC_CARD_H_M, out, ax, ay, az);
		}
		if (pass == 0) {
			vc_update_grab(&cd->place, out, ax, ay, az, VC_CARD_W_M, VC_CARD_H_M);
		}
	}
	/* Bright while looked at or handled. */
	{
		static const float minus_z[3] = { 0.0f, 0.0f, -1.0f };
		float eye[3], f[3];
		for (int k = 0; k < 3; ++k) {
			eye[k] = 0.5f * (views[0].position[k] + views[1].position[k]);
		}
		vc_rotate(views[0].orientation, minus_z, f);
		float to[3] = { out->position[0] - eye[0], out->position[1] - eye[1], out->position[2] - eye[2] };
		const float len = sqrtf(vc_dot(to, to));
		const float c = len > 1e-4f ? vc_dot(f, to) / len : 1.0f;
		const float angle = acosf(fmaxf(-1.0f, fminf(1.0f, c))) * 180.0f / 3.14159265f;
		if (cd->place.grab_hand || cd->place.hover_side[0] || cd->place.hover_side[1]) {
			cd->awake = 1;
		} else if (cd->awake && angle > VC_CARD_SLEEP_DEG) {
			cd->awake = 0;
		} else if (!cd->awake && angle < VC_CARD_WAKE_DEG) {
			cd->awake = 1;
		}
		const float goal = cd->awake ? 1.0f : VC_SLEEP_ALPHA;
		const float step = s_vc.dt / 0.2f;
		cd->alpha = cd->alpha < goal ? fminf(goal, cd->alpha + step) : fmaxf(goal, cd->alpha - step);
		if (s_vc.dt <= 0.0f) {
			cd->alpha = goal;
		}
	}

	/* Draw with the card as the current panel. */
	s_vc.pose = *out;
	memcpy(s_vc.ax, ax, sizeof ax);
	memcpy(s_vc.ay, ay, sizeof ay);
	memcpy(s_vc.az, az, sizeof az);
	s_vc.cur_place = &cd->place;
	s_vc.cur_w_m = VC_CARD_W_M;
	s_vc.cur_h_m = VC_CARD_H_M;
	s_vc.cur_w_px = (float)width;
	s_vc.cur_h_px = (float)height;
	const float lap_alpha = s_vc.alpha;
	s_vc.alpha = cd->alpha;
	const int n = vc_render_panel(cmd, card == VC_CARD_VOICE ? AERON_XR_PANEL_CARD2 : AERON_XR_PANEL_CARD, cd->list,
								  card == VC_CARD_VOICE ? vc_voice_draw : vc_card_draw, vc_cut_gloves, views, out,
								  width, height,
								  VC_CARD_W_M, VC_CARD_H_M, &cd->hash, &cd->hash_valid);
	s_vc.alpha = lap_alpha;
	return n;
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
			s_vc.lap.hover_side[h] = 0;
		}
		vc_grab_end(&s_vc.lap);
		s_vc.awake = 0;
	}
	for (int c = 0; c < VC_CARDS; ++c) {
		VcCard* cd = &s_vc.cards[c];
		if (!cd->drawn) {
			for (int h = 0; h < 2; ++h) {
				cd->place.hover_side[h] = 0;
			}
			vc_grab_end(&cd->place);
			cd->awake = 0;
		}
	}
	for (int h = 0; h < 2; ++h) {
		Aeron_XrCaptureGrip(h, s_vc.grip_capture[h]);
		s_vc.grip_capture[h] = 0;
	}
}

void XwaVrConsole_Shutdown(void) {
	if (s_vc.list) {
		AeronDrawList_Destroy(s_vc.list);
	}
	for (int c = 0; c < VC_CARDS; ++c) {
		for (int e = 0; e < 2; ++e) {
			if (s_vc.cards[c].list[e]) {
				AeronDrawList_Destroy(s_vc.cards[c].list[e]);
			}
		}
	}
	if (s_vc.list_r) {
		AeronDrawList_Destroy(s_vc.list_r);
	}
	for (int i = 0; i < AERON_XR_PANEL_COUNT; ++i) {
		if (s_base[i].list) {
			AeronDrawList_Destroy(s_base[i].list);
		}
		if (s_base[i].rt) {
			Aeron_DestroyRenderTarget(s_base[i].rt);
		}
	}
	memset(s_base, 0, sizeof s_base);
	for (int e = 0; e < 2; ++e) {
		for (int h = 0; h < 2; ++h) {
			if (s_mask.tex[e][h]) {
				Aeron_DestroyTexture(s_mask.tex[e][h]);
				s_mask.tex[e][h] = NULL;
			}
		}
	}
	if (s_vc.font_state == 1) {
		AeronFontAtlas_Release(&s_vc.font);
	}
	memset(&s_vc, 0, sizeof s_vc);
}

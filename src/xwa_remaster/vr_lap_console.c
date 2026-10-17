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
#include "xwa_runtime/runtime/vr_buttons.h"

#include <ctype.h>

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
#define VC_GRAB_FORGIVE (1.15f * 1.10f) /* zones 15 %, then 10 % larger than the original tuning */
#define VC_GRAB_X_M (0.06f * VC_GRAB_FORGIVE)
#define VC_GRAB_Y_M (0.05f * VC_GRAB_FORGIVE)
#define VC_GRAB_FRONT_M (0.09f * VC_GRAB_FORGIVE)
#define VC_GRAB_BACK_M (0.07f * VC_GRAB_FORGIVE)
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
	float font_size; /* 0: the default for its height */
	float radius;    /* corner radius, 0: the default */
	float border_px; /* 0: the default */
} VcButton;

/* The console's own buttons (not game actions): its help page. */
#define VC_LOCAL_HELP ((XwaVrConsoleAction)1000)
#define VC_LOCAL_BACK ((XwaVrConsoleAction)1001)
#define VC_LOCAL_CARD ((XwaVrConsoleAction)1002) /* arg: the card to show or hide */

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
	/* The console's colours from the HUD colour (vc_hud_colors). */
	float hud[3], hud_dark[3], hud_pale[3];
	/* The help page ("?"): two big buttons that show or hide the cards. */
	int help;
	int card_override[VC_CARDS]; /* 0 the setting, 1 shown, 2 hidden */
	/* Last frame's grip_capture (another panel or the stick had the grip:
	 * squeezing it there is a grab, not the quick wheel). */
	int grip_capture_prev[2];
} s_vc;

/* The quick wheel (see XwaVrConsole_CardFrame): held open with the grip of
 * the hand that has the game's LEFT controls, steered by where that hand
 * points, its trigger picks. Drawn on the voice card's layer while open
 * (the console and both cards are hidden meanwhile). */
#define VW_MAX_ITEMS 10
#define VW_CENTER (-2) /* hover: the centre button */
static struct {
	int open;
	int hand;      /* physical hand (0 left, 1 right) */
	int grip_held; /* its grip, with hysteresis */
	int level;     /* 0 the four categories, else 1 + the category */
	int hover;     /* item, VW_CENTER or -1 */
	int armed;     /* the trigger was let go since the last pick */
	int center_lock; /* just went back: the centre does nothing until left */
	int drawn;     /* updated this frame (the flight renderer ran) */
	float anim;    /* 0..1 grow-in after opening or changing level */
	float fwd[3];  /* smoothed head-yaw forward, level */
	float center[3], ax[3], ay[3], az[3], q[4];
	int hit;       /* the aim ray meets the wheel */
	float hit_px, hit_py;
	float ray_start[3], ray_end[3];
	VcButton items[VW_MAX_ITEMS];
	int item_count;
	VcButton center_button;
	float flash[VW_MAX_ITEMS + 1]; /* [VW_MAX_ITEMS]: the centre */
	AeronDrawList2D* list[2];
	uint64_t hash;
	int hash_valid;
	int logged;
} s_vw;
static void vw_close(void);

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

static int vc_card_visible(int card);
static void vc_grab_end(VcPlace* pl);

static void vc_push(XwaVrConsoleAction action, int arg) {
	if (action == VC_LOCAL_HELP || action == VC_LOCAL_BACK) {
		s_vc.help = action == VC_LOCAL_HELP;
		return;
	}
	if (action == VC_LOCAL_CARD) {
		if (arg >= 0 && arg < VC_CARDS) {
			const int show = !vc_card_visible(arg);
			s_vc.card_override[arg] = show ? 1 : 2;
			if (show) {
				/* Back at its spot beside the player, bright. */
				vc_grab_end(&s_vc.cards[arg].place);
				s_vc.cards[arg].place.custom = 0;
				s_vc.cards[arg].awake = 1;
			}
		}
		return;
	}
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

	if (s_vc.help) {
		/* Help: the two reference cards, on two big buttons; BACK where the
		 * "?" was. */
		vc_label(34.0f, 22.0f, "HELP", 28, 0, 1, 0);
		VcButton* back = vc_button(VC_W - 34.0f - 150.0f, 14.0f, 150.0f, 52.0f, "BACK", VC_LOCAL_BACK, 0);
		back->font_size = 24.0f;
		static const char* const l[2] = { "LIST OF VOICE\nRECOGNITION COMMANDS", "SHOW VR-CONTROLLER\nMAPPING" };
		static const int card[2] = { VC_CARD_VOICE, VC_CARD_BUTTONS };
		const float top = 84.0f, gap = 30.0f, w = (VC_W - 68.0f - gap) * 0.5f, h = VC_H - top - 34.0f;
		for (int i = 0; i < 2; ++i) {
			VcButton* b = vc_button(34.0f + (w + gap) * (float)i, top, w, h, l[i], VC_LOCAL_CARD, card[i]);
			b->font_size = 40.0f;
			b->lit = vc_card_visible(card[i]);
			b->sub = b->lit ? "shown beside you - press to hide" : "press to show it beside you";
		}
		return;
	}
	{
		/* Help ("?"), top right. */
		VcButton* help = vc_button(VC_W - 34.0f - 64.0f, 14.0f, 64.0f, 44.0f, "?", VC_LOCAL_HELP, 0);
		help->font_size = 30.0f;
	}

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
			static const char* const l[4] = { "NEAREST\nENEMY", "ATTACKING\nME", "ATTACKING\nMY TARGET",
											  "MATCH\nSPEED" };
			static const XwaVrConsoleAction a[4] = { XWA_VRC_TARGET_NEAREST_ENEMY, XWA_VRC_TARGET_ATTACKER,
													 XWA_VRC_TARGET_TARGETS_ATTACKER, XWA_VRC_MATCH_SPEED };
			vc_row(x, 62.0f, w, 66.0f, 4, l, a, row);
			row[2]->enabled = row[3]->enabled = st->has_target;
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
				row[i]->lit = st->memory_set[i] == 1;
				row[i]->sub = st->memory_set[i] == 1   ? "set"
							  : st->memory_set[i] == 2 ? "other region"
													   : "hold = store";
			}
		}
		{
			static const char* const l[4] = { "NEWEST", "OBJECTIVE", "INCOMING", "CLEAR" };
			static const XwaVrConsoleAction a[4] = { XWA_VRC_TARGET_NEWEST, XWA_VRC_TARGET_OBJECTIVE,
													 XWA_VRC_TARGET_INCOMING, XWA_VRC_TARGET_CLEAR };
			vc_row(x, 304.0f, w, 48.0f, 4, l, a, row);
			row[3]->enabled = st->has_target;
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
			static const char* const l[4] = { "GUNNER", "GUNNERS", "FLARE", "HYPERSPACE" };
			static const XwaVrConsoleAction a[4] = { XWA_VRC_GUNNER, XWA_VRC_GUNNER_ATTACK, XWA_VRC_COUNTERMEASURE,
													 XWA_VRC_HYPERSPACE };
			vc_row(x, 388.0f, w, 52.0f, 4, l, a, row);
			/* Gunners: at my target; held, the game's own toggle (back to
			 * defending the ship when they are on it). */
			row[1]->enabled = st->has_turrets;
			row[1]->hold_action = XWA_VRC_AUTO_GUNNER;
			row[1]->sub = !st->has_turrets ? NULL : st->gunners_on_target ? "on target" : "my target";
			row[1]->lit = st->gunners_on_target;
			/* Away from a hyperspace buoy the button finds one (the N key). */
			row[3]->hyper = st->hyper_ready;
			if (st->hyper_ready) {
				row[3]->sub = "ready";
			} else {
				row[3]->label = "NAV BUOY";
				row[3]->action = XWA_VRC_TARGET_NAV;
				row[3]->sub = "target next";
			}
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
	if (s_vw.open && s_vw.drawn && hand == s_vw.hand) {
		/* The quick wheel's pointer: bright. */
		memcpy(start, s_vw.ray_start, sizeof s_vw.ray_start);
		memcpy(end, s_vw.ray_end, sizeof s_vw.ray_end);
		return 2;
	}
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

/* Holo cyan from the console design: the cards keep it. */
#define VC_CYAN 0.498f, 0.910f, 1.0f
/* The console itself wears the HUD colour (vc_hud_colors): the holo tone,
 * a darker one for the plate and a pale one for the text. */
#define VC_HUD s_vc.hud[0], s_vc.hud[1], s_vc.hud[2]
#define VC_HUD_DARK s_vc.hud_dark[0], s_vc.hud_dark[1], s_vc.hud_dark[2]
#define VC_HUD_PALE s_vc.hud_pale[0], s_vc.hud_pale[1], s_vc.hud_pale[2]

static void vc_hud_colors(void) {
	static const float cyan[3] = { VC_CYAN };
	const uint32_t argb = s_vc.state.hud_argb;
	float c[3];
	float peak = 0.0f;
	for (int i = 0; i < 3; ++i) {
		c[i] = (float)((argb >> (16 - 8 * i)) & 255u) / 255.0f;
		peak = fmaxf(peak, c[i]);
	}
	/* The HUD's frame art is pure white, so the HUD shows exactly this
	 * colour (0x7020C0 for the default option): the console uses it as is,
	 * only the darker options lifted to the default's brightness. */
	const float lift = peak > 0.05f && peak < 0.753f ? 0.753f / peak : 1.0f;
	for (int i = 0; i < 3; ++i) {
		const float h = (argb >> 24) != 0 && peak > 0.05f ? c[i] * lift : cyan[i];
		s_vc.hud[i] = h;
		s_vc.hud_dark[i] = h * 0.75f;                  /* was 0.157, 0.549, 0.745 */
		s_vc.hud_pale[i] = 0.55f * h + 0.45f * 0.92f;  /* was 0.62, 0.85, 0.92 */
	}
}
#define VC_AMBER 1.0f, 0.710f, 0.278f
/* Hyperspace ready: orange-red. */
#define VC_HYPER 1.0f, 0.42f, 0.16f

/* A button's look: `hold` (0..1) fills a danger button while it is held. */
static void vc_draw_button_look(const VcButton* b, int hover, int pressed, float flash, float hold);

static void vc_draw_button(int i) {
	const VcButton* b = &s_vc.buttons[i];
	int hover = 0, pressed = 0;
	for (int h = 0; h < 2; ++h) {
		hover |= s_vc.hands[h].over_button == i;
		pressed |= s_vc.hands[h].press_button == i;
	}
	const float hold = pressed && b->danger && b->hold_action != XWA_VRC_NONE
						   ? fminf(1.0f, s_vc.hands[0].press_button == i ? s_vc.hands[0].press_time / VC_EJECT_HOLD_S
																		  : s_vc.hands[1].press_time / VC_EJECT_HOLD_S)
						   : 0.0f;
	vc_draw_button_look(b, hover, pressed, s_vc.flash[i] > 0.0f ? 1.0f : 0.0f, hold);
}

static void vc_draw_button_look(const VcButton* b, int hover, int pressed, float flash, float hold) {
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
		vc_pma(fill, VC_AMBER, 0.12f + 0.45f * hold + 0.1f * hover);
		vc_pma(border, VC_AMBER, 0.85f);
	} else {
		const float on = b->lit ? 0.30f : b->level ? 0.16f : 0.08f;
		vc_pma(fill, VC_HUD, on + 0.22f * hover + 0.25f * (pressed || flash));
		if (hover) {
			vc_pma(border, 1.0f, 1.0f, 1.0f, 1.0f);
		} else {
			vc_pma(border, VC_HUD, b->lit ? 1.0f : 0.62f);
		}
	}
	const float radius = b->radius > 0.0f ? b->radius : 7.0f;
	if (hyper_ready) {
		float glow[4];
		const float transparent[4] = { 0, 0, 0, 0 };
		vc_pma(glow, VC_HYPER, 0.25f + 0.5f * pulse);
		vc_rrect(b->x - 8.0f, b->y - 8.0f, b->w + 16.0f, b->h + 16.0f, radius + 6.0f, 8.0f, 14.0f, transparent, glow);
	} else if (b->enabled && (b->lit || hover || pressed || flash > 0.0f)) {
		float glow[4];
		const float transparent[4] = { 0, 0, 0, 0 };
		vc_pma(glow, VC_HUD, b->lit ? 0.45f : hover ? 0.55f : 0.3f);
		vc_rrect(b->x - 6.0f, b->y - 6.0f, b->w + 12.0f, b->h + 12.0f, radius + 5.0f, 6.0f, 10.0f, transparent, glow);
	}
	vc_rrect(b->x, b->y, b->w, b->h, radius, b->border_px > 0.0f ? b->border_px : 2.0f, 1.0f, fill, border);

	const float tr = !b->enabled ? 0.45f : hyper_ready ? 1.0f : b->danger ? 1.0f : 0.85f;
	const float tg = !b->enabled ? 0.52f : hyper_ready ? 0.90f : b->danger ? 0.92f : 0.97f;
	const float tb = !b->enabled ? 0.55f : hyper_ready ? 0.78f : b->danger ? 0.80f : 1.0f;
	const float text_a = b->lit || pressed ? 1.0f : 0.9f;
	float size = b->font_size > 0.0f ? b->font_size : b->h >= 60.0f ? 22.0f : 20.0f;
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
	if (line2[0] && b->sub) {
		vc_text(b->x + b->w * 0.5f, b->y + b->h * 0.5f - size * 1.5f, line1, size, 1, tr, tg, tb, text_a);
		vc_text(b->x + b->w * 0.5f, b->y + b->h * 0.5f - size * 0.45f, line2, size, 1, tr, tg, tb, text_a);
		vc_text(b->x + b->w * 0.5f, b->y + b->h * 0.5f + size * 0.9f, b->sub, fmaxf(15.0f, size * 0.5f), 1, tr, tg, tb,
				0.65f);
	} else if (line2[0]) {
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
		vc_pma(bar, VC_HUD, pl->grab_side == side ? 0.85f : hot ? 0.6f : 0.32f);
		vc_pma(rim, VC_HUD, hot ? 1.0f : 0.75f);
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
	vc_hud_colors();
	float fill[4], border[4], glow[4];
	/* Plate: faint tint, bright rim, soft halo. */
	vc_pma(glow, VC_HUD, 0.25f);
	vc_rrect(4.0f, 4.0f, VC_W - 8.0f, VC_H - 8.0f, 30.0f, 8.0f, 14.0f, transparent, glow);
	vc_pma(fill, VC_HUD_DARK, 0.10f);
	vc_pma(border, VC_HUD, 0.6f);
	vc_rrect(10.0f, 10.0f, VC_W - 20.0f, VC_H - 20.0f, 26.0f, 2.0f, 1.0f, fill, border);
	vc_draw_handles(&s_vc.lap);
	if (!s_vc.help) {
		vc_draw_energy(34.0f, 406.0f, 470.0f);
	}
	/* Scan lines, fixed to the panel. */
	for (float y = 14.0f; y < VC_H - 14.0f; y += 6.0f) {
		float line[4];
		vc_pma(line, VC_HUD, 0.025f);
		AeronDrawList_AddFill(s_vc.cur_list, 14.0f, y, VC_W - 28.0f, 2.0f, line, AERON_BLIT2D_BLEND_PMA, NULL);
	}
	/* Column dividers. */
	for (int i = 0; i < 2 && !s_vc.help; ++i) {
		float line[4];
		vc_pma(line, VC_HUD, 0.35f);
		const float x = i == 0 ? 520.0f : 1015.0f;
		AeronDrawList_AddFill(s_vc.cur_list, x, 40.0f, 2.0f, VC_H - 80.0f, line, AERON_BLIT2D_BLEND_PMA, NULL);
	}
	/* Rules between the stacked sections (targeting / weapons, wingmen /
	 * ship). */
	if (!s_vc.help) {
		float line[4];
		vc_pma(line, VC_HUD, 0.35f);
		AeronDrawList_AddFill(s_vc.cur_list, 538.0f, s_vc.divider_y, 460.0f, 2.0f, line, AERON_BLIT2D_BLEND_PMA, NULL);
		AeronDrawList_AddFill(s_vc.cur_list, 1032.0f, 274.0f, 470.0f, 2.0f, line, AERON_BLIT2D_BLEND_PMA, NULL);
	}
	for (int i = 0; i < s_vc.label_count; ++i) {
		const VcLabel* l = &s_vc.labels[i];
		if (l->section) {
			vc_text(l->x, l->y, l->text, (float)l->size, l->align, VC_HUD, 1.0f);
		} else if (l->dim) {
			vc_text(l->x, l->y, l->text, (float)l->size, l->align, 0.42f, 0.48f, 0.50f, 0.8f);
		} else {
			vc_text(l->x, l->y, l->text, (float)l->size, l->align, VC_HUD_PALE, 0.95f);
		}
	}
	for (int i = 0; i < s_vc.button_count; ++i) {
		vc_draw_button(i);
	}
}

/* Per-eye overlay of the lap console: the glove cut-outs, then the cursors. */
static void vc_cut_gloves(void);
static void vc_console_overlay(void) {
	vc_hud_colors();
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
			vc_pma(ring, VC_HUD, 0.9f);
			vc_rrect(hand->cx - 9.0f, hand->cy - 9.0f, 18.0f, 18.0f, 9.0f, 3.0f, 1.5f, c, ring);
		}
	}
}

/* ---- Frame ------------------------------------------------------------ */

static int vc_ensure(void);
static int vc_enabled(void);
static int vc_card_enabled(void);
static int vw_enabled(void);

void XwaVrConsole_BeginFrame(float dt_seconds) {
	s_vc.dt = dt_seconds > 0.0f && dt_seconds < 0.25f ? dt_seconds : 0.0f;
	s_vc.clock = fmodf(s_vc.clock + s_vc.dt, 1000.0f);
	s_vc.drawn = 0;
	s_vw.drawn = 0;
	for (int c = 0; c < VC_CARDS; ++c) {
		s_vc.cards[c].drawn = 0;
	}
	/* Load the font here, between frames, rather than while the flight
	 * renderer is recording its command buffer. */
	if (s_vc.state.valid && s_vc.font_state == 0 && (vc_enabled() || vc_card_enabled() || vw_enabled()) &&
		Aeron_XrIsPresenting()) {
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
	if (s_vw.open) {
		return 0; /* hidden while the quick wheel is open */
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


/* ---- Quick wheel ------------------------------------------------------- */

/* Held open with the grip of the hand that has the game's LEFT controls
 * (squeezed away from a panel's handle or the flight stick). It sits
 * VW_AHEAD_M in front of that controller, level and facing the player's
 * head yaw, and follows the controller's position but not its turning:
 * where the controller points (a beam) picks a slice, the trigger takes
 * it and the wheel stays open for the next one; letting go of the grip
 * closes it. Level 1: four categories round CONFIRM (Space); a category
 * opens its commands round BACK. Drawn on the voice card's layer (the
 * card's 1024 x 768 image, the wheel a circle in it). */
#define VW_W 1024.0f
#define VW_H 768.0f
#define VW_H_M 0.26f
#define VW_W_M (VW_H_M * VW_W / VW_H)
#define VW_PX_PER_M (VW_H / VW_H_M)
#define VW_AHEAD_M 0.15f
#define VW_PLATE_R 370.0f     /* px */
#define VW_CENTER_R 84.0f     /* px: the centre button, also where it is picked */
#define VW_SELECT_MAX_M 0.40f /* farther from the centre: nothing is picked */
#define VW_HYST_DEG 5.0f      /* a slice is kept this far past its edge */
#define VW_TRIGGER_ON 0.6f
#define VW_TRIGGER_OFF 0.3f
#define VW_GROW_S 0.09f
#define VW_PI 3.14159265f

enum { VW_CAT_SHIP, VW_CAT_ENERGY, VW_CAT_TARGETING, VW_CAT_WINGMEN, VW_CATS };
static const char* const k_vw_cat_label[VW_CATS] = { "SHIP &\nWEAPONS", "ENERGY", "TARGETING", "WINGMEN" };
static const char* const k_vw_cat_name[VW_CATS] = { "ship & weapons", "energy", "targeting", "wingmen" };
static const char* const k_vw_levels[5] = { "off", "low", "normal", "high", "max" };
static const char* const k_vw_balance[3] = { "front", "even", "rear" };

static int vw_enabled(void) {
	const char* env = getenv("XWA_XR_WHEEL");
	return !(env && (env[0] == '0' || env[0] == 'o' || env[0] == 'O'));
}

static void vw_close(void) {
	if (s_vw.open) {
		s_vw.open = 0;
		/* The voice card's image showed the wheel: draw the card again. */
		s_vc.cards[VC_CARD_VOICE].hash_valid = 0;
	}
	s_vw.hover = -1;
	s_vw.hit = 0;
}

static VcButton* vw_item(const char* label, XwaVrConsoleAction action, int arg) {
	if (s_vw.item_count >= VW_MAX_ITEMS) {
		static VcButton spare;
		memset(&spare, 0, sizeof spare);
		return &spare;
	}
	VcButton* b = &s_vw.items[s_vw.item_count++];
	memset(b, 0, sizeof *b);
	b->label = label;
	b->action = action;
	b->arg = arg;
	b->enabled = 1;
	return b;
}

static int vw_clamp_level(int level) { return level < 0 ? 0 : level > 4 ? 4 : level; }

/* The current level's items with their live state, laid out round the
 * centre: the first at 12 o'clock, then clockwise. */
static void vw_layout(void) {
	const XwaVrConsoleState* st = &s_vc.state;
	VcButton* b;
	s_vw.item_count = 0;
	switch (s_vw.level) {
		case 0:
			for (int c = 0; c < VW_CATS; ++c) {
				b = vw_item(k_vw_cat_label[c], XWA_VRC_NONE, c);
				b->font_size = 30.0f;
			}
			/* At a buoy with the jump ready: the ship slice pulses. */
			s_vw.items[VW_CAT_SHIP].hyper = st->hyper_ready;
			s_vw.items[VW_CAT_SHIP].sub = st->hyper_ready ? "hyperspace ready" : NULL;
			break;
		case 1 + VW_CAT_SHIP:
			b = vw_item("MATCH\nSPEED", XWA_VRC_MATCH_SPEED, 0);
			b->enabled = st->has_target;
			vw_item("CYCLE\nWARHEADS", XWA_VRC_CYCLE_WARHEAD, 0);
			vw_item("FIRE-LINK", XWA_VRC_FIRE_LINK, 0);
			vw_item("FLARE", XWA_VRC_COUNTERMEASURE, 0);
			b = vw_item("HYPERSPACE", XWA_VRC_HYPERSPACE, 0);
			b->hyper = st->hyper_ready;
			if (st->hyper_ready) {
				b->sub = "ready";
			} else {
				/* Away from a buoy: find one (the N key), as on the console. */
				b->label = "NAV BUOY";
				b->action = XWA_VRC_TARGET_NAV;
				b->sub = "target next";
			}
			b = vw_item("GUNNERS", XWA_VRC_GUNNER_ATTACK, 0);
			b->enabled = st->has_turrets;
			b->sub = !st->has_turrets ? NULL : st->gunners_on_target ? "on target" : "attack my target";
			b->lit = st->gunners_on_target;
			b = vw_item("BEAM\nWEAPON", XWA_VRC_BEAM_TOGGLE, 0);
			b->enabled = st->has_beam;
			b = vw_item("S-FOILS", XWA_VRC_SFOILS, 0);
			b->enabled = st->has_sfoils;
			b->sub = !st->has_sfoils ? NULL : st->sfoils_closed ? "closed" : "open";
			b->lit = st->has_sfoils && !st->sfoils_closed;
			break;
		case 1 + VW_CAT_ENERGY:
			/* Recharge rates step up one level a pick (MAX wraps to OFF, as
			 * the keys do). */
			b = vw_item("LASERS >\nSHIELDS", XWA_VRC_LASERS_TO_SHIELDS, 0);
			b->enabled = st->has_shields;
			b = vw_item("LASER\nRECHARGE", XWA_VRC_LASER_LEVEL, (vw_clamp_level(st->laser_level) + 1) % 5);
			b->sub = k_vw_levels[vw_clamp_level(st->laser_level)];
			b = vw_item("SHIELD\nRECHARGE", XWA_VRC_SHIELD_LEVEL, (vw_clamp_level(st->shield_level) + 1) % 5);
			b->enabled = st->has_shields;
			b->sub = st->has_shields ? k_vw_levels[vw_clamp_level(st->shield_level)] : NULL;
			b = vw_item("BEAM\nRECHARGE", XWA_VRC_BEAM_LEVEL, (vw_clamp_level(st->beam_level) + 1) % 5);
			b->enabled = st->has_beam;
			b->sub = st->has_beam ? k_vw_levels[vw_clamp_level(st->beam_level)] : NULL;
			b = vw_item("SHIELD\nBALANCE", XWA_VRC_SHIELD_BALANCE,
						((st->shield_balance < 0 || st->shield_balance > 2 ? 1 : st->shield_balance) + 1) % 3);
			b->enabled = st->has_shields;
			b->sub = st->has_shields && st->shield_balance >= 0 && st->shield_balance <= 2
						 ? k_vw_balance[st->shield_balance]
						 : NULL;
			vw_item("ENERGY\nPRESET 1", XWA_VRC_PRESET_RECALL, 0);
			vw_item("ENERGY\nPRESET 2", XWA_VRC_PRESET_RECALL, 1);
			b = vw_item("SHIELDS >\nLASERS", XWA_VRC_SHIELDS_TO_LASERS, 0);
			b->enabled = st->has_shields;
			break;
		case 1 + VW_CAT_TARGETING:
			vw_item("NEAREST\nENEMY", XWA_VRC_TARGET_NEAREST_ENEMY, 0);
			vw_item("NEXT\nTARGET", XWA_VRC_TARGET_NEXT, 0);
			vw_item("ATTACKING\nME", XWA_VRC_TARGET_ATTACKER, 0);
			b = vw_item("ATTACKING\nMY TARGET", XWA_VRC_TARGET_TARGETS_ATTACKER, 0);
			b->enabled = st->has_target;
			vw_item("INCOMING\nMISSILE", XWA_VRC_TARGET_INCOMING, 0);
			vw_item("OBJECTIVE", XWA_VRC_TARGET_OBJECTIVE, 0);
			vw_item("NEWEST", XWA_VRC_TARGET_NEWEST, 0);
			b = vw_item("NEXT\nSUBSYSTEM", XWA_VRC_SUBSYSTEM_NEXT, 0);
			b->enabled = st->has_target;
			vw_item("NAV BUOY", XWA_VRC_TARGET_NAV, 0);
			vw_item("PREVIOUS\nTARGET", XWA_VRC_TARGET_PREV, 0);
			break;
		case 1 + VW_CAT_WINGMEN:
			b = vw_item("ATTACK\nMY TARGET", XWA_VRC_WING_ATTACK, 0);
			b->enabled = st->has_target;
			vw_item("COVER ME", XWA_VRC_WING_COVER, 0);
			vw_item("EVADE", XWA_VRC_WING_EVADE, 0);
			vw_item("WAIT", XWA_VRC_WING_WAIT, 0);
			vw_item("GO AHEAD", XWA_VRC_WING_GO_AHEAD, 0);
			vw_item("RETURN\nHOME", XWA_VRC_WING_HOME, 0);
			vw_item("REINFORCE", XWA_VRC_WING_REINFORCE, 0);
			b = vw_item("IGNORE\nMY TARGET", XWA_VRC_WING_IGNORE, 0);
			b->enabled = st->has_target;
			break;
		default:
			s_vw.level = 0;
			break;
	}
	const int n = s_vw.item_count;
	const float ring = n <= 4 ? 222.0f : n <= 8 ? 266.0f : 278.0f;
	const float bw = n <= 4 ? 232.0f : n <= 8 ? 172.0f : 148.0f;
	const float bh = n <= 4 ? 124.0f : n <= 8 ? 90.0f : 80.0f;
	for (int i = 0; i < n; ++i) {
		const float a = (float)i * 2.0f * VW_PI / (float)n;
		VcButton* it = &s_vw.items[i];
		it->x = VW_W * 0.5f + ring * sinf(a) - bw * 0.5f;
		it->y = VW_H * 0.5f - ring * cosf(a) - bh * 0.5f;
		it->w = bw;
		it->h = bh;
		it->radius = 12.0f;
		if (it->font_size <= 0.0f) {
			it->font_size = n <= 8 ? 24.0f : 22.0f;
		}
	}
	VcButton* c = &s_vw.center_button;
	memset(c, 0, sizeof *c);
	c->x = VW_W * 0.5f - VW_CENTER_R;
	c->y = VW_H * 0.5f - VW_CENTER_R;
	c->w = c->h = 2.0f * VW_CENTER_R;
	c->radius = VW_CENTER_R;
	c->enabled = 1;
	c->font_size = 28.0f;
	if (s_vw.level == 0) {
		c->label = "CONFIRM";
		c->sub = "space";
		c->action = XWA_VRC_CONFIRM;
	} else {
		c->label = "BACK";
		c->sub = k_vw_cat_name[s_vw.level - 1];
	}
}

/* The slice the ray picks at (u, v) metres from the centre (+u right, +v
 * up), the centre, or -1. Keeps the current pick a little past its edge. */
static int vw_pick(float u, float v) {
	const float r = sqrtf(u * u + v * v);
	const float center_m = VW_CENTER_R / VW_PX_PER_M;
	if (r < (s_vw.hover == VW_CENTER ? center_m * 1.2f : center_m)) {
		return VW_CENTER;
	}
	const int n = s_vw.item_count;
	if (r > VW_SELECT_MAX_M || n <= 0) {
		return -1;
	}
	const float step = 360.0f / (float)n;
	float a = atan2f(u, v) * 180.0f / VW_PI;
	if (a < 0.0f) {
		a += 360.0f;
	}
	int pick = (int)floorf((a + 0.5f * step) / step) % n;
	if (s_vw.hover >= 0 && s_vw.hover < n && pick != s_vw.hover) {
		float d = fmodf(fabsf(a - step * (float)s_vw.hover), 360.0f);
		if (d > 180.0f) {
			d = 360.0f - d;
		}
		if (d < 0.5f * step + VW_HYST_DEG) {
			pick = s_vw.hover;
		}
	}
	return pick;
}

/* The trigger on the current pick. */
static void vw_activate(void) {
	const int hand = s_vw.hand;
	if (s_vw.hover == VW_CENTER) {
		if (s_vw.center_lock) {
			return; /* just came back here: point away first */
		}
		if (s_vw.level == 0) {
			vc_push(XWA_VRC_CONFIRM, 0);
			s_vw.flash[VW_MAX_ITEMS] = VC_FLASH_S;
			Aeron_XrHapticPulse(hand, 0.6f, 30);
		} else {
			s_vw.level = 0;
			s_vw.anim = fminf(s_vw.anim, 0.6f);
			s_vw.center_lock = 1;
			Aeron_XrHapticPulse(hand, 0.4f, 25);
		}
		return;
	}
	if (s_vw.hover < 0 || s_vw.hover >= s_vw.item_count) {
		return;
	}
	const VcButton* b = &s_vw.items[s_vw.hover];
	if (s_vw.level == 0) {
		s_vw.level = 1 + b->arg;
		s_vw.anim = fminf(s_vw.anim, 0.6f);
		s_vw.hover = -1;
		Aeron_XrHapticPulse(hand, 0.4f, 25);
		return;
	}
	if (!b->enabled || b->action == XWA_VRC_NONE) {
		Aeron_XrHapticPulse(hand, 0.1f, 10);
		return;
	}
	vc_push(b->action, b->arg);
	s_vw.flash[s_vw.hover] = VC_FLASH_S;
	Aeron_XrHapticPulse(hand, 0.6f, 30);
}

/* Once a frame (from the voice card's frame, after the console and the
 * button card had their turn with the grips): opens, steers and closes. */
static void vw_update(const AeronXrView views[2]) {
	static const float minus_z[3] = { 0.0f, 0.0f, -1.0f };
	const int hand = Aeron_XrLeftHanded() ? 1 : 0;
	AeronXrHand xh;
	memset(&xh, 0, sizeof xh);
	const int tracked = Aeron_XrGetHand(hand, &xh);
	s_vw.drawn = 1;
	if (s_vw.open && s_vw.hand != hand) {
		vw_close();
	}
	const int was_held = s_vw.grip_held;
	s_vw.grip_held = tracked && (was_held ? xh.grip > VC_GRIP_OFF : xh.grip >= VC_GRIP_ON);
	if (!s_vc.state.valid || !vw_enabled() || s_vc.font_state != 1 || !tracked) {
		vw_close();
		return;
	}
	if (!s_vw.open) {
		if (!s_vw.grip_held || was_held) {
			return;
		}
		/* A squeeze on a panel's handle or at the flight stick is theirs. */
		int owned = s_vc.grip_capture[hand] || s_vc.grip_capture_prev[hand] || s_vc.lap.grab_hand == hand + 1 ||
					s_vc.lap.hover_side[hand];
		for (int c = 0; c < VC_CARDS; ++c) {
			owned |= s_vc.cards[c].place.grab_hand == hand + 1 || s_vc.cards[c].place.hover_side[hand];
		}
		if (owned) {
			return;
		}
		s_vw.open = 1;
		s_vw.hand = hand;
		s_vw.level = 0;
		s_vw.hover = -1;
		s_vw.armed = xh.trigger < VW_TRIGGER_OFF; /* a held trigger must be let go first */
		s_vw.center_lock = 0;
		s_vw.anim = 0.0f;
		s_vw.fwd[0] = s_vw.fwd[1] = s_vw.fwd[2] = 0.0f;
		memset(s_vw.flash, 0, sizeof s_vw.flash);
		s_vw.hash_valid = 0;
		Aeron_XrHapticPulse(hand, 0.3f, 20);
		if (!s_vw.logged) {
			Aeron_LogInfo("xwa.remaster", "quick wheel: %s grip, %.2f m ahead, %.2f m wide", hand ? "right" : "left",
						  (double)VW_AHEAD_M, (double)(2.0f * VW_PLATE_R / VW_PX_PER_M));
			s_vw.logged = 1;
		}
	} else if (!s_vw.grip_held) {
		vw_close();
		return;
	}
	s_vw.anim = s_vc.dt > 0.0f ? fminf(1.0f, s_vw.anim + s_vc.dt / VW_GROW_S) : 1.0f;
	for (int i = 0; i <= VW_MAX_ITEMS; ++i) {
		if (s_vw.flash[i] > 0.0f) {
			s_vw.flash[i] -= s_vc.dt;
		}
	}

	/* Facing the head's yaw (level), eased so a glance does not swing it. */
	{
		float f[3];
		vc_rotate(views[0].orientation, minus_z, f);
		const float len = sqrtf(f[0] * f[0] + f[2] * f[2]);
		const int fresh = s_vw.fwd[0] == 0.0f && s_vw.fwd[2] == 0.0f;
		if (len > 0.2f) {
			f[0] /= len;
			f[2] /= len;
			const float k = fresh || s_vc.dt <= 0.0f ? 1.0f : fminf(1.0f, s_vc.dt / 0.12f);
			s_vw.fwd[0] += (f[0] - s_vw.fwd[0]) * k;
			s_vw.fwd[2] += (f[2] - s_vw.fwd[2]) * k;
		} else if (fresh) {
			s_vw.fwd[2] = -1.0f; /* looking straight down: straight ahead */
		}
		const float l = sqrtf(s_vw.fwd[0] * s_vw.fwd[0] + s_vw.fwd[2] * s_vw.fwd[2]);
		if (l > 1e-4f) {
			s_vw.fwd[0] /= l;
			s_vw.fwd[2] /= l;
		} else {
			s_vw.fwd[0] = 0.0f;
			s_vw.fwd[2] = -1.0f;
		}
		s_vw.fwd[1] = 0.0f;
	}
	const float az[3] = { -s_vw.fwd[0], 0.0f, -s_vw.fwd[2] }; /* towards the player */
	const float ax[3] = { az[2], 0.0f, -az[0] };
	const float ay[3] = { 0.0f, 1.0f, 0.0f };
	memcpy(s_vw.az, az, sizeof az);
	memcpy(s_vw.ax, ax, sizeof ax);
	memcpy(s_vw.ay, ay, sizeof ay);
	vc_basis_quat(ax, ay, az, s_vw.q);
	for (int k = 0; k < 3; ++k) {
		s_vw.center[k] = xh.position[k] + s_vw.fwd[k] * VW_AHEAD_M;
	}

	/* Where the controller points. */
	vw_layout();
	float d[3];
	vc_rotate(xh.orientation, minus_z, d);
	for (int k = 0; k < 3; ++k) {
		s_vw.ray_start[k] = xh.position[k] + d[k] * VC_TIP_M;
	}
	int hover = -1;
	s_vw.hit = 0;
	const float dn = vc_dot(d, az);
	if (dn < -0.15f) {
		const float rel[3] = { s_vw.center[0] - xh.position[0], s_vw.center[1] - xh.position[1],
							   s_vw.center[2] - xh.position[2] };
		const float t = vc_dot(rel, az) / dn;
		if (t > 0.0f && t < 1.5f) {
			float off[3];
			for (int k = 0; k < 3; ++k) {
				s_vw.ray_end[k] = xh.position[k] + d[k] * t;
				off[k] = s_vw.ray_end[k] - s_vw.center[k];
			}
			const float u = vc_dot(off, ax), v = vc_dot(off, ay);
			s_vw.hit = 1;
			s_vw.hit_px = (u / VW_W_M + 0.5f) * VW_W;
			s_vw.hit_py = (0.5f - v / VW_H_M) * VW_H;
			hover = vw_pick(u, v);
		}
	}
	if (!s_vw.hit) {
		for (int k = 0; k < 3; ++k) {
			s_vw.ray_end[k] = s_vw.ray_start[k] + d[k] * 0.25f;
		}
	}
	if (hover != s_vw.hover) {
		if (hover != -1) {
			Aeron_XrHapticPulse(hand, 0.25f, 25); /* a tick per slice */
		}
		s_vw.hover = hover;
	}
	if (s_vw.hover != VW_CENTER) {
		s_vw.center_lock = 0;
	}

	/* The trigger picks; it must be let go before it picks again. */
	if (!s_vw.armed) {
		s_vw.armed = xh.trigger < VW_TRIGGER_OFF;
	} else if (xh.trigger >= VW_TRIGGER_ON) {
		s_vw.armed = 0;
		vw_activate();
	}
	vw_layout(); /* the level may have changed */
}

static void vw_draw(void) {
	const float cx = VW_W * 0.5f, cy = VW_H * 0.5f, R = VW_PLATE_R;
	const float transparent[4] = { 0, 0, 0, 0 };
	float fill[4], border[4], glow[4], line[4];
	vc_hud_colors();
	/* The lap console's plate, round. */
	vc_pma(glow, VC_HUD, 0.25f);
	vc_rrect(cx - R - 6.0f, cy - R - 6.0f, 2.0f * R + 12.0f, 2.0f * R + 12.0f, R + 6.0f, 8.0f, 12.0f, transparent, glow);
	vc_pma(fill, VC_HUD_DARK, 0.16f);
	vc_pma(border, VC_HUD, 0.6f);
	vc_rrect(cx - R, cy - R, 2.0f * R, 2.0f * R, R, 2.0f, 1.0f, fill, border);
	vc_pma(line, VC_HUD, 0.025f);
	for (float y = cy - R + 10.0f; y < cy + R - 10.0f; y += 6.0f) {
		const float dy = y + 1.0f - cy;
		const float half = sqrtf(fmaxf(0.0f, (R - 8.0f) * (R - 8.0f) - dy * dy));
		if (half > 4.0f) {
			AeronDrawList_AddFill(s_vc.cur_list, cx - half, y, 2.0f * half, 2.0f, line, AERON_BLIT2D_BLEND_PMA, NULL);
		}
	}
	/* A faint track inside the rim, lit along the picked slice. */
	const float track = R - 16.0f;
	vc_pma(border, VC_HUD, 0.25f);
	vc_rrect(cx - track - 1.0f, cy - track - 1.0f, 2.0f * track + 2.0f, 2.0f * track + 2.0f, track + 1.0f, 2.0f, 1.0f,
			 transparent, border);
	const int n = s_vw.item_count;
	if (s_vw.hover >= 0 && s_vw.hover < n) {
		const float step = 2.0f * VW_PI / (float)n;
		const float a0 = ((float)s_vw.hover - 0.5f) * step + 0.02f, a1 = ((float)s_vw.hover + 0.5f) * step - 0.02f;
		const int segs = 24;
		vc_pma(line, VC_HUD, 0.9f);
		for (int i = 0; i < segs; ++i) {
			const float b0 = a0 + (a1 - a0) * (float)i / (float)segs;
			const float b1 = a0 + (a1 - a0) * (float)(i + 1) / (float)segs;
			AeronDrawList_AddLine(s_vc.cur_list, cx + track * sinf(b0), cy - track * cosf(b0), cx + track * sinf(b1),
								  cy - track * cosf(b1), 9.0f, line, AERON_BLIT2D_BLEND_PMA, NULL);
		}
	}
	for (int i = 0; i < n; ++i) {
		vc_draw_button_look(&s_vw.items[i], s_vw.hover == i, 0, s_vw.flash[i] > 0.0f ? 1.0f : 0.0f, 0.0f);
	}
	vc_draw_button_look(&s_vw.center_button, s_vw.hover == VW_CENTER && !s_vw.center_lock, 0,
						s_vw.flash[VW_MAX_ITEMS] > 0.0f ? 1.0f : 0.0f, 0.0f);
}

/* Per eye: where the beam lands, then the gloves cut out (the hand is in
 * front of the wheel). */
static void vw_overlay(void) {
	vc_hud_colors();
	if (s_vw.hit) {
		float c[4], ring[4];
		vc_pma(c, 1.0f, 1.0f, 1.0f, 0.95f);
		vc_pma(ring, VC_HUD, 0.9f);
		vc_rrect(s_vw.hit_px - 10.0f, s_vw.hit_py - 10.0f, 20.0f, 20.0f, 10.0f, 3.0f, 1.5f, c, ring);
	}
	vc_cut_gloves();
}

static int vw_frame(AeronCommandBuffer* cmd, int width, int height, const AeronXrView views[2],
					AeronXrPanel out[2]) {
	if (!vc_ensure()) {
		return 0;
	}
	for (int e = 0; e < 2; ++e) {
		if (!s_vw.list[e] && !(s_vw.list[e] = AeronDrawList_Create(0))) {
			return 0;
		}
	}
	/* Grows in (and pops on a level change). */
	const float t = s_vw.anim;
	const float ease = t * t * (3.0f - 2.0f * t);
	const float scale = 0.7f + 0.3f * ease;
	memset(out, 0, sizeof *out);
	memcpy(out->position, s_vw.center, sizeof s_vw.center);
	memcpy(out->orientation, s_vw.q, sizeof s_vw.q);
	out->size[0] = VW_W_M * scale;
	out->size[1] = VW_H_M * scale;
	s_vc.pose = *out;
	memcpy(s_vc.ax, s_vw.ax, sizeof s_vw.ax);
	memcpy(s_vc.ay, s_vw.ay, sizeof s_vw.ay);
	memcpy(s_vc.az, s_vw.az, sizeof s_vw.az);
	s_vc.cur_place = NULL;
	s_vc.cur_w_m = out->size[0];
	s_vc.cur_h_m = out->size[1];
	s_vc.cur_w_px = (float)width;
	s_vc.cur_h_px = (float)height;
	const float lap_alpha = s_vc.alpha;
	s_vc.alpha = 0.3f + 0.7f * ease;
	s_vc.cards[VC_CARD_VOICE].hash_valid = 0; /* its layer shows the wheel now */
	const int n = vc_render_panel(cmd, AERON_XR_PANEL_CARD2, s_vw.list, vw_draw, vw_overlay, views, out, width, height,
								  out->size[0], out->size[1], &s_vw.hash, &s_vw.hash_valid);
	s_vc.alpha = lap_alpha;
	return n;
}

/* ---- Button card ------------------------------------------------------- */

/* The controller layout in flight: Quest Touch controllers in outline,
 * three-quarter view from above, each control labelled. The labels come
 * from the VR bindings (xwa_runtime/runtime/vr_buttons.c), so they follow a
 * changed binding. */

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
	int button;        /* AeronXrButton whose bindings it shows, -1 = none */
	const char* fixed; /* the control's own role (axes, fire, grip) or NULL */
	const char* more;  /* fixed second line, or NULL */
	float y;           /* label row (card pixels) */
} VcCardLabel;

static const VcCardLabel k_card_left[] = {
	{ CTL_STICK, AERON_XR_BUTTON_LEFT_STICK, "Throttle (fwd/back) - roll", NULL, 96.0f },
	{ CTL_UPPER, AERON_XR_BUTTON_Y, NULL, NULL, 168.0f },
	{ CTL_LOWER, AERON_XR_BUTTON_X, NULL, NULL, 232.0f },
	{ CTL_MENU, -1, "Pause / menu", NULL, 296.0f },
	{ CTL_GRIP, -1, "Hold: quick wheel", "on a panel or the stick: move it", 380.0f },
	{ CTL_TRIGGER, AERON_XR_BUTTON_LEFT_TRIGGER, NULL, NULL, 452.0f },
};

static const VcCardLabel k_card_right[] = {
	{ CTL_STICK, AERON_XR_BUTTON_RIGHT_STICK, "Yaw / pitch", NULL, 96.0f },
	{ CTL_UPPER, AERON_XR_BUTTON_B, NULL, NULL, 168.0f },
	{ CTL_LOWER, AERON_XR_BUTTON_A, NULL, NULL, 232.0f },
	{ CTL_MENU, -1, "Meta: system menu", NULL, 296.0f },
	{ CTL_GRIP, -1, XWA_VR_RIGHT_GRIP_ACTION, "on the flight stick: fly with it", 380.0f },
	{ CTL_TRIGGER, -1, "Fire", XWA_VR_CHORD_LABEL ": land / hyperspace", 452.0f },
};

/* Left-handed controls (Aeron_XrLeftHanded): the controllers trade their
 * game roles; the menu buttons and what a grip does on a panel or the
 * flight stick stay with the hand. The face buttons name themselves
 * (B on the left controller shows as "Y"). */
static const VcCardLabel k_card_left_lh[] = {
	{ CTL_STICK, AERON_XR_BUTTON_RIGHT_STICK, "Yaw / pitch", NULL, 96.0f },
	{ CTL_UPPER, AERON_XR_BUTTON_B, NULL, NULL, 168.0f },
	{ CTL_LOWER, AERON_XR_BUTTON_A, NULL, NULL, 232.0f },
	{ CTL_MENU, -1, "Pause / menu", NULL, 296.0f },
	{ CTL_GRIP, -1, XWA_VR_RIGHT_GRIP_ACTION, "on a panel or the stick: move it", 380.0f },
	{ CTL_TRIGGER, -1, "Fire", XWA_VR_CHORD_LABEL ": land / hyperspace", 452.0f },
};

static const VcCardLabel k_card_right_lh[] = {
	{ CTL_STICK, AERON_XR_BUTTON_LEFT_STICK, "Throttle (fwd/back) - roll", NULL, 96.0f },
	{ CTL_UPPER, AERON_XR_BUTTON_Y, NULL, NULL, 168.0f },
	{ CTL_LOWER, AERON_XR_BUTTON_X, NULL, NULL, 232.0f },
	{ CTL_MENU, -1, "Meta: system menu", NULL, 296.0f },
	{ CTL_GRIP, -1, "Hold: quick wheel", "on the flight stick: fly with it", 380.0f },
	{ CTL_TRIGGER, AERON_XR_BUTTON_LEFT_TRIGGER, NULL, NULL, 452.0f },
};

/* "next target (T)" / "(A)": what a key does, with its keyboard key. */
static void vc_card_key_text(char* out, size_t size, uint16_t key, int with_key) {
	const char* action = XwaVrButtons_KeyAction(key);
	if (!action) {
		snprintf(out, size, "(%s)", XwaVrButtons_KeyName(key));
	} else if (with_key) {
		snprintf(out, size, "%s (%s)", action, XwaVrButtons_KeyName(key));
	} else {
		snprintf(out, size, "%s", action);
	}
}

/* The two label lines of a control, from its bindings. */
static void vc_card_label_text(const VcCardLabel* l, char* action, char* more, size_t size) {
	snprintf(action, size, "%s", l->fixed ? l->fixed : "");
	snprintf(more, size, "%s", l->more ? l->more : "");
	if (l->button < 0) {
		return;
	}
	const XwaVrButtonBinding* b = XwaVrButtons_Binding((AeronXrButton)l->button);
	char plain[64] = "", shifted[64] = "";
	if (b->plain) {
		vc_card_key_text(plain, sizeof plain, b->plain, 1);
	}
	if (b->shifted) {
		vc_card_key_text(shifted, sizeof shifted, b->shifted, b->shifted == 0x20 /* Space */);
	}
	const int face = l->part == CTL_UPPER || l->part == CTL_LOWER;
	if (l->part == CTL_STICK) {
		/* The stick's axes are its role; the click goes below. */
		snprintf(more, size, "%s%s%s%s%s", plain[0] ? "click: " : "", plain, plain[0] && shifted[0] ? "   " : "",
				 shifted[0] ? "grip+: " : "", shifted);
		return;
	}
	if (face) {
		snprintf(action, size, "%s: %s", XwaVrButtons_ButtonName((AeronXrButton)l->button, 0), plain);
	} else {
		snprintf(action, size, "%s", plain);
		action[0] = (char)toupper((unsigned char)action[0]);
	}
	snprintf(more, size, "%s%s%s", shifted[0] ? "grip+: " : "", shifted,
			 l->button == XWA_VR_MAP_EXIT_BUTTON ? "   map: exit" : "");
}

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
		char action[96], more[96];
		vc_card_label_text(l, action, more, sizeof action);
		vc_text(tx, l->y, action, 20.0f, align, tc[0], tc[1], tc[2], 1.0f);
		if (more[0]) {
			vc_text(tx, l->y + 24.0f, more, 15.0f, align, 0.62f, 0.85f, 0.92f, 0.9f);
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
	const int lh = Aeron_XrLeftHanded();
	vc_text(W - 44.0f, 38.0f, lh ? "hold the RIGHT grip: quick wheel" : "hold the LEFT grip: quick wheel", 18.0f, 2,
			0.62f, 0.85f, 0.92f, 0.9f);
	const float cy = 222.0f;
	const VcCardLabel* left = lh ? k_card_left_lh : k_card_left;
	const VcCardLabel* right = lh ? k_card_right_lh : k_card_right;
	const int left_n = lh ? (int)(sizeof k_card_left_lh / sizeof k_card_left_lh[0])
						  : (int)(sizeof k_card_left / sizeof k_card_left[0]);
	const int right_n = lh ? (int)(sizeof k_card_right_lh / sizeof k_card_right_lh[0])
						   : (int)(sizeof k_card_right / sizeof k_card_right[0]);
	vc_card_controller(W * 0.5f - 148.0f, cy, 1.0f, left, left_n, 236.0f, W * 0.5f);
	vc_card_controller(W * 0.5f + 148.0f, cy, -1.0f, right, right_n, W - 236.0f, W * 0.5f);
	vc_text(W * 0.5f - 148.0f - 80.0f, cy + 318.0f, "LEFT", 22.0f, 1, VC_CYAN, 0.9f);
	vc_text(W * 0.5f + 148.0f + 80.0f, cy + 318.0f, "RIGHT", 22.0f, 1, VC_CYAN, 0.9f);
	vc_text(W * 0.5f, H - 158.0f, "Flight stick (between your knees): right grip on it = fly, left grip = move it", 17.0f,
			1, 0.85f, 0.97f, 1.0f, 0.95f);
	vc_text(W * 0.5f, H - 132.0f, "Voice: double-tap a thumb rest, then say e.g. \"attack my target\", \"nearest enemy\"",
			17.0f, 1, 0.85f, 0.97f, 1.0f, 0.95f);
	vc_text(W * 0.5f, H - 104.0f,
			lh ? "Quick wheel: hold the RIGHT grip, point at a command, pull the right trigger"
			   : "Quick wheel: hold the LEFT grip, point at a command, pull the left trigger",
			17.0f, 1, 0.62f, 0.85f, 0.92f, 0.9f);
}

/* The voice command list (left card). Keep in step with the command table
 * in xwa_app/voice_core.c: one phrase per command; close wordings work too. */
static const char* const k_voice_wing[] = { "attack my target",  "cover me",       "ignore my target",
											"evade",             "wait",           "go ahead",
											"return to base",    "report in",      "call resupply",
											"call reinforcements", "gunner attack my target" };
static const char* const k_voice_target[] = { "nearest enemy",       "next target",           "previous target",
											  "target my attacker",  "who is attacking my target",
											  "target incoming missile", "target objective", "newest target",
											  "next subsystem",      "clear target",          "nav buoy" };
static const char* const k_voice_ship[] = { "match speed",       "countermeasures",   "s-foils",
											"lasers to shields", "shields to lasers", "fire link",
											"convergence",       "map",               "dock",
											"pick up",           "release cargo",     "hyperspace",
											"confirm" };

static void vc_voice_column(float x, const char* title, const char* const* lines, int count) {
	float rule[4];
	vc_text(x, 132.0f, title, 20.0f, 0, VC_CYAN, 0.95f);
	vc_pma(rule, VC_CYAN, 0.45f);
	AeronDrawList_AddFill(s_vc.cur_list, x, 160.0f, 270.0f, 2.0f, rule, AERON_BLIT2D_BLEND_PMA, NULL);
	for (int i = 0; i < count; ++i) {
		char text[64];
		snprintf(text, sizeof text, "\"%s\"", lines[i]);
		vc_text(x, 176.0f + 38.0f * (float)i, text, 20.0f, 0, 0.85f, 0.97f, 1.0f, 1.0f);
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

/* Shown: as the setting says, unless the help page showed or hid it. */
static int vc_card_visible(int card) {
	if (card < 0 || card >= VC_CARDS) {
		return 0;
	}
	if (s_vc.card_override[card]) {
		return s_vc.card_override[card] == 1;
	}
	return vc_card_enabled_kind(card);
}

static int vc_card_enabled(void) { return vc_card_visible(VC_CARD_BUTTONS) || vc_card_visible(VC_CARD_VOICE); }

int XwaVrConsole_CardFrame(AeronCommandBuffer* cmd, int card, int width, int height, const AeronXrView views[2],
						   AeronXrPanel out[2]) {
	/* The quick wheel takes the voice card's layer; both cards are hidden
	 * while it is open. */
	if (card == VC_CARD_VOICE && cmd && width > 0 && height > 0 && views && out) {
		vw_update(views);
		if (s_vw.open) {
			return vw_frame(cmd, width, height, views, out);
		}
	} else if (s_vw.open) {
		return 0;
	}
	if (card < 0 || card >= VC_CARDS || !cmd || width <= 0 || height <= 0 || !views || !out || !s_vc.state.valid ||
		!vc_card_visible(card) || s_vc.font_state == 0 || !vc_ensure()) {
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
	if (card == VC_CARD_VOICE && n > 0) {
		s_vw.hash_valid = 0; /* the shared layer shows the card again */
	}
	return n;
}

void XwaVrConsole_EndFrame(void) {
	if (!s_vw.drawn) {
		vw_close(); /* not in flight this frame (paused, a menu) */
	}
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
		s_vc.grip_capture_prev[h] = s_vc.grip_capture[h];
		Aeron_XrCaptureGrip(h, s_vc.grip_capture[h] || (s_vw.open && h == s_vw.hand));
		s_vc.grip_capture[h] = 0;
	}
	/* The quick wheel's trigger picks: the game does not see it (W). The
	 * console or this function above lets it go once the wheel closes. */
	if (s_vw.open) {
		Aeron_XrCaptureTrigger(s_vw.hand, 1);
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
	for (int e = 0; e < 2; ++e) {
		if (s_vw.list[e]) {
			AeronDrawList_Destroy(s_vw.list[e]);
		}
	}
	memset(&s_vw, 0, sizeof s_vw);
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

#include "xwa_remaster/vr_env.h"

#include "aeron/aeron.h"
#include "aeron/asset/opt_model.h"
#include "aeron/log.h"
#include "aeron/scene/billboard.h"
#include "aeron/scene/draw_list2d.h"
#include "aeron/scene/mesh.h"
#include "aeron/vfs.h"
#include "aeron/xr.h"
#include "xwa_2d.h"
#include "xwa_remaster/ship.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A prop of the classic carrier hangar (Hangar_EnterHangar's default
 * layout, highest detail level): position relative to the launch
 * reference, Q16 yaw and pitch relative to it. */
#define ENV_ON_FLOOR 0x7fffffff

typedef struct EnvProp {
	const char* opt;
	int x, y, z;
	int yaw, pitch;
} EnvProp;

static const EnvProp k_props[] = {
	{ "HangarMonitor", -1059, 1313, ENV_ON_FLOOR, 38600, 0 },
	{ "HangarCrane", 725, -416, ENV_ON_FLOOR, 23500, 0 },
	{ "HangarGenerator", -1352, 1440, ENV_ON_FLOOR, 536, 0 },
	{ "HangarWorkstand", -1065, -56, ENV_ON_FLOOR, 47736, 0 },
	{ "HangarWorkstand", 2079, 733, ENV_ON_FLOOR, 6400, 0 },
	{ "HangarWorkstand", -2488, 1215, ENV_ON_FLOOR, 11864, 0 },
	{ "HangarCrate", 794, -750, -848, 6800, 12500 },
	{ "HangarCrate", -803, -1464, ENV_ON_FLOOR, 49200, 0 },
	{ "HangarCrate", -1563, 944, ENV_ON_FLOOR, 51436, 0 },
	{ "ContainerBox", -506, -2136, -722, 32836, 16480 },
	{ "ContainerBox", -841, -1628, ENV_ON_FLOOR, 53100, 0 },
	{ "ContainerBox", 1240, -2098, -722, 272, 16480 },
	{ "ContainerSphere", -834, -2034, ENV_ON_FLOOR, 32336, 0 },
	{ "CargoCanister", -3474, -275, ENV_ON_FLOOR, 29436, 0 },
	{ "Ywing", -1038, -798, ENV_ON_FLOOR, 7700, 0 },
	{ "Shuttle", 1146, -1288, -741, 50536, 0 },
	{ "HangarWorkstand", 820, -665, ENV_ON_FLOOR, 49136, 0 },
	{ "HangarCrate", -873, -1456, -849, 60472, 20400 },
	{ "HangarCrate", -3095, -592, ENV_ON_FLOOR, 51436, 0 },
	{ "ContainerBox", -1751, -1996, ENV_ON_FLOOR, 31172, 0 },
	{ "ContainerBox", 979, -2103, ENV_ON_FLOOR, 37036, 0 },
	{ "ContainerSphere", -1374, -2068, ENV_ON_FLOOR, 40536, 0 },
	{ "CargoCanister", 2806, 1357, ENV_ON_FLOOR, 20536, 0 },
	{ "CargoCanister", -195, -1910, ENV_ON_FLOOR, 54372, 0 },
	{ "Awing", 2178, 506, -830, 40036, 1500 },
	{ "Ywing", 1100, -503, ENV_ON_FLOOR, 49036, 0 },
	{ "Xwing", -1003, 280, -825, 16600, 0 },
	{ "HangarDroid", 227, 351, ENV_ON_FLOOR, 58736, 0 },
	{ "HangarDroid2", -70, 400, ENV_ON_FLOOR, 0, 0 },
	{ "Shuttle", 1127, 959, -741, 43136, 0 },
	{ "HangarRoofCrane", -1400, 786, -282, 0, 0 },
};

#define ENV_PROP_COUNT ((int)(sizeof k_props / sizeof k_props[0]))

/* ---- "Battle of Endor": the box-art scene ---------------------------
 * A still frame loosely after the X-Wing Alliance cover: the Falcon below
 * left, the second Death Star upper right over Endor's rim, the rebel
 * fleet on the left with a cruiser bursting under the superlaser, Star
 * Destroyers before the station, X-wings diving in from the lower right.
 * A B-wing (left) and two TIE interceptors (right) sit just outside the
 * forward view. Directions: yaw (degrees, + right of the screen), pitch
 * (+ up), distance in metres from the viewer; orientation as the craft's
 * nose and up directions (world: x right, y forward, z up). */
typedef struct EnvShip {
	const char* opt; /* OPT name; alternatives separated by '|' */
	float yaw, pitch, dist;
	float nose[3], up[3];
	int aim; /* 1-based index (same scene) the nose points at (0 = use nose) */
	/* 1: nose and up are as the viewer sees the craft (x right, y away
	 * along the line of sight to it, z up), so it keeps the look it has
	 * on a picture wherever around the viewer it is placed. */
	int view;
} EnvShip;

#define ENV_DS_YAW 36.0f
#define ENV_DS_PITCH 21.0f
#define ENV_DS_NOSE { 0.5488f, 0.7553f, 0.3584f } /* toward the Death Star */

static const EnvShip k_endor[] = {
	{ "MilleniumFalcon2", -33.0f, -24.0f, 36.0f, { 0.62f, 0.72f, 0.30f }, { -0.35f, -0.25f, 0.9f } },
	/* X-wings and TIEs in a dogfight: each nose on its target. */
	{ "Xwing", 38.0f, -30.0f, 40.0f, ENV_DS_NOSE, { 0.2f, -0.4f, 0.9f }, 5 },
	{ "Xwing", 46.0f, -22.0f, 65.0f, ENV_DS_NOSE, { -0.3f, -0.3f, 0.9f }, 19 },
	{ "Xwing", 30.0f, -34.0f, 85.0f, ENV_DS_NOSE, { 0.0f, -0.3f, 0.95f }, 6 },
	{ "TieFighter", 44.0f, 4.0f, 160.0f, { -0.6f, -0.7f, -0.2f }, { 0.0f, 0.0f, 1.0f }, 2 },
	{ "TieFighter", 52.0f, 12.0f, 210.0f, { -0.5f, -0.8f, -0.1f }, { 0.2f, 0.0f, 1.0f }, 4 },
	{ "TieFighter", 20.0f, 32.0f, 260.0f, { -0.4f, -0.8f, -0.3f }, { 0.0f, 0.1f, 1.0f } },
	/* The Falcon's target, below the screen, firing back. */
	{ "TieFighter", -10.0f, -36.0f, 110.0f, { -0.4f, -0.9f, 0.1f }, { 0.0f, 0.0f, 1.0f }, 1 },
	{ "ImperialStarDestroyer2", 27.0f, 12.0f, 10000.0f, { -0.8f, -0.5f, -0.1f }, { 0.0f, 0.0f, 1.0f } },
	{ "ImperialStarDestroyer2", 47.0f, 16.0f, 13000.0f, { -0.8f, -0.55f, -0.05f }, { 0.05f, 0.0f, 1.0f } },
	{ "CalamariCruiserNew", -44.0f, 8.0f, 6000.0f, { 0.8f, 0.55f, 0.1f }, { 0.0f, 0.0f, 1.0f } },
	{ "CalamariWinged", -58.0f, 18.0f, 8000.0f, { 0.85f, 0.5f, 0.05f }, { 0.0f, 0.0f, 1.0f } },
	{ "Frigate2", -40.0f, -6.0f, 3000.0f, { 0.8f, 0.6f, 0.0f }, { 0.0f, 0.0f, 1.0f } },
	{ "Corvette2", -50.0f, -12.0f, 1500.0f, { 0.7f, 0.7f, 0.1f }, { 0.0f, 0.0f, 1.0f } },
	/* Hit by the superlaser (ENV_BURST). */
	{ "CalamariCruiserNew", -38.0f, 31.0f, 7000.0f, { 0.7f, 0.7f, -0.1f }, { 0.0f, 0.0f, 1.0f } },
	/* Out of the forward view: turn left / right. */
	{ "Bwing", -85.2f, -2.4f, 24.1f, { 0.15f, 1.0f, 0.05f }, { 0.9f, 0.0f, 0.4f } },
	{ "TieInterceptor", 92.6f, 5.2f, 22.1f, { -0.2f, 1.0f, 0.1f }, { 0.0f, 0.0f, 1.0f } },
	{ "TieInterceptor", 76.0f, 13.6f, 29.7f, { -0.2f, 1.0f, 0.15f }, { -0.2f, 0.0f, 1.0f } },
	/* Hit by the second X-wing: half inside its explosion (ENV_VICTIM). */
	{ "TieFighter", 50.0f, -8.0f, 85.0f, { -0.3f, -0.5f, 0.2f }, { 0.3f, 0.0f, 1.0f } },
};
#define ENV_ENDOR_COUNT ((int)(sizeof k_endor / sizeof k_endor[0]))
#define ENV_FALCON 0
#define ENV_FALCON_TARGET 7
#define ENV_BURST 14
#define ENV_VICTIM 18

/* Frozen laser salvos: shooter -> target (indices in the scene's ship
 * list) from the shooter's guns, as spans of the shooter-target distance
 * along its nose. */
typedef enum EnvGuns {
	ENV_GUNS_XWING, /* four wingtip cannons, red */
	ENV_GUNS_TIE,   /* two chin guns, green */
	ENV_GUNS_AWING, /* two wingtip cannons, red */
	ENV_GUNS_INTERCEPTOR, /* TIE Interceptor: two wingtip guns, green */
} EnvGuns;

typedef struct EnvSalvo {
	int shooter, target;
	EnvGuns guns;
	float spans[2][2];
	int span_count;
} EnvSalvo;

static const EnvSalvo k_salvos[] = {
	{ 1, 4, ENV_GUNS_XWING, { { 0.20f, 0.32f } }, 1 },
	{ 2, ENV_VICTIM, ENV_GUNS_XWING, { { 0.25f, 0.40f } }, 1 },
	{ 3, 5, ENV_GUNS_XWING, { { 0.20f, 0.30f } }, 1 },
	{ 4, 1, ENV_GUNS_TIE, { { 0.15f, 0.25f }, { 0.40f, 0.50f } }, 2 },
	{ 5, 3, ENV_GUNS_TIE, { { 0.15f, 0.25f }, { 0.40f, 0.50f } }, 2 },
	{ 7, 0, ENV_GUNS_TIE, { { 0.20f, 0.30f }, { 0.45f, 0.55f } }, 2 },
};

/* ---- "X-Wing": the X-Wing box art -------------------------------------
 * The cover of the original X-Wing and only what is on it: three X-wings
 * and three TIE fighters over a purple nebula, a fourth fighter bursting
 * upper left, red X-wing and green TIE fire. Each craft keeps the attitude
 * it has on the cover as the viewer sees it (view-relative nose / up: x
 * right, y away along the line of sight, z up), and keeps its place on the
 * cover, pushed out from the middle just far enough to clear the menu
 * screen (about 21 degrees either side, 19 below to 17 above, the logo
 * to 28 above within 12 either side). */
static const EnvShip k_boxart[] = {
	/* 0: lower right, the big one: from behind, engines toward us, banked
	 * with its wings rising to the right, climbing away to the upper left. */
	{ "Xwing", 36.0f, -24.0f, 24.0f, { -0.25f, 0.93f, 0.27f }, { -0.55f, -0.20f, 0.80f }, 0, 1 },
	/* 1: middle left: flying right and away, rolled near knife edge (upper
	 * wings to the upper left), its top and engines toward us. */
	{ "Xwing", -36.0f, -3.0f, 30.0f, { 0.55f, 0.80f, 0.0f }, { 0.68f, -0.47f, 0.40f }, 0, 1 },
	/* 2: upper left, by the burst: flying right, from its rear left quarter,
	 * banked right wing up. */
	{ "Xwing", -34.0f, 34.0f, 70.0f, { 0.75f, 0.60f, 0.15f }, { -0.35f, -0.25f, 0.90f }, 0, 1 },
	/* 3: upper middle (left of the logo): coming at us three-quarters
	 * from the front left, cockpit toward us, the wings seen obliquely
	 * with their axis rising to the right; firing to the upper left. */
	{ "TieFighter", -19.0f, 24.0f, 60.0f, { -0.65f, -0.65f, 0.40f }, { -0.09f, 0.59f, 0.80f }, 0, 1 },
	/* 4: right of the middle: three-quarters from the front left, window
	 * toward us, the wing axis falling steeply to the right; firing to the
	 * lower left. */
	{ "TieFighter", 30.0f, 7.0f, 52.0f, { -0.60f, -0.75f, -0.25f }, { 0.53f, -0.62f, 0.57f }, 0, 1 },
	/* 5: at the cover's right edge, mostly cut off there: as TIE 4. */
	{ "TieFighter", 52.0f, 4.0f, 80.0f, { -0.60f, -0.75f, -0.25f }, { 0.53f, -0.62f, 0.57f }, 0, 1 },
};

/* Where the fighter bursts, upper left by X-wing 2: yaw, pitch, metres. */
static const float k_box_burst[3] = { -21.0f, 36.0f, 72.0f };

static const EnvSalvo k_box_salvos[] = {
	/* The big X-wing's fire, away into the scene. */
	{ 0, 5, ENV_GUNS_XWING, { { 0.15f, 0.27f }, { 0.45f, 0.57f } }, 2 },
	/* X-wing 1's fire to the right. */
	{ 1, 4, ENV_GUNS_XWING, { { 0.15f, 0.28f } }, 1 },
	/* The two long green streaks to the upper left. */
	{ 3, 2, ENV_GUNS_TIE, { { 0.08f, 0.40f } }, 1 },
	/* TIE 4's fire to the left. */
	{ 4, 1, ENV_GUNS_TIE, { { 0.10f, 0.32f }, { 0.48f, 0.66f } }, 2 },
};

/* ---- "TIE Fighter": the TIE Fighter box art ---------------------------
 * The cover of TIE Fighter (its logo left out): a TIE Interceptor coming at
 * the viewer in front of a huge explosion in which an X-wing breaks up, a
 * second Interceptor upper left and a TIE fighter on the left firing green,
 * a TIE Bomber upper right firing, a small TIE fighter on the right, Darth
 * Vader's helmet looming behind it all over purple clouds. Placed and
 * oriented as on the cover (view-relative), pushed out from the middle just
 * far enough to clear the menu screen and logo; the helmet is the backdrop
 * the screen stands in front of. */
static const EnvShip k_tieart[] = {
	/* 0: the big Interceptor below the screen, coming at us, cockpit window
	 * toward us, seen a little from above, turned slightly to our left. */
	{ "TieInterceptor", -8.0f, -40.0f, 12.0f, { -0.12f, -0.97f, 0.20f }, { 0.10f, -0.25f, 0.96f }, 0, 1 },
	/* 1: Interceptor upper left: three-quarters from the front, wings rising
	 * to the right, firing to the upper left. */
	{ "TieInterceptor", -32.0f, 19.0f, 40.0f, { -0.60f, -0.45f, 0.65f }, { -0.31f, 0.88f, 0.33f }, 0, 1 },
	/* 2: TIE fighter on the left: window to the front left, firing left. */
	{ "TieFighter", -31.0f, -4.0f, 55.0f, { -0.75f, -0.55f, 0.10f }, { -0.08f, 0.27f, 0.89f }, 0, 1 },
	/* 3: TIE Bomber upper right: from the front above, rolled, firing to
	 * the upper right. */
	{ "TieBomber", 36.0f, 16.0f, 28.0f, { 0.30f, -0.60f, 0.75f }, { -0.45f, 0.60f, 0.66f }, 0, 1 },
	/* 4: the small TIE fighter right of the middle, seen from above. */
	{ "TieFighter", 28.0f, 2.0f, 90.0f, { 0.30f, -0.50f, -0.80f }, { 0.0f, -0.85f, 0.50f }, 0, 1 },
	/* 5: the X-wing breaking up in the explosion, nose down to the right. */
	{ "Xwing", 18.0f, -32.0f, 45.0f, { 0.55f, -0.35f, -0.75f }, { 0.30f, -0.60f, 0.50f }, 0, 1 },
};

/* The explosion's centre behind the big Interceptor: yaw, pitch, metres,
 * diameter in metres. */
static const float k_tie_blast[4] = { -6.0f, -33.0f, 70.0f, 58.0f };

static const EnvSalvo k_tie_salvos[] = {
	/* The long green streaks of the Interceptor upper left. */
	{ 1, 0, ENV_GUNS_INTERCEPTOR, { { 0.05f, 1.10f } }, 1 },
	/* The TIE fighter's fire to the left. */
	{ 2, 0, ENV_GUNS_TIE, { { 0.05f, 0.75f } }, 1 },
	/* The bomber's streak to the upper right. */
	{ 3, 0, ENV_GUNS_TIE, { { 0.10f, 1.60f } }, 1 },
};

typedef struct EnvSpaceScene {
	const EnvShip* ships;
	int count;
	const EnvSalvo* salvos;
	int salvo_count;
} EnvSpaceScene;

static const EnvSpaceScene k_scene_endor = { k_endor, ENV_ENDOR_COUNT, k_salvos,
											 (int)(sizeof k_salvos / sizeof k_salvos[0]) };
static const EnvSpaceScene k_scene_tieart = { k_tieart, (int)(sizeof k_tieart / sizeof k_tieart[0]), k_tie_salvos,
											  (int)(sizeof k_tie_salvos / sizeof k_tie_salvos[0]) };
static const EnvSpaceScene k_scene_boxart = { k_boxart, (int)(sizeof k_boxart / sizeof k_boxart[0]), k_box_salvos,
											  (int)(sizeof k_box_salvos / sizeof k_box_salvos[0]) };

/* Sprites from the game's resource DATs (group, sprite id) plus the logo
 * and a generated beam profile. */
enum {
	ENV_TEX_FRAME, /* first: the screen frame shows in every scene */
	ENV_TEX_DEATHSTAR,
	ENV_TEX_ENDOR,
	ENV_TEX_BLAST,
	ENV_TEX_FIRE,
	ENV_TEX_LOGO,
	ENV_TEX_BEAM,
	ENV_TEX_NEBULA, /* generated: the box art's purple nebula */
	ENV_TEX_VADER,  /* vr/vader.rgba: the helmet behind the TIE Fighter scene */
	ENV_TEX_COUNT
};

typedef struct EnvTexSource {
	const char* dat; /* under Resdata/ */
	int group, sprite;
} EnvTexSource;

static const EnvTexSource k_tex_sources[ENV_TEX_COUNT] = {
	{ NULL, 0, 0 },            /* vr/frame.rgba */
	{ "DSFire", 6250, -1 },    /* the second Death Star (first frame) */
	{ "Planet", 6026, -1 },    /* a green forest world for Endor */
	{ "Explosions", 2005, 7 },
	{ "Explosions", 2003, 7 },
	{ NULL, 0, 0 },            /* vr/logo.rgba */
	{ NULL, 0, 0 },            /* generated */
	{ NULL, 0, 0 },            /* generated */
	{ NULL, 0, 0 },            /* vr/vader.rgba */
};

typedef enum EnvMode { ENV_OFF = 0, ENV_ENDOR, ENV_HANGAR, ENV_BOXART, ENV_TIEART } EnvMode;

#define ENV_MAX_MESHES 24
#define ENV_MAX_INSTANCES 40
/* InsideHangar hardpoint: the hangar floor (classic launch base). */
#define ENV_HARDPOINT_INSIDE_HANGAR 25
/* Floor below the launch reference when the hardpoint is missing (the
 * explicit heights of floor-standing props put it near here). */
#define ENV_FLOOR_FALLBACK (-870.0f)

typedef struct EnvMesh {
	char opt[64]; /* as the scene names it (alternatives with '|') */
	AeronSceneMesh* mesh;
	float size_z; /* world units */
	int tried;
} EnvMesh;

static struct {
	EnvMode mode;
	EnvMesh meshes[ENV_MAX_MESHES];
	int mesh_count;
	int hangar; /* index of Hangar.opt in meshes */
	float floor_z;
	int laid_out;
	int logged;
	float view_pos[3];
	/* Instances, rebuilt whenever a mesh arrives. */
	int count;
	int mesh_of[ENV_MAX_INSTANCES];
	float transform[ENV_MAX_INSTANCES][16];
	uint8_t far[ENV_MAX_INSTANCES];
	/* Endor sprites. */
	AeronTexture* tex[ENV_TEX_COUNT];
	int tex_w[ENV_TEX_COUNT], tex_h[ENV_TEX_COUNT];
	int tex_tried[ENV_TEX_COUNT];
	/* The frame as its own layer over the screen. */
	AeronDrawList2D* frame_list;
	int frame_drawn, frame_layer_ok;
	float frame_tint;
} s_env;

static float env_float(const char* name, float fallback) {
	const char* v = getenv(name);
	return v && v[0] ? (float)atof(v) : fallback;
}

static EnvMode env_mode(void) {
	const char* v = getenv("XWA_XR_ENVIRONMENT");
	if (v && strcmp(v, "0") == 0) {
		return ENV_OFF;
	}
	if (v && SDL_strcasecmp(v, "hangar") == 0) {
		return ENV_HANGAR;
	}
	if (v && (SDL_strcasecmp(v, "xwing") == 0 || SDL_strcasecmp(v, "boxart") == 0)) {
		return ENV_BOXART;
	}
	if (v && SDL_strcasecmp(v, "tiefighter") == 0) {
		return ENV_TIEART;
	}
	if (v && SDL_strcasecmp(v, "endor") == 0) {
		return ENV_ENDOR;
	}
	/* "random" (and the default): one of the space scenes, picked once per
	 * start. */
	static EnvMode picked = ENV_OFF;
	if (picked == ENV_OFF) {
		static const EnvMode scenes[] = { ENV_ENDOR, ENV_BOXART, ENV_TIEART };
		const uint64_t seed = SDL_GetPerformanceCounter() ^ ((uint64_t)time(NULL) * 2654435761u);
		picked = scenes[(seed ^ (seed >> 17) ^ (seed >> 31)) % (sizeof scenes / sizeof scenes[0])];
		Aeron_LogInfo("xwa.remaster", "VR environment: random pick %s",
					  picked == ENV_ENDOR ? "endor" : picked == ENV_BOXART ? "xwing" : "tiefighter");
	}
	return picked;
}

/* The ship list of the selected space scene (NULL in the hangar). */
static const EnvSpaceScene* env_space(void) {
	switch (s_env.mode) {
		case ENV_ENDOR:
			return &k_scene_endor;
		case ENV_BOXART:
			return &k_scene_boxart;
		case ENV_TIEART:
			return &k_scene_tieart;
		default:
			return NULL;
	}
}

int XwaVrEnv_Enabled(void) { return env_mode() != ENV_OFF; }

int XwaVrEnv_IsSpace(void) {
	const EnvMode m = env_mode();
	return m == ENV_ENDOR || m == ENV_BOXART || m == ENV_TIEART;
}

static void env_set3(float out[3], float x, float y, float z) {
	out[0] = x;
	out[1] = y;
	out[2] = z;
}

int XwaVrEnv_SpaceLighting(XwaVrEnvLighting* out) {
	const EnvMode m = env_mode();
	if (!out || (m != ENV_ENDOR && m != ENV_BOXART && m != ENV_TIEART)) {
		return 0;
	}
	memset(out, 0, sizeof *out);
	const float sun = env_float("XWA_XR_ENDOR_SUN", 1.6f);
	if (m == ENV_ENDOR) {
		/* A warm sun from the upper left behind the viewer, a faint green
		 * bounce off the moon below right, deep-space ambient. */
		env_set3(out->key_dir, -0.55f, 0.35f, 0.75f);
		env_set3(out->key_color, 1.0f * sun, 0.95f * sun, 0.88f * sun);
		env_set3(out->fill_dir, 0.55f, 0.35f, -0.75f);
		env_set3(out->fill_color, 0.55f * 0.3f, 0.85f * 0.3f, 0.6f * 0.3f);
		env_set3(out->ambient_sides, 0.05f, 0.05f, 0.05f);
		env_set3(out->ambient_top, 0.06f, 0.06f, 0.06f);
		env_set3(out->ambient_bottom, 0.04f, 0.08f, 0.05f);
	} else if (m == ENV_TIEART) {
		/* The TIE Fighter cover: a cool blue-white light from the upper
		 * right, the explosion's orange glow from behind and below the
		 * craft, a violet ambient. */
		env_set3(out->key_dir, 0.45f, 0.25f, 0.85f);
		env_set3(out->key_color, 0.80f * sun, 0.85f * sun, 1.0f * sun);
		env_set3(out->fill_dir, 0.0f, 0.9f, -0.45f);
		env_set3(out->fill_color, 1.0f * 0.9f, 0.55f * 0.9f, 0.2f * 0.9f);
		env_set3(out->ambient_sides, 0.05f, 0.04f, 0.08f);
		env_set3(out->ambient_top, 0.05f, 0.04f, 0.09f);
		env_set3(out->ambient_bottom, 0.05f, 0.03f, 0.07f);
	} else {
		/* The box art: white light from the upper left ahead (it catches the
		 * tops and left sides of the craft flying away), the nebula's
		 * magenta glow from ahead, a violet ambient. */
		env_set3(out->key_dir, -0.45f, 0.55f, 0.70f);
		env_set3(out->key_color, 1.0f * sun, 0.96f * sun, 0.92f * sun);
		env_set3(out->fill_dir, 0.2f, 0.9f, -0.2f);
		env_set3(out->fill_color, 0.75f * 0.45f, 0.30f * 0.45f, 0.85f * 0.45f);
		env_set3(out->ambient_sides, 0.06f, 0.035f, 0.07f);
		env_set3(out->ambient_top, 0.07f, 0.04f, 0.08f);
		env_set3(out->ambient_bottom, 0.04f, 0.025f, 0.05f);
	}
	return 1;
}

static const char* env_craft(void) {
	const char* v = getenv("XWA_XR_ENV_CRAFT");
	if (v) {
		return v[0] && strcmp(v, "0") != 0 ? v : NULL;
	}
	return "Xwing";
}

static void env_list_add(const char* name) {
	for (int j = 0; j < s_env.mesh_count; ++j) {
		if (SDL_strcasecmp(s_env.meshes[j].opt, name) == 0) {
			return;
		}
	}
	if (s_env.mesh_count < ENV_MAX_MESHES) {
		snprintf(s_env.meshes[s_env.mesh_count].opt, sizeof s_env.meshes[0].opt, "%s", name);
		s_env.mesh_count++;
	}
}

/* The list of unique OPTs. Hangar: the hangar first, then the parked
 * craft, then the props. Endor: the Falcon first. */
static void env_list(void) {
	if (s_env.mesh_count) {
		return;
	}
	s_env.hangar = -1;
	s_env.floor_z = ENV_FLOOR_FALLBACK;
	const EnvSpaceScene* space = env_space();
	if (space) {
		for (int i = 0; i < space->count; ++i) {
			env_list_add(space->ships[i].opt);
		}
		return;
	}
	env_list_add("Hangar");
	s_env.hangar = 0;
	if (env_craft()) {
		env_list_add(env_craft());
	}
	for (int i = 0; i < ENV_PROP_COUNT; ++i) {
		env_list_add(k_props[i].opt);
	}
}

static int env_find(const char* opt) {
	for (int j = 0; j < s_env.mesh_count; ++j) {
		if (SDL_strcasecmp(s_env.meshes[j].opt, opt) == 0) {
			return s_env.meshes[j].mesh ? j : -1;
		}
	}
	return -1;
}

static void env_load(AeronCommandBuffer* cmd, int index) {
	EnvMesh* m = &s_env.meshes[index];
	AeronFlightModel model;
	m->tried = 1;
	memset(&model, 0, sizeof model);
	/* The first of the '|'-separated names the game data has. */
	int built = 0;
	for (const char* name = m->opt; *name && !built;) {
		const size_t n = strcspn(name, "|");
		char one[64];
		snprintf(one, sizeof one, "%.*s", (int)n, name);
		built = one[0] && XwaRemasterShip_BuildOptModel(one, &model);
		name += n + (name[n] == '|');
	}
	if (!built) {
		Aeron_LogWarn("xwa.remaster", "VR environment: no OPT for '%s'", m->opt);
		return;
	}
	m->size_z = (model.bounds.max.z - model.bounds.min.z) * AERON_OPT_UNITS_PER_METER;
	if (index == s_env.hangar) {
		int found = 0;
		for (uint32_t c = 0; c < model.component_count && !found; ++c) {
			const AeronFlightComponent* comp = &model.components[c];
			for (uint32_t h = 0; h < comp->hardpoint_count; ++h) {
				if (comp->hardpoints[h].type == ENV_HARDPOINT_INSIDE_HANGAR) {
					s_env.floor_z = comp->hardpoints[h].position.z * AERON_OPT_UNITS_PER_METER;
					found = 1;
					break;
				}
			}
		}
		Aeron_LogInfo("xwa.remaster",
					  "VR environment: hangar bounds x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f, floor %.0f (%s)",
					  model.bounds.min.x * AERON_OPT_UNITS_PER_METER, model.bounds.max.x * AERON_OPT_UNITS_PER_METER,
					  model.bounds.min.y * AERON_OPT_UNITS_PER_METER, model.bounds.max.y * AERON_OPT_UNITS_PER_METER,
					  model.bounds.min.z * AERON_OPT_UNITS_PER_METER, model.bounds.max.z * AERON_OPT_UNITS_PER_METER,
					  (double)s_env.floor_z, found ? "InsideHangar hardpoint" : "fallback");
	}
	AeronSceneMeshCreateStatus status;
	m->mesh = AeronScene_MeshCreate(cmd, &model, m->opt, &status);
	Aeron_FlightModelFree(&model);
	if (!m->mesh) {
		Aeron_LogWarn("xwa.remaster", "VR environment: '%s' mesh creation failed (%d)", m->opt, (int)status);
	}
}

/* Model -> scene transform of an object with Q16 pitch/yaw at `pos`
 * (world units): the Euler compose of FVIEW_calcrotatemove with no roll,
 * consumed like the flight renderer's object transforms (model metres ->
 * world units). */
static void env_object_matrix(int pitch, int yaw, const float pos[3], float m[16]) {
	const float q16 = 2.0f * 3.14159265358979f / 65536.0f;
	const float a = (float)(int16_t)(uint16_t)(0xc000 - pitch) * q16;
	const float b = (float)(int16_t)(uint16_t)(-(int16_t)(uint16_t)yaw) * q16;
	const float cb = cosf(b), sb = sinf(b), ca = cosf(a), sa = sinf(a);
	const float cur0[3] = { cb, sb, 0.0f };
	const float cur1[3] = { -sb * sa, cb * sa, -ca };
	const float cur2[3] = { -sb * ca, cb * ca, sa };
	/* basis rows (cur0, cur2, cur1); world = basis^T * model */
	const float* rows[3] = { cur0, cur2, cur1 };
	const float k = AERON_OPT_UNITS_PER_METER;
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			m[i * 4 + j] = k * rows[j][i];
		}
		m[i * 4 + 3] = pos[i];
	}
	m[12] = m[13] = m[14] = 0.0f;
	m[15] = 1.0f;
}

static void env_norm(float v[3]) {
	const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l > 1e-9f) {
		v[0] /= l;
		v[1] /= l;
		v[2] /= l;
	}
}

static void env_cross(const float a[3], const float b[3], float out[3]) {
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

/* Unit direction of yaw/pitch degrees (yaw 0 = forward +y, + = right). */
static void env_dir(float yaw, float pitch, float out[3]) {
	const float y = yaw * 3.14159265358979f / 180.0f, p = pitch * 3.14159265358979f / 180.0f;
	out[0] = sinf(y) * cosf(p);
	out[1] = cosf(y) * cosf(p);
	out[2] = sinf(p);
}

/* Model -> scene transform from a nose and an up direction: the model's
 * forward (-y) onto `nose`, its z onto the up made orthogonal, x onto
 * nose x up (the level yaw-0 pose of env_object_matrix is nose +y, up +z). */
static void env_pose_matrix(const float nose_in[3], const float up_in[3], const float pos[3], float m[16]) {
	float n[3] = { nose_in[0], nose_in[1], nose_in[2] }, r[3], u[3];
	env_norm(n);
	env_cross(n, up_in, r);
	env_norm(r);
	env_cross(r, n, u);
	const float k = AERON_OPT_UNITS_PER_METER;
	for (int i = 0; i < 3; ++i) {
		m[i * 4 + 0] = k * r[i];
		m[i * 4 + 1] = -k * n[i];
		m[i * 4 + 2] = k * u[i];
		m[i * 4 + 3] = pos[i];
	}
	m[12] = m[13] = m[14] = 0.0f;
	m[15] = 1.0f;
}

static void env_add(int mesh, int pitch, int yaw, float x, float y, float z) {
	const float pos[3] = { x, y, z };
	if (mesh < 0 || s_env.count >= ENV_MAX_INSTANCES) {
		return;
	}
	s_env.mesh_of[s_env.count] = mesh;
	s_env.far[s_env.count] = 0;
	env_object_matrix(pitch, yaw, pos, s_env.transform[s_env.count]);
	s_env.count++;
}

/* World position (units) of the space scene's ship i. */
static void env_ship_pos(int i, float out[3]) {
	const EnvShip* e = &env_space()->ships[i];
	env_dir(e->yaw, e->pitch, out);
	for (int a = 0; a < 3; ++a) {
		out[a] *= e->dist * AERON_OPT_UNITS_PER_METER;
	}
}

/* Orthonormal nose / right / up of the space scene's ship i, the nose
 * aimed at its target when it has one. */
static void env_ship_basis(int i, float n[3], float r[3], float u[3]) {
	const EnvSpaceScene* sc = env_space();
	const EnvShip* e = &sc->ships[i];
	float up[3];
	memcpy(n, e->nose, 3 * sizeof(float));
	memcpy(up, e->up, sizeof up);
	if (e->view) {
		/* From the frame of the line of sight to the craft. */
		static const float z[3] = { 0.0f, 0.0f, 1.0f };
		float f[3], vr[3], vu[3];
		env_dir(e->yaw, e->pitch, f);
		env_cross(f, z, vr);
		env_norm(vr);
		env_cross(vr, f, vu);
		for (int a = 0; a < 3; ++a) {
			n[a] = vr[a] * e->nose[0] + f[a] * e->nose[1] + vu[a] * e->nose[2];
			up[a] = vr[a] * e->up[0] + f[a] * e->up[1] + vu[a] * e->up[2];
		}
	}
	if (e->aim > 0 && e->aim <= sc->count) {
		float from[3], to[3];
		env_ship_pos(i, from);
		env_ship_pos(e->aim - 1, to);
		for (int a = 0; a < 3; ++a) {
			n[a] = to[a] - from[a];
		}
	}
	env_norm(n);
	env_cross(n, up, r);
	env_norm(r);
	env_cross(r, n, u);
}

static void env_layout(void) {
	s_env.count = 0;
	const EnvSpaceScene* space = env_space();
	if (space) {
		for (int i = 0; i < space->count && s_env.count < ENV_MAX_INSTANCES; ++i) {
			const int c = env_find(space->ships[i].opt);
			if (c < 0) {
				continue;
			}
			float pos[3];
			env_ship_pos(i, pos);
			s_env.mesh_of[s_env.count] = c;
			s_env.far[s_env.count] = space->ships[i].dist > 150.0f;
			float n[3], r[3], u[3];
			env_ship_basis(i, n, r, u);
			env_pose_matrix(n, u, pos, s_env.transform[s_env.count]);
			s_env.count++;
		}
		return;
	}
	/* The hangar at the launch reference (pitch 0x4000 = level). */
	env_add(env_find("Hangar"), 0x4000, 0, 0.0f, 0.0f, 0.0f);
	/* The player's craft where the classic parks it: 800 units behind the
	 * launch reference, standing on the floor. */
	if (env_craft()) {
		const int c = env_find(env_craft());
		if (c >= 0) {
			env_add(c, 0x4000, 0, 0.0f, -800.0f, s_env.floor_z + 0.5f * s_env.meshes[c].size_z);
		}
	}
	for (int i = 0; i < ENV_PROP_COUNT; ++i) {
		const EnvProp* p = &k_props[i];
		const int c = env_find(p->opt);
		if (c < 0) {
			continue;
		}
		const float z =
			p->z == ENV_ON_FLOOR ? s_env.floor_z + 0.5f * s_env.meshes[c].size_z : (float)p->z;
		env_add(c, 0x4000 + p->pitch, p->yaw, (float)p->x, (float)p->y, z);
	}
}

/* ---- sprite textures -------------------------------------------------- */

static float env_srgb_to_linear(float c) {
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float env_linear_to_srgb(float c) {
	return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

/* Straight-alpha sRGB RGBA -> premultiplied (in linear light) sRGB texture,
 * the billboards' PMA convention. */
static int env_make_texture(AeronCommandBuffer* cmd, int slot, const uint8_t* rgba, int w, int h) {
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
		return 0;
	}
	uint8_t* px = (uint8_t*)malloc((size_t)w * (size_t)h * 4u);
	if (!px) {
		return 0;
	}
	float lut[256];
	for (int i = 0; i < 256; ++i) {
		lut[i] = env_srgb_to_linear((float)i / 255.0f);
	}
	for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) {
		const float a = (float)rgba[i * 4 + 3] / 255.0f;
		for (int c = 0; c < 3; ++c) {
			px[i * 4 + c] = (uint8_t)(env_linear_to_srgb(lut[rgba[i * 4 + c]] * a) * 255.0f + 0.5f);
		}
		px[i * 4 + 3] = rgba[i * 4 + 3];
	}
	AeronTexture* t = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width = w,
		.height = h,
		.mip_count = 1,
		.format = AERON_TEXTURE_FORMAT_RGBA8_SRGB,
		.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.debug_name = "xwa.vr_env.sprite",
	});
	const int ok = t && Aeron_UploadTextureDataCmd(cmd, &(AeronTextureUploadDesc) {
		.texture = t,
		.width = w,
		.height = h,
		.raw_data = px,
		.raw_size = (size_t)w * (size_t)h * 4u,
	});
	free(px);
	if (!ok) {
		if (t) {
			Aeron_DestroyTexture(t);
		}
		return 0;
	}
	s_env.tex[slot] = t;
	s_env.tex_w[slot] = w;
	s_env.tex_h[slot] = h;
	return 1;
}

/* The beam profile: a bright core inside a soft glow across V, tapered at
 * both ends along U (additive; colour comes from the tint). */
static int env_make_beam(AeronCommandBuffer* cmd) {
	enum { BW = 64, BH = 32 };
	uint8_t px[BW * BH * 4];
	for (int y = 0; y < BH; ++y) {
		const float v = fabsf(((float)y + 0.5f) / (float)BH * 2.0f - 1.0f);
		for (int x = 0; x < BW; ++x) {
			float along = fminf(((float)x + 0.5f), ((float)BW - (float)x - 0.5f)) / 8.0f;
			along = fminf(along, 1.0f);
			const float core = expf(-v * v * 30.0f), glow = 0.45f * expf(-v * v * 4.0f);
			const float a = fminf(core + glow, 1.0f) * along;
			uint8_t* p = &px[(y * BW + x) * 4];
			p[0] = p[1] = p[2] = (uint8_t)(a * 255.0f + 0.5f);
			p[3] = 255;
		}
	}
	AeronTexture* t = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width = BW,
		.height = BH,
		.mip_count = 1,
		.format = AERON_TEXTURE_FORMAT_RGBA8_UNORM,
		.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.debug_name = "xwa.vr_env.beam",
	});
	if (!t || !Aeron_UploadTextureDataCmd(cmd, &(AeronTextureUploadDesc) {
			.texture = t,
			.width = BW,
			.height = BH,
			.raw_data = px,
			.raw_size = sizeof px,
		})) {
		if (t) {
			Aeron_DestroyTexture(t);
		}
		return 0;
	}
	s_env.tex[ENV_TEX_BEAM] = t;
	s_env.tex_w[ENV_TEX_BEAM] = BW;
	s_env.tex_h[ENV_TEX_BEAM] = BH;
	return 1;
}

/* Value noise on a wrapping lattice, for the nebula. */
static float env_hash(int x, int y, int seed) {
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return (float)((h ^ (h >> 16)) & 0xffffu) / 65535.0f;
}

static float env_noise(float x, float y, int seed) {
	const int xi = (int)floorf(x), yi = (int)floorf(y);
	float fx = x - (float)xi, fy = y - (float)yi;
	fx = fx * fx * (3.0f - 2.0f * fx);
	fy = fy * fy * (3.0f - 2.0f * fy);
	const float a = env_hash(xi, yi, seed), b = env_hash(xi + 1, yi, seed);
	const float c = env_hash(xi, yi + 1, seed), d = env_hash(xi + 1, yi + 1, seed);
	return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

static float env_fbm(float x, float y, int seed, int octaves) {
	float sum = 0.0f, amp = 0.5f, norm = 0.0f;
	for (int o = 0; o < octaves; ++o) {
		sum += amp * env_noise(x, y, seed + o * 17);
		norm += amp;
		x *= 2.03f;
		y *= 2.03f;
		amp *= 0.5f;
	}
	return sum / norm;
}

/* The box art's nebula: a long magenta-violet cloud, brighter pink
 * filaments in its core, ragged dark lanes, fading to nothing at the edges
 * (straight sRGB RGBA; made once, ~10 ms). */
static int env_make_nebula(AeronCommandBuffer* cmd) {
	enum { NW = 512, NH = 256 };
	uint8_t* px = (uint8_t*)malloc((size_t)NW * NH * 4u);
	if (!px) {
		return 0;
	}
	for (int y = 0; y < NH; ++y) {
		for (int x = 0; x < NW; ++x) {
			const float u = ((float)x + 0.5f) / (float)NW * 2.0f - 1.0f;
			const float v = ((float)y + 0.5f) / (float)NH * 2.0f - 1.0f;
			/* Domain warp for billowing shapes. */
			const float wx = env_fbm(u * 3.0f, v * 1.5f, 7, 4) - 0.5f;
			const float wy = env_fbm(u * 3.0f + 5.2f, v * 1.5f + 1.3f, 11, 4) - 0.5f;
			const float n = env_fbm(u * 4.0f + wx * 2.0f, v * 2.0f + wy * 2.0f, 3, 6);
			const float fil = env_fbm(u * 10.0f + wx * 4.0f, v * 5.0f + wy * 4.0f, 23, 5);
			/* Elliptical falloff, bent a little along its length. */
			const float bend = v - 0.25f * sinf(u * 2.2f);
			const float r = sqrtf(u * u + bend * bend * 1.6f);
			const float edge = fmaxf(0.0f, 1.0f - r);
			float density = fmaxf(0.0f, n - 0.32f) * 2.2f * edge * edge;
			const float lanes = fmaxf(0.0f, 0.55f - env_fbm(u * 6.0f + 3.1f, v * 3.0f - wx * 3.0f, 41, 5)) * 1.8f;
			density = fmaxf(0.0f, density - lanes * 0.35f * edge);
			const float core = powf(fmaxf(0.0f, fil - 0.45f) * 2.0f, 2.0f) * density;
			/* Violet body, magenta-pink to white-hot filaments. */
			float rr = 0.66f * density + 1.0f * core, gg = 0.18f * density + 0.62f * core,
				  bb = 0.80f * density + 0.90f * core;
			const float a = fminf(1.0f, density * 1.4f + core);
			uint8_t* p = &px[((size_t)y * NW + x) * 4];
			/* Straight alpha: the colour without coverage. */
			const float inv = a > 1e-4f ? 1.0f / a : 0.0f;
			p[0] = (uint8_t)(fminf(1.0f, rr * inv) * 255.0f + 0.5f);
			p[1] = (uint8_t)(fminf(1.0f, gg * inv) * 255.0f + 0.5f);
			p[2] = (uint8_t)(fminf(1.0f, bb * inv) * 255.0f + 0.5f);
			p[3] = (uint8_t)(a * 255.0f + 0.5f);
		}
	}
	const int ok = env_make_texture(cmd, ENV_TEX_NEBULA, px, NW, NH);
	free(px);
	return ok;
}

/* Image assets: "XWAL", u32 width, u32 height, straight RGBA. Shipped in
 * the APK (assets/vr/<name>.rgba) or next to the executable; never part of
 * the source tree. `override_env` names a variable that overrides the path.
 *   logo  — the game logo above the screen (XWA_XR_LOGO)
 *   frame — the frame around the 4:3 screen (XWA_XR_FRAME, 0 = none); its
 *           opening is the rectangle k_frame_opening in image pixels.
 *   vader — the helmet behind the TIE Fighter scene (XWA_XR_VADER), grey
 *           (tinted here), faded out at the bottom. */
static int env_load_rgba(AeronCommandBuffer* cmd, int slot, const char* name, const char* override_env) {
	const char* override = getenv(override_env);
	if (override && strcmp(override, "0") == 0) {
		return 0;
	}
	char asset[2][64];
	snprintf(asset[0], sizeof asset[0], "vr/%s.rgba", name);
	snprintf(asset[1], sizeof asset[1], "assets/vr/%s.rgba", name);
	const char* paths[] = { override && override[0] ? override : NULL, asset[0], asset[1] };
	for (size_t i = 0; i < sizeof paths / sizeof paths[0]; ++i) {
		if (!paths[i]) {
			continue;
		}
		size_t size = 0;
		uint8_t* data = (uint8_t*)SDL_LoadFile(paths[i], &size);
		if (!data) {
			continue;
		}
		int ok = 0;
		if (size >= 12 && memcmp(data, "XWAL", 4) == 0) {
			uint32_t w, h;
			memcpy(&w, data + 4, 4);
			memcpy(&h, data + 8, 4);
			if (w && h && w <= 4096 && h <= 4096 && size >= 12 + (size_t)w * h * 4u) {
				ok = env_make_texture(cmd, slot, data + 12, (int)w, (int)h);
			}
		}
		SDL_free(data);
		if (ok) {
			return 1;
		}
	}
	Aeron_LogInfo("xwa.remaster", "VR environment: no %s (vr/%s.rgba)", name, name);
	return 0;
}

static void env_load_dat_sprite(AeronCommandBuffer* cmd, int slot) {
	const EnvTexSource* src = &k_tex_sources[slot];
	/* The one DAT directly (the RESDATA.TXT index would read them all). */
	char path[2][64];
	snprintf(path[0], sizeof path[0], "Resdata/%s.dat", src->dat);
	snprintf(path[1], sizeof path[1], "RESDATA/%s.DAT", src->dat);
	for (char* c = path[1]; *c; ++c) {
		*c = (char)SDL_toupper((unsigned char)*c);
	}
	AeronVfs* vfs = Aeron_GetVfs();
	uint8_t* bytes = NULL;
	size_t size = 0;
	for (int i = 0; i < 2 && !bytes; ++i) {
		if (vfs && AeronVfs_Exists(vfs, AERON_VFS_ROOT_ASSET, path[i]) &&
			!AeronVfs_ReadAll(vfs, AERON_VFS_ROOT_ASSET, path[i], 64u << 20, &bytes, &size)) {
			bytes = NULL;
		}
	}
	if (!bytes) {
		Aeron_LogWarn("xwa.remaster", "VR environment: %s unavailable", path[0]);
		return;
	}
	Xwa2dFrameSet set;
	memset(&set, 0, sizeof set);
	char error[256];
	error[0] = 0;
	const int ok = Xwa2d_DatAppendGroup(bytes, size, (uint16_t)src->group, &set, error, sizeof error);
	free(bytes);
	if (!ok || !set.count) {
		Aeron_LogWarn("xwa.remaster", "VR environment: DAT group %d unavailable: %s", src->group, error);
		Xwa2dFrameSet_Free(&set);
		return;
	}
	const Xwa2dFrame* pick = NULL;
	for (int i = 0; i < set.count; ++i) {
		const Xwa2dFrame* f = &set.frames[i];
		if (src->sprite < 0 ? (!pick || f->sprite_id < pick->sprite_id) : f->sprite_id == src->sprite) {
			pick = f;
		}
	}
	if (pick && pick->rgba) {
		env_make_texture(cmd, slot, pick->rgba, pick->width, pick->height);
	}
	Xwa2dFrameSet_Free(&set);
}

/* The sprites a scene uses. */
static int env_tex_needed(int slot) {
	switch (s_env.mode) {
		case ENV_ENDOR:
			return slot != ENV_TEX_NEBULA && slot != ENV_TEX_VADER;
		case ENV_BOXART:
			return slot != ENV_TEX_DEATHSTAR && slot != ENV_TEX_ENDOR && slot != ENV_TEX_VADER;
		case ENV_TIEART:
			return slot != ENV_TEX_DEATHSTAR && slot != ENV_TEX_ENDOR;
		default:
			return slot == ENV_TEX_FRAME;
	}
}

/* One sprite per call; returns 1 when something was loaded. */
static int env_load_sprites(AeronCommandBuffer* cmd) {
	for (int i = 0; i < ENV_TEX_COUNT; ++i) {
		if (s_env.tex_tried[i] || !env_tex_needed(i)) {
			continue;
		}
		s_env.tex_tried[i] = 1;
		if (i == ENV_TEX_BEAM) {
			env_make_beam(cmd);
		} else if (i == ENV_TEX_NEBULA) {
			env_make_nebula(cmd);
		} else if (i == ENV_TEX_LOGO) {
			env_load_rgba(cmd, i, "logo", "XWA_XR_LOGO");
		} else if (i == ENV_TEX_FRAME) {
			env_load_rgba(cmd, i, "frame", "XWA_XR_FRAME");
		} else if (i == ENV_TEX_VADER) {
			env_load_rgba(cmd, i, "vader", "XWA_XR_VADER");
		} else {
			env_load_dat_sprite(cmd, i);
		}
		return 1;
	}
	return 0;
}

int XwaVrEnv_Prepare(AeronCommandBuffer* cmd) {
	if (!cmd) {
		return 0;
	}
	const EnvMode mode = env_mode();
	if (mode != s_env.mode) {
		XwaVrEnv_Release();
		s_env.mode = mode;
	}
	if (mode == ENV_OFF) {
		return 0;
	}
	env_list();
	/* One OPT (or sprite) per frame. */
	int busy = 0;
	for (int i = 0; i < s_env.mesh_count; ++i) {
		if (!s_env.meshes[i].tried) {
			env_load(cmd, i);
			s_env.laid_out = 0;
			busy = 1;
			break;
		}
	}
	if (!busy) {
		(void)env_load_sprites(cmd);
	}
	if (mode == ENV_HANGAR && !s_env.meshes[s_env.hangar].mesh) {
		if (s_env.meshes[s_env.hangar].tried && !s_env.logged) {
			Aeron_LogWarn("xwa.remaster", "VR environment: Hangar.opt unavailable, menus stay on the plain screen");
			s_env.logged = 1;
		}
		return 0;
	}
	if (!s_env.laid_out) {
		env_layout();
		s_env.laid_out = 1;
	}
	return 1;
}

void XwaVrEnv_View(float pos[3], float rows[9]) {
	/* XWA_XR_ENV_VIEW = x,y,yaw_degrees,eye_height_m (yaw 0 faces +y, the
	 * way the parked craft points; positive turns right). The space scene
	 * is laid out around the viewer at the origin. */
	float x = -500.0f, y = -1150.0f, yaw = 0.0f, eye = 1.7f;
	const char* v = getenv("XWA_XR_ENV_VIEW");
	if (env_space()) {
		x = y = 0.0f;
	} else if (v && v[0]) {
		sscanf(v, "%f,%f,%f,%f", &x, &y, &yaw, &eye);
	}
	const float r = yaw * 3.14159265358979f / 180.0f;
	const float f[3] = { sinf(r), cosf(r), 0.0f };
	/* XWA_XR_ENV_BACK: the viewer sits this far (metres) behind the scene's
	 * reference point, matching the screen being as much further away
	 * (the Quest default screen distance includes it). */
	const float back = env_float("XWA_XR_ENV_BACK", 0.8f) * AERON_OPT_UNITS_PER_METER;
	pos[0] = x - f[0] * back;
	pos[1] = y - f[1] * back;
	pos[2] = env_space() ? 0.0f : s_env.floor_z + eye * AERON_OPT_UNITS_PER_METER;
	memcpy(s_env.view_pos, pos, sizeof s_env.view_pos);
	rows[0] = f[1];
	rows[1] = -f[0];
	rows[2] = 0.0f; /* right */
	rows[3] = 0.0f;
	rows[4] = 0.0f;
	rows[5] = -1.0f; /* down */
	rows[6] = f[0];
	rows[7] = f[1];
	rows[8] = 0.0f; /* forward */
}

/* ---- billboards --------------------------------------------------------- */

/* Queue a quad: corners (world units) bottom-left, bottom-right, top-right,
 * top-left; tint linear RGBA (HDR allowed). */
static void env_quad(AeronScene3D* scene, int slot, AeronSceneBillboardStage stage, AeronSceneBillboardBlend blend,
					 const float c[4][3], const float tint[4]) {
	if (!s_env.tex[slot]) {
		return;
	}
	static const float uv[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
	AeronSceneBillboardDesc d;
	memset(&d, 0, sizeof d);
	d.texture = s_env.tex[slot];
	d.blend = blend;
	d.stage = stage;
	for (int v = 0; v < 4; ++v) {
		memcpy(d.corners[v], c[v], sizeof d.corners[v]);
		memcpy(d.uv[v], uv[v], sizeof d.uv[v]);
		/* PMA: alpha scales the colour too. */
		for (int k = 0; k < 3; ++k) {
			d.colors[v][k] = tint[k] * (blend == AERON_SCENE_BILLBOARD_BLEND_PMA ? tint[3] : 1.0f);
		}
		d.colors[v][3] = tint[3];
	}
	AeronScene_AddBillboard(scene, &d);
}

/* Corners of a rectangle at `center` spanned by half-axes `ax` (right) and
 * `ay` (up). */
static void env_rect(const float center[3], const float ax[3], const float ay[3], float c[4][3]) {
	for (int k = 0; k < 3; ++k) {
		c[0][k] = center[k] - ax[k] - ay[k];
		c[1][k] = center[k] + ax[k] - ay[k];
		c[2][k] = center[k] + ax[k] + ay[k];
		c[3][k] = center[k] - ax[k] + ay[k];
	}
}

/* A viewer-facing square of `size` (world units) at `center`, rolled by
 * `roll` degrees. */
static void env_facing(const float center[3], float size, float roll, float c[4][3]) {
	float f[3] = { center[0] - s_env.view_pos[0], center[1] - s_env.view_pos[1], center[2] - s_env.view_pos[2] };
	env_norm(f);
	static const float z[3] = { 0.0f, 0.0f, 1.0f };
	float r[3], u[3];
	env_cross(f, z, r);
	env_norm(r);
	env_cross(r, f, u);
	const float rr = roll * 3.14159265358979f / 180.0f, cr = cosf(rr), sr = sinf(rr);
	float ax[3], ay[3];
	for (int k = 0; k < 3; ++k) {
		ax[k] = (r[k] * cr + u[k] * sr) * 0.5f * size;
		ay[k] = (u[k] * cr - r[k] * sr) * 0.5f * size;
	}
	env_rect(center, ax, ay, c);
}

/* Sky sprites are drawn at the far plane; the distance only sets the
 * projection direction. */
#define ENV_SKY_DIST 2.0e6f

static void env_sky_point(float yaw, float pitch, float out[3]) {
	env_dir(yaw, pitch, out);
	for (int k = 0; k < 3; ++k) {
		out[k] = s_env.view_pos[k] + out[k] * ENV_SKY_DIST;
	}
}

/* A sky sprite of `size_deg` angular size toward yaw/pitch. */
static void env_sky_sprite(AeronScene3D* scene, int slot, float yaw, float pitch, float size_deg, float roll,
						   const float tint[4], float corners[4][3]) {
	float center[3];
	env_sky_point(yaw, pitch, center);
	env_facing(center, 2.0f * ENV_SKY_DIST * tanf(size_deg * 3.14159265358979f / 360.0f), roll, corners);
	env_quad(scene, slot, AERON_SCENE_BILLBOARD_STAGE_SKY, AERON_SCENE_BILLBOARD_BLEND_PMA, corners, tint);
}

/* A beam strip from `a` to `b` (world units), `width` wide, facing the
 * viewer. */
static void env_beam(AeronScene3D* scene, AeronSceneBillboardStage stage, const float a[3], const float b[3],
					 float width, const float tint[4]) {
	float along[3], mid[3], side[3];
	for (int k = 0; k < 3; ++k) {
		along[k] = b[k] - a[k];
		mid[k] = 0.5f * (a[k] + b[k]) - s_env.view_pos[k];
	}
	env_cross(along, mid, side);
	env_norm(side);
	float c[4][3];
	for (int k = 0; k < 3; ++k) {
		side[k] *= 0.5f * width;
		c[0][k] = a[k] - side[k];
		c[1][k] = b[k] - side[k];
		c[2][k] = b[k] + side[k];
		c[3][k] = a[k] + side[k];
	}
	env_quad(scene, ENV_TEX_BEAM, stage, AERON_SCENE_BILLBOARD_BLEND_ADDITIVE, c, tint);
}

/* The frame's opening (image pixels) that the 4:3 screen fills exactly;
 * the frame image is drawn behind the screen's quad layer, in its plane. */
static const float k_frame_opening[4] = { 114.0f, 144.0f, 1133.0f, 908.0f }; /* x0, y0, x1, y1 */

/* Where the 4:3 picture sits on the virtual screen: the 2D layers are
 * fitted into the screen quad keeping their aspect, centred. Lengths in
 * real metres; `k` converts them to scene units at the head's scale
 * (XWA_XR_WORLD_SCALE scales tracked head motion, so the screen's real
 * distance is k * metres for the stereo to agree with the quad layer). */
typedef struct EnvScreen {
	float dist, w, h, lift, k;
	/* The picture as the frame's opening shows it: the screen's picture
	 * divided by XWA_XR_FRAME_OVERSCAN (1.0: the picture fills the opening
	 * exactly). The opening is the frame's clear 4:3 area enlarged 2 %: the
	 * bezel overlaps under 1 % of the picture's edges (no menu text), dark
	 * backing fills any gap. The
	 * frame sits XWA_XR_FRAME_DEPTH (3 cm) in front of the picture for a
	 * little depth, scaled so its opening lines up from the viewer (leaning
	 * 20 cm shifts the two by under 4 mm, about a game pixel). */
	float pic_w, pic_h;
	float frame_dist, frame_scale;
} EnvScreen;

static void env_screen(EnvScreen* out) {
	float dist = 2.1f, sw = 2.1f, sh = 1.575f, lift = 0.0f;
	if (!Aeron_XrVirtualScreenGeometry(&dist, &sw, &sh, &lift)) {
		dist = env_float("AERON_XR_SCREEN_DISTANCE", dist);
		sw = env_float("AERON_XR_SCREEN_WIDTH", sw);
		sh = 0.75f * sw;
		lift = env_float("AERON_XR_SCREEN_HEIGHT_OFFSET", lift);
	}
	out->w = fminf(sw, sh * 4.0f / 3.0f);
	out->h = 0.75f * out->w;
	out->dist = dist;
	out->lift = lift;
	out->k = AERON_OPT_UNITS_PER_METER * env_float("XWA_XR_WORLD_SCALE", 1.0f);
	const float overscan = fmaxf(env_float("XWA_XR_FRAME_OVERSCAN", 1.0f), 0.5f);
	out->pic_w = out->w / overscan;
	out->pic_h = out->h / overscan;
	out->frame_dist = fmaxf(dist - env_float("XWA_XR_FRAME_DEPTH", 0.03f), 0.2f);
	out->frame_scale = dist > 0.01f ? out->frame_dist / dist : 1.0f;
}

/* Height (metres, in the picture's plane) the frame rises above the top of
 * the picture. */
static float env_frame_top(const EnvScreen* sc) {
	if (!s_env.tex[ENV_TEX_FRAME]) {
		return 0.0f;
	}
	return k_frame_opening[1] * sc->pic_w / (k_frame_opening[2] - k_frame_opening[0]);
}

/* The frame quad in head-relative metres: centre (x right, y up, z ahead)
 * and size. */
static void env_frame_quad(const EnvScreen* sc, float centre[3], float size[2]) {
	const float px = sc->frame_scale * sc->pic_w / (k_frame_opening[2] - k_frame_opening[0]);
	const float tw = (float)s_env.tex_w[ENV_TEX_FRAME], th = (float)s_env.tex_h[ENV_TEX_FRAME];
	size[0] = tw * px;
	size[1] = th * px;
	/* Image centre relative to the opening's centre (image y runs down). */
	centre[0] = (0.5f * tw - 0.5f * (k_frame_opening[0] + k_frame_opening[2])) * px;
	centre[1] = sc->lift * sc->frame_scale - (0.5f * th - 0.5f * (k_frame_opening[1] + k_frame_opening[3])) * px;
	centre[2] = sc->frame_dist;
}

/* Fallback when there is no frame layer: the frame as a scene sprite
 * (under the picture, which the compositor draws on top). */
static void env_submit_frame(AeronScene3D* scene) {
	if (!s_env.tex[ENV_TEX_FRAME] || s_env.frame_layer_ok) {
		return;
	}
	EnvScreen sc;
	env_screen(&sc);
	float q[3], size[2];
	env_frame_quad(&sc, q, size);
	const float k = sc.k;
	const float center[3] = { s_env.view_pos[0] + q[0] * k, s_env.view_pos[1] + q[2] * k,
							  s_env.view_pos[2] + q[1] * k };
	const float ax[3] = { 0.5f * size[0] * k, 0.0f, 0.0f };
	const float ay[3] = { 0.0f, 0.0f, 0.5f * size[1] * k };
	float c[4][3];
	env_rect(center, ax, ay, c);
	static const float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	env_quad(scene, ENV_TEX_FRAME, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, tint);
}

int XwaVrEnv_LayerRect(float centre[3], float size[2]) {
	if (!centre || !size) {
		return 0;
	}
	EnvScreen sc;
	env_screen(&sc);
	if (sc.dist <= 0.0f || sc.w <= 0.0f) {
		return 0;
	}
	if (s_env.tex[ENV_TEX_FRAME] && s_env.frame_layer_ok) {
		float q[3];
		env_frame_quad(&sc, q, size);
		centre[0] = q[0];
		centre[1] = q[1];
		centre[2] = -q[2];
		/* The frame always takes in the picture behind it. */
		return 1;
	}
	centre[0] = 0.0f;
	centre[1] = sc.lift;
	centre[2] = -sc.dist;
	size[0] = sc.w;
	size[1] = sc.h;
	return 1;
}

int XwaVrEnv_CommitFrameLayer(AeronCommandBuffer* cmd, float brightness) {
	if (!cmd || !s_env.tex[ENV_TEX_FRAME] || s_env.tex_w[ENV_TEX_FRAME] > AERON_XR_FRAME_WIDTH ||
		s_env.tex_h[ENV_TEX_FRAME] > AERON_XR_FRAME_HEIGHT) {
		s_env.frame_layer_ok = 0;
		return 0;
	}
	const float tint = fminf(fmaxf(brightness, 0.0f), 1.0f);
	if (!s_env.frame_drawn || fabsf(tint - s_env.frame_tint) > 0.004f) {
		int w = 0, h = 0;
		AeronRenderTarget* target = Aeron_XrStereoPanelTarget(AERON_XR_PANEL_FRAME, &w, &h);
		if (!target) {
			s_env.frame_layer_ok = 0;
			return 0;
		}
		if (!s_env.frame_list) {
			s_env.frame_list = AeronDrawList_Create(8);
		}
		if (!s_env.frame_list) {
			s_env.frame_layer_ok = 0;
			return 0;
		}
		AeronDrawList_Begin(s_env.frame_list, target, w, h, AERON_DRAWLIST2D_CLEAR, NULL);
		AeronDrawList2DSprite sp;
		memset(&sp, 0, sizeof sp);
		sp.texture = s_env.tex[ENV_TEX_FRAME];
		sp.src_u1 = 1.0f;
		sp.src_v1 = 1.0f;
		sp.dst_w = (float)s_env.tex_w[ENV_TEX_FRAME];
		sp.dst_h = (float)s_env.tex_h[ENV_TEX_FRAME];
		sp.tint[0] = sp.tint[1] = sp.tint[2] = tint;
		sp.tint[3] = 1.0f;
		sp.blend = AERON_BLIT2D_BLEND_NONE; /* premultiplied texels as they are */
		sp.filter = AERON_BLIT2D_FILTER_NEAREST;
		AeronDrawList_AddSprite(s_env.frame_list, &sp);
		AeronDrawList_Render(s_env.frame_list, cmd);
		s_env.frame_drawn = 1;
		s_env.frame_tint = tint;
	}
	EnvScreen sc;
	env_screen(&sc);
	float q[3], size[2];
	env_frame_quad(&sc, q, size);
	AeronXrPanel p;
	memset(&p, 0, sizeof p);
	p.rect[2] = s_env.tex_w[ENV_TEX_FRAME];
	p.rect[3] = s_env.tex_h[ENV_TEX_FRAME];
	p.position[0] = q[0];
	p.position[1] = q[1];
	p.position[2] = -q[2]; /* OpenXR: -Z ahead */
	p.orientation[3] = 1.0f;
	p.size[0] = size[0];
	p.size[1] = size[1];
	Aeron_XrStereoCommitPanelLayer(AERON_XR_PANEL_FRAME, &p, 1);
	s_env.frame_layer_ok = 1;
	return 1;
}

/* A sky sprite `w_deg` x `h_deg` toward yaw/pitch, rolled (for the
 * nebula's long clouds). */
static void env_sky_rect(AeronScene3D* scene, int slot, float yaw, float pitch, float w_deg, float h_deg, float roll,
						 const float tint[4]) {
	if (!s_env.tex[slot]) {
		return;
	}
	float center[3], f[3], r[3], u[3];
	static const float z[3] = { 0.0f, 0.0f, 1.0f };
	env_sky_point(yaw, pitch, center);
	for (int k = 0; k < 3; ++k) {
		f[k] = center[k] - s_env.view_pos[k];
	}
	env_norm(f);
	env_cross(f, z, r);
	env_norm(r);
	env_cross(r, f, u);
	const float rr = roll * 3.14159265358979f / 180.0f, cr = cosf(rr), sr = sinf(rr);
	const float hw = ENV_SKY_DIST * tanf(fminf(w_deg, 170.0f) * 3.14159265358979f / 360.0f);
	const float hh = ENV_SKY_DIST * tanf(fminf(h_deg, 170.0f) * 3.14159265358979f / 360.0f);
	float ax[3], ay[3], c[4][3];
	for (int k = 0; k < 3; ++k) {
		ax[k] = (r[k] * cr + u[k] * sr) * hw;
		ay[k] = (u[k] * cr - r[k] * sr) * hh;
	}
	env_rect(center, ax, ay, c);
	env_quad(scene, slot, AERON_SCENE_BILLBOARD_STAGE_SKY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, tint);
}

/* The dogfight: each salvo frozen along its shooter's nose. */
static void env_submit_salvos(AeronScene3D* scene) {
	const EnvSpaceScene* sc = env_space();
	const float k = AERON_OPT_UNITS_PER_METER;
	for (int si = 0; si < sc->salvo_count; ++si) {
		const EnvSalvo* sv = &sc->salvos[si];
		float from[3], to[3], n[3], r[3], u[3];
		env_ship_pos(sv->shooter, from);
		env_ship_pos(sv->target, to);
		env_ship_basis(sv->shooter, n, r, u);
		const float dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
		const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
		static const float xw_guns[4][2] = { { 5.2f, 1.3f }, { -5.2f, 1.3f }, { 5.2f, -1.3f }, { -5.2f, -1.3f } };
		static const float tie_guns[2][2] = { { 0.6f, -1.0f }, { -0.6f, -1.0f } };
		static const float aw_guns[2][2] = { { 3.3f, 0.0f }, { -3.3f, 0.0f } };
		static const float int_guns[2][2] = { { 3.6f, 1.6f }, { -3.6f, 1.6f } };
		static const float red[4] = { 2.4f, 0.35f, 0.25f, 1.0f };
		static const float green[4] = { 0.4f, 2.4f, 0.45f, 1.0f };
		const float(*guns)[2] = xw_guns;
		int gun_count = 4;
		const float* tint = red;
		switch (sv->guns) {
			case ENV_GUNS_TIE:
				guns = tie_guns;
				gun_count = 2;
				tint = green;
				break;
			case ENV_GUNS_AWING:
				guns = aw_guns;
				gun_count = 2;
				break;
			case ENV_GUNS_INTERCEPTOR:
				guns = int_guns;
				gun_count = 2;
				tint = green;
				break;
			default:
				break;
		}
		for (int sp = 0; sp < sv->span_count; ++sp) {
			for (int g = 0; g < gun_count; ++g) {
				float a0[3], a1[3];
				for (int a = 0; a < 3; ++a) {
					const float gun = from[a] + (r[a] * guns[g][0] + u[a] * guns[g][1]) * k;
					a0[a] = gun + n[a] * dist * sv->spans[sp][0];
					a1[a] = gun + n[a] * dist * sv->spans[sp][1];
				}
				env_beam(scene, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, a0, a1, 0.4f * k, tint);
			}
		}
	}
}

/* An explosion at `p` (world units), a little toward the viewer so the
 * depth test keeps the near half of a craft inside it visible. `scale` 1 =
 * a TIE's. */
static void env_submit_burst_at(AeronScene3D* scene, const float p[3], float scale) {
	const float k = AERON_OPT_UNITS_PER_METER;
	float f[3], r[3], c[4][3];
	static const float z[3] = { 0.0f, 0.0f, 1.0f };
	for (int a = 0; a < 3; ++a) {
		f[a] = p[a] - s_env.view_pos[a];
	}
	env_norm(f);
	env_cross(f, z, r);
	env_norm(r);
	static const float white[4] = { 1.5f, 1.4f, 1.3f, 1.0f };
	float q[3];
	for (int a = 0; a < 3; ++a) {
		q[a] = p[a] + (r[a] * 4.5f - f[a] * 5.0f) * k * scale;
	}
	env_facing(q, 13.0f * k * scale, 0.0f, c);
	env_quad(scene, ENV_TEX_BLAST, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, white);
	for (int a = 0; a < 3; ++a) {
		q[a] = p[a] + (r[a] * 5.5f + z[a] * 1.2f - f[a] * 5.5f) * k * scale;
	}
	env_facing(q, 7.0f * k * scale, 30.0f, c);
	env_quad(scene, ENV_TEX_FIRE, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, white);
}

/* A ship bursting: the explosion over its right half. */
static void env_submit_burst(AeronScene3D* scene, int ship, float scale) {
	float p[3];
	env_ship_pos(ship, p);
	env_submit_burst_at(scene, p, scale);
}

/* The logo above the virtual screen and its frame. */
static void env_submit_logo(AeronScene3D* scene) {
	if (!s_env.tex[ENV_TEX_LOGO]) {
		return;
	}
	EnvScreen sc;
	env_screen(&sc);
	float c[4][3];
	/* In the frame's plane, above it. */
	const float f = sc.frame_scale;
	const float lw = 0.6f * sc.pic_w * f * env_float("XWA_XR_LOGO_SCALE", 1.0f);
	const float lh = lw * (float)s_env.tex_h[ENV_TEX_LOGO] / (float)s_env.tex_w[ENV_TEX_LOGO];
	const float center[3] = { s_env.view_pos[0], s_env.view_pos[1] + (sc.frame_dist + 0.02f) * sc.k,
							  s_env.view_pos[2] +
								  ((sc.lift + 0.5f * sc.pic_h + env_frame_top(&sc)) * f + 0.05f + 0.5f * lh) * sc.k };
	const float ax[3] = { 0.5f * lw * sc.k, 0.0f, 0.0f };
	const float ay[3] = { 0.0f, 0.0f, 0.5f * lh * sc.k };
	env_rect(center, ax, ay, c);
	static const float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	env_quad(scene, ENV_TEX_LOGO, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, tint);
}

static void env_submit_endor_sprites(AeronScene3D* scene) {
	const float k = AERON_OPT_UNITS_PER_METER;
	float c[4][3];
	/* Endor's rim below right, then the Death Star over it. */
	static const float endor_tint[4] = { 0.62f, 0.95f, 0.66f, 1.0f };
	env_sky_sprite(scene, ENV_TEX_ENDOR, 40.0f, -48.0f, 70.0f, 15.0f, endor_tint, c);
	static const float ds_tint[4] = { 0.9f, 0.9f, 1.0f, 1.0f };
	float ds[4][3];
	env_sky_sprite(scene, ENV_TEX_DEATHSTAR, ENV_DS_YAW, ENV_DS_PITCH, 22.1f, -10.0f, ds_tint, ds);
	/* The superlaser: from the dish (texel ~0.36, 0.31 of the sprite) to
	 * the burning cruiser. */
	if (s_env.tex[ENV_TEX_DEATHSTAR]) {
		const float u = 0.36f, v = 0.31f;
		float dish[3], target[3];
		for (int a = 0; a < 3; ++a) {
			const float bottom = ds[0][a] + (ds[1][a] - ds[0][a]) * u;
			const float top = ds[3][a] + (ds[2][a] - ds[3][a]) * u;
			dish[a] = top + (bottom - top) * v;
		}
		env_sky_point(k_endor[ENV_BURST].yaw, k_endor[ENV_BURST].pitch, target);
		const float wide = 2.0f * ENV_SKY_DIST * tanf(1.2f * 3.14159265358979f / 360.0f);
		const float thin = 2.0f * ENV_SKY_DIST * tanf(0.35f * 3.14159265358979f / 360.0f);
		static const float glow[4] = { 0.25f, 1.6f, 0.35f, 1.0f };
		static const float core[4] = { 1.2f, 2.2f, 1.2f, 1.0f };
		env_beam(scene, AERON_SCENE_BILLBOARD_STAGE_SKY, dish, target, wide, glow);
		env_beam(scene, AERON_SCENE_BILLBOARD_STAGE_SKY, dish, target, thin, core);
	}
	/* The cruiser bursting. */
	{
		float p[3], q[3];
		env_dir(k_endor[ENV_BURST].yaw, k_endor[ENV_BURST].pitch, p);
		env_dir(k_endor[ENV_BURST].yaw + 1.5f, k_endor[ENV_BURST].pitch + 1.0f, q);
		const float d = (k_endor[ENV_BURST].dist - 200.0f) * k;
		for (int a = 0; a < 3; ++a) {
			p[a] = s_env.view_pos[a] + p[a] * d;
			q[a] = s_env.view_pos[a] + q[a] * (d - 100.0f * k);
		}
		static const float white[4] = { 1.6f, 1.5f, 1.4f, 1.0f };
		env_facing(p, 2400.0f * k, 0.0f, c);
		env_quad(scene, ENV_TEX_BLAST, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, white);
		env_facing(q, 1300.0f * k, 40.0f, c);
		env_quad(scene, ENV_TEX_FIRE, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, white);
	}
	env_submit_salvos(scene);
	/* The stricken TIE. */
	env_submit_burst(scene, ENV_VICTIM, 1.0f);
	/* Two bolts from the Falcon's top turret toward the TIE below the
	 * screen, frozen in flight. */
	{
		float from[3], to[3];
		env_ship_pos(ENV_FALCON, from);
		env_ship_pos(ENV_FALCON_TARGET, to);
		float up[3] = { k_endor[ENV_FALCON].up[0], k_endor[ENV_FALCON].up[1], k_endor[ENV_FALCON].up[2] };
		env_norm(up);
		for (int a = 0; a < 3; ++a) {
			from[a] += up[a] * 2.5f * k;
		}
		static const float red[4] = { 2.4f, 0.35f, 0.25f, 1.0f };
		static const float spans[2][2] = { { 0.05f, 0.15f }, { 0.24f, 0.34f } };
		for (int b = 0; b < 2; ++b) {
			float a0[3], a1[3];
			for (int a = 0; a < 3; ++a) {
				a0[a] = from[a] + (to[a] - from[a]) * spans[b][0];
				a1[a] = from[a] + (to[a] - from[a]) * spans[b][1];
			}
			env_beam(scene, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, a0, a1, 0.6f * k, red);
		}
	}
	env_submit_logo(scene);
}

/* The X-Wing box art: the nebula, the fire and the burst. */
static void env_submit_boxart_sprites(AeronScene3D* scene) {
	/* The nebula: a long magenta cloud from the upper left down through
	 * the view to the lower right, as on the cover. XWA_XR_NEBULA_BRIGHTNESS
	 * scales it. */
	{
		const float b = env_float("XWA_XR_NEBULA_BRIGHTNESS", 1.0f);
		static const struct {
			float yaw, pitch, w, h, roll, gain;
		} clouds[] = {
			{ -10.0f, 12.0f, 120.0f, 62.0f, -32.0f, 1.15f }, /* the main band, behind the screen */
			{ -62.0f, 38.0f, 80.0f, 40.0f, -28.0f, 1.0f },   /* its upper left end */
			{ 42.0f, -30.0f, 90.0f, 44.0f, -38.0f, 0.9f },   /* lower right, behind the big X-wing */
		};
		for (size_t i = 0; i < sizeof clouds / sizeof clouds[0]; ++i) {
			const float g = clouds[i].gain * b;
			const float tint[4] = { g, g, g, 1.0f };
			env_sky_rect(scene, ENV_TEX_NEBULA, clouds[i].yaw, clouds[i].pitch, clouds[i].w, clouds[i].h, clouds[i].roll,
						 tint);
		}
	}
	env_submit_salvos(scene);
	{
		float p[3];
		env_dir(k_box_burst[0], k_box_burst[1], p);
		for (int a = 0; a < 3; ++a) {
			p[a] = s_env.view_pos[a] + p[a] * k_box_burst[2] * AERON_OPT_UNITS_PER_METER;
		}
		env_submit_burst_at(scene, p, 1.6f);
	}
	env_submit_logo(scene);
}

/* The TIE Fighter box art: the helmet and the clouds behind, the fire, the
 * great explosion with the X-wing breaking up in it. */
static void env_submit_tieart_sprites(AeronScene3D* scene) {
	const float k = AERON_OPT_UNITS_PER_METER;
	/* Vader's helmet, looming behind the screen and the fight, a dark
	 * blue-violet; XWA_XR_VADER_BRIGHTNESS scales it. */
	if (s_env.tex[ENV_TEX_VADER]) {
		const float b = env_float("XWA_XR_VADER_BRIGHTNESS", 1.0f);
		const float tint[4] = { 0.18f * b, 0.17f * b, 0.42f * b, 1.0f };
		const float h = 58.5f; /* 35 % smaller than the cover's */
		const float w = h * (float)s_env.tex_w[ENV_TEX_VADER] / (float)s_env.tex_h[ENV_TEX_VADER];
		env_sky_rect(scene, ENV_TEX_VADER, 0.0f, 12.0f, w, h, 0.0f, tint);
	}
	/* Purple clouds below. */
	{
		static const struct {
			float yaw, pitch, w, h, roll, gain;
		} clouds[] = {
			{ -30.0f, -42.0f, 110.0f, 40.0f, 6.0f, 0.55f },
			{ 35.0f, -40.0f, 100.0f, 36.0f, -8.0f, 0.5f },
			{ 0.0f, -58.0f, 120.0f, 46.0f, 0.0f, 0.45f },
		};
		for (size_t i = 0; i < sizeof clouds / sizeof clouds[0]; ++i) {
			const float g = clouds[i].gain;
			const float tint[4] = { 0.55f * g, 0.45f * g, 1.0f * g, 1.0f };
			env_sky_rect(scene, ENV_TEX_NEBULA, clouds[i].yaw, clouds[i].pitch, clouds[i].w, clouds[i].h, clouds[i].roll,
						 tint);
		}
	}
	env_submit_salvos(scene);
	/* The great explosion behind the big Interceptor, flames licking round
	 * the breaking X-wing. */
	{
		float p[3], q[3], c[4][3];
		static const float white[4] = { 1.5f, 1.4f, 1.3f, 1.0f };
		/* Centred on the point: the blast with two fire clouds in it. */
		static const struct {
			float dyaw, dpitch, size, roll;
			int tex;
		} puffs[] = {
			{ 0.0f, 0.0f, 1.0f, 0.0f, ENV_TEX_BLAST },
			{ -6.0f, 4.0f, 0.62f, 40.0f, ENV_TEX_FIRE },
			{ 7.0f, -3.0f, 0.55f, -60.0f, ENV_TEX_FIRE },
		};
		for (size_t i = 0; i < sizeof puffs / sizeof puffs[0]; ++i) {
			env_dir(k_tie_blast[0] + puffs[i].dyaw, k_tie_blast[1] + puffs[i].dpitch, p);
			for (int a = 0; a < 3; ++a) {
				p[a] = s_env.view_pos[a] + p[a] * (k_tie_blast[2] - (float)i * 3.0f) * k;
			}
			env_facing(p, k_tie_blast[3] * puffs[i].size * k, puffs[i].roll, c);
			env_quad(scene, puffs[i].tex, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, white);
		}
		env_ship_pos(5, q);
		env_facing(q, 16.0f * k, 70.0f, c);
		env_quad(scene, ENV_TEX_FIRE, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, white);
		env_facing(q, 10.0f * k, -20.0f, c);
		env_quad(scene, ENV_TEX_BLAST, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, white);
	}
	env_submit_logo(scene);
}

void XwaVrEnv_Submit(AeronScene3D* scene) {
	for (int i = 0; i < s_env.count; ++i) {
		AeronSceneMeshInstance inst;
		memset(&inst, 0, sizeof inst);
		inst.mesh = s_env.meshes[s_env.mesh_of[i]].mesh;
		memcpy(inst.transform, s_env.transform[i], sizeof inst.transform);
		memcpy(inst.prev_transform, s_env.transform[i], sizeof inst.transform);
		inst.zero_velocity = 1;
		inst.no_local_lights = 1;
		inst.cull_mode = AERON_CULL_BACK;
		if ((s_env.mode == ENV_HANGAR && i == 0) || s_env.far[i]) {
			/* The enclosed hangar must not shadow its own ceiling light;
			 * distant capital ships stay out of the shadow volume. */
			inst.shadow_flags |= AERON_SCENE_INSTANCE_NO_CAST_SHADOW;
		}
		AeronScene_AddMeshInstance(scene, &inst);
	}
	if (s_env.mode == ENV_ENDOR) {
		env_submit_endor_sprites(scene);
	} else if (s_env.mode == ENV_BOXART) {
		env_submit_boxart_sprites(scene);
	} else if (s_env.mode == ENV_TIEART) {
		env_submit_tieart_sprites(scene);
	}
	env_submit_frame(scene);
}

void XwaVrEnv_Release(void) {
	for (int i = 0; i < s_env.mesh_count; ++i) {
		if (s_env.meshes[i].mesh) {
			AeronScene_MeshDestroy(s_env.meshes[i].mesh);
		}
	}
	for (int i = 0; i < ENV_TEX_COUNT; ++i) {
		if (s_env.tex[i]) {
			Aeron_DestroyTexture(s_env.tex[i]);
		}
	}
	if (s_env.frame_list) {
		AeronDrawList_Destroy(s_env.frame_list);
	}
	memset(&s_env, 0, sizeof s_env);
}

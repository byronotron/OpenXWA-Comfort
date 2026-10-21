#include "xwa_remaster/vr_intro_hoth.h"
#include "aeron/aeron.h"
#include "aeron/asset/opt_model.h"
#include "aeron/log.h"
#include "aeron/scene/billboard.h"
#include "aeron/scene/mesh.h"
#include "aeron/vfs.h"
#include "xwa_2d.h"
#include "xwa_remaster/ship.h"
#include <SDL3/SDL_stdinc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the storyboard ---------------------------------------------------------
 *
 * Directions as seen by the seated viewer: azimuth degrees (0 = the screen,
 * + = right), elevation degrees (+ = up), distance in metres. The film's
 * timings (INTRO_FINAL.SNM, 15 fps) were measured frame by frame: its
 * camera turns 49 degrees left onto Hoth over 52-63 s, the transport shows
 * against the planet from 60.9 s and fills the picture as it passes the
 * camera at 65.7 s, the camera whips round after it to the Star Destroyers
 * and the film cuts to an X-wing's cockpit at 67.93 s. */

#define HOTH_PLANET_GROUP 6030 /* the palest, icy world of PLANET.DAT: the film's Hoth */
#define HOTH_AZ (-70.0f)
#define HOTH_EL (-13.0f)
#define HOTH_RADIUS_DEG 45.0f
#define HOTH_FADE_FROM 50.0f
#define HOTH_FADE_TO 53.0f
/* The craft are drawn from here (out at 1.4-1.9 km, unlit). */
#define HOTH_SHIPS_FROM 51.0f
/* The light comes up over these seconds: the craft lit as they near, the
 * Star Destroyers out of the dark. */
#define HOTH_LIGHT_FROM 62.5f
#define HOTH_LIGHT_TO 66.0f
#define HOTH_LIGHT_FLOOR 0.004f
/* Where the run goes after the cut: between the two Star Destroyers. */
#define HOTH_AIM_AZ 18.0f
#define HOTH_AIM_EL 3.0f
#define HOTH_AIM_DIST 2600.0f
#define HOTH_RUN_SPEED 120.0f

typedef struct HothKey {
	float t, az, el, dist;
} HothKey;

static const HothKey k_transport[] = {
	{ 51.0f, -76.0f, 0.0f, 1450.0f },  { 60.9f, -75.0f, -1.0f, 450.0f }, { 62.9f, -74.0f, -1.2f, 265.0f },
	{ 64.7f, -66.0f, -2.0f, 105.0f },  { 65.2f, -55.0f, -3.0f, 60.0f },  { 65.7f, -15.0f, -9.0f, 18.0f },
	{ 66.3f, 40.0f, -2.0f, 45.0f },    { 66.9f, 35.0f, -0.5f, 95.0f },   { 67.93f, 22.0f, 0.5f, 220.0f },
};
static const HothKey k_xwing1[] = {
	{ 51.0f, -73.0f, 4.0f, 1650.0f }, { 62.9f, -72.0f, 3.0f, 470.0f }, { 64.8f, -69.0f, 3.7f, 260.0f },
	{ 65.6f, -65.0f, 4.7f, 160.0f },  { 66.36f, -48.0f, 8.5f, 62.0f }, { 66.75f, 0.0f, 14.0f, 30.0f },
	{ 67.2f, 40.0f, 5.0f, 70.0f },    { 67.93f, 28.0f, 2.0f, 160.0f },
};
static const HothKey k_xwing2[] = {
	{ 51.0f, -80.0f, 6.0f, 1900.0f }, { 62.9f, -79.0f, 5.0f, 520.0f }, { 65.0f, -72.0f, 8.0f, 245.0f },
	{ 65.7f, -63.0f, 12.2f, 141.0f }, { 66.4f, -15.0f, 25.0f, 60.0f }, { 66.95f, 24.0f, 15.0f, 95.0f },
	{ 67.93f, 14.0f, 8.0f, 220.0f },
};

typedef struct HothCapital {
	float az, el, dist;
} HothCapital;
/* Star Destroyer 1 (the lead, higher and nearer) and 2, their bows towards
 * the incoming ships (three-quarter view). */
static const HothCapital k_isd[2] = { { 30.0f, 7.0f, 1600.0f }, { 8.0f, 2.0f, 2600.0f } };

enum { HOTH_TRANSPORT, HOTH_XWING, HOTH_ISD, HOTH_MESHES };
static const char* const k_opt[HOTH_MESHES] = { "FamilyTransport", "XWing", "ImperialStarDestroyer2" };

#define HOTH_MAX_GLOWS 8

typedef struct HothMesh {
	AeronSceneMesh* mesh;
	int glow_count;
	float glow[HOTH_MAX_GLOWS][7]; /* model metres x y z, radius, core rgb */
	float len_m;
} HothMesh;

static struct {
	int step; /* the next load step */
	HothMesh mesh[HOTH_MESHES];
	AeronTexture* planet;
	float planet_disc; /* the disc's radius over half the image's width */
	AeronTexture* glow;
	AeronTexture* beam;
	/* The last Submit's view. */
	int submitted;
	float org[3], right[3], up[3], fwd[3], k;
} s_hoth;

/* ---- small maths ------------------------------------------------------------ */

static float hoth_clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
static float hoth_smooth(float x) {
	x = hoth_clamp01(x);
	return x * x * (3.0f - 2.0f * x);
}
static void hoth_dir(float az, float el, float d, float out[3]) {
	const float a = az * 0.017453293f, e = el * 0.017453293f;
	out[0] = d * cosf(e) * sinf(a);
	out[1] = d * sinf(e);
	out[2] = -d * cosf(e) * cosf(a);
}
static float hoth_dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void hoth_cross(const float a[3], const float b[3], float out[3]) {
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}
static int hoth_norm(float v[3]) {
	const float l = sqrtf(hoth_dot(v, v));
	if (l < 1e-9f) {
		return 0;
	}
	v[0] /= l;
	v[1] /= l;
	v[2] /= l;
	return 1;
}
/* A deterministic 0..1 from an integer. */
static float hoth_hash(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return (float)(x & 0xffffffU) / 16777215.0f;
}

/* LOCAL direction -> world direction (no scale). */
static void hoth_wdir(const float v[3], float out[3]) {
	for (int i = 0; i < 3; ++i) {
		out[i] = v[0] * s_hoth.right[i] + v[1] * s_hoth.up[i] - v[2] * s_hoth.fwd[i];
	}
}
/* LOCAL metres -> world. */
static void hoth_wpos(const float p[3], float out[3]) {
	float d[3];
	hoth_wdir(p, d);
	for (int i = 0; i < 3; ++i) {
		out[i] = s_hoth.org[i] + s_hoth.k * d[i];
	}
}

/* ---- the paths ---------------------------------------------------------------- */

/* Position on a keyed path at `t` (LOCAL metres): a cubic Hermite through
 * the keys with tangents from the neighbours, then a straight run at the
 * aim point. */
static void hoth_path(const HothKey* keys, int n, float t, float out[3]) {
	if (t <= keys[0].t) {
		hoth_dir(keys[0].az, keys[0].el, keys[0].dist, out);
		return;
	}
	if (t >= keys[n - 1].t) {
		float last[3], before[3], aim[3], d[3];
		hoth_dir(keys[n - 1].az, keys[n - 1].el, keys[n - 1].dist, last);
		hoth_dir(HOTH_AIM_AZ, HOTH_AIM_EL, HOTH_AIM_DIST, aim);
		hoth_dir(keys[n - 2].az, keys[n - 2].el, keys[n - 2].dist, before);
		for (int a = 0; a < 3; ++a) {
			d[a] = aim[a] - last[a];
		}
		hoth_norm(d);
		for (int a = 0; a < 3; ++a) {
			out[a] = last[a] + d[a] * HOTH_RUN_SPEED * (t - keys[n - 1].t);
		}
		(void)before;
		return;
	}
	int i = 0;
	while (i < n - 2 && t > keys[i + 1].t) {
		++i;
	}
	float p[4][3];
	float tt[4];
	for (int j = -1; j <= 2; ++j) {
		int idx = i + j;
		if (idx < 0) {
			idx = 0;
		}
		if (idx >= n) {
			/* Past the last key: the run's point one second on. */
			float last[3], aim[3], d[3];
			hoth_dir(keys[n - 1].az, keys[n - 1].el, keys[n - 1].dist, last);
			hoth_dir(HOTH_AIM_AZ, HOTH_AIM_EL, HOTH_AIM_DIST, aim);
			for (int a = 0; a < 3; ++a) {
				d[a] = aim[a] - last[a];
			}
			hoth_norm(d);
			for (int a = 0; a < 3; ++a) {
				p[j + 1][a] = last[a] + d[a] * HOTH_RUN_SPEED;
			}
			tt[j + 1] = keys[n - 1].t + 1.0f;
			continue;
		}
		hoth_dir(keys[idx].az, keys[idx].el, keys[idx].dist, p[j + 1]);
		tt[j + 1] = keys[idx].t;
	}
	const float t0 = tt[1], t1 = tt[2], h = t1 - t0;
	const float s = (t - t0) / h;
	const float h00 = 2.0f * s * s * s - 3.0f * s * s + 1.0f, h10 = s * s * s - 2.0f * s * s + s;
	const float h01 = -2.0f * s * s * s + 3.0f * s * s, h11 = s * s * s - s * s;
	for (int a = 0; a < 3; ++a) {
		const float m0 = (i > 0 ? (p[2][a] - p[0][a]) / (tt[2] - tt[0]) : (p[2][a] - p[1][a]) / h);
		const float m1 = (p[3][a] - p[1][a]) / (tt[3] - tt[1]);
		out[a] = h00 * p[1][a] + h10 * h * m0 + h01 * p[2][a] + h11 * h * m1;
	}
}

/* A craft's pose on its path: position, nose along the flight and up
 * banked into the turn. */
static void hoth_pose(const HothKey* keys, int n, float t, float pos[3], float nose[3], float upv[3]) {
	const float h = 0.05f;
	float a[3], b[3];
	hoth_path(keys, n, t, pos);
	hoth_path(keys, n, t - h, a);
	hoth_path(keys, n, t + h, b);
	float acc[3];
	for (int i = 0; i < 3; ++i) {
		nose[i] = b[i] - a[i];
		acc[i] = (b[i] - 2.0f * pos[i] + a[i]) / (h * h);
	}
	if (!hoth_norm(nose)) {
		nose[0] = 0.0f;
		nose[1] = 0.0f;
		nose[2] = -1.0f;
	}
	/* Bank: up leans into the turn (lift = gravity-like pull + the turn's
	 * pull), at most 70 degrees. */
	const float along = hoth_dot(acc, nose);
	float lat[3];
	for (int i = 0; i < 3; ++i) {
		lat[i] = acc[i] - along * nose[i];
	}
	const float g = 30.0f;
	float u[3] = { lat[0], g + lat[1], lat[2] };
	const float ln = sqrtf(hoth_dot(lat, lat));
	if (ln > g * 2.75f) { /* tan(70 deg) */
		const float sc = g * 2.75f / ln;
		u[0] = lat[0] * sc;
		u[1] = g + lat[1] * sc;
		u[2] = lat[2] * sc;
	}
	/* Orthogonal to the nose. */
	const float un = hoth_dot(u, nose);
	for (int i = 0; i < 3; ++i) {
		upv[i] = u[i] - un * nose[i];
	}
	if (!hoth_norm(upv)) {
		upv[0] = 0.0f;
		upv[1] = 1.0f;
		upv[2] = 0.0f;
	}
}

/* ---- loading ---------------------------------------------------------------- */

static float hoth_srgb_to_linear(float c) {
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}
static float hoth_linear_to_srgb(float c) {
	return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static AeronTexture* hoth_texture(AeronCommandBuffer* cmd, const uint8_t* px, int w, int h, int srgb,
								  const char* name) {
	AeronTexture* t = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width = w,
		.height = h,
		.mip_count = 1,
		.format = srgb ? AERON_TEXTURE_FORMAT_RGBA8_SRGB : AERON_TEXTURE_FORMAT_RGBA8_UNORM,
		.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.debug_name = name,
	});
	if (!t || !Aeron_UploadTextureDataCmd(cmd, &(AeronTextureUploadDesc) {
			.texture = t,
			.width = w,
			.height = h,
			.raw_data = px,
			.raw_size = (size_t)w * (size_t)h * 4u,
		})) {
		if (t) {
			Aeron_DestroyTexture(t);
		}
		return NULL;
	}
	return t;
}

/* The planet: the group's first sprite (its fully lit face), premultiplied. */
static void hoth_load_planet(AeronCommandBuffer* cmd) {
	static const char* const paths[2] = { "Resdata/Planet.dat", "RESDATA/PLANET.DAT" };
	AeronVfs* vfs = Aeron_GetVfs();
	uint8_t* bytes = NULL;
	size_t size = 0;
	for (int i = 0; i < 2 && !bytes; ++i) {
		if (vfs && AeronVfs_Exists(vfs, AERON_VFS_ROOT_ASSET, paths[i]) &&
			!AeronVfs_ReadAll(vfs, AERON_VFS_ROOT_ASSET, paths[i], 64u << 20, &bytes, &size)) {
			bytes = NULL;
		}
	}
	if (!bytes) {
		Aeron_LogWarn("xwa.remaster", "VR intro Hoth: no Resdata/Planet.dat");
		return;
	}
	Xwa2dFrameSet set;
	memset(&set, 0, sizeof set);
	char error[256];
	error[0] = 0;
	const int ok = Xwa2d_DatAppendGroup(bytes, size, (uint16_t)HOTH_PLANET_GROUP, &set, error, sizeof error);
	free(bytes);
	const Xwa2dFrame* pick = NULL;
	for (int i = 0; ok && i < set.count; ++i) {
		if (set.frames[i].rgba && (!pick || set.frames[i].sprite_id < pick->sprite_id)) {
			pick = &set.frames[i];
		}
	}
	if (pick) {
		const int w = pick->width, h = pick->height;
		uint8_t* px = (uint8_t*)malloc((size_t)w * (size_t)h * 4u);
		if (px) {
			/* The disc's radius: the farthest opaque texel from the centre. */
			float r2max = 0.0f;
			for (int y = 0; y < h; ++y) {
				for (int x = 0; x < w; ++x) {
					const uint8_t* s = &pick->rgba[((size_t)y * w + x) * 4];
					uint8_t* d = &px[((size_t)y * w + x) * 4];
					const float a = (float)s[3] / 255.0f;
					for (int c = 0; c < 3; ++c) {
						d[c] = (uint8_t)(hoth_linear_to_srgb(hoth_srgb_to_linear((float)s[c] / 255.0f) * a) * 255.0f +
										 0.5f);
					}
					d[3] = s[3];
					if (s[3] > 128) {
						const float dx = ((float)x + 0.5f) / (float)w - 0.5f, dy = ((float)y + 0.5f) / (float)h - 0.5f;
						r2max = fmaxf(r2max, dx * dx + dy * dy);
					}
				}
			}
			s_hoth.planet_disc = r2max > 0.01f ? 2.0f * sqrtf(r2max) : 0.95f;
			s_hoth.planet = hoth_texture(cmd, px, w, h, 1, "xwa.vr_intro_hoth.planet");
			free(px);
		}
	} else {
		Aeron_LogWarn("xwa.remaster", "VR intro Hoth: planet group %d unavailable: %s", HOTH_PLANET_GROUP, error);
	}
	Xwa2dFrameSet_Free(&set);
}

/* A soft round glow (engines) and a bolt: a bright core along u, soft
 * across v, tapered at both ends. */
static void hoth_make_sprites(AeronCommandBuffer* cmd) {
	enum { G = 64, BW = 64, BH = 16 };
	static uint8_t glow[G * G * 4], beam[BW * BH * 4];
	for (int y = 0; y < G; ++y) {
		for (int x = 0; x < G; ++x) {
			const float dx = ((float)x + 0.5f) / (float)G * 2.0f - 1.0f, dy = ((float)y + 0.5f) / (float)G * 2.0f - 1.0f;
			const float r2 = dx * dx + dy * dy;
			const float a = r2 >= 1.0f ? 0.0f : fminf(1.0f, expf(-r2 * 9.0f) + 0.35f * expf(-r2 * 2.5f)) * (1.0f - r2);
			uint8_t* p = &glow[(y * G + x) * 4];
			p[0] = p[1] = p[2] = (uint8_t)(a * 255.0f + 0.5f);
			p[3] = 255;
		}
	}
	for (int y = 0; y < BH; ++y) {
		for (int x = 0; x < BW; ++x) {
			const float u = ((float)x + 0.5f) / (float)BW, v = ((float)y + 0.5f) / (float)BH * 2.0f - 1.0f;
			const float across = expf(-v * v * 5.0f) + 0.6f * expf(-v * v * 40.0f);
			const float ends = hoth_smooth(u / 0.15f) * hoth_smooth((1.0f - u) / 0.15f);
			const float a = fminf(1.0f, across * ends);
			uint8_t* p = &beam[(y * BW + x) * 4];
			p[0] = p[1] = p[2] = (uint8_t)(a * 255.0f + 0.5f);
			p[3] = 255;
		}
	}
	s_hoth.glow = hoth_texture(cmd, glow, G, G, 0, "xwa.vr_intro_hoth.glow");
	s_hoth.beam = hoth_texture(cmd, beam, BW, BH, 0, "xwa.vr_intro_hoth.beam");
}

static void hoth_load_mesh(AeronCommandBuffer* cmd, int index) {
	HothMesh* m = &s_hoth.mesh[index];
	AeronFlightModel model;
	memset(&model, 0, sizeof model);
	if (!XwaRemasterShip_BuildOptModel(k_opt[index], &model)) {
		Aeron_LogWarn("xwa.remaster", "VR intro Hoth: no OPT for '%s'", k_opt[index]);
		return;
	}
	m->len_m = model.bounds.max.y - model.bounds.min.y;
	Aeron_LogInfo("xwa.remaster", "VR intro Hoth: %s %.0f x %.0f x %.0f m", k_opt[index],
				  (double)(model.bounds.max.x - model.bounds.min.x), (double)m->len_m,
				  (double)(model.bounds.max.z - model.bounds.min.z));
	m->glow_count = 0;
	for (uint32_t g = 0; g < model.engine_glow_count && m->glow_count < HOTH_MAX_GLOWS; ++g) {
		const AeronFlightEngineGlow* eg = &model.engine_glows[g];
		if (!eg->enabled) {
			continue;
		}
		float* o = m->glow[m->glow_count++];
		o[0] = eg->position.x;
		o[1] = eg->position.y;
		o[2] = eg->position.z;
		o[3] = 0.45f * fmaxf(eg->dimensions.x, eg->dimensions.y);
		o[4] = eg->core_rgba[0];
		o[5] = eg->core_rgba[1];
		o[6] = eg->core_rgba[2];
	}
	AeronSceneMeshCreateStatus status;
	m->mesh = AeronScene_MeshCreate(cmd, &model, k_opt[index], &status);
	Aeron_FlightModelFree(&model);
	if (!m->mesh) {
		Aeron_LogWarn("xwa.remaster", "VR intro Hoth: '%s' mesh creation failed (%d)", k_opt[index], (int)status);
	}
}

int XwaVrIntroHoth_Load(AeronCommandBuffer* cmd) {
	if (!cmd) {
		return 0;
	}
	switch (s_hoth.step) {
		case 0:
			hoth_load_planet(cmd);
			hoth_make_sprites(cmd);
			break;
		case 1:
		case 2:
		case 3:
			hoth_load_mesh(cmd, s_hoth.step - 1);
			break;
		default:
			return 1;
	}
	if (++s_hoth.step == 4) {
		Aeron_LogInfo("xwa.remaster", "VR intro Hoth: %s (planet %s, transport %s, X-wing %s, Star Destroyer %s)",
					  XwaVrIntroHoth_Ready() ? "ready" : "not available", s_hoth.planet ? "yes" : "no",
					  s_hoth.mesh[HOTH_TRANSPORT].mesh ? "yes" : "no", s_hoth.mesh[HOTH_XWING].mesh ? "yes" : "no",
					  s_hoth.mesh[HOTH_ISD].mesh ? "yes" : "no");
		return 1;
	}
	return 0;
}

int XwaVrIntroHoth_Ready(void) {
	return s_hoth.step >= 4 && s_hoth.planet && s_hoth.mesh[HOTH_TRANSPORT].mesh && s_hoth.mesh[HOTH_XWING].mesh &&
		   s_hoth.mesh[HOTH_ISD].mesh;
}

void XwaVrIntroHoth_Release(void) {
	for (int i = 0; i < HOTH_MESHES; ++i) {
		if (s_hoth.mesh[i].mesh) {
			AeronScene_MeshDestroy(s_hoth.mesh[i].mesh);
		}
	}
	AeronTexture* tex[3] = { s_hoth.planet, s_hoth.glow, s_hoth.beam };
	for (int i = 0; i < 3; ++i) {
		if (tex[i]) {
			Aeron_DestroyTexture(tex[i]);
		}
	}
	memset(&s_hoth, 0, sizeof s_hoth);
}

/* ---- drawing ------------------------------------------------------------------ */

static void hoth_billboard(AeronScene3D* scene, AeronTexture* tex, AeronSceneBillboardStage stage,
						   AeronSceneBillboardBlend blend, const float c[4][3], const float tint[4]) {
	AeronSceneBillboardDesc d;
	memset(&d, 0, sizeof d);
	d.texture = tex;
	d.blend = blend;
	d.stage = stage;
	static const float uv[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
	for (int i = 0; i < 4; ++i) {
		memcpy(d.corners[i], c[i], sizeof d.corners[i]);
		memcpy(d.uv[i], uv[i], sizeof d.uv[i]);
		memcpy(d.colors[i], tint, sizeof d.colors[i]);
	}
	AeronScene_AddBillboard(scene, &d);
}

/* A viewer-facing square (world units) round world point `c`. */
static void hoth_facing(const float c[3], float half, float out[4][3]) {
	float f[3] = { c[0] - s_hoth.org[0], c[1] - s_hoth.org[1], c[2] - s_hoth.org[2] };
	hoth_norm(f);
	float r[3], u[3];
	hoth_cross(f, s_hoth.up, r);
	if (!hoth_norm(r)) {
		memcpy(r, s_hoth.right, sizeof r);
	}
	hoth_cross(r, f, u);
	for (int k = 0; k < 3; ++k) {
		out[0][k] = c[k] - half * r[k] - half * u[k];
		out[1][k] = c[k] + half * r[k] - half * u[k];
		out[2][k] = c[k] + half * r[k] + half * u[k];
		out[3][k] = c[k] - half * r[k] + half * u[k];
	}
}

static void hoth_submit_planet(AeronScene3D* scene, float t) {
	const float fade = hoth_smooth((t - HOTH_FADE_FROM) / (HOTH_FADE_TO - HOTH_FADE_FROM));
	if (!s_hoth.planet || fade <= 0.0f) {
		return;
	}
	/* A sky quad (drawn under everything, at the far plane): its distance
	 * only sets the projection. */
	const float dist = 1.0e5f;
	float dir[3], cw[3], c[4][3];
	hoth_dir(HOTH_AZ, HOTH_EL, 1.0f, dir);
	hoth_wdir(dir, cw);
	float centre[3];
	for (int k = 0; k < 3; ++k) {
		centre[k] = s_hoth.org[k] + cw[k] * dist * s_hoth.k;
	}
	const float half = dist * s_hoth.k * tanf(HOTH_RADIUS_DEG * 0.017453293f) / fmaxf(s_hoth.planet_disc, 0.5f);
	hoth_facing(centre, half, c);
	const float tint[4] = { fade, fade, fade, fade }; /* premultiplied */
	hoth_billboard(scene, s_hoth.planet, AERON_SCENE_BILLBOARD_STAGE_SKY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, tint);
}

/* Model -> world of a craft at LOCAL `pos` with LOCAL nose and up. */
static void hoth_matrix(const float pos[3], const float nose[3], const float upv[3], float m[16]) {
	float n[3], u[3], r[3], p[3];
	hoth_wdir(nose, n);
	hoth_wdir(upv, u);
	hoth_cross(n, u, r);
	hoth_norm(r);
	hoth_cross(r, n, u);
	hoth_wpos(pos, p);
	const float k = s_hoth.k;
	for (int i = 0; i < 3; ++i) {
		m[i * 4 + 0] = k * r[i];
		m[i * 4 + 1] = -k * n[i];
		m[i * 4 + 2] = k * u[i];
		m[i * 4 + 3] = p[i];
	}
	m[12] = m[13] = m[14] = 0.0f;
	m[15] = 1.0f;
}

static void hoth_add(AeronScene3D* scene, int mesh, const float m[16], int far) {
	if (!s_hoth.mesh[mesh].mesh) {
		return;
	}
	AeronSceneMeshInstance inst;
	memset(&inst, 0, sizeof inst);
	inst.mesh = s_hoth.mesh[mesh].mesh;
	memcpy(inst.transform, m, sizeof inst.transform);
	memcpy(inst.prev_transform, m, sizeof inst.prev_transform);
	inst.zero_velocity = 1;
	inst.no_local_lights = 1;
	inst.cull_mode = AERON_CULL_BACK;
	if (far) {
		inst.shadow_flags |= AERON_SCENE_INSTANCE_NO_CAST_SHADOW;
	}
	AeronScene_AddMeshInstance(scene, &inst);
}

/* The engines' glow, seen from behind. */
static void hoth_glows(AeronScene3D* scene, int mesh, const float m[16]) {
	const HothMesh* hm = &s_hoth.mesh[mesh];
	if (!s_hoth.glow || !hm->glow_count) {
		return;
	}
	float back[3] = { m[1], m[5], m[9] }; /* model +y: the rear */
	hoth_norm(back);
	for (int g = 0; g < hm->glow_count; ++g) {
		const float* o = hm->glow[g];
		float p[3], to[3], c[4][3];
		for (int a = 0; a < 3; ++a) {
			p[a] = m[a * 4 + 0] * o[0] + m[a * 4 + 1] * o[1] + m[a * 4 + 2] * o[2] + m[a * 4 + 3];
			to[a] = s_hoth.org[a] - p[a];
		}
		hoth_norm(to);
		const float w = hoth_smooth(hoth_dot(back, to) * 1.6f);
		if (w <= 0.0f) {
			continue;
		}
		for (int a = 0; a < 3; ++a) {
			p[a] += to[a] * 0.6f * s_hoth.k;
		}
		const float b = 1.8f * w;
		const float core[4] = { (0.4f + 0.6f * o[4]) * b, (0.15f + 0.85f * o[5]) * b, (0.15f + 0.85f * o[6]) * b,
								1.0f };
		hoth_facing(p, o[3] * s_hoth.k, c);
		hoth_billboard(scene, s_hoth.glow, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_ADDITIVE,
					   c, core);
	}
}

/* A bolt from LOCAL `a` to `b`, `width` metres, facing the viewer. */
static void hoth_bolt(AeronScene3D* scene, const float a[3], const float b[3], float width, const float rgb[3]) {
	if (!s_hoth.beam) {
		return;
	}
	float wa[3], wb[3], along[3], mid[3], side[3];
	hoth_wpos(a, wa);
	hoth_wpos(b, wb);
	for (int k = 0; k < 3; ++k) {
		along[k] = wb[k] - wa[k];
		mid[k] = 0.5f * (wa[k] + wb[k]) - s_hoth.org[k];
	}
	hoth_cross(along, mid, side);
	if (!hoth_norm(side)) {
		return;
	}
	const float hw = 0.5f * width * s_hoth.k;
	AeronSceneBillboardDesc d;
	memset(&d, 0, sizeof d);
	d.texture = s_hoth.beam;
	d.blend = AERON_SCENE_BILLBOARD_BLEND_ADDITIVE;
	d.stage = AERON_SCENE_BILLBOARD_STAGE_OVERLAY;
	for (int k = 0; k < 3; ++k) {
		d.corners[0][k] = wa[k] - side[k] * hw;
		d.corners[1][k] = wb[k] - side[k] * hw;
		d.corners[2][k] = wb[k] + side[k] * hw;
		d.corners[3][k] = wa[k] + side[k] * hw;
	}
	static const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	for (int i = 0; i < 4; ++i) {
		memcpy(d.uv[i], uv[i], sizeof d.uv[i]);
		d.colors[i][0] = rgb[0];
		d.colors[i][1] = rgb[1];
		d.colors[i][2] = rgb[2];
		d.colors[i][3] = 1.0f;
	}
	AeronScene_AddBillboard(scene, &d);
}

typedef struct HothCraft {
	const HothKey* keys;
	int n;
} HothCraft;
static const HothCraft k_craft[3] = {
	{ k_transport, (int)(sizeof k_transport / sizeof k_transport[0]) },
	{ k_xwing1, (int)(sizeof k_xwing1 / sizeof k_xwing1[0]) },
	{ k_xwing2, (int)(sizeof k_xwing2 / sizeof k_xwing2[0]) },
};

/* Turbolasers from the Star Destroyers at the ships (near misses), from
 * 66 s; the X-wings' lasers at the Star Destroyers from 66.9 s. Each bolt
 * is a function of the film's time alone (no state). */
static void hoth_submit_fire(AeronScene3D* scene, float t) {
	static const float green[3] = { 0.35f * 4.0f, 1.0f * 4.0f, 0.4f * 4.0f };
	static const float red[3] = { 1.0f * 4.0f, 0.18f * 4.0f, 0.12f * 4.0f };
	const float speed_green = 650.0f, speed_red = 900.0f;
	for (int i = 0; i < 64; ++i) {
		const float tf = 66.0f + 0.27f * (float)i;
		if (tf > t) {
			break;
		}
		const HothCapital* c = &k_isd[i & 1];
		float from[3], dirc[3];
		hoth_dir(c->az, c->el, c->dist, from);
		/* A turret along the hull (+- a third of its length, across). */
		hoth_dir(c->az + 90.0f, 0.0f, 1.0f, dirc);
		const float along = (hoth_hash((uint32_t)i * 3u + 1u) - 0.5f) * 0.6f * s_hoth.mesh[HOTH_ISD].len_m;
		for (int a = 0; a < 3; ++a) {
			from[a] += dirc[a] * along;
		}
		from[1] += 60.0f + 80.0f * hoth_hash((uint32_t)i * 3u + 2u);
		const HothCraft* target = &k_craft[i % 3];
		float aim[3];
		float dist0 = 0.0f;
		{
			float tp[3];
			hoth_path(target->keys, target->n, tf + 3.0f, tp);
			for (int a = 0; a < 3; ++a) {
				dist0 += (tp[a] - from[a]) * (tp[a] - from[a]);
			}
			dist0 = sqrtf(dist0);
			const float travel = dist0 / speed_green;
			hoth_path(target->keys, target->n, tf + travel, aim);
		}
		/* Near misses: 20-60 m off. */
		for (int a = 0; a < 3; ++a) {
			aim[a] += (hoth_hash((uint32_t)i * 7u + (uint32_t)a) - 0.5f) * 80.0f;
		}
		float d[3] = { aim[0] - from[0], aim[1] - from[1], aim[2] - from[2] };
		const float len = sqrtf(hoth_dot(d, d));
		if (len < 1.0f) {
			continue;
		}
		hoth_norm(d);
		const float s = (t - tf) * speed_green;
		if (s > len * 1.6f) {
			continue;
		}
		float a0[3], a1[3];
		for (int a = 0; a < 3; ++a) {
			a0[a] = from[a] + d[a] * fmaxf(s - 70.0f, 0.0f);
			a1[a] = from[a] + d[a] * s;
		}
		hoth_bolt(scene, a0, a1, 5.0f, green);
	}
	for (int i = 0; i < 48; ++i) {
		const float tf = 66.9f + 0.22f * (float)i;
		if (tf > t) {
			break;
		}
		const HothCraft* shooter = &k_craft[1 + (i & 1)];
		float pos[3], nose[3], upv[3];
		hoth_pose(shooter->keys, shooter->n, tf, pos, nose, upv);
		float side[3];
		hoth_cross(nose, upv, side);
		hoth_norm(side);
		const float tip = ((i >> 1) & 1) ? 5.5f : -5.5f;
		float to[3];
		hoth_dir(k_isd[(i >> 2) & 1].az, k_isd[(i >> 2) & 1].el, k_isd[(i >> 2) & 1].dist, to);
		float d[3];
		for (int a = 0; a < 3; ++a) {
			pos[a] += side[a] * tip;
			d[a] = to[a] - pos[a];
		}
		hoth_norm(d);
		const float s = (t - tf) * speed_red;
		if (s > 2000.0f) {
			continue;
		}
		float a0[3], a1[3];
		for (int a = 0; a < 3; ++a) {
			a0[a] = pos[a] + d[a] * fmaxf(s - 18.0f, 0.0f);
			a1[a] = pos[a] + d[a] * s;
		}
		hoth_bolt(scene, a0, a1, 0.9f, red);
	}
}

void XwaVrIntroHoth_Submit(AeronScene3D* scene, const float org[3], const float right[3], const float up[3],
						   const float fwd[3], float k, float t) {
	s_hoth.submitted = 0;
	if (!scene || !XwaVrIntroHoth_Ready() || t < HOTH_FADE_FROM) {
		return;
	}
	memcpy(s_hoth.org, org, sizeof s_hoth.org);
	memcpy(s_hoth.right, right, sizeof s_hoth.right);
	memcpy(s_hoth.up, up, sizeof s_hoth.up);
	memcpy(s_hoth.fwd, fwd, sizeof s_hoth.fwd);
	s_hoth.k = k;
	s_hoth.submitted = 1;
	hoth_submit_planet(scene, t);
	if (t < HOTH_SHIPS_FROM) {
		return;
	}
	float m[16];
	/* The Star Destroyers wait, bows towards the ships coming in. */
	for (int i = 0; i < 2; ++i) {
		float pos[3], nose[3];
		hoth_dir(k_isd[i].az, k_isd[i].el, k_isd[i].dist, pos);
		/* Bows to the left and a little towards the viewer: long in view. */
		nose[0] = -1.0f;
		nose[1] = 0.0f;
		nose[2] = 0.35f;
		hoth_norm(nose);
		/* Rolled ~25 degrees towards the viewer: the decks and the bridge
		 * towers in view, not an edge-on sliver. */
		float tilt[3] = { -pos[0], -pos[1], -pos[2] };
		hoth_norm(tilt);
		const float roll_up[3] = { 0.47f * tilt[0], 1.0f + 0.47f * tilt[1], 0.47f * tilt[2] };
		hoth_matrix(pos, nose, roll_up, m);
		hoth_add(scene, HOTH_ISD, m, 1);
	}
	for (int c = 0; c < 3; ++c) {
		float pos[3], nose[3], upv[3];
		hoth_pose(k_craft[c].keys, k_craft[c].n, t, pos, nose, upv);
		const int mesh = c == 0 ? HOTH_TRANSPORT : HOTH_XWING;
		hoth_matrix(pos, nose, upv, m);
		hoth_add(scene, mesh, m, sqrtf(hoth_dot(pos, pos)) > 400.0f);
		hoth_glows(scene, mesh, m);
	}
	hoth_submit_fire(scene, t);
}

int XwaVrIntroHoth_Lighting(float t, XwaVrEnvLighting* out) {
	if (!out || !s_hoth.submitted) {
		return 0;
	}
	memset(out, 0, sizeof *out);
	const float l = HOTH_LIGHT_FLOOR +
					(1.0f - HOTH_LIGHT_FLOOR) * hoth_smooth((t - HOTH_LIGHT_FROM) / (HOTH_LIGHT_TO - HOTH_LIGHT_FROM));
	/* The sun from the front right, above (the film's craft are lit from
	 * there); Hoth's cool bounce from the left below; space's ambient. */
	float key[3], fill[3];
	hoth_dir(55.0f, 25.0f, 1.0f, key);
	hoth_dir(HOTH_AZ, HOTH_EL, 1.0f, fill);
	hoth_wdir(key, out->key_dir);
	hoth_wdir(fill, out->fill_dir);
	const float sun = 1.6f * l, bounce = 0.35f * l;
	out->key_color[0] = 1.0f * sun;
	out->key_color[1] = 0.97f * sun;
	out->key_color[2] = 0.92f * sun;
	out->fill_color[0] = 0.55f * bounce;
	out->fill_color[1] = 0.68f * bounce;
	out->fill_color[2] = 0.85f * bounce;
	for (int c = 0; c < 3; ++c) {
		out->ambient_sides[c] = 0.035f * l;
		out->ambient_top[c] = 0.035f * l;
		out->ambient_bottom[c] = 0.04f * l;
	}
	return 1;
}

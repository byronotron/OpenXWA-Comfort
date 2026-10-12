/* VR pilot gloves; see vr_gloves.h. */

#include "xwa_remaster/vr_gloves.h"

#include "aeron/aeron.h"
#include "aeron/log.h"
#include "aeron/scene/gltf_mesh.h"
#include "aeron/scene/mesh.h"
#include "xwa_remaster/vr_crest_pin.h"

#include <SDL3/SDL_timer.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Small 3x3 math (row-major, column vectors) ----------------------- */

typedef struct M3 {
	float m[9];
} M3;

static M3 m3_identity(void) {
	M3 r = { { 1, 0, 0, 0, 1, 0, 0, 0, 1 } };
	return r;
}

static M3 m3_mul(const M3* a, const M3* b) {
	M3 r;
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			r.m[i * 3 + j] = a->m[i * 3 + 0] * b->m[0 * 3 + j] + a->m[i * 3 + 1] * b->m[1 * 3 + j] +
							 a->m[i * 3 + 2] * b->m[2 * 3 + j];
		}
	}
	return r;
}

static void m3_apply(const M3* a, const float v[3], float out[3]) {
	float t[3];
	for (int i = 0; i < 3; ++i) {
		t[i] = a->m[i * 3 + 0] * v[0] + a->m[i * 3 + 1] * v[1] + a->m[i * 3 + 2] * v[2];
	}
	memcpy(out, t, sizeof t);
}

/* Rotation about +Y by `rad` (maps -Z towards -X for positive angles). */
static M3 m3_rot_y(float rad) {
	const float c = cosf(rad), s = sinf(rad);
	M3 r = { { c, 0, s, 0, 1, 0, -s, 0, c } };
	return r;
}

static M3 m3_rot_axis(const float axis[3], float rad) {
	const float c = cosf(rad), s = sinf(rad), t = 1.0f - c;
	const float x = axis[0], y = axis[1], z = axis[2];
	M3 r = { { t * x * x + c, t * x * y - s * z, t * x * z + s * y, t * x * y + s * z, t * y * y + c,
			   t * y * z - s * x, t * x * z - s * y, t * y * z + s * x, t * z * z + c } };
	return r;
}

/* Quaternion (x, y, z, w) -> rotation matrix. */
static M3 m3_from_quat(const float q[4]) {
	const float x = q[0], y = q[1], z = q[2], w = q[3];
	M3 r = { { 1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y), 2 * (x * y + w * z),
			   1 - 2 * (x * x + z * z), 2 * (y * z - w * x), 2 * (x * z - w * y), 2 * (y * z + w * x),
			   1 - 2 * (x * x + y * y) } };
	return r;
}

static void v3_norm(float v[3]) {
	const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l > 1e-8f) {
		v[0] /= l;
		v[1] /= l;
		v[2] /= l;
	}
}

static void v3_cross(const float a[3], const float b[3], float out[3]) {
	const float t[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
	memcpy(out, t, sizeof t);
}

/* Frame whose -Z points along `dir`; X stays as close to `hint` as possible. */
static M3 m3_from_dir(const float dir[3], const float hint[3]) {
	float z[3] = { -dir[0], -dir[1], -dir[2] }, x[3], y[3];
	v3_norm(z);
	v3_cross(hint, z, y); /* y = hint x z */
	v3_norm(y);
	v3_cross(y, z, x);
	M3 r = { { x[0], y[0], z[0], x[1], y[1], z[1], x[2], y[2], z[2] } };
	return r;
}

/* ---- Procedural meshes ------------------------------------------------- */

enum { GM_CYLINDER, GM_SPHERE, GM_ROUNDBOX, GM_CUFF, GM_PIN, GM_COUNT };

typedef struct GloveMeshData {
	AeronGltfVertex* vertices;
	uint16_t* indices;
	uint32_t vertex_count, index_count;
	AeronGltfMaterial material;
	uint32_t prim_material;
} GloveMeshData;

static int gm_reserve(GloveMeshData* d, uint32_t vertices, uint32_t indices) {
	d->vertices = (AeronGltfVertex*)calloc(vertices, sizeof *d->vertices);
	d->indices = (uint16_t*)calloc(indices, sizeof *d->indices);
	return d->vertices && d->indices;
}

static void gm_vertex(GloveMeshData* d, float x, float y, float z) {
	AeronGltfVertex* v = &d->vertices[d->vertex_count++];
	v->pos[0] = x;
	v->pos[1] = y;
	v->pos[2] = z;
	v->tangent[3] = 1.0f;
}

static void gm_tri(GloveMeshData* d, uint32_t a, uint32_t b, uint32_t c) {
	d->indices[d->index_count++] = (uint16_t)a;
	d->indices[d->index_count++] = (uint16_t)b;
	d->indices[d->index_count++] = (uint16_t)c;
}

/* Smooth normals from the triangles. */
static void gm_normals(GloveMeshData* d) {
	for (uint32_t i = 0; i + 2 < d->index_count; i += 3) {
		AeronGltfVertex* a = &d->vertices[d->indices[i]];
		AeronGltfVertex* b = &d->vertices[d->indices[i + 1]];
		AeronGltfVertex* c = &d->vertices[d->indices[i + 2]];
		const float e1[3] = { b->pos[0] - a->pos[0], b->pos[1] - a->pos[1], b->pos[2] - a->pos[2] };
		const float e2[3] = { c->pos[0] - a->pos[0], c->pos[1] - a->pos[1], c->pos[2] - a->pos[2] };
		float n[3];
		v3_cross(e1, e2, n);
		for (int k = 0; k < 3; ++k) {
			a->normal[k] += n[k];
			b->normal[k] += n[k];
			c->normal[k] += n[k];
		}
	}
	for (uint32_t i = 0; i < d->vertex_count; ++i) {
		v3_norm(d->vertices[i].normal);
	}
}

/* Unit cylinder: radius 1, from z = 0 to z = -1, open ends. */
static int gm_build_cylinder(GloveMeshData* d) {
	enum { SIDES = 10 };
	if (!gm_reserve(d, SIDES * 2, SIDES * 6)) {
		return 0;
	}
	for (int i = 0; i < SIDES; ++i) {
		const float a = 6.2831853f * (float)i / SIDES;
		gm_vertex(d, cosf(a), sinf(a), 0.0f);
		gm_vertex(d, cosf(a), sinf(a), -1.0f);
	}
	for (int i = 0; i < SIDES; ++i) {
		const uint32_t a = (uint32_t)i * 2, b = (uint32_t)((i + 1) % SIDES) * 2;
		gm_tri(d, a, b, a + 1);
		gm_tri(d, b, b + 1, a + 1);
	}
	/* Cylinder normals are radial; the triangle average gives that. */
	gm_normals(d);
	for (uint32_t i = 0; i < d->vertex_count; ++i) {
		d->vertices[i].normal[2] = 0.0f;
		v3_norm(d->vertices[i].normal);
	}
	return 1;
}

/* Unit sphere (or, with exponent < 1, a rounded box: a superellipsoid). */
static int gm_build_blob(GloveMeshData* d, float exponent) {
	enum { RINGS = 8, SEGS = 12 };
	if (!gm_reserve(d, (RINGS + 1) * (SEGS + 1), RINGS * SEGS * 6)) {
		return 0;
	}
	for (int r = 0; r <= RINGS; ++r) {
		const float v = -1.5707963f + 3.1415926f * (float)r / RINGS;
		for (int s = 0; s <= SEGS; ++s) {
			const float u = 6.2831853f * (float)s / SEGS;
			const float cv = cosf(v), sv = sinf(v), cu = cosf(u), su = sinf(u);
#define SPOW(x) (copysignf(powf(fabsf(x), exponent), (x)))
			gm_vertex(d, SPOW(cv) * SPOW(cu), SPOW(cv) * SPOW(su), SPOW(sv));
#undef SPOW
		}
	}
	for (int r = 0; r < RINGS; ++r) {
		for (int s = 0; s < SEGS; ++s) {
			const uint32_t a = (uint32_t)(r * (SEGS + 1) + s), b = a + 1, c = a + SEGS + 1, e = c + 1;
			gm_tri(d, a, b, e);
			gm_tri(d, a, e, c);
		}
	}
	gm_normals(d);
	return 1;
}

static int gm_build_pin(GloveMeshData* d) {
	if (!gm_reserve(d, XWA_VR_CREST_PIN_VERTEX_COUNT, XWA_VR_CREST_PIN_VERTEX_COUNT)) {
		return 0;
	}
	for (uint32_t i = 0; i < XWA_VR_CREST_PIN_VERTEX_COUNT; ++i) {
		gm_vertex(d, k_vr_crest_pin[i][0], k_vr_crest_pin[i][1], k_vr_crest_pin[i][2]);
		d->vertices[i].normal[0] = k_vr_crest_pin[i][3];
		d->vertices[i].normal[1] = k_vr_crest_pin[i][4];
		d->vertices[i].normal[2] = k_vr_crest_pin[i][5];
		d->indices[d->index_count++] = (uint16_t)i;
	}
	return 1;
}

static void gm_material(AeronGltfMaterial* m, float r, float g, float b, float metallic, float roughness) {
	memset(m, 0, sizeof *m);
	m->base_color_factor[0] = r;
	m->base_color_factor[1] = g;
	m->base_color_factor[2] = b;
	m->base_color_factor[3] = 1.0f;
	m->emissive_strength = 1.0f;
	m->metallic_factor = metallic;
	m->roughness_factor = roughness;
	m->double_sided = 1;
	m->alpha_mode = AERON_GLTF_ALPHA_OPAQUE;
	m->alpha_cutoff = 0.5f;
}

static AeronSceneMesh* gm_upload(AeronCommandBuffer* cmd, GloveMeshData* d, const char* name) {
	AeronFlightModel fm;
	memset(&fm, 0, sizeof fm);
	fm.render.vertices = d->vertices;
	fm.render.vertex_count = d->vertex_count;
	fm.render.indices = d->indices;
	fm.render.index_count = d->index_count;
	fm.render.opaque_index_count = d->index_count;
	fm.render.mask_index_offset = d->index_count;
	fm.render.blend_index_offset = d->index_count;
	fm.render.material_count = 1;
	fm.render.materials = &d->material;
	fm.render.variant_slots = 1;
	fm.render.total_prim_count = 1;
	d->prim_material = 0;
	fm.render.prim_variant_material = &d->prim_material;
	float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
	for (uint32_t i = 0; i < d->vertex_count; ++i) {
		for (int k = 0; k < 3; ++k) {
			mn[k] = fminf(mn[k], d->vertices[i].pos[k]);
			mx[k] = fmaxf(mx[k], d->vertices[i].pos[k]);
		}
	}
	fm.bounds.min.x = mn[0];
	fm.bounds.min.y = mn[1];
	fm.bounds.min.z = mn[2];
	fm.bounds.max.x = mx[0];
	fm.bounds.max.y = mx[1];
	fm.bounds.max.z = mx[2];
	fm.max_extent = fmaxf(fmaxf(mx[0] - mn[0], mx[1] - mn[1]), mx[2] - mn[2]);
	AeronSceneMeshCreateStatus status;
	return AeronScene_MeshCreate(cmd, &fm, name, &status);
}

/* ---- Hand model -------------------------------------------------------- */

/* Right hand in its controller's aim frame (metres; +X right, +Y up, +Z
 * back towards the wrist, the aim ray is -Z). The left hand mirrors X.
 * Holding a Touch controller the hand is in a handshake grip: the palm
 * faces inwards (-X), the knuckle line runs from the index finger at the
 * top to the little finger at the bottom, the thumb rests on top. */
#define GL_FINGERS 4
static const float k_knuckle[GL_FINGERS][3] = {
	{ 0.012f, -0.020f, 0.036f }, { 0.012f, -0.040f, 0.038f }, { 0.011f, -0.058f, 0.042f }, { 0.009f, -0.074f, 0.048f }
};
static const float k_phalanx[GL_FINGERS][3] = {
	{ 0.044f, 0.025f, 0.020f }, { 0.047f, 0.028f, 0.021f }, { 0.044f, 0.026f, 0.020f }, { 0.034f, 0.020f, 0.018f }
};
static const float k_finger_radius[GL_FINGERS] = { 0.0104f, 0.0108f, 0.0102f, 0.0091f };
static const float k_thumb_base[3] = { -0.002f, -0.014f, 0.088f };
static const float k_thumb_len[3] = { 0.036f, 0.031f, 0.026f };
#define GL_THUMB_RADIUS 0.0108f
static const float k_palm_center[3] = { 0.008f, -0.046f, 0.078f };
static const float k_palm_half[3] = { 0.0145f, 0.037f, 0.043f };
static const float k_cuff_center[3] = { 0.005f, -0.048f, 0.138f };
static const float k_cuff_half[3] = { 0.029f, 0.032f, 0.024f };

#define GL_MAX_PIECES 48
#define GL_MAX_CAPSULES 24

typedef struct GlPiece {
	int mesh;
	M3 basis;       /* hand space */
	float pos[3];   /* hand space */
	float scale[3];
} GlPiece;

typedef struct GlHand {
	int shown;
	float curl[GL_FINGERS]; /* 0 straight .. 1 fist */
	float thumb_up;         /* 0 resting on the controller .. 1 thumbs up */
	int pointing;
	float tip_local[3];
	XwaVrGloveCapsule capsules[GL_MAX_CAPSULES];
	int capsule_count;
} GlHand;

static struct {
	int tried, ok;
	GloveMeshData data[GM_COUNT];
	AeronSceneMesh* mesh[GM_COUNT];
	GlHand hands[2];
	GlPiece pieces[GL_MAX_PIECES];
	int piece_count;
	int logged;
} s_gl;

static int gl_enabled(void) {
	const char* env = getenv("XWA_XR_GLOVES");
	return !(env && env[0] == '0');
}

static int gl_ensure(AeronCommandBuffer* cmd) {
	if (s_gl.tried) {
		return s_gl.ok;
	}
	s_gl.tried = 1;
	const float grey = 0.055f; /* dark grey, linear */
	int ok = gm_build_cylinder(&s_gl.data[GM_CYLINDER]) && gm_build_blob(&s_gl.data[GM_SPHERE], 1.0f) &&
			 gm_build_blob(&s_gl.data[GM_ROUNDBOX], 0.55f) && gm_build_blob(&s_gl.data[GM_CUFF], 0.75f) &&
			 gm_build_pin(&s_gl.data[GM_PIN]);
	if (ok) {
		for (int i = 0; i < GM_PIN; ++i) {
			gm_material(&s_gl.data[i].material, grey, grey, grey * 1.08f, 0.0f, 0.78f);
		}
		gm_material(&s_gl.data[GM_PIN].material, 1.0f, 0.77f, 0.34f, 1.0f, 0.32f); /* gold */
		static const char* const names[GM_COUNT] = { "vr.glove.segment", "vr.glove.joint", "vr.glove.palm",
													 "vr.glove.cuff", "vr.glove.crest_pin" };
		for (int i = 0; i < GM_COUNT && ok; ++i) {
			s_gl.mesh[i] = gm_upload(cmd, &s_gl.data[i], names[i]);
			ok = s_gl.mesh[i] != NULL;
		}
	}
	if (!ok) {
		Aeron_LogWarn("xwa.remaster", "VR gloves: mesh creation failed, gloves are off");
	}
	s_gl.ok = ok;
	return ok;
}

static void gl_env3(const char* name, float out[3]) {
	const char* v = getenv(name);
	out[0] = out[1] = out[2] = 0.0f;
	if (v && v[0]) {
		sscanf(v, "%f,%f,%f", &out[0], &out[1], &out[2]);
	}
}

static void gl_add(int mesh, const M3* basis, const float pos[3], float sx, float sy, float sz) {
	if (s_gl.piece_count >= GL_MAX_PIECES) {
		return;
	}
	GlPiece* p = &s_gl.pieces[s_gl.piece_count++];
	p->mesh = mesh;
	p->basis = *basis;
	memcpy(p->pos, pos, sizeof p->pos);
	p->scale[0] = sx;
	p->scale[1] = sy;
	p->scale[2] = sz;
}

static void gl_capsule(GlHand* h, const float a[3], const float b[3], float r) {
	if (h->capsule_count < GL_MAX_CAPSULES) {
		XwaVrGloveCapsule* c = &h->capsules[h->capsule_count++];
		memcpy(c->a, a, sizeof c->a);
		memcpy(c->b, b, sizeof c->b);
		c->radius = r;
	}
}

/* Smoothly approach `goal`. */
static float gl_ease(float v, float goal, float dt) {
	const float a = dt > 0.0f ? 1.0f - expf(-dt / 0.06f) : 1.0f;
	return v + (goal - v) * a;
}

/* Builds the pieces of one hand in hand space (right-hand coordinates).
 * Capsules are recorded in hand space too and moved to LOCAL by the
 * caller. */
static void gl_build_hand(GlHand* h, int right) {
	static const float up[3] = { 0.0f, 1.0f, 0.0f };
	static const float side[3] = { 1.0f, 0.0f, 0.0f };
	M3 ident = m3_identity();
	gl_add(GM_ROUNDBOX, &ident, k_palm_center, k_palm_half[0], k_palm_half[1], k_palm_half[2]);
	gl_add(GM_CUFF, &ident, k_cuff_center, k_cuff_half[0], k_cuff_half[1], k_cuff_half[2]);
	{
		const float top[3] = { k_palm_center[0], k_palm_center[1] + 0.022f, k_palm_center[2] };
		const float bottom[3] = { k_palm_center[0], k_palm_center[1] - 0.022f, k_palm_center[2] };
		gl_capsule(h, top, bottom, 0.036f);
		gl_capsule(h, k_cuff_center, k_cuff_center, 0.034f);
	}
	/* Fingers: hinge about +Y at each joint; curl angles per joint. */
	static const float max_angle[3] = { 1.40f, 1.75f, 1.10f }; /* MCP, PIP, DIP (radians) */
	for (int f = 0; f < GL_FINGERS; ++f) {
		const float r = k_finger_radius[f];
		M3 frame = ident;
		float p[3];
		memcpy(p, k_knuckle[f], sizeof p);
		gl_add(GM_SPHERE, &ident, p, r * 1.05f, r * 1.05f, r * 1.05f);
		for (int j = 0; j < 3; ++j) {
			const M3 bend = m3_rot_y(h->curl[f] * max_angle[j]);
			frame = m3_mul(&frame, &bend);
			const float seg[3] = { 0.0f, 0.0f, -k_phalanx[f][j] };
			float d[3], q[3];
			m3_apply(&frame, seg, d);
			q[0] = p[0] + d[0];
			q[1] = p[1] + d[1];
			q[2] = p[2] + d[2];
			const float rj = r * (1.0f - 0.07f * (float)j);
			gl_add(GM_CYLINDER, &frame, p, rj, rj, k_phalanx[f][j]);
			gl_add(GM_SPHERE, &ident, q, rj * 0.98f, rj * 0.98f, rj * 0.98f);
			gl_capsule(h, p, q, rj);
			memcpy(p, q, sizeof p);
		}
		if (f == 0) {
			/* The pad of the fingertip, a little in from the end. */
			const float in[3] = { 0.0f, 0.0f, r * 0.6f };
			float d[3];
			m3_apply(&frame, in, d);
			h->tip_local[0] = p[0] + d[0];
			h->tip_local[1] = p[1] + d[1];
			h->tip_local[2] = p[2] + d[2];
		}
	}
	/* Thumb: lies on top of the controller pointing forward and inward, or
	 * stands up (thumbs up). */
	{
		const float rest[3] = { -0.30f, -0.10f, -0.95f };
		const float raised[3] = { -0.05f, 1.0f, -0.25f };
		float dir[3];
		for (int k = 0; k < 3; ++k) {
			dir[k] = rest[k] + (raised[k] - rest[k]) * h->thumb_up;
		}
		v3_norm(dir);
		M3 frame = m3_from_dir(dir, side);
		float p[3];
		memcpy(p, k_thumb_base, sizeof p);
		gl_add(GM_SPHERE, &ident, p, GL_THUMB_RADIUS * 1.15f, GL_THUMB_RADIUS * 1.15f, GL_THUMB_RADIUS * 1.15f);
		float axis[3];
		v3_cross(dir, up, axis);
		v3_norm(axis);
		for (int j = 0; j < 3; ++j) {
			if (j > 0) {
				/* Each joint bends a little towards the palm. */
				const float yaxis[3] = { frame.m[1], frame.m[4], frame.m[7] };
				const M3 bend = m3_rot_axis(yaxis, 0.22f * (1.0f - h->thumb_up));
				frame = m3_mul(&bend, &frame);
			}
			const float seg[3] = { 0.0f, 0.0f, -k_thumb_len[j] };
			float d[3], q[3];
			m3_apply(&frame, seg, d);
			q[0] = p[0] + d[0];
			q[1] = p[1] + d[1];
			q[2] = p[2] + d[2];
			const float rj = GL_THUMB_RADIUS * (1.0f - 0.08f * (float)j);
			gl_add(GM_CYLINDER, &frame, p, rj, rj, k_thumb_len[j]);
			gl_add(GM_SPHERE, &ident, q, rj, rj, rj);
			gl_capsule(h, p, q, rj);
			memcpy(p, q, sizeof p);
		}
		(void)axis;
	}
	/* The crest pin: right glove, back of the hand just above the thumb,
	 * facing outwards and up so it is seen when looking at the hand. */
	if (right) {
		float n[3] = { 0.75f, 0.66f, 0.0f };
		v3_norm(n);
		const float fwd[3] = { 0.0f, 0.0f, -1.0f }; /* the emblem's top points along the fingers */
		float x[3];
		v3_cross(fwd, n, x);
		v3_norm(x);
		float y[3];
		v3_cross(n, x, y);
		const M3 basis = { { x[0], y[0], n[0], x[1], y[1], n[1], x[2], y[2], n[2] } };
		/* On the surface of the back of the hand near its top edge. */
		const float pos[3] = { k_palm_center[0] + 0.0135f, k_palm_center[1] + 0.0285f, k_palm_center[2] - 0.004f };
		gl_add(GM_PIN, &basis, pos, 1.0f, 1.0f, 1.0f);
	}
}

static void gl_instance(AeronScene3D* scene, const GlPiece* piece, const M3* hand_rot, const M3* mirror,
						const float hand_pos[3], const float crows[9], const float cam_pos[3], float units) {
	/* hand space -> LOCAL -> ship eye (C = diag(1,-1,-1)) -> world. */
	const M3 local_basis_m = m3_mul(mirror, &piece->basis);
	const M3 local_basis = m3_mul(hand_rot, &local_basis_m);
	float pm[3], pl[3];
	m3_apply(mirror, piece->pos, pm);
	m3_apply(hand_rot, pm, pl);
	pl[0] += hand_pos[0];
	pl[1] += hand_pos[1];
	pl[2] += hand_pos[2];
	M3 rt_c; /* r^T * C */
	for (int i = 0; i < 3; ++i) {
		rt_c.m[i * 3 + 0] = crows[0 * 3 + i];
		rt_c.m[i * 3 + 1] = -crows[1 * 3 + i];
		rt_c.m[i * 3 + 2] = -crows[2 * 3 + i];
	}
	const M3 a = m3_mul(&rt_c, &local_basis);
	float t[3];
	m3_apply(&rt_c, pl, t);
	AeronSceneMeshInstance inst;
	memset(&inst, 0, sizeof inst);
	inst.mesh = s_gl.mesh[piece->mesh];
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			inst.transform[i * 4 + j] = a.m[i * 3 + j] * piece->scale[j] * units;
		}
		inst.transform[i * 4 + 3] = cam_pos[i] + t[i] * units;
	}
	inst.transform[15] = 1.0f;
	memcpy(inst.prev_transform, inst.transform, sizeof inst.transform);
	inst.zero_velocity = 1;
	inst.no_local_lights = 1;
	inst.shadow_flags = AERON_SCENE_INSTANCE_NO_CAST_SHADOW;
	AeronScene_AddMeshInstance(scene, &inst);
}

void XwaVrGloves_Submit(AeronScene3D* scene, AeronCommandBuffer* cmd, const float crows[9], const float cam_pos[3],
						float units) {
	static uint64_t last_ns;
	const uint64_t now_ns = SDL_GetTicksNS();
	const float dt = last_ns ? (float)((double)(now_ns - last_ns) * 1e-9) : 0.0f;
	last_ns = now_ns;
	if (!scene || !cmd || !crows || !cam_pos || units <= 0.0f || !gl_enabled() || !gl_ensure(cmd)) {
		XwaVrGloves_Idle();
		return;
	}
	float offset[3];
	gl_env3("XWA_XR_GLOVE_OFFSET", offset);
	for (int hand = 0; hand < 2; ++hand) {
		GlHand* h = &s_gl.hands[hand];
		AeronXrHand xh;
		h->shown = 0;
		h->capsule_count = 0;
		if (!Aeron_XrGetHand(hand, &xh)) {
			continue;
		}
		/* Pose from the controller: finger on the trigger -> resting curl
		 * that tightens with the pull; lifted -> pointing. */
		float index_goal;
		if (xh.index_touch == 1) {
			index_goal = 0.42f + 0.5f * xh.trigger;
		} else if (xh.index_touch == 0) {
			index_goal = 0.04f;
		} else {
			index_goal = 0.25f + 0.65f * xh.trigger;
		}
		const float grip_goal = 0.22f + 0.72f * xh.grip;
		float thumb_goal = 0.0f;
		if (xh.thumb_touch == 0) {
			thumb_goal = xh.grip > 0.5f ? 1.0f : 0.45f;
		}
		h->curl[0] = gl_ease(h->curl[0], index_goal, dt);
		for (int f = 1; f < GL_FINGERS; ++f) {
			h->curl[f] = gl_ease(h->curl[f], grip_goal, dt);
		}
		h->thumb_up = gl_ease(h->thumb_up, thumb_goal, dt);
		h->pointing = xh.index_touch != 1; /* lifted, or no touch sensing */

		s_gl.piece_count = 0;
		gl_build_hand(h, hand == 1);
		const M3 rot = m3_from_quat(xh.orientation);
		const M3 mirror = { { hand == 1 ? 1.0f : -1.0f, 0, 0, 0, 1, 0, 0, 0, 1 } };
		float hand_pos[3];
		{
			float o[3];
			m3_apply(&rot, offset, o);
			hand_pos[0] = xh.position[0] + o[0];
			hand_pos[1] = xh.position[1] + o[1];
			hand_pos[2] = xh.position[2] + o[2];
		}
		for (int i = 0; i < s_gl.piece_count; ++i) {
			gl_instance(scene, &s_gl.pieces[i], &rot, &mirror, hand_pos, crows, cam_pos, units);
		}
		/* Fingertip and capsules to LOCAL space. */
		float t[3];
		m3_apply(&mirror, h->tip_local, t);
		m3_apply(&rot, t, h->tip_local);
		for (int k = 0; k < 3; ++k) {
			h->tip_local[k] += hand_pos[k];
		}
		for (int c = 0; c < h->capsule_count; ++c) {
			XwaVrGloveCapsule* cap = &h->capsules[c];
			m3_apply(&mirror, cap->a, t);
			m3_apply(&rot, t, cap->a);
			m3_apply(&mirror, cap->b, t);
			m3_apply(&rot, t, cap->b);
			for (int k = 0; k < 3; ++k) {
				cap->a[k] += hand_pos[k];
				cap->b[k] += hand_pos[k];
			}
		}
		h->shown = 1;
	}
	if (!s_gl.logged) {
		Aeron_LogInfo("xwa.remaster", "VR gloves on (touch sensing %s)",
					  s_gl.hands[1].shown || s_gl.hands[0].shown ? "checked per frame" : "n/a");
		s_gl.logged = 1;
	}
}

void XwaVrGloves_Idle(void) {
	s_gl.hands[0].shown = 0;
	s_gl.hands[1].shown = 0;
}

int XwaVrGloves_IndexTip(int hand, float out[3], int* pointing) {
	if (hand < 0 || hand > 1 || !s_gl.hands[hand].shown) {
		return 0;
	}
	memcpy(out, s_gl.hands[hand].tip_local, sizeof s_gl.hands[hand].tip_local);
	if (pointing) {
		*pointing = s_gl.hands[hand].pointing;
	}
	return 1;
}

int XwaVrGloves_Capsules(int hand, XwaVrGloveCapsule* out, int max_out) {
	if (hand < 0 || hand > 1 || !s_gl.hands[hand].shown || !out) {
		return 0;
	}
	const int n = s_gl.hands[hand].capsule_count < max_out ? s_gl.hands[hand].capsule_count : max_out;
	memcpy(out, s_gl.hands[hand].capsules, (size_t)n * sizeof *out);
	return n;
}

void XwaVrGloves_Shutdown(void) {
	for (int i = 0; i < GM_COUNT; ++i) {
		if (s_gl.mesh[i]) {
			AeronScene_MeshDestroy(s_gl.mesh[i]);
		}
		free(s_gl.data[i].vertices);
		free(s_gl.data[i].indices);
	}
	memset(&s_gl, 0, sizeof s_gl);
}

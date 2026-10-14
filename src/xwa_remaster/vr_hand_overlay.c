#include "xwa_remaster/vr_hand_overlay.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "xwa_remaster/vr_env.h"
#include "xwa_remaster/vr_gloves.h"

/* Mask texels (square); a texel covers at least two eye pixels, so the
 * bilinear edge stays a couple of pixels wide. */
enum { HV_MASK = 384, HV_MAX_VERTS = 16384 };

static struct {
	AeronDrawList2D* list[2];
	AeronTexture* tex[2];
	int ready[2];
	uint8_t px[HV_MASK * HV_MASK * 4];
	float sx[HV_MAX_VERTS], sy[HV_MAX_VERTS];
	uint8_t ok[HV_MAX_VERTS], facing[HV_MAX_VERTS];
	int mw, mh;
} s_hv;

static int hv_enabled(void) {
	static int checked, enabled;
	if (!checked) {
		const char* env = getenv("XWA_XR_HANDS_OVER_SCREEN");
		enabled = !(env && env[0] == '0');
		checked = 1;
	}
	return enabled;
}

/* v rotated by the inverse of unit quaternion q (x, y, z, w). */
static void hv_unrotate(const float q[4], const float v[3], float out[3]) {
	const float x = -q[0], y = -q[1], z = -q[2], w = q[3];
	const float tx = 2.0f * (y * v[2] - z * v[1]);
	const float ty = 2.0f * (z * v[0] - x * v[2]);
	const float tz = 2.0f * (x * v[1] - y * v[0]);
	out[0] = v[0] + w * tx + (y * tz - z * ty);
	out[1] = v[1] + w * ty + (z * tx - x * tz);
	out[2] = v[2] + w * tz + (x * ty - y * tx);
}

/* LOCAL point -> eye image pixel. Returns 0 behind (or at) the eye. */
static int hv_project(const AeronXrView* view, const float p[3], int w, int h, float* x, float* y) {
	const float d[3] = { p[0] - view->position[0], p[1] - view->position[1], p[2] - view->position[2] };
	float e[3];
	hv_unrotate(view->orientation, d, e);
	if (-e[2] < 0.01f) {
		return 0;
	}
	const float tl = tanf(view->fov[0]), tr = tanf(view->fov[1]);
	const float tu = tanf(view->fov[2]), td = tanf(view->fov[3]);
	const float ax = e[0] / -e[2], ay = e[1] / -e[2];
	*x = (ax - tl) / (tr - tl) * (float)w;
	*y = (tu - ay) / (tu - td) * (float)h;
	return 1;
}

static void hv_tri(float x0, float y0, float x1, float y1, float x2, float y2) {
	const int mw = s_hv.mw, mh = s_hv.mh;
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
	int ix0 = (int)fmaxf(fminf(x0, fminf(x1, x2)), 0.0f), iy0 = (int)fmaxf(fminf(y0, fminf(y1, y2)), 0.0f);
	int ix1 = (int)fminf(fmaxf(x0, fmaxf(x1, x2)), (float)mw) + 1;
	int iy1 = (int)fminf(fmaxf(y0, fmaxf(y1, y2)), (float)mh) + 1;
	ix1 = ix1 > mw ? mw : ix1;
	iy1 = iy1 > mh ? mh : iy1;
	if (ix0 >= ix1 || iy0 >= iy1) {
		return;
	}
	const float a0 = -(y1 - y0), b0 = x1 - x0, a1 = -(y2 - y1), b1 = x2 - x1, a2 = -(y0 - y2), b2 = x0 - x2;
	const float sx = (float)ix0 + 0.5f, sy = (float)iy0 + 0.5f;
	float r0 = a0 * (sx - x0) + b0 * (sy - y0);
	float r1 = a1 * (sx - x1) + b1 * (sy - y1);
	float r2 = a2 * (sx - x2) + b2 * (sy - y2);
	for (int y = iy0; y < iy1; ++y) {
		float e0 = r0, e1 = r1, e2 = r2;
		uint8_t* row = &s_hv.px[(y * mw) * 4 + 3];
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

/* Bounding box (eye pixels) of both gloves' visible vertices, clipped to
 * the image. Returns 0 when neither glove is in view. */
static int hv_bounds(const AeronXrView* view, int w, int h, float b[4]) {
	b[0] = b[1] = 1e9f;
	b[2] = b[3] = -1e9f;
	for (int hand = 0; hand < 2; ++hand) {
		const float *v, *n;
		const uint16_t* idx;
		int vn, in;
		if (!XwaVrGloves_Mesh(hand, &v, &n, &vn, &idx, &in) || vn > HV_MAX_VERTS) {
			continue;
		}
		for (int i = 0; i < vn; ++i) {
			float x, y;
			if (hv_project(view, &v[i * 3], w, h, &x, &y)) {
				b[0] = fminf(b[0], x);
				b[1] = fminf(b[1], y);
				b[2] = fmaxf(b[2], x);
				b[3] = fmaxf(b[3], y);
			}
		}
	}
	b[0] = fmaxf(b[0], 0.0f);
	b[1] = fmaxf(b[1], 0.0f);
	b[2] = fminf(b[2], (float)w);
	b[3] = fminf(b[3], (float)h);
	return b[2] > b[0] && b[3] > b[1];
}

static int hv_eye(AeronCommandBuffer* cmd, int eye, const AeronXrView* view, int w, int h) {
	float b[4];
	s_hv.ready[eye] = 0;
	if (!s_hv.list[eye]) {
		s_hv.list[eye] = AeronDrawList_Create(8);
		if (!s_hv.list[eye]) {
			return 0;
		}
	}
	AeronDrawList_Begin(s_hv.list[eye], NULL, w, h, AERON_DRAWLIST2D_LOAD, NULL);
	/* Nothing of the eye image shows over the layers ... */
	const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	AeronDrawList_AddFill(s_hv.list[eye], 0.0f, 0.0f, (float)w, (float)h, zero, AERON_BLIT2D_BLEND_ALPHA_SET, NULL);
	int seen = 0;
	if (hv_bounds(view, w, h, b)) {
		/* ... but the gloves. */
		const float bw = b[2] - b[0], bh = b[3] - b[1];
		const float scale = fmaxf(2.0f, fmaxf(bw, bh) / (float)(HV_MASK - 5));
		const int mw = (int)ceilf(bw / scale) + 4, mh = (int)ceilf(bh / scale) + 4;
		const float ox = b[0] - 2.0f * scale, oy = b[1] - 2.0f * scale;
		s_hv.mw = mw;
		s_hv.mh = mh;
		memset(s_hv.px, 0, (size_t)mw * (size_t)mh * 4);
		for (int hand = 0; hand < 2; ++hand) {
			const float *v, *n;
			const uint16_t* idx;
			int vn, in;
			if (!XwaVrGloves_Mesh(hand, &v, &n, &vn, &idx, &in) || vn > HV_MAX_VERTS) {
				continue;
			}
			for (int i = 0; i < vn; ++i) {
				const float* p = &v[i * 3];
				const float to_eye[3] = { view->position[0] - p[0], view->position[1] - p[1],
										  view->position[2] - p[2] };
				s_hv.facing[i] = n[i * 3] * to_eye[0] + n[i * 3 + 1] * to_eye[1] + n[i * 3 + 2] * to_eye[2] > 0.0f;
				s_hv.ok[i] = (uint8_t)hv_project(view, p, w, h, &s_hv.sx[i], &s_hv.sy[i]);
				s_hv.sx[i] = (s_hv.sx[i] - ox) / scale;
				s_hv.sy[i] = (s_hv.sy[i] - oy) / scale;
			}
			for (int t = 0; t + 2 < in; t += 3) {
				const int k0 = idx[t], k1 = idx[t + 1], k2 = idx[t + 2];
				if (k0 >= vn || k1 >= vn || k2 >= vn || !s_hv.ok[k0] || !s_hv.ok[k1] || !s_hv.ok[k2] ||
					!(s_hv.facing[k0] | s_hv.facing[k1] | s_hv.facing[k2])) {
					continue;
				}
				hv_tri(s_hv.sx[k0], s_hv.sy[k0], s_hv.sx[k1], s_hv.sy[k1], s_hv.sx[k2], s_hv.sy[k2]);
				seen = 1;
			}
		}
		if (seen) {
			if (!s_hv.tex[eye]) {
				s_hv.tex[eye] = Aeron_CreateTexture(&(AeronTextureDesc) {
					.width = HV_MASK,
					.height = HV_MASK,
					.mip_count = 1,
					.format = AERON_TEXTURE_FORMAT_RGBA8_UNORM,
					.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
					.debug_name = "xwa.vr_hands.overlay_mask",
				});
			}
			if (!s_hv.tex[eye] || !Aeron_UploadTextureDataCmd(cmd, &(AeronTextureUploadDesc) {
					.texture = s_hv.tex[eye],
					.width = mw,
					.height = mh,
					.pixels = s_hv.px,
					.pitch = mw * 4,
					.pixel_format = AERON_PIXEL_FORMAT_RGBA8888,
					.cycle = 1,
				})) {
				return 0;
			}
			AeronDrawList2DSprite sp;
			memset(&sp, 0, sizeof sp);
			sp.texture = s_hv.tex[eye];
			sp.src_u0 = 1.0f / (float)HV_MASK;
			sp.src_v0 = 1.0f / (float)HV_MASK;
			sp.src_u1 = (float)(mw - 1) / (float)HV_MASK;
			sp.src_v1 = (float)(mh - 1) / (float)HV_MASK;
			sp.dst_x = ox + scale;
			sp.dst_y = oy + scale;
			sp.dst_w = (float)(mw - 2) * scale;
			sp.dst_h = (float)(mh - 2) * scale;
			sp.tint[0] = sp.tint[1] = sp.tint[2] = sp.tint[3] = 1.0f;
			sp.blend = AERON_BLIT2D_BLEND_ALPHA_OVER;
			sp.filter = AERON_BLIT2D_FILTER_LINEAR;
			AeronDrawList_AddSprite(s_hv.list[eye], &sp);
		}
	}
	s_hv.ready[eye] = AeronDrawList_Prepare(s_hv.list[eye], cmd);
	return s_hv.ready[eye] && seen;
}

/* Does a glove cover part of the screen/frame layers in this eye? Only then
 * is the overlay (a whole extra layer for the compositor) worth it. */
static int hv_over_layers(const AeronXrView* view, int w, int h) {
	float gb[4];
	if (!hv_bounds(view, w, h, gb)) {
		return 0;
	}
	float c[3], size[2];
	if (!XwaVrEnv_LayerRect(c, size)) {
		return 1; /* unknown: keep the hands in front */
	}
	float lb[4] = { 1e9f, 1e9f, -1e9f, -1e9f };
	for (int k = 0; k < 4; ++k) {
		const float p[3] = { c[0] + ((k & 1) ? 0.5f : -0.5f) * size[0], c[1] + ((k & 2) ? 0.5f : -0.5f) * size[1],
							 c[2] };
		float x, y;
		if (!hv_project(view, p, w, h, &x, &y)) {
			return 1; /* a corner beside or behind the eye: assume overlap */
		}
		lb[0] = fminf(lb[0], x);
		lb[1] = fminf(lb[1], y);
		lb[2] = fmaxf(lb[2], x);
		lb[3] = fmaxf(lb[3], y);
	}
	const float m = 8.0f; /* pixels */
	return gb[0] - m < lb[2] && gb[2] + m > lb[0] && gb[1] - m < lb[3] && gb[3] + m > lb[1];
}

int XwaVrHandOverlay_Prepare(AeronCommandBuffer* cmd, const AeronXrView views[2], int w, int h) {
	s_hv.ready[0] = s_hv.ready[1] = 0;
	if (!hv_enabled() || !cmd || !views || w <= 0 || h <= 0) {
		return 0;
	}
	int shown = 0;
	for (int hand = 0; hand < 2 && !shown; ++hand) {
		const float *v, *n;
		const uint16_t* idx;
		int vn, in;
		shown = XwaVrGloves_Mesh(hand, &v, &n, &vn, &idx, &in);
	}
	if (!shown || (!hv_over_layers(&views[0], w, h) && !hv_over_layers(&views[1], w, h))) {
		return 0;
	}
	int seen = 0;
	for (int e = 0; e < 2; ++e) {
		seen |= hv_eye(cmd, e, &views[e], w, h);
	}
	if (!seen || !s_hv.ready[0] || !s_hv.ready[1]) {
		s_hv.ready[0] = s_hv.ready[1] = 0;
		return 0;
	}
	return 1;
}

AeronDrawList2D* XwaVrHandOverlay_List(int eye) {
	return eye >= 0 && eye < 2 && s_hv.ready[eye] ? s_hv.list[eye] : NULL;
}

void XwaVrHandOverlay_Shutdown(void) {
	for (int e = 0; e < 2; ++e) {
		if (s_hv.list[e]) {
			AeronDrawList_Destroy(s_hv.list[e]);
		}
		if (s_hv.tex[e]) {
			Aeron_DestroyTexture(s_hv.tex[e]);
		}
	}
	memset(&s_hv, 0, sizeof s_hv);
}

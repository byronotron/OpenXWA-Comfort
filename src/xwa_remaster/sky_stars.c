/* XWA's GPU reproduction of FlightStarfield_Render. */

#include "xwa_remaster/sky_stars.h"

#include "aeron/aeron.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct SkyStarsVsUniform {
	float cube_to_clip[16];
	float view[4];
	float geom[4];
	float tone[4];
} SkyStarsVsUniform;

struct XwaRemasterSkyStars {
	AeronShader* vertex_shader;
	AeronShader* fragment_shader;
	AeronGraphicsPipeline* pipeline;
	AeronSampleCount pipeline_samples;
	SkyStarsVsUniform vertex_uniform;
	float fragment_uniform[4];
	uint32_t vertex_count;
	AeronTexture* dot_texture;
	int dot_failed;
};

static void sky_stars_mat4_mul(float out[16], const float lhs[16], const float rhs[16]) {
	float result[16];
	for (int r = 0; r < 4; ++r) {
		for (int c = 0; c < 4; ++c) {
			result[r * 4 + c] = lhs[r * 4 + 0] * rhs[0 * 4 + c] + lhs[r * 4 + 1] * rhs[1 * 4 + c] +
								lhs[r * 4 + 2] * rhs[2 * 4 + c] + lhs[r * 4 + 3] * rhs[3 * 4 + c];
		}
	}
	memcpy(out, result, sizeof result);
}

static int sky_stars_ensure_pipeline(XwaRemasterSkyStars* stars, AeronSampleCount sample_count) {
	if (stars->pipeline && stars->pipeline_samples == sample_count) {
		return 1;
	}
	if (stars->pipeline) {
		Aeron_DestroyGraphicsPipeline(stars->pipeline);
		stars->pipeline = NULL;
	}
	AeronColorTargetStateDesc color_target = {
		.format = AERON_TEXTURE_FORMAT_RGBA16_FLOAT,
		.blend = {
			.enabled = 1,
			.src_color = AERON_BLEND_ONE,
			.dst_color = AERON_BLEND_ONE,
			.color_op = AERON_BLEND_OP_ADD,
			.src_alpha = AERON_BLEND_ONE,
			.dst_alpha = AERON_BLEND_ONE,
			.alpha_op = AERON_BLEND_OP_ADD,
		},
	};
	stars->pipeline = Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc) {
		.vertex_shader = stars->vertex_shader,
		.fragment_shader = stars->fragment_shader,
		.primitive_type = AERON_PRIMITIVE_TRIANGLES,
		.cull_mode = AERON_CULL_NONE,
		.depth_format = AERON_TEXTURE_FORMAT_D32_FLOAT,
		.depth = { .depth_test = 1, .depth_write = 0, .compare = AERON_COMPARE_GREATER_EQUAL },
		.color_target_count = 1,
		.color_targets = &color_target,
		.sample_count = sample_count,
	});
	stars->pipeline_samples = stars->pipeline ? sample_count : 0;
	return stars->pipeline != NULL;
}

XwaRemasterSkyStars* XwaRemasterSkyStars_Create(void) {
	XwaRemasterSkyStars* stars = (XwaRemasterSkyStars*)calloc(1, sizeof *stars);
	if (!stars) {
		return NULL;
	}
	stars->vertex_shader = Aeron_CreateShader(&(AeronShaderDesc) {
		.name = "sky_stars.vert",
		.stage = AERON_SHADER_STAGE_VERTEX,
		.uniform_buffer_count = 1,
	});
	stars->fragment_shader = Aeron_CreateShader(&(AeronShaderDesc) {
		.name = "sky_stars.frag",
		.stage = AERON_SHADER_STAGE_FRAGMENT,
		.uniform_buffer_count = 1,
	});
	if (!stars->vertex_shader || !stars->fragment_shader) {
		Aeron_LogError("xwa.remaster", "starfield: GPU resource creation failed");
		XwaRemasterSkyStars_Destroy(stars);
		return NULL;
	}
	return stars;
}

void XwaRemasterSkyStars_Destroy(XwaRemasterSkyStars* stars) {
	if (!stars) {
		return;
	}
	if (stars->pipeline) {
		Aeron_DestroyGraphicsPipeline(stars->pipeline);
	}
	if (stars->vertex_shader) {
		Aeron_DestroyShader(stars->vertex_shader);
	}
	if (stars->fragment_shader) {
		Aeron_DestroyShader(stars->fragment_shader);
	}
	if (stars->dot_texture) {
		Aeron_DestroyTexture(stars->dot_texture);
	}
	free(stars);
}

int XwaRemasterSkyStars_Prepare(XwaRemasterSkyStars* stars, const AeronScene3D* scene,
								const float world_to_cube[9], const XwaRemasterSkyStarsParams* params) {
	if (!stars || !scene || !params || params->grid_n < 1.0f) {
		return 0;
	}
	const float* view_proj = AeronScene_JitteredViewProj(scene);
	if (!view_proj) {
		return 0;
	}

	static const float identity3[9] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f };
	const float* basis = world_to_cube ? world_to_cube : identity3;
	float cube_to_world[16] = {
		basis[0], basis[3], basis[6], 0.0f, basis[1], basis[4], basis[7], 0.0f,
		basis[2], basis[5], basis[8], 0.0f, 0.0f,     0.0f,     0.0f,     1.0f,
	};
	sky_stars_mat4_mul(stars->vertex_uniform.cube_to_clip, view_proj, cube_to_world);

	int render_w, render_h, output_w, output_h;
	AeronScene_RenderDims(scene, &render_w, &render_h);
	AeronScene_RtDims(scene, &output_w, &output_h);
	if (render_w <= 0 || render_h <= 0 || output_w <= 0 || output_h <= 0) {
		return 0;
	}
	const float pixel_scale = (float)render_w / (float)output_w;
	const float exposure = params->exposure > 0.0f ? params->exposure : 1.0f;
	stars->vertex_uniform.view[0] = (float)render_w;
	stars->vertex_uniform.view[1] = (float)render_h;
	stars->vertex_uniform.view[2] = params->grid_n;
	stars->vertex_uniform.view[3] = params->density;
	stars->vertex_uniform.geom[0] = params->core_radius_px * pixel_scale;
	stars->vertex_uniform.geom[1] = params->pixel_pitch_px * pixel_scale;
	stars->vertex_uniform.geom[2] = (float)params->game_time_ms;
	stars->vertex_uniform.geom[3] = params->flare_strength;
	stars->vertex_uniform.tone[0] = exposure * params->brightness;
	stars->vertex_uniform.tone[1] = 1.0f;

	stars->fragment_uniform[0] = params->core_radius_px * pixel_scale;
	stars->fragment_uniform[1] = params->pixel_pitch_px * pixel_scale;
	stars->fragment_uniform[2] = params->feather_px * pixel_scale;
	stars->fragment_uniform[3] = 0.0f;

	const uint32_t grid_n = (uint32_t)params->grid_n;
	stars->vertex_count = 36u * grid_n * grid_n;
	return 1;
}

void XwaRemasterSkyStars_Draw(AeronCommandBuffer* command_buffer, AeronRenderPass* render_pass, int rt_w,
							  int rt_h, void* user) {
	(void)rt_w;
	(void)rt_h;
	XwaRemasterSkyStars* stars = (XwaRemasterSkyStars*)user;
	if (!stars || !render_pass || stars->vertex_count == 0) {
		return;
	}
	if (!sky_stars_ensure_pipeline(stars, Aeron_RenderPassGetSampleCount(render_pass))) {
		Aeron_CommandBufferSetFailure(command_buffer, "Procedural starfield pipeline preparation failed");
		return;
	}
	Aeron_BindGraphicsPipeline(render_pass, stars->pipeline);
	Aeron_BindUniformData(render_pass, AERON_SHADER_STAGE_VERTEX, 0, &stars->vertex_uniform,
						  sizeof stars->vertex_uniform);
	Aeron_BindUniformData(render_pass, AERON_SHADER_STAGE_FRAGMENT, 0, stars->fragment_uniform,
						  sizeof stars->fragment_uniform);
	Aeron_Draw(render_pass, stars->vertex_count, 0);
}

/* ---- CPU evaluation (mirrors shaders/sky_stars.vert.hlsl exactly) ---- */

static const int k_star_cycle_off[4] = { 0, 39, 103, 148 };
static const int k_star_cycle_len[4] = { 39, 64, 45, 57 };
static const int k_star_pal[205] = {
	0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 15, 14,
	13, 12, 11, 10, 9,  8,  7,  6,  5,  4,  3,  2,  1,  0,  0,  0,  1,  1,  1,  2,  2,  3,  3,  4,  4,  5,
	6,  7,  8,  10, 12, 14, 16, 17, 18, 19, 20, 20, 21, 21, 22, 22, 22, 23, 23, 23, 23, 22, 22, 22, 21, 21,
	20, 20, 19, 18, 17, 16, 14, 12, 10, 8,  7,  6,  5,  4,  4,  3,  3,  2,  2,  1,  1,  1,  0,  0,  0,  0,
	2,  4,  6,  8,  10, 12, 14, 16, 18, 20, 22, 23, 23, 22, 20, 18, 16, 14, 12, 10, 8,  6,  4,  2,  0,  0,
	0,  0,  0,  0,  0,  1,  2,  3,  2,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  2,  3,  4,  5,  6,  7,
	8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 23, 23, 23, 23, 20, 22, 18, 21, 18, 20,
	19, 16, 18, 16, 17, 16, 15, 13, 14, 12, 12, 11, 10, 8,  5,  2,  0,  0,  0,  0,  0,  0,  0,
};

static uint32_t sky_star_hash(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

static uint32_t sky_cell_seed(int face, int cx, int cy) {
	const uint32_t k = (uint32_t)(cx + 4096) * 73856093u ^ (uint32_t)(cy + 4096) * 19349663u ^
					   ((uint32_t)face + 1u) * 83492791u;
	return sky_star_hash(k);
}

static float sky_hash01(uint32_t h) { return (float)(h >> 8) * (1.0f / 16777216.0f); }

static void sky_dir_from_face(int face, float u, float v, float out[3]) {
	switch (face) {
		case 0: out[0] = 1.0f; out[1] = -v; out[2] = -u; break;
		case 1: out[0] = -1.0f; out[1] = -v; out[2] = u; break;
		case 2: out[0] = u; out[1] = 1.0f; out[2] = v; break;
		case 3: out[0] = u; out[1] = -1.0f; out[2] = -v; break;
		case 4: out[0] = u; out[1] = -v; out[2] = 1.0f; break;
		default: out[0] = -u; out[1] = -v; out[2] = -1.0f; break;
	}
}

static float sky_ramp_channel(float value) { return powf(value > 0.0f ? value : 0.0f, 2.2f); }

/* Static part of the field (positions, cycle, attributes), cached. */
typedef struct SkyStarCell {
	float dir[3];
	uint8_t ci;
	uint32_t attrib;
} SkyStarCell;

static SkyStarCell* s_cells;
static int s_cell_count;
static int s_cell_grid;
static float s_cell_density = -1.0f;

static int sky_build_cells(int grid, float density) {
	if (s_cells && s_cell_grid == grid && s_cell_density == density) {
		return 1;
	}
	free(s_cells);
	s_cells = NULL;
	s_cell_count = 0;
	if (grid < 1 || grid > 256) {
		return 0;
	}
	s_cells = (SkyStarCell*)malloc(sizeof *s_cells * (size_t)(6 * grid * grid));
	if (!s_cells) {
		return 0;
	}
	for (int face = 0; face < 6; ++face) {
		for (int cy = 0; cy < grid; ++cy) {
			for (int cx = 0; cx < grid; ++cx) {
				uint32_t h = sky_cell_seed(face, cx, cy);
				if (sky_hash01(h) >= density) {
					continue;
				}
				h = sky_star_hash(h);
				const float sx = sky_hash01(h);
				h = sky_star_hash(h);
				const float sy = sky_hash01(h);
				h = sky_star_hash(h);
				const uint8_t ci = (uint8_t)(h & 3u);
				h = sky_star_hash(h);
				SkyStarCell* c = &s_cells[s_cell_count++];
				const float u = (((float)cx + sx) / (float)grid) * 2.0f - 1.0f;
				const float v = (((float)cy + sy) / (float)grid) * 2.0f - 1.0f;
				sky_dir_from_face(face, u, v, c->dir);
				const float len = sqrtf(c->dir[0] * c->dir[0] + c->dir[1] * c->dir[1] + c->dir[2] * c->dir[2]);
				c->dir[0] /= len;
				c->dir[1] /= len;
				c->dir[2] /= len;
				c->ci = ci;
				c->attrib = h;
			}
		}
	}
	s_cell_grid = grid;
	s_cell_density = density;
	return 1;
}

int XwaRemasterSkyStars_Collect(const XwaRemasterSkyStarsParams* params, const float cone_dir_cube[3], float cos_min,
								XwaRemasterSkyStarPoint* out, int max_out) {
	int count = 0;
	if (!params || !cone_dir_cube || !out || max_out <= 0 ||
		!sky_build_cells((int)params->grid_n, params->density)) {
		return 0;
	}
	const float tone = (params->exposure > 0.0f ? params->exposure : 1.0f) * params->brightness;
	const int step = (int)((float)params->game_time_ms / 60.0f);
	for (int i = 0; i < s_cell_count && count < max_out; ++i) {
		const SkyStarCell* c = &s_cells[i];
		if (c->dir[0] * cone_dir_cube[0] + c->dir[1] * cone_dir_cube[1] + c->dir[2] * cone_dir_cube[2] < cos_min) {
			continue;
		}
		const int cnt = k_star_cycle_len[c->ci];
		const int phase = (int)(c->attrib % (uint32_t)cnt);
		const int jitter = (int)((c->attrib >> 13) & 7u);
		const int idx = (step + phase) % cnt;
		const int n = k_star_pal[k_star_cycle_off[c->ci] + (idx < 0 ? idx + cnt : idx)];
		if (n <= 0) {
			continue; /* winked out this frame */
		}
		XwaRemasterSkyStarPoint* p = &out[count++];
		memcpy(p->dir_cube, c->dir, sizeof p->dir_cube);
		p->color[0] = sky_ramp_channel((float)n / 31.0f) * tone;
		p->color[1] = sky_ramp_channel((2.0f * (float)n) / 63.0f) * tone;
		p->color[2] = sky_ramp_channel((float)(n + jitter) / 31.0f) * tone;
		p->bright = params->flare_strength > 0.0f && n > 16;
	}
	return count;
}

AeronTexture* XwaRemasterSkyStars_DotTexture(XwaRemasterSkyStars* stars) {
	enum { DOT = 16 };
	uint8_t pixels[DOT * DOT * 4];
	if (!stars || stars->dot_failed) {
		return NULL;
	}
	if (stars->dot_texture) {
		return stars->dot_texture;
	}
	for (int y = 0; y < DOT; ++y) {
		for (int x = 0; x < DOT; ++x) {
			const float dx = ((float)x + 0.5f) / (DOT * 0.5f) - 1.0f;
			const float dy = ((float)y + 0.5f) / (DOT * 0.5f) - 1.0f;
			const float r = sqrtf(dx * dx + dy * dy);
			/* Solid core to r = 0.5, soft edge to r = 1 (premultiplied). */
			float a = r <= 0.5f ? 1.0f : (r >= 1.0f ? 0.0f : 1.0f - (r - 0.5f) / 0.5f);
			a = a * a * (3.0f - 2.0f * a);
			const uint8_t b = (uint8_t)(a * 255.0f + 0.5f);
			uint8_t* px = &pixels[(y * DOT + x) * 4];
			px[0] = px[1] = px[2] = px[3] = b;
		}
	}
	stars->dot_texture = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width = DOT,
		.height = DOT,
		.mip_count = 1,
		.format = AERON_TEXTURE_FORMAT_RGBA8_UNORM,
		.usage = AERON_TEXTURE_USAGE_SAMPLED,
		.debug_name = "xwa.sky_star_dot",
	});
	if (!stars->dot_texture || !Aeron_UploadTextureData(&(AeronTextureUploadDesc) {
			.texture = stars->dot_texture,
			.width = DOT,
			.height = DOT,
			.raw_data = pixels,
			.raw_size = sizeof pixels,
		})) {
		if (stars->dot_texture) {
			Aeron_DestroyTexture(stars->dot_texture);
			stars->dot_texture = NULL;
		}
		stars->dot_failed = 1;
		Aeron_LogWarn("xwa.remaster", "starfield: dot texture creation failed");
		return NULL;
	}
	return stars->dot_texture;
}

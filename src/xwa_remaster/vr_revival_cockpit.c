/* Optional X-Wing Revival 3D cockpits (see vr_revival_cockpit.h).
 * Derived from SgtBilko76's GPL-3 vr_revival_cockpit.c (github.com/SgtBilko76/OpenXWA-VR), with our modifications.
 * The GLB is parsed with cgltf_load_buffers + Aeron_FlightModelBuildData
 * because Aeron_FlightModelBuildMemory does not attach a GLB's BIN chunk. */
#include "xwa_remaster/vr_revival_cockpit.h"

#include "aeron/aeron.h"
#include "aeron/asset/flight_model.h"
#include "aeron/log.h"
#include "aeron/paths.h"
#include "xwa/assets/object_type.h"

#include "cgltf.h"

#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct RevivalCockpit {
	const char* file; /* in the revival_cockpits folder */
	AeronFlightModel model;
	AeronSceneMesh* mesh;
	char dir[512]; /* folder it was loaded from (or failed in) */
	int failed;
} RevivalCockpit;

enum {
	RC_XWING,
	RC_YWING,
	RC_AWING,
	RC_BWING,
	RC_TIEFIGHTER,
	RC_TIEINTERCEPTOR,
	RC_TIEBOMBER,
	RC_FALCON,
	RC_COUNT
};

static RevivalCockpit s_rc[RC_COUNT] = {
	[RC_XWING] = { "xwing.glb" },
	[RC_YWING] = { "ywing.glb" },
	[RC_AWING] = { "awing.glb" },
	[RC_BWING] = { "bwing.glb" },
	[RC_TIEFIGHTER] = { "tiefighter.glb" },
	[RC_TIEINTERCEPTOR] = { "tieinterceptor.glb" },
	[RC_TIEBOMBER] = { "tiebomber.glb" },
	[RC_FALCON] = { "falcon.glb" },
};

static char s_hud_name[96];
static int s_drawn = -1; /* this frame's cockpit, -1 none */

/* Letters and digits of the file name (no folder, no extension), upper case. */
static void rc_key(const char* name, char* out, size_t size) {
	size_t n = 0;
	const char* base = name;
	for (const char* p = name; *p; ++p) {
		if (*p == '/' || *p == '\\') {
			base = p + 1;
		}
	}
	for (const char* p = base; *p && *p != '.' && n + 1 < size; ++p) {
		if (isalnum((unsigned char)*p)) {
			out[n++] = (char)toupper((unsigned char)*p);
		}
	}
	out[n] = '\0';
}

/* XWA's cockpit OPT -> Revival cockpit. XWA flies every TIE but the
 * bomber in TieCockpit: only the TIE Fighter and Interceptor have Revival
 * cockpits of their own. XWA's YT-1300 and the Falcon share Revival's
 * YT-1300 cockpit. The Assault Gunboat has no cockpit of its own in XWA
 * (CombatSimCockpit) and keeps it. */
static int rc_pick(const char* cockpit_model_name, uint16_t object_type) {
	char key[64];
	rc_key(cockpit_model_name ? cockpit_model_name : "", key, sizeof key);
	if (strcmp(key, "XWINGCOCKPIT") == 0) {
		return RC_XWING;
	}
	if (strcmp(key, "YWINGCOCKPIT") == 0) {
		return RC_YWING;
	}
	if (strcmp(key, "AWINGCOCKPIT") == 0) {
		return RC_AWING;
	}
	if (strcmp(key, "BWINGCOCKPIT") == 0) {
		return RC_BWING;
	}
	if (strcmp(key, "TIECOCKPIT") == 0) {
		if (object_type == OBJ_TIEFighter) {
			return RC_TIEFIGHTER;
		}
		return object_type == OBJ_TIEInterceptor ? RC_TIEINTERCEPTOR : -1;
	}
	if (strcmp(key, "TIEBOMBERCOCKPIT") == 0) {
		return RC_TIEBOMBER;
	}
	if (strcmp(key, "MILLENIUMFALCON2COCKPIT") == 0 || strcmp(key, "CORELLIANTRANSPORT2COCKPIT") == 0) {
		return RC_FALCON;
	}
	return -1;
}

/* The VR option: XWA's cockpits unless XWA_XR_COCKPIT_MODEL=revival. */
static int rc_enabled(void) {
	const char* model = SDL_getenv("XWA_XR_COCKPIT_MODEL");
	return model && SDL_strcasecmp(model, "revival") == 0;
}

/* XWA_XR_REVIVAL_COCKPITS, else the APK assets folder revival_cockpits/
 * (Android reads relative paths from the APK), else revival_cockpits/ next
 * to the game. */
static const char* rc_dir(void) {
	const char* dir = SDL_getenv("XWA_XR_REVIVAL_COCKPITS");
	if (dir && dir[0]) {
		return dir;
	}
#if defined(SDL_PLATFORM_ANDROID)
	return "revival_cockpits";
#else
	static char s_app_dir[1024];
	if (!s_app_dir[0] && !Aeron_ApplicationPath("revival_cockpits", s_app_dir, sizeof s_app_dir)) {
		snprintf(s_app_dir, sizeof s_app_dir, "revival_cockpits");
	}
	return s_app_dir;
#endif
}

static void rc_release(RevivalCockpit* c) {
	if (c->mesh) {
		AeronScene_MeshDestroy(c->mesh);
		c->mesh = NULL;
	}
	Aeron_FlightModelFree(&c->model);
	memset(&c->model, 0, sizeof c->model);
	c->failed = 0;
	c->dir[0] = '\0';
}

static int rc_load(AeronCommandBuffer* cmd, RevivalCockpit* c, const char* dir) {
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", dir, c->file);
	snprintf(c->dir, sizeof c->dir, "%s", dir);
	c->failed = 1;
	size_t size = 0;
	void* bytes = SDL_LoadFile(path, &size);
	if (!bytes) {
		Aeron_LogWarn("xwa.revival", "%s not found (%s); keeping the XWA cockpit", path, SDL_GetError());
		return 0;
	}
	cgltf_options options;
	memset(&options, 0, sizeof options);
	cgltf_data* data = NULL;
	int built = 0;
	if (cgltf_parse(&options, bytes, size, &data) == cgltf_result_success &&
		cgltf_load_buffers(&options, data, NULL) == cgltf_result_success) {
		built = Aeron_FlightModelBuildData(data, path, &c->model);
	}
	if (data) {
		cgltf_free(data);
	}
	SDL_free(bytes);
	if (!built) {
		Aeron_LogWarn("xwa.revival", "%s: not a usable cockpit glTF", path);
		Aeron_FlightModelFree(&c->model);
		memset(&c->model, 0, sizeof c->model);
		return 0;
	}
	AeronSceneMeshCreateStatus status = AERON_SCENE_MESH_CREATE_SUCCESS;
	char label[64];
	snprintf(label, sizeof label, "revival.%s", c->file);
	c->mesh = AeronScene_MeshCreate(cmd, &c->model, label, &status);
	Aeron_FlightModelFree(&c->model); /* the mesh keeps what it needs */
	if (!c->mesh) {
		Aeron_LogWarn("xwa.revival", "%s: scene mesh creation failed (%d)", path, (int)status);
		return 0;
	}
	c->failed = 0;
	Aeron_LogInfo("xwa.revival",
				  "X-Wing Revival cockpit %s: %u vertices, %u triangles, %u primitives, "
				  "bounds (%.2f %.2f %.2f)..(%.2f %.2f %.2f) m",
				  path, c->mesh->vertex_count, c->mesh->index_count / 3u, c->mesh->total_prim_count,
				  c->mesh->bound_min[0], c->mesh->bound_min[1], c->mesh->bound_min[2], c->mesh->bound_max[0],
				  c->mesh->bound_max[1], c->mesh->bound_max[2]);
	return 1;
}

AeronSceneMesh* XwaVrRevivalCockpit_Mesh(AeronCommandBuffer* cmd, const char* cockpit_model_name,
										 uint16_t object_type) {
	const int which = rc_enabled() ? rc_pick(cockpit_model_name, object_type) : -1;
	if (which < 0) {
		return NULL;
	}
	const char* dir = rc_dir();
	RevivalCockpit* c = &s_rc[which];
	if ((c->mesh || c->failed) && strcmp(c->dir, dir) != 0) {
		rc_release(c); /* another folder: load again */
	}
	if (!c->mesh && !c->failed && cmd) {
		/* One Revival cockpit in memory at a time (2-5 MB of textures each). */
		for (int i = 0; i < RC_COUNT; ++i) {
			if (i != which && s_rc[i].mesh) {
				rc_release(&s_rc[i]);
			}
		}
		rc_load(cmd, c, dir);
	}
	if (!c->mesh) {
		return NULL;
	}
	s_drawn = which;
	char key[64];
	rc_key(c->file, key, sizeof key);
	snprintf(s_hud_name, sizeof s_hud_name, "Revival%s", key);
	return c->mesh;
}

void XwaVrRevivalCockpit_Matrix(const float cockpit_matrix[16], const float camera_local[3], float out[16]) {
	float k = 1.0f;
	const char* scale = SDL_getenv("XWA_XR_REVIVAL_SCALE");
	if (scale && scale[0]) {
		k = (float)SDL_atof(scale);
		if (!(k > 0.2f && k < 5.0f)) {
			k = 1.0f;
		}
	}
	/* The GLB's axes after Aeron's glTF import are the pilot's eye axes
	 * (x right, y down, z forward); the cockpit OPTs' are x right, -y forward,
	 * z up. Model x -> OPT x, model y (down) -> -OPT z, model z (forward) ->
	 * -OPT y. */
	for (int r = 0; r < 3; ++r) {
		out[r * 4 + 0] = cockpit_matrix[r * 4 + 0] * k;
		out[r * 4 + 1] = -cockpit_matrix[r * 4 + 2] * k;
		out[r * 4 + 2] = -cockpit_matrix[r * 4 + 1] * k;
		out[r * 4 + 3] = camera_local[r];
	}
	out[12] = out[13] = out[14] = 0.0f;
	out[15] = 1.0f;
}

const char* XwaVrRevivalCockpit_HudName(const char* fallback) {
	return s_drawn >= 0 ? s_hud_name : fallback;
}

void XwaVrRevivalCockpit_BeginFrame(void) {
	s_drawn = -1;
	if (!rc_enabled()) {
		/* Back to XWA's cockpits: let go of the Revival one (and try a
		 * missing file again when switched back on). */
		for (int i = 0; i < RC_COUNT; ++i) {
			if (s_rc[i].mesh || s_rc[i].failed) {
				rc_release(&s_rc[i]);
			}
		}
	}
}

void XwaVrRevivalCockpit_Shutdown(void) {
	for (int i = 0; i < RC_COUNT; ++i) {
		rc_release(&s_rc[i]);
	}
	s_drawn = -1;
}

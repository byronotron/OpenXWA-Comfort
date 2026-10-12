#include "xwa_remaster/vr_env.h"

#include "aeron/asset/opt_model.h"
#include "aeron/log.h"
#include "aeron/scene/mesh.h"
#include "xwa_remaster/ship.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define ENV_MAX_MESHES 24
/* InsideHangar hardpoint: the hangar floor (classic launch base). */
#define ENV_HARDPOINT_INSIDE_HANGAR 25
/* Floor below the launch reference when the hardpoint is missing (the
 * explicit heights of floor-standing props put it near here). */
#define ENV_FLOOR_FALLBACK (-870.0f)

typedef struct EnvMesh {
	char opt[32];
	AeronSceneMesh* mesh;
	float size_z; /* world units */
	int tried;
} EnvMesh;

static struct {
	EnvMesh meshes[ENV_MAX_MESHES];
	int mesh_count;
	int hangar; /* index of Hangar.opt in meshes */
	float floor_z;
	int laid_out;
	int logged;
	/* Instances, built once every mesh was tried. */
	int count;
	int mesh_of[ENV_PROP_COUNT + 2];
	float transform[ENV_PROP_COUNT + 2][16];
} s_env;

static float env_float(const char* name, float fallback) {
	const char* v = getenv(name);
	return v && v[0] ? (float)atof(v) : fallback;
}

int XwaVrEnv_Enabled(void) {
	const char* v = getenv("XWA_XR_ENVIRONMENT");
	return !(v && v[0] == '0');
}

static const char* env_craft(void) {
	const char* v = getenv("XWA_XR_ENV_CRAFT");
	if (v) {
		return v[0] && strcmp(v, "0") != 0 ? v : NULL;
	}
	return "Xwing";
}

/* The list of unique OPTs: the hangar first, then the parked craft, then
 * the props. */
static void env_list(void) {
	if (s_env.mesh_count) {
		return;
	}
	const char* names[ENV_PROP_COUNT + 2];
	int n = 0;
	names[n++] = "Hangar";
	if (env_craft()) {
		names[n++] = env_craft();
	}
	for (int i = 0; i < ENV_PROP_COUNT; ++i) {
		names[n++] = k_props[i].opt;
	}
	for (int i = 0; i < n; ++i) {
		int found = 0;
		for (int j = 0; j < s_env.mesh_count; ++j) {
			if (strcmp(s_env.meshes[j].opt, names[i]) == 0) {
				found = 1;
				break;
			}
		}
		if (!found && s_env.mesh_count < ENV_MAX_MESHES) {
			snprintf(s_env.meshes[s_env.mesh_count].opt, sizeof s_env.meshes[0].opt, "%s", names[i]);
			s_env.mesh_count++;
		}
	}
	s_env.hangar = 0;
	s_env.floor_z = ENV_FLOOR_FALLBACK;
}

static int env_find(const char* opt) {
	for (int j = 0; j < s_env.mesh_count; ++j) {
		if (strcmp(s_env.meshes[j].opt, opt) == 0) {
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
	if (!XwaRemasterShip_BuildOptModel(m->opt, &model)) {
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

static void env_add(int mesh, int pitch, int yaw, float x, float y, float z) {
	const float pos[3] = { x, y, z };
	if (mesh < 0 || s_env.count >= ENV_PROP_COUNT + 2) {
		return;
	}
	s_env.mesh_of[s_env.count] = mesh;
	env_object_matrix(pitch, yaw, pos, s_env.transform[s_env.count]);
	s_env.count++;
}

static void env_layout(void) {
	s_env.count = 0;
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

int XwaVrEnv_Prepare(AeronCommandBuffer* cmd) {
	if (!cmd) {
		return 0;
	}
	env_list();
	/* One OPT per frame. */
	for (int i = 0; i < s_env.mesh_count; ++i) {
		if (!s_env.meshes[i].tried) {
			env_load(cmd, i);
			s_env.laid_out = 0;
			break;
		}
	}
	if (!s_env.meshes[s_env.hangar].mesh) {
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
	 * way the parked craft points; positive turns right). */
	float x = -500.0f, y = -1150.0f, yaw = 0.0f, eye = 1.7f;
	const char* v = getenv("XWA_XR_ENV_VIEW");
	if (v && v[0]) {
		sscanf(v, "%f,%f,%f,%f", &x, &y, &yaw, &eye);
	}
	const float r = yaw * 3.14159265358979f / 180.0f;
	const float f[3] = { sinf(r), cosf(r), 0.0f };
	pos[0] = x;
	pos[1] = y;
	pos[2] = s_env.floor_z + eye * AERON_OPT_UNITS_PER_METER;
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
		if (i == 0) {
			/* The enclosed hangar must not shadow its own ceiling light. */
			inst.shadow_flags |= AERON_SCENE_INSTANCE_NO_CAST_SHADOW;
		}
		AeronScene_AddMeshInstance(scene, &inst);
	}
}

void XwaVrEnv_Release(void) {
	for (int i = 0; i < s_env.mesh_count; ++i) {
		if (s_env.meshes[i].mesh) {
			AeronScene_MeshDestroy(s_env.meshes[i].mesh);
		}
	}
	memset(&s_env, 0, sizeof s_env);
}

/* Included after exact production camera functions by test_camera.py. */
#include <assert.h>
#include <stdio.h>

static void close_float(float actual, float expected) {
	if (fabsf(actual - expected) > 0.0002f) {
		fprintf(stderr, "actual %.9g expected %.9g\n", actual, expected);
		abort();
	}
}

static XwaFlightCamera camera(void) {
	XwaFlightCamera cam = {0};
	cam.focus_obj_idx = cam.player_obj_idx = 7;
	cam.impact_pan_world[0] = -2.0f;
	cam.impact_pan_world[1] = 1.0f;
	cam.impact_pan_world[2] = -1.0f;
	return cam;
}

static void test_policy(void) {
	close_float(XwaVrComfort_ParseShake(NULL), 1.0f);
	close_float(XwaVrComfort_ParseShake(""), 1.0f);
	close_float(XwaVrComfort_ParseShake("garbage"), 1.0f);
	close_float(XwaVrComfort_ParseShake("nan"), 1.0f);
	close_float(XwaVrComfort_ParseShake("inf"), 1.0f);
	close_float(XwaVrComfort_ParseShake("0.25junk"), 1.0f);
	close_float(XwaVrComfort_ParseShake("0.25"), 0.25f);
	close_float(XwaVrComfort_ParseShake("-2"), 0.0f);
	close_float(XwaVrComfort_ParseShake("2"), 1.0f);
	XwaFlightCamera cam = camera();
	close_float(XwaVrComfort_ShakeScale(&cam, 0, NULL, "0"), 1.0f);
	close_float(XwaVrComfort_ShakeScale(&cam, 1, "0", "0"), 1.0f);
	close_float(XwaVrComfort_ShakeScale(&cam, 1, NULL, "0"), 0.0f);
	uint8_t* flags[] = { &cam.external, &cam.map_mode, &cam.film_overlay, &cam.in_hangar,
		&cam.hyperspace_phase };
	for (int i = 0; i < 5; i++) {
		*flags[i] = 1;
		close_float(XwaVrComfort_ShakeScale(&cam, 1, NULL, "0"), 1.0f);
		*flags[i] = 0;
	}
	cam.focus_obj_idx = 8;
	close_float(XwaVrComfort_ShakeScale(&cam, 1, NULL, "0"), 1.0f);
	cam = camera();
	const XwaFlightCamera saved = cam;
	for (int i = 0; i < 3; i++) {
		const float scale = (float[]){0.0f, 0.25f, 1.0f}[i];
		float pos[3] = {20, 30, 40};
		XwaVrComfort_ApplyCameraPan(&cam, scale, pos);
		for (int a = 0; a < 3; a++) {
			close_float(pos[a], (float[]){20,30,40}[a] - (1.0f - scale) * cam.impact_pan_world[a]);
		}
	}
	assert(memcmp(&cam, &saved, sizeof cam) == 0);
}

static void test_eye_tracking(void) {
	for (int ship_axis = 0; ship_axis < 3; ship_axis++) {
		for (int step = -4; step <= 4; step++) {
			AeronSceneCamera base = {0}, out;
			const float angle = step * 0.3926990817f;
			base.ori[0] = cosf(angle);
			base.ori[ship_axis + 1] = sinf(angle);
			base.pos[0] = 125; base.pos[1] = -42; base.pos[2] = 850;
			for (int head_axis = 0; head_axis < 3; head_axis++) {
				for (int pose = -3; pose <= 3; pose++) {
					AeronXrView v = {0};
					const float head = pose * 0.2617993878f;
					v.orientation[3] = cosf(head);
					v.orientation[head_axis] = sinf(head);
					v.fov[0] = -0.7f; v.fov[1] = 0.8f; v.fov[2] = 0.75f; v.fov[3] = -0.65f;
					const AeronSceneCamera saved_base = base;
					const AeronXrView saved_eye = v;
					fl_xr_eye_camera(&base, &v, &out);
					/* Head rotation alone never orbits around a ship/world origin. */
					for (int a = 0; a < 3; a++) close_float(out.pos[a], base.pos[a]);
					assert(memcmp(&base, &saved_base, sizeof base) == 0);
					assert(memcmp(&v, &saved_eye, sizeof v) == 0);
					float rows[9], actual[9];
					fl_quat_to_mat3(base.ori, rows);
					fl_quat_to_mat3(out.ori, actual);
					float norm = 0;
					for (int a = 0; a < 4; a++) norm += out.ori[a] * out.ori[a];
					close_float(norm, 1.0f);
					/* Identical current pose gives identical output immediately. */
					AeronSceneCamera repeated;
					fl_xr_eye_camera(&base, &v, &repeated);
					assert(memcmp(&out, &repeated, sizeof out) == 0);
					/* Physical head translation remains direct in the ship basis. */
					v.position[0] = 0.125f; v.position[1] = -0.06f; v.position[2] = 0.09f;
					fl_xr_eye_camera(&base, &v, &out);
					const float p[3] = {0.125f, 0.06f, -0.09f};
					for (int a = 0; a < 3; a++) {
						close_float(out.pos[a], base.pos[a] + AERON_OPT_UNITS_PER_METER *
							(rows[a] * p[0] + rows[3+a] * p[1] + rows[6+a] * p[2]));
					}
					/* Both eyes retain the supplied real eye separation. */
					AeronSceneCamera left, right;
					v.position[0] = -0.032f; fl_xr_eye_camera(&base, &v, &left);
					v.position[0] = 0.032f; fl_xr_eye_camera(&base, &v, &right);
					for (int a = 0; a < 3; a++) {
						close_float(right.pos[a] - left.pos[a], rows[a] * 0.064f * AERON_OPT_UNITS_PER_METER);
					}
				}
			}
		}
	}
}

static void test_cockpit_pan(void) {
	for (int seat = 0; seat < 3; seat++) {
		for (int rotation = 0; rotation < 8; rotation++) {
			XwaFlightCamera cam = camera();
			XwaCockpit cockpit = {0};
			cockpit.seat = (uint8_t)seat;
			cockpit.hardpoint_world[0] = 10.5f;
			cockpit.hardpoint_world[1] = -4.25f;
			cockpit.hardpoint_world[2] = 32;
			cockpit.camera_pan[0] = -31; cockpit.camera_pan[1] = 20; cockpit.camera_pan[2] = -7;
			const XwaCockpit saved = cockpit;
			XwaFlightObject anchor = {0};
			anchor.pitch = (int16_t)(rotation * 3193);
			anchor.yaw = (int16_t)(rotation * 5231);
			anchor.roll = (int16_t)(rotation * 7813);
			float rows[9], q[4] = {cosf(rotation * 0.2f), 0, sinf(rotation * 0.2f), 0};
			fl_quat_to_mat3(q, rows);
			float legacy[16], original[16], off[16], no_pan[16], pos[3] = {0};
			test_shake = "1";
			assert(legacy_fl_cockpit_model_matrix(&cockpit, &anchor, rows, pos, legacy));
			assert(fl_cockpit_model_matrix(&cockpit, &anchor, &cam, rows, pos, original));
			assert(memcmp(original, legacy, sizeof original) == 0);
			test_shake = "0";
			XwaVrComfort_ApplyCameraPan(&cam, 0.0f, pos);
			assert(fl_cockpit_model_matrix(&cockpit, &anchor, &cam, rows, pos, off));
			XwaCockpit reference = cockpit;
			memset(reference.camera_pan, 0, sizeof reference.camera_pan);
			float zero[3] = {0};
			assert(legacy_fl_cockpit_model_matrix(&reference, &anchor, rows, zero, no_pan));
			for (int a = 0; a < 3; a++) close_float(off[a*4+3] - pos[a], no_pan[a*4+3]);
			assert(memcmp(&saved, &cockpit, sizeof cockpit) == 0);
		}
	}
}

int main(void) {
	test_policy();
	test_eye_tracking();
	test_cockpit_pan();
	puts("PASS: comfort gates and scaling; 567 eye poses; 24 cockpit/turret poses; Original matches upstream; inputs unchanged");
	return 0;
}

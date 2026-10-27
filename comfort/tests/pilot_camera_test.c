static void near_value(float a,float b) { assert(fabsf(a-b)<0.0003f); }

static void test_rig_and_tracking(void) {
	_putenv_s("XWA_COMFORT_IMPACT_SHAKE","1");
	for(int pose=0;pose<25;pose++) for(int seat=0;seat<3;seat++) {
		XwaRemasterFlightView view={0};
		float angle=pose*0.13f;
		view.camera.ori[0]=cosf(angle); view.camera.ori[2]=sinf(angle);
		view.camera.pos[0]=8; view.camera.pos[1]=-3; view.camera.pos[2]=1;
		view.camera.near_z=0.01f;
		XwaRemasterFlightView before=view;
		XwaVrPilotMotion_Reset(&s_pilot_motion);
		int count=reprojects;
		fl_pilot_motion_apply_view(&view);
		assert(reprojects==count && memcmp(&view,&before,sizeof view)==0);
		float rows[9]; fl_quat_to_mat3(view.camera.ori,rows);
		XwaCockpit cockpit={0}; cockpit.seat=seat; cockpit.hardpoint_world[0]=340; cockpit.hardpoint_world[2]=-120;
		XwaFlightCamera cam={0}; cam.player_obj_idx=cam.focus_obj_idx=2;
		XwaFlightObject anchor={0}; anchor.yaw=pose*1327;
		float old_matrix[16],new_matrix[16];
		assert(fl_cockpit_model_matrix(&cockpit,&anchor,&cam,rows,view.camera.pos,old_matrix));
		const float d[3]={-180.25f,71.5f,-63.75f};
		memcpy(s_pilot_motion.offset_world,d,sizeof d);
		fl_pilot_motion_apply_view(&view);
		assert(reprojects==count+1);
		assert(memcmp(view.camera.ori,before.camera.ori,sizeof view.camera.ori)==0);
		assert(fl_cockpit_model_matrix(&cockpit,&anchor,&cam,rows,view.camera.pos,new_matrix));
		for(int i=0;i<16;i++) near_value(new_matrix[i]-old_matrix[i],i==3 ? d[0] : i==7 ? d[1] : i==11 ? d[2] : 0);
		for(int eye=0;eye<2;eye++) {
			AeronXrView tracked={0}; tracked.orientation[3]=cosf(angle/3); tracked.orientation[1]=sinf(angle/3);
			tracked.position[0]=eye ? 0.032f : -0.032f;
			tracked.position[1]=0.1f; tracked.position[2]=0.05f;
			tracked.fov[0]=-0.9f; tracked.fov[1]=0.7f; tracked.fov[2]=0.8f; tracked.fov[3]=-0.9f;
			AeronXrView saved=tracked;
			AeronSceneCamera a,b;
			fl_xr_eye_camera(&before.camera,&tracked,&a); fl_xr_eye_camera(&view.camera,&tracked,&b);
			assert(memcmp(a.ori,b.ori,sizeof a.ori)==0 && memcmp(&saved,&tracked,sizeof saved)==0);
			for(int j=0;j<3;j++) near_value(b.pos[j]-a.pos[j],d[j]);
		}
	}
}

static void test_runtime_gates(void) {
	XwaSnapshot* snap=calloc(1,sizeof *snap); assert(snap);
	snap->cockpit_valid=1; snap->cockpit.hardpoint_world[0]=300;
	snap->flight_camera.cockpit_visible=1;
	snap->flight_camera.player_obj_idx=snap->flight_camera.focus_obj_idx=1;
	_putenv_s("XWA_COMFORT_PILOT_CENTER",""); _putenv_s("XWA_COMFORT_SEAT_INERTIA","");
	_putenv_s("XWA_COMFORT_PILOT_INERTIA","0.035"); /* retired exterior-only key must not enable new effect */
	fl_pilot_motion_update(snap,1,1000000); assert(!s_pilot_motion.have);
	_putenv_s("XWA_COMFORT_PILOT_CENTER","1");
	fl_pilot_motion_update(snap,1,1010000); assert(s_pilot_motion.have);
	snap->game_time_ms=10; snap->flight_camera.world_pos[0]=121;
	snap->flight_camera.impact_pan_world[0]=1;
	fl_pilot_motion_update(snap,1,1020000);
	near_value(s_pilot_motion.pivot[0],-120); /* exact camera seat, not float cockpit hardpoint */
	snap->flight_camera.world_pos[0]=122; snap->flight_camera.impact_pan_world[0]=2;
	snap->game_time_ms=20;
	fl_pilot_motion_update(snap,1,1030000);
	near_value(s_pilot_motion.pivot[0],-120); /* impact shake must not drive centering */
	for(int mode=0;mode<8;mode++) {
		snap->flight_camera.external=mode==0; snap->flight_camera.film_overlay=mode==1;
		snap->flight_camera.map_mode=mode==2; snap->flight_camera.in_hangar=mode==3;
		snap->flight_camera.hyperspace_phase=mode==4; snap->flight_camera.cockpit_visible=mode!=5;
		unfocused=mode==6; s.hyperspace_preview=mode==7;
		fl_pilot_motion_update(snap,1,1020000+mode*10000); assert(!s_pilot_motion.have);
	}
	s.hyperspace_preview=0; unfocused=0;
	fl_pilot_motion_update(snap,0,1110000); assert(!s_pilot_motion.have);
	_putenv_s("XWA_COMFORT_PILOT_CENTER","0"); _putenv_s("XWA_COMFORT_SEAT_INERTIA","nan");
	fl_pilot_motion_update(snap,1,1120000); assert(!s_pilot_motion.have);
	_putenv_s("XWA_COMFORT_SEAT_INERTIA","0.01");
	fl_pilot_motion_update(snap,1,1130000); assert(s_pilot_motion.have);
	free(snap);
}

static void test_relative_inertia(void) {
	for(int pose=0;pose<25;pose++) for(int seat=0;seat<3;seat++) {
		XwaRemasterFlightView view={0};
		float angle=pose*0.13f;
		view.camera.ori[0]=cosf(angle); view.camera.ori[2]=sinf(angle);
		view.camera.pos[0]=8; view.camera.pos[1]=-3; view.camera.pos[2]=1;
		XwaRemasterFlightView before=view;
		XwaVrPilotMotion_Reset(&s_pilot_motion);
		s_pilot_motion.units_per_m=40.96f;
		const float pivot[3]={13,-28,45}, inertia[3]={0.01f,-0.02f,0.015f};
		for(int a=0;a<3;a++) {
			s_pilot_motion.inertia_m[a]=inertia[a];
			s_pilot_motion.offset_world[a]=pivot[a]+inertia[a]*40.96f;
		}
		fl_pilot_motion_apply_view(&view);
		fl_pilot_motion_space(&view.camera);
		float rig[3],rows[9]; fl_pilot_motion_rig_base(&view.camera,rig);
		fl_quat_to_mat3(view.camera.ori,rows);
		for(int a=0;a<3;a++) {
			near_value(rig[a],before.camera.pos[a]+pivot[a]);
			near_value(view.camera.pos[a]-rig[a],inertia[a]*40.96f);
		}
		XwaCockpit cockpit={0}; cockpit.seat=seat; cockpit.hardpoint_world[0]=340; cockpit.hardpoint_world[2]=-120;
		XwaFlightCamera cam={0}; cam.player_obj_idx=cam.focus_obj_idx=2;
		XwaFlightObject anchor={0}; anchor.yaw=pose*1327;
		float old_matrix[16],new_matrix[16];
		fl_cockpit_model_matrix(&cockpit,&anchor,&cam,rows,before.camera.pos,old_matrix);
		fl_cockpit_model_matrix(&cockpit,&anchor,&cam,rows,rig,new_matrix);
		for(int a=0;a<3;a++) {
			/* Relative eye/cockpit displacement must now be -inertia, not zero. */
			near_value((new_matrix[a*4+3]-view.camera.pos[a])-(old_matrix[a*4+3]-before.camera.pos[a]),-inertia[a]*40.96f);
		}
		const float* d=XwaVrPilotSpace_Offset();
		for(int a=0;a<3;a++) {
			float world=(rows[a]*d[0]-rows[3+a]*d[1]-rows[6+a]*d[2])*40.96f;
			near_value(world,inertia[a]*40.96f);
		}
	}
	XwaVrPilotMotion_Reset(&s_pilot_motion);
	AeronSceneCamera camera={0}; camera.ori[0]=1;
	fl_pilot_motion_space(&camera);
	for(int a=0;a<3;a++) assert(XwaVrPilotSpace_Offset()[a]==0);
}

int main(void) {
	test_rig_and_tracking(); test_runtime_gates(); test_relative_inertia();
	puts("PASS: production render-view offset, Off byte identity, 75 cockpit/turret rigs, 150 tracked eyes, relative eye/cockpit inertia on 75 poses, unchanged headset inputs/orientation, runtime settings and excluded-view gates");
}

static void near_value(float a,float b) { assert(fabsf(a-b)<0.00001f); }
static void update(void) { test_ns+=10000000; test_stick_update(); }

static void test_pose_and_layers(void) {
	AeronXrHand raw={0}; raw.valid=raw.grip_valid=1;
	raw.position[0]=0.2f; raw.position[1]=-0.5f; raw.position[2]=-0.4f;
	raw.grip_position[0]=0.22f; raw.grip_position[1]=-0.51f; raw.grip_position[2]=-0.37f;
	raw.orientation[1]=sinf(0.3f); raw.orientation[3]=cosf(0.3f);
	raw.grip_orientation[3]=1; raw.buttons=5; raw.trigger=0.7f; raw.grip=0.4f;
	raw_hands[0]=raw; tracked[0]=1;
	XwaVrPilotSpace_Reset();
	AeronXrHand hand;
	assert(XwaVrPilotSpace_GetHand(0,&hand)); assert(memcmp(&raw,&hand,sizeof raw)==0);
	const float d[3]={0.01f,-0.02f,0.015f}; XwaVrPilotSpace_Set(d);
	assert(XwaVrPilotSpace_GetHand(0,&hand));
	for(int a=0;a<3;a++) { near_value(hand.position[a],raw.position[a]+d[a]); near_value(hand.grip_position[a],raw.grip_position[a]+d[a]); }
	assert(memcmp(hand.orientation,raw.orientation,sizeof raw.orientation)==0);
	assert(memcmp(hand.grip_orientation,raw.grip_orientation,sizeof raw.grip_orientation)==0);
	assert(hand.buttons==raw.buttons && hand.trigger==raw.trigger && hand.grip==raw.grip);
	assert(memcmp(&raw,&raw_hands[0],sizeof raw)==0);
	AeronXrView views[2]={0}, cockpit[2];
	for(int e=0;e<2;e++) { views[e].orientation[3]=1; views[e].position[0]=e ? 0.032f : -0.032f; views[e].fov[0]=-0.8f; }
	AeronXrView saved[2]; memcpy(saved,views,sizeof views);
	XwaVrPilotSpace_Views(views,cockpit);
	assert(memcmp(views,saved,sizeof views)==0);
	near_value(cockpit[1].position[0]-cockpit[0].position[0],0.064f);
	for(int e=0;e<2;e++) assert(memcmp(cockpit[e].orientation,views[e].orientation,sizeof views[e].orientation)==0);
	AeronXrPanel panels[2]={0};
	for(int i=0;i<2;i++) { panels[i].position[0]=0.2f; panels[i].position[1]=-0.4f; panels[i].position[2]=-0.6f; panels[i].orientation[3]=1; panels[i].size[0]=0.3f; panels[i].rect[2]=512; }
	AeronXrPanel before[2]; memcpy(before,panels,sizeof before);
	XwaVrPilotSpace_Panels(panels,2);
	for(int i=0;i<2;i++) {
		for(int a=0;a<3;a++) {
			/* Same eye-to-panel ray in cockpit and presentation coordinates. */
			near_value(before[i].position[a]-cockpit[i].position[a],panels[i].position[a]-views[i].position[a]);
			/* Same hand ray/hit point in cockpit and tracking coordinates. */
			near_value(hand.position[a]-before[i].position[a],raw.position[a]-panels[i].position[a]);
		}
		assert(memcmp(panels[i].orientation,before[i].orientation,sizeof panels[i].orientation)==0);
		assert(panels[i].rect[2]==512 && panels[i].size[0]==0.3f);
		XwaVrPilotSpace_ToCockpit(panels[i].position);
		for(int a=0;a<3;a++) near_value(panels[i].position[a],before[i].position[a]);
	}
	model_tracked=1; float pos[3],q[4]; AeronXrRenderModelNode node;
	/* Cache cockpit-fixed geometry in C and viewer-fixed geometry in T.
	 * Body motion between frames must not drag static panels in the hand mask. */
	AeronXrPanel physical=before[0],presented=physical,cache[2],occlusion[2]; uint8_t flags[2];
	XwaVrPilotSpace_ToTracking(presented.position);
	XwaVrPilotSpace_CacheLayer(&presented,1,&cache[0],&flags[0]);
	AeronXrPanel world=before[1]; world.position[2]=-250;
	XwaVrPilotSpace_CacheLayer(&world,0,&cache[1],&flags[1]);
	for(int frame=0;frame<100;frame++) {
		float next[3]={0.02f*sinf(frame),0.015f*cosf(frame*0.3f),0.02f*sinf(frame*0.7f)};
		XwaVrPilotSpace_Set(next);
		XwaVrPilotSpace_Views(views,cockpit);
		XwaVrPilotSpace_LayerInCockpit(&cache[0],flags[0],&occlusion[0]);
		XwaVrPilotSpace_LayerInCockpit(&cache[1],flags[1],&occlusion[1]);
		for(int a=0;a<3;a++) {
			near_value(occlusion[0].position[a],physical.position[a]);
			near_value(occlusion[1].position[a]-cockpit[0].position[a],world.position[a]-views[0].position[a]);
		}
	}
	XwaVrPilotSpace_Set(d);
	assert(XwaVrPilotSpace_LocateRenderModel(0,pos,q,&node,1));
	near_value(pos[0],0.1f+d[0]); near_value(pos[1],-0.2f+d[1]); near_value(pos[2],-0.3f+d[2]);
	assert(q[3]==1 && node.position[1]==0.04f && node.visible);
	tracked[0]=0; assert(!XwaVrPilotSpace_GetHand(0,&hand));
	model_tracked=0; assert(!XwaVrPilotSpace_LocateRenderModel(0,pos,q,&node,1));
	float invalid[3]={NAN,0,0}; XwaVrPilotSpace_Set(invalid);
	for(int a=0;a<3;a++) assert(XwaVrPilotSpace_Offset()[a]==0);
	XwaVrPilotSpace_Views(views,cockpit); assert(memcmp(views,cockpit,sizeof views)==0);
}

static void init_stick(int hand,const float d[3]) {
	memset(&s_st,0,sizeof s_st); memset(raw_hands,0,sizeof raw_hands);
	tracked[0]=tracked[1]=0; tracked[hand]=1;
	_putenv_s("XWA_XR_STICK_POS",""); _putenv_s("XWA_XR_STICK_TWIST","0");
	XwaVrPilotSpace_Set(d);
	for(int a=0;a<3;a++) raw_hands[hand].position[a]=k_default_pivot[a]+(a==1 ? ST_GRIP_CENTER_M : 0)-d[a];
	raw_hands[hand].orientation[3]=1;
	update(); raw_hands[hand].grip=0.9f; update();
}

static void test_no_inertia_steering_or_placement(void) {
	float initial[3]={0.01f,-0.005f,0.01f}; init_stick(1,initial);
	assert(s_st.held && virtual_active);
	for(int i=0;i<100;i++) {
		float d[3]={0.02f*sinf(i),0.015f*cosf(i*0.3f),0.02f*sinf(i*0.7f)};
		XwaVrPilotSpace_Set(d); update();
		assert(s_st.held && virtual_active);
		for(int a=0;a<3;a++) near_value(virtual_axes[a],0);
	}
	XwaVrPilotSpace_Reset(); update();
	for(int a=0;a<3;a++) near_value(virtual_axes[a],0);
	raw_hands[1].position[0]+=0.04f; update();
	assert(virtual_axes[0]>0.2f); float side=virtual_axes[0];
	XwaVrPilotSpace_Set(initial); update(); near_value(virtual_axes[0],side);
	raw_hands[1].grip=0; update(); assert(!s_st.held);
	init_stick(0,initial); assert(s_st.moving);
	for(int i=0;i<100;i++) {
		float d[3]={0.02f*sinf(i),0.015f*cosf(i*0.3f),0.02f*sinf(i*0.7f)};
		XwaVrPilotSpace_Set(d); update();
		for(int a=0;a<3;a++) near_value(s_st.pivot[a],k_default_pivot[a]);
	}
	raw_hands[0].position[0]+=0.05f; update(); near_value(s_st.pivot[0],k_default_pivot[0]+0.05f);
	raw_hands[0].grip=0; update(); assert(!s_st.moving && s_st.dirty);
	float x,y,z; assert(sscanf(s_st.placement,"%f,%f,%f",&x,&y,&z)==3);
	near_value(x,k_default_pivot[0]+0.05f); near_value(y,k_default_pivot[1]); near_value(z,k_default_pivot[2]);
}

int main(void) {
	test_pose_and_layers(); test_no_inertia_steering_or_placement();
	puts("PASS: translated copies of aim/grip/runtime model poses; unchanged tracking orientation/buttons; stereo eye/hand/panel ray alignment; Off byte identity; 100 body-motion changes cannot steer a held stick or drift its placement; real hand movement still controls it");
}

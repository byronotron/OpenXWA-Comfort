/* Host tests include the production mask with a recording GPU backend. */
#include <assert.h>
#include <stdio.h>
#include "xwa_remaster/vr_peripheral_mask.c"

static int allocations, uploads, draws, passes, fail_upload_at;
static uint8_t uploaded[2][MASK_SIZE*MASK_SIZE*4];
static float drawn_alpha;
static AeronGraphicsPipelineDesc recorded_pipeline;
#define FAKE(type) ((type*)(uintptr_t)16)
AeronShader* Aeron_CreateShader(const AeronShaderDesc* d) {
	assert(strcmp(d->name,"scene_blit4.vert")==0 || strcmp(d->name,"scene_blit.frag")==0);
	allocations++; return FAKE(AeronShader);
}
AeronSampler* Aeron_CreateSampler(const AeronSamplerDesc* d) { (void)d; allocations++; return FAKE(AeronSampler); }
AeronTexture* Aeron_CreateTexture(const AeronTextureDesc* d) { assert(d->width==MASK_SIZE); allocations++; return FAKE(AeronTexture); }
AeronGraphicsPipeline* Aeron_CreateGraphicsPipeline(const AeronGraphicsPipelineDesc* d) {
	recorded_pipeline=*d; allocations++; return FAKE(AeronGraphicsPipeline);
}
void Aeron_DestroyGraphicsPipeline(AeronGraphicsPipeline* p) { (void)p; }
void Aeron_DestroyShader(AeronShader* p) { (void)p; }
void Aeron_DestroySampler(AeronSampler* p) { (void)p; }
void Aeron_DestroyTexture(AeronTexture* p) { (void)p; }
int Aeron_UploadTextureDataCmd(AeronCommandBuffer* cmd,const AeronTextureUploadDesc* d) {
	(void)cmd; uploads++;
	if(uploads==fail_upload_at) return 0;
	memcpy(uploaded[(uploads-1)%2],d->pixels,sizeof uploaded[0]); return 1;
}
AeronRenderPass* Aeron_BeginRenderPass(const AeronRenderPassDesc* d) {
	assert(!d->clear_color && !d->discard_color && !d->depth_target && !d->extra_color_target_count);
	passes++; return FAKE(AeronRenderPass);
}
void Aeron_EndRenderPass(AeronRenderPass* p) { (void)p; }
void Aeron_SetViewport(AeronRenderPass* p,const AeronRectI* r) { (void)p; assert(r->width>0); }
void Aeron_SetScissor(AeronRenderPass* p,const AeronRectI* r) { (void)p; (void)r; }
void Aeron_BindGraphicsPipeline(AeronRenderPass* p,AeronGraphicsPipeline* g) { (void)p; (void)g; }
void Aeron_BindTextureSampler(AeronRenderPass* p,AeronShaderStage s,uint32_t n,AeronTexture* t,AeronSampler* m) {
	(void)p; (void)s; (void)n; (void)t; (void)m;
}
void Aeron_BindUniformData(AeronRenderPass* p,AeronShaderStage s,uint32_t slot,const void* data,uint32_t size) {
	(void)p; (void)s; (void)slot; assert(size==128); drawn_alpha=((const MaskUniform*)data)->tint[3];
}
void Aeron_Draw(AeronRenderPass* p,uint32_t n,uint32_t first) { (void)p; assert(n==4 && first==0); draws++; }
void Aeron_LogInfo(const char* category,const char* fmt,...) { (void)category; (void)fmt; }
void Aeron_LogWarn(const char* category,const char* fmt,...) { (void)category; (void)fmt; }

static void near_value(float a,float b,float tolerance) { assert(fabsf(a-b)<tolerance); }
static void pose(float q[4],float degrees,int axis) {
	memset(q,0,4*sizeof(float)); q[0]=cosf(degrees*0.00872664626f); q[axis+1]=sinf(degrees*0.00872664626f);
}

static void test_motion(void) {
	for(int axis=0;axis<3;axis++) {
		XwaVrMaskMotion m={0}; float q[4]; uint64_t now=1000000;
		for(int i=0;i<=200;i++) {
			pose(q,i*0.9f,axis); /* 90 deg/s through quaternion wrap */
			if(i&1) for(int k=0;k<4;k++) q[k]=-q[k];
			XwaVrMask_UpdateMotion(&m,q,i*10,now+i*10000,7,1,1);
		}
		near_value(m.speed_dps,90,0.02f); assert(m.activation>0.9f);
		float prior=m.activation;
		for(int i=201;i<501;i++) {
			/* Held simulation, even if the user's head is turning. */
			XwaVrMask_UpdateMotion(&m,q,2000,now+i*10000,7,1,1);
			if(i>211) assert(m.activation<=prior+0.000001f);
			prior=m.activation;
		}
		assert(m.activation<0.003f && m.speed_dps==0);
		XwaVrMask_UpdateMotion(&m,q,5010,now+5010000,8,1,1); assert(m.activation==0);
		XwaVrMask_UpdateMotion(&m,q,5020,now+5020000,8,0,1); assert(!m.have);
	}
	assert(XwaVrMask_Response(60,0.65f)<XwaVrMask_Response(60,1));
	assert(XwaVrMask_Response(60,1)<XwaVrMask_Response(60,1.5f));
	assert(XwaVrMask_Response(0,3)==0);
	near_value(XwaVrMask_Parse("nan",0.65f,0,1),0.65f,0.00001f);
	near_value(XwaVrMask_Parse("999",1,0.25f,3),3,0.00001f);
	XwaVrMaskMotion m={0}; float q[4]={1,0,0,0};
	XwaVrMask_UpdateMotion(&m,q,0,1000000,1,1,1);
	pose(q,90,0); XwaVrMask_UpdateMotion(&m,q,10,1010000,1,1,1); assert(m.activation==0);
	/* Time reversal, pause/resume and long gaps re-prime, never flash. */
	XwaVrMask_UpdateMotion(&m,q,0,1020000,1,1,1); assert(m.activation==0);
	XwaVrMask_UpdateMotion(&m,q,20,2000000,1,1,1); assert(m.activation==0);
	float invalid[4]={0}; XwaVrMask_UpdateMotion(&m,invalid,30,2010000,1,1,1); assert(!m.have);
}

static void test_ship_basis(void) {
	for(int axis=0;axis<3;axis++) for(int step=-8;step<=8;step++) {
		XwaFlightObject f={0};
		if(axis==0) f.pitch=(int16_t)(step*3127);
		if(axis==1) f.yaw=(int16_t)(step*4127);
		if(axis==2) f.roll=(int16_t)(step*5127);
		float r[3][3], q[4], actual[9];
		fl_curmat_from_euler(&f,r); fl_comfort_ship_quaternion(&f,q); fl_quat_to_mat3(q,actual);
		for(int i=0;i<9;i++) near_value(actual[i],(&r[0][0])[i],0.0001f);
		for(int i=0;i<3;i++) {
			f.rows[i]=(int16_t)(r[0][i]*32767);
			f.rows[3+i]=(int16_t)(-r[2][i]*32767);
			f.rows[6+i]=(int16_t)(r[1][i]*32767);
		}
		f.has_mobj=1; fl_comfort_ship_quaternion(&f,q); fl_quat_to_mat3(q,actual);
		for(int i=0;i<9;i++) near_value(actual[i],(&r[0][0])[i],0.0002f);
	}
	XwaVrMaskMotion m={0};
	for(int i=0;i<200;i++) {
		XwaFlightObject f={0}; f.yaw=(int16_t)(i*0.9f*65536/360);
		float q[4]; fl_comfort_ship_quaternion(&f,q);
		XwaVrMask_UpdateMotion(&m,q,i*10,1000000+i*10000,1,1,1);
	}
	near_value(m.speed_dps,90,1);
}

static void test_head_and_stereo(void) {
	XwaVrMaskMotion m={0}; float ship[4]={1,0,0,0};
	for(int axis=0;axis<3;axis++) for(int i=0;i<200;i++) {
		float q[4], xr[4], axes[2][9]; pose(q,i*2.0f,axis);
		xr[0]=q[1]; xr[1]=q[2]; xr[2]=q[3]; xr[3]=q[0];
		XwaVrMask_EyeAxes(xr,xr,axes);
		for(int e=0;e<2;e++) for(int k=0;k<9;k++) near_value(axes[e][k],k%4==0 ? 1 : 0,0.00001f);
		XwaVrMask_UpdateMotion(&m,ship,(axis*200+i)*10,1000000+(axis*200+i)*10000,1,1,1);
		assert(m.speed_dps==0 && m.activation==0);
	}
	float left[4]={0,-sinf(0.05f),0,cosf(0.05f)}, right[4]={0,sinf(0.05f),0,cosf(0.05f)}, axes[2][9];
	XwaVrMask_EyeAxes(left,right,axes);
	float alpha[2];
	for(int e=0;e<2;e++) {
		/* Same 45-degree head-space ray mapped into each canted eye. */
		float x=axes[e][0]-axes[e][6], y=axes[e][1]-axes[e][7], z=axes[e][2]-axes[e][8];
		alpha[e]=XwaVrMask_AngularAlpha(x/-z,y/-z,axes[e]);
	}
	near_value(alpha[0],alpha[1],0.00001f);
	near_value(alpha[0],0.5f,0.00001f);
	float identity[9]={1,0,0,0,1,0,0,0,1};
	assert(XwaVrMask_AngularAlpha(0,0,identity)==0);
	assert(XwaVrMask_AngularAlpha(tanf(0.5f),0,identity)==0);
	assert(XwaVrMask_AngularAlpha(tanf(1.1f),0,identity)==1);
}

static void test_gpu_contract(void) {
	AeronXrView views[2]={0};
	for(int e=0;e<2;e++) {
		views[e].orientation[3]=1;
		views[e].fov[0]=e ? -0.7f : -0.9f; views[e].fov[1]=e ? 0.9f : 0.7f;
		views[e].fov[2]=0.85f; views[e].fov[3]=-0.85f;
	}
	AeronCommandBuffer* cmd=FAKE(AeronCommandBuffer);
	AeronRenderTarget* eyes[2]={FAKE(AeronRenderTarget),FAKE(AeronRenderTarget)};
	float q[4]={1,0,0,0};
	_putenv_s("XWA_COMFORT_MASK","");
	XwaVrPeripheralMask_Update(q,0,1000000,1,1);
	XwaVrPeripheralMask_Prepare(cmd,views,AERON_TEXTURE_FORMAT_RGBA8_UNORM);
	assert(XwaVrPeripheralMask_Render(cmd,eyes,1848,1936));
	assert(allocations==0 && uploads==0 && draws==0 && passes==0);
	_putenv_s("XWA_COMFORT_MASK","1");
	_putenv_s("XWA_COMFORT_MASK_STRENGTH","0.65");
	XwaVrPeripheralMask_Update(q,10,1010000,1,1);
	XwaVrPeripheralMask_Prepare(cmd,views,AERON_TEXTURE_FORMAT_RGBA8_UNORM);
	assert(uploads==2);
	assert(recorded_pipeline.blend.color_write_mask_enable && recorded_pipeline.blend.color_write_mask==7);
	assert(recorded_pipeline.blend.src_color==AERON_BLEND_ZERO);
	assert(recorded_pipeline.blend.dst_color==AERON_BLEND_ONE_MINUS_SRC_ALPHA);
	for(int y=0;y<MASK_SIZE;y++) for(int x=0;x<MASK_SIZE;x++) {
		int a=uploaded[0][4*(y*MASK_SIZE+x)+3];
		int b=uploaded[1][4*(y*MASK_SIZE+MASK_SIZE-1-x)+3];
		assert(abs(a-b)<=1);
	}
	for(int i=1;i<=200;i++) {
		pose(q,i*0.9f,1);
		XwaVrPeripheralMask_Update(q,10+i*10,1010000+i*10000,1,1);
	}
	XwaVrPeripheralMask_Prepare(cmd,views,AERON_TEXTURE_FORMAT_RGBA8_UNORM);
	assert(uploads==2); /* activation does not regenerate/upload mask textures */
	assert(XwaVrPeripheralMask_Render(cmd,eyes,1848,1936));
	assert(passes==2 && draws==2 && drawn_alpha>0.5f && drawn_alpha<=0.65f);
	views[0].fov[0]-=0.01f;
	fail_upload_at=uploads+2;
	XwaVrPeripheralMask_Prepare(cmd,views,AERON_TEXTURE_FORMAT_RGBA8_UNORM);
	assert(XwaVrPeripheralMask_Render(cmd,eyes,1848,1936)); assert(draws==2); /* both eyes skip on failure */
	_putenv_s("XWA_COMFORT_MASK","0");
	XwaVrPeripheralMask_Update(q,2020,3020000,1,1);
	XwaVrPeripheralMask_Prepare(cmd,views,AERON_TEXTURE_FORMAT_RGBA8_UNORM);
	assert(XwaVrPeripheralMask_Render(cmd,eyes,1848,1936)); assert(draws==2 && s_mask.opacity==0);
	XwaVrPeripheralMask_Shutdown();
}

int main(void) {
	test_motion(); test_ship_basis(); test_head_and_stereo(); test_gpu_contract();
	puts("PASS: ship yaw/pitch/roll, cached/Euler body basis, head-only immunity, wrap/sign, pause/reset, sensitivity, stereo angles, RGB-only GPU contract, cached uploads, Off zero GPU work");
	return 0;
}

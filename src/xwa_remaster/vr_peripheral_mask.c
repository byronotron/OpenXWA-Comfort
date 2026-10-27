#include "xwa_remaster/vr_peripheral_mask.h"
#include "xwa_remaster/vr_peripheral_math.h"
#include "aeron/log.h"

enum { MASK_SIZE = 128 };
typedef struct MaskUniform {
	float corners[4][4], q[4], depth[4], tint[4], bias[4];
} MaskUniform;
typedef char MaskUniformSize[sizeof(MaskUniform) == 128 ? 1 : -1];

static struct {
	XwaVrMaskMotion motion;
	int enabled, ready, cached, failed_logged;
	float strength, opacity, fov[2][4], axes[2][9];
	uint64_t log_us;
	AeronShader *vs, *fs;
	AeronSampler* sampler;
	AeronTexture* tex[2];
	AeronGraphicsPipeline* pipeline;
	AeronTextureFormat format;
	uint8_t pixels[MASK_SIZE * MASK_SIZE * 4];
} s_mask;

void XwaVrPeripheralMask_Reset(void) {
	memset(&s_mask.motion,0,sizeof s_mask.motion);
	s_mask.enabled = s_mask.ready = 0;
	s_mask.opacity = 0;
}

void XwaVrPeripheralMask_Update(const float ship_q[4], uint32_t stamp, uint64_t now,
	uint64_t identity, int eligible) {
	const char* enabled = getenv("XWA_COMFORT_MASK");
	if (!eligible || !enabled || strcmp(enabled,"1") != 0) {
		XwaVrPeripheralMask_Reset();
		return;
	}
	s_mask.strength = XwaVrMask_Parse(getenv("XWA_COMFORT_MASK_STRENGTH"),0.65f,0,1);
	float sensitivity = XwaVrMask_Parse(getenv("XWA_COMFORT_MASK_SENSITIVITY"),1,0.25f,3);
	s_mask.enabled = s_mask.strength > 0;
	XwaVrMask_UpdateMotion(&s_mask.motion,ship_q,stamp,now,identity,s_mask.enabled,sensitivity);
	s_mask.opacity = s_mask.strength * s_mask.motion.activation;
	if(now > s_mask.log_us + 5000000) {
		Aeron_LogInfo("xwa.comfort","mask: ship %.1f deg/s, sensitivity %.2f, strength %.2f, opacity %.3f",
			s_mask.motion.speed_dps,sensitivity,s_mask.strength,s_mask.opacity);
		s_mask.log_us=now;
	}
}

static int mask_resources(AeronTextureFormat format) {
	if (!s_mask.vs) s_mask.vs = Aeron_CreateShader(&(AeronShaderDesc){
		.name="scene_blit4.vert", .stage=AERON_SHADER_STAGE_VERTEX, .uniform_buffer_count=1 });
	if (!s_mask.fs) s_mask.fs = Aeron_CreateShader(&(AeronShaderDesc){
		.name="scene_blit.frag", .stage=AERON_SHADER_STAGE_FRAGMENT, .sampler_count=1 });
	if (!s_mask.sampler) s_mask.sampler = Aeron_CreateSampler(&(AeronSamplerDesc){
		.min_filter=AERON_FILTER_LINEAR, .mag_filter=AERON_FILTER_LINEAR,
		.mip_filter=AERON_FILTER_NEAREST, .address_u=AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_v=AERON_ADDRESS_CLAMP_TO_EDGE, .address_w=AERON_ADDRESS_CLAMP_TO_EDGE });
	if (!s_mask.vs || !s_mask.fs || !s_mask.sampler) return 0;
	if (s_mask.pipeline && s_mask.format != format) {
		Aeron_DestroyGraphicsPipeline(s_mask.pipeline);
		s_mask.pipeline=NULL;
	}
	if (!s_mask.pipeline) {
		s_mask.pipeline = Aeron_CreateGraphicsPipeline(&(AeronGraphicsPipelineDesc){
			.vertex_shader=s_mask.vs, .fragment_shader=s_mask.fs,
			.primitive_type=AERON_PRIMITIVE_TRIANGLE_STRIP, .cull_mode=AERON_CULL_NONE,
			.color_format=format, .sample_count=AERON_SAMPLE_COUNT_1,
			.blend={ .enabled=1, .src_color=AERON_BLEND_ZERO,
				.dst_color=AERON_BLEND_ONE_MINUS_SRC_ALPHA, .color_op=AERON_BLEND_OP_ADD,
				.src_alpha=AERON_BLEND_ZERO, .dst_alpha=AERON_BLEND_ONE, .alpha_op=AERON_BLEND_OP_ADD,
				/* Preserve hand-overlay alpha: never turn a vignette into an opaque HUD layer. */
				.color_write_mask_enable=1, .color_write_mask=0x7 } });
		s_mask.format=format;
	}
	for(int e=0;e<2;e++) if(!s_mask.tex[e]) s_mask.tex[e]=Aeron_CreateTexture(&(AeronTextureDesc){
		.width=MASK_SIZE, .height=MASK_SIZE, .mip_count=1, .format=AERON_TEXTURE_FORMAT_RGBA8_UNORM,
		.usage=AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.debug_name=e ? "comfort.mask.right" : "comfort.mask.left" });
	return s_mask.pipeline && s_mask.tex[0] && s_mask.tex[1];
}

void XwaVrPeripheralMask_Prepare(AeronCommandBuffer* cmd, const AeronXrView views[2], AeronTextureFormat format) {
	s_mask.ready=0;
	if(!s_mask.enabled || !cmd || !views) return;
	float axes[2][9];
	XwaVrMask_EyeAxes(views[0].orientation,views[1].orientation,axes);
	int same=s_mask.cached;
	for(int e=0;e<2;e++) {
		for(int i=0;i<4;i++) same &= fabsf(views[e].fov[i]-s_mask.fov[e][i]) < 0.00001f;
		for(int i=0;i<9;i++) same &= fabsf(axes[e][i]-s_mask.axes[e][i]) < 0.0005f;
		for(int i=0;i<4;i++) if(!isfinite(views[e].fov[i])) return;
		if(views[e].fov[1] <= views[e].fov[0] || views[e].fov[2] <= views[e].fov[3]) return;
	}
	if(!mask_resources(format)) {
		if(!s_mask.failed_logged) Aeron_LogWarn("xwa.comfort","Peripheral mask unavailable; leaving both eyes unchanged");
		s_mask.failed_logged=1;
		return;
	}
	if(!same) {
		s_mask.cached=0;
		for(int e=0;e<2;e++) {
			float l=tanf(views[e].fov[0]), r=tanf(views[e].fov[1]);
			float u=tanf(views[e].fov[2]), d=tanf(views[e].fov[3]);
			for(int y=0;y<MASK_SIZE;y++) for(int x=0;x<MASK_SIZE;x++) {
				float tx=l+(r-l)*((float)x+0.5f)/MASK_SIZE;
				float ty=u+(d-u)*((float)y+0.5f)/MASK_SIZE;
				uint8_t* p=&s_mask.pixels[4*(y*MASK_SIZE+x)];
				p[0]=p[1]=p[2]=0;
				p[3]=(uint8_t)(255*XwaVrMask_AngularAlpha(tx,ty,axes[e])+0.5f);
			}
			if(!Aeron_UploadTextureDataCmd(cmd,&(AeronTextureUploadDesc){
				.texture=s_mask.tex[e], .width=MASK_SIZE, .height=MASK_SIZE,
				.pixels=s_mask.pixels, .pitch=MASK_SIZE*4,
				.pixel_format=AERON_PIXEL_FORMAT_RGBA8888, .cycle=1 })) return;
			memcpy(s_mask.fov[e],views[e].fov,sizeof s_mask.fov[e]);
			memcpy(s_mask.axes[e],axes[e],sizeof axes[e]);
		}
		s_mask.cached=1;
	}
	s_mask.ready=1;
}

int XwaVrPeripheralMask_Render(AeronCommandBuffer* cmd, AeronRenderTarget* const eyes[2], int w, int h) {
	if(!s_mask.ready || s_mask.opacity < 0.001f) return 1;
	MaskUniform u={
		.corners={{-1,1,0,0},{1,1,1,0},{-1,-1,0,1},{1,-1,1,1}},
		.q={1,1,1,1}, .tint={0,0,0,s_mask.opacity} };
	for(int e=0;e<2;e++) {
		AeronRenderPass* pass=Aeron_BeginRenderPass(&(AeronRenderPassDesc){
			.color_target=eyes[e], .command_buffer=cmd, .debug_label="Comfort peripheral mask" });
		if(!pass) return 0;
		const AeronRectI full={0,0,w,h};
		Aeron_SetViewport(pass,&full); Aeron_SetScissor(pass,&full);
		Aeron_BindGraphicsPipeline(pass,s_mask.pipeline);
		Aeron_BindTextureSampler(pass,AERON_SHADER_STAGE_FRAGMENT,0,s_mask.tex[e],s_mask.sampler);
		Aeron_BindUniformData(pass,AERON_SHADER_STAGE_VERTEX,0,&u,sizeof u);
		Aeron_Draw(pass,4,0);
		Aeron_EndRenderPass(pass);
	}
	return 1;
}

void XwaVrPeripheralMask_Shutdown(void) {
	if(s_mask.pipeline) Aeron_DestroyGraphicsPipeline(s_mask.pipeline);
	if(s_mask.vs) Aeron_DestroyShader(s_mask.vs);
	if(s_mask.fs) Aeron_DestroyShader(s_mask.fs);
	if(s_mask.sampler) Aeron_DestroySampler(s_mask.sampler);
	for(int e=0;e<2;e++) if(s_mask.tex[e]) Aeron_DestroyTexture(s_mask.tex[e]);
	memset(&s_mask,0,sizeof s_mask);
}

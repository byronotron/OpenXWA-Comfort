/*
 * OpenXWA Vulkan foveation shim (Quest).
 *
 * Loaded by SDL in place of libvulkan.so (SDL_HINT_VULKAN_LIBRARY); passes
 * everything through to the system loader except a handful of calls, to
 * give the 3D scene fixed foveated rendering with VK_EXT_fragment_density_map
 * - the mechanism Meta's own FFR uses - without changes to SDL:
 *
 *   vkCreateDevice      enables the extension and its features when the
 *                       device has them (fragmentDensityMap and
 *                       fragmentDensityMapNonSubsampledImages).
 *   vkCreateRenderPass  adds a fragment density map attachment to every render
 *                       pass with a multisampled colour attachment: the 3D
 *                       scene's forward pass (and the pipelines created for
 *                       it, which SDL makes against such a pass).
 *   vkCreateFramebuffer gives those passes their map: the foveated one when the
 *                       framebuffer is the VR eye size, else a full-density map
 *                       (no change).
 *
 * Attachments stay ordinary (not subsampled): the GPU shades the periphery in
 * larger fragments and writes them to every covered pixel.
 *
 * Environment (read when needed):
 *   XWA_XR_FOVEATION         0/off, low (default), medium, high
 *   OPENXWA_FOVEATION_EYE    "w h cx_left cy_left cx_right cy_right": the eye
 *                            framebuffer size and each eye's optical centre in
 *                            [-1,1] image coordinates (+y down), set by the game
 *                            before it renders the first eye.
 */

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_log[4096];

/* Logs to logcat (liblog, looked up at run time) and keeps the line for the
 * game's log. */
static void shim_log(int prio, const char* fmt, ...) {
	char text[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof text, fmt, ap);
	va_end(ap);
	static int (*print)(int, const char*, const char*, ...);
	static int tried;
	if (!tried) {
		void* liblog = dlopen("liblog.so", RTLD_NOW | RTLD_LOCAL);
		print = liblog ? (int (*)(int, const char*, const char*, ...))dlsym(liblog, "__android_log_print") : NULL;
		tried = 1;
	}
	if (print) {
		print(prio, "openxwa.vkshim", "%s", text);
	}
	/* Kept for the game, which forwards it to its own log
	 * (OpenXWA_VkShim_TakeLog). */
	pthread_mutex_lock(&g_log_lock);
	const size_t used = strlen(g_log), add = strlen(text);
	if (used + add + 3 < sizeof g_log) {
		g_log[used] = prio >= 5 ? '!' : ' ';
		memcpy(g_log + used + 1, text, add);
		g_log[used + 1 + add] = '\n';
		g_log[used + 2 + add] = '\0';
	}
	pthread_mutex_unlock(&g_log_lock);
}

/* Moves the messages logged so far into `out` (lines; a leading '!' marks a
 * warning). Returns their length. */
__attribute__((visibility("default"))) int OpenXWA_VkShim_TakeLog(char* out, int size) {
	pthread_mutex_lock(&g_log_lock);
	int n = (int)strlen(g_log);
	if (n >= size) {
		n = size - 1;
	}
	if (n > 0) {
		memcpy(out, g_log, (size_t)n);
	}
	out[n > 0 ? n : 0] = '\0';
	g_log[0] = '\0';
	pthread_mutex_unlock(&g_log_lock);
	return n;
}
#define LOGI(...) shim_log(4, __VA_ARGS__)
#define LOGW(...) shim_log(5, __VA_ARGS__)

#define SHIM_EXPORT __attribute__((visibility("default")))

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void* g_lib;
static PFN_vkGetInstanceProcAddr g_real_gipa;
static VkInstance g_instance;

/* The device we foveate (one). */
static struct {
	VkDevice device;
	VkPhysicalDevice physical;
	uint32_t queue_family;
	int active;
	VkExtent2D texel; /* map texel size in framebuffer pixels */
	PFN_vkGetDeviceProcAddr gdpa;
	PFN_vkCreateRenderPass CreateRenderPass;
	PFN_vkDestroyRenderPass DestroyRenderPass;
	PFN_vkCreateFramebuffer CreateFramebuffer;
	PFN_vkDestroyDevice DestroyDevice;
	PFN_vkCreateImage CreateImage;
	PFN_vkDestroyImage DestroyImage;
	PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
	PFN_vkAllocateMemory AllocateMemory;
	PFN_vkFreeMemory FreeMemory;
	PFN_vkBindImageMemory BindImageMemory;
	PFN_vkCreateImageView CreateImageView;
	PFN_vkDestroyImageView DestroyImageView;
	PFN_vkCreateBuffer CreateBuffer;
	PFN_vkDestroyBuffer DestroyBuffer;
	PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
	PFN_vkBindBufferMemory BindBufferMemory;
	PFN_vkMapMemory MapMemory;
	PFN_vkUnmapMemory UnmapMemory;
	PFN_vkCreateCommandPool CreateCommandPool;
	PFN_vkDestroyCommandPool DestroyCommandPool;
	PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
	PFN_vkBeginCommandBuffer BeginCommandBuffer;
	PFN_vkEndCommandBuffer EndCommandBuffer;
	PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
	PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
	PFN_vkGetDeviceQueue GetDeviceQueue;
	PFN_vkQueueSubmit QueueSubmit;
	PFN_vkQueueWaitIdle QueueWaitIdle;
	VkPhysicalDeviceMemoryProperties memory;
} g_dev;

/* Render passes that got a map attachment. */
#define SHIM_MAX_PASSES 512
static VkRenderPass g_passes[SHIM_MAX_PASSES];
static int g_pass_count;

/* Density maps: one full-density map (any size up to its extent) and the
 * foveated one for the eye framebuffer. */
typedef struct ShimMap {
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	uint32_t w, h; /* texels */
} ShimMap;
static ShimMap g_full, g_eye;
static int g_eye_w, g_eye_h, g_eye_tried, g_logged_eye;

/* ---- Helpers ------------------------------------------------------------ */

static int shim_level(void) {
	const char* v = getenv("XWA_XR_FOVEATION");
	if (!v || !v[0]) {
		return 1;
	}
	if (v[0] == '0' || strcmp(v, "off") == 0) {
		return 0;
	}
	if (strcmp(v, "low") == 0 || v[0] == '1') {
		return 1;
	}
	if (strcmp(v, "high") == 0 || v[0] == '3') {
		return 3;
	}
	if (strcmp(v, "medium") == 0 || v[0] == '2') {
		return 2;
	}
	return 1;
}

static int pass_known(VkRenderPass p) {
	for (int i = 0; i < g_pass_count; ++i) {
		if (g_passes[i] == p) {
			return 1;
		}
	}
	return 0;
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
	for (uint32_t i = 0; i < g_dev.memory.memoryTypeCount; ++i) {
		if ((bits & (1u << i)) && (g_dev.memory.memoryTypes[i].propertyFlags & want) == want) {
			return i;
		}
	}
	return UINT32_MAX;
}

static void map_destroy(ShimMap* m) {
	if (m->view) {
		g_dev.DestroyImageView(g_dev.device, m->view, NULL);
	}
	if (m->image) {
		g_dev.DestroyImage(g_dev.device, m->image, NULL);
	}
	if (m->memory) {
		g_dev.FreeMemory(g_dev.device, m->memory, NULL);
	}
	memset(m, 0, sizeof *m);
}

/* Creates an R8G8 density map of w x h texels with `texels` (2 bytes each),
 * uploads it and leaves it in FRAGMENT_DENSITY_MAP_OPTIMAL. Synchronous: one
 * submission on the device's queue, waited for. */
static int map_create(ShimMap* m, uint32_t w, uint32_t h, const uint8_t* texels) {
	VkResult r;
	memset(m, 0, sizeof *m);
	const VkImageCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8_UNORM,
		.extent = { w, h, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if ((r = g_dev.CreateImage(g_dev.device, &ici, NULL, &m->image)) != VK_SUCCESS) {
		LOGW("density map image: %d", r);
		return 0;
	}
	VkMemoryRequirements req;
	g_dev.GetImageMemoryRequirements(g_dev.device, m->image, &req);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size };
	mai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (mai.memoryTypeIndex == UINT32_MAX) {
		mai.memoryTypeIndex = memory_type(req.memoryTypeBits, 0);
	}
	if (g_dev.AllocateMemory(g_dev.device, &mai, NULL, &m->memory) != VK_SUCCESS ||
		g_dev.BindImageMemory(g_dev.device, m->image, m->memory, 0) != VK_SUCCESS) {
		map_destroy(m);
		return 0;
	}
	const VkImageViewCreateInfo vci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = m->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	if (g_dev.CreateImageView(g_dev.device, &vci, NULL, &m->view) != VK_SUCCESS) {
		map_destroy(m);
		return 0;
	}
	/* Staging buffer. */
	const VkDeviceSize bytes = (VkDeviceSize)w * h * 2;
	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory staging_mem = VK_NULL_HANDLE;
	const VkBufferCreateInfo bci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = bytes,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	int ok = 0;
	VkCommandPool pool = VK_NULL_HANDLE;
	if (g_dev.CreateBuffer(g_dev.device, &bci, NULL, &staging) == VK_SUCCESS) {
		VkMemoryRequirements breq;
		g_dev.GetBufferMemoryRequirements(g_dev.device, staging, &breq);
		VkMemoryAllocateInfo bmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = breq.size };
		bmai.memoryTypeIndex = memory_type(breq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
																	 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		void* ptr = NULL;
		if (bmai.memoryTypeIndex != UINT32_MAX &&
			g_dev.AllocateMemory(g_dev.device, &bmai, NULL, &staging_mem) == VK_SUCCESS &&
			g_dev.BindBufferMemory(g_dev.device, staging, staging_mem, 0) == VK_SUCCESS &&
			g_dev.MapMemory(g_dev.device, staging_mem, 0, bytes, 0, &ptr) == VK_SUCCESS) {
			memcpy(ptr, texels, (size_t)bytes);
			g_dev.UnmapMemory(g_dev.device, staging_mem);
			const VkCommandPoolCreateInfo pci = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
				.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
				.queueFamilyIndex = g_dev.queue_family,
			};
			VkCommandBuffer cb = VK_NULL_HANDLE;
			const VkCommandBufferAllocateInfo cai = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandBufferCount = 1,
				.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			};
			VkCommandBufferAllocateInfo cai2 = cai;
			if (g_dev.CreateCommandPool(g_dev.device, &pci, NULL, &pool) == VK_SUCCESS) {
				cai2.commandPool = pool;
			}
			if (pool && g_dev.AllocateCommandBuffers(g_dev.device, &cai2, &cb) == VK_SUCCESS) {
				const VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
													  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
				g_dev.BeginCommandBuffer(cb, &bi);
				VkImageMemoryBarrier b = {
					.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					.srcAccessMask = 0,
					.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
					.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.image = m->image,
					.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
				};
				g_dev.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
										 NULL, 0, NULL, 1, &b);
				const VkBufferImageCopy copy = {
					.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
					.imageExtent = { w, h, 1 },
				};
				g_dev.CmdCopyBufferToImage(cb, staging, m->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
				b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				b.dstAccessMask = VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT;
				b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				b.newLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
				g_dev.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
										 VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT, 0, 0, NULL, 0, NULL, 1, &b);
				g_dev.EndCommandBuffer(cb);
				VkQueue queue = VK_NULL_HANDLE;
				g_dev.GetDeviceQueue(g_dev.device, g_dev.queue_family, 0, &queue);
				const VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
										  .commandBufferCount = 1,
										  .pCommandBuffers = &cb };
				ok = queue && g_dev.QueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS &&
					 g_dev.QueueWaitIdle(queue) == VK_SUCCESS;
			}
		}
	}
	if (pool) {
		g_dev.DestroyCommandPool(g_dev.device, pool, NULL);
	}
	if (staging) {
		g_dev.DestroyBuffer(g_dev.device, staging, NULL);
	}
	if (staging_mem) {
		g_dev.FreeMemory(g_dev.device, staging_mem, NULL);
	}
	if (!ok) {
		LOGW("density map upload failed");
		map_destroy(m);
		return 0;
	}
	m->w = w;
	m->h = h;
	return 1;
}

/* Density (255 = every pixel, 128 = every 2nd, 64 = every 4th) of each zone
 * by level: zone 0 is the centre. Values are R = horizontal, G = vertical. */
static void level_zones(int level, float radius[3], uint8_t dens[4][2]) {
	static const float k_r[4][3] = {
		{ 9.0f, 9.0f, 9.0f },    /* off (unused) */
		{ 0.62f, 0.85f, 9.0f },  /* low */
		{ 0.52f, 0.72f, 0.92f }, /* medium */
		{ 0.42f, 0.60f, 0.80f }, /* high */
	};
	static const uint8_t k_d[4][4][2] = {
		{ { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 } },
		{ { 255, 255 }, { 128, 255 }, { 128, 128 }, { 128, 128 } },
		{ { 255, 255 }, { 128, 255 }, { 128, 128 }, { 64, 128 } },
		{ { 255, 255 }, { 128, 128 }, { 64, 128 }, { 64, 64 } },
	};
	memcpy(radius, k_r[level], sizeof k_r[level]);
	memcpy(dens, k_d[level], sizeof k_d[level]);
}

/* The foveated map for the eye framebuffer; NULL when not available. */
static ShimMap* eye_map(uint32_t fb_w, uint32_t fb_h) {
	const char* env = getenv("OPENXWA_FOVEATION_EYE");
	int w = 0, h = 0;
	float cxl = 0, cyl = 0, cxr = 0, cyr = 0;
	if (!env || sscanf(env, "%d %d %f %f %f %f", &w, &h, &cxl, &cyl, &cxr, &cyr) != 6 || (uint32_t)w != fb_w ||
		(uint32_t)h != fb_h) {
		return NULL;
	}
	if (g_eye.view && (int)fb_w == g_eye_w && (int)fb_h == g_eye_h) {
		return &g_eye;
	}
	if (g_eye_tried && (int)fb_w == g_eye_w && (int)fb_h == g_eye_h) {
		return NULL;
	}
	const int level = shim_level();
	g_eye_tried = 1;
	g_eye_w = (int)fb_w;
	g_eye_h = (int)fb_h;
	if (level == 0) {
		return NULL;
	}
	if (g_eye.view) {
		map_destroy(&g_eye);
	}
	const uint32_t tw = (fb_w + g_dev.texel.width - 1) / g_dev.texel.width;
	const uint32_t th = (fb_h + g_dev.texel.height - 1) / g_dev.texel.height;
	uint8_t* texels = (uint8_t*)malloc((size_t)tw * th * 2);
	if (!texels) {
		return NULL;
	}
	float radius[3];
	uint8_t dens[4][2];
	level_zones(level, radius, dens);
	/* Both eyes share the framebuffer (rendered in turn), so the full-detail
	 * zone spans both optical centres. */
	const float cx = 0.5f * (cxl + cxr), half_x = 0.5f * fabsf(cxl - cxr);
	const float cy = 0.5f * (cyl + cyr), half_y = 0.5f * fabsf(cyl - cyr);
	int counts[4] = { 0, 0, 0, 0 };
	for (uint32_t j = 0; j < th; ++j) {
		for (uint32_t i = 0; i < tw; ++i) {
			const float px = ((float)i + 0.5f) * (float)g_dev.texel.width / (float)fb_w * 2.0f - 1.0f;
			const float py = ((float)j + 0.5f) * (float)g_dev.texel.height / (float)fb_h * 2.0f - 1.0f;
			const float dx = fmaxf(0.0f, fabsf(px - cx) - half_x);
			/* The lenses blur less vertically than across: a slightly taller zone. */
			const float dy = fmaxf(0.0f, fabsf(py - cy) - half_y) * 0.85f;
			const float r = sqrtf(dx * dx + dy * dy);
			int zone = 0;
			while (zone < 3 && r > radius[zone]) {
				zone++;
			}
			counts[zone]++;
			texels[(j * tw + i) * 2 + 0] = dens[zone][0];
			texels[(j * tw + i) * 2 + 1] = dens[zone][1];
		}
	}
	const int ok = map_create(&g_eye, tw, th, texels);
	free(texels);
	if (ok) {
		const float n = (float)(tw * th);
		LOGI("foveation level %d on %ux%u eye framebuffers (%ux%u map of %ux%u px texels): zones %.0f/%.0f/%.0f/%.0f %%",
			 level, fb_w, fb_h, tw, th, g_dev.texel.width, g_dev.texel.height, 100.0f * counts[0] / n,
			 100.0f * counts[1] / n, 100.0f * counts[2] / n, 100.0f * counts[3] / n);
		return &g_eye;
	}
	return NULL;
}

static ShimMap* full_map(uint32_t fb_w, uint32_t fb_h) {
	const uint32_t tw = (fb_w + g_dev.texel.width - 1) / g_dev.texel.width;
	const uint32_t th = (fb_h + g_dev.texel.height - 1) / g_dev.texel.height;
	if (g_full.view && g_full.w >= tw && g_full.h >= th) {
		return &g_full;
	}
	const uint32_t w = tw > 256 ? tw : 256, h = th > 256 ? th : 256;
	uint8_t* texels = (uint8_t*)malloc((size_t)w * h * 2);
	if (!texels) {
		return NULL;
	}
	memset(texels, 255, (size_t)w * h * 2);
	/* The old map may still be referenced by framebuffers: keep it alive. */
	ShimMap fresh;
	const int ok = map_create(&fresh, w, h, texels);
	free(texels);
	if (!ok) {
		return NULL;
	}
	g_full = fresh;
	return &g_full;
}

/* ---- Hooks -------------------------------------------------------------- */

static VKAPI_ATTR VkResult VKAPI_CALL shim_CreateInstance(const VkInstanceCreateInfo* ci,
														  const VkAllocationCallbacks* a, VkInstance* out) {
	PFN_vkCreateInstance real = (PFN_vkCreateInstance)g_real_gipa(VK_NULL_HANDLE, "vkCreateInstance");
	const VkResult r = real(ci, a, out);
	if (r == VK_SUCCESS) {
		g_instance = *out;
	}
	LOGI("vkCreateInstance: %d (API %u.%u)", r, ci && ci->pApplicationInfo ? VK_API_VERSION_MAJOR(ci->pApplicationInfo->apiVersion) : 0,
		 ci && ci->pApplicationInfo ? VK_API_VERSION_MINOR(ci->pApplicationInfo->apiVersion) : 0);
	return r;
}

#define DEV_LOAD(name) g_dev.name = (PFN_vk##name)g_dev.gdpa(g_dev.device, "vk" #name)

static VKAPI_ATTR VkResult VKAPI_CALL shim_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
														const VkAllocationCallbacks* a, VkDevice* out) {
	PFN_vkCreateDevice real = (PFN_vkCreateDevice)g_real_gipa(g_instance, "vkCreateDevice");
	LOGI("vkCreateDevice (instance %s, level %d)", g_instance ? "known" : "UNKNOWN", shim_level());
	if (!real) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	if (shim_level() == 0 || g_dev.device) {
		return real(phys, ci, a, out);
	}
	/* Does the device have it? */
	PFN_vkEnumerateDeviceExtensionProperties enum_ext =
		(PFN_vkEnumerateDeviceExtensionProperties)g_real_gipa(g_instance, "vkEnumerateDeviceExtensionProperties");
	/* The KHR entry points first: SDL enables that instance extension, while
	 * the core 1.1 names may be unusable on a Vulkan 1.0 instance. */
	PFN_vkGetPhysicalDeviceFeatures2 features2 =
		(PFN_vkGetPhysicalDeviceFeatures2)g_real_gipa(g_instance, "vkGetPhysicalDeviceFeatures2KHR");
	PFN_vkGetPhysicalDeviceProperties2 props2 =
		(PFN_vkGetPhysicalDeviceProperties2)g_real_gipa(g_instance, "vkGetPhysicalDeviceProperties2KHR");
	if (!features2 || !props2) {
		LOGW("no vkGetPhysicalDevice*2KHR: foveation off");
		return real(phys, ci, a, out);
	}
	PFN_vkGetPhysicalDeviceMemoryProperties memprops =
		(PFN_vkGetPhysicalDeviceMemoryProperties)g_real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties");
	int has_ext = 0, already = 0;
	uint32_t n = 0;
	if (enum_ext && enum_ext(phys, NULL, &n, NULL) == VK_SUCCESS && n) {
		VkExtensionProperties* e = (VkExtensionProperties*)calloc(n, sizeof *e);
		if (e && enum_ext(phys, NULL, &n, e) == VK_SUCCESS) {
			for (uint32_t i = 0; i < n; ++i) {
				has_ext |= strcmp(e[i].extensionName, VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME) == 0;
			}
		}
		free(e);
	}
	for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) {
		already |= strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME) == 0;
	}
	VkPhysicalDeviceFragmentDensityMapFeaturesEXT feat = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT };
	VkPhysicalDeviceFragmentDensityMapPropertiesEXT prop = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT };
	if (has_ext && features2 && props2 && memprops && !already) {
		VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &feat };
		VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &prop };
		features2(phys, &f2);
		props2(phys, &p2);
	}
	LOGI("fragment density map: extension %d, feature %u, non-subsampled images %u, texel %ux%u..%ux%u", has_ext,
		 feat.fragmentDensityMap, feat.fragmentDensityMapNonSubsampledImages, prop.minFragmentDensityTexelSize.width,
		 prop.minFragmentDensityTexelSize.height, prop.maxFragmentDensityTexelSize.width,
		 prop.maxFragmentDensityTexelSize.height);
	if (!has_ext || already || !feat.fragmentDensityMap || !feat.fragmentDensityMapNonSubsampledImages ||
		prop.maxFragmentDensityTexelSize.width == 0) {
		LOGW("foveation unavailable on this device: rendering without it");
		return real(phys, ci, a, out);
	}

	/* The application's request plus the extension and its features. */
	VkDeviceCreateInfo mine = *ci;
	const char** names = (const char**)calloc(ci->enabledExtensionCount + 1, sizeof *names);
	if (!names) {
		return real(phys, ci, a, out);
	}
	memcpy(names, ci->ppEnabledExtensionNames, ci->enabledExtensionCount * sizeof *names);
	names[ci->enabledExtensionCount] = VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME;
	mine.enabledExtensionCount = ci->enabledExtensionCount + 1;
	mine.ppEnabledExtensionNames = names;
	VkPhysicalDeviceFragmentDensityMapFeaturesEXT enable = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT,
		.pNext = (void*)ci->pNext,
		.fragmentDensityMap = VK_TRUE,
		.fragmentDensityMapNonSubsampledImages = VK_TRUE,
	};
	mine.pNext = &enable;
	VkResult r = real(phys, &mine, a, out);
	free(names);
	if (r != VK_SUCCESS) {
		LOGW("device with fragment density map failed (%d): rendering without it", r);
		return real(phys, ci, a, out);
	}

	memset(&g_dev, 0, sizeof g_dev);
	g_dev.device = *out;
	g_dev.physical = phys;
	g_dev.queue_family = ci->queueCreateInfoCount ? ci->pQueueCreateInfos[0].queueFamilyIndex : 0;
	/* Map texel: the GPU derives it from the framebuffer / map size ratio
	 * (rounded up to a power of two, clamped to [min, max]), so a map built
	 * for 32 px texels is read at 32 px - fine enough for smooth zones (the
	 * Quest reports 16..256; the maximum gave a 6x7 map of 256 px blocks). */
	g_dev.texel.width = 32;
	g_dev.texel.height = 32;
	if (g_dev.texel.width < prop.minFragmentDensityTexelSize.width) {
		g_dev.texel.width = prop.minFragmentDensityTexelSize.width;
	}
	if (g_dev.texel.height < prop.minFragmentDensityTexelSize.height) {
		g_dev.texel.height = prop.minFragmentDensityTexelSize.height;
	}
	if (g_dev.texel.width > prop.maxFragmentDensityTexelSize.width) {
		g_dev.texel.width = prop.maxFragmentDensityTexelSize.width;
	}
	if (g_dev.texel.height > prop.maxFragmentDensityTexelSize.height) {
		g_dev.texel.height = prop.maxFragmentDensityTexelSize.height;
	}
	memprops(phys, &g_dev.memory);
	g_dev.gdpa = (PFN_vkGetDeviceProcAddr)g_real_gipa(g_instance, "vkGetDeviceProcAddr");
	DEV_LOAD(CreateRenderPass);
	DEV_LOAD(DestroyRenderPass);
	DEV_LOAD(CreateFramebuffer);
	DEV_LOAD(DestroyDevice);
	DEV_LOAD(CreateImage);
	DEV_LOAD(DestroyImage);
	DEV_LOAD(GetImageMemoryRequirements);
	DEV_LOAD(AllocateMemory);
	DEV_LOAD(FreeMemory);
	DEV_LOAD(BindImageMemory);
	DEV_LOAD(CreateImageView);
	DEV_LOAD(DestroyImageView);
	DEV_LOAD(CreateBuffer);
	DEV_LOAD(DestroyBuffer);
	DEV_LOAD(GetBufferMemoryRequirements);
	DEV_LOAD(BindBufferMemory);
	DEV_LOAD(MapMemory);
	DEV_LOAD(UnmapMemory);
	DEV_LOAD(CreateCommandPool);
	DEV_LOAD(DestroyCommandPool);
	DEV_LOAD(AllocateCommandBuffers);
	DEV_LOAD(BeginCommandBuffer);
	DEV_LOAD(EndCommandBuffer);
	DEV_LOAD(CmdPipelineBarrier);
	DEV_LOAD(CmdCopyBufferToImage);
	DEV_LOAD(GetDeviceQueue);
	DEV_LOAD(QueueSubmit);
	DEV_LOAD(QueueWaitIdle);
	g_dev.active = g_dev.CreateRenderPass && g_dev.CreateFramebuffer && g_dev.CreateImage && g_dev.QueueSubmit &&
				   g_dev.CmdCopyBufferToImage && g_dev.GetDeviceQueue;
	LOGI("foveation %s (map texel %ux%u)", g_dev.active ? "ready" : "unavailable", g_dev.texel.width,
		 g_dev.texel.height);
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL shim_DestroyDevice(VkDevice device, const VkAllocationCallbacks* a) {
	if (device == g_dev.device && g_dev.DestroyDevice) {
		PFN_vkDestroyDevice real = g_dev.DestroyDevice;
		pthread_mutex_lock(&g_lock);
		map_destroy(&g_eye);
		map_destroy(&g_full);
		g_pass_count = 0;
		g_eye_tried = 0;
		memset(&g_dev, 0, sizeof g_dev);
		pthread_mutex_unlock(&g_lock);
		real(device, a);
		return;
	}
	PFN_vkDestroyDevice real = (PFN_vkDestroyDevice)g_real_gipa(g_instance, "vkDestroyDevice");
	real(device, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* ci,
															const VkAllocationCallbacks* a, VkRenderPass* out) {
	int msaa = 0;
	for (uint32_t i = 0; i < ci->attachmentCount; ++i) {
		msaa |= ci->pAttachments[i].samples != VK_SAMPLE_COUNT_1_BIT;
	}
	if (device != g_dev.device || !g_dev.active || !msaa || g_pass_count >= SHIM_MAX_PASSES) {
		PFN_vkCreateRenderPass real = device == g_dev.device && g_dev.CreateRenderPass
										  ? g_dev.CreateRenderPass
										  : (PFN_vkCreateRenderPass)g_real_gipa(g_instance, "vkCreateRenderPass");
		return real(device, ci, a, out);
	}
	VkAttachmentDescription att[16];
	if (ci->attachmentCount >= 16) {
		return g_dev.CreateRenderPass(device, ci, a, out);
	}
	memcpy(att, ci->pAttachments, ci->attachmentCount * sizeof *att);
	att[ci->attachmentCount] = (VkAttachmentDescription) {
		.format = VK_FORMAT_R8G8_UNORM,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT,
		.finalLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT,
	};
	VkRenderPassFragmentDensityMapCreateInfoEXT fdm = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT,
		.pNext = ci->pNext,
		.fragmentDensityMapAttachment = { ci->attachmentCount, VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT },
	};
	VkRenderPassCreateInfo mine = *ci;
	mine.pNext = &fdm;
	mine.attachmentCount = ci->attachmentCount + 1;
	mine.pAttachments = att;
	const VkResult r = g_dev.CreateRenderPass(device, &mine, a, out);
	if (r != VK_SUCCESS) {
		LOGW("render pass with density map failed (%d): plain pass", r);
		return g_dev.CreateRenderPass(device, ci, a, out);
	}
	pthread_mutex_lock(&g_lock);
	g_passes[g_pass_count++] = *out;
	pthread_mutex_unlock(&g_lock);
	static int logged;
	if (logged < 4) {
		logged++;
		LOGI("multisampled render pass %d: density map attachment added (%u attachments)", logged,
			 mine.attachmentCount);
	}
	return r;
}

static VKAPI_ATTR void VKAPI_CALL shim_DestroyRenderPass(VkDevice device, VkRenderPass pass,
														 const VkAllocationCallbacks* a) {
	pthread_mutex_lock(&g_lock);
	for (int i = 0; i < g_pass_count; ++i) {
		if (g_passes[i] == pass) {
			g_passes[i] = g_passes[--g_pass_count];
			break;
		}
	}
	pthread_mutex_unlock(&g_lock);
	PFN_vkDestroyRenderPass real = device == g_dev.device && g_dev.DestroyRenderPass
									   ? g_dev.DestroyRenderPass
									   : (PFN_vkDestroyRenderPass)g_real_gipa(g_instance, "vkDestroyRenderPass");
	real(device, pass, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_CreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* ci,
															 const VkAllocationCallbacks* a, VkFramebuffer* out) {
	pthread_mutex_lock(&g_lock);
	const int ours = device == g_dev.device && g_dev.active && pass_known(ci->renderPass);
	ShimMap* map = NULL;
	if (ours) {
		map = eye_map(ci->width, ci->height);
		if (!map) {
			map = full_map(ci->width, ci->height);
		} else if (!g_logged_eye) {
			g_logged_eye = 1;
		}
	}
	pthread_mutex_unlock(&g_lock);
	if (!ours) {
		PFN_vkCreateFramebuffer real = device == g_dev.device && g_dev.CreateFramebuffer
										   ? g_dev.CreateFramebuffer
										   : (PFN_vkCreateFramebuffer)g_real_gipa(g_instance, "vkCreateFramebuffer");
		return real(device, ci, a, out);
	}
	if (!map || ci->attachmentCount >= 16) {
		LOGW("no density map for a %ux%u framebuffer", ci->width, ci->height);
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	VkImageView views[16];
	memcpy(views, ci->pAttachments, ci->attachmentCount * sizeof *views);
	views[ci->attachmentCount] = map->view;
	VkFramebufferCreateInfo mine = *ci;
	mine.attachmentCount = ci->attachmentCount + 1;
	mine.pAttachments = views;
	static int logged;
	if (logged < 6) {
		logged++;
		LOGI("framebuffer %ux%u for a multisampled pass: %s density map", ci->width, ci->height,
			 map == &g_eye ? "foveated" : "full");
	}
	return g_dev.CreateFramebuffer(device, &mine, a, out);
}

/* ---- Dispatch ----------------------------------------------------------- */

static PFN_vkVoidFunction shim_device_hook(const char* name) {
	if (strcmp(name, "vkCreateRenderPass") == 0) {
		return (PFN_vkVoidFunction)shim_CreateRenderPass;
	}
	if (strcmp(name, "vkDestroyRenderPass") == 0) {
		return (PFN_vkVoidFunction)shim_DestroyRenderPass;
	}
	if (strcmp(name, "vkCreateFramebuffer") == 0) {
		return (PFN_vkVoidFunction)shim_CreateFramebuffer;
	}
	if (strcmp(name, "vkDestroyDevice") == 0) {
		return (PFN_vkVoidFunction)shim_DestroyDevice;
	}
	return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL shim_GetDeviceProcAddr(VkDevice device, const char* name);

static int shim_load(void) {
	if (g_real_gipa) {
		return 1;
	}
	g_lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
	if (!g_lib) {
		LOGW("libvulkan.so: %s", dlerror());
		return 0;
	}
	g_real_gipa = (PFN_vkGetInstanceProcAddr)dlsym(g_lib, "vkGetInstanceProcAddr");
	LOGI("loaded in front of libvulkan.so (%s)", g_real_gipa ? "ok" : "no vkGetInstanceProcAddr");
	return g_real_gipa != NULL;
}

SHIM_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name) {
	if (!shim_load() || !name) {
		return NULL;
	}
	if (strcmp(name, "vkGetInstanceProcAddr") == 0) {
		return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
	}
	if (strcmp(name, "vkCreateInstance") == 0) {
		return (PFN_vkVoidFunction)shim_CreateInstance;
	}
	if (instance && g_instance == VK_NULL_HANDLE) {
		g_instance = instance;
	}
	if (strcmp(name, "vkCreateDevice") == 0) {
		return (PFN_vkVoidFunction)shim_CreateDevice;
	}
	if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
		return (PFN_vkVoidFunction)shim_GetDeviceProcAddr;
	}
	PFN_vkVoidFunction hook = shim_device_hook(name);
	if (hook) {
		return hook;
	}
	return g_real_gipa(instance, name);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL shim_GetDeviceProcAddr(VkDevice device, const char* name) {
	if (!name) {
		return NULL;
	}
	if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
		return (PFN_vkVoidFunction)shim_GetDeviceProcAddr;
	}
	PFN_vkVoidFunction hook = shim_device_hook(name);
	if (hook) {
		return hook;
	}
	PFN_vkGetDeviceProcAddr real = (PFN_vkGetDeviceProcAddr)g_real_gipa(g_instance, "vkGetDeviceProcAddr");
	return real ? real(device, name) : NULL;
}

/* Global entry points some loaders look up directly. */
SHIM_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* layer, uint32_t* count,
																				   VkExtensionProperties* props) {
	if (!shim_load()) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	PFN_vkEnumerateInstanceExtensionProperties real =
		(PFN_vkEnumerateInstanceExtensionProperties)g_real_gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
	return real ? real(layer, count, props) : VK_ERROR_INITIALIZATION_FAILED;
}

SHIM_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* ci,
															const VkAllocationCallbacks* a, VkInstance* out) {
	if (!shim_load()) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	return shim_CreateInstance(ci, a, out);
}

/* For the game: 1 when the eye framebuffers are being foveated. */
SHIM_EXPORT int OpenXWA_VkShim_Foveating(void) { return g_eye.view != VK_NULL_HANDLE; }

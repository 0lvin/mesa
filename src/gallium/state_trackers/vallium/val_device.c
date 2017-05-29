
#include "val_private.h"

#include "pipe-loader/pipe_loader.h"
#include "git_sha1.h"

#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "state_tracker/drisw_api.h"

#include "loader/loader.h"

#include "util/u_memory.h"
#include "util/u_cpu_detect.h"
#include "util/u_box.h"

static VkResult
val_physical_device_init(struct val_physical_device *device,
                         struct val_instance *instance,
                         struct pipe_loader_device *pld)
{
	VkResult result;
	device->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
	device->instance = instance;
	device->pld = pld;

	result = val_init_wsi(device);
	if (result != VK_SUCCESS)
		return vk_error(result);

	device->pscreen = pipe_loader_create_screen(device->pld);
	if (!device->pscreen)
		return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

	return VK_SUCCESS;
}

static const VkExtensionProperties global_extensions[] = {
	{
		.extensionName = VK_KHR_SURFACE_EXTENSION_NAME,
		.specVersion = 25,
	},
#ifdef VK_USE_PLATFORM_XCB_KHR
	{
		.extensionName = VK_KHR_XCB_SURFACE_EXTENSION_NAME,
		.specVersion = 6,
	},
#endif
#ifdef VK_USE_PLATFORM_XLIB_KHR
	{
		.extensionName = VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
		.specVersion = 6,
	},
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
	{
		.extensionName = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
		.specVersion = 5,
	},
#endif
};

static const VkExtensionProperties device_extensions[] = {
   {
      .extensionName = VK_KHR_SWAPCHAIN_EXTENSION_NAME,
      .specVersion = 67,
   },
};

void
val_mmap(struct val_device_memory *mem, struct val_device *device)
{
	val_finishme("Premapped(%p) %p ~ %p ~ %p", mem, mem->map, mem->pmem, mem->bo);

	//already mapped
	if (mem->map)
		return;

	if (!mem->bo) {
		mem->bo = device->pscreen->resource_create(device->pscreen,
							  &mem->template);
		val_finishme("Asked about map before create image/buffer, create without user memmory %p", mem->bo);
	}
	if (mem->bo) {

		assert(mem->bo->last_level == 0);

		if (mem->bo->target == PIPE_BUFFER)
			u_box_1d(0,
					 mem->bo->width0,
					 &mem->box);
		else
			u_box_3d(0, 0, 0,
					 mem->bo->width0, mem->bo->height0, mem->bo->depth0,
					 &mem->box);

		val_finishme("Map type %d %d * %d * %d \n", mem->bo->target,
		              mem->box.width, mem->box.height, mem->box.depth);


		mem->map = device->pctx->transfer_map(device->pctx,
					    mem->bo,
					    0,
					    PIPE_TRANSFER_READ | PIPE_TRANSFER_WRITE,
					    &mem->box,
					    &mem->bo_t);

		if (mem->bo->target == PIPE_BUFFER)
			for(int i = 0; i < mem->bo->width0; i ++) {
				printf(" 0x%2x",  ((char*)mem->map)[i] & 0xff);
				if (i % 16 == 15)
					printf("\n");
			}
	} else {
		u_box_1d(0,
				 mem->map_size,
				 &mem->box);

		val_finishme("Map type 'NoBo' %d * %d * %d",
		             mem->box.width, mem->box.height, mem->box.depth);

		// reuse already allocated memory
		mem->map = mem->pmem;
	}
	val_finishme("Mapped %p ~ %p ~ %d", mem->map, mem->pmem, mem->box.width * mem->box.height * mem->box.depth);
}

void
val_munmap(struct val_device_memory *mem, struct val_device *device)
{
	if (mem->bo) {
		if (mem->bo->target == PIPE_BUFFER)
			for(int i = 0; i < mem->bo->width0; i ++) {
				printf(" 0x%2x",  ((char*)mem->map)[i] & 0xff);
				if (i % 16 == 15)
					printf("\n");
			}
		device->pctx->transfer_unmap(device->pctx, mem->bo_t);
	}
	mem->map = NULL;
}

static void *
default_alloc_func(void *pUserData, size_t size, size_t align,
                   VkSystemAllocationScope allocationScope)
{
   return malloc(size);
}

static void *
default_realloc_func(void *pUserData, void *pOriginal, size_t size,
                     size_t align, VkSystemAllocationScope allocationScope)
{
   return realloc(pOriginal, size);
}

static void
default_free_func(void *pUserData, void *pMemory)
{
   free(pMemory);
}

static const VkAllocationCallbacks default_alloc = {
   .pUserData = NULL,
   .pfnAllocation = default_alloc_func,
   .pfnReallocation = default_realloc_func,
   .pfnFree = default_free_func,
};

VkResult val_CreateInstance(
    const VkInstanceCreateInfo*                 pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkInstance*                                 pInstance)
{
	struct val_instance *instance;

	assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);

	uint32_t client_version;
	if (pCreateInfo->pApplicationInfo &&
	    pCreateInfo->pApplicationInfo->apiVersion != 0) {
		client_version = pCreateInfo->pApplicationInfo->apiVersion;
	} else {
		client_version = VK_MAKE_VERSION(1, 0, 0);
	}

	if (VK_MAKE_VERSION(1, 0, 0) > client_version ||
	    client_version > VK_MAKE_VERSION(1, 0, 0xfff)) {
		return vk_errorf(VK_ERROR_INCOMPATIBLE_DRIVER,
				 "Client requested version %d.%d.%d",
				 VK_VERSION_MAJOR(client_version),
				 VK_VERSION_MINOR(client_version),
				 VK_VERSION_PATCH(client_version));
	}

	for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
		bool found = false;
		for (uint32_t j = 0; j < ARRAY_SIZE(global_extensions); j++) {
			if (strcmp(pCreateInfo->ppEnabledExtensionNames[i],
			    global_extensions[j].extensionName) == 0) {
				found = true;
				break;
			}
		}
		if (!found)
			return vk_error(VK_ERROR_EXTENSION_NOT_PRESENT);
	}

	instance = vk_alloc2(&default_alloc, pAllocator, sizeof(*instance), 8,
					VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
	if (!instance)
		return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

	instance->_loader_data.loaderMagic = ICD_LOADER_MAGIC;

	if (pAllocator)
		instance->alloc = *pAllocator;
	else
		instance->alloc = default_alloc;

	instance->apiVersion = client_version;
	instance->physicalDeviceCount = -1;

	util_cpu_detect();

	//   _mesa_locale_init();

	//   VG(VALGRIND_CREATE_MEMPOOL(instance, 0, false));

	*pInstance = val_instance_to_handle(instance);

	fprintf(stderr, "got into create instance\n");
	return VK_SUCCESS;
}


void val_DestroyInstance(
			 VkInstance                                  _instance,
			 const VkAllocationCallbacks*                pAllocator)
{
	VAL_FROM_HANDLE(val_instance, instance, _instance);

	if (   instance->physicalDeviceCount != -1) {
		val_finish_wsi(&instance->physicalDevice);
	}
	//   _mesa_locale_fini();
	vk_free(&instance->alloc, instance);
}

static void val_get_image(struct dri_drawable *dri_drawable,
                          int x, int y, unsigned width, unsigned height, unsigned stride,
                          void *data)
{

}

static void val_put_image(struct dri_drawable *dri_drawable,
                          void *data, unsigned width, unsigned height)
{
   fprintf(stderr, "put image %dx%d\n", width, height);
}

static void val_put_image2(struct dri_drawable *dri_drawable,
                           void *data, int x, int y, unsigned width, unsigned height,
                           unsigned stride)
{
   fprintf(stderr, "put image 2 %d,%d %dx%d\n", x, y, width, height);
}

static struct drisw_loader_funcs val_sw_lf = {
   .get_image = val_get_image,
   .put_image = val_put_image,
   .put_image2 = val_put_image2,
};

VkResult val_EnumeratePhysicalDevices(
				      VkInstance                                  _instance,
				      uint32_t*                                   pPhysicalDeviceCount,
				      VkPhysicalDevice*                           pPhysicalDevices)
{
	VAL_FROM_HANDLE(val_instance, instance, _instance);
	VkResult result = VK_ERROR_INCOMPATIBLE_DRIVER;

	if (instance->physicalDeviceCount < 0) {

		/* sw only for now */
		instance->num_devices = pipe_loader_sw_probe(NULL, 0);

		assert(instance->num_devices == 1);

		int fd = -1;
		char path[20];
		for (unsigned i = 0; i < 8; i++) {
			snprintf(path, sizeof(path), "/dev/dri/renderD%d", 128 + i);
			fd = loader_open_device(path);
			if (fd >= 0) break;
		}

		if (fd >= 0 && pipe_loader_drm_probe_fd(&instance->devs, fd)) {
			fprintf(stderr, "use real device %s.\n", instance->devs[0].driver_name);
			result = val_physical_device_init(&instance->physicalDevice,
							instance, &instance->devs[0]);
		}

		if (result != VK_SUCCESS && pipe_loader_sw_probe_dri(&instance->devs, &val_sw_lf)) {
			fprintf(stderr, "use software failback %s.\n", instance->devs[0].driver_name);
			result = val_physical_device_init(&instance->physicalDevice,
							instance, &instance->devs[0]);
		}

		if (result == VK_ERROR_INCOMPATIBLE_DRIVER) {
			instance->physicalDeviceCount = 0;
		} else if (result == VK_SUCCESS) {
			instance->physicalDeviceCount = 1;
		} else {
			return result;
		}
	}

	if (!pPhysicalDevices) {
		*pPhysicalDeviceCount = instance->physicalDeviceCount;
	} else if (*pPhysicalDeviceCount >= 1) {
		pPhysicalDevices[0] = val_physical_device_to_handle(&instance->physicalDevice);
		*pPhysicalDeviceCount = 1;
	} else {
		*pPhysicalDeviceCount = 0;
	}

	return VK_SUCCESS;
}

void val_GetPhysicalDeviceFeatures(
				   VkPhysicalDevice                            physicalDevice,
				   VkPhysicalDeviceFeatures*                   pFeatures)
{
	*pFeatures = (VkPhysicalDeviceFeatures) {
		.robustBufferAccess                       = true,
		.fullDrawIndexUint32                      = true,
		.imageCubeArray                           = false,
		.independentBlend                         = true,
		.geometryShader                           = true,
		.tessellationShader                       = false,
		.sampleRateShading                        = false,
		.dualSrcBlend                             = true,
		.logicOp                                  = true,
		.multiDrawIndirect                        = false,
		.drawIndirectFirstInstance                = false,
		.depthClamp                               = false,
		.depthBiasClamp                           = false,
		.fillModeNonSolid                         = true,
		.depthBounds                              = false,
		.wideLines                                = true,
		.largePoints                              = true,
		.alphaToOne                               = false,
		.multiViewport                            = true,
		.samplerAnisotropy                        = false, /* FINISHME */
		.textureCompressionETC2                   = true,
		.textureCompressionASTC_LDR               = true,
		.textureCompressionBC                     = true,
		.occlusionQueryPrecise                    = true,
		.pipelineStatisticsQuery                  = true,
		.vertexPipelineStoresAndAtomics           = false,
		.fragmentStoresAndAtomics                 = true,
		.shaderTessellationAndGeometryPointSize   = true,
		.shaderImageGatherExtended                = true,
		.shaderStorageImageExtendedFormats        = false,
		.shaderStorageImageMultisample            = false,
		.shaderUniformBufferArrayDynamicIndexing  = true,
		.shaderSampledImageArrayDynamicIndexing   = true,
		.shaderStorageBufferArrayDynamicIndexing  = true,
		.shaderStorageImageArrayDynamicIndexing   = true,
		.shaderStorageImageReadWithoutFormat      = false,
		.shaderStorageImageWriteWithoutFormat     = true,
		.shaderClipDistance                       = false,
		.shaderCullDistance                       = false,
		.shaderFloat64                            = false,
		.shaderInt64                              = false,
		.shaderInt16                              = false,
		.alphaToOne                               = true,
		.variableMultisampleRate                  = false,
		.inheritedQueries                         = false,
	};

}

void
val_device_get_cache_uuid(void *uuid)
{
   memset(uuid, 0, VK_UUID_SIZE);
   snprintf(uuid, VK_UUID_SIZE, "val-%s", MESA_GIT_SHA1 + 4);
}

void val_GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
				     VkPhysicalDeviceProperties *pProperties)
{
	VAL_FROM_HANDLE(val_physical_device, pdevice, physicalDevice);

	val_finishme("Get correct values for VkPhysicalDeviceLimits");

	const float time_stamp_base = 80.0;

	VkSampleCountFlags sample_counts = 1u;

	VkPhysicalDeviceLimits limits = {
		.maxImageDimension1D                      = (1 << 14),
		.maxImageDimension2D                      = (1 << 14),
		.maxImageDimension3D                      = (1 << 10),
		.maxImageDimensionCube                    = (1 << 14),
		.maxImageArrayLayers                      = (1 << 10),
		.maxTexelBufferElements                   = 128 * 1024 * 1024,
		.maxUniformBufferRange                    = UINT32_MAX,
		.maxStorageBufferRange                    = UINT32_MAX,
		.maxPushConstantsSize                     = MAX_PUSH_CONSTANTS_SIZE,
		.maxMemoryAllocationCount                 = UINT32_MAX,
		.maxSamplerAllocationCount                = 64 * 1024,
		.bufferImageGranularity                   = 64, /* A cache line */
		.sparseAddressSpaceSize                   = 0,
		.maxBoundDescriptorSets                   = MAX_SETS,
		.maxPerStageDescriptorSamplers            = 64,
		.maxPerStageDescriptorUniformBuffers      = 64,
		.maxPerStageDescriptorStorageBuffers      = 64,
		.maxPerStageDescriptorSampledImages       = 64,
		.maxPerStageDescriptorStorageImages       = 64,
		.maxPerStageDescriptorInputAttachments    = 64,
		.maxPerStageResources                     = 128,
		.maxDescriptorSetSamplers                 = 256,
		.maxDescriptorSetUniformBuffers           = 256,
		.maxDescriptorSetUniformBuffersDynamic    = 256,
		.maxDescriptorSetStorageBuffers           = 256,
		.maxDescriptorSetStorageBuffersDynamic    = 256,
		.maxDescriptorSetSampledImages            = 256,
		.maxDescriptorSetStorageImages            = 256,
		.maxDescriptorSetInputAttachments         = 256,
		.maxVertexInputAttributes                 = 32,
		.maxVertexInputBindings                   = 32,
		.maxVertexInputAttributeOffset            = 2047,
		.maxVertexInputBindingStride              = 2048,
		.maxVertexOutputComponents                = 128,
		.maxTessellationGenerationLevel           = 0,
		.maxTessellationPatchSize                 = 0,
		.maxTessellationControlPerVertexInputComponents = 0,
		.maxTessellationControlPerVertexOutputComponents = 0,
		.maxTessellationControlPerPatchOutputComponents = 0,
		.maxTessellationControlTotalOutputComponents = 0,
		.maxTessellationEvaluationInputComponents = 0,
		.maxTessellationEvaluationOutputComponents = 0,
		.maxGeometryShaderInvocations             = 32,
		.maxGeometryInputComponents               = 64,
		.maxGeometryOutputComponents              = 128,
		.maxGeometryOutputVertices                = 256,
		.maxGeometryTotalOutputComponents         = 1024,
		.maxFragmentInputComponents               = 128,
		.maxFragmentOutputAttachments             = 8,
		.maxFragmentDualSrcAttachments            = 2,
		.maxFragmentCombinedOutputResources       = 8,
		.maxComputeSharedMemorySize               = 32768,
		.maxComputeWorkGroupCount                 = { 65535, 65535, 65535 },
		.maxComputeWorkGroupInvocations           = 0,
		.maxComputeWorkGroupSize = { 0, 0, 0 },
		.subPixelPrecisionBits                    = 4 /* FIXME */,
		.subTexelPrecisionBits                    = 4 /* FIXME */,
		.mipmapPrecisionBits                      = 4 /* FIXME */,
		.maxDrawIndexedIndexValue                 = UINT32_MAX,
		.maxDrawIndirectCount                     = UINT32_MAX,
		.maxSamplerLodBias                        = 16,
		.maxSamplerAnisotropy                     = 16,
		.maxViewports                             = MAX_VIEWPORTS,
		.maxViewportDimensions                    = { (1 << 14), (1 << 14) },
		.viewportBoundsRange                      = { -16384.0, 16384.0 },
		.viewportSubPixelBits                     = 13, /* We take a float? */
		.minMemoryMapAlignment                    = 4096, /* A page */
		.minTexelBufferOffsetAlignment            = 1,
		.minUniformBufferOffsetAlignment          = 1,
		.minStorageBufferOffsetAlignment          = 1,
		.minTexelOffset                           = -8,
		.maxTexelOffset                           = 7,
		.minTexelGatherOffset                     = -8,
		.maxTexelGatherOffset                     = 7,
		.minInterpolationOffset                   = 0, /* FIXME */
		.maxInterpolationOffset                   = 0, /* FIXME */
		.subPixelInterpolationOffsetBits          = 0, /* FIXME */
		.maxFramebufferWidth                      = (1 << 14),
		.maxFramebufferHeight                     = (1 << 14),
		.maxFramebufferLayers                     = (1 << 10),
		.framebufferColorSampleCounts             = sample_counts,
		.framebufferDepthSampleCounts             = sample_counts,
		.framebufferStencilSampleCounts           = sample_counts,
		.framebufferNoAttachmentsSampleCounts     = sample_counts,
		.maxColorAttachments                      = MAX_RTS,
		.sampledImageColorSampleCounts            = sample_counts,
		.sampledImageIntegerSampleCounts          = VK_SAMPLE_COUNT_1_BIT,
		.sampledImageDepthSampleCounts            = sample_counts,
		.sampledImageStencilSampleCounts          = sample_counts,
		.storageImageSampleCounts                 = VK_SAMPLE_COUNT_1_BIT,
		.maxSampleMaskWords                       = 1,
		.timestampComputeAndGraphics              = false,
		.timestampPeriod                          = time_stamp_base / (1000 * 1000 * 1000),
		.maxClipDistances                         = 0 /* FIXME */,
		.maxCullDistances                         = 0 /* FIXME */,
		.maxCombinedClipAndCullDistances          = 0 /* FIXME */,
		.discreteQueuePriorities                  = 1,
		.pointSizeRange                           = { 0.125, 255.875 },
		.lineWidthRange                           = { 0.0, 7.9921875 },
		.pointSizeGranularity                     = (1.0 / 8.0),
		.lineWidthGranularity                     = (1.0 / 128.0),
		.strictLines                              = false, /* FINISHME */
		.standardSampleLocations                  = true,
		.optimalBufferCopyOffsetAlignment         = 128,
		.optimalBufferCopyRowPitchAlignment       = 128,
		.nonCoherentAtomSize                      = 64,
	};

	*pProperties = (VkPhysicalDeviceProperties) {
		.apiVersion = VK_MAKE_VERSION(1, 0, 2),
		.driverVersion = 1,
		.vendorID = 0,
		.deviceID = 0,
		.deviceType = VK_PHYSICAL_DEVICE_TYPE_CPU,
		.limits = limits,
		.sparseProperties = {0}, /* Broadwell doesn't do sparse. */
	};

	strcpy(pProperties->deviceName, pdevice->pscreen->get_name(pdevice->pscreen));
	val_device_get_cache_uuid(pProperties->pipelineCacheUUID);

}

void val_GetPhysicalDeviceQueueFamilyProperties(
						VkPhysicalDevice                            physicalDevice,
						uint32_t*                                   pCount,
						VkQueueFamilyProperties*                    pQueueFamilyProperties)
{
   if (pQueueFamilyProperties == NULL) {
      *pCount = 1;
      return;
   }

   assert(*pCount >= 1);

   *pQueueFamilyProperties = (VkQueueFamilyProperties) {
      .queueFlags = VK_QUEUE_GRAPHICS_BIT |
      VK_QUEUE_COMPUTE_BIT |
                    VK_QUEUE_TRANSFER_BIT,
      .queueCount = 1,
      .timestampValidBits = 36, /* XXX: Real value here */
      .minImageTransferGranularity = (VkExtent3D) { 1, 1, 1 },
   };
}

void val_GetPhysicalDeviceMemoryProperties(
					   VkPhysicalDevice                            physicalDevice,
					   VkPhysicalDeviceMemoryProperties*           pMemoryProperties)
{
   pMemoryProperties->memoryTypeCount = 1;
   pMemoryProperties->memoryTypes[0] = (VkMemoryType) {
      .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
      VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
      .heapIndex = 0,
   };

   pMemoryProperties->memoryHeapCount = 1;
   pMemoryProperties->memoryHeaps[0] = (VkMemoryHeap) {
      .size = 1024*1024*1024,
      .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT,
   };
}

PFN_vkVoidFunction val_GetInstanceProcAddr(
					   VkInstance                                  instance,
					   const char*                                 pName)
{
	return val_lookup_entrypoint(pName);
}

/* The loader wants us to expose a second GetInstanceProcAddr function
 * to work around certain LD_PRELOAD issues seen in apps.
 */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(
								   VkInstance                                  instance,
								   const char*                                 pName);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(
								   VkInstance                                  instance,
								   const char*                                 pName)
{
	return val_GetInstanceProcAddr(instance, pName);
}

PFN_vkVoidFunction val_GetDeviceProcAddr(
					 VkDevice                                    device,
					 const char*                                 pName)
{
	return val_lookup_entrypoint(pName);
}


static VkResult
val_queue_init(struct val_device *device, struct val_queue *queue)
{
	queue->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
	queue->device = device;

	return VK_SUCCESS;
}

VkResult val_CreateDevice(
			  VkPhysicalDevice                            physicalDevice,
			  const VkDeviceCreateInfo*                   pCreateInfo,
			  const VkAllocationCallbacks*                pAllocator,
			  VkDevice*                                   pDevice)
{
   VAL_FROM_HANDLE(val_physical_device, physical_device, physicalDevice);
   struct val_device *device;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);

   for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
      bool found = false;
      for (uint32_t j = 0; j < ARRAY_SIZE(device_extensions); j++) {
         if (strcmp(pCreateInfo->ppEnabledExtensionNames[i],
                    device_extensions[j].extensionName) == 0) {
            found = true;
            break;
         }
      }
      if (!found)
         return vk_error(VK_ERROR_EXTENSION_NOT_PRESENT);
   }

   device = vk_alloc2(&physical_device->instance->alloc, pAllocator,
                       sizeof(*device), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   device->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
   device->instance = physical_device->instance;
   device->physical_device = physical_device;

   if (pAllocator)
      device->alloc = *pAllocator;
   else
      device->alloc = physical_device->instance->alloc;

   val_queue_init(device, &device->queue);

   device->pscreen = physical_device->pscreen;

   device->pctx = device->pscreen->context_create(device->pscreen,
                                                  NULL, PIPE_CONTEXT_ROBUST_BUFFER_ACCESS);

   *pDevice = val_device_to_handle(device);

   return VK_SUCCESS;

}

void val_DestroyDevice(
		       VkDevice                                    _device,
		       const VkAllocationCallbacks*                pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);

   vk_free(&device->alloc, device);
}

VkResult val_EnumerateInstanceExtensionProperties(
						  const char*                                 pLayerName,
						  uint32_t*                                   pPropertyCount,
						  VkExtensionProperties*                      pProperties)
{
   if (pProperties == NULL) {
      *pPropertyCount = ARRAY_SIZE(global_extensions);
      return VK_SUCCESS;
   }

   assert(*pPropertyCount >= ARRAY_SIZE(global_extensions));

   *pPropertyCount = ARRAY_SIZE(global_extensions);
   memcpy(pProperties, global_extensions, sizeof(global_extensions));

   return VK_SUCCESS;
}

VkResult val_EnumerateDeviceExtensionProperties(
						VkPhysicalDevice                            physicalDevice,
						const char*                                 pLayerName,
						uint32_t*                                   pPropertyCount,
						VkExtensionProperties*                      pProperties)
{
   if (pProperties == NULL) {
      *pPropertyCount = ARRAY_SIZE(device_extensions);
      return VK_SUCCESS;
   }

   assert(*pPropertyCount >= ARRAY_SIZE(device_extensions));

   *pPropertyCount = ARRAY_SIZE(device_extensions);
   memcpy(pProperties, device_extensions, sizeof(device_extensions));

   return VK_SUCCESS;
}

VkResult val_EnumerateInstanceLayerProperties(
					      uint32_t*                                   pPropertyCount,
					      VkLayerProperties*                          pProperties)
{
   if (pProperties == NULL) {
      *pPropertyCount = 0;
      return VK_SUCCESS;
   }

   /* None supported at this time */
   return vk_error(VK_ERROR_LAYER_NOT_PRESENT);
}

VkResult val_EnumerateDeviceLayerProperties(
					    VkPhysicalDevice                            physicalDevice,
					    uint32_t*                                   pPropertyCount,
					    VkLayerProperties*                          pProperties)
{
   if (pProperties == NULL) {
      *pPropertyCount = 0;
      return VK_SUCCESS;
   }

   /* None supported at this time */
   return vk_error(VK_ERROR_LAYER_NOT_PRESENT);
}

void val_GetDeviceQueue(
			VkDevice                                    _device,
			uint32_t                                    queueNodeIndex,
			uint32_t                                    queueIndex,
			VkQueue*                                    pQueue)
{
	VAL_FROM_HANDLE(val_device, device, _device);

	assert(queueIndex == 0);

	*pQueue = val_queue_to_handle(&device->queue);
}


VkResult val_QueueSubmit(
			 VkQueue                                     _queue,
			 uint32_t                                    submitCount,
			 const VkSubmitInfo*                         pSubmits,
			 VkFence                                     _fence)
{
   VAL_FROM_HANDLE(val_queue, queue, _queue);
//   VAL_FROM_HANDLE(val_fence, fence, _fence);
   struct val_device *device = queue->device;

   for (uint32_t i = 0; i < submitCount; i++) {
      for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; j++) {
         VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer,
                         pSubmits[i].pCommandBuffers[j]);
         val_execute_cmds(device, cmd_buffer);
      }
   }
   return VK_SUCCESS;
}

VkResult val_QueueWaitIdle(
			   VkQueue                                     _queue)
{
	stub_return(VK_SUCCESS);
}


VkResult val_DeviceWaitIdle(
			    VkDevice                                    _device)
{
	stub_return(VK_SUCCESS);
}


VkResult val_AllocateMemory(
			    VkDevice                                    _device,
			    const VkMemoryAllocateInfo*                 pAllocateInfo,
			    const VkAllocationCallbacks*                pAllocator,
			    VkDeviceMemory*                             pMem)
{
	val_finishme("Enter dev=%p", _device);

	VAL_FROM_HANDLE(val_device, device, _device);
	struct val_device_memory *mem;
	unsigned allocate_align = MAX2(64, util_cpu_caps.cacheline);

	assert(pAllocateInfo->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);

	if (pAllocateInfo->allocationSize == 0) {
		/* Apparently, this is allowed */
		*pMem = VK_NULL_HANDLE;
		return VK_SUCCESS;
	}

	mem = vk_alloc2(&device->alloc, pAllocator, sizeof(*mem), 8,
			    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (mem == NULL)
		return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

	uint64_t alloc_size = align_u64(pAllocateInfo->allocationSize, 4096);
	mem->pmem = align_malloc(alloc_size, allocate_align);
	if (!mem->pmem) {
		vk_free2(&device->alloc, pAllocator, mem);
		return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
	}

	mem->bo = NULL;
	mem->map = NULL;

	mem->map_size = alloc_size;

	val_buffer_template(device, &mem->template,
						mem->map_size);

	mem->type_index = pAllocateInfo->memoryTypeIndex;

	*pMem = val_device_memory_to_handle(mem);

	val_finishme("Leave dev=%p/mem=%p[%lu]", _device, mem, mem->map_size);

	return VK_SUCCESS;
}

void val_FreeMemory(
		    VkDevice                                    _device,
		    VkDeviceMemory                              _mem,
		    const VkAllocationCallbacks*                pAllocator)
{
	val_finishme("Enter dev=%p/mem=%p", _device, _mem);

	VAL_FROM_HANDLE(val_device, device, _device);
	VAL_FROM_HANDLE(val_device_memory, mem, _mem);

	if (mem == NULL)
		return;

	if (mem->bo)
		device->pscreen->resource_destroy(device->pscreen, mem->bo);

//   if (mem->bo.map)
//      val_gem_munmap(mem->bo.map, mem->bo.size);

	align_free(mem->pmem);
	vk_free2(&device->alloc, pAllocator, mem);
}

VkResult val_MapMemory(
		       VkDevice                                    _device,
		       VkDeviceMemory                              _memory,
		       VkDeviceSize                                offset,
		       VkDeviceSize                                size,
		       VkMemoryMapFlags                            flags,
		       void**                                      ppData)
{
	val_finishme("Enter dev=%p/mem=%p/offset=%lu/size=%lu", _device, _memory, offset, size);

	VAL_FROM_HANDLE(val_device_memory, mem, _memory);
	VAL_FROM_HANDLE(val_device, device, _device);

	if (mem == NULL) {
		*ppData = NULL;
		return VK_SUCCESS;
	}

	val_mmap(mem, device);

	if (!mem->map)
		return vk_error(VK_ERROR_INCOMPATIBLE_DRIVER);

	*ppData = (char*)mem->map + offset;

	stub_return(VK_SUCCESS);
}

void val_UnmapMemory(
		     VkDevice                                    _device,
		     VkDeviceMemory                              _memory)
{
	val_finishme("Enter dev=%p/mem=%p", _device, _memory);

	VAL_FROM_HANDLE(val_device_memory, mem, _memory);
	VAL_FROM_HANDLE(val_device, device, _device);

	if (mem == NULL)
		return;

	fprintf(stderr, "Size: %d\n", mem->box.width * mem->box.height * mem->box.depth);

	val_munmap(mem, device);
	stub();
}

VkResult val_FlushMappedMemoryRanges(
				     VkDevice                                    _device,
				     uint32_t                                    memoryRangeCount,
				     const VkMappedMemoryRange*                  pMemoryRanges)
{
	stub_return(VK_SUCCESS);
}

VkResult val_InvalidateMappedMemoryRanges(
					  VkDevice                                    _device,
					  uint32_t                                    memoryRangeCount,
					  const VkMappedMemoryRange*                  pMemoryRanges)
{
	stub_return(VK_SUCCESS);
}

void val_GetBufferMemoryRequirements(
				     VkDevice                                    device,
				     VkBuffer                                    _buffer,
				     VkMemoryRequirements*                       pMemoryRequirements)
{
   VAL_FROM_HANDLE(val_buffer, buffer, _buffer);

   /* The Vulkan spec (git aaed022) says:
    *
    *    memoryTypeBits is a bitfield and contains one bit set for every
    *    supported memory type for the resource. The bit `1<<i` is set if and
    *    only if the memory type `i` in the VkPhysicalDeviceMemoryProperties
    *    structure for the physical device is supported.
    *
    * We support exactly one memory type.
    */
   pMemoryRequirements->memoryTypeBits = 1;

   pMemoryRequirements->size = buffer->size;
   pMemoryRequirements->alignment = MAX2(64, util_cpu_caps.cacheline);
}

void val_GetImageMemoryRequirements(
				    VkDevice                                    device,
				    VkImage                                     _image,
				    VkMemoryRequirements*                       pMemoryRequirements)
{
   VAL_FROM_HANDLE(val_image, image, _image);
   pMemoryRequirements->memoryTypeBits = 1;

   pMemoryRequirements->size = image->size;
   pMemoryRequirements->alignment = image->alignment;
}

void val_GetImageSparseMemoryRequirements(
					  VkDevice                                    device,
					  VkImage                                     image,
					  uint32_t*                                   pSparseMemoryRequirementCount,
					  VkSparseImageMemoryRequirements*            pSparseMemoryRequirements)
{
   stub();
}

void val_GetDeviceMemoryCommitment(
    VkDevice                                    device,
    VkDeviceMemory                              memory,
    VkDeviceSize*                               pCommittedMemoryInBytes)
{
   *pCommittedMemoryInBytes = 0;
}

VkResult val_BindBufferMemory(
    VkDevice                                    _device,
    VkBuffer                                    _buffer,
    VkDeviceMemory                              _memory,
    VkDeviceSize                                memoryOffset)
{
	val_finishme("Enter dev=%p/buf=%p/mem=%p", _device, _buffer, _memory);

	VAL_FROM_HANDLE(val_device, device, _device);
	VAL_FROM_HANDLE(val_device_memory, mem, _memory);
	VAL_FROM_HANDLE(val_buffer, buffer, _buffer);

	if (mem) {
		val_finishme("%p: Offset %p[%ld] ~ %lu", mem, mem->pmem, memoryOffset, mem->map_size);
		if (mem->bo) {
			val_finishme("We already have some allocated memmory: %p, offset %lu ignored", mem->bo, memoryOffset);
			buffer->bo = mem->bo;
		}
		if (!buffer->bo) {
			val_finishme("Use failback for create resource.");
			buffer->bo = device->pscreen->resource_create(device->pscreen,
								&buffer->template);
		}
		if (!buffer->bo) {
			return vk_error(VK_ERROR_INCOMPATIBLE_DRIVER);
		}
		mem->bo = buffer->bo;
	} else {
		buffer->bo = NULL;
	}
	return VK_SUCCESS;
}

VkResult val_BindImageMemory(
    VkDevice                                    _device,
    VkImage                                     _image,
    VkDeviceMemory                              _memory,
    VkDeviceSize                                memoryOffset)
{
	val_finishme("Enter dev=%p/image=%p/mem=%p", _device, _image, _memory);

	VAL_FROM_HANDLE(val_device, device, _device);
	VAL_FROM_HANDLE(val_device_memory, mem, _memory);
	VAL_FROM_HANDLE(val_image, image, _image);

	if (mem) {
		val_finishme("Offset %p[%ld]", mem->pmem, memoryOffset);
		if (mem->bo) {
			val_finishme("We already have some allocated memmory: %p", mem->bo);
			image->bo = mem->bo;
		}
		if (!image->bo) {
			image->bo = device->pscreen->resource_create(device->pscreen,
							    &image->template);
		}
		if (!image->bo) {
			return vk_error(VK_ERROR_INCOMPATIBLE_DRIVER);
		}
		mem->bo = image->bo;
	} else {
		image->bo = NULL;
	}
	return VK_SUCCESS;
}

VkResult val_QueueBindSparse(
    VkQueue                                     queue,
    uint32_t                                    bindInfoCount,
    const VkBindSparseInfo*                     pBindInfo,
    VkFence                                     fence)
{
   stub_return(VK_ERROR_INCOMPATIBLE_DRIVER);
}


VkResult val_CreateFence(
    VkDevice                                    _device,
    const VkFenceCreateInfo*                    pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkFence*                                    pFence)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   struct val_fence *fence;

   fence = vk_alloc2(&device->alloc, pAllocator, sizeof(*fence), 8,
                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (fence == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   *pFence = val_fence_to_handle(fence);

   return VK_SUCCESS;
}

void val_DestroyFence(
    VkDevice                                    _device,
    VkFence                                     _fence,
    const VkAllocationCallbacks*                pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   vk_free2(&device->alloc, pAllocator, val_fence_from_handle(_fence));
}

VkResult val_ResetFences(
    VkDevice                                    _device,
    uint32_t                                    fenceCount,
    const VkFence*                              pFences)
{
	stub_return(VK_SUCCESS);
}

VkResult val_GetFenceStatus(
    VkDevice                                    _device,
    VkFence                                     _fence)
{
	stub_return(VK_SUCCESS);
}


void
val_buffer_template(struct val_device* device, struct pipe_resource *template, VkDeviceSize size)
{
	memset(template, 0, sizeof(*template));
	template->screen = device->pscreen;
	template->target = PIPE_BUFFER;
	template->format = PIPE_FORMAT_R8_UNORM;
	template->width0 = size;
	template->height0 = 1;
	template->depth0 = 1;
	template->array_size = 1;
}

// Buffer functions
VkResult val_CreateBuffer(
    VkDevice                                    _device,
    const VkBufferCreateInfo*                   pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkBuffer*                                   pBuffer)
{
	val_finishme("Enter dev=%p", _device);

	VAL_FROM_HANDLE(val_device, device, _device);
	struct val_buffer *buffer;

	assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);

	buffer = vk_alloc2(&device->alloc, pAllocator, sizeof(*buffer), 8,
					   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (buffer == NULL)
		return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

	buffer->size = pCreateInfo->size;
	buffer->usage = pCreateInfo->usage;
	buffer->offset = 0;

	buffer->bo = NULL;
	val_buffer_template(device, &buffer->template,
						buffer->size);
	buffer->total_size = val_texture_size(&buffer->template);
	*pBuffer = val_buffer_to_handle(buffer);

	val_finishme("Leave dev=%p/buffer=%p[%lu]", _device, buffer, buffer->size);

	return VK_SUCCESS;
}

void val_DestroyBuffer(
    VkDevice                                    _device,
    VkBuffer                                    _buffer,
    const VkAllocationCallbacks*                pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_buffer, buffer, _buffer);

   vk_free2(&device->alloc, pAllocator, buffer);
}

VkResult val_CreateFramebuffer(
    VkDevice                                    _device,
    const VkFramebufferCreateInfo*              pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkFramebuffer*                              pFramebuffer)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   struct val_framebuffer *framebuffer;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);

   size_t size = sizeof(*framebuffer) +
                 sizeof(struct val_image_view *) * pCreateInfo->attachmentCount;
   framebuffer = vk_alloc2(&device->alloc, pAllocator, size, 8,
                            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (framebuffer == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   framebuffer->attachment_count = pCreateInfo->attachmentCount;
   for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
      VkImageView _iview = pCreateInfo->pAttachments[i];
      framebuffer->attachments[i] = val_image_view_from_handle(_iview);
   }

   framebuffer->width = pCreateInfo->width;
   framebuffer->height = pCreateInfo->height;
   framebuffer->layers = pCreateInfo->layers;

   *pFramebuffer = val_framebuffer_to_handle(framebuffer);

   return VK_SUCCESS;
}

void val_DestroyFramebuffer(
    VkDevice                                    _device,
    VkFramebuffer                               _fb,
    const VkAllocationCallbacks*                pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_framebuffer, fb, _fb);

   vk_free2(&device->alloc, pAllocator, fb);
}


VkResult val_WaitForFences(
    VkDevice                                    _device,
    uint32_t                                    fenceCount,
    const VkFence*                              pFences,
    VkBool32                                    waitAll,
    uint64_t                                    timeout)
{
	stub_return(VK_SUCCESS);
}

VkResult val_CreateSemaphore(
    VkDevice                                    device,
    const VkSemaphoreCreateInfo*                pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkSemaphore*                                pSemaphore)
{
   /* The DRM execbuffer ioctl always execute in-oder, even between different
    * rings. As such, there's nothing to do for the user space semaphore.
    */

   *pSemaphore = (VkSemaphore)1;

	stub_return(VK_SUCCESS);
}

void val_DestroySemaphore(
    VkDevice                                    device,
    VkSemaphore                                 semaphore,
    const VkAllocationCallbacks*                pAllocator)
{
	stub();
}


VkResult val_CreateEvent(
    VkDevice                                    _device,
    const VkEventCreateInfo*                    pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkEvent*                                    pEvent)
{
	stub_return(VK_SUCCESS);
}


void val_DestroyEvent(
    VkDevice                                    _device,
    VkEvent                                     _event,
    const VkAllocationCallbacks*                pAllocator)
{
	stub();
}


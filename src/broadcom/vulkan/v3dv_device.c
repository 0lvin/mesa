/*
 * Copyright © 2019 Raspberry Pi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <xf86drm.h>

#include "v3dv_private.h"

#include "common/v3d_debug.h"
#include "compiler/glsl_types.h"
#include "drm-uapi/v3d_drm.h"
#include "vk_util.h"

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

VkResult
v3dv_EnumerateInstanceExtensionProperties(const char *pLayerName,
                                          uint32_t *pPropertyCount,
                                          VkExtensionProperties *pProperties)
{
   /* We don't support any layers  */
   if (pLayerName)
      return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);

   VK_OUTARRAY_MAKE(out, pProperties, pPropertyCount);

   for (int i = 0; i < V3DV_INSTANCE_EXTENSION_COUNT; i++) {
      if (v3dv_instance_extensions_supported.extensions[i]) {
         vk_outarray_append(&out, prop) {
            *prop = v3dv_instance_extensions[i];
         }
      }
   }

   return vk_outarray_status(&out);
}

VkResult
v3dv_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkInstance *pInstance)
{
   struct v3dv_instance *instance;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);

   struct v3dv_instance_extension_table enabled_extensions = {};
   for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
      int idx;
      for (idx = 0; idx < V3DV_INSTANCE_EXTENSION_COUNT; idx++) {
         if (strcmp(pCreateInfo->ppEnabledExtensionNames[i],
                    v3dv_instance_extensions[idx].extensionName) == 0)
            break;
      }

      if (idx >= V3DV_INSTANCE_EXTENSION_COUNT)
         return vk_error(NULL, VK_ERROR_EXTENSION_NOT_PRESENT);

      if (!v3dv_instance_extensions_supported.extensions[idx])
         return vk_error(NULL, VK_ERROR_EXTENSION_NOT_PRESENT);

      enabled_extensions.extensions[idx] = true;
   }

   instance = vk_alloc2(&default_alloc, pAllocator, sizeof(*instance), 8,
                        VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!instance)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   instance->_loader_data.loaderMagic = ICD_LOADER_MAGIC;

   if (pAllocator)
      instance->alloc = *pAllocator;
   else
      instance->alloc = default_alloc;

   v3d_process_debug_variable();

   instance->app_info = (struct v3dv_app_info) { .api_version = 0 };
   if (pCreateInfo->pApplicationInfo) {
      const VkApplicationInfo *app = pCreateInfo->pApplicationInfo;

      instance->app_info.app_name =
         vk_strdup(&instance->alloc, app->pApplicationName,
                   VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
      instance->app_info.app_version = app->applicationVersion;

      instance->app_info.engine_name =
         vk_strdup(&instance->alloc, app->pEngineName,
                   VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
      instance->app_info.engine_version = app->engineVersion;

      instance->app_info.api_version = app->apiVersion;
   }

   if (instance->app_info.api_version == 0)
      instance->app_info.api_version = VK_API_VERSION_1_0;

   instance->enabled_extensions = enabled_extensions;

   for (unsigned i = 0; i < ARRAY_SIZE(instance->dispatch.entrypoints); i++) {
      /* Vulkan requires that entrypoints for extensions which have not been
       * enabled must not be advertised.
       */
      if (!v3dv_instance_entrypoint_is_enabled(i,
                                              instance->app_info.api_version,
                                              &instance->enabled_extensions)) {
         instance->dispatch.entrypoints[i] = NULL;
      } else {
         instance->dispatch.entrypoints[i] =
            v3dv_instance_dispatch_table.entrypoints[i];
      }
   }

   struct v3dv_physical_device *pdevice = &instance->physicalDevice;
   for (unsigned i = 0; i < ARRAY_SIZE(pdevice->dispatch.entrypoints); i++) {
      /* Vulkan requires that entrypoints for extensions which have not been
       * enabled must not be advertised.
       */
      if (!v3dv_physical_device_entrypoint_is_enabled(i,
                                                     instance->app_info.api_version,
                                                     &instance->enabled_extensions)) {
         pdevice->dispatch.entrypoints[i] = NULL;
      } else {
         pdevice->dispatch.entrypoints[i] =
            v3dv_physical_device_dispatch_table.entrypoints[i];
      }
   }

   for (unsigned i = 0; i < ARRAY_SIZE(instance->device_dispatch.entrypoints); i++) {
      /* Vulkan requires that entrypoints for extensions which have not been
       * enabled must not be advertised.
       */
      if (!v3dv_device_entrypoint_is_enabled(i,
                                            instance->app_info.api_version,
                                            &instance->enabled_extensions,
                                            NULL)) {
         instance->device_dispatch.entrypoints[i] = NULL;
      } else {
         instance->device_dispatch.entrypoints[i] =
            v3dv_device_dispatch_table.entrypoints[i];
      }
   }

   instance->physicalDeviceCount = -1;

   result = vk_debug_report_instance_init(&instance->debug_report_callbacks);
   if (result != VK_SUCCESS) {
      vk_free2(&default_alloc, pAllocator, instance);
      return vk_error(NULL, result);
   }

   glsl_type_singleton_init_or_ref();

   VG(VALGRIND_CREATE_MEMPOOL(instance, 0, false));

   *pInstance = v3dv_instance_to_handle(instance);

   return VK_SUCCESS;
}

static void
physical_device_finish(struct v3dv_physical_device *device)
{
   close(device->local_fd);
   if (device->master_fd >= 0)
      close(device->master_fd);

   free(device->name);

#if using_v3d_simulator
   v3d_simulator_destroy(device->sim_file);
#endif
}

void
v3dv_DestroyInstance(VkInstance _instance,
                     const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_instance, instance, _instance);

   if (!instance)
      return;

   if (instance->physicalDeviceCount > 0) {
      /* We support at most one physical device. */
      assert(instance->physicalDeviceCount == 1);
      physical_device_finish(&instance->physicalDevice);
   }

   vk_free(&instance->alloc, (char *)instance->app_info.app_name);
   vk_free(&instance->alloc, (char *)instance->app_info.engine_name);

   VG(VALGRIND_DESTROY_MEMPOOL(instance));

   vk_debug_report_instance_destroy(&instance->debug_report_callbacks);

   glsl_type_singleton_decref();

   vk_free(&instance->alloc, instance);
}

static uint64_t
compute_heap_size()
{
   /* Query the total ram from the system */
   struct sysinfo info;
   sysinfo(&info);

   uint64_t total_ram = (uint64_t)info.totalram * (uint64_t)info.mem_unit;

   /* We don't want to burn too much ram with the GPU.  If the user has 4GiB
    * or less, we use at most half.  If they have more than 4GiB, we use 3/4.
    */
   uint64_t available_ram;
   if (total_ram <= 4ull * 1024ull * 1024ull * 1024ull)
      available_ram = total_ram / 2;
   else
      available_ram = total_ram * 3 / 4;

   return available_ram;
}

static VkResult
physical_device_init(struct v3dv_physical_device *device,
                     struct v3dv_instance *instance,
                     drmDevicePtr drm_device)
{
   VkResult result = VK_SUCCESS;
   int32_t master_fd = -1;

   const char *path = drm_device->nodes[DRM_NODE_RENDER];
   int32_t fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0)
      return vk_error(instance, VK_ERROR_INCOMPATIBLE_DRIVER);

   device->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
   device->instance = instance;

   assert(strlen(path) < ARRAY_SIZE(device->path));
   snprintf(device->path, ARRAY_SIZE(device->path), "%s", path);

   /* FIXME: we will have to do plenty more here */
   device->local_fd = fd;
   device->master_fd = master_fd;

   uint8_t zeroes[VK_UUID_SIZE] = { 0 };
   memcpy(device->pipeline_cache_uuid, zeroes, VK_UUID_SIZE);

#if using_v3d_simulator
   device->sim_file = v3d_simulator_init(device->local_fd);
#endif

   if (!v3d_get_device_info(device->local_fd, &device->devinfo, &v3dv_ioctl)) {
      result = VK_ERROR_INCOMPATIBLE_DRIVER;
      goto fail;
   }

   asprintf(&device->name, "V3D %d.%d",
            device->devinfo.ver / 10, device->devinfo.ver % 10);

   /* Setup available memory heaps and types */
   VkPhysicalDeviceMemoryProperties *mem = &device->memory;
   mem->memoryHeapCount = 1;
   mem->memoryHeaps[0].size = compute_heap_size();
   mem->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

   mem->memoryTypeCount = 2;

   /* This is the only combination required by the spec */
   mem->memoryTypes[0].propertyFlags =
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   mem->memoryTypes[0].heapIndex = 0;

   mem->memoryTypes[1].propertyFlags =
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
   mem->memoryTypes[1].heapIndex = 0;

fail:
   close(fd);
   if (master_fd != -1)
      close(master_fd);

   return result;
}

static VkResult
enumerate_devices(struct v3dv_instance *instance)
{
   /* TODO: Check for more devices? */
   drmDevicePtr devices[8];
   VkResult result = VK_ERROR_INCOMPATIBLE_DRIVER;
   int max_devices;

   instance->physicalDeviceCount = 0;

   max_devices = drmGetDevices2(0, devices, ARRAY_SIZE(devices));
   if (max_devices < 1)
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   for (unsigned i = 0; i < (unsigned)max_devices; i++) {
      if (devices[i]->available_nodes & 1 << DRM_NODE_RENDER &&
          devices[i]->bustype == DRM_BUS_PCI &&
          /* FIXME: so we can run past this point for now */
          (true || devices[i]->deviceinfo.pci->vendor_id == 0x14E4)) {
         result = physical_device_init(&instance->physicalDevice,
                                       instance, devices[i]);
         if (result != VK_ERROR_INCOMPATIBLE_DRIVER)
            break;
      }
   }
   drmFreeDevices(devices, max_devices);

   if (result == VK_SUCCESS)
      instance->physicalDeviceCount = 1;

   return result;
}

static VkResult
instance_ensure_physical_device(struct v3dv_instance *instance)
{
   if (instance->physicalDeviceCount < 0) {
      VkResult result = enumerate_devices(instance);
      if (result != VK_SUCCESS &&
          result != VK_ERROR_INCOMPATIBLE_DRIVER)
         return result;
   }

   return VK_SUCCESS;
}

VkResult
v3dv_EnumeratePhysicalDevices(VkInstance _instance,
                              uint32_t *pPhysicalDeviceCount,
                              VkPhysicalDevice *pPhysicalDevices)
{
   V3DV_FROM_HANDLE(v3dv_instance, instance, _instance);
   VK_OUTARRAY_MAKE(out, pPhysicalDevices, pPhysicalDeviceCount);
 
   VkResult result = instance_ensure_physical_device(instance);
   if (result != VK_SUCCESS)
      return result;

   if (instance->physicalDeviceCount == 0)
      return VK_SUCCESS;

   assert(instance->physicalDeviceCount == 1);
   vk_outarray_append(&out, i) {
      *i = v3dv_physical_device_to_handle(&instance->physicalDevice);
   }

   return vk_outarray_status(&out);
}

void
v3dv_GetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                               VkPhysicalDeviceFeatures *pFeatures)
{
   memset(pFeatures, 0, sizeof(*pFeatures));

   *pFeatures = (VkPhysicalDeviceFeatures) {
      .robustBufferAccess = false,
      .fullDrawIndexUint32 = false,
      .imageCubeArray = false,
      .independentBlend = false,
      .geometryShader = false,
      .tessellationShader = false,
      .sampleRateShading = false,
      .dualSrcBlend = false,
      .logicOp = false,
      .multiDrawIndirect = false,
      .drawIndirectFirstInstance = false,
      .depthClamp = false,
      .depthBiasClamp = false,
      .fillModeNonSolid = false,
      .depthBounds = false,
      .wideLines = false,
      .largePoints = false,
      .alphaToOne = false,
      .multiViewport = false,
      .samplerAnisotropy = false,
      .textureCompressionETC2 = false,
      .textureCompressionASTC_LDR = false,
      .textureCompressionBC = false,
      .occlusionQueryPrecise = false,
      .pipelineStatisticsQuery = false,
      .vertexPipelineStoresAndAtomics = false,
      .fragmentStoresAndAtomics = false,
      .shaderTessellationAndGeometryPointSize = false,
      .shaderImageGatherExtended = false,
      .shaderStorageImageExtendedFormats = false,
      .shaderStorageImageMultisample = false,
      .shaderStorageImageReadWithoutFormat = false,
      .shaderStorageImageWriteWithoutFormat = false,
      .shaderUniformBufferArrayDynamicIndexing = false,
      .shaderSampledImageArrayDynamicIndexing = false,
      .shaderStorageBufferArrayDynamicIndexing = false,
      .shaderStorageImageArrayDynamicIndexing = false,
      .shaderClipDistance = false,
      .shaderCullDistance = false,
      .shaderFloat64 = false,
      .shaderInt64 = false,
      .shaderInt16 = false,
      .shaderResourceResidency = false,
      .shaderResourceMinLod = false,
      .sparseBinding = false,
      .sparseResidencyBuffer = false,
      .sparseResidencyImage2D = false,
      .sparseResidencyImage3D = false,
      .sparseResidency2Samples = false,
      .sparseResidency4Samples = false,
      .sparseResidency8Samples = false,
      .sparseResidency16Samples = false,
      .sparseResidencyAliased = false,
      .variableMultisampleRate = false,
      .inheritedQueries = false,
   };
}

void
v3dv_GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                 VkPhysicalDeviceProperties *pProperties)
{
   V3DV_FROM_HANDLE(v3dv_physical_device, pdevice, physicalDevice);

   const uint32_t page_size = 4096;
   const uint32_t mem_size = compute_heap_size();

   /* Per-stage limits */
   const uint32_t max_samplers = 16;
   const uint32_t max_uniform_buffers = 12;
   const uint32_t max_storage_buffers = 4;
   const uint32_t max_sampled_images = 16;
   const uint32_t max_storage_images = 4;

   const uint32_t max_vertex_attributes = 16;
   const uint32_t max_varying_components = 16 * 4;
   const uint32_t max_render_targets = 4;

   const uint32_t v3d_coord_shift = 6;
   const uint32_t v3d_coord_scale = (1 << v3d_coord_shift);
   const float point_size_granularity = 2.0f / v3d_coord_scale;

   const uint32_t max_fb_size = 4096;

   const VkSampleCountFlags supported_sample_counts = VK_SAMPLE_COUNT_1_BIT;

   /* FIXME: this will probably require an in-depth review */
   VkPhysicalDeviceLimits limits = {
      .maxImageDimension1D                      = 4096,
      .maxImageDimension2D                      = 4096,
      .maxImageDimension3D                      = 4096,
      .maxImageDimensionCube                    = 4096,
      .maxImageArrayLayers                      = 2048,
      .maxTexelBufferElements                   = (1ul << 28),
      .maxUniformBufferRange                    = (1ul << 27) - 1,
      .maxStorageBufferRange                    = (1ul << 27) - 1,
      .maxPushConstantsSize                     = (1ul << 27) - 1,
      .maxMemoryAllocationCount                 = mem_size / page_size,
      .maxSamplerAllocationCount                = 64 * 1024,
      .bufferImageGranularity                   = 256, /* A cache line */
      .sparseAddressSpaceSize                   = 0,
      .maxBoundDescriptorSets                   = 16,
      .maxPerStageDescriptorSamplers            = max_samplers,
      .maxPerStageDescriptorUniformBuffers      = max_uniform_buffers,
      .maxPerStageDescriptorStorageBuffers      = max_storage_buffers,
      .maxPerStageDescriptorSampledImages       = max_sampled_images,
      .maxPerStageDescriptorStorageImages       = max_storage_images,
      .maxPerStageDescriptorInputAttachments    = 4,
      .maxPerStageResources                     = 128,

      /* We multiply some limits by 6 to account for all shader stages */
      .maxDescriptorSetSamplers                 = 6 * max_samplers,
      .maxDescriptorSetUniformBuffers           = 6 * max_uniform_buffers,
      .maxDescriptorSetUniformBuffersDynamic    = 8,
      .maxDescriptorSetStorageBuffers           = 6 * max_storage_buffers,
      .maxDescriptorSetStorageBuffersDynamic    = 4,
      .maxDescriptorSetSampledImages            = 6 * max_sampled_images,
      .maxDescriptorSetStorageImages            = 6 * max_storage_images,
      .maxDescriptorSetInputAttachments         = 4,

      /* Vertex limits */
      .maxVertexInputAttributes                 = max_vertex_attributes,
      .maxVertexInputBindings                   = max_vertex_attributes,
      .maxVertexInputAttributeOffset            = 0xffffffff,
      .maxVertexInputBindingStride              = 0xffffffff,
      .maxVertexOutputComponents                = max_varying_components,

      /* Tessellation limits */
      .maxTessellationGenerationLevel           = 0,
      .maxTessellationPatchSize                 = 0,
      .maxTessellationControlPerVertexInputComponents = 0,
      .maxTessellationControlPerVertexOutputComponents = 0,
      .maxTessellationControlPerPatchOutputComponents = 0,
      .maxTessellationControlTotalOutputComponents = 0,
      .maxTessellationEvaluationInputComponents = 0,
      .maxTessellationEvaluationOutputComponents = 0,

      /* Geometry limits */
      .maxGeometryShaderInvocations             = 0,
      .maxGeometryInputComponents               = 0,
      .maxGeometryOutputComponents              = 0,
      .maxGeometryOutputVertices                = 0,
      .maxGeometryTotalOutputComponents         = 0,

      /* Fragment limits */
      .maxFragmentInputComponents               = max_varying_components,
      .maxFragmentOutputAttachments             = 4,
      .maxFragmentDualSrcAttachments            = 0,
      .maxFragmentCombinedOutputResources       = max_render_targets +
                                                  max_storage_buffers +
                                                  max_storage_images,

      /* Compute limits */
      .maxComputeSharedMemorySize               = 16384,
      .maxComputeWorkGroupCount                 = { 65535, 65535, 65535 },
      .maxComputeWorkGroupInvocations           = 256,
      .maxComputeWorkGroupSize                  = { 256, 256, 256 },

      .subPixelPrecisionBits                    = v3d_coord_shift,
      .subTexelPrecisionBits                    = 8,
      .mipmapPrecisionBits                      = 8,
      .maxDrawIndexedIndexValue                 = 0x00ffffff,
      .maxDrawIndirectCount                     = 0x7fffffff,
      .maxSamplerLodBias                        = 14.0f,
      .maxSamplerAnisotropy                     = 16.0f,
      .maxViewports                             = 1,
      .maxViewportDimensions                    = { max_fb_size, max_fb_size },
      .viewportBoundsRange                      = { -2.0 * max_fb_size,
                                                    2.0 * max_fb_size - 1 },
      .viewportSubPixelBits                     = 0,
      .minMemoryMapAlignment                    = page_size,
      .minTexelBufferOffsetAlignment            = 16,
      .minUniformBufferOffsetAlignment          = 256,
      .minStorageBufferOffsetAlignment          = 256,
      .minTexelOffset                           = -8,
      .maxTexelOffset                           = 7,
      .minTexelGatherOffset                     = -8,
      .maxTexelGatherOffset                     = 7,
      .minInterpolationOffset                   = -0.5,
      .maxInterpolationOffset                   = 0.5,
      .subPixelInterpolationOffsetBits          = v3d_coord_shift,
      .maxFramebufferWidth                      = max_fb_size,
      .maxFramebufferHeight                     = max_fb_size,
      .maxFramebufferLayers                     = 256,
      .framebufferColorSampleCounts             = supported_sample_counts,
      .framebufferDepthSampleCounts             = supported_sample_counts,
      .framebufferStencilSampleCounts           = supported_sample_counts,
      .framebufferNoAttachmentsSampleCounts     = supported_sample_counts,
      .maxColorAttachments                      = max_render_targets,
      .sampledImageColorSampleCounts            = supported_sample_counts,
      .sampledImageIntegerSampleCounts          = supported_sample_counts,
      .sampledImageDepthSampleCounts            = supported_sample_counts,
      .sampledImageStencilSampleCounts          = supported_sample_counts,
      .storageImageSampleCounts                 = VK_SAMPLE_COUNT_1_BIT,
      .maxSampleMaskWords                       = 1,
      .timestampComputeAndGraphics              = false,
      .timestampPeriod                          = 0.0f,
      .maxClipDistances                         = 0,
      .maxCullDistances                         = 0,
      .maxCombinedClipAndCullDistances          = 0,
      .discreteQueuePriorities                  = 2,
      .pointSizeRange                           = { point_size_granularity,
                                                    512.0f },
      .lineWidthRange                           = { 1.0f, 1.0f },
      .pointSizeGranularity                     = point_size_granularity,
      .lineWidthGranularity                     = 0.0f,
      .strictLines                              = true,
      .standardSampleLocations                  = false,
      .optimalBufferCopyOffsetAlignment         = 32,
      .optimalBufferCopyRowPitchAlignment       = 32,
      .nonCoherentAtomSize                      = 256,
   };

   /* FIXME:
    * Getting deviceID and UUID will probably require to use the kernel pci
    * interface. See this:
    * https://www.kernel.org/doc/html/latest/PCI/pci.html#how-to-find-pci-devices-manually
    * And check the getparam ioctl in the i915 kernel with CHIPSET_ID for
    * example.
    */
   *pProperties = (VkPhysicalDeviceProperties) {
      .apiVersion = v3dv_physical_device_api_version(pdevice),
      .driverVersion = vk_get_driver_version(),
      .vendorID = 0x14E4,
      .deviceID = 0, /* FIXME */
      .deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
      .limits = limits,
      .sparseProperties = { 0 },
   };

   snprintf(pProperties->deviceName, sizeof(pProperties->deviceName),
            "%s", pdevice->name);
   memcpy(pProperties->pipelineCacheUUID,
          pdevice->pipeline_cache_uuid, VK_UUID_SIZE);
}

/* We support exactly one queue family. */
static const VkQueueFamilyProperties
v3dv_queue_family_properties = {
   .queueFlags = VK_QUEUE_GRAPHICS_BIT |
                 VK_QUEUE_COMPUTE_BIT |
                 VK_QUEUE_TRANSFER_BIT,
   .queueCount = 1,
   .timestampValidBits = 0, /* FIXME */
   .minImageTransferGranularity = { 1, 1, 1 },
};

void
v3dv_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice,
                                            uint32_t *pCount,
                                            VkQueueFamilyProperties *pQueueFamilyProperties)
{
   VK_OUTARRAY_MAKE(out, pQueueFamilyProperties, pCount);

   vk_outarray_append(&out, p) {
      *p = v3dv_queue_family_properties;
   }
}

void
v3dv_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
                                       VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
   V3DV_FROM_HANDLE(v3dv_physical_device, device, physicalDevice);
   *pMemoryProperties = device->memory;
}


PFN_vkVoidFunction
v3dv_GetInstanceProcAddr(VkInstance _instance,
                         const char *pName)
{
   V3DV_FROM_HANDLE(v3dv_instance, instance, _instance);

   /* The Vulkan 1.0 spec for vkGetInstanceProcAddr has a table of exactly
    * when we have to return valid function pointers, NULL, or it's left
    * undefined.  See the table for exact details.
    */
   if (pName == NULL)
      return NULL;

#define LOOKUP_V3DV_ENTRYPOINT(entrypoint)              \
   if (strcmp(pName, "vk" #entrypoint) == 0)            \
      return (PFN_vkVoidFunction)v3dv_##entrypoint

   LOOKUP_V3DV_ENTRYPOINT(EnumerateInstanceExtensionProperties);
   LOOKUP_V3DV_ENTRYPOINT(CreateInstance);

#undef LOOKUP_V3DV_ENTRYPOINT

   if (instance == NULL)
      return NULL;

   int idx = v3dv_get_instance_entrypoint_index(pName);
   if (idx >= 0)
      return instance->dispatch.entrypoints[idx];

   idx = v3dv_get_physical_device_entrypoint_index(pName);
   if (idx >= 0)
      return instance->physicalDevice.dispatch.entrypoints[idx];

   idx = v3dv_get_device_entrypoint_index(pName);
   if (idx >= 0)
      return instance->device_dispatch.entrypoints[idx];

   return NULL;
}

/* With version 1+ of the loader interface the ICD should expose
 * vk_icdGetInstanceProcAddr to work around certain LD_PRELOAD issues seen in apps.
 */
PUBLIC
VKAPI_ATTR PFN_vkVoidFunction
VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                     const char *pName);

PUBLIC
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance,
                          const char*                                 pName)
{
   return v3dv_GetInstanceProcAddr(instance, pName);
}

PFN_vkVoidFunction
v3dv_GetDeviceProcAddr(VkDevice _device,
                       const char *pName)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);

   if (!device || !pName)
      return NULL;

   int idx = v3dv_get_device_entrypoint_index(pName);
   if (idx < 0)
      return NULL;

   return device->dispatch.entrypoints[idx];
}

/* With version 4+ of the loader interface the ICD should expose
 * vk_icdGetPhysicalDeviceProcAddr()
 */
PUBLIC
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance  _instance,
                                const char* pName);

PFN_vkVoidFunction
vk_icdGetPhysicalDeviceProcAddr(VkInstance  _instance,
                                const char* pName)
{
   V3DV_FROM_HANDLE(v3dv_instance, instance, _instance);

   if (!pName || !instance)
      return NULL;

   int idx = v3dv_get_physical_device_entrypoint_index(pName);
   if (idx < 0)
      return NULL;

   return instance->physicalDevice.dispatch.entrypoints[idx];
}

VkResult
v3dv_EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                        const char *pLayerName,
                                        uint32_t *pPropertyCount,
                                        VkExtensionProperties *pProperties)
{
   /* We don't support any layers */
   if (pLayerName)
      return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);

   V3DV_FROM_HANDLE(v3dv_physical_device, device, physicalDevice);
   VK_OUTARRAY_MAKE(out, pProperties, pPropertyCount);

   for (int i = 0; i < V3DV_DEVICE_EXTENSION_COUNT; i++) {
      if (device->supported_extensions.extensions[i]) {
         vk_outarray_append(&out, prop) {
            *prop = v3dv_device_extensions[i];
         }
      }
   }

   return vk_outarray_status(&out);
}

VkResult
v3dv_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                      VkLayerProperties *pProperties)
{
   if (pProperties == NULL) {
      *pPropertyCount = 0;
      return VK_SUCCESS;
   }

   return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);
}

VkResult
v3dv_EnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
                                    uint32_t *pPropertyCount,
                                    VkLayerProperties *pProperties)
{
   V3DV_FROM_HANDLE(v3dv_physical_device, physical_device, physicalDevice);

   if (pProperties == NULL) {
      *pPropertyCount = 0;
      return VK_SUCCESS;
   }

   return vk_error(physical_device->instance, VK_ERROR_LAYER_NOT_PRESENT);
}

static VkResult
queue_init(struct v3dv_device *device, struct v3dv_queue *queue)
{
   queue->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
   queue->device = device;
   queue->flags = 0;
   return VK_SUCCESS;
}

static void
queue_finish(struct v3dv_queue *queue)
{
}

static void
init_device_dispatch(struct v3dv_device *device)
{
   for (unsigned i = 0; i < ARRAY_SIZE(device->dispatch.entrypoints); i++) {
      /* Vulkan requires that entrypoints for extensions which have not been
       * enabled must not be advertised.
       */
      if (!v3dv_device_entrypoint_is_enabled(i, device->instance->app_info.api_version,
                                             &device->instance->enabled_extensions,
                                             &device->enabled_extensions)) {
         device->dispatch.entrypoints[i] = NULL;
      } else {
         device->dispatch.entrypoints[i] =
            v3dv_device_dispatch_table.entrypoints[i];
      }
   }
}

VkResult
v3dv_CreateDevice(VkPhysicalDevice physicalDevice,
                  const VkDeviceCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator,
                  VkDevice *pDevice)
{
   V3DV_FROM_HANDLE(v3dv_physical_device, physical_device, physicalDevice);
   struct v3dv_instance *instance = physical_device->instance;
   VkResult result;
   struct v3dv_device *device;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);

   /* Check enabled extensions */
   struct v3dv_device_extension_table enabled_extensions = { };
   for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
      int idx;
      for (idx = 0; idx < V3DV_DEVICE_EXTENSION_COUNT; idx++) {
         if (strcmp(pCreateInfo->ppEnabledExtensionNames[i],
                    v3dv_device_extensions[idx].extensionName) == 0)
            break;
      }

      if (idx >= V3DV_DEVICE_EXTENSION_COUNT)
         return vk_error(instance, VK_ERROR_EXTENSION_NOT_PRESENT);

      if (!physical_device->supported_extensions.extensions[idx])
         return vk_error(instance, VK_ERROR_EXTENSION_NOT_PRESENT);

      enabled_extensions.extensions[idx] = true;
   }

   /* Check enabled features */
   if (pCreateInfo->pEnabledFeatures) {
      VkPhysicalDeviceFeatures supported_features;
      v3dv_GetPhysicalDeviceFeatures(physicalDevice, &supported_features);
      VkBool32 *supported_feature = (VkBool32 *)&supported_features;
      VkBool32 *enabled_feature = (VkBool32 *)pCreateInfo->pEnabledFeatures;
      unsigned num_features = sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32);
      for (uint32_t i = 0; i < num_features; i++) {
         if (enabled_feature[i] && !supported_feature[i])
            return vk_error(instance, VK_ERROR_FEATURE_NOT_PRESENT);
      }
   }

   /* Check requested queues (we only expose one queue ) */
   assert(pCreateInfo->queueCreateInfoCount == 1);
   for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
      assert(pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex == 0);
      assert(pCreateInfo->pQueueCreateInfos[i].queueCount == 1);
      if (pCreateInfo->pQueueCreateInfos[i].flags != 0)
         return vk_error(instance, VK_ERROR_INITIALIZATION_FAILED);
   }

   device = vk_alloc2(&physical_device->instance->alloc, pAllocator,
                       sizeof(*device), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   device->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
   device->instance = instance;

   if (pAllocator)
      device->alloc = *pAllocator;
   else
      device->alloc = physical_device->instance->alloc;

   device->fd = open(physical_device->path, O_RDWR | O_CLOEXEC);
   if (device->fd == -1) {
      result = vk_error(instance, VK_ERROR_INITIALIZATION_FAILED);
      goto fail_device;
   }

   result = queue_init(device, &device->queue);
   if (result != VK_SUCCESS)
      goto fail_fd;

   device->devinfo = physical_device->devinfo;
   device->enabled_extensions = enabled_extensions;

   init_device_dispatch(device);

   *pDevice = v3dv_device_to_handle(device);

   return VK_SUCCESS;

 fail_fd:
   close(device->fd);
 fail_device:
   vk_free(&device->alloc, device);

   return result;
}

void
v3dv_DestroyDevice(VkDevice _device,
                   const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);

   queue_finish(&device->queue);
}

void
v3dv_GetDeviceQueue(VkDevice _device,
                    uint32_t queueFamilyIndex,
                    uint32_t queueIndex,
                    VkQueue *pQueue)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);

   assert(queueIndex == 0);
   assert(queueFamilyIndex == 0);

   *pQueue = v3dv_queue_to_handle(&device->queue);
}

VkResult
v3dv_DeviceWaitIdle(VkDevice _device)
{
   /* FIXME: stub */
   return VK_SUCCESS;
}

VkResult
v3dv_CreateDebugReportCallbackEXT(VkInstance _instance,
                                 const VkDebugReportCallbackCreateInfoEXT* pCreateInfo,
                                 const VkAllocationCallbacks* pAllocator,
                                 VkDebugReportCallbackEXT* pCallback)
{
   V3DV_FROM_HANDLE(v3dv_instance, instance, _instance);
   return vk_create_debug_report_callback(&instance->debug_report_callbacks,
                                          pCreateInfo, pAllocator, &instance->alloc,
                                          pCallback);
}

void
v3dv_DestroyDebugReportCallbackEXT(VkInstance _instance,
                                  VkDebugReportCallbackEXT _callback,
                                  const VkAllocationCallbacks* pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_instance, instance, _instance);
   vk_destroy_debug_report_callback(&instance->debug_report_callbacks,
                                    _callback, pAllocator, &instance->alloc);
}

static VkResult
device_alloc(struct v3dv_device *device,
             struct v3dv_device_memory *mem,
             VkDeviceSize size)
{
   /* FIXME: implement a BO cache like in v3d */

   /* Our kernel interface is 32-bit */
   assert((size & 0xffffffff) == size);

   const uint32_t page_align = 4096; /* Always allocate full pages */
   size = align(size, page_align);
   struct drm_v3d_create_bo create = {
      .size = size
   };

   int ret = v3dv_ioctl(device->fd, DRM_IOCTL_V3D_CREATE_BO, &create);
   if (ret != 0)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   assert(create.offset % page_align == 0);
   assert((create.offset & 0xffffffff) == create.offset);

   mem->handle = create.handle;
   mem->size = size;
   mem->offset = create.offset;
   mem->map = NULL;
   mem->map_size = 0;

   return VK_SUCCESS;
}

static void
device_free(struct v3dv_device *device, struct v3dv_device_memory *mem)
{
   struct drm_gem_close c;
   memset(&c, 0, sizeof(c));
   c.handle = mem->handle;
   int ret = v3dv_ioctl(device->fd, DRM_IOCTL_GEM_CLOSE, &c);
   if (ret != 0)
      fprintf(stderr, "close object %d: %s\n", mem->handle, strerror(errno));
}

static VkResult
device_map_unsynchronized(struct v3dv_device *device,
                          struct v3dv_device_memory *mem,
                          uint32_t size)
{
   assert(mem != NULL && size < mem->size);

   /* From the spec:
    *
    *   "After a successful call to vkMapMemory the memory object memory is
    *   considered to be currently host mapped. It is an application error to
    *   call vkMapMemory on a memory object that is already host mapped."
    */
   assert(mem->map = NULL);

   struct drm_v3d_mmap_bo map;
   memset(&map, 0, sizeof(map));
   map.handle = mem->handle;
   int ret = v3dv_ioctl(device->fd, DRM_IOCTL_V3D_MMAP_BO, &map);
   if (ret != 0) {
      fprintf(stderr, "map ioctl failure\n");
      return VK_ERROR_MEMORY_MAP_FAILED;
   }

   mem->map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   device->fd, map.offset);
   if (mem->map == MAP_FAILED) {
      fprintf(stderr, "mmap of bo %d (offset 0x%016llx, size %d) failed\n",
              mem->handle, (long long)map.offset, (uint32_t)mem->size);
      return VK_ERROR_MEMORY_MAP_FAILED;
   }
   VG(VALGRIND_MALLOCLIKE_BLOCK(mem->map, mem->size, 0, false));

   mem->map_size = size;

   return VK_SUCCESS;
}

static bool
device_memory_wait(struct v3dv_device *device,
                   struct v3dv_device_memory *mem,
                   uint64_t timeout_ns)
{
   struct drm_v3d_wait_bo wait = {
      .handle = mem->handle,
      .timeout_ns = timeout_ns,
   };
   int ret = v3dv_ioctl(device->fd, DRM_IOCTL_V3D_WAIT_BO, &wait);
   if (ret == -1)
      return -errno;
   else
      return 0;
}

static VkResult
device_map(struct v3dv_device *device,
           struct v3dv_device_memory *mem,
           uint32_t size)
{
   VkResult result = device_map_unsynchronized(device, mem, size);
   if (result != VK_SUCCESS)
      return result;

   const uint64_t infinite = 0xffffffffffffffffull;
   bool ok = device_memory_wait(device, mem, infinite);
   if (!ok) {
      fprintf(stderr, "memory wait for map failed\n");
      return VK_ERROR_MEMORY_MAP_FAILED;
   }

   return VK_SUCCESS;
}

static void
device_unmap(struct v3dv_device *device, struct v3dv_device_memory *mem)
{
   assert(mem->map && mem->map_size > 0);

   munmap(mem->map, mem->map_size);
   VG(VALGRIND_FREELIKE_BLOCK(mem->map, 0));
   mem->map = NULL;
   mem->map_size = 0;

   struct drm_gem_close c;
   memset(&c, 0, sizeof(c));
   c.handle = mem->handle;
   int ret = v3dv_ioctl(device->fd, DRM_IOCTL_GEM_CLOSE, &c);
   if (ret != 0)
      fprintf(stderr, "close object %d: %s\n", mem->handle, strerror(errno));
}

VkResult
v3dv_AllocateMemory(VkDevice _device,
                    const VkMemoryAllocateInfo *pAllocateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkDeviceMemory *pMem)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   struct v3dv_device_memory *mem;
   struct v3dv_physical_device *pdevice = &device->instance->physicalDevice;

   assert(pAllocateInfo->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);

   /* The Vulkan 1.0.33 spec says "allocationSize must be greater than 0". */
   assert(pAllocateInfo->allocationSize > 0);

   mem = vk_alloc2(&device->alloc, pAllocator, sizeof(*mem), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (mem == NULL)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   assert(pAllocateInfo->memoryTypeIndex < pdevice->memory.memoryTypeCount);
   mem->type = &pdevice->memory.memoryTypes[pAllocateInfo->memoryTypeIndex];

   VkResult result = device_alloc(device, mem, pAllocateInfo->allocationSize);

   *pMem = v3dv_device_memory_to_handle(mem);
   return result;
}

void
v3dv_FreeMemory(VkDevice _device,
                VkDeviceMemory _mem,
                const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_device_memory, mem, _mem);

   if (mem == NULL)
      return;

   if (mem->map)
      v3dv_UnmapMemory(_device, _mem);

   device_free(device, mem);

   vk_free2(&device->alloc, pAllocator, mem);
}

VkResult
v3dv_MapMemory(VkDevice _device,
               VkDeviceMemory _memory,
               VkDeviceSize offset,
               VkDeviceSize size,
               VkMemoryMapFlags flags,
               void **ppData)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_device_memory, mem, _memory);

   if (mem == NULL) {
      *ppData = NULL;
      return VK_SUCCESS;
   }

   assert(offset < mem->size);

   /* We always map from the beginning of the region, so if our offset
    * is not 0 and we are not mapping the entire region, we need to
    * add the offset to the map size.
    */
   if (size == VK_WHOLE_SIZE)
      size = mem->size;
   else if (offset > 0)
      size += offset;

   VkResult result = device_map(device, mem, size);
   if (result != VK_SUCCESS)
      return vk_error(device->instance, result);

   *ppData = ((uint8_t *) mem->map) + offset;
   return VK_SUCCESS;
}

void
v3dv_UnmapMemory(VkDevice _device,
                 VkDeviceMemory _memory)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_device_memory, mem, _memory);

   if (mem == NULL)
      return;

   device_unmap(device, mem);
}

VkResult
v3dv_FlushMappedMemoryRanges(VkDevice _device,
                             uint32_t memoryRangeCount,
                             const VkMappedMemoryRange *pMemoryRanges)
{
   /* FIXME: stub (although note that both radv and tu just returns success
    * here. Pending further research)
    */
   return VK_SUCCESS;
}

VkResult
v3dv_InvalidateMappedMemoryRanges(VkDevice _device,
                                  uint32_t memoryRangeCount,
                                  const VkMappedMemoryRange *pMemoryRanges)
{
   /* FIXME: stub (although note that both radv and tu just returns success
    * here. Pending further research)
    */
   return VK_SUCCESS;
}

void
v3dv_GetImageMemoryRequirements(VkDevice _device,
                                VkImage _image,
                                VkMemoryRequirements *pMemoryRequirements)
{
   V3DV_FROM_HANDLE(v3dv_image, image, _image);

   assert(image->size > 0);

   pMemoryRequirements->size = image->size;
   pMemoryRequirements->alignment = image->alignment;
   pMemoryRequirements->memoryTypeBits = 0x3; /* Both memory types */
}

VkResult
v3dv_BindImageMemory(VkDevice _device,
                     VkImage _image,
                     VkDeviceMemory _memory,
                     VkDeviceSize memoryOffset)
{
   V3DV_FROM_HANDLE(v3dv_device_memory, mem, _memory);
   V3DV_FROM_HANDLE(v3dv_image, image, _image);

   /* Valid usage:
    *
    *   "memoryOffset must be an integer multiple of the alignment member of
    *    the VkMemoryRequirements structure returned from a call to
    *    vkGetImageMemoryRequirements with image"
    */
   assert(memoryOffset % image->alignment == 0);
   assert(memoryOffset < mem->size);

   image->mem = mem;
   image->mem_offset = memoryOffset;

   return VK_SUCCESS;
}

void
v3dv_GetBufferMemoryRequirements(VkDevice _device,
                                 VkBuffer _buffer,
                                 VkMemoryRequirements* pMemoryRequirements)
{
   V3DV_FROM_HANDLE(v3dv_buffer, buffer, _buffer);

   pMemoryRequirements->memoryTypeBits = 0x3; /* Both memory types */
   pMemoryRequirements->alignment = buffer->alignment;
   pMemoryRequirements->size =
      align64(buffer->size, pMemoryRequirements->alignment);
}

VkResult
v3dv_CreateBuffer(VkDevice  _device,
                  const VkBufferCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator,
                  VkBuffer *pBuffer)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   struct v3dv_buffer *buffer;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
   assert(pCreateInfo->usage != 0);

   /* We don't support any flags for now */
   assert(pCreateInfo->flags == 0);

   buffer = vk_alloc2(&device->alloc, pAllocator, sizeof(*buffer), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (buffer == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   buffer->size = pCreateInfo->size;
   buffer->usage = pCreateInfo->usage;
   buffer->alignment = 256; /* nonCoherentAtomSize */

   assert((buffer->size & 0xffffffff) == buffer->size);

   *pBuffer = v3dv_buffer_to_handle(buffer);

   return VK_SUCCESS;
}

void
v3dv_DestroyBuffer(VkDevice _device,
                   VkBuffer _buffer,
                   const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_buffer, buffer, _buffer);

   if (!buffer)
      return;

   vk_free2(&device->alloc, pAllocator, buffer);
}

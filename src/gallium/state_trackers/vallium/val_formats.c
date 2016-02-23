#include "val_private.h"
#include "util/u_format.h"
#define COMMON_NAME(x) [VK_FORMAT_##x] = PIPE_FORMAT_##x
static enum pipe_format format_to_vk_table[VK_FORMAT_END_RANGE] = {
   COMMON_NAME(R8_UNORM),
   COMMON_NAME(R8_SNORM),
   COMMON_NAME(R8G8B8A8_UNORM),
   COMMON_NAME(B8G8R8A8_UNORM),
   COMMON_NAME(R8G8B8A8_SRGB),
   COMMON_NAME(B8G8R8A8_SRGB),
   [VK_FORMAT_D16_UNORM] = PIPE_FORMAT_Z16_UNORM,
   [VK_FORMAT_D24_UNORM_S8_UINT] = PIPE_FORMAT_Z24_UNORM_S8_UINT,
   [VK_FORMAT_R32G32B32_SFLOAT] = PIPE_FORMAT_R32G32B32_FLOAT,
   [VK_FORMAT_R32G32B32A32_SFLOAT] = PIPE_FORMAT_R32G32B32A32_FLOAT,
};

enum pipe_format vk_format_to_pipe(VkFormat format)
{
   return format_to_vk_table[format];
}

void val_GetPhysicalDeviceFormatProperties(
    VkPhysicalDevice                            physicalDevice,
    VkFormat                                    format,
    VkFormatProperties*                         pFormatProperties)
{
   VAL_FROM_HANDLE(val_physical_device, physical_device, physicalDevice);
   enum pipe_format pformat = vk_format_to_pipe(format);
   int features = 0;
   if (pformat == PIPE_FORMAT_NONE) {
     pFormatProperties->linearTilingFeatures = 0;
     pFormatProperties->optimalTilingFeatures = 0;
     return;
   }

   if (physical_device->pscreen->is_format_supported(physical_device->pscreen, pformat,
                                             PIPE_TEXTURE_2D, 0, PIPE_BIND_DEPTH_STENCIL)) {
      pFormatProperties->linearTilingFeatures = 0;
      pFormatProperties->optimalTilingFeatures = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
      return;
   }

   if (physical_device->pscreen->is_format_supported(physical_device->pscreen, pformat,
                                             PIPE_BUFFER, 0, PIPE_BIND_VERTEX_BUFFER)) {
      features |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
   }

   if (physical_device->pscreen->is_format_supported(physical_device->pscreen, pformat,
                                             PIPE_TEXTURE_2D, 0, PIPE_BIND_SAMPLER_VIEW)) {
      features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
   }

   if (physical_device->pscreen->is_format_supported(physical_device->pscreen, pformat,
                                             PIPE_TEXTURE_2D, 0, PIPE_BIND_RENDER_TARGET)) {
      features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
      features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
   }

   pFormatProperties->linearTilingFeatures = features;
   pFormatProperties->optimalTilingFeatures = features;
   return VK_SUCCESS;
}

VkResult val_GetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice                            physicalDevice,
    VkFormat                                    format,
    VkImageType                                 type,
    VkImageTiling                               tiling,
    VkImageUsageFlags                           usage,
    VkImageCreateFlags                          createFlags,
    VkImageFormatProperties*                    pImageFormatProperties)
{
     VAL_FROM_HANDLE(val_physical_device, physical_device, physicalDevice);
}

void val_GetPhysicalDeviceSparseImageFormatProperties(
    VkPhysicalDevice                            physicalDevice,
    VkFormat                                    format,
    VkImageType                                 type,
    uint32_t                                    samples,
    VkImageUsageFlags                           usage,
    VkImageTiling                               tiling,
    uint32_t*                                   pNumProperties,
    VkSparseImageFormatProperties*              pProperties)
{
   /* Sparse images are not yet supported. */
   *pNumProperties = 0;
}

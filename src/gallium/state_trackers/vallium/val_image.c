#include "val_private.h"
#include "util/u_format.h"
#include "pipe/p_state.h"
VkResult
val_image_create(VkDevice _device,
                 const struct val_image_create_info *create_info,
                 const VkAllocationCallbacks* alloc,
                 VkImage *pImage)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   const VkImageCreateInfo *pCreateInfo = create_info->vk_info;
   struct val_image *image;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);

   image = vk_alloc2(&device->alloc, alloc, sizeof(*image), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (image == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   memset(image, 0, sizeof(*image));

   {
      memset(&image->template, 0, sizeof(image->template));

      image->template.screen = device->pscreen;
      image->template.target = PIPE_TEXTURE_2D;
      image->template.format = vk_format_to_pipe(pCreateInfo->format);
      image->template.width0 = pCreateInfo->extent.width;
      image->template.height0 = pCreateInfo->extent.height;
      image->template.depth0 = pCreateInfo->extent.depth;
      image->template.array_size = pCreateInfo->arrayLayers;
      image->template.last_level = pCreateInfo->mipLevels - 1;
      if (image->template.array_size > 1)
         image->template.target = PIPE_TEXTURE_2D_ARRAY;
      if (create_info->bind_flags)
         image->template.bind = create_info->bind_flags;
      image->bo = device->pscreen->resource_create_unbacked(device->pscreen,
                                                            &image->template,
                                                            &image->size);
   }
   *pImage = val_image_to_handle(image);

   return VK_SUCCESS;
}

VkResult
val_CreateImage(VkDevice device,
                const VkImageCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator,
                VkImage *pImage)
{
   return val_image_create(device,
      &(struct val_image_create_info) {
         .vk_info = pCreateInfo,
         .bind_flags = 0,
      },
      pAllocator,
      pImage);
}

void
val_DestroyImage(VkDevice _device, VkImage _image,
                 const VkAllocationCallbacks *pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);

   vk_free2(&device->alloc, pAllocator, val_image_from_handle(_image));
}


VkResult
val_CreateImageView(VkDevice _device,
                    const VkImageViewCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkImageView *pView)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_image, image, pCreateInfo->image);
   struct val_image_view *view;

   view = vk_alloc2(&device->alloc, pAllocator, sizeof(*view), 8,
                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (view == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   view->view_type = pCreateInfo->viewType;
   view->format = pCreateInfo->format;
   view->components = pCreateInfo->components;
   view->subresourceRange = pCreateInfo->subresourceRange;
   view->image = image;
   view->surface = NULL;
   *pView = val_image_view_to_handle(view);

   return VK_SUCCESS;
}

void
val_DestroyImageView(VkDevice _device, VkImageView _iview,
                     const VkAllocationCallbacks *pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_image_view, iview, _iview);

   vk_free2(&device->alloc, pAllocator, iview);
}

void val_GetImageSubresourceLayout(
    VkDevice                                    device,
    VkImage                                     _image,
    const VkImageSubresource*                   pSubresource,
    VkSubresourceLayout*                        pLayout)
{
   VAL_FROM_HANDLE(val_image, image, _image);

   pLayout->offset = 0;
   pLayout->rowPitch = util_format_get_stride(image->bo->format, image->bo->width0);
   pLayout->arrayPitch = 0;
   pLayout->size = image->size;
   switch (pSubresource->aspectMask) {
   case VK_IMAGE_ASPECT_COLOR_BIT:
      break;
   case VK_IMAGE_ASPECT_DEPTH_BIT:
      break;
   case VK_IMAGE_ASPECT_STENCIL_BIT:
      break;
   default:
      assert(!"Invalid image aspect");
   }
}

VkResult
val_CreateBufferView(VkDevice _device,
                     const VkBufferViewCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkBufferView *pView)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   struct val_buffer_view *view;
   view = vk_alloc2(&device->alloc, pAllocator, sizeof(*view), 8,
                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!view)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   *pView = val_buffer_view_to_handle(view);

   return VK_SUCCESS;
}

void
val_DestroyBufferView(VkDevice _device, VkBufferView bufferView,
                      const VkAllocationCallbacks *pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_buffer_view, view, bufferView);

   vk_free2(&device->alloc, pAllocator, view);
}

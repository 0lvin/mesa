#include "val_private.h"

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

   image = val_alloc2(&device->alloc, alloc, sizeof(*image), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (image == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   memset(image, 0, sizeof(*image));

   {
      struct pipe_resource template;

      memset(&template, 0, sizeof(template));
      
      template.screen = device->pscreen;
      template.target = PIPE_TEXTURE_2D;
      template.format = vk_format_to_pipe(pCreateInfo->format);
      template.width0 = pCreateInfo->extent.width;
      template.height0 = pCreateInfo->extent.height;
      template.depth0 = pCreateInfo->extent.depth;
      template.array_size = 1;
      if (create_info->bind_flags)
         template.bind = create_info->bind_flags;
      image->bo = device->pscreen->resource_create_unbacked(device->pscreen,
                                                            &template,
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

   val_free2(&device->alloc, pAllocator, val_image_from_handle(_image));
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

   view = val_alloc2(&device->alloc, pAllocator, sizeof(*view), 8,
                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (view == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   view->view_type = pCreateInfo->viewType;
   view->format = pCreateInfo->format;
   view->components = pCreateInfo->components;
   view->subresourceRange = pCreateInfo->subresourceRange;
   view->image = image;
   *pView = val_image_view_to_handle(view);

   return VK_SUCCESS;
}

void
val_DestroyImageView(VkDevice _device, VkImageView _iview,
                     const VkAllocationCallbacks *pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_image_view, iview, _iview);

   val_free2(&device->alloc, pAllocator, iview);
}

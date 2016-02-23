#include "val_private.h"

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

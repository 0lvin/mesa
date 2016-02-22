/*
 * Copyright © 2015 Intel Corporation
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

#include "val_wsi.h"

VkResult
val_init_wsi(struct val_instance *instance)
{
   VkResult result;

   result = val_x11_init_wsi(instance);
   if (result != VK_SUCCESS)
      return result;

#ifdef HAVE_WAYLAND_PLATFORM
   result = val_wl_init_wsi(instance);
   if (result != VK_SUCCESS) {
      val_x11_finish_wsi(instance);
      return result;
   }
#endif

   return VK_SUCCESS;
}

void
val_finish_wsi(struct val_instance *instance)
{
#ifdef HAVE_WAYLAND_PLATFORM
   val_wl_finish_wsi(instance);
#endif
   val_x11_finish_wsi(instance);
}

void val_DestroySurfaceKHR(
    VkInstance                                   _instance,
    VkSurfaceKHR                                 _surface,
    const VkAllocationCallbacks*                 pAllocator)
{
   VAL_FROM_HANDLE(val_instance, instance, _instance);
   VAL_FROM_HANDLE(_VkIcdSurfaceBase, surface, _surface);

   val_free2(&instance->alloc, pAllocator, surface);
}

VkResult val_GetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice                            physicalDevice,
    uint32_t                                    queueFamilyIndex,
    VkSurfaceKHR                                _surface,
    VkBool32*                                   pSupported)
{
   VAL_FROM_HANDLE(val_physical_device, device, physicalDevice);
   VAL_FROM_HANDLE(_VkIcdSurfaceBase, surface, _surface);
   struct val_wsi_interface *iface = device->instance->wsi[surface->platform];

   return iface->get_support(surface, device, queueFamilyIndex, pSupported);
}

VkResult val_GetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice                            physicalDevice,
    VkSurfaceKHR                                _surface,
    VkSurfaceCapabilitiesKHR*                   pSurfaceCapabilities)
{
   VAL_FROM_HANDLE(val_physical_device, device, physicalDevice);
   VAL_FROM_HANDLE(_VkIcdSurfaceBase, surface, _surface);
   struct val_wsi_interface *iface = device->instance->wsi[surface->platform];

   return iface->get_capabilities(surface, device, pSurfaceCapabilities);
}

VkResult val_GetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice                            physicalDevice,
    VkSurfaceKHR                                _surface,
    uint32_t*                                   pSurfaceFormatCount,
    VkSurfaceFormatKHR*                         pSurfaceFormats)
{
   VAL_FROM_HANDLE(val_physical_device, device, physicalDevice);
   VAL_FROM_HANDLE(_VkIcdSurfaceBase, surface, _surface);
   struct val_wsi_interface *iface = device->instance->wsi[surface->platform];

   return iface->get_formats(surface, device, pSurfaceFormatCount,
                             pSurfaceFormats);
}

VkResult val_GetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice                            physicalDevice,
    VkSurfaceKHR                                _surface,
    uint32_t*                                   pPresentModeCount,
    VkPresentModeKHR*                           pPresentModes)
{
   VAL_FROM_HANDLE(val_physical_device, device, physicalDevice);
   VAL_FROM_HANDLE(_VkIcdSurfaceBase, surface, _surface);
   struct val_wsi_interface *iface = device->instance->wsi[surface->platform];

   return iface->get_present_modes(surface, device, pPresentModeCount,
                                   pPresentModes);
}

VkResult val_CreateSwapchainKHR(
    VkDevice                                     _device,
    const VkSwapchainCreateInfoKHR*              pCreateInfo,
    const VkAllocationCallbacks*                 pAllocator,
    VkSwapchainKHR*                              pSwapchain)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(_VkIcdSurfaceBase, surface, pCreateInfo->surface);
   struct val_wsi_interface *iface = device->instance->wsi[surface->platform];
   struct val_swapchain *swapchain;

   VkResult result = iface->create_swapchain(surface, device, pCreateInfo,
                                             pAllocator, &swapchain);
   if (result != VK_SUCCESS)
      return result;

   *pSwapchain = val_swapchain_to_handle(swapchain);

   return VK_SUCCESS;
}

void val_DestroySwapchainKHR(
    VkDevice                                     device,
    VkSwapchainKHR                               _swapchain,
    const VkAllocationCallbacks*                 pAllocator)
{
   VAL_FROM_HANDLE(val_swapchain, swapchain, _swapchain);

   swapchain->destroy(swapchain, pAllocator);
}

VkResult val_GetSwapchainImagesKHR(
    VkDevice                                     device,
    VkSwapchainKHR                               _swapchain,
    uint32_t*                                    pSwapchainImageCount,
    VkImage*                                     pSwapchainImages)
{
   VAL_FROM_HANDLE(val_swapchain, swapchain, _swapchain);

   return swapchain->get_images(swapchain, pSwapchainImageCount,
                                pSwapchainImages);
}

VkResult val_AcquireNextImageKHR(
    VkDevice                                     device,
    VkSwapchainKHR                               _swapchain,
    uint64_t                                     timeout,
    VkSemaphore                                  semaphore,
    VkFence                                      fence,
    uint32_t*                                    pImageIndex)
{
   VAL_FROM_HANDLE(val_swapchain, swapchain, _swapchain);

   return swapchain->acquire_next_image(swapchain, timeout, semaphore,
                                        pImageIndex);
}

VkResult val_QueuePresentKHR(
    VkQueue                                  _queue,
    const VkPresentInfoKHR*                  pPresentInfo)
{
   VAL_FROM_HANDLE(val_queue, queue, _queue);
   VkResult result;

   for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
      VAL_FROM_HANDLE(val_swapchain, swapchain, pPresentInfo->pSwapchains[i]);

      assert(swapchain->device == queue->device);

      result = swapchain->queue_present(swapchain, queue,
                                        pPresentInfo->pImageIndices[i]);
      /* TODO: What if one of them returns OUT_OF_DATE? */
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

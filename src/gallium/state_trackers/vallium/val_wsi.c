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

static const struct wsi_callbacks wsi_cbs = {
   .get_phys_device_format_properties = val_GetPhysicalDeviceFormatProperties,
};

VkResult
val_init_wsi(struct val_physical_device *physical_device)
{
	VkResult result;

	memset(physical_device->wsi_device.wsi, 0, sizeof(physical_device->wsi_device.wsi));
#ifdef VK_USE_PLATFORM_XCB_KHR
	result = val_x11_init_wsi(&physical_device->wsi_device, &physical_device->instance->alloc);
	if (result != VK_SUCCESS)
		return vk_error(result);
#endif

#ifdef HAVE_WAYLAND_PLATFORM
	result = val_wl_init_wsi(&physical_device->wsi_device, &physical_device->instance->alloc,
				 val_physical_device_to_handle(physical_device),
				 &wsi_cbs);
	if (result != VK_SUCCESS) {
#ifdef VK_USE_PLATFORM_XCB_KHR
		wsi_x11_finish_wsi(&physical_device->wsi_device, &physical_device->instance->alloc);
#endif
		return vk_error(result);
	}
#endif

   return VK_SUCCESS;
}

void
val_finish_wsi(struct val_physical_device *physical_device)
{
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
	wsi_wl_finish_wsi(&physical_device->wsi_device, &physical_device->instance->alloc);
#endif

#ifdef VK_USE_PLATFORM_XCB_KHR
	wsi_x11_finish_wsi(&physical_device->wsi_device, &physical_device->instance->alloc);
#endif
}

void val_DestroySurfaceKHR(
    VkInstance                                   _instance,
    VkSurfaceKHR                                 _surface,
    const VkAllocationCallbacks*                 pAllocator)
{
   VAL_FROM_HANDLE(val_instance, instance, _instance);
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);

   vk_free2(&instance->alloc, pAllocator, surface);
}

VkResult val_GetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice                            physicalDevice,
    uint32_t                                    queueFamilyIndex,
    VkSurfaceKHR                                _surface,
    VkBool32*                                   pSupported)
{
   VAL_FROM_HANDLE(val_physical_device, device, physicalDevice);
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
   struct wsi_interface *iface = device->wsi_device.wsi[surface->platform];

   return iface->get_support(surface, &device->wsi_device,
                             &device->instance->alloc,
                             queueFamilyIndex, pSupported);
}

VkResult val_GetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice                            physicalDevice,
    VkSurfaceKHR                                _surface,
    VkSurfaceCapabilitiesKHR*                   pSurfaceCapabilities)
{
   VAL_FROM_HANDLE(val_physical_device, device, physicalDevice);
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
   struct wsi_interface *iface = device->wsi_device.wsi[surface->platform];

   return iface->get_capabilities(surface, pSurfaceCapabilities);
}

VkResult val_GetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice                            physicalDevice,
    VkSurfaceKHR                                _surface,
    uint32_t*                                   pSurfaceFormatCount,
    VkSurfaceFormatKHR*                         pSurfaceFormats)
{
   VAL_FROM_HANDLE(val_physical_device, device, physicalDevice);
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
   struct wsi_interface *iface = device->wsi_device.wsi[surface->platform];

   return iface->get_formats(surface, &device->wsi_device, pSurfaceFormatCount,
                             pSurfaceFormats);
}

VkResult val_GetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice                            physicalDevice,
    VkSurfaceKHR                                _surface,
    uint32_t*                                   pPresentModeCount,
    VkPresentModeKHR*                           pPresentModes)
{
   VAL_FROM_HANDLE(val_physical_device, device, physicalDevice);
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
   struct wsi_interface *iface = device->wsi_device.wsi[surface->platform];

   return iface->get_present_modes(surface, pPresentModeCount,
                                   pPresentModes);
}

static const struct wsi_image_fns val_wsi_image_fns = {
   .create_wsi_image = NULL,
   .free_wsi_image = NULL,
};

VkResult val_CreateSwapchainKHR(
    VkDevice                                     _device,
    const VkSwapchainCreateInfoKHR*              pCreateInfo,
    const VkAllocationCallbacks*                 pAllocator,
    VkSwapchainKHR*                              pSwapchain)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, pCreateInfo->surface);
   struct wsi_interface *iface = device->physical_device->wsi_device.wsi[surface->platform];
   struct wsi_swapchain *swapchain;
	const VkAllocationCallbacks *alloc;
	if (pAllocator)
		alloc = pAllocator;
	else
		alloc = &device->alloc;
	VkResult result = iface->create_swapchain(surface, _device,
						  &device->physical_device->wsi_device,
						  pCreateInfo,
						  alloc, &val_wsi_image_fns,
						  &swapchain);

   if (result != VK_SUCCESS)
      return vk_error(result);

   *pSwapchain = wsi_swapchain_to_handle(swapchain);

   return VK_SUCCESS;
}

void val_DestroySwapchainKHR(
    VkDevice                                     _device,
    VkSwapchainKHR                               _swapchain,
    const VkAllocationCallbacks*                 pAllocator)
{
	VAL_FROM_HANDLE(val_device, device, _device);
	VAL_FROM_HANDLE(wsi_swapchain, swapchain, _swapchain);
	const VkAllocationCallbacks *alloc;

	if (!_swapchain)
		return;

	if (pAllocator)
		alloc = pAllocator;
	else
		alloc = &device->alloc;

   swapchain->destroy(swapchain, alloc);
}

VkResult val_GetSwapchainImagesKHR(
    VkDevice                                     device,
    VkSwapchainKHR                               _swapchain,
    uint32_t*                                    pSwapchainImageCount,
    VkImage*                                     pSwapchainImages)
{
   VAL_FROM_HANDLE(wsi_swapchain, swapchain, _swapchain);

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
   VAL_FROM_HANDLE(wsi_swapchain, swapchain, _swapchain);

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
      VAL_FROM_HANDLE(wsi_swapchain, swapchain, pPresentInfo->pSwapchains[i]);

      assert(val_device_from_handle(swapchain->device) == queue->device);

      result = swapchain->queue_present(swapchain, pPresentInfo->pImageIndices[i]);
      /* TODO: What if one of them returns OUT_OF_DATE? */
      if (result != VK_SUCCESS)
         return vk_error(result);
   }

   return VK_SUCCESS;
}

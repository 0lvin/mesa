#include "val_private.h"

VkResult val_CreateQueryPool(
    VkDevice                                    _device,
    const VkQueryPoolCreateInfo*                pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkQueryPool*                                pQueryPool)
{
	val_finishme("Implement %s", __func__);
	return VK_SUCCESS;
}

void val_DestroyQueryPool(
    VkDevice                                    _device,
    VkQueryPool                                 _pool,
    const VkAllocationCallbacks*                pAllocator)
{
	val_finishme("Implement %s", __func__);
}

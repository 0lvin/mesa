#include "val_private.h"

VkResult val_CreateQueryPool(
    VkDevice                                    _device,
    const VkQueryPoolCreateInfo*                pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkQueryPool*                                pQueryPool)
{
	val_finishme("%s: Not implemented.", __func__);
	return VK_SUCCESS;
}

void val_DestroyQueryPool(
    VkDevice                                    _device,
    VkQueryPool                                 _pool,
    const VkAllocationCallbacks*                pAllocator)
{
	val_finishme("%s: Not implemented.", __func__);
}

void val_CmdResetQueryPool(
	VkCommandBuffer                             commandBuffer,
	VkQueryPool                                 queryPool,
	uint32_t                                    firstQuery,
	uint32_t                                    queryCount)
{
	val_finishme("%s: Not implemented.", __func__);
}

void val_CmdBeginQuery(
    VkCommandBuffer                             commandBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    query,
    VkQueryControlFlags                         flags)
{
	val_finishme("%s: Not implemented.", __func__);
}

void val_CmdEndQuery(
    VkCommandBuffer                             commandBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    query)
{
	val_finishme("%s: Not implemented.", __func__);
}

void val_CmdCopyQueryPoolResults(
    VkCommandBuffer                             commandBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    firstQuery,
    uint32_t                                    queryCount,
    VkBuffer                                    dstBuffer,
    VkDeviceSize                                dstOffset,
    VkDeviceSize                                stride,
    VkQueryResultFlags                          flags)
{
	val_finishme("%s: Not implemented.", __func__);
}

VkResult val_GetQueryPoolResults(
	VkDevice                                    _device,
	VkQueryPool                                 queryPool,
	uint32_t                                    firstQuery,
	uint32_t                                    queryCount,
	size_t                                      dataSize,
	void*                                       pData,
	VkDeviceSize                                stride,
	VkQueryResultFlags                          flags)
{
	val_finishme("%s: Not implemented.", __func__);
	return VK_NOT_READY;
}

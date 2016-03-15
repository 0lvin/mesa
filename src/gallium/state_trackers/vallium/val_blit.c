#include "val_private.h"

void val_CmdCopyBufferToImage(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    srcBuffer,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                    pRegions)
{

}
void val_CmdCopyImageToBuffer(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkBuffer                                    destBuffer,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                    pRegions)
{


}

void val_CmdCopyImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout,
    uint32_t                                    regionCount,
    const VkImageCopy*                          pRegions)
{

}


void val_CmdCopyBuffer(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    srcBuffer,
    VkBuffer                                    destBuffer,
    uint32_t                                    regionCount,
    const VkBufferCopy*                         pRegions)
{

}

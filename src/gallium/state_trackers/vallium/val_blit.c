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
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_image, src_image, srcImage);
   VAL_FROM_HANDLE(val_buffer, dst_buffer, destBuffer);
   struct val_cmd_buffer_entry *cmd;

   int i;
   cmd = val_alloc(&cmd_buffer->pool->alloc,
                             sizeof(*cmd),
                             8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmd)
      return;

   cmd->cmd_type = VAL_CMD_COPY_IMAGE_TO_BUFFER;

   cmd->u.img_to_buffer.src = src_image;
   cmd->u.img_to_buffer.dst = dst_buffer;
   cmd->u.img_to_buffer.src_layout = srcImageLayout;
   cmd->u.img_to_buffer.region_count = regionCount;

   {
      VkBufferImageCopy *regions;

      regions = malloc(regionCount * sizeof(VkBufferImageCopy));
      memcpy(regions, pRegions, regionCount * sizeof(VkBufferImageCopy));
      cmd->u.img_to_buffer.regions = regions;
   }
   list_addtail(&cmd->cmd_link, &cmd_buffer->cmds);
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
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_image, src_image, srcImage);
   VAL_FROM_HANDLE(val_image, dest_image, destImage);
   struct val_cmd_buffer_entry *cmd;

   int i;
   cmd = val_alloc(&cmd_buffer->pool->alloc,
		   sizeof(*cmd),
		   8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmd)
      return;

   cmd->cmd_type = VAL_CMD_COPY_IMAGE;

   cmd->u.copy_image.src = src_image;
   cmd->u.copy_image.dst = dest_image;
   cmd->u.copy_image.src_layout = srcImageLayout;
   cmd->u.copy_image.dst_layout = destImageLayout;
   cmd->u.copy_image.region_count = regionCount;

   {
      VkImageCopy *regions;

      regions = malloc(regionCount * sizeof(VkImageCopy));
      memcpy(regions, pRegions, regionCount * sizeof(VkImageCopy));
      cmd->u.copy_image.regions = regions;
   }
   list_addtail(&cmd->cmd_link, &cmd_buffer->cmds);
}


void val_CmdCopyBuffer(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    srcBuffer,
    VkBuffer                                    destBuffer,
    uint32_t                                    regionCount,
    const VkBufferCopy*                         pRegions)
{

}

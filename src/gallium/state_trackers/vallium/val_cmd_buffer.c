#include "val_private.h"

static VkResult val_create_cmd_buffer(
    struct val_device *                         device,
    struct val_cmd_pool *                       pool,
    VkCommandBufferLevel                        level,
    VkCommandBuffer*                            pCommandBuffer)
{
   struct val_cmd_buffer *cmd_buffer;
   VkResult result;

   cmd_buffer = val_alloc(&pool->alloc, sizeof(*cmd_buffer), 8,
                          VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (cmd_buffer == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   cmd_buffer->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
   cmd_buffer->device = device;
   cmd_buffer->pool = pool;

   list_inithead(&cmd_buffer->cmds);
   if (pool) {
      list_addtail(&cmd_buffer->pool_link, &pool->cmd_buffers);
   } else {
      /* Init the pool_link so we can safefly call list_del when we destroy
       * the command buffer
       */
      list_inithead(&cmd_buffer->pool_link);
   }
   *pCommandBuffer = val_cmd_buffer_to_handle(cmd_buffer);

   return VK_SUCCESS;
}

VkResult val_AllocateCommandBuffers(
    VkDevice                                    _device,
    const VkCommandBufferAllocateInfo*          pAllocateInfo,
    VkCommandBuffer*                            pCommandBuffers)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_cmd_pool, pool, pAllocateInfo->commandPool);

   VkResult result = VK_SUCCESS;
   uint32_t i;
   
   for (i = 0; i < pAllocateInfo->commandBufferCount; i++) {
      result = val_create_cmd_buffer(device, pool, pAllocateInfo->level,
                                     &pCommandBuffers[i]);
      if (result != VK_SUCCESS)
         break;
   }

   if (result != VK_SUCCESS)
      val_FreeCommandBuffers(_device, pAllocateInfo->commandPool,
                             i, pCommandBuffers);

   return result;
}

static void
val_cmd_buffer_destroy(struct val_cmd_buffer *cmd_buffer)
{
   list_del(&cmd_buffer->pool_link);
   val_free(&cmd_buffer->pool->alloc, cmd_buffer);
}

void val_FreeCommandBuffers(
    VkDevice                                    device,
    VkCommandPool                               commandPool,
    uint32_t                                    commandBufferCount,
    const VkCommandBuffer*                      pCommandBuffers)
{
   for (uint32_t i = 0; i < commandBufferCount; i++) {
      VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, pCommandBuffers[i]);

      val_cmd_buffer_destroy(cmd_buffer);
   }
}

VkResult val_ResetCommandBuffer(
    VkCommandBuffer                             commandBuffer,
    VkCommandBufferResetFlags                   flags)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);

   return VK_SUCCESS;
}

VkResult val_BeginCommandBuffer(
    VkCommandBuffer                             commandBuffer,
    const VkCommandBufferBeginInfo*             pBeginInfo)
{
   return VK_SUCCESS;
}

VkResult val_EndCommandBuffer(
    VkCommandBuffer                             commandBuffer)
{

}

VkResult val_CreateCommandPool(
    VkDevice                                    _device,
    const VkCommandPoolCreateInfo*              pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkCommandPool*                              pCmdPool)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   struct val_cmd_pool *pool;

   pool = val_alloc2(&device->alloc, pAllocator, sizeof(*pool), 8,
                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pool == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   if (pAllocator)
      pool->alloc = *pAllocator;
   else
      pool->alloc = device->alloc;

   list_inithead(&pool->cmd_buffers);

   *pCmdPool = val_cmd_pool_to_handle(pool);

   return VK_SUCCESS;
}

void val_DestroyCommandPool(
    VkDevice                                    _device,
    VkCommandPool                               commandPool,
    const VkAllocationCallbacks*                pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_cmd_pool, pool, commandPool);

   val_ResetCommandPool(_device, commandPool, 0);

   val_free2(&device->alloc, pAllocator, pool);
}

VkResult val_ResetCommandPool(
    VkDevice                                    device,
    VkCommandPool                               commandPool,
    VkCommandPoolResetFlags                     flags)
{

}

void val_CmdBeginRenderPass(
    VkCommandBuffer                             commandBuffer,
    const VkRenderPassBeginInfo*                pRenderPassBegin,
    VkSubpassContents                           contents)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_render_pass, pass, pRenderPassBegin->renderPass);
//   VAL_FROM_HANDLE(val_framebuffer, framebuffer, pRenderPassBegin->framebuffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = val_alloc(&cmd_buffer->pool->alloc,
                             sizeof(*cmd),
                             8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmd)
      return;

   cmd->cmd_type = VAL_CMD_BEGIN_RENDER_PASS;

   list_addtail(&cmd->cmd_link, &cmd_buffer->cmds);
}

void val_CmdBindVertexBuffers(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    firstBinding,
    uint32_t                                    bindingCount,
    const VkBuffer*                             pBuffers,
    const VkDeviceSize*                         pOffsets)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = val_alloc(&cmd_buffer->pool->alloc,
                             sizeof(*cmd),
                             8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmd)
      return;

   cmd->cmd_type = VAL_CMD_BIND_VERTEX_BUFFERS;

   cmd->u.vertex_buffers.first = firstBinding;
   cmd->u.vertex_buffers.binding_count = bindingCount;
   cmd->u.vertex_buffers.buffers = (const struct val_buffer *)pBuffers;
   cmd->u.vertex_buffers.offsets = pOffsets;
   list_addtail(&cmd->cmd_link, &cmd_buffer->cmds);

}


void val_CmdBindPipeline(
    VkCommandBuffer                             commandBuffer,
    VkPipelineBindPoint                         pipelineBindPoint,
    VkPipeline                                  _pipeline)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_pipeline, pipeline, _pipeline);
   struct val_cmd_buffer_entry *cmd;

   cmd = val_alloc(&cmd_buffer->pool->alloc,
                             sizeof(*cmd),
                             8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmd)
      return;
   cmd->cmd_type = VAL_CMD_BIND_PIPELINE;
   
   cmd->u.pipeline.bind_point = pipelineBindPoint;
   cmd->u.pipeline.pipeline = pipeline;

   list_addtail(&cmd->cmd_link, &cmd_buffer->cmds);
}

void val_CmdBindDescriptorSets(
    VkCommandBuffer                             commandBuffer,
    VkPipelineBindPoint                         pipelineBindPoint,
    VkPipelineLayout                            _layout,
    uint32_t                                    firstSet,
    uint32_t                                    descriptorSetCount,
    const VkDescriptorSet*                      pDescriptorSets,
    uint32_t                                    dynamicOffsetCount,
    const uint32_t*                             pDynamicOffsets)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   VAL_FROM_HANDLE(val_pipeline_layout, layout, _layout);
   struct val_cmd_buffer_entry *cmd;

   cmd = val_alloc(&cmd_buffer->pool->alloc,
                   sizeof(*cmd),
                   8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmd)
      return;

   cmd->cmd_type = VAL_CMD_BIND_DESCRIPTOR_SETS;
   cmd->u.descriptor_sets.bind_point = pipelineBindPoint;
   cmd->u.descriptor_sets.layout = layout;
   cmd->u.descriptor_sets.first = firstSet;
   cmd->u.descriptor_sets.count = descriptorSetCount;
   cmd->u.descriptor_sets.sets = (const struct val_descriptor_set *)pDescriptorSets;
   cmd->u.descriptor_sets.dynamic_offset_count = dynamicOffsetCount;
   cmd->u.descriptor_sets.dynamic_offsets = pDynamicOffsets;

   list_addtail(&cmd->cmd_link, &cmd_buffer->cmds);   
}

void val_CmdDraw(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    vertexCount,
    uint32_t                                    instanceCount,
    uint32_t                                    firstVertex,
    uint32_t                                    firstInstance)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = val_alloc(&cmd_buffer->pool->alloc,
                   sizeof(*cmd),
                   8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmd)
      return;

   cmd->cmd_type = VAL_CMD_DRAW;
   cmd->u.draw.vertex_count = vertexCount;
   cmd->u.draw.instance_count = instanceCount;
   cmd->u.draw.first_vertex = firstVertex;
   cmd->u.draw.first_instance = firstInstance;
   list_addtail(&cmd->cmd_link, &cmd_buffer->cmds);   
}


void val_CmdEndRenderPass(
    VkCommandBuffer                             commandBuffer)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);
   struct val_cmd_buffer_entry *cmd;

   cmd = val_alloc(&cmd_buffer->pool->alloc,
                             sizeof(*cmd),
                             8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmd)
      return;

   cmd->cmd_type = VAL_CMD_END_RENDER_PASS;

   list_addtail(&cmd->cmd_link, &cmd_buffer->cmds);
//   val_cmd_buffer_resolve_subpass(cmd_buffer);
}

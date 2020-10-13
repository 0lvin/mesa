/*
 * Copyright © 2019 Raspberry Pi
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

#include "v3dv_private.h"
#include "broadcom/cle/v3dx_pack.h"
#include "util/u_pack_color.h"
#include "vk_format_info.h"

const struct v3dv_dynamic_state default_dynamic_state = {
   .viewport = {
      .count = 0,
   },
   .scissor = {
      .count = 0,
   },
   .stencil_compare_mask =
   {
     .front = ~0u,
     .back = ~0u,
   },
   .stencil_write_mask =
   {
     .front = ~0u,
     .back = ~0u,
   },
   .stencil_reference =
   {
     .front = 0u,
     .back = 0u,
   },
};

void
v3dv_job_add_bo(struct v3dv_job *job, struct v3dv_bo *bo)
{
   if (!bo)
      return;

   if (_mesa_set_search(job->bos, bo))
      return;

   _mesa_set_add(job->bos, bo);
   job->bo_count++;
}

void
v3dv_job_add_extra_bo(struct v3dv_job *job, struct v3dv_bo *bo)
{
   assert(bo);
   assert(!_mesa_set_search(job->extra_bos, bo));
   _mesa_set_add(job->extra_bos, bo);
}

static void
subpass_start(struct v3dv_cmd_buffer *cmd_buffer, uint32_t subpass_idx);

static void
subpass_finish(struct v3dv_cmd_buffer *cmd_buffer);

static void
cmd_buffer_emit_render_pass_rcl(struct v3dv_cmd_buffer *cmd_buffer);

VkResult
v3dv_CreateCommandPool(VkDevice _device,
                       const VkCommandPoolCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator,
                       VkCommandPool *pCmdPool)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   struct v3dv_cmd_pool *pool;

   /* We only support one queue */
   assert(pCreateInfo->queueFamilyIndex == 0);

   pool = vk_alloc2(&device->alloc, pAllocator, sizeof(*pool), 8,
                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pool == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   if (pAllocator)
      pool->alloc = *pAllocator;
   else
      pool->alloc = device->alloc;

   list_inithead(&pool->cmd_buffers);

   *pCmdPool = v3dv_cmd_pool_to_handle(pool);

   return VK_SUCCESS;
}

static void
cmd_buffer_init(struct v3dv_cmd_buffer *cmd_buffer,
                struct v3dv_device *device,
                struct v3dv_cmd_pool *pool,
                VkCommandBufferLevel level)
{
   /* Do not reset the loader data header! If we are calling this from
    * a command buffer reset that would reset the loader's dispatch table for
    * the command buffer.
    */
   const uint32_t ld_size = sizeof(VK_LOADER_DATA);
   uint8_t *cmd_buffer_driver_start = ((uint8_t *) cmd_buffer) + ld_size;
   memset(cmd_buffer_driver_start, 0, sizeof(*cmd_buffer) - ld_size);

   cmd_buffer->device = device;
   cmd_buffer->pool = pool;
   cmd_buffer->level = level;

   list_inithead(&cmd_buffer->submit_jobs);

   assert(pool);
   list_addtail(&cmd_buffer->pool_link, &pool->cmd_buffers);

   cmd_buffer->status = V3DV_CMD_BUFFER_STATUS_INITIALIZED;
}

static VkResult
cmd_buffer_create(struct v3dv_device *device,
                  struct v3dv_cmd_pool *pool,
                  VkCommandBufferLevel level,
                  VkCommandBuffer *pCommandBuffer)
{
   struct v3dv_cmd_buffer *cmd_buffer;
   cmd_buffer = vk_alloc(&pool->alloc, sizeof(*cmd_buffer), 8,
                         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (cmd_buffer == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   cmd_buffer_init(cmd_buffer, device, pool, level);

   cmd_buffer->_loader_data.loaderMagic = ICD_LOADER_MAGIC;

   *pCommandBuffer = v3dv_cmd_buffer_to_handle(cmd_buffer);

   return VK_SUCCESS;
}

static void
job_destroy(struct v3dv_job *job)
{
   assert(job);

   list_del(&job->list_link);

   v3dv_cl_destroy(&job->bcl);
   v3dv_cl_destroy(&job->rcl);
   v3dv_cl_destroy(&job->indirect);

   /* Since we don't ref BOs, when we add them to the command buffer, don't
    * unref them here either.
    */
#if 0
   set_foreach(job->bos, entry) {
      struct v3dv_bo *bo = (struct v3dv_bo *)entry->key;
      v3dv_bo_free(cmd_buffer->device, bo);
   }
#endif
   _mesa_set_destroy(job->bos, NULL);

   set_foreach(job->extra_bos, entry) {
      struct v3dv_bo *bo = (struct v3dv_bo *)entry->key;
      v3dv_bo_free(job->cmd_buffer->device, bo);
   }
   _mesa_set_destroy(job->extra_bos, NULL);

   v3dv_bo_free(job->cmd_buffer->device, job->tile_alloc);
   v3dv_bo_free(job->cmd_buffer->device, job->tile_state);
}

static void
cmd_buffer_free_resources(struct v3dv_cmd_buffer *cmd_buffer)
{
   list_del(&cmd_buffer->pool_link);

   list_for_each_entry_safe(struct v3dv_job, job,
                            &cmd_buffer->submit_jobs, list_link) {
      job_destroy(job);
   }

   if (cmd_buffer->state.job)
      job_destroy(cmd_buffer->state.job);

   if (cmd_buffer->state.attachments) {
      assert(cmd_buffer->state.attachment_count > 0);
      vk_free(&cmd_buffer->pool->alloc, cmd_buffer->state.attachments);
   }

   if (cmd_buffer->push_constants_descriptor.bo)
      v3dv_bo_free(cmd_buffer->device, cmd_buffer->push_constants_descriptor.bo);
}

static void
cmd_buffer_destroy(struct v3dv_cmd_buffer *cmd_buffer)
{
   cmd_buffer_free_resources(cmd_buffer);
   vk_free(&cmd_buffer->pool->alloc, cmd_buffer);
}

void
v3dv_job_emit_binning_flush(struct v3dv_job *job)
{
   assert(job);
   v3dv_cl_ensure_space_with_branch(&job->bcl, cl_packet_length(FLUSH));
   cl_emit(&job->bcl, FLUSH, flush);
}

static bool
attachment_list_is_subset(struct v3dv_subpass_attachment *l1, uint32_t l1_count,
                          struct v3dv_subpass_attachment *l2, uint32_t l2_count)
{
   for (uint32_t i = 0; i < l1_count; i++) {
      uint32_t attachment_idx = l1[i].attachment;
      if (attachment_idx == VK_ATTACHMENT_UNUSED)
         continue;

      uint32_t j;
      for (j = 0; j < l2_count; j++) {
         if (l2[j].attachment == attachment_idx)
            break;
      }
      if (j == l2_count)
         return false;
   }

   return true;
 }

static bool
cmd_buffer_can_merge_subpass(struct v3dv_cmd_buffer *cmd_buffer)
{
   const struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   assert(state->pass);

   const struct v3dv_physical_device *physical_device =
      &cmd_buffer->device->instance->physicalDevice;

   if (!physical_device->options.merge_jobs)
      return false;

   /* Each render pass starts a new job */
   if (state->subpass_idx == 0)
      return false;

   /* Two subpasses can be merged in the same job if we can emit a single RCL
    * for them (since the RCL includes the END_OF_RENDERING command that
    * triggers the "render job finished" interrupt). We can do this so long
    * as both subpasses render against the same attachments.
    */
   uint32_t prev_subpass_idx = state->subpass_idx - 1;
   struct v3dv_subpass *prev_subpass = &state->pass->subpasses[prev_subpass_idx];
   struct v3dv_subpass *subpass = &state->pass->subpasses[state->subpass_idx];

   /* Because the list of subpass attachments can include VK_ATTACHMENT_UNUSED,
    * we need to check that for each subpass all its used attachments are
    * used by the other subpass.
    */
   bool compatible =
      attachment_list_is_subset(prev_subpass->color_attachments,
                                prev_subpass->color_count,
                                subpass->color_attachments,
                                subpass->color_count);
   if (!compatible)
      return false;

   compatible =
      attachment_list_is_subset(subpass->color_attachments,
                                subpass->color_count,
                                prev_subpass->color_attachments,
                                prev_subpass->color_count);
   if (!compatible)
      return false;

   /* FIXME: resolve attachments */

   if (subpass->ds_attachment.attachment !=
       prev_subpass->ds_attachment.attachment)
      return false;

   return true;
}

/**
 * Computes and sets the job frame tiling information required to setup frame
 * binning and rendering.
 */
static struct v3dv_frame_tiling *
job_compute_frame_tiling(struct v3dv_job *job,
                         uint32_t width,
                         uint32_t height,
                         uint32_t layers,
                         uint32_t render_target_count,
                         uint8_t max_internal_bpp)
{
   static const uint8_t tile_sizes[] = {
      64, 64,
      64, 32,
      32, 32,
      32, 16,
      16, 16,
   };

   assert(job);
   struct v3dv_frame_tiling *tiling = &job->frame_tiling;

   tiling->width = width;
   tiling->height = height;
   tiling->layers = layers;
   tiling->render_target_count = render_target_count;

   uint32_t tile_size_index = 0;

   /* FIXME: MSAA */

   if (render_target_count > 2)
      tile_size_index += 2;
   else if (render_target_count > 1)
      tile_size_index += 1;

   tiling->internal_bpp = max_internal_bpp;
   tile_size_index += tiling->internal_bpp;
   assert(tile_size_index < ARRAY_SIZE(tile_sizes));

   tiling->tile_width = tile_sizes[tile_size_index * 2];
   tiling->tile_height = tile_sizes[tile_size_index * 2 + 1];

   tiling->draw_tiles_x = DIV_ROUND_UP(width, tiling->tile_width);
   tiling->draw_tiles_y = DIV_ROUND_UP(height, tiling->tile_height);

   /* Size up our supertiles until we get under the limit */
   const uint32_t max_supertiles = 256;
   tiling->supertile_width = 1;
   tiling->supertile_height = 1;
   for (;;) {
      tiling->frame_width_in_supertiles =
         DIV_ROUND_UP(tiling->draw_tiles_x, tiling->supertile_width);
      tiling->frame_height_in_supertiles =
         DIV_ROUND_UP(tiling->draw_tiles_y, tiling->supertile_height);
      const uint32_t num_supertiles = tiling->frame_width_in_supertiles *
                                      tiling->frame_height_in_supertiles;
      if (num_supertiles < max_supertiles)
         break;

      if (tiling->supertile_width < tiling->supertile_height)
         tiling->supertile_width++;
      else
         tiling->supertile_height++;
   }

   return tiling;
}

void
v3dv_cmd_buffer_start_frame(struct v3dv_cmd_buffer *cmd_buffer,
                            uint32_t width,
                            uint32_t height,
                            uint32_t layers,
                            uint32_t render_target_count,
                            uint8_t max_internal_bpp)
{
   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   /* Start by computing frame tiling spec for this job */
   const struct v3dv_frame_tiling *tiling =
      job_compute_frame_tiling(job,
                               width, height, layers,
                               render_target_count, max_internal_bpp);

   v3dv_cl_ensure_space_with_branch(&job->bcl, 256);

   /* The PTB will request the tile alloc initial size per tile at start
    * of tile binning.
    */
   uint32_t tile_alloc_size = 64 * tiling->layers *
                              tiling->draw_tiles_x *
                              tiling->draw_tiles_y;

   /* The PTB allocates in aligned 4k chunks after the initial setup. */
   tile_alloc_size = align(tile_alloc_size, 4096);

   /* Include the first two chunk allocations that the PTB does so that
    * we definitely clear the OOM condition before triggering one (the HW
    * won't trigger OOM during the first allocations).
    */
   tile_alloc_size += 8192;

   /* For performance, allocate some extra initial memory after the PTB's
    * minimal allocations, so that we hopefully don't have to block the
    * GPU on the kernel handling an OOM signal.
    */
   tile_alloc_size += 512 * 1024;

   job->tile_alloc = v3dv_bo_alloc(cmd_buffer->device, tile_alloc_size,
                                   "tile_alloc");
   v3dv_job_add_bo(job, job->tile_alloc);

   const uint32_t tsda_per_tile_size = 256;
   const uint32_t tile_state_size = tiling->layers *
                                    tiling->draw_tiles_x *
                                    tiling->draw_tiles_y *
                                    tsda_per_tile_size;
   job->tile_state = v3dv_bo_alloc(cmd_buffer->device, tile_state_size, "TSDA");
   v3dv_job_add_bo(job, job->tile_state);

   /* This must go before the binning mode configuration. It is
    * required for layered framebuffers to work.
    */
   cl_emit(&job->bcl, NUMBER_OF_LAYERS, config) {
      config.number_of_layers = layers;
   }

   cl_emit(&job->bcl, TILE_BINNING_MODE_CFG, config) {
      config.width_in_pixels = tiling->width;
      config.height_in_pixels = tiling->height;
      config.number_of_render_targets = MAX2(tiling->render_target_count, 1);
      config.multisample_mode_4x = false; /* FIXME */
      config.maximum_bpp_of_all_render_targets = tiling->internal_bpp;
   }

   /* There's definitely nothing in the VCD cache we want. */
   cl_emit(&job->bcl, FLUSH_VCD_CACHE, bin);

   /* Disable any leftover OQ state from another job. */
   cl_emit(&job->bcl, OCCLUSION_QUERY_COUNTER, counter);

   /* "Binning mode lists must have a Start Tile Binning item (6) after
    *  any prefix state data before the binning list proper starts."
    */
   cl_emit(&job->bcl, START_TILE_BINNING, bin);

   job->ez_state = VC5_EZ_UNDECIDED;
   job->first_ez_state = VC5_EZ_UNDECIDED;
}

static void
cmd_buffer_end_render_pass_frame(struct v3dv_cmd_buffer *cmd_buffer)
{
   assert(cmd_buffer->state.job);
   cmd_buffer_emit_render_pass_rcl(cmd_buffer);
   v3dv_job_emit_binning_flush(cmd_buffer->state.job);
}

void
v3dv_cmd_buffer_finish_job(struct v3dv_cmd_buffer *cmd_buffer)
{
   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);
   assert(v3dv_cl_offset(&job->bcl) != 0);

   /* When we merge multiple subpasses into the same job we must only emit one
    * RCL, so we do that here, when we decided that we need to finish the job.
    * Any rendering that happens outside a render pass is never merged, so
    * the RCL should have been emitted by the time we got here.
    */
   assert(v3dv_cl_offset(&job->rcl) != 0 || cmd_buffer->state.pass);
   if (cmd_buffer->state.pass)
      cmd_buffer_end_render_pass_frame(cmd_buffer);

   list_addtail(&job->list_link, &cmd_buffer->submit_jobs);
   cmd_buffer->state.job = NULL;
}

struct v3dv_job *
v3dv_cmd_buffer_start_job(struct v3dv_cmd_buffer *cmd_buffer,
                          int32_t subpass_idx)
{
   /* Don't create a new job if we can merge the current subpass into
    * the current job.
    */
   if (cmd_buffer->state.pass &&
       subpass_idx != -1 &&
       cmd_buffer_can_merge_subpass(cmd_buffer)) {
      cmd_buffer->state.job->is_subpass_finish = false;
      return cmd_buffer->state.job;
   }

   /* Ensure we are not starting a new job without finishing a previous one */
   if (cmd_buffer->state.job != NULL)
      v3dv_cmd_buffer_finish_job(cmd_buffer);

   assert(cmd_buffer->state.job == NULL);
   struct v3dv_job *job = vk_zalloc(&cmd_buffer->device->alloc,
                                    sizeof(struct v3dv_job), 8,
                                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   assert(job);

   job->cmd_buffer = cmd_buffer;

   job->bos =
      _mesa_set_create(NULL, _mesa_hash_pointer, _mesa_key_pointer_equal);
   job->bo_count = 0;

   job->extra_bos =
      _mesa_set_create(NULL, _mesa_hash_pointer, _mesa_key_pointer_equal);

   v3dv_cl_init(job, &job->bcl);
   v3dv_cl_begin(&job->bcl);

   v3dv_cl_init(job, &job->rcl);
   v3dv_cl_begin(&job->rcl);

   v3dv_cl_init(job, &job->indirect);
   v3dv_cl_begin(&job->indirect);

   /* Keep track of the first subpass that we are recording in this new job.
    * We will use this when we emit the RCL to decide how to emit our loads
    * and stores.
    */
   if (cmd_buffer->state.pass)
      job->first_subpass = subpass_idx;

   cmd_buffer->state.job = job;

   return job;
}

static VkResult
cmd_buffer_reset(struct v3dv_cmd_buffer *cmd_buffer,
                 VkCommandBufferResetFlags flags)
{
   if (cmd_buffer->status != V3DV_CMD_BUFFER_STATUS_INITIALIZED) {
      struct v3dv_device *device = cmd_buffer->device;
      struct v3dv_cmd_pool *pool = cmd_buffer->pool;
      VkCommandBufferLevel level = cmd_buffer->level;

      /* FIXME: For now we always free all resources as if
       * VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT was set.
       */
      if (cmd_buffer->status != V3DV_CMD_BUFFER_STATUS_NEW)
         cmd_buffer_free_resources(cmd_buffer);

      cmd_buffer_init(cmd_buffer, device, pool, level);
   }

   assert(cmd_buffer->status == V3DV_CMD_BUFFER_STATUS_INITIALIZED);
   return VK_SUCCESS;
}

VkResult
v3dv_AllocateCommandBuffers(VkDevice _device,
                            const VkCommandBufferAllocateInfo *pAllocateInfo,
                            VkCommandBuffer *pCommandBuffers)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_cmd_pool, pool, pAllocateInfo->commandPool);

   /* FIXME: implement secondary command buffers */
   assert(pAllocateInfo->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY);

   VkResult result = VK_SUCCESS;
   uint32_t i;

   for (i = 0; i < pAllocateInfo->commandBufferCount; i++) {
      result = cmd_buffer_create(device, pool, pAllocateInfo->level,
                                 &pCommandBuffers[i]);
      if (result != VK_SUCCESS)
         break;
   }

   if (result != VK_SUCCESS) {
      v3dv_FreeCommandBuffers(_device, pAllocateInfo->commandPool,
                              i, pCommandBuffers);
      for (i = 0; i < pAllocateInfo->commandBufferCount; i++)
         pCommandBuffers[i] = VK_NULL_HANDLE;
   }

   return result;
}

void
v3dv_FreeCommandBuffers(VkDevice device,
                        VkCommandPool commandPool,
                        uint32_t commandBufferCount,
                        const VkCommandBuffer *pCommandBuffers)
{
   for (uint32_t i = 0; i < commandBufferCount; i++) {
      V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, pCommandBuffers[i]);

      if (!cmd_buffer)
         continue;

      cmd_buffer_destroy(cmd_buffer);
   }
}

void
v3dv_DestroyCommandPool(VkDevice _device,
                        VkCommandPool commandPool,
                        const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_cmd_pool, pool, commandPool);

   if (!pool)
      return;

   list_for_each_entry_safe(struct v3dv_cmd_buffer, cmd_buffer,
                            &pool->cmd_buffers, pool_link) {
      cmd_buffer_destroy(cmd_buffer);
   }

   vk_free2(&device->alloc, pAllocator, pool);
}

VkResult
v3dv_BeginCommandBuffer(VkCommandBuffer commandBuffer,
                        const VkCommandBufferBeginInfo *pBeginInfo)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);

   /* If this is the first vkBeginCommandBuffer, we must initialize the
    * command buffer's state. Otherwise, we must reset its state. In both
    * cases we reset it.
    */
   VkResult result = cmd_buffer_reset(cmd_buffer, 0);
   if (result != VK_SUCCESS)
      return result;

   assert(cmd_buffer->status == V3DV_CMD_BUFFER_STATUS_INITIALIZED);

   cmd_buffer->usage_flags = pBeginInfo->flags;

   cmd_buffer->status = V3DV_CMD_BUFFER_STATUS_RECORDING;

   return VK_SUCCESS;
}

VkResult
v3dv_ResetCommandBuffer(VkCommandBuffer commandBuffer,
                        VkCommandBufferResetFlags flags)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   return cmd_buffer_reset(cmd_buffer, flags);
}

VkResult
v3dv_ResetCommandPool(VkDevice device,
                      VkCommandPool commandPool,
                      VkCommandPoolResetFlags flags)
{
   V3DV_FROM_HANDLE(v3dv_cmd_pool, pool, commandPool);

   VkCommandBufferResetFlags reset_flags = 0;
   if (flags & VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT)
      reset_flags = VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT;
   list_for_each_entry(struct v3dv_cmd_buffer, cmd_buffer,
                       &pool->cmd_buffers, pool_link) {
      cmd_buffer_reset(cmd_buffer, reset_flags);
   }

   return VK_SUCCESS;
}
static void
emit_clip_window(struct v3dv_job *job, const VkRect2D *rect)
{
   assert(job);
   cl_emit(&job->bcl, CLIP_WINDOW, clip) {
      clip.clip_window_left_pixel_coordinate = rect->offset.x;
      clip.clip_window_bottom_pixel_coordinate = rect->offset.y;
      clip.clip_window_width_in_pixels = rect->extent.width;
      clip.clip_window_height_in_pixels = rect->extent.height;
   }
}

void
v3dv_get_hw_clear_color(const VkClearColorValue *color,
                        uint32_t internal_type,
                        uint32_t internal_size,
                        uint32_t *hw_color)
{
   union util_color uc;
   switch (internal_type) {
   case V3D_INTERNAL_TYPE_8:
      util_pack_color(color->float32, PIPE_FORMAT_R8G8B8A8_UNORM, &uc);
      memcpy(hw_color, uc.ui, internal_size);
   break;
   case V3D_INTERNAL_TYPE_8I:
   case V3D_INTERNAL_TYPE_8UI:
      hw_color[0] = ((color->uint32[0] & 0xff) |
                     (color->uint32[1] & 0xff) << 8 |
                     (color->uint32[2] & 0xff) << 16 |
                     (color->uint32[3] & 0xff) << 24);
   break;
   case V3D_INTERNAL_TYPE_16F:
      util_pack_color(color->float32, PIPE_FORMAT_R16G16B16A16_FLOAT, &uc);
      memcpy(hw_color, uc.ui, internal_size);
   break;
   case V3D_INTERNAL_TYPE_16I:
   case V3D_INTERNAL_TYPE_16UI:
      hw_color[0] = ((color->uint32[0] & 0xffff) | color->uint32[1] << 16);
      hw_color[1] = ((color->uint32[2] & 0xffff) | color->uint32[3] << 16);
   break;
   case V3D_INTERNAL_TYPE_32F:
   case V3D_INTERNAL_TYPE_32I:
   case V3D_INTERNAL_TYPE_32UI:
      memcpy(hw_color, color->uint32, internal_size);
      break;
   }
}

static void
cmd_buffer_state_set_attachment_clear_color(struct v3dv_cmd_buffer *cmd_buffer,
                                            uint32_t attachment_idx,
                                            const VkClearColorValue *color)
{
   assert(attachment_idx < cmd_buffer->state.pass->attachment_count);

   const struct v3dv_render_pass_attachment *attachment =
      &cmd_buffer->state.pass->attachments[attachment_idx];

   uint32_t internal_type, internal_bpp;
   const struct v3dv_format *format = v3dv_get_format(attachment->desc.format);
   v3dv_get_internal_type_bpp_for_output_format(format->rt_type,
                                                &internal_type,
                                                &internal_bpp);

   uint32_t internal_size = 4 << internal_bpp;

   struct v3dv_cmd_buffer_attachment_state *attachment_state =
      &cmd_buffer->state.attachments[attachment_idx];

   v3dv_get_hw_clear_color(color, internal_type, internal_size,
                           &attachment_state->clear_value.color[0]);
}

static void
cmd_buffer_state_set_attachment_clear_depth_stencil(
   struct v3dv_cmd_buffer *cmd_buffer,
   uint32_t attachment_idx,
   bool clear_depth, bool clear_stencil,
   const VkClearDepthStencilValue *ds)
{
   struct v3dv_cmd_buffer_attachment_state *attachment_state =
      &cmd_buffer->state.attachments[attachment_idx];

   if (clear_depth)
      attachment_state->clear_value.z = ds->depth;

   if (clear_stencil)
      attachment_state->clear_value.s = ds->stencil;
}

static void
cmd_buffer_state_set_clear_values(struct v3dv_cmd_buffer *cmd_buffer,
                                  uint32_t count, const VkClearValue *values)
{
   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const struct v3dv_render_pass *pass = state->pass;
   assert(count <= pass->attachment_count);

   for (uint32_t i = 0; i < count; i++) {
      const struct v3dv_render_pass_attachment *attachment =
         &pass->attachments[i];

      if (attachment->desc.loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR)
         continue;

      VkImageAspectFlags aspects = vk_format_aspects(attachment->desc.format);
      if (aspects & VK_IMAGE_ASPECT_COLOR_BIT) {
         cmd_buffer_state_set_attachment_clear_color(cmd_buffer, i,
                                                     &values[i].color);
      } else if (aspects & (VK_IMAGE_ASPECT_DEPTH_BIT |
                            VK_IMAGE_ASPECT_STENCIL_BIT)) {
         cmd_buffer_state_set_attachment_clear_depth_stencil(
            cmd_buffer, i,
            aspects & VK_IMAGE_ASPECT_DEPTH_BIT,
            aspects & VK_IMAGE_ASPECT_STENCIL_BIT,
            &values[i].depthStencil);
      }
   }
}

static void
cmd_buffer_init_render_pass_attachment_state(struct v3dv_cmd_buffer *cmd_buffer,
                                             const VkRenderPassBeginInfo *pRenderPassBegin)
{
   cmd_buffer_state_set_clear_values(cmd_buffer,
                                     pRenderPassBegin->clearValueCount,
                                     pRenderPassBegin->pClearValues);
}

static void
cmd_buffer_ensure_render_pass_attachment_state(struct v3dv_cmd_buffer *cmd_buffer)
{
   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const struct v3dv_render_pass *pass = state->pass;

   if (state->attachment_count < pass->attachment_count) {
      if (state->attachment_count > 0)
         vk_free(&cmd_buffer->device->alloc, state->attachments);

      uint32_t size = sizeof(struct v3dv_cmd_buffer_attachment_state) *
                      pass->attachment_count;
      state->attachments = vk_zalloc(&cmd_buffer->device->alloc, size, 8,
                                     VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      state->attachment_count = pass->attachment_count;
   }

   assert(state->attachment_count >= pass->attachment_count);
}

void
v3dv_CmdBeginRenderPass(VkCommandBuffer commandBuffer,
                        const VkRenderPassBeginInfo *pRenderPassBegin,
                        VkSubpassContents contents)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   V3DV_FROM_HANDLE(v3dv_render_pass, pass, pRenderPassBegin->renderPass);
   V3DV_FROM_HANDLE(v3dv_framebuffer, framebuffer, pRenderPassBegin->framebuffer);

   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   state->pass = pass;
   state->framebuffer = framebuffer;

   cmd_buffer_ensure_render_pass_attachment_state(cmd_buffer);
   cmd_buffer_init_render_pass_attachment_state(cmd_buffer, pRenderPassBegin);

   /* FIXME: probably need to align the render area to tile boundaries since
    *        the tile clears will render full tiles anyway.
    *        See vkGetRenderAreaGranularity().
    */
   state->render_area = pRenderPassBegin->renderArea;

   /* Setup for first subpass */
   subpass_start(cmd_buffer, 0);
}

void
v3dv_CmdNextSubpass(VkCommandBuffer commandBuffer, VkSubpassContents contents)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);

   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   assert(state->subpass_idx < state->pass->subpass_count - 1);

   /* Finish the previous subpass */
   subpass_finish(cmd_buffer);

   /* Start the next subpass */
   subpass_start(cmd_buffer, state->subpass_idx + 1);
}

void
v3dv_render_pass_setup_render_target(struct v3dv_cmd_buffer *cmd_buffer,
                                     int rt,
                                     uint32_t *rt_bpp,
                                     uint32_t *rt_type,
                                     uint32_t *rt_clamp)
{
   const struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;

   assert(state->subpass_idx < state->pass->subpass_count);
   const struct v3dv_subpass *subpass =
      &state->pass->subpasses[state->subpass_idx];

   if (rt >= subpass->color_count)
      return;

   struct v3dv_subpass_attachment *attachment = &subpass->color_attachments[rt];
   const uint32_t attachment_idx = attachment->attachment;
   if (attachment_idx == VK_ATTACHMENT_UNUSED)
      return;

   const struct v3dv_framebuffer *framebuffer = state->framebuffer;
   assert(attachment_idx < framebuffer->attachment_count);
   struct v3dv_image_view *iview = framebuffer->attachments[attachment_idx];
   assert(iview->aspects & VK_IMAGE_ASPECT_COLOR_BIT);

   *rt_bpp = iview->internal_bpp;
   *rt_type = iview->internal_type;
   *rt_clamp = V3D_RENDER_TARGET_CLAMP_NONE;
}

static void
cmd_buffer_render_pass_emit_load(struct v3dv_cmd_buffer *cmd_buffer,
                                 struct v3dv_cl *cl,
                                 struct v3dv_image_view *iview,
                                 uint32_t layer,
                                 uint32_t buffer)
{
   const struct v3dv_image *image = iview->image;
   const struct v3d_resource_slice *slice = &image->slices[iview->base_level];
   uint32_t layer_offset = v3dv_layer_offset(image,
                                             iview->base_level,
                                             iview->first_layer + layer);

   cl_emit(cl, LOAD_TILE_BUFFER_GENERAL, load) {
      load.buffer_to_load = buffer;
      load.address = v3dv_cl_address(image->mem->bo, layer_offset);

      load.input_image_format = iview->format->rt_type;
      load.r_b_swap = iview->swap_rb;
      load.memory_format = slice->tiling;

      if (slice->tiling == VC5_TILING_UIF_NO_XOR ||
          slice->tiling == VC5_TILING_UIF_XOR) {
         load.height_in_ub_or_stride =
            slice->padded_height_of_output_image_in_uif_blocks;
      } else if (slice->tiling == VC5_TILING_RASTER) {
         load.height_in_ub_or_stride = slice->stride;
      }

      if (image->samples > VK_SAMPLE_COUNT_1_BIT)
         load.decimate_mode = V3D_DECIMATE_MODE_ALL_SAMPLES;
      else
         load.decimate_mode = V3D_DECIMATE_MODE_SAMPLE_0;
   }
}

static void
cmd_buffer_render_pass_emit_loads(struct v3dv_cmd_buffer *cmd_buffer,
                                  struct v3dv_cl *cl,
                                  uint32_t layer)
{
   const struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const struct v3dv_framebuffer *framebuffer = state->framebuffer;
   const struct v3dv_render_pass *pass = state->pass;
   const struct v3dv_subpass *subpass = &pass->subpasses[state->subpass_idx];

   for (uint32_t i = 0; i < subpass->color_count; i++) {
      uint32_t attachment_idx = subpass->color_attachments[i].attachment;

      if (attachment_idx == VK_ATTACHMENT_UNUSED)
         continue;

      const struct v3dv_render_pass_attachment *attachment =
         &state->pass->attachments[attachment_idx];

      /* According to the Vulkan spec:
       *
       *    "The load operation for each sample in an attachment happens before
       *     any recorded command which accesses the sample in the first subpass
       *     where the attachment is used."
       *
       * If the load operation is CLEAR, we must only clear once on the first
       * subpass that uses the attachment (and in that case we don't LOAD).
       * After that, we always want to load so we don't lose any rendering done
       * by a previous subpass to the same attachment. We also want to load
       * if the current job is continuing subpass work started by a previous
       * job, for the same reason.
       */
      assert(state->job->first_subpass >= attachment->first_subpass);
      bool needs_load =
         state->job->first_subpass > attachment->first_subpass ||
         state->job->is_subpass_continue ||
         attachment->desc.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD;

      if (needs_load) {
         struct v3dv_image_view *iview = framebuffer->attachments[attachment_idx];
         cmd_buffer_render_pass_emit_load(cmd_buffer, cl, iview,
                                          layer, RENDER_TARGET_0 + i);
      }
   }

   uint32_t ds_attachment_idx = subpass->ds_attachment.attachment;
   if (ds_attachment_idx != VK_ATTACHMENT_UNUSED) {
      const struct v3dv_render_pass_attachment *ds_attachment =
         &state->pass->attachments[ds_attachment_idx];

      assert(state->job->first_subpass >= ds_attachment->first_subpass);
      bool needs_load =
         state->job->first_subpass > ds_attachment->first_subpass ||
         state->job->is_subpass_continue ||
         ds_attachment->desc.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD;

      if (needs_load) {
         struct v3dv_image_view *iview =
            framebuffer->attachments[ds_attachment_idx];
         /* From the Vulkan spec:
          *
          *   "When an image view of a depth/stencil image is used as a
          *   depth/stencil framebuffer attachment, the aspectMask is ignored
          *   and both depth and stencil image subresources are used."
          */
         const uint32_t zs_buffer =
            v3dv_zs_buffer_from_vk_format(iview->image->vk_format);
         cmd_buffer_render_pass_emit_load(cmd_buffer, cl,
                                          iview, layer, zs_buffer);
      }
   }

   cl_emit(cl, END_OF_LOADS, end);
}

static void
cmd_buffer_render_pass_emit_store(struct v3dv_cmd_buffer *cmd_buffer,
                                  struct v3dv_cl *cl,
                                  uint32_t attachment_idx,
                                  uint32_t layer,
                                  uint32_t buffer,
                                  bool clear)
{
   const struct v3dv_image_view *iview =
      cmd_buffer->state.framebuffer->attachments[attachment_idx];
   const struct v3dv_image *image = iview->image;
   const struct v3d_resource_slice *slice = &image->slices[iview->base_level];
   uint32_t layer_offset = v3dv_layer_offset(image,
                                             iview->base_level,
                                             iview->first_layer + layer);

   cl_emit(cl, STORE_TILE_BUFFER_GENERAL, store) {
      store.buffer_to_store = buffer;
      store.address = v3dv_cl_address(image->mem->bo, layer_offset);
      store.clear_buffer_being_stored = clear;

      store.output_image_format = iview->format->rt_type;
      store.r_b_swap = iview->swap_rb;
      store.memory_format = slice->tiling;

      if (slice->tiling == VC5_TILING_UIF_NO_XOR ||
          slice->tiling == VC5_TILING_UIF_XOR) {
         store.height_in_ub_or_stride =
            slice->padded_height_of_output_image_in_uif_blocks;
      } else if (slice->tiling == VC5_TILING_RASTER) {
         store.height_in_ub_or_stride = slice->stride;
      }

      if (image->samples > VK_SAMPLE_COUNT_1_BIT)
         store.decimate_mode = V3D_DECIMATE_MODE_ALL_SAMPLES;
      else
         store.decimate_mode = V3D_DECIMATE_MODE_SAMPLE_0;
   }
}

static void
cmd_buffer_render_pass_emit_stores(struct v3dv_cmd_buffer *cmd_buffer,
                                   struct v3dv_cl *cl,
                                   uint32_t layer)
{
   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const struct v3dv_subpass *subpass =
      &state->pass->subpasses[state->subpass_idx];

   bool has_stores = false;
   for (uint32_t i = 0; i < subpass->color_count; i++) {
      uint32_t attachment_idx = subpass->color_attachments[i].attachment;

      if (attachment_idx == VK_ATTACHMENT_UNUSED)
         continue;

      const struct v3dv_render_pass_attachment *attachment =
         &state->pass->attachments[attachment_idx];

      assert(state->job->first_subpass >= attachment->first_subpass);
      assert(state->subpass_idx >= attachment->first_subpass);
      assert(state->subpass_idx <= attachment->last_subpass);

      /* Only clear once on the first subpass that uses the attachment */
      bool needs_clear =
         state->job->first_subpass == attachment->first_subpass &&
         attachment->desc.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR &&
         !state->job->is_subpass_continue;

      /* Skip the last store if it is not required  */
      bool needs_store =
         state->subpass_idx < attachment->last_subpass ||
         attachment->desc.storeOp == VK_ATTACHMENT_STORE_OP_STORE ||
         needs_clear ||
         !state->job->is_subpass_finish;

      if (needs_store) {
         cmd_buffer_render_pass_emit_store(cmd_buffer, cl,
                                           attachment_idx, layer,
                                           RENDER_TARGET_0 + i,
                                           needs_clear);
         has_stores = true;
      }
   }

   /* FIXME: separate stencil
    *
    * GFXH-1461/GFXH-1689: The per-buffer store command's clear
    * buffer bit is broken for depth/stencil.  In addition, the
    * clear packet's Z/S bit is broken, but the RTs bit ends up
    * clearing Z/S.
    *
    * This means that when we implement depth/stencil clearing we
    * need to emit a separate clear before we start the render pass,
    * since the RTs bit is for clearing all render targets, and we might
    * not want to do that. We might want to consider emitting clears for
    * all RTs needing clearing just once ahead of the first subpass.
    */
   bool needs_ds_clear = false;
   uint32_t ds_attachment_idx = subpass->ds_attachment.attachment;
   if (ds_attachment_idx != VK_ATTACHMENT_UNUSED) {
      const struct v3dv_render_pass_attachment *ds_attachment =
         &state->pass->attachments[ds_attachment_idx];

      assert(state->job->first_subpass >= ds_attachment->first_subpass);
      assert(state->subpass_idx >= ds_attachment->first_subpass);
      assert(state->subpass_idx <= ds_attachment->last_subpass);

      /* Only clear once on the first subpass that uses the attachment */
      needs_ds_clear =
         state->job->first_subpass == ds_attachment->first_subpass &&
         ds_attachment->desc.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR &&
         !state->job->is_subpass_continue;

      /* Skip the last store if it is not required  */
      bool needs_ds_store =
         state->subpass_idx < ds_attachment->last_subpass ||
         ds_attachment->desc.storeOp == VK_ATTACHMENT_STORE_OP_STORE ||
         needs_ds_clear ||
         !state->job->is_subpass_finish;

      if (needs_ds_store) {
         struct v3dv_image_view *iview =
            state->framebuffer->attachments[ds_attachment_idx];
         /* From the Vulkan spec:
          *
          *   "When an image view of a depth/stencil image is used as a
          *   depth/stencil framebuffer attachment, the aspectMask is ignored
          *   and both depth and stencil image subresources are used."
          */
         const uint32_t zs_buffer =
            v3dv_zs_buffer_from_vk_format(iview->image->vk_format);
         cmd_buffer_render_pass_emit_store(cmd_buffer, cl,
                                           ds_attachment_idx, layer,
                                           zs_buffer, needs_ds_clear);
         has_stores = true;
      }
   }

   /* We always need to emit at least one dummy store */
   if (!has_stores) {
      cl_emit(cl, STORE_TILE_BUFFER_GENERAL, store) {
         store.buffer_to_store = NONE;
      }
   }

   /* FIXME: see fixme remark for depth/stencil above */
   if (needs_ds_clear) {
      cl_emit(cl, CLEAR_TILE_BUFFERS, clear) {
         clear.clear_z_stencil_buffer = true;
         clear.clear_all_render_targets = true;
      }
   }
}

static void
cmd_buffer_render_pass_emit_per_tile_rcl(struct v3dv_cmd_buffer *cmd_buffer,
                                         uint32_t layer)
{
   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   /* Emit the generic list in our indirect state -- the rcl will just
    * have pointers into it.
    */
   struct v3dv_cl *cl = &job->indirect;
   v3dv_cl_ensure_space(cl, 200, 1);
   struct v3dv_cl_reloc tile_list_start = v3dv_cl_get_address(cl);

   cl_emit(cl, TILE_COORDINATES_IMPLICIT, coords);

   cmd_buffer_render_pass_emit_loads(cmd_buffer, cl, layer);

   /* The binner starts out writing tiles assuming that the initial mode
    * is triangles, so make sure that's the case.
    */
   cl_emit(cl, PRIM_LIST_FORMAT, fmt) {
      fmt.primitive_type = LIST_TRIANGLES;
   }

   cl_emit(cl, BRANCH_TO_IMPLICIT_TILE_LIST, branch);

   cmd_buffer_render_pass_emit_stores(cmd_buffer, cl, layer);

   cl_emit(cl, END_OF_TILE_MARKER, end);

   cl_emit(cl, RETURN_FROM_SUB_LIST, ret);

   cl_emit(&job->rcl, START_ADDRESS_OF_GENERIC_TILE_LIST, branch) {
      branch.start = tile_list_start;
      branch.end = v3dv_cl_get_address(cl);
   }
}

static void
cmd_buffer_emit_render_pass_layer_rcl(struct v3dv_cmd_buffer *cmd_buffer,
                                      uint32_t layer)
{
   const struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;

   struct v3dv_job *job = cmd_buffer->state.job;
   struct v3dv_cl *rcl = &job->rcl;

   /* If doing multicore binning, we would need to initialize each
    * core's tile list here.
    */
   const struct v3dv_frame_tiling *tiling = &job->frame_tiling;
   const uint32_t tile_alloc_offset =
      64 * layer * tiling->draw_tiles_x * tiling->draw_tiles_y;
   cl_emit(rcl, MULTICORE_RENDERING_TILE_LIST_SET_BASE, list) {
      list.address = v3dv_cl_address(job->tile_alloc, tile_alloc_offset);
   }

   cl_emit(rcl, MULTICORE_RENDERING_SUPERTILE_CFG, config) {
      config.number_of_bin_tile_lists = 1;
      config.total_frame_width_in_tiles = tiling->draw_tiles_x;
      config.total_frame_height_in_tiles = tiling->draw_tiles_y;

      config.supertile_width_in_tiles = tiling->supertile_width;
      config.supertile_height_in_tiles = tiling->supertile_height;

      config.total_frame_width_in_supertiles =
         tiling->frame_width_in_supertiles;
      config.total_frame_height_in_supertiles =
         tiling->frame_height_in_supertiles;
   }

   /* Start by clearing the tile buffer. */
   cl_emit(rcl, TILE_COORDINATES, coords) {
      coords.tile_column_number = 0;
      coords.tile_row_number = 0;
   }

   /* Emit an initial clear of the tile buffers. This is necessary
    * for any buffers that should be cleared (since clearing
    * normally happens at the *end* of the generic tile list), but
    * it's also nice to clear everything so the first tile doesn't
    * inherit any contents from some previous frame.
    *
    * Also, implement the GFXH-1742 workaround. There's a race in
    * the HW between the RCL updating the TLB's internal type/size
    * and the spawning of the QPU instances using the TLB's current
    * internal type/size. To make sure the QPUs get the right
    * state, we need 1 dummy store in between internal type/size
    * changes on V3D 3.x, and 2 dummy stores on 4.x.
    */
   for (int i = 0; i < 2; i++) {
      if (i > 0)
         cl_emit(rcl, TILE_COORDINATES, coords);
      cl_emit(rcl, END_OF_LOADS, end);
      cl_emit(rcl, STORE_TILE_BUFFER_GENERAL, store) {
         store.buffer_to_store = NONE;
      }
      if (i == 0) {
         cl_emit(rcl, CLEAR_TILE_BUFFERS, clear) {
            clear.clear_z_stencil_buffer = true;
            clear.clear_all_render_targets = true;
         }
      }
      cl_emit(rcl, END_OF_TILE_MARKER, end);
   }

   cl_emit(rcl, FLUSH_VCD_CACHE, flush);

   cmd_buffer_render_pass_emit_per_tile_rcl(cmd_buffer, layer);

   uint32_t supertile_w_in_pixels =
      tiling->tile_width * tiling->supertile_width;
   uint32_t supertile_h_in_pixels =
      tiling->tile_height * tiling->supertile_height;
   const uint32_t min_x_supertile =
      state->render_area.offset.x / supertile_w_in_pixels;
   const uint32_t min_y_supertile =
      state->render_area.offset.y / supertile_h_in_pixels;

   uint32_t max_render_x = state->render_area.offset.x;
   if (state->render_area.extent.width > 0)
      max_render_x += state->render_area.extent.width - 1;
   uint32_t max_render_y = state->render_area.offset.y;
   if (state->render_area.extent.height > 0)
      max_render_y += state->render_area.extent.height - 1;
   const uint32_t max_x_supertile = max_render_x / supertile_w_in_pixels;
   const uint32_t max_y_supertile = max_render_y / supertile_h_in_pixels;

   for (int y = min_y_supertile; y <= max_y_supertile; y++) {
      for (int x = min_x_supertile; x <= max_x_supertile; x++) {
         cl_emit(rcl, SUPERTILE_COORDINATES, coords) {
            coords.column_number_in_supertiles = x;
            coords.row_number_in_supertiles = y;
         }
      }
   }
}

static void
set_rcl_early_z_config(struct v3dv_job *job,
                       bool *early_z_disable,
                       uint32_t *early_z_test_and_update_direction)
{
   switch (job->first_ez_state) {
   case VC5_EZ_UNDECIDED:
   case VC5_EZ_LT_LE:
      *early_z_disable = false;
      *early_z_test_and_update_direction = EARLY_Z_DIRECTION_LT_LE;
      break;
   case VC5_EZ_GT_GE:
      *early_z_disable = false;
      *early_z_test_and_update_direction = EARLY_Z_DIRECTION_GT_GE;
      break;
   case VC5_EZ_DISABLED:
      *early_z_disable = true;
      break;
   }
}

static void
cmd_buffer_emit_render_pass_rcl(struct v3dv_cmd_buffer *cmd_buffer)
{
   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   const struct v3dv_frame_tiling *tiling = &job->frame_tiling;

   const struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const struct v3dv_framebuffer *framebuffer = state->framebuffer;
   const uint32_t fb_layers = framebuffer->layers;

   v3dv_cl_ensure_space_with_branch(&job->rcl, 200 +
                                    MAX2(fb_layers, 1) * 256 *
                                    cl_packet_length(SUPERTILE_COORDINATES));

   assert(state->subpass_idx < state->pass->subpass_count);
   const struct v3dv_subpass *subpass =
      &state->pass->subpasses[state->subpass_idx];

   struct v3dv_cl *rcl = &job->rcl;

   /* Comon config must be the first TILE_RENDERING_MODE_CFG and
    * Z_STENCIL_CLEAR_VALUES must be last. The ones in between are optional
    * updates to the previous HW state.
    */
   const uint32_t ds_attachment_idx = subpass->ds_attachment.attachment;

   cl_emit(rcl, TILE_RENDERING_MODE_CFG_COMMON, config) {
      config.image_width_pixels = framebuffer->width;
      config.image_height_pixels = framebuffer->height;
      config.number_of_render_targets = MAX2(subpass->color_count, 1);
      config.multisample_mode_4x = false; /* FIXME */
      config.maximum_bpp_of_all_render_targets = tiling->internal_bpp;

      if (ds_attachment_idx != VK_ATTACHMENT_UNUSED) {
         const struct v3dv_image_view *iview =
            framebuffer->attachments[ds_attachment_idx];
         config.internal_depth_type = iview->internal_type;
         set_rcl_early_z_config(job,
                                &config.early_z_disable,
                                &config.early_z_test_and_update_direction);
      } else {
         config.early_z_disable = true;
      }
   }

   for (uint32_t i = 0; i < subpass->color_count; i++) {
      uint32_t attachment_idx = subpass->color_attachments[i].attachment;
      if (attachment_idx == VK_ATTACHMENT_UNUSED)
         continue;

      struct v3dv_image_view *iview =
         state->framebuffer->attachments[attachment_idx];

      const struct v3dv_image *image = iview->image;
      const struct v3d_resource_slice *slice = &image->slices[iview->base_level];

      const uint32_t *clear_color =
         &state->attachments[attachment_idx].clear_value.color[0];

      uint32_t clear_pad = 0;
      if (slice->tiling == VC5_TILING_UIF_NO_XOR ||
          slice->tiling == VC5_TILING_UIF_XOR) {
         int uif_block_height = v3d_utile_height(image->cpp) * 2;

         uint32_t implicit_padded_height =
            align(framebuffer->height, uif_block_height) / uif_block_height;

         if (slice->padded_height_of_output_image_in_uif_blocks -
             implicit_padded_height >= 15) {
            clear_pad = slice->padded_height_of_output_image_in_uif_blocks;
         }
      }

      /* FIXME: the tile buffer clears don't seem to honor the scissor rect
       * so if the current combination of scissor + renderArea doesn't cover
       * the full extent of the render target we won't get correct behavior.
       * We probably need to detect these cases, implement the clearing by
       * drawing a rect and skip clearing here.
       */
      cl_emit(rcl, TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART1, clear) {
         clear.clear_color_low_32_bits = clear_color[0];
         clear.clear_color_next_24_bits = clear_color[1] & 0xffffff;
         clear.render_target_number = i;
      };

      if (iview->internal_bpp >= V3D_INTERNAL_BPP_64) {
         cl_emit(rcl, TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART2, clear) {
            clear.clear_color_mid_low_32_bits =
              ((clear_color[1] >> 24) | (clear_color[2] << 8));
            clear.clear_color_mid_high_24_bits =
              ((clear_color[2] >> 24) | ((clear_color[3] & 0xffff) << 8));
            clear.render_target_number = i;
         };
      }

      if (iview->internal_bpp >= V3D_INTERNAL_BPP_128 || clear_pad) {
         cl_emit(rcl, TILE_RENDERING_MODE_CFG_CLEAR_COLORS_PART3, clear) {
            clear.uif_padded_height_in_uif_blocks = clear_pad;
            clear.clear_color_high_16_bits = clear_color[3] >> 16;
            clear.render_target_number = i;
         };
      }
   }

   cl_emit(rcl, TILE_RENDERING_MODE_CFG_COLOR, rt) {
      v3dv_render_pass_setup_render_target(cmd_buffer, 0,
                                           &rt.render_target_0_internal_bpp,
                                           &rt.render_target_0_internal_type,
                                           &rt.render_target_0_clamp);
      v3dv_render_pass_setup_render_target(cmd_buffer, 1,
                                           &rt.render_target_1_internal_bpp,
                                           &rt.render_target_1_internal_type,
                                           &rt.render_target_1_clamp);
      v3dv_render_pass_setup_render_target(cmd_buffer, 2,
                                           &rt.render_target_2_internal_bpp,
                                           &rt.render_target_2_internal_type,
                                           &rt.render_target_2_clamp);
      v3dv_render_pass_setup_render_target(cmd_buffer, 3,
                                           &rt.render_target_3_internal_bpp,
                                           &rt.render_target_3_internal_type,
                                           &rt.render_target_3_clamp);
   }

   /* Ends rendering mode config. */
   if (ds_attachment_idx != VK_ATTACHMENT_UNUSED) {
      cl_emit(rcl, TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES, clear) {
         clear.z_clear_value =
            state->attachments[ds_attachment_idx].clear_value.z;
         clear.stencil_clear_value =
            state->attachments[ds_attachment_idx].clear_value.s;
      };
   } else {
      cl_emit(rcl, TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES, clear) {
         clear.z_clear_value = 1.0f;
         clear.stencil_clear_value = 0;
      };
   }

   /* Always set initial block size before the first branch, which needs
    * to match the value from binning mode config.
    */
   cl_emit(rcl, TILE_LIST_INITIAL_BLOCK_SIZE, init) {
      init.use_auto_chained_tile_lists = true;
      init.size_of_first_block_in_chained_tile_lists =
         TILE_ALLOCATION_BLOCK_SIZE_64B;
   }

   for (int layer = 0; layer < MAX2(1, fb_layers); layer++)
      cmd_buffer_emit_render_pass_layer_rcl(cmd_buffer, layer);

   cl_emit(rcl, END_OF_RENDERING, end);
}

static void
subpass_start(struct v3dv_cmd_buffer *cmd_buffer, uint32_t subpass_idx)
{
   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   assert(subpass_idx < state->pass->subpass_count);

   /* Starting a new job can trigger a finish of the current one, so don't
    * change the command buffer state for the new job until we are done creating
    * the new job.
    */
   struct v3dv_job *job = v3dv_cmd_buffer_start_job(cmd_buffer, subpass_idx);

   state->subpass_idx = subpass_idx;

   /* If we are starting a new job we need to setup binning. */
   if (job->first_subpass == state->subpass_idx) {
      const struct v3dv_subpass *subpass =
         &state->pass->subpasses[state->subpass_idx];

      const struct v3dv_framebuffer *framebuffer = state->framebuffer;

      const uint8_t internal_bpp =
         v3dv_framebuffer_compute_internal_bpp(framebuffer, subpass);

      v3dv_cmd_buffer_start_frame(cmd_buffer,
                                  framebuffer->width,
                                  framebuffer->height,
                                  framebuffer->layers,
                                  subpass->color_count,
                                  internal_bpp);
   }

   /* If we don't have a scissor or viewport defined let's just use the render
    * area as clip_window, as that would be required for a clear in any
    * case. If we have that, it would be emitted as part of the pipeline
    * dynamic state flush
    *
    * FIXME: this is mostly just needed for clear. radv has dedicated paths
    * for them, so we could get that idea. In any case, need to revisit if
    * this is the place to emit the clip window.
    */
   if (cmd_buffer->state.dynamic.scissor.count == 0 &&
       cmd_buffer->state.dynamic.viewport.count == 0) {
      emit_clip_window(job, &state->render_area);
   }
}

static void
subpass_finish(struct v3dv_cmd_buffer *cmd_buffer)
{
   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);
   job->is_subpass_finish = true;
}

void
v3dv_CmdEndRenderPass(VkCommandBuffer commandBuffer)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);

   /* Emit last subpass */
   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   assert(state->subpass_idx == state->pass->subpass_count - 1);
   subpass_finish(cmd_buffer);
   v3dv_cmd_buffer_finish_job(cmd_buffer);

   /* We are no longer inside a render pass */
   state->pass = NULL;
   state->framebuffer = NULL;
}

VkResult
v3dv_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);

   cmd_buffer->status = V3DV_CMD_BUFFER_STATUS_EXECUTABLE;

   struct v3dv_job *job = cmd_buffer->state.job;
   if (!job)
      return VK_SUCCESS;

   /* We get here if we recorded commands after the last render pass in the
    * command buffer. Make sure we finish this last job. */
   assert(v3dv_cl_offset(&job->bcl) != 0);
   v3dv_cmd_buffer_finish_job(cmd_buffer);

   return VK_SUCCESS;
}

/* This goes though the list of possible dynamic states in the pipeline and,
 * for those that are not configured as dynamic, copies relevant state into
 * the command buffer.
 */
static void
cmd_buffer_bind_pipeline_static_state(struct v3dv_cmd_buffer *cmd_buffer,
                                      const struct v3dv_dynamic_state *src)
{
   struct v3dv_dynamic_state *dest = &cmd_buffer->state.dynamic;
   uint32_t dynamic_mask = src->mask;
   uint32_t dirty = 0;

   /* See note on SetViewport. We follow radv approach to only allow to set
    * the number of viewports/scissors at pipeline creation time.
    */
   dest->viewport.count = src->viewport.count;
   dest->scissor.count = src->scissor.count;

   if (!(dynamic_mask & V3DV_DYNAMIC_VIEWPORT)) {
      if (memcmp(&dest->viewport.viewports, &src->viewport.viewports,
                 src->viewport.count * sizeof(VkViewport))) {
         typed_memcpy(dest->viewport.viewports,
                      src->viewport.viewports,
                      src->viewport.count);
         typed_memcpy(dest->viewport.scale, src->viewport.scale,
                      src->viewport.count);
         typed_memcpy(dest->viewport.translate, src->viewport.translate,
                      src->viewport.count);
         dirty |= V3DV_CMD_DIRTY_VIEWPORT;
      }
   }

   if (!(dynamic_mask & V3DV_DYNAMIC_SCISSOR)) {
      if (memcmp(&dest->scissor.scissors, &src->scissor.scissors,
                 src->scissor.count * sizeof(VkRect2D))) {
         typed_memcpy(dest->scissor.scissors,
                      src->scissor.scissors, src->scissor.count);
         dirty |= V3DV_CMD_DIRTY_SCISSOR;
      }
   }

   if (!(dynamic_mask & V3DV_DYNAMIC_STENCIL_COMPARE_MASK)) {
      if (memcmp(&dest->stencil_compare_mask, &src->stencil_compare_mask,
                 sizeof(src->stencil_compare_mask))) {
         dest->stencil_compare_mask = src->stencil_compare_mask;
         dirty |= V3DV_CMD_DIRTY_STENCIL_COMPARE_MASK;
      }
   }

   if (!(dynamic_mask & V3DV_DYNAMIC_STENCIL_WRITE_MASK)) {
      if (memcmp(&dest->stencil_write_mask, &src->stencil_write_mask,
                 sizeof(src->stencil_write_mask))) {
         dest->stencil_write_mask = src->stencil_write_mask;
         dirty |= V3DV_CMD_DIRTY_STENCIL_WRITE_MASK;
      }
   }

   if (!(dynamic_mask & V3DV_DYNAMIC_STENCIL_REFERENCE)) {
      if (memcmp(&dest->stencil_reference, &src->stencil_reference,
                 sizeof(src->stencil_reference))) {
         dest->stencil_reference = src->stencil_reference;
         dirty |= V3DV_CMD_DIRTY_STENCIL_REFERENCE;
      }
   }

   cmd_buffer->state.dynamic.mask = dynamic_mask;
   cmd_buffer->state.dirty |= dirty;
}

static void
cmd_buffer_update_ez_state(struct v3dv_cmd_buffer *cmd_buffer,
                           struct v3dv_pipeline *pipeline)
{
   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   switch (pipeline->ez_state) {
   case VC5_EZ_UNDECIDED:
      /* If the pipeline didn't pick a direction but didn't disable, then go
       * along with the current EZ state. This allows EZ optimization for Z
       * func == EQUAL or NEVER.
       */
      break;

   case VC5_EZ_LT_LE:
   case VC5_EZ_GT_GE:
      /* If the pipeline picked a direction, then it needs to match the current
       * direction if we've decided on one.
       */
      if (job->ez_state == VC5_EZ_UNDECIDED)
         job->ez_state = pipeline->ez_state;
      else if (job->ez_state != pipeline->ez_state)
         job->ez_state = VC5_EZ_DISABLED;
      break;

   case VC5_EZ_DISABLED:
      /* If the pipeline disables EZ because of a bad Z func or stencil
       * operation, then we can't do any more EZ in this frame.
       */
      job->ez_state = VC5_EZ_DISABLED;
      break;
   }

   /* If the FS writes Z, then it may update against the chosen EZ direction */
   if (pipeline->fs->prog_data.fs->writes_z)
      job->ez_state = VC5_EZ_DISABLED;

   if (job->first_ez_state == VC5_EZ_UNDECIDED &&
       job->ez_state != VC5_EZ_DISABLED) {
      job->first_ez_state = job->ez_state;
   }
}

static void
bind_graphics_pipeline(struct v3dv_cmd_buffer *cmd_buffer,
                       struct v3dv_pipeline *pipeline)
{
   if (cmd_buffer->state.pipeline == pipeline)
      return;

   cmd_buffer->state.pipeline = pipeline;

   cmd_buffer_bind_pipeline_static_state(cmd_buffer, &pipeline->dynamic_state);
   cmd_buffer_update_ez_state(cmd_buffer, pipeline);

   cmd_buffer->state.dirty |= V3DV_CMD_DIRTY_PIPELINE;
}

void
v3dv_CmdBindPipeline(VkCommandBuffer commandBuffer,
                     VkPipelineBindPoint pipelineBindPoint,
                     VkPipeline _pipeline)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   V3DV_FROM_HANDLE(v3dv_pipeline, pipeline, _pipeline);

   switch (pipelineBindPoint) {
   case VK_PIPELINE_BIND_POINT_COMPUTE:
      assert(!"VK_PIPELINE_BIND_POINT_COMPUTE not supported yet");
      break;

   case VK_PIPELINE_BIND_POINT_GRAPHICS:
      bind_graphics_pipeline(cmd_buffer, pipeline);
      break;

   default:
      assert(!"invalid bind point");
      break;
   }
}

/* FIXME: C&P from radv. tu has similar code. Perhaps common place? */
void
v3dv_viewport_compute_xform(const VkViewport *viewport,
                            float scale[3],
                            float translate[3])
{
   float x = viewport->x;
   float y = viewport->y;
   float half_width = 0.5f * viewport->width;
   float half_height = 0.5f * viewport->height;
   double n = viewport->minDepth;
   double f = viewport->maxDepth;

   scale[0] = half_width;
   translate[0] = half_width + x;
   scale[1] = half_height;
   translate[1] = half_height + y;

   scale[2] = (f - n);
   translate[2] = n;
}

void
v3dv_CmdSetViewport(VkCommandBuffer commandBuffer,
                    uint32_t firstViewport,
                    uint32_t viewportCount,
                    const VkViewport *pViewports)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const uint32_t total_count = firstViewport + viewportCount;

   assert(firstViewport < MAX_VIEWPORTS);
   assert(total_count >= 1 && total_count <= MAX_VIEWPORTS);

   /* anv allows CmdSetViewPort to change how many viewports are being used,
    * while radv not, using the value set on the pipeline creation. spec
    * doesn't specify, but radv approach makes more sense, as CmdSetViewport
    * is intended to set dynamically a specific viewport, increasing the
    * number of viewport used seems like a non-defined collateral
    * effect. Would make sense to open a spec issue to clarify. For now, as we
    * only support one, it is not really important, but we follow radv
    * approach.
    */
   if (!memcmp(state->dynamic.viewport.viewports + firstViewport,
               pViewports, viewportCount * sizeof(*pViewports))) {
      return;
   }

   memcpy(state->dynamic.viewport.viewports + firstViewport, pViewports,
          viewportCount * sizeof(*pViewports));

   for (uint32_t i = firstViewport; i < firstViewport + viewportCount; i++) {
      v3dv_viewport_compute_xform(&state->dynamic.viewport.viewports[i],
                                  state->dynamic.viewport.scale[i],
                                  state->dynamic.viewport.translate[i]);
   }

   cmd_buffer->state.dirty |= V3DV_CMD_DIRTY_VIEWPORT;
}

void
v3dv_CmdSetScissor(VkCommandBuffer commandBuffer,
                   uint32_t firstScissor,
                   uint32_t scissorCount,
                   const VkRect2D *pScissors)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   const uint32_t total_count = firstScissor + scissorCount;

   assert(firstScissor < MAX_SCISSORS);
   assert(total_count >= 1 && total_count <= MAX_SCISSORS);

   /* See note on CmdSetViewport related to anv/radv differences about setting
    * total viewports used. Also applies to scissor.
    */
   if (!memcmp(state->dynamic.scissor.scissors + firstScissor,
               pScissors, scissorCount * sizeof(*pScissors))) {
      return;
   }

   memcpy(state->dynamic.scissor.scissors + firstScissor, pScissors,
          scissorCount * sizeof(*pScissors));

   cmd_buffer->state.dirty |= V3DV_CMD_DIRTY_SCISSOR;
}

static void
emit_scissor(struct v3dv_cmd_buffer *cmd_buffer)
{
   struct v3dv_dynamic_state *dynamic = &cmd_buffer->state.dynamic;

   /* FIXME: right now we only support one viewport. viewporst[0] would work
    * now, but would need to change if we allow multiple viewports.
    */
   float *vptranslate = dynamic->viewport.translate[0];
   float *vpscale = dynamic->viewport.scale[0];

   float vp_minx = -fabsf(vpscale[0]) + vptranslate[0];
   float vp_maxx = fabsf(vpscale[0]) + vptranslate[0];
   float vp_miny = -fabsf(vpscale[1]) + vptranslate[1];
   float vp_maxy = fabsf(vpscale[1]) + vptranslate[1];

   /* Quoting from v3dx_emit:
    * "Clip to the scissor if it's enabled, but still clip to the
    * drawable regardless since that controls where the binner
    * tries to put things.
    *
    * Additionally, always clip the rendering to the viewport,
    * since the hardware does guardband clipping, meaning
    * primitives would rasterize outside of the view volume."
    */

   VkRect2D clip_window;
   uint32_t minx, miny, maxx, maxy;

   /* From the Vulkan spec:
    *
    * "The application must ensure (using scissor if necessary) that all
    *  rendering is contained within the render area. The render area must be
    *  contained within the framebuffer dimensions."
    *
    * So it is the application's responsibility to ensure this. Still, we can
    * help by automatically restricting the scissor rect to the render area.
    */
   minx = MAX2(vp_minx, cmd_buffer->state.render_area.offset.x);
   miny = MAX2(vp_miny, cmd_buffer->state.render_area.offset.y);
   maxx = MIN2(vp_maxx, cmd_buffer->state.render_area.offset.x +
                        cmd_buffer->state.render_area.extent.width);
   maxy = MIN2(vp_maxy, cmd_buffer->state.render_area.offset.y +
                        cmd_buffer->state.render_area.extent.height);

   /* Clip against user provided scissor if needed.
    *
    * FIXME: right now we only allow one scissor. Below would need to be
    * updated if we support more
    */
   if (dynamic->scissor.count > 0) {
      VkRect2D *scissor = &dynamic->scissor.scissors[0];
      minx = MAX2(minx, scissor->offset.x);
      miny = MAX2(miny, scissor->offset.y);
      maxx = MIN2(maxx, scissor->offset.x + scissor->extent.width);
      maxy = MIN2(maxy, scissor->offset.y + scissor->extent.height);
   }

   clip_window.offset.x = minx;
   clip_window.offset.y = miny;
   clip_window.extent.width = maxx - minx;
   clip_window.extent.height = maxy - miny;

   emit_clip_window(cmd_buffer->state.job, &clip_window);
}

static void
emit_viewport(struct v3dv_cmd_buffer *cmd_buffer)
{
   struct v3dv_dynamic_state *dynamic = &cmd_buffer->state.dynamic;
   /* FIXME: right now we only support one viewport. viewporst[0] would work
    * now, would need to change if we allow multiple viewports
    */
   float *vptranslate = dynamic->viewport.translate[0];
   float *vpscale = dynamic->viewport.scale[0];

   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   cl_emit(&job->bcl, CLIPPER_XY_SCALING, clip) {
      clip.viewport_half_width_in_1_256th_of_pixel = vpscale[0] * 256.0f;
      clip.viewport_half_height_in_1_256th_of_pixel = vpscale[1] * 256.0f;
   }

   cl_emit(&job->bcl, CLIPPER_Z_SCALE_AND_OFFSET, clip) {
      clip.viewport_z_offset_zc_to_zs = vptranslate[2];
      clip.viewport_z_scale_zc_to_zs = vpscale[2];
   }
   cl_emit(&job->bcl, CLIPPER_Z_MIN_MAX_CLIPPING_PLANES, clip) {
      float z1 = (vptranslate[2] - vpscale[2]);
      float z2 = (vptranslate[2] + vpscale[2]);
      clip.minimum_zw = MIN2(z1, z2);
      clip.maximum_zw = MAX2(z1, z2);
   }

   cl_emit(&job->bcl, VIEWPORT_OFFSET, vp) {
      vp.viewport_centre_x_coordinate = vptranslate[0];
      vp.viewport_centre_y_coordinate = vptranslate[1];
   }
}

static void
emit_stencil(struct v3dv_cmd_buffer *cmd_buffer)
{
   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   struct v3dv_pipeline *pipeline = cmd_buffer->state.pipeline;
   struct v3dv_dynamic_state *dynamic_state = &cmd_buffer->state.dynamic;

   const uint32_t dynamic_stencil_states = V3DV_DYNAMIC_STENCIL_COMPARE_MASK |
                                           V3DV_DYNAMIC_STENCIL_WRITE_MASK |
                                           V3DV_DYNAMIC_STENCIL_REFERENCE;

   for (uint32_t i = 0; i < 2; i++) {
      if (pipeline->emit_stencil_cfg[i]) {
         if (dynamic_state->mask & dynamic_stencil_states) {
            cl_emit_with_prepacked(&job->bcl, STENCIL_CFG,
                                   pipeline->stencil_cfg[i], config) {
               if (dynamic_state->mask & V3DV_DYNAMIC_STENCIL_COMPARE_MASK) {
                  config.stencil_test_mask =
                     i == 0 ? dynamic_state->stencil_compare_mask.front :
                              dynamic_state->stencil_compare_mask.back;
               }
               if (dynamic_state->mask & V3DV_DYNAMIC_STENCIL_WRITE_MASK) {
                  config.stencil_write_mask =
                     i == 0 ? dynamic_state->stencil_write_mask.front :
                              dynamic_state->stencil_write_mask.back;
               }
               if (dynamic_state->mask & V3DV_DYNAMIC_STENCIL_REFERENCE) {
                  config.stencil_ref_value =
                     i == 0 ? dynamic_state->stencil_reference.front :
                              dynamic_state->stencil_reference.back;
               }
            }
         } else {
            cl_emit_prepacked(&job->bcl, &pipeline->stencil_cfg[i]);
         }
      }
   }

   const uint32_t dynamic_stencil_dirty_flags =
      V3DV_CMD_DIRTY_STENCIL_COMPARE_MASK |
      V3DV_CMD_DIRTY_STENCIL_WRITE_MASK |
      V3DV_CMD_DIRTY_STENCIL_REFERENCE;
   cmd_buffer->state.dirty &= ~dynamic_stencil_dirty_flags;
}

static void
emit_flat_shade_flags(struct v3dv_job *job,
                      int varying_offset,
                      uint32_t varyings,
                      enum V3DX(Varying_Flags_Action) lower,
                      enum V3DX(Varying_Flags_Action) higher)
{
   cl_emit(&job->bcl, FLAT_SHADE_FLAGS, flags) {
      flags.varying_offset_v0 = varying_offset;
      flags.flat_shade_flags_for_varyings_v024 = varyings;
      flags.action_for_flat_shade_flags_of_lower_numbered_varyings = lower;
      flags.action_for_flat_shade_flags_of_higher_numbered_varyings = higher;
   }
}

static void
emit_noperspective_flags(struct v3dv_job *job,
                         int varying_offset,
                         uint32_t varyings,
                         enum V3DX(Varying_Flags_Action) lower,
                         enum V3DX(Varying_Flags_Action) higher)
{
   cl_emit(&job->bcl, NON_PERSPECTIVE_FLAGS, flags) {
      flags.varying_offset_v0 = varying_offset;
      flags.non_perspective_flags_for_varyings_v024 = varyings;
      flags.action_for_non_perspective_flags_of_lower_numbered_varyings = lower;
      flags.action_for_non_perspective_flags_of_higher_numbered_varyings = higher;
   }
}

static void
emit_centroid_flags(struct v3dv_job *job,
                    int varying_offset,
                    uint32_t varyings,
                    enum V3DX(Varying_Flags_Action) lower,
                    enum V3DX(Varying_Flags_Action) higher)
{
   cl_emit(&job->bcl, CENTROID_FLAGS, flags) {
      flags.varying_offset_v0 = varying_offset;
      flags.centroid_flags_for_varyings_v024 = varyings;
      flags.action_for_centroid_flags_of_lower_numbered_varyings = lower;
      flags.action_for_centroid_flags_of_higher_numbered_varyings = higher;
   }
}

static bool
emit_varying_flags(struct v3dv_job *job,
                   uint32_t num_flags,
                   const uint32_t *flags,
                   void (*flag_emit_callback)(struct v3dv_job *job,
                                              int varying_offset,
                                              uint32_t flags,
                                              enum V3DX(Varying_Flags_Action) lower,
                                              enum V3DX(Varying_Flags_Action) higher))
{
   bool emitted_any = false;
   for (int i = 0; i < num_flags; i++) {
      if (!flags[i])
         continue;

      if (emitted_any) {
        flag_emit_callback(job, i, flags[i],
                           V3D_VARYING_FLAGS_ACTION_UNCHANGED,
                           V3D_VARYING_FLAGS_ACTION_UNCHANGED);
      } else if (i == 0) {
        flag_emit_callback(job, i, flags[i],
                           V3D_VARYING_FLAGS_ACTION_UNCHANGED,
                           V3D_VARYING_FLAGS_ACTION_ZEROED);
      } else {
        flag_emit_callback(job, i, flags[i],
                           V3D_VARYING_FLAGS_ACTION_ZEROED,
                           V3D_VARYING_FLAGS_ACTION_ZEROED);
      }

      emitted_any = true;
   }

   return emitted_any;
}

static void
emit_graphics_pipeline(struct v3dv_cmd_buffer *cmd_buffer)
{
   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   struct v3dv_pipeline *pipeline = state->pipeline;
   assert(pipeline);

   /* Upload the uniforms to the indirect CL first */
   struct v3dv_cl_reloc fs_uniforms =
      v3dv_write_uniforms(cmd_buffer, pipeline->fs);

   struct v3dv_cl_reloc vs_uniforms =
      v3dv_write_uniforms(cmd_buffer, pipeline->vs);

   struct v3dv_cl_reloc vs_bin_uniforms =
      v3dv_write_uniforms(cmd_buffer, pipeline->vs_bin);

   /* Update the cache dirty flag based on the shader progs data */
   job->tmu_dirty_rcl |= pipeline->vs_bin->prog_data.vs->base.tmu_dirty_rcl;
   job->tmu_dirty_rcl |= pipeline->vs->prog_data.vs->base.tmu_dirty_rcl;
   job->tmu_dirty_rcl |= pipeline->fs->prog_data.fs->base.tmu_dirty_rcl;

   /* See GFXH-930 workaround below */
   uint32_t num_elements_to_emit = MAX2(pipeline->va_count, 1);

   uint32_t shader_rec_offset =
      v3dv_cl_ensure_space(&job->indirect,
                           cl_packet_length(GL_SHADER_STATE_RECORD) +
                           num_elements_to_emit *
                           cl_packet_length(GL_SHADER_STATE_ATTRIBUTE_RECORD),
                           32);

   cl_emit_with_prepacked(&job->indirect, GL_SHADER_STATE_RECORD,
                          pipeline->shader_state_record, shader) {

      /* FIXME: we are setting this values here and during the
       * prepacking. This is because both cl_emit_with_prepacked and v3dv_pack
       * asserts for minimum values of these. It would be good to get
       * v3dv_pack to assert on the final value if possible
       */
      shader.min_coord_shader_input_segments_required_in_play =
         pipeline->vpm_cfg_bin.As;
      shader.min_vertex_shader_input_segments_required_in_play =
         pipeline->vpm_cfg.As;

      shader.coordinate_shader_code_address =
         v3dv_cl_address(pipeline->vs_bin->assembly_bo, 0);
      shader.vertex_shader_code_address =
         v3dv_cl_address(pipeline->vs->assembly_bo, 0);
      shader.fragment_shader_code_address =
         v3dv_cl_address(pipeline->fs->assembly_bo, 0);

      shader.coordinate_shader_uniforms_address = vs_bin_uniforms;
      shader.vertex_shader_uniforms_address = vs_uniforms;
      shader.fragment_shader_uniforms_address = fs_uniforms;

      shader.address_of_default_attribute_values =
         v3dv_cl_address(pipeline->default_attribute_values, 0);
   }

   /* Upload vertex element attributes (SHADER_STATE_ATTRIBUTE_RECORD) */
   bool cs_loaded_any = false;
   const uint32_t packet_length =
      cl_packet_length(GL_SHADER_STATE_ATTRIBUTE_RECORD);

   for (uint32_t i = 0; i < pipeline->va_count; i++) {
      uint32_t binding = pipeline->va[i].binding;
      uint32_t location = pipeline->va[i].driver_location;

      struct v3dv_vertex_binding *c_vb = &cmd_buffer->state.vertex_bindings[binding];

      cl_emit_with_prepacked(&job->indirect, GL_SHADER_STATE_ATTRIBUTE_RECORD,
                             &pipeline->vertex_attrs[i * packet_length], attr) {

         assert(c_vb->buffer->mem->bo);
         attr.address = v3dv_cl_address(c_vb->buffer->mem->bo,
                                        c_vb->buffer->mem_offset +
                                        pipeline->va[i].offset +
                                        c_vb->offset);

         attr.number_of_values_read_by_coordinate_shader =
            pipeline->vs_bin->prog_data.vs->vattr_sizes[location];
         attr.number_of_values_read_by_vertex_shader =
            pipeline->vs->prog_data.vs->vattr_sizes[location];

         /* GFXH-930: At least one attribute must be enabled and read by CS
          * and VS.  If we have attributes being consumed by the VS but not
          * the CS, then set up a dummy load of the last attribute into the
          * CS's VPM inputs.  (Since CS is just dead-code-elimination compared
          * to VS, we can't have CS loading but not VS).
          */
         if (pipeline->vs_bin->prog_data.vs->vattr_sizes[location])
            cs_loaded_any = true;

         if (binding == pipeline->va_count - 1 && !cs_loaded_any) {
            attr.number_of_values_read_by_coordinate_shader = 1;
         }

         attr.maximum_index = 0xffffff;
      }
   }

   if (pipeline->va_count == 0) {
      /* GFXH-930: At least one attribute must be enabled and read
       * by CS and VS.  If we have no attributes being consumed by
       * the shader, set up a dummy to be loaded into the VPM.
       */
      cl_emit(&job->indirect, GL_SHADER_STATE_ATTRIBUTE_RECORD, attr) {
         /* Valid address of data whose value will be unused. */
         attr.address = v3dv_cl_address(job->indirect.bo, 0);

         attr.type = ATTRIBUTE_FLOAT;
         attr.stride = 0;
         attr.vec_size = 1;

         attr.number_of_values_read_by_coordinate_shader = 1;
         attr.number_of_values_read_by_vertex_shader = 1;
      }
   }

   cl_emit_prepacked(&job->bcl, &pipeline->vcm_cache_size);

   cl_emit(&job->bcl, GL_SHADER_STATE, state) {
      state.address = v3dv_cl_address(job->indirect.bo,
                                      shader_rec_offset);
      state.number_of_attribute_arrays = num_elements_to_emit;
   }

   cl_emit_with_prepacked(&job->bcl, CFG_BITS, pipeline->cfg_bits, config) {
      config.early_z_updates_enable = job->ez_state != VC5_EZ_DISABLED;
   }

   emit_stencil(cmd_buffer);

   const uint32_t num_flags =
      ARRAY_SIZE(pipeline->fs->prog_data.fs->flat_shade_flags);
   const uint32_t *flat_shade_flags =
      pipeline->fs->prog_data.fs->flat_shade_flags;
   const uint32_t *noperspective_flags =
      pipeline->fs->prog_data.fs->noperspective_flags;
   const uint32_t *centroid_flags =
      pipeline->fs->prog_data.fs->centroid_flags;

   if (!emit_varying_flags(job, num_flags, flat_shade_flags,
                           emit_flat_shade_flags)) {
      cl_emit(&job->bcl, ZERO_ALL_FLAT_SHADE_FLAGS, flags);
   }

   if (!emit_varying_flags(job, num_flags, noperspective_flags,
                           emit_noperspective_flags)) {
      cl_emit(&job->bcl, ZERO_ALL_NON_PERSPECTIVE_FLAGS, flags);
   }

   if (!emit_varying_flags(job, num_flags, centroid_flags,
                           emit_centroid_flags)) {
      cl_emit(&job->bcl, ZERO_ALL_CENTROID_FLAGS, flags);
   }
}

/* FIXME: C&P from v3dx_draw. Refactor to common place? */
static uint32_t
v3d_hw_prim_type(enum pipe_prim_type prim_type)
{
   switch (prim_type) {
   case PIPE_PRIM_POINTS:
   case PIPE_PRIM_LINES:
   case PIPE_PRIM_LINE_LOOP:
   case PIPE_PRIM_LINE_STRIP:
   case PIPE_PRIM_TRIANGLES:
   case PIPE_PRIM_TRIANGLE_STRIP:
   case PIPE_PRIM_TRIANGLE_FAN:
      return prim_type;

   case PIPE_PRIM_LINES_ADJACENCY:
   case PIPE_PRIM_LINE_STRIP_ADJACENCY:
   case PIPE_PRIM_TRIANGLES_ADJACENCY:
   case PIPE_PRIM_TRIANGLE_STRIP_ADJACENCY:
      return 8 + (prim_type - PIPE_PRIM_LINES_ADJACENCY);

   default:
      unreachable("Unsupported primitive type");
   }
}

struct v3dv_draw_info {
   uint32_t vertex_count;
   uint32_t instance_count;
   uint32_t first_vertex;
   uint32_t first_instance;
};

static void
cmd_buffer_emit_draw(struct v3dv_cmd_buffer *cmd_buffer,
                     struct v3dv_draw_info *info)
{
   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   struct v3dv_cmd_buffer_state *state = &cmd_buffer->state;
   struct v3dv_pipeline *pipeline = state->pipeline;

   assert(pipeline);

   uint32_t prim_tf_enable = 0;
   uint32_t hw_prim_type = v3d_hw_prim_type(pipeline->vs->topology);

   /* FIXME: using VERTEX_ARRAY_PRIMS always as it fits our test caselist
    * right now. Need to be choosen based on the current case.
    */
   cl_emit(&job->bcl, VERTEX_ARRAY_PRIMS, prim) {
      prim.mode = hw_prim_type | prim_tf_enable;
      prim.length = info->vertex_count;
      prim.index_of_first_vertex = info->first_vertex;
   }
}

static void
cmd_buffer_emit_pre_draw(struct v3dv_cmd_buffer *cmd_buffer)
{
   /* FIXME: likely to be filtered by really needed states */
   uint32_t *dirty = &cmd_buffer->state.dirty;
   struct v3dv_dynamic_state *dynamic = &cmd_buffer->state.dynamic;

   if (*dirty & (V3DV_CMD_DIRTY_PIPELINE |
                 V3DV_CMD_DIRTY_VERTEX_BUFFER |
                 V3DV_CMD_DIRTY_DESCRIPTOR_SETS |
                 V3DV_CMD_DIRTY_PUSH_CONSTANTS)) {
      emit_graphics_pipeline(cmd_buffer);
   }

   if (*dirty & (V3DV_CMD_DIRTY_VIEWPORT | V3DV_CMD_DIRTY_SCISSOR)) {
      assert(dynamic->scissor.count > 0 || dynamic->viewport.count > 0);
      emit_scissor(cmd_buffer);
   }

   if (*dirty & V3DV_CMD_DIRTY_VIEWPORT) {
      emit_viewport(cmd_buffer);
   }

   const uint32_t dynamic_stencil_dirty_flags =
      V3DV_CMD_DIRTY_STENCIL_COMPARE_MASK |
      V3DV_CMD_DIRTY_STENCIL_WRITE_MASK |
      V3DV_CMD_DIRTY_STENCIL_REFERENCE;
   if (*dirty & dynamic_stencil_dirty_flags)
      emit_stencil(cmd_buffer);

   cmd_buffer->state.dirty &= ~(*dirty);
}

static void
cmd_buffer_draw(struct v3dv_cmd_buffer *cmd_buffer,
                struct v3dv_draw_info *info)
{
   cmd_buffer_emit_pre_draw(cmd_buffer);
   cmd_buffer_emit_draw(cmd_buffer, info);
}

void
v3dv_CmdDraw(VkCommandBuffer commandBuffer,
             uint32_t vertexCount,
             uint32_t instanceCount,
             uint32_t firstVertex,
             uint32_t firstInstance)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   struct v3dv_draw_info info = {};

   info.vertex_count = vertexCount;
   info.instance_count = instanceCount;
   info.first_instance = firstInstance;
   info.first_vertex = firstVertex;

   cmd_buffer_draw(cmd_buffer, &info);
}

void
v3dv_CmdDrawIndexed(VkCommandBuffer commandBuffer,
                    uint32_t indexCount,
                    uint32_t instanceCount,
                    uint32_t firstIndex,
                    int32_t vertexOffset,
                    uint32_t firstInstance)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);

   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   cmd_buffer_emit_pre_draw(cmd_buffer);

   const struct v3dv_pipeline *pipeline = cmd_buffer->state.pipeline;
   uint32_t hw_prim_type = v3d_hw_prim_type(pipeline->vs->topology);
   uint8_t index_type = ffs(cmd_buffer->state.index_size) - 1;
   uint32_t index_offset = firstIndex * cmd_buffer->state.index_size;

   if (vertexOffset != 0 || firstInstance != 0) {
      cl_emit(&job->bcl, BASE_VERTEX_BASE_INSTANCE, base) {
         base.base_instance = firstInstance;
         base.base_vertex = vertexOffset;
      }
   }

   if (instanceCount == 1) {
      cl_emit(&job->bcl, INDEXED_PRIM_LIST, prim) {
         prim.index_type = index_type;
         prim.length = indexCount;
         prim.index_offset = index_offset;
         prim.mode = hw_prim_type;
         prim.enable_primitive_restarts = pipeline->primitive_restart;
      }
   } else if (instanceCount > 1) {
      cl_emit(&job->bcl, INDEXED_INSTANCED_PRIM_LIST, prim) {
         prim.index_type = index_type;
         prim.index_offset = index_offset;
         prim.mode = hw_prim_type;
         prim.enable_primitive_restarts = pipeline->primitive_restart;
         prim.number_of_instances = instanceCount;
         prim.instance_length = indexCount;
      }
   }
}

void
v3dv_CmdDrawIndirect(VkCommandBuffer commandBuffer,
                     VkBuffer _buffer,
                     VkDeviceSize offset,
                     uint32_t drawCount,
                     uint32_t stride)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   V3DV_FROM_HANDLE(v3dv_buffer, buffer, _buffer);

   /* drawCount is the number of draws to execute, and can be zero. */
   if (drawCount == 0)
      return;

   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   cmd_buffer_emit_pre_draw(cmd_buffer);

   const struct v3dv_pipeline *pipeline = cmd_buffer->state.pipeline;
   uint32_t hw_prim_type = v3d_hw_prim_type(pipeline->vs->topology);

   cl_emit(&job->bcl, INDIRECT_VERTEX_ARRAY_INSTANCED_PRIMS, prim) {
      prim.mode = hw_prim_type;
      prim.number_of_draw_indirect_array_records = drawCount;
      prim.stride_in_multiples_of_4_bytes = stride >> 2;
      prim.address = v3dv_cl_address(buffer->mem->bo, offset);
   }
}

void
v3dv_CmdDrawIndexedIndirect(VkCommandBuffer commandBuffer,
                            VkBuffer _buffer,
                            VkDeviceSize offset,
                            uint32_t drawCount,
                            uint32_t stride)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   V3DV_FROM_HANDLE(v3dv_buffer, buffer, _buffer);

   /* drawCount is the number of draws to execute, and can be zero. */
   if (drawCount == 0)
      return;

   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   cmd_buffer_emit_pre_draw(cmd_buffer);

   const struct v3dv_pipeline *pipeline = cmd_buffer->state.pipeline;
   uint32_t hw_prim_type = v3d_hw_prim_type(pipeline->vs->topology);
   uint8_t index_type = ffs(cmd_buffer->state.index_size) - 1;

   cl_emit(&job->bcl, INDIRECT_INDEXED_INSTANCED_PRIM_LIST, prim) {
      prim.index_type = index_type;
      prim.mode = hw_prim_type;
      prim.enable_primitive_restarts = pipeline->primitive_restart;
      prim.number_of_draw_indirect_indexed_records = drawCount;
      prim.stride_in_multiples_of_4_bytes = stride >> 2;
      prim.address = v3dv_cl_address(buffer->mem->bo, offset);
   }
}

void
v3dv_CmdPipelineBarrier(VkCommandBuffer commandBuffer,
                        VkPipelineStageFlags srcStageMask,
                        VkPipelineStageFlags dstStageMask,
                        VkDependencyFlags dependencyFlags,
                        uint32_t memoryBarrierCount,
                        const VkMemoryBarrier *pMemoryBarriers,
                        uint32_t bufferMemoryBarrierCount,
                        const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                        uint32_t imageMemoryBarrierCount,
                        const VkImageMemoryBarrier *pImageMemoryBarriers)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);

   struct v3dv_job *job = cmd_buffer->state.job;
   if (!job)
      return;

   v3dv_cmd_buffer_finish_job(cmd_buffer);
}

void
v3dv_CmdBindVertexBuffers(VkCommandBuffer commandBuffer,
                          uint32_t firstBinding,
                          uint32_t bindingCount,
                          const VkBuffer *pBuffers,
                          const VkDeviceSize *pOffsets)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   struct v3dv_vertex_binding *vb = cmd_buffer->state.vertex_bindings;

   /* We have to defer setting up vertex buffer since we need the buffer
    * stride from the pipeline.
    */

   assert(firstBinding + bindingCount <= MAX_VBS);
   for (uint32_t i = 0; i < bindingCount; i++) {
      vb[firstBinding + i].buffer = v3dv_buffer_from_handle(pBuffers[i]);
      vb[firstBinding + i].offset = pOffsets[i];
   }

   cmd_buffer->state.dirty |= V3DV_CMD_DIRTY_VERTEX_BUFFER;
}

void
v3dv_CmdBindIndexBuffer(VkCommandBuffer commandBuffer,
                        VkBuffer buffer,
                        VkDeviceSize offset,
                        VkIndexType indexType)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   V3DV_FROM_HANDLE(v3dv_buffer, ibuffer, buffer);

   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   cl_emit(&job->bcl, INDEX_BUFFER_SETUP, ib) {
      ib.address = v3dv_cl_address(ibuffer->mem->bo, offset);
      ib.size = ibuffer->mem->bo->size;
   }

   switch (indexType) {
   case VK_INDEX_TYPE_UINT16:
      cmd_buffer->state.index_size = 2;
      break;
   case VK_INDEX_TYPE_UINT32:
      cmd_buffer->state.index_size = 4;
      break;
   default:
      unreachable("Unsupported index type");
   }
}

void
v3dv_CmdSetStencilCompareMask(VkCommandBuffer commandBuffer,
                              VkStencilFaceFlags faceMask,
                              uint32_t compareMask)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);

   if (faceMask & VK_STENCIL_FACE_FRONT_BIT)
      cmd_buffer->state.dynamic.stencil_compare_mask.front = compareMask & 0xff;
   if (faceMask & VK_STENCIL_FACE_BACK_BIT)
      cmd_buffer->state.dynamic.stencil_compare_mask.back = compareMask & 0xff;

   cmd_buffer->state.dirty |= V3DV_CMD_DIRTY_STENCIL_COMPARE_MASK;
}

void
v3dv_CmdSetStencilWriteMask(VkCommandBuffer commandBuffer,
                            VkStencilFaceFlags faceMask,
                            uint32_t writeMask)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);

   if (faceMask & VK_STENCIL_FACE_FRONT_BIT)
      cmd_buffer->state.dynamic.stencil_write_mask.front = writeMask & 0xff;
   if (faceMask & VK_STENCIL_FACE_BACK_BIT)
      cmd_buffer->state.dynamic.stencil_write_mask.back = writeMask & 0xff;

   cmd_buffer->state.dirty |= V3DV_CMD_DIRTY_STENCIL_WRITE_MASK;
}

void
v3dv_CmdSetStencilReference(VkCommandBuffer commandBuffer,
                            VkStencilFaceFlags faceMask,
                            uint32_t reference)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);

   if (faceMask & VK_STENCIL_FACE_FRONT_BIT)
      cmd_buffer->state.dynamic.stencil_reference.front = reference & 0xff;
   if (faceMask & VK_STENCIL_FACE_BACK_BIT)
      cmd_buffer->state.dynamic.stencil_reference.back = reference & 0xff;

   cmd_buffer->state.dirty |= V3DV_CMD_DIRTY_STENCIL_REFERENCE;
}

void
v3dv_CmdBindDescriptorSets(VkCommandBuffer commandBuffer,
                           VkPipelineBindPoint pipelineBindPoint,
                           VkPipelineLayout _layout,
                           uint32_t firstSet,
                           uint32_t descriptorSetCount,
                           const VkDescriptorSet *pDescriptorSets,
                           uint32_t dynamicOffsetCount,
                           const uint32_t *pDynamicOffsets)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   V3DV_FROM_HANDLE(v3dv_pipeline_layout, layout, _layout);

   uint32_t dyn_index = 0;

   assert(pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS);
   assert(firstSet + descriptorSetCount <= MAX_SETS);

   struct v3dv_descriptor_state *descriptor_state =
      &cmd_buffer->state.descriptor_state;

   for (uint32_t i = 0; i < descriptorSetCount; i++) {
      V3DV_FROM_HANDLE(v3dv_descriptor_set, set, pDescriptorSets[i]);
      uint32_t index = firstSet + i;

      descriptor_state->descriptor_sets[index] = set;
      descriptor_state->valid |= (1u << index);

      for (uint32_t j = 0; j < set->layout->dynamic_offset_count; j++, dyn_index++) {
         uint32_t idx = j + layout->set[i + firstSet].dynamic_offset_start;

         descriptor_state->dynamic_offsets[idx] = pDynamicOffsets[dyn_index];
      }
   }

   cmd_buffer->state.dirty |= V3DV_CMD_DIRTY_DESCRIPTOR_SETS;
}

void
v3dv_CmdPushConstants(VkCommandBuffer commandBuffer,
                      VkPipelineLayout layout,
                      VkShaderStageFlags stageFlags,
                      uint32_t offset,
                      uint32_t size,
                      const void *pValues)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);

   memcpy((void*) cmd_buffer->push_constants_data + offset, pValues, size);

   cmd_buffer->state.dirty |= V3DV_CMD_DIRTY_PUSH_CONSTANTS;
}

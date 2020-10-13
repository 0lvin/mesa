/*
 * Copyright © 2019 Raspberry Pi
 *
 * Based in part on v3d driver which is:
 *
 * Copyright © 2014-2017 Broadcom
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
#include "vk_format_info.h"

/*
 * This method checks if the ubo used for push constants is needed to be
 * updated or not.
 *
 * push contants ubo is only used for push constants accessed by a non-const
 * index.
 *
 * FIXME: right now for this cases we are uploading the full
 * push_constants_data. An improvement would be to upload only the data that
 * we need to rely on a UBO.
 */
static void
check_push_constants_ubo(struct v3dv_cmd_buffer *cmd_buffer)
{
   if (!(cmd_buffer->state.dirty & V3DV_CMD_DIRTY_PUSH_CONSTANTS) ||
       cmd_buffer->state.pipeline->layout->push_constant_size == 0)
      return;

   if (cmd_buffer->push_constants_resource.bo == NULL) {
      cmd_buffer->push_constants_resource.bo =
         v3dv_bo_alloc(cmd_buffer->device, MAX_PUSH_CONSTANTS_SIZE,
                       "push constants", true);

      if (!cmd_buffer->push_constants_resource.bo) {
         fprintf(stderr, "Failed to allocate memory for push constants\n");
         abort();
      }

      bool ok = v3dv_bo_map(cmd_buffer->device,
                            cmd_buffer->push_constants_resource.bo,
                            MAX_PUSH_CONSTANTS_SIZE);
      if (!ok) {
         fprintf(stderr, "failed to map push constants buffer\n");
         abort();
      }
   } else {
      if (cmd_buffer->push_constants_resource.offset + MAX_PUSH_CONSTANTS_SIZE <=
          cmd_buffer->push_constants_resource.bo->size) {
         cmd_buffer->push_constants_resource.offset += MAX_PUSH_CONSTANTS_SIZE;
      } else {
         /* FIXME: we got out of space for push descriptors. Should we create
          * a new bo? This could be easier with a uploader
          */
      }
   }

   memcpy(cmd_buffer->push_constants_resource.bo->map +
          cmd_buffer->push_constants_resource.offset,
          cmd_buffer->push_constants_data,
          MAX_PUSH_CONSTANTS_SIZE);

   cmd_buffer->state.dirty &= ~V3DV_CMD_DIRTY_PUSH_CONSTANTS;
}

/** V3D 4.x TMU configuration parameter 0 (texture) */
static void
write_tmu_p0(struct v3dv_cmd_buffer *cmd_buffer,
             struct v3dv_pipeline *pipeline,
             struct v3dv_cl_out **uniforms,
             uint32_t data)
{
   int unit = v3d_unit_data_get_unit(data);
   uint32_t texture_idx;
   struct v3dv_job *job = cmd_buffer->state.job;
   struct v3dv_descriptor_state *descriptor_state =
      &cmd_buffer->state.descriptor_state[v3dv_pipeline_get_binding_point(pipeline)];

   v3dv_pipeline_combined_index_key_unpack(pipeline->combined_index_to_key_map[unit],
                                           &texture_idx,
                                           NULL);

   /* We need to ensure that the texture bo is added to the job */
   struct v3dv_image_view *image_view =
      v3dv_descriptor_map_get_image_view(descriptor_state, &pipeline->texture_map,
                                         pipeline->layout, texture_idx);

   assert(image_view);
   v3dv_job_add_bo(job, image_view->image->mem->bo);

   struct v3dv_cl_reloc state_reloc =
      v3dv_descriptor_map_get_texture_shader_state(descriptor_state,
                                                   &pipeline->texture_map,
                                                   pipeline->layout,
                                                   texture_idx);

   cl_aligned_reloc(&job->indirect, uniforms,
                    state_reloc.bo,
                    state_reloc.offset +
                    v3d_unit_data_get_offset(data));
}

/** V3D 4.x TMU configuration parameter 1 (sampler) */
static void
write_tmu_p1(struct v3dv_cmd_buffer *cmd_buffer,
             struct v3dv_pipeline *pipeline,
             struct v3dv_cl_out **uniforms,
             uint32_t data)
{
   uint32_t unit = v3d_unit_data_get_unit(data);
   uint32_t sampler_idx;
   struct v3dv_job *job = cmd_buffer->state.job;
   struct v3dv_descriptor_state *descriptor_state =
      &cmd_buffer->state.descriptor_state[v3dv_pipeline_get_binding_point(pipeline)];

   v3dv_pipeline_combined_index_key_unpack(pipeline->combined_index_to_key_map[unit],
                                           NULL, &sampler_idx);
   assert(sampler_idx != V3DV_NO_SAMPLER_IDX);

   struct v3dv_cl_reloc sampler_state_reloc =
      v3dv_descriptor_map_get_sampler_state(descriptor_state, &pipeline->sampler_map,
                                            pipeline->layout, sampler_idx);

   cl_aligned_reloc(&job->indirect, uniforms,
                    sampler_state_reloc.bo,
                    sampler_state_reloc.offset +
                    v3d_unit_data_get_offset(data));
}

static void
write_ubo_ssbo_uniforms(struct v3dv_cmd_buffer *cmd_buffer,
                        struct v3dv_pipeline *pipeline,
                        struct v3dv_cl_out **uniforms,
                        enum quniform_contents content,
                        uint32_t data)
{
   struct v3dv_job *job = cmd_buffer->state.job;
   struct v3dv_descriptor_state *descriptor_state =
      &cmd_buffer->state.descriptor_state[v3dv_pipeline_get_binding_point(pipeline)];

   struct v3dv_descriptor_map *map =
      content == QUNIFORM_UBO_ADDR ?
      &pipeline->ubo_map : &pipeline->ssbo_map;

   uint32_t offset =
      content == QUNIFORM_UBO_ADDR ?
      v3d_unit_data_get_offset(data) :
      0;

   uint32_t dynamic_offset = 0;

   /* For ubos, index is shifted, as 0 is reserved for push constants.
    */
   if (content == QUNIFORM_UBO_ADDR &&
       v3d_unit_data_get_unit(data) == 0) {
      /* This calls is to ensure that the push_constant_ubo is
       * updated. It already take into account it is should do the
       * update or not
       */
      check_push_constants_ubo(cmd_buffer);

      struct v3dv_resource *resource =
         &cmd_buffer->push_constants_resource;
      assert(resource->bo);

      cl_aligned_reloc(&job->indirect, uniforms,
                       resource->bo,
                       resource->offset + offset + dynamic_offset);

   } else {
      uint32_t index =
         content == QUNIFORM_UBO_ADDR ?
         v3d_unit_data_get_unit(data) - 1 :
         data;

      struct v3dv_descriptor *descriptor =
         v3dv_descriptor_map_get_descriptor(descriptor_state, map,
                                            pipeline->layout,
                                            index, &dynamic_offset);
      assert(descriptor);
      assert(descriptor->buffer);

      cl_aligned_reloc(&job->indirect, uniforms,
                       descriptor->buffer->mem->bo,
                       descriptor->buffer->mem_offset +
                       descriptor->offset + offset + dynamic_offset);
   }
}

static uint32_t
get_texture_size(struct v3dv_cmd_buffer *cmd_buffer,
                 struct v3dv_pipeline *pipeline,
                 enum quniform_contents contents,
                 uint32_t data)
{
   int unit = v3d_unit_data_get_unit(data);
   uint32_t texture_idx;
   struct v3dv_descriptor_state *descriptor_state =
      &cmd_buffer->state.descriptor_state[v3dv_pipeline_get_binding_point(pipeline)];

   v3dv_pipeline_combined_index_key_unpack(pipeline->combined_index_to_key_map[unit],
                                           &texture_idx,
                                           NULL);

   struct v3dv_image_view *image_view =
      v3dv_descriptor_map_get_image_view(descriptor_state, &pipeline->texture_map,
                                         pipeline->layout, texture_idx);

   assert(image_view);

   switch(contents) {
   case QUNIFORM_TEXTURE_WIDTH:
      /* We don't u_minify the values, as we are using the image_view
       * extents
       */
      return image_view->extent.width;
   case QUNIFORM_TEXTURE_HEIGHT:
      return image_view->extent.height;
   case QUNIFORM_TEXTURE_DEPTH:
      return image_view->extent.depth;
   case QUNIFORM_TEXTURE_ARRAY_SIZE:
      return image_view->last_layer - image_view->first_layer + 1;
   case QUNIFORM_TEXTURE_LEVELS:
      return image_view->max_level - image_view->base_level + 1;
   default:
      unreachable("Bad texture size field");
   }
}

struct v3dv_cl_reloc
v3dv_write_uniforms(struct v3dv_cmd_buffer *cmd_buffer,
                    struct v3dv_pipeline_stage *p_stage)
{
   struct v3d_uniform_list *uinfo =
      &p_stage->current_variant->prog_data.base->uniforms;
   struct v3dv_dynamic_state *dynamic = &cmd_buffer->state.dynamic;
   struct v3dv_pipeline *pipeline = p_stage->pipeline;

   struct v3dv_job *job = cmd_buffer->state.job;
   assert(job);

   /* The hardware always pre-fetches the next uniform (also when there
    * aren't any), so we always allocate space for an extra slot. This
    * fixes MMU exceptions reported since Linux kernel 5.4 when the
    * uniforms fill up the tail bytes of a page in the indirect
    * BO. In that scenario, when the hardware pre-fetches after reading
    * the last uniform it will read beyond the end of the page and trigger
    * the MMU exception.
    */
   v3dv_cl_ensure_space(&job->indirect, (uinfo->count + 1) * 4, 4);

   struct v3dv_cl_reloc uniform_stream = v3dv_cl_get_address(&job->indirect);

   struct v3dv_cl_out *uniforms = cl_start(&job->indirect);

   for (int i = 0; i < uinfo->count; i++) {
      uint32_t data = uinfo->data[i];

      switch (uinfo->contents[i]) {
      case QUNIFORM_CONSTANT:
         cl_aligned_u32(&uniforms, data);
         break;

      case QUNIFORM_UNIFORM:
         assert(pipeline->use_push_constants);
         cl_aligned_u32(&uniforms, cmd_buffer->push_constants_data[data]);
         break;

      case QUNIFORM_VIEWPORT_X_SCALE:
         cl_aligned_f(&uniforms, dynamic->viewport.scale[0][0] * 256.0f);
         break;

      case QUNIFORM_VIEWPORT_Y_SCALE:
         cl_aligned_f(&uniforms, dynamic->viewport.scale[0][1] * 256.0f);
         break;

      case QUNIFORM_VIEWPORT_Z_OFFSET:
         cl_aligned_f(&uniforms, dynamic->viewport.translate[0][2]);
         break;

      case QUNIFORM_VIEWPORT_Z_SCALE:
         cl_aligned_f(&uniforms, dynamic->viewport.scale[0][2]);
         break;

      case QUNIFORM_SSBO_OFFSET:
      case QUNIFORM_UBO_ADDR:
         write_ubo_ssbo_uniforms(cmd_buffer, pipeline, &uniforms,
                                 uinfo->contents[i], data);
        break;

      case QUNIFORM_TMU_CONFIG_P0:
         write_tmu_p0(cmd_buffer, pipeline, &uniforms, data);
         break;

      case QUNIFORM_TMU_CONFIG_P1:
         write_tmu_p1(cmd_buffer, pipeline, &uniforms, data);
         break;

      case QUNIFORM_TEXTURE_WIDTH:
      case QUNIFORM_TEXTURE_HEIGHT:
      case QUNIFORM_TEXTURE_DEPTH:
      case QUNIFORM_TEXTURE_ARRAY_SIZE:
      case QUNIFORM_TEXTURE_LEVELS:
         cl_aligned_u32(&uniforms,
                        get_texture_size(cmd_buffer,
                                         pipeline,
                                         uinfo->contents[i],
                                         data));
         break;

      case QUNIFORM_NUM_WORK_GROUPS:
         assert(job->type == V3DV_JOB_TYPE_GPU_CSD);
         assert(job->csd.workgroup_count[data] > 0);
         cl_aligned_u32(&uniforms, job->csd.workgroup_count[data]);
         break;

      case QUNIFORM_SHARED_OFFSET:
         assert(job->type == V3DV_JOB_TYPE_GPU_CSD);
         assert(job->csd.shared_memory);
         cl_aligned_reloc(&job->indirect, &uniforms, job->csd.shared_memory, 0);
         break;

      default:
         unreachable("unsupported quniform_contents uniform type\n");
      }
   }

   cl_end(&job->indirect, uniforms);

   return uniform_stream;
}

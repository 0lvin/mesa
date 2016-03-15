/* use a gallium context to execute a command buffer */

#include "val_private.h"

#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "val_conv.h"

#include "pipe/p_shader_tokens.h"
#include "tgsi/tgsi_text.h"
#include "tgsi/tgsi_parse.h"

#include "util/u_sampler.h"
struct rendering_state {
   struct pipe_context *pctx;

   bool blend_dirty;
   bool rs_dirty;
   bool dsa_dirty;
   bool stencil_ref_dirty;
   bool clip_state_dirty;
   bool blend_color_dirty;
   bool ve_dirty;
   bool vb_dirty;
   bool constbuf_dirty[PIPE_SHADER_TYPES];
   bool vp_dirty;
   struct pipe_draw_info info;

   struct pipe_framebuffer_state framebuffer;

   struct pipe_blend_state blend_state;
   void *blend_handle;
   struct pipe_rasterizer_state rs_state;
   void *rast_handle;
   struct pipe_depth_stencil_alpha_state dsa_state;
   void *dsa_handle;

   struct pipe_blend_color blend_color;
   struct pipe_stencil_ref stencil_ref;
   struct pipe_clip_state clip_state;

   int num_scissors;
   struct pipe_scissor_state *scissors;

   int num_viewports;
   struct pipe_viewport_state viewports[16];

   struct pipe_index_buffer index_buffer;

   struct pipe_constant_buffer const_buffer[PIPE_SHADER_TYPES][16];
   int num_const_bufs;
   int start_vb, num_vb;
   struct pipe_vertex_buffer vb[PIPE_MAX_ATTRIBS];
   int num_ve;
   struct pipe_vertex_element ve[PIPE_MAX_ATTRIBS];

   struct pipe_sampler_view *sv[PIPE_SHADER_TYPES][PIPE_MAX_SAMPLERS];
   int num_sampler_views[PIPE_SHADER_TYPES];
   struct pipe_sampler_state ss[PIPE_SHADER_TYPES][PIPE_MAX_SAMPLERS];
   int num_sampler_states[PIPE_SHADER_TYPES];
   bool sv_dirty[PIPE_SHADER_TYPES];
   bool ss_dirty[PIPE_SHADER_TYPES];

   void *ss_cso[PIPE_SHADER_TYPES][16];
   void *velems_cso;
   void *shader_cso[PIPE_SHADER_TYPES];
};

static VkResult emit_state(struct rendering_state *state)
{
   int sh;
   if (state->blend_dirty) {
      if (state->blend_handle)
         state->pctx->delete_blend_state(state->pctx, state->blend_handle);
      state->blend_handle = state->pctx->create_blend_state(state->pctx,
                                                            &state->blend_state);
      state->pctx->bind_blend_state(state->pctx, state->blend_handle);

      state->blend_dirty = false;
   }

   if (state->rs_dirty) {
      if (state->rast_handle)
         state->pctx->delete_rasterizer_state(state->pctx, state->rast_handle); 
      state->rast_handle = state->pctx->create_rasterizer_state(state->pctx,
                                                                &state->rs_state);
      state->pctx->bind_rasterizer_state(state->pctx, state->rast_handle);
      state->rs_dirty = false;
   }

   if (state->dsa_dirty) {
      if (state->dsa_handle)
         state->pctx->delete_depth_stencil_alpha_state(state->pctx, state->dsa_handle);
      state->dsa_handle = state->pctx->create_depth_stencil_alpha_state(state->pctx,
                                                                        &state->dsa_state);
      state->pctx->bind_depth_stencil_alpha_state(state->pctx, state->dsa_handle);

      state->dsa_dirty = false;
   }

   if (state->stencil_ref_dirty) {
      state->pctx->set_stencil_ref(state->pctx, &state->stencil_ref);
      state->stencil_ref_dirty = false;
   }

   if (state->vb_dirty) {
      state->pctx->set_vertex_buffers(state->pctx, state->start_vb,
                                      state->num_vb, state->vb);
      state->vb_dirty = false;
   }

   if (state->ve_dirty) {
      void *ve = NULL;
      if (state->velems_cso)
         ve = state->velems_cso;

      state->velems_cso = state->pctx->create_vertex_elements_state(state->pctx, state->num_ve,
                                                                    state->ve);
      state->pctx->bind_vertex_elements_state(state->pctx, state->velems_cso);
//      state->pctx->delete_vertex_elements_state(state->pctx, ve);

      if (ve)
         state->pctx->delete_vertex_elements_state(state->pctx, ve);
   }

   if (state->constbuf_dirty[PIPE_SHADER_VERTEX]) {
      state->pctx->set_constant_buffer(state->pctx, PIPE_SHADER_VERTEX,
                                       1, state->const_buffer[PIPE_SHADER_VERTEX]);
   }

   for (sh = 0; sh < PIPE_SHADER_TYPES; sh++) {

      if (!state->sv_dirty[sh])
         continue;

      state->pctx->set_sampler_views(state->pctx, sh, 0, state->num_sampler_views[sh],
                                     state->sv[sh]);
      state->sv_dirty[sh] = false;
   }

   for (sh = 0; sh < PIPE_SHADER_TYPES; sh++) {
      void *ss[16] = { 0 };
      int i;
      if (!state->ss_dirty[sh])
         continue;

      for (i = 0; i < state->num_sampler_states[sh]; i++) {
         if (state->ss_cso[sh][i])
            ss[i] = state->ss_cso[sh][i];

         state->ss_cso[sh][i] = state->pctx->create_sampler_state(state->pctx, &state->ss[sh][i]);
      }

      state->pctx->bind_sampler_states(state->pctx, sh, 0, state->num_sampler_states[sh], state->ss_cso[sh]);
   }

   if (state->vp_dirty) {
      state->pctx->set_viewport_states(state->pctx, 0, 1, state->viewports);
      state->vp_dirty = false;
   }
   return VK_SUCCESS;
}

static void *parse_fragment_shader(struct pipe_context *pctx,
                                  const char *text)
{
   struct tgsi_token tokens[1024];
   struct pipe_shader_state state;

   if (!tgsi_text_translate(text, tokens, ARRAY_SIZE(tokens)))
      return NULL;

   memset(&state, 0, sizeof(state));
   state.tokens = tokens;
   return pctx->create_fs_state(pctx, &state);
}

static void *parse_vertex_shader(struct pipe_context *pctx,
                                  const char *text)
{
   struct tgsi_token tokens[1024];
   struct pipe_shader_state state;

   if (!tgsi_text_translate(text, tokens, ARRAY_SIZE(tokens)))
      return NULL;

   memset(&state, 0, sizeof(state));
   state.tokens = tokens;
   return pctx->create_vs_state(pctx, &state);
}


static VkResult handle_pipeline(struct val_cmd_buffer_entry *cmd,
                                struct rendering_state *state)
{
   struct val_pipeline *pipeline = cmd->u.pipeline.pipeline;
   bool dynamic_state_viewport = false, dynamic_state_scissor = false;

   if (pipeline->create_info.pDynamicState)
   {
      const VkPipelineDynamicStateCreateInfo *dyn = pipeline->create_info.pDynamicState;
      int i;
      for (i = 0; i < dyn->dynamicStateCount; i++) {
         if (dyn->pDynamicStates[i] == VK_DYNAMIC_STATE_VIEWPORT)
            dynamic_state_viewport = true;
         else if (dyn->pDynamicStates[i] == VK_DYNAMIC_STATE_SCISSOR)
            dynamic_state_scissor = true;
      }
   }

   {
      int i;
      for (i = 0; i < pipeline->create_info.stageCount; i++) {
         const VkPipelineShaderStageCreateInfo *sh = &pipeline->create_info.pStages[i];
         void *shader;
	 struct pipe_shader_state shstate;
	 memset(&shstate, 0, sizeof(shstate));
	 VAL_FROM_HANDLE(val_shader_module, module,
			 sh->module);
         switch (sh->stage) {
         case VK_SHADER_STAGE_FRAGMENT_BIT:
            shstate.tokens = pipeline->pipeline_tgsi[MESA_SHADER_FRAGMENT];

	    state->shader_cso[PIPE_SHADER_FRAGMENT] = state->pctx->create_fs_state(state->pctx, &shstate);
            state->pctx->bind_fs_state(state->pctx, state->shader_cso[PIPE_SHADER_FRAGMENT]);
            break;
         case VK_SHADER_STAGE_VERTEX_BIT:
            shstate.tokens = pipeline->pipeline_tgsi[MESA_SHADER_VERTEX];
            state->shader_cso[PIPE_SHADER_VERTEX] = state->pctx->create_vs_state(state->pctx, &shstate);
            state->pctx->bind_vs_state(state->pctx, state->shader_cso[PIPE_SHADER_VERTEX]);
            break;
         default:
            assert(0);
            break;
         }
      }
   }
   /* rasterization state */
   if (pipeline->create_info.pRasterizationState) {
      const VkPipelineRasterizationStateCreateInfo *rsc = pipeline->create_info.pRasterizationState;
      state->rs_state.depth_clip = rsc->depthClampEnable;
      state->rs_state.rasterizer_discard = rsc->rasterizerDiscardEnable;
      state->rs_state.front_ccw = (rsc->frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE);
      state->rs_state.cull_face = vk_cull_to_pipe(rsc->cullMode);
      state->rs_state.fill_front = vk_polygon_mode_to_pipe(rsc->polygonMode);
      state->rs_state.fill_back = vk_polygon_mode_to_pipe(rsc->polygonMode);
      state->rs_dirty = true;
   }

   if (pipeline->create_info.pDepthStencilState) {
      const VkPipelineDepthStencilStateCreateInfo *dsa = pipeline->create_info.pDepthStencilState;

      state->dsa_state.depth.enabled = dsa->depthTestEnable;
      state->dsa_state.depth.writemask = dsa->depthWriteEnable;
      state->dsa_state.depth.func = dsa->depthCompareOp;
      state->dsa_state.depth.bounds_test = dsa->depthBoundsTestEnable;
      state->dsa_state.depth.bounds_min = dsa->minDepthBounds;
      state->dsa_state.depth.bounds_max = dsa->maxDepthBounds;
      
      state->dsa_state.stencil[0].enabled = dsa->stencilTestEnable;
      state->dsa_state.stencil[0].func = dsa->front.compareOp;
      state->dsa_state.stencil[0].fail_op = vk_conv_stencil_op(dsa->front.failOp);
      state->dsa_state.stencil[0].zpass_op = vk_conv_stencil_op(dsa->front.passOp);
      state->dsa_state.stencil[0].zfail_op = vk_conv_stencil_op(dsa->front.depthFailOp);
      state->dsa_state.stencil[0].valuemask = dsa->front.compareMask;
      state->dsa_state.stencil[0].writemask = dsa->front.writeMask;
      
      state->dsa_state.stencil[1].enabled = dsa->stencilTestEnable;
      state->dsa_state.stencil[1].func = dsa->back.compareOp;
      state->dsa_state.stencil[1].fail_op = vk_conv_stencil_op(dsa->back.failOp);
      state->dsa_state.stencil[1].zpass_op = vk_conv_stencil_op(dsa->back.passOp);
      state->dsa_state.stencil[1].zfail_op = vk_conv_stencil_op(dsa->back.depthFailOp);
      state->dsa_state.stencil[1].valuemask = dsa->back.compareMask;
      state->dsa_state.stencil[1].writemask = dsa->back.writeMask;

      if (dsa->stencilTestEnable) {
         state->stencil_ref.ref_value[0] = dsa->front.reference;
         state->stencil_ref.ref_value[1] = dsa->back.reference;
         state->stencil_ref_dirty = true;
      }

      state->dsa_dirty = true;
   }

   if (pipeline->create_info.pColorBlendState) {
      const VkPipelineColorBlendStateCreateInfo *cb = pipeline->create_info.pColorBlendState;
      int i;
      if (cb->attachmentCount > 1)
         state->blend_state.independent_blend_enable = true;
      for (i = 0; i < cb->attachmentCount; i++) {
         state->blend_state.rt[i].colormask = cb->pAttachments[i].colorWriteMask;
      }
      state->blend_dirty = true;
   }

   {
      const VkPipelineVertexInputStateCreateInfo *vi = pipeline->create_info.pVertexInputState;
      int i;

      for (i = 0; i < vi->vertexBindingDescriptionCount; i++) {
         state->vb[i].stride = vi->pVertexBindingDescriptions[i].stride;
      }

      for (i = 0; i < vi->vertexAttributeDescriptionCount; i++) {
         state->ve[i].src_offset = vi->pVertexAttributeDescriptions[i].offset;
         state->ve[i].vertex_buffer_index = vi->pVertexAttributeDescriptions[i].binding;
         state->ve[i].src_format = vk_format_to_pipe(vi->pVertexAttributeDescriptions[i].format);
      }
      state->num_ve = vi->vertexAttributeDescriptionCount;
      state->vb_dirty = true;
      state->ve_dirty = true;
   }

   {
      const VkPipelineInputAssemblyStateCreateInfo *ia = pipeline->create_info.pInputAssemblyState;

      state->info.mode = vk_conv_topology(ia->topology);
      state->info.primitive_restart = ia->primitiveRestartEnable;
   }

   if (pipeline->create_info.pViewportState) {
      const VkPipelineViewportStateCreateInfo *vpi= pipeline->create_info.pViewportState;
      int i;

      if (!dynamic_state_viewport) {
         for (i = 0; i < vpi->viewportCount; i++) {
            const VkViewport *vp = &vpi->pViewports[i];
            state->viewports[i].scale[0] = vp->width / 2;
            state->viewports[i].scale[1] = vp->height / 2;
            state->viewports[i].scale[2] = 1.0;
            state->viewports[i].translate[0] = vp->x + vp->width / 2;
            state->viewports[i].translate[1] = vp->y + vp->height / 2;
            state->viewports[i].translate[2] = 0.0;
         }
         state->vp_dirty = true;
      }
   }
   return VK_SUCCESS;
}

static VkResult handle_vertex_buffers(struct val_cmd_buffer_entry *cmd,
                                      struct rendering_state *state)
{
   int i;
   struct val_cmd_bind_vertex_buffers *vcb = &cmd->u.vertex_buffers;
   for (i = 0; i < vcb->binding_count; i++) {
      int idx = i + vcb->first;

      state->vb[idx].buffer_offset = vcb->offsets[i];
      state->vb[idx].buffer = vcb->buffers[i]->bo;
   }
   state->start_vb = vcb->first;
   state->num_vb = vcb->binding_count;
   state->vb_dirty = true;
   return VK_SUCCESS;
}

static VkResult handle_descriptor_sets(struct val_cmd_buffer_entry *cmd,
                                       struct rendering_state *state)
{
   struct val_cmd_bind_descriptor_sets *bds = &cmd->u.descriptor_sets;
   int i;
   int j;
   state->num_const_bufs = 0;
   state->num_sampler_views[PIPE_SHADER_VERTEX] = 0;
   state->num_sampler_views[PIPE_SHADER_FRAGMENT] = 0;
   for (i = 0; i < bds->count; i++) {
      const struct val_descriptor_set *set = bds->sets[i];

      if (set->layout->shader_stages & VK_SHADER_STAGE_VERTEX_BIT) {
         int sidx = PIPE_SHADER_VERTEX;
         for (j = 0; j < set->buffer_count; j++) {
            int idx = state->num_const_bufs;
            if (set->descriptors[j].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
               state->const_buffer[sidx][idx].buffer = set->buffer_views->bo;
               state->const_buffer[sidx][idx].buffer_offset = set->buffer_views->offset;
               state->const_buffer[sidx][idx].buffer_size = set->buffer_views->range;
            }
         }
         state->constbuf_dirty[sidx] = true;
      }

      if (set->layout->shader_stages & VK_SHADER_STAGE_FRAGMENT_BIT) {
         int sidx = PIPE_SHADER_FRAGMENT;
         int j;
         for (j = 0; j < set->layout->binding_count; j++) {
            if (set->descriptors[j].type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
               struct val_image_view *iv = set->descriptors[j].image_view;
               struct val_sampler *samp = set->descriptors[j].sampler;
               struct pipe_sampler_view templ;
               state->ss[sidx][i].wrap_s = vk_conv_wrap_mode(samp->create_info.addressModeU);
               state->ss[sidx][i].wrap_t = vk_conv_wrap_mode(samp->create_info.addressModeV);
               state->ss[sidx][i].wrap_r = vk_conv_wrap_mode(samp->create_info.addressModeW);
               state->ss[sidx][i].min_img_filter = PIPE_TEX_FILTER_NEAREST;
               state->ss[sidx][i].min_lod = samp->create_info.minLod;
               state->ss[sidx][i].max_lod = samp->create_info.maxLod;
               state->ss[sidx][i].normalized_coords = !samp->create_info.unnormalizedCoordinates;

               state->num_sampler_states[sidx]++;

               u_sampler_view_default_template(&templ,
                                               iv->image->bo,
                                               vk_format_to_pipe(iv->format));

               state->sv[sidx][i] = state->pctx->create_sampler_view(state->pctx, iv->image->bo, &templ);
               state->ss_dirty[sidx] = true;
               state->num_sampler_views[sidx]++;

               state->sv_dirty[sidx] = true;
            }
         }
      }
   }

   return VK_SUCCESS;
}

static VkResult handle_begin_render_pass(struct val_cmd_buffer_entry *cmd,
                                         struct rendering_state *state)
{
   int i;
   /* this will set framebuffer state and do initial clearing */
   state->framebuffer.width = cmd->u.begin_render_pass.framebuffer->width;
   state->framebuffer.height = cmd->u.begin_render_pass.framebuffer->height;

   state->framebuffer.nr_cbufs = 0;
   for (i = 0 ; i < cmd->u.begin_render_pass.framebuffer->attachment_count; i++) {
      struct val_image_view *imgv = cmd->u.begin_render_pass.framebuffer->attachments[i];

      if (!imgv->surface) {
         struct pipe_surface template;

         memset(&template, 0, sizeof(struct pipe_surface));

         template.format = vk_format_to_pipe(cmd->u.begin_render_pass.render_pass->attachments[i].format);
         template.width = cmd->u.begin_render_pass.framebuffer->width;
         template.height = cmd->u.begin_render_pass.framebuffer->height;

         if (template.format == PIPE_FORMAT_NONE)
            return;
         imgv->surface = state->pctx->create_surface(state->pctx,
                                                     imgv->image->bo, &template);
      }
      if (imgv->subresourceRange.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
         /* use as ZS buffers */
         state->framebuffer.zsbuf = imgv->surface;
      } else if (imgv->subresourceRange.aspectMask & (VK_IMAGE_ASPECT_COLOR_BIT)) {
         state->framebuffer.cbufs[state->framebuffer.nr_cbufs] = imgv->surface;
         state->framebuffer.nr_cbufs++;
      }
   }

   state->pctx->set_framebuffer_state(state->pctx,
                                      &state->framebuffer);

   if (cmd->u.begin_render_pass.render_pass->attachment_count) {
      int i;

      for (i = 0; i < cmd->u.begin_render_pass.render_pass->attachment_count; i++) {
         if (cmd->u.begin_render_pass.render_pass->attachments[i].load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            union pipe_color_union color;
            const VkClearValue value = cmd->u.begin_render_pass.clear_values[i];
            double dclear_val = value.depthStencil.depth;
            uint32_t sclear_val = value.depthStencil.stencil;

            if (cmd->u.begin_render_pass.render_pass->attachments[i].final_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
               color.ui[0] = value.color.uint32[0];
               color.ui[1] = value.color.uint32[1];
               color.ui[2] = value.color.uint32[2];
               color.ui[3] = value.color.uint32[3];
               state->pctx->clear(state->pctx,
                                  PIPE_CLEAR_COLOR,
                                  &color, dclear_val, sclear_val);
            } else if (cmd->u.begin_render_pass.render_pass->attachments[i].final_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
               state->pctx->clear(state->pctx,
                                  PIPE_CLEAR_DEPTHSTENCIL,
                                  &color, dclear_val, sclear_val);
            }
         }
      }
   }
   return VK_SUCCESS;
}

static VkResult handle_end_render_pass(struct val_cmd_buffer_entry *cmd,
                                struct rendering_state *state)
{
   state->pctx->flush(state->pctx, NULL, 0);
   return VK_SUCCESS;
}

static VkResult handle_draw(struct val_cmd_buffer_entry *cmd,
                            struct rendering_state *state)
{
   state->info.start = cmd->u.draw.first_vertex;
   state->info.count = cmd->u.draw.vertex_count;
   state->info.start_instance = cmd->u.draw.first_instance;
   state->info.instance_count = cmd->u.draw.instance_count;
   state->pctx->draw_vbo(state->pctx, &state->info);
   return VK_SUCCESS;
}

static VkResult handle_dyn_set_viewport(struct val_cmd_buffer_entry *cmd,
                                        struct rendering_state *state)
{
   int i;

   for (i = 0; i < cmd->u.dyn_set_viewport.viewport_count; i++) {
      int idx = i + cmd->u.dyn_set_viewport.first_viewport;
      const VkViewport *vp = &cmd->u.dyn_set_viewport.viewports[i];
      state->viewports[idx].scale[0] = vp->width / 2;
      state->viewports[idx].scale[1] = vp->height / 2;
      state->viewports[idx].scale[2] = 1.0;
      state->viewports[idx].translate[0] = vp->x + vp->width / 2;
      state->viewports[idx].translate[1] = vp->y + vp->height / 2;
      state->viewports[idx].translate[2] = 0.0;
   }
   state->vp_dirty = true;
   return VK_SUCCESS;
}

static VkResult handle_copy_image_to_buffer(struct val_cmd_buffer_entry *cmd,
                                            struct rendering_state *state)
{
   int i;
   struct val_cmd_copy_image_to_buffer *copycmd = &cmd->u.img_to_buffer;
   struct pipe_blit_info info;

   memset(&info, 0, sizeof(info));

   info.src.resource = copycmd->src->bo;
   info.dst.resource = copycmd->dst->bo;

   for (i = 0; i < copycmd->region_count; i++) {

      info.src.level = copycmd->regions[i].imageSubresource.mipLevel;
      info.src.box.x = copycmd->regions[i].imageOffset.x;
      info.src.box.y = copycmd->regions[i].imageOffset.y;
      info.src.box.z = copycmd->regions[i].imageOffset.z;
      info.src.box.width = copycmd->regions[i].imageExtent.width;
      info.src.box.height = copycmd->regions[i].imageExtent.height;
      info.src.box.depth = copycmd->regions[i].imageExtent.depth;

      info.dst.box = info.src.box;
      info.mask = PIPE_MASK_RGBA;
      state->pctx->blit(state->pctx, &info);
   }

   return VK_SUCCESS;
}

VkResult val_execute_cmds(struct val_device *device,
                          struct val_cmd_buffer *cmd_buffer)
{
   struct val_cmd_buffer_entry *cmd;
   struct rendering_state state;

   memset(&state, 0, sizeof(state));
   state.pctx = device->pscreen->context_create(device->pscreen,
                                                NULL, PIPE_CONTEXT_ROBUST_BUFFER_ACCESS);

   if (!state.pctx)
      return VK_ERROR_INITIALIZATION_FAILED;

   state.blend_dirty = true;
   state.dsa_dirty = true;
   /* create a gallium context */
   LIST_FOR_EACH_ENTRY(cmd, &cmd_buffer->cmds, cmd_link) {
      fprintf(stderr, "cmd type %d\n", cmd->cmd_type);
      switch (cmd->cmd_type) {
      case VAL_CMD_BIND_PIPELINE:
         handle_pipeline(cmd, &state);
         break;
      case VAL_CMD_BIND_VERTEX_BUFFERS:
         handle_vertex_buffers(cmd, &state);
         break;
      case VAL_CMD_BIND_DESCRIPTOR_SETS:
         handle_descriptor_sets(cmd, &state);
         break;
      case VAL_CMD_BEGIN_RENDER_PASS:
         handle_begin_render_pass(cmd, &state);
         break;
      case VAL_CMD_END_RENDER_PASS:
         handle_end_render_pass(cmd, &state);
         break;
      case VAL_CMD_DRAW:
         emit_state(&state);
         handle_draw(cmd, &state);
         break;
      case VAL_CMD_DYN_SET_VIEWPORT:
         handle_dyn_set_viewport(cmd, &state);
         break;
      case VAL_CMD_COPY_IMAGE_TO_BUFFER:
         handle_copy_image_to_buffer(cmd, &state);
         break;
      }
   }

   state.pctx->bind_vertex_elements_state(state.pctx, NULL);
   state.pctx->bind_vs_state(state.pctx, NULL);
   state.pctx->bind_fs_state(state.pctx, NULL);
   if (state.velems_cso)
      state.pctx->delete_vertex_elements_state(state.pctx, state.velems_cso);
   if (state.shader_cso[PIPE_SHADER_VERTEX])
      state.pctx->delete_vs_state(state.pctx, state.shader_cso[PIPE_SHADER_VERTEX]);
   if (state.shader_cso[PIPE_SHADER_FRAGMENT])
   state.pctx->delete_fs_state(state.pctx, state.shader_cso[PIPE_SHADER_FRAGMENT]);
   state.pctx->destroy(state.pctx);
   return VK_SUCCESS;
}

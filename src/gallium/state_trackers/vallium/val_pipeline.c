#include "val_private.h"

#include "glsl_types.h"
#include "nir/spirv/nir_spirv.h"
#include "nir/nir_to_tgsi.h"

#include "tgsi/tgsi_parse.h"
#include "tgsi/tgsi_dump.h"
#define SPIR_V_MAGIC_NUMBER 0x07230203
VkResult val_CreateShaderModule(
    VkDevice                                    _device,
    const VkShaderModuleCreateInfo*             pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkShaderModule*                             pShaderModule)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   struct val_shader_module *module;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
   assert(pCreateInfo->flags == 0);

   module = val_alloc2(&device->alloc, pAllocator,
                       sizeof(*module) + pCreateInfo->codeSize, 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (module == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   module->size = pCreateInfo->codeSize;
   memcpy(module->data, pCreateInfo->pCode, module->size);

   *pShaderModule = val_shader_module_to_handle(module);

   return VK_SUCCESS;

}

void val_DestroyShaderModule(
    VkDevice                                    _device,
    VkShaderModule                              _module,
    const VkAllocationCallbacks*                pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_shader_module, module, _module);

   if (module->tgsi)
      tgsi_free_tokens(module->tgsi);
   val_free2(&device->alloc, pAllocator, module);
}

void val_DestroyPipeline(
    VkDevice                                    _device,
    VkPipeline                                  _pipeline,
    const VkAllocationCallbacks*                pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_pipeline, pipeline, _pipeline);
   val_free2(&device->alloc, pAllocator, pipeline);
}

static VkResult
deep_copy_shader_stage(struct VkPipelineShaderStageCreateInfo *dst,
                       const struct VkPipelineShaderStageCreateInfo *src)
{
   dst->sType = src->sType;
   dst->pNext = NULL;
   dst->flags = src->flags;
   dst->stage = src->stage;
   dst->module = src->module;
   dst->pName = src->pName;
   if (src->pSpecializationInfo) {
      val_finishme("specialization info");
   }
   return VK_SUCCESS;
}

static VkResult
deep_copy_vertex_input_state(struct VkPipelineVertexInputStateCreateInfo *dst,
                             const struct VkPipelineVertexInputStateCreateInfo *src)
{
   int i;
   VkVertexInputBindingDescription *dst_binding_descriptions;
   VkVertexInputAttributeDescription *dst_attrib_descriptions;
   dst->sType = src->sType;
   dst->pNext = NULL;
   dst->flags = src->flags;
   dst->vertexBindingDescriptionCount = src->vertexBindingDescriptionCount;

   dst_binding_descriptions = malloc(src->vertexBindingDescriptionCount * sizeof(VkVertexInputBindingDescription));
   if (!dst_binding_descriptions)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   for (i = 0; i < dst->vertexBindingDescriptionCount; i++) {
      memcpy(&dst_binding_descriptions[i], &src->pVertexBindingDescriptions[i], sizeof(VkVertexInputBindingDescription));
   }
   dst->pVertexBindingDescriptions = dst_binding_descriptions;
   
   dst->vertexAttributeDescriptionCount = src->vertexAttributeDescriptionCount;
   
   dst_attrib_descriptions = malloc(src->vertexAttributeDescriptionCount * sizeof(VkVertexInputAttributeDescription));
   if (!dst_attrib_descriptions)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   
   for (i = 0; i < dst->vertexAttributeDescriptionCount; i++) {
      memcpy(&dst_attrib_descriptions[i], &src->pVertexAttributeDescriptions[i], sizeof(VkVertexInputAttributeDescription));
   }
   dst->pVertexAttributeDescriptions = dst_attrib_descriptions;
   return VK_SUCCESS;
}

static VkResult
deep_copy_viewport_state(VkPipelineViewportStateCreateInfo *dst,
                         const VkPipelineViewportStateCreateInfo *src)
{
   int i;
   VkViewport *viewports;
   VkRect2D *scissors;
   dst->sType = src->sType;
   dst->pNext = src->pNext;

   dst->flags = src->flags;

   if (src->pViewports) {
      viewports = malloc(src->viewportCount * sizeof(VkViewport));
      for (i = 0; i < src->viewportCount; i++)
         memcpy(&viewports[i], &src->pViewports[i], sizeof(VkViewport));
      dst->pViewports = viewports;
   }
   dst->viewportCount = src->viewportCount;

   if (src->pScissors) {
      scissors = malloc(src->scissorCount * sizeof(VkRect2D));
      for (i = 0; i < src->scissorCount; i++)
         memcpy(&scissors[i], &src->pScissors[i], sizeof(VkRect2D));
      dst->pScissors = scissors;
   }
   dst->scissorCount = src->scissorCount;

   return VK_SUCCESS;
}

static VkResult
deep_copy_color_blend_state(VkPipelineColorBlendStateCreateInfo *dst,
                            const VkPipelineColorBlendStateCreateInfo *src)
{
   VkPipelineColorBlendAttachmentState *attachments;
   dst->sType = src->sType;
   dst->pNext = src->pNext;
   dst->flags = src->flags;
   dst->logicOpEnable = src->logicOpEnable;
   dst->logicOp = src->logicOp;

   attachments = malloc(src->attachmentCount * sizeof(VkPipelineColorBlendAttachmentState));
   memcpy(attachments, src->pAttachments, src->attachmentCount * sizeof(VkPipelineColorBlendAttachmentState));
   dst->attachmentCount = src->attachmentCount;
   dst->pAttachments = attachments;
   
   memcpy(&dst->blendConstants, &src->blendConstants, sizeof(float) * 4);
          
   return VK_SUCCESS;
}

static VkResult
deep_copy_dynamic_state(VkPipelineDynamicStateCreateInfo *dst,
                        const VkPipelineDynamicStateCreateInfo *src)
{
   VkDynamicState *dynamic_states;
   dst->sType = src->sType;
   dst->pNext = src->pNext;
   dst->flags = src->flags;

   dynamic_states = malloc(src->dynamicStateCount * sizeof(VkDynamicState));
   if (!dynamic_states)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   memcpy(dynamic_states, src->pDynamicStates, src->dynamicStateCount * sizeof(VkDynamicState));
   dst->dynamicStateCount = src->dynamicStateCount;
   dst->pDynamicStates = dynamic_states;
   return VK_SUCCESS;
}

static VkResult
deep_copy_create_info(VkGraphicsPipelineCreateInfo *dst,
                      const VkGraphicsPipelineCreateInfo *src)
{
   int i;
   VkResult result;
   VkPipelineShaderStageCreateInfo *stages;
   VkPipelineVertexInputStateCreateInfo *vertex_input;
   VkPipelineInputAssemblyStateCreateInfo *input_assembly;
   VkPipelineRasterizationStateCreateInfo* raster_state;

   dst->sType = src->sType;
   dst->pNext = NULL;
   dst->flags = src->flags;
   dst->layout = src->layout;
   dst->renderPass = src->renderPass;
   dst->subpass = src->subpass;
   dst->basePipelineHandle = src->basePipelineHandle;
   dst->basePipelineIndex = src->basePipelineIndex;

   /* pStages */
   dst->stageCount = src->stageCount;
   stages = malloc(dst->stageCount * sizeof(VkPipelineShaderStageCreateInfo));
   for (i = 0 ; i < dst->stageCount; i++) {
      result = deep_copy_shader_stage(&stages[i], &src->pStages[i]);
      if (result != VK_SUCCESS)
         return result;
   }
   dst->pStages = stages;

   /* pVertexInputState */
   vertex_input = malloc(sizeof(VkPipelineVertexInputStateCreateInfo));
   result = deep_copy_vertex_input_state(vertex_input,
                                         src->pVertexInputState);
   if (result != VK_SUCCESS)
      return result;
   dst->pVertexInputState = vertex_input;

   /* pInputAssemblyState */
   input_assembly = malloc(sizeof(VkPipelineInputAssemblyStateCreateInfo));
   if (!input_assembly)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   memcpy(input_assembly, src->pInputAssemblyState, sizeof(VkPipelineInputAssemblyStateCreateInfo));
   dst->pInputAssemblyState = input_assembly;

   /* pTessellationState - TODO */
   if (src->pTessellationState)
      val_finishme("tess state\n");
   
   /* pViewportState */
   if (src->pViewportState) {
      VkPipelineViewportStateCreateInfo *viewport_state;
      viewport_state = malloc(sizeof(VkPipelineViewportStateCreateInfo));
      if (!viewport_state)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      deep_copy_viewport_state(viewport_state, src->pViewportState);
      dst->pViewportState = viewport_state;
   } else
      dst->pViewportState = NULL;
   
   /* pRasterizationState */
   raster_state = malloc(sizeof(VkPipelineRasterizationStateCreateInfo));
   if (!raster_state)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   memcpy(raster_state, src->pRasterizationState, sizeof(VkPipelineRasterizationStateCreateInfo));
   dst->pRasterizationState = raster_state;

   /* pMultisampleState */
   if (src->pMultisampleState) {
      VkPipelineMultisampleStateCreateInfo*   ms_state;
      ms_state = malloc(sizeof(VkPipelineMultisampleStateCreateInfo));
      if (!ms_state)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      /* does samplemask need deep copy? */
      memcpy(ms_state, src->pMultisampleState, sizeof(VkPipelineMultisampleStateCreateInfo));
      dst->pMultisampleState = ms_state;
   } else
      dst->pMultisampleState = NULL;

   /* pDepthStencilState */
   if (src->pDepthStencilState) {
      VkPipelineDepthStencilStateCreateInfo*  ds_state;

      ds_state = malloc(sizeof(VkPipelineDepthStencilStateCreateInfo));
      if (!ds_state)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      memcpy(ds_state, src->pDepthStencilState, sizeof(VkPipelineDepthStencilStateCreateInfo));
      dst->pDepthStencilState = ds_state;
   } else
      dst->pDepthStencilState = NULL;

   /* pColorBlendState */
   if (src->pColorBlendState) {
      VkPipelineColorBlendStateCreateInfo*    cb_state;

      cb_state = malloc(sizeof(VkPipelineColorBlendStateCreateInfo));
      if (!cb_state)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      deep_copy_color_blend_state(cb_state, src->pColorBlendState);
      dst->pColorBlendState = cb_state;
   } else
      dst->pColorBlendState = NULL;

   if (src->pDynamicState) {
      VkPipelineDynamicStateCreateInfo*       dyn_state;

      /* pDynamicState */
      dyn_state = malloc(sizeof(VkPipelineDynamicStateCreateInfo));
      if (!dyn_state)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      deep_copy_dynamic_state(dyn_state, src->pDynamicState);
      dst->pDynamicState = dyn_state;
   } else
      dst->pDynamicState = NULL;
   
   return VK_SUCCESS;
}

static inline unsigned
st_shader_stage_to_ptarget(gl_shader_stage stage)
{
   switch (stage) {
   case MESA_SHADER_VERTEX:
      return PIPE_SHADER_VERTEX;
   case MESA_SHADER_FRAGMENT:
      return PIPE_SHADER_FRAGMENT;
   case MESA_SHADER_GEOMETRY:
      return PIPE_SHADER_GEOMETRY;
   case MESA_SHADER_TESS_CTRL:
      return PIPE_SHADER_TESS_CTRL;
   case MESA_SHADER_TESS_EVAL:
      return PIPE_SHADER_TESS_EVAL;
   case MESA_SHADER_COMPUTE:
      return PIPE_SHADER_COMPUTE;
   }

   assert(!"should not be reached");
   return PIPE_SHADER_VERTEX;
}

static inline unsigned
st_shader_stage_to_tgsi_target(gl_shader_stage stage)
{
   switch (stage) {
   case MESA_SHADER_VERTEX:
      return TGSI_PROCESSOR_VERTEX;
   case MESA_SHADER_FRAGMENT:
      return TGSI_PROCESSOR_FRAGMENT;
   case MESA_SHADER_GEOMETRY:
      return PIPE_SHADER_GEOMETRY;
   case MESA_SHADER_TESS_CTRL:
      return PIPE_SHADER_TESS_CTRL;
   case MESA_SHADER_TESS_EVAL:
      return PIPE_SHADER_TESS_EVAL;
   case MESA_SHADER_COMPUTE:
      return PIPE_SHADER_COMPUTE;
   }

   assert(!"should not be reached");
   return PIPE_SHADER_VERTEX;
}

static void
val_shader_compile_to_tgsi(struct val_pipeline *pipeline,
                           struct val_shader_module *module,
                           const char *entrypoint_name,
                           gl_shader_stage stage,
                           const VkSpecializationInfo *spec_info)
{
   if (strcmp(entrypoint_name, "main") != 0) {
      val_finishme("Multiple shaders per module not really supported");
   }

   nir_shader *nir;
   nir_function *entry_point;
   nir_shader_compiler_options options = {};
   bool progress;
   options.native_integers = true;
   {
      uint32_t *spirv = (uint32_t *) module->data;
      assert(spirv[0] == SPIR_V_MAGIC_NUMBER);
      assert(module->size % 4 == 0);

      uint32_t num_spec_entries = 0;
      struct nir_spirv_specialization *spec_entries = NULL;
      if (spec_info && spec_info->mapEntryCount > 0) {
         num_spec_entries = spec_info->mapEntryCount;
         spec_entries = malloc(num_spec_entries * sizeof(*spec_entries));
         for (uint32_t i = 0; i < num_spec_entries; i++) {
            const uint32_t *data =
               spec_info->pData + spec_info->pMapEntries[i].offset;
            assert((const void *)(data + 1) <=
                   spec_info->pData + spec_info->dataSize);

            spec_entries[i].id = spec_info->pMapEntries[i].constantID;
            spec_entries[i].data = *data;
         }
      }

      entry_point = spirv_to_nir(spirv, module->size / 4,
                                 spec_entries, num_spec_entries,
                                 stage, entrypoint_name, &options);

      nir = entry_point->shader;
      nir_validate_shader(nir);

      free(spec_entries);

      nir_lower_global_vars_to_local(nir);
      nir_lower_returns(nir);
      nir_validate_shader(nir);

      nir_inline_functions(nir);
      nir_validate_shader(nir);

      nir_opt_global_to_local(nir);

      nir_lower_load_const_to_scalar(nir);
      do {
         progress = false;

         nir_lower_vars_to_ssa(nir);
         progress |= nir_copy_prop(nir);
         progress |= nir_opt_dce(nir);
         progress |= nir_opt_cse(nir);
         progress |= nir_opt_algebraic(nir);
         progress |= nir_opt_constant_folding(nir);
      } while (progress);

      nir_remove_dead_variables(nir, nir_var_local);
      module->tgsi = nir_to_tgsi(nir, st_shader_stage_to_tgsi_target(stage));
      if (module->tgsi)
         tgsi_dump(module->tgsi, 0);

      ralloc_free(nir);
   }
}


static VkResult
val_pipeline_compile(struct val_pipeline *pipeline,
                     struct val_shader_module *module,
                     const char *entrypoint,
                     gl_shader_stage stage,
                     const VkSpecializationInfo *spec_info)
{
   val_shader_compile_to_tgsi(pipeline, module, entrypoint, stage, spec_info);
   return VK_SUCCESS;
}

static VkResult
val_pipeline_init(struct val_pipeline *pipeline,
                  struct val_device *device,
                  struct val_pipeline_cache *cache,
                  const VkGraphicsPipelineCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *alloc)
{
   VkResult result;
   if (alloc == NULL)
      alloc = &device->alloc;
   pipeline->device = device;
   pipeline->layout = val_pipeline_layout_from_handle(pCreateInfo->layout);

   /* recreate createinfo */
   deep_copy_create_info(&pipeline->create_info, pCreateInfo);

   for (uint32_t i = 0; i < pCreateInfo->stageCount; i++) {
      VAL_FROM_HANDLE(val_shader_module, module,
                      pCreateInfo->pStages[i].module);

      switch (pCreateInfo->pStages[i].stage) {
      case VK_SHADER_STAGE_VERTEX_BIT:
         val_pipeline_compile(pipeline, module,
                              pCreateInfo->pStages[i].pName,
                              MESA_SHADER_VERTEX,
                              pCreateInfo->pStages[i].pSpecializationInfo);
         break;
      case VK_SHADER_STAGE_FRAGMENT_BIT:
         val_pipeline_compile(pipeline, module,
                              pCreateInfo->pStages[i].pName,
                              MESA_SHADER_FRAGMENT,
                              pCreateInfo->pStages[i].pSpecializationInfo);
         break;
      default:
         val_finishme("Unsupported shader stage");
      }
   }
   return VK_SUCCESS;
}


static VkResult
val_graphics_pipeline_create(
   VkDevice _device,
   VkPipelineCache _cache,
   const VkGraphicsPipelineCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkPipeline *pPipeline)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_pipeline_cache, cache, _cache);
   struct val_pipeline *pipeline;
   VkResult result;
   uint32_t offset, length;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);

   pipeline = val_alloc2(&device->alloc, pAllocator, sizeof(*pipeline), 8,
                         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pipeline == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   result = val_pipeline_init(pipeline, device, cache, pCreateInfo,
                              pAllocator);
   if (result != VK_SUCCESS) {
      val_free2(&device->alloc, pAllocator, pipeline);
      return result;
   }

//   if (cache == NULL)
//      cache = &device->default_pipeline_cache;
   *pPipeline = val_pipeline_to_handle(pipeline);

   return VK_SUCCESS;
}

VkResult val_CreateGraphicsPipelines(
    VkDevice                                    _device,
    VkPipelineCache                             pipelineCache,
    uint32_t                                    count,
    const VkGraphicsPipelineCreateInfo*         pCreateInfos,
    const VkAllocationCallbacks*                pAllocator,
    VkPipeline*                                 pPipelines)
{
   VkResult result;;
   unsigned i = 0;
   
   for (; i < count; i++) {
      result = val_graphics_pipeline_create(_device,
                                            pipelineCache,
                                            &pCreateInfos[i],
                                            pAllocator, &pPipelines[i]);
      if (result != VK_SUCCESS) {
         for (unsigned j = 0; j < i; j++) {
            val_DestroyPipeline(_device, pPipelines[j], pAllocator);
         }

         return result;
      }
   }

   return VK_SUCCESS;

}

VkResult val_CreateComputePipelines(
    VkDevice                                    _device,
    VkPipelineCache                             pipelineCache,
    uint32_t                                    count,
    const VkComputePipelineCreateInfo*          pCreateInfos,
    const VkAllocationCallbacks*                pAllocator,
    VkPipeline*                                 pPipelines)
{
   return VK_SUCCESS;

}

void val_CmdPipelineBarrier(
      VkCommandBuffer                             commandBuffer,
    VkPipelineStageFlags                        srcStageMask,
    VkPipelineStageFlags                        destStageMask,
    VkBool32                                    byRegion,
    uint32_t                                    memoryBarrierCount,
    const VkMemoryBarrier*                      pMemoryBarriers,
    uint32_t                                    bufferMemoryBarrierCount,
    const VkBufferMemoryBarrier*                pBufferMemoryBarriers,
    uint32_t                                    imageMemoryBarrierCount,
    const VkImageMemoryBarrier*                 pImageMemoryBarriers)
{
   VAL_FROM_HANDLE(val_cmd_buffer, cmd_buffer, commandBuffer);

}

#include "val_private.h"

VkResult val_CreateDescriptorSetLayout(
    VkDevice                                    _device,
    const VkDescriptorSetLayoutCreateInfo*      pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDescriptorSetLayout*                      pSetLayout)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   struct val_descriptor_set_layout *set_layout;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
   uint32_t max_binding = 0;
   uint32_t immutable_sampler_count = 0;
   for (uint32_t j = 0; j < pCreateInfo->bindingCount; j++) {
      max_binding = MAX(max_binding, pCreateInfo->pBindings[j].binding);
      if (pCreateInfo->pBindings[j].pImmutableSamplers)
         immutable_sampler_count += pCreateInfo->pBindings[j].descriptorCount;
   }

   size_t size = sizeof(struct val_descriptor_set_layout) +
                 (max_binding + 1) * sizeof(set_layout->binding[0]) +
                 immutable_sampler_count * sizeof(struct val_sampler *);

   set_layout = val_alloc2(&device->alloc, pAllocator, size, 8,
                           VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!set_layout)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   /* We just allocate all the samplers at the end of the struct */
   struct val_sampler **samplers =
      (struct val_sampler **)&set_layout->binding[max_binding + 1];

   set_layout->binding_count = max_binding + 1;
   set_layout->shader_stages = 0;
   set_layout->size = 0;

   for (uint32_t b = 0; b <= max_binding; b++) {
      /* Initialize all binding_layout entries to -1 */
      memset(&set_layout->binding[b], -1, sizeof(set_layout->binding[b]));

      set_layout->binding[b].immutable_samplers = NULL;
   }

   /* Initialize all samplers to 0 */
   memset(samplers, 0, immutable_sampler_count * sizeof(*samplers));

   uint32_t sampler_count[MESA_SHADER_STAGES] = { 0, };
   uint32_t surface_count[MESA_SHADER_STAGES] = { 0, };
   uint32_t image_count[MESA_SHADER_STAGES] = { 0, };
   uint32_t buffer_count = 0;
   uint32_t dynamic_offset_count = 0;

   for (uint32_t j = 0; j < pCreateInfo->bindingCount; j++) {
      const VkDescriptorSetLayoutBinding *binding = &pCreateInfo->pBindings[j];
      uint32_t b = binding->binding;

      assert(binding->descriptorCount > 0);
      set_layout->binding[b].array_size = binding->descriptorCount;
      set_layout->binding[b].descriptor_index = set_layout->size;
      set_layout->size += binding->descriptorCount;

      switch (binding->descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
         val_foreach_stage(s, binding->stageFlags) {
            set_layout->binding[b].stage[s].sampler_index = sampler_count[s];
            sampler_count[s] += binding->descriptorCount;
         }
         break;
      default:
         break;
      }

      switch (binding->descriptorType) {
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         set_layout->binding[b].buffer_index = buffer_count;
         buffer_count += binding->descriptorCount;
         /* fall through */

      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         val_foreach_stage(s, binding->stageFlags) {
            set_layout->binding[b].stage[s].surface_index = surface_count[s];
            surface_count[s] += binding->descriptorCount;
         }
         break;
      default:
         break;
      }

      switch (binding->descriptorType) {
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         set_layout->binding[b].dynamic_offset_index = dynamic_offset_count;
         dynamic_offset_count += binding->descriptorCount;
         break;
      default:
         break;
      }

      switch (binding->descriptorType) {
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         val_foreach_stage(s, binding->stageFlags) {
            set_layout->binding[b].stage[s].image_index = image_count[s];
            image_count[s] += binding->descriptorCount;
         }
         break;
      default:
         break;
      }

      if (binding->pImmutableSamplers) {
         set_layout->binding[b].immutable_samplers = samplers;
         samplers += binding->descriptorCount;

         for (uint32_t i = 0; i < binding->descriptorCount; i++)
            set_layout->binding[b].immutable_samplers[i] =
               val_sampler_from_handle(binding->pImmutableSamplers[i]);
      } else {
         set_layout->binding[b].immutable_samplers = NULL;
      }

      set_layout->shader_stages |= binding->stageFlags;
   }

   set_layout->buffer_count = buffer_count;
   set_layout->dynamic_offset_count = dynamic_offset_count;

   *pSetLayout = val_descriptor_set_layout_to_handle(set_layout);

   return VK_SUCCESS;
}

void val_DestroyDescriptorSetLayout(
    VkDevice                                    _device,
    VkDescriptorSetLayout                       _set_layout,
    const VkAllocationCallbacks*                pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_descriptor_set_layout, set_layout, _set_layout);

   val_free2(&device->alloc, pAllocator, set_layout);
}

VkResult val_CreatePipelineLayout(
    VkDevice                                    _device,
    const VkPipelineLayoutCreateInfo*           pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkPipelineLayout*                           pPipelineLayout)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   struct val_pipeline_layout *layout;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);

   layout = val_alloc2(&device->alloc, pAllocator, sizeof(*layout), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (layout == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
   *pPipelineLayout = val_pipeline_layout_to_handle(layout);

   return VK_SUCCESS;
  
}

void val_DestroyPipelineLayout(
    VkDevice                                    _device,
    VkPipelineLayout                            _pipelineLayout,
    const VkAllocationCallbacks*                pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_pipeline_layout, pipeline_layout, _pipelineLayout);

   val_free2(&device->alloc, pAllocator, pipeline_layout);
}

VkResult
val_descriptor_set_create(struct val_device *device,
                          const struct val_descriptor_set_layout *layout,
                          struct val_descriptor_set **out_set)
{
   struct val_descriptor_set *set;
   size_t size = sizeof(*set) + layout->size * sizeof(set->descriptors[0]);

   set = val_alloc(&device->alloc /* XXX: Use the pool */, size, 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!set)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   /* A descriptor set may not be 100% filled. Clear the set so we can can
    * later detect holes in it.
    */
   memset(set, 0, size);

   set->layout = layout;

   /* Go through and fill out immutable samplers if we have any */
   struct val_descriptor *desc = set->descriptors;
   for (uint32_t b = 0; b < layout->binding_count; b++) {
      if (layout->binding[b].immutable_samplers) {
         for (uint32_t i = 0; i < layout->binding[b].array_size; i++)
            desc[i].sampler = layout->binding[b].immutable_samplers[i];
      }
      desc += layout->binding[b].array_size;
   }

   /* XXX: Use the pool */
   set->buffer_views =
      val_alloc(&device->alloc,
                sizeof(set->buffer_views[0]) * layout->buffer_count, 8,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!set->buffer_views) {
      val_free(&device->alloc, set);
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
   }

//   for (uint32_t b = 0; b < layout->buffer_count; b++) {
//      set->buffer_views[b].surface_state =
//         val_state_pool_alloc(&device->surface_state_pool, 64, 64);
//   }
   set->buffer_count = layout->buffer_count;
   *out_set = set;

   return VK_SUCCESS;
}

void
val_descriptor_set_destroy(struct val_device *device,
                           struct val_descriptor_set *set)
{
   /* XXX: Use the pool */
//   for (uint32_t b = 0; b < set->buffer_count; b++)
//      val_state_pool_free(&device->surface_state_pool,
//                          set->buffer_views[b].surface_state);

   val_free(&device->alloc, set->buffer_views);
   val_free(&device->alloc, set);
}


VkResult val_AllocateDescriptorSets(
    VkDevice                                    _device,
    const VkDescriptorSetAllocateInfo*          pAllocateInfo,
    VkDescriptorSet*                            pDescriptorSets)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VkResult result = VK_SUCCESS;
   struct val_descriptor_set *set;
   uint32_t i;

   for (i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
      VAL_FROM_HANDLE(val_descriptor_set_layout, layout,
                      pAllocateInfo->pSetLayouts[i]);

      result = val_descriptor_set_create(device, layout, &set);
      if (result != VK_SUCCESS)
         break;

      pDescriptorSets[i] = val_descriptor_set_to_handle(set);
   }

   if (result != VK_SUCCESS)
      val_FreeDescriptorSets(_device, pAllocateInfo->descriptorPool,
                             i, pDescriptorSets);

   return result;
}

VkResult val_FreeDescriptorSets(
    VkDevice                                    _device,
    VkDescriptorPool                            descriptorPool,
    uint32_t                                    count,
    const VkDescriptorSet*                      pDescriptorSets)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   for (uint32_t i = 0; i < count; i++) {
      VAL_FROM_HANDLE(val_descriptor_set, set, pDescriptorSets[i]);

      val_descriptor_set_destroy(device, set);
   }
   return VK_SUCCESS;
}


void val_UpdateDescriptorSets(
    VkDevice                                    _device,
    uint32_t                                    descriptorWriteCount,
    const VkWriteDescriptorSet*                 pDescriptorWrites,
    uint32_t                                    descriptorCopyCount,
    const VkCopyDescriptorSet*                  pDescriptorCopies)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   for (uint32_t i = 0; i < descriptorWriteCount; i++) {
      const VkWriteDescriptorSet *write = &pDescriptorWrites[i];
      VAL_FROM_HANDLE(val_descriptor_set, set, write->dstSet);
      const struct val_descriptor_set_binding_layout *bind_layout =
         &set->layout->binding[write->dstBinding];
      struct val_descriptor *desc =
         &set->descriptors[bind_layout->descriptor_index];
      desc += write->dstArrayElement;

      switch (write->descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            VAL_FROM_HANDLE(val_sampler, sampler,
                            write->pImageInfo[j].sampler);

            desc[j] = (struct val_descriptor) {
               .type = VK_DESCRIPTOR_TYPE_SAMPLER,
               .sampler = sampler,
            };
         }
         break;

      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            VAL_FROM_HANDLE(val_image_view, iview,
                            write->pImageInfo[j].imageView);
            VAL_FROM_HANDLE(val_sampler, sampler,
                            write->pImageInfo[j].sampler);

            desc[j].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            desc[j].image_view = iview;

            /* If this descriptor has an immutable sampler, we don't want
             * to stomp on it.
             */
            if (sampler)
               desc[j].sampler = sampler;
         }
         break;

      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            VAL_FROM_HANDLE(val_image_view, iview,
                            write->pImageInfo[j].imageView);

            desc[j] = (struct val_descriptor) {
               .type = write->descriptorType,
               .image_view = iview,
            };
         }
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            VAL_FROM_HANDLE(val_buffer_view, bview,
                            write->pTexelBufferView[j]);

            desc[j] = (struct val_descriptor) {
               .type = write->descriptorType,
               .buffer_view = bview,
            };
         }
         break;

      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         val_finishme("input attachments not implemented");
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            assert(write->pBufferInfo[j].buffer);
            VAL_FROM_HANDLE(val_buffer, buffer, write->pBufferInfo[j].buffer);
            assert(buffer);

            struct val_buffer_view *view =
               &set->buffer_views[bind_layout->buffer_index];
            view += write->dstArrayElement + j;

//            const struct val_format *format =
//               val_format_for_descriptor_type(write->descriptorType);

            view->format = 0;//TODOformat->isl_format;
            view->bo = buffer->bo;
            view->offset = buffer->offset + write->pBufferInfo[j].offset;

            /* For buffers with dynamic offsets, we use the full possible
             * range in the surface state and do the actual range-checking
             * in the shader.
             */
            if (bind_layout->dynamic_offset_index >= 0 ||
                write->pBufferInfo[j].range == VK_WHOLE_SIZE)
               view->range = buffer->size - write->pBufferInfo[j].offset;
            else
               view->range = write->pBufferInfo[j].range;

//            val_fill_buffer_surface_state(device, view->surface_state,
//                                          view->format,
//                                          view->offset, view->range, 1);

            desc[j] = (struct val_descriptor) {
               .type = write->descriptorType,
               .buffer_view = view,
            };

         }

      default:
         break;
      }
   }

   for (uint32_t i = 0; i < descriptorCopyCount; i++) {
      const VkCopyDescriptorSet *copy = &pDescriptorCopies[i];
      VAL_FROM_HANDLE(val_descriptor_set, src, copy->dstSet);
      VAL_FROM_HANDLE(val_descriptor_set, dst, copy->dstSet);

      const struct val_descriptor_set_binding_layout *src_layout =
         &src->layout->binding[copy->srcBinding];
      struct val_descriptor *src_desc =
         &src->descriptors[src_layout->descriptor_index];
      src_desc += copy->srcArrayElement;

      const struct val_descriptor_set_binding_layout *dst_layout =
         &dst->layout->binding[copy->dstBinding];
      struct val_descriptor *dst_desc =
         &dst->descriptors[dst_layout->descriptor_index];
      dst_desc += copy->dstArrayElement;

      for (uint32_t j = 0; j < copy->descriptorCount; j++)
         dst_desc[j] = src_desc[j];
   }
}

VkResult val_CreateDescriptorPool(
    VkDevice                                    device,
    const VkDescriptorPoolCreateInfo*           pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDescriptorPool*                           pDescriptorPool)
{
   val_finishme("VkDescriptorPool is a stub");
   *pDescriptorPool = (VkDescriptorPool)1;
   return VK_SUCCESS;
}

void val_DestroyDescriptorPool(
    VkDevice                                    _device,
    VkDescriptorPool                            _pool,
    const VkAllocationCallbacks*                pAllocator)
{
   val_finishme("VkDescriptorPool is a stub: free the pool's descriptor sets");
}

VkResult val_ResetDescriptorPool(
    VkDevice                                    device,
    VkDescriptorPool                            descriptorPool,
    VkDescriptorPoolResetFlags                  flags)
{
   val_finishme("VkDescriptorPool is a stub: free the pool's descriptor sets");
   return VK_SUCCESS;
}

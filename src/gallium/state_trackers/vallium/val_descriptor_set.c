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

VkResult val_AllocateDescriptorSets(
    VkDevice                                    _device,
    const VkDescriptorSetAllocateInfo*          pAllocateInfo,
    VkDescriptorSet*                            pDescriptorSets)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VkResult result = VK_SUCCESS;
   struct val_descriptor_set *set;
   uint32_t i;

   return result;
}

VkResult val_FreeDescriptorSets(
    VkDevice                                    _device,
    VkDescriptorPool                            descriptorPool,
    uint32_t                                    count,
    const VkDescriptorSet*                      pDescriptorSets)
{
   VAL_FROM_HANDLE(val_device, device, _device);

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

}

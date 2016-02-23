#include "val_private.h"

VkResult val_CreateRenderPass(
    VkDevice                                    _device,
    const VkRenderPassCreateInfo*               pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkRenderPass*                               pRenderPass)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   struct val_render_pass *pass;
   size_t size;
   size_t attachments_offset;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);

   size = sizeof(*pass);
   size += pCreateInfo->subpassCount * sizeof(pass->subpasses[0]);
   attachments_offset = size;
   size += pCreateInfo->attachmentCount * sizeof(pass->attachments[0]);

   pass = val_alloc2(&device->alloc, pAllocator, size, 8,
                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pass == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   /* Clear the subpasses along with the parent pass. This required because
    * each array member of val_subpass must be a valid pointer if not NULL.
    */
   memset(pass, 0, size);
   pass->attachment_count = pCreateInfo->attachmentCount;
   pass->subpass_count = pCreateInfo->subpassCount;
   pass->attachments = (void *) pass + attachments_offset;

   for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
      struct val_render_pass_attachment *att = &pass->attachments[i];

//      att->format = val_format_for_vk_format(pCreateInfo->pAttachments[i].format);
      att->samples = pCreateInfo->pAttachments[i].samples;
      att->load_op = pCreateInfo->pAttachments[i].loadOp;
      att->stencil_load_op = pCreateInfo->pAttachments[i].stencilLoadOp;
      // att->store_op = pCreateInfo->pAttachments[i].storeOp;
      // att->stencil_store_op = pCreateInfo->pAttachments[i].stencilStoreOp;
   }

   uint32_t subpass_attachment_count = 0, *p;
   for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
      const VkSubpassDescription *desc = &pCreateInfo->pSubpasses[i];

      subpass_attachment_count +=
         desc->inputAttachmentCount +
         desc->colorAttachmentCount +
         /* Count colorAttachmentCount again for resolve_attachments */
         desc->colorAttachmentCount;
   }

   pass->subpass_attachments =
      val_alloc2(&device->alloc, pAllocator,
                 subpass_attachment_count * sizeof(uint32_t), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pass->subpass_attachments == NULL) {
      val_free2(&device->alloc, pAllocator, pass);
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   p = pass->subpass_attachments;
   for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
      const VkSubpassDescription *desc = &pCreateInfo->pSubpasses[i];
      struct val_subpass *subpass = &pass->subpasses[i];

      subpass->input_count = desc->inputAttachmentCount;
      subpass->color_count = desc->colorAttachmentCount;

      if (desc->inputAttachmentCount > 0) {
         subpass->input_attachments = p;
         p += desc->inputAttachmentCount;

         for (uint32_t j = 0; j < desc->inputAttachmentCount; j++) {
            subpass->input_attachments[j]
               = desc->pInputAttachments[j].attachment;
         }
      }

      if (desc->colorAttachmentCount > 0) {
         subpass->color_attachments = p;
         p += desc->colorAttachmentCount;

         for (uint32_t j = 0; j < desc->colorAttachmentCount; j++) {
            subpass->color_attachments[j]
               = desc->pColorAttachments[j].attachment;
         }
      }

      subpass->has_resolve = false;
      if (desc->pResolveAttachments) {
         subpass->resolve_attachments = p;
         p += desc->colorAttachmentCount;

         for (uint32_t j = 0; j < desc->colorAttachmentCount; j++) {
            uint32_t a = desc->pResolveAttachments[j].attachment;
            subpass->resolve_attachments[j] = a;
            if (a != VK_ATTACHMENT_UNUSED)
               subpass->has_resolve = true;
         }
      }

      if (desc->pDepthStencilAttachment) {
         subpass->depth_stencil_attachment =
            desc->pDepthStencilAttachment->attachment;
      } else {
         subpass->depth_stencil_attachment = VK_ATTACHMENT_UNUSED;
      }
   }

   *pRenderPass = val_render_pass_to_handle(pass);

   return VK_SUCCESS;
}

void val_DestroyRenderPass(
    VkDevice                                    _device,
    VkRenderPass                                _pass,
    const VkAllocationCallbacks*                pAllocator)
{
   VAL_FROM_HANDLE(val_device, device, _device);
   VAL_FROM_HANDLE(val_render_pass, pass, _pass);

   val_free2(&device->alloc, pAllocator, pass->subpass_attachments);
   val_free2(&device->alloc, pAllocator, pass);
}

void val_GetRenderAreaGranularity(
    VkDevice                                    device,
    VkRenderPass                                renderPass,
    VkExtent2D*                                 pGranularity)
{
   *pGranularity = (VkExtent2D) { 1, 1 };
}

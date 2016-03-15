
#pragma once

static inline unsigned vk_cull_to_pipe(uint32_t vk_cull)
{
   /* these correspond */
   return vk_cull;
}

static inline unsigned vk_polygon_mode_to_pipe(uint32_t vk_poly_mode)
{
   /* these correspond */
   return vk_poly_mode;
}

static inline unsigned vk_conv_stencil_op(uint32_t vk_stencil_op)
{
   switch (vk_stencil_op) {
   case VK_STENCIL_OP_KEEP:
      return PIPE_STENCIL_OP_KEEP;
   case VK_STENCIL_OP_ZERO:
      return PIPE_STENCIL_OP_ZERO;
   case VK_STENCIL_OP_REPLACE:
      return PIPE_STENCIL_OP_REPLACE;
   case VK_STENCIL_OP_INCREMENT_AND_CLAMP:
      return PIPE_STENCIL_OP_INCR;
   case VK_STENCIL_OP_DECREMENT_AND_CLAMP:
      return PIPE_STENCIL_OP_DECR;
   case VK_STENCIL_OP_INVERT:
      return PIPE_STENCIL_OP_INVERT;
   case VK_STENCIL_OP_INCREMENT_AND_WRAP:
      return PIPE_STENCIL_OP_INCR_WRAP;
   case VK_STENCIL_OP_DECREMENT_AND_WRAP:
      return PIPE_STENCIL_OP_DECR_WRAP;
   default:
      assert(0);
      return 0;
   }
}

static inline unsigned vk_conv_topology(VkPrimitiveTopology topology)
{
   switch (topology) {
   case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
      return PIPE_PRIM_POINTS;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
      return PIPE_PRIM_LINES;
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      return PIPE_PRIM_LINE_STRIP;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
      return PIPE_PRIM_TRIANGLES;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
      return PIPE_PRIM_TRIANGLE_STRIP;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
      return PIPE_PRIM_TRIANGLE_FAN;
   default:
      assert(0);
      return 0;
   }
}

static inline unsigned vk_conv_wrap_mode(enum VkSamplerAddressMode addr_mode)
{
   switch (addr_mode) {
   case VK_SAMPLER_ADDRESS_MODE_REPEAT:
      return PIPE_TEX_WRAP_REPEAT;
   case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
      return PIPE_TEX_WRAP_MIRROR_REPEAT;
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:
      return PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
      return PIPE_TEX_WRAP_CLAMP_TO_BORDER;
   case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE:
      return PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE;
   default:
      assert(0);
      return 0;
   }
}

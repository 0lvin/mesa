
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

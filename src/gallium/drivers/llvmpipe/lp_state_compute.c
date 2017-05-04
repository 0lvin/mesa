/**************************************************************************
 *
 * Copyright 2007 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "lp_context.h"
#include "lp_state.h"
#include "lp_texture.h"
#include "lp_debug.h"

#include "pipe/p_defines.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "draw/draw_context.h"
#include "tgsi/tgsi_dump.h"
#include "tgsi/tgsi_scan.h"
#include "tgsi/tgsi_parse.h"

static void *
llvmpipe_create_compute_state(struct pipe_context *pipe,
                              const struct pipe_compute_state *templ)
{
   struct llvmpipe_context *llvmpipe = llvmpipe_context(pipe);
   const struct tgsi_token *tokens;
   struct lp_compute_shader *state;
   if (templ->ir_type != PIPE_SHADER_IR_TGSI)
      return NULL;

   tokens = templ->prog;

   state = CALLOC_STRUCT(lp_compute_shader);

   state->shader = *templ;
   state->tokens = tgsi_dup_tokens(tokens);
   tgsi_scan_shader(state->tokens, &state->info);

   state->max_sampler = state->info.file_max[TGSI_FILE_SAMPLER];

   return state;
}

static void
llvmpipe_bind_compute_state(struct pipe_context *pipe,
                            void *cs)
{
   struct llvmpipe_context *llvmpipe = llvmpipe_context(pipe);
   struct lp_compute_shader *state = (struct lp_compute_shader *)cs;
   if (llvmpipe->cs == state)
      return;

   llvmpipe->cs = state;
}

static void
llvmpipe_delete_compute_state(struct pipe_context *pipe,
                              void *cs)
{
   struct llvmpipe_context *llvmpipe = llvmpipe_context(pipe);
   struct lp_compute_shader *state = (struct lp_compute_shader *)cs;

   assert(llvmpipe->cs != state);
   tgsi_free_tokens(state->tokens);
   FREE(state);
}


void
llvmpipe_init_compute_funcs(struct llvmpipe_context *llvmpipe)
{
   llvmpipe->pipe.create_compute_state = llvmpipe_create_compute_state;
   llvmpipe->pipe.bind_compute_state = llvmpipe_bind_compute_state;
   llvmpipe->pipe.delete_compute_state = llvmpipe_delete_compute_state;
}

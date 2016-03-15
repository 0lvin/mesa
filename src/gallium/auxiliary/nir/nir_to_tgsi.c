/*
 * Copyright © 2014-2015 Broadcom
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

#include "compiler/nir/nir.h"
#include "tgsi/tgsi_ureg.h"
#include "nir/nir_to_tgsi.h"

struct ntt_compile {
   nir_shader *s;
   struct ureg_program *ureg;
   unsigned target;

   bool addr_declared;
   struct ureg_dst addr_reg;

   unsigned loop_label;

   /** Mapping from nir_register * or nir_def * to struct ureg_dst * */
   struct hash_table *def_ht;

   /* Mappings from driver_location to TGSI input/output number.
    *
    * We'll be declaring TGSI input/outputs in an arbitrary order, and they get
    * their numbers assigned incrementally, unlike inputs or constants.
    */
   unsigned *input_index_map;
   unsigned *output_index_map;
};

static void ntt_emit_cf_list(struct ntt_compile *c, struct exec_list *list);

static inline unsigned
st_get_generic_varying_index(bool needs_texcoord, GLuint attr)
{
   if (attr >= VARYING_SLOT_VAR0) {
      if (needs_texcoord)
         return attr - VARYING_SLOT_VAR0;
      else
         return 9 + (attr - VARYING_SLOT_VAR0);
   }
   if (attr == VARYING_SLOT_PNTC) {
       assert(!needs_texcoord);
      return 8;
   }
   if (attr >= VARYING_SLOT_TEX0 && attr <= VARYING_SLOT_TEX7) {
      assert(!needs_texcoord);
      return attr - VARYING_SLOT_TEX0;
   }

   assert(0);
   return 0;
}

static int translate_semantic_attr(int attr, bool needs_texcoord,
				   unsigned *tgsi_name, unsigned *tgsi_slot)
{
    switch (attr) {
    case VARYING_SLOT_POS:
	*tgsi_name = TGSI_SEMANTIC_POSITION;
	*tgsi_slot = 0;
	break;
    case VARYING_SLOT_COL0:
	*tgsi_name = TGSI_SEMANTIC_COLOR;
	*tgsi_slot = 0;
	break;
    case VARYING_SLOT_COL1:
	*tgsi_name = TGSI_SEMANTIC_COLOR;
	*tgsi_slot = 1;
	break;
    case VARYING_SLOT_BFC0:
	*tgsi_name = TGSI_SEMANTIC_BCOLOR;
	*tgsi_slot = 0;
	break;
    case VARYING_SLOT_BFC1:
	*tgsi_name = TGSI_SEMANTIC_BCOLOR;
	*tgsi_slot = 1;
	break;
    case VARYING_SLOT_FOGC:
	*tgsi_name = TGSI_SEMANTIC_FOG;
	*tgsi_slot = 0;
	break;
    case VARYING_SLOT_PSIZ:
	*tgsi_name = TGSI_SEMANTIC_PSIZE;
	*tgsi_slot = 0;
	break;
    case VARYING_SLOT_CLIP_DIST0:
	*tgsi_name = TGSI_SEMANTIC_CLIPDIST;
	*tgsi_slot = 0;
	break;
    case VARYING_SLOT_CLIP_DIST1:
	*tgsi_name = TGSI_SEMANTIC_CLIPDIST;
	*tgsi_slot = 1;
	break;
    case VARYING_SLOT_EDGE:
	assert(0);
	break;
    case VARYING_SLOT_CLIP_VERTEX:
	*tgsi_name = TGSI_SEMANTIC_CLIPVERTEX;
	*tgsi_slot = 0;
	break;
    case VARYING_SLOT_LAYER:
	*tgsi_name = TGSI_SEMANTIC_LAYER;
	*tgsi_slot = 0;
	break;
    case VARYING_SLOT_VIEWPORT:
	*tgsi_name = TGSI_SEMANTIC_VIEWPORT_INDEX;
	*tgsi_slot = 0;
	break;

    case VARYING_SLOT_TEX0:
    case VARYING_SLOT_TEX1:
    case VARYING_SLOT_TEX2:
    case VARYING_SLOT_TEX3:
    case VARYING_SLOT_TEX4:
    case VARYING_SLOT_TEX5:
    case VARYING_SLOT_TEX6:
    case VARYING_SLOT_TEX7:
	if (needs_texcoord) {
	    *tgsi_name = TGSI_SEMANTIC_TEXCOORD;
	    *tgsi_slot = attr - VARYING_SLOT_TEX0;
	    break;
	}
	/* fall through */
    case VARYING_SLOT_VAR0:
    default:
	assert(attr >= VARYING_SLOT_VAR0 ||
	       (attr >= VARYING_SLOT_TEX0 && attr <= VARYING_SLOT_TEX7));
	*tgsi_name = TGSI_SEMANTIC_GENERIC;
	*tgsi_slot =
	    st_get_generic_varying_index(needs_texcoord, attr);
	break;
    }
    return 0;
}

static void
ntt_setup_inputs(struct ntt_compile *c)
{
   unsigned num_inputs = 0;
   foreach_list_typed(nir_variable, var, node, &c->s->inputs) {
      unsigned array_len = MAX2(glsl_get_length(var->type), 1);

      num_inputs = MAX2(num_inputs, var->data.driver_location + array_len);
   }

   c->input_index_map = ralloc_array(c, unsigned, num_inputs);

   foreach_list_typed(nir_variable, var, node, &c->s->inputs) {
      unsigned array_len = MAX2(glsl_get_length(var->type), 1);
      unsigned i;
      struct ureg_src decl;
      unsigned loc;
      /* XXX: map loc slots to semantics */

      if (c->target == TGSI_PROCESSOR_VERTEX) {
	  loc = var->data.location - VERT_ATTRIB_GENERIC0;
	  decl = ureg_DECL_vs_input(c->ureg, loc);
      } else if (c->target == TGSI_PROCESSOR_FRAGMENT) {
         static const unsigned interpolation_map[] = {
            TGSI_INTERPOLATE_PERSPECTIVE, /* INTERP_QUALIFIER_NONE */
            TGSI_INTERPOLATE_PERSPECTIVE, /* INTERP_QUALIFIER_SMOOTH */
            TGSI_INTERPOLATE_CONSTANT,    /* INTERP_QUALIFIER_FLAT */
            TGSI_INTERPOLATE_LINEAR,      /* INTERP_QUALIFIER_NOPERSPECTIVE */
         };
         unsigned interpolation = interpolation_map[var->data.interpolation];
         unsigned sample_loc;
         unsigned semantic_name = var->data.location;
         unsigned semantic_index = var->data.index;

	 translate_semantic_attr(var->data.location, false,
				 &semantic_name, &semantic_index);

         if (var->data.sample)
            sample_loc = TGSI_INTERPOLATE_LOC_SAMPLE;
         else if (var->data.centroid)
            sample_loc = TGSI_INTERPOLATE_LOC_CENTROID;
         else
            sample_loc = TGSI_INTERPOLATE_LOC_CENTER;

         decl = ureg_DECL_fs_input_cyl_centroid(c->ureg,
                                                semantic_name,
                                                semantic_index,
                                                interpolation,
                                                0,
                                                sample_loc, 0, 1);

         /* XXX: fs coord origin */
      } else {
         fprintf(stderr, "Unknown shader stage %d\n", c->target);
         abort();
      }

      for (i = 0; i < array_len; i++)
         c->input_index_map[var->data.driver_location + i] = decl.Index + i;
   }
}

static void
ntt_setup_outputs(struct ntt_compile *c)
{
   unsigned num_outputs = 0;

   foreach_list_typed(nir_variable, var, node, &c->s->outputs) {
      unsigned array_len = MAX2(glsl_get_length(var->type), 1);

      num_outputs = MAX2(num_outputs, var->data.driver_location + array_len);
   }

   c->output_index_map = ralloc_array(c, unsigned, num_outputs);

   foreach_list_typed(nir_variable, var, node, &c->s->outputs) {
      unsigned array_len = MAX2(glsl_get_length(var->type), 1);
      unsigned semantic_name = var->data.location;
      unsigned semantic_index = var->data.index;
      unsigned i;
      struct ureg_dst decl;

      if (c->target == TGSI_PROCESSOR_VERTEX) {
	  translate_semantic_attr(var->data.location, false,
				  &semantic_name, &semantic_index);
      } else {
	  if (var->data.location == FRAG_RESULT_DATA0) {
	      semantic_name = TGSI_SEMANTIC_COLOR;
	      semantic_index = 0;
	  }
      }

      decl = ureg_DECL_output(c->ureg, semantic_name, semantic_index);

      for (i = 0; i < array_len; i++)
         c->output_index_map[var->data.driver_location + i] = decl.Index + i;
   }
}

static void
ntt_setup_uniforms(struct ntt_compile *c)
{
   foreach_list_typed(nir_variable, var, node, &c->s->uniforms) {
      unsigned array_len = MAX2(glsl_get_length(var->type), 1);
      unsigned i;

      for (i = 0; i < array_len ; i++) {
         ureg_DECL_constant(c->ureg, var->data.driver_location + i);
      }
   }
}

static void
ntt_setup_registers(struct ntt_compile *c, struct exec_list *list)
{
   foreach_list_typed(nir_register, nir_reg, node, list) {
      struct ureg_dst *decl = ralloc(c->def_ht, struct ureg_dst);

      if (nir_reg->num_array_elems == 0) {
         *decl = ureg_DECL_temporary(c->ureg);
      } else {
         *decl = ureg_DECL_array_temporary(c->ureg, nir_reg->num_array_elems,
                                           true);
      }

      _mesa_hash_table_insert(c->def_ht, nir_reg, decl);
   }
}

static struct ureg_src
ntt_get_src(struct ntt_compile *c, nir_src src)
{
   struct hash_entry *entry;

   if (src.is_ssa)
      entry = _mesa_hash_table_search(c->def_ht, src.ssa);
   else {
      entry = _mesa_hash_table_search(c->def_ht, src.reg.reg);
      assert(!src.reg.indirect); /* XXX: Not supported yet. */
   }
   struct ureg_dst dst = *((struct ureg_dst *) entry->data);
   struct ureg_src usrc = ureg_src(dst);

   return usrc;
}

static struct ureg_src
ntt_get_alu_src(struct ntt_compile *c, nir_alu_src src)
{
   struct ureg_src usrc = ntt_get_src(c, src.src);

   usrc.SwizzleX = src.swizzle[0];
   usrc.SwizzleY = src.swizzle[1];
   usrc.SwizzleZ = src.swizzle[2];
   usrc.SwizzleW = src.swizzle[3];
   usrc.Negate = src.negate;
   usrc.Absolute = src.abs;

   return usrc;
}

static struct ureg_dst
ntt_get_dst(struct ntt_compile *c, nir_dest dest)
{
   assert(!dest.is_ssa);
   struct hash_entry *entry  =
      _mesa_hash_table_search(c->def_ht, dest.reg.reg);
   struct ureg_dst dst = *((struct ureg_dst *) entry->data);

   assert(!dest.reg.indirect); /* XXX: Not supported yet. */

   return dst;
}

static void
ntt_emit_scalar(struct ntt_compile *c, unsigned tgsi_op,
                struct ureg_dst dst,
                struct ureg_src src0,
                struct ureg_src src1)
{
   unsigned i;

   /* Ignore src1 except for POW. */
   if (tgsi_op != TGSI_OPCODE_POW)
      src1 = src0;

   for (i = 0; i < 4; i++) {
      if (dst.WriteMask & (1 << i)) {
         struct ureg_dst this_dst = dst;
         struct ureg_src this_src0 = ureg_scalar(src0, i);
         struct ureg_src this_src1 = ureg_scalar(src1, i);
         this_dst.WriteMask = (1 << i);

         switch (tgsi_op) {
         case TGSI_OPCODE_RCP:
            ureg_RCP(c->ureg, this_dst, this_src0);
            break;
         case TGSI_OPCODE_RSQ:
            ureg_RSQ(c->ureg, this_dst, this_src0);
            break;
         case TGSI_OPCODE_SQRT:
            ureg_SQRT(c->ureg, this_dst, this_src0);
            break;
         case TGSI_OPCODE_EX2:
            ureg_EX2(c->ureg, this_dst, this_src0);
            break;
         case TGSI_OPCODE_LG2:
            ureg_LG2(c->ureg, this_dst, this_src0);
            break;
         case TGSI_OPCODE_SIN:
            ureg_SIN(c->ureg, this_dst, this_src0);
            break;
         case TGSI_OPCODE_COS:
            ureg_COS(c->ureg, this_dst, this_src0);
            break;
         case TGSI_OPCODE_POW:
            ureg_POW(c->ureg, this_dst, this_src0, this_src1);
            break;
         default:
            fprintf(stderr, "Bad opcode %d for ntt_emit_scalar()\n", tgsi_op);
            abort();
         }
      }
   }
}

static void
ntt_emit_alu(struct ntt_compile *c, nir_alu_instr *instr)
{
   struct ureg_src src[4];
   struct ureg_dst dst;
   unsigned i;
   int zero = 0;

   assert(nir_op_infos[instr->op].num_inputs <= ARRAY_SIZE(src));
   for (i = 0; i < nir_op_infos[instr->op].num_inputs; i++)
      src[i] = ntt_get_alu_src(c, instr->src[i]);
   dst = ntt_get_dst(c, instr->dest.dest);

   if (instr->dest.saturate)
      dst.Saturate = 1;

   dst.WriteMask = instr->dest.write_mask;

   switch (instr->op) {
   case nir_op_fmov:
      ureg_MOV(c->ureg, dst, src[0]);
      break;

   case nir_op_imov:
      if (src[0].Absolute || src[0].Negate) {
         /* There's apparently no TGSI UMOV to get integer negate/abs. */
         ureg_UADD(c->ureg, dst, src[0],
                   ureg_DECL_immediate_int(c->ureg, &zero, 1));
      } else {
         ureg_MOV(c->ureg, dst, src[0]);
      }
      break;

   case nir_op_fabs:
      ureg_ABS(c->ureg, dst, src[0]);
      break;

   case nir_op_fsat:
      ureg_MOV(c->ureg, ureg_saturate(dst), src[0]);
      break;

   case nir_op_fneg:
      ureg_MOV(c->ureg, dst, ureg_negate(src[0]));
      break;

   case nir_op_iabs:
      ureg_IABS(c->ureg, dst, src[0]);
      break;

   case nir_op_ineg:
      ureg_INEG(c->ureg, dst, src[0]);
      break;

   case nir_op_inot:
      ureg_NOT(c->ureg, dst, src[0]);
      break;

   case nir_op_fsign:
      ureg_SSG(c->ureg, dst, src[0]);
      break;

   case nir_op_isign:
      ureg_ISSG(c->ureg, dst, src[0]);
      break;

   case nir_op_frcp:
      ntt_emit_scalar(c, TGSI_OPCODE_RCP, dst, src[0], src[1]);
      break;

   case nir_op_frsq:
      ntt_emit_scalar(c, TGSI_OPCODE_RSQ, dst, src[0], src[1]);
      break;

   case nir_op_fsqrt:
      ntt_emit_scalar(c, TGSI_OPCODE_SQRT, dst, src[0], src[1]);
      break;

   case nir_op_fexp2:
      ntt_emit_scalar(c, TGSI_OPCODE_EX2, dst, src[0], src[1]);
      break;

   case nir_op_flog2:
      ntt_emit_scalar(c, TGSI_OPCODE_LG2, dst, src[0], src[1]);
      break;

   case nir_op_f2i:
      ureg_F2I(c->ureg, dst, src[0]);
      break;

   case nir_op_f2u:
      ureg_F2U(c->ureg, dst, src[0]);
      break;

   case nir_op_i2f:
      ureg_I2F(c->ureg, dst, src[0]);
      break;

   case nir_op_u2f:
      ureg_U2F(c->ureg, dst, src[0]);
      break;

/* XXX:
   case nir_op_f2b:
   case nir_op_b2f:
   case nir_op_i2b:
   case nir_op_b2i:
*/
      /* XXX: bany, etc. */

   case nir_op_ftrunc:
      ureg_TRUNC(c->ureg, dst, src[0]);
      break;

   case nir_op_fceil:
      ureg_CEIL(c->ureg, dst, src[0]);
      break;

   case nir_op_ffloor:
      ureg_FLR(c->ureg, dst, src[0]);
      break;

   case nir_op_ffract:
      ureg_FRC(c->ureg, dst, src[0]);
      break;

   case nir_op_fround_even:
      ureg_ROUND(c->ureg, dst, src[0]);
      break;

   case nir_op_fsin:
      ntt_emit_scalar(c, TGSI_OPCODE_SIN, dst, src[0], src[1]);
      break;

   case nir_op_fcos:
      ntt_emit_scalar(c, TGSI_OPCODE_COS, dst, src[0], src[1]);
      break;

   case nir_op_fddx:
      ureg_DDX(c->ureg, dst, src[0]);
      break;

   case nir_op_fddy:
      ureg_DDY(c->ureg, dst, src[0]);
      break;

   case nir_op_fddx_fine:
      ureg_DDX_FINE(c->ureg, dst, src[0]);
      break;

   case nir_op_fddy_fine:
      ureg_DDY_FINE(c->ureg, dst, src[0]);
      break;

      /* XXX: pack/unpack */
      /* XXX: bitfield */

   case nir_op_fadd:
      ureg_ADD(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_iadd:
      ureg_UADD(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_fsub:
      ureg_SUB(c->ureg, dst, src[0], src[1]);
      break;

/* XXX:
   case nir_op_isub:
      ureg_ISUB(c->ureg, dst, src[0], src[1]);
      break;
*/

   case nir_op_fmul:
      ureg_MUL(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_imul:
      ureg_UMUL(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_imul_high:
      ureg_IMUL_HI(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_umul_high:
      ureg_UMUL_HI(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_fdiv:
      ureg_DIV(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_idiv:
      ureg_IDIV(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_udiv:
      ureg_UDIV(c->ureg, dst, src[0], src[1]);
      break;

      /* XXX: carry */

   case nir_op_fmod:
      ureg_MOD(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_flt:
      ureg_FSLT(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_fge:
      ureg_FSGE(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_feq:
      ureg_FSEQ(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_fne:
      ureg_FSNE(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_ilt:
      ureg_ISLT(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_ige:
      ureg_ISGE(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_ieq:
      ureg_USEQ(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_ine:
      ureg_USNE(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_ult:
      ureg_USLT(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_uge:
      ureg_USGE(c->ureg, dst, src[0], src[1]);
      break;

      /* XXX: ball/bany/fall equal/nequal */

   case nir_op_slt:
      ureg_SLT(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_sge:
      ureg_SGE(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_seq:
      ureg_SEQ(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_sne:
      ureg_SNE(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_ishl:
      ureg_SHL(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_ishr:
      ureg_ISHR(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_ushr:
      ureg_USHR(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_iand:
      ureg_AND(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_ior:
      ureg_OR(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_ixor:
      ureg_XOR(c->ureg, dst, src[0], src[1]);
      break;

      /* XXX: fand/for/fxor */

   case nir_op_fdot2:
      ureg_DP2(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_fdot3:
      ureg_DP3(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_fdot4:
      ureg_DP4(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_fmin:
      ureg_MIN(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_imin:
      ureg_IMIN(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_umin:
      ureg_UMIN(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_fmax:
      ureg_MAX(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_imax:
      ureg_IMAX(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_umax:
      ureg_UMAX(c->ureg, dst, src[0], src[1]);
      break;

   case nir_op_fpow:
      ntt_emit_scalar(c, TGSI_OPCODE_POW, dst, src[0], src[1]);
      break;

   case nir_op_ffma:
      ureg_MAD(c->ureg, dst, src[0], src[1], src[2]);
      break;

   case nir_op_flrp:
      ureg_LRP(c->ureg, dst, src[2], src[1], src[0]);
      break;

   case nir_op_fcsel:
      ureg_UCMP(c->ureg, dst, ureg_negate(src[0]), src[1], src[2]);
      break;

   case nir_op_bcsel:
      ureg_UCMP(c->ureg, dst, src[0], src[1], src[2]);
      break;

   default:
      fprintf(stderr, "Unknown NIR opcode: \n");
      nir_print_instr(&instr->instr, stderr);
      fprintf(stderr, "\n");
      abort();
   }
}

static void
ntt_emit_intrinsic(struct ntt_compile *c, nir_intrinsic_instr *instr)
{
   struct ureg_dst *dst = NULL;
   const nir_intrinsic_info *info = &nir_intrinsic_infos[instr->intrinsic];
   bool only_def = false;

   if (info->has_dest) {
      struct hash_entry *entry = _mesa_hash_table_search(c->def_ht,
                                                         instr->dest.reg.reg);
      dst = ((struct ureg_dst *) entry->data);
//TODO      only_def = instr->dest.reg.reg->defs->entries == 1;
   }

   switch (instr->intrinsic) {

   case nir_intrinsic_vulkan_resource_index: {
       uint32_t set = nir_intrinsic_desc_set(instr);
       uint32_t binding = nir_intrinsic_binding(instr);

       ureg_DECL_constant2D(c->ureg, 0, 11, set + 1);
       break;
   }

   case nir_intrinsic_load_var: {
      struct ureg_src src;
      if (!strcmp(instr->variables[0]->var->name, "gl_VertexIndex")) {
         src = ureg_DECL_system_value(c->ureg, TGSI_SEMANTIC_VERTEXID, 0);
         ureg_MOV(c->ureg, *dst, src);
      } else {
         goto out;
      }
      break;
   }
   case nir_intrinsic_load_ubo: {
       nir_const_value *const_offset;
       struct ureg_src src;

       const_offset = nir_src_as_const_value(instr->src[1]);
       if (const_offset) {
	   int fine = const_offset->u[0] - ((const_offset->u[0]/16)*16);
	   fprintf(stderr, "const is %d fine is %d\n", const_offset->u[0], fine);
	   src = ureg_src_dimension(ureg_src_register(TGSI_FILE_CONSTANT, const_offset->u[0]/16), instr->const_index[0] + 1);

           if (only_def) {
              *dst = ureg_dst(src);
           } else {
              ureg_MOV(c->ureg, *dst, src);
           }
       } else {
          struct ureg_dst temp = ureg_DECL_temporary(c->ureg);
          unsigned value = 4;
          if (!c->addr_declared) {
             c->addr_reg = ureg_DECL_address(c->ureg);
             c->addr_declared = true;
          }
          ureg_USHR(c->ureg, temp, ntt_get_src(c, instr->src[1]), ureg_DECL_immediate_uint(c->ureg, &value, 1));

          ureg_UARL(c->ureg, c->addr_reg, ureg_src(temp));
          ureg_MOV(c->ureg, *dst,
                   ureg_src_dimension(ureg_src_indirect(ureg_src_register(TGSI_FILE_CONSTANT, 0), ureg_src(c->addr_reg)),
                                      instr->const_index[0] + 1));
       }

       break;
   }
   case nir_intrinsic_load_uniform: {
      uint32_t index = instr->const_index[0];
      struct ureg_src src = ureg_src_register(TGSI_FILE_CONSTANT, index);

      assert(instr->const_index[1] == 1);

      if (only_def) {
         *dst = ureg_dst(src);
      } else {
         ureg_MOV(c->ureg, *dst, src);
      }
   }
      break;
#if 0
   case nir_intrinsic_load_uniform_indirect: {
      uint32_t index = instr->const_index[0];

      if (!c->addr_declared) {
         c->addr_reg = ureg_DECL_address(c->ureg);
         c->addr_declared = true;
      }

      ureg_UARL(c->ureg, c->addr_reg, ntt_get_src(c, instr->src[0]));
      assert(instr->const_index[1] == 1);
      ureg_MOV(c->ureg, *dst,
               ureg_src_indirect(ureg_src_register(TGSI_FILE_CONSTANT, index),
                                 ureg_src(c->addr_reg)));
   }
      break;
#endif
   case nir_intrinsic_load_input: {
      uint32_t index = instr->const_index[0];
      struct ureg_src src = ureg_src_register(TGSI_FILE_INPUT,
                                              c->input_index_map[index]);

      //   assert(instr->const_index[1] == 1);

      if (only_def) {
         *dst = ureg_dst(src);
      } else {
         ureg_MOV(c->ureg, *dst, src);
      }
   }
      break;

   case nir_intrinsic_load_output: {
      uint32_t index = instr->const_index[0];
      struct ureg_src src = ureg_src_register(TGSI_FILE_OUTPUT,
                                              c->output_index_map[index]);

      //   assert(instr->const_index[1] == 1);

      if (only_def) {
         *dst = ureg_dst(src);
      } else {
         ureg_MOV(c->ureg, *dst, src);
      }
      break;
   }
   case nir_intrinsic_store_output: {
      struct ureg_src src = ntt_get_src(c, instr->src[0]);
      uint32_t index = instr->const_index[0];
      struct ureg_dst out = ureg_dst_register(TGSI_FILE_OUTPUT,
                                              c->output_index_map[index]);

//      assert(instr->const_index[1] == 1);

      ureg_MOV(c->ureg, out, src);
      break;
   }
   case nir_intrinsic_discard:
      ureg_KILL(c->ureg);
      break;

   default:
      goto out;
      break;
   }

   return;
 out:
   fprintf(stderr, "Unknown intrinsic: ");
   nir_print_instr(&instr->instr, stderr);
   fprintf(stderr, "\n");

}

static void
ntt_emit_load_const(struct ntt_compile *c, nir_load_const_instr *instr)
{
   struct ureg_dst *decl = ralloc(c->def_ht, struct ureg_dst);
   struct ureg_src imm = ureg_DECL_immediate_uint(c->ureg, instr->value.u,
                                                  instr->def.num_components);

   if (imm.SwizzleX == TGSI_SWIZZLE_X &&
       (imm.SwizzleY == TGSI_SWIZZLE_Y || instr->def.num_components < 2) &&
       (imm.SwizzleZ == TGSI_SWIZZLE_Z || instr->def.num_components < 3) &&
       (imm.SwizzleW == TGSI_SWIZZLE_W || instr->def.num_components < 4)) {
      *decl = ureg_dst(imm);
   } else {
      *decl = ureg_DECL_temporary(c->ureg);
      ureg_MOV(c->ureg, *decl, imm);
   }

   _mesa_hash_table_insert(c->def_ht, &instr->def, decl);
}

static void
ntt_emit_texture(struct ntt_compile *c, nir_tex_instr *instr)
{
   struct ureg_dst dst = ntt_get_dst(c, instr->dest);
   struct ureg_src sampler = ureg_DECL_sampler(c->ureg, instr->sampler_index);
   struct ureg_src coordinate, shadow_comparitor, lod, lod2, proj;
   struct ureg_src srcs[4];
   unsigned num_srcs = 0;
   unsigned i;
   unsigned target;
   unsigned shadow_chan = 0, lod_chan = 0, proj_chan = 0;
   unsigned tex_opcode = TGSI_OPCODE_TEX;

   switch (instr->op) {
   case nir_texop_tex:
   default:
      tex_opcode = TGSI_OPCODE_TEX;
      break;
   case nir_texop_txf:
      tex_opcode = TGSI_OPCODE_TXF;
      break;
   }

   for (i = 0; i < instr->num_srcs; i++) {
      struct ureg_src src = ntt_get_src(c, instr->src[i].src);

      switch (instr->src[i].src_type) {
      case nir_tex_src_coord:
         coordinate = src;
         break;
      case nir_tex_src_bias:
         lod = ureg_scalar(src, TGSI_SWIZZLE_X);
         lod_chan = TGSI_WRITEMASK_W;
         tex_opcode = TGSI_OPCODE_TXB;
         break;
      case nir_tex_src_lod:
         lod = ureg_scalar(src, TGSI_SWIZZLE_X);
         lod_chan = TGSI_WRITEMASK_W;
         tex_opcode = TGSI_OPCODE_TXL;
         break;
      case nir_tex_src_comparitor:
         shadow_comparitor = ureg_scalar(src, TGSI_SWIZZLE_X);
         break;
      case nir_tex_src_ddx:
         lod = ureg_scalar(src, TGSI_SWIZZLE_X);
         tex_opcode = TGSI_OPCODE_TXD;
         break;
      case nir_tex_src_ddy:
         lod2 = ureg_scalar(src, TGSI_SWIZZLE_X);
         break;
      case nir_tex_src_projector:
         proj = ureg_scalar(src, TGSI_SWIZZLE_X);
         proj_chan = TGSI_WRITEMASK_W;
         tex_opcode = TGSI_OPCODE_TXP;
         break;
      case nir_tex_src_sampler_offset:
         unreachable("not yet supported");
      default:
         unreachable("unknown texture source");
      }
   }

   switch (instr->sampler_dim) {
   case GLSL_SAMPLER_DIM_1D:
      if (instr->is_array) {
         if (instr->is_shadow) {
            target = TGSI_TEXTURE_SHADOW1D_ARRAY;
            shadow_chan = TGSI_WRITEMASK_Z;
         } else {
            target = TGSI_TEXTURE_1D_ARRAY;
         }
      } else {
         if (instr->is_shadow) {
            target = TGSI_TEXTURE_SHADOW1D;
            shadow_chan = TGSI_WRITEMASK_Z;
         } else {
            target = TGSI_TEXTURE_1D;
         }
      }
      break;
   case GLSL_SAMPLER_DIM_2D:
      if (instr->is_array) {
         if (instr->is_shadow) {
            target = TGSI_TEXTURE_SHADOW2D_ARRAY;
            shadow_chan = TGSI_WRITEMASK_W;
         } else {
            target = TGSI_TEXTURE_2D_ARRAY;
         }
      } else {
         if (instr->is_shadow) {
            target = TGSI_TEXTURE_SHADOW2D;
            shadow_chan = TGSI_WRITEMASK_Z;
         } else {
            target = TGSI_TEXTURE_2D;
         }
      }
      break;
   case GLSL_SAMPLER_DIM_MS:
      if (instr->is_array) {
         target = TGSI_TEXTURE_2D_ARRAY_MSAA;
         shadow_chan = TGSI_WRITEMASK_Z;
      } else {
         target = TGSI_TEXTURE_2D_ARRAY;
      }
      break;
   case GLSL_SAMPLER_DIM_3D:
      assert(!instr->is_shadow);
      target = TGSI_TEXTURE_3D;
      break;
   case GLSL_SAMPLER_DIM_RECT:
      if (instr->is_shadow) {
         target = TGSI_TEXTURE_SHADOWRECT;
         shadow_chan = TGSI_WRITEMASK_Z;
      } else {
         target = TGSI_TEXTURE_RECT;
      }
      break;
   case GLSL_SAMPLER_DIM_CUBE:
      if (instr->is_array) {
         if (instr->is_shadow) {
            target = TGSI_TEXTURE_SHADOWCUBE;
            shadow_chan = TGSI_WRITEMASK_W;
         } else {
            target = TGSI_TEXTURE_CUBE;
         }
      } else {
         if (instr->is_shadow) {
            target = TGSI_TEXTURE_SHADOWCUBE_ARRAY;
            shadow_chan = TGSI_WRITEMASK_W;
         } else {
            target = TGSI_TEXTURE_CUBE_ARRAY;
         }
      }
      break;
   case GLSL_SAMPLER_DIM_BUF:
      break;
   default:
      fprintf(stderr, "Unknown sampler dimensions: %d\n", instr->sampler_dim);
      abort();
   }

   if (shadow_chan != 0 || lod_chan != 0 || proj_chan != 0) {
      struct ureg_dst merged_coord = ureg_DECL_temporary(c->ureg);

      ureg_MOV(c->ureg,
               ureg_writemask(merged_coord, ~(proj_chan |
                                              shadow_chan |
                                              lod_chan)),
               coordinate);
      if (proj_chan) {
         ureg_MOV(c->ureg, ureg_writemask(merged_coord, proj_chan),
                  proj);
      }
      if (shadow_chan) {
         ureg_MOV(c->ureg, ureg_writemask(merged_coord, shadow_chan),
                  shadow_comparitor);
      }
      if (lod_chan) {
         ureg_MOV(c->ureg, ureg_writemask(merged_coord, lod_chan),
                  lod);
      }

      coordinate = ureg_src(merged_coord);
   }

   srcs[num_srcs++] = coordinate;
   if (instr->op == nir_texop_txd) {
      srcs[num_srcs++] = lod;
      srcs[num_srcs++] = lod2;
   }
   srcs[num_srcs++] = sampler;

   ureg_tex_insn(c->ureg, tex_opcode,
                 &dst, 1,
                 target,
                 NULL, 0, /* offsets */
                 srcs, num_srcs);
}

static void
ntt_emit_jump(struct ntt_compile *c, nir_jump_instr *jump)
{
   switch (jump->type) {
   case nir_jump_break:
      ureg_BRK(c->ureg);
      break;

   case nir_jump_continue:
      ureg_CONT(c->ureg);
      break;

   default:
      fprintf(stderr, "Unknown jump instruction: ");
      nir_print_instr(&jump->instr, stderr);
      fprintf(stderr, "\n");
      abort();
   }
}

static void
ntt_emit_instr(struct ntt_compile *c, nir_instr *instr)
{
   switch (instr->type) {
   case nir_instr_type_alu:
      ntt_emit_alu(c, nir_instr_as_alu(instr));
      break;

   case nir_instr_type_intrinsic:
      ntt_emit_intrinsic(c, nir_instr_as_intrinsic(instr));
      break;

   case nir_instr_type_load_const:
      ntt_emit_load_const(c, nir_instr_as_load_const(instr));
      break;

   case nir_instr_type_tex:
      ntt_emit_texture(c, nir_instr_as_tex(instr));
      break;

   case nir_instr_type_jump:
      ntt_emit_jump(c, nir_instr_as_jump(instr));
      break;

   default:
      fprintf(stderr, "Unknown NIR instr type: ");
      nir_print_instr(instr, stderr);
      fprintf(stderr, "\n");
      abort();
   }
}

static bool
ntt_emit_if_as_conditional_discard(struct ntt_compile *c, nir_if *if_stmt)
{
   /* XXX: Detect that it is actually this style of IF. */
   nir_cf_node *first_if_node = nir_if_first_then_node(if_stmt);
   nir_block *block = nir_cf_node_as_block(first_if_node);
   nir_instr *instr = nir_block_first_instr(block);

   nir_intrinsic_instr *discard = nir_instr_as_intrinsic(instr);
   if (discard->intrinsic != nir_intrinsic_discard)
      return false;

   ureg_KILL_IF(c->ureg, ureg_scalar(ntt_get_src(c, if_stmt->condition),
                                     TGSI_SWIZZLE_X));

   return true;
}

static void
ntt_emit_if(struct ntt_compile *c, nir_if *if_stmt)
{
   if (ntt_emit_if_as_conditional_discard(c, if_stmt))
      return;

   unsigned label;
   ureg_UIF(c->ureg, ntt_get_src(c, if_stmt->condition), &label);
   ntt_emit_cf_list(c, &if_stmt->then_list);

   if (!exec_list_is_empty(&if_stmt->else_list)) {
      ureg_fixup_label(c->ureg, label, ureg_get_instruction_number(c->ureg));
      ureg_ELSE(c->ureg, &label);
      ntt_emit_cf_list(c, &if_stmt->else_list);
   }

   ureg_fixup_label(c->ureg, label, ureg_get_instruction_number(c->ureg));
   ureg_ENDIF(c->ureg);
}

static void
ntt_emit_loop(struct ntt_compile *c, nir_loop *loop)
{
   unsigned last_loop_label = c->loop_label;

   unsigned begin_label;
   ureg_BGNLOOP(c->ureg, &begin_label);
   ntt_emit_cf_list(c, &loop->body);

   // XXX ureg_fixup_label(c->ureg, label, ureg_get_instruction_number(c->ureg));
   unsigned end_label;
   ureg_ENDLOOP(c->ureg, &end_label);

   c->loop_label = last_loop_label;
}

static void
ntt_emit_block(struct ntt_compile *c, nir_block *block)
{
   nir_foreach_instr(block, instr) {
      ntt_emit_instr(c, instr);
   }
}

static void
ntt_emit_cf_list(struct ntt_compile *c, struct exec_list *list)
{
   foreach_list_typed(nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_block:
         ntt_emit_block(c, nir_cf_node_as_block(node));
         break;

      case nir_cf_node_if:
         ntt_emit_if(c, nir_cf_node_as_if(node));
         break;

      case nir_cf_node_loop:
         ntt_emit_loop(c, nir_cf_node_as_loop(node));
         break;

      default:
         assert(0);
      }
   }
}

static void
ntt_emit_impl(struct ntt_compile *c, nir_function_impl *impl)
{
   ntt_setup_registers(c, &impl->registers);
   ntt_emit_cf_list(c, &impl->body);
}

const void *
nir_to_tgsi(struct nir_shader *s, unsigned tgsi_target)
{
   struct ntt_compile *c;
   const void *tgsi_tokens;

   nir_print_shader(s, stdout);
   nir_convert_from_ssa(s, false);
   nir_lower_vec_to_movs(s);

   if (s->stage == MESA_SHADER_VERTEX) {
      foreach_list_typed(nir_variable, var, node, &s->inputs) {
         var->data.driver_location = var->data.location;
      }
      foreach_list_typed(nir_variable, var, node, &s->outputs) {
         var->data.driver_location = var->data.location;
      }
   }


   nir_lower_io(s, nir_var_shader_in, glsl_type_size_vec4);
   nir_lower_io(s, nir_var_shader_out, glsl_type_size_vec4);

   nir_print_shader(s, stdout);
   c = rzalloc(NULL, struct ntt_compile);

   c->s = s;
   c->ureg = ureg_create(tgsi_target);
   c->target = tgsi_target;

   c->def_ht = _mesa_hash_table_create(c,
                                       _mesa_hash_pointer,
                                       _mesa_key_pointer_equal);

   ntt_setup_inputs(c);
   ntt_setup_outputs(c);
   ntt_setup_uniforms(c);
   ntt_setup_registers(c, &c->s->registers);

   /* Find the main function and emit the body. */
   nir_foreach_function(c->s, function) {
      assert(strcmp(function->name, "main") == 0);
      assert(function->impl);
      ntt_emit_impl(c, function->impl);
      ureg_END(c->ureg);
   }

   tgsi_tokens = ureg_get_tokens(c->ureg, NULL);

   ureg_destroy(c->ureg);

   ralloc_free(c);

   return tgsi_tokens;
}

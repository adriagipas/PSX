/*
 * Copyright 2015-2026 Adrià Giménez Pastor.
 *
 * This file is part of adriagipas/PSX.
 *
 * adriagipas/PSX is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * adriagipas/PSX is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with adriagipas/PSX.  If not, see <https://www.gnu.org/licenses/>.
 */
/*
 *  cpu_decode.c - Implementació del descodificador.
 *
 */


#include <stddef.h>
#include <stdlib.h>

#include "PSX.h"




/**********/
/* MACROS */
/**********/

#define SPECIAL_RD_RT_SA(NAME)        		\
  inst->name= NAME;        			\
  inst->op1= PSX_RD; inst->extra.rd= rd;        \
  inst->op2= PSX_RT; inst->extra.rt= rt;        \
  inst->op3= PSX_SA; inst->extra.sa= sa

#define SPECIAL_RD_RT_RS(NAME)        		\
  inst->name= NAME;        			\
  inst->op1= PSX_RD; inst->extra.rd= rd;        \
  inst->op2= PSX_RT; inst->extra.rt= rt;        \
  inst->op3= PSX_RS; inst->extra.rs= rs

#define SPECIAL_RD_RS_RT(NAME)        		\
  inst->name= NAME;        			\
  inst->op1= PSX_RD; inst->extra.rd= rd;        \
  inst->op2= PSX_RS; inst->extra.rs= rs;        \
  inst->op3= PSX_RT; inst->extra.rt= rt

#define SPECIAL_RD_RS(NAME)        		\
  inst->name= NAME;        			\
  inst->op1= PSX_RD; inst->extra.rd= rd;        \
  inst->op2= PSX_RS; inst->extra.rs= rs;        \
  inst->op3= PSX_NONE

#define SPECIAL_RS_RT(NAME)        		\
  inst->name= NAME;        			\
  inst->op1= PSX_RS; inst->extra.rs= rs;        \
  inst->op2= PSX_RT; inst->extra.rt= rt;        \
  inst->op3= PSX_NONE

#define SPECIAL_RD(NAME)        		\
  inst->name= NAME;        			\
  inst->op1= PSX_RD; inst->extra.rd= rd;        \
  inst->op2= inst->op3= PSX_NONE

#define SPECIAL_RS(NAME)        		\
  inst->name= NAME;        			\
  inst->op1= PSX_RS; inst->extra.rs= rs;        \
  inst->op2= inst->op3= PSX_NONE

#define SPECIAL_NONE(NAME)        		\
  inst->name= NAME;        			\
  inst->op1= inst->op2= inst->op3= PSX_NONE

#define SIGN_EXTEND18(U16)        		\
  (((int32_t) ((int16_t) (U16)))<<2)

#define SIGN_EXTEND16(U16)                      \
  ((uint32_t) ((int32_t) ((int16_t) (U16))))




/*********************/
/* FUNCIONS PRIVADES */
/*********************/

static bool
mem_read (
          const uint32_t  addr,
          uint32_t       *dst
          )
{

  if ( addr&0x3 ) return false;

  /* kuseg */
  if ( addr < 0x80000000 )
    {
      if ( !PSX_mem_read ( addr, dst ) ) return false;
    }

  /* ksge0. */
  else if ( addr < 0xA0000000 )
    {
      if ( !PSX_mem_read ( addr&0x1FFFFFFF, dst ) ) return false;
    }

  /* kseg1. */
  else if ( addr < 0xC0000000 )
    {
      if ( (addr >= 0xBF800000 && addr <= 0xBF801000 ) ) return false;
      if ( !PSX_mem_read ( addr&0x1FFFFFFF, dst ) ) return false;
    }

  /* Control de cache i basura. */
  /* En realitat hi han zones ací que es poden llegir però sols és un
     registre i 0s, per tant per a descodificar ho considere error. */
  else return false;
  
  return true;
  
} /* end mem_read */


static void
decode_special (
        	const PSX_Word  word,
        	PSX_Inst       *inst
        	)
{

  uint32_t rs,rt,rd,sa,func;

  
  /* Camps comuns. */
  rs= (word.v>>21)&0x1F;
  rt= (word.v>>16)&0x1F;
  rd= (word.v>>11)&0x1F;
  sa= (word.v>>6)&0x1F;
  func= word.v&0x3F;

  /* Descodifica. */
  switch ( func )
    {
      
    case 0x00: SPECIAL_RD_RT_SA ( PSX_SLL ); break;
      
    case 0x02: SPECIAL_RD_RT_SA ( PSX_SRL ); break;
    case 0x03: SPECIAL_RD_RT_SA ( PSX_SRA ); break;
    case 0x04: SPECIAL_RD_RT_RS ( PSX_SLLV ); break;
      
    case 0x06: SPECIAL_RD_RT_RS ( PSX_SRLV ); break;
    case 0x07: SPECIAL_RD_RT_RS ( PSX_SRAV ); break;
    case 0x08: SPECIAL_RS ( PSX_JR ); break;
    case 0x09: SPECIAL_RD_RS ( PSX_JALR ); break;
      
    case 0x0C: SPECIAL_NONE ( PSX_SYSCALL ); break;
    case 0x0D: SPECIAL_NONE ( PSX_BREAK ); break;
      
    case 0x10: SPECIAL_RD ( PSX_MFHI ); break;
    case 0x11: SPECIAL_RS ( PSX_MTHI ); break;
    case 0x12: SPECIAL_RD ( PSX_MFLO ); break;
    case 0x13: SPECIAL_RS ( PSX_MTLO ); break;
      
    case 0x18: SPECIAL_RS_RT ( PSX_MULT ); break;
    case 0x19: SPECIAL_RS_RT ( PSX_MULTU ); break;
    case 0x1A: SPECIAL_RS_RT ( PSX_DIV ); break;
    case 0x1B: SPECIAL_RS_RT ( PSX_DIVU ); break;

    case 0x20: SPECIAL_RD_RS_RT ( PSX_ADD ); break;
    case 0x21: SPECIAL_RD_RS_RT ( PSX_ADDU ); break;
    case 0x22: SPECIAL_RD_RS_RT ( PSX_SUB ); break;
    case 0x23: SPECIAL_RD_RS_RT ( PSX_SUBU ); break;
    case 0x24: SPECIAL_RD_RS_RT ( PSX_AND ); break;
    case 0x25: SPECIAL_RD_RS_RT ( PSX_OR ); break;
    case 0x26: SPECIAL_RD_RS_RT ( PSX_XOR ); break;
    case 0x27: SPECIAL_RD_RS_RT ( PSX_NOR ); break;

    case 0x2A: SPECIAL_RD_RS_RT ( PSX_SLT ); break;
    case 0x2B: SPECIAL_RD_RS_RT ( PSX_SLTU ); break;
      
    default:
      inst->name= PSX_UNK;
      inst->op1= inst->op2= inst->op3= PSX_NONE;
    }
  
} /* end decode_special */


static void
decode_bcond (
              const PSX_Word  word,
              PSX_Inst       *inst
              )
{
  
  /* Fixa camps. */
  inst->extra.rs= (word.v>>21)&0x1F;
  inst->extra.off= SIGN_EXTEND18 ( word.w.v0 );
  inst->op1= PSX_RS;
  inst->op2= PSX_OFFSET;
  inst->op3= PSX_UNK;

  
  switch ( (word.v>>16)&0x1F )
    {
      
    case 0x00: inst->name= PSX_BLTZ; break;
    case 0x01: inst->name= PSX_BGEZ; break;

    case 0x10: inst->name= PSX_BLTZAL; break;
    case 0x11: inst->name= PSX_BGEZAL; break;
      
    default:
      inst->name= PSX_UNK;
      inst->op1= inst->op2= inst->op3= PSX_NONE;
        
    }
  
} /* end decode_bcond */


static void
decode_target (
               const PSX_Word      word,
               PSX_Inst           *inst,
               const uint32_t      addr,
               const PSX_Mnemonic  name
               )
{

  inst->name= name;
  inst->extra.imm= ((addr+2)&0xF0000000) | ((word.v&0x03FFFFFF)<<2);
  inst->op1= PSX_ADDR;
  inst->op2= inst->op3= PSX_NONE;
  
} /* end decode_target */


static void
decode_rt_offset_base (
        	       const PSX_Word      word,
        	       PSX_Inst           *inst,
        	       const PSX_Mnemonic  name
        	       )
{

  inst->name= name;
  inst->extra.rs= (word.v>>21)&0x1F;
  inst->extra.rt= (word.v>>16)&0x1F;
  inst->extra.off= (int32_t) SIGN_EXTEND16 ( word.w.v0 );
  inst->op1= PSX_RT;
  inst->op2= PSX_OFFSET_BASE;
  inst->op3= PSX_NONE;
  
} /* end decode_rt_offset_base */


static void
decode_rs_rt_offset (
        	     const PSX_Word      word,
        	     PSX_Inst           *inst,
        	     const PSX_Mnemonic  name
        	     )
{

  inst->name= name;
  inst->extra.rs= (word.v>>21)&0x1F;
  inst->extra.rt= (word.v>>16)&0x1F;
  inst->extra.off= SIGN_EXTEND18 ( word.w.v0 );
  inst->op1= PSX_RS;
  inst->op2= PSX_RT;
  inst->op3= PSX_OFFSET;
  
} /* end decode_rs_rt_offset */


static void
decode_rt_rs_simm (
        	   const PSX_Word      word,
        	   PSX_Inst           *inst,
        	   const PSX_Mnemonic  name
        	   )
{

  inst->name= name;
  inst->extra.rs= (word.v>>21)&0x1F;
  inst->extra.rt= (word.v>>16)&0x1F;
  inst->extra.imm= SIGN_EXTEND16 ( word.w.v0 );
  inst->op1= PSX_RT;
  inst->op2= PSX_RS;
  inst->op3= PSX_IMMEDIATE;
  
} /* end decode_rt_rs_simm */


static void
decode_rt_rs_imm (
        	  const PSX_Word      word,
        	  PSX_Inst           *inst,
        	  const PSX_Mnemonic  name
        	  )
{

  inst->name= name;
  inst->extra.rs= (word.v>>21)&0x1F;
  inst->extra.rt= (word.v>>16)&0x1F;
  inst->extra.imm= (uint32_t) word.w.v0;
  inst->op1= PSX_RT;
  inst->op2= PSX_RS;
  inst->op3= PSX_IMMEDIATE;
  
} /* end decode_rt_rs_imm */


static void
decode_rs_offset (
        	  const PSX_Word      word,
        	  PSX_Inst           *inst,
        	  const PSX_Mnemonic  name
        	  )
{

  inst->name= name;
  inst->extra.rs= (word.v>>21)&0x1F;
  inst->extra.off= SIGN_EXTEND18 ( word.w.v0 );
  inst->op1= PSX_RS;
  inst->op2= PSX_OFFSET;
  inst->op3= PSX_NONE;
  
} /* end decode_rs_offset */


static void
decode_cop0 (
             const PSX_Word  word,
             PSX_Inst       *inst
             )
{
  
  uint32_t rs,rt,rd,func;

  
  /* Camps comuns. */
  rs= (word.v>>21)&0x1F;
  rt= (word.v>>16)&0x1F;
  rd= (word.v>>11)&0x1F;
  func= word.v&0x3F;

  /* Descodifica. */
  if ( rs&0x10 )
    {
      inst->op1= inst->op2= inst->op3= PSX_NONE;
      switch ( func )
        {
        case 0x01: inst->name= PSX_COP0_TLBR; break;
        case 0x02: inst->name= PSX_COP0_TLBWI; break;
        case 0x06: inst->name= PSX_COP0_TLBWR; break;
        case 0x08: inst->name= PSX_COP0_TLBP; break;
        case 0x10: inst->name= PSX_COP0_RFE; break;
        default: inst->name= PSX_UNK;
        }
    }
  else
    {
      inst->op1= PSX_RT; inst->extra.rt= rt;
      inst->op2= PSX_COP0_REG; inst->extra.rd= rd;
      inst->op3= PSX_NONE;
      switch ( rs )
        {
        case 0x00: inst->name= PSX_MFC0; break;
        case 0x04: inst->name= PSX_MTC0; break;
        default:
          inst->name= PSX_UNK;
          inst->op1= inst->op2= inst->op3= PSX_NONE;
        }
    }
  
} /* end decode_cop0 */


static void
decode_cop2 (
             const PSX_Word  word,
             PSX_Inst       *inst
             )
{
  
  uint32_t rs,rt,rd,func;
  
  
  /* Camps comuns. */
  rs= (word.v>>21)&0x1F;
  rt= (word.v>>16)&0x1F;
  rd= (word.v>>11)&0x1F;
  func= word.v&0x3F;
  
  /* Descodifica. */
  if ( rs&0x10 )
    {
      inst->extra.cop2_sf= ((word.v&0x00080000)!=0 ? 12 : 0);
      inst->extra.cop2_lm_is_0= (word.v&0x00000400)==0;
      inst->extra.cop2_mx= ((word.v>>17)&0x3);
      inst->extra.cop2_v= ((word.v>>15)&0x3);
      inst->extra.cop2_cv= ((word.v>>13)&0x3);
      inst->op1= inst->op2= inst->op3= PSX_NONE;
      switch ( func )
        {
        case 0x01: inst->name= PSX_COP2_RTPS; inst->op1= PSX_COP2_SF; break;
        case 0x06: inst->name= PSX_COP2_NCLIP; break;
        case 0x0C:
          inst->name= PSX_COP2_OP;
          inst->op1= PSX_COP2_SF;
          inst->op2= PSX_COP2_LM;
          break;
        case 0x10: inst->name= PSX_COP2_DPCS; inst->op1= PSX_COP2_SF; break;
        case 0x11: inst->name= PSX_COP2_INTPL; inst->op1= PSX_COP2_SF; break;
        case 0x12:
          inst->name= PSX_COP2_MVMVA;
          inst->op1= PSX_COP2_SF;
          inst->op2= PSX_COP2_MX_V_CV;
          inst->op3= PSX_COP2_LM;
          break;
        case 0x13: inst->name= PSX_COP2_NCDS; inst->op1= PSX_COP2_SF; break;
        case 0x14: inst->name= PSX_COP2_CDP; inst->op1= PSX_COP2_SF; break;
        case 0x16: inst->name= PSX_COP2_NCDT; inst->op1= PSX_COP2_SF; break;
        case 0x1B: inst->name= PSX_COP2_NCCS; inst->op1= PSX_COP2_SF; break;
        case 0x1C: inst->name= PSX_COP2_CC; inst->op1= PSX_COP2_SF; break;
        case 0x1E: inst->name= PSX_COP2_NCS; inst->op1= PSX_COP2_SF; break;
        case 0x20: inst->name= PSX_COP2_NCT; inst->op1= PSX_COP2_SF; break;
        case 0x28: inst->name= PSX_COP2_SQR; inst->op1= PSX_COP2_SF; break;
        case 0x29: inst->name= PSX_COP2_DCPL; inst->op1= PSX_COP2_SF; break;
        case 0x2A: inst->name= PSX_COP2_DPCT; inst->op1= PSX_COP2_SF; break;
        case 0x2D: inst->name= PSX_COP2_AVSZ3; break;
        case 0x2E: inst->name= PSX_COP2_AVSZ4; break;
        case 0x30: inst->name= PSX_COP2_RTPT; inst->op1= PSX_COP2_SF; break;
        case 0x3D:
          inst->name= PSX_COP2_GPF;
          inst->op1= PSX_COP2_SF;
          inst->op2= PSX_COP2_LM;
          break;
        case 0x3E:
          inst->name= PSX_COP2_GPL;
          inst->op1= PSX_COP2_SF;
          inst->op2= PSX_COP2_LM;
          break;
        case 0x3F: inst->name= PSX_COP2_NCCT; inst->op1= PSX_COP2_SF; break;
        default: inst->name= PSX_UNK;
        }
    }
  else
    {
      inst->op1= PSX_RT; inst->extra.rt= rt;
      inst->extra.rd= rd;
      inst->op3= PSX_NONE;
      switch ( rs )
        {
        case 0x00: inst->name= PSX_MFC2; inst->op2= PSX_COP2_REG; break;
        case 0x02: inst->name= PSX_CFC2; inst->op2= PSX_COP2_REG_CTRL; break;
        case 0x04: inst->name= PSX_MTC2; inst->op2= PSX_COP2_REG; break;
        case 0x06: inst->name= PSX_CTC2; inst->op2= PSX_COP2_REG_CTRL; break;
        default:
          inst->name= PSX_UNK;
          inst->op1= inst->op2= inst->op3= PSX_NONE;
        }
    }
  
} /* end decode_cop2 */


static void
decode (
        const PSX_Word  word,
        PSX_Inst       *inst,
        const uint32_t  addr
        )
{

  uint32_t opcode;


  inst->word= word.v;
  
  opcode= word.v>>26;
  switch ( opcode )
    {
    case 0x00: return decode_special ( word, inst );
    case 0x01: return decode_bcond ( word, inst );
    case 0x02: return decode_target ( word, inst, addr, PSX_J );
    case 0x03: return decode_target ( word, inst, addr, PSX_JAL );
    case 0x04: return decode_rs_rt_offset ( word, inst, PSX_BEQ );
    case 0x05: return decode_rs_rt_offset ( word, inst, PSX_BNE );
    case 0x06: return decode_rs_offset ( word, inst, PSX_BLEZ );
    case 0x07: return decode_rs_offset ( word, inst, PSX_BGTZ );
    case 0x08: return decode_rt_rs_simm ( word, inst, PSX_ADDI );
    case 0x09: return decode_rt_rs_simm ( word, inst, PSX_ADDIU );
    case 0x0A: return decode_rt_rs_simm ( word, inst, PSX_SLTI );
    case 0x0B: return decode_rt_rs_simm ( word, inst, PSX_SLTIU );
    case 0x0C: return decode_rt_rs_imm ( word, inst, PSX_ANDI );
    case 0x0D: return decode_rt_rs_imm ( word, inst, PSX_ORI );
    case 0x0E: return decode_rt_rs_imm ( word, inst, PSX_XORI );
    case 0x0F: /* LUI */
      inst->name= PSX_LUI;
      inst->extra.rt= (word.v>>16)&0x1F;
      inst->extra.imm= ((uint32_t) word.w.v0)<<16;
      inst->op1= PSX_RT;
      inst->op2= PSX_IMMEDIATE;
      inst->op3= PSX_NONE;
      break;
    case 0x10: return decode_cop0 ( word, inst );

    case 0x12: return decode_cop2 ( word, inst );
      
    case 0x20: return decode_rt_offset_base ( word, inst, PSX_LB );
    case 0x21: return decode_rt_offset_base ( word, inst, PSX_LH );
    case 0x22: return decode_rt_offset_base ( word, inst, PSX_LWL );
    case 0x23: return decode_rt_offset_base ( word, inst, PSX_LW );
    case 0x24: return decode_rt_offset_base ( word, inst, PSX_LBU );
    case 0x25: return decode_rt_offset_base ( word, inst, PSX_LHU );
    case 0x26: return decode_rt_offset_base ( word, inst, PSX_LWR );

    case 0x28: return decode_rt_offset_base ( word, inst, PSX_SB );
    case 0x29: return decode_rt_offset_base ( word, inst, PSX_SH );
    case 0x2A: return decode_rt_offset_base ( word, inst, PSX_SWL );
    case 0x2B: return decode_rt_offset_base ( word, inst, PSX_SW );
      
    case 0x2E: return decode_rt_offset_base ( word, inst, PSX_SWR );

    case 0x32: return decode_rt_offset_base ( word, inst, PSX_LWC2 );

    case 0x3A: return decode_rt_offset_base ( word, inst, PSX_SWC2 );
      
    default:
      inst->name= PSX_UNK;
      inst->op1= inst->op2= inst->op3= PSX_NONE;
    }
  
} /* end decode */



/**********************/
/* FUNCIONS PÚBLIQUES */
/**********************/

bool
PSX_cpu_decode (
        	const uint32_t  addr,
        	PSX_Inst       *inst
        	)
{

  PSX_Word word;

  
  if ( !PSX_cpu_test_next_inst () ) return false;
  if ( !mem_read ( addr, &word.v ) ) return false;
  decode ( word, inst, addr );
  
  return true;
  
} // end PSX_cpu_decode

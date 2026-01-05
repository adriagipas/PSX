/*
 * Copyright 2017-2026 Adrià Giménez Pastor.
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
 *  psxmodule.c - Mòdul que implementa un simulador de PlayStation per
 *                a debug.
 *
 */

#include <Python.h>
#include <SDL/SDL.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "PSX.h"



// NYAPA PER A DEBUG
int MY_GLOBAL_PRINT=0;

/**********/
/* MACROS */
/**********/

#define CHECK_INITIALIZED                                               \
  do {                                                                  \
    if ( !_initialized )                                                \
      {                                                                 \
        PyErr_SetString ( PSXError, "Module must be initialized" );      \
        return NULL;                                                    \
      }                                                                 \
  } while(0)
  
#define SHOW_PC_CC                                                      \
  if ( _tracer.dbg_flags&DBG_SHOW_PC_CC )                               \
    printf ( "STP: %016lX CC: %016lX PC: %08X  ",                       \
             _tracer.steps, _tracer.cc, _tracer.pc )

#define DBG_MEM_CHANGED     0x01
#define DBG_MEM_ACCESS      0x02
#define DBG_MEM_ACCESS16    0x04
#define DBG_MEM_ACCESS8     0x08
#define DBG_CPU_INST        0x10
#define DBG_GPU_CMD_TRACE   0x20
#define DBG_CD_CMD_TRACE    0x40
#define DBG_INT_TRACE       0x80
#define DBG_SHOW_PC_CC      0x100
#define DBG_DMA_TRANSFER    0x200
#define DBG_GTE_MEM_ACCESS  0x400
#define DBG_GTE_CMD_TRACE   0x800
#define DBG_BIOS_FUNC_TRACE 0x1000

#define MEMCARD_SIZE (128*1024)

#define NBUFF 4




/*********/
/* TIPUS */
/*********/

typedef struct
{
  
  int16_t      *v;
  volatile int  full;
  
} buffer_t;




/*********/
/* ESTAT */
/*********/

/* Error. */
static PyObject *PSXError;

/* Inicialitzat. */
static char _initialized;

/* Pantalla. */
static struct
{
  
  int          width;
  int          height;
  SDL_Surface *surface;
  
} _screen;

/* Control. */
static struct
{
  
  PSX_ControllerState pad1;
  PSX_ControllerState pad2;
  
} _control;

/* Estat so. */
static struct
{
  
  buffer_t buffers[NBUFF];
  int      buff_in;
  int      buff_out;
  char     silence;
  int      pos;
  int      size;
  int      nsamples;
  double   ratio;
  double   pos2;
  
} _audio;

/* BIOS. */
static uint8_t _bios[PSX_BIOS_SIZE];

/* Renderer. */
static PSX_Renderer *_renderer;

/* Tracer. */
static struct
{
  PyObject *obj;
  int       has_cpu_inst;
  int       has_mem_changed;
  int       has_mem_access;
  int       has_mem_access16;
  int       has_mem_access8;
  int       has_gpu_cmd_trace;
  int       has_cd_cmd_trace;
  int       dbg_flags;
  uint64_t  cc;
  uint32_t  pc;
  uint64_t  steps;
} _tracer;

/* Disc. */
static CD_Disc *_disc;




/*********/
/* DEBUG */
/*********/

static const char *
get_inst_mnemonic (
        	   const PSX_Mnemonic name
        	   )
{

  switch ( name )
    {
    case PSX_UNK        : return "UNK        ";
    case PSX_ADD        : return "ADD        ";
    case PSX_ADDI       : return "ADDI       ";
    case PSX_ADDIU      : return "ADDIU      ";
    case PSX_ADDU       : return "ADDU       ";
    case PSX_AND        : return "AND        ";
    case PSX_ANDI       : return "ANDI       ";
    case PSX_BEQ        : return "BEQ        ";
    case PSX_BGEZ       : return "BGEZ       ";
    case PSX_BGEZAL     : return "BGEZAL     ";
    case PSX_BGTZ       : return "BGTZ       ";
    case PSX_BLEZ       : return "BLEZ       ";
    case PSX_BLTZ       : return "BLTZ       ";
    case PSX_BLTZAL     : return "BLTZAL     ";
    case PSX_BNE        : return "BNE        ";
    case PSX_BREAK      : return "BREAK      ";
    case PSX_CFC2       : return "CFC2       ";
    case PSX_COP0_RFE   : return "COP0.RFE   ";
    case PSX_COP0_TLBP  : return "COP0.TLBP  ";
    case PSX_COP0_TLBR  : return "COP0.TLBR  ";
    case PSX_COP0_TLBWI : return "COP0.TLBWI ";
    case PSX_COP0_TLBWR : return "COP0.TLBWR ";
    case PSX_COP2_RTPS  : return "COP2.RTPS  ";
    case PSX_COP2_RTPT  : return "COP2.RTPT  ";
    case PSX_COP2_NCLIP : return "COP2.NCLIP ";
    case PSX_COP2_AVSZ3 : return "COP2.AVSZ3 ";
    case PSX_COP2_AVSZ4 : return "COP2.AVSZ4 ";
    case PSX_COP2_MVMVA : return "COP2.MVMVA ";
    case PSX_COP2_SQR   : return "COP2.SQR   ";
    case PSX_COP2_OP    : return "COP2.OP    ";
    case PSX_COP2_NCS   : return "COP2.NCS   ";
    case PSX_COP2_NCT   : return "COP2.NCT   ";
    case PSX_COP2_NCCS  : return "COP2.NCCS  ";
    case PSX_COP2_NCCT  : return "COP2.NCCT  ";
    case PSX_COP2_NCDS  : return "COP2.NCDS  ";
    case PSX_COP2_NCDT  : return "COP2.NCDT  ";
    case PSX_COP2_CC    : return "COP2.CC    ";
    case PSX_COP2_CDP   : return "COP2.CDP   ";
    case PSX_COP2_DCPL  : return "COP2.DCPL  ";
    case PSX_COP2_DPCS  : return "COP2.DPCS  ";
    case PSX_COP2_DPCT  : return "COP2.DPCT  ";
    case PSX_COP2_INTPL : return "COP2.INTPL ";
    case PSX_COP2_GPF   : return "COP2.GPF   ";
    case PSX_COP2_GPL   : return "COP2.GPL   ";
    case PSX_CTC2       : return "CTC2       ";
    case PSX_DIV        : return "DIV        ";
    case PSX_DIVU       : return "DIVU       ";
    case PSX_J          : return "J          ";
    case PSX_JAL        : return "JAL        ";
    case PSX_JALR       : return "JALR       ";
    case PSX_JR         : return "JR         ";
    case PSX_LB         : return "LB         ";
    case PSX_LBU        : return "LBU        ";
    case PSX_LH         : return "LH         ";
    case PSX_LHU        : return "LHU        ";
    case PSX_LUI        : return "LUI        ";
    case PSX_LW         : return "LW         ";
    case PSX_LWC2       : return "LWC2       ";
    case PSX_LWL        : return "LWL        ";
    case PSX_LWR        : return "LWR        ";
    case PSX_MFC0       : return "MFC0       ";
    case PSX_MFC2       : return "MFC2       ";
    case PSX_MFHI       : return "MFHI       ";
    case PSX_MFLO       : return "MFLO       ";
    case PSX_MTC0       : return "MTC0       ";
    case PSX_MTC2       : return "MTC2       ";
    case PSX_MTHI       : return "MTHI       ";
    case PSX_MTLO       : return "MTLO       ";
    case PSX_MULT       : return "MULT       ";
    case PSX_MULTU      : return "MULTU      ";
    case PSX_NOR        : return "NOR        ";
    case PSX_OR         : return "OR         ";
    case PSX_ORI        : return "ORI        ";
    case PSX_SB         : return "SB         ";
    case PSX_SH         : return "SH         ";
    case PSX_SLL        : return "SLL        ";
    case PSX_SLLV       : return "SLLV       ";
    case PSX_SLT        : return "SLT        ";
    case PSX_SLTI       : return "SLTI       ";
    case PSX_SLTIU      : return "SLTIU      ";
    case PSX_SLTU       : return "SLTU       ";
    case PSX_SRA        : return "SRA        ";
    case PSX_SRAV       : return "SRAV       ";
    case PSX_SRL        : return "SRL        ";
    case PSX_SRLV       : return "SRLV       ";
    case PSX_SUB        : return "SUB        ";
    case PSX_SUBU       : return "SUBU       ";
    case PSX_SW         : return "SW         ";
    case PSX_SWC2       : return "SWC2       ";
    case PSX_SWL        : return "SWL        ";
    case PSX_SWR        : return "SWR        ";
    case PSX_SYSCALL    : return "SYSCALL    ";
    case PSX_XOR        : return "XOR        ";
    case PSX_XORI       : return "XORI       ";
    default             : return "UNK        ";
    }
  
} // end get_inst_mnemonic


static const char *
get_inst_reg_name (
        	   const int reg
        	   )
{

  static char buffer[10];


  switch ( reg )
    {
    case 0: return "zero";
    case 1: return "$at";
    case 2 ... 3: sprintf ( buffer, "$v%d", reg-2 ); break;
    case 4 ... 7: sprintf ( buffer, "$a%d", reg-4 ); break;
    case 8 ... 15: sprintf ( buffer, "$t%d", reg-8 ); break;
    case 16 ... 23: sprintf ( buffer, "$s%d", reg-16 ); break;
    case 24 ... 25: sprintf ( buffer, "$t%d", reg-24+8 ); break;
    case 26 ... 27: sprintf ( buffer, "$k%d", reg-26 ); break;
    case 28: return "$gp";
    case 29: return "$sp";
    case 30: return "$fp";
    case 31:
    default: return "$ra";
    }
  
  return &(buffer[0]);
  
} // end get_inst_reg_name


static const char *
get_inst_cop0_reg_name (
        		const int reg
        		)
{

  static char buffer[10];


  switch ( reg )
    {
    case 3: return "BPC";
    case 5: return "BDA";
    case 6: return "JUMPDEST";
    case 7: return "DCIC";
    case 8: return "BadVaddr";
    case 9: return "BDAM";
    case 11: return "BPCM";
    case 12: return "SR";
    case 13: return "CAUSE";
    case 14: return "EPC";
    case 15: return "PRID";
    default:
      sprintf ( buffer, "r%d", reg );
      return &(buffer[0]);
    }
  
} // end get_inst_cop0_reg_name

static const char *GTE_REGS[]=
  {
    "VXY0", "VZ0", "VXY1", "VZ1", "VXY2", "VZ2", "RGBC", "OTZ", "IR0",
    "IR1", "IR2", "IR3", "SXY0", "SXY1", "SXY2", "SXYP", "SZ0", "SZ1",
    "SZ2", "SZ3", "RGB0", "RGB1", "RGB2", "RES1", "MAC0", "MAC1", "MAC2",
    "MAC3", "IRGB", "ORGB", "LZCS", "LZCR", "RT11RT12", "RT13RT21",
    "RT22RT23", "RT31RT32", "RT33", "TRX", "TRY", "TRZ", "L11L12", "L13L21",
    "L22L23", "L31L32", "L33", "RBK", "GBK", "BBK", "LR1LR2", "LR3LG1",
    "LG2LG3", "LB1LB2", "LB3", "RFC", "GFC", "BFC", "OFX", "OFY", "H",
    "DQA", "DQB", "ZSF3", "ZSF4", "FLAG"
  };


static void
print_inst_op (
               const PSX_Inst   *inst,
               const PSX_OpType  op,
               const uint32_t    addr
               )
{

  uint32_t aux;
  const char *name;
  
  
  putchar ( ' ' );
  switch ( op )
    {
    case PSX_RD: printf ( "%s", get_inst_reg_name ( inst->extra.rd ) ); break;
    case PSX_RS: printf ( "%s", get_inst_reg_name ( inst->extra.rs ) ); break;
    case PSX_RT: printf ( "%s", get_inst_reg_name ( inst->extra.rt ) ); break;
    case PSX_ADDR:
    case PSX_IMMEDIATE: printf ( "$%08X", inst->extra.imm  ); break;
    case PSX_OFFSET:
      aux= addr + inst->extra.off + 4;
      printf ( "%d [%08X]", inst->extra.off, aux );
      break;
    case PSX_OFFSET_BASE:
      name= get_inst_reg_name ( inst->extra.rs );
      if ( inst->extra.off >= 10 )
        printf ( "%d(%s) [$%X(%s)]",
        	 inst->extra.off, name,
        	 inst->extra.off, name );
      else printf ( "%d(%s)", inst->extra.off, name );
      break;
    case PSX_SA: printf ( "%d", inst->extra.sa ); break;
    case PSX_COP2_SF: printf ( "sf=%d", inst->extra.cop2_sf ); break;
    case PSX_COP2_MX_V_CV:
      printf ( "mx=%d, v=%d, cv=%d", inst->extra.cop2_mx,
               inst->extra.cop2_v, inst->extra.cop2_cv );
      break;
    case PSX_COP2_LM: printf ( "lm=%d", inst->extra.cop2_lm_is_0 ); break;
    case PSX_COP0_REG:
      printf ( "cop0.%s", get_inst_cop0_reg_name ( inst->extra.rd ) );
      break;
    case PSX_COP2_REG:
      printf ( "cop2.%s", GTE_REGS[inst->extra.rd] );
      break;
    case PSX_COP2_REG_CTRL:
      printf ( "cop2.%s", GTE_REGS[inst->extra.rd+32] );
      break;
    default:
      printf ( "???" );
    }
  
} // end print_inst_op


static void
dbg_cpu_inst (
              const PSX_Inst *inst,
              const uint32_t  addr,
              void           *udata
              )
{

  SHOW_PC_CC;
  printf ( "[CPU] %08X   %08x   ", addr, inst->word );
  if ( inst->word == 0 ) { printf ( "NOP\n" ); return; }
  printf ( "%s", get_inst_mnemonic ( inst->name ) );
  if ( inst->op1 != PSX_NONE ) print_inst_op ( inst, inst->op1, addr );
  if ( inst->op2 != PSX_NONE )
    {
      putchar ( ',' );
      print_inst_op ( inst, inst->op2, addr );
    }
  if ( inst->op3 != PSX_NONE )
    {
      putchar ( ',' );
      print_inst_op ( inst, inst->op3, addr );
    }
  putchar ( '\n' );
  
} // end dbg_cpu_inst


static const char *
gpu_mnemonic2str (
        	  const PSX_GPUMnemonic name
        	  )
{
  switch ( name )
    {
    case PSX_GP0_POL3: return "(GP0) Draw Polygon 3";
    case PSX_GP0_POL4: return "(GP0) Draw Polygon 4";
    case PSX_GP0_LINE: return "(GP0) Draw Line";
    case PSX_GP0_POLYLINE: return "(GP0) Draw Polyline";
    case PSX_GP0_POLYLINE_CONT: return "(GP0)  ... PolyLine next point";
    case PSX_GP0_RECT: return "(GP0) Draw Rectangle";
    case PSX_GP0_SET_DRAW_MODE: return "(GP0) Set Draw Mode";
    case PSX_GP0_SET_TEXT_WIN: return "(GP0) Set Texture Window";
    case PSX_GP0_SET_TOP_LEFT: return "(GP0) Set Drawing Area Top-Left";
    case PSX_GP0_SET_BOTTOM_RIGHT: return "(GP0) Set Drawing Bottom-Right";
    case PSX_GP0_SET_OFFSET: return "(GP0) Set Drawing Offset";
    case PSX_GP0_SET_MASK_BIT: return "(GP0) Mask Bit Setting";
    case PSX_GP0_CLEAR_CACHE: return "(GP0) Clear Cache";
    case PSX_GP0_FILL: return "(GP0) Fill Rectangle";
    case PSX_GP0_COPY_VRAM2VRAM: return "(GP0) Copy Rectangle (VRAM to VRAM)";
    case PSX_GP0_COPY_CPU2VRAM: return "(GP0) Copy Rectangle (CPU to VRAM)";
    case PSX_GP0_COPY_VRAM2CPU: return "(GP0) Copy Rectangle (VRAM to CPU)";
    case PSX_GP0_IRQ1: return "(GP0) Interrupt Request (IRQ1)";
    case PSX_GP0_NOP: return "(GP0) NOP";
    case PSX_GP0_UNK: return "(GP0) Unknown";
    case PSX_GP1_RESET: return "(GP1) Reset GPU";
    case PSX_GP1_RESET_BUFFER: return "(GP1) Reset Command Buffer";
    case PSX_GP1_ACK: return "(GP1) Acknowledge GPU Interrupt (IRQ1)";
    case PSX_GP1_ENABLE: return "(GP1) Display Enable";
    case PSX_GP1_DATA_REQUEST: return "(GP1) DMA Direction / Data Request";
    case PSX_GP1_START_DISP: return "(GP1) Start of Display Area (VRAM)";
    case PSX_GP1_HOR_DISP_RANGE:
      return "(GP1) Horizontal Display Range (Screen)";
    case PSX_GP1_VER_DISP_RANGE: return "(GP1) Vertical Display Range (Screen)";
    case PSX_GP1_SET_DISP_MODE: return "(GP1) Display Mode";
    case PSX_GP1_TEXT_DISABLE: return "(GP1) Texture Disable";
    case PSX_GP1_GET_INFO: return "(GP1) Get GPU Info";
    case PSX_GP1_OLD_TEXT_DISABLE: return "(GP1) Ancient Texture Disable";
    default:
    case PSX_GP1_UNK: return "(GP1) Unknown";
    }
  
} // end gpu_mnemonic2str


static void
dbg_gpu_print_transparency (
        		    const PSX_GPUCmd *cmd
        		    )
{

  if ( !(cmd->ops&PSX_GP_TRANSPARENCY) ) return;
  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    SemiTransparent" );
  
} // end dbg_gpu_print_transparency


static void
dbg_gpu_print_color (
        	     const PSX_GPUCmd *cmd
        	     )
{

  int r,g,b;

  
  if ( !(cmd->ops&PSX_GP_COLOR) ) return;
  r= cmd->word&0xff;
  g= (cmd->word>>8)&0xff;
  b= (cmd->word>>16)&0xff;
  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    color: RGB(%d,%d,%d)", r, g, b );
  
} // dbg_gpu_print_color


static void
dbg_gpu_print_wh (
        	  const PSX_GPUCmd *cmd
        	  )
{

  if ( cmd->width == -1 ) return;
  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    width: %d  height: %d", cmd->width, cmd->height );
  
} // end dbg_gpu_print_wh


static void
dbg_gpu_print_vertex (
        	      const PSX_GPUCmd *cmd,
        	      const int         v
        	      )
{

  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    v[%d] => x: %d  y: %d", v, cmd->v[v].x, cmd->v[v].y );
  if ( cmd->ops&(PSX_GP_TEXT_BLEND|PSX_GP_RAW_TEXT) )
    printf ( "  u: %d  v: %d", cmd->v[v].u, cmd->v[v].v );
  if ( cmd->ops&PSX_GP_V_COLOR )
    printf ( "  RGB(%d,%d,%d)", cmd->v[v].r, cmd->v[v].g, cmd->v[v].b );
  
} // end dbg_gpu_print_vertex


static void
dbg_gpu_print_texture (
        	       const PSX_GPUCmd *cmd
        	       )
{

  const char *mode;

  
  if ( !(cmd->ops&(PSX_GP_TEXT_BLEND|PSX_GP_RAW_TEXT)) ) return;
  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    Texture => CLUT.x: %d  CLUT.y: %d  Page.x: %d  Page.y: %d",
           cmd->texclut_x, cmd->texclut_y, cmd->texpage_x, cmd->texpage_y );
  if ( cmd->name == PSX_GP0_POL3 || cmd->name == PSX_GP0_POL4 )
    {
      fputc ( '\n', stdout );
      switch ( cmd->tex_pol_mode )
        {
        case 0: mode= "4bit"; break;
        case 1: mode= "8bit"; break;
        case 2: mode= "15bit"; break;
        default:
        case 3: mode= "¿¿??"; break;
        }
      SHOW_PC_CC;
      printf("[GPU]               Texture page colors: %s", mode );
      if ( cmd->ops&PSX_GP_TRANSPARENCY )
        {
          switch ( cmd->tex_pol_transparency )
            {
            case 0: mode= "B/2+F/2"; break;
            case 1: mode= "B+F"; break;
            case 2: mode= "B-F"; break;
            default:
            case 3: mode= "B+F/4"; break;
            }
          printf ( "  Semi Transparency Mode: %s", mode );
        }
    }
  
} // end dbg_gpu_print_texture


static void
dbg_gpu_print_data_request (
        		    const PSX_GPUCmd *cmd
        		    )
{

  const char *val;


  switch ( cmd->word&0x3 )
    {
    case 0: val= "Off"; break;
    case 1: val= "FIFO"; break;
    case 2: val= "CPUtoGP0"; break;
    case 3: val= "GPUREADtoCPU"; break;
    }
  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    Mode: %s", val );
  
} // end dbg_gpu_print_data_request


static void
dbg_gpu_print_set_xy_corner (
        		     const PSX_GPUCmd *cmd
        		     )
{

  int x,y;


  x= cmd->word&0x3FF;
  y= (cmd->word>>10)&0x3FF;
  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    x: %d  y: %d", x, y );
  
} // end dbg_gpu_print_set_xy_corner


static void
dbg_gpu_print_set_offset (
        		  const PSX_GPUCmd *cmd
        		  )
{

  int x,y;

  
  x= cmd->word&0x7FF;
  if ( x >= 0x400 ) x-= 0x800;
  y= (cmd->word>>11)&0x7FF;
  if ( y >= 0x400 ) y-= 0x800;
  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    x: %d  y: %d", x, y );
  
} // end dbg_gpu_print_set_offset


static void
dbg_gpu_print_set_text_win (
        		    const PSX_GPUCmd *cmd
        		    )
{

  uint8_t maskx,masky,offx,offy;


  maskx= (uint8_t) (cmd->word&0x1F);
  masky= (uint8_t) ((cmd->word>>5)&0x1F);
  offx= (uint8_t) ((cmd->word>>10)&0x1F);
  offy= (uint8_t) ((cmd->word>>15)&0x1F);
  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    Mask X: %02X  Mask Y: %02X  Offset X:"
           " %02X  Offset Y: %02X",
           maskx, masky, offx, offy );
  
} // end dbg_gpu_print_set_text_win


static void
dbg_gpu_print_set_mask_bit (
        		    const PSX_GPUCmd *cmd
        		    )
{

  const char *val1;
  const char *val2;


  val1= (cmd->word&0x1) ? "ForceBit15" : "TextureBit15";
  val2= (cmd->word&0x2) ? "Draw if Bit15==0" : "Draw Always";
  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    Mode: %s, %s", val1, val2 );
  
} // end dbg_gpu_print_set_mask_bit


static void
dbg_gpu_print_set_draw_mode (
        		     const PSX_GPUCmd *cmd
        		     )
{

  int x,y;
  const char *mode;
  

  // Texture pages base
  x= (cmd->word&0xF)*64;
  y= ((cmd->word>>4)&0x1)*256;
  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    Texture page X Base: %d  Texture page Y Base: %d", x, y );

  // Semi transparency mode
  switch ( (cmd->word>>5)&0x3 )
    {
    case 0: mode= "B/2+F/2"; break;
    case 1: mode= "B+F"; break;
    case 2: mode= "B-F"; break;
    case 3: mode= "B+F/4"; break;
    }
  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    Semi Transparency Mode: %s", mode );

  // Texture page colors
  switch ( (cmd->word>>7)&0x3 )
    {
    case 0: mode= "4bit"; break;
    case 1: mode= "8bit"; break;
    case 2: mode= "15bit"; break;
    case 3: mode= "¿¿??"; break;
    }
  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    Texture page colors: %s", mode );

  // Dither
  mode= ((cmd->word>>9)&0x1) ? "Enabled" : "Disabled";
  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    Dither 24bit to 15bit: %s", mode );
  
  // Drawing to display area.
  mode= ((cmd->word>>10)&0x1) ? "Allowed" : "Prohibited";
  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    Drawing to display area: %s", mode );
  
  // Texture Disable
  mode= ((cmd->word>>11)&0x1) ? "Disable if GP1(09)" : "Normal";
  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    Texture Disable: %s", mode );

  // Textured Rectangle X-FLIP
  mode= ((cmd->word>>12)&0x1) ? "Enabled" : "Disabled";
  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    Textured Rectangle X-Flip: %s", mode );
  
  // Textured Rectangle Y-FLIP
  mode= ((cmd->word>>13)&0x1) ? "Enabled" : "Disabled";
  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    Textured Rectangle Y-Flip: %s", mode );
  
} // end dbg_gpu_print_set_draw_mode


static void
dbg_gpu_print_set_disp_mode (
        		     const PSX_GPUCmd *cmd
        		     )
{

  int hor,ver;
  bool interlace;
  const char *mode;
  
  
  // Resolution
  switch ( cmd->word&0x3 )
    {
    case 0: hor= 256; break;
    case 1: hor= 320; break;
    case 2: hor= 512; break;
    case 3: hor= 640; break;
    }
  interlace= ((cmd->word>>5)&0x1)==1;
  ver= (cmd->word>>2)&0x1;
  ver= (ver && interlace) ? 480 : 240;
  if ( (cmd->word>>6)&0x1 ) hor= 368;
  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    Resolution: %dx%d (Interlace= %s)", hor, ver,
           interlace ? "On" : "Off" );
  
  // Video mode
  mode= ((cmd->word>>3)&0x1) ? "PAL/50Hz" : "NTSC/60Hz";
  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    Video Mode: %s", mode );
  
  // Color depth
  mode= ((cmd->word>>4)&0x1) ? "24bit" : "15bit";
  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    Display Area Color Depth: %s", mode );
  
  // Reverseflag
  mode= ((cmd->word>>7)&0x1) ? "Distorted" : "Normal";
  fputc ( '\n', stdout );
  SHOW_PC_CC;
  printf ( "[GPU]    Reverseflag: %s", mode );
  
} // end dbg_gpu_print_set_disp_mode


static void
dbg_gpu_cmd_trace (
        	   const PSX_GPUCmd *cmd,
        	   void             *udata
        	   )
{

  int n;


  SHOW_PC_CC;
  printf ( "[GPU] %08x  %s", cmd->word, gpu_mnemonic2str ( cmd->name ) );
  dbg_gpu_print_transparency ( cmd );
  dbg_gpu_print_color ( cmd );
  dbg_gpu_print_wh ( cmd );
  for ( n= 0; n < cmd->Nv; ++n )
    dbg_gpu_print_vertex ( cmd, n );
  dbg_gpu_print_texture ( cmd );
  switch ( cmd->name )
    {
    case PSX_GP1_DATA_REQUEST: dbg_gpu_print_data_request ( cmd ); break;
    case PSX_GP0_SET_TOP_LEFT:
    case PSX_GP0_SET_BOTTOM_RIGHT: dbg_gpu_print_set_xy_corner ( cmd ); break;
    case PSX_GP0_SET_OFFSET: dbg_gpu_print_set_offset ( cmd ); break;
    case PSX_GP0_SET_TEXT_WIN: dbg_gpu_print_set_text_win ( cmd ); break;
    case PSX_GP0_SET_MASK_BIT: dbg_gpu_print_set_mask_bit ( cmd ); break;
    case PSX_GP0_SET_DRAW_MODE: dbg_gpu_print_set_draw_mode ( cmd ); break;
    case PSX_GP1_SET_DISP_MODE: dbg_gpu_print_set_disp_mode ( cmd ); break;
    default: break;
    }
  fputc ( '\n', stdout );
  
} // end dbg_gpu_cmd_trace


static void
dbg_cd_cmd_trace (
        	  const PSX_CDCmd *cmd,
        	  void            *udata
        	  )
{

  const char *name;
  int n;
  

  SHOW_PC_CC;
  switch ( cmd->name )
    {
    case PSX_CD_SYNC: name= "Sync"; break;
    case PSX_CD_SET_MODE: name= "Setmode"; break;
    case PSX_CD_INIT: name= "Init"; break;
    case PSX_CD_RESET: name= "Reset"; break;
    case PSX_CD_MOTOR_ON: name= "MotorOn"; break;
    case PSX_CD_STOP: name= "Stop"; break;
    case PSX_CD_PAUSE: name= "Pause"; break;
    case PSX_CD_SETLOC: name= "Setloc"; break;
    case PSX_CD_SEEKL: name= "SeekL"; break;
    case PSX_CD_SEEKP: name= "SeekP"; break;
    case PSX_CD_SET_SESSION: name= "SetSession"; break;
    case PSX_CD_READN: name= "ReadN"; break;
    case PSX_CD_READS: name= "ReadS"; break;
    case PSX_CD_READ_TOC: name= "ReadTOC"; break;
    case PSX_CD_GET_STAT: name= "Getstat"; break;
    case PSX_CD_GET_PARAM: name= "Getparam"; break;
    case PSX_CD_GET_LOC_L: name= "GetlocL"; break;
    case PSX_CD_GET_LOC_P: name= "GetlocP"; break;
    case PSX_CD_GET_TN: name= "GetTN"; break;
    case PSX_CD_GET_TD: name= "GetTD"; break;
    case PSX_CD_GET_Q: name= "GetQ"; break;
    case PSX_CD_GET_ID: name= "GetID"; break;
    case PSX_CD_TEST: name= "Test"; break;
    case PSX_CD_MUTE: name= "Mute"; break;
    case PSX_CD_DEMUTE: name= "Demute"; break;
    case PSX_CD_PLAY: name= "Play"; break;
    case PSX_CD_FORWARD: name= "Forward"; break;
    case PSX_CD_BACKWARD: name= "Backward"; break;
    case PSX_CD_SET_FILTER: name= "Setfilter"; break;
    default:
    case PSX_CD_UNK: name= "Unknown"; break;
    }
  printf ( "[CD] %02x  %s", cmd->cmd, name );
  if ( cmd->args.N )
    {
      putchar ( '(' );
      printf ( "%02xh", cmd->args.v[0] );
      for ( n= 1; n < cmd->args.N; ++n )
        printf ( ",%02xh", cmd->args.v[n] );
      putchar ( ')' );
    }
  putchar ( '\n' );
  
} // end dbg_cd_cmd_trace


static void
dbg_mem_changed (
        	 void *udata
        	 )
{
} // end dbg_mem_changed


static void
dbg_mem_access (
        	const PSX_MemAccessType  type,
        	const uint32_t           addr,
        	const uint32_t           data,
        	const bool               error,
        	void                    *udata
        	)
{

  SHOW_PC_CC;
  if ( type == PSX_READ )
    printf ( "[MEM] %08X --> %08X\n", addr, data );
  else
    printf ( "[MEM] %08X <-- %08X\n", addr, data );
  
} // end dbg_mem_access


static void
dbg_mem_access16 (
        	  const PSX_MemAccessType  type,
        	  const uint32_t           addr,
        	  const uint16_t           data,
        	  const bool               error,
        	  void                    *udata
        	  )
{

  SHOW_PC_CC;
  if ( type == PSX_READ )
    printf ( "[MEM] %08X --> %04X\n", addr, data );
  else
    printf ( "[MEM] %08X <-- %04X\n", addr, data );
  
} // end dbg_mem_access16


static void
dbg_mem_access8 (
        	 const PSX_MemAccessType  type,
        	 const uint32_t           addr,
        	 const uint8_t            data,
        	 const bool               error,
        	 void                    *udata
        	 )
{

  SHOW_PC_CC;
  if ( type == PSX_READ )
    printf ( "[MEM] %08X --> %02X\n", addr, data );
  else
    printf ( "[MEM] %08X <-- %02X\n", addr, data );
  
} // end dbg_mem_access8


static void
dbg_int_trace (
               const bool      is_ack,
               const uint32_t  old_i_stat,
               const uint32_t  new_i_stat,
               const uint32_t  i_mask,
               void           *udata
               )
{

  SHOW_PC_CC;
  printf ( "[INT] (%s) I_STAT: %04X -> %04X (%04X)  I_MASK: %04X\n",
           is_ack ? "ACK" : "IRQ",
           old_i_stat, new_i_stat, old_i_stat^new_i_stat,
           i_mask );
  
} // end dbg_int_trace


static void
dbg_dma_transfer (
        	  const int       channel,
        	  const bool      to_ram,
        	  const uint32_t  addr,
        	  void           *udata
        	  )
{

  SHOW_PC_CC;
  printf ( "[DMA] chn: %d  dir: %s  addr: %08X\n",
           channel,
           to_ram ? "-> RAM" : "RAM ->",
           addr );
  
} // end dbg_dma_transfer


static void
dbg_gte_get_reg_names (
                       const int    reg,
                       const char **v1,
                       const char **v2
                       )
{

  *v1= *v2= NULL;
  switch ( reg )
    {
    case 0: *v1= "VX0"; *v2= "VY0"; break;
    case 1: *v1= "VZ0"; break;
    case 2: *v1= "VX1"; *v2= "VY1"; break;
    case 3: *v1= "VZ1"; break;
    case 4: *v1= "VX2"; *v2= "VY2"; break;
    case 5: *v1= "VZ2"; break;
    case 6: *v1= "RGBC"; break;
    case 7: *v1= "OTZ"; break;
    case 8: *v1= "IR0"; break;
    case 9: *v1= "IR1"; break;
    case 10: *v1= "IR2"; break;
    case 11: *v1= "IR3"; break;
    case 12: *v1= "SX0"; *v2= "SY0"; break;
    case 13: *v1= "SX1"; *v2= "SY1"; break;
    case 14: *v1= "SX2"; *v2= "SY2"; break;
    case 15: *v1= "SXP"; *v2= "SYP"; break;
    case 16: *v1= "SZ0"; break;
    case 17: *v1= "SZ1"; break;
    case 18: *v1= "SZ2"; break;
    case 19: *v1= "SZ3"; break;
    case 20: *v1= "RGB0"; break;
    case 21: *v1= "RGB1"; break;
    case 22: *v1= "RGB2"; break;
    case 23: *v1= "RGB3"; break;
    case 24: *v1= "MAC0"; break;
    case 25: *v1= "MAC1"; break;
    case 26: *v1= "MAC2"; break;
    case 27: *v1= "MAC3"; break;
    case 28:
    case 29: *v1= "IRGB/ORGB"; break;
    case 30: *v1= "LZCS"; break;
    case 31: *v1= "LZCR"; break;
    case 32: *v1= "RT11"; *v2= "RT12"; break;
    case 33: *v1= "RT13"; *v2= "RT21"; break;
    case 34: *v1= "RT22"; *v2= "RT23"; break;
    case 35: *v1= "RT31"; *v2= "RT32"; break;
    case 36: *v1= "RT33"; break;
    case 37: *v1= "TRX"; break;
    case 38: *v1= "TRY"; break;
    case 39: *v1= "TRZ"; break;
    case 40: *v1= "L11"; *v2= "L12"; break;
    case 41: *v1= "L13"; *v2= "L21"; break;
    case 42: *v1= "L22"; *v2= "L23"; break;
    case 43: *v1= "L31"; *v2= "L32"; break;
    case 44: *v1= "L33"; break;
    case 45: *v1= "RBK"; break;
    case 46: *v1= "GBK"; break;
    case 47: *v1= "BBK"; break;
    case 48: *v1= "LR1"; *v2= "LR2"; break;
    case 49: *v1= "LR3"; *v2= "LG1"; break;
    case 50: *v1= "LG2"; *v2= "LG3"; break;
    case 51: *v1= "LB1"; *v2= "LB2"; break;
    case 52: *v1= "LB3"; break;
    case 53: *v1= "RFC"; break;
    case 54: *v1= "GFC"; break;
    case 55: *v1= "BFC"; break;
    case 56: *v1= "OFX"; break;
    case 57: *v1= "OFY"; break;
    case 58: *v1= "H"; break;
    case 59: *v1= "DQA"; break;
    case 60: *v1= "DQB"; break;
    case 61: *v1= "ZSF3"; break;
    case 62: *v1= "ZSF4"; break;
    case 63: *v1= "FLAG"; break;
    default: *v1= "UNK";
    }
  
} // dbg_gte_get_reg_names


static void
dbg_gte_cmd_trace (
                   uint32_t  regs_prev[64],
                   uint32_t  regs_after[64],
                   void     *udata
                   )
{

  int i;
  const char *v1,*v2;
  
  
  SHOW_PC_CC;
  printf ( "[GTE] RUN OP\n" );
  for ( i= 0; i < 64; ++i )
    {
      SHOW_PC_CC;
      dbg_gte_get_reg_names ( i, &v1, &v2 );
      printf ( "[GTE]    REG%02d ", i );
      if ( v2 == NULL )
        printf ( "%s: %08X ==> %08X", v1, regs_prev[i], regs_after[i] );
      else
        printf ( "%s: %04X ==> %04X   %s: %04X ==> %04X",
                 v1, regs_prev[i]&0xFFFF, regs_after[i]&0xFFFF,
                 v2, regs_prev[i]>>16, regs_after[i]>>16 );
      printf ( "\n" );
    }
  
} // end gte_cmd_trace


static void
dbg_gte_mem_access (
        	    const bool      read,
        	    const int       reg,
        	    const uint32_t  val,
        	    const bool      ok,
        	    void           *udata
        	    )
{

  uint32_t tmp;
  const char *sym;
  const char *v1;
  const char *v2;
  
  
  SHOW_PC_CC;
  tmp= ok ? val : 0xFFFFFFFF;
  sym= read ? "-->" : ":=";
  dbg_gte_get_reg_names ( reg, &v1, &v2 );
  printf ( "[GTE] %s (%s):  ", read ? "Read" : "Write", ok ? "OK" : "Failed" );
  if ( v2 == NULL )
    printf ( "%s %s %08X", v1, sym, tmp );
  else
    printf ( "%s %s %04X  %s %s %04X",
             v1, sym, tmp&0xFFFF,
             v2, sym, tmp>>16 );
  printf ( "\n" );
  
} // end dbg_gte_mem_access


static void
dbg_bios_func_trace_00A0 (void)
{
  
  // Basat en la documentació de NOCASH
  bool show_args;


  show_args= true;
  SHOW_PC_CC;
  printf ( "[BIOS] " );
  switch ( PSX_cpu_regs.gpr[9].v )
    {
    case 0x00: printf ( "FileOpen(filename,accessmode)" ); break;
    case 0x01: printf ( "FileSeek(fd,offset,seektype)" ); break;
    case 0x02: printf ( "FileRead(fd,dst,length)" ); break;
    case 0x03: printf ( "FileWrite(fd,src,length)" ); break;
    case 0x04: printf ( "FileClose(fd)" ); break;
    case 0x05: printf ( "FileIoctl(fd,cmd,arg)" ); break;
    case 0x06: printf ( "exit(exitcode)" ); break;
    case 0x07: printf ( "FileGetDeviceFlag(fd)" ); break;
    case 0x08: printf ( "FileGetc(fd)" ); break;
    case 0x09: printf ( "FilePutc(char,fd)" ); break;
    case 0x0A: printf ( "todigit(char)" ); break;
    case 0x0B:
      printf ( "atof(src)     ;Does NOT work - uses (ABSENT) cop1 !!!" );
      break;
    case 0x0C: printf ( "strtoul(src,src_end,base)" ); break;
    case 0x0D: printf ( "strtol(src,src_end,base)" ); break;
    case 0x0E: printf ( "abs(val)" ); break;
    case 0x0F: printf ( "labs(val)" ); break;
    case 0x10: printf ( "atoi(src)" ); break;
    case 0x11: printf ( "atol(src)" ); break;
    case 0x12: printf ( "atob(src,num_dst)" ); break;
    case 0x13: printf ( "SaveState(buf)" ); break;
    case 0x14: printf ( "RestoreState(buf,param)" ); break;
    case 0x15: printf ( "strcat(dst,src)" ); break;
    case 0x16: printf ( "strncat(dst,src,maxlen)" ); break;
    case 0x17: printf ( "strcmp(str1,str2)" ); break;
    case 0x18: printf ( "strncmp(str1,str2,maxlen)" ); break;
    case 0x19: printf ( "strcpy(dst,src)" ); break;
    case 0x1A: printf ( "strncpy(dst,src,maxlen)" ); break;
    case 0x1B: printf ( "strlen(src)" ); break;
    case 0x1C: printf ( "index(src,char)" ); break;
    case 0x1D: printf ( "rindex(src,char)" ); break;
    case 0x1E:
      printf ( "strchr(src,char)  ;exactly the same as 'index'" );
      break;
    case 0x1F:
      printf ( "strrchr(src,char) ;exactly the same as 'rindex'" );
      break;
    case 0x20: printf ( "strpbrk(src,list)" ); break;
    case 0x21: printf ( "strspn(src,list)" ); break;
    case 0x22: printf ( "strcspn(src,list)" ); break;
    case 0x23:
      printf ( "strtok(src,list)  ;use strtok(0,list) in further calls" );
      break;
    case 0x24: printf ( "strstr(str,substr) - buggy" ); break;
    case 0x25: printf ( "toupper(char)" ); break;
    case 0x26: printf ( "tolower(char)" ); break;
    case 0x27: printf ( "bcopy(src,dst,len)" ); break;
    case 0x28: printf ( "bzero(dst,len)" ); break;
    case 0x29: printf ( "bcmp(ptr1,ptr2,len)      ;Bugged" ); break;
    case 0x2A: printf ( "memcpy(dst,src,len)" ); break;
    case 0x2B: printf ( "memset(dst,fillbyte,len)" ); break;
    case 0x2C: printf ( "memmove(dst,src,len)     ;Bugged" ); break;
    case 0x2D: printf ( "memcmp(src1,src2,len)    ;Bugged" ); break;
    case 0x2E: printf ( "memchr(src,scanbyte,len)" ); break;
    case 0x2F: printf ( "rand()" ); break;
    case 0x30: printf ( "srand(seed)" ); break;
    case 0x31: printf ( "qsort(base,nel,width,callback)" ); break;
    case 0x32:
      printf ( "strtod(src,src_end) ;Does NOT work - uses (ABSENT) cop1 !!!" );
      break;
    case 0x33: printf ( "malloc(size)" ); break;
    case 0x34: printf ( "free(buf)" ); break;
    case 0x35: printf ( "lsearch(key,base,nel,width,callback)" ); break;
    case 0x36: printf ( "bsearch(key,base,nel,width,callback)" ); break;
    case 0x37: printf ( "calloc(sizx,sizy)            ;SLOW!" ); break;
    case 0x38: printf ( "realloc(old_buf,new_siz)     ;SLOW!" ); break;
    case 0x39: printf ( "InitHeap(addr,size)" ); break;
    case 0x3A: printf ( "SystemErrorExit(exitcode)" ); break;
    case 0x3B: printf ( "or B(3Ch) std_in_getchar()" ); break;
    case 0x3C: printf ( "or B(3Dh) std_out_putchar(char)" ); break;
    case 0x3D: printf ( "or B(3Eh) std_in_gets(dst)" ); break;
    case 0x3E: printf ( "or B(3Fh) std_out_puts(src)" ); break;
    case 0x3F: printf ( "printf(txt,param1,param2,etc.)" ); break;
    case 0x40: printf ( "SystemErrorUnresolvedException()" ); break;
    case 0x41: printf ( "LoadExeHeader(filename,headerbuf)" ); break;
    case 0x42: printf ( "LoadExeFile(filename,headerbuf)" ); break;
    case 0x43: printf ( "DoExecute(headerbuf,param1,param2)" ); break;
    case 0x44: printf ( "FlushCache()" ); break;
    case 0x45: printf ( "init_a0_b0_c0_vectors" ); break;
    case 0x46: printf ( "GPU_dw(Xdst,Ydst,Xsiz,Ysiz,src)" ); break;
    case 0x47: printf ( "gpu_send_dma(Xdst,Ydst,Xsiz,Ysiz,src)" ); break;
    case 0x48: printf ( "SendGP1Command(gp1cmd)" ); break;
    case 0x49:
      printf ( "GPU_cw(gp0cmd=%Xh)   ;send GP0 command word",
               PSX_cpu_regs.gpr[4].v );
      show_args= false;
      break;
    case 0x4A:
      printf ( "GPU_cwp(src,num) ;send GP0 command word and parameter words" );
      break;
    case 0x4B: printf ( "send_gpu_linked_list(src)" ); break;
    case 0x4C: printf ( "gpu_abort_dma()" ); break;
    case 0x4D: printf ( "GetGPUStatus()" ); break;
    case 0x4E: printf ( "gpu_sync()" ); break;
    case 0x4F: printf ( "SystemError" ); break;
    case 0x50: printf ( "SystemError" ); break;
    case 0x51:
      printf ( "LoadAndExecute(filename,stackbase,stackoffset)" );
      break;
    case 0x52: printf ( "SystemError ----OR---- 'GetSysSp()' ?" ); break;
    case 0x53:
      printf ( "SystemError            ;PS2: set_ioabort_handler(src)" );
      break;
    case 0x54: printf ( "CdInit()" ); break;
    case 0x55:
      printf ( "_bu_init()" );
      break;
    case 0x56: printf ( "CdRemove()" ); break;
    case 0x57: printf ( "return 0" ); break;
    case 0x58: printf ( "return 0" ); break;
    case 0x59: printf ( "return 0" ); break;
    case 0x5A: printf ( "return 0" ); break;
    case 0x5B:
      printf ( "dev_tty_init()" );
      break;
    case 0x5C:
      printf ( "dev_tty_open(fcb,and unused:'path\\name',accessmode)" );
      break;
    case 0x5D: printf ( "dev_tty_in_out(fcb,cmd); PS2: SystemError" ); break;
    case 0x5E: printf ( "dev_tty_ioctl(fcb,cmd,arg); PS2: SystemError" ); break;
    case 0x5F: printf ( "dev_cd_open(fcb,'path\\name',accessmode)" ); break;
    case 0x60: printf ( "dev_cd_read(fcb,dst,len)" ); break;
    case 0x61: printf ( "dev_cd_close(fcb)" ); break;
    case 0x62: printf ( "dev_cd_firstfile(fcb,'path\\name',direntry)" ); break;
    case 0x63: printf ( "dev_cd_nextfile(fcb,direntry)" ); break;
    case 0x64: printf ( "dev_cd_chdir(fcb,'path')" ); break;
    case 0x65: printf ( "dev_card_open(fcb,'path\name',accessmode)" ); break;
    case 0x66: printf ( "dev_card_read(fcb,dst,len)" ); break;
    case 0x67: printf ( "dev_card_write(fcb,src,len)" ); break;
    case 0x68: printf ( "dev_card_close(fcb)" ); break;
    case 0x69:
      printf ( "dev_card_firstfile(fcb,'path\\name',direntry)" );
      break;
    case 0x6A: printf ( "dev_card_nextfile(fcb,direntry)" ); break;
    case 0x6B: printf ( "dev_card_erase(fcb,'path\\name')" ); break;
    case 0x6C: printf ( "dev_card_undelete(fcb,'path\\name)'" ); break;
    case 0x6D: printf ( "dev_card_format(fcb)" ); break;
    case 0x6E:
      printf ( "dev_card_rename(fcb1,'path\\name1',fcb2,'path\\name2')" );
      break;
  case 0x6F:
    printf ( "dev_card_clear_error_or_so(fcb);[r4+18h]=00000000h" );
    break;
    case 0x70: printf ( "_bu_init()" ); break;
    case 0x71: printf ( "CdInit()" ); break;
    case 0x72: printf ( "CdRemove()" ); break;
    case 0x73: printf ( "return 0" ); break;
    case 0x74: printf ( "return 0" ); break;
    case 0x75: printf ( "return 0" ); break;
    case 0x76: printf ( "return 0" ); break;
    case 0x77: printf ( "return 0" ); break;
    case 0x78: printf ( "CdAsyncSeekL(src)" ); break;
    case 0x79:
      printf ( "return 0 ;DTL-H2000: CdAsyncSeekP(src)" );
      break;
    case 0x7A:
      printf ( "return 0 ;DTL-H2000: CdAsyncGetlocL(dst?)" );
      break;
    case 0x7B:
      printf ( "return 0 ;DTL-H2000: CdAsyncGetlocP(dst?)" );
      break;
    case 0x7C: printf ( "CdAsyncGetStatus(dst)" ); break;
    case 0x7D:
      printf ( "return 0 ;DTL-H2000: CdAsyncGetParam(dst?)" );
      break;
    case 0x7E: printf ( "CdAsyncReadSector(count,dst,mode)" ); break;
    case 0x7F:
      printf ( "return 0 ;DTL-H2000: CdAsyncReadWithNewMode(mode)" );
      break;
    case 0x80:
      printf ( "return 0 ;DTL-H2000: CdAsyncReadFinalCount1(r4)" );
      break;
    case 0x81: printf ( "CdAsyncSetMode(mode)" ); break;
    case 0x82:
      printf ( "return 0              ;DTL-H2000: CdAsyncMotorOn()" );
      break;
    case 0x83:
      printf ( "return 0              ;DTL-H2000: CdAsyncPause()" );
      break;
    case 0x84: printf ( "return 0 ;DTL-H2000: CdAsyncPlayOrReadS()" ); break;
    case 0x85: printf ( "return 0 ;DTL-H2000: CdAsyncStop()" ); break;
    case 0x86: printf ( "return 0 ;DTL-H2000: CdAsyncMute()" ); break;
    case 0x87: printf ( "return 0 ;DTL-H2000: CdAsyncDemute()" ); break;
    case 0x88:
      printf ( "return 0 ;DTL-H2000: CdSetAudioVolume(src)  ;4-byte src" );
      break;
    case 0x89: printf ( "return 0 ;DTL-H2000: CdAsyncSetSession1(dst)" ); break;
    case 0x8A:
      printf ( "return 0 ;DTL-H2000: CdAsyncSetSession(session,dst)" );
      break;
    case 0x8B: printf ( "return 0 ;DTL-H2000: CdAsyncForward()" ); break;
    case 0x8C: printf ( "return 0 ;DTL-H2000: CdAsyncBackward()" ); break;
    case 0x8D: printf ( "return 0 ;DTL-H2000: CdAsyncPlay()" ); break;
    case 0x8E:
      printf ( "return 0 ;DTL-H2000: CdAsyncGetStatSpecial(r4,r5)" );
      break;
    case 0x8F: printf ( "return 0 ;DTL-H2000: CdAsyncGetID(r4,r5)" ); break;
    case 0x90: printf ( "CdromIoIrqFunc1()" ); break;
    case 0x91: printf ( "CdromDmaIrqFunc1()" ); break;
    case 0x92: printf ( "CdromIoIrqFunc2()" ); break;
    case 0x93: printf ( "CdromDmaIrqFunc2()" ); break;
    case 0x94: printf ( "CdromGetInt5errCode(dst1,dst2)" ); break;
    case 0x95: printf ( "CdInitSubFunc()" ); break;
    case 0x96: printf ( "AddCDROMDevice()" ); break;
    case 0x97: printf ( "AddMemCardDevice() ;DTL-H2000: SystemError" ); break;
    case 0x98:
      printf ( "AddDuartTtyDevice() ;DTL-H2000: "
               "AddAdconsTtyDevice ;PS2: SystemError" );
      break;
    case 0x99: printf ( "AddDummyTtyDevice()" ); break;
    case 0x9A: printf ( "SystemError ;DTL-H: AddMessageWindowDevice" ); break;
    case 0x9B: printf ( "SystemError ;DTL-H: AddCdromSimDevice" ); break;
    case 0x9C: printf ( "SetConf(num_EvCB,num_TCB,stacktop)" ); break;
    case 0x9D:
      printf ( "GetConf(num_EvCB_dst,num_TCB_dst,stacktop_dst)" );
      break;
    case 0x9E: printf ( "SetCdromIrqAutoAbort(type,flag)" ); break;
    case 0x9F: printf ( "SetMemSize(megabytes)" ); break;
    case 0xA0: printf ( "WarmBoot()" ); break;
    case 0xA1: printf ( "SystemErrorBootOrDiskFailure(type,errorcode)" ); break;
    case 0xA2: printf ( "EnqueueCdIntr()  ;with prio=0 (fixed)" ); break;
    case 0xA3: printf ( "DequeueCdIntr()" ); break;
    case 0xA4:
      printf ( "CdGetLbn(filename) ;"
               "get 1st sector number (or garbage when not found)" );
      break;
    case 0xA5: printf ( "CdReadSector(count,sector,buffer)" ); break;
    case 0xA6: printf ( "CdGetStatus()" ); break;
    case 0xA7: printf ( "bu_callback_okay()" ); break;
    case 0xA8: printf ( "bu_callback_err_write()" ); break;
    case 0xA9: printf ( "bu_callback_err_busy()" ); break;
    case 0xAA: printf ( "bu_callback_err_eject()" ); break;
    case 0xAB: printf ( "_card_info(port)" ); break;
    case 0xAC: printf ( "_card_async_load_directory(port)" ); break;
    case 0xAD: printf ( "set_card_auto_format(flag)" ); break;
    case 0xAE: printf ( "bu_callback_err_prev_write()" ); break;
    case 0xAF:
      printf ( "card_write_test(port)  ;CEX-1000: jump_to_00000000h" );
      break;
    case 0xB0: printf ( "return 0 ;CEX-1000: jump_to_00000000h" ); break;
    case 0xB1: printf ( "return 0 ;CEX-1000: jump_to_00000000h" ); break;
    case 0xB2:
      printf ( "ioabort_raw(param);CEX-1000: jump_to_00000000h" );
      break;
    case 0xB3: printf ( "return 0 ;CEX-1000: jump_to_00000000h" ); break;
    case 0xB4:
      printf ( "GetSystemInfo(index) ;CEX-1000: jump_to_00000000h" );
      break;
    case 0xB5 ... 0xBF: printf ( "N/A ;jump_to_00000000h" ); break;
    default: printf ( "Unknown_00A0(%Xh)", PSX_cpu_regs.gpr[9].v );
    }
  if ( show_args )
    printf ( "  ARGS: %X, %X, %X, %X, ...",
             PSX_cpu_regs.gpr[4].v,
             PSX_cpu_regs.gpr[5].v,
             PSX_cpu_regs.gpr[6].v,
             PSX_cpu_regs.gpr[7].v );
  printf ( "\n" );
  
} // end dbg_bios_func_trace_00A0


static void
dbg_bios_func_trace_00B0 (void)
{
  
  bool show_args;


  show_args= true;
  SHOW_PC_CC;
  printf ( "[BIOS] " );
  switch ( PSX_cpu_regs.gpr[9].v )
    {
    case 0x00: printf ( "alloc_kernel_memory(size)" ); break;
    case 0x01: printf ( "free_kernel_memory(buf)" ); break;
    case 0x02: printf ( "init_timer(t,reload,flags)" ); break;
    case 0x03: printf ( "get_timer(t)" ); break;
    case 0x04: printf ( "enable_timer_irq(t)" ); break;
    case 0x05: printf ( "disable_timer_irq(t)" ); break;
    case 0x06: printf ( "restart_timer(t)" ); break;
    case 0x07:
      printf ( "DeliverEvent(class=%Xh, spec=%Xh)",
               PSX_cpu_regs.gpr[4].v, PSX_cpu_regs.gpr[5].v );
      show_args= false;
      break;
    case 0x08:
      printf ( "OpenEvent(class=%Xh, spec=%Xh, mode=%Xh, func=%Xh)",
               PSX_cpu_regs.gpr[4].v, PSX_cpu_regs.gpr[5].v,
               PSX_cpu_regs.gpr[6].v, PSX_cpu_regs.gpr[7].v);
      show_args= false;
      break;
    case 0x09:
      printf ( "CloseEvent(event=%Xh)", PSX_cpu_regs.gpr[4].v );
      show_args= false;
      break;
    case 0x0A:
      printf ( "WaitEvent(event=%Xh)", PSX_cpu_regs.gpr[4].v );
      show_args= false;
      break;
    case 0x0B:
      printf ( "TestEvent(event=%Xh)", PSX_cpu_regs.gpr[4].v );
      show_args= false;
      break;
    case 0x0C:
      printf ( "EnableEvent(event=%Xh)", PSX_cpu_regs.gpr[4].v );
      show_args= false;
      break;
    case 0x0D:
      printf ( "DisableEvent(event=%Xh)", PSX_cpu_regs.gpr[4].v );
      show_args= false;
      break;
    case 0x0E: printf ( "OpenThread(reg_PC,reg_SP_FP,reg_GP)" ); break;
    case 0x0F: printf ( "CloseThread(handle)" ); break;
    case 0x10: printf ( "ChangeThread(handle)" ); break;
    case 0x11: printf ( "jump_to_00000000h" ); break;
    case 0x12: printf ( "InitPad(buf1,siz1,buf2,siz2)" ); break;
    case 0x13: printf ( "StartPad()" ); break;
    case 0x14: printf ( "StopPad()" ); break;
    case 0x15:
      printf ( "OutdatedPadInitAndStart(type,button_dest,unused,unused)" );
      break;
    case 0x16: printf ( "OutdatedPadGetButtons()" ); break;
    case 0x17: printf ( "ReturnFromException()" ); show_args= false; break;
    case 0x18: printf ( "SetDefaultExitFromException()" ); break;
    case 0x19: printf ( "SetCustomExitFromException(addr)" ); break;
    case 0x1A: printf ( "SystemError  ;PS2: return 0" ); break;
    case 0x1B: printf ( "SystemError  ;PS2: return 0" ); break;
    case 0x1C: printf ( "SystemError  ;PS2: return 0" ); break;
    case 0x1D: printf ( "SystemError  ;PS2: return 0" ); break;
    case 0x1E: printf ( "SystemError  ;PS2: return 0" ); break;
    case 0x1F: printf ( "SystemError  ;PS2: return 0" ); break;
    case 0x20:
      printf ( "UnDeliverEvent(class=%Xh, spec=%Xh)",
               PSX_cpu_regs.gpr[4].v, PSX_cpu_regs.gpr[5].v );
      show_args= false;
      break;
    case 0x21: printf ( "SystemError  ;PS2: return 0" ); break;
    case 0x22: printf ( "SystemError  ;PS2: return 0" ); break;
    case 0x23: printf ( "SystemError  ;PS2: return 0" ); break;
    case 0x24: printf ( "jump_to_00000000h" ); break;
    case 0x25: printf ( "jump_to_00000000h" ); break;
    case 0x26: printf ( "jump_to_00000000h" ); break;
    case 0x27: printf ( "jump_to_00000000h" ); break;
    case 0x28: printf ( "jump_to_00000000h" ); break;
    case 0x29: printf ( "jump_to_00000000h" ); break;
    case 0x2A: printf ( "SystemError  ;PS2: return 0" ); break;
    case 0x2B: printf ( "SystemError  ;PS2: return 0" ); break;
    case 0x2C: printf ( "jump_to_00000000h" ) ; break;
    case 0x2D: printf ( "jump_to_00000000h" ); break;
    case 0x2E: printf ( "jump_to_00000000h" ); break;
    case 0x2F: printf ( "jump_to_00000000h" ); break;
    case 0x30: printf ( "jump_to_00000000h" ); break;
    case 0x31: printf ( "jump_to_00000000h" ); break;
    case 0x32: printf ( "FileOpen(filename,accessmode)" ); break;
    case 0x33: printf ( "FileSeek(fd,offset,seektype)" ); break;
    case 0x34: printf ( "FileRead(fd,dst,length)" ); break;
    case 0x35:
      printf ( "FileWrite(fd=%d,src=%Xh,length=%d)",
               (int) PSX_cpu_regs.gpr[4].v,
               PSX_cpu_regs.gpr[5].v,
               (int) PSX_cpu_regs.gpr[6].v );
      show_args= false;
      break;
    case 0x36: printf ( "FileClose(fd)" ); break;
    case 0x37: printf ( "FileIoctl(fd,cmd,arg)" ); break;
    case 0x38: printf ( "exit(exitcode)" ); break;
    case 0x39: printf ( "FileGetDeviceFlag(fd)" ); break;
    case 0x3A: printf ( "FileGetc(fd)" ); break;
    case 0x3B: printf ( "FilePutc(char,fd)" ); break;
    case 0x3C: printf ( "std_in_getchar()" ); break;
    case 0x3D:
      printf ( "std_out_putchar('%c')", (int) PSX_cpu_regs.gpr[4].v );
      show_args= false;
      break;
    case 0x3E: printf ( "std_in_gets(dst)" ); break;
    case 0x3F: printf ( "std_out_puts(src)" ); break;
    case 0x40: printf ( "chdir(name)" ); break;
    case 0x41: printf ( "FormatDevice(devicename)" ); break;
    case 0x42: printf ( "firstfile(filename,direntry)" ); break;
    case 0x43: printf ( "nextfile(direntry)" ); break;
    case 0x44: printf ( "FileRename(old_filename,new_filename)" ); break;
    case 0x45: printf ( "FileDelete(filename)" ); break;
    case 0x46: printf ( "FileUndelete(filename)" ); break;
    case 0x47:
      printf ( "AddDevice(device_info);"
               " subfunction for AddXxxDevice functions" );
      break;
    case 0x48: printf ( "RemoveDevice(device_name_lowercase)" ); break;
    case 0x49: printf ( "PrintInstalledDevices()" ); break;
    case 0x4A: printf ( "InitCard(pad_enable)  ;uses/destroys k0/k1" ); break;
    case 0x4B: printf ( "StartCard()" ); break;
    case 0x4C: printf ( "StopCard()" ); break;
    case 0x4D:
      printf ( "_card_info_subfunc(port) ;subfunction for _card_info" );
      break;
    case 0x4E: printf ( "write_card_sector(port,sector,src)" ); break;
    case 0x4F: printf ( "read_card_sector(port,sector,dst)" ); break;
    case 0x50: printf ( "allow_new_card()" ); break;
    case 0x51: printf ( "Krom2RawAdd(shiftjis_code)" ); break;
    case 0x52: printf ( "SystemError  ;PS2: return 0" ); break;
    case 0x53: printf ( "Krom2Offset(shiftjis_code)" ); break;
    case 0x54: printf ( "GetLastError()" ); break;
    case 0x55: printf ( "GetLastFileError(fd)" ); break;
    case 0x56: printf ( "GetC0Table" ); break;
    case 0x57: printf ( "GetB0Table" ); break;
    case 0x58: printf ( "get_bu_callback_port()" ); break;
    case 0x59: printf ( "testdevice(devicename)" ); break;
    case 0x5A: printf ( "SystemError  ;PS2: return 0" ); break;
    case 0x5B: printf ( "ChangeClearPad(int)" ); break;
    case 0x5C: printf ( "get_card_status(slot)" ); break;
    case 0x5D: printf ( "wait_card_status(slot)" ); break;
    case 0x5E ... 0xFF: printf ( "N/A ;jump_to_00000000h" ); break;
    default: printf ( "Unknown_00B0(%Xh)", PSX_cpu_regs.gpr[9].v );
    }
  if ( show_args )
    printf ( "  ARGS: %Xh, %Xh, %Xh, %Xh, ...",
             PSX_cpu_regs.gpr[4].v,
             PSX_cpu_regs.gpr[5].v,
             PSX_cpu_regs.gpr[6].v,
             PSX_cpu_regs.gpr[7].v );
  printf ( "\n" );
  
} // end dbg_bios_func_trace_00B0


static void
dbg_bios_func_trace_00C0 (void)
{

  bool show_args;


  show_args= true;
  SHOW_PC_CC;
  printf ( "[BIOS] " );
  switch ( PSX_cpu_regs.gpr[9].v&0x7F )
    {
    case 0x00:
      printf ( "EnqueueTimerAndVblankIrqs(priority) ;used with prio=1" );
      break;
    case 0x01:
      printf ( "EnqueueSyscallHandler(priority) ;used with prio=0" );
      break;
    case 0x02: printf ( "SysEnqIntRP(priority,struc)" ); break;
    case 0x03: printf ( "SysDeqIntRP(priority,struc)" ); break;
    case 0x04: printf ( "get_free_EvCB_slot()" ); break;
    case 0x05: printf ( "get_free_TCB_slot()" ); break;
    case 0x06: printf ( "ExceptionHandler()" ); break;
    case 0x07:
      printf ( "InstallExceptionHandlers()  ;destroys/uses k0/k1" );
      break;
    case 0x08: printf ( "SysInitMemory(addr,size)" ); break;
    case 0x09: printf ( "SysInitKernelVariables()" ); break;
    case 0x0A: printf ( "ChangeClearRCnt(t,flag)" ); break;
    case 0x0B: printf ( "SystemError  ;PS2: return 0" ); break;
    case 0x0C: printf ( "InitDefInt(priority) ;used with prio=3" ); break;
    case 0x0D: printf ( "SetIrqAutoAck(irq,flag)" ); break;
    case 0x0E: printf ( "return 0 ;DTL-H2000: dev_sio_init" ); break;
    case 0x0F: printf ( "return 0 ;DTL-H2000: dev_sio_open" ); break;
    case 0x10: printf ( "return 0 ;DTL-H2000: dev_sio_in_out" ); break;
    case 0x11: printf ( "return 0 ;DTL-H2000: dev_sio_ioctl" ); break;
    case 0x12: printf ( "InstallDevices(ttyflag)" ); break;
    case 0x13: printf ( "FlushStdInOutPut()" ); break;
    case 0x14: printf ( "return 0 ;DTL-H2000: SystemError" ); break;
    case 0x15: printf ( "tty_cdevinput(circ,char)" ); break;
    case 0x16: printf ( "tty_cdevscan()" ); break;
    case 0x17:
      printf ( "tty_circgetc(circ) ;uses r5 as garbage txt for ioabort" );
      break;
    case 0x18: printf ( "tty_circputc(char,circ)" ); break;
    case 0x19: printf ( "ioabort(txt1,txt2)" ); break;
    case 0x1A:
      printf ( "set_card_find_mode(mode)  ;0=normal, 1=find deleted files" );
      break;
    case 0x1B:
      printf ( "KernelRedirect(ttyflag) ;PS2: ttyflag=1 causes SystemError" );
      break;
    case 0x1C: printf ( "AdjustA0Table()" ); break;
    case 0x1D: printf ( "get_card_find_mode()" ); break;
    case 0x1E ... 0x7F: printf ( "N/A ;jump_to_00000000h" ); break;
    default: printf ( "Unknown_00C0(%Xh)", PSX_cpu_regs.gpr[9].v );
    }
  if ( show_args )
    printf ( "  ARGS: %X, %X, %X, %X, ...",
             PSX_cpu_regs.gpr[4].v,
             PSX_cpu_regs.gpr[5].v,
             PSX_cpu_regs.gpr[6].v,
             PSX_cpu_regs.gpr[7].v );
  printf ( "\n" );
  
} // end dbg_bios_func_trace_00C0


static void
dbg_bios_func_trace (
                     const uint32_t  addr,
                     void           *udata
                     )
{

  switch ( addr )
    {
    case 0x000000A0:
    case 0x800000A0:
    case 0xA00000A0:
      dbg_bios_func_trace_00A0 ();
      break;
    case 0x000000B0:
    case 0x800000B0:
    case 0xA00000B0:
      dbg_bios_func_trace_00B0 ();
      break;
    case 0x000000C0:
    case 0x800000C0:
    case 0xA00000C0:
      dbg_bios_func_trace_00C0 ();
      break;
    }
  
} // end dbg_bios_func_trace




/*********************/
/* FUNCIONS PRIVADES */
/*********************/

static int
has_method (
            PyObject   *obj,
            const char *name
            )
{
  
  PyObject *aux;
  int ret;
  
  if ( !PyObject_HasAttrString ( obj, name ) ) return 0;
  aux= PyObject_GetAttrString ( obj, name );
  ret= PyMethod_Check ( aux );
  Py_DECREF ( aux );
  
  return ret;
  
} /* end has_method */


static void
loop (void)
{

  int cc;
  gint64 t0,last;
  bool stop;

  /* Un valor un poc arreu. Si cada T és correspon amb un cicle de
     rellotge que va a 33.87MHz approx, tenim que és
     comprova cada 1/100 segons. */
  static const int CCTOCHECK= 338700;
  
  
  stop= false;
  cc= 0; last= 0;
  for (;;)
    {
      
      /* Temps i casos especials. */
      if ( last == 0 ) { last= g_get_monotonic_time (); continue; }
      t0= g_get_monotonic_time ();
      if ( t0 <= last ) { last= t0; continue; }
      
      /* Executa. */
      cc+= (int) ((PSX_CYCLES_PER_SEC/1000000.0)*(t0-last) + 0.5);
      while ( cc > 0 )
        {
          cc-= PSX_iter ( CCTOCHECK, &stop );
          if ( stop ) return;
        }

      /* Actualitza. */
      last= t0;
      
      /* Quant més xicotet siga l'interval millor, però més
         lent. Menys d'1ms va molt lent. */
      g_usleep ( 1000 );
      
    }
  
} /* end loop */


static void 
sres_changed (
              const int width,
              const int height
              )
{
  
  /* Nou surface. */
  _screen.width= width;
  _screen.height= height;
  //_screen.surface= SDL_SetVideoMode ( width, height, 32, 0 );
  _screen.surface= SDL_SetVideoMode ( width, height, 32,
                                      SDL_HWSURFACE | SDL_GL_DOUBLEBUFFER );
  if ( _screen.surface == NULL )
    {
      fprintf ( stderr, "FATAL ERROR!!!: %s", SDL_GetError () );
      SDL_Quit ();
      return;
    }
  SDL_WM_SetCaption ( "PSX", "PSX" );
  
} /* end sres_changed */


static void
mem_changed (
             void *udata
             )
{

  PyObject *ret;
  

  if ( _tracer.dbg_flags&DBG_MEM_CHANGED )
    return dbg_mem_changed ( udata );
  
  if ( _tracer.obj == NULL ||
       !_tracer.has_mem_changed ||
       PyErr_Occurred () != NULL ) return;
  ret= PyObject_CallMethod ( _tracer.obj, "mem_changed", "" );
  Py_XDECREF ( ret );
  
} /* end mem_changed */


static void
mem_access (
            const PSX_MemAccessType  type,
            const uint32_t           addr,
            const uint32_t           data,
            const bool               error,
            void                    *udata
            )
{

  PyObject *ret;
  int aux;
  

  if ( _tracer.dbg_flags&DBG_MEM_ACCESS )
    return dbg_mem_access ( type, addr, data, error, udata );
  
  if ( _tracer.obj == NULL ||
       !_tracer.has_mem_access ||
       PyErr_Occurred () != NULL ) return;
  aux= (int) error;
  ret= PyObject_CallMethod ( _tracer.obj, "mem_access",
                             "iIIi", type, addr, data, aux );
  Py_XDECREF ( ret );
  
} /* end mem_access */


static void
mem_access16 (
              const PSX_MemAccessType  type,
              const uint32_t           addr,
              const uint16_t           data,
              const bool               error,
              void                    *udata
            )
{

  PyObject *ret;
  int aux;
  

  if ( _tracer.dbg_flags&DBG_MEM_ACCESS16 )
    return dbg_mem_access16 ( type, addr, data, error, udata );
  
  if ( _tracer.obj == NULL ||
       !_tracer.has_mem_access16 ||
       PyErr_Occurred () != NULL ) return;
  aux= (int) error;
  ret= PyObject_CallMethod ( _tracer.obj, "mem_access16",
                             "iIHi", type, addr, data, aux );
  Py_XDECREF ( ret );
  
} /* end mem_access16 */


static void
mem_access8 (
             const PSX_MemAccessType  type,
             const uint32_t           addr,
             const uint8_t            data,
             const bool               error,
             void                    *udata
             )
{

  PyObject *ret;
  int aux;
  

  if ( _tracer.dbg_flags&DBG_MEM_ACCESS8 )
    return dbg_mem_access8 ( type, addr, data, error, udata );
  
  if ( _tracer.obj == NULL ||
       !_tracer.has_mem_access8 ||
       PyErr_Occurred () != NULL ) return;
  aux= (int) error;
  ret= PyObject_CallMethod ( _tracer.obj, "mem_access8",
                             "iIBi", type, addr, data, aux );
  Py_XDECREF ( ret );
  
} /* end mem_access8 */


static void
audio_callback (
                void  *userdata,
                Uint8 *stream,
                int    len
                )
{
  
  int i;
  const int16_t *buffer;
  
  
  assert ( _audio.size == len );
  if ( _audio.buffers[_audio.buff_out].full )
    {
      buffer= _audio.buffers[_audio.buff_out].v;
      for ( i= 0; i < len; ++i )
        stream[i]= ((Uint8 *) buffer)[i];
      _audio.buffers[_audio.buff_out].full= 0;
      _audio.buff_out= (_audio.buff_out+1)%NBUFF;
    }
  else {/*printf("XOF!!\n");*/for ( i= 0; i < len; ++i ) stream[i]= _audio.silence;}
  
} /* end audio_callback */


/* Torna 0 si tot ha anat bé. */
static const char *
init_audio (void)
{
  
  SDL_AudioSpec desired, obtained;
  int n;
  Uint8 *mem;
  
  
  /* Únic camp de l'estat que s'inicialitza abans. */
  _audio.buff_out= _audio.buff_in= 0;
  for ( n= 0; n < NBUFF; ++n ) _audio.buffers[n].full= 0;
  
  /* Inicialitza. */
  desired.freq= 44100;
  desired.format= AUDIO_S16;
  desired.channels= 2;
  desired.samples= 2048;
  desired.size= 8192;
  desired.callback= audio_callback;
  desired.userdata= NULL;
  if ( SDL_OpenAudio ( &desired, &obtained ) == -1 )
    return SDL_GetError ();
  if ( obtained.format != desired.format )
    {
      fprintf ( stderr, "Força format audio\n" );
      SDL_CloseAudio ();
      if ( SDL_OpenAudio ( &desired, NULL ) == -1 )
        return SDL_GetError ();
      obtained= desired;
    }
  
  /* Inicialitza estat. */
  mem= malloc ( obtained.size*NBUFF );
  for ( n= 0; n < NBUFF; ++n, mem+= obtained.size )
    _audio.buffers[n].v= (int16_t *) mem;
  _audio.silence= (char) obtained.silence;
  _audio.pos= 0;
  _audio.size= obtained.size;
  _audio.nsamples= _audio.size/2;
  if ( obtained.freq > 44100 )
    {
      SDL_CloseAudio ();
      return "Freqüència massa gran";
    }
  _audio.ratio= 44100 / (double) obtained.freq;
  _audio.pos2= 0.0;
  
  return NULL;
  
} /* end init_audio */


static void
close_audio (void)
{
  
  SDL_CloseAudio ();
  free ( _audio.buffers[0].v );
  
} /* end close_audio */




/************/
/* FRONTEND */
/************/

static void
warning (
         void       *udata,
         const char *format,
         ...
         )
{
  
  va_list ap;
  
  
  va_start ( ap, format );
  fprintf ( stderr, "Warning: " );
  vfprintf ( stderr, format, ap );
  putc ( '\n', stderr );
  va_end ( ap );
  
} /* end warning */


static void
check_signals (
               bool *stop,
               bool *reset,
               void *udata
               )
{

  SDL_Event event;
  
  
  *stop= *reset= false;
  while ( SDL_PollEvent ( &event ) )
    switch ( event.type )
      {
      case SDL_ACTIVEEVENT:
        if ( event.active.state&SDL_APPINPUTFOCUS &&
             !event.active.gain )
          _control.pad1.buttons= 0;
        break;
      case SDL_KEYDOWN:
        if ( event.key.keysym.mod&KMOD_CTRL )
          {
            switch ( event.key.keysym.sym )
              {
              case SDLK_q: *stop= true; break;
              case SDLK_w: MY_GLOBAL_PRINT= 1; break;
              case SDLK_r: *reset= true;
              default: break;
              }
          }
        else
          {
            switch ( event.key.keysym.sym )
              {
              case SDLK_RETURN:
        	_control.pad1.buttons|= PSX_BUTTON_SELECT;
        	break;
              case SDLK_SPACE:
        	_control.pad1.buttons|= PSX_BUTTON_START;
        	break;
              case SDLK_w:
        	_control.pad1.buttons|= PSX_BUTTON_UP;
        	break;
              case SDLK_s:
        	_control.pad1.buttons|= PSX_BUTTON_DOWN;
        	break;
              case SDLK_a:
        	_control.pad1.buttons|= PSX_BUTTON_LEFT;
        	break;
              case SDLK_d:
        	_control.pad1.buttons|= PSX_BUTTON_RIGHT;
        	break;
              case SDLK_i:
        	_control.pad1.buttons|= PSX_BUTTON_TRIANGLE;
        	break;
              case SDLK_o:
        	_control.pad1.buttons|= PSX_BUTTON_CIRCLE;
        	break;
              case SDLK_k:
        	_control.pad1.buttons|= PSX_BUTTON_SQUARE;
        	break;
              case SDLK_l:
        	_control.pad1.buttons|= PSX_BUTTON_CROSS;
        	break;
              case SDLK_u:
        	_control.pad1.buttons|= PSX_BUTTON_L1;
        	break;
              case SDLK_p:
        	_control.pad1.buttons|= PSX_BUTTON_R1;
        	break;
              case SDLK_q:
        	_control.pad1.buttons|= PSX_BUTTON_L2;
        	break;
              case SDLK_e:
        	_control.pad1.buttons|= PSX_BUTTON_R2;
        	break;
              default: break;
              }
          }
        break;
      case SDL_KEYUP:
        switch ( event.key.keysym.sym )
          {
          case SDLK_RETURN:
            _control.pad1.buttons&= ~PSX_BUTTON_SELECT;
            break;
          case SDLK_SPACE:
            _control.pad1.buttons&= ~PSX_BUTTON_START;
            break;
          case SDLK_w:
            _control.pad1.buttons&= ~PSX_BUTTON_UP;
            break;
          case SDLK_s:
            _control.pad1.buttons&= ~PSX_BUTTON_DOWN;
            break;
          case SDLK_a:
            _control.pad1.buttons&= ~PSX_BUTTON_LEFT;
            break;
          case SDLK_d:
            _control.pad1.buttons&= ~PSX_BUTTON_RIGHT;
            break;
          case SDLK_i:
            _control.pad1.buttons&= ~PSX_BUTTON_TRIANGLE;
            break;
          case SDLK_o:
            _control.pad1.buttons&= ~PSX_BUTTON_CIRCLE;
            break;
          case SDLK_k:
            _control.pad1.buttons&= ~PSX_BUTTON_SQUARE;
            break;
          case SDLK_l:
            _control.pad1.buttons&= ~PSX_BUTTON_CROSS;
            break;
          case SDLK_u:
            _control.pad1.buttons&= ~PSX_BUTTON_L1;
            break;
          case SDLK_p:
            _control.pad1.buttons&= ~PSX_BUTTON_R1;
            break;
          case SDLK_q:
            _control.pad1.buttons&= ~PSX_BUTTON_L2;
            break;
          case SDLK_e:
            _control.pad1.buttons&= ~PSX_BUTTON_R2;
            break;
          default: break;
          }
        break;
      default: break;
      }
  
} /* end check_signals */


static Uint32
reorder_color (
               const Uint32 color
               )
{
  return 
    (((uint32_t) (((uint8_t *) &color)[0]))<<16) |
    (((uint32_t) (((uint8_t *) &color)[1]))<<8) |
    (((uint32_t) (((uint8_t *) &color)[2]))<<0);
} // end reorder_color

static void
update_screen (
               const uint32_t                 *fb,
               const PSX_UpdateScreenGeometry *g,
               void                           *udata
               )
{

  Uint32 *data;
  int i;
  //static gint64 t= 0;gint64 aux;

  
  //aux= g_get_monotonic_time ();printf("%ld\n",aux-t);t=aux;
  //return;
  if ( g->width != _screen.width || g->height != _screen.height )
    sres_changed ( g->width, g->height );
  if ( SDL_MUSTLOCK ( _screen.surface ) )
    SDL_LockSurface ( _screen.surface );
  data= _screen.surface->pixels;
  for ( i= 0; i < _screen.width*_screen.height; ++i )
    data[i]= reorder_color ( (Uint32) fb[i] );
  
  if ( SDL_MUSTLOCK ( _screen.surface ) )
    SDL_UnlockSurface ( _screen.surface );

  //SDL_UpdateRect(_screen.surface, 0, 0, 0, 0);
  if ( SDL_Flip ( _screen.surface ) == -1 )
    {
      fprintf ( stderr, "ERROR FATAL !!!: %s\n", SDL_GetError () );
      SDL_Quit ();
    }
  
} /* end update_screen */


static void
play_sound (
            const int16_t  samples[PSX_AUDIO_BUFFER_SIZE*2],
            void          *udata
            )
{

  int nofull, j;
  int16_t *buffer;
  
  
  for (;;)
    {
      
      while ( _audio.buffers[_audio.buff_in].full ) SDL_Delay ( 1 );
      buffer= _audio.buffers[_audio.buff_in].v;
      
      j= (int) (_audio.pos2 + 0.5);
      while ( (nofull= (_audio.pos != _audio.nsamples)) &&
              j < PSX_AUDIO_BUFFER_SIZE )
        {
          buffer[_audio.pos++]= samples[2*j];
          buffer[_audio.pos++]= samples[2*j+1];
          _audio.pos2+= _audio.ratio;
          j= (int) (_audio.pos2 + 0.5);
        }
      if ( !nofull )
        {
          _audio.pos= 0;
          _audio.buffers[_audio.buff_in].full= 1;
          _audio.buff_in= (_audio.buff_in+1)%NBUFF;
        }
      if ( j >= PSX_AUDIO_BUFFER_SIZE )
        {
          _audio.pos2-= PSX_AUDIO_BUFFER_SIZE;
          break;
        }
      
    }
  
} // end play_sound


static const PSX_ControllerState *
get_controller_state (
        	      const int  joy,
        	      void      *udata
        	      )
{

  //if ( joy == 1 ) return NULL;
  
  return joy==0 ? &_control.pad1 : &_control.pad2;
  
} // end get_controller_state

static void
cpu_inst (
          const PSX_Inst *inst,
          const uint32_t  addr,
          void           *udata
          )
{
  
  PyObject *ret;
  

  if ( _tracer.dbg_flags&DBG_BIOS_FUNC_TRACE )
    dbg_bios_func_trace ( addr, udata );
  if ( _tracer.dbg_flags&DBG_CPU_INST )
    return dbg_cpu_inst ( inst, addr, udata );
  
  if ( _tracer.obj == NULL ||
       !_tracer.has_cpu_inst ||
       PyErr_Occurred () != NULL ) return;

  ret= PyObject_CallMethod ( _tracer.obj, "cpu_inst",
        		     "IIiiii"
        		     "(iiiIiiibiii)",
        		     addr,
        		     inst->word,
        		     inst->name,
        		     inst->op1,
        		     inst->op2,
        		     inst->op3,
        		     inst->extra.rd,
        		     inst->extra.rs,
        		     inst->extra.rt,
        		     inst->extra.imm,
        		     inst->extra.off,
        		     inst->extra.sa,
        		     inst->extra.cop2_sf,
        		     inst->extra.cop2_lm_is_0,
        		     inst->extra.cop2_mx,
        		     inst->extra.cop2_v,
        		     inst->extra.cop2_cv );
  Py_XDECREF ( ret );
  
} /* end cpu_inst */


static void
gpu_cmd_trace (
               const PSX_GPUCmd *cmd,
               void             *udata
               )
{
  
  PyObject *ret;
  

  if ( _tracer.dbg_flags&DBG_GPU_CMD_TRACE )
    return dbg_gpu_cmd_trace ( cmd, udata );
  
  if ( _tracer.obj == NULL ||
       !_tracer.has_gpu_cmd_trace ||
       PyErr_Occurred () != NULL ) return;
  
  ret= PyObject_CallMethod ( _tracer.obj, "gpu_cmd_trace",
        		     "Iiliii((iiBBBBB)(iiBBBBB)(iiBBBBB)(iiBBBBB))"
        		     "iiiiii",
        		     cmd->word,
        		     cmd->name,
        		     cmd->ops,
        		     cmd->width,
        		     cmd->height,
        		     cmd->Nv,
        		     cmd->v[0].x,cmd->v[0].y,
        		     cmd->v[0].u,cmd->v[0].v,
        		     cmd->v[0].r,cmd->v[0].g,cmd->v[0].b,
        		     cmd->v[1].x,cmd->v[1].y,
        		     cmd->v[1].u,cmd->v[1].v,
        		     cmd->v[1].r,cmd->v[1].g,cmd->v[1].b,
        		     cmd->v[2].x,cmd->v[2].y,
        		     cmd->v[2].u,cmd->v[2].v,
        		     cmd->v[2].r,cmd->v[2].g,cmd->v[2].b,
        		     cmd->v[3].x,cmd->v[3].y,
        		     cmd->v[3].u,cmd->v[3].v,
        		     cmd->v[3].r,cmd->v[3].g,cmd->v[3].b,
        		     cmd->texclut_x,cmd->texclut_y,
        		     cmd->texpage_x,cmd->texpage_y,
        		     cmd->tex_pol_transparency,
        		     cmd->tex_pol_mode);
  Py_XDECREF ( ret );
  
} // end gpu_cmd_trace


static void
cd_cmd_trace (
              const PSX_CDCmd *cmd,
              void            *udata
              )
{
  
  PyObject *ret;
  
  
  if ( _tracer.dbg_flags&DBG_CD_CMD_TRACE )
    return dbg_cd_cmd_trace ( cmd, udata );
  
  if ( _tracer.obj == NULL ||
       !_tracer.has_cd_cmd_trace ||
       PyErr_Occurred () != NULL ) return;
  
  ret= PyObject_CallMethod ( _tracer.obj, "cd_cmd_trace",
        		     "Biy#",
        		     cmd->cmd,
        		     cmd->name,
        		     cmd->args.v,
        		     cmd->args.N );
  Py_XDECREF ( ret );
  
} // end cd_cmd_trace


static void
int_trace (
           const bool      is_ack,
           const uint32_t  old_i_stat,
           const uint32_t  new_i_stat,
           const uint32_t  i_mask,
           void           *udata
           )
{

  if ( _tracer.dbg_flags&DBG_INT_TRACE )
    return dbg_int_trace ( is_ack, old_i_stat, new_i_stat, i_mask, udata );

  /*
  if ( _tracer.obj == NULL ||
       !_tracer.has_mem_changed ||
       PyErr_Occurred () != NULL ) return;
  ret= PyObject_CallMethod ( _tracer.obj, "mem_changed", "" );
  Py_XDECREF ( ret );
  */
  
} // end int_trace


static void
dma_transfer (
              const int       channel,
              const bool      to_ram, // false indica from_ram
              const uint32_t  addr,
              void           *udata
              )
{

  if ( _tracer.dbg_flags&DBG_DMA_TRANSFER )
    return dbg_dma_transfer ( channel, to_ram, addr, udata );

  /*
  if ( _tracer.obj == NULL ||
       !_tracer.has_mem_changed ||
       PyErr_Occurred () != NULL ) return;
  ret= PyObject_CallMethod ( _tracer.obj, "mem_changed", "" );
  Py_XDECREF ( ret );
  */
  
} // end dma_transfer


static void
gte_cmd_trace (
               uint32_t  regs_prev[64],
               uint32_t  regs_after[64],
               void     *udata
               )
{
  
  if ( _tracer.dbg_flags&DBG_GTE_CMD_TRACE )
    return dbg_gte_cmd_trace ( regs_prev, regs_after, udata );
  
} // end gte_cmd_trace


static void
gte_mem_access (
        	const bool      read,
        	const int       reg,
        	const uint32_t  val,
        	const bool      ok,
        	void           *udata
        	)
{

  if ( _tracer.dbg_flags&DBG_GTE_MEM_ACCESS )
    return dbg_gte_mem_access ( read, reg, val, ok, udata );

  /*
  if ( _tracer.obj == NULL ||
       !_tracer.has_mem_changed ||
       PyErr_Occurred () != NULL ) return;
  ret= PyObject_CallMethod ( _tracer.obj, "mem_changed", "" );
  Py_XDECREF ( ret );
  */
  
} // end gte_mem_access




/******************/
/* FUNCIONS MÒDUL */
/******************/

static PyObject *
PSX_check_signals (
        	   PyObject *self,
        	   PyObject *args
        	   )
{
  
  bool stop, reset;
  
  
  CHECK_INITIALIZED;
  
  check_signals ( &stop, &reset, NULL );
  
  Py_RETURN_NONE;
  
} /* end PSX_check_signals */


static PyObject *
PSX_close_ (
            PyObject *self,
            PyObject *args
            )
{
  
  if ( !_initialized ) Py_RETURN_NONE;

  if ( _renderer != NULL ) PSX_renderer_free ( _renderer );
  _renderer= NULL;
  close_audio ();
  SDL_Quit ();
  /*
  if ( _sram != NULL ) free ( _sram );
  if ( _eeprom != NULL ) free ( _eeprom );
  */
  _initialized= false;
  /*Py_XDECREF ( _tracer.obj );*/
  
  Py_RETURN_NONE;
  
} /* end PSX_close_ */


static PyObject *
PSX_init_module (
        	 PyObject *self,
        	 PyObject *args
        	 )
{

  static const PSX_TraceCallbacks trace_callbacks=
    {
      mem_changed,
      mem_access,
      mem_access16,
      mem_access8,
      cpu_inst,
      gpu_cmd_trace,
      cd_cmd_trace,
      int_trace,
      dma_transfer,
      gte_cmd_trace,
      gte_mem_access
    };
  static const PSX_Frontend frontend=
    {
      warning,
      check_signals,
      play_sound,
      get_controller_state,
      &trace_callbacks
    };
  
  const char *err;
  PyObject *bytes;
  Py_ssize_t size;
  

  if ( _initialized ) Py_RETURN_NONE;
  if ( !PyArg_ParseTuple ( args, "O!", &PyBytes_Type, &bytes ) )
    return NULL;

  /* Prepara. */
  _renderer= NULL;
  
  /* Comprova BIOS. */
  size= PyBytes_Size ( bytes );
  if ( size != PSX_BIOS_SIZE )
    {
      PyErr_SetString ( PSXError, "Invalid BIOS size" );
      return NULL;
    }
  memcpy ( _bios, PyBytes_AS_STRING ( bytes ), sizeof(_bios) );
  
  /* Renderer. */
  _renderer= PSX_create_default_renderer ( update_screen, NULL );
  //_renderer= PSX_create_stats_renderer ();
  if ( _renderer == NULL ) goto error;
  
  /* SDL */
  if ( SDL_Init ( SDL_INIT_VIDEO |
                  SDL_INIT_NOPARACHUTE |
                  SDL_INIT_AUDIO ) == -1 )
    {
      PyErr_SetString ( PSXError, SDL_GetError () );
      goto error;
    }
  _screen.surface= NULL;
  _screen.width= -1;
  _screen.height= -1;
  if ( (err= init_audio ()) != NULL )
    {
      PyErr_SetString ( PSXError, err );
      SDL_Quit ();
      return NULL; 
    }
  
  /* Control. */
  _control.pad1.buttons= 0;
  _control.pad2.buttons= 0;
  
  /* Tracer. */
  _tracer.obj= NULL;
  _tracer.dbg_flags= 0;
  _tracer.pc= 0;
  _tracer.cc= 0;
  _tracer.steps= 0;

  /* Inicialitza el simulador. */
  PSX_init ( _bios, &frontend, NULL, _renderer );
  PSX_plug_controllers ( PSX_CONTROLLER_STANDARD, PSX_CONTROLLER_STANDARD);
  _tracer.pc= PSX_cpu_regs.pc;

  // Disc.
  _disc= NULL;
  
  _initialized= true;
  
  Py_RETURN_NONE;

 error:
  if ( _renderer != NULL ) PSX_renderer_free ( _renderer );
  _renderer= NULL;
  return NULL;
  
} /* end PSX_init_module */


static PyObject *
PSX_steps_module (
                  PyObject *self,
                  PyObject *args
                  )
{

  bool stop;
  int nsteps,cc;
  int n;

  
  CHECK_INITIALIZED;
  
  if ( !PyArg_ParseTuple ( args, "i", &nsteps ) )
    return NULL;
  
  for ( n= 0; n < NBUFF; ++n ) _audio.buffers[n].full= 0;
  SDL_PauseAudio ( 0 );
  cc= nsteps;
  while ( cc > 0 )
    cc-= PSX_iter ( cc, &stop );
  SDL_PauseAudio ( 1 );
  
  Py_RETURN_NONE;
  
} // end PSX_steps_module


static PyObject *
PSX_loop_module (
        	 PyObject *self,
        	 PyObject *args
        	 )
{

  int n;

  
  CHECK_INITIALIZED;
  
  for ( n= 0; n < NBUFF; ++n ) _audio.buffers[n].full= 0;
  SDL_PauseAudio ( 0 );
  loop ();
  SDL_PauseAudio ( 1 );
  
  Py_RETURN_NONE;
  
} /* end PSX_loop_module */


static PyObject *
PSX_plug_mem_cards_ (
                     PyObject *self,
                     PyObject *args
                     )
{
  
  static uint8_t mem1[MEMCARD_SIZE];
  static uint8_t mem2[MEMCARD_SIZE];
  
  PyObject *mc1,*mc2;
  uint8_t *memc1, *memc2;
  char *tmp;

  
  CHECK_INITIALIZED;
  if ( !PyArg_ParseTuple ( args, "OO", &mc1, &mc2 ) )
    return NULL;
  if ( mc1 == Py_None ) memc1= NULL;
  else if ( !PyBytes_Check ( mc1 ) )
    {
      PyErr_SetString ( PSXError, "memory card 1 must be Byts o None" );
      return NULL;
    }
  else
    {
      if ( PyBytes_Size ( mc1 ) != MEMCARD_SIZE )
        {
          PyErr_SetString ( PSXError, "memory card 1 size must be 128KB" );
          return NULL;
        }
      tmp= PyBytes_AsString ( mc1 );
      assert ( tmp != NULL );
      memcpy ( mem1, tmp, MEMCARD_SIZE );
      memc1= &(mem1[0]);
    }
  if ( mc2 == Py_None ) memc2= NULL;
  else if ( !PyBytes_Check ( mc2 ) )
    {
      PyErr_SetString ( PSXError, "memory card 2 must be Byts o None" );
      return NULL;
    }
  else
    {
      if ( PyBytes_Size ( mc2 ) != MEMCARD_SIZE )
        {
          PyErr_SetString ( PSXError, "memory card 2 size must be 128KB" );
          return NULL;
        }
      tmp= PyBytes_AsString(mc2);
      assert ( tmp != NULL );
      memcpy ( mem2, tmp, MEMCARD_SIZE );
      memc2= &(mem2[0]);
    }
  
  // Plug memory cards.
  PSX_plug_mem_cards ( memc1, memc2 );
  
  Py_RETURN_NONE;
  
} // end PSX_plug_mem_cards_


static PyObject *
PSX_set_disc_ (
               PyObject *self,
               PyObject *args
               )
{

  const char *fn;
  char *err;
  
  
  CHECK_INITIALIZED;
  if ( !PyArg_ParseTuple ( args, "z", &fn ) )
    return NULL;

  // Free current disc.
  if ( _disc != NULL )
    {
      CD_disc_free ( _disc );
      _disc= NULL;
    }

  // Open new disc if any.
  if ( fn != NULL )
    {
      _disc= CD_disc_new ( fn, &err );
      if ( _disc == NULL )
        {
          PyErr_SetString ( PSXError, err );
          free ( err );
          return NULL;
        }
    }

  // Set/unset disc.
  PSX_set_disc ( _disc );
  
  Py_RETURN_NONE;
  
} // end PSX_set_dsic_


static PyObject *
PSX_set_tracer (
        	PyObject *self,
        	PyObject *args
        	)
{
  
  PyObject *aux;
  
  
  CHECK_INITIALIZED;
  if ( !PyArg_ParseTuple ( args, "O", &aux ) )
    return NULL;
  Py_XDECREF ( _tracer.obj );
  _tracer.obj= aux;
  Py_INCREF ( _tracer.obj );
  
  if ( _tracer.obj != NULL )
    {
      _tracer.has_mem_changed= has_method ( _tracer.obj, "mem_changed" );
      _tracer.has_mem_access= has_method ( _tracer.obj, "mem_access" );
      _tracer.has_mem_access16= has_method ( _tracer.obj, "mem_access16" );
      _tracer.has_mem_access8= has_method ( _tracer.obj, "mem_access8" );
      _tracer.has_cpu_inst= has_method ( _tracer.obj, "cpu_inst" );
      _tracer.has_gpu_cmd_trace= has_method ( _tracer.obj, "gpu_cmd_trace" );
      _tracer.has_cd_cmd_trace= has_method ( _tracer.obj, "cd_cmd_trace" );
    }
  
  Py_RETURN_NONE;
  
} /* end PSX_set_tracer */


static PyObject *
PSX_trace_module (
        	  PyObject *self,
        	  PyObject *args
        	  )
{
  
  int cc,nsteps,n,inst_cc;
  
  
  CHECK_INITIALIZED;

  nsteps= 1;
  if ( !PyArg_ParseTuple ( args, "|i", &nsteps ) )
    return NULL;
  SDL_PauseAudio ( 0 );
  cc= 0;
  for ( n= 0; n < nsteps; ++n)
    {
      inst_cc= PSX_trace ();
      cc+= inst_cc;
      ++_tracer.steps;
      _tracer.cc+= (uint64_t) inst_cc;
      _tracer.pc= PSX_cpu_regs.pc;
    }
  SDL_PauseAudio ( 1 );
  if ( PyErr_Occurred () != NULL ) return NULL;
  
  return PyLong_FromLong ( cc );
  
} /* end PSX_trace_module */


static PyObject *
PSX_get_mem_map (
        	 PyObject *self,
        	 PyObject *args
        	 )
{

  PSX_MemMap map;
  PyObject *dict, *aux, *aux2;
  
  
  CHECK_INITIALIZED;

  PSX_mem_get_map ( &map );
  dict= PyDict_New ();
  if ( dict == NULL ) return NULL;

  /* RAM. */
  aux= PyTuple_New ( 3 );
  if ( aux == NULL ) goto error;
  if ( PyDict_SetItemString ( dict, "ram", aux ) == -1 ) goto error_aux;
  Py_XDECREF ( aux );
  aux2= PyLong_FromLong ( map.ram.end_ram );
  if ( aux2 == NULL ) goto error;
  PyTuple_SET_ITEM ( aux, 0, aux2 );
  aux2= PyLong_FromLong ( map.ram.end_hz );
  if ( aux2 == NULL ) goto error;
  PyTuple_SET_ITEM ( aux, 1, aux2 );
  aux2= PyLong_FromLong ( map.ram.locked_00800000 );
  if ( aux2 == NULL ) goto error;
  PyTuple_SET_ITEM ( aux, 2, aux2 );
  
  return dict;

 error_aux:
  Py_XDECREF ( aux );
 error:
  Py_XDECREF ( dict );
  return NULL;
  
} /* end PSX_get_mem_map */


static PyObject *
PSX_get_frame_buffer (
        	      PyObject *self,
        	      PyObject *args
        	      )
{

  CHECK_INITIALIZED;
  
  return PyBytes_FromStringAndSize ( (const char *) PSX_gpu_get_frame_buffer (),
        			     1024*512*sizeof(uint16_t) );
  
} // end PSX_get_frame_buffer


static PyObject *
PSX_config_debug (
        	  PyObject *self,
        	  PyObject *args
        	  )
{
  
  int flags;
  
  
  CHECK_INITIALIZED;
  if ( !PyArg_ParseTuple ( args, "i", &flags ) )
    return NULL;
  _tracer.dbg_flags= flags;
  
  Py_RETURN_NONE;
  
} // end PSX_config_debug


static PyObject *
PSX_print_regs (
                PyObject *self,
                PyObject *args
                )
{

  CHECK_INITIALIZED;
  
  // Registres especials.
  SHOW_PC_CC;
  printf ( "[CPU] PC:%08X HI:%08X LO:%08X\n",
           PSX_cpu_regs.pc, PSX_cpu_regs.hi, PSX_cpu_regs.lo );

  // Registres generals
  SHOW_PC_CC;
  printf ( "[CPU] ZERO:%08X AT:%08X V0:%08X V1:%08X\n",
           PSX_cpu_regs.gpr[0].v,
           PSX_cpu_regs.gpr[1].v,
           PSX_cpu_regs.gpr[2].v,
           PSX_cpu_regs.gpr[3].v );
  SHOW_PC_CC;
  printf ( "[CPU] A0:%08X   A1:%08X A2:%08X A3:%08X\n",
           PSX_cpu_regs.gpr[4].v,
           PSX_cpu_regs.gpr[5].v,
           PSX_cpu_regs.gpr[6].v,
           PSX_cpu_regs.gpr[7].v );
  SHOW_PC_CC;
  printf ( "[CPU] T0:%08X   T1:%08X T2:%08X T3:%08X\n",
           PSX_cpu_regs.gpr[8].v,
           PSX_cpu_regs.gpr[9].v,
           PSX_cpu_regs.gpr[10].v,
           PSX_cpu_regs.gpr[11].v );
  SHOW_PC_CC;
  printf ( "[CPU] T4:%08X   T5:%08X T6:%08X T7:%08X\n",
           PSX_cpu_regs.gpr[12].v,
           PSX_cpu_regs.gpr[13].v,
           PSX_cpu_regs.gpr[14].v,
           PSX_cpu_regs.gpr[15].v );
  SHOW_PC_CC;
  printf ( "[CPU] S0:%08X   S1:%08X S2:%08X S3:%08X\n",
           PSX_cpu_regs.gpr[16].v,
           PSX_cpu_regs.gpr[17].v,
           PSX_cpu_regs.gpr[18].v,
           PSX_cpu_regs.gpr[19].v );
  SHOW_PC_CC;
  printf ( "[CPU] S4:%08X   S5:%08X S6:%08X S7:%08X\n",
           PSX_cpu_regs.gpr[20].v,
           PSX_cpu_regs.gpr[21].v,
           PSX_cpu_regs.gpr[22].v,
           PSX_cpu_regs.gpr[23].v );
  SHOW_PC_CC;
  printf ( "[CPU] T8:%08X   T9:%08X K0:%08X K1:%08X\n",
           PSX_cpu_regs.gpr[24].v,
           PSX_cpu_regs.gpr[25].v,
           PSX_cpu_regs.gpr[26].v,
           PSX_cpu_regs.gpr[27].v );
  SHOW_PC_CC;
  printf ( "[CPU] GP:%08X   SP:%08X FP:%08X RA:%08X\n",
           PSX_cpu_regs.gpr[28].v,
           PSX_cpu_regs.gpr[29].v,
           PSX_cpu_regs.gpr[30].v,
           PSX_cpu_regs.gpr[31].v );
  
  // Registres COP0
  SHOW_PC_CC;
  printf( "[CPU] BPC:%08X  BDA:%08X  DCIC:%08X BAD_VADDR:%08X\n",
          PSX_cpu_regs.cop0r3_bpc,
          PSX_cpu_regs.cop0r5_bda,
          PSX_cpu_regs.cop0r7_dcic,
          PSX_cpu_regs.cop0r8_bad_vaddr );
  SHOW_PC_CC;
  printf( "[CPU] BDAM:%08X BPCM:%08X SR:%08X   CAUSE:%08X\n",
          PSX_cpu_regs.cop0r9_bdam,
          PSX_cpu_regs.cop0r11_bpcm,
          PSX_cpu_regs.cop0r12_sr,
          PSX_cpu_regs.cop0r13_cause );
  SHOW_PC_CC;
  printf( "[CPU] EPC:%08X\n",
          PSX_cpu_regs.cop0r14_epc );
  
  Py_RETURN_NONE;
  
} // end PSX_print_regs


static PyObject *
PSX_press_button (
                  PyObject *self,
                  PyObject *args
                  )
{
  
  int but;

  
  CHECK_INITIALIZED;

  if ( !PyArg_ParseTuple ( args, "i", &but ) )
    return NULL;
  _control.pad1.buttons|= but;
  
  Py_RETURN_NONE;
  
} // end PSX_press_button


static PyObject *
PSX_release_button (
                    PyObject *self,
                    PyObject *args
                    )
{
  
  int but;

  
  CHECK_INITIALIZED;

  if ( !PyArg_ParseTuple ( args, "i", &but ) )
    return NULL;
  _control.pad1.buttons&= ~but;
  
  Py_RETURN_NONE;
  
} // end PSX_release_button




/************************/
/* INICIALITZACIÓ MÒDUL */
/************************/

static PyMethodDef PSXMethods[]=
  {
    { "check_signals", PSX_check_signals, METH_VARARGS,
      "Force signals checking" },
    { "close", PSX_close_, METH_VARARGS,
      "Free module resources and close the module" },
    { "init", PSX_init_module, METH_VARARGS,
      "Initialize the module (requires bios as argument)" },
    { "loop", PSX_loop_module, METH_VARARGS,
      "Run the simulator into a loop and block" },
    { "steps", PSX_steps_module, METH_VARARGS,
      "Run the simulator the number of specified cycles" },
    { "plug_mem_cards", PSX_plug_mem_cards_, METH_VARARGS,
      "Plugs memory cards" },
    { "set_disc", PSX_set_disc_, METH_VARARGS,
      "Set a new CD disc. None means no disc" },
    { "set_tracer", PSX_set_tracer, METH_VARARGS,
      "Set a python object to trace the execution." },
    { "trace", PSX_trace_module, METH_VARARGS,
      "Executes nstep instruction\n"
      "  trace(nsteps=1)"},
    { "get_mem_map", PSX_get_mem_map, METH_VARARGS,
      "Gets the current memory map" },
    { "get_frame_buffer", PSX_get_frame_buffer, METH_NOARGS,
      "Returns the frame buffer" },
    { "config_debug", PSX_config_debug, METH_VARARGS,
      "Enable C debugger" },
    { "print_regs", PSX_print_regs, METH_NOARGS,
      "Print CPU registers" },
    { "press_button", PSX_press_button, METH_VARARGS,
      "Press button" },
    { "release_button", PSX_release_button, METH_VARARGS,
      "Release button" },
    
    { NULL, NULL, 0, NULL }
  };


static struct PyModuleDef PSXmodule=
  {
    PyModuleDef_HEAD_INIT,
    "PSX",
    NULL,
    -1,
    PSXMethods
  };


PyMODINIT_FUNC
PyInit_PSX (void)
{
  
  PyObject *m;
  
  
  m= PyModule_Create ( &PSXmodule );
  if ( m == NULL ) return NULL;
  
  _initialized= false;
  PSXError= PyErr_NewException ( "PSX.error", NULL, NULL );
  Py_INCREF ( PSXError );
  PyModule_AddObject ( m, "error", PSXError );

  /* Mnemonics. */
  PyModule_AddIntConstant ( m, "UNK", PSX_UNK );
  PyModule_AddIntConstant ( m, "ADD", PSX_ADD );
  PyModule_AddIntConstant ( m, "ADDI", PSX_ADDI );
  PyModule_AddIntConstant ( m, "ADDIU", PSX_ADDIU );
  PyModule_AddIntConstant ( m, "ADDU", PSX_ADDU );
  PyModule_AddIntConstant ( m, "AND", PSX_AND );
  PyModule_AddIntConstant ( m, "ANDI", PSX_ANDI );
  PyModule_AddIntConstant ( m, "BEQ", PSX_BEQ );
  PyModule_AddIntConstant ( m, "BGEZ", PSX_BGEZ );
  PyModule_AddIntConstant ( m, "BGEZAL", PSX_BGEZAL );
  PyModule_AddIntConstant ( m, "BGTZ", PSX_BGTZ );
  PyModule_AddIntConstant ( m, "BLEZ", PSX_BLEZ );
  PyModule_AddIntConstant ( m, "BLTZ", PSX_BLTZ );
  PyModule_AddIntConstant ( m, "BLTZAL", PSX_BLTZAL );
  PyModule_AddIntConstant ( m, "BNE", PSX_BNE );
  PyModule_AddIntConstant ( m, "BREAK", PSX_BREAK );
  PyModule_AddIntConstant ( m, "CFC2", PSX_CFC2 );
  PyModule_AddIntConstant ( m, "COP0_RFE", PSX_COP0_RFE );
  PyModule_AddIntConstant ( m, "COP0_TLBP", PSX_COP0_TLBP );
  PyModule_AddIntConstant ( m, "COP0_TLBR", PSX_COP0_TLBR );
  PyModule_AddIntConstant ( m, "COP0_TLBWI", PSX_COP0_TLBWI );
  PyModule_AddIntConstant ( m, "COP0_TLBWR", PSX_COP0_TLBWR );
  PyModule_AddIntConstant ( m, "COP2_RTPS", PSX_COP2_RTPS );
  PyModule_AddIntConstant ( m, "COP2_RTPT", PSX_COP2_RTPT );
  PyModule_AddIntConstant ( m, "COP2_NCLIP", PSX_COP2_NCLIP );
  PyModule_AddIntConstant ( m, "COP2_AVSZ3", PSX_COP2_AVSZ3 );
  PyModule_AddIntConstant ( m, "COP2_AVSZ4", PSX_COP2_AVSZ4 );
  PyModule_AddIntConstant ( m, "COP2_MVMVA", PSX_COP2_MVMVA );
  PyModule_AddIntConstant ( m, "COP2_SQR", PSX_COP2_SQR );
  PyModule_AddIntConstant ( m, "COP2_OP", PSX_COP2_OP );
  PyModule_AddIntConstant ( m, "COP2_NCS", PSX_COP2_NCS );
  PyModule_AddIntConstant ( m, "COP2_NCT", PSX_COP2_NCT );
  PyModule_AddIntConstant ( m, "COP2_NCCS", PSX_COP2_NCCS );
  PyModule_AddIntConstant ( m, "COP2_NCCT", PSX_COP2_NCCT );
  PyModule_AddIntConstant ( m, "COP2_NCDS", PSX_COP2_NCDS );
  PyModule_AddIntConstant ( m, "COP2_NCDT", PSX_COP2_NCDT );
  PyModule_AddIntConstant ( m, "COP2_CC", PSX_COP2_CC );
  PyModule_AddIntConstant ( m, "COP2_CDP", PSX_COP2_CDP );
  PyModule_AddIntConstant ( m, "COP2_DCPL", PSX_COP2_DCPL );
  PyModule_AddIntConstant ( m, "COP2_DPCS", PSX_COP2_DPCS );
  PyModule_AddIntConstant ( m, "COP2_DPCT", PSX_COP2_DPCT );
  PyModule_AddIntConstant ( m, "COP2_INTPL", PSX_COP2_INTPL );
  PyModule_AddIntConstant ( m, "COP2_GPF", PSX_COP2_GPF );
  PyModule_AddIntConstant ( m, "COP2_GPL", PSX_COP2_GPL );
  PyModule_AddIntConstant ( m, "CTC2", PSX_CTC2 );
  PyModule_AddIntConstant ( m, "DIV", PSX_DIV );
  PyModule_AddIntConstant ( m, "DIVU", PSX_DIVU );
  PyModule_AddIntConstant ( m, "J", PSX_J );
  PyModule_AddIntConstant ( m, "JAL", PSX_JAL );
  PyModule_AddIntConstant ( m, "JALR", PSX_JALR );
  PyModule_AddIntConstant ( m, "JR", PSX_JR );
  PyModule_AddIntConstant ( m, "LB", PSX_LB );
  PyModule_AddIntConstant ( m, "LBU", PSX_LBU );
  PyModule_AddIntConstant ( m, "LH", PSX_LH );
  PyModule_AddIntConstant ( m, "LHU", PSX_LHU );
  PyModule_AddIntConstant ( m, "LUI", PSX_LUI );
  PyModule_AddIntConstant ( m, "LW", PSX_LW );
  PyModule_AddIntConstant ( m, "LWC2", PSX_LWC2 );
  PyModule_AddIntConstant ( m, "LWL", PSX_LWL );
  PyModule_AddIntConstant ( m, "LWR", PSX_LWR );
  PyModule_AddIntConstant ( m, "MFC0", PSX_MFC0 );
  PyModule_AddIntConstant ( m, "MFC2", PSX_MFC2 );
  PyModule_AddIntConstant ( m, "MFHI", PSX_MFHI );
  PyModule_AddIntConstant ( m, "MFLO", PSX_MFLO );
  PyModule_AddIntConstant ( m, "MTC0", PSX_MTC0 );
  PyModule_AddIntConstant ( m, "MTC2", PSX_MTC2 );
  PyModule_AddIntConstant ( m, "MTHI", PSX_MTHI );
  PyModule_AddIntConstant ( m, "MTLO", PSX_MTLO );
  PyModule_AddIntConstant ( m, "MULT", PSX_MULT );
  PyModule_AddIntConstant ( m, "MULTU", PSX_MULTU );
  PyModule_AddIntConstant ( m, "NOR", PSX_NOR );
  PyModule_AddIntConstant ( m, "OR", PSX_OR );
  PyModule_AddIntConstant ( m, "ORI", PSX_ORI );
  PyModule_AddIntConstant ( m, "SB", PSX_SB );
  PyModule_AddIntConstant ( m, "SH", PSX_SH );
  PyModule_AddIntConstant ( m, "SLL", PSX_SLL );
  PyModule_AddIntConstant ( m, "SLLV", PSX_SLLV );
  PyModule_AddIntConstant ( m, "SLT", PSX_SLT );
  PyModule_AddIntConstant ( m, "SLTI", PSX_SLTI );
  PyModule_AddIntConstant ( m, "SLTIU", PSX_SLTIU );
  PyModule_AddIntConstant ( m, "SLTU", PSX_SLTU );
  PyModule_AddIntConstant ( m, "SRA", PSX_SRA );
  PyModule_AddIntConstant ( m, "SRAV", PSX_SRAV );
  PyModule_AddIntConstant ( m, "SRL", PSX_SRL );
  PyModule_AddIntConstant ( m, "SRLV", PSX_SRLV );
  PyModule_AddIntConstant ( m, "SUB", PSX_SUB );
  PyModule_AddIntConstant ( m, "SUBU", PSX_SUBU );
  PyModule_AddIntConstant ( m, "SW", PSX_SW );
  PyModule_AddIntConstant ( m, "SWC2", PSX_SWC2 );
  PyModule_AddIntConstant ( m, "SWL", PSX_SWL );
  PyModule_AddIntConstant ( m, "SWR", PSX_SWR );
  PyModule_AddIntConstant ( m, "SYSCALL", PSX_SYSCALL );
  PyModule_AddIntConstant ( m, "XOR", PSX_XOR );
  PyModule_AddIntConstant ( m, "XORI", PSX_XORI );

  /* Tipus d'operador. */
  PyModule_AddIntConstant ( m, "NONE", PSX_NONE );
  PyModule_AddIntConstant ( m, "RD", PSX_RD );
  PyModule_AddIntConstant ( m, "RS", PSX_RS );
  PyModule_AddIntConstant ( m, "RT", PSX_RT );
  PyModule_AddIntConstant ( m, "IMMEDIATE", PSX_IMMEDIATE );
  PyModule_AddIntConstant ( m, "OFFSET", PSX_OFFSET );
  PyModule_AddIntConstant ( m, "ADDR", PSX_ADDR );
  PyModule_AddIntConstant ( m, "OFFSET_BASE", PSX_OFFSET_BASE );
  PyModule_AddIntConstant ( m, "SA", PSX_SA );
  PyModule_AddIntConstant ( m, "COP2_SF", PSX_COP2_SF );
  PyModule_AddIntConstant ( m, "COP2_MX_V_CV", PSX_COP2_MX_V_CV );
  PyModule_AddIntConstant ( m, "COP2_LM", PSX_COP2_LM );
  PyModule_AddIntConstant ( m, "COP0_REG", PSX_COP0_REG );
  PyModule_AddIntConstant ( m, "COP2_REG", PSX_COP2_REG );
  PyModule_AddIntConstant ( m, "COP2_REG_CTRL", PSX_COP2_REG_CTRL );

  /* TIPUS D'ACCESSOS A MEMÒRIA. */
  PyModule_AddIntConstant ( m, "READ", PSX_READ );
  PyModule_AddIntConstant ( m, "WRITE", PSX_WRITE );

  // GPU Mnemonics.
  PyModule_AddIntConstant ( m, "GP0_POL3", PSX_GP0_POL3 );
  PyModule_AddIntConstant ( m, "GP0_POL4", PSX_GP0_POL4 );
  PyModule_AddIntConstant ( m, "GP0_LINE", PSX_GP0_LINE );
  PyModule_AddIntConstant ( m, "GP0_POLYLINE", PSX_GP0_POLYLINE );
  PyModule_AddIntConstant ( m, "GP0_POLYLINE_CONT", PSX_GP0_POLYLINE_CONT );
  PyModule_AddIntConstant ( m, "GP0_RECT", PSX_GP0_RECT );
  PyModule_AddIntConstant ( m, "GP0_SET_DRAW_MODE", PSX_GP0_SET_DRAW_MODE );
  PyModule_AddIntConstant ( m, "GP0_SET_TEXT_WIN", PSX_GP0_SET_TEXT_WIN );
  PyModule_AddIntConstant ( m, "GP0_SET_TOP_LEFT", PSX_GP0_SET_TOP_LEFT );
  PyModule_AddIntConstant ( m, "GP0_SET_BOTTOM_RIGHT",
        		    PSX_GP0_SET_BOTTOM_RIGHT );
  PyModule_AddIntConstant ( m, "GP0_SET_OFFSET", PSX_GP0_SET_OFFSET );
  PyModule_AddIntConstant ( m, "GP0_SET_MASK_BIT", PSX_GP0_SET_MASK_BIT );
  PyModule_AddIntConstant ( m, "GP0_CLEAR_CACHE", PSX_GP0_CLEAR_CACHE );
  PyModule_AddIntConstant ( m, "GP0_FILL", PSX_GP0_FILL );
  PyModule_AddIntConstant ( m, "GP0_COPY_VRAM2VRAM", PSX_GP0_COPY_VRAM2VRAM );
  PyModule_AddIntConstant ( m, "GP0_COPY_CPU2VRAM", PSX_GP0_COPY_CPU2VRAM );
  PyModule_AddIntConstant ( m, "GP0_COPY_VRAM2CPU", PSX_GP0_COPY_VRAM2CPU );
  PyModule_AddIntConstant ( m, "GP0_IRQ1", PSX_GP0_IRQ1 );
  PyModule_AddIntConstant ( m, "GP0_NOP", PSX_GP0_NOP );
  PyModule_AddIntConstant ( m, "GP0_UNK", PSX_GP0_UNK );
  
  PyModule_AddIntConstant ( m, "GP1_RESET", PSX_GP1_RESET );
  PyModule_AddIntConstant ( m, "GP1_RESET_BUFFER", PSX_GP1_RESET_BUFFER );
  PyModule_AddIntConstant ( m, "GP1_ACK", PSX_GP1_ACK );
  PyModule_AddIntConstant ( m, "GP1_ENABLE", PSX_GP1_ENABLE );
  PyModule_AddIntConstant ( m, "GP1_DATA_REQUEST", PSX_GP1_DATA_REQUEST );
  PyModule_AddIntConstant ( m, "GP1_START_DISP", PSX_GP1_START_DISP );
  PyModule_AddIntConstant ( m, "GP1_HOR_DISP_RANGE", PSX_GP1_HOR_DISP_RANGE );
  PyModule_AddIntConstant ( m, "GP1_VER_DISP_RANGE", PSX_GP1_VER_DISP_RANGE );
  PyModule_AddIntConstant ( m, "GP1_SET_DISP_MODE", PSX_GP1_SET_DISP_MODE );
  PyModule_AddIntConstant ( m, "GP1_TEXT_DISABLE", PSX_GP1_TEXT_DISABLE );
  PyModule_AddIntConstant ( m, "GP1_GET_INFO", PSX_GP1_GET_INFO );
  PyModule_AddIntConstant ( m, "GP1_OLD_TEXT_DISABLE",
        		    PSX_GP1_OLD_TEXT_DISABLE );
  PyModule_AddIntConstant ( m, "GP1_UNK", PSX_GP1_UNK );
  
  // Flags GPU commands.
  PyModule_AddIntConstant ( m, "GP_COLOR", PSX_GP_COLOR );
  PyModule_AddIntConstant ( m, "GP_TRANSPARENCY", PSX_GP_TRANSPARENCY );
  PyModule_AddIntConstant ( m, "GP_TEXT_BLEND", PSX_GP_TEXT_BLEND );
  PyModule_AddIntConstant ( m, "GP_V_COLOR", PSX_GP_V_COLOR );
  PyModule_AddIntConstant ( m, "GP_RAW_TEXT", PSX_GP_RAW_TEXT );

  // Mnemonics comandaments CD
  PyModule_AddIntConstant ( m, "CD_SYNC", PSX_CD_SYNC );
  PyModule_AddIntConstant ( m, "CD_SET_MODE", PSX_CD_SET_MODE );
  PyModule_AddIntConstant ( m, "CD_INIT", PSX_CD_INIT );
  PyModule_AddIntConstant ( m, "CD_RESET", PSX_CD_RESET );
  PyModule_AddIntConstant ( m, "CD_MOTOR_ON", PSX_CD_MOTOR_ON );
  PyModule_AddIntConstant ( m, "CD_STOP", PSX_CD_STOP );
  PyModule_AddIntConstant ( m, "CD_PAUSE", PSX_CD_PAUSE );
  PyModule_AddIntConstant ( m, "CD_SETLOC", PSX_CD_SETLOC );
  PyModule_AddIntConstant ( m, "CD_SEEKL", PSX_CD_SEEKL );
  PyModule_AddIntConstant ( m, "CD_SEEKP", PSX_CD_SEEKP );
  PyModule_AddIntConstant ( m, "CD_SET_SESSION", PSX_CD_SET_SESSION );
  PyModule_AddIntConstant ( m, "CD_READN", PSX_CD_READN );
  PyModule_AddIntConstant ( m, "CD_READS", PSX_CD_READS );
  PyModule_AddIntConstant ( m, "CD_READ_TOC", PSX_CD_READ_TOC );
  PyModule_AddIntConstant ( m, "CD_GET_STAT", PSX_CD_GET_STAT );
  PyModule_AddIntConstant ( m, "CD_GET_PARAM", PSX_CD_GET_PARAM );
  PyModule_AddIntConstant ( m, "CD_GET_LOC_L", PSX_CD_GET_LOC_L );
  PyModule_AddIntConstant ( m, "CD_GET_LOC_P", PSX_CD_GET_LOC_P );
  PyModule_AddIntConstant ( m, "CD_GET_TN", PSX_CD_GET_TN );
  PyModule_AddIntConstant ( m, "CD_GET_TD", PSX_CD_GET_TD );
  PyModule_AddIntConstant ( m, "CD_GET_Q", PSX_CD_GET_Q );
  PyModule_AddIntConstant ( m, "CD_GET_ID", PSX_CD_GET_ID );
  PyModule_AddIntConstant ( m, "CD_TEST", PSX_CD_TEST );
  PyModule_AddIntConstant ( m, "CD_MUTE", PSX_CD_MUTE );
  PyModule_AddIntConstant ( m, "CD_DEMUTE", PSX_CD_DEMUTE );
  PyModule_AddIntConstant ( m, "CD_PLAY", PSX_CD_PLAY );
  PyModule_AddIntConstant ( m, "CD_PLAY", PSX_CD_FORWARD );
  PyModule_AddIntConstant ( m, "CD_PLAY", PSX_CD_BACKWARD );
  PyModule_AddIntConstant ( m, "CD_SET_FILTER", PSX_CD_SET_FILTER );
  PyModule_AddIntConstant ( m, "CD_UNK", PSX_CD_UNK );

  // Debug flags.
  PyModule_AddIntConstant ( m, "DBG_MEM_CHANGED", DBG_MEM_CHANGED );
  PyModule_AddIntConstant ( m, "DBG_MEM_ACCESS", DBG_MEM_ACCESS );
  PyModule_AddIntConstant ( m, "DBG_MEM_ACCESS16", DBG_MEM_ACCESS16 );
  PyModule_AddIntConstant ( m, "DBG_MEM_ACCESS8", DBG_MEM_ACCESS8 );
  PyModule_AddIntConstant ( m, "DBG_CPU_INST", DBG_CPU_INST );
  PyModule_AddIntConstant ( m, "DBG_GPU_CMD_TRACE", DBG_GPU_CMD_TRACE );
  PyModule_AddIntConstant ( m, "DBG_CD_CMD_TRACE", DBG_CD_CMD_TRACE );
  PyModule_AddIntConstant ( m, "DBG_INT_TRACE", DBG_INT_TRACE );
  PyModule_AddIntConstant ( m, "DBG_SHOW_PC_CC", DBG_SHOW_PC_CC );
  PyModule_AddIntConstant ( m, "DBG_DMA_TRANSFER", DBG_DMA_TRANSFER );
  PyModule_AddIntConstant ( m, "DBG_GTE_MEM_ACCESS", DBG_GTE_MEM_ACCESS );
  PyModule_AddIntConstant ( m, "DBG_GTE_CMD_TRACE", DBG_GTE_CMD_TRACE );
  PyModule_AddIntConstant ( m, "DBG_BIOS_FUNC_TRACE", DBG_BIOS_FUNC_TRACE );

  // Botons
  PyModule_AddIntConstant ( m, "BUTTON_SELECT", PSX_BUTTON_SELECT );
  PyModule_AddIntConstant ( m, "BUTTON_START", PSX_BUTTON_START );
  PyModule_AddIntConstant ( m, "BUTTON_UP", PSX_BUTTON_UP );
  PyModule_AddIntConstant ( m, "BUTTON_RIGHT", PSX_BUTTON_RIGHT );
  PyModule_AddIntConstant ( m, "BUTTON_DOWN", PSX_BUTTON_DOWN );
  PyModule_AddIntConstant ( m, "BUTTON_LEFT", PSX_BUTTON_LEFT );
  PyModule_AddIntConstant ( m, "BUTTON_L2", PSX_BUTTON_L2 );
  PyModule_AddIntConstant ( m, "BUTTON_R2", PSX_BUTTON_R2 );
  PyModule_AddIntConstant ( m, "BUTTON_L1", PSX_BUTTON_L1 );
  PyModule_AddIntConstant ( m, "BUTTON_R1", PSX_BUTTON_R1 );
  PyModule_AddIntConstant ( m, "BUTTON_TRIANGLE", PSX_BUTTON_TRIANGLE );
  PyModule_AddIntConstant ( m, "BUTTON_CIRCLE", PSX_BUTTON_CIRCLE );
  PyModule_AddIntConstant ( m, "BUTTON_CROSS", PSX_BUTTON_CROSS );
  PyModule_AddIntConstant ( m, "BUTTON_SQUARE", PSX_BUTTON_SQUARE );
  
  return m;

} /* end PyInit_PSX */

/*
 * Copyright 2016-2022 Adrià Giménez Pastor.
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
 *  gte.c - Implementació del mòdul que simula el "Geometry
 *          Transformation Engine".
 *
 */
/*
 * NOTES:
 *
 * - Amb els registres de 16bits amb signe no tinc molt clar que fer
 *   quan es lligen. ¿Extendre el signe o no?
 *
 * - Hi han restriccions de temps al llegir, per exemple en IRGB:
 *   "After writing to IRGB, the result can be read from IR3 after TWO
 *   nop's, and from IR1,IR2 after THREE nop's (for uncached code, ONE
 *   nop would work)." Com és pot rotllo asumiré que es lligen quan
 *   toca, i no fare restriccions de temps.
 *
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "PSX.h"




/**********/
/* MACROS */
/**********/

#define I16TOU32(R) ((uint32_t) ((uint16_t) (R)))
#define PACKR16(RH,RL) (((I16TOU32(RH))<<16) | (I16TOU32(RL)))

#define S16_U16(S16) ((uint16_t) (S16))
#define S16_U32(S16) ((uint32_t) S16_U16(S16))
#define S16_S32(S16) ((int32_t) (S16))
#define S16_S64(S16) ((int64_t) (S16))
#define U16_U64(U16) ((uint64_t) (U16))
#define U16_S64(U16) ((int64_t) U16_U64(U16))
#define EXT_S16_U32(S16) ((uint32_t) S16_S32(S16))
#define S32_U32(S32) ((uint32_t) (S32))
#define S32_S16(S32) ((int16_t) (S32))
#define S32_S64(S32) ((int64_t) (S32))
#define S32_U16(S32) ((uint16_t) S32_S16(S32))
#define U32_U64(U32) ((uint64_t) (U32))
#define U32_S64(U32) ((int64_t) U32_U64(U32))
#define S64_S32(S64) ((int32_t) (S64))

#define GET_SF(DATA) (((DATA)&0x00080000)!=0 ? 12 : 0)
#define GET_MX(DATA) (((DATA)>>17)&0x3)
#define GET_VX(DATA) (((DATA)>>15)&0x3)
#define GET_TX(DATA) (((DATA)>>13)&0x3)
#define CHECK_LM_IS_0(DATA) (((DATA)&0x00000400)==0)

#define VX0 _regs.vx0
#define VY0 _regs.vy0
#define VZ0 _regs.vz0

#define VX1 _regs.vx1
#define VY1 _regs.vy1
#define VZ1 _regs.vz1

#define VX2 _regs.vx2
#define VY2 _regs.vy2
#define VZ2 _regs.vz2

#define IR1 _regs.ir1
#define IR2 _regs.ir2
#define IR3 _regs.ir3

#define RT11 _regs.rt11
#define RT12 _regs.rt12
#define RT13 _regs.rt13
#define RT21 _regs.rt21
#define RT22 _regs.rt22
#define RT23 _regs.rt23
#define RT31 _regs.rt31
#define RT32 _regs.rt32
#define RT33 _regs.rt33

#define D1 RT11
#define D2 RT22
#define D3 RT33

#define L11 _regs.l11
#define L12 _regs.l12
#define L13 _regs.l13
#define L21 _regs.l21
#define L22 _regs.l22
#define L23 _regs.l23
#define L31 _regs.l31
#define L32 _regs.l32
#define L33 _regs.l33

#define LR1 _regs.lr1
#define LR2 _regs.lr2
#define LR3 _regs.lr3
#define LG1 _regs.lg1
#define LG2 _regs.lg2
#define LG3 _regs.lg3
#define LB1 _regs.lb1
#define LB2 _regs.lb2
#define LB3 _regs.lb3

#define TRX _regs.trx
#define TRY _regs.try
#define TRZ _regs.trz

#define RBK _regs.rbk
#define GBK _regs.gbk
#define BBK _regs.bbk

#define RFC _regs.rfc
#define GFC _regs.gfc
#define BFC _regs.bfc

#define OFX _regs.ofx
#define OFY _regs.ofy
#define H _regs.h
#define DQA _regs.dqa
#define DQB _regs.dqb

#define SX0 _regs.sx0
#define SY0 _regs.sy0
#define SX1 _regs.sx1
#define SY1 _regs.sy1
#define SX2 _regs.sx2
#define SY2 _regs.sy2
#define SZ0 _regs.sz0
#define SZ1 _regs.sz1
#define SZ2 _regs.sz2
#define SZ3 _regs.sz3

#define MAC0 _regs.mac0
#define MAC1 _regs.mac1
#define MAC2 _regs.mac2
#define MAC3 _regs.mac3

#define FLAG _regs.flag

/* NOTA: 31 -> Error Flag (Bit30..23, and 18..13 ORed together) (Read only) */
#define F_A1_POS 0xC0000000 /* MAC1 Result larger than 43 bits and positive */
#define F_A2_POS 0xA0000000 /* MAC2 Result larger than 43 bits and positive */
#define F_A3_POS 0x90000000 /* MAC3 Result larger than 43 bits and positive */
#define F_A1_NEG 0x88000000 /* MAC1 Result larger than 43 bits and negative */
#define F_A2_NEG 0x84000000 /* MAC2 Result larger than 43 bits and negative */
#define F_A3_NEG 0x82000000 /* MAC3 Result larger than 43 bits and negative */
#define F_B1     0x81000000 /* IR1 saturated */
#define F_B2     0x80800000 /* IR2 saturated */
#define F_B3     0x00400000 /* IR3 saturated */
#define F_C1     0x00200000 /* Color-FIFO-R saturated */
#define F_C2     0x00100000 /* Color-FIFO-G saturated */
#define F_C3     0x00080000 /* Color-FIFO-B saturated */
#define F_D      0x80040000 /* SZ3 or OTZ saturated */
#define F_E      0x80020000 /* Divide overflow */
#define F_F_POS  0x80010000 /* MAC0 Result larger than 31 bits and positive */
#define F_F_NEG  0x80008000 /* MAC0 Result larger than 31 bits and negative */
#define F_G1     0x80004000 /* SX2 saturated */
#define F_G2     0x80002000 /* SY2 saturated */
#define F_H      0x00001000 /* IR0 saturated */

#define IR0 _regs.ir0

#define ZSF3 _regs.zsf3
#define ZSF4 _regs.zsf4
#define OTZ _regs.otz

#define LZCS _regs.lzcs
#define LZCR _regs.lzcr

#define RGBC _regs.rgbc
#define RGB0 _regs.rgb0
#define RGB1 _regs.rgb1
#define RGB2 _regs.rgb2
#define RES1 _regs.res1

#define GET_R_REG(REG) ((REG)&0x000000FF)
#define GET_G_REG(REG) (((REG)&0x0000FF00)>>8)
#define GET_B_REG(REG) (((REG)&0x00FF0000)>>16)
#define GET_R GET_R_REG ( RGBC )
#define GET_G GET_G_REG ( RGBC )
#define GET_B GET_B_REG ( RGBC )




/*********/
/* ESTAT */
/*********/

/* Callbacks. */
static PSX_Warning *_warning;
static PSX_GTECmdTrace *_cmd_trace;
static PSX_GTEMemAccess *_mem_access;
static void *_udata;

// Traça
static int (*_read) (const int nreg,uint32_t *dst);
static void (*_write) (const int nreg,const uint32_t data);
static void (*_execute) (const uint32_t cmd);

// Cicles pendents de processar i ja consumits de l'actual
// iteració. El motiu és que quan una instrucció encara està pendent
// no es pot fer una altra cosa.
static int _cc;
static int _cc_used;

/* Registres. */
static struct
{

  /* 16bit Vectors (R/W) (1,15,0) o (1,3,12). */
  int16_t vx0;
  int16_t vy0;
  int16_t vz0;
  
  int16_t vx1;
  int16_t vy1;
  int16_t vz1;
  
  int16_t vx2;
  int16_t vy2;
  int16_t vz2;

  int16_t ir1;
  int16_t ir2;
  int16_t ir3;

  /* Rotation matrix (RT) (1,3,12). */
  int16_t rt11;
  int16_t rt12;
  int16_t rt13;
  int16_t rt21;
  int16_t rt22;
  int16_t rt23;
  int16_t rt31;
  int16_t rt32;
  int16_t rt33;

  /* Light matrix (LM) (1,3,12). */
  int16_t l11;
  int16_t l12;
  int16_t l13;
  int16_t l21;
  int16_t l22;
  int16_t l23;
  int16_t l31;
  int16_t l32;
  int16_t l33;

  /* Light Color matrix (LCM) (1,3,12). */
  int16_t lr1;
  int16_t lr2;
  int16_t lr3;
  int16_t lg1;
  int16_t lg2;
  int16_t lg3;
  int16_t lb1;
  int16_t lb2;
  int16_t lb3;

  /* Translation Vector (TR) (R/W) (1,31,0). */
  int32_t trx;
  int32_t try;
  int32_t trz;

  /* Background Color (BK) (R/W?) (1,19,12). */
  int32_t rbk;
  int32_t gbk;
  int32_t bbk;

  /* Far Color (FC) (R/W?) (1,27,4). */
  int32_t rfc;
  int32_t gfc;
  int32_t bfc;
  
  /* Screen Offset and Distance (R/W) */
  /*  -> (1,15,16). */
  int32_t ofx;
  int32_t ofy;
  /*  -> (0,16,0). */
  uint16_t h;
  /*  -> (1,7,8). */
  int16_t dqa;
  /*  -> (1,7,24). */
  int32_t dqb;

  /* Screen XYZ Coordinate FIFOs. */
  /*  -> (1,15,0). */
  int16_t sx0;
  int16_t sy0;
  int16_t sx1;
  int16_t sy1;
  int16_t sx2;
  int16_t sy2;
  /*  -> (0,16,0). */
  uint16_t sz0;
  uint16_t sz1;
  uint16_t sz2;
  uint16_t sz3;

  /* Accumulators (R/W) (1,31,0). */
  int32_t mac0;
  int32_t mac1;
  int32_t mac2;
  int32_t mac3;

  /* Returns any calculation errors. */
  uint32_t flag;

  /* Interpolation Factor (R/W) (1,3,12). */
  int16_t ir0;

  /* Average Z Registers (R/W). */
  /*  -> (1,3,12). */
  int16_t zsf3;
  int16_t zsf4;
  /*  -> (0,15,0). */
  uint16_t otz;

  /* Count-Leading-Zeroes/Leading-Ones */
  int32_t lzcs;
  uint32_t lzcr;

  /* Color Register and Color FIFO (RW) (CODE,B,G,R). */
  uint32_t rgbc;
  uint32_t rgb0;
  uint32_t rgb1;
  uint32_t rgb2;
  uint32_t res1; /* RES1 seems to be unused... looks like an unused
        	    Fifo stage... RES1 is read/write-able... unlike
        	    SXYP (for SXYn Fifo) it does not mirror to RGB2,
        	    nor does it have a move-on-write function... */

} _regs;




/*************/
/* CONSTANTS */
/*************/

static const int64_t INT43_MAX= ((int64_t)1<<43);
static const int64_t INT43_MIN= -((int64_t)1<<43);
static const int64_t INT31_MAX= ((int64_t)1<<31)-(int64_t)1;
static const int64_t INT31_MIN= -((int64_t)1<<31);

static const uint8_t UNR_TABLE[0x101]=
  {
   0xFF,0xFD,0xFB,0xF9,0xF7,0xF5,0xF3,0xF1,
   0xEF,0xEE,0xEC,0xEA,0xE8,0xE6,0xE4,0xE3,
   0xE1,0xDF,0xDD,0xDC,0xDA,0xD8,0xD6,0xD5,
   0xD3,0xD1,0xD0,0xCE,0xCD,0xCB,0xC9,0xC8,
   0xC6,0xC5,0xC3,0xC1,0xC0,0xBE,0xBD,0xBB,
   0xBA,0xB8,0xB7,0xB5,0xB4,0xB2,0xB1,0xB0,
   0xAE,0xAD,0xAB,0xAA,0xA9,0xA7,0xA6,0xA4,
   0xA3,0xA2,0xA0,0x9F,0x9E,0x9C,0x9B,0x9A,
   0x99,0x97,0x96,0x95,0x94,0x92,0x91,0x90,
   0x8F,0x8D,0x8C,0x8B,0x8A,0x89,0x87,0x86,
   0x85,0x84,0x83,0x82,0x81,0x7F,0x7E,0x7D,
   0x7C,0x7B,0x7A,0x79,0x78,0x77,0x75,0x74,
   0x73,0x72,0x71,0x70,0x6F,0x6E,0x6D,0x6C,
   0x6B,0x6A,0x69,0x68,0x67,0x66,0x65,0x64,
   0x63,0x62,0x61,0x60,0x5F,0x5E,0x5D,0x5D,
   0x5C,0x5B,0x5A,0x59,0x58,0x57,0x56,0x55,
   0x54,0x53,0x53,0x52,0x51,0x50,0x4F,0x4E,
   0x4D,0x4D,0x4C,0x4B,0x4A,0x49,0x48,0x48,
   0x47,0x46,0x45,0x44,0x43,0x43,0x42,0x41,
   0x40,0x3F,0x3F,0x3E,0x3D,0x3C,0x3C,0x3B,
   0x3A,0x39,0x39,0x38,0x37,0x36,0x36,0x35,
   0x34,0x33,0x33,0x32,0x31,0x31,0x30,0x2F,
   0x2E,0x2E,0x2D,0x2C,0x2C,0x2B,0x2A,0x2A,
   0x29,0x28,0x28,0x27,0x26,0x26,0x25,0x24,
   0x24,0x23,0x22,0x22,0x21,0x20,0x20,0x1F,
   0x1E,0x1E,0x1D,0x1D,0x1C,0x1B,0x1B,0x1A,
   0x19,0x19,0x18,0x18,0x17,0x16,0x16,0x15,
   0x15,0x14,0x14,0x13,0x12,0x12,0x11,0x11,
   0x10,0x0F,0x0F,0x0E,0x0E,0x0D,0x0D,0x0C,
   0x0C,0x0B,0x0A,0x0A,0x09,0x09,0x08,0x08,
   0x07,0x07,0x06,0x06,0x05,0x05,0x04,0x04,
   0x03,0x03,0x02,0x02,0x01,0x01,0x00,0x00,
   0x00
  };




/*********************/
/* FUNCIONS PRIVADES */
/*********************/

static int
clz (
     uint32_t a
     )
{
  
  int ret;

  
  if ( a == 0 ) return 32;
  
  ret= 0;
  while ( ((int32_t) a) > 0 )
    {
      ++ret;
      a<<= 1;
    }
  
  return ret;
  
} /* end clz */


#define SET_IR_TMP32(I,TMP)        					\
  if ( lm_0 )        							\
    {        								\
      if ( (TMP) > 0x7FFF )       { FLAG|= F_B ## I; IR ## I= 0x7FFF; }        \
      else if ( (TMP) < -0x8000 ) { FLAG|= F_B ## I; IR ## I= -0x8000; } \
      else IR ## I= S32_S16 ( TMP );        				\
    }        								\
  else        								\
    {        								\
      if ( (TMP) > 0x7FFF ) { FLAG|= F_B ## I; IR ## I= 0x7FFF; }        \
      else if ( (TMP) < 0 ) { FLAG|= F_B ## I; IR ## I= 0; }        	\
      else IR ## I= S32_S16 ( TMP );        				\
    }

#define SET_IR(I) SET_IR_TMP32 ( I, MAC ## I )

#define SET_MAC0_TMP64(TMP)        		 \
  if      ( (TMP) > INT31_MAX ) FLAG|= F_F_POS;  \
  else if ( (TMP) < INT31_MIN ) FLAG|= F_F_NEG;         \
  MAC0= S64_S32 ( (TMP) )

#define SET_MAC0_TMP64_SHIFT(TMP)        		 \
  if      ( (TMP) > INT31_MAX ) FLAG|= F_F_POS;          \
  else if ( (TMP) < INT31_MIN ) FLAG|= F_F_NEG;          \
  (TMP)>>= 16;                                           \
  MAC0= S64_S32 ( (TMP) )

#define SET_SZ3OTZ_TMP32(TO,TMP)        			\
  if ( (TMP) > 0xFFFF )      { FLAG|= F_D; (TO)= 0xFFFF; }        \
  else if ( (TMP) < 0x0000 ) { FLAG|= F_D; (TO)= 0x0000; }        \
  else (TO)= S32_U16 ( (TMP) )

#define SET_MAC1_TMP64(TMP)        			\
  if ( (TMP) > INT43_MAX )      FLAG|= F_A1_POS;        \
  else if ( (TMP) < INT43_MIN ) FLAG|= F_A1_NEG;        \
  MAC1= S64_S32 ( (TMP) )

#define SET_MAC2_TMP64(TMP)        		  \
  if ( (TMP) > INT43_MAX )      FLAG|= F_A2_POS;  \
  else if ( (TMP) < INT43_MIN ) FLAG|= F_A2_NEG;  \
  MAC2= S64_S32 ( (TMP) )

#define SET_MAC3_TMP64(TMP)        		  \
  if ( (TMP) > INT43_MAX )      FLAG|= F_A3_POS;  \
  else if ( (TMP) < INT43_MIN ) FLAG|= F_A3_NEG;  \
  MAC3= S64_S32 ( (TMP) )

static int
count_leading_zeroes_i16 (
                          int64_t val
                          )
{

  int i;


  if ( val == 0 ) return 15;
  for ( i= 0; i < 16; ++i )
    {
      if ( val&0x8000 ) break;
      val<<= 1;
    }

  return i;
  
} // count_leading_zeroes


static int64_t
calc_div (
          const int64_t num,
          const int64_t den
          )
{

  int64_t ret,n,d,u;
  int z;
  
  
  if ( num < den*2 )
    {
      z= count_leading_zeroes_i16 ( den );
      n= num<<z; d= den<<z;
      assert ( d >= 0x8000 );
      u= ((int64_t) UNR_TABLE[(d-0x7FC0)>>7]) + 0x101;
      d= (0x2000080 - (d*u))>>8;
      d= (0x0000080 + (d*u))>>8;
      ret= ((n*d) + 0x8000)>>16;
      if ( ret > 0x1FFFF ) ret= 0x1FFFF;
    }
  else { ret= 0x1FFFF; FLAG|= F_E; }

  return ret;
  
} // end calc_div


static void
rtp_body (
          const int16_t vx,
          const int16_t vy,
          const int16_t vz,
          const int     sf,
          const bool    calc_ir0
          )
{

  int64_t tmp,div;
  int32_t tmp_mac3,tmp_mac0;

  
  /* Note: The command does saturate IR1,IR2,IR3 to -8000h..+7FFFh
     (regardless of lm bit). */
  static const bool lm_0= true;
  
  
  /* NOTA: Les operacions no són random, s'està tenint en compter quin
     tipus de representació empra cada registre i com es supossa que
     es fan les operacions. */
  /* [1,31,0] MAC1=A1[TRX + R11*VX0 + R12*VY0 + R13*VZ0] [1,31,12] */
  tmp=
    ((S16_S64(TRX)<<12) +
     S16_S64(RT11)*S16_S64(vx) +
     S16_S64(RT12)*S16_S64(vy) +
     S16_S64(RT13)*S16_S64(vz)) >> sf;
  SET_MAC1_TMP64 ( tmp );
  
  /* [1,31,0] MAC2=A2[TRY + R21*VX0 + R22*VY0 + R23*VZ0] [1,31,12] */
  tmp=
    ((S16_S64(TRY)<<12) +
     S16_S64(RT21)*S16_S64(vx) +
     S16_S64(RT22)*S16_S64(vy) +
     S16_S64(RT23)*S16_S64(vz)) >> sf;
  SET_MAC2_TMP64 ( tmp );
  
  /* [1,31,0] MAC3=A3[TRZ + R31*VX0 + R32*VY0 + R33*VZ0] [1,31,12] */
  tmp=
    ((S16_S64(TRZ)<<12) +
     S16_S64(RT31)*S16_S64(vx) +
     S16_S64(RT32)*S16_S64(vy) +
     S16_S64(RT33)*S16_S64(vz)) >> sf;
  SET_MAC3_TMP64 ( tmp );
  
  /* [1,15,0] IR1= Lm_B1[MAC1] [1,31,0] */
  /* [1,15,0] IR2= Lm_B2[MAC2] [1,31,0] */
  /* [1,15,0] IR3= Lm_B3[MAC3] [1,31,0] */
  SET_IR ( 1 );
  SET_IR ( 2 );
  /* Notes: When using RTP with sf=0, then the IR3 saturation flag
     (FLAG.22) gets set <only> if "MAC3 SAR 12" exceeds -8000h..+7FFFh
     (although IR3 is saturated when "MAC3" exceeds
     -8000h..+7FFFh). */
  if ( sf == 0 )
    {
      if ( MAC3 > 0x7FFF )       IR3= 0x7FFF;
      else if ( MAC3 < -0x8000 ) IR3= -0x8000;
      else                       IR3= S32_S16 ( MAC3 );
      tmp_mac3= MAC3>>12;
      if ( tmp_mac3 < 0x8000 || tmp_mac3 > 0x7FFF ) FLAG|= F_B3;
    }
  else { SET_IR ( 3 ); tmp_mac3= MAC3; }
  
  /* SZ0<-SZ1<-SZ2<-SZ3 */
  SZ0= SZ1; SZ1= SZ2; SZ2= SZ3;
  
  /* [0,16,0] SZ3= Lm_D(MAC3) [1,31,0] */
  SET_SZ3OTZ_TMP32 ( SZ3, tmp_mac3 );
  
  /* SX0<-SX1<-SX2, SY0<-SY1<-SY2 */
  SX0= SX1; SX1= SX2;
  SY0= SY1; SY1= SY2;
  
  /* Division for next instructions. */
  div= calc_div ( U16_S64(H), U16_S64(SZ3) );
  /*
  if ( SZ3 == 0 ) { FLAG|= F_E; div= 0x1FFFF;}
  else
    {
      div= (((U16_S64 ( H )<<17) / U16_S64 ( SZ3 ))+1)>>1;
      if ( div > 0x1FFFF ) { FLAG|= F_E; div= 0x1FFFF; }
    }
  */
  
  /* IR1 seria ací (1,15,0). */
  /* El valor (..,..,16) es guarda tal cual en MAC0??? Aparentment sí. */
  /* MAC0=(((H*20000h/SZ3)+1)/2)*IR1+OFX, SX2=MAC0/10000h */
  /* [1,15,0] SX2= Lm_G1[F[OFX + IR1*(H/SZ)]] [1,27,16] */
  tmp= div*S16_S64 ( IR1 ) + S32_S64 ( OFX );
  SET_MAC0_TMP64_SHIFT ( tmp );
  if ( MAC0 < -0x400 )     { FLAG|= F_G1; SX2= -0x400; }
  else if ( MAC0 > 0x3FF ) { FLAG|= F_G1; SX2= 0x3FF; }
  else SX2= S32_S16 ( MAC0 );
  
  /* MAC0=(((H*20000h/SZ3)+1)/2)*IR2+OFY, SY2=MAC0/10000h */
  /* [1,15,0] SY2= Lm_G2[F[OFY + IR2*(H/SZ)]] [1,27,16] */
  tmp= (div*S16_S64 ( IR2 ) + S32_S64 ( OFY ));
  SET_MAC0_TMP64_SHIFT ( tmp );
  if ( MAC0 < -0x400 )     { FLAG|= F_G2; SY2= -0x400; }
  else if ( MAC0 > 0x3FF ) { FLAG|= F_G2; SY2= 0x3FF; }
  else SY2= S32_S16 ( MAC0 );
  
  if ( calc_ir0 )
    {
      /* Ací els càlculs es fan en 2^24. Entenc per tant que el valor 2^24
         es guarda tal cual en MAC0, i després es pasa a 2^12, és a dir
         IR0 és (1,3,12). */
      /* MAC0=(((H*20000h/SZ3)+1)/2)*DQA+DQB, IR0=MAC0/1000h */
      /* [1,31,0] MAC0= F[DQB + DQA * (H/SZ)] [1,19,24] */
      /* [1,15,0] IR0= Lm_H[MAC0] [1,31,0] */
      tmp= div*S16_S64 ( DQA ) + S32_S64 ( DQB );
      SET_MAC0_TMP64 ( tmp );
      tmp_mac0= tmp>>12;
      if ( tmp_mac0 < 0 )           { FLAG|= F_H; IR0= 0; }
      else if ( tmp_mac0 > 0x1000 ) { FLAG|= F_H; IR0= 0x1000; }
      else IR0= S32_S16 ( tmp_mac0 );
    }
  
} /* end rtp_body */


/* Perspective Transformation single. */
static void
rtps (
      const uint32_t cmd
      )
{

  int sf;
  
  
  /* Reset flags and cycles. */
  FLAG= 0;
  _cc= 15;

  /* Execute. */
  sf= GET_SF ( cmd );
  rtp_body ( VX0, VY0, VZ0, sf, true );
  
} /* end rtps */


// Perspective Transformation triple.
static void
rtpt (
      const uint32_t cmd
      )
{

  int sf;
  
  
  // Reset flags and cycles.
  FLAG= 0;
  _cc= 23;
  
  // Execute.
  sf= GET_SF ( cmd );
  rtp_body ( VX0, VY0, VZ0, sf, false );
  rtp_body ( VX1, VY1, VZ1, sf, false );
  rtp_body ( VX2, VY2, VZ2, sf, true );
  
} /* end rtpt */


/* Normal clipping. */
static void
nclip (void)
{

  int64_t tmp;

  
  /* Reset flags and cycles. */
  FLAG= 0;
  _cc= 8;

  /* MAC0 = SX0*SY1 + SX1*SY2 + SX2*SY0 - SX0*SY2 - SX1*SY0 - SX2*SY1 */
  /* [1,31,0] MAC0 =
     F[SX0*SY1+SX1*SY2+SX2*SY0-SX0*SY2-SX1*SY0-SX2*SY1] [1,43,0] */
  tmp=
    S16_S64(SX0)*S16_S64(SY1) + S16_S64(SX1)*S16_S64(SY2) +
    S16_S64(SX2)*S16_S64(SY0) - S16_S64(SX0)*S16_S64(SY2) -
    S16_S64(SX1)*S16_S64(SY0) - S16_S64(SX2)*S16_S64(SY1);
  SET_MAC0_TMP64 ( tmp );
  
} /* end nclip */


/* Average of three Z values (for Triangles). */
static void
avsz3 (void)
{

  int64_t tmp;
  int32_t tmp2;

  
  /* Reset flags and cycles. */
  FLAG= 0;
  _cc= 5;
  
  /* MAC0 =  ZSF3*(SZ1+SZ2+SZ3) */
  /* [1,31,0] MAC0=F[ZSF3*SZ1 + ZSF3*SZ2 + ZSF3*SZ3] [1,31,12] */
  tmp=
    S16_S64(ZSF3)*U16_S64(SZ1) +
    S16_S64(ZSF3)*U16_S64(SZ2) +
    S16_S64(ZSF3)*U16_S64(SZ3);
  SET_MAC0_TMP64 ( tmp );

  /* OTZ  =  MAC0/1000h ;for both (saturated to 0..FFFFh) */
  /* [0,16,0] OTZ=Lm_D[MAC0] [1,31,0] */
  tmp2= MAC0>>12;
  SET_SZ3OTZ_TMP32 ( OTZ, tmp2 );
  
} /* end avsz3 */


/* Average of four Z values (for Quads). */
static void
avsz4 (void)
{

  int64_t tmp;
  int32_t tmp2;


  /* Reset flags and cycles. */
  FLAG= 0;
  //_cc= 6;
  _cc= 5; // Mednafen diu 5, cosa rara perquè tardaria el mateix que
          // AVSZ3
  
  /* MAC0 =  ZSF4*(SZ0+SZ1+SZ2+SZ3) */
  /* [1,31,0] MAC0=F[ZSF4*SZ0 + ZSF4*SZ1 + ZSF4*SZ2 + ZSF4*SZ3] [1,31,12] */
  tmp=
    S16_S64(ZSF4)*U16_S64(SZ0) +
    S16_S64(ZSF4)*U16_S64(SZ1) +
    S16_S64(ZSF4)*U16_S64(SZ2) +
    S16_S64(ZSF4)*U16_S64(SZ3);
  SET_MAC0_TMP64 ( tmp );

  /* OTZ  =  MAC0/1000h ;for both (saturated to 0..FFFFh) */
  /* [0,16,0] OTZ=Lm_D[MAC0] [1,31,0] */
  tmp2= MAC0>>12;
  SET_SZ3OTZ_TMP32 ( OTZ, tmp2 );
  
} /* end avsz4 */


/* Multiply vector by matrix and vector addition. */
static void
mvmva (
       const uint32_t cmd
       )
{

  int64_t tx1,tx2,tx3;
  int64_t mx11,mx12,mx13,mx21,mx22,mx23,mx31,mx32,mx33;
  int64_t vx1,vx2,vx3;
  int64_t tmp;
  bool bugged,lm_0;
  int sf;

  
  /* Reset flags and cycles. */
  FLAG= 0;
  _cc= 8;

  /* Select Mx */
  switch ( GET_MX ( cmd ) )
    {
    case 0: /* Rotation. */
      mx11= S16_S64(RT11); mx12= S16_S64(RT12); mx13= S16_S64(RT13);
      mx21= S16_S64(RT21); mx22= S16_S64(RT22); mx23= S16_S64(RT23);
      mx31= S16_S64(RT31); mx32= S16_S64(RT32); mx33= S16_S64(RT33);
      break;
    case 1: /* Light. */
      mx11= S16_S64(L11); mx12= S16_S64(L12); mx13= S16_S64(L13);
      mx21= S16_S64(L21); mx22= S16_S64(L22); mx23= S16_S64(L23);
      mx31= S16_S64(L31); mx32= S16_S64(L32); mx33= S16_S64(L33);
      break;
    case 2: /* Color. */
      mx11= S16_S64(LR1); mx12= S16_S64(LR2); mx13= S16_S64(LR3);
      mx21= S16_S64(LG1); mx22= S16_S64(LG2); mx23= S16_S64(LG3);
      mx31= S16_S64(LB1); mx32= S16_S64(LB2); mx33= S16_S64(LB3);
      break;
    case 3: /* Reserved (Garbage). */
      mx11= -0x60;         mx12= 0x60;          mx13= S16_S64(IR0);
      mx21= S16_S64(RT13); mx22= S16_S64(RT13); mx23= S16_S64(RT13);
      mx31= S16_S64(RT22); mx32= S16_S64(RT22); mx33= S16_S64(RT22);
      break;
    }

  /* Select Vx. */
  switch ( GET_VX ( cmd ) )
    {
    case 0: /* V0. */
      vx1= S16_S64(VX0); vx2= S16_S64(VY0); vx3= S16_S64(VZ0);
      break;
    case 1: /* V1. */
      vx1= S16_S64(VX1); vx2= S16_S64(VY1); vx3= S16_S64(VZ1);
      break;
    case 2: /* V2. */
      vx1= S16_S64(VX2); vx2= S16_S64(VY2); vx3= S16_S64(VZ2);
      break;
    case 3: /* IR. */
      vx1= S16_S64(IR1); vx2= S16_S64(IR2); vx3= S16_S64(IR3);
      break;
    }
  
  /* Select Tx. */
  switch ( GET_TX ( cmd ) )
    {
    case 0: /* TR. */
      bugged= false;
      tx1= S32_S64(TRX); tx2= S32_S64(TRY); tx3= S32_S64(TRZ);
      break;
    case 1: /* BK. */
      bugged= false;
      tx1= S32_S64(RBK); tx2= S32_S64(GBK); tx3= S32_S64(BBK);
      break;
    case 2: /* FC/Bugged. */
      bugged= true;
      tx1= S32_S64(RFC); tx2= S32_S64(GFC); tx3= S32_S64(BFC);
      break;
    case 3: /* None. */
      bugged= false;
      tx1= 0; tx2= 0; tx3= 0;
      break;
    }

  /* Get sf and lm. */
  sf= GET_SF ( cmd );
  lm_0= CHECK_LM_IS_0 ( cmd );

  /* NOTA: Aparentment tots els vectors Tx es tracten com si foren
     (1,31,0) !!! Encara que sols TR ho és. */
  /*
   * MAC1 = (Tx1*1000h + Mx11*Vx1 + Mx12*Vx2 + Mx13*Vx3) SAR (sf*12)
   * MAC2 = (Tx2*1000h + Mx21*Vx1 + Mx22*Vx2 + Mx23*Vx3) SAR (sf*12)
   * MAC3 = (Tx3*1000h + Mx31*Vx1 + Mx32*Vx2 + Mx33*Vx3) SAR (sf*12)
   *
   * MAC1=A1[CV1 + MX11*V1 + MX12*V2 + MX13*V3]
   * MAC2=A2[CV2 + MX21*V1 + MX22*V2 + MX23*V3]
   * MAC3=A3[CV3 + MX31*V1 + MX32*V2 + MX33*V3]
   */
  if ( bugged )
    {
      tmp= ((tx1<<12) + mx13*vx3)>>sf;
      SET_MAC1_TMP64 ( tmp );
      tmp= ((tx2<<12) + mx23*vx3)>>sf;
      SET_MAC2_TMP64 ( tmp );
      tmp= ((tx3<<12) + mx33*vx3)>>sf;
      SET_MAC3_TMP64 ( tmp );
    }
  else
    {
      tmp= ((tx1<<12) + mx11*vx1 + mx12*vx2 + mx13*vx3)>>sf;
      SET_MAC1_TMP64 ( tmp );
      tmp= ((tx2<<12) + mx21*vx1 + mx22*vx2 + mx23*vx3)>>sf;
      SET_MAC2_TMP64 ( tmp );
      tmp= ((tx3<<12) + mx31*vx1 + mx32*vx2 + mx33*vx3)>>sf;
      SET_MAC3_TMP64 ( tmp );
    }

  /* [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] */
  /* IR1=Lm_B1[MAC1] */
  /* IR2=Lm_B2[MAC2] */
  /* IR3=Lm_B3[MAC3] */
  SET_IR ( 1 );
  SET_IR ( 2 );
  SET_IR ( 3 );
  
} /* end mvmva */


/* Square vector. */
static void
sqr (
     const uint32_t cmd
     )
{
  
  const static bool lm_0= false;
  
  int sf;
  int64_t tmp;

  
  /* Reset flags and cycles. */
  FLAG= 0;
  _cc= 5;
  
  /* Get sf. */
  sf= GET_SF ( cmd );

  /* [MAC1,MAC2,MAC3] = [IR1*IR1,IR2*IR2,IR3*IR3] SHR (sf*12) */
  /* 
   * [1,31,0][1,19,12] MAC1=A1[IR1*IR1] [1,43,0][1,31,12]
   * [1,31,0][1,19,12] MAC2=A2[IR2*IR2] [1,43,0][1,31,12]
   * [1,31,0][1,19,12] MAC3=A3[IR3*IR3] [1,43,0][1,31,12]
   */
  tmp= (S16_S64(IR1)*S16_S64(IR1))>>sf; SET_MAC1_TMP64 ( tmp );
  tmp= (S16_S64(IR2)*S16_S64(IR2))>>sf; SET_MAC2_TMP64 ( tmp );
  tmp= (S16_S64(IR3)*S16_S64(IR3))>>sf; SET_MAC3_TMP64 ( tmp );

  /* [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] */
  /* 
   * [1,15,0][1,3,12]  IR1=Lm_B1[MAC1] [1,31,0][1,19,12][lm=1]
   * [1,15,0][1,3,12]  IR2=Lm_B2[MAC2] [1,31,0][1,19,12][lm=1]
   * [1,15,0][1,3,12]  IR3=Lm_B3[MAC3] [1,31,0][1,19,12][lm=1]
   */
  SET_IR ( 1 );
  SET_IR ( 2 );
  SET_IR ( 3 );
  
} /* end sqr */


/* Outer product of 2 vectors. */
static void
op (
    const uint32_t cmd
    )
{

  int64_t tmp;
  int sf;
  bool lm_0;
  
  
  /* Reset flags and cycles. */
  FLAG= 0;
  _cc= 6;

  /* Get sf and lm_0. */
  sf= GET_SF ( cmd );
  lm_0= CHECK_LM_IS_0 ( cmd );

  /* [MAC1,MAC2,MAC3] = [IR3*D2-IR2*D3, IR1*D3-IR3*D1, IR2*D1-IR1*D2]
     SAR (sf*12) */
  /* Note: D1,D2,D3 are meant to be the RT11,RT22,RT33 elements of the
     RT matrix "misused" as vector. */
  /*
   * Calculation: (D1=R11R12,D2=R22R23,D3=R33) <-- Diferent? 
   *                                               Faig cas al d'abans
   * MAC1=A1[D2*IR3 - D3*IR2]
   * MAC2=A2[D3*IR1 - D1*IR3]
   * MAC3=A3[D1*IR2 - D2*IR1]
   */
  tmp= (S16_S64(IR3)*S16_S64(D2) - S16_S64(IR2)*S16_S64(D3))>>sf;
  SET_MAC1_TMP64 ( tmp );
  tmp= (S16_S64(IR1)*S16_S64(D3) - S16_S64(IR3)*S16_S64(D1))>>sf;
  SET_MAC2_TMP64 ( tmp );
  tmp= (S16_S64(IR2)*S16_S64(D1) - S16_S64(IR1)*S16_S64(D2))>>sf;
  SET_MAC3_TMP64 ( tmp );

  /* [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] */
  SET_IR ( 1 );
  SET_IR ( 2 );
  SET_IR ( 3 );
  
} /* end op */


static void
color_LLM_mult_V0 (
        	   const int16_t vx,
        	   const int16_t vy,
        	   const int16_t vz,
        	   const int     sf
        	   )
{

  static const bool lm_0= false;

  int64_t tmp;


  /* [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (LLM*V0) SAR (sf*12) */
  /*
   * [1,19,12] MAC1=A1[L11*VX0 + L12*VY0 + L13*VZ0] [1,19,24]
   * [1,19,12] MAC2=A2[L21*VX0 + L22*VY0 + L23*VZ0] [1,19,24]
   * [1,19,12] MAC3=A3[L31*VX0 + L32*VY0 + L33*VZ0] [1,19,24]
   * [1,3,12]  IR1= Lm_B1[MAC1] [1,19,12][lm=1]
   * [1,3,12]  IR2= Lm_B2[MAC2] [1,19,12][lm=1]
   * [1,3,12]  IR3= Lm_B3[MAC3] [1,19,12][lm=1]
   */
  tmp=
    (S16_S64(L11)*S16_S64(vx) +
     S16_S64(L12)*S16_S64(vy) +
     S16_S64(L13)*S16_S64(vz))>>sf;
  SET_MAC1_TMP64 ( tmp );
  tmp=
    (S16_S64(L21)*S16_S64(vx) +
     S16_S64(L22)*S16_S64(vy) +
     S16_S64(L23)*S16_S64(vz))>>sf;
  SET_MAC2_TMP64 ( tmp );
  tmp=
    (S16_S64(L31)*S16_S64(vx) +
     S16_S64(L32)*S16_S64(vy) +
     S16_S64(L33)*S16_S64(vz))>>sf;
  SET_MAC3_TMP64 ( tmp );
  SET_IR ( 1 );
  SET_IR ( 2 );
  SET_IR ( 3 );

} /* end color_LLM_mult_V0 */


static void
color_BK_plus_LCM_mult_IR (
        		   const int     sf
        		   )
{

  static const bool lm_0= false;

  int64_t tmp;


  /* [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12) */
  /*
   * [1,19,12] MAC1=A1[RBK + LR1*IR1 + LR2*IR2 + LR3*IR3] [1,19,24]
   * [1,19,12] MAC2=A2[GBK + LG1*IR1 + LG2*IR2 + LG3*IR3] [1,19,24]
   * [1,19,12] MAC3=A3[BBK + LB1*IR1 + LB2*IR2 + LB3*IR3] [1,19,24]
   * [1,3,12]  IR1= Lm_B1[MAC1] [1,19,12][lm=1]
   * [1,3,12]  IR2= Lm_B2[MAC2] [1,19,12][lm=1]
   * [1,3,12]  IR3= Lm_B3[MAC3] [1,19,12][lm=1]
   */
  tmp=
    ((S32_S64(RBK)<<12) +
     S16_S64(LR1)*S16_S64(IR1) +
     S16_S64(LR2)*S16_S64(IR2) +
     S16_S64(LR3)*S16_S64(IR3))>>sf;
  SET_MAC1_TMP64 ( tmp );
  tmp=
    ((S32_S64(GBK)<<12) +
     S16_S64(LG1)*S16_S64(IR1) +
     S16_S64(LG2)*S16_S64(IR2) +
     S16_S64(LG3)*S16_S64(IR3))>>sf;
  SET_MAC2_TMP64 ( tmp );
  tmp=
    ((S32_S64(BBK)<<12) +
     S16_S64(LB1)*S16_S64(IR1) +
     S16_S64(LB2)*S16_S64(IR2) +
     S16_S64(LB3)*S16_S64(IR3))>>sf;
  SET_MAC3_TMP64 ( tmp );
  SET_IR ( 1 );
  SET_IR ( 2 );
  SET_IR ( 3 );
  
} /* end color_BK_plus_LCM_mult_IR */


static void
color_fifo (void)
{

  int32_t tmp;

  
  /* Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE] */
  /* 
   * [0,8,0]   Cd0<-Cd1<-Cd2<- CODE
   * [0,8,0]   R0<-R1<-R2<- Lm_C1[MAC1] [1,27,4]
   * [0,8,0]   G0<-G1<-G2<- Lm_C2[MAC2] [1,27,4]
   * [0,8,0]   B0<-B1<-B2<- Lm_C3[MAC3] [1,27,4]
   */
  RGB0= RGB1;
  RGB1= RGB2;
  RGB2= (RGBC&0xFF000000); /* CODE */
  /* B */
  tmp= MAC3>>4;
  if ( tmp > 0xFF )   { FLAG|= F_C3; RGB2|= 0x00FF0000; }
  else if ( tmp < 0 ) { FLAG|= F_C3; }
  else RGB2|= (tmp<<16);
  /* G */
  tmp= MAC2>>4;
  if ( tmp > 0xFF )   { FLAG|= F_C2; RGB2|= 0x0000FF00; }
  else if ( tmp < 0 ) { FLAG|= F_C2; }
  else RGB2|= (tmp<<8);
  /* R */
  tmp= MAC1>>4;
  if ( tmp > 0xFF )   { FLAG|= F_C1; RGB2|= 0x000000FF; }
  else if ( tmp < 0 ) { FLAG|= F_C1; }
  else RGB2|= tmp;
  
} /* end color_fifo */


static void
nc_body (
         const int16_t vx,
         const int16_t vy,
         const int16_t vz,
         const int     sf
         )
{

  color_LLM_mult_V0 ( vx, vy, vz, sf );
  color_BK_plus_LCM_mult_IR ( sf );
  color_fifo ();
  
} /* end nc_body */


/* Normal color (single) */
static void
ncs (
     const uint32_t cmd
     )
{

  int sf;
  
  
  /* Reset flags and cycles. */
  FLAG= 0;
  _cc= 14;
  
  /* Execute. */
  sf= GET_SF ( cmd );
  nc_body ( VX0, VY0, VZ0, sf );
  
} /* end ncs */


/* Normal color (triple) */
static void
nct (
     const uint32_t cmd
     )
{

  int sf;
  
  
  /* Reset flags and cycles. */
  FLAG= 0;
  _cc= 30;
  
  /* Execute. */
  sf= GET_SF ( cmd );
  nc_body ( VX0, VY0, VZ0, sf );
  nc_body ( VX1, VY1, VZ1, sf );
  nc_body ( VX2, VY2, VZ2, sf );
  
} /* end nct */


static void
ncc_ncd_common_begin (void)
{

  int64_t tmp;

  
  /* [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4 */
  /*
   * [1,27,4]  MAC1=A1[R*IR1] [1,27,16]
   * [1,27,4]  MAC2=A2[G*IR2] [1,27,16]
   * [1,27,4]  MAC3=A3[B*IR3] [1,27,16]
   */
  tmp= (U32_S64(GET_R)*S16_S64(IR1))<<4;
  SET_MAC1_TMP64 ( tmp );
  tmp= (U32_S64(GET_G)*S16_S64(IR2))<<4;
  SET_MAC2_TMP64 ( tmp );
  tmp= (U32_S64(GET_B)*S16_S64(IR3))<<4;
  SET_MAC3_TMP64 ( tmp );
  
} /* end ncc_ncd_common_begin */


static void
ncc_ncd_common_end (
        	    const int sf,
                    const int lm_0
        	    )
{

  int64_t tmp;


  /* [MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SAR (sf*12) */
  /* [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] */
  /*
   * [1,27,4]  MAC1=A1[R*IR1] [1,27,16]
   * [1,27,4]  MAC2=A2[G*IR2] [1,27,16]
   * [1,27,4]  MAC3=A3[B*IR3] [1,27,16]
   * [1,3,12]  IR1= Lm_B1[MAC1] [1,27,4][lm=1]
   * [1,3,12]  IR2= Lm_B2[MAC2] [1,27,4][lm=1]
   * [1,3,12]  IR3= Lm_B3[MAC3] [1,27,4][lm=1]
   */
  tmp= S32_S64(MAC1)>>sf; SET_MAC1_TMP64 ( tmp );
  tmp= S32_S64(MAC2)>>sf; SET_MAC2_TMP64 ( tmp );
  tmp= S32_S64(MAC3)>>sf; SET_MAC3_TMP64 ( tmp );
  SET_IR ( 1 );
  SET_IR ( 2 );
  SET_IR ( 3 );
  
} // end ncc_ncd_common_end


static void
ncc_body (
          const int16_t vx,
          const int16_t vy,
          const int16_t vz,
          const int     sf,
          const bool    lm_0
          )
{
  
  color_LLM_mult_V0 ( vx, vy, vz, sf );
  color_BK_plus_LCM_mult_IR ( sf );
  ncc_ncd_common_begin ();
  ncc_ncd_common_end ( sf, lm_0 );
  color_fifo ();
  
} // end ncc_body


// Normal Color Color (single vector).
static void
nccs (
      const uint32_t cmd
      )
{

  int sf;
  bool lm_0;
  
  
  // Reset flags and cycles.
  FLAG= 0;
  _cc= 17;
  
  // Execute.
  sf= GET_SF ( cmd );
  lm_0= CHECK_LM_IS_0 ( cmd );
  ncc_body ( VX0, VY0, VZ0, sf, lm_0 );
  
} // end nccs


// Normal Color Color (triple vector).
static void
ncct (
      const uint32_t cmd
      )
{
  
  int sf;
  bool lm_0;
  
  // Reset flags and cycles.
  FLAG= 0;
  _cc= 39;
  
  // Execute.
  sf= GET_SF ( cmd );
  lm_0= CHECK_LM_IS_0 ( cmd );
  ncc_body ( VX0, VY0, VZ0, sf, lm_0 );
  ncc_body ( VX1, VY1, VZ1, sf, lm_0 );
  ncc_body ( VX2, VY2, VZ2, sf, lm_0 );
  
} // end ncct


static void
depth_que_calc (
        	const int sf
        	)
{

  static const bool lm_0= true;

  int64_t tmp;
  int32_t tmp32;
  

  /* [IR1,IR2,IR3] = (([RFC,GFC,BFC] SHL 12) - [MAC1,MAC2,MAC3]) SAR (sf*12) */
  tmp32= ((S32_S64(RFC)<<12) - S32_S64(MAC1))>>sf;
  SET_IR_TMP32 ( 1, tmp32 );
  tmp32= ((S32_S64(GFC)<<12) - S32_S64(MAC2))>>sf;
  SET_IR_TMP32 ( 2, tmp32 );
  tmp32= ((S32_S64(BFC)<<12) - S32_S64(MAC3))>>sf;
  SET_IR_TMP32 ( 3, tmp32 );

  /* [MAC1,MAC2,MAC3] = (([IR1,IR2,IR3] * IR0) + [MAC1,MAC2,MAC3]) */
  tmp= S16_S64(IR1)*S16_S64(IR0) + S32_S64(MAC1); SET_MAC1_TMP64 ( tmp );
  tmp= S16_S64(IR2)*S16_S64(IR0) + S32_S64(MAC2); SET_MAC2_TMP64 ( tmp );
  tmp= S16_S64(IR3)*S16_S64(IR0) + S32_S64(MAC3); SET_MAC3_TMP64 ( tmp );
  
} /* end depth_que_calc */


static void
ncd_body (
          const int16_t vx,
          const int16_t vy,
          const int16_t vz,
          const int     sf,
          const int     lm_0
          )
{
  
  color_LLM_mult_V0 ( vx, vy, vz, sf );
  color_BK_plus_LCM_mult_IR ( sf );
  ncc_ncd_common_begin ();
  depth_que_calc ( sf );
  ncc_ncd_common_end ( sf, lm_0 );
  color_fifo ();
  
} // end ncd_body


// Normal color depth cue (single vector).
static void
ncds (
      const uint32_t cmd
      )
{
  
  int sf;
  bool lm_0;
  
  
  // Reset flags and cycles.
  FLAG= 0;
  _cc= 19;
  
  // Execute.
  sf= GET_SF ( cmd );
  lm_0= CHECK_LM_IS_0 ( cmd );
  ncd_body ( VX0, VY0, VZ0, sf, lm_0 );
  
} // end ncds


// Normal color depth cue (triple vectors).
static void
ncdt (
      const uint32_t cmd
      )
{
  
  int sf;
  bool lm_0;
  
  
  // Reset flags and cycles.
  FLAG= 0;
  _cc= 44;
  
  // Execute.
  sf= GET_SF ( cmd );
  lm_0= CHECK_LM_IS_0 ( cmd );
  ncd_body ( VX0, VY0, VZ0, sf, lm_0 );
  ncd_body ( VX1, VY1, VZ1, sf, lm_0 );
  ncd_body ( VX2, VY2, VZ2, sf, lm_0 );
  
} // end ncdt


/* Color Color. */
static void
cc (
    const uint32_t cmd
    )
{

  int sf;
  bool lm_0;

  
  // Reset flags and cycles.
  FLAG= 0;
  _cc= 11;
  
  // Execute.
  sf= GET_SF ( cmd );
  lm_0= CHECK_LM_IS_0 ( cmd );
  color_BK_plus_LCM_mult_IR ( sf );
  ncc_ncd_common_begin ();
  ncc_ncd_common_end ( sf, lm_0 );
  color_fifo ();
  
} // end cc


// Color Color.
static void
cdp (
     const uint32_t cmd
     )
{

  int sf;
  bool lm_0;
  
  
  // Reset flags and cycles.
  FLAG= 0;
  _cc= 13;
  
  // Execute.
  sf= GET_SF ( cmd );
  lm_0= CHECK_LM_IS_0 ( cmd );
  color_BK_plus_LCM_mult_IR ( sf );
  ncc_ncd_common_begin ();
  depth_que_calc ( sf );
  ncc_ncd_common_end ( sf, lm_0 );
  color_fifo ();
  
} // end cdp


// Depth Cue Color light.
static void
dcpl (
      const uint32_t cmd
      )
{

  int sf;
  bool lm_0;
  
  
  // Reset flags and cycles.
  FLAG= 0;
  _cc= 8;
  
  // Execute.
  sf= GET_SF ( cmd );
  lm_0= CHECK_LM_IS_0 ( cmd );
  ncc_ncd_common_begin ();
  depth_que_calc ( sf );
  ncc_ncd_common_end ( sf, lm_0 );
  color_fifo ();
  
} // end dcpl


static void
dpc_body (
          const int      sf,
          const int      lm_0,
          const uint32_t reg
          )
{

  int64_t tmp;

  
  // [MAC1,MAC2,MAC3] = [R,G,B] SHL 16
  tmp= U32_S64(GET_R_REG ( reg ))<<16;
  SET_MAC1_TMP64 ( tmp );
  tmp= U32_S64(GET_G_REG ( reg ))<<16;
  SET_MAC2_TMP64 ( tmp );
  tmp= U32_S64(GET_B_REG ( reg ))<<16;
  SET_MAC3_TMP64 ( tmp );

  // Resta de la instrucció.
  depth_que_calc ( sf );
  ncc_ncd_common_end ( sf, lm_0 );
  color_fifo ();
  
} // end dpc_body


// Depth Cueing (single).
static void
dpcs (
      const uint32_t cmd
      )
{

  int sf;
  bool lm_0;

  
  // Reset flags and cycles.
  FLAG= 0;
  _cc= 8;
  
  // Execute.
  sf= GET_SF ( cmd );
  lm_0= CHECK_LM_IS_0 ( cmd );
  dpc_body ( sf, lm_0, RGBC );
  
} // end dpcs


// Depth Cueing (triple).
static void
dpct (
      const uint32_t cmd
      )
{
  
  int sf;
  bool lm_0;
  
  
  // Reset flags and cycles.
  FLAG= 0;
  _cc= 17;
  
  // Execute.
  sf= GET_SF ( cmd );
  lm_0= CHECK_LM_IS_0 ( cmd );
  dpc_body ( sf, lm_0, RGB0 );
  dpc_body ( sf, lm_0, RGB0 );
  dpc_body ( sf, lm_0, RGB0 );
  
} // end dpct


// Interpolation of a vector and far color
static void
intpl (
       const uint32_t cmd
       )
{

  int sf;
  bool lm_0;
  int64_t tmp;
  
  
  // Reset flags and cycles.
  FLAG= 0;
  _cc= 8;

  // Execute.
  sf= GET_SF ( cmd );
  lm_0= CHECK_LM_IS_0 ( cmd );
  
  // [MAC1,MAC2,MAC3] = [IR1,IR2,IR3] SHL 12
  tmp= S16_S64(IR1)<<12;
  SET_MAC1_TMP64 ( tmp );
  tmp= S16_S64(IR2)<<12;
  SET_MAC2_TMP64 ( tmp );
  tmp= S16_S64(IR3)<<12;
  SET_MAC3_TMP64 ( tmp );
  
  // Resta de la instrucció.
  depth_que_calc ( sf );
  ncc_ncd_common_end ( sf, lm_0 );
  color_fifo ();
  
} // end intpl


/* General purpose Interpolation. */
static void
gpf (
     const uint32_t cmd
     )
{

  int sf;
  bool lm_0;
  int64_t tmp;

  
  /* Reset flags and cycles. */
  FLAG= 0;
  _cc= 5;

  /* Execute. */
  sf= GET_SF ( cmd );
  lm_0= CHECK_LM_IS_0 ( cmd );

  /* [MAC1,MAC2,MAC3] = [0,0,0] */
  /* MAC1= MAC2= MAC3= 0; <-- NO CAL !!! */

  /* [MAC1,MAC2,MAC3] = (([IR1,IR2,IR3] * IR0) + [MAC1,MAC2,MAC3]) SAR
     (sf*12) */
  /*
   * MAC1=A1[IR0 * IR1]
   * MAC2=A2[IR0 * IR2]
   * MAC3=A3[IR0 * IR3]
   */
  tmp= (S16_S64(IR1)*S16_S64(IR0) /* + S32_S64(MAC1) */)>>sf;
  SET_MAC1_TMP64 ( tmp );
  tmp= (S16_S64(IR2)*S16_S64(IR0) /* + S32_S64(MAC2) */)>>sf;
  SET_MAC2_TMP64 ( tmp );
  tmp= (S16_S64(IR3)*S16_S64(IR0) /* + S32_S64(MAC3) */)>>sf;
  SET_MAC3_TMP64 ( tmp );

  /* [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] */
  SET_IR ( 1 );
  SET_IR ( 2 );
  SET_IR ( 3 );

  /* Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], */
  color_fifo ();
  
} /* end gpf */


/* General Interpolation with base. */
static void
gpl (
     const uint32_t cmd
     )
{

  int sf;
  bool lm_0;
  int64_t tmp;

  
  /* Reset flags and cycles. */
  FLAG= 0;
  _cc= 5;

  /* Execute. */
  sf= GET_SF ( cmd );
  lm_0= CHECK_LM_IS_0 ( cmd );

  /* MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SHL (sf*12) */
  tmp= S32_S64(MAC1)<<sf; SET_MAC1_TMP64 ( tmp );
  tmp= S32_S64(MAC2)<<sf; SET_MAC2_TMP64 ( tmp );
  tmp= S32_S64(MAC3)<<sf; SET_MAC3_TMP64 ( tmp );

  /* [MAC1,MAC2,MAC3] = (([IR1,IR2,IR3] * IR0) + [MAC1,MAC2,MAC3]) SAR
     (sf*12) */
  tmp= (S16_S64(IR1)*S16_S64(IR0) + S32_S64(MAC1))>>sf;
  SET_MAC1_TMP64 ( tmp );
  tmp= (S16_S64(IR2)*S16_S64(IR0) + S32_S64(MAC2))>>sf;
  SET_MAC2_TMP64 ( tmp );
  tmp= (S16_S64(IR3)*S16_S64(IR0) + S32_S64(MAC3))>>sf;
  SET_MAC3_TMP64 ( tmp );

  /* [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] */
  SET_IR ( 1 );
  SET_IR ( 2 );
  SET_IR ( 3 );
  
  /* Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], */
  color_fifo ();
  
} /* end gpl */


static void
clock (void)
{

  int cc;


  // NOTA!! Un concepte important és que el GTE està bloquejat, no pot
  // ni tan sols ordenar executar una altra instrucció fins que no
  // s'haja executat aquesta. De fet fa que la UCP es bloqueje. Per
  // tant no és necessari preocupar-se si en tants CC es podria haver
  // executat més d'una operació, perquè fins que no es cride a aquest
  // clock la UCP ha estat bloquejada intentat executar açò.
  cc= PSX_Clock - _cc_used;
  if ( cc <= 0 ) return;
  _cc_used+= cc;
  if ( cc > _cc ) _cc= 0;
  else            _cc-= cc;
  
} // end clock


static int
read (
      const int  nreg,
      uint32_t  *dst
      )
{

  int16_t tmp;
  int ret;
  

  clock ();
  ret= PSX_CYCLES_INST;
  if ( _cc > 0 ) { ret+= _cc; _cc= 0; }
  _cc_used+= ret;
  
  switch ( nreg )
    {
    case 0: *dst= (S16_U32 ( VY0 )<<16) | S16_U32 ( VX0 ); break;
    case 1: *dst= EXT_S16_U32 ( VZ0 ); break;
    case 2: *dst= (S16_U32 ( VY1 )<<16) | S16_U32 ( VX1 ); break;
    case 3: *dst= EXT_S16_U32 ( VZ1 ); break;
    case 4: *dst= (S16_U32 ( VY2 )<<16) | S16_U32 ( VX2 ); break;
    case 5: *dst= EXT_S16_U32 ( VZ2 ); break;
    case 6: *dst= RGBC; break;
    case 7: *dst= OTZ&0x7FFF; break;
    case 8: *dst= EXT_S16_U32 ( IR0 ); break;
    case 9: *dst= EXT_S16_U32 ( IR1 ); break;
    case 10: *dst= EXT_S16_U32 ( IR2 ); break;
    case 11: *dst= EXT_S16_U32 ( IR3 ); break;
    case 12: *dst= (S16_U32 ( SY0 )<<16) | S16_U32 ( SX0 ); break;
    case 13: *dst= (S16_U32 ( SY1 )<<16) | S16_U32 ( SX1 ); break;
    case 14: *dst= (S16_U32 ( SY2 )<<16) | S16_U32 ( SX2 ); break;
    case 15: *dst= (S16_U32 ( SY2 )<<16) | S16_U32 ( SX2 ); break;
    case 16: *dst= SZ0; break;
    case 17: *dst= SZ1; break;
    case 18: *dst= SZ2; break;
    case 19: *dst= SZ3; break;
    case 20: *dst= RGB0; break;
    case 21: *dst= RGB1; break;
    case 22: *dst= RGB2; break;
    case 23: *dst= RES1; break;
    case 24: *dst= S32_U32 ( MAC0 ); break;
    case 25: *dst= S32_U32 ( MAC1 ); break;
    case 26: *dst= S32_U32 ( MAC2 ); break;
    case 27: *dst= S32_U32 ( MAC3 ); break;
    case 28: // ORGB is a read-only mirror de IRGB
    case 29:
      *dst= 0;
      tmp= IR1>>7;
      if ( tmp >= 0x1F )  *dst|= 0x0000001F;
      else if ( tmp > 0 ) *dst|= tmp;
      tmp= IR2>>7;
      if ( tmp >= 0x1F )  *dst|= 0x000003E0;
      else if ( tmp > 0 ) *dst|= (tmp<<5);
      tmp= IR3>>7;
      if ( tmp >= 0x1F )  *dst|= 0x00007C00;
      else if ( tmp > 0 ) *dst|= (tmp<<10);
      break;
    case 30: *dst= S32_U32 ( LZCS ); break;
    case 31: *dst= LZCR; break;
    case 32: *dst= (S16_U32 ( RT12 )<<16) | S16_U32 ( RT11 ); break;
    case 33: *dst= (S16_U32 ( RT21 )<<16) | S16_U32 ( RT13 ); break;
    case 34: *dst= (S16_U32 ( RT23 )<<16) | S16_U32 ( RT22 ); break;
    case 35: *dst= (S16_U32 ( RT32 )<<16) | S16_U32 ( RT31 ); break;
    case 36: *dst= EXT_S16_U32 ( RT33 ); break;
    case 37: *dst= S32_U32 ( TRX ); break;
    case 38: *dst= S32_U32 ( TRY ); break;
    case 39: *dst= S32_U32 ( TRZ ); break;
    case 40: *dst= (S16_U32 ( L12 )<<16) | S16_U32 ( L11 ); break;
    case 41: *dst= (S16_U32 ( L21 )<<16) | S16_U32 ( L13 ); break;
    case 42: *dst= (S16_U32 ( L23 )<<16) | S16_U32 ( L22 ); break;
    case 43: *dst= (S16_U32 ( L32 )<<16) | S16_U32 ( L31 ); break;
    case 44: *dst= EXT_S16_U32 ( L33 ); break;
    case 45: *dst= S32_U32 ( RBK ); break;
    case 46: *dst= S32_U32 ( GBK ); break;
    case 47: *dst= S32_U32 ( BBK ); break;
    case 48: *dst= (S16_U32 ( LR2 )<<16) | S16_U32 ( LR1 ); break;
    case 49: *dst= (S16_U32 ( LG1 )<<16) | S16_U32 ( LR3 ); break;
    case 50: *dst= (S16_U32 ( LG3 )<<16) | S16_U32 ( LG2 ); break;
    case 51: *dst= (S16_U32 ( LB2 )<<16) | S16_U32 ( LB1 ); break;
    case 52: *dst= EXT_S16_U32 ( LB3 ); break;
    case 53: *dst= S32_U32 ( RFC ); break;
    case 54: *dst= S32_U32 ( GFC ); break;
    case 55: *dst= S32_U32 ( BFC ); break;
    case 56: *dst= S32_U32 ( OFX ); break;
    case 57: *dst= S32_U32 ( OFY ); break;
    case 58: *dst= S16_U32 ( (int16_t) H ); break;
    case 59: *dst= S16_U32 ( DQA ); break;
    case 60: *dst= S32_U32 ( DQB ); break;
    case 61: *dst= S16_U32 ( ZSF3 ); break;
    case 62: *dst= S16_U32 ( ZSF4 ); break;
    case 63: *dst= (FLAG&0xFFFFF000); break;
      
    default: *dst= 0;
    }
  
  return ret;
  
} // end read


static int
read_trace (
            const int  nreg,
            uint32_t  *dst
            )
{
  
  int ret;

  
  ret= read ( nreg, dst );
  _mem_access ( true, nreg, *dst, ret==0, _udata );

  return ret;
  
} // end read_trace


static void
write (
       const int      nreg,
       const uint32_t data
       )
{

  switch ( nreg )
    {
    case 0: VX0= (int16_t) (data&0xFFFF); VY0= (int16_t) (data>>16); break;
    case 1: VZ0= (int16_t) (data&0xFFFF); break;
    case 2: VX1= (int16_t) (data&0xFFFF); VY1= (int16_t) (data>>16); break;
    case 3: VZ1= (int16_t) (data&0xFFFF); break;
    case 4: VX2= (int16_t) (data&0xFFFF); VY2= (int16_t) (data>>16); break;
    case 5: VZ2= (int16_t) (data&0xFFFF); break;
    case 6: RGBC= data; break;
    case 7: /* OTZ= (uint16_t) (data&0x7FFF); ¿READ ONLY? */ break;
    case 8: IR0= (int16_t) (data&0xFFFF); break;
    case 9: IR1= (int16_t) (data&0xFFFF); break;
    case 10: IR2= (int16_t) (data&0xFFFF); break;
    case 11: IR3= (int16_t) (data&0xFFFF); break;
    case 12: SX0= (int16_t) (data&0xFFFF); SY0= (int16_t) (data>>16); break;
    case 13: SX1= (int16_t) (data&0xFFFF); SY1= (int16_t) (data>>16); break;
    case 14: SX2= (int16_t) (data&0xFFFF); SY2= (int16_t) (data>>16); break;
    case 15:
      SX0= SX1; SY0= SY1;
      SX1= SX2; SY1= SY2;
      SX2= (int16_t) (data&0xFFFF); SY2= (int16_t) (data>>16);
      break;
    case 16: SZ0= (uint16_t) (data&0xFFFF); break;
    case 17: SZ1= (uint16_t) (data&0xFFFF); break;
    case 18: SZ2= (uint16_t) (data&0xFFFF); break;
    case 19: SZ3= (uint16_t) (data&0xFFFF); break;
    case 20: RGB0= data; break;
    case 21: RGB1= data; break;
    case 22: RGB2= data; break;
    case 23: RES1= data; break;
    case 24: MAC0= (int32_t) data; break;
    case 25: MAC1= (int32_t) data; break;
    case 26: MAC2= (int32_t) data; break;
    case 27: MAC3= (int32_t) data; break;
    case 28:
      IR1= (data&0x1F)<<7;
      IR2= ((data>>5)&0x1F)<<7;
      IR3= ((data>>10)&0x1F)<<7;
      break;
    case 29: /* ORGB= data; Read only. */ break;
    case 30:
      LZCS= (int32_t) data;
      LZCR= (LZCS >= 0) ? clz ( data ) : clz ( ~data );
      break;
    case 31: /* LZCR= data; ¿READ ONLY? */ break;
    case 32: RT11= (int16_t) (data&0xFFFF); RT12= (int16_t) (data>>16); break;
    case 33: RT13= (int16_t) (data&0xFFFF); RT21= (int16_t) (data>>16); break;
    case 34: RT22= (int16_t) (data&0xFFFF); RT23= (int16_t) (data>>16); break;
    case 35: RT31= (int16_t) (data&0xFFFF); RT32= (int16_t) (data>>16); break;
    case 36: RT33= (int16_t) (data&0xFFFF); break;
    case 37: TRX= (int32_t) data; break;
    case 38: TRY= (int32_t) data; break;
    case 39: TRZ= (int32_t) data; break;
    case 40: L11= (int16_t) (data&0xFFFF); L12= (int16_t) (data>>16); break;
    case 41: L13= (int16_t) (data&0xFFFF); L21= (int16_t) (data>>16); break;
    case 42: L22= (int16_t) (data&0xFFFF); L23= (int16_t) (data>>16); break;
    case 43: L31= (int16_t) (data&0xFFFF); L32= (int16_t) (data>>16); break;
    case 44: L33= (int16_t) (data&0xFFFF); break;
    case 45: RBK= (int32_t) data; break;
    case 46: GBK= (int32_t) data; break;
    case 47: BBK= (int32_t) data; break;
    case 48: LR1= (int16_t) (data&0xFFFF); LR2= (int16_t) (data>>16); break;
    case 49: LR3= (int16_t) (data&0xFFFF); LG1= (int16_t) (data>>16); break;
    case 50: LG2= (int16_t) (data&0xFFFF); LG3= (int16_t) (data>>16); break;
    case 51: LB1= (int16_t) (data&0xFFFF); LB2= (int16_t) (data>>16); break;
    case 52: LB3= (int16_t) (data&0xFFFF); break;
    case 53: RFC= (int32_t) data; break;
    case 54: GFC= (int32_t) data; break;
    case 55: BFC= (int32_t) data; break;
    case 56: OFX= (int32_t) data; break;
    case 57: OFY= (int32_t) data; break;
    case 58: H= (uint16_t) (data&0xFFFF); break;
    case 59: DQA= (int16_t) (data&0xFFFF); break;
    case 60: DQB= (int32_t) data; break;
    case 61: ZSF3= (int16_t) (data&0xFFFF); break;
    case 62: ZSF4= (int16_t) (data&0xFFFF); break;
    case 63: FLAG= (data&0x7FFFF000); break;
      
    default: break;
    }
  
} // end write


static void
write_trace (
             const int      nreg,
             const uint32_t data
             )
{

  write ( nreg, data );
  _mem_access ( false, nreg, data, true, _udata );
  
} // end write_trace


static void
execute (
         const uint32_t cmd
         )
{
  
  switch ( (cmd&0x3F) )
    {

    case 0x01: rtps ( cmd ); break;

    case 0x06: nclip (); break;

    case 0x0C: op ( cmd ); break;

    case 0x10: dpcs ( cmd ); break;
    case 0x11: intpl ( cmd ); break;
    case 0x12: mvmva ( cmd ); break;
    case 0x13: ncds ( cmd ); break;
    case 0x14: cdp ( cmd ); break;
      
    case 0x16: ncdt ( cmd ); break;
      
    case 0x1B: nccs ( cmd ); break;
    case 0x1C: cc ( cmd ); break;
      
    case 0x1E: ncs ( cmd ); break;

    case 0x20: nct ( cmd ); break;
      
    case 0x28: sqr ( cmd ); break;
    case 0x29: dcpl ( cmd ); break;
    case 0x2A: dpct ( cmd ); break;
      
    case 0x2D: avsz3 (); break;
    case 0x2E: avsz4 (); break;
      
    case 0x30: rtpt ( cmd ); break;

    case 0x3D: gpf ( cmd ); break;
    case 0x3E: gpl ( cmd ); break;
    case 0x3F: ncct ( cmd ); break;
      
    default:
      _warning ( _udata,
        	 "instrucció COP2 (GTE) desconeguda, func: %02x",
        	 (cmd&0x3F) );
    }
  
} // end execute


static void
copy_regs (
           uint32_t regs[64]
           )
{

  int16_t tmp;
  uint32_t val;

  
  // Vector 0
  regs[0]= PACKR16(VY0,VX0);
  regs[1]= PACKR16(0,VZ0);

  // Vector 1
  regs[2]= PACKR16(VY1,VX1);
  regs[3]= PACKR16(0,VZ1);

  // Vector 2
  regs[4]= PACKR16(VY2,VX2);
  regs[5]= PACKR16(0,VZ2);

  // Color code
  regs[6]= RGBC;
  
  // Average Z registers
  regs[7]= PACKR16(0,OTZ);

  // Interpolation factor
  regs[8]= PACKR16(0,IR0);
  
  // Vector 3
  regs[9]= PACKR16(0,IR1);
  regs[10]= PACKR16(0,IR2);
  regs[11]= PACKR16(0,IR3);
  
  // Screen XYZ coordinate FIFO
  regs[12]= PACKR16(SY0,SX0);
  regs[13]= PACKR16(SY1,SX1);
  regs[14]= PACKR16(SY2,SX2);
  regs[15]= PACKR16(SY2,SX2);
  regs[16]= PACKR16(0,SZ0);
  regs[17]= PACKR16(0,SZ1);
  regs[18]= PACKR16(0,SZ2);
  regs[19]= PACKR16(0,SZ3);

  // Color FIFO
  regs[20]= RGB0;
  regs[21]= RGB1;
  regs[22]= RGB2;
  regs[23]= RES1;

  // MAC registers
  regs[24]= (uint32_t) MAC0;
  regs[25]= (uint32_t) MAC1;
  regs[26]= (uint32_t) MAC2;
  regs[27]= (uint32_t) MAC3;

  // IRGB/ORGB
  val= 0;
  tmp= IR1>>7;
  if ( tmp >= 0x1F )  val|= 0x0000001F;
  else if ( tmp > 0 ) val|= tmp;
  tmp= IR2>>7;
  if ( tmp >= 0x1F )  val|= 0x000003E0;
  else if ( tmp > 0 ) val|= (tmp<<5);
  tmp= IR3>>7;
  if ( tmp >= 0x1F )  val|= 0x00007C00;
  else if ( tmp > 0 ) val|= (tmp<<10);
  regs[28]= val;
  regs[29]= val;
  
  // Count leading bits
  regs[30]= (uint32_t) LZCS;
  regs[31]= LZCR;
  
  // Rotation matrix
  regs[32]= PACKR16(RT12,RT11);
  regs[33]= PACKR16(RT21,RT13);
  regs[34]= PACKR16(RT23,RT22);
  regs[35]= PACKR16(RT32,RT31);
  regs[36]= PACKR16(0,RT33);

  // Translation vector
  regs[37]= (uint32_t) TRX;
  regs[38]= (uint32_t) TRY;
  regs[39]= (uint32_t) TRZ;
  
  // Light matrix
  regs[40]= PACKR16(L12,L11);
  regs[41]= PACKR16(L21,L13);
  regs[42]= PACKR16(L23,L22);
  regs[43]= PACKR16(L32,L31);
  regs[44]= PACKR16(0,L33);

  // Background color
  regs[45]= (uint32_t) RBK;
  regs[46]= (uint32_t) GBK;
  regs[47]= (uint32_t) BBK;
  
  // Light color matrix
  regs[48]= PACKR16(LR2,LR1);
  regs[49]= PACKR16(LG1,LR3);
  regs[50]= PACKR16(LG3,LG2);
  regs[51]= PACKR16(LB2,LB1);
  regs[52]= PACKR16(0,LB3);

  // Far color
  regs[53]= (uint32_t) RFC;
  regs[54]= (uint32_t) GFC;
  regs[55]= (uint32_t) BFC;

  // Screen offset and distance
  regs[56]= (uint32_t) OFX;
  regs[57]= (uint32_t) OFY;
  regs[58]= (uint32_t) H;
  regs[59]= (uint32_t) ((uint16_t) DQA);
  regs[60]= (uint32_t) DQB;
  
  // Average Z registers.
  regs[61]= PACKR16(0,ZSF3);
  regs[62]= PACKR16(0,ZSF4);

  // FLAG
  regs[63]= FLAG;
  
} // end copy_regs


static void
execute_trace (
               const uint32_t cmd
               )
{

  uint32_t regs_prev[64], regs_after[64];


  copy_regs ( regs_prev );
  execute ( cmd );
  copy_regs ( regs_after );
  _cmd_trace ( regs_prev, regs_after, _udata );
  
} // end execute_trace




/**********************/
/* FUNCIONS PÚBLIQUES */
/**********************/

void
PSX_gte_init (
              PSX_Warning      *warning,
              PSX_GTECmdTrace  *cmd_trace,
              PSX_GTEMemAccess *mem_access,
              void             *udata
              )
{

  _warning= warning;
  _cmd_trace= cmd_trace;
  _mem_access= mem_access;
  _udata= udata;
  _read= read;
  _write= write;
  _execute= execute;
  _cc= 0;
  _cc_used= 0;
  memset ( &_regs, 0, sizeof(_regs) );
  
} /* end PSX_get_init */


void
PSX_gte_end_iter (void)

{

  clock ();
  _cc_used= 0; // <-- Fi d'iteració.
  
} // end PSX_gte_clock


int
PSX_gte_execute (
        	 const uint32_t cmd
        	 )
{

  int ret;

  
  clock ();
  ret= PSX_CYCLES_INST;
  if ( _cc > 0 ) { ret+= _cc; _cc= 0; }
  _cc_used+= ret;
  
  _execute ( cmd );

  _cc-= PSX_CYCLES_INST;
  if ( _cc < 0 ) _cc= 0;
  
  return ret;
  
} // end PSX_gte_execute


int
PSX_gte_read (
              const int  nreg,
              uint32_t  *dst
              )
{
  
  int ret;

  
  ret= _read ( nreg, dst );
  
  return ret;
  
} // end PSX_gte_read


void
PSX_gte_write (
               const int      nreg,
               const uint32_t data
               )
{
  _write ( nreg, data );
} // end PSX_gte_write


void
PSX_gte_set_mode_trace (
        		const bool enable
        		)
{

  if ( _mem_access != NULL )
    {
      if ( enable )
        {
          _read= read_trace;
          _write= write_trace;
        }
      else
        {
          _read= read;
          _write= write;
        }
    }

  if ( _cmd_trace != NULL )
    {
      _execute= enable ? execute_trace : execute;
    }
  
} // end PSX_gte_set_mode_trace

/*
 * Copyright 2017-2022 Adrià Giménez Pastor.
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
 *  spu.c - Implementació del mòdul que simmula el xip de so.
 *
 */
/*
 * NOTES!!!
 *
 *  - He decidit que la configuració de l'ADSR sols s'actualitza quan
 *    s'activa el ON.
 *
 *  - He decidit emprar per a ADSR el mateix mode EXP-INC que en el
 *    envelope.
 *
 *  - De moment no implemente el DMA de lectura "glitchy".
 *
 *  - De moment no implemente el bug de vIIR i altres volumns en el
 *    reverb quan el seu valor és -0x8000.
 *
 *  - Decisions que he pres sobre la transferència:
 * 
 *    1.- Ficar a STOP el mode d'escriptura reseteja la FIFO.
 *
 *    2.- Quan vaig a fer una petició d'escriptura fique el busy a
 *        cert i en el següent cicel ho faig. No sé si estic emprant
 *        masa cicles o no. ATENCIÓ!!! Ho he comentat i he fet que no
 *        s'espere!!!! Per tant sempre està el bit busy a 0!!! Pareix
 *        que va millor així.
 *
 *    3.- Quan es demana un DMA read, inmediatament s'ompli la FIFO, i
 *        el buidar-la també.
 *
 *    4.- Si escrivint amb DMA la següent peitició aplega i encara no
 *        s'ha buidat la FIFO, la buide en el moment.
 *
 *  - Sobre el IRQ amb els voices, com que si no està aliniat amb el
 *    bloc ADPCM resulta que moltes vegades falla, he decidit no ser
 *    molt exacte i fer la interrupció durant el decoding del bloc si
 *    l'adreça està en el rang del bloc.
 */


#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "PSX.h"


int FLAG=0;

/**********/
/* MACROS */
/**********/

#define RAM_SIZE 512*1024

#define RAM_MASK ((RAM_SIZE)-1)

#define SAMPLES_PER_BLOCK 28

#define MUL16TO32(a,b)        						\
  ((int32_t) (((int32_t) (((int32_t) (a)) * ((int32_t) (b))))>>15))
#define MUL3216(a,b) (((a) * ((int32_t) (b)))>>15)
#define MUL16(a,b) ((int16_t) MUL16TO32(a,b))
#define I16(a) ((int16_t) (a))
#define U16(a) ((uint16_t) (a))
#define I32(a) ((int32_t) (a))
#define U32(a) ((uint32_t) (a))

#define FIFO_SIZE 32

#define CCPERSAMPLE 768 /* 33868800/44100 */

#define REC_BUF ((int16_t *) &(_ram[0]))

#define TOVOL(VAL)        						\
  ((int16_t) (((VAL)<-0x8000) ? -0x8000 : ((VAL)>0x7FFF?0x7FFF:(VAL))))




/*************/
/* CONSTANTS */
/*************/

static int32_t GAUSS[]=
  {
    -0x001,-0x001,-0x001,-0x001,-0x001,-0x001,-0x001,-0x001,
    -0x001,-0x001,-0x001,-0x001,-0x001,-0x001,-0x001,-0x001,
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0001,
    0x0001,0x0001,0x0001,0x0002,0x0002,0x0002,0x0003,0x0003,
    0x0003,0x0004,0x0004,0x0005,0x0005,0x0006,0x0007,0x0007,
    0x0008,0x0009,0x0009,0x000A,0x000B,0x000C,0x000D,0x000E,
    0x000F,0x0010,0x0011,0x0012,0x0013,0x0015,0x0016,0x0018,
    0x0019,0x001B,0x001C,0x001E,0x0020,0x0021,0x0023,0x0025,
    0x0027,0x0029,0x002C,0x002E,0x0030,0x0033,0x0035,0x0038,
    0x003A,0x003D,0x0040,0x0043,0x0046,0x0049,0x004D,0x0050,
    0x0054,0x0057,0x005B,0x005F,0x0063,0x0067,0x006B,0x006F,
    0x0074,0x0078,0x007D,0x0082,0x0087,0x008C,0x0091,0x0096,
    0x009C,0x00A1,0x00A7,0x00AD,0x00B3,0x00BA,0x00C0,0x00C7,
    0x00CD,0x00D4,0x00DB,0x00E3,0x00EA,0x00F2,0x00FA,0x0101,
    0x010A,0x0112,0x011B,0x0123,0x012C,0x0135,0x013F,0x0148,
    0x0152,0x015C,0x0166,0x0171,0x017B,0x0186,0x0191,0x019C,
    0x01A8,0x01B4,0x01C0,0x01CC,0x01D9,0x01E5,0x01F2,0x0200,
    0x020D,0x021B,0x0229,0x0237,0x0246,0x0255,0x0264,0x0273,
    0x0283,0x0293,0x02A3,0x02B4,0x02C4,0x02D6,0x02E7,0x02F9,
    0x030B,0x031D,0x0330,0x0343,0x0356,0x036A,0x037E,0x0392,
    0x03A7,0x03BC,0x03D1,0x03E7,0x03FC,0x0413,0x042A,0x0441,
    0x0458,0x0470,0x0488,0x04A0,0x04B9,0x04D2,0x04EC,0x0506,
    0x0520,0x053B,0x0556,0x0572,0x058E,0x05AA,0x05C7,0x05E4,
    0x0601,0x061F,0x063E,0x065C,0x067C,0x069B,0x06BB,0x06DC,
    0x06FD,0x071E,0x0740,0x0762,0x0784,0x07A7,0x07CB,0x07EF,
    0x0813,0x0838,0x085D,0x0883,0x08A9,0x08D0,0x08F7,0x091E,
    0x0946,0x096F,0x0998,0x09C1,0x09EB,0x0A16,0x0A40,0x0A6C,
    0x0A98,0x0AC4,0x0AF1,0x0B1E,0x0B4C,0x0B7A,0x0BA9,0x0BD8,
    0x0C07,0x0C38,0x0C68,0x0C99,0x0CCB,0x0CFD,0x0D30,0x0D63,
    0x0D97,0x0DCB,0x0E00,0x0E35,0x0E6B,0x0EA1,0x0ED7,0x0F0F,
    0x0F46,0x0F7F,0x0FB7,0x0FF1,0x102A,0x1065,0x109F,0x10DB,
    0x1116,0x1153,0x118F,0x11CD,0x120B,0x1249,0x1288,0x12C7,
    0x1307,0x1347,0x1388,0x13C9,0x140B,0x144D,0x1490,0x14D4,
    0x1517,0x155C,0x15A0,0x15E6,0x162C,0x1672,0x16B9,0x1700,
    0x1747,0x1790,0x17D8,0x1821,0x186B,0x18B5,0x1900,0x194B,
    0x1996,0x19E2,0x1A2E,0x1A7B,0x1AC8,0x1B16,0x1B64,0x1BB3,
    0x1C02,0x1C51,0x1CA1,0x1CF1,0x1D42,0x1D93,0x1DE5,0x1E37,
    0x1E89,0x1EDC,0x1F2F,0x1F82,0x1FD6,0x202A,0x207F,0x20D4,
    0x2129,0x217F,0x21D5,0x222C,0x2282,0x22DA,0x2331,0x2389,
    0x23E1,0x2439,0x2492,0x24EB,0x2545,0x259E,0x25F8,0x2653,
    0x26AD,0x2708,0x2763,0x27BE,0x281A,0x2876,0x28D2,0x292E,
    0x298B,0x29E7,0x2A44,0x2AA1,0x2AFF,0x2B5C,0x2BBA,0x2C18,
    0x2C76,0x2CD4,0x2D33,0x2D91,0x2DF0,0x2E4F,0x2EAE,0x2F0D,
    0x2F6C,0x2FCC,0x302B,0x308B,0x30EA,0x314A,0x31AA,0x3209,
    0x3269,0x32C9,0x3329,0x3389,0x33E9,0x3449,0x34A9,0x3509,
    0x3569,0x35C9,0x3629,0x3689,0x36E8,0x3748,0x37A8,0x3807,
    0x3867,0x38C6,0x3926,0x3985,0x39E4,0x3A43,0x3AA2,0x3B00,
    0x3B5F,0x3BBD,0x3C1B,0x3C79,0x3CD7,0x3D35,0x3D92,0x3DEF,
    0x3E4C,0x3EA9,0x3F05,0x3F62,0x3FBD,0x4019,0x4074,0x40D0,
    0x412A,0x4185,0x41DF,0x4239,0x4292,0x42EB,0x4344,0x439C,
    0x43F4,0x444C,0x44A3,0x44FA,0x4550,0x45A6,0x45FC,0x4651,
    0x46A6,0x46FA,0x474E,0x47A1,0x47F4,0x4846,0x4898,0x48E9,
    0x493A,0x498A,0x49D9,0x4A29,0x4A77,0x4AC5,0x4B13,0x4B5F,
    0x4BAC,0x4BF7,0x4C42,0x4C8D,0x4CD7,0x4D20,0x4D68,0x4DB0,
    0x4DF7,0x4E3E,0x4E84,0x4EC9,0x4F0E,0x4F52,0x4F95,0x4FD7,
    0x5019,0x505A,0x509A,0x50DA,0x5118,0x5156,0x5194,0x51D0,
    0x520C,0x5247,0x5281,0x52BA,0x52F3,0x532A,0x5361,0x5397,
    0x53CC,0x5401,0x5434,0x5467,0x5499,0x54CA,0x54FA,0x5529,
    0x5558,0x5585,0x55B2,0x55DE,0x5609,0x5632,0x565B,0x5684,
    0x56AB,0x56D1,0x56F6,0x571B,0x573E,0x5761,0x5782,0x57A3,
    0x57C3,0x57E2,0x57FF,0x581C,0x5838,0x5853,0x586D,0x5886,
    0x589E,0x58B5,0x58CB,0x58E0,0x58F4,0x5907,0x5919,0x592A,
    0x593A,0x5949,0x5958,0x5965,0x5971,0x597C,0x5986,0x598F,
    0x5997,0x599E,0x59A4,0x59A9,0x59AD,0x59B0,0x59B2,0x59B3
  };




/*********/
/* TIPUS */
/*********/

typedef struct
{
  uint16_t reg;
  int16_t  level;
  bool     changed;
  bool     sweep_mode;
  // Sweep
  bool     exp;
  bool     inc;
  int      sign;
  int      shift;
  int      step;
  int      wait;
  int      counter;
} volume_t;

typedef struct voice voice_t;

struct voice
{

  uint32_t mask_id; // Màscara identificadora. 1<<num_voice
  uint32_t start_addr;
  uint32_t repeat_addr;
  uint16_t sample_rate;
  uint32_t adsr_reg; // Per si es vol llegir.
  bool     use_noise;
  bool     use_reverb;
  struct
  {
    uint32_t current_addr; // Adreça del bloc descodificat.
    int      end_mode; // Que fer quan canviar de bloc.
    int16_t  *v; // Mostres decodificades, apunten a _v_mem[3]
    int16_t   v_mem[SAMPLES_PER_BLOCK+3]; // Els 3 primers guarden els
                                          // 3 últims d'avans.
    
    int16_t  older,old; // Valors anteriors (Quan resetejar???)
    
  } dec; // Descodificació ADPCM
  struct
  {
    voice_t  *mod; // Veu per a modular o NULL is no en té.
    uint32_t  counter;
    int16_t   out; // Última mostra
  } pit; // Pitch (Genera la mostra ADPCM a 44100Hz a partir de dec)
  struct
  {
    enum {
      STOP,
      ATTACK,
      DECAY,
      SUSTAIN,
      RELEASE
    }        mode;
    int      counter; // Cona cicles.
    int      wait; // Cicles a esperar.
    int16_t  level; // Nivell de volum.
    int16_t  real_level; // El que es gasta finalment.
    int16_t  out; // Mostra d'eixida actual.
    uint32_t rec_base_addr; // Buffer en RAM per a recording
    unsigned short rec_p; // Següent posició.
    // Attack.
    bool     att_exp;
    int      att_shift;
    int      att_step;
    // Decay.
    int      dec_shift;
    // Sustain.
    int16_t  sus_level;
    bool     sus_exp;
    bool     sus_inc;
    int      sus_shift;
    int      sus_step;
    // Release.
    bool     rel_exp;
    int      rel_shift;
  } adsr; // Mostres després d'haver aplicat ADSR
  struct
  {
    volume_t vol; // Envelope
    int16_t  out; // Exida
  } lr[2]; // Eixida final: 0 - Left; 1 - Right
  
};




/*********/
/* ESTAT */
/*********/

// Callbacks.
static PSX_PlaySound *_play_sound;
static PSX_Warning *_warning;
static void *_udata;

// Exida.
static struct
{
  int16_t v[PSX_AUDIO_BUFFER_SIZE*2];
  int     N;
} _out;

// Veus.
static voice_t _voices[24];

// Memòria.
static uint8_t _ram[RAM_SIZE];

// Registres globals.
static struct
{
  uint32_t pmon;
  uint32_t endx;
  uint32_t non; // noise ON/OFF.
  uint32_t eon; // echo ON/OFF (Reverb)
  uint32_t kon; // Key On
  uint32_t koff; // Key Off
  uint16_t unk_da0;
  uint16_t unk_dbc[2];
  uint16_t unk_e60[16];
} _regs;

// Soroll.
static struct
{
  int32_t timer;
  int     step;
  int     shift;
  int16_t out;
} _noise;

// CDrom.
static struct
{
  int16_t         vol_l;
  int16_t         vol_r;
  uint32_t        rec_base_addr_l; // Buffer en RAM per a recording
  uint32_t        rec_base_addr_r; // Buffer en RAM per a recording
  unsigned short  rec_p; // Següent posició.
  int16_t         out[2]; // 0 - left; 1 - right
} _cd;

// Main Volum i altres.
static struct
{

  volume_t l; // left
  volume_t r; // right
  int16_t  ext_l; // External ??
  int16_t  ext_r; // External ??
  
} _vol;

// Transferència.
static struct
{
  enum {
    STOP_IO= 0,
    MANUAL_WRITE= 1,
    DMA_WRITE= 2,
    DMA_READ= 3
  }        mode;
  int      transfer_type;
  uint16_t transfer_reg;
  uint16_t fifo[FIFO_SIZE];
  int      N; // Elements en la FIFO.
  uint16_t addr;
  uint32_t current_addr;
  bool     busy; // Ara mateix indica que en el següent cicle copiem
        	 // el FIFO a MEM. És una manera com un altar de fer
        	 // que no siga immediat la copia, però no sé si
        	 // m'estic passant de cicles o no aplegue.
} _io;

// Control/Estat.
static struct
{
  bool enabled; // SPU enabled (no afecta al CD)
  bool mute; // No afecta al CD
  bool reverb_master_enabled;
  bool irq_enabled;
  bool reverb_ext_enabled;
  bool reverb_cd_enabled;
  bool ext_enabled;
  bool cd_enabled;
  // Els bits 0-5 s'actualitzen més tart (en el STAT!!!)
  uint16_t reg;
  uint16_t reg_read; // Part baixa que es mostra en el STAT
} _stat;

// Interrupcions.
static struct
{
  bool     request;
  uint32_t addr;
  uint16_t addr16;
  uint16_t addr_reg;
} _int;

// Controla els cicles.
static struct
{
  int cc;
  int cc_used;
} _timing;

// Reverb.
static struct
{
  // Regs.
  int16_t  vlout;
  int16_t  vrout;
  uint16_t mbase;
  uint16_t regs[32];
  // Aux.
  uint32_t current_addr;
  uint32_t base_addr;
  int16_t  out[2]; // 0 - left; 1 - right
  int16_t  tmp_l,tmp_r; // Input and output tmps.
  int      step; // Switch 0(left)/1(right)
} _reverb;

static struct
{

  bool ready;
  int  p;
  int  N;
  
} _dma;




/***************/
/* DEFINICIONS */
/***************/

static void
adsr_release_init (
        	   voice_t *v
        	   );




/*********************/
/* FUNCIONS PRIVADES */
/*********************/

static void
set_int (void)
{

  if ( !_int.request && _stat.irq_enabled ) // ???
    {
      _int.request= true;
      PSX_int_interruption ( PSX_INT_SPU );
    }
  
} // end set_int


static void
volume_set_reg (
        	volume_t       *v,
        	const uint16_t  reg
        	)
{

  v->reg= reg;
  v->changed= true;
  
} // end volume_set_reg


// Es crida quan es va a executar un pas i ha canviat.
static void
volume_update (
               volume_t *v
               )
{
  
  static const int STEP_INC[]= { 7, 6, 5, 4 };
  static const int STEP_DEC[]= { -8, -7, -6, -5 };

  int tmp;
  
  
  v->sweep_mode= (v->reg&0x8000)!=0;
  if ( v->sweep_mode ) // Sweep
    {
      // Paràmetres.
      v->exp= (v->reg&0x4000)!=0;
      v->inc= (v->reg&0x2000)==0;
      v->sign= (v->reg&0x1000)!=0 ? -1 : 1;
      v->shift= (v->reg>>2)&0x1F;
      tmp= v->reg&0x3;
      v->step= v->inc ? STEP_INC[tmp] : STEP_DEC[tmp];
      // Comptadors.
      v->counter= 0;
      tmp= v->shift - 11; if ( tmp < 0 ) tmp= 0;
      v->wait= 1<<tmp;
      if ( v->exp && v->inc && v->level > 0x6000 )
        v->wait*= 4;
    }
  else v->level= (int16_t) (v->reg<<1);
  v->changed= false;
  
} // end volume_update


static void
volume_step (
             volume_t *v
             )
{
  
  int32_t tmp,step;


  if ( v->changed ) volume_update ( v );
  
  if ( !v->sweep_mode ) return;
  if ( ++(v->counter) != v->wait ) return;
  
  // Update wait i prepara step.
  v->counter= 0;
  tmp= v->shift - 11; if ( tmp < 0 ) tmp= 0;
  v->wait= 1<<tmp;
  tmp= 11 - v->shift; if ( tmp < 0 ) tmp= 0;
  step= v->step<<tmp;
  if ( v->exp )
    {
      if ( !v->inc ) step= MUL16TO32(step,v->level);
      else if ( v->level > 0x6000 ) v->wait*= 4;
    }
  
  // Actualitza.
  tmp= (((int32_t) v->level) + step)*v->sign;
  if ( tmp > 0x7FFF ) v->level= 0x7FFF;
  else if ( tmp < -0x8000 ) v->level= -0x8000;
  else v->level= (int16_t) tmp;
  
} // end volume_step


static void
decode_current_block (
        	      voice_t *v
        	      )
{

  // NOTA!! Abans ho tenia implementat gastant coma flotant. No crec
  // que estaguera mal, però no ix exactament que quan es gasta
  // sencers. Per poder depurar millor el codi, sobretot pensant en
  // comparar-me en mednafen, vaig a gastar sencers.
  /*
  static const double F0[]= { 0.0, 60/64.0, 115/64.0, 98/64.0, 122/64.0 };
  static const double F1[]= { 0.0, 0.0, -52/64.0, -55/64.0, -60/64.0 };
  */
  static const int32_t F0[]= { 0, 60, 115, 98, 122 };
  static const int32_t F1[]= { 0, 0, -52, -55, -60 };
  
  
  uint32_t addr;
  uint8_t val;
  int shift,filter,i;
  int16_t old,older,lo,hi,s;
  int32_t tmp,f0,f1;
  
  
  // Prepara.
  addr= (v->dec.current_addr&RAM_MASK);
  // -> Shift/filter
  if ( addr == _int.addr ) set_int ();
  val= _ram[addr]; addr= (addr+1)&RAM_MASK;
  shift= val&0xF; if ( shift >= 13 ) shift= 9;
  filter= (val>>4)&0x7; filter%= 5; // Que passa quan filter és 5, 6 o 7 ????
  for ( i= 0; i < 3; ++i )
    v->dec.v[i-3]= v->dec.v[SAMPLES_PER_BLOCK+(i-3)];
  
  // -> Flags
  if ( addr == _int.addr ) set_int ();
  val= _ram[addr]; addr= (addr+1)&RAM_MASK;
  v->dec.end_mode= val&0x3;
  if ( val&0x4 ) v->repeat_addr= v->dec.current_addr;
  
  // Descodifica.
  old= v->dec.old;
  older= v->dec.older;
  f0= F0[filter]; f1= F1[filter];
  for ( i= 0; i < SAMPLES_PER_BLOCK/2; ++i )
    {
      if ( addr == _int.addr ) set_int ();
      val= _ram[addr];
      lo= (int16_t) (val&0xF);
      hi= (int16_t) (val>>4);
      // Primera mostra.
      //tmp= (((int16_t) (lo<<12))>>shift) + (old*f0 + older*f1 + 0.5);
      tmp=
        (((int32_t) (int16_t) (lo<<12))>>shift) +
        ((((int32_t) old)*f0)>>6) +
        ((((int32_t) older)*f1)>>6);
      if ( tmp > 32767 ) s= 32767;
      else if ( tmp < -32768 ) s= -32768;
      else s= (int16_t) tmp;
      v->dec.v[2*i]= s; older= old; old= s;
      // Segona mostra.
      //tmp= (((int16_t) (hi<<12))>>shift) + (old*f0 + older*f1 + 0.5);
      tmp=
        (((int32_t) (int16_t) (hi<<12))>>shift) +
        ((((int32_t) old)*f0)>>6) +
        ((((int32_t) older)*f1)>>6);
      if ( tmp > 32767 ) s= 32767;
      else if ( tmp < -32768 ) s= -32768;
      else s= (int16_t) tmp;
      v->dec.v[2*i+1]= s; older= old; old= s;
      addr= (addr+1)&RAM_MASK;
    }
  v->dec.old= old;
  v->dec.older= older;
  
} // end decode_current_block


static void
finish_current_block (
        	      voice_t *v
        	      )
{

  switch ( v->dec.end_mode )
    {
    case 0: // continue at next 16-byte block
    case 2:
    default:
      v->dec.current_addr= (v->dec.current_addr+16)&RAM_MASK;
      break;
    case 1: // jump to Loop-address, set ENDX flag, Release, Env=0000h
      // NOTA!! Segons mednafen s'alinia al assignar.
      v->dec.current_addr= v->repeat_addr&(~0xF);
      v->adsr.level= v->adsr.real_level= 0;
      adsr_release_init ( v );
      _regs.endx|= v->mask_id;
      break;
    case 3: // jump to Loop-address, set ENDX flag
      // NOTA!! Segons mednafen s'alinia al assignar.
      v->dec.current_addr= v->repeat_addr&(~0xF);
      _regs.endx|= v->mask_id;
      break;
    }
  
} // end finish_current_block


static void
get_next_adpcm_sample (
        	       voice_t *v
        	       )
{

  uint32_t step;
  int32_t factor,tmp;
  int s,ss;
  
  
  // Actualitza comptador (Step= 0x1000 és una freq de 44100Hz).
  if ( v->pit.mod != NULL ) // Freqüència modulada per la veu anterior.
    {
      // NOTA!! La fórmula és lleugerament diferent a la de NOCASH,
      // gaste la de MEDNAFEN, que matemàticament és molt semblant
      // però es suposa que solventa algun tipo de bug.
      factor= ((int32_t) v->pit.mod->adsr.out); //+ 0x8000;
      // --> hardware glitch on VxPitch(sample_rate)>7FFFh, make sign
      step=
        (uint32_t)
        (
         ((int32_t) ((uint32_t) v->sample_rate)) +
         ((((int32_t) ((int16_t) (v->sample_rate))) * factor)>>15)
         );
      // --> hardware glitch on VxPitch>7FFFh, kill sign
      //step&= 0xFFFF;
    }
  else step= (uint32_t) v->sample_rate;
  if ( step > 0x3FFF ) step= 0x4000; // Segons NOCash cap a 0x4000
  v->pit.counter+= step;
  
  // Obté la següent mostra de dec.
  // NOTA: Com extraguem mostres a 44.1KHz, cada vegada que incrementa
  // 0x1000 canvíem de mostra.
  ss= (v->pit.counter&0xFF0)>>4; // Subsample, s'empra com a índex per
        			 // a interpolar.
  s= v->pit.counter>>12;
  
  // Descodifica blocs i reajusta counter si escau.
  while ( s >= SAMPLES_PER_BLOCK )
    {
      s-= SAMPLES_PER_BLOCK;
      v->pit.counter= (s<<12) | (v->pit.counter&0xFFF);
      finish_current_block ( v );
      decode_current_block ( v );
    }
  
  // Interpola amb els tres anteriors aplicant filtre gaussià.
  tmp= (int32_t) (GAUSS[0x0FF-ss]*((int32_t) v->dec.v[s-3]));
  tmp+= (int32_t) (GAUSS[0x1FF-ss]*((int32_t) v->dec.v[s-2]));
  tmp+= (int32_t) (GAUSS[0x100+ss]*((int32_t) v->dec.v[s-1]));
  tmp+= (int32_t) (GAUSS[0x000+ss]*((int32_t) v->dec.v[s]));
  tmp>>=15;
  v->pit.out= v->use_noise ? _noise.out : (int16_t) tmp;
  
} // get_next_adpcm_sample


static void
update_voice_adsr_values (
        		  voice_t *v
        		  )
{

  static const int ATT_STEP[]= { 7, 6, 5, 4 };
  static const int SUS_STEP_INC[]= { 7, 6, 5, 4 };
  static const int SUS_STEP_DEC[]= { -8, -7, -6, -5 };

  int32_t tmp;


  v->adsr.att_exp= (v->adsr_reg&0x8000)!=0;
  v->adsr.att_shift= (v->adsr_reg>>10)&0x1F;
  v->adsr.att_step= ATT_STEP[(v->adsr_reg>>8)&0x3];
  v->adsr.dec_shift= (v->adsr_reg>>4)&0xF;
  // Què passa amb sus_level==0xF ??? Ara he fet que siga màxim 0x7FFF
  tmp= ((v->adsr_reg&0xF)+1)*0x800;
  if ( tmp == 0x8000 ) tmp= 0x7FFF;
  v->adsr.sus_level= (int16_t) tmp;
  v->adsr.sus_exp= (v->adsr_reg&0x80000000)!=0;
  v->adsr.sus_inc= (v->adsr_reg&0x40000000)==0;
  v->adsr.sus_shift= (v->adsr_reg>>24)&0x1F;
  tmp= (v->adsr_reg>>22)&0x3;
  v->adsr.sus_step= v->adsr.sus_inc ? SUS_STEP_INC[tmp] : SUS_STEP_DEC[tmp];
  v->adsr.rel_exp= (v->adsr_reg&0x00200000)!=0;
  v->adsr.rel_shift= (v->adsr_reg>>16)&0x1F;
  
} // end update_voice_adsr_values


static void
adsr_release_step (
        	   voice_t *v
        	   )
{

  int32_t tmp,step;
  
  
  if ( ++(v->adsr.counter) != v->adsr.wait ) return;

  // Update wait i prepara step.
  v->adsr.counter= 0;
  tmp= v->adsr.rel_shift - 11; if ( tmp < 0 ) tmp= 0;
  v->adsr.wait= 1<<tmp;
  tmp= 11-v->adsr.rel_shift; if ( tmp < 0 ) tmp= 0;
  step= -8<<tmp;
  if ( v->adsr.rel_exp ) step= MUL16TO32(step,v->adsr.level);
  
  // Actualitza.
  tmp= ((int32_t) v->adsr.level) + step;
  if ( tmp > 0x7FFF ) v->adsr.level= 0x7FFF;
  else if ( tmp < 0 ) v->adsr.level= 0x0000;
  else v->adsr.level= (int16_t) tmp;
  v->adsr.real_level= v->adsr.level;

  // Actualitza mode.
  if ( v->adsr.level == 0x0000 ) v->adsr.mode= STOP;
  
} // end adsr_release_step


static void
adsr_release_init (
        	   voice_t *v
        	   )
{

  int tmp;


  v->adsr.counter= 0;
  tmp= v->adsr.rel_shift - 11; if ( tmp < 0 ) tmp= 0;
  v->adsr.wait= 1<<tmp;
  v->adsr.mode= RELEASE;
  
} // end adsr_release_init


static void
adsr_sustain_step (
        	   voice_t *v
        	   )
{

  int32_t tmp,step;
  
  
  if ( ++(v->adsr.counter) != v->adsr.wait ) return;
  
  // Update wait i prepara step.
  v->adsr.counter= 0;
  tmp= v->adsr.sus_shift - 11; if ( tmp < 0 ) tmp= 0;
  v->adsr.wait= 1<<tmp;
  tmp= 11-v->adsr.sus_shift; if ( tmp < 0 ) tmp= 0;
  step= v->adsr.sus_step<<tmp;
  if ( v->adsr.sus_exp )
    {
      if ( !v->adsr.sus_inc ) step= MUL16TO32(step,v->adsr.level);
      else if ( v->adsr.level > 0x6000 ) v->adsr.wait*= 4;
    }
  
  // Actualitza.
  tmp= ((int32_t) v->adsr.level) + step;
  if ( tmp > 0x7FFF ) v->adsr.level= 0x7FFF;
  else if ( tmp < 0 ) v->adsr.level= 0x0000;
  else v->adsr.level= (int16_t) tmp;
  v->adsr.real_level= v->adsr.level;
  
} // end adsr_sustain_step


static void
adsr_sustain_init (
        	   voice_t *v
        	   )
{

  int tmp;


  v->adsr.counter= 0;
  tmp= v->adsr.sus_shift - 11; if ( tmp < 0 ) tmp= 0;
  v->adsr.wait= 1<<tmp;
  if ( v->adsr.sus_exp && v->adsr.sus_inc && v->adsr.level > 0x6000 )
    v->adsr.wait*= 4;
  v->adsr.mode= SUSTAIN;
  
} // end adsr_sustain_init


static void
adsr_decay_step (
        	 voice_t *v
        	 )
{

  int32_t tmp,step;
  
  
  if ( ++(v->adsr.counter) != v->adsr.wait ) return;

  // Update wait i prepara step.
  v->adsr.counter= 0;
  tmp= v->adsr.dec_shift - 11; if ( tmp < 0 ) tmp= 0;
  v->adsr.wait= 1<<tmp;
  tmp= 11-v->adsr.dec_shift; if ( tmp < 0 ) tmp= 0;
  step= -8<<tmp;
  step= MUL16TO32(step,v->adsr.level); // <-- Dec-Exp
  
  // Actualitza.
  tmp= ((int32_t) v->adsr.level) + step;
  if ( tmp > 0x7FFF ) v->adsr.level= 0x7FFF;
  else if ( tmp < v->adsr.sus_level ) v->adsr.level= v->adsr.sus_level;
  else v->adsr.level= (int16_t) tmp;
  v->adsr.real_level= v->adsr.level;

  // Actualitza mode.
  if ( v->adsr.level == v->adsr.sus_level )
    adsr_sustain_init ( v );
  
} // end adsr_decay_step


static void
adsr_decay_init (
        	 voice_t *v
        	 )
{

  int tmp;


  v->adsr.counter= 0;
  tmp= v->adsr.dec_shift - 11; if ( tmp < 0 ) tmp= 0;
  v->adsr.wait= 1<<tmp;
  v->adsr.mode= DECAY;
  
} // end adsr_decay_init


static void
adsr_attack_step (
        	  voice_t *v
        	  )
{

  int32_t tmp,step;
  
  
  if ( ++(v->adsr.counter) != v->adsr.wait ) return;

  // Update wait i prepara step.
  v->adsr.counter= 0;
  tmp= v->adsr.att_shift - 11; if ( tmp < 0 ) tmp= 0;
  v->adsr.wait= 1<<tmp;
  tmp= 11-v->adsr.att_shift; if ( tmp < 0 ) tmp= 0;
  step= v->adsr.att_step<<tmp;
  if ( v->adsr.att_exp && v->adsr.level > 0x6000 ) v->adsr.wait*= 4;
  
  // Actualitza.
  tmp= ((int32_t) v->adsr.level) + step;
  if ( tmp > 0x7FFF ) v->adsr.level= 0x7FFF;
  else if ( tmp < 0 ) v->adsr.level= 0x0000;
  else v->adsr.level= (int16_t) tmp;
  v->adsr.real_level= v->adsr.level;

  // Actualitza mode.
  if ( v->adsr.level == 0x7FFF )
    {
      if ( v->adsr.sus_level == 0x7FFF ) adsr_sustain_init ( v );
      else adsr_decay_init ( v );
    }
  
} // end adsr_attack_step


static void
adsr_attack_init (
        	  voice_t *v
        	  )
{

  int tmp;


  v->adsr.counter= 0;
  tmp= v->adsr.att_shift - 11; if ( tmp < 0 ) tmp= 0;
  v->adsr.wait= 1<<tmp;
  if ( v->adsr.att_exp && v->adsr.level > 0x6000 ) v->adsr.wait*= 4;
  v->adsr.mode= ATTACK;

  // Init recording.
  v->adsr.rec_p= 0;
  
} // end adsr_attack_init


static void
get_next_adsr_sample (
        	      voice_t *v
        	      )
{

  uint32_t addr;

  
  // Actualitza level ADSR.
  switch ( v->adsr.mode )
    {
    default:
    case STOP: v->adsr.level= v->adsr.real_level= 0; break;
    case ATTACK: adsr_attack_step ( v ); break;
    case DECAY: adsr_decay_step ( v ); break;
    case SUSTAIN: adsr_sustain_step ( v ); break;
    case RELEASE: adsr_release_step ( v ); break;
    }
  
  // Obté la mostra i pondera.
  get_next_adpcm_sample ( v );
  v->adsr.out= MUL16(v->pit.out,v->adsr.real_level);
  
  // Recording.
  if ( v->adsr.rec_base_addr != 0xFFFF )
    {
      addr= v->adsr.rec_base_addr + ((v->adsr.rec_p++)&0x1FF);
      // --> ¿¿?? Capture IRQs do NOT occur if 1F801DACh.bit3-2 are both zero.
      if ( (_io.transfer_type&0x6)!=0 && addr == _int.addr16 ) set_int ();
      REC_BUF[addr]= v->adsr.out;
    }
  
} // end get_next_adsr_sample


// Desa els valors left/right en v->lr[0..1]
static void
get_next_voice_sample (
        	       voice_t *v
        	       )
{

  int i;

  
  get_next_adsr_sample ( v );
  for ( i= 0; i < 2; ++i )
    {
      volume_step ( &(v->lr[i].vol) );
      v->lr[i].out= MUL16(v->adsr.out,v->lr[i].vol.level);
    }
  
} // end get_next_voice_sample


static void
get_next_noise_sample (void)
{

  int parity_bit;

  
  _noise.timer-= _noise.step;
  parity_bit=
    ((_noise.out>>15)^(_noise.out>>12)^(_noise.out>>11)^(_noise.out>>10)^1)&0x1;
  if ( _noise.timer < 0 )
    {
      _noise.out= _noise.out*2 + parity_bit;
      _noise.timer+= (0x20000>>_noise.shift);
      if ( _noise.timer < 0 )
        _noise.timer+= (0x20000>>_noise.shift);
    }
  
} // end get_next_noise_sample


static void
fifo2ram (void)
{

  const int MASK= (RAM_SIZE>>1)-1;
  
  uint32_t addr;
  uint16_t *ram;
  int n;

  
  _io.busy= false;
  // NOTA: Com que addr sempre és fica com un valor*8, i incrementem
  // d'un en un, assegurem que a nivell de halfworld sempre etarà
  // aliniada.
  addr= _io.current_addr>>1;
  ram= (uint16_t *) &(_ram[0]);
  switch ( _io.transfer_type )
    {
    case 2: // Normal
      for ( n= 0; n < _io.N; ++n )
        {
          if ( addr == _int.addr16 ) set_int ();
          ram[addr]= _io.fifo[n];
          addr= (addr+1)&MASK;
        }
      break;
    case 3: // Rep2
      for ( n= 0; n < _io.N; ++n )
        {
          if ( addr == _int.addr16 ) set_int ();
          ram[addr]= _io.fifo[n&0x1E];
          addr= (addr+1)&MASK;
        }
      break;
    case 4: // Rep4
      for ( n= 0; n < _io.N; ++n )
        {
          if ( addr == _int.addr16 ) set_int ();
          ram[addr]= _io.fifo[n&0x1C];
          addr= (addr+1)&MASK;
        }
      break;
    case 5: // Rep8
      for ( n= 0; n < _io.N; ++n )
        {
          if ( addr == _int.addr16 ) set_int ();
          ram[addr]= _io.fifo[(n&0x18)+7];
          addr= (addr+1)&MASK;
        }
      break;
    default: // Fill
      for ( n= 0; n < _io.N; ++n )
        {
          if ( addr == _int.addr16 ) set_int ();
          ram[addr]= _io.fifo[_io.N-1];
          addr= (addr+1)&MASK;
        }
    }
  _io.current_addr= addr<<1;
  _io.N= 0;
  
} // end fifo2ram


static void
ram2fifo (void)
{

  const int MASK= (RAM_SIZE>>1)-1;
  
  uint32_t addr;
  uint16_t *ram;
  int n;

  
  // NOTA: Com que addr sempre és fica com un valor*8, i incrementem
  // d'un en un, assegurem que a nivell de halfworld sempre etarà
  // aliniada.
  addr= _io.current_addr>>1;
  ram= (uint16_t *) &(_ram[0]);
  _io.N= 0; // ???????
  switch ( _io.transfer_type )
    {
    default: // Fill????
    case 2: // Normal
      for ( n= 0; n < FIFO_SIZE; ++n )
        {
          if ( addr == _int.addr16 ) set_int ();
          _io.fifo[n]= ram[addr];
          addr= (addr+1)&MASK;
        }
      break;
    case 3: // Rep2
      for ( n= 0; n < FIFO_SIZE; ++n )
        {
          if ( addr == _int.addr16 ) set_int ();
          _io.fifo[n]= ram[addr&(~0x1)];
          addr= (addr+1)&MASK;
        }
      break;
    case 4: // Rep4
      for ( n= 0; n < FIFO_SIZE; ++n )
        {
          if ( addr == _int.addr16 ) set_int ();
          _io.fifo[n]= ram[addr&(~0x3)];
          addr= (addr+1)&MASK;
        }
      break;
    case 5: // Rep8
      for ( n= 0; n < FIFO_SIZE; ++n )
        {
          if ( addr == _int.addr16 ) set_int ();
          _io.fifo[n]= ram[addr&(~0x7)];
          addr= (addr+1)&MASK;
        }
      break;
    }
  _io.current_addr= addr<<1;
  
} // end ram2fifo


static void
io_set_mode (
             const int mode
             )
{
  
  _io.mode= mode;
  switch ( _io.mode )
    {
    case STOP_IO:
      _io.N= 0;
      _io.busy= false;
      break;
    case MANUAL_WRITE:
      fifo2ram (); // <--- Res de busy !!!!
      _io.busy= false;
      //_io.busy= true;
      break;
    case DMA_READ:
      _io.busy= false;
      ram2fifo ();
      break;
    case DMA_WRITE:
    default: break;
    }
  
} // end io_set_mode


#define VOL(REG) I16(_reverb.regs[(REG)])
#define DISP(REG) I16(_reverb.regs[(REG)]) /* Signed???????? */
#define SRC_DST(REG) (_reverb.current_addr + (U32(_reverb.regs[(REG)])<<3))

#define dAPF1   DISP(0x00)
#define dAPF2   DISP(0x01)
#define vIIR    VOL(0x02)
#define vCOMB1  VOL(0x03)
#define vCOMB2  VOL(0x04)
#define vCOMB3  VOL(0x05)
#define vCOMB4  VOL(0x06)
#define vWALL   VOL(0x07)
#define vAPF1   VOL(0x08)
#define vAPF2   VOL(0x09)
#define mLSAME  SRC_DST(0x0A)
#define mRSAME  SRC_DST(0x0B)
#define mLCOMB1 SRC_DST(0x0C)
#define mRCOMB1 SRC_DST(0x0D)
#define mLCOMB2 SRC_DST(0x0E)
#define mRCOMB2 SRC_DST(0x0F)
#define dLSAME  SRC_DST(0x10)
#define dRSAME  SRC_DST(0x11)
#define mLDIFF  SRC_DST(0x12)
#define mRDIFF  SRC_DST(0x13)
#define mLCOMB3 SRC_DST(0x14)
#define mRCOMB3 SRC_DST(0x15)
#define mLCOMB4 SRC_DST(0x16)
#define mRCOMB4 SRC_DST(0x17)
#define dLDIFF  SRC_DST(0x18)
#define dRDIFF  SRC_DST(0x19)
#define mLAPF1  SRC_DST(0x1A)
#define mRAPF1  SRC_DST(0x1B)
#define mLAPF2  SRC_DST(0x1C)
#define mRAPF2  SRC_DST(0x1D)
#define vLIN    VOL(0x1E)
#define vRIN    VOL(0x1F)

#define CALC_ADDR(VAR,ADDR,TMP)        					\
  (TMP)= (ADDR)&0x7FFFE;        					\
  (VAR)= ((TMP)<_reverb.base_addr) ?        				\
    (_reverb.base_addr + (TMP)) /*¿¿???Cosa meua la suma*/ : (TMP);        \
  if ( (VAR) == _int.addr ) set_int ()

#define RAM16(ADDR) (((int16_t *) &(_ram[0]))[(ADDR)>>1])

static void
reverb_step_left (void)
{

  int32_t lin,lout,aux;
  uint32_t tmp,dlsame_p,mlsame_p,mlsame_2_p,drdiff_p,mldiff_2_p,mldiff_p,
    mlcomb1_p,mlcomb2_p,mlcomb3_p,mlcomb4_p,mlapf1_dapf1_p,mlapf1_p,
    mlapf2_dapf2_p,mlapf2_p;

  
  // Input.
  lin= MUL16TO32(vLIN,_reverb.tmp_l);

  // Same Side Reflection (left-to-left)
  CALC_ADDR ( dlsame_p, dLSAME, tmp );
  CALC_ADDR ( mlsame_2_p, mLSAME-2, tmp );
  if ( _stat.reverb_master_enabled )
    {
      aux= lin + MUL16TO32(RAM16(dlsame_p),vWALL) - I32(RAM16(mlsame_2_p));
      aux= MUL3216(aux,vIIR) + I32(RAM16(mlsame_2_p));
      CALC_ADDR ( mlsame_p, mLSAME, tmp );
      RAM16(mlsame_p)= TOVOL(aux);
    }

  // Different Side Reflection (right-to-left)
  CALC_ADDR ( drdiff_p, dRDIFF, tmp );
  CALC_ADDR ( mldiff_2_p, mLDIFF-2, tmp );
  if ( _stat.reverb_master_enabled )
    {
      aux= lin + MUL16TO32(RAM16(drdiff_p),vWALL) - I32(RAM16(mldiff_2_p));
      aux= MUL3216(aux,vIIR) + I32(RAM16(mldiff_2_p));
      CALC_ADDR ( mldiff_p, mLDIFF, tmp );
      RAM16(mldiff_p)= TOVOL(aux);
    }

  // Early Echo (Comb Filter, with input from buffer)
  CALC_ADDR ( mlcomb1_p, mLCOMB1, tmp );
  CALC_ADDR ( mlcomb2_p, mLCOMB2, tmp );
  CALC_ADDR ( mlcomb3_p, mLCOMB3, tmp );
  CALC_ADDR ( mlcomb4_p, mLCOMB4, tmp );
  lout= MUL16TO32(RAM16(mlcomb1_p),vCOMB1)
    +  MUL16TO32(RAM16(mlcomb2_p),vCOMB2)
    +  MUL16TO32(RAM16(mlcomb3_p),vCOMB3)
    +  MUL16TO32(RAM16(mlcomb4_p),vCOMB4);
  
  // Late Reverb APF1 (All Pass Filter 1, with input from COMB)
  CALC_ADDR ( mlapf1_p, mLAPF1, tmp );
  CALC_ADDR ( mlapf1_dapf1_p, mLAPF1-dAPF1, tmp );
  if ( _stat.reverb_master_enabled )
    {
      aux= lout - MUL16TO32(vAPF1,RAM16(mlapf1_dapf1_p));
      RAM16(mlapf1_p)= TOVOL(aux);
    }
  lout= I32(RAM16(mlapf1_dapf1_p)) + MUL16TO32(RAM16(mlapf1_p),vAPF1);

  // Late Reverb APF2 (All Pass Filter 2, with input from APF1)
  CALC_ADDR ( mlapf2_p, mLAPF2, tmp );
  CALC_ADDR ( mlapf2_dapf2_p, mLAPF2-dAPF2, tmp );
  if ( _stat.reverb_master_enabled )
    {
      aux= lout - MUL16TO32(vAPF2,RAM16(mlapf2_dapf2_p));
      RAM16(mlapf2_p)= TOVOL(aux);
    }
  lout= I32(RAM16(mlapf2_dapf2_p)) + MUL16TO32(RAM16(mlapf2_p),vAPF2);
  
  // Finish.
  _reverb.tmp_l= TOVOL(lout);
  
} // end reverb_step_left


static void
reverb_step_right (void)
{

  int32_t rin,rout,aux;
  uint32_t tmp,drsame_p,mrsame_p,mrsame_2_p,dldiff_p,mrdiff_2_p,mrdiff_p,
    mrcomb1_p,mrcomb2_p,mrcomb3_p,mrcomb4_p,mrapf1_dapf1_p,mrapf1_p,
    mrapf2_dapf2_p,mrapf2_p;

  
  // Input.
  rin= MUL16TO32(vRIN,_reverb.tmp_r);

  // Same Side Reflection (right-to-right)
  CALC_ADDR ( drsame_p, dRSAME, tmp );
  CALC_ADDR ( mrsame_2_p, mRSAME-2, tmp );
  if ( _stat.reverb_master_enabled )
    {
      aux= rin + MUL16TO32(RAM16(drsame_p),vWALL) - I32(RAM16(mrsame_2_p));
      aux= MUL3216(aux,vIIR) + I32(RAM16(mrsame_2_p));
      CALC_ADDR ( mrsame_p, mRSAME, tmp );
      RAM16(mrsame_p)= TOVOL(aux);
    }

  // Different Side Reflection (left-to-right)
  CALC_ADDR ( dldiff_p, dLDIFF, tmp );
  CALC_ADDR ( mrdiff_2_p, mRDIFF-2, tmp );
  if ( _stat.reverb_master_enabled )
    {
      aux= rin + MUL16TO32(RAM16(dldiff_p),vWALL) - I32(RAM16(mrdiff_2_p));
      aux= MUL3216(aux,vIIR) + I32(RAM16(mrdiff_2_p));
      CALC_ADDR ( mrdiff_p, mRDIFF, tmp );
      RAM16(mrdiff_p)= TOVOL(aux);
    }

  // Early Echo (Comb Filter, with input from buffer)
  CALC_ADDR ( mrcomb1_p, mRCOMB1, tmp );
  CALC_ADDR ( mrcomb2_p, mRCOMB2, tmp );
  CALC_ADDR ( mrcomb3_p, mRCOMB3, tmp );
  CALC_ADDR ( mrcomb4_p, mRCOMB4, tmp );
  rout= MUL16TO32(RAM16(mrcomb1_p),vCOMB1)
    +  MUL16TO32(RAM16(mrcomb2_p),vCOMB2)
    +  MUL16TO32(RAM16(mrcomb3_p),vCOMB3)
    +  MUL16TO32(RAM16(mrcomb4_p),vCOMB4);
  
  // Late Reverb APF1 (All Pass Filter 1, with input from COMB)
  CALC_ADDR ( mrapf1_p, mRAPF1, tmp );
  CALC_ADDR ( mrapf1_dapf1_p, mRAPF1-dAPF1, tmp );
  if ( _stat.reverb_master_enabled )
    {
      aux= rout - MUL16TO32(vAPF1,RAM16(mrapf1_dapf1_p));
      RAM16(mrapf1_p)= TOVOL(aux);
    }
  rout= I32(RAM16(mrapf1_dapf1_p)) + MUL16TO32(RAM16(mrapf1_p),vAPF1);

  // Late Reverb APF2 (All Pass Filter 2, with input from APF1)
  CALC_ADDR ( mrapf2_p, mRAPF2, tmp );
  CALC_ADDR ( mrapf2_dapf2_p, mRAPF2-dAPF2, tmp );
  if ( _stat.reverb_master_enabled )
    {
      aux= rout - MUL16TO32(vAPF2,RAM16(mrapf2_dapf2_p));
      RAM16(mrapf2_p)= TOVOL(aux);
    }
  rout= I32(RAM16(mrapf2_dapf2_p)) + MUL16TO32(RAM16(mrapf2_p),vAPF2);
  
  // Finish.
  _reverb.tmp_r= TOVOL(rout);
  
} // end reverb_step_right


static void
reverb_step (
             const int16_t l,
             const int16_t r
             )
{

  if ( _reverb.step ) // Right
    {
      reverb_step_right ();
      _reverb.out[0]= MUL16(_reverb.tmp_l,_reverb.vlout);
      _reverb.out[1]= MUL16(_reverb.tmp_r,_reverb.vrout);
      _reverb.current_addr= (_reverb.current_addr+2)&0x7FFFE;
      if ( _reverb.current_addr < _reverb.base_addr )
        _reverb.current_addr= _reverb.base_addr;
    }
  else // Left
    {
      _reverb.tmp_l= l;
      _reverb.tmp_r= r;
      reverb_step_left ();
    }
  _reverb.step^= 1;
  
} // end reverb_step


static void
get_next_cd_sample (void)
{

  uint32_t addr;
  int16_t l,r;
  
  
  // LLig les mostres.
  PSX_cd_next_sound_sample ( &l, &r );
  
  // Left.
  _cd.out[0]= MUL16(l,_cd.vol_l);
  addr= _cd.rec_base_addr_l + ((_cd.rec_p)&0x1FF);
  // --> ¿¿?? Capture IRQs do NOT occur if 1F801DACh.bit3-2 are both zero.
  if ( (_io.transfer_type&0x6)!=0 && addr == _int.addr16 ) set_int ();
  REC_BUF[addr]= l;

  // Right.
  _cd.out[1]= MUL16(r,_cd.vol_r);
  addr= _cd.rec_base_addr_r + ((_cd.rec_p)&0x1FF);
  // --> ¿¿?? Capture IRQs do NOT occur if 1F801DACh.bit3-2 are both zero.
  if ( (_io.transfer_type&0x6)!=0 && addr == _int.addr16 ) set_int ();
  REC_BUF[addr]= r;

  // Actualitza posició.
  ++_cd.rec_p;
  
} // end get_next_cd_sample


static void
run_sample (void)
{

  int n,c;
  int32_t tmp_out[2],tmp_reverb[2];
  const voice_t *voice;
  
  
  // NOTA!! Vaig a fer que el enable sols afecte al volum i al IRQ,
  // però que no pare res.

  // Preeliminars.
  // --> Copia FIFO a RAM pendents.
  if ( _io.busy ) fifo2ram (); // <-- ARA MATEIXA DESACTIVAT, NO HI HA BUSY
  // --> Bits 0-5 SPUCNT s'actualitzen.
  _stat.reg_read= _stat.reg&0x3F;
  
  // Executa la generació de mostres.
  get_next_noise_sample ();
  for ( n= 0; n < 24; ++n )
    get_next_voice_sample ( &(_voices[n]) );
  get_next_cd_sample ();
  volume_step ( &_vol.l );
  volume_step ( &_vol.r );
  
  // Mixer.
  for ( c= 0; c < 2; ++c )
    {
      
      // Init.
      tmp_out[c]= tmp_reverb[c]= 0;
      
      // Veus.
      if ( _stat.enabled && !_stat.mute )
        for ( n= 0; n < 24; ++n )
          {
            voice= &(_voices[n]);
            tmp_out[c]+= I32(voice->lr[c].out);
            if ( voice->use_reverb )
              tmp_reverb[c]+= I32(voice->lr[c].out);
          }
      
      // CDRom
      if ( _stat.cd_enabled )
        {
          tmp_out[c]+= I32(_cd.out[c]);
          if ( _stat.reverb_cd_enabled )
            tmp_reverb[c]+= I32(_cd.out[c]);
        }
        
      // Extern ???????
    }

  // Reverberació.
  reverb_step ( TOVOL(tmp_reverb[0]), TOVOL(tmp_reverb[1]) );
  tmp_out[0]+= I32(_reverb.out[0]);
  tmp_out[1]+= I32(_reverb.out[1]);
  
  // Volum i ompli el buffer.
  tmp_out[0]= MUL3216(tmp_out[0],_vol.l.level);
  tmp_out[1]= MUL3216(tmp_out[1],_vol.r.level);
  _out.v[_out.N*2]= TOVOL(tmp_out[0]);
  _out.v[_out.N*2+1]= TOVOL(tmp_out[1]);
  
  if ( ++_out.N == PSX_AUDIO_BUFFER_SIZE )
    {
      _play_sound ( _out.v, _udata );
      _out.N= 0;
    }
  
} // end run_sample


static void
clock (void)
{

  int nsamples,n,cc,tmp;


  cc= PSX_Clock-_timing.cc_used;
  if ( cc > 0 ) { _timing.cc+= cc; _timing.cc_used+= cc; }
  
  nsamples= _timing.cc/CCPERSAMPLE;
  _timing.cc%= CCPERSAMPLE;
  for ( n= 0; n < nsamples; ++n )
    run_sample ();

  tmp= PSX_Clock + PSX_spu_next_event_cc ();
  if ( tmp < PSX_NextEventCC )
    PSX_NextEventCC= tmp;
  
} // end clock




/**********************/
/* FUNCIONS PÚBLIQUES */
/**********************/

void
PSX_spu_end_iter (void)
{

  int cc;


  cc= PSX_Clock-_timing.cc_used;
  if ( cc > 0 )
    {
      _timing.cc+= cc;
      _timing.cc_used+= cc;
      if ( _timing.cc >= CCPERSAMPLE )
        clock ();
    }
  _timing.cc_used= 0;
  
} // end PSX_spu_end_iter


int
PSX_spu_next_event_cc (void)
{

  int ret;
  
  
  ret= CCPERSAMPLE - _timing.cc;
  assert ( ret >= 0 );
  
  return ret;
  
} // end PSX_spu_next_event_cc


void
PSX_spu_init (
              PSX_PlaySound *play_sound,
              PSX_Warning   *warning,
              void          *udata
              )
{

  int i;

  
  // Callbacks.
  _play_sound= play_sound;
  _warning= warning;
  _udata= udata;

  // Inicialitza memòria.
  memset ( _ram, 0, sizeof(_ram) );
  memset ( &_regs, 0, sizeof(_regs) );
  memset ( _voices, 0, sizeof(_voices) );
  for ( i= 0; i < 24; ++i )
    {
      _voices[i].dec.v= &(_voices[i].dec.v_mem[3]);
      _voices[i].mask_id= 1<<i;
      _regs.endx|= _voices[i].mask_id;
      decode_current_block ( &_voices[i] );
      _voices[i].pit.mod= NULL;
      update_voice_adsr_values ( &_voices[i] );
      _voices[i].adsr.rec_base_addr= 0xFFFF;
    }
  _voices[0].adsr.rec_base_addr= (0x800>>1);
  _voices[2].adsr.rec_base_addr= (0xC00>>1);
  memset ( &_noise, 0, sizeof(_noise) );
  memset ( &_vol, 0, sizeof(_vol) );
  memset ( &_stat, 0, sizeof(_stat) );
  memset ( &_io, 0, sizeof(_io) );
  memset ( &_reverb, 0, sizeof(_reverb) );
  memset ( &_int, 0, sizeof(_int) );
  memset ( &_cd, 0, sizeof(_cd) );
  _cd.rec_base_addr_l= (0x000>>1);
  _cd.rec_base_addr_r= (0x400>>1);
  
  // Timing
  _timing.cc= 0;
  _timing.cc_used= 0;

  // Eixida.
  _out.N= 0;

  // DMA.
  _dma.ready= false;
  _dma.p= 0;
  _dma.N= 0;
  
} // end PSX_spu_init


uint16_t
PSX_spu_voice_get_start_addr (
        		      const int voice
        		      )
{

  clock ();

  return (uint16_t) (_voices[voice].start_addr>>3);
  
} // end PSX_spu_voice_get_start_addr


void
PSX_spu_voice_set_start_addr (
        		      const int      voice,
        		      const uint16_t val
        		      )
{

  clock ();
  _voices[voice].start_addr= ((uint32_t) val)<<3;
  
} // end PSX_spu_voice_set_start_addr


uint16_t
PSX_spu_voice_get_repeat_addr (
        		       const int voice
        		       )
{

  clock ();

  return (uint16_t) (_voices[voice].repeat_addr>>3);
  
} // end PSX_spu_voice_get_repeat_addr


void
PSX_spu_voice_set_repeat_addr (
        		       const int      voice,
        		       const uint16_t val
        		       )
{

  clock ();
  _voices[voice].repeat_addr= ((uint32_t) val)<<3;
  
} // end PSX_spu_voice_set_repear_addr


uint16_t
PSX_spu_voice_get_sample_rate (
        		       const int voice
        		       )
{
  return _voices[voice].sample_rate;
} // end PSX_spu_voice_get_sample_rate


void
PSX_spu_voice_set_sample_rate (
        		       const int      voice,
        		       const uint16_t val
        		       )
{

  clock ();
  
  _voices[voice].sample_rate= val;
  
} // end PSX_spu_voice_set_sample_rate


void
PSX_spu_set_pmon_lo (
        	     const uint16_t data
        	     )
{

  int i;
  uint16_t sel;
  
  
  clock ();
  
  _regs.pmon= (_regs.pmon&0xFFFF0000) | ((uint32_t) data);
  sel= 0x0002;
  for ( i= 1; i < 16; ++i )
    {
      _voices[i].pit.mod= (data&sel) ? &(_voices[i-1]) : NULL;
      sel<<= 1;
    }
  
} // end PSX_spu_set_pmon_lo


void
PSX_spu_set_pmon_up (
        	     const uint16_t data
        	     )
{

  int i;
  uint16_t sel;
  
  
  clock ();
  
  _regs.pmon= (_regs.pmon&0x0000FFFF) | (((uint32_t) data)<<16);
  sel= 0x0001;
  for ( i= 16; i < 24; ++i )
    {
      _voices[i].pit.mod= (data&sel) ? &(_voices[i-1]) : NULL;
      sel<<= 1;
    }
  
} // end PSX_spu_set_pmon_up


uint32_t
PSX_spu_get_pmon (void)
{
  return _regs.pmon;
} // end PSX_spu_get_pmon


void
PSX_spu_voice_set_adsr_lo (
        		   const int      voice,
        		   const uint16_t val
        		   )
{

  clock ();

  _voices[voice].adsr_reg=
    (_voices[voice].adsr_reg&0xFFFF0000) |
    (uint32_t) val;
  // --> He decidit fer-ho amb el ON.
  //update_voice_adsr_values ( &_voices[voice] );
  
} // end PSX_spu_voice_set_adsr_lo


void
PSX_spu_voice_set_adsr_up (
        		   const int      voice,
        		   const uint16_t val
        		   )
{

  clock ();

  _voices[voice].adsr_reg=
    (_voices[voice].adsr_reg&0x0000FFFF) |
    (((uint32_t) val)<<16);
  // --> He decidit fer-ho amb el ON.
  //update_voice_adsr_values ( &_voices[voice] );
  
} // end PSX_spu_voice_set_adsr_up


uint32_t
PSX_spu_voice_get_adsr (
        		const int voice
        		)
{
  return _voices[voice].adsr_reg;
} // end PSX_spu_voice_get_adsr


void
PSX_spu_key_on_lo (
        	   const uint16_t data
        	   )
{

  int i;
  uint16_t sel;
  voice_t *v;

  
  clock ();

  _regs.kon= (_regs.kon&0xFFFF0000) | ((uint32_t) data);
  sel= 0x0001;
  for ( i= 0; i < 16; ++i )
    {
      if ( data&sel )
        {
          v= &(_voices[i]);
          // -> Decoder
          // NOTA!! Segons mednafen s'alinia.
          v->dec.current_addr= v->start_addr&(~0xF);
          v->dec.older= v->dec.old= 0; // ????????????
          decode_current_block ( v );
          // -> Pitch/ADPCM
          v->pit.counter= 0;
          // -> ADSR
          update_voice_adsr_values ( v ); // ????
          v->adsr.level= v->adsr.real_level= 0;
          adsr_attack_init ( v );
          v->adsr.rec_p= 0;
          // -> ENDX
          _regs.endx&= ~(v->mask_id);
        }
      sel<<= 1;
    }
  
} // end PSX_spu_key_on_lo


void
PSX_spu_key_on_up (
        	   const uint16_t data
        	   )
{

  int i;
  uint16_t sel;
  voice_t *v;

  
  clock ();

  _regs.kon= (_regs.kon&0x0000FFFF) | (((uint32_t) data)<<16);
  sel= 0x0001;
  for ( i= 16; i < 24; ++i )
    {
      if ( data&sel )
        {
          v= &(_voices[i]);
          // -> Decoder
          v->dec.current_addr= v->start_addr&(~0xF);
          v->dec.older= v->dec.old= 0; // ????????????
          decode_current_block ( v );
          // -> Pitch/ADPCM
          v->pit.counter= 0;
          // -> ADSR
          update_voice_adsr_values ( v ); // ????
          v->adsr.level= v->adsr.real_level= 0;
          adsr_attack_init ( v );
          v->adsr.rec_p= 0;
          // -> ENDX
          _regs.endx&= ~(v->mask_id);
        }
      sel<<= 1;
    }
  
} // end PSX_spu_key_on_up


void
PSX_spu_key_off_lo (
        	    const uint16_t data
        	    )
{

  int i;
  uint16_t sel;
  voice_t *v;

  
  clock ();

  _regs.koff= (_regs.koff&0xFFFF0000) | ((uint32_t) data);
  sel= 0x0001;
  for ( i= 0; i < 16; ++i )
    {
      if ( data&sel )
        {
          v= &(_voices[i]);
          if ( v->adsr.mode != STOP ) adsr_release_init ( v );
          _regs.endx|= v->mask_id; // ????????
        }
      sel<<= 1;
    }
  
} // end PSX_spu_key_off_lo


void
PSX_spu_key_off_up (
        	    const uint16_t data
        	    )
{

  int i;
  uint16_t sel;
  voice_t *v;

  
  clock ();

  _regs.koff= (_regs.koff&0x0000FFFF) | (((uint32_t) data)<<16);
  sel= 0x0001;
  for ( i= 16; i < 24; ++i )
    {
      if ( data&sel )
        {
          v= &(_voices[i]);
          if ( v->adsr.mode != STOP ) adsr_release_init ( v );
          _regs.endx|= v->mask_id; // ????????
        }
      sel<<= 1;
    }
  
} // end PSX_spu_key_off_up


uint32_t
PSX_spu_get_kon (void)
{
  return _regs.kon;
} // end PSX_spu_get_kon


uint32_t
PSX_spu_get_koff (void)
{
  return _regs.koff;
} // end PSX_spu_get_koff


uint32_t
PSX_spu_get_endx (void)
{

  clock ();

  return _regs.endx;
  
} // end PSX_spu_get_endx


void
PSX_spu_set_endx_lo (
                     const uint16_t data
                     )
{

  clock ();
  
  _regs.endx= (_regs.endx&0xFFFF0000) | ((uint32_t) data);
  
} // end PSX_spu_set_endx_lo


void
PSX_spu_set_endx_up (
                     const uint16_t data
                     )
{

  clock ();
  
  _regs.endx= (_regs.endx&0x0000FFFF) | (((uint32_t) data)<<16);
  
} // end PSX_spu_set_endx_up


void
PSX_spu_set_non_lo (
        	    const uint16_t data
        	    )
{
  
  int i;
  uint16_t sel;
  
  
  clock ();
  
  _regs.non= (_regs.non&0xFFFF0000) | ((uint32_t) data);
  sel= 0x0001;
  for ( i= 0; i < 16; ++i )
    {
      _voices[i].use_noise= (data&sel)!=0;
      sel<<= 1;
    }
  
} // end PSX_spu_set_non_lo


void
PSX_spu_set_non_up (
        	    const uint16_t data
        	    )
{
  
  int i;
  uint16_t sel;
  
  
  clock ();
  
  _regs.non= (_regs.non&0x0000FFFF) | (((uint32_t) data)<<16);
  sel= 0x0001;
  for ( i= 16; i < 24; ++i )
    {
      _voices[i].use_noise= (data&sel)!=0;
      sel<<= 1;
    }
  
} // end PSX_spu_set_non_up


uint32_t
PSX_spu_get_non (void)
{
  return _regs.non;
} // end PSX_spu_get_non


void
PSX_spu_voice_set_cur_vol (
        		   const int      voice,
        		   const uint16_t val
        		   )
{

  clock ();

  _voices[voice].adsr.real_level= (int16_t) val;
  
} // end PSX_spu_voice_set_cur_vol


uint16_t
PSX_spu_voice_get_cur_vol (
        		   const int voice
        		   )
{

  clock ();

  return (uint16_t)_voices[voice].adsr.real_level;
  
} // end PSX_spu_voice_get_cur_vol


void
PSX_spu_voice_set_left_vol (
        		    const int      voice,
        		    const uint16_t val
        		    )
{

  clock ();

  volume_set_reg ( &(_voices[voice].lr[0].vol), val );
  
} // end PSX_spu_voice_set_left_vol


void
PSX_spu_voice_set_right_vol (
        		     const int      voice,
        		     const uint16_t val
        		     )
{

  clock ();

  volume_set_reg ( &(_voices[voice].lr[1].vol), val );
  
} // end PSX_spu_voice_set_right_vol


uint16_t
PSX_spu_voice_get_left_vol (
        		    const int voice
        		    )
{
  return _voices[voice].lr[0].vol.reg;
} // end PSX_spu_voice_get_left_vol


uint16_t
PSX_spu_voice_get_right_vol (
        		     const int voice
        		     )
{
  return _voices[voice].lr[1].vol.reg;
} // end PSX_spu_voice_get_right_vol


uint32_t
PSX_spu_voice_get_cur_vol_lr (
        		      const int voice
        		      )
{

  clock ();

  return
    ((uint16_t) _voices[voice].lr[0].vol.level) |
    (((uint16_t) _voices[voice].lr[1].vol.level)<<16);
  
} // end PSX_spu_voice_get_cur_vol_lr


void
PSX_spu_set_left_vol (
        	      const uint16_t data
        	      )
{

  clock ();

  volume_set_reg ( &_vol.l, data );
  
} // end PSX_spu_set_left_vol


void
PSX_spu_set_right_vol (
        	       const uint16_t data
        	       )
{

  clock ();

  volume_set_reg ( &_vol.r, data );
  
} // end PSX_spu_set_right_vol


uint16_t
PSX_spu_get_left_vol (void)
{
  return _vol.l.reg;
} // end PSX_spu_get_left_vol


uint16_t
PSX_spu_get_right_vol (void)
{
  return _vol.r.reg;
} // end PSX_spu_get_right_vol


uint32_t
PSX_spu_get_cur_vol_lr (void)
{
  
  clock ();
  
  return
    ((uint16_t) _vol.l.level) |
    (((uint16_t) _vol.r.level)<<16);
  
} // end PSX_spu_get_cur_vol_lr


void
PSX_spu_set_cd_vol_l (
        	      const uint16_t data
        	      )
{
  
  clock ();

  _cd.vol_l= (int16_t) data;
  
} // end PSX_spu_set_cd_vol_l


void
PSX_spu_set_cd_vol_r (
        	      const uint16_t data
        	      )
{
  
  clock ();
  
  _cd.vol_r= (int16_t) data;
  
} // end PSX_spu_set_cd_vol_r


void
PSX_spu_set_ext_vol_l (
        	       const uint16_t data
        	       )
{

  clock ();

  _vol.ext_l= (int16_t) data;
  
} // end PSX_spu_set_ext_vol_l


void
PSX_spu_set_ext_vol_r (
        	       const uint16_t data
        	       )
{

  clock ();

  _vol.ext_r= (int16_t) data;
  
} // end PSX_spu_set_ext_vol_r


uint32_t
PSX_spu_get_cd_vol (void)
{
  return
    (((uint32_t) ((uint16_t) _cd.vol_r))<<16) |
    ((uint32_t) ((uint16_t) _cd.vol_l));
} // end PSX_spu_get_cd_vol


uint32_t
PSX_spu_get_ext_vol (void)
{
  return
    (((uint32_t) ((uint16_t) _vol.ext_r))<<16) |
    ((uint32_t) ((uint16_t) _vol.ext_l));
} // end PSX_spu_get_ext_vol


void
PSX_spu_set_control (
        	     const uint16_t data
        	     )
{

  static const int STEP[]= { 4, 5, 6, 7 };

  
  clock ();
  
  // Registre.
  _stat.reg= data;

  // Valors que ja es poden modificar.
  _stat.enabled= (data&0x8000)!=0;
  _stat.mute= (data&0x4000)==0;
  _noise.shift= (data>>10)&0xF;
  _noise.step= STEP[(data>>8)&0x3];
  _stat.reverb_master_enabled= (data&0x80)!=0;
  _stat.irq_enabled= _stat.enabled && ((data&0x40)!=0);
  if ( (data&0x40) == 0 ) _int.request= false;

  // NOTA!!! Vaig a assumit que en realitat sí que s'actualitzen ací
  // tots els paràmetres. Simplement, en Status els 5 bits inferiors
  // tarden en apareixer.
  io_set_mode ( (_stat.reg>>4)&0x3 );
  _stat.reverb_ext_enabled= (_stat.reg&0x8)!=0;
  _stat.reverb_cd_enabled= (_stat.reg&0x4)!=0;
  _stat.ext_enabled= (_stat.reg&0x2)!=0;
  _stat.cd_enabled= (_stat.reg&0x1)!=0;
  
} // end PSX_spu_set_control


uint16_t
PSX_spu_get_control (void)
{

  clock ();
  
  return _stat.reg;
  
} // end PSX_spu_get_control


void
PSX_spu_set_addr (
        	  const uint16_t data
        	  )
{

  clock ();
  
  _io.addr= data;
  _io.current_addr= ((uint32_t) data)<<3;
  
} // end PSX_spu_set_addr


uint16_t
PSX_spu_get_addr (void)
{
  return _io.addr;
} // end PSX_spu_get_addr


void
PSX_spu_write (
               const uint16_t data
               )
{

  clock ();
  
  if ( _io.N == FIFO_SIZE )
    _warning ( _udata, "SPU write: la FIFO està plena" );
  _io.fifo[_io.N++]= data;
  
} // end PSX_spu_write


void
PSX_spu_set_transfer_type (
        		   const uint16_t data
        		   )
{

  clock ();
  
  _io.transfer_reg= data;
  _io.transfer_type= (data>>1)&0x7;
  
} // end PSX_spu_set_transfer_type


uint16_t
PSX_spu_get_transfer_type (void)
{
  return _io.transfer_reg;
} // end PSX_spu_get_transfer_type


bool
PSX_spu_dma_sync (
                  const uint32_t nwords
                  )
{
  
  if ( _io.mode != DMA_WRITE && _io.mode != DMA_READ )
    {
      _warning ( _udata,
                 "SPU (DMA4) sync: el canal està desactivat i totes les"
                 " peticions de transferència seran ignorades" );
      return true;
    }

  if ( !_dma.ready )
    {
      _dma.p= 0;
      _dma.N= nwords;
      _dma.ready= true;
    }
  PSX_dma_active_channel ( 4 );

  return true;
  
} // end PSX_spu_dma_sync


void
PSX_spu_dma_write (
                   uint32_t data
                   )
{

  if ( _io.mode != DMA_WRITE )
    {
      _warning ( _udata,
        	 "SPU (DMA4) write: el canal no està en mode escriptura" );
      return;
    }

  clock ();
  
  // Escriu
  if ( _dma.p == 0 ) _io.busy= true;
  if ( _dma.p == FIFO_SIZE/2 )
    _warning ( _udata,
               "SPU (DMA4) write: la mostra no cap en el buffer"
               " i es descartarà" );
  else
    {
      _io.fifo[2*_dma.p]= (uint16_t) data;
      _io.fifo[2*_dma.p+1]= (uint16_t) (data>>16);
    }
  if ( ++(_dma.p) == _dma.N )
    {
      _dma.ready= false;
      _io.N= _dma.N*2;
      fifo2ram ();
      _io.busy= false;
    }
  
} // end PSX_spu_dma_write


uint32_t
PSX_spu_dma_read (void)
{

  uint32_t ret;

  
  if ( _io.mode != DMA_READ )
    {
      _warning ( _udata,
        	 "SPU (DMA4) read: el canal no està en mode lectura" );
      return 0xFF00FF00;
    }
  
  clock ();
  
  // Llig
  if ( _dma.p == 0 ) _io.busy= true;
  if ( _dma.p == FIFO_SIZE/2 )
    {
      _warning ( _udata,
                 "SPU (DMA4) read: s'han llegit totes les mostres del buffer" );
      ret= 0xFF00FF00;
    }
  else
    ret=
      ((uint32_t) _io.fifo[2*_dma.p]) |
      (((uint32_t) _io.fifo[2*_dma.p+1])<<16);
  if ( ++(_dma.p) == _dma.N )
    {
      _dma.ready= false;
      ram2fifo (); // Plena la fifo per a una possible futura lectura.
      _io.busy= false; // Per si de cas.
    }

  return ret;
  
} // end PSX_spu_dma_read


void
PSX_spu_reverb_set_vlout (
        		  const uint16_t data
        		  )
{

  clock ();

  _reverb.vlout= (int16_t) data;
  
} // end PSX_spu_reverb_set_vlout


void
PSX_spu_reverb_set_vrout (
        		  const uint16_t data
        		  )
{

  clock ();

  _reverb.vrout= (int16_t) data;
  
} // end PSX_spu_reverb_set_vrout


void
PSX_spu_reverb_set_mbase (
        		  const uint16_t data
        		  )
{

  clock ();

  _reverb.mbase= data;
  _reverb.current_addr= (((uint32_t) data)<<3)&RAM_MASK;
  _reverb.base_addr= _reverb.current_addr;
  
} // end PSX_spu_reverb_set_mbase


uint16_t
PSX_spu_reverb_get_vlout (void)
{
  return _reverb.vlout;
} // end PSX_spu_reverb_get_vlout


uint16_t
PSX_spu_reverb_get_vrout (void)
{
  return _reverb.vrout;
} // end PSX_spu_reverb_get_vrout


uint16_t
PSX_spu_reverb_get_mbase (void)
{
  return _reverb.mbase;
} // end PSX_spu_reverb_get_mbase


void
PSX_spu_reverb_set_reg (
        		const int      reg,
        		const uint16_t data
        		)
{

  clock ();

  _reverb.regs[reg]= data;
  
} // end PSX_spu_reverb_set_reg


uint16_t
PSX_spu_reverb_get_reg (
        		const int reg
        		)
{
  return _reverb.regs[reg];
} // end PSX_spu_reverb_get_reg


void
PSX_spu_set_eon_lo (
        	    const uint16_t data
        	    )
{
  
  int i;
  uint16_t sel;
  
  
  clock ();
  
  _regs.eon= (_regs.eon&0xFFFF0000) | ((uint32_t) data);
  sel= 0x0001;
  for ( i= 0; i < 16; ++i )
    {
      _voices[i].use_reverb= (data&sel)!=0;
      sel<<= 1;
    }
  
} // end PSX_spu_set_eon_lo


void
PSX_spu_set_eon_up (
        	    const uint16_t data
        	    )
{

  int i;
  uint16_t sel;
  
  
  clock ();
  
  _regs.eon= (_regs.eon&0x0000FFFF) | (((uint32_t) data)<<16);
  sel= 0x0001;
  for ( i= 16; i < 24; ++i )
    {
      _voices[i].use_reverb= (data&sel)!=0;
      sel<<= 1;
    }
  
} // end PSX_spu_set_eon_up


uint32_t
PSX_spu_get_eon (void)
{
  return _regs.eon;
} // end PSX_spu_get_eon


void
PSX_spu_set_irq_addr (
        	      const uint16_t data
        	      )
{

  clock ();

  _int.addr_reg= data;
  _int.addr= ((uint32_t) data)<<3;
  _int.addr16= (_int.addr>>1);
  
} // end PSX_spu_set_irq_addr


uint16_t
PSX_spu_get_irq_addr (void)
{
  return _int.addr_reg;
} // end PSX_spu_get_irq_addr


uint16_t
PSX_spu_get_status (void)
{
  
  clock ();
  
  return (uint16_t)
    (
     // Writing to First/Second half of Capture Buffers (0=First, 1=Second)
     // Segons NOCASH deuria testejar en _io.transfer 0xC, però
     // mednafen fa 0x4
     (((_cd.rec_p&0x100)!=0&&(_io.transfer_reg&0x4)!=0)<<11) |
     // Data Transfer Busy Flag (0=Ready, 1=Busy)
     ((_io.busy==true)<<10) |
     // Data Transfer DMA Read Request (0=No, 1=Yes)
     ((_io.mode==DMA_READ)<<9) |
     // Data Transfer DMA Write Request (0=No, 1=Yes)
     ((_io.mode==DMA_WRITE)<<8) |
     // Data Transfer DMA Read/Write Request ;seems to be same as SPUCNT.Bit5
     ((_io.mode==DMA_WRITE||_io.mode==DMA_READ)<<7) |
     // IRQ9 Flag (0=No, 1=Interrupt Request)
     ((_int.request==true)<<6) |
     // Current SPU Mode (same as SPUCNT.Bit5-0, but, applied a bit delayed)
     _stat.reg_read
     );
  
} // end PSX_spu_get_status


void
PSX_spu_set_unk_da0 (
        	     const uint16_t data
        	     )
{
  _regs.unk_da0= data;
} // end PSX_spu_set_unk_da0


uint16_t
PSX_spu_get_unk_da0 (void)
{
  return _regs.unk_da0;
} // end PSX_spu_get_unk_da0


void
PSX_spu_set_unk_dbc (
        	     const int      ind,
        	     const uint16_t data
        	     )
{
  _regs.unk_dbc[ind]= data;
} // end PSX_spu_set_unk_dbc


uint16_t
PSX_spu_get_unk_dbc (
        	     const int ind
        	     )
{
  return _regs.unk_dbc[ind];
} // end PSX_spu_get_unk_dbc


void
PSX_spu_set_unk_e60 (
        	     const int      reg,
        	     const uint16_t data
        	     )
{
  _regs.unk_e60[reg]= data;
} // end PSX_spu_set_unk_e60


uint16_t
PSX_spu_get_unk_e60 (
        	     const int reg
        	     )
{
  return _regs.unk_e60[reg];
} // end PSX_spu_get_unk_e60


void
PSX_spu_reset (void)
{

  // Eixida.
  _out.N= 0;

  // DMA.
  _dma.ready= false;
  _dma.p= 0;
  _dma.N= 0;
  
} // end PSX_spu_reset

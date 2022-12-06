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
 *  mem.c - Implementació del mòdul de memòria.
 *
 */
/*
 * NOTA!!! De moment vaig a ignorar els temps d'accés.
 */


#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PSX.h"




/**********/
/* MACROS */
/**********/

#define RAM_SIZE (2*1024*1024)

#define RAM_MASK 0x1FFFFF
#define RAM_MASK_16 ((RAM_MASK)>>1)
#define RAM_MASK_32 ((RAM_MASK)>>2)

#define BIOS_MASK 0x7FFFF
#define BIOS_MASK_16 ((BIOS_MASK)>>1)
#define BIOS_MASK_32 ((BIOS_MASK)>>2)

#define SP_MASK 0x3FF
#define SP_MASK_16 ((SP_MASK)>>1)
#define SP_MASK_32 ((SP_MASK)>>2)

#define W16(data,port)        			\
  (((uint16_t) (data))<<(8*((port)&0x1)))

#define W32(data,port)        			\
  (((uint32_t) (data))<<(8*((port)&0x3)))

/* Per a 16 bits. */
#define WW32(data,port)        			\
  (((uint32_t) (data))<<(16*((port)&0x1)))

// Per als 16bits de mem_write8
#define WW32F8(data,port)        		\
  (((uint32_t) (data))<<(16*((port)&0x2)))

#define U16(data) ((uint16_t) (data))
#define U32(data) ((uint32_t) (data))

#define R8(data,port)        			\
  ((uint8_t) (((data)>>(8*((port)&0x3)))&0xFF))

#define R8F16(data,port)        		\
  ((uint8_t) (((data)>>(8*((port)&0x1)))&0xFF))

#define R16(data,port)        				\
  ((uint16_t) (((data)>>(16*((port)&0x1)))&0xFFFF))




/*********/
/* TIPUS */
/*********/

typedef struct
{
  
  uint32_t reg_val;
  uint32_t end8;
  uint32_t end16;
  uint32_t end32;
  
} delay_size_t;


typedef struct
{

  delay_size_t ds;
  uint32_t     addr8;
  uint32_t     addr16;
  uint32_t     addr32;
  
} exp_t;




/*********/
/* ESTAT */
/*********/

/* Callbacks. */
static PSX_MemChanged *_mem_changed;
static PSX_MemAccess *_mem_access;
static PSX_MemAccess16 *_mem_access16;
static PSX_MemAccess8 *_mem_access8;
static void *_udata;

/* Funcions. */
static bool (*_mem_read) (const uint32_t addr,uint32_t *dst);
static bool (*_mem_write) (const uint32_t addr,const uint32_t data);
static bool (*_mem_read16) (const uint32_t addr,uint16_t *dst,const bool is_le);
static bool (*_mem_write16) (const uint32_t addr,const uint16_t data,
        		     const bool is_le);
static bool (*_mem_read8) (const uint32_t addr,uint8_t *dst,const bool is_le);
static bool (*_mem_write8) (const uint32_t addr,const uint8_t data,
        		    const uint16_t data16,const bool is_le);

/* RAM */
static struct
{
  
  uint32_t ram_size; /* Valor del registre. */
  uint8_t  v[RAM_SIZE];
  uint32_t end_ram32;
  uint32_t end_ram16;
  uint32_t end_ram8;
  uint32_t end_hz32;
  uint32_t end_hz16;
  uint32_t end_hz8;
  bool     locked_00800000;
  
} _ram;

/* BIOS. */
static struct
{

  delay_size_t ds;
  uint8_t      v[PSX_BIOS_SIZE];
  
} _bios;

/* Altres. */
static delay_size_t _exp3;
static exp_t _exp1,_exp2;
static uint32_t _spu,_cdrom,_com;

static uint8_t _scratchpad[1024];




/*********************/
/* FUNCIONS PRIVADES */
/*********************/

static void
write_ram_size (
        	const uint32_t data
        	)
{

  /* NOTA: De moment ignore algunes coses:
   *
   * - 3 Crashes when zero (except older consoles whose BIOS <did> set
   *   bit3=0)
   * - 7 Delay on simultaneous CODE+DATA fetch from RAM (0=None, 1=One
   *   Cycle)
   * - 8 Unknown (no effect) (should be set for 8MB, cleared for 2MB)
   */
  
  _ram.ram_size= data;
  switch ( (data>>9)&0x7 )
    {
    case 0: /* 1MB Memory + 7MB Locked */
      _ram.end_ram8= 0x00100000;
      _ram.end_hz8= 0x00100000;
      _ram.locked_00800000= false;
      break;
    case 1: /* 4MB Memory + 4MB Locked */
      _ram.end_ram8= 0x00400000;
      _ram.end_hz8= 0x00400000;
      _ram.locked_00800000= false;
      break;
    case 2: /* 1MB Memory + 1MB HighZ + 6MB Locked */
      _ram.end_ram8= 0x00100000;
      _ram.end_hz8= 0x00200000;
      _ram.locked_00800000= false;
      break;
    case 3: /* 4MB Memory + 4MB HighZ */
      _ram.end_ram8= 0x00400000;
      _ram.end_hz8= 0x00800000;
      _ram.locked_00800000= false;
      break;
    case 4: /* 2MB Memory + 6MB Locked */
      _ram.end_ram8= 0x00200000;
      _ram.end_hz8= 0x00200000;
      _ram.locked_00800000= false;
      break;
    case 5: /* 8MB Memory */
      _ram.end_ram8= 0x00800000;
      _ram.end_hz8= 0x00800000;
      _ram.locked_00800000= true;
      break;
    case 6: /* 2MB Memory + 2MB HighZ + 4MB Locked */
      _ram.end_ram8= 0x00200000;
      _ram.end_hz8= 0x00400000;
      _ram.locked_00800000= false;
      break;
    case 7: /* 8MB Memory */
      _ram.end_ram8= 0x00800000;
      _ram.end_hz8= 0x00800000;
      _ram.locked_00800000= true;
      break;
    }
  _ram.end_ram16= _ram.end_ram8>>1;
  _ram.end_hz16= _ram.end_hz8>>1;
  _ram.end_ram32= _ram.end_ram8>>2;
  _ram.end_hz32= _ram.end_hz8>>2;
  if ( _mem_changed != NULL ) _mem_changed ( _udata );
  
} /* end write_ram_size */


static void
init_ram (void)
{

  memset ( _ram.v, 0, RAM_SIZE ); /* Ja sé que és redundant! */
  write_ram_size ( 0x00000B88 ); /* Valor típic. */
  
} /* end init_ram */


#ifdef PSX_BE
static void
swap_u32 (
          uint32_t       *dst,
          const uint32_t *src,
          const size_t    length
          )
{

  size_t i;
  const uint8_t *val;
  uint8_t *to;

  for ( i= 0; i < length; ++i )
    {
      val= (const uint8_t *) &(src[i]);
      to= (uint8_t *) &(dst[i]);
      to[0]= val[3];
      to[1]= val[2];
      to[2]= val[1];
      to[3]= val[0];
    }
  
} /* end swap_bios */
#endif

static void
write_delay_size (
        	  delay_size_t   *ds,
        	  const uint32_t  data,
        	  const uint32_t  offset
        	  );

static void
write_base_addr (
        	 exp_t          *exp,
        	 const uint32_t  addr
        	 )
{

  exp->addr8= addr;
  exp->addr16= addr>>1;
  exp->addr32= addr>>2;
  write_delay_size ( &(exp->ds), exp->ds.reg_val, addr );
  
} /* end write_base_addr */


static void
write_exp1_base_addr (
        	      const uint32_t addr
        	      )
{
  write_base_addr ( &_exp1, addr );
} /* end write_exp1_base_addr */


static void
write_exp2_base_addr (
        	      const uint32_t addr
        	      )
{
  write_base_addr ( &_exp2, addr );
} /* end write_exp2_base_addr */


static void
write_delay_size (
        	  delay_size_t   *ds,
        	  const uint32_t  data,
        	  const uint32_t  offset
        	  )
{
  
  ds->reg_val= data;
  ds->end8= offset + (1<<((data>>16)&0x1F));
  ds->end16= ds->end8>>1;
  ds->end32= ds->end8>>2;
  
} /* end write_delay_size */


static void
write_bios_delay_size (
        	       const uint32_t data
        	       )
{
  write_delay_size ( &_bios.ds, data, 0x1FC00000 );
} /* end write_bios_delay_size */


static void
write_exp1_delay_size (
        	       const uint32_t data
        	       )
{
  write_delay_size ( &_exp1.ds, data, _exp1.addr8 );
} /* end write_exp1_delay_size */


static void
write_exp3_delay_size (
        	       const uint32_t data
        	       )
{
  write_delay_size ( &_exp3, data, 0x1FA00000 );
} /* end write_exp3_delay_size */


static void
write_exp2_delay_size (
        	       const uint32_t data
        	       )
{
  write_delay_size ( &_exp2.ds, data, _exp2.addr8 );
} /* end write_exp2_delay_size */


static void
init_bios (
           const uint8_t bios[PSX_BIOS_SIZE]
           )
{

#ifdef PSX_LE
  memcpy ( _bios.v, bios, PSX_BIOS_SIZE );
#else
  swap_u32 ( (uint32_t *) _bios.v, (const uint32_t *) bios, PSX_BIOS_SIZE>>2 );
#endif
  write_bios_delay_size ( 0x0013243F );
  
} /* end init_bios */


static void
init_exp (
          exp_t          *exp,
          const uint32_t  baddr,
          const uint32_t  delay_size
          )
{

  exp->ds.reg_val= delay_size;
  write_base_addr ( exp, baddr );
  
} /* end init_exp */


static void
dma_bcr_write8 (
        	const int     chn,
        	const int     port,
        	const uint8_t data
        	)
{

  int shift;
  uint32_t tmp;
  
  
  shift= 8*(port&0x3);
  tmp= PSX_dma_bcr_read ( chn ) & (~(0xFF<<shift));
  PSX_dma_bcr_write ( chn, tmp | (((uint32_t) data)<<shift) );
  
} /* end dma_bcr_write8 */


static void
dma_bcr_write16 (
        	 const int      chn,
        	 const int      port,
        	 const uint16_t data
        	 )
{

  int shift;
  uint32_t tmp;
  
  
  shift= 16*(port&0x1);
  tmp= PSX_dma_bcr_read ( chn ) & (~(0xFFFF<<shift));
  PSX_dma_bcr_write ( chn, tmp | (((uint32_t) data)<<shift) );
  
} /* end dma_bcr_write16 */


static bool
mem_read (
          const uint32_t  addr,
          uint32_t       *dst
          )
{

  uint32_t aux;
  int port;
  
  
  aux= addr>>2;
  
  /* RAM. */
  if ( aux < _ram.end_ram32 ) *dst= ((uint32_t *) _ram.v)[aux&RAM_MASK_32];

  /* La resta de l'àrea de la RAM. */
  else if ( aux <= (0x00800000>>2) )
    {
      if ( aux < _ram.end_hz32 ) *dst= 0xFFFFFFFF;
      else if ( aux == (0x00800000>>2) && !_ram.locked_00800000 ) *dst= 0;
      else return false;
    }

  /* Àrea en blanc. */
  else if ( aux < (0x1F000000>>2) ) return false;
  
  /* Expansion Region 1 */
  else if ( aux < (0x1F800000>>2) )
    {
      if ( aux >= _exp1.addr32 && aux < _exp1.ds.end32 )
        {
          printf("R32 Expansion 1\n");
          *dst= 0;
        }
      else return false;
    }

  /* Scratchpad */
  else if ( aux < (0x1F800400>>2) )
    *dst= ((uint32_t *) _scratchpad)[aux&SP_MASK_32];
  
  /* Region after scratchpad. */
  else if ( aux < (0x1F801000>>2) ) return false;
  
  /* I/O Ports */
  else if ( aux < (0x1F802000>>2) )
    {
      port= aux&(0xFFF>>2);
      switch ( port )
        {
          // Memory Control 1
        case (0x000>>2): *dst= _exp1.addr32; break;
        case (0x004>>2): *dst= _exp2.addr32; break;
        case (0x008>>2): *dst= _exp1.ds.reg_val; break;
        case (0x00C>>2): *dst= _exp3.reg_val; break;
        case (0x010>>2): *dst= _bios.ds.reg_val; break;
        case (0x014>>2): *dst= _spu; break;
        case (0x018>>2): *dst= _cdrom; break;
        case (0x01C>>2): *dst= _exp2.ds.reg_val; break;
        case (0x020>>2): *dst= _com; break;
          // Peripheral I/O Ports
        case (0x040>>2): *dst= PSX_joy_rx_data (); break;
        case (0x044>>2): *dst= PSX_joy_stat (); break;
        case (0x048>>2):
          *dst= U32(PSX_joy_mode_read ());
          *dst|= U32(PSX_joy_ctrl_read ())<<16;
          break;
        case (0x04C>>2):
          *dst= U32(PSX_joy_baud_read ())<<16;
          break;
          // Memory Control 2
        case (0x060>>2): *dst= _ram.ram_size; break;
          // Interrupt Control
        case (0x070>>2): *dst= PSX_int_read_state (); break;
        case (0x074>>2): *dst= PSX_int_read_imask (); break;
          // DMA Registers
        case (0x080>>2): *dst= PSX_dma_madr_read ( 0 ); break;
        case (0x084>>2): *dst= PSX_dma_bcr_read ( 0 ); break;
        case (0x088>>2): *dst= PSX_dma_chcr_read ( 0 ); break;
        case (0x090>>2): *dst= PSX_dma_madr_read ( 1 ); break;
        case (0x094>>2): *dst= PSX_dma_bcr_read ( 1 ); break;
        case (0x098>>2): *dst= PSX_dma_chcr_read ( 1 ); break;
        case (0x0A0>>2): *dst= PSX_dma_madr_read ( 2 ); break;
        case (0x0A4>>2): *dst= PSX_dma_bcr_read ( 2 ); break;
        case (0x0A8>>2): *dst= PSX_dma_chcr_read ( 2 ); break;
        case (0x0B0>>2): *dst= PSX_dma_madr_read ( 3 ); break;
        case (0x0B4>>2): *dst= PSX_dma_bcr_read ( 3 ); break;
        case (0x0B8>>2): *dst= PSX_dma_chcr_read ( 3 ); break;
        case (0x0C0>>2): *dst= PSX_dma_madr_read ( 4 ); break;
        case (0x0C4>>2): *dst= PSX_dma_bcr_read ( 4 ); break;
        case (0x0C8>>2): *dst= PSX_dma_chcr_read ( 4 ); break;
        case (0x0D0>>2): *dst= PSX_dma_madr_read ( 5 ); break;
        case (0x0D4>>2): *dst= PSX_dma_bcr_read ( 5 ); break;
        case (0x0D8>>2): *dst= PSX_dma_chcr_read ( 5 ); break;
        case (0x0E0>>2): *dst= PSX_dma_madr_read ( 6 ); break;
        case (0x0E4>>2): *dst= PSX_dma_bcr_read ( 6 ); break;
        case (0x0E8>>2): *dst= PSX_dma_chcr_read ( 6 ); break;
        case (0x0F0>>2): *dst= PSX_dma_dpcr_read (); break;
        case (0x0F4>>2): *dst= PSX_dma_dicr_read (); break;
        case (0x0F8>>2): *dst= PSX_dma_unk1_read (); break;
        case (0x0FC>>2): *dst= PSX_dma_unk2_read (); break;
          // Timers
        case (0x100>>2): *dst= PSX_timers_get_counter_value ( 0 ); break;
        case (0x104>>2): *dst= PSX_timers_get_counter_mode ( 0 ); break;
        case (0x108>>2): *dst= PSX_timers_get_target_value ( 0 ); break;
        case (0x110>>2): *dst= PSX_timers_get_counter_value ( 1 ); break;
        case (0x114>>2): *dst= PSX_timers_get_counter_mode ( 1 ); break;
        case (0x118>>2): *dst= PSX_timers_get_target_value ( 1 ); break;
        case (0x120>>2): *dst= PSX_timers_get_counter_value ( 2 ); break;
        case (0x124>>2): *dst= PSX_timers_get_counter_mode ( 2 ); break;
        case (0x128>>2): *dst= PSX_timers_get_target_value ( 2 ); break;
          // CDROM Registers
        case (0x800>>2):
          *dst= U32(PSX_cd_status ())*0x01010101;
          // NOTA!! Trying to do a 32bit read from 1F801800h returns
          // the 8bit value at 1F801800h multiplied by 01010101h.
          break;
          // GPU Registers
        case (0x810>>2): *dst= PSX_gpu_read (); break;
        case (0x814>>2): *dst= PSX_gpu_stat (); break;
          // MDEC Registers
        case (0x820>>2): *dst= PSX_mdec_data_read (); break;
        case (0x824>>2): *dst= PSX_mdec_status (); break;
          // SPU Voice 0..23 Registers
        case (0xC00>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 0 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 0 ))<<16;
          break;
        case (0xC04>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 0 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 0 ))<<16;
          break;
        case (0xC08>>2): *dst= PSX_spu_voice_get_adsr ( 0 ); break;
        case (0xC0C>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 0 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 0 ))<<16;
          break;
        case (0xC10>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 1 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 1 ))<<16;
          break;
        case (0xC14>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 1 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 1 ))<<16;
          break;
        case (0xC18>>2): *dst= PSX_spu_voice_get_adsr ( 1 ); break;
        case (0xC1C>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 1 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 1 ))<<16;
          break;
        case (0xC20>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 2 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 2 ))<<16;
          break;
        case (0xC24>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 2 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 2 ))<<16;
          break;
        case (0xC28>>2): *dst= PSX_spu_voice_get_adsr ( 2 ); break;
        case (0xC2C>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 2 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 2 ))<<16;
          break;
        case (0xC30>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 3 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 3 ))<<16;
          break;
        case (0xC34>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 3 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 3 ))<<16;
          break;
        case (0xC38>>2): *dst= PSX_spu_voice_get_adsr ( 3 ); break;
        case (0xC3C>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 3 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 3 ))<<16;
          break;
        case (0xC40>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 4 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 4 ))<<16;
          break;
        case (0xC44>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 4 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 4 ))<<16;
          break;
        case (0xC48>>2): *dst= PSX_spu_voice_get_adsr ( 4 ); break;
        case (0xC4C>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 4 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 4 ))<<16;
          break;
        case (0xC50>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 5 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 5 ))<<16;
          break;
        case (0xC54>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 5 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 5 ))<<16;
          break;
        case (0xC58>>2): *dst= PSX_spu_voice_get_adsr ( 5 ); break;
        case (0xC5C>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 5 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 5 ))<<16;
          break;
        case (0xC60>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 6 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 6 ))<<16;
          break;
        case (0xC64>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 6 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 6 ))<<16;
          break;
        case (0xC68>>2): *dst= PSX_spu_voice_get_adsr ( 6 ); break;
        case (0xC6C>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 6 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 6 ))<<16;
          break;
        case (0xC70>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 7 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 7 ))<<16;
          break;
        case (0xC74>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 7 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 7 ))<<16;
          break;
        case (0xC78>>2): *dst= PSX_spu_voice_get_adsr ( 7 ); break;
        case (0xC7C>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 7 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 7 ))<<16;
          break;
        case (0xC80>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 8 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 8 ))<<16;
          break;
        case (0xC84>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 8 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 8 ))<<16;
          break;
        case (0xC88>>2): *dst= PSX_spu_voice_get_adsr ( 8 ); break;
        case (0xC8C>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 8 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 8 ))<<16;
          break;
        case (0xC90>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 9 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 9 ))<<16;
          break;
        case (0xC94>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 9 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 9 ))<<16;
          break;
        case (0xC98>>2): *dst= PSX_spu_voice_get_adsr ( 9 ); break;
        case (0xC9C>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 9 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 9 ))<<16;
          break;
        case (0xCA0>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 10 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 10 ))<<16;
          break;
        case (0xCA4>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 10 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 10 ))<<16;
          break;
        case (0xCA8>>2): *dst= PSX_spu_voice_get_adsr ( 10 ); break;
        case (0xCAC>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 10 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 10 ))<<16;
          break;
        case (0xCB0>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 11 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 11 ))<<16;
          break;
        case (0xCB4>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 11 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 11 ))<<16;
          break;
        case (0xCB8>>2): *dst= PSX_spu_voice_get_adsr ( 11 ); break;
        case (0xCBC>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 11 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 11 ))<<16;
          break;
        case (0xCC0>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 12 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 12 ))<<16;
          break;
        case (0xCC4>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 12 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 12 ))<<16;
          break;
        case (0xCC8>>2): *dst= PSX_spu_voice_get_adsr ( 12 ); break;
        case (0xCCC>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 12 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 12 ))<<16;
          break;
        case (0xCD0>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 13 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 13 ))<<16;
          break;
        case (0xCD4>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 13 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 13 ))<<16;
          break;
        case (0xCD8>>2): *dst= PSX_spu_voice_get_adsr ( 13 ); break;
        case (0xCDC>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 13 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 13 ))<<16;
          break;
        case (0xCE0>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 14 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 14 ))<<16;
          break;
        case (0xCE4>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 14 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 14 ))<<16;
          break;
        case (0xCE8>>2): *dst= PSX_spu_voice_get_adsr ( 14 ); break;
        case (0xCEC>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 14 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 14 ))<<16;
          break;
        case (0xCF0>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 15 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 15 ))<<16;
          break;
        case (0xCF4>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 15 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 15 ))<<16;
          break;
        case (0xCF8>>2): *dst= PSX_spu_voice_get_adsr ( 15 ); break;
        case (0xCFC>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 15 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 15 ))<<16;
          break;
        case (0xD00>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 16 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 16 ))<<16;
          break;
        case (0xD04>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 16 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 16 ))<<16;
          break;
        case (0xD08>>2): *dst= PSX_spu_voice_get_adsr ( 16 ); break;
        case (0xD0C>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 16 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 16 ))<<16;
          break;
        case (0xD10>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 17 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 17 ))<<16;
          break;
        case (0xD14>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 17 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 17 ))<<16;
          break;
        case (0xD18>>2): *dst= PSX_spu_voice_get_adsr ( 17 ); break;
        case (0xD1C>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 17 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 17 ))<<16;
          break;
        case (0xD20>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 18 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 18 ))<<16;
          break;
        case (0xD24>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 18 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 18 ))<<16;
          break;
        case (0xD28>>2): *dst= PSX_spu_voice_get_adsr ( 18 ); break;
        case (0xD2C>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 18 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 18 ))<<16;
          break;
        case (0xD30>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 19 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 19 ))<<16;
          break;
        case (0xD34>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 19 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 19 ))<<16;
          break;
        case (0xD38>>2): *dst= PSX_spu_voice_get_adsr ( 19 ); break;
        case (0xD3C>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 19 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 19 ))<<16;
          break;
        case (0xD40>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 20 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 20 ))<<16;
          break;
        case (0xD44>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 20 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 20 ))<<16;
          break;
        case (0xD48>>2): *dst= PSX_spu_voice_get_adsr ( 20 ); break;
        case (0xD4C>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 20 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 20 ))<<16;
          break;
        case (0xD50>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 21 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 21 ))<<16;
          break;
        case (0xD54>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 21 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 21 ))<<16;
          break;
        case (0xD58>>2): *dst= PSX_spu_voice_get_adsr ( 21 ); break;
        case (0xD5C>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 21 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 21 ))<<16;
          break;
        case (0xD60>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 22 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 22 ))<<16;
          break;
        case (0xD64>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 22 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 22 ))<<16;
          break;
        case (0xD68>>2): *dst= PSX_spu_voice_get_adsr ( 22 ); break;
        case (0xD6C>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 22 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 22 ))<<16;
          break;
        case (0xD70>>2):
          *dst= U32(PSX_spu_voice_get_left_vol ( 23 ));
          *dst|= U32(PSX_spu_voice_get_right_vol ( 23 ))<<16;
          break;
        case (0xD74>>2):
          *dst= U32(PSX_spu_voice_get_sample_rate ( 23 ));
          *dst|= U32(PSX_spu_voice_get_start_addr ( 23 ))<<16;
          break;
        case (0xD78>>2): *dst= PSX_spu_voice_get_adsr ( 23 ); break;
        case (0xD7C>>2):
          *dst= U32(PSX_spu_voice_get_cur_vol ( 23 ));
          *dst|= U32(PSX_spu_voice_get_repeat_addr ( 23 ))<<16;
          break;
          // SPU Control Registers
        case (0xD80>>2):
          *dst= U32(PSX_spu_get_left_vol ());
          *dst|= U32(PSX_spu_get_right_vol ())<<16;
          break;
        case (0xD84>>2):
          *dst= U32(PSX_spu_reverb_get_vlout ());
          *dst|= U32(PSX_spu_reverb_get_vrout ())<<16;
          break;
        case (0xD88>>2): *dst= PSX_spu_get_kon (); break;
        case (0xD8C>>2): *dst= PSX_spu_get_koff (); break;
        case (0xD90>>2): *dst= PSX_spu_get_pmon (); break;
        case (0xD94>>2): *dst= PSX_spu_get_non (); break;
        case (0xD98>>2): *dst= PSX_spu_get_eon (); break;
        case (0xD9C>>2): *dst= PSX_spu_get_endx (); break;
        case (0xDA0>>2):
          *dst= U32(PSX_spu_get_unk_da0 ());
          *dst|= U32(PSX_spu_reverb_get_mbase ())<<16;
          break;
        case (0xDA4>>2):
          *dst= U32(PSX_spu_get_irq_addr ());
          *dst|= U32(PSX_spu_get_addr ())<<16;
          break;
        case (0xDA8>>2):
          *dst= 0; // Sound RAM Data Transfer Fifo
          *dst|= U32(PSX_spu_get_control ())<<16;
          break;
        case (0xDAC>>2):
          *dst= U32(PSX_spu_get_transfer_type ());
          *dst|= U32(PSX_spu_get_status ())<<16;
          break;
        case (0xDB0>>2): *dst= PSX_spu_get_cd_vol (); break;
        case (0xDB4>>2): *dst= PSX_spu_get_ext_vol (); break;
        case (0xDB8>>2): *dst= PSX_spu_get_cur_vol_lr (); break;
        case (0xDBC>>2):
          *dst= U32(PSX_spu_get_unk_dbc ( 0 ));
          *dst|= U32(PSX_spu_get_unk_dbc ( 1 ))<<16;
          break;
          // SPU Reverb Configuration Area
        case (0xDC0>>2) ... (0xDFC>>2):
          *dst= U32(PSX_spu_reverb_get_reg ( (aux<<1)&0x1F ));
          *dst|= U32(PSX_spu_reverb_get_reg ( ((aux<<1)&0x1F)|1 ))<<16;
          break;
          // SPU Internal Registers
        case (0xE00>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 0 ); break;
        case (0xE04>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 1 ); break;
        case (0xE08>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 2 ); break;
        case (0xE0C>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 3 ); break;
        case (0xE10>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 4 ); break;
        case (0xE14>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 5 ); break;
        case (0xE18>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 6 ); break;
        case (0xE1C>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 7 ); break;
        case (0xE20>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 8 ); break;
        case (0xE24>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 9 ); break;
        case (0xE28>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 10 ); break;
        case (0xE2C>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 11 ); break;
        case (0xE30>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 12 ); break;
        case (0xE34>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 13 ); break;
        case (0xE38>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 14 ); break;
        case (0xE3C>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 15 ); break;
        case (0xE40>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 16 ); break;
        case (0xE44>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 17 ); break;
        case (0xE48>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 18 ); break;
        case (0xE4C>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 19 ); break;
        case (0xE50>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 20 ); break;
        case (0xE54>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 21 ); break;
        case (0xE58>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 22 ); break;
        case (0xE5C>>2): *dst= PSX_spu_voice_get_cur_vol_lr ( 23 ); break;
        case (0xE60>>2) ... (0xE7C>>2):
          *dst= U32(PSX_spu_get_unk_e60 ( (aux<<1)&0xF ));
          *dst|= U32(PSX_spu_get_unk_e60 ( ((aux<<1)&0xF)|1 ))<<16;
          break;
        case (0xE80>>2) ... (0xFFC>>2):
          *dst= 0xFFFFFFFF;
          break; 
          // Locked
        default:
          printf("R32 I/O (0x%x)\nCal return false!!\n",port<<2);
          /*return false;*/ *dst= 0;
        }
    }
  
  /* Expansion Region 2 i 3 */
  else if ( aux < (0x1FC00000>>2) )
    {
      if ( aux >= _exp2.addr32 && aux < _exp2.ds.end32 )
        {
          printf("R32 Expansion 2\n");
          *dst= 0;
        }
      else if ( aux >= (0x1FA00000>>2) && aux < _exp3.end32 )
        {
          printf("R32 Expansion 3\n");
          *dst= 0;
        }
      else return false;
    }
  
  /* BIOS i resta fins 1FFFFFFF */
  else
    {
      if ( aux < _bios.ds.end32 )
        *dst= ((uint32_t *) _bios.v)[aux&BIOS_MASK_32];
      else return false;
    }
  
  return true;
  
} /* end mem_read */


static bool
mem_read16 (
            const uint32_t  addr,
            uint16_t       *dst,
            const bool      is_le
            )
{

  uint32_t aux;
  int port;
  

  aux= addr>>1;
  
  /* RAM. */
  if ( aux < _ram.end_ram16 )
    {
#ifdef PSX_LE
      if ( !is_le ) aux^= 1;
#else
      if ( is_le ) aux^= 1;
#endif
      *dst= ((uint16_t *) _ram.v)[aux&RAM_MASK_16];
    }

  /* La resta de l'àrea de la RAM. */
  else if ( aux <= (0x00800000>>1) )
    {
      if ( aux < _ram.end_hz16 ) *dst= 0xFFFF;
      else if ( aux == (0x00800000>>1) && !_ram.locked_00800000 ) *dst= 0;
      else return false;
    }
  
  /* Àrea en blanc. */
  else if ( aux < (0x1F000000>>1) ) return false;
  
  /* Expansion Region 1 */
  else if ( aux < (0x1F800000>>1) )
    {
      if ( aux >= _exp1.addr16 && aux < _exp1.ds.end16 )
        {
          printf("R16 Expansion 1\n");
          *dst= 0;
        }
      else return false;
    }

  /* Scratchpad */
  else if ( aux < (0x1F800400>>1) )
    {
#ifdef PSX_LE
      if ( !is_le ) aux^= 1;
#else
      if ( is_le ) aux^= 1;
#endif  
      *dst= ((uint16_t *) _scratchpad)[aux&SP_MASK_16];
    }
  
  /* Region after scratchpad */
  else if ( aux < (0x1F801000>>1) ) return false;
  
  /* I/O Ports */
  else if ( aux < (0x1F802000>>1) )
    {
      port= aux&(0xFFF>>1);
      switch ( port )
        {
          // Memory Control 1
        case (0x000>>1):
        case (0x002>>1): *dst= R16(_exp1.addr32,port); break;
        case (0x004>>1):
        case (0x006>>1): *dst= R16(_exp2.addr32,port); break;
        case (0x008>>1):
        case (0x00A>>1): *dst= R16(_exp1.ds.reg_val,port); break;
        case (0x00C>>1):
        case (0x00E>>1): *dst= R16(_exp3.reg_val,port); break;
        case (0x010>>1):
        case (0x012>>1): *dst= R16(_bios.ds.reg_val,port); break;
        case (0x014>>1):
        case (0x016>>1): *dst= R16(_spu,port); break;
        case (0x018>>1):
        case (0x01A>>1): *dst= R16(_cdrom,port); break;
        case (0x01C>>1):
        case (0x01E>>1): *dst= R16(_exp2.ds.reg_val,port); break;
        case (0x020>>1):
        case (0x022>>1): *dst= R16(_com,port); break;
          // Peripheral I/O Ports
        case (0x040>>1):
        case (0x042>>1): *dst= R16(PSX_joy_rx_data (),port); break;
        case (0x044>>1):
        case (0x046>>1): *dst= R16(PSX_joy_stat (),port); break;
        case (0x048>>1): *dst= PSX_joy_mode_read (); break;
        case (0x04A>>1): *dst= PSX_joy_ctrl_read (); break;
        case (0x04C>>1): *dst= 0; break; // <-- No hi ha res
        case (0x04E>>1): *dst= PSX_joy_baud_read (); break;
          // Memory Control 2
        case (0x060>>1):
        case (0x062>>1): *dst= R16(_ram.ram_size,port); break;
          // Interrupt Control.
        case (0x070>>1):
        case (0x072>>1): *dst= R16(PSX_int_read_state (),port); break;
        case (0x074>>1):
        case (0x076>>1): *dst= R16(PSX_int_read_imask (),port); break;
          // DMA Registers
        case (0x080>>1):
        case (0x082>>1): *dst= R16(PSX_dma_madr_read ( 0 ),port); break;
        case (0x084>>1):
        case (0x086>>1): *dst= R16(PSX_dma_bcr_read ( 0 ),port); break;
        case (0x088>>1):
        case (0x08A>>1): *dst= R16(PSX_dma_chcr_read ( 0 ),port); break;
        case (0x090>>1):
        case (0x092>>1): *dst= R16(PSX_dma_madr_read ( 1 ),port); break;
        case (0x094>>1):
        case (0x096>>1): *dst= R16(PSX_dma_bcr_read ( 1 ),port); break;
        case (0x098>>1):
        case (0x09A>>1): *dst= R16(PSX_dma_chcr_read ( 1 ),port); break;
        case (0x0A0>>1):
        case (0x0A2>>1): *dst= R16(PSX_dma_madr_read ( 2 ),port); break;
        case (0x0A4>>1):
        case (0x0A6>>1): *dst= R16(PSX_dma_bcr_read ( 2 ),port); break;
        case (0x0A8>>1):
        case (0x0AA>>1): *dst= R16(PSX_dma_chcr_read ( 2 ),port); break;
        case (0x0B0>>1):
        case (0x0B2>>1): *dst= R16(PSX_dma_madr_read ( 3 ),port); break;
        case (0x0B4>>1):
        case (0x0B6>>1): *dst= R16(PSX_dma_bcr_read ( 3 ),port); break;
        case (0x0B8>>1):
        case (0x0BA>>1): *dst= R16(PSX_dma_chcr_read ( 3 ),port); break;
        case (0x0C0>>1):
        case (0x0C2>>1): *dst= R16(PSX_dma_madr_read ( 4 ),port); break;
        case (0x0C4>>1):
        case (0x0C6>>1): *dst= R16(PSX_dma_bcr_read ( 4 ),port); break;
        case (0x0C8>>1):
        case (0x0CA>>1): *dst= R16(PSX_dma_chcr_read ( 4 ),port); break;
        case (0x0D0>>1):
        case (0x0D2>>1): *dst= R16(PSX_dma_madr_read ( 5 ),port); break;
        case (0x0D4>>1):
        case (0x0D6>>1): *dst= R16(PSX_dma_bcr_read ( 5 ),port); break;
        case (0x0D8>>1):
        case (0x0DA>>1): *dst= R16(PSX_dma_chcr_read ( 5 ),port); break;
        case (0x0E0>>1):
        case (0x0E2>>1): *dst= R16(PSX_dma_madr_read ( 6 ),port); break;
        case (0x0E4>>1):
        case (0x0E6>>1): *dst= R16(PSX_dma_bcr_read ( 6 ),port); break;
        case (0x0E8>>1):
        case (0x0EA>>1): *dst= R16(PSX_dma_chcr_read ( 6 ),port); break;
        case (0x0F0>>1):
        case (0x0F2>>1): *dst= R16(PSX_dma_dpcr_read (),port); break;
        case (0x0F4>>1):
        case (0x0F6>>1): *dst= R16(PSX_dma_dicr_read (),port); break;
        case (0x0F8>>1):
        case (0x0FA>>1): *dst= R16(PSX_dma_unk1_read (),port); break;
        case (0x0FC>>1):
        case (0x0FE>>1): *dst= R16(PSX_dma_unk2_read (),port); break;
          // Timers
        case (0x100>>1):
        case (0x102>>1):
          *dst= R16(PSX_timers_get_counter_value ( 0 ),port);
          break;
        case (0x104>>1):
        case (0x106>>1):
          *dst= R16(PSX_timers_get_counter_mode ( 0 ),port);
          break;
        case (0x108>>1):
        case (0x10A>>1):
          *dst= R16(PSX_timers_get_target_value ( 0 ),port);
          break;
        case (0x110>>1):
        case (0x112>>1):
          *dst= R16(PSX_timers_get_counter_value ( 1 ),port);
          break;
        case (0x114>>1):
        case (0x116>>1):
          *dst= R16(PSX_timers_get_counter_mode ( 1 ),port);
          break;
        case (0x118>>1):
        case (0x11A>>1):
          *dst= R16(PSX_timers_get_target_value ( 1 ),port);
          break;
        case (0x120>>1):
        case (0x122>>1):
          *dst= R16(PSX_timers_get_counter_value ( 2 ),port);
          break;
        case (0x124>>1):
        case (0x126>>1):
          *dst= R16(PSX_timers_get_counter_mode ( 2 ),port);
          break;
        case (0x128>>1):
        case (0x12A>>1):
          *dst= R16(PSX_timers_get_target_value ( 2 ),port);
          break;
          // CDROM Registers
        case (0x800>>1):
          *dst= U16(PSX_cd_status ());
          *dst|= U16(PSX_cd_port1_read ())<<8;
          break;
        case (0x802>>1):
          *dst= U16(PSX_cd_port2_read ());
          *dst|= U16(PSX_cd_port2_read ())<<8; // <-- Repetisc port 2
          // NOTA!!! Port 1F801802h can be accessed with 8bit or 16bit reads 
          break;
          // GPU Registers
        case (0x810>>1):
        case (0x812>>1): *dst= R16(PSX_gpu_read (),port); break;
        case (0x814>>1):
        case (0x816>>1): *dst= R16(PSX_gpu_stat (),port); break;
          // MDEC Registers
        case (0x820>>1):
        case (0x822>>1): *dst= R16(PSX_mdec_data_read (),port); break;
        case (0x824>>1):
        case (0x826>>1): *dst= R16(PSX_mdec_status (),port); break;
          // SPU Voice 0..23 Registers
        case (0xC00>>1): *dst= PSX_spu_voice_get_left_vol ( 0 ); break;
        case (0xC02>>1): *dst= PSX_spu_voice_get_right_vol ( 0 ); break;
        case (0xC04>>1): *dst= PSX_spu_voice_get_sample_rate ( 0 ); break;
        case (0xC06>>1): *dst= PSX_spu_voice_get_start_addr ( 0 ); break;
        case (0xC08>>1):
        case (0xC0A>>1): *dst= R16(PSX_spu_voice_get_adsr ( 0 ),port); break;
        case (0xC0C>>1): *dst= PSX_spu_voice_get_cur_vol ( 0 ); break;
        case (0xC0E>>1): *dst= PSX_spu_voice_get_repeat_addr ( 0 ); break;
        case (0xC10>>1): *dst= PSX_spu_voice_get_left_vol ( 1 ); break;
        case (0xC12>>1): *dst= PSX_spu_voice_get_right_vol ( 1 ); break;
        case (0xC14>>1): *dst= PSX_spu_voice_get_sample_rate ( 1 ); break;
        case (0xC16>>1): *dst= PSX_spu_voice_get_start_addr ( 1 ); break;
        case (0xC18>>1):
        case (0xC1A>>1): *dst= R16(PSX_spu_voice_get_adsr ( 1 ),port); break;
        case (0xC1C>>1): *dst= PSX_spu_voice_get_cur_vol ( 1 ); break;
        case (0xC1E>>1): *dst= PSX_spu_voice_get_repeat_addr ( 1 ); break;
        case (0xC20>>1): *dst= PSX_spu_voice_get_left_vol ( 2 ); break;
        case (0xC22>>1): *dst= PSX_spu_voice_get_right_vol ( 2 ); break;
        case (0xC24>>1): *dst= PSX_spu_voice_get_sample_rate ( 2 ); break;
        case (0xC26>>1): *dst= PSX_spu_voice_get_start_addr ( 2 ); break;
        case (0xC28>>1):
        case (0xC2A>>1): *dst= R16(PSX_spu_voice_get_adsr ( 2 ),port); break;
        case (0xC2C>>1): *dst= PSX_spu_voice_get_cur_vol ( 2 ); break;
        case (0xC2E>>1): *dst= PSX_spu_voice_get_repeat_addr ( 2 ); break;
        case (0xC30>>1): *dst= PSX_spu_voice_get_left_vol ( 3 ); break;
        case (0xC32>>1): *dst= PSX_spu_voice_get_right_vol ( 3 ); break;
        case (0xC34>>1): *dst= PSX_spu_voice_get_sample_rate ( 3 ); break;
        case (0xC36>>1): *dst= PSX_spu_voice_get_start_addr ( 3 ); break;
        case (0xC38>>1):
        case (0xC3A>>1): *dst= R16(PSX_spu_voice_get_adsr ( 3 ),port); break;
        case (0xC3C>>1): *dst= PSX_spu_voice_get_cur_vol ( 3 ); break;
        case (0xC3E>>1): *dst= PSX_spu_voice_get_repeat_addr ( 3 ); break;
        case (0xC40>>1): *dst= PSX_spu_voice_get_left_vol ( 4 ); break;
        case (0xC42>>1): *dst= PSX_spu_voice_get_right_vol ( 4 ); break;
        case (0xC44>>1): *dst= PSX_spu_voice_get_sample_rate ( 4 ); break;
        case (0xC46>>1): *dst= PSX_spu_voice_get_start_addr ( 4 ); break;
        case (0xC48>>1):
        case (0xC4A>>1): *dst= R16(PSX_spu_voice_get_adsr ( 4 ),port); break;
        case (0xC4C>>1): *dst= PSX_spu_voice_get_cur_vol ( 4 ); break;
        case (0xC4E>>1): *dst= PSX_spu_voice_get_repeat_addr ( 4 ); break;
        case (0xC50>>1): *dst= PSX_spu_voice_get_left_vol ( 5 ); break;
        case (0xC52>>1): *dst= PSX_spu_voice_get_right_vol ( 5 ); break;
        case (0xC54>>1): *dst= PSX_spu_voice_get_sample_rate ( 5 ); break;
        case (0xC56>>1): *dst= PSX_spu_voice_get_start_addr ( 5 ); break;
        case (0xC58>>1):
        case (0xC5A>>1): *dst= R16(PSX_spu_voice_get_adsr ( 5 ),port); break;
        case (0xC5C>>1): *dst= PSX_spu_voice_get_cur_vol ( 5 ); break;
        case (0xC5E>>1): *dst= PSX_spu_voice_get_repeat_addr ( 5 ); break;
        case (0xC60>>1): *dst= PSX_spu_voice_get_left_vol ( 6 ); break;
        case (0xC62>>1): *dst= PSX_spu_voice_get_right_vol ( 6 ); break;
        case (0xC64>>1): *dst= PSX_spu_voice_get_sample_rate ( 6 ); break;
        case (0xC66>>1): *dst= PSX_spu_voice_get_start_addr ( 6 ); break;
        case (0xC68>>1):
        case (0xC6A>>1): *dst= R16(PSX_spu_voice_get_adsr ( 6 ),port); break;
        case (0xC6C>>1): *dst= PSX_spu_voice_get_cur_vol ( 6 ); break;
        case (0xC6E>>1): *dst= PSX_spu_voice_get_repeat_addr ( 6 ); break;
        case (0xC70>>1): *dst= PSX_spu_voice_get_left_vol ( 7 ); break;
        case (0xC72>>1): *dst= PSX_spu_voice_get_right_vol ( 7 ); break;
        case (0xC74>>1): *dst= PSX_spu_voice_get_sample_rate ( 7 ); break;
        case (0xC76>>1): *dst= PSX_spu_voice_get_start_addr ( 7 ); break;
        case (0xC78>>1):
        case (0xC7A>>1): *dst= R16(PSX_spu_voice_get_adsr ( 7 ),port); break;
        case (0xC7C>>1): *dst= PSX_spu_voice_get_cur_vol ( 7 ); break;
        case (0xC7E>>1): *dst= PSX_spu_voice_get_repeat_addr ( 7 ); break;
        case (0xC80>>1): *dst= PSX_spu_voice_get_left_vol ( 8 ); break;
        case (0xC82>>1): *dst= PSX_spu_voice_get_right_vol ( 8 ); break;
        case (0xC84>>1): *dst= PSX_spu_voice_get_sample_rate ( 8 ); break;
        case (0xC86>>1): *dst= PSX_spu_voice_get_start_addr ( 8 ); break;
        case (0xC88>>1):
        case (0xC8A>>1): *dst= R16(PSX_spu_voice_get_adsr ( 8 ),port); break;
        case (0xC8C>>1): *dst= PSX_spu_voice_get_cur_vol ( 8 ); break;
        case (0xC8E>>1): *dst= PSX_spu_voice_get_repeat_addr ( 8 ); break;
        case (0xC90>>1): *dst= PSX_spu_voice_get_left_vol ( 9 ); break;
        case (0xC92>>1): *dst= PSX_spu_voice_get_right_vol ( 9 ); break;
        case (0xC94>>1): *dst= PSX_spu_voice_get_sample_rate ( 9 ); break;
        case (0xC96>>1): *dst= PSX_spu_voice_get_start_addr ( 9 ); break;
        case (0xC98>>1):
        case (0xC9A>>1): *dst= R16(PSX_spu_voice_get_adsr ( 9 ),port); break;
        case (0xC9C>>1): *dst= PSX_spu_voice_get_cur_vol ( 9 ); break;
        case (0xC9E>>1): *dst= PSX_spu_voice_get_repeat_addr ( 9 ); break;
        case (0xCA0>>1): *dst= PSX_spu_voice_get_left_vol ( 10 ); break;
        case (0xCA2>>1): *dst= PSX_spu_voice_get_right_vol ( 10 ); break;
        case (0xCA4>>1): *dst= PSX_spu_voice_get_sample_rate ( 10 ); break;
        case (0xCA6>>1): *dst= PSX_spu_voice_get_start_addr ( 10 ); break;
        case (0xCA8>>1):
        case (0xCAA>>1): *dst= R16(PSX_spu_voice_get_adsr ( 10 ),port); break;
        case (0xCAC>>1): *dst= PSX_spu_voice_get_cur_vol ( 10 ); break;
        case (0xCAE>>1): *dst= PSX_spu_voice_get_repeat_addr ( 10 ); break;
        case (0xCB0>>1): *dst= PSX_spu_voice_get_left_vol ( 11 ); break;
        case (0xCB2>>1): *dst= PSX_spu_voice_get_right_vol ( 11 ); break;
        case (0xCB4>>1): *dst= PSX_spu_voice_get_sample_rate ( 11 ); break;
        case (0xCB6>>1): *dst= PSX_spu_voice_get_start_addr ( 11 ); break;
        case (0xCB8>>1):
        case (0xCBA>>1): *dst= R16(PSX_spu_voice_get_adsr ( 11 ),port); break;
        case (0xCBC>>1): *dst= PSX_spu_voice_get_cur_vol ( 11 ); break;
        case (0xCBE>>1): *dst= PSX_spu_voice_get_repeat_addr ( 11 ); break;
        case (0xCC0>>1): *dst= PSX_spu_voice_get_left_vol ( 12 ); break;
        case (0xCC2>>1): *dst= PSX_spu_voice_get_right_vol ( 12 ); break;
        case (0xCC4>>1): *dst= PSX_spu_voice_get_sample_rate ( 12 ); break;
        case (0xCC6>>1): *dst= PSX_spu_voice_get_start_addr ( 12 ); break;
        case (0xCC8>>1):
        case (0xCCA>>1): *dst= R16(PSX_spu_voice_get_adsr ( 12 ),port); break;
        case (0xCCC>>1): *dst= PSX_spu_voice_get_cur_vol ( 12 ); break;
        case (0xCCE>>1): *dst= PSX_spu_voice_get_repeat_addr ( 12 ); break;
        case (0xCD0>>1): *dst= PSX_spu_voice_get_left_vol ( 13 ); break;
        case (0xCD2>>1): *dst= PSX_spu_voice_get_right_vol ( 13 ); break;
        case (0xCD4>>1): *dst= PSX_spu_voice_get_sample_rate ( 13 ); break;
        case (0xCD6>>1): *dst= PSX_spu_voice_get_start_addr ( 13 ); break;
        case (0xCD8>>1):
        case (0xCDA>>1): *dst= R16(PSX_spu_voice_get_adsr ( 13 ),port); break;
        case (0xCDC>>1): *dst= PSX_spu_voice_get_cur_vol ( 13 ); break;
        case (0xCDE>>1): *dst= PSX_spu_voice_get_repeat_addr ( 13 ); break;
        case (0xCE0>>1): *dst= PSX_spu_voice_get_left_vol ( 14 ); break;
        case (0xCE2>>1): *dst= PSX_spu_voice_get_right_vol ( 14 ); break;
        case (0xCE4>>1): *dst= PSX_spu_voice_get_sample_rate ( 14 ); break;
        case (0xCE6>>1): *dst= PSX_spu_voice_get_start_addr ( 14 ); break;
        case (0xCE8>>1):
        case (0xCEA>>1): *dst= R16(PSX_spu_voice_get_adsr ( 14 ),port); break;
        case (0xCEC>>1): *dst= PSX_spu_voice_get_cur_vol ( 14 ); break;
        case (0xCEE>>1): *dst= PSX_spu_voice_get_repeat_addr ( 14 ); break;
        case (0xCF0>>1): *dst= PSX_spu_voice_get_left_vol ( 15 ); break;
        case (0xCF2>>1): *dst= PSX_spu_voice_get_right_vol ( 15 ); break;
        case (0xCF4>>1): *dst= PSX_spu_voice_get_sample_rate ( 15 ); break;
        case (0xCF6>>1): *dst= PSX_spu_voice_get_start_addr ( 15 ); break;
        case (0xCF8>>1):
        case (0xCFA>>1): *dst= R16(PSX_spu_voice_get_adsr ( 15 ),port); break;
        case (0xCFC>>1): *dst= PSX_spu_voice_get_cur_vol ( 15 ); break;
        case (0xCFE>>1): *dst= PSX_spu_voice_get_repeat_addr ( 15 ); break;
        case (0xD00>>1): *dst= PSX_spu_voice_get_left_vol ( 16 ); break;
        case (0xD02>>1): *dst= PSX_spu_voice_get_right_vol ( 16 ); break;
        case (0xD04>>1): *dst= PSX_spu_voice_get_sample_rate ( 16 ); break;
        case (0xD06>>1): *dst= PSX_spu_voice_get_start_addr ( 16 ); break;
        case (0xD08>>1):
        case (0xD0A>>1): *dst= R16(PSX_spu_voice_get_adsr ( 16 ),port); break;
        case (0xD0C>>1): *dst= PSX_spu_voice_get_cur_vol ( 16 ); break;
        case (0xD0E>>1): *dst= PSX_spu_voice_get_repeat_addr ( 16 ); break;
        case (0xD10>>1): *dst= PSX_spu_voice_get_left_vol ( 17 ); break;
        case (0xD12>>1): *dst= PSX_spu_voice_get_right_vol ( 17 ); break;
        case (0xD14>>1): *dst= PSX_spu_voice_get_sample_rate ( 17 ); break;
        case (0xD16>>1): *dst= PSX_spu_voice_get_start_addr ( 17 ); break;
        case (0xD18>>1):
        case (0xD1A>>1): *dst= R16(PSX_spu_voice_get_adsr ( 17 ),port); break;
        case (0xD1C>>1): *dst= PSX_spu_voice_get_cur_vol ( 17 ); break;
        case (0xD1E>>1): *dst= PSX_spu_voice_get_repeat_addr ( 17 ); break;
        case (0xD20>>1): *dst= PSX_spu_voice_get_left_vol ( 18 ); break;
        case (0xD22>>1): *dst= PSX_spu_voice_get_right_vol ( 18 ); break;
        case (0xD24>>1): *dst= PSX_spu_voice_get_sample_rate ( 18 ); break;
        case (0xD26>>1): *dst= PSX_spu_voice_get_start_addr ( 18 ); break;
        case (0xD28>>1):
        case (0xD2A>>1): *dst= R16(PSX_spu_voice_get_adsr ( 18 ),port); break;
        case (0xD2C>>1): *dst= PSX_spu_voice_get_cur_vol ( 18 ); break;
        case (0xD2E>>1): *dst= PSX_spu_voice_get_repeat_addr ( 18 ); break;
        case (0xD30>>1): *dst= PSX_spu_voice_get_left_vol ( 19 ); break;
        case (0xD32>>1): *dst= PSX_spu_voice_get_right_vol ( 19 ); break;
        case (0xD34>>1): *dst= PSX_spu_voice_get_sample_rate ( 19 ); break;
        case (0xD36>>1): *dst= PSX_spu_voice_get_start_addr ( 19 ); break;
        case (0xD38>>1):
        case (0xD3A>>1): *dst= R16(PSX_spu_voice_get_adsr ( 19 ),port); break;
        case (0xD3C>>1): *dst= PSX_spu_voice_get_cur_vol ( 19 ); break;
        case (0xD3E>>1): *dst= PSX_spu_voice_get_repeat_addr ( 19 ); break;
        case (0xD40>>1): *dst= PSX_spu_voice_get_left_vol ( 20 ); break;
        case (0xD42>>1): *dst= PSX_spu_voice_get_right_vol ( 20 ); break;
        case (0xD44>>1): *dst= PSX_spu_voice_get_sample_rate ( 20 ); break;
        case (0xD46>>1): *dst= PSX_spu_voice_get_start_addr ( 20 ); break;
        case (0xD48>>1):
        case (0xD4A>>1): *dst= R16(PSX_spu_voice_get_adsr ( 20 ),port); break;
        case (0xD4C>>1): *dst= PSX_spu_voice_get_cur_vol ( 20 ); break;
        case (0xD4E>>1): *dst= PSX_spu_voice_get_repeat_addr ( 20 ); break;
        case (0xD50>>1): *dst= PSX_spu_voice_get_left_vol ( 21 ); break;
        case (0xD52>>1): *dst= PSX_spu_voice_get_right_vol ( 21 ); break;
        case (0xD54>>1): *dst= PSX_spu_voice_get_sample_rate ( 21 ); break;
        case (0xD56>>1): *dst= PSX_spu_voice_get_start_addr ( 21 ); break;
        case (0xD58>>1):
        case (0xD5A>>1): *dst= R16(PSX_spu_voice_get_adsr ( 21 ),port); break;
        case (0xD5C>>1): *dst= PSX_spu_voice_get_cur_vol ( 21 ); break;
        case (0xD5E>>1): *dst= PSX_spu_voice_get_repeat_addr ( 21 ); break;
        case (0xD60>>1): *dst= PSX_spu_voice_get_left_vol ( 22 ); break;
        case (0xD62>>1): *dst= PSX_spu_voice_get_right_vol ( 22 ); break;
        case (0xD64>>1): *dst= PSX_spu_voice_get_sample_rate ( 22 ); break;
        case (0xD66>>1): *dst= PSX_spu_voice_get_start_addr ( 22 ); break;
        case (0xD68>>1):
        case (0xD6A>>1): *dst= R16(PSX_spu_voice_get_adsr ( 22 ),port); break;
        case (0xD6C>>1): *dst= PSX_spu_voice_get_cur_vol ( 22 ); break;
        case (0xD6E>>1): *dst= PSX_spu_voice_get_repeat_addr ( 22 ); break;
        case (0xD70>>1): *dst= PSX_spu_voice_get_left_vol ( 23 ); break;
        case (0xD72>>1): *dst= PSX_spu_voice_get_right_vol ( 23 ); break;
        case (0xD74>>1): *dst= PSX_spu_voice_get_sample_rate ( 23 ); break;
        case (0xD76>>1): *dst= PSX_spu_voice_get_start_addr ( 23 ); break;
        case (0xD78>>1):
        case (0xD7A>>1): *dst= R16(PSX_spu_voice_get_adsr ( 23 ),port); break;
        case (0xD7C>>1): *dst= PSX_spu_voice_get_cur_vol ( 23 ); break;
        case (0xD7E>>1): *dst= PSX_spu_voice_get_repeat_addr ( 23 ); break;
          // SPU Control Registers
        case (0xD80>>1): *dst= PSX_spu_get_left_vol (); break;
        case (0xD82>>1): *dst= PSX_spu_get_right_vol (); break;
        case (0xD84>>1): *dst= PSX_spu_reverb_get_vlout (); break;
        case (0xD86>>1): *dst= PSX_spu_reverb_get_vrout (); break;
        case (0xD88>>1):
        case (0xD8A>>1): *dst= R16(PSX_spu_get_kon (),port); break;
        case (0xD8C>>1):
        case (0xD8E>>1): *dst= R16(PSX_spu_get_koff (),port); break;
        case (0xD90>>1):
        case (0xD92>>1): *dst= R16(PSX_spu_get_pmon (),port); break;
        case (0xD94>>1):
        case (0xD96>>1): *dst= R16(PSX_spu_get_non (),port); break;
        case (0xD98>>1):
        case (0xD9A>>1): *dst= R16(PSX_spu_get_eon (),port); break;
        case (0xD9C>>1):
        case (0xD9E>>1): *dst= R16(PSX_spu_get_endx (),port); break;
        case (0xDA0>>1): *dst= PSX_spu_get_unk_da0 (); break;
        case (0xDA2>>1): *dst= PSX_spu_reverb_get_mbase (); break;
        case (0xDA4>>1): *dst= PSX_spu_get_irq_addr (); break;
        case (0xDA6>>1): *dst= PSX_spu_get_addr (); break;
        case (0xDA8>>1):
          *dst= 0;
          printf ( "R16 port: 0xDA8: cal implementar??\n" );
          break;
        case (0xDAA>>1): *dst= PSX_spu_get_control (); break;
        case (0xDAC>>1): *dst= PSX_spu_get_transfer_type (); break;
        case (0xDAE>>1): *dst= PSX_spu_get_status (); break;
        case (0xDB0>>1):
        case (0xDB2>>1): *dst= R16(PSX_spu_get_cd_vol (),port); break;
        case (0xDB4>>1):
        case (0xDB6>>1): *dst= R16(PSX_spu_get_ext_vol (),port); break;
        case (0xDB8>>1):
        case (0xDBA>>1): *dst= R16(PSX_spu_get_cur_vol_lr (),port); break;
        case (0xDBC>>1): *dst= PSX_spu_get_unk_dbc ( 0 ); break;
        case (0xDBE>>1): *dst= PSX_spu_get_unk_dbc ( 1 ); break;
          // SPU Reverb Configuration Area
        case (0xDC0>>1) ... (0xDFE>>1):
          *dst= PSX_spu_reverb_get_reg ( aux&0x1F ); break;
          // SPU Internal Registers
        case (0xE00>>1):
        case (0xE02>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 0 ),port);
          break;
        case (0xE04>>1):
        case (0xE06>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 1 ),port);
          break;
        case (0xE08>>1):
        case (0xE0A>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 2 ),port);
          break;
        case (0xE0C>>1):
        case (0xE0E>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 3 ),port);
          break;
        case (0xE10>>1):
        case (0xE12>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 4 ),port);
          break;
        case (0xE14>>1):
        case (0xE16>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 5 ),port);
          break;
        case (0xE18>>1):
        case (0xE1A>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 6 ),port);
          break;
        case (0xE1C>>1):
        case (0xE1E>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 7 ),port);
          break;
        case (0xE20>>1):
        case (0xE22>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 8 ),port);
          break;
        case (0xE24>>1):
        case (0xE26>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 9 ),port);
          break;
        case (0xE28>>1):
        case (0xE2A>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 10 ),port);
          break;
        case (0xE2C>>1):
        case (0xE2E>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 11 ),port);
          break;
        case (0xE30>>1):
        case (0xE32>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 12 ),port);
          break;
        case (0xE34>>1):
        case (0xE36>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 13 ),port);
          break;
        case (0xE38>>1):
        case (0xE3A>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 14 ),port);
          break;
        case (0xE3C>>1):
        case (0xE3E>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 15 ),port);
          break;
        case (0xE40>>1):
        case (0xE42>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 16 ),port);
          break;
        case (0xE44>>1):
        case (0xE46>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 17 ),port);
          break;
        case (0xE48>>1):
        case (0xE4A>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 18 ),port);
          break;
        case (0xE4C>>1):
        case (0xE4E>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 19 ),port);
          break;
        case (0xE50>>1):
        case (0xE52>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 20 ),port);
          break;
        case (0xE54>>1):
        case (0xE56>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 21 ),port);
          break;
        case (0xE58>>1):
        case (0xE5A>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 22 ),port);
          break;
        case (0xE5C>>1):
        case (0xE5E>>1):
          *dst= R16(PSX_spu_voice_get_cur_vol_lr ( 23 ),port);
          break;
        case (0xE60>>1) ... (0xE7E>>1):
          *dst= PSX_spu_get_unk_e60 ( aux&0xF );
          break;
        case (0xE80>>1) ... (0xFFE>>1):
          *dst= 0xFFFF;
          break;
          // Locked
        default:
          printf("R16 I/O (0x%x)\nCal return false!!\n",port<<1);
          /*return false;*/ *dst= 0;
        }
    }
  
  /* Expansion Region 2 i 3 */
  else if ( aux < (0x1FC00000>>1) )
    {
      if ( aux >= _exp2.addr16 && aux < _exp2.ds.end16 )
        {
          printf("R16 Expansion 2\n");
          *dst= 0;
        }
      else if ( aux >= (0x1FA00000>>1) && aux < _exp3.end16 )
        {
          printf("R16 Expansion 3\n");
          *dst= 0;
        }
      else return false;
    }
  
  /* BIOS i resta fins 1FFFFFFF */
  else
    {
      if ( aux < _bios.ds.end16 )
        {
#ifdef PSX_LE
          if ( !is_le ) aux^= 1;
#else
          if ( is_le ) aux^= 1;
#endif
          *dst= ((uint16_t *) _bios.v)[aux&BIOS_MASK_16];
        }
      else return false;
    }
  
  return true;
  
} /* end mem_read16 */


static bool
mem_read8 (
           const uint32_t  addr,
           uint8_t        *dst,
           const bool      is_le
           )
{
  
  uint32_t aux;
  int port;
  
  
  aux= addr;
  
  /* RAM. */
  if ( aux < _ram.end_ram8 )
    {
#ifdef PSX_LE
      if ( !is_le ) aux^= 3;
#else
      if ( is_le ) aux^= 3;
#endif
      *dst= _ram.v[aux&RAM_MASK];
    }
  
  /* La resta de l'àrea de la RAM. */
  else if ( aux <= 0x00800000 )
    {
      if ( aux < _ram.end_hz8 ) *dst= 0xFF;
      else if ( aux == 0x00800000 && !_ram.locked_00800000 ) *dst= 0;
      else return false;
    }

  /* Àrea en blanc. */
  else if ( aux < 0x1F000000 ) return false;
  
  /* Expansion Region 1 */
  else if ( aux < 0x1F800000 )
    {
      if ( aux >= _exp1.addr8 && aux < _exp1.ds.end8 )
        {
          printf("R8 Expansion 1\n");
          *dst= 0;
        }
      else return false;
    }

  /* Scratchpad */
  else if ( aux < 0x1F800400 )
    {
#ifdef PSX_LE
      if ( !is_le ) aux^= 3;
#else
      if ( is_le ) aux^= 3;
#endif  
      *dst= _scratchpad[aux&SP_MASK];
    }

  /* Region after scratchpad */
  else if ( aux < 0x1F801000 ) return false;
  
  /* I/O Ports */
  else if ( aux < 0x1F802000 )
    {
      port= aux&0xFFF;
      switch ( port )
        {
          // Memory Control 1
        case 0x000:
        case 0x001:
        case 0x002:
        case 0x003: *dst= R8(_exp1.addr32,port); break;
        case 0x004:
        case 0x005:
        case 0x006:
        case 0x007: *dst= R8(_exp2.addr32,port); break;
        case 0x008:
        case 0x009:
        case 0x00A:
        case 0x00B: *dst= R8(_exp1.ds.reg_val,port); break;
        case 0x00C:
        case 0x00D:
        case 0x00E:
        case 0x00F: *dst= R8(_exp3.reg_val,port); break;
        case 0x010:
        case 0x011:
        case 0x012:
        case 0x013: *dst= R8(_bios.ds.reg_val,port); break;
        case 0x014:
        case 0x015:
        case 0x016:
        case 0x017: *dst= R8(_spu,port); break;
        case 0x018:
        case 0x019:
        case 0x01A:
        case 0x01B: *dst= R8(_cdrom,port); break;
        case 0x01C:
        case 0x01D:
        case 0x01E:
        case 0x01F: *dst= R8(_exp2.ds.reg_val,port); break;
        case 0x020:
        case 0x021:
        case 0x022:
        case 0x023: *dst= R8(_com,port); break;
          // Peripheral I/O Ports
        case 0x040:
        case 0x041:
        case 0x042:
        case 0x043: *dst= R8(PSX_joy_rx_data (),port); break;
        case 0x044:
        case 0x045:
        case 0x046:
        case 0x047: *dst= R8(PSX_joy_stat (),port); break;
        case 0x048:
        case 0x049: *dst= R8F16(PSX_joy_mode_read (),port); break;
        case 0x04A:
        case 0x04B: *dst= R8F16(PSX_joy_ctrl_read (),port); break;
        case 0x04C:
        case 0x04D: *dst= 0; break; // <-- No hi ha res
        case 0x04E:
        case 0x04F: *dst= R8F16(PSX_joy_baud_read (),port); break;
          // Memory Control 2
        case 0x060:
        case 0x061:
        case 0x062:
        case 0x063: *dst= R8(_ram.ram_size,port); break;
          // Interrupt Control
        case 0x070:
        case 0x071:
        case 0x072:
        case 0x073: *dst= R8(PSX_int_read_state (),port); break;
        case 0x074:
        case 0x075:
        case 0x076:
        case 0x077: *dst= R8(PSX_int_read_imask (),port); break;
          // DMA Registers
        case 0x080:
        case 0x081:
        case 0x082:
        case 0x083: *dst= R8(PSX_dma_madr_read ( 0 ),port); break;
        case 0x084:
        case 0x085:
        case 0x086:
        case 0x087: *dst= R8(PSX_dma_bcr_read ( 0 ),port); break;
        case 0x088:
        case 0x089:
        case 0x08A:
        case 0x08B: *dst= R8(PSX_dma_chcr_read ( 0 ),port); break;
        case 0x090:
        case 0x091:
        case 0x092:
        case 0x093: *dst= R8(PSX_dma_madr_read ( 1 ),port); break;
        case 0x094:
        case 0x095:
        case 0x096:
        case 0x097: *dst= R8(PSX_dma_bcr_read ( 1 ),port); break;
        case 0x098:
        case 0x099:
        case 0x09A:
        case 0x09B: *dst= R8(PSX_dma_chcr_read ( 1 ),port); break;
        case 0x0A0:
        case 0x0A1:
        case 0x0A2:
        case 0x0A3: *dst= R8(PSX_dma_madr_read ( 2 ),port); break;
        case 0x0A4:
        case 0x0A5:
        case 0x0A6:
        case 0x0A7: *dst= R8(PSX_dma_bcr_read ( 2 ),port); break;
        case 0x0A8:
        case 0x0A9:
        case 0x0AA:
        case 0x0AB: *dst= R8(PSX_dma_chcr_read ( 2 ),port); break;
        case 0x0B0:
        case 0x0B1:
        case 0x0B2:
        case 0x0B3: *dst= R8(PSX_dma_madr_read ( 3 ),port); break;
        case 0x0B4:
        case 0x0B5:
        case 0x0B6:
        case 0x0B7: *dst= R8(PSX_dma_bcr_read ( 3 ),port); break;
        case 0x0B8:
        case 0x0B9:
        case 0x0BA:
        case 0x0BB: *dst= R8(PSX_dma_chcr_read ( 3 ),port); break;
        case 0x0C0:
        case 0x0C1:
        case 0x0C2:
        case 0x0C3: *dst= R8(PSX_dma_madr_read ( 4 ),port); break;
        case 0x0C4:
        case 0x0C5:
        case 0x0C6:
        case 0x0C7: *dst= R8(PSX_dma_bcr_read ( 4 ),port); break;
        case 0x0C8:
        case 0x0C9:
        case 0x0CA:
        case 0x0CB: *dst= R8(PSX_dma_chcr_read ( 4 ),port); break;
        case 0x0D0:
        case 0x0D1:
        case 0x0D2:
        case 0x0D3: *dst= R8(PSX_dma_madr_read ( 5 ),port); break;
        case 0x0D4:
        case 0x0D5:
        case 0x0D6:
        case 0x0D7: *dst= R8(PSX_dma_bcr_read ( 5 ),port); break;
        case 0x0D8:
        case 0x0D9:
        case 0x0DA:
        case 0x0DB: *dst= R8(PSX_dma_chcr_read ( 5 ),port); break;
        case 0x0E0:
        case 0x0E1:
        case 0x0E2:
        case 0x0E3: *dst= R8(PSX_dma_madr_read ( 6 ),port); break;
        case 0x0E4:
        case 0x0E5:
        case 0x0E6:
        case 0x0E7: *dst= R8(PSX_dma_bcr_read ( 6 ),port); break;
        case 0x0E8:
        case 0x0E9:
        case 0x0EA:
        case 0x0EB: *dst= R8(PSX_dma_chcr_read ( 6 ),port); break;
        case 0x0F0:
        case 0x0F1:
        case 0x0F2:
        case 0x0F3: *dst= R8(PSX_dma_dpcr_read (),port); break;
        case 0x0F4:
        case 0x0F5:
        case 0x0F6:
        case 0x0F7: *dst= R8(PSX_dma_dicr_read (),port); break;
        case 0x0F8:
        case 0x0F9:
        case 0x0FA:
        case 0x0FB: *dst= R8(PSX_dma_unk1_read (),port); break;
        case 0x0FC:
        case 0x0FD:
        case 0x0FE:
        case 0x0FF: *dst= R8(PSX_dma_unk2_read (),port); break;
          // Timers
        case 0x100:
        case 0x101:
        case 0x102:
        case 0x103: *dst= R8(PSX_timers_get_counter_value ( 0 ),port); break;
        case 0x104:
        case 0x105:
        case 0x106:
        case 0x107: *dst= R8(PSX_timers_get_counter_mode ( 0 ),port); break;
        case 0x108:
        case 0x109:
        case 0x10A:
        case 0x10B: *dst= R8(PSX_timers_get_target_value ( 0 ),port); break;
        case 0x110:
        case 0x111:
        case 0x112:
        case 0x113: *dst= R8(PSX_timers_get_counter_value ( 1 ),port); break;
        case 0x114:
        case 0x115:
        case 0x116:
        case 0x117: *dst= R8(PSX_timers_get_counter_mode ( 1 ),port); break;
        case 0x118:
        case 0x119:
        case 0x11A:
        case 0x11B: *dst= R8(PSX_timers_get_target_value ( 1 ),port); break;
        case 0x120:
        case 0x121:
        case 0x122:
        case 0x123: *dst= R8(PSX_timers_get_counter_value ( 2 ),port); break;
        case 0x124:
        case 0x125:
        case 0x126:
        case 0x127: *dst= R8(PSX_timers_get_counter_mode ( 2 ),port); break;
        case 0x128:
        case 0x129:
        case 0x12A:
        case 0x12B: *dst= R8(PSX_timers_get_target_value ( 2 ),port); break;
          // CDROM Registers
        case 0x800: *dst= PSX_cd_status (); break;
        case 0x801: *dst= PSX_cd_port1_read (); break;
        case 0x802: *dst= PSX_cd_port2_read (); break;
        case 0x803: *dst= PSX_cd_port3_read (); break;
          // GPU Registers
        case 0x810:
        case 0x811:
        case 0x812:
        case 0x813: *dst= R8(PSX_gpu_read (),port); break;
        case 0x814:
        case 0x815:
        case 0x816:
        case 0x817: *dst= R8(PSX_gpu_stat (),port); break;
          // MDEC Registers
        case 0x820:
        case 0x821:
        case 0x822:
        case 0x823: *dst= R8(PSX_mdec_data_read (),port); break;
        case 0x824:
        case 0x825:
        case 0x826:
        case 0x827: *dst= R8(PSX_mdec_status (),port); break;
          // SPU Voice 0..23 Registers
        case 0xC00:
        case 0xC01:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 0 ),port);
          break;
        case 0xC02:
        case 0xC03:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 0 ),port);
          break;
        case 0xC04:
        case 0xC05:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 0 ),port);
          break;
        case 0xC06:
        case 0xC07:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 0 ),port);
          break;
        case 0xC08:
        case 0xC09:
        case 0xC0A:
        case 0xC0B: *dst= R8(PSX_spu_voice_get_adsr ( 0 ),port); break;
        case 0xC0C:
        case 0xC0D:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 0 ),port);
          break;
        case 0xC0E:
        case 0xC0F:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 0 ),port);
          break;
        case 0xC10:
        case 0xC11:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 1 ),port);
          break;
        case 0xC12:
        case 0xC13:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 1 ),port);
          break;
        case 0xC14:
        case 0xC15:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 1 ),port);
          break;
        case 0xC16:
        case 0xC17:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 1 ),port);
          break;
        case 0xC18:
        case 0xC19:
        case 0xC1A:
        case 0xC1B: *dst= R8(PSX_spu_voice_get_adsr ( 1 ),port); break;
        case 0xC1C:
        case 0xC1D:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 1 ),port);
          break;
        case 0xC1E:
        case 0xC1F:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 1 ),port);
          break;
        case 0xC20:
        case 0xC21:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 2 ),port);
          break;
        case 0xC22:
        case 0xC23:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 2 ),port);
          break;
        case 0xC24:
        case 0xC25:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 2 ),port);
          break;
        case 0xC26:
        case 0xC27:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 2 ),port);
          break;
        case 0xC28:
        case 0xC29:
        case 0xC2A:
        case 0xC2B: *dst= R8(PSX_spu_voice_get_adsr ( 2 ),port); break;
        case 0xC2C:
        case 0xC2D:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 2 ),port);
          break;
        case 0xC2E:
        case 0xC2F:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 2 ),port);
          break;
        case 0xC30:
        case 0xC31:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 3 ),port);
          break;
        case 0xC32:
        case 0xC33:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 3 ),port);
          break;
        case 0xC34:
        case 0xC35:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 3 ),port);
          break;
        case 0xC36:
        case 0xC37:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 3 ),port);
          break;
        case 0xC38:
        case 0xC39:
        case 0xC3A:
        case 0xC3B: *dst= R8(PSX_spu_voice_get_adsr ( 3 ),port); break;
        case 0xC3C:
        case 0xC3D:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 3 ),port);
          break;
        case 0xC3E:
        case 0xC3F:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 3 ),port);
          break;
        case 0xC40:
        case 0xC41:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 4 ),port);
          break;
        case 0xC42:
        case 0xC43:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 4 ),port);
          break;
        case 0xC44:
        case 0xC45:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 4 ),port);
          break;
        case 0xC46:
        case 0xC47:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 4 ),port);
          break;
        case 0xC48:
        case 0xC49:
        case 0xC4A:
        case 0xC4B: *dst= R8(PSX_spu_voice_get_adsr ( 4 ),port); break;
        case 0xC4C:
        case 0xC4D:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 4 ),port);
          break;
        case 0xC4E:
        case 0xC4F:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 4 ),port);
          break;
        case 0xC50:
        case 0xC51:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 5 ),port);
          break;
        case 0xC52:
        case 0xC53:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 5 ),port);
          break;
        case 0xC54:
        case 0xC55:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 5 ),port);
          break;
        case 0xC56:
        case 0xC57:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 5 ),port);
          break;
        case 0xC58:
        case 0xC59:
        case 0xC5A:
        case 0xC5B: *dst= R8(PSX_spu_voice_get_adsr ( 5 ),port); break;
        case 0xC5C:
        case 0xC5D:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 5 ),port);
          break;
        case 0xC5E:
        case 0xC5F:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 5 ),port);
          break;
        case 0xC60:
        case 0xC61:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 6 ),port);
          break;
        case 0xC62:
        case 0xC63:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 6 ),port);
          break;
        case 0xC64:
        case 0xC65:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 6 ),port);
          break;
        case 0xC66:
        case 0xC67:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 6 ),port);
          break;
        case 0xC68:
        case 0xC69:
        case 0xC6A:
        case 0xC6B: *dst= R8(PSX_spu_voice_get_adsr ( 6 ),port); break;
        case 0xC6C:
        case 0xC6D:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 6 ),port);
          break;
        case 0xC6E:
        case 0xC6F:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 6 ),port);
          break;
        case 0xC70:
        case 0xC71:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 7 ),port);
          break;
        case 0xC72:
        case 0xC73:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 7 ),port);
          break;
        case 0xC74:
        case 0xC75:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 7 ),port);
          break;
        case 0xC76:
        case 0xC77:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 7 ),port);
          break;
        case 0xC78:
        case 0xC79:
        case 0xC7A:
        case 0xC7B: *dst= R8(PSX_spu_voice_get_adsr ( 7 ),port); break;
        case 0xC7C:
        case 0xC7D:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 7 ),port);
          break;
        case 0xC7E:
        case 0xC7F:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 7 ),port);
          break;
        case 0xC80:
        case 0xC81:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 8 ),port);
          break;
        case 0xC82:
        case 0xC83:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 8 ),port);
          break;
        case 0xC84:
        case 0xC85:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 8 ),port);
          break;
        case 0xC86:
        case 0xC87:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 8 ),port);
          break;
        case 0xC88:
        case 0xC89:
        case 0xC8A:
        case 0xC8B: *dst= R8(PSX_spu_voice_get_adsr ( 8 ),port); break;
        case 0xC8C:
        case 0xC8D:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 8 ),port);
          break;
        case 0xC8E:
        case 0xC8F:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 8 ),port);
          break;
        case 0xC90:
        case 0xC91:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 9 ),port);
          break;
        case 0xC92:
        case 0xC93:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 9 ),port);
          break;
        case 0xC94:
        case 0xC95:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 9 ),port);
          break;
        case 0xC96:
        case 0xC97:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 9 ),port);
          break;
        case 0xC98:
        case 0xC99:
        case 0xC9A:
        case 0xC9B: *dst= R8(PSX_spu_voice_get_adsr ( 9 ),port); break;
        case 0xC9C:
        case 0xC9D:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 9 ),port);
          break;
        case 0xC9E:
        case 0xC9F:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 9 ),port);
          break;
        case 0xCA0:
        case 0xCA1:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 10 ),port);
          break;
        case 0xCA2:
        case 0xCA3:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 10 ),port);
          break;
        case 0xCA4:
        case 0xCA5:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 10 ),port);
          break;
        case 0xCA6:
        case 0xCA7:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 10 ),port);
          break;
        case 0xCA8:
        case 0xCA9:
        case 0xCAA:
        case 0xCAB: *dst= R8(PSX_spu_voice_get_adsr ( 10 ),port); break;
        case 0xCAC:
        case 0xCAD:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 10 ),port);
          break;
        case 0xCAE:
        case 0xCAF:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 10 ),port);
          break;
        case 0xCB0:
        case 0xCB1:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 11 ),port);
          break;
        case 0xCB2:
        case 0xCB3:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 11 ),port);
          break;
        case 0xCB4:
        case 0xCB5:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 11 ),port);
          break;
        case 0xCB6:
        case 0xCB7:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 11 ),port);
          break;
        case 0xCB8:
        case 0xCB9:
        case 0xCBA:
        case 0xCBB: *dst= R8(PSX_spu_voice_get_adsr ( 11 ),port); break;
        case 0xCBC:
        case 0xCBD:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 11 ),port);
          break;
        case 0xCBE:
        case 0xCBF:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 11 ),port);
          break;
        case 0xCC0:
        case 0xCC1:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 12 ),port);
          break;
        case 0xCC2:
        case 0xCC3:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 12 ),port);
          break;
        case 0xCC4:
        case 0xCC5:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 12 ),port);
          break;
        case 0xCC6:
        case 0xCC7:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 12 ),port);
          break;
        case 0xCC8:
        case 0xCC9:
        case 0xCCA:
        case 0xCCB: *dst= R8(PSX_spu_voice_get_adsr ( 12 ),port); break;
        case 0xCCC:
        case 0xCCD:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 12 ),port);
          break;
        case 0xCCE:
        case 0xCCF:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 12 ),port);
          break;
        case 0xCD0:
        case 0xCD1:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 13 ),port);
          break;
        case 0xCD2:
        case 0xCD3:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 13 ),port);
          break;
        case 0xCD4:
        case 0xCD5:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 13 ),port);
          break;
        case 0xCD6:
        case 0xCD7:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 13 ),port);
          break;
        case 0xCD8:
        case 0xCD9:
        case 0xCDA:
        case 0xCDB: *dst= R8(PSX_spu_voice_get_adsr ( 13 ),port); break;
        case 0xCDC:
        case 0xCDD:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 13 ),port);
          break;
        case 0xCDE:
        case 0xCDF:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 13 ),port);
          break;
        case 0xCE0:
        case 0xCE1:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 14 ),port);
          break;
        case 0xCE2:
        case 0xCE3:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 14 ),port);
          break;
        case 0xCE4:
        case 0xCE5:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 14 ),port);
          break;
        case 0xCE6:
        case 0xCE7:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 14 ),port);
          break;
        case 0xCE8:
        case 0xCE9:
        case 0xCEA:
        case 0xCEB: *dst= R8(PSX_spu_voice_get_adsr ( 14 ),port); break;
        case 0xCEC:
        case 0xCED:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 14 ),port);
          break;
        case 0xCEE:
        case 0xCEF:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 14 ),port);
          break;
        case 0xCF0:
        case 0xCF1:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 15 ),port);
          break;
        case 0xCF2:
        case 0xCF3:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 15 ),port);
          break;
        case 0xCF4:
        case 0xCF5:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 15 ),port);
          break;
        case 0xCF6:
        case 0xCF7:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 15 ),port);
          break;
        case 0xCF8:
        case 0xCF9:
        case 0xCFA:
        case 0xCFB: *dst= R8(PSX_spu_voice_get_adsr ( 15 ),port); break;
        case 0xCFC:
        case 0xCFD:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 15 ),port);
          break;
        case 0xCFE:
        case 0xCFF:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 15 ),port);
          break;
        case 0xD00:
        case 0xD01:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 16 ),port);
          break;
        case 0xD02:
        case 0xD03:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 16 ),port);
          break;
        case 0xD04:
        case 0xD05:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 16 ),port);
          break;
        case 0xD06:
        case 0xD07:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 16 ),port);
          break;
        case 0xD08:
        case 0xD09:
        case 0xD0A:
        case 0xD0B: *dst= R8(PSX_spu_voice_get_adsr ( 16 ),port); break;
        case 0xD0C:
        case 0xD0D:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 16 ),port);
          break;
        case 0xD0E:
        case 0xD0F:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 16 ),port);
          break;
        case 0xD10:
        case 0xD11:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 17 ),port);
          break;
        case 0xD12:
        case 0xD13:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 17 ),port);
          break;
        case 0xD14:
        case 0xD15:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 17 ),port);
          break;
        case 0xD16:
        case 0xD17:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 17 ),port);
          break;
        case 0xD18:
        case 0xD19:
        case 0xD1A:
        case 0xD1B: *dst= R8(PSX_spu_voice_get_adsr ( 17 ),port); break;
        case 0xD1C:
        case 0xD1D:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 17 ),port);
          break;
        case 0xD1E:
        case 0xD1F:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 17 ),port);
          break;
        case 0xD20:
        case 0xD21:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 18 ),port);
          break;
        case 0xD22:
        case 0xD23:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 18 ),port);
          break;
        case 0xD24:
        case 0xD25:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 18 ),port);
          break;
        case 0xD26:
        case 0xD27:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 18 ),port);
          break;
        case 0xD28:
        case 0xD29:
        case 0xD2A:
        case 0xD2B: *dst= R8(PSX_spu_voice_get_adsr ( 18 ),port); break;
        case 0xD2C:
        case 0xD2D:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 18 ),port);
          break;
        case 0xD2E:
        case 0xD2F:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 18 ),port);
          break;
        case 0xD30:
        case 0xD31:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 19 ),port);
          break;
        case 0xD32:
        case 0xD33:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 19 ),port);
          break;
        case 0xD34:
        case 0xD35:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 19 ),port);
          break;
        case 0xD36:
        case 0xD37:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 19 ),port);
          break;
        case 0xD38:
        case 0xD39:
        case 0xD3A:
        case 0xD3B: *dst= R8(PSX_spu_voice_get_adsr ( 19 ),port); break;
        case 0xD3C:
        case 0xD3D:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 19 ),port);
          break;
        case 0xD3E:
        case 0xD3F:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 19 ),port);
          break;
        case 0xD40:
        case 0xD41:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 20 ),port);
          break;
        case 0xD42:
        case 0xD43:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 20 ),port);
          break;
        case 0xD44:
        case 0xD45:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 20 ),port);
          break;
        case 0xD46:
        case 0xD47:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 20 ),port);
          break;
        case 0xD48:
        case 0xD49:
        case 0xD4A:
        case 0xD4B: *dst= R8(PSX_spu_voice_get_adsr ( 20 ),port); break;
        case 0xD4C:
        case 0xD4D:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 20 ),port);
          break;
        case 0xD4E:
        case 0xD4F:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 20 ),port);
          break;
        case 0xD50:
        case 0xD51:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 21 ),port);
          break;
        case 0xD52:
        case 0xD53:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 21 ),port);
          break;
        case 0xD54:
        case 0xD55:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 21 ),port);
          break;
        case 0xD56:
        case 0xD57:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 21 ),port);
          break;
        case 0xD58:
        case 0xD59:
        case 0xD5A:
        case 0xD5B: *dst= R8(PSX_spu_voice_get_adsr ( 21 ),port); break;
        case 0xD5C:
        case 0xD5D:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 21 ),port);
          break;
        case 0xD5E:
        case 0xD5F:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 21 ),port);
          break;
        case 0xD60:
        case 0xD61:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 22 ),port);
          break;
        case 0xD62:
        case 0xD63:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 22 ),port);
          break;
        case 0xD64:
        case 0xD65:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 22 ),port);
          break;
        case 0xD66:
        case 0xD67:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 22 ),port);
          break;
        case 0xD68:
        case 0xD69:
        case 0xD6A:
        case 0xD6B: *dst= R8(PSX_spu_voice_get_adsr ( 22 ),port); break;
        case 0xD6C:
        case 0xD6D:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 22 ),port);
          break;
        case 0xD6E:
        case 0xD6F:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 22 ),port);
          break;
        case 0xD70:
        case 0xD71:
          *dst= R8F16(PSX_spu_voice_get_left_vol ( 23 ),port);
          break;
        case 0xD72:
        case 0xD73:
          *dst= R8F16(PSX_spu_voice_get_right_vol ( 23 ),port);
          break;
        case 0xD74:
        case 0xD75:
          *dst= R8F16(PSX_spu_voice_get_sample_rate ( 23 ),port);
          break;
        case 0xD76:
        case 0xD77:
          *dst= R8F16(PSX_spu_voice_get_start_addr ( 23 ),port);
          break;
        case 0xD78:
        case 0xD79:
        case 0xD7A:
        case 0xD7B: *dst= R8(PSX_spu_voice_get_adsr ( 23 ),port); break;
        case 0xD7C:
        case 0xD7D:
          *dst= R8F16(PSX_spu_voice_get_cur_vol ( 23 ),port);
          break;
        case 0xD7E:
        case 0xD7F:
          *dst= R8F16(PSX_spu_voice_get_repeat_addr ( 23 ),port);
          break;
          // SPU Control Registers
        case 0xD80:
        case 0xD81: *dst= R8F16(PSX_spu_get_left_vol (),port); break;
        case 0xD82:
        case 0xD83: *dst= R8F16(PSX_spu_get_right_vol (),port); break;
        case 0xD84:
        case 0xD85: *dst= R8F16(PSX_spu_reverb_get_vlout (),port); break;
        case 0xD86:
        case 0xD87: *dst= R8F16(PSX_spu_reverb_get_vrout (),port); break;
        case 0xD88:
        case 0xD89:
        case 0xD8A:
        case 0xD8B: *dst= R8(PSX_spu_get_kon (),port); break;
        case 0xD8C:
        case 0xD8D:
        case 0xD8E:
        case 0xD8F: *dst= R8(PSX_spu_get_koff (),port); break;
        case 0xD90:
        case 0xD91:
        case 0xD92:
        case 0xD93: *dst= R8(PSX_spu_get_pmon (),port); break;
        case 0xD94:
        case 0xD95:
        case 0xD96:
        case 0xD97: *dst= R8(PSX_spu_get_non (),port); break;
        case 0xD98:
        case 0xD99:
        case 0xD9A:
        case 0xD9B: *dst= R8(PSX_spu_get_eon (),port); break;
        case 0xD9C:
        case 0xD9D:
        case 0xD9E:
        case 0xD9F: *dst= R8(PSX_spu_get_endx (),port); break;
        case 0xDA0:
        case 0xDA1: *dst= R8F16(PSX_spu_get_unk_da0 (),port); break;
        case 0xDA2:
        case 0xDA3: *dst= R8F16(PSX_spu_reverb_get_mbase (),port); break;
        case 0xDA4:
        case 0xDA5: *dst= R8F16(PSX_spu_get_irq_addr (),port); break;
        case 0xDA6:
        case 0xDA7: *dst= R8F16(PSX_spu_get_addr (),port); break;
        case 0xDA8:
        case 0xDA9:
          *dst= 0;
          printf ( "R8 port: 0xDA8..0xDA9: cal implementar??\n" );
          break;
        case 0xDAA:
        case 0xDAB: *dst= R8F16(PSX_spu_get_control (),port); break;
        case 0xDAC:
        case 0xDAD: *dst= R8F16(PSX_spu_get_transfer_type (),port); break;
        case 0xDAE:
        case 0xDAF: *dst= R8F16(PSX_spu_get_status (),port); break;
        case 0xDB0:
        case 0xDB1:
        case 0xDB2:
        case 0xDB3: *dst= R8(PSX_spu_get_cd_vol (),port); break;
        case 0xDB4:
        case 0xDB5:
        case 0xDB6:
        case 0xDB7: *dst= R8(PSX_spu_get_ext_vol (),port); break;
        case 0xDB8:
        case 0xDB9:
        case 0xDBA:
        case 0xDBB: *dst= R8(PSX_spu_get_cur_vol_lr (),port); break;
        case 0xDBC ... 0xDBF:
          *dst= R8F16(PSX_spu_get_unk_dbc ( (port>>1)&0x1 ),port);
          break;
          // SPU Reverb Configuration Area
        case 0xDC0 ... 0xDFF:
          *dst= R8F16(PSX_spu_reverb_get_reg ( (port>>1)&0x1F ),port);
          break;
          // SPU Internal Registers
        case 0xE00:
        case 0xE01:
        case 0xE02:
        case 0xE03: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 0 ),port); break;
        case 0xE04:
        case 0xE05:
        case 0xE06:
        case 0xE07: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 1 ),port); break;
        case 0xE08:
        case 0xE09:
        case 0xE0A:
        case 0xE0B: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 2 ),port); break;
        case 0xE0C:
        case 0xE0D:
        case 0xE0E:
        case 0xE0F: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 3 ),port); break;
        case 0xE10:
        case 0xE11:
        case 0xE12:
        case 0xE13: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 4 ),port); break;
        case 0xE14:
        case 0xE15:
        case 0xE16:
        case 0xE17: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 5 ),port); break;
        case 0xE18:
        case 0xE19:
        case 0xE1A:
        case 0xE1B: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 6 ),port); break;
        case 0xE1C:
        case 0xE1D:
        case 0xE1E:
        case 0xE1F: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 7 ),port); break;
        case 0xE20:
        case 0xE21:
        case 0xE22:
        case 0xE23: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 8 ),port); break;
        case 0xE24:
        case 0xE25:
        case 0xE26:
        case 0xE27: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 9 ),port); break;
        case 0xE28:
        case 0xE29:
        case 0xE2A:
        case 0xE2B: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 10 ),port); break;
        case 0xE2C:
        case 0xE2D:
        case 0xE2E:
        case 0xE2F: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 11 ),port); break;
        case 0xE30:
        case 0xE31:
        case 0xE32:
        case 0xE33: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 12 ),port); break;
        case 0xE34:
        case 0xE35:
        case 0xE36:
        case 0xE37: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 13 ),port); break;
        case 0xE38:
        case 0xE39:
        case 0xE3A:
        case 0xE3B: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 14 ),port); break;
        case 0xE3C:
        case 0xE3D:
        case 0xE3E:
        case 0xE3F: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 15 ),port); break;
        case 0xE40:
        case 0xE41:
        case 0xE42:
        case 0xE43: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 16 ),port); break;
        case 0xE44:
        case 0xE45:
        case 0xE46:
        case 0xE47: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 17 ),port); break;
        case 0xE48:
        case 0xE49:
        case 0xE4A:
        case 0xE4B: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 18 ),port); break;
        case 0xE4C:
        case 0xE4D:
        case 0xE4E:
        case 0xE4F: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 19 ),port); break;
        case 0xE50:
        case 0xE51:
        case 0xE52:
        case 0xE53: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 20 ),port); break;
        case 0xE54:
        case 0xE55:
        case 0xE56:
        case 0xE57: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 21 ),port); break;
        case 0xE58:
        case 0xE59:
        case 0xE5A:
        case 0xE5B: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 22 ),port); break;
        case 0xE5C:
        case 0xE5D:
        case 0xE5E:
        case 0xE5F: *dst= R8(PSX_spu_voice_get_cur_vol_lr ( 23 ),port); break;
        case 0xE60 ... 0xE7F:
          *dst= R8F16(PSX_spu_get_unk_e60 ( (port>>1)&0xF ),port);
          break;
        case 0xE80 ... 0xFFF:
          *dst= 0xFF;
          break;
          // Locked
        default:
          printf("R8 I/O (0x%x)\nCal return false!!\n",port);
          /*return false;*/ *dst= 0;
        }
    }
  
  /* Expansion Region 2 i 3 */
  else if ( aux < 0x1FC00000 )
    {
      if ( aux >= _exp2.addr8 && aux < _exp2.ds.end8 )
        {
          printf("R8 Expansion 2\n");
          *dst= 0;
        }
      else if ( aux >= 0x1FA00000 && aux < _exp3.end8 )
        {
          printf("R8 Expansion 3\n");
          *dst= 0;
        }
      else return false;
    }
  
  /* BIOS i resta fins 1FFFFFFF */
  else
    {
      if ( aux < _bios.ds.end8 )
        {
#ifdef PSX_LE
          if ( !is_le ) aux^= 3;
#else
          if ( is_le ) aux^= 3;
#endif
          *dst= _bios.v[aux&BIOS_MASK];
        }
      else return false;
    }
  
  return true;
  
} /* end mem_read8 */


static bool
mem_write (
           const uint32_t addr,
           const uint32_t data
           )
{

  uint32_t aux;
  int port;
  
  
  aux= addr>>2;
  
  /* RAM. */
  if ( aux < _ram.end_ram32 ) ((uint32_t *) _ram.v)[aux&RAM_MASK_32]= data;

  /* La resta de l'àrea de la RAM. */
  else if ( aux <= (0x00800000>>2) )
    {
      if ( aux >= _ram.end_hz32 &&
           (aux != (0x00800000>>2) || _ram.locked_00800000 ) )
        return false;
    }
  
  /* Àrea en blanc. */
  else if ( aux < (0x1F000000>>2) ) return false;
  
  /* Expansion Region 1 */
  else if ( aux < (0x1F800000>>2) )
    {
      if ( aux >= _exp1.addr32 && aux < _exp1.ds.end32 )
        printf("W32 Expansion 1\n");
      else return false;
    }
  
  /* Scratchpad */
  else if ( aux < (0x1F800400>>2) )
    ((uint32_t *) _scratchpad)[aux&SP_MASK_32]= data;
  
  /* Region after scratchpad. */
  else if ( aux < (0x1F801000>>2) ) return false;
  
  /* I/O Ports */
  else if ( aux < (0x1F802000>>2) )
    {
      port= aux&(0xFFF>>2);
      switch ( port )
        {
          // Memory Control 1
        case (0x000>>2): write_exp1_base_addr ( data ); break;
        case (0x004>>2): write_exp2_base_addr ( data ); break;
        case (0x008>>2): write_exp1_delay_size ( data ); break;
        case (0x00C>>2): write_exp3_delay_size ( data ); break;
        case (0x010>>2): write_bios_delay_size ( data ); break;
        case (0x014>>2): _spu= data; break;
        case (0x018>>2): _cdrom= data; break;
        case (0x01C>>2): write_exp2_delay_size ( data ); break;
        case (0x020>>2): _com= data; break;
          // Peripheral I/O Ports
        case (0x040>>2): PSX_joy_tx_data ( data ); break;
        case (0x044>>2): break; // Read-only
        case (0x048>>2):
          // Fa CROP!!! write only lower 16bit (and leave upper 16bit unchanged)
          // Per tant ignora 0x04A
          PSX_joy_mode_write ( (uint16_t) (data&0xFFFF) );
          break;
        case (0x04C>>2):
          // Fa CROP!!! write only lower 16bit (and leave upper 16bit unchanged)
          // Per tant ignora 0x04E
          break;
          // Memory Control 2
        case (0x060>>2): write_ram_size ( data ); break;
          // Interruption Control
        case (0x070>>2): PSX_int_ack ( data ); break;
        case (0x074>>2): PSX_int_write_imask ( data ); break;
          // DMA Registers
        case (0x080>>2): PSX_dma_madr_write ( 0, data ); break;
        case (0x084>>2): PSX_dma_bcr_write ( 0, data ); break;
        case (0x088>>2): PSX_dma_chcr_write ( 0, data ); break;
        case (0x090>>2): PSX_dma_madr_write ( 1, data ); break;
        case (0x094>>2): PSX_dma_bcr_write ( 1, data ); break;
        case (0x098>>2): PSX_dma_chcr_write ( 1, data ); break;
        case (0x0A0>>2): PSX_dma_madr_write ( 2, data ); break;
        case (0x0A4>>2): PSX_dma_bcr_write ( 2, data ); break;
        case (0x0A8>>2): PSX_dma_chcr_write ( 2, data ); break;
        case (0x0B0>>2): PSX_dma_madr_write ( 3, data ); break;
        case (0x0B4>>2): PSX_dma_bcr_write ( 3, data ); break;
        case (0x0B8>>2): PSX_dma_chcr_write ( 3, data ); break;
        case (0x0C0>>2): PSX_dma_madr_write ( 4, data ); break;
        case (0x0C4>>2): PSX_dma_bcr_write ( 4, data ); break;
        case (0x0C8>>2): PSX_dma_chcr_write ( 4, data ); break;
        case (0x0D0>>2): PSX_dma_madr_write ( 5, data ); break;
        case (0x0D4>>2): PSX_dma_bcr_write ( 5, data ); break;
        case (0x0D8>>2): PSX_dma_chcr_write ( 5, data ); break;
        case (0x0E0>>2): PSX_dma_madr_write ( 6, data ); break;
        case (0x0E4>>2): PSX_dma_bcr_write ( 6, data ); break;
        case (0x0E8>>2): PSX_dma_chcr_write ( 6, data ); break;
        case (0x0F0>>2): PSX_dma_dpcr_write ( data ); break;
        case (0x0F4>>2): PSX_dma_dicr_write ( data ); break;
          // Timers
        case (0x100>>2): PSX_timers_set_counter_value ( data, 0 ); break;
        case (0x104>>2): PSX_timers_set_counter_mode ( data, 0 ); break;
        case (0x108>>2): PSX_timers_set_target_value ( data, 0 ); break;
        case (0x110>>2): PSX_timers_set_counter_value ( data, 1 ); break;
        case (0x114>>2): PSX_timers_set_counter_mode ( data, 1 ); break;
        case (0x118>>2): PSX_timers_set_target_value ( data, 1 ); break;
        case (0x120>>2): PSX_timers_set_counter_value ( data, 2 ); break;
        case (0x124>>2): PSX_timers_set_counter_mode ( data, 2 ); break;
        case (0x128>>2): PSX_timers_set_target_value ( data, 2 ); break;
          // CDROM Registers ¿¿????
          // GPU Registers
        case (0x810>>2): PSX_gpu_gp0 ( data ); break;
        case (0x814>>2): PSX_gpu_gp1 ( data ); break;
          // MDEC Registers
        case (0x820>>2): PSX_mdec_data_write ( data ); break;
        case (0x824>>2): PSX_mdec_control ( data ); break;
          // SPU Voice 0..23 Registers
        case (0xC00>>2):
          PSX_spu_voice_set_left_vol ( 0, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 0, (uint16_t) (data>>16) );
          break;
        case (0xC04>>2):
          PSX_spu_voice_set_sample_rate ( 0, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 0, (uint16_t) (data>>16) );
          break;
        case (0xC08>>2):
          PSX_spu_voice_set_adsr_lo ( 0, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 0, (uint16_t) (data>>16) );
          break;
        case (0xC0C>>2):
          PSX_spu_voice_set_cur_vol ( 0, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 0, (uint16_t) (data>>16) );
          break;
        case (0xC10>>2):
          PSX_spu_voice_set_left_vol ( 1, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 1, (uint16_t) (data>>16) );
          break;
        case (0xC14>>2):
          PSX_spu_voice_set_sample_rate ( 1, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 1, (uint16_t) (data>>16) );
          break;
        case (0xC18>>2):
          PSX_spu_voice_set_adsr_lo ( 1, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 1, (uint16_t) (data>>16) );
          break;
        case (0xC1C>>2):
          PSX_spu_voice_set_cur_vol ( 1, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 1, (uint16_t) (data>>16) );
          break;
        case (0xC20>>2):
          PSX_spu_voice_set_left_vol ( 2, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 2, (uint16_t) (data>>16) );
          break;
        case (0xC24>>2):
          PSX_spu_voice_set_sample_rate ( 2, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 2, (uint16_t) (data>>16) );
          break;
        case (0xC28>>2):
          PSX_spu_voice_set_adsr_lo ( 2, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 2, (uint16_t) (data>>16) );
          break;
        case (0xC2C>>2):
          PSX_spu_voice_set_cur_vol ( 2, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 2, (uint16_t) (data>>16) );
          break;
        case (0xC30>>2):
          PSX_spu_voice_set_left_vol ( 3, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 3, (uint16_t) (data>>16) );
          break;
        case (0xC34>>2):
          PSX_spu_voice_set_sample_rate ( 3, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 3, (uint16_t) (data>>16) );
          break;
        case (0xC38>>2):
          PSX_spu_voice_set_adsr_lo ( 3, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 3, (uint16_t) (data>>16) );
          break;
        case (0xC3C>>2):
          PSX_spu_voice_set_cur_vol ( 3, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 3, (uint16_t) (data>>16) );
          break;
        case (0xC40>>2):
          PSX_spu_voice_set_left_vol ( 4, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 4, (uint16_t) (data>>16) );
          break;
        case (0xC44>>2):
          PSX_spu_voice_set_sample_rate ( 4, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 4, (uint16_t) (data>>16) );
          break;
        case (0xC48>>2):
          PSX_spu_voice_set_adsr_lo ( 4, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 4, (uint16_t) (data>>16) );
          break;
        case (0xC4C>>2):
          PSX_spu_voice_set_cur_vol ( 4, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 4, (uint16_t) (data>>16) );
          break;
        case (0xC50>>2):
          PSX_spu_voice_set_left_vol ( 5, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 5, (uint16_t) (data>>16) );
          break;
        case (0xC54>>2):
          PSX_spu_voice_set_sample_rate ( 5, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 5, (uint16_t) (data>>16) );
          break;
        case (0xC58>>2):
          PSX_spu_voice_set_adsr_lo ( 5, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 5, (uint16_t) (data>>16) );
          break;
        case (0xC5C>>2):
          PSX_spu_voice_set_cur_vol ( 5, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 5, (uint16_t) (data>>16) );
          break;
        case (0xC60>>2):
          PSX_spu_voice_set_left_vol ( 6, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 6, (uint16_t) (data>>16) );
          break;
        case (0xC64>>2):
          PSX_spu_voice_set_sample_rate ( 6, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 6, (uint16_t) (data>>16) );
          break;
        case (0xC68>>2):
          PSX_spu_voice_set_adsr_lo ( 6, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 6, (uint16_t) (data>>16) );
          break;
        case (0xC6C>>2):
          PSX_spu_voice_set_cur_vol ( 6, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 6, (uint16_t) (data>>16) );
          break;
        case (0xC70>>2):
          PSX_spu_voice_set_left_vol ( 7, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 7, (uint16_t) (data>>16) );
          break;
        case (0xC74>>2):
          PSX_spu_voice_set_sample_rate ( 7, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 7, (uint16_t) (data>>16) );
          break;
        case (0xC78>>2):
          PSX_spu_voice_set_adsr_lo ( 7, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 7, (uint16_t) (data>>16) );
          break;
        case (0xC7C>>2):
          PSX_spu_voice_set_cur_vol ( 7, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 7, (uint16_t) (data>>16) );
          break;
        case (0xC80>>2):
          PSX_spu_voice_set_left_vol ( 8, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 8, (uint16_t) (data>>16) );
          break;
        case (0xC84>>2):
          PSX_spu_voice_set_sample_rate ( 8, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 8, (uint16_t) (data>>16) );
          break;
        case (0xC88>>2):
          PSX_spu_voice_set_adsr_lo ( 8, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 8, (uint16_t) (data>>16) );
          break;
        case (0xC8C>>2):
          PSX_spu_voice_set_cur_vol ( 8, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 8, (uint16_t) (data>>16) );
          break;
        case (0xC90>>2):
          PSX_spu_voice_set_left_vol ( 9, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 9, (uint16_t) (data>>16) );
          break;
        case (0xC94>>2):
          PSX_spu_voice_set_sample_rate ( 9, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 9, (uint16_t) (data>>16) );
          break;
        case (0xC98>>2):
          PSX_spu_voice_set_adsr_lo ( 9, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 9, (uint16_t) (data>>16) );
          break;
        case (0xC9C>>2):
          PSX_spu_voice_set_cur_vol ( 9, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 9, (uint16_t) (data>>16) );
          break;
        case (0xCA0>>2):
          PSX_spu_voice_set_left_vol ( 10, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 10, (uint16_t) (data>>16) );
          break;
        case (0xCA4>>2):
          PSX_spu_voice_set_sample_rate ( 10, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 10, (uint16_t) (data>>16) );
          break;
        case (0xCA8>>2):
          PSX_spu_voice_set_adsr_lo ( 10, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 10, (uint16_t) (data>>16) );
          break;
        case (0xCAC>>2):
          PSX_spu_voice_set_cur_vol ( 10, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 10, (uint16_t) (data>>16) );
          break;
        case (0xCB0>>2):
          PSX_spu_voice_set_left_vol ( 11, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 11, (uint16_t) (data>>16) );
          break;
        case (0xCB4>>2):
          PSX_spu_voice_set_sample_rate ( 11, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 11, (uint16_t) (data>>16) );
          break;
        case (0xCB8>>2):
          PSX_spu_voice_set_adsr_lo ( 11, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 11, (uint16_t) (data>>16) );
          break;
        case (0xCBC>>2):
          PSX_spu_voice_set_cur_vol ( 11, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 11, (uint16_t) (data>>16) );
          break;
        case (0xCC0>>2):
          PSX_spu_voice_set_left_vol ( 12, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 12, (uint16_t) (data>>16) );
          break;
        case (0xCC4>>2):
          PSX_spu_voice_set_sample_rate ( 12, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 12, (uint16_t) (data>>16) );
          break;
        case (0xCC8>>2):
          PSX_spu_voice_set_adsr_lo ( 12, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 12, (uint16_t) (data>>16) );
          break;
        case (0xCCC>>2):
          PSX_spu_voice_set_cur_vol ( 12, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 12, (uint16_t) (data>>16) );
          break;
        case (0xCD0>>2):
          PSX_spu_voice_set_left_vol ( 13, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 13, (uint16_t) (data>>16) );
          break;
        case (0xCD4>>2):
          PSX_spu_voice_set_sample_rate ( 13, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 13, (uint16_t) (data>>16) );
          break;
        case (0xCD8>>2):
          PSX_spu_voice_set_adsr_lo ( 13, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 13, (uint16_t) (data>>16) );
          break;
        case (0xCDC>>2):
          PSX_spu_voice_set_cur_vol ( 13, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 13, (uint16_t) (data>>16) );
          break;
        case (0xCE0>>2):
          PSX_spu_voice_set_left_vol ( 14, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 14, (uint16_t) (data>>16) );
          break;
        case (0xCE4>>2):
          PSX_spu_voice_set_sample_rate ( 14, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 14, (uint16_t) (data>>16) );
          break;
        case (0xCE8>>2):
          PSX_spu_voice_set_adsr_lo ( 14, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 14, (uint16_t) (data>>16) );
          break;
        case (0xCEC>>2):
          PSX_spu_voice_set_cur_vol ( 14, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 14, (uint16_t) (data>>16) );
          break;
        case (0xCF0>>2):
          PSX_spu_voice_set_left_vol ( 15, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 15, (uint16_t) (data>>16) );
          break;
        case (0xCF4>>2):
          PSX_spu_voice_set_sample_rate ( 15, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 15, (uint16_t) (data>>16) );
          break;
        case (0xCF8>>2):
          PSX_spu_voice_set_adsr_lo ( 15, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 15, (uint16_t) (data>>16) );
          break;
        case (0xCFC>>2):
          PSX_spu_voice_set_cur_vol ( 15, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 15, (uint16_t) (data>>16) );
          break;
        case (0xD00>>2):
          PSX_spu_voice_set_left_vol ( 16, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 16, (uint16_t) (data>>16) );
          break;
        case (0xD04>>2):
          PSX_spu_voice_set_sample_rate ( 16, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 16, (uint16_t) (data>>16) );
          break;
        case (0xD08>>2):
          PSX_spu_voice_set_adsr_lo ( 16, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 16, (uint16_t) (data>>16) );
          break;
        case (0xD0C>>2):
          PSX_spu_voice_set_cur_vol ( 16, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 16, (uint16_t) (data>>16) );
          break;
        case (0xD10>>2):
          PSX_spu_voice_set_left_vol ( 17, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 17, (uint16_t) (data>>16) );
          break;
        case (0xD14>>2):
          PSX_spu_voice_set_sample_rate ( 17, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 17, (uint16_t) (data>>16) );
          break;
        case (0xD18>>2):
          PSX_spu_voice_set_adsr_lo ( 17, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 17, (uint16_t) (data>>16) );
          break;
        case (0xD1C>>2):
          PSX_spu_voice_set_cur_vol ( 17, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 17, (uint16_t) (data>>16) );
          break;
        case (0xD20>>2):
          PSX_spu_voice_set_left_vol ( 18, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 18, (uint16_t) (data>>16) );
          break;
        case (0xD24>>2):
          PSX_spu_voice_set_sample_rate ( 18, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 18, (uint16_t) (data>>16) );
          break;
        case (0xD28>>2):
          PSX_spu_voice_set_adsr_lo ( 18, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 18, (uint16_t) (data>>16) );
          break;
        case (0xD2C>>2):
          PSX_spu_voice_set_cur_vol ( 18, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 18, (uint16_t) (data>>16) );
          break;
        case (0xD30>>2):
          PSX_spu_voice_set_left_vol ( 19, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 19, (uint16_t) (data>>16) );
          break;
        case (0xD34>>2):
          PSX_spu_voice_set_sample_rate ( 19, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 19, (uint16_t) (data>>16) );
          break;
        case (0xD38>>2):
          PSX_spu_voice_set_adsr_lo ( 19, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 19, (uint16_t) (data>>16) );
          break;
        case (0xD3C>>2):
          PSX_spu_voice_set_cur_vol ( 19, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 19, (uint16_t) (data>>16) );
          break;
        case (0xD40>>2):
          PSX_spu_voice_set_left_vol ( 20, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 20, (uint16_t) (data>>16) );
          break;
        case (0xD44>>2):
          PSX_spu_voice_set_sample_rate ( 20, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 20, (uint16_t) (data>>16) );
          break;
        case (0xD48>>2):
          PSX_spu_voice_set_adsr_lo ( 20, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 20, (uint16_t) (data>>16) );
          break;
        case (0xD4C>>2):
          PSX_spu_voice_set_cur_vol ( 20, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 20, (uint16_t) (data>>16) );
          break;
        case (0xD50>>2):
          PSX_spu_voice_set_left_vol ( 21, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 21, (uint16_t) (data>>16) );
          break;
        case (0xD54>>2):
          PSX_spu_voice_set_sample_rate ( 21, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 21, (uint16_t) (data>>16) );
          break;
        case (0xD58>>2):
          PSX_spu_voice_set_adsr_lo ( 21, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 21, (uint16_t) (data>>16) );
          break;
        case (0xD5C>>2):
          PSX_spu_voice_set_cur_vol ( 21, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 21, (uint16_t) (data>>16) );
          break;
        case (0xD60>>2):
          PSX_spu_voice_set_left_vol ( 22, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 22, (uint16_t) (data>>16) );
          break;
        case (0xD64>>2):
          PSX_spu_voice_set_sample_rate ( 22, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 22, (uint16_t) (data>>16) );
          break;
        case (0xD68>>2):
          PSX_spu_voice_set_adsr_lo ( 22, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 22, (uint16_t) (data>>16) );
          break;
        case (0xD6C>>2):
          PSX_spu_voice_set_cur_vol ( 22, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 22, (uint16_t) (data>>16) );
          break;
        case (0xD70>>2):
          PSX_spu_voice_set_left_vol ( 23, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_right_vol ( 23, (uint16_t) (data>>16) );
          break;
        case (0xD74>>2):
          PSX_spu_voice_set_sample_rate ( 23, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_start_addr ( 23, (uint16_t) (data>>16) );
          break;
        case (0xD78>>2):
          PSX_spu_voice_set_adsr_lo ( 23, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_adsr_up ( 23, (uint16_t) (data>>16) );
          break;
        case (0xD7C>>2):
          PSX_spu_voice_set_cur_vol ( 23, (uint16_t) data&0xFFFF );
          PSX_spu_voice_set_repeat_addr ( 23, (uint16_t) (data>>16) );
          break;
          // SPU Control Registers
        case (0xD80>>2):
          PSX_spu_set_left_vol ( (uint16_t) data&0xFFFF );
          PSX_spu_set_right_vol ( (uint16_t) (data>>16) );
          break;
        case (0xD84>>2):
          PSX_spu_reverb_set_vlout ( (uint16_t) data&0xFFFF );
          PSX_spu_reverb_set_vrout ( (uint16_t) (data>>16) );
          break;
        case (0xD88>>2):
          PSX_spu_key_on_lo ( (uint16_t) data&0xFFFF );
          PSX_spu_key_on_up ( (uint16_t) (data>>16) );
          break;
        case (0xD8C>>2):
          PSX_spu_key_off_lo ( (uint16_t) data&0xFFFF );
          PSX_spu_key_off_up ( (uint16_t) (data>>16) );
          break;
        case (0xD90>>2):
          PSX_spu_set_pmon_lo ( (uint16_t) data&0xFFFF );
          PSX_spu_set_pmon_up ( (uint16_t) (data>>16) );
          break;
        case (0xD94>>2):
          PSX_spu_set_non_lo ( (uint16_t) data&0xFFFF );
          PSX_spu_set_non_up ( (uint16_t) (data>>16) );
          break;
        case (0xD98>>2):
          PSX_spu_set_eon_lo ( (uint16_t) data&0xFFFF );
          PSX_spu_set_eon_up ( (uint16_t) (data>>16) );
          break;
        case (0xD9C>>2):
          PSX_spu_set_endx_lo ( (uint16_t) data&0xFFFF );
          PSX_spu_set_endx_up ( (uint16_t) (data>>16) );
          break;
        case (0xDA0>>2):
          PSX_spu_set_unk_da0 ( (uint16_t) data );
          PSX_spu_reverb_set_mbase ( (uint16_t) (data>>16) );
          break;
        case (0xDA4>>2):
          PSX_spu_set_irq_addr ( (uint16_t) data&0xFFFF );
          PSX_spu_set_addr ( (uint16_t) (data>>16) );
        case (0xDA8>>2):
          PSX_spu_write ( (uint16_t) (data&0xFFFF) );
          PSX_spu_set_control ( (uint16_t) (data>>16) );
          break;
        case (0xDAC>>2):
          PSX_spu_set_transfer_type ( (uint16_t) (data&0xFFFF) );
          // SPU Status Register (SPUSTAT) (R)
          break;
        case (0xDB0>>2):
          PSX_spu_set_cd_vol_l ( (uint16_t) (data&0xFFFF) );
          PSX_spu_set_cd_vol_r ( (uint16_t) (data>>16) );
          break;
        case (0xDB4>>2):
          PSX_spu_set_ext_vol_l ( (uint16_t) (data&0xFFFF) );
          PSX_spu_set_ext_vol_r ( (uint16_t) (data>>16) );
          break;
        case (0xDB8>>2):
          printf ( "W32 port: 0xDB8: cal implementar??\n" );
          break;
        case (0xDBC>>2):
          PSX_spu_set_unk_dbc ( 0, (uint16_t) (data&0xFFFF) );
          PSX_spu_set_unk_dbc ( 1, (uint16_t) (data>>16) );
          break;
          // SPU Reverb Configuration Area
        case (0xDC0>>2) ... (0xDFC>>2):
          PSX_spu_reverb_set_reg ( (aux<<1)&0x1F, (uint16_t) (data&0xFFFF) );
          PSX_spu_reverb_set_reg ( ((aux<<1)&0x1F)|1, (uint16_t) (data>>16) );
          break;
          // SPU Internal Registers
        case (0xE00>>2) ... (0xE5C>>2):
          break; // Voice 0..23 Current Volume Left/Right
        case (0xE60>>2) ... (0xE7C>>2):
          PSX_spu_set_unk_e60 ( (aux<<1)&0xF, (uint16_t) (data&0xFFFF) );
          PSX_spu_set_unk_e60 ( ((aux<<1)&0xF)|1, (uint16_t) (data>>16) );
          break;
        case (0xE80>>2) ... (0xFFC>>2):
          break; // Unknown? (Unused or Write only?)
          // Locked
        default:
          printf("W32 I/O (0x%x)\nCal return false!!\n",port<<2);
          /*return false;*/
        }
    }
  
  /* Expansion Region 2 i 3 */
  else if ( aux < (0x1FC00000>>2) )
    {
      if ( aux >= _exp2.addr32 && aux < _exp2.ds.end32 )
        printf("W32 Expansion 2\n");
      else if ( aux >= (0x1FA00000>>2) && aux < _exp3.end32 )
        printf("W32 Expansion 3\n");
      else return false;
    }
  
  /* BIOS i resta fins 1FFFFFFF */
  else
    {
      if ( aux < _bios.ds.end32 ) return true;
      else return false;
    }
  
  return true;
  
} /* end mem_write */


static bool
mem_write16 (
             const uint32_t addr,
             const uint16_t data,
             const bool     is_le
             )
{

  uint32_t aux;
  int port;
  
  
  aux= addr>>1;
  
  /* RAM. */
  if ( aux < _ram.end_ram16 )
    {
#ifdef PSX_LE
      if ( !is_le ) aux^= 1;
#else
      if ( is_le ) aux^= 1;
#endif
      ((uint16_t *) _ram.v)[aux&RAM_MASK_16]= data;
    }

  /* La resta de l'àrea de la RAM. */
  else if ( aux <= (0x00800000>>1) )
    {
      if ( aux >= _ram.end_hz16 &&
           (aux != (0x00800000>>1) || _ram.locked_00800000 ) )
        return false;
    }

  /* Àrea en blanc. */
  else if ( aux < (0x1F000000>>1) ) return false;
  
  /* Expansion Region 1 */
  else if ( aux < (0x1F800000>>1) )
    {
      if ( aux >= _exp1.addr16 && aux < _exp1.ds.end16 )
        printf("W16 Expansion 1\n");
      else return false;
    }
  
  /* Scratchpad */
  else if ( aux < (0x1F800400>>1) )
    {
#ifdef PSX_LE
      if ( !is_le ) aux^= 1;
#else
      if ( is_le ) aux^= 1;
#endif  
      ((uint16_t *) _scratchpad)[aux&SP_MASK_16]= data;
    }
  
  /* Region after scratchpad */
  else if ( aux < (0x1F801000>>1) ) return false;
  
  /* I/O Ports */
  else if ( aux < (0x1F802000>>1) )
    {
      port= aux&(0xFFF>>1);
      switch ( port )
        {
          // Memory Control 1
        case (0x000>>1):
        case (0x002>>1): write_exp1_base_addr ( WW32(data,port) ); break;
        case (0x004>>1):
        case (0x006>>1): write_exp2_base_addr ( WW32(data,port) ); break;
        case (0x008>>1):
        case (0x00A>>1): write_exp1_delay_size ( WW32(data,port) ); break;
        case (0x00C>>1):
        case (0x00E>>1): write_exp3_delay_size ( WW32(data,port) ); break;
        case (0x010>>1):
        case (0x012>>1): write_bios_delay_size ( WW32(data,port) ); break;
        case (0x014>>1):
        case (0x016>>1): _spu= WW32(data,port); break;
        case (0x018>>1):
        case (0x01A>>1): _cdrom= WW32(data,port); break;
        case (0x01C>>1):
        case (0x01E>>1): write_exp2_delay_size ( WW32(data,port) ); break;
        case (0x020>>1):
        case (0x022>>1): _com= WW32(data,port); break;
          // Peripheral I/O Ports
        case (0x040>>1): PSX_joy_tx_data ( (uint32_t) data ); break;
        case (0x042>>1): break; // Els ignora???
        case (0x044>>1): break; // Read-only
        case (0x046>>1): break; // Read-only
        case (0x048>>1): PSX_joy_mode_write ( data ); break;
        case (0x04A>>1): PSX_joy_ctrl_write ( data ); break;
        case (0x04C>>1): break;
        case (0x04E>>1): PSX_joy_baud_write ( data ); break;
          // Memory Control 2
        case (0x060>>1):
        case (0x062>>1): write_ram_size ( WW32(data,port) ); break;
          // Interruption Control
        case (0x070>>1):
        case (0x072>>1): PSX_int_ack ( WW32(data,port) ); break;
        case (0x074>>1):
        case (0x076>>1): PSX_int_write_imask ( WW32(data,port) ); break;
          // DMA Registers
        case (0x080>>1):
        case (0x082>>1): PSX_dma_madr_write ( 0, WW32(data,port) ); break;
        case (0x084>>1):
        case (0x086>>1): dma_bcr_write16 ( 0, port, data ); break;
        case (0x088>>1):
        case (0x08A>>1): PSX_dma_chcr_write ( 0, WW32(data,port) ); break;
        case (0x090>>1):
        case (0x092>>1): PSX_dma_madr_write ( 1, WW32(data,port) ); break;
        case (0x094>>1):
        case (0x096>>1): dma_bcr_write16 ( 1, port, data ); break;
        case (0x098>>1):
        case (0x09A>>1): PSX_dma_chcr_write ( 1, WW32(data,port) ); break;
        case (0x0A0>>1):
        case (0x0A2>>1): PSX_dma_madr_write ( 2, WW32(data,port) ); break;
        case (0x0A4>>1):
        case (0x0A6>>1): dma_bcr_write16 ( 2, port, data ); break;
        case (0x0A8>>1):
        case (0x0AA>>1): PSX_dma_chcr_write ( 2, WW32(data,port) ); break;
        case (0x0B0>>1):
        case (0x0B2>>1): PSX_dma_madr_write ( 3, WW32(data,port) ); break;
        case (0x0B4>>1):
        case (0x0B6>>1): dma_bcr_write16 ( 3, port, data ); break;
        case (0x0B8>>1):
        case (0x0BA>>1): PSX_dma_chcr_write ( 3, WW32(data,port) ); break;
        case (0x0C0>>1):
        case (0x0C2>>1): PSX_dma_madr_write ( 4, WW32(data,port) ); break;
        case (0x0C4>>1):
        case (0x0C6>>1): dma_bcr_write16 ( 4, port, data ); break;
        case (0x0C8>>1):
        case (0x0CA>>1): PSX_dma_chcr_write ( 4, WW32(data,port) ); break;
        case (0x0D0>>1):
        case (0x0D2>>1): PSX_dma_madr_write ( 5, WW32(data,port) ); break;
        case (0x0D4>>1):
        case (0x0D6>>1): dma_bcr_write16 ( 5, port, data ); break;
        case (0x0D8>>1):
        case (0x0DA>>1): PSX_dma_chcr_write ( 5, WW32(data,port) ); break;
        case (0x0E0>>1):
        case (0x0E2>>1): PSX_dma_madr_write ( 6, WW32(data,port) ); break;
        case (0x0E4>>1):
        case (0x0E6>>1): dma_bcr_write16 ( 6, port, data ); break;
        case (0x0E8>>1):
        case (0x0EA>>1): PSX_dma_chcr_write ( 6, WW32(data,port) ); break;
        case (0x0F0>>1):
        case (0x0F2>>1): PSX_dma_dpcr_write ( WW32(data,port) ); break;
        case (0x0F4>>1):
        case (0x0F6>>1): PSX_dma_dicr_write ( WW32(data,port) ); break;
          // Timers
        case (0x100>>1):
        case (0x102>>1):
          PSX_timers_set_counter_value ( WW32(data,port), 0 );
          break;
        case (0x104>>1):
        case (0x106>>1):
          PSX_timers_set_counter_mode ( WW32(data,port), 0 );
          break;
        case (0x108>>1):
        case (0x10A>>1):
          PSX_timers_set_target_value ( WW32(data,port), 0 );
          break;
        case (0x110>>1):
        case (0x112>>1):
          PSX_timers_set_counter_value ( WW32(data,port), 1 );
          break;
        case (0x114>>1):
        case (0x116>>1):
          PSX_timers_set_counter_mode ( WW32(data,port), 1 );
          break;
        case (0x118>>1):
        case (0x11A>>1):
          PSX_timers_set_target_value ( WW32(data,port), 1 );
          break;
        case (0x120>>1):
        case (0x122>>1):
          PSX_timers_set_counter_value ( WW32(data,port), 2 );
          break;
        case (0x124>>1):
        case (0x126>>1):
          PSX_timers_set_counter_mode ( WW32(data,port), 2 );
          break;
        case (0x128>>1):
        case (0x12A>>1):
          PSX_timers_set_target_value ( WW32(data,port), 2 );
          break;
          // CDROM Registers ¿¿????
          // GPU Registers ¿¿?? 
          // MDEC Registers ¿¿??
          // SPU Voice 0..23 Registers
        case (0xC00>>1): PSX_spu_voice_set_left_vol ( 0, data ); break;
        case (0xC02>>1): PSX_spu_voice_set_right_vol ( 0, data ); break;
        case (0xC04>>1): PSX_spu_voice_set_sample_rate ( 0, data ); break;
        case (0xC06>>1): PSX_spu_voice_set_start_addr ( 0, data ); break;
        case (0xC08>>1): PSX_spu_voice_set_adsr_lo ( 0, data ); break;
        case (0xC0A>>1): PSX_spu_voice_set_adsr_up ( 0, data ); break;
        case (0xC0C>>1): PSX_spu_voice_set_cur_vol ( 0, data ); break;
        case (0xC0E>>1): PSX_spu_voice_set_repeat_addr ( 0, data ); break;
        case (0xC10>>1): PSX_spu_voice_set_left_vol ( 1, data ); break;
        case (0xC12>>1): PSX_spu_voice_set_right_vol ( 1, data ); break;
        case (0xC14>>1): PSX_spu_voice_set_sample_rate ( 1, data ); break;
        case (0xC16>>1): PSX_spu_voice_set_start_addr ( 1, data ); break;
        case (0xC18>>1): PSX_spu_voice_set_adsr_lo ( 1, data ); break;
        case (0xC1A>>1): PSX_spu_voice_set_adsr_up ( 1, data ); break;
        case (0xC1C>>1): PSX_spu_voice_set_cur_vol ( 1, data ); break;
        case (0xC1E>>1): PSX_spu_voice_set_repeat_addr ( 1, data ); break;
        case (0xC20>>1): PSX_spu_voice_set_left_vol ( 2, data ); break;
        case (0xC22>>1): PSX_spu_voice_set_right_vol ( 2, data ); break;
        case (0xC24>>1): PSX_spu_voice_set_sample_rate ( 2, data ); break;
        case (0xC26>>1): PSX_spu_voice_set_start_addr ( 2, data ); break;
        case (0xC28>>1): PSX_spu_voice_set_adsr_lo ( 2, data ); break;
        case (0xC2A>>1): PSX_spu_voice_set_adsr_up ( 2, data ); break;
        case (0xC2C>>1): PSX_spu_voice_set_cur_vol ( 2, data ); break;
        case (0xC2E>>1): PSX_spu_voice_set_repeat_addr ( 2, data ); break;
        case (0xC30>>1): PSX_spu_voice_set_left_vol ( 3, data ); break;
        case (0xC32>>1): PSX_spu_voice_set_right_vol ( 3, data ); break;
        case (0xC34>>1): PSX_spu_voice_set_sample_rate ( 3, data ); break;
        case (0xC36>>1): PSX_spu_voice_set_start_addr ( 3, data ); break;
        case (0xC38>>1): PSX_spu_voice_set_adsr_lo ( 3, data ); break;
        case (0xC3A>>1): PSX_spu_voice_set_adsr_up ( 3, data ); break;
        case (0xC3C>>1): PSX_spu_voice_set_cur_vol ( 3, data ); break;
        case (0xC3E>>1): PSX_spu_voice_set_repeat_addr ( 3, data ); break;
        case (0xC40>>1): PSX_spu_voice_set_left_vol ( 4, data ); break;
        case (0xC42>>1): PSX_spu_voice_set_right_vol ( 4, data ); break;
        case (0xC44>>1): PSX_spu_voice_set_sample_rate ( 4, data ); break;
        case (0xC46>>1): PSX_spu_voice_set_start_addr ( 4, data ); break;
        case (0xC48>>1): PSX_spu_voice_set_adsr_lo ( 4, data ); break;
        case (0xC4A>>1): PSX_spu_voice_set_adsr_up ( 4, data ); break;
        case (0xC4C>>1): PSX_spu_voice_set_cur_vol ( 4, data ); break;
        case (0xC4E>>1): PSX_spu_voice_set_repeat_addr ( 4, data ); break;
        case (0xC50>>1): PSX_spu_voice_set_left_vol ( 5, data ); break;
        case (0xC52>>1): PSX_spu_voice_set_right_vol ( 5, data ); break;
        case (0xC54>>1): PSX_spu_voice_set_sample_rate ( 5, data ); break;
        case (0xC56>>1): PSX_spu_voice_set_start_addr ( 5, data ); break;
        case (0xC58>>1): PSX_spu_voice_set_adsr_lo ( 5, data ); break;
        case (0xC5A>>1): PSX_spu_voice_set_adsr_up ( 5, data ); break;
        case (0xC5C>>1): PSX_spu_voice_set_cur_vol ( 5, data ); break;
        case (0xC5E>>1): PSX_spu_voice_set_repeat_addr ( 5, data ); break;
        case (0xC60>>1): PSX_spu_voice_set_left_vol ( 6, data ); break;
        case (0xC62>>1): PSX_spu_voice_set_right_vol ( 6, data ); break;
        case (0xC64>>1): PSX_spu_voice_set_sample_rate ( 6, data ); break;
        case (0xC66>>1): PSX_spu_voice_set_start_addr ( 6, data ); break;
        case (0xC68>>1): PSX_spu_voice_set_adsr_lo ( 6, data ); break;
        case (0xC6A>>1): PSX_spu_voice_set_adsr_up ( 6, data ); break;
        case (0xC6C>>1): PSX_spu_voice_set_cur_vol ( 6, data ); break;
        case (0xC6E>>1): PSX_spu_voice_set_repeat_addr ( 6, data ); break;
        case (0xC70>>1): PSX_spu_voice_set_left_vol ( 7, data ); break;
        case (0xC72>>1): PSX_spu_voice_set_right_vol ( 7, data ); break;
        case (0xC74>>1): PSX_spu_voice_set_sample_rate ( 7, data ); break;
        case (0xC76>>1): PSX_spu_voice_set_start_addr ( 7, data ); break;
        case (0xC78>>1): PSX_spu_voice_set_adsr_lo ( 7, data ); break;
        case (0xC7A>>1): PSX_spu_voice_set_adsr_up ( 7, data ); break;
        case (0xC7C>>1): PSX_spu_voice_set_cur_vol ( 7, data ); break;
        case (0xC7E>>1): PSX_spu_voice_set_repeat_addr ( 7, data ); break;
        case (0xC80>>1): PSX_spu_voice_set_left_vol ( 8, data ); break;
        case (0xC82>>1): PSX_spu_voice_set_right_vol ( 8, data ); break;
        case (0xC84>>1): PSX_spu_voice_set_sample_rate ( 8, data ); break;
        case (0xC86>>1): PSX_spu_voice_set_start_addr ( 8, data ); break;
        case (0xC88>>1): PSX_spu_voice_set_adsr_lo ( 8, data ); break;
        case (0xC8A>>1): PSX_spu_voice_set_adsr_up ( 8, data ); break;
        case (0xC8C>>1): PSX_spu_voice_set_cur_vol ( 8, data ); break;
        case (0xC8E>>1): PSX_spu_voice_set_repeat_addr ( 8, data ); break;
        case (0xC90>>1): PSX_spu_voice_set_left_vol ( 9, data ); break;
        case (0xC92>>1): PSX_spu_voice_set_right_vol ( 9, data ); break;
        case (0xC94>>1): PSX_spu_voice_set_sample_rate ( 9, data ); break;
        case (0xC96>>1): PSX_spu_voice_set_start_addr ( 9, data ); break;
        case (0xC98>>1): PSX_spu_voice_set_adsr_lo ( 9, data ); break;
        case (0xC9A>>1): PSX_spu_voice_set_adsr_up ( 9, data ); break;
        case (0xC9C>>1): PSX_spu_voice_set_cur_vol ( 9, data ); break;
        case (0xC9E>>1): PSX_spu_voice_set_repeat_addr ( 9, data ); break;
        case (0xCA0>>1): PSX_spu_voice_set_left_vol ( 10, data ); break;
        case (0xCA2>>1): PSX_spu_voice_set_right_vol ( 10, data ); break;
        case (0xCA4>>1): PSX_spu_voice_set_sample_rate ( 10, data ); break;
        case (0xCA6>>1): PSX_spu_voice_set_start_addr ( 10, data ); break;
        case (0xCA8>>1): PSX_spu_voice_set_adsr_lo ( 10, data ); break;
        case (0xCAA>>1): PSX_spu_voice_set_adsr_up ( 10, data ); break;
        case (0xCAC>>1): PSX_spu_voice_set_cur_vol ( 10, data ); break;
        case (0xCAE>>1): PSX_spu_voice_set_repeat_addr ( 10, data ); break;
        case (0xCB0>>1): PSX_spu_voice_set_left_vol ( 11, data ); break;
        case (0xCB2>>1): PSX_spu_voice_set_right_vol ( 11, data ); break;
        case (0xCB4>>1): PSX_spu_voice_set_sample_rate ( 11, data ); break;
        case (0xCB6>>1): PSX_spu_voice_set_start_addr ( 11, data ); break;
        case (0xCB8>>1): PSX_spu_voice_set_adsr_lo ( 11, data ); break;
        case (0xCBA>>1): PSX_spu_voice_set_adsr_up ( 11, data ); break;
        case (0xCBC>>1): PSX_spu_voice_set_cur_vol ( 11, data ); break;
        case (0xCBE>>1): PSX_spu_voice_set_repeat_addr ( 11, data ); break;
        case (0xCC0>>1): PSX_spu_voice_set_left_vol ( 12, data ); break;
        case (0xCC2>>1): PSX_spu_voice_set_right_vol ( 12, data ); break;
        case (0xCC4>>1): PSX_spu_voice_set_sample_rate ( 12, data ); break;
        case (0xCC6>>1): PSX_spu_voice_set_start_addr ( 12, data ); break;
        case (0xCC8>>1): PSX_spu_voice_set_adsr_lo ( 12, data ); break;
        case (0xCCA>>1): PSX_spu_voice_set_adsr_up ( 12, data ); break;
        case (0xCCC>>1): PSX_spu_voice_set_cur_vol ( 12, data ); break;
        case (0xCCE>>1): PSX_spu_voice_set_repeat_addr ( 12, data ); break;
        case (0xCD0>>1): PSX_spu_voice_set_left_vol ( 13, data ); break;
        case (0xCD2>>1): PSX_spu_voice_set_right_vol ( 13, data ); break;
        case (0xCD4>>1): PSX_spu_voice_set_sample_rate ( 13, data ); break;
        case (0xCD6>>1): PSX_spu_voice_set_start_addr ( 13, data ); break;
        case (0xCD8>>1): PSX_spu_voice_set_adsr_lo ( 13, data ); break;
        case (0xCDA>>1): PSX_spu_voice_set_adsr_up ( 13, data ); break;
        case (0xCDC>>1): PSX_spu_voice_set_cur_vol ( 13, data ); break;
        case (0xCDE>>1): PSX_spu_voice_set_repeat_addr ( 13, data ); break;
        case (0xCE0>>1): PSX_spu_voice_set_left_vol ( 14, data ); break;
        case (0xCE2>>1): PSX_spu_voice_set_right_vol ( 14, data ); break;
        case (0xCE4>>1): PSX_spu_voice_set_sample_rate ( 14, data ); break;
        case (0xCE6>>1): PSX_spu_voice_set_start_addr ( 14, data ); break;
        case (0xCE8>>1): PSX_spu_voice_set_adsr_lo ( 14, data ); break;
        case (0xCEA>>1): PSX_spu_voice_set_adsr_up ( 14, data ); break;
        case (0xCEC>>1): PSX_spu_voice_set_cur_vol ( 14, data ); break;
        case (0xCEE>>1): PSX_spu_voice_set_repeat_addr ( 14, data ); break;
        case (0xCF0>>1): PSX_spu_voice_set_left_vol ( 15, data ); break;
        case (0xCF2>>1): PSX_spu_voice_set_right_vol ( 15, data ); break;
        case (0xCF4>>1): PSX_spu_voice_set_sample_rate ( 15, data ); break;
        case (0xCF6>>1): PSX_spu_voice_set_start_addr ( 15, data ); break;
        case (0xCF8>>1): PSX_spu_voice_set_adsr_lo ( 15, data ); break;
        case (0xCFA>>1): PSX_spu_voice_set_adsr_up ( 15, data ); break;
        case (0xCFC>>1): PSX_spu_voice_set_cur_vol ( 15, data ); break;
        case (0xCFE>>1): PSX_spu_voice_set_repeat_addr ( 15, data ); break;
        case (0xD00>>1): PSX_spu_voice_set_left_vol ( 16, data ); break;
        case (0xD02>>1): PSX_spu_voice_set_right_vol ( 16, data ); break;
        case (0xD04>>1): PSX_spu_voice_set_sample_rate ( 16, data ); break;
        case (0xD06>>1): PSX_spu_voice_set_start_addr ( 16, data ); break;
        case (0xD08>>1): PSX_spu_voice_set_adsr_lo ( 16, data ); break;
        case (0xD0A>>1): PSX_spu_voice_set_adsr_up ( 16, data ); break;
        case (0xD0C>>1): PSX_spu_voice_set_cur_vol ( 16, data ); break;
        case (0xD0E>>1): PSX_spu_voice_set_repeat_addr ( 16, data ); break;
        case (0xD10>>1): PSX_spu_voice_set_left_vol ( 17, data ); break;
        case (0xD12>>1): PSX_spu_voice_set_right_vol ( 17, data ); break;
        case (0xD14>>1): PSX_spu_voice_set_sample_rate ( 17, data ); break;
        case (0xD16>>1): PSX_spu_voice_set_start_addr ( 17, data ); break;
        case (0xD18>>1): PSX_spu_voice_set_adsr_lo ( 17, data ); break;
        case (0xD1A>>1): PSX_spu_voice_set_adsr_up ( 17, data ); break;
        case (0xD1C>>1): PSX_spu_voice_set_cur_vol ( 17, data ); break;
        case (0xD1E>>1): PSX_spu_voice_set_repeat_addr ( 17, data ); break;
        case (0xD20>>1): PSX_spu_voice_set_left_vol ( 18, data ); break;
        case (0xD22>>1): PSX_spu_voice_set_right_vol ( 18, data ); break;
        case (0xD24>>1): PSX_spu_voice_set_sample_rate ( 18, data ); break;
        case (0xD26>>1): PSX_spu_voice_set_start_addr ( 18, data ); break;
        case (0xD28>>1): PSX_spu_voice_set_adsr_lo ( 18, data ); break;
        case (0xD2A>>1): PSX_spu_voice_set_adsr_up ( 18, data ); break;
        case (0xD2C>>1): PSX_spu_voice_set_cur_vol ( 18, data ); break;
        case (0xD2E>>1): PSX_spu_voice_set_repeat_addr ( 18, data ); break;
        case (0xD30>>1): PSX_spu_voice_set_left_vol ( 19, data ); break;
        case (0xD32>>1): PSX_spu_voice_set_right_vol ( 19, data ); break;
        case (0xD34>>1): PSX_spu_voice_set_sample_rate ( 19, data ); break;
        case (0xD36>>1): PSX_spu_voice_set_start_addr ( 19, data ); break;
        case (0xD38>>1): PSX_spu_voice_set_adsr_lo ( 19, data ); break;
        case (0xD3A>>1): PSX_spu_voice_set_adsr_up ( 19, data ); break;
        case (0xD3C>>1): PSX_spu_voice_set_cur_vol ( 19, data ); break;
        case (0xD3E>>1): PSX_spu_voice_set_repeat_addr ( 19, data ); break;
        case (0xD40>>1): PSX_spu_voice_set_left_vol ( 20, data ); break;
        case (0xD42>>1): PSX_spu_voice_set_right_vol ( 20, data ); break;
        case (0xD44>>1): PSX_spu_voice_set_sample_rate ( 20, data ); break;
        case (0xD46>>1): PSX_spu_voice_set_start_addr ( 20, data ); break;
        case (0xD48>>1): PSX_spu_voice_set_adsr_lo ( 20, data ); break;
        case (0xD4A>>1): PSX_spu_voice_set_adsr_up ( 20, data ); break;
        case (0xD4C>>1): PSX_spu_voice_set_cur_vol ( 20, data ); break;
        case (0xD4E>>1): PSX_spu_voice_set_repeat_addr ( 20, data ); break;
        case (0xD50>>1): PSX_spu_voice_set_left_vol ( 21, data ); break;
        case (0xD52>>1): PSX_spu_voice_set_right_vol ( 21, data ); break;
        case (0xD54>>1): PSX_spu_voice_set_sample_rate ( 21, data ); break;
        case (0xD56>>1): PSX_spu_voice_set_start_addr ( 21, data ); break;
        case (0xD58>>1): PSX_spu_voice_set_adsr_lo ( 21, data ); break;
        case (0xD5A>>1): PSX_spu_voice_set_adsr_up ( 21, data ); break;
        case (0xD5C>>1): PSX_spu_voice_set_cur_vol ( 21, data ); break;
        case (0xD5E>>1): PSX_spu_voice_set_repeat_addr ( 21, data ); break;
        case (0xD60>>1): PSX_spu_voice_set_left_vol ( 22, data ); break;
        case (0xD62>>1): PSX_spu_voice_set_right_vol ( 22, data ); break;
        case (0xD64>>1): PSX_spu_voice_set_sample_rate ( 22, data ); break;
        case (0xD66>>1): PSX_spu_voice_set_start_addr ( 22, data ); break;
        case (0xD68>>1): PSX_spu_voice_set_adsr_lo ( 22, data ); break;
        case (0xD6A>>1): PSX_spu_voice_set_adsr_up ( 22, data ); break;
        case (0xD6C>>1): PSX_spu_voice_set_cur_vol ( 22, data ); break;
        case (0xD6E>>1): PSX_spu_voice_set_repeat_addr ( 22, data ); break;
        case (0xD70>>1): PSX_spu_voice_set_left_vol ( 23, data ); break;
        case (0xD72>>1): PSX_spu_voice_set_right_vol ( 23, data ); break;
        case (0xD74>>1): PSX_spu_voice_set_sample_rate ( 23, data ); break;
        case (0xD76>>1): PSX_spu_voice_set_start_addr ( 23, data ); break;
        case (0xD78>>1): PSX_spu_voice_set_adsr_lo ( 23, data ); break;
        case (0xD7A>>1): PSX_spu_voice_set_adsr_up ( 23, data ); break;
        case (0xD7C>>1): PSX_spu_voice_set_cur_vol ( 23, data ); break;
        case (0xD7E>>1): PSX_spu_voice_set_repeat_addr ( 23, data ); break;
          // SPU Control Registers
        case (0xD80>>1): PSX_spu_set_left_vol ( data ); break;
        case (0xD82>>1): PSX_spu_set_right_vol ( data ); break;
        case (0xD84>>1): PSX_spu_reverb_set_vlout ( data ); break;
        case (0xD86>>1): PSX_spu_reverb_set_vrout ( data ); break;
        case (0xD88>>1): PSX_spu_key_on_lo ( data ); break;
        case (0xD8A>>1): PSX_spu_key_on_up ( data ); break;
        case (0xD8C>>1): PSX_spu_key_off_lo ( data ); break;
        case (0xD8E>>1): PSX_spu_key_off_up ( data ); break;
        case (0xD90>>1): PSX_spu_set_pmon_lo ( data ); break;
        case (0xD92>>1): PSX_spu_set_pmon_up ( data ); break;
        case (0xD94>>1): PSX_spu_set_non_lo ( data ); break;
        case (0xD96>>1): PSX_spu_set_non_up ( data ); break;
        case (0xD98>>1): PSX_spu_set_eon_lo ( data ); break;
        case (0xD9A>>1): PSX_spu_set_eon_up ( data ); break;
        case (0xD9C>>1): PSX_spu_set_endx_lo ( data ); break;
        case (0xD9E>>1): PSX_spu_set_endx_up ( data ); break;
        case (0xDA0>>1): PSX_spu_set_unk_da0 ( data ); break;
        case (0xDA2>>1): PSX_spu_reverb_set_mbase ( data ); break;
        case (0xDA4>>1): PSX_spu_set_irq_addr ( data ); break;
        case (0xDA6>>1): PSX_spu_set_addr ( data ); break;
        case (0xDA8>>1): PSX_spu_write ( data ); break;
        case (0xDAA>>1): PSX_spu_set_control ( data ); break;
        case (0xDAC>>1): PSX_spu_set_transfer_type ( data ); break;
        case (0xDAE>>1):
          printf ( "W16 port: 0xDAE: cal implementar??\n" );
          break;
        case (0xDB0>>1): PSX_spu_set_cd_vol_l ( data ); break;
        case (0xDB2>>1): PSX_spu_set_cd_vol_r ( data ); break;
        case (0xDB4>>1): PSX_spu_set_ext_vol_l ( data ); break;
        case (0xDB6>>1): PSX_spu_set_ext_vol_r ( data ); break;
        case (0xDB8>>1):
        case (0xDBA>>1):
          printf ( "W16 port: 0xDB8-0xDBA: cal implementar??\n" );
          break;
        case (0xDBC>>1): PSX_spu_set_unk_dbc ( 0, data ); break;
        case (0xDBE>>1): PSX_spu_set_unk_dbc ( 1, data ); break;
          // SPU Reverb Configuration Area
        case (0xDC0>>1) ... (0xDFE>>1):
          PSX_spu_reverb_set_reg ( aux&0x1F, data );
          break;
          // SPU Internal Registers
        case (0xE00>>1) ... (0xE5E>>1):
          break; // Voice 0..23 Current Volume Left/Right
        case (0xE60>>1) ... (0xE7E>>1):
          PSX_spu_set_unk_e60 ( aux&0xF, data );
          break;
        case (0xE80>>1) ... (0xFFE>>1):
          break; // Unknown? (Read: FFh-filled) (Unused or Write only?)
          // Locked
        default:
          printf("W16 I/O (0x%x)\nCal return false!!\n",port<<1);
          /*return false;*/
        }
    }
  
  /* Expansion Region 2 i 3 */
  else if ( aux < (0x1FC00000>>1) )
    {
      if ( aux >= _exp2.addr16 && aux < _exp2.ds.end16 )
        printf("W16 Expansion 2\n");
      else if ( aux >= (0x1FA00000>>1) && aux < _exp3.end16 )
        printf("W16 Expansion 3\n");
      else return false;
    }
  
  /* BIOS i resta fins 1FFFFFFF */
  else
    {
      if ( aux < _bios.ds.end16 ) return true;
      else return false;
    }
  
  return true;
  
} /* end mem_write16 */


// ATENCIÓ!!!! Segons NOCASH en SPU encara que faja un sw.b escriu els 16 !!!!
// ARA MATEIXA estic escrivint sols la part baixa i la alta a 0 !!!!
static bool
mem_write8 (
            const uint32_t addr,
            const uint8_t  data,
            const uint16_t data16,
            const bool     is_le
            )
{

  uint32_t aux;
  int port;
  
  
  aux= addr;
  
  /* RAM. */
  if ( aux < _ram.end_ram8 )
    {
#ifdef PSX_LE
      if ( !is_le ) aux^= 1;
#else
      if ( is_le ) aux^= 1;
#endif
      _ram.v[aux&RAM_MASK]= data;
    }

  /* La resta de l'àrea de la RAM. */
  else if ( aux <= 0x00800000 )
    {
      if ( aux >= _ram.end_hz8 &&
           (aux != 0x00800000 || _ram.locked_00800000 ) )
        return false;
    }

  /* Àrea en blanc. */
  else if ( aux < 0x1F000000 ) return false;
  
  /* Expansion Region 1 */
  else if ( aux < 0x1F800000 )
    {
      if ( aux >= _exp1.addr8 && aux < _exp1.ds.end8 )
        printf("W8 Expansion 1\n");
      else return false;
    }

  /* Scratchpad */
  else if ( aux < 0x1F800400 )
    {
#ifdef PSX_LE
      if ( !is_le ) aux^= 3;
#else
      if ( is_le ) aux^= 3;
#endif  
      _scratchpad[aux&SP_MASK]= data;
    }

  /* Region after scratchpad */
  else if ( aux < 0x1F801000 ) return false;
  
  /* I/O Ports */
  else if ( aux < 0x1F802000 )
    {
      port= aux&0xFFF;
      switch ( port )
        {
          // Memory Control 1
        case 0x000:
        case 0x001:
        case 0x002:
        case 0x003: write_exp1_base_addr ( W32(data,port) ); break;
        case 0x004:
        case 0x005:
        case 0x006:
        case 0x007: write_exp2_base_addr ( W32(data,port) ); break;
        case 0x008:
        case 0x009:
        case 0x00A:
        case 0x00B: write_exp1_delay_size ( W32(data,port) ); break;
        case 0x00C:
        case 0x00D:
        case 0x00E:
        case 0x00F: write_exp3_delay_size ( W32(data,port) ); break;
        case 0x010:
        case 0x011:
        case 0x012:
        case 0x013: write_bios_delay_size ( W32(data,port) ); break;
        case 0x014:
        case 0x015:
        case 0x016:
        case 0x017: _spu= W32(data,port); break;
        case 0x018:
        case 0x019:
        case 0x01A:
        case 0x01B: _cdrom= W32(data,port); break;
        case 0x01C:
        case 0x01D:
        case 0x01E:
        case 0x01F: write_exp2_delay_size ( W32(data,port) ); break;
        case 0x020:
        case 0x021:
        case 0x022:
        case 0x023: _com= W32(data,port); break;
          // Peripheral I/O Ports
        case 0x040:
        case 0x041: PSX_joy_tx_data ( W16(data,port) ); break;
        case 0x042:
        case 0x043: break; // Els ignora???
        case 0x044:
        case 0x045: break; // Read-only
        case 0x046:
        case 0x047: break; // Read-only
        case 0x048:
        case 0x049: PSX_joy_mode_write ( W16(data,port) ); break;
        case 0x04A:
        case 0x04B: PSX_joy_ctrl_write ( W16(data,port) ); break;
        case 0x04C:
        case 0x04D: break;
        case 0x04E:
        case 0x04F: PSX_joy_baud_write ( W16(data,port) ); break;
          // Memory Control 2
        case 0x060:
        case 0x061:
        case 0x062:
        case 0x063: write_ram_size ( W32(data,port) ); break;
          // Interruption Control
        case 0x070:
        case 0x071:
        case 0x072:
        case 0x073: PSX_int_ack ( W32(data,port) ); break;
        case 0x074:
        case 0x075:
        case 0x076:
        case 0x077: PSX_int_write_imask ( W32(data,port) ); break;
          // DMA Registers
        case 0x080:
        case 0x081:
        case 0x082:
        case 0x083: PSX_dma_madr_write ( 0, W32(data,port) ); break;
        case 0x084:
        case 0x085:
        case 0x086:
        case 0x087: dma_bcr_write8 ( 0, port, data ); break;
        case 0x088:
        case 0x089:
        case 0x08A:
        case 0x08B: PSX_dma_chcr_write ( 0, W32(data,port) ); break;
        case 0x090:
        case 0x091:
        case 0x092:
        case 0x093: PSX_dma_madr_write ( 1, W32(data,port) ); break;
        case 0x094:
        case 0x095:
        case 0x096:
        case 0x097: dma_bcr_write8 ( 1, port, data ); break;
        case 0x098:
        case 0x099:
        case 0x09A:
        case 0x09B: PSX_dma_chcr_write ( 1, W32(data,port) ); break;
        case 0x0A0:
        case 0x0A1:
        case 0x0A2:
        case 0x0A3: PSX_dma_madr_write ( 2, W32(data,port) ); break;
        case 0x0A4:
        case 0x0A5:
        case 0x0A6:
        case 0x0A7: dma_bcr_write8 ( 2, port, data ); break;
        case 0x0A8:
        case 0x0A9:
        case 0x0AA:
        case 0x0AB: PSX_dma_chcr_write ( 2, W32(data,port) ); break;
        case 0x0B0:
        case 0x0B1:
        case 0x0B2:
        case 0x0B3: PSX_dma_madr_write ( 3, W32(data,port) ); break;
        case 0x0B4:
        case 0x0B5:
        case 0x0B6:
        case 0x0B7: dma_bcr_write8 ( 3, port, data ); break;
        case 0x0B8:
        case 0x0B9:
        case 0x0BA:
        case 0x0BB: PSX_dma_chcr_write ( 3, W32(data,port) ); break;
        case 0x0C0:
        case 0x0C1:
        case 0x0C2:
        case 0x0C3: PSX_dma_madr_write ( 4, W32(data,port) ); break;
        case 0x0C4:
        case 0x0C5:
        case 0x0C6:
        case 0x0C7: dma_bcr_write8 ( 4, port, data ); break;
        case 0x0C8:
        case 0x0C9:
        case 0x0CA:
        case 0x0CB: PSX_dma_chcr_write ( 4, W32(data,port) ); break;
        case 0x0D0:
        case 0x0D1:
        case 0x0D2:
        case 0x0D3: PSX_dma_madr_write ( 5, W32(data,port) ); break;
        case 0x0D4:
        case 0x0D5:
        case 0x0D6:
        case 0x0D7: dma_bcr_write8 ( 5, port, data ); break;
        case 0x0D8:
        case 0x0D9:
        case 0x0DA:
        case 0x0DB: PSX_dma_chcr_write ( 5, W32(data,port) ); break;
        case 0x0E0:
        case 0x0E1:
        case 0x0E2:
        case 0x0E3: PSX_dma_madr_write ( 6, W32(data,port) ); break;
        case 0x0E4:
        case 0x0E5:
        case 0x0E6:
        case 0x0E7: dma_bcr_write8 ( 6, port, data ); break;
        case 0x0E8:
        case 0x0E9:
        case 0x0EA:
        case 0x0EB: PSX_dma_chcr_write ( 6, W32(data,port) ); break;
        case 0x0F0:
        case 0x0F1:
        case 0x0F2:
        case 0x0F3: PSX_dma_dpcr_write ( W32(data,port) ); break;
        case 0x0F4:
        case 0x0F5:
        case 0x0F6:
        case 0x0F7: PSX_dma_dicr_write ( W32(data,port) ); break;
          // Timers
        case 0x100:
        case 0x101:
        case 0x102:
        case 0x103: PSX_timers_set_counter_value ( W32(data,port), 0 ); break;
        case 0x104:
        case 0x105:
        case 0x106:
        case 0x107: PSX_timers_set_counter_mode ( W32(data,port), 0 ); break;
        case 0x108:
        case 0x109:
        case 0x10A:
        case 0x10B: PSX_timers_set_target_value ( W32(data,port), 0 ); break;
        case 0x110:
        case 0x111:
        case 0x112:
        case 0x113: PSX_timers_set_counter_value ( W32(data,port), 1 ); break;
        case 0x114:
        case 0x115:
        case 0x116:
        case 0x117: PSX_timers_set_counter_mode ( W32(data,port), 1 ); break;
        case 0x118:
        case 0x119:
        case 0x11A:
        case 0x11B: PSX_timers_set_target_value ( W32(data,port), 1 ); break;
        case 0x120:
        case 0x121:
        case 0x122:
        case 0x123: PSX_timers_set_counter_value ( W32(data,port), 2 ); break;
        case 0x124:
        case 0x125:
        case 0x126:
        case 0x127: PSX_timers_set_counter_mode ( W32(data,port), 2 ); break;
        case 0x128:
        case 0x129:
        case 0x12A:
        case 0x12B: PSX_timers_set_target_value ( W32(data,port), 2 ); break;
          // CDROM Registers
        case 0x800: PSX_cd_set_index ( data ); break;
        case 0x801: PSX_cd_port1_write ( data ); break;
        case 0x802: PSX_cd_port2_write ( data ); break;
        case 0x803: PSX_cd_port3_write ( data ); break;
          // GPU Registers ¿¿??
          // MDEC Registers ¿¿??
          // SPU Voice 0..23 Registers
        case 0xC00:
        case 0xC01:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 0, data16 );
          break;
        case 0xC02:
        case 0xC03:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 0, data16 );
          break;
        case 0xC04:
        case 0xC05:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 0, data16 );
          break;
        case 0xC06:
        case 0xC07:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 0, data16 );
          break;
        case 0xC08:
        case 0xC09:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 0, data16 );
          break;
        case 0xC0A:
        case 0xC0B:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 0, data16 );
          break;
        case 0xC0C:
        case 0xC0D:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol ( 0, data16 );
          break;
        case 0xC0E:
        case 0xC0F:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 0, data16 );
          break;
        case 0xC10:
        case 0xC11:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 1, data16 );
          break;
        case 0xC12:
        case 0xC13:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 1, data16 );
          break;
        case 0xC14:
        case 0xC15:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 1, data16 );
          break;
        case 0xC16:
        case 0xC17:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 1, data16 );
          break;
        case 0xC18:
        case 0xC19:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 1, data16 );
          break;
        case 0xC1A:
        case 0xC1B:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 1, data16 );
          break;
        case 0xC1C:
        case 0xC1D:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 1, data16 );
          break;
        case 0xC1E:
        case 0xC1F:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 1, data16 );
          break;
        case 0xC20:
        case 0xC21:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 2, data16 );
          break;
        case 0xC22:
        case 0xC23:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 2, data16 );
          break;
        case 0xC24:
        case 0xC25:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 2, data16 );
          break;
        case 0xC26:
        case 0xC27:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 2, data16 );
          break;
        case 0xC28:
        case 0xC29:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 2, data16 );
          break;
        case 0xC2A:
        case 0xC2B:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 2, data16 );
          break;
        case 0xC2C:
        case 0xC2D:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 2, data16 );
          break;
        case 0xC2E:
        case 0xC2F:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 2, data16 );
          break;
        case 0xC30:
        case 0xC31:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 3, data16 );
          break;
        case 0xC32:
        case 0xC33:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 3, data16 );
          break;
        case 0xC34:
        case 0xC35:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 3, data16 );
          break;
        case 0xC36:
        case 0xC37:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 3, data16 );
          break;
        case 0xC38:
        case 0xC39:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 3, data16 );
          break;
        case 0xC3A:
        case 0xC3B:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 3, data16 );
          break;
        case 0xC3C:
        case 0xC3D:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 3, data16 );
          break;
        case 0xC3E:
        case 0xC3F:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 3, data16 );
          break;
        case 0xC40:
        case 0xC41:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 4, data16 );
          break;
        case 0xC42:
        case 0xC43:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 4, data16 );
          break;
        case 0xC44:
        case 0xC45:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 4, data16 );
          break;
        case 0xC46:
        case 0xC47:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 4, data16 );
          break;
        case 0xC48:
        case 0xC49:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 4, data16 );
          break;
        case 0xC4A:
        case 0xC4B:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 4, data16 );
          break;
        case 0xC4C:
        case 0xC4D:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 4, data16 );
          break;
        case 0xC4E:
        case 0xC4F:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 4, data16 );
          break;
        case 0xC50:
        case 0xC51:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 5, data16 );
          break;
        case 0xC52:
        case 0xC53:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 5, data16 );
          break;
        case 0xC54:
        case 0xC55:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 5, data16 );
          break;
        case 0xC56:
        case 0xC57:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 5, data16 );
          break;
        case 0xC58:
        case 0xC59:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 5, data16 );
          break;
        case 0xC5A:
        case 0xC5B:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 5, data16 );
          break;
        case 0xC5C:
        case 0xC5D:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 5, data16 );
          break;
        case 0xC5E:
        case 0xC5F:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 5, data16 );
          break;
        case 0xC60:
        case 0xC61:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 6, data16 );
          break;
        case 0xC62:
        case 0xC63:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 6, data16 );
          break;
        case 0xC64:
        case 0xC65:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 6, data16 );
          break;
        case 0xC66:
        case 0xC67:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 6, data16 );
          break;
        case 0xC68:
        case 0xC69:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 6, data16 );
          break;
        case 0xC6A:
        case 0xC6B:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 6, data16 );
          break;
        case 0xC6C:
        case 0xC6D:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 6, data16 );
          break;
        case 0xC6E:
        case 0xC6F:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 6, data16 );
          break;
        case 0xC70:
        case 0xC71:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 7, data16 );
          break;
        case 0xC72:
        case 0xC73:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 7, data16 );
          break;
        case 0xC74:
        case 0xC75:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 7, data16 );
          break;
        case 0xC76:
        case 0xC77:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 7, data16 );
          break;
        case 0xC78:
        case 0xC79:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 7, data16 );
          break;
        case 0xC7A:
        case 0xC7B:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 7, data16 );
          break;
        case 0xC7C:
        case 0xC7D:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 7, data16 );
          break;
        case 0xC7E:
        case 0xC7F:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 7, data16 );
          break;
        case 0xC80:
        case 0xC81:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 8, data16 );
          break;
        case 0xC82:
        case 0xC83:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 8, data16 );
          break;
        case 0xC84:
        case 0xC85:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 8, data16 );
          break;
        case 0xC86:
        case 0xC87:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 8, data16 );
          break;
        case 0xC88:
        case 0xC89:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 8, data16 );
          break;
        case 0xC8A:
        case 0xC8B:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 8, data16 );
          break;
        case 0xC8C:
        case 0xC8D:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 8, data16 );
          break;
        case 0xC8E:
        case 0xC8F:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 8, data16 );
          break;
        case 0xC90:
        case 0xC91:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 9, data16 );
          break;
        case 0xC92:
        case 0xC93:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 9, data16 );
          break;
        case 0xC94:
        case 0xC95:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 9, data16 );
          break;
        case 0xC96:
        case 0xC97:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 9, data16 );
          break;
        case 0xC98:
        case 0xC99:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 9, data16 );
          break;
        case 0xC9A:
        case 0xC9B:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 9, data16 );
          break;
        case 0xC9C:
        case 0xC9D:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 9, data16 );
          break;
        case 0xC9E:
        case 0xC9F:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 9, data16 );
          break;
        case 0xCA0:
        case 0xCA1:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 10, data16 );
          break;
        case 0xCA2:
        case 0xCA3:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 10, data16 );
          break;
        case 0xCA4:
        case 0xCA5:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 10, data16 );
          break;
        case 0xCA6:
        case 0xCA7:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 10, data16 );
          break;
        case 0xCA8:
        case 0xCA9:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 10, data16 );
          break;
        case 0xCAA:
        case 0xCAB:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 10, data16 );
          break;
        case 0xCAC:
        case 0xCAD:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 10, data16 );
          break;
        case 0xCAE:
        case 0xCAF:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 10, data16 );
          break;
        case 0xCB0:
        case 0xCB1:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 11, data16 );
          break;
        case 0xCB2:
        case 0xCB3:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 11, data16 );
          break;
        case 0xCB4:
        case 0xCB5:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 11, data16 );
          break;
        case 0xCB6:
        case 0xCB7:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 11, data16 );
          break;
        case 0xCB8:
        case 0xCB9:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 11, data16 );
          break;
        case 0xCBA:
        case 0xCBB:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 11, data16 );
          break;
        case 0xCBC:
        case 0xCBD:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 11, data16 );
          break;
        case 0xCBE:
        case 0xCBF:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 11, data16 );
          break;
        case 0xCC0:
        case 0xCC1:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 12, data16 );
          break;
        case 0xCC2:
        case 0xCC3:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 12, data16 );
          break;
        case 0xCC4:
        case 0xCC5:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 12, data16 );
          break;
        case 0xCC6:
        case 0xCC7:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 12, data16 );
          break;
        case 0xCC8:
        case 0xCC9:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 12, data16 );
          break;
        case 0xCCA:
        case 0xCCB:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 12, data16 );
          break;
        case 0xCCC:
        case 0xCCD:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 12, data16 );
          break;
        case 0xCCE:
        case 0xCCF:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 12, data16 );
          break;
        case 0xCD0:
        case 0xCD1:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 13, data16 );
          break;
        case 0xCD2:
        case 0xCD3:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 13, data16 );
          break;
        case 0xCD4:
        case 0xCD5:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 13, data16 );
          break;
        case 0xCD6:
        case 0xCD7:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 13, data16 );
          break;
        case 0xCD8:
        case 0xCD9:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 13, data16 );
          break;
        case 0xCDA:
        case 0xCDB:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 13, data16 );
          break;
        case 0xCDC:
        case 0xCDD:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 13, data16 );
          break;
        case 0xCDE:
        case 0xCDF:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 13, data16 );
          break;
        case 0xCE0:
        case 0xCE1:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 14, data16 );
          break;
        case 0xCE2:
        case 0xCE3:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 14, data16 );
          break;
        case 0xCE4:
        case 0xCE5:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 14, data16 );
          break;
        case 0xCE6:
        case 0xCE7:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 14, data16 );
          break;
        case 0xCE8:
        case 0xCE9:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 14, data16 );
          break;
        case 0xCEA:
        case 0xCEB:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 14, data16 );
          break;
        case 0xCEC:
        case 0xCED:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 14, data16 );
          break;
        case 0xCEE:
        case 0xCEF:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 14, data16 );
          break;
        case 0xCF0:
        case 0xCF1:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 15, data16 );
          break;
        case 0xCF2:
        case 0xCF3:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 15, data16 );
          break;
        case 0xCF4:
        case 0xCF5:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 15, data16 );
          break;
        case 0xCF6:
        case 0xCF7:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 15, data16 );
          break;
        case 0xCF8:
        case 0xCF9:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 15, data16 );
          break;
        case 0xCFA:
        case 0xCFB:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 15, data16 );
          break;
        case 0xCFC:
        case 0xCFD:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 15, data16 );
          break;
        case 0xCFE:
        case 0xCFF:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 15, data16 );
          break;
        case 0xD00:
        case 0xD01:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 16, data16 );
          break;
        case 0xD02:
        case 0xD03:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 16, data16 );
          break;
        case 0xD04:
        case 0xD05:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 16, data16 );
          break;
        case 0xD06:
        case 0xD07:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 16, data16 );
          break;
        case 0xD08:
        case 0xD09:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 16, data16 );
          break;
        case 0xD0A:
        case 0xD0B:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 16, data16 );
          break;
        case 0xD0C:
        case 0xD0D:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 16, data16 );
          break;
        case 0xD0E:
        case 0xD0F:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 16, data16 );
          break;
        case 0xD10:
        case 0xD11:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 17, data16 );
          break;
        case 0xD12:
        case 0xD13:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 17, data16 );
          break;
        case 0xD14:
        case 0xD15:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 17, data16 );
          break;
        case 0xD16:
        case 0xD17:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 17, data16 );
          break;
        case 0xD18:
        case 0xD19:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 17, data16 );
          break;
        case 0xD1A:
        case 0xD1B:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 17, data16 );
          break;
        case 0xD1C:
        case 0xD1D:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 17, data16 );
          break;
        case 0xD1E:
        case 0xD1F:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 17, data16 );
          break;
        case 0xD20:
        case 0xD21:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 18, data16 );
          break;
        case 0xD22:
        case 0xD23:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 18, data16 );
          break;
        case 0xD24:
        case 0xD25:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 18, data16 );
          break;
        case 0xD26:
        case 0xD27:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 18, data16 );
          break;
        case 0xD28:
        case 0xD29:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 18, data16 );
          break;
        case 0xD2A:
        case 0xD2B:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 18, data16 );
          break;
        case 0xD2C:
        case 0xD2D:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 18, data16 );
          break;
        case 0xD2E:
        case 0xD2F:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 18, data16 );
          break;
        case 0xD30:
        case 0xD31:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 19, data16 );
          break;
        case 0xD32:
        case 0xD33:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 19, data16 );
          break;
        case 0xD34:
        case 0xD35:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 19, data16 );
          break;
        case 0xD36:
        case 0xD37:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 19, data16 );
          break;
        case 0xD38:
        case 0xD39:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 19, data16 );
          break;
        case 0xD3A:
        case 0xD3B:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 19, data16 );
          break;
        case 0xD3C:
        case 0xD3D:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 19, data16 );
          break;
        case 0xD3E:
        case 0xD3F:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 19, data16 );
          break;
        case 0xD40:
        case 0xD41:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 20, data16 );
          break;
        case 0xD42:
        case 0xD43:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 20, data16 );
          break;
        case 0xD44:
        case 0xD45:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 20, data16 );
          break;
        case 0xD46:
        case 0xD47:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 20, data16 );
          break;
        case 0xD48:
        case 0xD49:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 20, data16 );
          break;
        case 0xD4A:
        case 0xD4B:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 20, data16 );
          break;
        case 0xD4C:
        case 0xD4D:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 20, data16 );
          break;
        case 0xD4E:
        case 0xD4F:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 20, data16 );
          break;
        case 0xD50:
        case 0xD51:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 21, data16 );
          break;
        case 0xD52:
        case 0xD53:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 21, data16 );
          break;
        case 0xD54:
        case 0xD55:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 21, data16 );
          break;
        case 0xD56:
        case 0xD57:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 21, data16 );
          break;
        case 0xD58:
        case 0xD59:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 21, data16 );
          break;
        case 0xD5A:
        case 0xD5B:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 21, data16 );
          break;
        case 0xD5C:
        case 0xD5D:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 21, data16 );
          break;
        case 0xD5E:
        case 0xD5F:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 21, data16 );
          break;
        case 0xD60:
        case 0xD61:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 22, data16 );
          break;
        case 0xD62:
        case 0xD63:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 22, data16 );
          break;
        case 0xD64:
        case 0xD65:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 22, data16 );
          break;
        case 0xD66:
        case 0xD67:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 22, data16 );
          break;
        case 0xD68:
        case 0xD69:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 22, data16 );
          break;
        case 0xD6A:
        case 0xD6B:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 22, data16 );
          break;
        case 0xD6C:
        case 0xD6D:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 22, data16 );
          break;
        case 0xD6E:
        case 0xD6F:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 22, data16 );
          break;
        case 0xD70:
        case 0xD71:
          if ( !(port&0x1) ) PSX_spu_voice_set_left_vol ( 23, data16 );
          break;
        case 0xD72:
        case 0xD73:
          if ( !(port&0x1) ) PSX_spu_voice_set_right_vol ( 23, data16 );
          break;
        case 0xD74:
        case 0xD75:
          if ( !(port&0x1) ) PSX_spu_voice_set_sample_rate ( 23, data16 );
          break;
        case 0xD76:
        case 0xD77:
          if ( !(port&0x1) ) PSX_spu_voice_set_start_addr ( 23, data16 );
          break;
        case 0xD78:
        case 0xD79:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_lo ( 23, data16 );
          break;
        case 0xD7A:
        case 0xD7B:
          if ( !(port&0x1) ) PSX_spu_voice_set_adsr_up ( 23, data16 );
          break;
        case 0xD7C:
        case 0xD7D:
          if (!(port&0x1)) PSX_spu_voice_set_cur_vol( 23, data16 );
          break;
        case 0xD7E:
        case 0xD7F:
          if ( !(port&0x1) ) PSX_spu_voice_set_repeat_addr ( 23, data16 );
          break;
          // SPU Control Registers
        case 0xD80:
        case 0xD81:
          if ( !(port&0x1) ) PSX_spu_set_left_vol ( data16 );
          break;
        case 0xD82:
        case 0xD83:
          if ( !(port&0x1) ) PSX_spu_set_right_vol ( data16 );
          break;
        case 0xD84:
        case 0xD85:
          if ( !(port&0x1) ) PSX_spu_reverb_set_vlout ( data16 );
          break;
        case 0xD86:
        case 0xD87:
          if ( !(port&0x1) ) PSX_spu_reverb_set_vrout ( data16 );
          break;
        case 0xD88:
        case 0xD89:
          if ( !(port&0x1) ) PSX_spu_key_on_lo ( data16 );
          break;
        case 0xD8A:
        case 0xD8B:
          if ( !(port&0x1) ) PSX_spu_key_on_up ( data16 );
          break;
        case 0xD8C:
        case 0xD8D:
          if ( !(port&0x1) ) PSX_spu_key_off_lo ( data16 );
          break;
        case 0xD8E:
        case 0xD8F:
          if ( !(port&0x1) ) PSX_spu_key_off_up ( data16 );
          break;
        case 0xD90:
        case 0xD91:
          if ( !(port&0x1) ) PSX_spu_set_pmon_lo ( data16 );
          break;
        case 0xD92:
        case 0xD93:
          if ( !(port&0x1) ) PSX_spu_set_pmon_up ( data16 );
          break;
        case 0xD94:
        case 0xD95:
          if ( !(port&0x1) ) PSX_spu_set_non_lo ( data16 );
          break;
        case 0xD96:
        case 0xD97:
          if ( !(port&0x1) ) PSX_spu_set_non_up ( data16 );
          break;
        case 0xD98:
        case 0xD99:
          if ( !(port&0x1) ) PSX_spu_set_eon_lo ( data16 );
          break;
        case 0xD9A:
        case 0xD9B:
          if ( !(port&0x1) ) PSX_spu_set_eon_up ( data16 );
          break;
        case 0xD9C:
        case 0xD9D:
          if ( !(port&0x1) ) PSX_spu_set_endx_lo ( data16 );
          break;
        case 0xD9E:
        case 0xD9F:
          if ( !(port&0x1) ) PSX_spu_set_endx_up ( data16 );
          break;
        case 0xDA0:
        case 0xDA1: if ( !(port&0x1) ) PSX_spu_set_unk_da0 ( data16 ); break;
        case 0xDA2:
        case 0xDA3:
          if ( !(port&0x1) ) PSX_spu_reverb_set_mbase ( data16 );
          break;
        case 0xDA4:
        case 0xDA5:
          if ( !(port&0x1) ) PSX_spu_set_irq_addr ( data16 );
          break;
        case 0xDA6:
        case 0xDA7:
          if ( !(port&0x1) ) PSX_spu_set_addr ( data16 );
          break;
        case 0xDA8:
        case 0xDA9:
          if ( !(port&0x1) ) PSX_spu_write ( data16 );
          break;
        case 0xDAA:
        case 0xDAB:
          if ( !(port&0x1) ) PSX_spu_set_control ( data16 );
          break;
        case 0xDAC:
        case 0xDAD:
          if ( !(port&0x1) ) PSX_spu_set_transfer_type ( data16 );
          break;
        case 0xDAE:
        case 0xDAF:
          printf ( "W8 port: 0xDAE...0xDAF: cal implementar??\n" );
          break;
        case 0xDB0:
        case 0xDB1:
          if ( !(port&0x1) ) PSX_spu_set_cd_vol_l ( data16 );
          break;
        case 0xDB2:
        case 0xDB3:
          if ( !(port&0x1) ) PSX_spu_set_cd_vol_r ( data16 );
          break;
        case 0xDB4:
        case 0xDB5:
          if ( !(port&0x1) ) PSX_spu_set_ext_vol_l ( data16 );
          break;
        case 0xDB6:
        case 0xDB7:
          if ( !(port&0x1) ) PSX_spu_set_ext_vol_r ( data16 );
          break;
        case 0xDB8 ... 0xDBB:
          printf ( "W8 port: 0xDB8...0xDBB: cal implementar??\n" );
          break;
        case 0xDBC ... 0xDBF:
          if ( !(port&0x1) ) PSX_spu_set_unk_dbc ( (port>>1)&0x1, data16 );
          break;
          // SPU Reverb Configuration Area
        case 0xDC0 ... 0xDFF:
          if ( !(port&0x1) ) PSX_spu_reverb_set_reg ( (port>>1)&0x1F, data16 );
          break;
          // SPU Internal Registers
        case 0xE00 ... 0xE5F:
          printf ( "W8 port: 0xE00...0xE5F: cal implementar??\n" );
          break;
        case 0xE60 ... 0xE7F:
          if ( !(port&0x1) ) PSX_spu_set_unk_e60 ( (port>>1)&0xF, data16 );
          break;
        case 0xE80 ... 0xFFF:
          break; // Unknown? (Read: FFh-filled) (Unused or Write only?)
          // Locked 
        default:
          printf("W8 I/O (0x%x)\nCal return false!!\n",port);
          /*return false;*/
        }
    }
  
  /* Expansion Region 2 i 3 */
  else if ( aux < 0x1FC00000 )
    {
      if ( aux >= _exp2.addr8 && aux < _exp2.ds.end8 )
        printf("W8 Expansion 2\n");
      else if ( aux >= 0x1FA00000 && aux < _exp3.end8 )
        printf("W8 Expansion 3\n");
      else return false;
    }
  
  /* BIOS i resta fins 1FFFFFFF */
  else
    {
      if ( aux < _bios.ds.end8 ) return true;
      else return false;
    }
  
  return true;
  
} /* end mem_write8 */


static bool
mem_read_trace (
        	const uint32_t  addr,
        	uint32_t       *dst
        	)
{

  bool ret;


  ret= mem_read ( addr, dst );
  _mem_access ( PSX_READ, addr, *dst, !ret, _udata );
  
  return ret;
  
} /* end mem_read_trace */


static bool
mem_read16_trace (
        	  const uint32_t  addr,
        	  uint16_t       *dst,
        	  const bool      is_le
        	  )
{

  bool ret;


  ret= mem_read16 ( addr, dst, is_le );
  _mem_access16 ( PSX_READ, addr, *dst, !ret, _udata );
  
  return ret;
  
} /* end mem_read16_trace */


static bool
mem_read8_trace (
        	 const uint32_t  addr,
        	 uint8_t        *dst,
        	 const bool      is_le
        	 )
{

  bool ret;


  ret= mem_read8 ( addr, dst, is_le );
  _mem_access8 ( PSX_READ, addr, *dst, !ret, _udata );
  
  return ret;
  
} /* end mem_read8_trace */


static bool
mem_write_trace (
        	 const uint32_t addr,
        	 const uint32_t data
        	 )
{

  bool ret;

  
  ret= mem_write ( addr, data );
  _mem_access ( PSX_WRITE, addr, data, !ret, _udata );
  
  return ret;
  
} /* end mem_write_trace */


static bool
mem_write16_trace (
        	   const uint32_t addr,
        	   const uint16_t data,
        	   const bool     is_le
        	   )
{

  bool ret;


  ret= mem_write16 ( addr, data, is_le );
  _mem_access16 ( PSX_WRITE, addr, data, !ret, _udata );
  
  return ret;
  
} /* end mem_write16_trace */


static bool
mem_write8_trace (
        	  const uint32_t addr,
        	  const uint8_t  data,
        	  const uint16_t data16,
        	  const bool     is_le
        	  )
{

  bool ret;
  uint32_t tmp;
  

  ret= mem_write8 ( addr, data, data16, is_le );
  tmp= addr&0x1FFFFFFF;
  if ( tmp >= 0x1F801C00 && tmp <= 0x1F801E7F )
    _mem_access16 ( PSX_WRITE, addr, data16, !ret, _udata );
  else
    _mem_access8 ( PSX_WRITE, addr, data, !ret, _udata );
  
  return ret;
  
} /* end mem_write8_trace */




/**********************/
/* FUNCIONS PÚBLIQUES */
/**********************/

void
PSX_mem_init (
              const uint8_t    bios[PSX_BIOS_SIZE],
              PSX_MemChanged  *mem_changed,
              PSX_MemAccess   *mem_access,
              PSX_MemAccess16 *mem_access16,
              PSX_MemAccess8  *mem_access8,
              void            *udata
              )
{

  /* Callbacks. */
  _mem_changed= mem_changed;
  _mem_access= mem_access;
  _mem_access16= mem_access16;
  _mem_access8= mem_access8;
  _udata= udata;
  
  /* Funcions. */
  _mem_read= mem_read;
  _mem_write= mem_write;
  _mem_read16= mem_read16;
  _mem_write16= mem_write16;
  _mem_read8= mem_read8;
  _mem_write8= mem_write8;
  
  /* Estat. */
  init_ram ();
  init_bios ( bios );
  write_exp3_delay_size ( 0x00003022 );
  init_exp ( &_exp1, 0x1F000000, 0x0013243F );
  init_exp ( &_exp2, 0x1F802000, 0x00070777 );
  _spu= 0x200931E1;
  _cdrom= 0x00020843;
  _com= 0x00031125;
  memset ( _scratchpad, 0, 1024 );
  
} /* end PSX_mem_init */


bool
PSX_mem_read (
              const uint32_t  addr,
              uint32_t       *dst
              )
{
  return _mem_read ( addr, dst );
} /* end PSX_mem_read */


bool
PSX_mem_read16 (
        	const uint32_t  addr,
        	uint16_t       *dst,
        	const bool      is_le
        	)
{
  return _mem_read16 ( addr, dst, is_le );
} /* end PSX_mem_read16 */


bool
PSX_mem_read8 (
               const uint32_t  addr,
               uint8_t        *dst,
               const bool      is_le
               )
{
  return _mem_read8 ( addr, dst, is_le );
} /* end PSX_mem_read8 */


bool
PSX_mem_write (
               const uint32_t addr,
               const uint32_t data
               )
{
  return _mem_write ( addr, data );
} /* end PSX_mem_write */


bool
PSX_mem_write16 (
        	 const uint32_t addr,
        	 const uint16_t data,
        	 const bool     is_le
        	 )
{
  return _mem_write16 ( addr, data, is_le );
} /* end PSX_mem_write16 */


bool
PSX_mem_write8 (
        	const uint32_t addr,
        	const uint8_t  data,
        	const uint16_t data16,
        	const bool     is_le
        	)
{
  return _mem_write8 ( addr, data, data16, is_le );
} /* end PSX_mem_write8 */


void
PSX_mem_set_mode_trace (
        		const bool val
        		)
{

  if ( _mem_access != NULL )
    {
      if ( val )
        {
          _mem_read= mem_read_trace;
          _mem_write= mem_write_trace;
        }
      else
        {
          _mem_read= mem_read;
          _mem_write= mem_write;
        }
    }
  if ( _mem_access16 != NULL )
    {
      if ( val )
        {
          _mem_read16= mem_read16_trace;
          _mem_write16= mem_write16_trace;
        }
      else
        {
          _mem_read16= mem_read16;
          _mem_write16= mem_write16;
        }
    }
  if ( _mem_access8 != NULL )
    {
      if ( val )
        {
          _mem_read8= mem_read8_trace;
          _mem_write8= mem_write8_trace;
        }
      else
        {
          _mem_read8= mem_read8;
          _mem_write8= mem_write8;
        }
    }
  
} /* end PSX_mem_set_mode_trace */


void
PSX_mem_get_map (
        	 PSX_MemMap *map
        	 )
{

  map->ram.end_ram= _ram.end_ram8;
  map->ram.end_hz= _ram.end_hz8;
  map->ram.locked_00800000= _ram.locked_00800000;
  
} /* end PSX_mem_get_map */


void
PSX_change_bios (
                 const uint8_t bios[PSX_BIOS_SIZE]
                 )
{

#ifdef PSX_LE
  memcpy ( _bios.v, bios, PSX_BIOS_SIZE );
#else
  swap_u32 ( (uint32_t *) _bios.v, (const uint32_t *) bios, PSX_BIOS_SIZE>>2 );
#endif
  
} // end PSX_change_bios

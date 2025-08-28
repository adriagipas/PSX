/*
 * Copyright 2015-2025 Adrià Giménez Pastor.
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
 *  cpu_interpreter.c - Implementació de l'intèrpret del R3000A.
 *
 */
/*
 * NOTA: Sobre COP2 (GTE). Nocash PSX diu que llegir i escriure en GTE
 * té un "delay" de 2 instruccions. No tinc molt clar si es refereix a
 * un delay intern del GTE o del propi processador. De moment assumiré
 * que el codi està ben programat i seguiré amb el delay per defecte
 * del processador.
 */


#include <stddef.h>
#include <stdlib.h>

#include "PSX.h"




/**********/
/* MACROS */
/**********/

#define WW (_warning)
#define UDATA (_udata)

#define SET_BRANCH(ADDR,COND)        		\
  ++_delayed_ops;        			\
  _branch.addr= (ADDR);        			\
  _branch.cond= (COND);        			\
  _branch.state= BRANCH_WAITING

#define SET_LDELAYED(REG,VAL) set_ldelayed ( (REG), (VAL), false )

#define SET_LDELAYED_LWLR(REG,VAL) set_ldelayed ( (REG), (VAL), true )

#define SET_REG(REG,VAL)        			\
  GPR[(REG)].v= (VAL);        				\
  _ldelayed.v[(REG)].proceed= false

#define SET_COP0WRITE(REG,VAL) set_cop0write ( (REG), (VAL) )

#define SET_COP2WRITE(REG,VAL) set_cop2write ( (REG), (VAL) )

/* Torna a executar l'última instrucció. */
#define HALT (new_PC-= 4)


/*********/
/* TIPUS */
/*********/

typedef struct
{
  int      shift;
  uint32_t mask;
} lwlr_op_t;


/* REGISTRES ******************************************************************/
#define GPR (PSX_cpu_regs.gpr)
#define HI (PSX_cpu_regs.hi)
#define LO (PSX_cpu_regs.lo)
#define PC (PSX_cpu_regs.pc)

#define GET_LWLR_REG_VAL(REG)                                           \
  ((_ldelayed.v[REG].state!=LDELAYED_EMPTY && _ldelayed.v[REG].is_lwlr) ? \
   _ldelayed.v[REG].val : GPR[REG].v)

/* COP0 i CACHE ***************************************************************/
#define COP0R3_BPC (PSX_cpu_regs.cop0r3_bpc)
#define COP0R5_BDA (PSX_cpu_regs.cop0r5_bda)
#define COP0R7_DCIC (PSX_cpu_regs.cop0r7_dcic)
#define COP0R8_BAD_VADDR (PSX_cpu_regs.cop0r8_bad_vaddr)
#define COP0R9_BDAM (PSX_cpu_regs.cop0r9_bdam)
#define COP0R11_BPCM (PSX_cpu_regs.cop0r11_bpcm)
#define COP0R12_SR (PSX_cpu_regs.cop0r12_sr)
#define COP0R13_CAUSE (PSX_cpu_regs.cop0r13_cause)
#define COP0R14_EPC (PSX_cpu_regs.cop0r14_epc)

#define COP0_SR_IEC 0x00000001
#define COP0_SR_KUC 0x00000002
#define COP0_SR_ISC 0x00010000
#define COP0_SR_SWC 0x00020000
#define COP0_SR_BEV 0x00400000
#define COP0_SR_RE  0x02000000
#define COP0_SR_CU0 0x10000000
#define COP0_SR_CU2 0x40000000

#define COP0_CAUSE_BD 0x80000000

#define CACHE_CONTROL (PSX_cpu_regs.cache_control)

#define CC_SCRATCHPAD_ENABLE_1 0x00000008
#define CC_SCRATCHPAD_ENABLE_2 0x00000080
#define CC_CODECACHE_ENABLE    0x00000800


/* DESCODIFICA ****************************************************************/
#define OPCODE _opcode
#define RS _rs_field
#define RT _rt_field
#define RD _rd_field
#define SA _sa_field
#define FUNCTION _func_field
#define INSTR_INDEX _index_field
#define IMMEDIATE _imm_field


/* EXCEPCIONS *****************************************************************/
#define EXCEPTION(EXCP)        exception ( EXCP )

#define EXCEPTION_ADDR(EXCP,ADDR)        	\
  COP0R8_BAD_VADDR= (ADDR);        		\
  EXCEPTION ( EXCP )

#define EXCEPTION_COP(EXCP,COP)        				\
  COP0R13_CAUSE= (COP0R13_CAUSE&0xCFFFFFFF) | ((COP)<<27);        \
  EXCEPTION ( EXCP )

#define INTERRUPT_EXCP 0
#define ADDRESS_ERROR_LOAD_EXCP 4
#define ADDRESS_ERROR_STORE_EXCP 5
#define BUS_ERROR_INST_EXCP 6
#define BUS_ERROR_DATA_EXCP 7
#define SYSTEM_CALL_EXCP 8
#define BREAKPOINT_EXCP 9
#define RESERVED_INST_EXCP 10
#define COP_UNUSABLE_EXCP 11
#define INTEGER_OVERFLOW_EXCP 12




/**************/
/* DEFINCIONS */
/**************/

static int
cop2 (void);

static void
cop0_write_reg (
                const int      reg,
                const uint32_t val
                );

static bool
mem_read (
          const uint32_t  addr,
          uint32_t       *dst,
          const bool      read_data
          );




/*********/
/* ESTAT */
/*********/

/* Callbacks. */
static PSX_Warning *_warning;
static void *_udata;

/* Per a descodificar la instrucció. ATENCIÓ!!! Açò realment no és
   estat. */
static PSX_Word _inst_word;
static uint32_t _opcode;
static uint32_t _rs_field;
static uint32_t _rt_field;
static uint32_t _rd_field;
static uint32_t _sa_field;
static uint32_t _func_field;
static uint32_t _index_field;
static uint16_t _imm_field;

/* Contador de delays pendents. Açò sí que és estat. */
static int _delayed_ops;

// Bàsicament, per a aconsseguir que el l'excepció provocada en un RFE
// es produisca en la instrucció al tornar.
// NOTA!! En realitat ho podria llevar, però ara mateixa és una
// optimització, ja que sols fa la comprovació de les interrupcions
// quan ha canviat alguna cosa que pot afectar a les interrupcions.
static bool _check_int;

// Nou valor del PC.
static uint32_t new_PC;

/* Per al branch. Açò sí que és estat. */
static struct
{

  enum {
    BRANCH_EMPTY,
    BRANCH_WAITING,
    BRANCH_READY
  }        state;
  uint32_t addr;
  bool     cond;
  
} _branch;

/* Per a la lecture aplaçada. Açò sí que és estat. */
static struct
{
  
  struct
  {
    enum {
      LDELAYED_EMPTY,
      LDELAYED_WAITING,
      LDELAYED_READY
    }        state;
    uint32_t val;
    bool     proceed;
    bool     is_lwlr;
  } v[32];
  int as[32];
  int N;
  
} _ldelayed;

// Per a l'escriptura aplaçada en el COP0.
static struct
{

  struct
  {
    enum {
      COP0WRITE_EMPTY,
      COP0WRITE_WAITING,
      COP0WRITE_READY
    }        state;
    uint32_t val;
  }   v[64];
  int as[64];
  int N;
  
} _cop0write;

// Per a l'escriptura aplaçada en el COP2.
static struct
{

  struct
  {
    enum {
      COP2WRITE_EMPTY,
      COP2WRITE_WAITING,
      COP2WRITE_READY
    }        state;
    uint32_t val;
  }   v[64];
  int as[64];
  int N;
  
} _cop2write;

/* Estat auxiliar que es calcula a partir dels registres i que serveix
   per a consultar ràpidament l'estat. */
static struct
{

  bool cache_isolated;
  bool scratchpad_enabled;
  bool user_mode;
  bool is_le;
  bool cop0_enabled;
  bool cop2_enabled;
  
} _qflags;




/*********************/
/* FUNCIONS PRIVADES */
/*********************/

/* Altres *********************************************************************/
#if 0
static void
clear_ldelayed (void)
{

  int n,reg;


  for ( n= 0; n < _ldelayed.N; ++n )
    {
      reg= _ldelayed.as[n];
      _ldelayed.v[reg].state= LDELAYED_EMPTY;
      _ldelayed.v[reg].proceed= false;
    }
  _ldelayed.N= 0;
  
} // end clear_ldelayed
#endif


static void
set_ldelayed (
              const int      reg,
              const uint32_t val,
              const bool     is_lwlr
              )
{

  if ( reg == 0 ) return;
  if ( _ldelayed.v[reg].state == LDELAYED_EMPTY )
    {
      _ldelayed.as[_ldelayed.N++]= reg;
      ++_delayed_ops;
    }
  _ldelayed.v[reg].state= LDELAYED_WAITING;
  _ldelayed.v[reg].val= val;
  _ldelayed.v[reg].proceed= true;
  _ldelayed.v[reg].is_lwlr= is_lwlr;
  
} /* end set_ldelayed */


static void
update_ldelayed (void)
{

  int i, reg;


  i= 0;
  while ( i < _ldelayed.N )
    {
      reg= _ldelayed.as[i];
      switch ( _ldelayed.v[reg].state )
        {
        case LDELAYED_WAITING:
          _ldelayed.v[reg].state= LDELAYED_READY;
          ++i;
          break;
        case LDELAYED_READY:
          if ( _ldelayed.v[reg].proceed )
            GPR[reg].v= _ldelayed.v[reg].val;
          _ldelayed.v[reg].state= LDELAYED_EMPTY;
          _ldelayed.as[i]= _ldelayed.as[--_ldelayed.N];
          --_delayed_ops;
          break;
        default: printf ( "WTF !!! - update_ldelayed\n" ); break;
        }
    }
  
} /* update_ldelayed */


static void
set_cop0write (
               const int      reg,
               const uint32_t val
               )
{

  if ( _cop0write.v[reg].state == COP0WRITE_EMPTY )
    {
      _cop0write.as[_cop0write.N++]= reg;
      ++_delayed_ops;
    }
  else _warning ( _udata, "set_cop0write: overwrite reg %d", reg );
  _cop0write.v[reg].state= COP0WRITE_WAITING;
  _cop0write.v[reg].val= val;
  
} // end set_cop0write


static void
update_cop0write (void)
{
  
  int i, reg;
  
  
  i= 0;
  while ( i < _cop0write.N )
    {
      reg= _cop0write.as[i];
      switch ( _cop0write.v[reg].state )
        {
        case COP0WRITE_WAITING:
          _cop0write.v[reg].state= COP0WRITE_READY;
          ++i;
          break;
        case COP0WRITE_READY:
          cop0_write_reg ( reg, _cop0write.v[reg].val );
          _cop0write.v[reg].state= COP0WRITE_EMPTY;
          _cop0write.as[i]= _cop0write.as[--_cop0write.N];
          --_delayed_ops;
          break;
        default: printf ( "WTF !!! - update_cop0write\n" ); break;
        }
    }
  
} // update_cop0write


static void
set_cop2write (
               const int      reg,
               const uint32_t val
               )
{
  
  if ( _cop2write.v[reg].state == COP2WRITE_EMPTY )
    {
      _cop2write.as[_cop2write.N++]= reg;
      ++_delayed_ops;
    }
  else PSX_gte_write ( reg, _cop2write.v[reg].val );
  _cop2write.v[reg].state= COP2WRITE_WAITING;
  _cop2write.v[reg].val= val;
  
} // end set_cop2write


static void
update_cop2write (void)
{
  
  int i, reg;
  
  
  i= 0;
  while ( i < _cop2write.N )
    {
      reg= _cop2write.as[i];
      switch ( _cop2write.v[reg].state )
        {
        case COP2WRITE_WAITING:
          _cop2write.v[reg].state= COP2WRITE_READY;
          ++i;
          break;
        case COP2WRITE_READY:
          PSX_gte_write ( reg, _cop2write.v[reg].val );
          _cop2write.v[reg].state= COP2WRITE_EMPTY;
          _cop2write.as[i]= _cop2write.as[--_cop2write.N];
          --_delayed_ops;
          break;
        default: printf ( "WTF !!! - update_cop2write\n" ); break;
        }
    }
  
} // update_cop2write


static void
update_qflags (void)
{
  
  _qflags.scratchpad_enabled=
    (CACHE_CONTROL&(CC_SCRATCHPAD_ENABLE_1|CC_SCRATCHPAD_ENABLE_2))==
    (CC_SCRATCHPAD_ENABLE_1|CC_SCRATCHPAD_ENABLE_2);
  
  _qflags.cache_isolated=
    !_qflags.scratchpad_enabled && ((COP0R12_SR&COP0_SR_ISC)!=0);
  
  /* NOTA!!! En nocash està al revés del pdf R3000.pdf !!!! */
  /*_qflags.user_mode= ((COP0R12_SR&COP0_SR_KUC)==0);*/
  _qflags.user_mode= ((COP0R12_SR&COP0_SR_KUC)!=0);
  
  /* Per defecte està en Little-endian. */
  _qflags.is_le= !_qflags.user_mode || ((COP0R12_SR&COP0_SR_RE)==0);
  
  _qflags.cop0_enabled= !_qflags.user_mode || ((COP0R12_SR&COP0_SR_CU0)!=0);
  _qflags.cop2_enabled= ((COP0R12_SR&COP0_SR_CU2)!=0);

  /* <--- S'activa per algun motiu, però no pareix que importe res.
  if ( (COP0R12_SR&COP0_SR_SWC) != 0 )
    {
      WW ( UDATA, "no s'ha implementat el swap de la memòria cau (SwC)" );
    }
  */
  
} /* end update_qflags */


/* Excepcions *****************************************************************/
#if 0
static void
clear_delayed_ops (void)
{

  _branch.state= BRANCH_EMPTY;
  clear_ldelayed ();
  _cop0write.state= COP0WRITE_EMPTY;
  _cop2write.state= COP2WRITE_EMPTY;
  _delayed_ops= 0;
  
} // end clear_delayed_ops
#endif


static void
exception (
           const int id
           )
{

  uint32_t tmp;

  
  /* Fixa EPC. */
  /* PC ja s'ha incrementat quan estem ací. */
  if ( _branch.state == BRANCH_READY ) 
    {
      /* ESTEM EXECUTANT UNA INSTRUCCIÓ EN EL SLOT ! */
      COP0R14_EPC= PC-4;
      COP0R13_CAUSE|= COP0_CAUSE_BD;
    }
  else
    {
      // Pot ser que la següent instrucció siga un COP2, en eixe cas,
      // l'executem immediatament.
      tmp= _inst_word.v;
      mem_read ( PC, &_inst_word.v, false );
      if ( (_inst_word.v&0xFE000000) == 0x4A000000 )
        {
          cop2 ();
          _inst_word.v= tmp;
        }
      COP0R14_EPC= PC;
      COP0R13_CAUSE&= ~COP0_CAUSE_BD;
    }

  // Deshabilita els banch pendents.
  //if ( _delayed_ops ) clear_delayed_ops ();
  if ( _branch.state != BRANCH_EMPTY )
    {
      _branch.state= BRANCH_EMPTY;
      --_delayed_ops;
    }
  
  /* NOTA!!! Resulta que en el R3000A el KUc va al revés, 0 és superusuari. */
  /* KUp,IEp -> KUo,IEo; KUc,IEc -> KUp,IEp; 0,0 -> KUc,IEc. */
  COP0R12_SR= (COP0R12_SR&0xFFFFFFC0) | ((COP0R12_SR&0xF)<<2) | 0x00000000;
  update_qflags ();

  /* Fixa la causa. */
  COP0R13_CAUSE= (COP0R13_CAUSE&0xFFFFFF83) | (id<<2);
  
  /* Canvia PC. */
  /* No implemente la excepció TLB !!! */
  new_PC= (COP0R12_SR&COP0_SR_BEV) ? 0x1FC00180 : 0x00000080;
  
} /* end exception */


// Simula un reset però sense llegir res de memòria.
static void
first_reset (void)
{

  COP0R14_EPC= PC;
  COP0R13_CAUSE&= ~COP0_CAUSE_BD;
  COP0R12_SR= (COP0R12_SR&0xFFFFFFC0) | ((COP0R12_SR&0xF)<<2) | 0x00000000;
  update_qflags ();
  COP0R13_CAUSE= (COP0R13_CAUSE&0xFFFFFF83);
  COP0R12_SR|= COP0_SR_BEV;
  new_PC= PC= 0x1FC00000;
  
} // end first_reset


// Torna cert en cas d'excepció.
static bool
check_interruptions (void)
{
  
  if ( !(COP0R12_SR&COP0_SR_IEC) ) return false;
  if ( COP0R12_SR&COP0R13_CAUSE&0x0000FF00 )
    {
      exception ( INTERRUPT_EXCP );
      PC= new_PC;
      return true;
    }

  return false;
  
} /* end check_interruptions */


/* Memòria ********************************************************************/
/* Torna true si s'ha llegit correctament. */
static bool
mem_read (
          const uint32_t  addr,
          uint32_t       *dst,
          const bool      read_data
          )
{
  
  if ( addr&0x3 ) goto error_addr;

  /* kuseg */
  if ( addr < 0x80000000 )
    {

      if ( _qflags.cache_isolated && read_data )
        {
          WW ( UDATA, "accedint a memòria en cau (%08x) amb la memòria"
               " cau aïllada. La memòria cau no està implementada.", addr );
        }

      if ( !_qflags.scratchpad_enabled &&
           (addr >= 0x1F800000 && addr <= 0x1F801000 ) )
        {
          WW ( UDATA, "accedint al scratchpad (%08x) amb este desactivat",
               addr );
          goto error_bus;
        }
      
      if ( !PSX_mem_read ( addr, dst ) ) goto error_bus;
      
    }

  /* ksge0. */
  else if ( addr < 0xA0000000 )
    {

      if ( _qflags.user_mode ) goto error_addr;
      
      if ( _qflags.cache_isolated && read_data )
        {
          WW ( UDATA, "accedint a memòria en cau (%08x) amb la memòria"
               " cau aïllada. La memòria cau no està implementada.", addr );
        }
      
      if ( !_qflags.scratchpad_enabled &&
           (addr >= 0x9F800000 && addr <= 0x9F801000 ) )
        {
          WW ( UDATA, "accedint al scratchpad (%08x) amb este desactivat",
               addr );
          goto error_bus;
        }

      if ( !PSX_mem_read ( addr&0x1FFFFFFF, dst ) ) goto error_bus;
      
    }

  /* kseg1. */
  else if ( addr < 0xC0000000 )
    {

      if ( _qflags.user_mode ) goto error_addr;
      
      if ( (addr >= 0xBF800000 && addr <= 0xBF801000 ) ) goto error_bus;
      
      if ( !PSX_mem_read ( addr&0x1FFFFFFF, dst ) ) goto error_bus;
      
    }

  /* Control de cache i basura. */
  else
    {

      if ( _qflags.user_mode ) goto error_addr;
      
      /* Control de cache. */
      if ( addr == 0xFFFE0130 )        *dst= CACHE_CONTROL&0x00000ABF;
      
      /* Basura. */
      else if ( (addr>=0xFFFE0000 && addr<0xFFFE0020) ||
        	(addr>=0xFFFE0100 && addr<0xFFFE0130) ||
        	(addr>=0xFFFE0132 && addr<0xFFFE0140) )
        *dst= 0;
      
      else goto error_bus;
      
    }
  
  return true;

 error_addr:
  EXCEPTION_ADDR ( ADDRESS_ERROR_LOAD_EXCP, addr );
  return false;
  
 error_bus:
  EXCEPTION ( read_data ? BUS_ERROR_DATA_EXCP : BUS_ERROR_INST_EXCP );
  return false;
  
} /* end mem_read */


/* Torna true si s'ha llegit correctament. */
static bool
mem_read16 (
            const uint32_t  addr,
            uint16_t       *dst,
            const bool      is_le
            )
{

  if ( addr&0x1 ) goto error_addr;

  /* kuseg */
  if ( addr < 0x80000000 )
    {

      if ( _qflags.cache_isolated )
        {
          WW ( UDATA, "accedint a memòria en cau (%08x) amb la memòria"
               " cau aïllada. La memòria cau no està implementada.", addr );
        }

      if ( !_qflags.scratchpad_enabled &&
           (addr >= 0x1F800000 && addr <= 0x1F801000 ) )
        {
          WW ( UDATA, "accedint al scratchpad (%08x) amb este desactivat",
               addr );
          goto error_bus;
        }
      
      if ( !PSX_mem_read16 ( addr, dst, is_le ) ) goto error_bus;
      
    }

  /* ksge0. */
  else if ( addr < 0xA0000000 )
    {

      if ( _qflags.user_mode ) goto error_addr;
      
      if ( _qflags.cache_isolated )
        {
          WW ( UDATA, "accedint a memòria en cau (%08x) amb la memòria"
               " cau aïllada. La memòria cau no està implementada.", addr );
        }
      
      if ( !_qflags.scratchpad_enabled &&
           (addr >= 0x9F800000 && addr <= 0x9F801000 ) )
        {
          WW ( UDATA, "accedint al scratchpad (%08x) amb este desactivat",
               addr );
          goto error_bus;
        }

      if ( !PSX_mem_read16 ( addr&0x1FFFFFFF, dst, is_le ) ) goto error_bus;
      
    }

  /* kseg1. */
  else if ( addr < 0xC0000000 )
    {

      if ( _qflags.user_mode ) goto error_addr;
      
      if ( (addr >= 0xBF800000 && addr <= 0xBF801000 ) ) goto error_bus;
      
      if ( !PSX_mem_read16 ( addr&0x1FFFFFFF, dst, is_le ) ) goto error_bus;
      
    }

  /* Control de cache i basura. */
  else
    {

      if ( _qflags.user_mode ) goto error_addr;
      
      /* Control de cache. */
      if ( (addr&0xFFFFFFFE) == 0xFFFE0130 )
        {
          switch ( (addr&0x1)^is_le )
            {
            case 0: *dst= 0; break;
            case 1: *dst= (uint16_t) (CACHE_CONTROL&0x00000ABF); break;
            }
        }
      
      /* Basura. */
      else if ( (addr>=0xFFFE0000 && addr<0xFFFE0020) ||
        	(addr>=0xFFFE0100 && addr<0xFFFE0130) ||
        	(addr>=0xFFFE0132 && addr<0xFFFE0140) )
        *dst= 0;
      
      else goto error_bus;
      
    }
  
  return true;

 error_addr:
  EXCEPTION_ADDR ( ADDRESS_ERROR_LOAD_EXCP, addr );
  return false;
  
 error_bus:
  EXCEPTION ( BUS_ERROR_DATA_EXCP );
  return false;
  
} /* end mem_read16 */


/* Torna true si s'ha llegit correctament. */
static bool
mem_read8 (
           const uint32_t  addr,
           uint8_t        *dst,
           const bool      is_le
           )
{

  /* kuseg */
  if ( addr < 0x80000000 )
    {
      
      if ( _qflags.cache_isolated )
        {
          WW ( UDATA, "accedint a memòria en cau (%08x) amb la memòria"
               " cau aïllada. La memòria cau no està implementada.", addr );
        }
      
      if ( !_qflags.scratchpad_enabled &&
           (addr >= 0x1F800000 && addr <= 0x1F801000 ) )
        {
          WW ( UDATA, "accedint al scratchpad (%08x) amb este desactivat",
               addr );
          goto error_bus;
        }
      
      if ( !PSX_mem_read8 ( addr, dst, is_le ) ) goto error_bus;
      
    }

  /* ksge0. */
  else if ( addr < 0xA0000000 )
    {

      if ( _qflags.user_mode ) goto error_addr;
      
      if ( _qflags.cache_isolated )
        {
          WW ( UDATA, "accedint a memòria en cau (%08x) amb la memòria"
               " cau aïllada. La memòria cau no està implementada.", addr );
        }
      
      if ( !_qflags.scratchpad_enabled &&
           (addr >= 0x9F800000 && addr <= 0x9F801000 ) )
        {
          WW ( UDATA, "accedint al scratchpad (%08x) amb este desactivat",
               addr );
          goto error_bus;
        }

      if ( !PSX_mem_read8 ( addr&0x1FFFFFFF, dst, is_le ) ) goto error_bus;
      
    }

  /* kseg1. */
  else if ( addr < 0xC0000000 )
    {

      if ( _qflags.user_mode ) goto error_addr;
      
      if ( (addr >= 0xBF800000 && addr <= 0xBF801000 ) ) goto error_bus;
      
      if ( !PSX_mem_read8 ( addr&0x1FFFFFFF, dst, is_le ) ) goto error_bus;
      
    }

  /* Control de cache i basura. */
  else
    {

      if ( _qflags.user_mode ) goto error_addr;
      
      /* Control de cache. */
      if ( (addr&0xFFFFFFFC) == 0xFFFE0130 )
        {
          switch ( (addr&0x3)^(is_le*0x3) )
            {
            case 0: *dst= 0; break;
            case 1: *dst= 0; break;
            case 2: *dst= (uint8_t) ((CACHE_CONTROL&0x00000ABF)>>8); break;
            case 3: *dst= (uint8_t) (CACHE_CONTROL&0x000000BF); break;
            }
        }
      
      /* Basura. */
      else if ( (addr>=0xFFFE0000 && addr<0xFFFE0020) ||
        	(addr>=0xFFFE0100 && addr<0xFFFE0130) ||
        	(addr>=0xFFFE0132 && addr<0xFFFE0140) )
        *dst= 0;
      
      else goto error_bus;
      
    }
  
  return true;

 error_addr:
  EXCEPTION_ADDR ( ADDRESS_ERROR_LOAD_EXCP, addr );
  return false;
  
 error_bus:
  EXCEPTION ( BUS_ERROR_DATA_EXCP );
  return false;
  
} /* end mem_read8 */


/* Torna true si s'ha llegit correctament. */
static bool
mem_write (
           const uint32_t addr,
           const uint32_t data
           )
{
  
  if ( addr&0x3 ) goto error_addr;
  
  /* kuseg */
  if ( addr < 0x80000000 )
    {
      
      if ( _qflags.cache_isolated )
        {
          /* HO EMPRA LA BIOS PER A INICIALITZAR A 0 LA CAU, NO CAL
             MOSTRAR MISSATGES !!!!
          WW ( UDATA, "accedint a memòria en cau (%08x) amb la memòria"
               " cau aïllada. La memòria cau no està implementada.", addr );
          */
          return true;
        }
      
      if ( !_qflags.scratchpad_enabled &&
           (addr >= 0x1F800000 && addr < 0x1F801000 ) )
        {
          WW ( UDATA, "accedint al scratchpad (%08x) amb este desactivat",
               addr );
          goto error_bus;
        }
      
      if ( !PSX_mem_write ( addr, data ) ) goto error_bus;
      
    }

  /* ksge0. */
  else if ( addr < 0xA0000000 )
    {

      if ( _qflags.user_mode ) goto error_addr;
      
      if ( _qflags.cache_isolated )
        {
          /* HO EMPRA LA BIOS PER A INICIALITZAR A 0 LA CAU, NO CAL
             MOSTRAR MISSATGES !!!!
          WW ( UDATA, "accedint a memòria en cau (%08x) amb la memòria"
               " cau aïllada. La memòria cau no està implementada.", addr );
          */
          return true;
        }
      
      if ( !_qflags.scratchpad_enabled &&
           (addr >= 0x9F800000 && addr < 0x9F801000 ) )
        {
          WW ( UDATA, "accedint al scratchpad (%08x) amb este desactivat",
               addr );
          goto error_bus;
        }

      if ( !PSX_mem_write ( addr&0x1FFFFFFF, data ) ) goto error_bus;
      
    }

  /* kseg1. */
  else if ( addr < 0xC0000000 )
    {

      if ( _qflags.user_mode ) goto error_addr;
      
      if ( (addr >= 0xBF800000 && addr <= 0xBF801000 ) ) goto error_bus;
      
      if ( !PSX_mem_write ( addr&0x1FFFFFFF, data ) ) goto error_bus;
      
    }

  /* Control de cache i basura. */
  else
    {
      
      if ( _qflags.user_mode ) goto error_addr;
      
      /* Control de cache. */
      if ( addr == 0xFFFE0130 )
        {
          CACHE_CONTROL= data&0x00000ABF;
          update_qflags ();
        }
      
      /* Basura. */
      else if ( (addr>=0xFFFE0000 && addr<0xFFFE0020) ||
        	(addr>=0xFFFE0100 && addr<0xFFFE0130) ||
        	(addr>=0xFFFE0132 && addr<0xFFFE0140) )
        return true;
      
      else goto error_bus;
      
    }
  
  return true;

 error_addr:
  EXCEPTION_ADDR ( ADDRESS_ERROR_STORE_EXCP, addr );
  return false;
  
 error_bus:
  EXCEPTION ( BUS_ERROR_DATA_EXCP );
  return false;
  
} /* end mem_write */


/* Torna true si s'ha llegit correctament. */
static bool
mem_write16 (
             const uint32_t addr,
             const uint16_t data,
             const bool     is_le
             )
{

  if ( addr&0x1 ) goto error_addr;
  
  /* kuseg */
  if ( addr < 0x80000000 )
    {

      if ( _qflags.cache_isolated )
        {
          /* HO EMPRA LA BIOS PER A INICIALITZAR A 0 LA CAU, NO CAL
             MOSTRAR MISSATGES !!!!
          WW ( UDATA, "accedint a memòria en cau (%08x) amb la memòria"
               " cau aïllada. La memòria cau no està implementada.", addr );
          */
          return true;
        }

      if ( !_qflags.scratchpad_enabled &&
           (addr >= 0x1F800000 && addr < 0x1F801000 ) )
        {
          WW ( UDATA, "accedint al scratchpad (%08x) amb este desactivat",
               addr );
          goto error_bus;
        }
      
      if ( !PSX_mem_write16 ( addr, data, is_le ) ) goto error_bus;
      
    }

  /* ksge0. */
  else if ( addr < 0xA0000000 )
    {

      if ( _qflags.user_mode ) goto error_addr;
      
      if ( _qflags.cache_isolated )
        {
          /* HO EMPRA LA BIOS PER A INICIALITZAR A 0 LA CAU, NO CAL
             MOSTRAR MISSATGES !!!!
          WW ( UDATA, "accedint a memòria en cau (%08x) amb la memòria"
               " cau aïllada. La memòria cau no està implementada.", addr );
          */
          return true;
        }
      
      if ( !_qflags.scratchpad_enabled &&
           (addr >= 0x9F800000 && addr < 0x9F801000 ) )
        {
          WW ( UDATA, "accedint al scratchpad (%08x) amb este desactivat",
               addr );
          goto error_bus;
        }

      if ( !PSX_mem_write16 ( addr&0x1FFFFFFF, data, is_le ) ) goto error_bus;
      
    }

  /* kseg1. */
  else if ( addr < 0xC0000000 )
    {

      if ( _qflags.user_mode ) goto error_addr;
      
      if ( (addr >= 0xBF800000 && addr <= 0xBF801000 ) ) goto error_bus;
      
      if ( !PSX_mem_write16 ( addr&0x1FFFFFFF, data, is_le ) ) goto error_bus;
      
    }

  /* Control de cache i basura. */
  else
    {

      if ( _qflags.user_mode ) goto error_addr;
      
      /* Control de cache. */
      if ( (addr&0xFFFFFFFE) == 0xFFFE0130 )
        {
          switch ( (addr&0x1)^is_le )
            {
            case 0: break;
            case 1:
              CACHE_CONTROL= (uint32_t) (data&0x00000ABF);
              update_qflags ();
              break;
            }
        }
      
      /* Basura. */
      else if ( (addr>=0xFFFE0000 && addr<0xFFFE0020) ||
        	(addr>=0xFFFE0100 && addr<0xFFFE0130) ||
        	(addr>=0xFFFE0132 && addr<0xFFFE0140) )
        return true;
      
      else goto error_bus;
      
    }
  
  return true;

 error_addr:
  EXCEPTION_ADDR ( ADDRESS_ERROR_STORE_EXCP, addr );
  return false;
  
 error_bus:
  EXCEPTION ( BUS_ERROR_DATA_EXCP );
  return false;
  
} /* end mem_write16 */


/* Torna true si s'ha llegit correctament. */
static bool
mem_write8 (
            const uint32_t addr,
            const uint8_t  data,
            const uint16_t data16,
            const bool     is_le
            )
{

  /* kuseg */
  if ( addr < 0x80000000 )
    {
      
      if ( _qflags.cache_isolated )
        {
          /* HO EMPRA LA BIOS PER A INICIALITZAR A 0 LA CAU, NO CAL
             MOSTRAR MISSATGES !!!!
          WW ( UDATA, "accedint a memòria en cau (%08x) amb la memòria"
               " cau aïllada. La memòria cau no està implementada.", addr );
          */
          return true;
        }
      
      if ( !_qflags.scratchpad_enabled &&
           (addr >= 0x1F800000 && addr < 0x1F801000 ) )
        {
          WW ( UDATA, "accedint al scratchpad (%08x) amb este desactivat",
               addr );
          goto error_bus;
        }
      
      if ( !PSX_mem_write8 ( addr, data, data16, is_le ) ) goto error_bus;
      
    }

  /* ksge0. */
  else if ( addr < 0xA0000000 )
    {

      if ( _qflags.user_mode ) goto error_addr;
      
      if ( _qflags.cache_isolated )
        {
          /* HO EMPRA LA BIOS PER A INICIALITZAR A 0 LA CAU, NO CAL
             MOSTRAR MISSATGES !!!!
          WW ( UDATA, "accedint a memòria en cau (%08x) amb la memòria"
               " cau aïllada. La memòria cau no està implementada.", addr );
          */
          return true;
        }
      
      if ( !_qflags.scratchpad_enabled &&
           (addr >= 0x9F800000 && addr < 0x9F801000 ) )
        {
          WW ( UDATA, "accedint al scratchpad (%08x) amb este desactivat",
               addr );
          goto error_bus;
        }

      if ( !PSX_mem_write8 ( addr&0x1FFFFFFF, data, data16, is_le ) )
        goto error_bus;
      
    }

  /* kseg1. */
  else if ( addr < 0xC0000000 )
    {

      if ( _qflags.user_mode ) goto error_addr;
      
      if ( (addr >= 0xBF800000 && addr <= 0xBF801000 ) ) goto error_bus;
      
      if ( !PSX_mem_write8 ( addr&0x1FFFFFFF, data, data16, is_le ) )
        goto error_bus;
      
    }

  /* Control de cache i basura. */
  else
    {

      if ( _qflags.user_mode ) goto error_addr;
      
      /* Control de cache. */
      if ( (addr&0xFFFFFFFC) == 0xFFFE0130 )
        {
          switch ( (addr&0x3)^(is_le*0x3) )
            {
            case 0: break;
            case 1: break;
            case 2:
              CACHE_CONTROL=
        	(CACHE_CONTROL&0xFFFF00FF) | (((uint32_t) (data&0x0A))<<8);
              update_qflags ();
              break;
            case 3:
              CACHE_CONTROL=
        	(CACHE_CONTROL&0xFFFFFF00) | ((uint32_t) (data&0xBF));
              update_qflags ();
              break;
            }
        }
      
      /* Basura. */
      else if ( (addr>=0xFFFE0000 && addr<0xFFFE0020) ||
        	(addr>=0xFFFE0100 && addr<0xFFFE0130) ||
        	(addr>=0xFFFE0132 && addr<0xFFFE0140) )
        return true;
      
      else goto error_bus;
      
    }
  
  return true;

 error_addr:
  EXCEPTION_ADDR ( ADDRESS_ERROR_STORE_EXCP, addr );
  return false;
  
 error_bus:
  EXCEPTION ( BUS_ERROR_DATA_EXCP );
  return false;
  
} /* end mem_write8 */


/* Decode *********************************************************************/
static void
decode_i_inst (void)
{

  RS= (_inst_word.v>>21)&0x1F;
  RT= (_inst_word.v>>16)&0x1F;
  IMMEDIATE= _inst_word.w.v0;
  
} /* end decode_i_inst */


static void
decode_j_inst (void)
{

  INSTR_INDEX= _inst_word.v&0x03FFFFFF;
  
} /* end decode_j_inst */


static void
decode_r_inst (void)
{

  RS= (_inst_word.v>>21)&0x1F;
  RT= (_inst_word.v>>16)&0x1F;
  RD= (_inst_word.v>>11)&0x1F;
  SA= (_inst_word.v>>6)&0x1F;
  FUNCTION= _inst_word.v&0x3F;
  
} /* end decode_r_inst */


static void
unk_inst (void)
{
  
  WW ( UDATA, "instrucció desconeguda: %02x", OPCODE );
  EXCEPTION ( RESERVED_INST_EXCP );
  
} /* end unk_inst */


#include "cpu_interpreter_cop0.h"
#include "cpu_interpreter_cop2.h"
#include "cpu_interpreter_inst1.h"


static void
unk_special_inst (void)
{
  
  WW ( UDATA, "instrucció SPECIAL desconeguda, funció: %02x", FUNCTION );
  EXCEPTION ( RESERVED_INST_EXCP );
  
} /* end unk_special_inst */


static void
special (void)
{
  
  decode_r_inst ();

  switch ( FUNCTION )
    {

    case 0x00: return sll ();

    case 0x02: return srl ();
    case 0x03: return sra ();
    case 0x04: return sllv ();

    case 0x06: return srlv ();
    case 0x07: return srav ();
    case 0x08: return jr ();
    case 0x09: return jalr ();

    case 0x0C: return syscall ();
    case 0x0D: return break_ ();

    case 0x10: return mfhi ();
    case 0x11: return mthi ();
    case 0x12: return mflo ();
    case 0x13: return mtlo ();

    case 0x18: return mult ();
    case 0x19: return multu ();
    case 0x1A: return div_ ();
    case 0x1B: return divu ();
      
    case 0x20: return add ();
    case 0x21: return addu ();
    case 0x22: return sub ();
    case 0x23: return subu ();
    case 0x24: return and ();
    case 0x25: return or ();
    case 0x26: return xor ();
    case 0x27: return nor ();

    case 0x2A: return slt ();
    case 0x2B: return sltu ();
      
    default: return  unk_special_inst ();
      
    }
  
} /* end special */


static void
unk_bcond_inst (void)
{
  
  WW ( UDATA, "instrucció BCOND desconeguda, funció: %02x", RT );
  EXCEPTION ( RESERVED_INST_EXCP );
  
} /* end unk_bcond_inst */


static void
bcond (void)
{

  decode_i_inst ();

  switch ( RT )
    {

    case 0x00: return bltz ();
    case 0x01: return bgez ();

    case 0x10: return bltzal ();
    case 0x11: return bgezal ();
      
    default: return  unk_bcond_inst ();
        
    }
  
} /* end bcond */


static int
exec_decoded_inst (void)
{

  int ret;

  
  ret= PSX_CYCLES_INST;
  
  switch ( OPCODE )
    {

    case 0x00: special (); break;
    case 0x01: bcond (); break;
    case 0x02: j (); break;
    case 0x03: jal (); break;
    case 0x04: beq (); break;
    case 0x05: bne (); break;
    case 0x06: blez (); break;
    case 0x07: bgtz (); break;
    case 0x08: addi (); break;
    case 0x09: addiu (); break;
    case 0x0A: slti (); break;
    case 0x0B: sltiu (); break;
    case 0x0C: andi (); break;
    case 0x0D: ori (); break;
    case 0x0E: xori (); break;
    case 0x0F: lui (); break;
    case 0x10: cop0 (); break;

    case 0x12: ret= cop2 (); break;
      
    case 0x20: lb (); break;
    case 0x21: lh (); break;
    case 0x22: lwl (); break;
    case 0x23: lw (); break;
    case 0x24: lbu (); break;
    case 0x25: lhu (); break;
    case 0x26: lwr (); break;

    case 0x28: sb (); break;
    case 0x29: sh (); break;
    case 0x2A: swl (); break;
    case 0x2B: sw (); break;

    case 0x2E: swr (); break;

    case 0x32: lwc2 (); break;

    case 0x3A: ret= swc2 (); break;
      
    default: unk_inst (); break;
      
    }

  return ret;
  
} /* end exec_decoded_inst */


static void
run_delayed_ops (void)
{
  
  /* Branch. */
  switch ( _branch.state )
    {
    case BRANCH_WAITING:
      _branch.state= BRANCH_READY;
      break;
    case BRANCH_READY:
      if ( _branch.cond ) new_PC= _branch.addr;
      _branch.state= BRANCH_EMPTY;
      --_delayed_ops;
      break;
    default: break;
    }
  
  /* Load delayed. */
  if ( _ldelayed.N > 0 ) update_ldelayed ();
  
  // Escritura cop0.
  if ( _cop0write.N > 0 ) update_cop0write ();
  
  // Escritura cop2.
  if ( _cop2write.N > 0 ) update_cop2write ();
  
} // end run_delayed_ops




/**********************/
/* FUNCIONS PÚBLIQUES */
/**********************/

int
PSX_cpu_next_inst (void)
{

  int ret;

  
  // Comprova excepció pendent (RFE).
  if ( _check_int )
    {
      _check_int= false;
      if ( check_interruptions () ) return PSX_CYCLES_INST;
    }
  
  // Decodifica la instrucció.
  mem_read ( PC, &_inst_word.v, false );
  new_PC= PC + 4;
  OPCODE= _inst_word.v>>26;
  
  // Executa.
  ret= exec_decoded_inst ();

  // Executa operacions 'delayed'.
  if ( _delayed_ops ) run_delayed_ops ();

  // Actualitza.
  PC= new_PC;

  return ret;
  
} /* end PSX_cpu_next_inst */


void
PSX_cpu_init (
              PSX_Warning *warning,
              void        *udata
              )
{

  /* Callbacks. */
  _warning= warning;
  _udata= udata;

  /* Registres. */
  PSX_cpu_init_regs ();
  new_PC= PC;
  
  /* Altres. */
  _delayed_ops= 0;
  _check_int= false;
  _branch.state= BRANCH_EMPTY;
  _ldelayed.N= 0;
  _cop0write.N= 0;
  _cop2write.N= 0;
  update_qflags ();

  // Reseteja.
  first_reset ();
  
} /* end PSX_cpu_init */


void
PSX_cpu_set_int (
                 const int  id,
                 const bool active
                 )
{

  // NOTA!! No estic simulant l'estat de les senyals d'entrada, perquè
  // el seu valor es veu en tot moment reflexat en COP0R13, per tant
  // seria redundant.
  
  uint32_t mask;

  
  mask= (1<<(10+id))&0x0000FC00;
  if ( active ) COP0R13_CAUSE|= mask;
  else          COP0R13_CAUSE&= ~mask;
  _check_int= true; // <-- IMPORTANT !!! Les interrupcions externes no
        	    // són culpa de les instruccions que al
        	    // executar-se permeten que es produisca una
        	    // interrupció pendent. Per tant al fer
        	    // _check_int= true en compte de
        	    // check_interruptions, l'excepció s'apuntarà a la
        	    // següent instrucció que està pendent
        	    // d'executar-se.
  
} // end PSX_cpu_set_int


void
PSX_cpu_reset (void)
{

  exception ( 0 );
  COP0R12_SR|= COP0_SR_BEV;
  new_PC= PC= 0x1FC00000;
  
} /* end PSX_cpu_reset */


void
PSX_cpu_update_state_interpreter (void)
{
  update_qflags ();
} /* end PSX_cpu_update_state_interpreter */

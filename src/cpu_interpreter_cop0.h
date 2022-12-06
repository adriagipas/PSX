/*
 * Copyright 2015-2022 Adrià Giménez Pastor.
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
 *  cpu_interpreter_cop0.h - Implementació instruccions COP0.
 *
 */


/********************************/
/* RFE - Restore from Exception */
/********************************/

static void
cop0_rfe (void)
{

  COP0R12_SR= (COP0R12_SR&0xFFFFFFF0) | ((COP0R12_SR&0x0000003C)>>2);
  update_qflags ();
  _check_int= true;
  
} /* end cop0_rfe */


/********************/
/* TLBP - TLB Probe */
/********************/

static void
cop0_tlbp (void)
{
  WW ( UDATA, "TLBP no implementada" );
} /* end cop0_tlbp */


/*********************************/
/* TLBR - Read Indexed TLB Entry */
/*********************************/

static void
cop0_tlbr (void)
{
  WW ( UDATA, "TLBR no implementada" );
} /* end cop0_tlbr */


/***********************************/
/* TLBWI - Write Indexed TLB Entry */
/***********************************/

static void
cop0_tlbwi (void)
{
  WW ( UDATA, "TLBWI no implementada" );
} /* end cop0_tlbwi */


/**********************************/
/* TLBWR - Write Random TLB Entry */
/**********************************/

static void
cop0_tlbwr (void)
{
  WW ( UDATA, "TLBWR no implementada" );
} /* end cop0_tlbwr */


/**********************************/
/* MFC0 - Move From Coprocessor 0 */
/**********************************/

static void
mfc0 (void)
{

  uint32_t val;

  
  switch ( RD )
    {
      
    case 3: /* BPC */
      val= COP0R3_BPC;
      break;

    case 5: /* BDA */
      val= COP0R5_BDA;
      break;

    case 6: /* JUMPDEST */
      val= 0;
      WW ( UDATA, "Funcionalitat de cop0:JUMPDEST no implementada" );
      break;

    case 7: /* DCIC */
      val= COP0R7_DCIC;
      WW ( UDATA, "Llegint de COP0.DCIC. Funcionalitat no implementada" );
      break;

    case 8: /* BadVaddr */
      val= COP0R8_BAD_VADDR;
      break;

    case 9: /* BDAM */
      val= COP0R9_BDAM;
      break;

    case 11: /* BPCM */
      val= COP0R11_BPCM;
      break;

    case 12: /* SR */
      val= COP0R12_SR&0xF27FFF3F;
      break;

    case 13: /* CAUSE */
      val= COP0R13_CAUSE&0xB000FF7C;
      break;

    case 14: /* EPC */
      val= COP0R14_EPC;
      break;

    case 15: /* PRID */
      val= 0x00000002; /* El que gasta Nocash PSX. */
      break;

    case 16: /* Basura */
    case 17:
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
    case 26:
    case 27:
    case 28:
    case 29:
    case 30:
    case 31:
      /* En realitat no és 0!! Però el comportament és extrany. */
      val= 0;
      break;
      
    default: /* Reserved. */
      EXCEPTION ( RESERVED_INST_EXCP );
      return;
    }

  if ( RT != 0 )
    {
      SET_LDELAYED ( RT, val );
    }
  
} /* end mfc0 */


/********************************/
/* MTC0 - Move To Coprocessor 0 */
/********************************/

static void
cop0_write_reg (
        	const int      reg,
        	const uint32_t val
        	)
{
  
  switch ( reg )
    {
      
    case 3: /* BPC */
      COP0R3_BPC= val;
      break;
      
    case 5: /* BDA */
      COP0R5_BDA= val;
      break;

    case 6: /* JUMPDEST */
      break;

    case 7: /* DCIC */
      //COP0R7_DCIC= val&0xFF80F03F;
      COP0R7_DCIC= val;
      WW ( UDATA,
           "Escrivint en COP0.DCIC=%08X. Funcionalitat no implementada",
           val );
      break;

    case 8: /* BadVaddr */
      break;

    case 9: /* BDAM */
      COP0R9_BDAM= val;
      break;

    case 11: /* BPCM */
      COP0R11_BPCM= val;
      break;

    case 12: /* SR */
      COP0R12_SR= val&0xF27FFF3F;
      update_qflags ();
      _check_int= true;
      //check_interruptions ();
      break;

    case 13: /* CAUSE */
      /* Es poden actualitzar 2 bits d'interrupcions. */
      COP0R13_CAUSE= (COP0R13_CAUSE&0xFFFFFCFF) | (val&0x00000300);
      _check_int= true;
      //check_interruptions ();
      break;

    case 14: /* EPC */
      break;

    case 15: /* PRID */
      break;

    case 16: /* Basura */
    case 17:
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
    case 26:
    case 27:
    case 28:
    case 29:
    case 30:
    case 31:
      break;
      
    default: /* Reserved. */
      EXCEPTION ( RESERVED_INST_EXCP );
      return;
    }
  
} /* end cop0_write_reg */


static void
mtc0 (void)
{
  SET_COP0WRITE ( RD, GPR[RT].v );
} /* end mtc0 */

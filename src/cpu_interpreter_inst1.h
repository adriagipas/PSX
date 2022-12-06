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
 *  cpu_interpreter_inst1.h - Primer conjunt d'instruccions implementat.
 *
 */


/******************/
/* ADD - Add Word */
/******************/

#define CHECK_OVERFLOW(RES,OP1,OP2,MBIT)        \
  ((~((OP1)^(OP2)))&((OP1)^(RES))&(MBIT))

#define CHECK_OVERFLOW32(RES,OP1,OP2)        	\
  CHECK_OVERFLOW(RES,OP1,OP2,0x80000000)

static void
add (void)
{

  uint32_t tmp;
  
  
  tmp= GPR[RS].v + GPR[RT].v;
  if ( CHECK_OVERFLOW32 ( tmp, GPR[RS].v, GPR[RT].v ) )
    {
      EXCEPTION ( INTEGER_OVERFLOW_EXCP );
    }
  else if ( RD != 0 ) { SET_REG ( RD, tmp ); }
  
} /* end add */


/*****************************/
/* ADDI - Add Immediate Word */
/*****************************/

#define SIGN_EXTEND16(U16)        		\
  ((uint32_t) ((int32_t) ((int16_t) (U16))))

static void
addi (void)
{
  
  uint32_t tmp, val;
  

  decode_i_inst ();
  
  val= SIGN_EXTEND16 ( IMMEDIATE );
  tmp= GPR[RS].v + val;
  if ( CHECK_OVERFLOW32 ( tmp, GPR[RS].v, val ) )
    {
      EXCEPTION ( INTEGER_OVERFLOW_EXCP );
    }
  else if ( RT != 0 ) { SET_REG ( RT, tmp ); }
  
} /* end addi */


/***************************************/
/* ADDIU - Add Immediate Unsigned Word */
/***************************************/

static void
addiu (void)
{
  
  decode_i_inst ();
  
  if ( RT == 0 ) return;
  SET_REG ( RT, GPR[RS].v + SIGN_EXTEND16 ( IMMEDIATE ) );
  
} /* end addiu */


/****************************/
/* ADDU - Add Unsigned Word */
/****************************/

static void
addu (void)
{
  
  if ( RD == 0 ) return;
  SET_REG ( RD, GPR[RS].v + GPR[RT].v );
  
} /* end addu */


/*************/
/* AND - And */
/*************/

static void
and (void)
{
  
  if ( RD == 0 ) return;
  SET_REG ( RD, GPR[RS].v & GPR[RT].v );
  
} /* end and */


/************************/
/* ANDI - And Immediate */
/************************/

static void
andi (void)
{
  
  decode_i_inst ();
  
  if ( RT == 0 ) return;
  SET_REG ( RT, GPR[RS].v & ((uint32_t) IMMEDIATE) );
  
} /* end andi */


/*************************/
/* BEQ - Branch on Equal */
/*************************/

#define SIGN_EXTEND18(U16)        			\
  ((uint32_t) (((int32_t) ((int16_t) (U16)))<<2))

static void
beq (void)
{

  uint32_t addr;

  
  decode_i_inst ();

  addr= new_PC + SIGN_EXTEND18 ( IMMEDIATE );
  SET_BRANCH ( addr, GPR[RS].v == GPR[RT].v );
  
} /* end beq */


/**************************************************/
/* BGEZ - Branch on Greater Than or Equal to Zero */
/**************************************************/

static void
bgez (void)
{

  uint32_t addr;


  addr= new_PC + SIGN_EXTEND18 ( IMMEDIATE );
  SET_BRANCH ( addr, ((int32_t) GPR[RS].v) >= 0 );
  
} /* end bgez */


/*************************************************************/
/* BGEZAL - Branch on Greater Than or Equal to Zero and Link */
/*************************************************************/

static void
bgezal (void)
{

  uint32_t addr;


  SET_REG ( 31, new_PC + 4 );
  addr= new_PC + SIGN_EXTEND18 ( IMMEDIATE );
  SET_BRANCH ( addr, ((int32_t) GPR[RS].v) >= 0 );
  
} /* end bgezal */


/**************************************/
/* BGTZ - Branch on Greater Than Zero */
/**************************************/

static void
bgtz (void)
{

  uint32_t addr;


  decode_i_inst ();
  
  addr= new_PC + SIGN_EXTEND18 ( IMMEDIATE );
  SET_BRANCH ( addr, ((int32_t) GPR[RS].v) > 0 );
  
} /* end bgtz */


/***********************************************/
/* BLEZ - Branch on Less Than or Equal to Zero */
/***********************************************/

static void
blez (void)
{

  uint32_t addr;


  decode_i_inst ();

  addr= new_PC + SIGN_EXTEND18 ( IMMEDIATE );
  SET_BRANCH ( addr, ((int32_t) GPR[RS].v) <= 0 );
  
} /* end blez */


/***********************************/
/* BLTZ - Branch on Less Than Zero */
/***********************************/

static void
bltz (void)
{
  
  uint32_t addr;
  
  
  addr= new_PC + SIGN_EXTEND18 ( IMMEDIATE );
  SET_BRANCH ( addr, ((int32_t) GPR[RS].v) < 0 );
  
} /* end bltz */


/**********************************************/
/* BLTZAL - Branch on Less Than Zero and Link */
/**********************************************/

static void
bltzal (void)
{
  
  uint32_t addr;
  

  SET_REG ( 31, new_PC + 4 );
  addr= new_PC + SIGN_EXTEND18 ( IMMEDIATE );
  SET_BRANCH ( addr, ((int32_t) GPR[RS].v) < 0 );
  
} /* end bltzal */


/*****************************/
/* BNE - Branch on Not Equal */
/*****************************/

static void
bne (void)
{
  
  uint32_t addr;
  
  
  decode_i_inst ();
  
  addr= new_PC + SIGN_EXTEND18 ( IMMEDIATE );
  SET_BRANCH ( addr, GPR[RS].v != GPR[RT].v );
  
} /* end bne */


/**********************/
/* BREAK - Breakpoint */
/**********************/

static void
break_ (void)
{
  EXCEPTION ( BREAKPOINT_EXCP );
} /* end break_ */


/*************************************************/
/* COP0 - Coprocessor Operation to Coprocessor 2 */
/*************************************************/

static void
cop0 (void)
{
  
  if ( !_qflags.cop0_enabled )
    {
      EXCEPTION_COP ( COP_UNUSABLE_EXCP, 0 );
      return;
    }
  
  decode_r_inst ();
  
  /* Coprocessor operation. */
  if ( RS&0x10 )
    switch ( FUNCTION )
      {
      case 0x01: return cop0_tlbr ();
      case 0x02: return cop0_tlbwi ();
      case 0x06: return cop0_tlbwr ();
      case 0x08: return cop0_tlbp ();
      case 0x10: return cop0_rfe ();
      default:
        WW ( UDATA, "instrucció COP0 desconeguda, cofunc: %02x", FUNCTION );
        EXCEPTION ( RESERVED_INST_EXCP );
      }
  else
    switch ( RS )
      {
      case 0x00: return mfc0 ();
      case 0x04: return mtc0 ();
      default:
        WW ( UDATA, "instrucció COP0 desconeguda, camp RS: %02x", RS );
        EXCEPTION ( RESERVED_INST_EXCP );
      }
  
} /* end cop0 */


/*************************************************/
/* COP2 - Coprocessor Operation to Coprocessor 2 */
/*************************************************/

static int
cop2 (void)
{

  int ret;


  ret= PSX_CYCLES_INST;
  
  if ( !_qflags.cop2_enabled )
    {
      EXCEPTION_COP ( COP_UNUSABLE_EXCP, 2 );
      return ret;
    }
  
  decode_r_inst ();
  
  /* Coprocessor operation. */
  if ( RS&0x10 ) ret= PSX_gte_execute ( _inst_word.v );
  else
    switch ( RS )
      {
      case 0x00: ret= mfc2 (); break;
      case 0x02: ret= cfc2 (); break;
      case 0x04: mtc2 (); break;
      case 0x06: ctc2 (); break;
      default:
        WW ( UDATA, "instrucció COP2 desconeguda, camp RS: %02x", RS );
        EXCEPTION ( RESERVED_INST_EXCP );
      }

  return ret;
  
} // end cop2


/*********************/
/* DIV - Divide Word */
/*********************/

static void
div_ (void)
{

  // El comportament en casos extrems ho he copiat de mednafen, la
  // documentació oficial no diu res.
  if ( GPR[RT].v == 0 )
    {
      if ( GPR[RS].v&0x80000000 ) LO= 1;
      else                        LO= 0xFFFFFFFF;
      HI= GPR[RS].v;
    }
  else if ( GPR[RS].v == 0x80000000 && GPR[RT].v == 0xFFFFFFFF )
    {
      LO= 0x80000000;
      HI= 0;
    }
  else
    {
      LO= (uint32_t) ((int32_t) GPR[RS].v / (int32_t) GPR[RT].v);
      HI= (uint32_t) ((int32_t) GPR[RS].v % (int32_t) GPR[RT].v);
    }
  
} // end div_


/*******************************/
/* DIVU - Divide Unsigned Word */
/*******************************/

static void
divu (void)
{

  // El comportament en casos extrems ho he copiat de mednafen, la
  // documentació oficial no diu res.
  if ( GPR[RT].v != 0 )
    {
      LO= GPR[RS].v / GPR[RT].v;
      HI= GPR[RS].v % GPR[RT].v;
    }
  else
    {
      LO= 0xFFFFFFFF;
      HI= GPR[RS].v;
    }
  
} // end divu


/************/
/* J - Jump */
/************/

static void
j (void)
{

  uint32_t addr;


  decode_j_inst ();
  
  addr= (new_PC&0xF0000000) | (INSTR_INDEX<<2);
  SET_BRANCH ( addr, true );
  
} /* end j */


/***********************/
/* JAL - Jump and Link */
/***********************/

static void
jal (void)
{

  uint32_t addr;
  
  
  decode_j_inst ();

  SET_REG ( 31, new_PC + 4 );
  addr= (new_PC&0xF0000000) | (INSTR_INDEX<<2);
  SET_BRANCH ( addr, true );
  
} /* end jal */


/*********************************/
/* JALR - Jump and Link Register */
/*********************************/

static void
jalr (void)
{

  if ( RD != 0 ) { SET_REG ( RD, new_PC + 4 ); }
  SET_BRANCH ( GPR[RS].v, true );
  
} /* end jalr */


/**********************/
/* JR - Jump Register */
/**********************/

static void
jr (void)
{
  SET_BRANCH ( GPR[RS].v, true );
} /* end jr */


/******************/
/* LB - Load Byte */
/******************/

#define SIGN_EXTEND8(U8)        		\
  ((uint32_t) ((int32_t) ((int8_t) (U8))))

static void
lb (void)
{

  uint8_t val;

  
  decode_i_inst ();
  
  if ( !mem_read8 ( GPR[RS].v + SIGN_EXTEND16 ( IMMEDIATE),
        	    &val, _qflags.is_le ) )
    return;
  if ( RT != 0 )
    {
      SET_LDELAYED ( RT, SIGN_EXTEND8 ( val ) );
    }
  
} /* end lb */


/****************************/
/* LBU - Load Byte Unsigned */
/****************************/

static void
lbu (void)
{

  uint8_t val;
  
  
  decode_i_inst ();
  
  if ( !mem_read8 ( GPR[RS].v + SIGN_EXTEND16 ( IMMEDIATE),
        	    &val, _qflags.is_le ) )
    return;
  if ( RT != 0 )
    {
      SET_LDELAYED ( RT, (uint32_t) val );
    }
  
} /* end lbu */


/**********************/
/* LH - Load Halfword */
/**********************/

static void
lh (void)
{

  uint16_t val;
  
  
  decode_i_inst ();
  
  if ( !mem_read16 ( GPR[RS].v + SIGN_EXTEND16 ( IMMEDIATE ),
        	     &val, _qflags.is_le ) )
    return;
  if ( RT != 0 )
    {
      SET_LDELAYED ( RT, SIGN_EXTEND16 ( val ) );
    }
  
} /* end lh */


/********************************/
/* LHU - Load Halfword Unsigned */
/********************************/

static void
lhu (void)
{

  uint16_t val;
  
  
  decode_i_inst ();
  
  if ( !mem_read16 ( GPR[RS].v + SIGN_EXTEND16 ( IMMEDIATE ),
        	     &val, _qflags.is_le ) )
    return;
  if ( RT != 0 )
    {
      SET_LDELAYED ( RT, (uint32_t) val );
    }
  
} /* end lhu */


/******************************/
/* LUI - Load Upper Immediate */
/******************************/

static void
lui (void)
{

  decode_i_inst ();

  if ( RT == 0 ) return;
  SET_REG ( RT, ((uint32_t) IMMEDIATE)<<16 );
  
} /* end lui */


/******************/
/* LW - Load Word */
/******************/

static void
lw (void)
{

  uint32_t val;
  
  
  decode_i_inst ();
  
  if ( !mem_read ( GPR[RS].v + SIGN_EXTEND16 ( IMMEDIATE),
        	   &val, true ) )
    return;
  if ( RT != 0 )
    {
      SET_LDELAYED ( RT, val );
    }
  
} /* end lw */


/*************************************/
/* LWC2 - Load Word To Coprocessor 2 */
/*************************************/

static void
lwc2 (void)
{

  uint32_t val;

  
  if ( !_qflags.cop2_enabled )
    {
      EXCEPTION_COP ( COP_UNUSABLE_EXCP, 2 );
      return;
    }
  
  decode_i_inst ();
  
  if ( !mem_read ( GPR[RS].v + SIGN_EXTEND16 ( IMMEDIATE),
        	   &val, true ) )
    return;
  SET_COP2WRITE ( RT, val );
  
} /* end lwc2 */


/************************/
/* LWL - Load Word Left */
/************************/

static void
lwl (void)
{

  static const lwlr_op_t OPVALS[2][4]=
    {
     // Big-endian
     {{0, 0x00000000},
      {8, 0x000000FF},
      {16,0x0000FFFF},
      {24,0x00FFFFFF}},
     // Little-endian
     {{24,0x00FFFFFF},
      {16,0x0000FFFF},
      {8, 0x000000FF},
      {0, 0x00000000}}
    };
  
  uint32_t val, addr, new_val;
  lwlr_op_t op;
  
  
  decode_i_inst ();

  addr= GPR[RS].v + SIGN_EXTEND16 ( IMMEDIATE);
  if ( !mem_read ( addr&0xFFFFFFFC, &val, true ) )
    return;
  if ( RT != 0 )
    {
      new_val= GET_LWLR_REG_VAL ( RT );
      op= OPVALS[_qflags.is_le][addr&0x3];
      new_val= (val<<op.shift) | (new_val&op.mask);
      SET_LDELAYED_LWLR ( RT, new_val );
    }
  
} /* end lwl */


/*************************/
/* LWR - Load Word Right */
/*************************/

static void
lwr (void)
{

  static const lwlr_op_t OPVALS[2][4]=
    {
     // Big-endian
     {{24,0xFFFFFF00},
      {16,0xFFFF0000},
      {8, 0xFF000000},
      {0, 0x00000000}},
     // Little-endian
     {{0, 0x00000000},
      {8, 0xFF000000},
      {16,0xFFFF0000},
      {24,0xFFFFFF00}}
    };
  
  uint32_t val, addr, new_val;
  lwlr_op_t op;
  
  
  decode_i_inst ();

  addr= GPR[RS].v + SIGN_EXTEND16 ( IMMEDIATE);
  if ( !mem_read ( addr&0xFFFFFFFC, &val, true ) )
    return;
  if ( RT != 0 )
    {
      new_val= GET_LWLR_REG_VAL ( RT );
      op= OPVALS[_qflags.is_le][addr&0x3];
      new_val= (val>>op.shift) | (new_val&op.mask);
      SET_LDELAYED_LWLR ( RT, new_val );
    }
  
} /* end lwr */


/***********************/
/* MFHI - Move From HI */
/***********************/

static void
mfhi (void)
{

  if ( RD == 0 ) return;
  SET_REG ( RD, HI );
  
} /* end mfhi */


/***********************/
/* MFLO - Move From Lo */
/***********************/

static void
mflo (void)
{

  if ( RD == 0 ) return;
  SET_REG ( RD, LO );
  
} /* end mflo */


/*********************/
/* MTHI - Move To HI */
/*********************/

static void
mthi (void)
{
  HI= GPR[RS].v;
} /* end mthi */


/*********************/
/* MTLO - Move To LO */
/*********************/

static void
mtlo (void)
{
  LO= GPR[RS].v;
} /* end mtlo */


/************************/
/* MULT - Multiply Word */
/************************/

#define SIGN_EXTEND32(U32)        		\
  ((uint64_t) ((int64_t) ((int32_t) (U32))))

static void
mult (void)
{

  uint64_t tmp;

  
  tmp=
    (uint64_t) (((int64_t) SIGN_EXTEND32 ( GPR[RS].v )) *
        	((int64_t) SIGN_EXTEND32 ( GPR[RT].v )));
  LO= (uint32_t) (tmp&0xFFFFFFFF);
  HI= (uint32_t) (tmp>>32);
  
} /* end mult */


/**********************************/
/* MULTU - Multiply Unsigned Word */
/**********************************/

static void
multu (void)
{

  uint64_t tmp;
  
  
  tmp= ((uint64_t) GPR[RS].v) * ((uint64_t) GPR[RT].v);
  LO= (uint32_t) (tmp&0xFFFFFFFF);
  HI= (uint32_t) (tmp>>32);
  
} /* end multu */


/*************/
/* NOR - Nor */
/*************/

static void
nor (void)
{

  if ( RD == 0 ) return;
  SET_REG ( RD, ~(GPR[RS].v | GPR[RT].v) );
  
} /* end nor */


/***********/
/* OR - Or */
/***********/

static void
or (void)
{

  if ( RD == 0 ) return;
  SET_REG ( RD, (GPR[RS].v | GPR[RT].v) );
  
} /* end or */


/**********************/
/* ORI - Or Immediate */
/**********************/

static void
ori (void)
{
  
  decode_i_inst ();
  
  if ( RT == 0 ) return;
  SET_REG ( RT, GPR[RS].v | ((uint32_t) IMMEDIATE) );
  
} /* end ori */


/*******************/
/* SB - Store Byte */
/*******************/

static void
sb (void)
{

  decode_i_inst ();

  mem_write8 ( GPR[RS].v + SIGN_EXTEND16 ( IMMEDIATE ),
               GPR[RT].b.v0, GPR[RT].w.v0, _qflags.is_le );
  
} /* end sb */


/***********************/
/* SH - Store Halfword */
/***********************/

static void
sh (void)
{

  decode_i_inst ();
  
  mem_write16 ( GPR[RS].v + SIGN_EXTEND16 ( IMMEDIATE ),
        	GPR[RT].w.v0, _qflags.is_le );
  
} /* end sh */


/*********************************/
/* SLL - Shift Word Left Logical */
/*********************************/

static void
sll (void)
{

  if ( RD == 0 ) return;
  SET_REG ( RD, GPR[RT].v<<SA );
  
} /* end sll */


/*******************************************/
/* SLLV - Shift Word Left Logical Variable */
/*******************************************/

static void
sllv (void)
{

  if ( RD == 0 ) return;
  SET_REG ( RD, GPR[RT].v<<(GPR[RS].v&0x1F) );
  
} /* end sllv */


/**************************/
/* SLT - Set On Less Than */
/**************************/

static void
slt (void)
{
  
  if ( RD == 0 ) return;
  SET_REG ( RD, (((int32_t) GPR[RS].v) < ((int32_t) GPR[RT].v)) );
  
} /* end slt */


/*************************************/
/* SLTI - Set On Less Than Immediate */
/*************************************/

static void
slti (void)
{

  decode_i_inst ();

  if ( RT == 0 ) return;
  SET_REG ( RT,
            (((int32_t) GPR[RS].v) < ((int32_t) SIGN_EXTEND16 ( IMMEDIATE ))) );
  
} /* end slti */


/***********************************************/
/* SLTIU - Set On Less Than Immediate Unsigned */
/***********************************************/

static void
sltiu (void)
{

  decode_i_inst ();

  if ( RT == 0 ) return;
  SET_REG ( RT, (GPR[RS].v < SIGN_EXTEND16 ( IMMEDIATE )) );
  
} /* end sltiu */


/************************************/
/* SLTU - Set On Less Than Unsigned */
/************************************/

static void
sltu (void)
{

  if ( RD == 0 ) return;
  SET_REG ( RD, (GPR[RS].v < GPR[RT].v) );
  
} /* end sltu */


/*************************************/
/* SRA - Shift Word Right Arithmetic */
/*************************************/

static void
sra (void)
{

  if ( RD == 0 ) return;
  SET_REG ( RD, (uint32_t) (((int32_t) GPR[RT].v)>>SA) );
  
} /* end sra */


/***********************************************/
/* SRAV - Shift Word Right Arithmetic Variable */
/***********************************************/

static void
srav (void)
{

  if ( RD == 0 ) return;
  SET_REG ( RD, (uint32_t) (((int32_t) GPR[RT].v)>>(GPR[RS].v&0x1F)) );
  
} /* srav */


/**********************************/
/* SRL - Shift Word Right Logical */
/**********************************/

static void
srl (void)
{

  if ( RD == 0 ) return;
  SET_REG ( RD, GPR[RT].v>>SA );
  
} /* end srl */


/********************************************/
/* SRLV - Shift Word Right Logical Variable */
/********************************************/

static void
srlv (void)
{

  if ( RD == 0 ) return;
  SET_REG ( RD, GPR[RT].v>>(GPR[RS].v&0x1F) );
  
} /* end srlv */


/***********************/
/* SUB - Subtract Word */
/***********************/

static void
sub (void)
{

  uint32_t tmp, op2;
  

  op2= ~(GPR[RT].v); /* Complement a 1. */
  tmp= GPR[RS].v + op2 + 1;
  if ( CHECK_OVERFLOW32 ( tmp, GPR[RS].v, op2 ) )
    {
      EXCEPTION ( INTEGER_OVERFLOW_EXCP );
    }
  else if ( RD != 0 ) { SET_REG ( RD, tmp ); }
  
} /* end sub */


/*********************************/
/* SUBU - Subtract Unsigned Word */
/*********************************/

static void
subu (void)
{
  
  if ( RD == 0 ) return;
  SET_REG ( RD, GPR[RS].v - GPR[RT].v );
  
} /* end subu */


/*******************/
/* SW - Store Word */
/*******************/

static void
sw (void)
{
  
  decode_i_inst ();
  mem_write ( GPR[RS].v + SIGN_EXTEND16 ( IMMEDIATE ), GPR[RT].v );
  
} /* end sw */


/****************************************/
/* SWC2 - Store Word From Coprocessor 2 */
/****************************************/

static int
swc2 (void)
{

  uint32_t val;
  int ret;
  
  
  if ( !_qflags.cop2_enabled )
    {
      EXCEPTION_COP ( COP_UNUSABLE_EXCP, 2 );
      return PSX_CYCLES_INST;
    }
  
  decode_i_inst ();
  
  ret= PSX_gte_read ( RT, &val );
  mem_write ( GPR[RS].v + SIGN_EXTEND16 ( IMMEDIATE ), val );
  
  return ret;
  
} // end swc2


/*************************/
/* SWL - Store Word Left */
/*************************/

static void
swl (void)
{

  static const lwlr_op_t OPVALS[2][4]=
    {
     // Big-endian
     {{0, 0x00000000},
      {8, 0xFF000000},
      {16,0xFFFF0000},
      {24,0xFFFFFF00}},
     // Little-endian
     {{24,0xFFFFFF00},
      {16,0xFFFF0000},
      {8, 0xFF000000},
      {0, 0x00000000}}
    };
  
  uint32_t val, addr, new_val;
  lwlr_op_t op;

  
  decode_i_inst ();

  addr= GPR[RS].v + SIGN_EXTEND16 ( IMMEDIATE);
  if ( !mem_read ( addr&0xFFFFFFFC, &val, true ) )
    return;
  op= OPVALS[_qflags.is_le][addr&0x3];
  new_val= (GPR[RT].v>>op.shift) | (val&op.mask);
  mem_write ( addr&0xFFFFFFFC, new_val );
  
} /* end swl */


/**************************/
/* SWR - Store Word Right */
/**************************/

static void
swr (void)
{

  static const lwlr_op_t OPVALS[2][4]=
    {
     // Big-endian
     {{24,0x00FFFFFF},
      {16,0x0000FFFF},
      {8, 0x000000FF},
      {0, 0x00000000}},
     // Little-endian
     {{0, 0x00000000},
      {8, 0x000000FF},
      {16,0x0000FFFF},
      {24,0x00FFFFFF}}
    };
  
  uint32_t val, addr, new_val;
  lwlr_op_t op;
  
  
  decode_i_inst ();

  addr= GPR[RS].v + SIGN_EXTEND16 ( IMMEDIATE );
  if ( !mem_read ( addr&0xFFFFFFFC, &val, true ) )
    return;
  op= OPVALS[_qflags.is_le][addr&0x3];
  new_val= (GPR[RT].v<<op.shift) | (val&op.mask);
  mem_write ( addr&0xFFFFFFFC, new_val );
  
} /* end swr */


/*************************/
/* SYSCALL - System Call */
/*************************/

static void
syscall (void)
{
  EXCEPTION ( SYSTEM_CALL_EXCP );
} /* end syscall */


/**********************/
/* XOR - Exclusive Or */
/**********************/

static void
xor (void)
{

  if ( RD == 0 ) return;
  SET_REG ( RD, GPR[RS].v ^ GPR[RT].v );
  
} /* end xor */


/*********************************/
/* XORI - Exclusive OR Immediate */
/*********************************/

static void
xori (void)
{

  decode_i_inst ();
  
  if ( RT == 0 ) return;
  SET_REG ( RT, GPR[RS].v ^ ((uint32_t) IMMEDIATE) );
  
} /* end xori */

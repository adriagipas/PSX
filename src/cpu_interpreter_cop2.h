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
 *  cpu_interpreter_cop2.h - Implementació instruccions COP2.
 *
 */


/***********************************************/
/* CFC2 - Move Control Word From Coprocessor 2 */
/***********************************************/

static int
cfc2 (void)
{

  uint32_t data;
  int ret;
  
  
  ret= PSX_gte_read ( RD+32, &data );
  if ( RT != 0 )
    {
      SET_LDELAYED ( RT, data );
    }

  return ret;
  
} /* end cfc2 */


/*********************************************/
/* CTC2 - Move Control Word to Coprocessor 2 */
/*********************************************/

static void
ctc2 (void)
{
  SET_COP2WRITE ( RD+32, GPR[RT].v );
} /* end ctc2 */


/**********************************/
/* MFC2 - Move From Coprocessor 2 */
/**********************************/

static int
mfc2 (void)
{

  uint32_t data;
  int ret;
  

  ret= PSX_gte_read ( RD, &data );
  if ( RT != 0 )
    {
      SET_LDELAYED ( RT, data );
    }

  return ret;
  
} /* end mfc2 */


/********************************/
/* MTC2 - Move To Coprocessor 2 */
/********************************/

static void
mtc2 (void)
{
  SET_COP2WRITE ( RD, GPR[RT].v );
} /* end mtc2 */

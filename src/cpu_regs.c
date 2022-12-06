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
 *  cpu_regs.c - Estat de la CPU compartida per les diferents
 *               implementacions.
 *
 */


#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "PSX.h"




/****************/
/* ESTAT PÚBLIC */
/****************/

PSX_CPU PSX_cpu_regs;




/**********************/
/* FUNCIONS PÚBLIQUES */
/**********************/

void
PSX_cpu_init_regs (void)
{
  memset ( &PSX_cpu_regs, 0, sizeof(PSX_cpu_regs) );
} /* end PSX_cpu_init_regs */

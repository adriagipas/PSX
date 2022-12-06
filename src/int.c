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
 *  int.c - Implementa el mòdul d'interrupcions.
 *
 */


#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "PSX.h"




/**********/
/* MACROS */
/**********/

#define UPDATE_CPU_INT        					\
  PSX_cpu_interruption ( 0, (_i_stat&_i_mask)!=0 )




/*********/
/* ESTAT */
/*********/

/* Estat i màscara. */
static uint32_t _i_stat;
static uint32_t _i_mask;

// Trace.
static void *_udata;
static PSX_IntTrace *_int;

// Clock.
//static int _last_clock;

static void (*_interruption) (const PSX_Interruption flag);
static void (*_int_ack) (const uint32_t data);



/*********************/
/* FUNCIONS PRIVADES */
/*********************/

static void
interruption (
              const PSX_Interruption flag
              )
{
  
  _i_stat|= flag;
  UPDATE_CPU_INT;
  
} // end interruption


static void
interruption_trace (
        	    const PSX_Interruption flag
        	    )
{

  uint32_t old_i_stat,i_mask;
  

  old_i_stat= _i_stat;
  i_mask= _i_mask;
  interruption ( flag );
  _int ( false, old_i_stat, _i_stat, i_mask, _udata );
  
} // end interruption_trace


static void
int_ack (
         const uint32_t data
         )
{
  
  _i_stat&= data;
  UPDATE_CPU_INT;
  
} // end int_ack


static void
int_ack_trace (
               const uint32_t data
               )
{
  uint32_t old_i_stat,i_mask;
  

  old_i_stat= _i_stat;
  i_mask= _i_mask;
  int_ack ( data );
  _int ( true, old_i_stat, _i_stat, i_mask, _udata );
  
} // end int_ack_trace

#if 0
static void
clock (void)
{

  _last_clock= PSX_Clock;
  PSX_gpu_clock ( false );
  PSX_cd_clock ( false );
  PSX_spu_clock ( false );
  PSX_joy_clock ( false );
  PSX_timers_clock ( false );
  
} // end clock
#endif




/**********************/
/* FUNCIONS PÚBLIQUES */
/**********************/

void
PSX_int_init (
              PSX_IntTrace *int_trace,
              void         *udata
              )
{

  // Callbacks.
  _udata= udata;
  _int= int_trace;
  _interruption= interruption;
  _int_ack= int_ack;
  
  // Inicialitza.
  _i_stat= _i_mask= 0;
  //_last_clock= 0;
  
} // end PSX_int_init

/*
void
PSX_int_end_iter (void)
{
  _last_clock= 0;
} // end PSX_int_end_iter
*/

void
PSX_int_interruption (
        	      const PSX_Interruption flag
        	      )
{
  _interruption ( flag );
} // end PSX_int_interruption


uint32_t
PSX_int_read_state (void)
{

  return _i_stat | 0x1F800000;
  
} // end PSX_int_read_state


void
PSX_int_ack (
             const uint32_t data
             )
{

  _int_ack ( data );
  
} // end PSX_int_ack


uint32_t
PSX_int_read_imask (void)
{

  return _i_mask | 0x1F800000;
  
} // end PSX_int_read_imask


void
PSX_int_write_imask (
        	     const uint32_t data
        	     )
{

  _i_mask= data&0xFFFF07FF;
  UPDATE_CPU_INT;
  
} // end PSX_int_write_imask


void
PSX_int_set_mode_trace (
        		bool enable
        		)
{

  if ( _int != NULL && enable )
    {
      _interruption= interruption_trace;
      _int_ack= int_ack_trace;
    }
  else
    {
      _interruption= interruption;
      _int_ack= int_ack;
    }
  
} // end PSX_int_set_mode_trace

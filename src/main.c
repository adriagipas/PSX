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
 *  main.c - Implementació del mòdul principal.
 *
 */


#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "PSX.h"




/*********/
/* ESTAT */
/*********/

/* Senyals. */
static bool _reset;

/* Frontend. */
static PSX_CheckSignals *_check;
static PSX_Warning *_warning;
static void *_udata;

/* Callbacks. */
static PSX_CPUInst *_cpu_inst;




/*********************/
/* FUNCIONS PRIVADES */
/*********************/

static void
reset (void)
{
  
  PSX_cd_reset ();
  PSX_gpu_reset ();
  PSX_mdec_reset ();
  PSX_spu_reset ();
  PSX_dma_reset ();
  PSX_cpu_reset (); // Important que siga l'últim.
  _reset= false;
  
} // end reset




/***********************/
/* VARIABLES PÚBLIQUES */
/***********************/

int PSX_Clock;
int PSX_NextEventCC;
PSX_BusOwnerType PSX_BusOwner;




/**********************/
/* FUNCIONS PÚBLIQUES */
/**********************/

void
PSX_init (
          const uint8_t bios[PSX_BIOS_SIZE],
          const PSX_Frontend *frontend,       // Frontend.
          void               *udata,          // Dades frontend.
          PSX_Renderer       *renderer
          )
{

  _reset= false;
  
  // Callbacks.
  _check= frontend->check;
  _warning= frontend->warning;
  _udata= udata;

  // Tracer.
  _cpu_inst= frontend->trace!=NULL ?
    frontend->trace->cpu_inst : NULL;

  // Clock.
  PSX_Clock= 0;
  PSX_NextEventCC= INT_MAX;
  PSX_BusOwner= PSX_BUS_OWNER_CPU;
  
  // Mòduls.
  PSX_cpu_init ( frontend->warning, udata );
  PSX_gte_init ( frontend->warning,
        	 frontend->trace!=NULL?frontend->trace->gte_cmd_trace:NULL,
        	 frontend->trace!=NULL?frontend->trace->gte_mem_access:NULL,
        	 udata );
  PSX_mem_init ( bios,
        	 frontend->trace!=NULL?frontend->trace->mem_changed:NULL,
        	 frontend->trace!=NULL?frontend->trace->mem_access:NULL,
        	 frontend->trace!=NULL?frontend->trace->mem_access16:NULL,
        	 frontend->trace!=NULL?frontend->trace->mem_access8:NULL,
        	 udata );
  PSX_int_init ( frontend->trace!=NULL?frontend->trace->int_trace:NULL, udata );
  PSX_dma_init ( frontend->trace!=NULL?frontend->trace->dma_transfer:NULL,
        	 frontend->warning, udata );
  PSX_mdec_init ( frontend->warning, udata );
  PSX_timers_init ();
  PSX_gpu_init ( renderer,
        	 frontend->trace!=NULL?frontend->trace->gpu_cmd:NULL,
        	 frontend->warning,
        	 udata );
  PSX_cd_init ( frontend->trace!=NULL?frontend->trace->cd_cmd:NULL,
        	frontend->warning, udata );
  PSX_spu_init ( frontend->play_sound, frontend->warning, udata );
  PSX_joy_init ( frontend->warning,
        	 frontend->get_ctrl_state,
        	 udata );

} // end PSX_init


int
PSX_iter (
          const int  cc,
          bool      *stop
          )
{

  /* NOTA!!! L'esquema de clock(end_iter) està relacionat en executar
   * un bloc d'instruccions seguit i que es puguen produir events en
   * eixe bloc. Les interrupcions són molt sensibles a això. Per
   * aquest motiu 'INT' clockeja tots els components abans
   * d'actualitzar les mascares i altres valors. No obstant açò amb la
   * nova aproximació de parar abans de cada event en realitat ja no
   * és necessaria que 'INT' faça res. Sí que és cert, que si en el
   * futur la únitat mínima deixara de ser la instrucció i fora un
   * bloc d'instruccions tornaria a ser necessari. Per aquest motiu he
   * decidit comentar l'esquema clock(end_iter), per si de cas en el
   * futur ho recupere.
   */
  
  int cc_remain,cc_total,tmp;

  
  cc_remain= cc;
  cc_total= 0;
  while ( cc_remain > 0 )
    {

      // Inicialitza iteració.
      PSX_NextEventCC= cc_remain;
      // NOTA!! GTE no genera events.
      tmp= PSX_dma_next_event_cc ();
      if ( tmp != -1 && tmp < PSX_NextEventCC ) PSX_NextEventCC= tmp;
      tmp= PSX_mdec_next_event_cc ();
      if ( tmp < PSX_NextEventCC ) PSX_NextEventCC= tmp;
      tmp= PSX_gpu_next_event_cc ();
      if ( tmp != -1 && tmp < PSX_NextEventCC ) PSX_NextEventCC= tmp;
      tmp= PSX_cd_next_event_cc ();
      if ( tmp < PSX_NextEventCC ) PSX_NextEventCC= tmp;
      tmp= PSX_spu_next_event_cc ();
      if ( tmp < PSX_NextEventCC ) PSX_NextEventCC= tmp;
      tmp= PSX_joy_next_event_cc ();
      if ( tmp < PSX_NextEventCC ) PSX_NextEventCC= tmp;
      tmp= PSX_timers_next_event_cc ();
      if ( tmp < PSX_NextEventCC ) PSX_NextEventCC= tmp;
      PSX_Clock= 0;

      // Itera tot els que es puga.
      // NOTA!! Qualsevol actualització de PSX_NextEventCC s'ha de fer
      // tenint en compte que en aquesta iteració en portem PSX_Clock
      // cicles executats.
      do {
        switch ( PSX_BusOwner )
          {
          case PSX_BUS_OWNER_CPU:
            PSX_Clock+= tmp= PSX_cpu_next_inst ();
            break;
          case PSX_BUS_OWNER_DMA:
            PSX_Clock+= tmp= PSX_dma_run ();
            break;
          case PSX_BUS_OWNER_CPU_DMA:
            PSX_Clock+= tmp= PSX_cpu_next_inst ();
            PSX_dma_run_cc ( tmp );
            break;
          }
      } while ( PSX_Clock < PSX_NextEventCC );
      
      // Consumeix cicles pendets (executa possible event)
      PSX_dma_end_iter ();
      PSX_gte_end_iter ();
      PSX_mdec_end_iter ();
      PSX_gpu_end_iter ();
      PSX_cd_end_iter ();
      PSX_spu_end_iter ();
      PSX_joy_end_iter ();
      PSX_timers_end_iter ();

      // Prepara següent iteració.
      cc_total+= PSX_Clock;
      cc_remain-= PSX_Clock;
      
    }

  // Senyals externes
  if ( _check != NULL )
    {
      _check ( stop, &_reset, _udata );
      if ( _reset ) reset ();
    }
  
  return cc_total;
  
} // end PSX_iter


void
PSX_reset (void)
{
  _reset= true;
} // end PSX_reset


int
PSX_trace (void)
{
  
  PSX_Inst inst;
  uint32_t addr;
  int ret;
  

  if ( PSX_BusOwner == PSX_BUS_OWNER_CPU ||
       PSX_BusOwner == PSX_BUS_OWNER_CPU_DMA )
    {
      if ( _cpu_inst != NULL )
        {
          addr= PSX_cpu_regs.pc;
          if ( PSX_cpu_decode ( addr, &inst ) )
            _cpu_inst ( &inst, addr, _udata );
        }
    }

  // Inicialitza traça
  PSX_mem_set_mode_trace ( true );
  PSX_gte_set_mode_trace ( true );
  PSX_gpu_set_mode_trace ( true );
  PSX_cd_set_mode_trace ( true );
  PSX_dma_set_mode_trace ( true );
  PSX_int_set_mode_trace ( true );

  // Inicialitza iteració.
  PSX_NextEventCC= 1;
  switch ( PSX_BusOwner )
    {
    case PSX_BUS_OWNER_CPU:
      PSX_Clock= PSX_cpu_next_inst ();
      break;
    case PSX_BUS_OWNER_DMA:
      PSX_Clock= PSX_dma_run ();
      break;
    case PSX_BUS_OWNER_CPU_DMA:
      PSX_Clock= PSX_cpu_next_inst ();
      PSX_dma_run_cc ( PSX_Clock );
      break;
    }
  
  // End iter
  PSX_dma_end_iter ();
  PSX_gte_end_iter ();
  PSX_mdec_end_iter ();
  PSX_gpu_end_iter ();
  PSX_cd_end_iter ();
  PSX_spu_end_iter ();
  PSX_joy_end_iter ();
  PSX_timers_end_iter ();

  // Finalitza traça
  PSX_int_set_mode_trace ( false );
  PSX_dma_set_mode_trace ( false );
  PSX_cd_set_mode_trace ( false );
  PSX_gpu_set_mode_trace ( false );
  PSX_gte_set_mode_trace ( false );
  PSX_mem_set_mode_trace ( false );
  ret= PSX_Clock;
  PSX_Clock= 0;
  
  return ret;
  
} // end PSX_trace

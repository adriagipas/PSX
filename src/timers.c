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
 *  timers.c - Implementació del mòdul de comptadors.
 *
 */


#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "PSX.h"




/**********/
/* MACROS */
/**********/

#define SIGNAL_HBLANK        						\
  PSX_gpu_signal_hblank ( _timer0.sync_enabled  || _timer1_use_hblank )




/*********/
/* TIPUS */
/*********/

typedef struct
{
  
  uint16_t counter;
  uint32_t target;
  int      clocks_to_target;
  int      clocks_to_FFFF;
  bool     sync_enabled;
  int      sync_mode;
  int      source;
  bool     paused;
  bool     reset_after_target;
  bool     irq_when_target;
  bool     irq_when_FFFF;
  bool     irq_one_shot;
  bool     irq_toggle_bit;
  bool     irq_requested;
  bool     target_reached;
  bool     FFFF_reached;
  bool     irq_triggered;
  
} psx_timer_t;




/*********/
/* ESTAT */
/*********/

static struct
{

  int cc; /* Cicles de CPUx11, és a dir, fraccions 1/11 de CPU. 7
             fraccions son un cicle de GPU. */
  int dot; /* Número de cicles gpu per a cada punt. */
  
} _dotclock;

static psx_timer_t _timer0;
static bool _timer0_use_dotclock;

static psx_timer_t _timer1;
static bool _timer1_use_hblank;

static psx_timer_t _timer2;
static struct
{
  int  cc;
  bool enabled;
} _timer2_cc8;

// Controla quan cridar a clock.
static struct
{
  
  int cc_used;
  int cc;
  int cctoIRQ;
  int cctoEvent;
  
} _timing;




/*********************/
/* FUNCIONS PRIVADES */
/*********************/

static void
update_timing_event (void)
{

  int tmp;

  
  // Actualitza cctoEvent
  _timing.cctoEvent= 100000; // ¿¿?????
  if ( _timing.cctoIRQ && _timing.cctoIRQ < _timing.cctoEvent )
    _timing.cctoEvent= _timing.cctoIRQ;
  
  // Update PSX_NextEventCC
  tmp= PSX_Clock + PSX_timers_next_event_cc ();
  if ( tmp < PSX_NextEventCC )
    PSX_NextEventCC= tmp;
  
} // end update_timing_event


// 0 significa que no s'ha d'activar.
static int
calc_timer_clockstoIRQ (
        		psx_timer_t *timer
        		)
{

  int ret;


  ret= 0;
  if ( timer->irq_when_target && !ret )
    ret= timer->clocks_to_target;
  if ( timer->irq_when_FFFF && (!ret || timer->clocks_to_FFFF < ret) )
    ret= timer->clocks_to_FFFF;
  
  return ret;
  
} // end calc_timer_clockstoIRQ


static void
update_timing (void)
{

  int aux;
  
  
  _timing.cctoIRQ= 0;
  
  // Timer0.
  if ( !_timer0.paused )
    {
      aux= calc_timer_clockstoIRQ ( &_timer0 );
      if ( aux != 0 )
        {
          if ( _timer0_use_dotclock )
            {
              aux*= 7*_dotclock.dot;
              aux= aux/11 + ((aux%11!=0) ? 1 : 0);
            }
          if ( !_timing.cctoIRQ || aux < _timing.cctoIRQ )
            _timing.cctoIRQ= aux;
        }
    }

  // Timer 1.
  // NOTA!! Si està en mode HBlank, el clock es fa explícit després de
  // cada HBlank, per tant mentre siga així no és necessari
  // preocupar-se en el timing per eixe cas.
  if ( !_timer1.paused && !_timer1_use_hblank )
    {
      aux= calc_timer_clockstoIRQ ( &_timer1 );
      if ( !aux && (!_timing.cctoIRQ || aux < _timing.cctoIRQ) )
        _timing.cctoIRQ= aux;
    }

  // Timer 2.
  if ( !_timer2.paused )
    {
      aux= calc_timer_clockstoIRQ ( &_timer2 );
      if ( aux != 0 )
        {
          if ( _timer2_cc8.enabled ) aux*= 8;
          if ( !_timing.cctoIRQ || aux < _timing.cctoIRQ )
            _timing.cctoIRQ= aux;
        }
    }

  update_timing_event ();
  
} // end update_timing


static void
update_clocks_to (
        	  psx_timer_t *timer
        	  )
{
  
  /* to 0xFFFF */
  timer->clocks_to_FFFF=
    (timer->counter==0xFFFF) ? 0x10000 : 0xFFFF-timer->counter;

  /* to target. */
  timer->clocks_to_target=
    (timer->counter >= timer->target) ?
    (0x10000-timer->counter) + timer->target :
    timer->target - timer->counter;
  
} /* end update_clocks_to */


static void
init_timer (
            psx_timer_t *timer
            )
{

  timer->counter= 0x0000;
  timer->target= 0x10000; // 0x10000 és 0
  timer->sync_mode= 0;
  timer->sync_enabled= false;
  timer->source= 0;
  timer->paused= false;
  timer->irq_triggered= false;
  timer->reset_after_target= false;
  timer->irq_when_target= false;
  timer->irq_when_FFFF= false;
  timer->irq_one_shot= true;
  timer->irq_toggle_bit= false;
  timer->irq_requested= false;
  timer->target_reached= false;
  timer->FFFF_reached= false;
  update_clocks_to ( timer );
  
} /* end init_timer */


static void
set_counter_value (
        	   psx_timer_t    *timer,
        	   const uint32_t  val
        	   )
{
  
  timer->counter= (uint16_t) (val&0xFFFF);
  update_clocks_to ( timer );
  update_timing ();
  
} // end set_counter_value


static void
set_counter_mode (
        	  psx_timer_t    *timer,
        	  const uint32_t  data
        	  )
{

  timer->sync_enabled= (data&0x1)!=0;
  timer->sync_mode= (data>>1)&0x3;
  timer->reset_after_target= (data&0x8)!=0;
  timer->irq_when_target= (data&0x10)!=0;
  timer->irq_when_FFFF= (data&0x20)!=0;
  timer->irq_one_shot= (data&0x40)==0;
  timer->irq_toggle_bit= (data&0x80)!=0;
  timer->source= (data>>8)&0x3;
  
  /* Reset values. */
  timer->irq_requested= false;
  timer->target_reached= false;
  timer->FFFF_reached= false;
  timer->counter= 0x0000;
  timer->paused= false;
  timer->irq_triggered= false;
  update_clocks_to ( timer );
  
} /* end set_counter_mode */


static uint32_t
get_counter_mode (
        	  psx_timer_t *timer
        	  )
{
  
  uint32_t ret;

  
  ret=
    (timer->sync_enabled?0x1:0x0) |
    (timer->sync_mode<<1) |
    ((timer->reset_after_target?0x1:0x0)<<3) |
    ((timer->irq_when_target?0x1:0x0)<<4) |
    ((timer->irq_when_FFFF?0x1:0x0)<<5) |
    ((timer->irq_one_shot?0x0:0x1)<<6) |
    ((timer->irq_toggle_bit?0x1:0x0)<<7) |
    (timer->source<<8) |
    ((timer->irq_requested?0x0:0x1)<<10) |
    ((timer->target_reached?0x1:0x0)<<11) |
    ((timer->FFFF_reached?0x1:0x0)<<12);
  timer->target_reached= false;
  timer->FFFF_reached= false;
  
  return ret;
  
} /* end get_counter_mode */


static void
set_target_value (
        	  psx_timer_t    *timer,
        	  const uint32_t  val
        	  )
{

  timer->target= (val&0xFFFF);
  if ( timer->target == 0 )
    timer->target= 0x10000;
  update_clocks_to ( timer );
  update_timing ();
  
} // end set_target_value


static void
clock_timer (
             psx_timer_t *timer,
             const int    clocks,
             const int    IRQ
             )
{

  bool update;
  int irqs;

  
  /* Emula els pocs cicles del IRQ pulse. */
  /* Si és massa poc puc ficar un comptador amb 2 o 3 clocks. */
  if ( timer->irq_requested && !timer->irq_toggle_bit )
    timer->irq_requested= false;
  
  if ( timer->paused ) return;
  
  /* Processa clocks. */
  irqs= 0;
  update= false;
  timer->clocks_to_target-= clocks;
  timer->clocks_to_FFFF-= clocks;
  timer->counter+= clocks;
  if ( timer->clocks_to_target <= 0 )
    {
      update= true;
      timer->target_reached= true;
      if ( timer->reset_after_target )
        {
          timer->counter= (-timer->clocks_to_target)%timer->target;
          if ( timer->irq_when_target )
            irqs= 1 + (-timer->clocks_to_target)/timer->target;
        }
      else if ( timer->irq_when_target ) irqs= 1;
    }
  if ( timer->clocks_to_FFFF <= 0 )
    {
      update= true;
      timer->FFFF_reached= true;
      if ( !timer->reset_after_target ) timer->counter+= clocks;
      if ( timer->irq_when_FFFF ) irqs= 1;
    }
  if ( update ) update_clocks_to ( timer );

  /* IRQ. */
  for ( ; irqs; --irqs )
    {
      if ( timer->irq_one_shot && timer->irq_triggered ) return;
      timer->irq_triggered= true;
      if ( timer->irq_toggle_bit )
        timer->irq_requested= !timer->irq_requested;
      else timer->irq_requested= true;
      if ( timer->irq_requested ) PSX_int_interruption ( IRQ );
    }
  
} // end clock_timer


static void
init_timer0 (void)
{

  init_timer ( &_timer0 );
  _timer0_use_dotclock= false;
  
} /* end init_timer0 */


static void
timer0_set_counter_mode (
        		 const uint32_t data
        		 )
{

  set_counter_mode ( &_timer0, data );
  _timer0_use_dotclock= (_timer0.source&0x1)!=0;
  SIGNAL_HBLANK;
  if ( _timer0.sync_enabled && _timer0.sync_mode == 3 )
    _timer0.paused= true;
  update_timing ();
  
} // end timer0_set_counter_mode


static void
init_timer1 (void)
{

  init_timer ( &_timer1 );
  _timer1_use_hblank= false;
  
} /* end init_timer1 */


static void
timer1_set_counter_mode (
        		 const uint32_t data
        		 )
{

  set_counter_mode ( &_timer1, data );
  _timer1_use_hblank= (_timer1.source&0x1)!=0;
  SIGNAL_HBLANK;
  if ( _timer1.sync_enabled && _timer1.sync_mode == 3 )
    _timer1.paused= true;
  update_timing ();
  
} // end timer1_set_counter_mode


static void
init_timer2 (void)
{

  init_timer ( &_timer2 );
  _timer2_cc8.cc= 0;
  _timer2_cc8.enabled= false;
  
} /* end init_timer2 */


static void
timer2_set_counter_mode (
        		 const uint32_t data
        		 )
{

  set_counter_mode ( &_timer2, data );
  _timer2_cc8.enabled= (_timer2.source>=2);
  if ( _timer2.sync_enabled &&
       (_timer2.sync_mode == 0 || _timer2.sync_mode==3) )
    _timer2.paused= true;
  update_timing ();
  
} // end timer2_set_counter_mode


static void
clock (void)
{
  
  int dots,cc8,cc;
  

  cc= PSX_Clock-_timing.cc_used;
  if ( cc > 0 ) { _timing.cc+= cc; _timing.cc_used+= cc; }
  if ( _timing.cc == 0 ) return;
  
  // Timer 0.
  _dotclock.cc+= 11*_timing.cc;
  dots= _dotclock.cc/(7*_dotclock.dot);
  _dotclock.cc%= 7*_dotclock.dot;
  if ( !_timer0_use_dotclock )
    clock_timer ( &_timer0, _timing.cc, PSX_INT_TMR0 );
  else if ( dots > 0 )
    clock_timer ( &_timer0, dots, PSX_INT_TMR0 );

  // Timer 1.
  if ( !_timer1_use_hblank )
    clock_timer ( &_timer1, _timing.cc, PSX_INT_TMR1 );

  // Timer 2.
  _timer2_cc8.cc+= _timing.cc;
  cc8= _timer2_cc8.cc/8;
  _timer2_cc8.cc%= 8;
  if ( !_timer2_cc8.enabled )
    clock_timer ( &_timer2, _timing.cc, PSX_INT_TMR2 );
  else if ( cc8 > 0 )
    clock_timer ( &_timer2, cc8, PSX_INT_TMR2 );

  // Actualitza timing.
  _timing.cc= 0;
  update_timing ();
  
} // end clock




/**********************/
/* FUNCIONS PÚBLIQUES */
/**********************/
#if 0
void
PSX_timers_clock (
        	  const bool end_iter
        	  )
{

  int cc;

  
  cc= PSX_Clock-_timing.cc_used;
  if ( cc > 0 )
    {
      _timing.cc+= cc;
      _timing.cc_used+= cc;
      if ( (_timing.cctoIRQ && _timing.cc >= _timing.cctoIRQ) ||
           _timing.cc >= 100000 )
        clock ();
    }
  if ( end_iter ) _timing.cc_used= 0;
  
} // end PSX_timers_clock
#endif

void
PSX_timers_end_iter (void)
{

  int cc;
  
  
  cc= PSX_Clock-_timing.cc_used;
  if ( cc > 0 )
    {
      _timing.cc+= cc;
      _timing.cc_used+= cc;
      if ( _timing.cc >= _timing.cctoEvent )
        clock ();
    }
  _timing.cc_used= 0;
  
} // end PSX_timers_clock


int
PSX_timers_next_event_cc (void)
{

  int ret;
  
  
  ret= _timing.cctoEvent - _timing.cc;
  assert ( ret >= 0 );
  
  return ret;
  
} // end PSX_timers_next_event_cc


void
PSX_timers_init (void)
{

  _dotclock.cc= 0;
  _dotclock.dot= 7; // HRES_256 !!!! Un poc guarrà.
  init_timer0 ();
  init_timer1 ();
  init_timer2 ();
  _timing.cc_used= 0;
  update_timing ();
  
} // end PSX_timers_init


void
PSX_timers_hblank_in (void)
{

  // Timer 0.
  if ( _timer0.sync_enabled )
    {
      
      clock ();
      
      switch ( _timer0.sync_mode )
        {
        case 0: // Pause counter during Hblank(s)
          _timer0.paused= true;
          update_timing ();
          break;
        case 1: // Reset counter to 0000h at Hblank(s)
        case 2: // Reset counter to 0000h at Hblank(s) and pause
        	// outside of Hblank
          _timer0.paused= false;
          _timer0.counter= 0x0000;
          update_clocks_to ( &_timer0 );
          update_timing ();
          break;
        case 3: // Pause until Hblank occurs once, then switch to Free
        	// Run
          _timer0.paused= false;
          _timer0.sync_enabled= false;
          update_timing ();
          break;
        }
    }

  // Timer 1.
  if ( _timer1_use_hblank )
    {
      clock ();
      clock_timer ( &_timer1, 1, PSX_INT_TMR1 );
    }
  
} // end PSX_timers_hblank_in


void
PSX_timers_hblank_out (void)
{

  // Timer 0.
  if ( _timer0.sync_enabled )
    {
      
      clock ();
      
      switch ( _timer0.sync_mode )
        {
        case 0: // Pause counter during Hblank(s)
          _timer0.paused= false;
          update_timing ();
          break;
        case 1: // Reset counter to 0000h at Hblank(s)
          break;
        case 2: // Reset counter to 0000h at Hblank(s) and pause
        	// outside of Hblank
          _timer0.paused= true;
          update_timing ();
          break;
        case 3: // Pause until Hblank occurs once, then switch to Free
        	// Run
          break;
        }
      
    }
  
} // end PSX_timers_hblank_out


void
PSX_timers_vblank_in (void)
{

  // Timer 1.
  if ( _timer1.sync_enabled )
    {

      clock ();
      
      switch ( _timer1.sync_mode )
        {
        case 0: // Pause counter during Vblank(s)
          _timer1.paused= true;
          update_timing ();
          break;
        case 1: // Reset counter to 0000h at Vblank(s)
        case 2: // Reset counter to 0000h at Vblank(s) and pause
        	// outside of Vblank
          _timer1.paused= false;
          _timer1.counter= 0x0000;
          update_clocks_to ( &_timer1 );
          update_timing ();
          break;
        case 3: // Pause until Vblank occurs once, then switch to Free
        	// Run
          _timer1.paused= false;
          _timer1.sync_enabled= false;
          update_timing ();
          break;
        }
      
    }
  
} // end PSX_timers_vblank_in


void
PSX_timers_vblank_out (void)
{

  // Timer 1.
  if ( _timer1.sync_enabled )
    {
      
      clock ();
      
      switch ( _timer1.sync_mode )
        {
        case 0: // Pause counter during Vblank(s)
          _timer1.paused= false;
          update_timing ();
          break;
        case 1: // Reset counter to 0000h at Vblank(s)
          break;
        case 2: // Reset counter to 0000h at Vblank(s) and pause
        	// outside of Vblank
          _timer1.paused= true;
          update_timing ();
          break;
        case 3: // Pause until Vblank occurs once, then switch to Free
        	// Run
          break;
        }
    }
  
} // end PSX_timers_vblank_out


void
PSX_timers_set_counter_value (
        		      const uint32_t data,
        		      const int      timer
        		      )
{

  clock ();
  
  switch ( timer )
    {
    case 0: set_counter_value ( &_timer0, data ); break;
    case 1: set_counter_value ( &_timer1, data ); break;
    case 2: set_counter_value ( &_timer2, data ); break;
    default: break;
    }
  
} // end PSX_timers_set_counter_value


uint32_t
PSX_timers_get_counter_value (
        		      const int timer
        		      )
{
  uint32_t ret;

  
  clock ();
  
  switch ( timer )
    {
    case 0: ret= (uint32_t) _timer0.counter; break;
    case 1: ret= (uint32_t) _timer1.counter; break;
    case 2: ret= (uint32_t) _timer2.counter; break;
    default: ret= 0x0000; break;
    }
  
  return ret;
  
} // end PSX_timers_get_counter_value


void
PSX_timers_set_counter_mode (
        		     const uint32_t data,
        		     const int      timer
        		     )
{

  clock ();
  
  switch ( timer )
    {
    case 0: timer0_set_counter_mode ( data ); break;
    case 1: timer1_set_counter_mode ( data ); break;
    case 2: timer2_set_counter_mode ( data ); break;
    default: break;
    }
  
} // end PSX_timers_set_counter_mode


uint32_t
PSX_timers_get_counter_mode (
        		     const int timer
        		     )
{

  uint32_t ret;

  
  clock (); // Sobretot pel irq_requested.
  
  switch ( timer )
    {
    case 0: ret= get_counter_mode ( &_timer0 ); break;
    case 1: ret= get_counter_mode ( &_timer1 ); break;
    case 2: ret= get_counter_mode ( &_timer2 ); break;
    default: ret= 0; break;
    }
  
  return ret;
  
} // end PSX_timers_get_counter_mode


void
PSX_timers_set_target_value (
        		     const uint32_t data,
        		     const int      timer
        		     )
{
  
  clock ();
  
  switch ( timer )
    {
    case 0: set_target_value ( &_timer0, data ); break;
    case 1: set_target_value ( &_timer1, data ); break;
    case 2: set_target_value ( &_timer2, data ); break;
    default: break;
    }
  
} // end PSX_timers_set_target_value


uint32_t
PSX_timers_get_target_value (
        		     const int timer
        		     )
{

  switch ( timer )
    {
    case 0: return (uint32_t) (_timer0.target&0xFFFF);
    case 1: return (uint32_t) (_timer1.target&0xFFFF);
    case 2: return (uint32_t) (_timer2.target&0xFFFF);
    default: return 0x0000;
    }
  
} // end PSX_timers_get_target_value


void
PSX_timers_set_dot_gpucc (
        		  const int gpucc
        		  )
{

  clock ();
  
  _dotclock.dot= gpucc;
  update_timing ();
  
} // end PSX_timers_set_dot_gpucc

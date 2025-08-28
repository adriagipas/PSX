/*
 * Copyright 2018-2025 Adrià Giménez Pastor.
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
 *  joy.c - Implementació del mòdul dels controladors i memory cards.
 *
 */


#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PSX.h"




/**********/
/* MACROS */
/**********/

#define BAUDRATE_MASK 0x1FFFFF

// Assumint paraula fixa de 8bits!!
#define JOY_STAT1_READY (!_transfer.activated || _transfer.nbits>=1)
#define JOY_STAT2_READY (!_transfer.activated || _transfer.nbits>=8)

// NOTA!! Havia malinterpretat mal açò, atenent al codi de la BIOS,
// pareix que hi ha un delay entre rebre el byte i fixar el IRQ7 degut
// al ACK del perifèric.
#define CC2ACK_LOW 10 // És necessari???????
// Aparentment el ACK torna automàticament a la posició de HIGH (0)
// després d'uns 100 cicles
#define CC2ACK_HIGH 100

#define MEMCARD_SIZE (128*1024)




/*********/
/* ESTAT */
/*********/

// Callbacks.
static PSX_Warning *_warning;
static PSX_GetControllerState *_get_ctrl_state;
static void *_udata;

// Control
static struct
{

  bool txen;
  bool txen_latched;
  bool joyn_select;
  bool rxen;
  bool unk1;
  bool unk2;
  int  rx_int_mode; // when RX FIFO contains 1,2,4,8 bytes
  bool tx_int_enabled;
  bool rx_int_enabled;
  bool ack_int_enabled;
  int  slot_number;
  
} _ctrl;

// Mode
static struct
{

  int  baudrate_reload_factor; // 1=MUL1, 2=MUL16, 3=MUL64 (or 0=MUL1, too)
  int  char_length; // 0=5bits, 1=6bits, 2=7bits, 3=8bits
  bool parity_enabled;
  bool parity_odd;
  bool out_polarity_inverse;
  
} _mode;

// Baudrate reload value
static uint16_t _baudrate_reload_value;

// Timing.
static struct
{
  
  int32_t baudrate_timer; // 21bit timer
  int32_t cc;
  int32_t cc_used;
  int32_t cc2ack_low;
  int32_t cc2ack_high;
  bool    wait_ack;
  bool    wait_ack_high;
  int32_t cctoEvent;
  
} _timing;

// Transfer state.
static struct
{

  uint16_t tx_fifo;
  int      tx_fifo_N;
  uint64_t rx_fifo;
  int      rx_fifo_N;
  uint8_t  byte;
  int      nbits; // Número de bits transferits
  bool     activated;
  
} _transfer;

// Status.
static struct
{

  bool rx_parity_error; // No implementat !!!
  bool ack;
  bool irq_request;
  
} _status;

// Estat dels controladors.
static struct
{

  bool            selected;
  PSX_Controller  type;
  int             step;
  bool            mode_memcard;
  uint8_t        *memc;
  uint8_t         memc_flag;
  enum {
    MEMC_READ,
    MEMC_GET_ID,
    MEMC_WRITE
  }               memc_cmd;
  uint8_t         memc_chk;
  uint8_t         memc_pre;
  uint8_t         memc_msb;
  uint8_t         memc_lsb;
  unsigned int    memc_p;
  
} _devs[2];




/*********************/
/* FUNCIONS PRIVADES */
/*********************/

static void
update_timing_event (void)
{

  int tmp;

  
  // Actualitza cctoEvent
  _timing.cctoEvent= _timing.baudrate_timer;
  if ( _timing.wait_ack && _timing.cc2ack_low < _timing.cctoEvent )
    _timing.cctoEvent= _timing.cc2ack_low;
  if ( _timing.wait_ack_high && _timing.cc2ack_high < _timing.cctoEvent )
    _timing.cctoEvent= _timing.cc2ack_high;
  
  // Update PSX_NextEventCC
  tmp= PSX_Clock + PSX_joy_next_event_cc ();
  if ( tmp < PSX_NextEventCC )
    PSX_NextEventCC= tmp;
  
} // end update_timing_event


static bool
joy_none_read (
               const int      joy,
               const uint8_t  cmd,
               uint8_t       *data
               )
{
  
  switch ( _devs[joy].step )
    {
    case 1: // 42h idlo Receive ID bit0..7 (variable) and Send Read
            // Command (ASCII "B")
      if ( cmd != 0x42 ) goto end;
      *data= 0xFF;
      _devs[joy].step= 2;
      return true;
    case 2: // TAP  idhi  Receive ID bit8..15
      *data= 0xFF;
      _devs[joy].step= 0;
      return false;
    default: goto end;
    }

 end:
  *data= 0xFF;
  _devs[joy].step= 0;
  return false;
  
} // end joy_none_read


static bool
joy_standard_read (
        	   const int      joy,
        	   const uint8_t  cmd,
        	   uint8_t       *data
        	   )
{

  const PSX_ControllerState *cstate;
  
  
  switch ( _devs[joy].step )
    {
    case 1: // 42h idlo Receive ID bit0..7 (variable) and Send Read
            // Command (ASCII "B")
      if ( cmd != 0x42 ) goto end;
      *data= 0x41;
      _devs[joy].step= 2;
      return true;
    case 2: // TAP  idhi  Receive ID bit8..15 (usually/always 5Ah)
      *data= 0x5A;
      _devs[joy].step= 3;
      return true;
    case 3: // MOT  swlo  Receive Digital Switches bit0..7
      cstate= _get_ctrl_state ( joy, _udata );
      if ( cstate == NULL ) goto end;
      *data= ~((uint8_t) cstate->buttons);
      _devs[joy].step= 4;
      return true;
    case 4: // MOT  swhi  Receive Digital Switches bit8..15
      cstate= _get_ctrl_state ( joy, _udata );
      if ( cstate == NULL ) goto end;
      *data= ~((uint8_t) (cstate->buttons>>8));
      _devs[joy].step= 0;
      return false;
    default: goto end;
    }

 end:
  *data= 0xFF;
  _devs[joy].step= 0;
  return false;
  
} // end joy_standard_read


static bool
memc_read (
           const int      joy,
           const uint8_t  cmd,
           uint8_t       *data
           )
{

  uint8_t byte;

  
  if ( _devs[joy].memc == NULL ) goto end;
  switch ( _devs[joy].step )
    {
    case 2: // 00h  5Ah   Receive Memory Card ID1
      if ( cmd != 0x00 ) goto end;
      *data= 0x5A;
      _devs[joy].step= 3;
      return true;
    case 3: // 0h  5Dh   Receive Memory Card ID2
      if ( cmd != 0x00 ) goto end;
      *data= 0x5D;
      _devs[joy].step= 4;
      return true;
    case 4: // MSB  (00h) Send Address MSB  ;\sector number (0..3FFh)
      *data= 0x00;
      _devs[joy].memc_pre= cmd;
      _devs[joy].memc_msb= cmd&0x3;
      _devs[joy].memc_chk= cmd&0x3;
      _devs[joy].step= 5;
      return true;
    case 5: // LSB  (pre) Send Address LSB  ;/
      *data= _devs[joy].memc_pre;
      _devs[joy].memc_lsb= cmd;
      _devs[joy].memc_chk^= cmd;
      _devs[joy].step= 6;
      return true;
    case 6: // 00h 5Ch Receive Command Acknowledge 1 ;<-- late /ACK
            // after this byte-pair
      if ( cmd != 0x00 ) goto end;
      *data= 0x5C;
      _devs[joy].step= 7;
      return true;
    case 7: // 00h  5Dh   Receive Command Acknowledge 2
      if ( cmd != 0x00 ) goto end;
      *data= 0x5D;
      _devs[joy].step= 8;
      return true;
    case 8: // 00h  MSB   Receive Confirmed Address MSB
      if ( cmd != 0x00 ) goto end;
      *data= _devs[joy].memc_msb;
      _devs[joy].step= 9;
      return true;
    case 9: // 00h  LSB   Receive Confirmed Address LSB
      if ( cmd != 0x00 ) goto end;
      *data= _devs[joy].memc_lsb;
      _devs[joy].memc_p=
        ((((unsigned int) _devs[joy].memc_msb)<<8) |
         ((unsigned int) _devs[joy].memc_lsb))<<7;
      _devs[joy].step= 10;
      return true;
    case 10 ... 137: // 00h  ...   Receive Data Sector (128 bytes)
      if ( cmd != 0x00 ) goto end;
      assert ( _devs[joy].memc_p < MEMCARD_SIZE );
      *data= byte= _devs[joy].memc[_devs[joy].memc_p++];
      _devs[joy].memc_chk^= byte;
      ++(_devs[joy].step);
      return true;
    case 138: // 00h  CHK   Receive Checksum (MSB xor LSB xor Data bytes)
      if ( cmd != 0x00 ) goto end;
      *data= _devs[joy].memc_chk;
      _devs[joy].step= 139;
      return true;
    case 139: // 00h 47h Receive Memory End Byte (should be always
              // 47h="G"=Good for Read)
      if ( cmd != 0x00 ) goto end;
      *data= 0x47;
      _devs[joy].step= 0;
      return false;
    }
  
  end:
  *data= 0xFF;
  _devs[joy].step= 0;
  return false;
  
} // end memc_read


static bool
memc_get_id (
             const int      joy,
             const uint8_t  cmd,
             uint8_t       *data
             )
{

  if ( _devs[joy].memc == NULL ) goto end;
  if ( cmd != 0x00 ) goto end;
  
  switch ( _devs[joy].step )
    {
    case 2: // 00h  5Ah   Receive Memory Card ID1
      *data= 0x5A;
      _devs[joy].step= 3;
      return true;
    case 3: // 00h  5Dh   Receive Memory Card ID2
      *data= 0x5D;
      _devs[joy].step= 4;
      return true;
    case 4: // 00h  5Ch   Receive Command Acknowledge 1
      *data= 0x5C;
      _devs[joy].step= 5;
      return true;
    case 5: // 00h  5Dh   Receive Command Acknowledge 2
      *data= 0x5D;
      _devs[joy].step= 6;
      return true;
    case 6: // 00h  04h   Receive 04h
      *data= 0x04;
      _devs[joy].step= 7;
      return true;
    case 7: // 0h  00h   Receive 00h
      *data= 0x00;
      _devs[joy].step= 8;
      return true;
    case 8: // 00h  00h   Receive 00h
      *data= 0x00;
      _devs[joy].step= 9;
      return true;
    case 9: // 00h  80h   Receive 80h
      *data= 0x80;
      _devs[joy].step= 0;
      return false;
    default: goto end;
    }

 end:
  *data= 0xFF;
  _devs[joy].step= 0;
  return false;
  
} // end memc_get_id


static bool
memc_write (
            const int      joy,
            const uint8_t  cmd,
            uint8_t       *data
            )
{

  if ( _devs[joy].memc == NULL ) goto end;

  switch ( _devs[joy].step )
    {
    case 2: // 00h  5Ah   Receive Memory Card ID1
      if ( cmd != 0x00 ) goto end;
      *data= 0x5A;
      _devs[joy].step= 3;
      return true;
    case 3: // 0h  5Dh   Receive Memory Card ID2
      if ( cmd != 0x00 ) goto end;
      *data= 0x5D;
      _devs[joy].step= 4;
      return true;
    case 4: // MSB  (00h) Send Address MSB  ;\sector number (0..3FFh)
      *data= 0x00;
      _devs[joy].memc_pre= cmd;
      _devs[joy].memc_msb= cmd&0x3;
      _devs[joy].memc_chk= cmd&0x3;
      _devs[joy].step= 5;
      return true;
    case 5: // LSB  (pre) Send Address LSB  ;/
      *data= _devs[joy].memc_pre;
      _devs[joy].memc_lsb= cmd;
      _devs[joy].memc_chk^= cmd;
      _devs[joy].memc_pre= cmd;
      _devs[joy].memc_p=
        ((((unsigned int) _devs[joy].memc_msb)<<8) |
         ((unsigned int) _devs[joy].memc_lsb))<<7;
      _devs[joy].step= 6;
      return true;
    case 6 ... 133: // ...  (pre) Send Data Sector (128 bytes)
      *data= _devs[joy].memc_pre;
      _devs[joy].memc_pre= cmd;
      assert ( _devs[joy].memc_p < MEMCARD_SIZE );
      _devs[joy].memc[_devs[joy].memc_p++]= cmd;
      _devs[joy].memc_chk^= cmd;
      ++(_devs[joy].step);
      return true;
    case 134: // CHK  (pre) Send Checksum (MSB xor LSB xor Data bytes)
      *data= _devs[joy].memc_pre;
      // Reutilitze pre
      _devs[joy].memc_pre= (cmd==_devs[joy].memc_chk) ? 0x47 : 0x4E;
      _devs[joy].step= 135;
      return true;
    case 135: // 00h  5Ch   Receive Command Acknowledge 1
      if ( cmd != 0x00 ) goto end;
      *data= 0x5C;
      _devs[joy].step= 136;
      return true;
    case 136: // 00h  5Dh   Receive Command Acknowledge 2
      if ( cmd != 0x00 ) goto end;
      *data= 0x5D;
      _devs[joy].step= 137;
      return true;
    case 137: // 00h 4xh Receive Memory End Byte (47h=Good,
              // 4Eh=BadChecksum, FFh=BadSector)
      if ( cmd != 0x00 ) goto end;
      // FFh=BadSector ???
      *data= _devs[joy].memc_pre;
      if ( _devs[joy].memc_pre == 0x47 )
        _devs[joy].memc_flag&= ~0x08; // Reset
      _devs[joy].step= 0;
      return false;
    }
  
  end:
  *data= 0xFF;
  _devs[joy].step= 0;
  return false;
  
} // end memc_write


// Torna cert si encara queden bytes.
static bool
joy_read (
          const int      joy,
          const uint8_t  cmd,
          uint8_t       *data
          )
{
  
  if ( !_devs[joy].selected ) { *data= 0xFF; return false; }
  
  // Primer pas, decideix si entrar en mode memory card o pad.
  if ( _devs[joy].step == 0 )
    {
      switch ( cmd )
        {
          
        case 0x01: // Controller
          *data= 0xFF;
          //if ( _devs[joy].type == PSX_CONTROLLER_NONE ) return false;
          // Pareix que és millor deixar que llisca FFFF en el id.
          // NOTA!! En realitat no està gens clar que és millor.
          _devs[joy].step= 1;
          _devs[joy].mode_memcard= false;
          return true;

        case 0x81: // Memorycard
          *data= 0xFF;
          if ( _devs[joy].memc == NULL )
            return false;
          _devs[joy].step= 1;
          _devs[joy].mode_memcard= true;
          return true;
          
        default: return false;
        }
    }
  else if ( _devs[joy].mode_memcard ) // Memory card 
    {
      if ( _devs[joy].step == 1 ) // Selecciona comandament
        {
          *data= _devs[joy].memc_flag;
          switch ( cmd )
            {
            case 0x52: // Read
              _devs[joy].step= 2;
              _devs[joy].memc_cmd= MEMC_READ;
              return true;
            case 0x53: // Get ID
              _devs[joy].step= 2;
              _devs[joy].memc_cmd= MEMC_GET_ID;
              return true;
            case 0x57: // Write
              _devs[joy].step= 2;
              _devs[joy].memc_cmd= MEMC_WRITE;
              return true;
            default:
              _devs[joy].step= 0;
              return false;
            }
        }
      else // Comandament (step >=2)
        {
          switch ( _devs[joy].memc_cmd )
            {
            case MEMC_READ: return memc_read ( joy, cmd, data );
            case MEMC_GET_ID: return memc_get_id ( joy, cmd, data );
            case MEMC_WRITE: return memc_write ( joy, cmd, data );
            default: // ??
              _devs[joy].step= 0;
              return false;
            }
        }
    }
  else // Controllers
    {
      switch ( _devs[joy].type )
        {
        case PSX_CONTROLLER_NONE:
          return joy_none_read ( joy, cmd, data );
        case PSX_CONTROLLER_STANDARD:
          return joy_standard_read ( joy, cmd, data );
        default:
          *data= 0xFF;
          _devs[joy].step= 0;
          return false;
        }
    }
  
} // end joy_read


static void
update_irq (void)
{

  static const int rx_mode[]= { 1, 2, 4, 8 };

  
  _status.irq_request=
    ( (_ctrl.ack_int_enabled && _status.ack) ||
      (_ctrl.tx_int_enabled &&
       _transfer.activated && // <-- APOSTA
       (JOY_STAT1_READY || JOY_STAT2_READY)) ||
      (_ctrl.rx_int_enabled &&
       rx_mode[_ctrl.rx_int_mode]==_transfer.rx_fifo_N) )
    ;
  PSX_int_interruption ( PSX_INT_IRQ7, _status.irq_request );
  
} // end update_irq


static void
reload_baudrate_timer (void)
{

  uint32_t tmp,scale;
  
  
  switch ( _mode.baudrate_reload_factor )
    {
    case 0:
    case 1: scale= 1; break;
    case 2: scale= 16; break;
    case 3: scale= 64; break;
    default: scale= 0; break;
    }
  tmp= ((uint32_t) _baudrate_reload_value) * scale;
  tmp&= BAUDRATE_MASK;
  if ( tmp == 0 ) tmp= 1;
  _timing.baudrate_timer= (int32_t) tmp;
  
} // end reload_baudrate_timer


static void
try_activate_transfer (void)
{

  // Com que sempre que s'aplegue a la transmissió del bit8 desactivaré
  // la transferència la primera comprovació es redundant amb
  // JOY_STAT2_READY, però per claredat la deixe.
  if ( _transfer.activated || // <-- Redundant en la pràctica, veure NOTA
       !JOY_STAT2_READY || (!_ctrl.txen && !_ctrl.txen_latched) ||
       _transfer.tx_fifo_N == 0 )
    return;
  _transfer.byte= (uint8_t) (_transfer.tx_fifo&0xFF);
  _transfer.tx_fifo>>= 8;
  --_transfer.tx_fifo_N;
  _transfer.nbits= 0;
  _transfer.activated= true;
  
} // end try_activate_transfer


static void
transfer_byte (void)
{

  int joy_num,newN,desp;
  uint8_t rbyte;
  bool ack;
  
  // Transfereix.
  rbyte= 0xFF;
  joy_num= _ctrl.joyn_select ? _ctrl.slot_number : 0;
  ack= joy_read ( joy_num, _transfer.byte, &rbyte );
  if ( ack ) { _timing.cc2ack_low= CC2ACK_LOW; _timing.wait_ack= true; }
  
  // Inserta byte en la RX FIFO.
  if ( _ctrl.joyn_select || _ctrl.rxen )
    {
      newN= _transfer.rx_fifo_N+1;
      if ( newN > 8 ) newN= 8;
      desp= (newN-1)*8;
      _transfer.rx_fifo&= ~(((uint64_t) 0xFF)<<desp);
      _transfer.rx_fifo|= (((uint64_t) rbyte)<<desp);
      _transfer.rx_fifo_N= newN;
    }
  _ctrl.rxen= false;
  
  // Comprova interrupcions.
  update_irq ();
  
} // end transfer_byte


static void
clock (void)
{

  int32_t remain,cc;
  

  cc= PSX_Clock-_timing.cc_used;
  assert ( cc >= 0 );
  if ( cc > 0 ) { _timing.cc+= cc; _timing.cc_used+= cc; }
  
  if ( _timing.wait_ack )
    {
      _timing.cc2ack_low-= _timing.cc;
      if ( _timing.cc2ack_low <= 0 )
        {
          _status.ack= true;
          _timing.wait_ack= false;
          _timing.wait_ack_high= true;
          _timing.cc2ack_high= CC2ACK_HIGH;
          update_irq ();
        }
    }

  if ( _timing.wait_ack_high )
    {
      _timing.cc2ack_high-= _timing.cc;
      if ( _timing.cc2ack_high <= 0 )
        {
          _status.ack= false;
          _timing.wait_ack_high= false;
        }
    }
  
  _timing.baudrate_timer-= _timing.cc;
  while ( _timing.baudrate_timer <= 0 )
    {
      
      // Transfer bit.
      if ( _transfer.activated && ++_transfer.nbits==8 )
        {
          transfer_byte ();
          _transfer.activated= false;
          try_activate_transfer ();
        }
      
      // Update timers.
      remain= -_timing.baudrate_timer;
      reload_baudrate_timer ();
      _timing.baudrate_timer-= remain;
      
    }
  _timing.cc= 0;
  
  update_timing_event ();
  
} // end clock


static void
reset_most_joy_registers (void)
{

  // ¿¿Què RESETEJE???
  // Entenc que el de control no perquè s'està modificant mentre
  // escrivim açò.
  // Entenc que els IRQ tampoc perquè hi ha un altre bit per fer-ho.

  // Para transferència i reseteja fifo???
  _transfer.activated= false;
  _transfer.tx_fifo_N= 0;
  _transfer.rx_fifo_N= 0;

  // Mode a valor per defecte ??
  memset ( &_mode, 0, sizeof(_mode) );
  _mode.baudrate_reload_factor= 1;
  _mode.char_length= 0x3;

  // Baudrate timer a valor per defecte???
  _baudrate_reload_value= 0x0088;
  reload_baudrate_timer ();

  // Estat dispositius ???
  //_devs[0].step= 0;
  //_devs[1].step= 0;
  
} // end reset_most_joy_registers


static void
select_joys (void)
{

  if ( !_ctrl.joyn_select )
    _devs[0].selected= _devs[1].selected= false;
  else
    {
      _devs[_ctrl.slot_number^1].selected= false;
      if ( !_devs[_ctrl.slot_number].selected )
        {
          _devs[_ctrl.slot_number].selected= true;
          _devs[_ctrl.slot_number].step= 0;
        }
    }
  
} // end select_joys




/**********************/
/* FUNCIONS PÚBLIQUES */
/**********************/

void
PSX_joy_init (
              PSX_Warning            *warning,
              PSX_GetControllerState *get_ctrl_state,
              void                   *udata
              )
{

  // Callbacks.
  _warning= warning;
  _get_ctrl_state= get_ctrl_state;
  _udata= udata;

  // Inicialitza estat.
  memset ( &_ctrl, 0, sizeof(_ctrl) );
  memset ( &_mode, 0, sizeof(_mode) );
  _mode.baudrate_reload_factor= 1;
  _mode.char_length= 0x3;
  _baudrate_reload_value= 0x0088;
  memset ( &_transfer, 0, sizeof(_transfer) );
  memset ( &_status, 0, sizeof(_status) );

  // Controladors.
  _devs[0].type= PSX_CONTROLLER_NONE;
  _devs[0].step= 0;
  _devs[0].selected= false;
  _devs[0].memc= NULL;
  _devs[1].type= PSX_CONTROLLER_NONE;
  _devs[1].step= 0;
  _devs[1].selected= false;
  _devs[1].memc= NULL;
  
  // Inicialitza timing
  _timing.cc= 0;
  _timing.cc_used= 0;
  _timing.wait_ack= false;
  _timing.wait_ack_high= false;
  _timing.cctoEvent= 0;
  reload_baudrate_timer ();
  update_timing_event ();
  
} // end PSX_joy_init


void
PSX_joy_tx_data (
        	 const uint32_t data
        	 )
{

  int newN,desp;
  uint8_t byte;
  
  
  clock ();
  
  // Fica en la fifo.
  byte= (uint8_t) (data&0xFF);
  if ( !JOY_STAT1_READY ) _transfer.byte= byte; // Overwrite current tx value
  else
    {
      newN= _transfer.tx_fifo_N+1;
      if ( newN > 2 ) newN= 2; // Overwrite last value
      desp= (newN-1)*8;
      _transfer.tx_fifo&= ~(0x00FF<<desp);
      _transfer.tx_fifo|= (((uint16_t) byte)<<desp);
      _transfer.tx_fifo_N= newN;
    }
  
  // Prova a transferir.
  _ctrl.txen_latched= _ctrl.txen;
  try_activate_transfer ();
  
} // end PSX_joy_tx_data


uint32_t
PSX_joy_rx_data (void)
{

  uint32_t ret;

  
  clock ();
  
  ret= (uint32_t) (_transfer.rx_fifo&0xFFFFFFFF);
  if ( _transfer.rx_fifo_N )
    {
      _transfer.rx_fifo=
        (_transfer.rx_fifo>>8) | 0xFF00000000000000;
      --_transfer.rx_fifo_N;
      if ( _ctrl.rx_int_enabled )
        update_irq (); // Buidar byte pot generar una int lectura
    }
  
  return ret;
  
} // end PSX_joy_rx_data


uint32_t
PSX_joy_stat (void)
{
  
  uint32_t ret;
  
  clock ();

  ret= (uint32_t)
    (
     (JOY_STAT1_READY ? 0x1 : 0x0) |
     ((_transfer.rx_fifo_N ? 0x1 : 0x0)<<1) |
     ((JOY_STAT2_READY ? 0x1 : 0x0)<<2) |
     ((_status.rx_parity_error ? 0x1 : 0x0)<<3) |
     ((_status.ack ? 0x1 : 0x0)<<7) |
     ((_status.irq_request ? 0x1 : 0x0)<<9) |
     (((uint32_t) _timing.baudrate_timer)<<11)
     );
  
  return ret;
  
} // end PSX_joy_stat


void
PSX_joy_mode_write (
        	    const uint16_t data
        	    )
{
  
  clock ();

  _mode.baudrate_reload_factor= data&0x3;
  _mode.char_length= (data>>2)&0x3;
  _mode.parity_enabled= (data&0x10)!=0;
  _mode.parity_odd= (data&0x20)!=0;
  _mode.out_polarity_inverse= (data&0x100)!=0;

  if ( _mode.char_length != 3 )
    printf ( "JOY char_length: %d no implementat\n", _mode.char_length );
  if ( _mode.parity_enabled )
    printf ( "JOY parity no implementat\n" );
  if ( _mode.parity_odd )
    printf ( "JOY parity odd no implementat\n" );
  if ( _mode.out_polarity_inverse )
    printf ( "JOY out polarity inverse no implementat\n" );
  
} // end PSX_joy_mode_write


uint16_t
PSX_joy_mode_read (void)
{

  uint16_t ret;

  
  ret= (uint16_t) (_mode.baudrate_reload_factor |
                   ((_mode.char_length)<<2) |
                   ((_mode.parity_enabled==true)<<4) |
                   ((_mode.parity_odd==true)<<5) |
                   ((_mode.out_polarity_inverse==true)<<8));
  
  return ret;
  
} // end PSX_joy_mode_read


void
PSX_joy_ctrl_write (
        	    const uint16_t data
        	    )
{
  
  clock ();
  
  _ctrl.txen= ((data&0x1)!=0);
  _ctrl.joyn_select= ((data&0x2)!=0);
  _ctrl.rxen= ((data&0x4)!=0);
  _ctrl.unk1= ((data&0x8)!=0);
  if ( data&0x10 ) // ACK
    {
      _status.rx_parity_error= false;
      _status.ack= false;
      _timing.wait_ack_high= false;
    }
  _ctrl.unk2= ((data&0x20)!=0);
  if ( data&0x40 ) // Reset most JOY_registers to zero???
    reset_most_joy_registers ();
  _ctrl.rx_int_mode= (data>>8)&0x3;
  _ctrl.tx_int_enabled= ((data&0x400)!=0);
  _ctrl.rx_int_enabled= ((data&0x800)!=0);
  _ctrl.ack_int_enabled= ((data&0x1000)!=0);
  _ctrl.slot_number= (data>>13)&0x1;
  select_joys ();
  // Al canviar el txen es pot activar la transferència.
  try_activate_transfer ();
  // Comprova interrupcions.
  update_irq ();
  // Actualitza comptadors temps.
  update_timing_event ();
  
} // end PSX_joy_ctrl_write


uint16_t
PSX_joy_ctrl_read (void)
{

  uint16_t ret;

  
  ret= (uint16_t)
    (
     (_ctrl.txen ? 0x1 : 0x0) |
     ((_ctrl.joyn_select ? 0x1 : 0x0)<<1) |
     ((_ctrl.rxen ? 0x1 : 0x0)<<2) |
     ((_ctrl.unk1 ? 0x1 : 0x0)<<3) |
     ((_ctrl.unk2 ? 0x1 : 0x0)<<5) |
     (((uint16_t) _ctrl.rx_int_mode)<<8) |
     ((_ctrl.rx_int_enabled ? 0x1 : 0x0)<<10) |
     ((_ctrl.tx_int_enabled ? 0x1 : 0x0)<<11) |
     ((_ctrl.ack_int_enabled ? 0x1 : 0x0)<<12) |
     (((uint16_t) _ctrl.slot_number)<<13)
     );
  
  return ret;
  
} // end PSX_joy_ctrl_read


void
PSX_joy_baud_write (
        	    const uint16_t data
        	    )
{
  
  clock ();

  _baudrate_reload_value= data;
  reload_baudrate_timer ();
  update_timing_event ();
  
} // end PSX_joy_baud_write


uint16_t
PSX_joy_baud_read (void)
{
  return _baudrate_reload_value;
} // end PSX_joy_baud_read


void
PSX_joy_end_iter (void)
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
  
} // end PSX_joy_end_iter


int
PSX_joy_next_event_cc (void)
{

  int ret;


  ret= _timing.cctoEvent - _timing.cc;
  assert ( ret >= 0 );
  
  return ret;
  
} // end PSX_joy_next_event_cc


void
PSX_plug_controllers (
        	      const PSX_Controller ctrl1,
        	      const PSX_Controller ctrl2
        	      )
{

  clock ();
  
  if ( _devs[0].type != ctrl1 )
    {
      _devs[0].type= ctrl1;
      _devs[0].step= 0;
    }
  if ( _devs[1].type != ctrl2 )
    {
      _devs[1].type= ctrl2;
      _devs[1].step= 0;
    }
  
} // end PSX_plug_controllers


void
PSX_plug_mem_cards (
        	    uint8_t *memc1,
        	    uint8_t *memc2
        	    )
{

  clock ();
  
  if ( memc1 != _devs[0].memc )
    {
      _devs[0].memc= memc1;
      _devs[0].step= 0;
      _devs[0].memc_flag= 0x08;
    }
  if ( memc2 != _devs[1].memc )
    {
      _devs[1].memc= memc2;
      _devs[1].step= 0;
      _devs[1].memc_flag= 0x08;
    }
    
} // end PSX_plug_mem_cards

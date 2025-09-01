/*
 * Copyright 2017-2025 Adrià Giménez Pastor.
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
 *  cd.c - Implementació del mòdul del CD-ROM.
 *
 */
/*
 *  NOTES:
 *
 *   - El tema d'habilitar/deshabilitar interrupcions vaig a assumir,
 *     que s'ha d'interpretar com generar o no IRQ, però que el
 *     sistema de generar INT en registres i confirmar-los per
 *     continuar és obligatori.
 *
 *   - La frase: "If there are any pending cdrom interrupts, these
 *     MUST be acknowledged before sending the command (otherwise bit7
 *     of 1F801800h will stay set forever)." No acaba d'encaixar amb
 *     la resta del document, així que de moment no vaig a resetejar
 *     el bisy flag ahí, de fet, ja s'haurà resetejat abans.
 *
 *   - Sobre els tracks:
 *
 *     1.- Recordar que a banda dels 'tracks' en cada sessió hi ha un
 *         lead-in i un lead-out, però aquests no es consideren
 *         'tracks'.
 *
 *     2.- Independentment a quina sessió ocorreguent tots els tracks
 *         estan identificats amb un número global al disc (01..99).
 *
 *     3.- Vaig a assumir que les operacions que tenen per paràmetre
 *         un número BCD de track fan referència a tracks globals, no
 *         a números relatius al track inicial de la sessió actual.
 *
 *   - Falten implementar SoundMap.
 *
 *   - Vaig a intentar implementar el que diu NOCASH, que el buffer
 *     intern és de 8, però que una vegada es llig el més vell
 *     s'ignoren la resta fins al més nou.
 *
 *   - Mirant el codi de mednafen pareix que és molt important emular
 *     el temps de seek. No obstant això, mirant NOCASH no tinc gens
 *     clar quant és eixe temps, així que vaig a copiar-me la rutina
 *     de mednafen.
 *
 *   - Com es suposa que la primera resposta és sempre molt ràpida,
 *     vaig a actulitzar _cmd.stat immediatament.
 *
 */


#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "PSX.h"




/**********/
/* MACROS */
/**********/

#define FIFO_SIZE 16

#define MAXCC PSX_CYCLES_PER_SEC*10 // Si està 10 segons sense clock, clock.

#define STAT_ERROR      0x01
#define STAT_MOTOR_ON   0x02
#define STAT_ID_ERROR   0x08
#define STAT_SHELL_OPEN 0x10
#define STAT_READ       0x20
#define STAT_SEEK       0x40
#define STAT_PLAY       0x80

// En la nova implementació basada en els timings de mednafen no vaig
// a gastar açò.
/*
#define DEFAULT_CC_FIRST 0xc4e1
#define CC_BUSY_FACTOR 4 // Arreu
*/
// Açò és una inventada total!!! Es supossa que hi han operacions on
// el temps la segona resposta depén del temps del seek time. El que
// he fet és fixar-me en temps de segona resposta d'altres i ficar un
// número paregut i que siga més que el first response d'Init
#define DEFAULT_CC_SEEK_SECOND_SIMPLE 0x0004a00
#define DEFAULT_CC_SEEK_SECOND 0x10bd93
#define DEFAULT_CC_SEEK_SECOND_DOUBLE (DEFAULT_CC_SEEK_SECOND>>1)

///#define CC2READ (PSX_CYCLES_PER_SEC/75)
#define CC2READ 451584 // Calculat per a que 1/8 ADPCM a doble encaixe
                       // amb SPU. NOTA!!! És exactament
                       // PSX_CYCLES_PER_SEC/75
#define CC2READ_DOUBLE (CC2READ>>1)

// Valor inspirat en mednafen. Havia ficat 2000, que és el que gasta
// mednafen, però en el RE2 2000 no és prou, ficant 4000 ja funciona.
//#define CC2IRQ_EXPIRED 2000
#define CC2IRQ_EXPIRED 4000


#define NBUFS 6
#define MAXBUFSIZE 0x930
#define HEADERSIZE 8

#define BCD2DEC(BYTE) (((BYTE)>>4)*10 + ((BYTE)&0xF))

#define ADPCM_MAXLEN_BUF (((18*28*8*2)/6)*7)

#define ADPCM_NBUFS 4




/*********/
/* TIPUS */
/*********/

typedef struct
{
  
  uint8_t data[MAXBUFSIZE];
  int     nbytes;
  
} sector_t;

typedef struct
{
  
  uint8_t v[MAXBUFSIZE];
  bool    audio;
  
} raw_sector_t;

typedef struct
{

  int16_t left[ADPCM_MAXLEN_BUF];
  int16_t right[ADPCM_MAXLEN_BUF];
  int     length;
  
} adpcm_buf_t;


typedef enum
  {
    REGION_JAPAN,
    REGION_AMERICA,
    REGION_EUROPE,
    REGION_NONE
  } region_t;

typedef struct
{
  int16_t v[0x20];
  int     p;
} ringbuf_t;

typedef enum
  {
   READ_NEXT_SECTOR_ERROR,
   READ_NEXT_SECTOR_OK,
   READ_NEXT_SECTOR_OK_INT
  } read_next_sector_status_t;



/*************/
/* CONSTANTS */
/*************/

static const int64_t adpcm_interpolate_tables[7][29]=
  {
    { 0x0, 0x0, 0x0, 0x0, 0x0, -0x0002, 0x000A, -0x0022, 0x0041, -0x0054,
      0x0034, 0x0009, -0x010A, 0x0400, -0x0A78, 0x234C, 0x6794, -0x1780,
      0x0BCD, -0x0623, 0x0350, -0x016D, 0x006B, 0x000A, -0x0010, 0x0011,
      -0x0008, 0x0003, -0x0001},
    { 0x0, 0x0, 0x0, -0x0002, 0x0, 0x0003, -0x0013, 0x003C, -0x004B, 0x00A2,
      -0x00E3, 0x0132, -0x0043, -0x0267, 0x0C9D, 0x74BB, -0x11B4, 0x09B8,
      -0x05BF, 0x0372, -0x01A8, 0x00A6, -0x001B, 0x0005, 0x0006, -0x0008,
      0x0003, -0x0001, 0x0 },
    { 0x0, 0x0, -0x0001, 0x0003, -0x0002, -0x0005, 0x001F, -0x004A, 0x00B3,
      -0x0192, 0x02B1, -0x039E, 0x04F8, -0x05A6, 0x7939, -0x05A6, 0x04F8,
      -0x039E, 0x02B1, -0x0192, 0x00B3, -0x004A, 0x001F, -0x0005, -0x0002,
      0x0003, -0x0001, 0x0, 0x0 },
    { 0x0, -0x0001, 0x0003, -0x0008, 0x0006, 0x0005, -0x001B, 0x00A6, -0x01A8,
      0x0372, -0x05BF, 0x09B8, -0x11B4, 0x74BB, 0x0C9D, -0x0267, -0x0043,
      0x0132, -0x00E3, 0x00A2, -0x004B, 0x003C, -0x0013, 0x0003, 0x0, -0x0002,
      0x0, 0x0, 0x0 },
    { -0x0001, 0x0003, -0x0008, 0x0011, -0x0010, 0x000A, 0x006B, -0x016D,
      0x0350, -0x0623, 0x0BCD, -0x1780, 0x6794, 0x234C, -0x0A78, 0x0400,
      -0x010A, 0x0009, 0x0034, -0x0054, 0x0041, -0x0022, 0x000A, -0x0001,
      0x0, 0x0001, 0x0, 0x0, 0x0 },
    { 0x0002, -0x0008, 0x0010, -0x0023, 0x002B, 0x001A, -0x00EB, 0x027B,
      -0x0548, 0x0AFA, -0x16FA, 0x53E0, 0x3C07, -0x1249, 0x080E, -0x0347,
      0x015B, -0x0044, -0x0017, 0x0046, -0x0023, 0x0011, -0x0005, 0x0, 0x0,
      0x0, 0x0, 0x0, 0x0 },
    { -0x0005, 0x0011, -0x0023, 0x0046, -0x0017, -0x0044, 0x015B, -0x0347,
      0x080E, -0x1249, 0x3C07, 0x53E0, -0x16FA, 0x0AFA, -0x0548, 0x027B,
      -0x00EB, 0x001A, 0x002B, -0x0023, 0x0010, -0x0008, 0x0002, 0x0, 0x0,
      0x0, 0x0, 0x0, 0x0 }
  };




/*********/
/* ESTAT */
/*********/

// Callbacks.
static PSX_Warning *_warning;
static void *_udata;
static PSX_CDCmdTrace *_cd_cmd_trace;

// Per a executar el comandament.
static void (*_run_cmd) (void);

// Índex
static int _index;

// FIFO paràmetres.
static struct
{
  uint8_t v[FIFO_SIZE];
  int     N;
} _fifop;

// FIFO resposta.
static struct
{
  uint8_t v[FIFO_SIZE];
  int     N;
  int     p;
} _fifor;

// FIFO dades.
static struct
{
  uint8_t v[MAXBUFSIZE];
  int     N;
  int     p;
} _fifod;

// timing.
static struct
{

  int cc;
  int cc_used;
  int cc2first_response;
  int cc2second_response;
  int cc2disc_inserted;
  int cc2read;
  int cc2reset;
  int cc2seek;
  int cc2irq_expired;
  int cctoEvent;
  
} _timing;

// Comandament.
static struct
{
  uint8_t cmd; // Codi
  bool    pendent; // Cal executar cmd
  int     first_response; // Pendent, ha de ficar-se quan toque.
  int     second_response; // Pendent, ha de ficar-se quan toque.
  int     irq_pendent_response; // -1 vol dir que no hi ha res.
  struct
  {
    int     N;
    uint8_t v[FIFO_SIZE];
    uint8_t set_bits;
    uint8_t reset_bits;
  }       first,second,irq_pendent; // fifor per a la primera i segona resposta
  bool    waiting_first_response;
  bool    waiting_second_response;
  bool    waiting_read;
  bool    waiting_reset;
  bool    waiting_seek;
  bool    waiting_irq_expired;
  bool    ack; // Indica que el comandament ja ha sigut 'acknwoledged'.
  bool    paused; // Flag especial per al calc_seek.
  uint8_t stat;
  // NOTES de nocash sobre l'ignore bit!!!  The "Ignore Bit" does
  // reportedly force a sector size of 2328 bytes (918h), however,
  // that doesn't seem to be true. Instead, Bit4 seems to cause the
  // controller to ignore the sector size in Bit5 (instead, the size
  // is kept from the most recent Setmode command which didn't have
  // Bit4 set). Also, Bit4 seems to cause the controller to ignore the
  // <exact> Setloc position (instead, data is randomly returned from
  // the "Setloc position minus 0..3 sectors"). And, Bit4 causes INT1
  // to return status.Bit3=set (IdError). Purpose of Bit4 is unknown?
  struct
  {
    bool double_speed;
    bool xa_adpcm_enabled;
    bool sector_size_924h_bit; // <-- Sols s'aplica si ignore bit és cert.
    bool ignore_bit;
    bool use_xa_filter;
    bool enable_report_ints;
    bool audio_pause;
    bool enable_read_cdda_sectors;
    bool sector_size_924h;
  }       mode;
  struct
  {
    int  amm; // Minut
    int  ass; // Segon
    int  asect; // Sector
    bool data_mode;
    bool processed; // Indica si s'ha processat
    enum
      {
       AFTER_SEEK_STAT= 0,
       AFTER_SEEK_READ,
       AFTER_SEEK_PLAY
      }  after; // Què es fa després del seek????
  }       seek; // Per als comandaments de seek
  struct
  {
    uint8_t file;
    uint8_t channel;
  } filter;
} _cmd;

// Interruptions.
static struct
{
  int mask;
  int v;
} _ints;

// Requests
static struct
{
  bool smen;
  bool bfwr;
  bool bfrd;
} _request;

// Per al disc.
static struct
{
  CD_Info *info;
  CD_Disc *current;
  CD_Disc *next;
  bool     inserted;
  region_t region;
} _disc;

// Buffer lectura

// NOTA!!! Intentant seguir les obsevacions de NOCASH i el que fa
// Mednafen vaig a ficar 2 nivells de buffers. El primer nivell amb
// espai per a 2 sectors, cada vegada que s'intenta insertar un sector
// que no cap passa al 2 nivell de 6. Quan es es carrega un sector en
// fifod sempre es llig el més antic del segon nivell i a continuació
// es buida.
static struct
{

  // Primer nivell
  int          p1;
  int          N1;
  raw_sector_t v1[2];

  // Segon nivell
  int      p2;
  int      N2;
  sector_t v2[NBUFS];
  
  // Altres
  uint8_t  subq[CD_SUBCH_SIZE];
  uint8_t  last_header[HEADERSIZE];
  bool     last_header_ok;
  int      counter; // Compta sectors raw llegits.
  
} _bread;

// Audio enviat a la SPU.
static struct
{

  bool    playing; // Açò sols per a audio no comprimit.
  int     track; // Track actual quan estem en mode playing.
  int     remaining_sectors; // Sectors que queden per llegir. Sols en
        		     // mode paying.
  int     total_sectors; // Número total de sectors.
  bool    mute;
  int16_t buf[0x930/2]; // L,R,L,R,L,R ....
  int     p; // Posició actual en buf
  int     inc; // Increment
  bool    backward_mode;
  
  // ADPCM
  struct
  {
    bool        demute;
    adpcm_buf_t v[ADPCM_NBUFS];
    int         current; // Posició primer buffer
    int         p; // Posició dins del buffer.
    int         N; // Nñumero de buffers plens
    int16_t     old_l,older_l,old_r,older_r;
    ringbuf_t   rbl,rbr;
  } adpcm;

  // Volume
  uint8_t tmp_vol_l2l,vol_l2l;
  uint8_t tmp_vol_l2r,vol_l2r;
  uint8_t tmp_vol_r2l,vol_r2l;
  uint8_t tmp_vol_r2r,vol_r2r;

} _audio;




/*********************/
/* FUNCIONS PRIVADES */
/*********************/


static void
update_timing_event (void)
{

  int tmp;

  
  // Actualitza cctoEvent
  _timing.cctoEvent= MAXCC;
  if ( _cmd.waiting_first_response &&
       _timing.cc2first_response < _timing.cctoEvent )
    _timing.cctoEvent= _timing.cc2first_response;
  if ( _cmd.waiting_second_response &&
       _timing.cc2second_response < _timing.cctoEvent )
    _timing.cctoEvent= _timing.cc2second_response;
  if ( _cmd.waiting_read && _timing.cc2read < _timing.cctoEvent )
    _timing.cctoEvent= _timing.cc2read;
  if ( _cmd.waiting_reset && _timing.cc2reset < _timing.cctoEvent )
    _timing.cctoEvent= _timing.cc2reset;
  if ( _cmd.waiting_seek && _timing.cc2seek < _timing.cctoEvent )
    _timing.cctoEvent= _timing.cc2seek;
  if ( _cmd.waiting_irq_expired && _timing.cc2irq_expired < _timing.cctoEvent )
    _timing.cctoEvent= _timing.cc2irq_expired;
  if ( _disc.inserted && _timing.cc2disc_inserted < _timing.cctoEvent )
    _timing.cctoEvent= _timing.cc2disc_inserted;

  // Update PSX_NextEventCC
  tmp= PSX_Clock + PSX_cd_next_event_cc ();
  if ( tmp < PSX_NextEventCC )
    PSX_NextEventCC= tmp;
  
} // end update_timing_event


static long
cdpos2long (
            const CD_Position pos
            )
{

  long ret;


  ret= ((pos.mm>>4)*10 + (pos.mm&0xF))*60*75;
  ret+= ((pos.ss>>4)*10 + (pos.ss&0xF))*75;
  ret+= (pos.sec>>4)*10 + (pos.sec&0xF);
  
  return ret;
  
} // end cdpos2long


static void
long2cdpos (
            const long  val,
            uint8_t    *mm,
            uint8_t    *ss,
            uint8_t    *sec
            )
{

  long mm_l,ss_l,sec_l,tmp;


  // Obté valors.
  mm_l= val/(60*75);
  tmp= val%(60*75);
  ss_l= tmp/75;
  sec_l= tmp%75;

  // Passa a BCD.
  *mm= (uint8_t) ((mm_l/10)*0x10 + mm_l%10);
  *ss= (uint8_t) ((ss_l/10)*0x10 + ss_l%10);
  *sec= (uint8_t) ((sec_l/10)*0x10 + sec_l%10);
  
} // long2cdpos


// RUTINA COPIADA DE MEDNAFEN!!!! Bo, no està literalment copiada però
// bàsicament fa el que fa MEDNAFEN.
static int
calc_seek_time (void)
{

  int init,ret,dist,tmp,target;
  CD_Position pos;
  

  assert ( _disc.current != NULL );
  
  ret= 0;
  
  // Si el motor està parat té un sobrecost d'un segon i s'enten que
  // el sector inicial és el 0.
  if ( (_cmd.stat&STAT_MOTOR_ON) == 0 )
    {
      ret+= PSX_CYCLES_PER_SEC; // 1 segon per a encendre
      init= 0;
    }
  else
    {
      pos= CD_disc_tell ( _disc.current );
      init= (int) cdpos2long ( pos );
    }

  // Distància.
  // NOTA!!! _cmd.seek està en decimal.
  target= _cmd.seek.amm*75*60 + _cmd.seek.ass*75 + _cmd.seek.asect;
  dist= init-target;
  if ( dist < 0 ) dist= -dist;
  
  // Cicles estándar. No entenc d'on ix açò, pero el que es veu és que
  // mednafen està assumint que en 1 segon, una vegada el motor encés,
  // el lector pot recorrer tot un CD de 72 minuts.... Em pareix un poc estrany
  // tenint en compte el que tarda en llegir, però en fi.
  // Recordem que cada minut té 60 segons i cada segon té 75 sectors.
  // Probablement açò siga cert en sectors propers.
  
  tmp= (int) ( dist * ((double) PSX_CYCLES_PER_SEC)/(72*60*75) );
  // --> 20000 seria aproximadament 191.32 sectors.
  if ( tmp < 20000 ) tmp= 20000;
  ret+= tmp;

  // En sectors molt allunyats afegim una penalització de 0.3 segons
  // per algun motiu.
  if ( dist >= 2250 )
    ret+= (int) (PSX_CYCLES_PER_SEC*0.3);
  
  // Si no és el cas però el sistema estva en pausa aleshores es fan
  // coses encara més estranyes.
  //
  // MEDNAFEN diu:
  //
  // The delay to restart from a Pause state is...very....WEIRD.  The
  // time it takes is related to the amount of time that has passed
  // since the pause, and where on the disc the laser head is, with
  // generally more time passed = longer to resume, except that
  // there's a window of time where it takes a ridiculous amount of
  // time when not much time has passed.
  // 
  // What we have here will be EXTREMELY simplified.
  else if ( _cmd.paused )
    {
      ret+= 1237952 * (_cmd.mode.double_speed ? 1 : 2);
    }

  // En mednafen hi ha un radom perquè sí, amb una implementció
  //propia... En fi vaig a emprar el rand estàndard.
  ret+= rand ()%25000;
  
  return ret;
  
} // end calc_seek_time


static void
check_irq (void)
{
  PSX_int_interruption ( PSX_INT_CDROM, (_ints.mask&_ints.v)!=0 );
  /*

  int activation;


  activation= _ints.mask&_ints.v;
  if ( (activation^_ints.old_activation)&activation )
    {
      PSX_int_interruption ( PSX_INT_CDROM, true );
      PSX_int_interruption ( PSX_INT_CDROM, false );
    }
  _ints.old_activation= activation;
  */
} // end check_irq


#ifdef PSX_BE
static void
swap_bytes (
            uint32_t  *buffer,
            const int  nwords
            )
{

  int n;
  uint32_t tmp,tmp2;
  
  
  for ( n= 0; n < nwords; ++n )
    {
      tmp= buffer[n];
      ((uint8_t *) &tmp2)[0]= ((const uint8_t *) &tmp)[3];
      ((uint8_t *) &tmp2)[1]= ((const uint8_t *) &tmp)[2];
      ((uint8_t *) &tmp2)[2]= ((const uint8_t *) &tmp)[1];
      ((uint8_t *) &tmp2)[3]= ((const uint8_t *) &tmp)[0];
      buffer[n]= tmp2;
    }
  
} // end swap_bytes
#endif


// Quan estem gastant 4bits per representar cada mostra. És
// independent de si és estéreo o mono. 8 blocks.
static void
decode_adpcm_4bit (
        	   const uint8_t *portion, // 128 byte portion
        	   const int      blk, // 0-7
        	   int16_t       *dst,
        	   int16_t       *old,
        	   int16_t       *older
        	   )
{
  
  static const double F0[]= { 0.0, 60/64.0, 115/64.0, 98/64.0 };
  static const double F1[]= { 0.0, 0.0, -52/64.0, -55/64.0 };

  
  uint8_t header;
  int shift,filter,i,sel_nibble,sel_blk;
  double f0,f1,tmp;
  int16_t sample;
  

  // Ignorem 4 bytes inicials.
  header= portion[4+blk];
  
  // Shift. Valors vàlids són 0..12 (13..15->9)
  shift= (int) (header&0xF);
  if ( shift > 12 ) shift= 9;

  // Filter.
  filter= (header>>4)&0x3;
  f0= F0[filter]; f1= F1[filter];

  // Transforma.
  sel_nibble= blk&0x1 ? 4 : 0;
  sel_blk= 16 + (blk>>1); // Ignorem els 16 bytes de la capçalera.
  for ( i= 0; i < 28; ++i )
    {
      sample= (int16_t) ((portion[sel_blk+i*4]>>sel_nibble)&0xF);
      tmp= (((int16_t) (sample<<12))>>shift) + ((*old)*f0 + (*older)*f1 + 0.5);
      if ( tmp > 32767 ) sample= 32767;
      else if ( tmp < -32768 ) sample= -32768;
      else sample= (int16_t) tmp;
      *(dst++)= sample;
      *older= *old; *old= sample;
    }
  
} // end decode_adpcm_4bit


// Quan estem gastant 8bits per representar cada mostra. És
// independent de si és estéreo o mono. 4 blocks.
static void
decode_adpcm_8bit (
        	   const uint8_t *portion, // 128 byte portion
        	   const int      blk, // 0-3
        	   int16_t       *dst,
        	   int16_t       *old,
        	   int16_t       *older
        	   )
{
  
  static const double F0[]= { 0.0, 60/64.0, 115/64.0, 98/64.0 };
  static const double F1[]= { 0.0, 0.0, -52/64.0, -55/64.0 };

  
  uint8_t header;
  int shift,filter,i;
  double f0,f1;
  int16_t sample,tmp;
  

  // Ignorem 4 bytes inicials.
  header= portion[4+blk];
  
  // Shift.
  // Valors vàlids són 0..12 (13..15->9)
  // Deurien de ser 0..8 però el 9..12 s'accepten.
  shift= (int) (header&0xF);
  if ( shift > 12 ) shift= 9;
  
  // Filter.
  filter= (header>>4)&0x3;
  f0= F0[filter]; f1= F1[filter];

  // Transforma.
  for ( i= 0; i < 28; ++i )
    {
      sample= (int16_t) portion[16+blk+i*4];
      tmp= (((int16_t) (sample<<8))>>shift) + ((*old)*f0 + (*older)*f1 + 0.5);
      if ( tmp > 32767 ) sample= 32767;
      else if ( tmp < -32768 ) sample= -32768;
      else sample= (int16_t) tmp;
      *(dst++)= sample;
      *older= *old; *old= sample;
    }
  
} // end decode_adpcm_8bit


// 18 (portions) * 28 (samples per block) * 4 (blocks) sector és un
// punter als 0x914 bytes amb les dades. NOTA!! Els últims 0x14
// s'ignoren.
static void
decode_adpcm_sector_4bit_stereo (
        			 const uint8_t *sector,
        			 int16_t        left[18*28*4],
        			 int16_t        right[18*28*4],
        			 int16_t       *old_l,
        			 int16_t       *older_l,
        			 int16_t       *old_r,
        			 int16_t       *older_r
        			 )
{

  const uint8_t *src;
  int16_t *dst_l,*dst_r;
  int i,blk;
  
  
  src= sector;
  dst_l= &(left[0]);
  dst_r= &(right[0]);
  for ( i= 0; i < 18; ++i )
    {
      for ( blk= 0; blk < 4; ++blk )
        {
          decode_adpcm_4bit ( src, 2*blk, dst_l, old_l, older_l );
          decode_adpcm_4bit ( src, 2*blk+1, dst_r, old_r, older_r );
          dst_l+= 28; dst_r+= 28; // 28 samples per block.
        }
      src+= 128; // Cada porció són 128 bytes.
    }
  
} // end decode_adpcm_sector_4bit_stereo


// 18 (portions) * 28 (samples per block) * 8 (blocks) sector és un
// punter als 0x914 bytes amb les dades. NOTA!! Els últims 0x14
// s'ignoren.
static void
decode_adpcm_sector_4bit_mono (
        		       const uint8_t *sector,
        		       int16_t        out[18*28*8],
        		       int16_t       *old,
        		       int16_t       *older
        		       )
{

  const uint8_t *src;
  int16_t *dst;
  int i,blk;
  
  
  src= sector;
  dst= &(out[0]);
  for ( i= 0; i < 18; ++i )
    {
      for ( blk= 0; blk < 8; ++blk )
        {
          decode_adpcm_4bit ( src, blk, dst, old, older );
          dst+= 28; // 28 samples per block.
        }
      src+= 128; // Cada porció són 128 bytes.
    }
  
} // end decode_adpcm_sector_4bit_mono


// 18 (portions) * 28 (samples per block) * 2 (blocks) sector és un
// punter als 0x914 bytes amb les dades. NOTA!! Els últims 0x14
// s'ignoren.
static void
decode_adpcm_sector_8bit_stereo (
        			 const uint8_t *sector,
        			 int16_t        left[18*28*2],
        			 int16_t        right[18*28*2],
        			 int16_t       *old_l,
        			 int16_t       *older_l,
        			 int16_t       *old_r,
        			 int16_t       *older_r
        			 )
{

  const uint8_t *src;
  int16_t *dst_l,*dst_r;
  int i,blk;
  
  
  src= sector;
  dst_l= &(left[0]);
  dst_r= &(right[0]);
  for ( i= 0; i < 18; ++i )
    {
      for ( blk= 0; blk < 2; ++blk )
        {
          decode_adpcm_8bit ( src, 2*blk, dst_l, old_l, older_l );
          decode_adpcm_8bit ( src, 2*blk+1, dst_r, old_r, older_r );
          dst_l+= 28; dst_r+= 28; // 28 samples per block.
        }
      src+= 128; // Cada porció són 128 bytes.
    }
  
} // end decode_adpcm_sector_8bit_stereo


// 18 (portions) * 28 (samples per block) * 4 (blocks) sector és un
// punter als 0x914 bytes amb les dades. NOTA!! Els últims 0x14
// s'ignoren.
static void
decode_adpcm_sector_8bit_mono (
        		       const uint8_t *sector,
        		       int16_t        out[18*28*4],
        		       int16_t       *old,
        		       int16_t       *older
        		       )
{

  const uint8_t *src;
  int16_t *dst;
  int i,blk;
  
  
  src= sector;
  dst= &(out[0]);
  old= older= 0;
  for ( i= 0; i < 18; ++i )
    {
      for ( blk= 0; blk < 4; ++blk )
        {
          decode_adpcm_8bit ( src, blk, dst, old, older );
          dst+= 28; // 28 samples per block.
        }
      src+= 128; // Cada porció són 128 bytes.
    }
  
} // end decode_adpcm_sector_8bit_mono


static void
adpcm_37800_to_44100 (
                      ringbuf_t     *buf,
        	      const int16_t *src,
        	      const size_t   length, // Múltiple de 6
        	      int16_t       *dst // length/6*7
        	      )
{

  int sample;
  int counter,j,k;
  size_t i;
  const int64_t *table;
  int64_t sum;
  double tmp;
  

  counter= 6;
  for ( i= 0; i < length; ++i )
    {
      buf->v[buf->p]= src[i];
      buf->p= (buf->p+1)&0x1F;
      if ( --counter == 0 )
        {
          counter= 6;
          // Cada 6 samples genera 7.
          for ( j= 0; j < 7; ++j )
            {
              table= &(adpcm_interpolate_tables[j][0]);
              sum= 0;
              for ( k= 0; k < 29; ++k )
        	sum+= ((int64_t) buf->v[(buf->p-k-1)&0x1F])*table[k];
              tmp= (sum / (double) 0x8000) + 0.5;
              if ( tmp > 32767 ) sample= 32767;
              else if ( tmp < -32768 ) sample= -32768;
              else sample= (int16_t) tmp;
              *(dst++)= sample;
            }
        }
    }
  
} // end adpcm_37800_to_44100


// Simplement duplique.
static void
adpcm_18900_to_37800 (
        	      const int16_t *src,
        	      const size_t   length,
        	      int16_t       *dst // length*2
        	      )
{

  size_t i;
  int16_t val;
  

  for ( i= 0; i < length; ++i )
    {
      val= src[i];
      dst[0]= dst[1]= val;
      dst+= 2;
    }
  
} // end adpcm_18900_to_37800


static void
decode_adpcm_sector (
        	     const uint8_t  coding_info,
        	     const uint8_t *data, // 0x914 bytes
        	     int16_t       *old_l,
        	     int16_t       *older_l,
        	     int16_t       *old_r,
        	     int16_t       *older_r
        	     )
{

  static int16_t auxl[ADPCM_MAXLEN_BUF];
  static int16_t auxr[ADPCM_MAXLEN_BUF];
  static int16_t tmp[ADPCM_MAXLEN_BUF];
  bool stereo,bps4,rate_189;
  int length;
  adpcm_buf_t *buf;
  

  // Alloc a buffer.
  if ( _audio.adpcm.N == ADPCM_NBUFS )
    {
      _warning ( _udata,
        	 "CD (Play XA-ADPCM): no empty buffers,"
        	 " discarding ADPCM sector" );
      return;
    }
  buf= &(_audio.adpcm.v[(_audio.adpcm.current+_audio.adpcm.N)%ADPCM_NBUFS]);
  ++_audio.adpcm.N;
  
  // Decode info.
  stereo= ((coding_info&0x01)!=0);
  bps4= ((coding_info&0x10)==0);
  rate_189= ((coding_info&0x04)!=0);
  if ( (coding_info&0x40) )
    printf("CD Play ADPCM - Emphasis not implemented\n");
  
  // Estéreo
  if ( stereo )
    {
      if ( bps4 )
        {
          decode_adpcm_sector_4bit_stereo ( data, auxl, auxr,
        				    old_l, older_l,
        				    old_r, older_r );
          length= 18*28*4;
        }
      else
        {
          decode_adpcm_sector_8bit_stereo ( data, auxl, auxr,
        				    old_l, older_l,
        				    old_r, older_r );
          length= 18*28*2;
        }
      if ( rate_189 )
        {
          length*= 2;
          adpcm_18900_to_37800 ( auxl, (size_t) (length/2), tmp );
          adpcm_37800_to_44100 ( &_audio.adpcm.rbl, tmp,
                                 (size_t) length, buf->left );
          adpcm_18900_to_37800 ( auxr, (size_t) (length/2), tmp );
          adpcm_37800_to_44100 ( &_audio.adpcm.rbr, tmp,
                                 (size_t) length, buf->right );
        }
      else
        {
          adpcm_37800_to_44100 ( &_audio.adpcm.rbl, auxl,
                                 (size_t) length, buf->left );
          adpcm_37800_to_44100 ( &_audio.adpcm.rbr, auxr,
                                 (size_t) length, buf->right );
        }
      buf->length= (length/6)*7;
    }

  // Mono
  else
    {
      if ( bps4 )
        {
          decode_adpcm_sector_4bit_mono ( data, auxl, old_l, older_l );
          length= 18*28*8;
        }
      else
        {
          decode_adpcm_sector_8bit_mono ( data, auxl, old_l, older_l );
          length= 18*28*4;
        }
      *old_r= *old_l; *older_r= *older_l;
      if ( rate_189 )
        {
          length*= 2;
          adpcm_18900_to_37800 ( auxl, (size_t) (length/2), tmp );
          adpcm_37800_to_44100 ( &_audio.adpcm.rbl, tmp,
                                 (size_t) length, buf->left );
        }
      else adpcm_37800_to_44100 ( &_audio.adpcm.rbl, auxl,
                                  (size_t) length, buf->left );
      buf->length= (length/6)*7;
      memcpy ( buf->right, buf->left, buf->length*sizeof(int16_t) );
    }
  
} // end decode_adpcm_sector


static void
error_wrong_number_of_parameters (void)
{

  _cmd.first.v[0]= _cmd.stat|STAT_ERROR;
  _cmd.first.v[1]= 0x20;
  _cmd.first.N= 2;
  _cmd.first_response= 5;
  
} // end error_wrong_number_of_parameters


static void
error_0x80 (void)
{

  _cmd.first.v[0]= _cmd.stat|STAT_ERROR; // <-- ????????
  _cmd.first.v[1]= 0x80;
  _cmd.first.N= 2;
  _cmd.first_response= 5;
  
} // end error_0x80

#define error_disc_missing error_0x80
#define error_playing error_0x80


static void
stop_waiting (void)
{


  // Waiting read.
  if ( _cmd.waiting_read )
    {
      _cmd.waiting_read= false;
      _timing.cc2read= 0;
    }

  // Waiting play.
  _audio.playing= false;

  // Waiting reset.
  if ( _cmd.waiting_reset )
    {
      _cmd.waiting_reset= false;
      _timing.cc2reset= 0;
    }

  // Seek.
  if ( _cmd.waiting_seek )
    {
      _cmd.waiting_seek= false;
      _timing.cc2seek= 0;
    }

  // IRQ expired
  if ( _cmd.waiting_irq_expired )
    {
      _cmd.waiting_irq_expired= false;
      _timing.cc2irq_expired= 0;
    }
  
} // end stop_waiting


static void
sync (void)
{

  if ( _fifop.N != 0 ) { error_wrong_number_of_parameters (); return; }
  stop_waiting ();
  _cmd.first.v[0]= _cmd.stat|STAT_ERROR;
  _cmd.first.v[1]= 0x40;
  _cmd.first.N= 2;
  _cmd.stat&= ~(STAT_PLAY|STAT_SEEK|STAT_READ); // <-- ???
  _cmd.first_response= 5;
  
} // end sync


static void
set_cmd_mode (
              const uint8_t data
              )
{

  _cmd.mode.double_speed= (data&0x80)!=0;
  _cmd.mode.xa_adpcm_enabled= (data&0x40)!=0;
  _cmd.mode.sector_size_924h_bit= (data&0x20)!=0;
  _cmd.mode.ignore_bit= (data&0x10)!=0;
  _cmd.mode.use_xa_filter= (data&0x08)!=0;
  _cmd.mode.enable_report_ints= (data&0x04)!=0;
  _cmd.mode.audio_pause= (data&0x02)!=0;
  _cmd.mode.enable_read_cdda_sectors= (data&0x01)!=0;
  if ( !_cmd.mode.ignore_bit )
    _cmd.mode.sector_size_924h= _cmd.mode.sector_size_924h_bit;
  
} // end set_cmd_mode


static void
set_mode (void)
{

  if ( _fifop.N != 1 ) { error_wrong_number_of_parameters (); return; }

  // First response.
  _cmd.first.v[0]= _cmd.stat;
  _cmd.first.N= 1;
  
  // Body.
  set_cmd_mode ( _fifop.v[0] );
  _cmd.first_response= 3;
  
} // end set_mode


static void
init (void)
{
  
  if ( _fifop.N != 0 ) { error_wrong_number_of_parameters (); return; }

  // First response.
  _cmd.first.v[0]= _cmd.stat;
  _cmd.first.N= 1;

  // NOTA!! Mednafen s'espera a que passe un cert temps a fer el reset
  // (en mednafen és reset). Vaig a fer el mateix.
  _cmd.first_response= 3;
  
  // Espera reset. Números de mednafen.
  if ( !_cmd.waiting_reset )
    {
      _cmd.waiting_reset= true;
      _timing.cc2reset= 1136000;
    }
  
} // end init


static void
reset (void)
{

  // Fa alguna cosa aquest comandament?!!!! Mednafen no implementa
  // res. NOCASH dona a entendre que reseteja, que és com ficar i
  // llevar el disco, pero desprès dona a entendre que igual és eixe
  // efecte però no fa res: "Executing the command produces a click
  // sound in the drive mechanics, maybe it's just a rapid motor
  // on/off, but it might something more serious, like ignoring the
  // /POS0 signal...?"
  //
  // He decidir fer un parar coses i tornar status i ja.
  
  // First step.
  _cmd.first.v[0]= _cmd.stat;
  _cmd.first.N= 1;

  // Body.
  stop_waiting ();
  if ( _disc.current != NULL )
    CD_disc_reset ( _disc.current );
  _cmd.stat&= ~(STAT_PLAY|STAT_SEEK|STAT_READ); // <-- ?????
  _bread.N1= 0;
  _bread.p1= 0;
  _bread.N2= 0;
  _bread.p2= 0;
  _bread.last_header_ok= false;
  _bread.counter= 0;
  _cmd.first_response= 3;
  
} // end reset


static void
motor_on (void)
{

  if ( _fifop.N!=0 || (_cmd.stat&STAT_MOTOR_ON) )
    {
      error_wrong_number_of_parameters ();
      return;
    }

  // First response.
  _cmd.first.v[0]= _cmd.stat;
  _cmd.first.N= 1;

  // Body.
  assert ( (_cmd.stat&(STAT_PLAY|STAT_SEEK|STAT_READ)) == 0 );
  _bread.N1= 0;
  _bread.p1= 0;
  _bread.N2= 0;
  _bread.p2= 0;
  _bread.last_header_ok= false;
  _bread.counter= 0;
  _cmd.paused= false;
  
  // Second response.
  _cmd.second.reset_bits= 0;
  _cmd.second.set_bits= STAT_MOTOR_ON;
  // --> És un poc redundant, però el response no té per què ser el
  //     stat.
  _cmd.second.v[0]= _cmd.stat|STAT_MOTOR_ON;
  _cmd.second.N= 1;
  
  // Response.
  _cmd.first_response= 3;
  _cmd.waiting_second_response= true;
  _cmd.second_response= 2;
  
  // Temps.
  // --> Inspirat per Mednafen
  _timing.cc2second_response= PSX_CYCLES_PER_SEC / 10;
  
} // end motor_on


static void
stop (void)
{

  bool stopped;

  
  if ( _fifop.N != 0 ) { error_wrong_number_of_parameters (); return; }

  // First response.
  _cmd.first.v[0]= _cmd.stat;
  _cmd.first.N= 1;

  // Body.
  stopped= (_cmd.stat&STAT_MOTOR_ON)==0;
  if ( !stopped )
    {
      stop_waiting ();
      _bread.N1= 0;
      _bread.p1= 0;
      _bread.N2= 0;
      _bread.p2= 0;
      _bread.last_header_ok= false;
      _bread.counter= 0;
    }
  else
    {
      assert ( (_cmd.stat&(STAT_PLAY|STAT_SEEK|STAT_READ)) == 0 );
    }
  if ( _disc.current != NULL )
    CD_disc_move_to_track ( _disc.current, 1 );
  _cmd.paused= false;
  
  // Second response
  _cmd.second.reset_bits= STAT_MOTOR_ON|STAT_SEEK|STAT_PLAY|STAT_READ;
  _cmd.second.set_bits= 0;
  _cmd.second.v[0]= _cmd.stat&(~(STAT_MOTOR_ON|STAT_SEEK|STAT_PLAY|STAT_READ));
  _cmd.second.N= 1;
  
  // Response.
  _cmd.first_response= 3;
  _cmd.waiting_second_response= true;
  _cmd.second_response= 2;
  
  // Temps.
  if ( stopped )
    _timing.cc2second_response= 0x0001d7b;
  else if ( _cmd.mode.double_speed )
    _timing.cc2second_response= 0x18a6076;
  else
    _timing.cc2second_response= 0x0d38aca;
  
} // end stop


static void
pause (void)
{

  bool paused;
  CD_Position pos;
  int sec;
  
  
  if ( _fifop.N != 0 ) { error_wrong_number_of_parameters (); return; }

  // First response
  _cmd.first.v[0]= _cmd.stat;
  _cmd.first.N= 1;

  // Body
  paused=
    ((_cmd.stat&STAT_MOTOR_ON)==0) ||
    ((_cmd.stat&(STAT_SEEK|STAT_READ|STAT_PLAY))==0);
  if ( !paused )
    {
      // NOTA!!!!!!!!!!!!!! Mednafen quan fa un pause retrocedeix 4
      // sectors. Crec que en realitat açò té que veure en el fet de
      // que mednafen sols avisa quan té dos sectors llegits en el
      // buffer, i quan fa un buffer buida tot. Els jocs problemàtics
      // són Bedlam, Rise 2
      //
      // Per un altre costat NOCASH diu que s'ha de repetir l'última
      // lectura en cas d'estar llegint. De moment vaig a fer açò.
      _bread.N1= 0;
      _bread.p1= 0;
      _bread.N2= 0;
      _bread.p2= 0;
      _bread.last_header_ok= false;
      _bread.counter= 0;
      sec= -1;
      // NOTA!! Millor detector que _cmd.waiting_read, perquè si està
      // a tru és que almenys ha llegit un.
      if ( _cmd.stat&STAT_READ && _disc.current != NULL )
        {
          // Al final faig el mateix que mednafen.
          pos= CD_disc_tell ( _disc.current );
          sec= (int) cdpos2long ( pos );
          sec-= _bread.counter < 4 ? _bread.counter : 4;
          if ( sec < 0 ) sec= 0;
          long2cdpos ( (long) sec, &pos.mm, &pos.ss, &pos.sec );
          if ( !CD_disc_seek ( _disc.current, BCD2DEC(pos.mm),
                               BCD2DEC(pos.ss), BCD2DEC(pos.sec) ) )
            _warning ( _udata,
                       "CD (Seek): l'operació de retrocedir 1 en 'pause'"
                       " a %d.%d.%d ha fallat",
                       pos.mm, pos.ss, pos.sec );
        }
      stop_waiting ();
      _cmd.paused= true;
    }
  
  // Second response
  _cmd.second.reset_bits= STAT_SEEK|STAT_PLAY|STAT_READ;
  _cmd.second.set_bits= 0;
  _cmd.second.v[0]= _cmd.stat&(~(STAT_SEEK|STAT_PLAY|STAT_READ));
  _cmd.second.N= 1;
  
  // Response.
  _cmd.first_response= 3;
  _cmd.waiting_second_response= true;
  _cmd.second_response= 2;
  
  // Temps. He decidit utilitzar el timing de mednafen.
  if ( paused || sec == -1 ) _timing.cc2second_response= 5000;
  else
    // Mednafe diu que és una aproximació.
    _timing.cc2second_response=
      ((1124584 + (int) (sec*42596 /(75*60)) ) *
       (_cmd.mode.double_speed ? 1 : 2));
  
  /*
  if ( paused )
    _timing.cc2second_response= 0x0001df2;
  else if ( _cmd.mode.double_speed )
    _timing.cc2second_response= 0x010bd93;
  else
    _timing.cc2second_response= 0x021181c;
  */
  
} // end pause


static void
setloc (void)
{
  
  if ( _fifop.N != 3 ) { error_wrong_number_of_parameters (); return; }
  // Realment és BCD ací ???
  _cmd.seek.amm= BCD2DEC(_fifop.v[0]);
  _cmd.seek.ass= BCD2DEC(_fifop.v[1]);
  _cmd.seek.asect= BCD2DEC(_fifop.v[2]);
  _cmd.seek.processed= false;
  _cmd.first.v[0]= _cmd.stat;
  _cmd.first.N= 1;
  
  // Response i.
  _cmd.first_response= 3;
  
} // end setloc


static void
apply_setloc (void)
{

  assert ( _disc.current != NULL );
  assert ( !_cmd.seek.processed );
  
  if ( !CD_disc_seek ( _disc.current, _cmd.seek.amm,
        	       _cmd.seek.ass, _cmd.seek.asect ) )
    _warning ( _udata,
               "CD (Seek): l'operació de 'seek' a %d.%d.%d (Mode: %s)"
               " ha fallat",
               _cmd.seek.amm, _cmd.seek.ass, _cmd.seek.asect,
               _cmd.seek.data_mode ? "dades" : "audio" );
  _cmd.seek.processed= true;
  
} // end apply_setloc


// NOTA!! no implement l'error que a vegades ocorre de fer un SeekL
// amb un track d'audio.
static void
seek (
      const bool data_mode
      )
{
  
  if ( _fifop.N != 0 ) { error_wrong_number_of_parameters (); return; }
  if ( _disc.current == NULL ) { error_disc_missing (); return; }

  // First response.
  _cmd.first.v[0]= _cmd.stat;
  _cmd.first.N= 1;

  // Body. Prepara seek.
  stop_waiting (); // ¿¿?????
  _cmd.stat|= STAT_MOTOR_ON;
  _cmd.stat&= ~(STAT_PLAY|STAT_SEEK|STAT_READ);
  _cmd.stat|= STAT_SEEK;
  _cmd.seek.data_mode= data_mode;
  _cmd.seek.after= AFTER_SEEK_STAT;
  _cmd.waiting_seek= true;
  _timing.cc2seek= calc_seek_time ();
  if ( data_mode )
    _timing.cc2seek+= (_cmd.mode.double_speed ? CC2READ_DOUBLE : CC2READ);
  // HeaderbufferValid= false ??????

  // Response.
  _cmd.first_response= 3;
  
} // end seek


// NOTA!! el tema d'active-play o play-spin-up, el que faré és que si
// està en play fare error i au.
static void
set_session (void)
{

  int sess,nsess;


  printf ( "[WW] cd.c - Cal revisar la implementació de SetSession!!\n" );

  // SetSession no està en mednafen!! En compte hi ha un comandament
  // molt estrany ReadT. Per aquest motiu he decidit no implementar el
  // seek sen SetSession i de moment deixar-ho com està.
  
  if ( _fifop.N != 1 ) { error_wrong_number_of_parameters (); return; }
  if ( _disc.current == NULL ) { error_disc_missing (); return; }
  if ( _cmd.stat&STAT_PLAY ) { error_playing (); return; }
  
  // Obté sessió i comprova que no és la 0.
  sess= _fifop.v[0];
  if ( sess == 0 )
    {
      _cmd.first.v[0]= 0x03;
      _cmd.first.v[1]= 0x10;
      _cmd.first.N= 2;
      _cmd.first_response= 5;
      return;
    }

  // Primera resposta
  _cmd.first.v[0]= _cmd.stat;
  _cmd.first.N= 1;

  // Segona resposta
  _cmd.second.reset_bits= STAT_PLAY|STAT_READ|STAT_SEEK;
  _cmd.second.set_bits= STAT_MOTOR_ON;
  nsess= CD_disc_get_num_sessions ( _disc.current );
  if ( nsess == 1 && sess != 1 )
    {
      _cmd.second.v[0]= 0x06;
      _cmd.second.v[1]= 0x40;
      _cmd.second.N= 2;
      _cmd.second_response= 5;
    }
  else if ( nsess > 1 && sess >= nsess )
    {
      _cmd.second.v[0]= 0x06;
      _cmd.second.v[1]= 0x20;
      _cmd.second.N= 2;
      _cmd.second_response= 5;
    }
  else
    {
      _cmd.second.v[0]=
        (_cmd.stat&(~(STAT_PLAY|STAT_READ|STAT_SEEK)))|STAT_MOTOR_ON;
      _cmd.second.N= 1;
      _cmd.second_response= 2;
      CD_disc_move_to_session ( _disc.current, sess );
    }
  
  // Response.
  _cmd.first_response= 3;
  _cmd.waiting_second_response= true;
  
  // Temps.
  _timing.cc2second_response=
    _cmd.mode.double_speed ?
    DEFAULT_CC_SEEK_SECOND_DOUBLE :
    DEFAULT_CC_SEEK_SECOND;
  
} // end set_session


static sector_t *
get_new_sector (void)
{

  sector_t *sec;
  int to;
  

  if ( _bread.N2 == NBUFS )
    {
      /*
      _warning ( _udata,
                 "CD (ReadNextSector): buffers interns nivell 2 complets,"
                 " es va a descartar un sector" );
      */
      _bread.p2= (_bread.p2+1)%NBUFS;
      --_bread.N2;
    }
  to= (_bread.p2+_bread.N2)%NBUFS;
  sec= &(_bread.v2[to]);
  ++_bread.N2;

  return sec;
  
} // end get_new_sector


// Força que un sector del buffer de nivell 1 passe al buffer de nivell 2.
static read_next_sector_status_t
process_sector (void)
{

  read_next_sector_status_t ret;
  int off;
  uint8_t mode;
  sector_t *sec;
  raw_sector_t *tmp;
  
  assert ( _bread.N1 > 0 );

  // Obté sector de primer nivell.
  tmp= &(_bread.v1[_bread.p1]);
  _bread.p1^= 1;
  --_bread.N1;
  
  // Processa.
  ret= READ_NEXT_SECTOR_OK;
  if ( _cmd.mode.enable_read_cdda_sectors )
    {
      if ( !tmp->audio ) ret= READ_NEXT_SECTOR_ERROR;
      else
        {
          sec= get_new_sector ();
          memcpy ( sec->data, tmp->v, 0x930 );
          sec->nbytes= 0x930;
          ret= READ_NEXT_SECTOR_OK_INT;
        }
    }
  else if ( tmp->audio ) ret= READ_NEXT_SECTOR_ERROR;
  else
    {
      memcpy ( _bread.last_header, &(tmp->v[0xC]), HEADERSIZE );
      _bread.last_header_ok= true;
      _cmd.stat&= ~(STAT_READ|STAT_SEEK|STAT_PLAY);
      _cmd.stat|= STAT_READ;
      
      if ( _bread.last_header[3]==0x02 && // Mode CD-XA
           _cmd.mode.xa_adpcm_enabled &&
           // Vaig assumir que seria &0x04 (bit audio) pero mednaden
           // sols ignora si 0x64 (audio, RT i form2) estan activats.
           ((tmp->v[0x12]&0x64)==0x64) ) // Audio, rt i form2
        {
          // Els sectors CD-XA no es desen en el buffer.
          // Però no tots es processen per el ADPCM, cal veure si
          // estem filtrant i de fer-ho si encaixa.
          if ( !_cmd.mode.use_xa_filter ||
               (_cmd.filter.file == tmp->v[0x10] &&
                _cmd.filter.channel == tmp->v[0x11]) )
            {
              // En mode CD-XA s'ignora el sector_size_924h
              decode_adpcm_sector ( tmp->v[0x13], // codinginfo
                                    &(tmp->v[0x18]), // data
                                    &_audio.adpcm.old_l,
                                    &_audio.adpcm.older_l,
                                    &_audio.adpcm.old_r,
                                    &_audio.adpcm.older_r );
            }
        }
      else if ( _cmd.mode.sector_size_924h )
        {
          sec= get_new_sector ();
          memcpy ( sec->data, &(tmp->v[0xC]), 0x924 );
          sec->nbytes= 0x924;
          ret= READ_NEXT_SECTOR_OK_INT;
        }
      else
        {
          sec= get_new_sector ();
          mode= tmp->v[0xC+3];
          off= mode==0x01 ? 0x10 : 0x18;
          memcpy ( sec->data, &(tmp->v[off]), 0x800 );
          sec->nbytes= 0x800;
          ret= READ_NEXT_SECTOR_OK_INT;
        }
    }

  return ret;
  
} // end process_sector


// NOTA!!! Açò ho faig perquè funcione PE. Bàsicament, el que ocorre
// és que abans de que aplegue un altre IRQ del CD el joc demana
// plenar el Data Fifo. En realitat sí que tinc llegits sectors per si
// de cas abans, però tinc molts dubtes que aquest siga el
// comportament esperat. Probablement el que està passant és que el
// timing no és correcte o que hi ha algun error de la CPU i està
// executant la rutina d'interrupció del CD quan no és el que tocava.
// La prova de què alguna cosa no està bé, és el fet que es plene el
// Data Fifo sense generar cap interrupció.
static bool
try_fill_buffer_l2 (void)
{

  read_next_sector_status_t ret;

  
  while ( _bread.N1 > 0 )
    {
      ret= process_sector ();
      if ( ret == READ_NEXT_SECTOR_ERROR )
        {
          _warning ( _udata,
                     "CD (TryFillBufferL2): s'ha produit un error que"
                     " s'ignorarà sense llegnerar una excepció" );
          return false;
        }
      else if ( ret == READ_NEXT_SECTOR_OK_INT )
        return true;
      // si és OK continue buscant, perquè en eixe cas no es carrega sector.
    }
  
  return false;

  
} // end try_fill_buffer_l2


static read_next_sector_status_t
read_next_sector (void)
{

  read_next_sector_status_t ret;
  bool crc_ok;
  raw_sector_t *tmp;
  uint8_t tmp_subq[CD_SUBCH_SIZE];
  
  
  assert ( _disc.current != NULL );

  // Inicialitza.
  ret= READ_NEXT_SECTOR_OK;
  
  // Fes espai si és necessari.
  if ( _bread.N1 == 2 )
    ret= process_sector ();
  
  // Llig següent sector
  tmp= &(_bread.v1[(_bread.p1+_bread.N1)&1]); // sols 2 buffers
  if ( CD_disc_read_q ( _disc.current, tmp_subq, &crc_ok, false ) &&
       CD_disc_read ( _disc.current, tmp->v, &(tmp->audio), true ) )
    {
      if ( crc_ok ) memcpy ( _bread.subq, tmp_subq, CD_SUBCH_SIZE );
      ++_bread.N1;
      ++_bread.counter;
    }
  else ret= READ_NEXT_SECTOR_ERROR;
  
  return ret;
  
} // end read_next_sector


static void
read (void)
{
  
  if ( _fifop.N != 0 ) { error_wrong_number_of_parameters (); return; }
  if ( _disc.current == NULL ) { error_disc_missing (); return; }
  // Si ja estava llegint no fa res.
  if ( _cmd.waiting_read && _cmd.seek.processed ) return;

  // First response.
  _cmd.first.v[0]= _cmd.stat;
  _cmd.first.N= 1;
  
  // Cos.
  stop_waiting ();
  _bread.N1= 0;
  _bread.p1= 0;
  _bread.N2= 0;
  _bread.p2= 0;
  _bread.last_header_ok= false;
  _bread.counter= 0;
  _cmd.stat|= STAT_MOTOR_ON;
  _cmd.stat&= ~(STAT_PLAY|STAT_SEEK|STAT_READ);
  // Pareix que fins que no llig un està en mode seek.
  _cmd.stat|= STAT_SEEK;
  if ( !_cmd.seek.processed )
    {
      _cmd.seek.data_mode= true;
      _cmd.seek.after= AFTER_SEEK_READ;
      _cmd.waiting_seek= true;
      _timing.cc2seek= calc_seek_time ();
    }
  else
    {
      _cmd.waiting_read= true;
      _timing.cc2read= (_cmd.mode.double_speed ? CC2READ_DOUBLE : CC2READ);
    }
  
  // Response.
  _cmd.first_response= 3;
  
} // end read


static void
read_toc (void)
{

  CD_Position pos_bak;
  bool bak;
  int read_toc_time;

  
  if ( _fifop.N != 0 ) { error_wrong_number_of_parameters (); return; }
  if ( _disc.current == NULL ) { error_disc_missing (); return; }

  // First response.
  _cmd.first.v[0]= _cmd.stat;
  _cmd.first.N= 1;

  // Body. prepara a seek.
  stop_waiting ();
  // Calcula temps seek El 30000000 és de mednafen....
  // Fulla.
  pos_bak.mm= _cmd.seek.amm;
  pos_bak.ss= _cmd.seek.ass;
  pos_bak.sec= _cmd.seek.asect;
  bak= _cmd.seek.processed;
  _cmd.seek.amm= 0;
  _cmd.seek.ass= 0;
  _cmd.seek.asect= 0;
  _cmd.seek.processed= false;
  read_toc_time= 30000000 + calc_seek_time ();
  _cmd.seek.amm= pos_bak.mm;
  _cmd.seek.ass= pos_bak.ss;
  _cmd.seek.asect= pos_bak.sec;
  _cmd.seek.processed= bak;
  // Paused.
  _cmd.paused= true;

  // Second response.
  _cmd.second.reset_bits= STAT_PLAY|STAT_SEEK|STAT_READ;
  _cmd.second.set_bits= STAT_MOTOR_ON;
  _cmd.second.v[0]=
    (_cmd.stat&(~(STAT_PLAY|STAT_SEEK|STAT_READ)))|STAT_MOTOR_ON;
  _cmd.second.N= 1;
  
  // Response i busy.
  _cmd.first_response= 3;
  _cmd.waiting_second_response= true;
  _cmd.second_response= 2;
  
  // Temps.
  _timing.cc2second_response= read_toc_time;
  
} // end read_toc


static void
get_stat (void)
{

  if ( _fifop.N != 0 ) { error_wrong_number_of_parameters (); return; }
  _cmd.first.v[0]= _cmd.stat;
  _cmd.stat&= ~STAT_SHELL_OPEN;
  _cmd.first.N= 1;
  
  // Response.
  _cmd.first_response= 3;
  
} // end get_stat


static void
get_param (void)
{

  if ( _fifop.N != 0 ) { error_wrong_number_of_parameters (); return; }

  // First response.
  _cmd.first.v[0]= _cmd.stat;
  _cmd.first.v[1]=
    (uint8_t)
    ((_cmd.mode.double_speed!=false)<<7) |
    ((_cmd.mode.xa_adpcm_enabled!=false)<<6) |
    ((_cmd.mode.sector_size_924h_bit!=false)<<5) |
    ((_cmd.mode.ignore_bit!=false)<<4) |
    ((_cmd.mode.use_xa_filter!=false)<<3) |
    ((_cmd.mode.enable_report_ints!=false)<<2) |
    ((_cmd.mode.audio_pause!=false)<<1) |
    (_cmd.mode.enable_read_cdda_sectors!=false);
  _cmd.first.v[2]= 0x00;
  _cmd.first.v[3]= _cmd.filter.file;
  _cmd.first.v[4]= _cmd.filter.channel;
  _cmd.first.N= 5;
  
  // Response.
  _cmd.first_response= 3;
  
} // end get_param


static void
get_loc_l (void)
{
  
  if ( _fifop.N != 0 ) { error_wrong_number_of_parameters (); return; }
  if ( _disc.current == NULL ) { error_disc_missing (); return; }
  if ( _cmd.stat&STAT_SEEK ) { error_0x80 (); return; }
  if ( !_bread.last_header_ok ) { error_0x80 (); return; }

  // First response i body.
  // Obté capçalera
  memcpy ( _cmd.first.v, _bread.last_header, HEADERSIZE );
  _cmd.first.N= HEADERSIZE;
  
  // Response.
  _cmd.first_response= 3;
  
} // end get_loc_l


static void
get_loc_p (void)
{
  
  if ( _fifop.N != 0 ) { error_wrong_number_of_parameters (); return; }
  if ( _disc.current == NULL ) { error_disc_missing (); return; }

  // First response i body.
  _cmd.first.v[0]= _bread.subq[2]; // ADR=1 -> track
  _cmd.first.v[1]= _bread.subq[3]; // ADR=1 -> index
  _cmd.first.v[2]= _bread.subq[4]; // ADR=1 -> mm
  _cmd.first.v[3]= _bread.subq[5]; // ADR=1 -> ss
  _cmd.first.v[4]= _bread.subq[6]; // ADR=1 -> sect
  _cmd.first.v[5]= _bread.subq[8]; // ADR=1 -> amm
  _cmd.first.v[6]= _bread.subq[9]; // ADR=1 -> ass
  _cmd.first.v[7]= _bread.subq[10]; // ADR=1 -> asect
  _cmd.first.N= 8;
  
  // Response.
  _cmd.first_response= 3;
  
} // end get_loc_p


static void
get_tn (void)
{

  int sess,ntracks;

  
  if ( _fifop.N != 0 ) { error_wrong_number_of_parameters (); return; }
  if ( _disc.current == NULL ) { error_disc_missing (); return; }

  // First response i body.
  // Llig.
  sess= CD_disc_get_current_session ( _disc.current );
  _cmd.first.v[0]= _cmd.stat;
  _cmd.first.v[1]= _disc.info->sessions[sess].tracks[0].id;
  ntracks= _disc.info->sessions[sess].ntracks;
  _cmd.first.v[2]= _disc.info->sessions[sess].tracks[ntracks-1].id;
  _cmd.first.N= 3;

  // Response.
  _cmd.first_response= 3;
  
} // end get_tn


static void
get_td (void)
{
  
  int track,index;
  uint8_t mm,ss;
  

  if ( _fifop.N > 1 ) { error_wrong_number_of_parameters (); return; }
  if ( _disc.current == NULL ) { error_disc_missing (); return; }

  // First response i body.
  // Llig.
  track= _fifop.N != 0 ? BCD2DEC(_fifop.v[0]) : 0;
  if ( track > _disc.info->ntracks )
    {
      _cmd.first.v[0]= _cmd.stat|STAT_ERROR;
      _cmd.first.v[1]= 0x10;
      _cmd.first.N= 2;
      _cmd.first_response= 5;
    }
  else
    {
      if ( track == 0 )
        {
          track= _disc.info->ntracks-1;
          mm= _disc.info->tracks[track].pos_last_sector.mm;
          ss= _disc.info->tracks[track].pos_last_sector.ss;
        }
      else
        {
          --track;
          assert ( _disc.info->tracks[track].nindexes > 0 );
          index= 0;
          if ( _disc.info->tracks[track].indexes[0].id == 0 &&
               _disc.info->tracks[track].nindexes > 1 )
            index= 1;
          mm= _disc.info->tracks[track].indexes[index].pos.mm;
          ss= _disc.info->tracks[track].indexes[index].pos.ss;
        }
      _cmd.first.v[0]= _cmd.stat;
      _cmd.first.v[1]= mm;
      _cmd.first.v[2]= ss;
      _cmd.first.N= 3;
      _cmd.first_response= 3;
    }
  
} // end get_td


static void
get_q (void)
{

  printf ( "CD: CAL IMPLEMENTAR GET_Q !!!!!\n" );
  /*
  uint8_t addr,point;
  uint8_t buf[CD_SUBCH_SIZE];
  bool ok;
  
  if ( _fifop.N != 2 ) { error_wrong_number_of_parameters (); return; }
  if ( _disc.current == NULL ) { error_disc_missing (); return; }
  stop_read ();
  stop_play ();
  
  // Cos.
  addr= _fifop.v[0]&0xF;
  point= _fifop.v[1];
  ok= CD_disc_move_to_leadin ( _disc.current );
  if ( !ok ) goto error;
  for (;;)
    {
      ok= CD_disc_read_q ( _disc.current, buf, true );
      if ( !ok ) goto error;
      if ( buf[1] != 0 ) goto error; // Track number
      if ( (buf[0]&0xF) == addr && buf[2] ==  point )
        break;
    }
  _cmd.first.v[0]= _cmd.stat;
  _cmd.first.N= 1;
  memcpy ( _cmd.second.v, buf, 10 );
  _cmd.second.v[10]= 0x00;
  _cmd.second.N= 11;

  // Response i busy.
  _cmd.busy= true;
  _cmd.waiting_first_response= true;
  _cmd.first_response= 3;
  _cmd.waiting_second_response= true;
  _cmd.second_response= 2;
  
  // Temps.
  _timing.cc2first_response= DEFAULT_CC_FIRST;
  _timing.cc2clear_busy= DEFAULT_CC_FIRST/CC_BUSY_FACTOR;
  _timing.cc2second_response= DEFAULT_CC_SEEK_SECOND;
  _timing.cc2second_response+= _timing.cc2first_response;
  
  return;

 error: // NOTA!!! He decidit ignorar el segon INT5 i el play del Track1
  _cmd.first.v[0]= _cmd.stat|STAT_ERROR;
  _cmd.first.N= 1;
  _cmd.second.v[0]= _cmd.stat|STAT_ERROR;
  _cmd.second.v[1]= 0x20;
  _cmd.second.N= 2;
  _cmd.busy= true;
  _cmd.waiting_first_response= true;
  _cmd.first_response= 3;
  _cmd.waiting_second_response= true;
  _cmd.second_response= 5;
  _timing.cc2first_response= DEFAULT_CC_FIRST;
  _timing.cc2clear_busy= DEFAULT_CC_FIRST/CC_BUSY_FACTOR;
  _timing.cc2second_response= DEFAULT_CC_SEEK_SECOND;
  _timing.cc2second_response+= _timing.cc2first_response;
  */
} // end get_q


static void
get_id (void)
{

  uint8_t flags;
  bool denied,err,mode2;
  
  
  if ( _fifop.N != 0 ) { error_wrong_number_of_parameters (); return; }
  if ( _disc.inserted ) { error_0x80 (); return; }
  // FALTARIA errors spin-up i detect busy pero pase.

  // First response.
  _cmd.first.v[0]= _cmd.stat;
  _cmd.first.N= 1;
  
  // Cos i segona resposta.
  denied= (_disc.current!=NULL &&
           (_disc.info->type==CD_DISK_TYPE_AUDIO ||
            _disc.info->type==CD_DISK_TYPE_UNK ||
            _disc.region==REGION_NONE) );
  _cmd.second.reset_bits= 0;
  if ( _disc.current != NULL )
    _cmd.second.set_bits= STAT_MOTOR_ON; // ?????
  else
    _cmd.second.set_bits= 0;
  _cmd.second.v[0]= _cmd.stat | (denied ? STAT_ID_ERROR : 0x00);
  flags=
    (denied?0x80:0x00) |
    (_disc.current==NULL ? 0x40 : 0x00) |
    ((_disc.current!=NULL &&
      _disc.info->type==CD_DISK_TYPE_AUDIO) ? 0x10 : 0x00);
  _cmd.second.v[1]= flags;
  mode2= (_disc.current!=NULL &&
          (_disc.info->type==CD_DISK_TYPE_MODE2 ||
           _disc.info->type==CD_DISK_TYPE_MODE2_AUDIO));
  _cmd.second.v[2]= mode2 ? 0x20 : 0x00;
  _cmd.second.v[3]= 0x00; // Usually 00h (or 8bit ATIP from Point=C0h,
        		 // if session info exists)
  
  if ( _disc.region == REGION_NONE )
    {
      _cmd.second.v[4]= 0;
      _cmd.second.v[5]= 0;
      _cmd.second.v[6]= 0;
      _cmd.second.v[7]= 0;
    }
  else
    {
      _cmd.second.v[4]= 'S';
      _cmd.second.v[5]= 'C';
      _cmd.second.v[6]= 'E';
      switch ( _disc.region )
        {
        case REGION_JAPAN:   _cmd.second.v[7]= 'I'; break;
        case REGION_AMERICA: _cmd.second.v[7]= 'A'; break;
        case REGION_EUROPE:  _cmd.second.v[7]= 'E'; break;
        default: break;
        }
    }
  _cmd.second.N= 8;
  err= _disc.current==NULL || denied;
  if ( err ) _cmd.second.v[0]|= STAT_ID_ERROR;
  
  // Response.
  _cmd.first_response= 3;
  _cmd.waiting_second_response= true;
  _cmd.second_response= err ? 5 : 2;
  
  // Temps. Valors de mednafen.
  _timing.cc2second_response= 33868;
  
} // end get_id


static void
test (void)
{

  uint8_t subcmd;

  
  if ( _fifop.N == 0 ) { error_wrong_number_of_parameters (); return; }

  // Cos
  subcmd= _fifop.v[0];
  switch ( subcmd )
    {

    case 0x20: // Get cdrom BIOS date/version (yy,mm,dd,ver)
      // NOTA!! Valors emprats en mednafen (10/01/97 v:C2)
      _cmd.first.v[0]= 0x97;
      _cmd.first.v[1]= 0x01;
      _cmd.first.v[2]= 0x10;
      _cmd.first.v[3]= 0xC2;
      _cmd.first.N= 4;
      goto set_response;
      
    case 0x06 ... 0x0F:
    case 0x1B ... 0x1F:
    case 0x26 ... 0x2F: // N/A (11h,20h when NONZERO number of params)
      _cmd.first.v[1]= _fifop.N>1 ? 0x20 : 0x10;
      goto not_available;
    case 0x30 ... 0x4F:
    case 0x51 ... 0x5F:
    case 0x61 ... 0x70:
    case 0x77 ... 0xFF: // N/A
      _cmd.first.v[1]= 0x10;
      goto not_available;
    default:
      printf("CD unknown Test subcomand: %02X\n",subcmd);
      _cmd.first.v[1]= 0x00;
      goto not_available;
      break;
    }

 not_available:
  _cmd.first.v[0]= _cmd.stat|STAT_ERROR;
  _cmd.first.N= 2;
  _cmd.first_response= 5;
  goto finish_response;
 set_response:
  _cmd.first_response= 3;
 finish_response:
  return;
  
} // end test


static void
mute (void)
{

  if ( _fifop.N != 0 ) { error_wrong_number_of_parameters (); return; }
  if ( _disc.current == NULL ) { error_disc_missing (); return; }

  // First response.
  _cmd.first.v[0]= _cmd.stat;
  _cmd.first.N= 1;

  // Body.
  _audio.mute= true;
  
  // Response.
  _cmd.first_response= 3;
  
} // end mute


static void
demute (void)
{

  if ( _fifop.N != 0 ) { error_wrong_number_of_parameters (); return; }
  if ( _disc.current == NULL ) { error_disc_missing (); return; }

  // First response.
  _cmd.first.v[0]= _cmd.stat;
  _cmd.first.N= 1;

  // Body.
  _audio.mute= false;
  
  // Response.
  _cmd.first_response= 3;
  
} // end demute


// NOTA!! Sempre es crida després d'haver llegit el sector!!!!!!
static void
play_report_ints (void)
{

  long abs_pos,track_pos;
  uint8_t amm,ass,asec;
  int16_t val,maxval;
  int maxi,i;
  
  
  assert ( _disc.current != NULL );
  
  // Fixa respotres.
  // --> Stat/track/index
  _fifor.v[0]= _cmd.stat;
  _fifor.v[1]= _audio.track;
  _fifor.v[2]= CD_disc_get_current_index ( _disc.current );
  
  // --> Posició
  abs_pos= cdpos2long ( CD_disc_tell ( _disc.current ) ) - 1; // L'últim llegit
  long2cdpos ( abs_pos, &amm, &ass, &asec );
  if ( (asec&0xF) != 0x0 ) return;
  if ( (asec>>4)%2 == 0 ) // Absolut, asect=00h,20h,40h,60h
    {
      _fifor.v[3]= amm;
      _fifor.v[4]= ass;
      _fifor.v[5]= asec;
    }
  else // Relatiu, asect=10h,30h,50h,70h
    {
      track_pos=
        _audio.track==1 ? 0 :
        cdpos2long ( _disc.info->tracks[_audio.track-2].pos_last_sector ) + 1;
      long2cdpos ( abs_pos-track_pos, &_fifor.v[3],
        	   &_fifor.v[4], &_fifor.v[5] );
      _fifor.v[4]+= 0x80;
    }
  
  // --> Peak
  maxval= _audio.buf[0]; if ( maxval < 0 ) maxval= -maxval;
  maxi= 0;
  for ( i= 1; i < 0x930/2; ++i )
    {
      val= _audio.buf[i];
      if ( val < 0 ) val= -val;
      if ( val > maxval )
        {
          maxval= val;
          maxi= i;
        }
    }
  if ( maxi%2 == 1 ) maxval|= 0x8000; // Bit L/R
  _fifor.v[6]= (uint8_t) (maxval&0xFF);
  _fifor.v[7]= (uint8_t) ((maxval>>8)&0xFF);
  // NOTA!!! Segons NOCASH el bit L/R sols es canvia al final de cada
  // lectura SUBQ, algo així com cada 10 frames. He decidit
  // actualitza-ho en cada sector.
  
  // Interrupció.
  _fifor.N= 8;
  _ints.v= (_ints.v&(~0xF)) | 1;
  _fifor.p= 0;
  check_irq ();
  
} // end play_report_ints


static void
play_read_next_sector (void)
{
  
  bool ret,is_audio;
  uint8_t buf[CD_SEC_SIZE];
  uint8_t *p,mm,ss,sec;
  int i;
  long pos;
  

  _cmd.stat|= STAT_PLAY;
  ret= CD_disc_read ( _disc.current, buf, &is_audio, true );
  if ( !ret )
    {
      _warning ( _udata,
        	 "CD (En play_read_next_sector): Error de lectura inesperat"
        	 " mentre s'intentava reproduir un sector d'àudio");
      return;
    }
  if ( _cmd.mode.enable_report_ints ) play_report_ints ();
  if ( ret && is_audio )
    for ( p= &(buf[0]), i= 0; i < 0x930/2; ++i, p+= 2 )
      _audio.buf[i]= (int16_t) ((((uint16_t) p[1])<<8) | (uint16_t) p[0]);
  else memset ( _audio.buf, 0, sizeof(_audio.buf) );
  _audio.p= 0;

  // Skip sectors. No comprova si hi han errors d'EOF
  if ( _audio.backward_mode )
    {
      pos= cdpos2long ( CD_disc_tell ( _disc.current ) ) - 1 - _audio.inc;
      long2cdpos ( pos, &mm, &ss, &sec );
      CD_disc_seek ( _disc.current, BCD2DEC(mm), BCD2DEC(ss), BCD2DEC(sec) );
    }
  else
    for ( int i= 1; i < _audio.inc; ++i )
      CD_disc_read ( _disc.current, buf, &is_audio, true );
  
} // end play_read_next_sector


static void
play_init (void)
{

  CD_Position aux;
  int cpos,epos;
  
  
  assert ( _disc.current != NULL );
  
  // Inicialitza
  _audio.playing= true;
  _audio.track= CD_disc_get_current_track ( _disc.current );
  aux= CD_disc_tell ( _disc.current );
  cpos= BCD2DEC(aux.mm)*60*75 + BCD2DEC(aux.ss)*75 + BCD2DEC(aux.sec);
  aux= _disc.info->tracks[_audio.track-1].pos_last_sector;
  epos= BCD2DEC(aux.mm)*60*75 + BCD2DEC(aux.ss)*75 + BCD2DEC(aux.sec) + 1;
  _audio.remaining_sectors= epos-cpos;
  _audio.total_sectors= epos;
  assert ( _audio.remaining_sectors > 0 );

  // Prepara següent sector.
  play_read_next_sector ();
  
} // end play_init


static void
play (void)
{

  int track;

  
  if ( _fifop.N > 1 ) { error_wrong_number_of_parameters (); return; }
  if ( _disc.current == NULL ) { error_disc_missing (); return; }
  
  // Cos.
  stop_waiting ();
  _bread.N1= 0;
  _bread.p1= 0;
  _bread.N2= 0;
  _bread.p2= 0;
  _bread.last_header_ok= false;
  _bread.counter= 0;
  _audio.p= 0;
  _audio.inc= 1;
  _audio.backward_mode= false;
  _cmd.stat|= STAT_MOTOR_ON;
  _cmd.stat&= ~(STAT_PLAY|STAT_SEEK|STAT_READ);
  // Pareix que fins que no llig un està en mode seek.
  _cmd.stat|= STAT_SEEK;
  if ( _fifop.N == 0 || _fifop.v[0] == 0x00 )
    {
      if ( !_cmd.seek.processed )
        {
          _cmd.seek.data_mode= false;
          _cmd.seek.after= AFTER_SEEK_PLAY;
          _cmd.waiting_seek= true;
          _timing.cc2seek= calc_seek_time ();
        }
      else play_init ();
    }
  else // N==1 (track)
    {
      // NOTA!! Passe d'emular el seek si s'indica la track.
      // NOTA!!! Vaig a assumir que el track fa referència als tracks
      // globals, veure nota en la capçalera.
      track= BCD2DEC(_fifop.v[0]);
      if ( track > _disc.info->ntracks )
        track= CD_disc_get_current_track ( _disc.current );
      CD_disc_move_to_track ( _disc.current, track );
      play_init ();
    }
  
  // Response.
  _cmd.first_response= 3;
  
} // end play


static void
forward (void)
{
  
  if ( _fifop.N != 0 ) { error_wrong_number_of_parameters (); return; }
  if ( _disc.current == NULL ) { error_disc_missing (); return; }
  if ( !(_cmd.stat&STAT_PLAY) || _audio.backward_mode )
    { error_playing (); return; }
  
  _cmd.first.v[0]= _cmd.stat;
  _cmd.first.N= 1;
  _audio.inc*= 8;
  
  // Response.
  _cmd.first_response= 3;
  
} // end forward


static void
backward (void)
{
  
  if ( _fifop.N != 0 ) { error_wrong_number_of_parameters (); return; }
  if ( _disc.current == NULL ) { error_disc_missing (); return; }
  if ( !(_cmd.stat&STAT_PLAY) ) { error_playing (); return; }
  
  _cmd.first.v[0]= _cmd.stat;
  _cmd.first.N= 1;
  _audio.inc*= 8;
  _audio.backward_mode= true;
  
  // Response.
  _cmd.first_response= 3;
  
} // end backward


static void
set_filter (void)
{

  if ( _fifop.N != 2 ) { error_wrong_number_of_parameters (); return; }
  if ( _disc.current == NULL ) { error_disc_missing (); return; }

  // First response.
  _cmd.first.v[0]= _cmd.stat;
  _cmd.first.N= 1;

  // Body.
  _cmd.filter.file= _fifop.v[0];
  _cmd.filter.channel= _fifop.v[1];
  // --> Açò d'inicialitzar-ho ací és una inventada.
  _audio.adpcm.old_l= _audio.adpcm.older_l= 0;
  _audio.adpcm.old_r= _audio.adpcm.older_r= 0;
  
  // Response.
  _cmd.first_response= 3;
  
} // end set_filter


// S'espera que no estiga ocupat, que hi hasca un comandament esperant.
static void
run_cmd (void)
{
  
  _cmd.pendent= false;
  _cmd.ack= false;
  switch ( _cmd.cmd )
    {
    case 0x00: sync (); break;
    case 0x01: get_stat (); break;
    case 0x02: setloc (); break;
    case 0x03: play (); break;
    case 0x04: forward (); break;
    case 0x05: backward (); break;
    case 0x06: read (); break; // ReadN
    case 0x07: motor_on (); break;
    case 0x08: stop (); break;
    case 0x09: pause (); break;
    case 0x0A: init (); break;
    case 0x0B: mute (); break;
    case 0x0C: demute (); break;
    case 0x0D: set_filter (); break;
    case 0x0E: set_mode (); break;
    case 0x0F: get_param (); break;
    case 0x10: get_loc_l (); break;
    case 0x11: get_loc_p (); break;
    case 0x12: set_session (); break;
    case 0x13: get_tn (); break;
    case 0x14: get_td (); break;
    case 0x15: seek ( true ); break; // SeekL
    case 0x16: seek ( false ); break; // SeekP

    case 0x19: test (); break;
    case 0x1A: get_id (); break;
      // NOTA!!!!!!!!! Igual he mal interpretat aquesta línia
      //
      //  ReadS - Command 1Bh --> INT3(stat) --> INT1(stat) --> datablock
      //  Read without automatic retry. Not sure what that
      //  means... does WHAT on errors? Maybe intended for continous
      //  streaming video output (to skip bad frames, rather than to
      //  interrupt the stream by performing read-retrys).
      //
      // El tema és que si em fixe en la resta d'emuladors, ReadS es
      // sol implementar com ReadN.
      //
      // Ja he entès el text!!! Es refereix a que en ReadN el lector
      // intenta rellegir un sector amb errors, mentre que ReadS passa
      // al següent!! Jo havia interpretat que ReadS sols llig el
      // següent sector!! Mala cacaua mental.
    case 0x1B: read (); break; // ReadS
    case 0x1C: reset (); break;
    case 0x1D: get_q (); break;
    case 0x1E: read_toc (); break;

    default:
      _warning ( _udata,
        	 "CD (Run CMD): comandament desconegut %02X\n",
        	 _cmd.cmd  );
    }

  // Com ara s'executa dins del clock no actualitze res.
  //update_timing_event ();
  
  // Interrupcions.
  /*
   * He decidit fer com mednafen i simplement ficar el bit a 1 quan es
   * faça un _request._smen.
  if ( _ints.command_start_enabled && !_ints.command_start && _request.smen )
    {
      PSX_int_interruption ( PSX_INT_CDROM );
      _request.smen= false; // ???? Si no és ací quan???
    }
  _ints.command_start= true;
  */
  
} // end run_cmd


static void
run_cmd_trace (void)
{

  PSX_CDCmd cmd;
  int n;
  

  cmd.cmd= _cmd.cmd;
  cmd.args.N= _fifop.N;
  for ( n= 0; n < _fifop.N; ++n )
    cmd.args.v[n]= _fifop.v[n];
  switch ( _cmd.cmd )
    {
    case 0x00: cmd.name= PSX_CD_SYNC; break;
    case 0x01: cmd.name= PSX_CD_GET_STAT; break;
    case 0x02: cmd.name= PSX_CD_SETLOC; break;
    case 0x03: cmd.name= PSX_CD_PLAY; break;
    case 0x04: cmd.name= PSX_CD_FORWARD; break;
    case 0x05: cmd.name= PSX_CD_BACKWARD; break;
    case 0x06: cmd.name= PSX_CD_READN; break;
    case 0x07: cmd.name= PSX_CD_MOTOR_ON; break;
    case 0x08: cmd.name= PSX_CD_STOP; break;
    case 0x09: cmd.name= PSX_CD_PAUSE; break;
    case 0x0A: cmd.name= PSX_CD_INIT; break;
    case 0x0B: cmd.name= PSX_CD_MUTE; break;
    case 0x0C: cmd.name= PSX_CD_DEMUTE; break;
    case 0x0D: cmd.name= PSX_CD_SET_FILTER; break;
    case 0x0E: cmd.name= PSX_CD_SET_MODE; break;
    case 0x0F: cmd.name= PSX_CD_GET_PARAM; break;
    case 0x10: cmd.name= PSX_CD_GET_LOC_L; break;
    case 0x11: cmd.name= PSX_CD_GET_LOC_P; break;
    case 0x12: cmd.name= PSX_CD_SET_SESSION; break;
    case 0x13: cmd.name= PSX_CD_GET_TN; break;
    case 0x14: cmd.name= PSX_CD_GET_TD; break;
    case 0x15: cmd.name= PSX_CD_SEEKL; break;
    case 0x16: cmd.name= PSX_CD_SEEKP; break;

    case 0x19: cmd.name= PSX_CD_TEST; break;
    case 0x1A: cmd.name= PSX_CD_GET_ID; break;
    case 0x1B: cmd.name= PSX_CD_READS; break;
    case 0x1C: cmd.name= PSX_CD_RESET; break;
    case 0x1D: cmd.name= PSX_CD_GET_Q; break;
    case 0x1E: cmd.name= PSX_CD_READ_TOC; break;
      
    default: cmd.name= PSX_CD_UNK; break;
    }
  run_cmd ();
  _cd_cmd_trace ( &cmd, _udata );
  
} // end run_cmd_trace


// Torna cert si no hi han errors.
static bool
get_region (
            CD_Disc  *disc,
            region_t *reg
            )
{

  bool audio,ret;
  uint8_t buf[CD_SEC_SIZE];
  const uint8_t *header;
  const char *data;
  int i;
  
  
  // La informacó sobre la llicència està en el quint sector del track 1.
  if ( !CD_disc_move_to_track ( disc, 1 ) )
    return false;
  for ( i= 0; i < 5; ++i )
    {
      ret= CD_disc_read ( disc, buf, &audio, true );
      if ( !ret ) return false;
    }

  // Obté la regió.
  if ( audio ) goto not_licensed;
  header= &(buf[0xC]);
  switch ( header[3] ) // Mode
    {
    case 0x1: data= (const char *) &(buf[0x10]); break;
    case 0x2: data= (const char *) &(buf[0x18]); break;
    default: goto not_licensed;
    }
  if ( strncmp ( data,
        	 "          Licensed  by          "
        	 "Sony Computer Entertainment ",
        	 32+(32-4) ) )
    goto not_licensed;
  data+= 32+(32-4); // Bota la part comú.
  if ( !strncmp ( data, "Euro pe   ", 10 ) )
    *reg= REGION_EUROPE;
  else if ( !strncmp ( data, "Inc.\n", 5 ) )
    *reg= REGION_JAPAN;
  else if ( !strncmp ( data, "Amer  ica ", 10 ) )
    *reg= REGION_AMERICA;
  else *reg= REGION_NONE;
  
  return true;

 not_licensed:
  *reg= REGION_NONE;
  return true;
  
} // end get_region


static void
register_irq (
              const int     response,
              const uint8_t res[],
              const int     N,
              const uint8_t set_bits,
              const uint8_t reset_bits
              )
{

  int n;

  
  // Si no estem esperant irq_expired es processa i s'activa el
  // irq_expired.
  if ( !_cmd.waiting_irq_expired )
    {
      _ints.v= (_ints.v&(~0xF)) | response;
      _cmd.stat&= ~(reset_bits);
      _cmd.stat|= set_bits;
      _fifor.N= N;
      for ( n= 0; n < N; ++n )
        _fifor.v[n]= res[n];
      _fifor.p= 0;
      check_irq ();
      _cmd.waiting_irq_expired= true;
      _timing.cc2irq_expired= CC2IRQ_EXPIRED;
    }

  // si hi ha un irq_expired pendent aleshores registrem el irq per
  // ser processat quan acave açò.
  else if ( _cmd.irq_pendent_response == -1 ) // No hi ha pendent
    {
      _cmd.irq_pendent_response= response;
      _cmd.irq_pendent.set_bits= set_bits;
      _cmd.irq_pendent.reset_bits= reset_bits;
      for ( n= 0; n < N; ++n )
        _cmd.irq_pendent.v[n]= res[n];
      _cmd.irq_pendent.N= N;
    }

  // S'han anidat dos irq pendents!!!
  else _warning ( _udata, "CD (register_irq): s'han anidat dos IRQ pendents."
                  " Actual: %d, nou: %d",
                  _cmd.irq_pendent_response, response );
  
} // end register_irq


static void
clock_first_response (void)
{

  _timing.cc2first_response= 0;
  _run_cmd ();
  _fifop.N= 0; // <-- He decidit buidar la fifo ací.
  register_irq ( _cmd.first_response, _cmd.first.v, _cmd.first.N, 0, 0 );
  _cmd.waiting_first_response= false;
  // ATENCIÓ!!! ACÍ NO S'INICIA UN NOU COMANDAMENT, es fa quan
  // l'ACK.
  
} // end clock_first_response


static void
clock_second_response (void)
{

  // ATENCIÓ!!! Si no s'ha fet ACK del primer ho matxaque sense
  // esperar-me al ACK.

  _timing.cc2second_response= 0;
  register_irq ( _cmd.second_response, _cmd.second.v, _cmd.second.N,
                 _cmd.second.set_bits, _cmd.second.reset_bits );
  _cmd.waiting_second_response= false;
  // ATENCIÓ!!! ACÍ NO S'INICIA UN NOU COMANDAMENT, es fa quan
  // l'ACK.
  
} // end clock_second_response


static void
clock_read (void)
{

  uint8_t buf[FIFO_SIZE];
  read_next_sector_status_t ret;
  
  
  //_timing.cc2read= 0; <-- Si té cicles negatius és important
  //                        que es tinguen en compte, perquè
  //                        la sincronització siga
  //                        perfecta.
  _timing.cc2read+= _cmd.mode.double_speed ? CC2READ_DOUBLE : CC2READ;
  
  // Llig.
  ret= read_next_sector ();
  switch ( ret )
    {
    case READ_NEXT_SECTOR_ERROR:
      //_cmd.stat&= (STAT_READ|STAT_SEEK|STAT_PLAY);
      buf[0]= _cmd.stat|STAT_ERROR; // ???
      buf[1]= 0x40;
      register_irq ( 1, buf, 2, 0, 0 );
      break;
    case READ_NEXT_SECTOR_OK_INT:
      // Açò es canvia en la primera lectura vàlida de capçalera.
      //_cmd.stat&= ~(STAT_READ|STAT_SEEK|STAT_PLAY);
      //_cmd.stat|= STAT_READ;
      buf[0]= _cmd.stat;
      register_irq ( 1, buf, 1, 0, 0 );
      break;
    case READ_NEXT_SECTOR_OK:
    default:
      break;
    }
  
} // end clock_read


static void
clock_seek (void)
{

  uint8_t buf[FIFO_SIZE];
  bool crc_ok;
  uint8_t tmp_subq[CD_SUBCH_SIZE];


  assert ( _disc.current != NULL );
  
  // Fa el seek.
  _cmd.paused= false;
  apply_setloc ();
  if ( _cmd.seek.data_mode )
    {
      if ( !CD_disc_read_q ( _disc.current, tmp_subq, &crc_ok, false ) )
        _warning ( _udata,
                   "CD (clock_seek): ha fallat el read_q" );
      else if ( crc_ok ) memcpy ( _bread.subq, tmp_subq, CD_SUBCH_SIZE );
    }
  
  // After seek.
  switch ( _cmd.seek.after )
    {
      
    case AFTER_SEEK_STAT:
      _cmd.stat&= ~STAT_SEEK;
      buf[0]= _cmd.stat;
      register_irq ( 2, buf, 1, 0, 0 );
      break;
      
    case AFTER_SEEK_READ:
      // ATENCIÓ!! Vaig a desactivar el seek en el moment de llegir el
      // primer sector.
      _cmd.waiting_read= true;
      _timing.cc2read= _cmd.mode.double_speed ? CC2READ_DOUBLE : CC2READ;
      break;

    case AFTER_SEEK_PLAY:
      _cmd.stat&= ~STAT_SEEK;
      play_init ();
      break;

    }
  
  // Actualitza
  _cmd.waiting_seek= false;
  _timing.cc2seek= 0;
  
} // end clock_seek


static void
clock_reset (void)
{

  uint8_t buf[FIFO_SIZE];

  
  // Reseteja estat.
  stop_waiting (); // Reseteja també el reset !!! no passa res xD
  set_cmd_mode ( 0x20 ); // NOCASH diu 0x00 però segons mednafen 0x20
  _cmd.stat|= STAT_MOTOR_ON;
  _cmd.stat&= ~(STAT_PLAY|STAT_SEEK|STAT_READ);
  _bread.N1= 0;
  _bread.p1= 0;
  _bread.N2= 0;
  _bread.p2= 0;
  _bread.last_header_ok= false;
  _bread.counter= 0;
  _cmd.paused= true;
  _audio.mute= false; // ¿¿¿Mednafen dubta???
  _cmd.waiting_first_response= false;
  _cmd.waiting_second_response= false;
  _cmd.pendent= false;
  _cmd.ack= true;
  if ( _disc.current != NULL )
    CD_disc_move_to_track ( _disc.current, 1 );
  _cmd.seek.amm= 0;
  _cmd.seek.ass= 0;
  _cmd.seek.asect= 0;
  _cmd.seek.data_mode= false;
  _cmd.seek.processed= false;
  
  // Torna segona resposta.
  buf[0]= _cmd.stat;
  register_irq ( 2, buf, 1, 0, 0 );
  _cmd.waiting_reset= false;
  _timing.cc2reset= 0;
  
} // end clock_reset


static void
clock_disc (void)
{

  _timing.cc2disc_inserted= 0;
  if ( _disc.info != NULL ) CD_info_free ( _disc.info );
  _disc.current= _disc.next;
  _cmd.stat&= ~STAT_MOTOR_ON; // ¿¿????
  if ( _disc.current != NULL )
    {
      _disc.info= CD_disc_get_info ( _disc.current );
      if ( _disc.info == NULL )
        {
          fprintf ( stderr,
                    "[EE] Insert CD disk, get info - cannot"
                    " allocate memory" );
          exit ( EXIT_FAILURE );
        }
      if ( !get_region ( _disc.current, &_disc.region ) )
        {
          _warning ( _udata,
                     "CD (get_region): error inesperat mentre"
                     " s'intentava llegir la regió" );
          _disc.region= REGION_NONE;
        }
      _cmd.stat|= STAT_MOTOR_ON; // ¿¿????
    }
  // ASSUMISC QUE SOLS GetStat pot resetejar aquest
  // flag. Mednafen fa el mateix.
  //_cmd.stat&= ~STAT_SHELL_OPEN; // ¿¿??
  _disc.next= NULL;
  _disc.inserted= false;
  
} // end clock_disc


static void
clock_irq_expired (void)
{

  int n;

  
  // Processa IRQ pendent si hi han.
  if ( _cmd.irq_pendent_response != -1 )
    {
      _ints.v= (_ints.v&(~0xF)) | _cmd.irq_pendent_response;
      _cmd.stat&= ~(_cmd.irq_pendent.reset_bits);
      _cmd.stat|= _cmd.irq_pendent.set_bits;
      _fifor.N= _cmd.irq_pendent.N;
      for ( n= 0; n < _cmd.irq_pendent.N; ++n )
        _fifor.v[n]= _cmd.irq_pendent.v[n];
      _fifor.p= 0;
      check_irq ();
    }
  
  // Reseteja.
  _timing.cc2irq_expired= 0;
  _cmd.waiting_irq_expired= false;
  _cmd.irq_pendent_response= -1;
  
} // end clock_irq_expired


static void
clock (
       const bool update_timing
       )
{

  int cc;
  

  // NOTA!!! Molt important les dependències entre events.
  //
  //  irq_expired     --> --
  //  read            --> read,irq_expired
  //  seek            --> read,irq_expired
  //  second_response --> irq_expired
  //  first_response  --> irq_expired,read,seek,second_response,reset
  //  reset           --> irq_expired
  //  disc            --> --
  
  
  cc= PSX_Clock-_timing.cc_used;
  assert ( cc >= 0 );
  if ( cc == 0 && _timing.cc == 0 ) return;
  else if ( cc > 0 )
    {
      _timing.cc+= cc;
      _timing.cc_used+= cc;
    }

  // NOTA!!! És important que siga el primer, perquè més avall es pot
  // activar.
  if ( _cmd.waiting_irq_expired )
    {
      _timing.cc2irq_expired-= _timing.cc;
      if ( _timing.cc2irq_expired <= 0 ) clock_irq_expired ();
    }
  
  // Lectura. NOTA!! El seek pot iniciar lectures! Millor processar
  // lectures pendents (és possible??) abans de proecessar el seek.
  if ( _cmd.waiting_read )
    {
      _timing.cc2read-= _timing.cc;
      if ( _timing.cc2read <= 0 ) clock_read ();
    }

  // Seek. NOTA!!! Important que estiga abans de first, el first pot
  // insertar seeks.
  if ( _cmd.waiting_seek )
    {
      _timing.cc2seek-= _timing.cc;
      if ( _timing.cc2seek <= 0 ) clock_seek ();
    }

  // Reset. IMPORTANT que estiga en esta posició!!!
  if ( _cmd.waiting_reset )
    {
      _timing.cc2reset-= _timing.cc;
      if ( _timing.cc2reset <= 0 ) clock_reset ();
    }
  
  // Second response.
  // NOTA!! és important processar el second response abans del
  // first_response. El first response pot clavar nous
  // second_response.
  if ( _cmd.waiting_second_response )
    {
      _timing.cc2second_response-= _timing.cc;
      if ( _timing.cc2second_response <= 0 ) clock_second_response ();
    }
  
  // First response.
  if ( _cmd.waiting_first_response )
    {
      _timing.cc2first_response-= _timing.cc;
      if ( _timing.cc2first_response <= 0 ) clock_first_response ();
    }

  // S'ha insertat un disc.
  if ( _disc.inserted )
    {
      _timing.cc2disc_inserted-= _timing.cc;
      if ( _timing.cc2disc_inserted <= 0 ) clock_disc ();
    }
  
  // Actualitza.
  _timing.cc= 0;
  if ( update_timing ) update_timing_event ();
  
} // end clock


static void
fifod_load (void)
{


  if ( _fifod.N > 0 ) return; // Sols carrega si no hi han dades pendents
  if ( _bread.N2 == 0 && !try_fill_buffer_l2 () ) 
    {
      // REVISAR TryFillBufferL2
      _warning ( _udata,
                 "CD (LoadDataFIFO): no hi han sectors carregats en memòria" );
      return;
    }
  memcpy ( _fifod.v, _bread.v2[_bread.p2].data, _bread.v2[_bread.p2].nbytes );
  _fifod.N= _bread.v2[_bread.p2].nbytes;
  _fifod.p= 0;
  _bread.p2= 0;
  _bread.N2= 0;
  
} // end fifod_load


static void
apply_volume (
              int16_t *l,
              int16_t *r
              )
{

  int32_t l2l,l2r,r2l,r2r,tmp;

  
  // NOTA!! 0x80 seria deixar el vol a 1, 0xFF doblar i 0x40 a la meitat.
  // >>7 és dividir entre 0x80
  // Aportacions.
  l2l= (((int32_t) _audio.vol_l2l)*((int32_t) *l))>>7;
  l2r= (((int32_t) _audio.vol_l2r)*((int32_t) *l))>>7;
  r2l= (((int32_t) _audio.vol_r2l)*((int32_t) *r))>>7;
  r2r= (((int32_t) _audio.vol_r2r)*((int32_t) *r))>>7;

  // Left
  tmp= l2l + r2l;
  if ( tmp > 32767 ) *l= 32767;
  else if ( tmp < -32768 ) *l= -32768;
  else *l= (int16_t) tmp;

  // Right
  tmp= l2r + r2r;
  if ( tmp > 32767 ) *r= 32767;
  else if ( tmp < -32768 ) *r= -32768;
  else *r= (int16_t) tmp;
  
} // end apply_volume




/**********************/
/* FUNCIONS PÚBLIQUES */
/**********************/

void
PSX_cd_end_iter (void)

{

  int cc;


  cc= PSX_Clock-_timing.cc_used;
  if ( cc > 0 )
    {
      _timing.cc+= cc;
      _timing.cc_used+= cc;
      if ( _timing.cc >= _timing.cctoEvent )
        clock ( true );
    }
  _timing.cc_used= 0;
  
} // end PSX_cd_end_iter


int
PSX_cd_next_event_cc (void)
{

  int ret;

  
  ret= _timing.cctoEvent - _timing.cc;
  assert ( ret >= 0 );
  
  return ret;
  
} // end PSX_cd_next_event_cc


void
PSX_cd_init (
             PSX_CDCmdTrace *cd_cmd,
             PSX_Warning    *warning,
             void           *udata
             )
{

  // Callbacks.
  _warning= warning;
  _udata= udata;
  _cd_cmd_trace= cd_cmd;

  // Índex.
  _index= 0;

  // FIFOs.
  _fifop.N= 0;
  memset ( _fifor.v, 0, sizeof(_fifor.v) );
  _fifor.N= 0;
  _fifor.p= 0;
  memset ( _fifod.v, 0, sizeof(_fifod.v) );
  _fifod.N= 0;
  _fifod.p= 0;
  
  // Interrupcions.
  memset ( &_ints, 0, sizeof(_ints) );

  // Comandaments.
  memset ( &_cmd, 0, sizeof(_cmd) );
  _cmd.ack= true;
  _cmd.stat= 0;
  _cmd.paused= false;

  // Timing.
  memset ( &_timing, 0, sizeof(_timing) );
  
  // Request.
  _request.smen= false;
  _request.bfwr= false;
  _request.bfrd= false;

  // Discs.
  _disc.current= NULL;
  _disc.next= NULL;
  _disc.inserted= false;
  _disc.info= NULL;
  _disc.region= REGION_NONE;

  // Buffer lectura
  memset ( &_bread, 0, sizeof(_bread) );
  _bread.p1= _bread.N1= 0;
  _bread.p2= _bread.N2= 0;
  _bread.last_header_ok= false;
  _bread.counter= 0;

  // Audio.
  memset ( &_audio, 0, sizeof(_audio) );

  // Vols CD.
  _audio.vol_l2l= 0x80; _audio.vol_l2r= 0x00;
  _audio.vol_r2l= 0x00; _audio.vol_r2r= 0x80;

  PSX_cd_set_mode_trace ( false );
  
} // end PSX_cd_init


void
PSX_cd_set_index (
        	  const uint8_t data
        	  )
{

  clock ( false );

  _index= data&0x3;

  update_timing_event ();
  
} // end PSX_cd_set_index


uint8_t
PSX_cd_status (void)
{

  uint8_t ret;

  
  clock ( true );
  
  ret=
    _index |
    // XA-ADPCM fifo empty  (0=Empty)  (set when playing XA-ADPCM sound)
    0 |
    // Parameter fifo empty (1=Empty)  (triggered before writing FIRST byte)
    ((_fifop.N==0)<<3) |
    // Parameter fifo full  (0=Full)   (triggered after writing 16 bytes)
    (((_fifop.N==FIFO_SIZE)?0x0:0x1)<<4) |
    // Response fifo empty  (0=Empty)  (triggered after reading LAST byte)
    ((_fifor.N!=0)<<5) |
    // Data fifo empty      (0=Empty)  (triggered after reading LAST byte)
    ((_fifod.N!=0)<<6) |
    // Command/parameter transmission busy  (1=Busy)
    ((_cmd.waiting_first_response==true)<<7)
    ;
  
  return ret;
  
} // end PSX_cd_status


void
PSX_cd_port1_write (
        	    const uint8_t data
        	    )
{

  clock ( false );
  
  switch ( _index )
    {
    case 0: // Command Register (W)
      if ( _cmd.waiting_first_response )
        {
          _warning ( _udata,
        	     "CD Port1.0 (W): s'ha intentat executar un comandament"
        	     " en la fase 'busy', per tant %02X s'ignorarà",
        	     data );
          goto update;
        }
      if ( _cmd.pendent )
        {
          _warning ( _udata,
        	     "CD Port1.0 (W): s'ha intentat executar un comandament"
        	     " quan ja hi ha un altre pendent per executar, per tant"
        	     " $02X s'ignorarà",
        	     data );
          goto update;
        }
      _cmd.pendent= true;
      _cmd.cmd= data;
      // Vaig a fer com mednafen, ací sempre es sobreescriu el comandament.
      if ( !_cmd.ack )
        {
          _warning ( _udata,
                     "CD Port1.0 (W): exectutant comandament nou sense"
                     " fer un acknowledge de l'anterior.");
        }
      if ( _cmd.waiting_first_response )
        {
          _warning ( _udata,
                     "CD Port1.0 (W): hi havia un comandament pendent"
                     " d'execució que ha sigut sobreescrit");
        }

      // Per als temps gaste la fórmula de mednafen. En realitat
      // mednafen permet que durant la fase busy s'afegisquen més
      // paràmetres, però jo de moment vaig a assumir que es fiquen
      // tots abans d'executar.
      // Amb aix vull dir 1815 cc per paràmetre.
      _cmd.waiting_first_response= true;
      _timing.cc2first_response= 10500 + rand()%3000 + 1815;
      _timing.cc2first_response+= _fifop.N * 1815;
      _timing.cc2first_response+= 8500;
      break;
    case 1: // Sound Map Data Out (W)
      printf("CD PORT1.1 W\n");
      break;
    case 2: // Sound Map Coding Info (W)
      printf("CD PORT1.2 W\n");
      break;
    case 3: // Audio Volume for Right-CD-Out to Right-SPU-Input (W)
      _audio.tmp_vol_r2r= data;
      break;
    default: break;
    }

 update:
  update_timing_event ();
  
} // end PSX_cd_port1_write


void
PSX_cd_port2_write (
        	    const uint8_t data
        	    )
{

  clock ( false );
  
  switch ( _index )
    {
    case 0: // Parameter Fifo (W)
      if ( _fifop.N == FIFO_SIZE )
        {
          _warning ( _udata,
        	     "CD Port2.0 (W): la FIFO per a paràmetres està plena" );
          goto update;
        }
      // Si aquest assert falla caldrà emular el ficar paràmetres en
      // fase busy.
      assert ( !_cmd.waiting_first_response ); 
      _fifop.v[_fifop.N++]= data;
      break;
    case 1: // Interrupt Enable Register (W)
      _ints.mask= data&0x1F;
      break;
    case 2: // Audio Volume for Left-CD-Out to Left-SPU-Input (W)
      _audio.tmp_vol_l2l= data;
      break;
    case 3: // Audio Volume for Right-CD-Out to Left-SPU-Input (W)
      _audio.tmp_vol_r2l= data;
      break;
    default: break;
    }
 update:
  update_timing_event ();
} // end PSX_cd_port2_write


void
PSX_cd_port3_write (
        	    const uint8_t data
        	    )
{

  clock ( false );
  
  switch ( _index )
    {
    case 0: // Request Register (W)
      if ( data&0x20 ) _request.smen= true;
      _request.bfwr= (data&0x40)!=0;
      _request.bfrd= (data&0x80)!=0;
      if ( _request.bfrd ) fifod_load ();
      else { _fifod.N= 0; _fifod.p= 0; }
      break;
    case 1: // Interrupt Flag Register (W)
      _ints.v&= ~data;
      check_irq ();
      if ( !(_ints.v&0x7) ) // ACK (S'executa encara que ja fora 0)
        {
          // NOTA!!! Segons NOCASH ací es deuria resetejar la FIFOR,
          // però mednafen no opina el mateix. De fet, el que veig en
          // la simulació pareix concordar més amb mednafen, així que
          // ho comente.
          //memset ( _fifor.v, 0, sizeof(_fifor.v) );
          //_fifor.N= 0;
          _cmd.ack= true;
          // NOTA!! Vaig a fer una implementació pareguda a
          // mednafen. Ací no es llancen comandaments pendents.
          /*
          if ( _cmd.pendent && !_cmd.busy &&
               !_cmd.waiting_first_response &&
               !_cmd.waiting_second_response )
            _run_cmd ();
          */
        }
      break;
    case 2: // Audio Volume for Left-CD-Out to Right-SPU-Input (W)
      _audio.tmp_vol_l2r= data;
      break;
    case 3: // Audio Volume Apply Changes (by writing bit5=1)
      _audio.adpcm.demute= ((data&0x1)==0);
      if ( data&0x20 )
        {
          _audio.vol_l2l= _audio.tmp_vol_l2l;
          _audio.vol_l2r= _audio.tmp_vol_l2r;
          _audio.vol_r2l= _audio.tmp_vol_r2l;
          _audio.vol_r2r= _audio.tmp_vol_r2r;
        }
      break;
    default: break;
    }
  update_timing_event ();
  
} // end PSX_cd_port3_write


uint8_t
PSX_cd_port1_read (void)
{

  uint8_t ret;

  
  clock ( false );
  
  ret= _fifor.v[_fifor.p];
  _fifor.p= (_fifor.p+1)%FIFO_SIZE;
  if ( --_fifor.N < 0 ) _fifor.N= 0;
  update_timing_event ();
  
  return ret;
  
} // end PSX_cd_port1_read


uint8_t
PSX_cd_port2_read (void)
{

  uint8_t ret;

  
  clock ( false );

  ret= _fifod.v[_fifod.p];
  if ( _fifod.N ) { ++_fifod.p; --_fifod.N; }
  update_timing_event ();
  
  return ret;
  
} // end PSX_cd_port2_read


uint8_t
PSX_cd_port3_read (void)
{
  
  uint8_t ret;

  
  clock ( true );

  switch ( _index )
    {
    case 0: //Interrupt Enable Register (R)
    case 2:
      ret= _ints.mask | 0xE0;
      break;
    case 1: // Interrupt Flag Register (R)
    case 3:
      ret= _ints.v | 0xE0;
      break;
    default: ret= 0xFF;
    }

  return ret;
  
} // end PSX_cd_port3_read


CD_Disc *
PSX_set_disc (
              CD_Disc *disc // Pot ser NULL
              )
{

  CD_Disc *ret;

  
  clock ( false );

  stop_waiting ();
  if ( _disc.info != NULL ) { CD_info_free ( _disc.info ); _disc.info= NULL; }
  ret= _disc.current;
  _disc.current= NULL;
  _disc.next= disc;
  _disc.inserted= true;
  _timing.cc2disc_inserted= PSX_CYCLES_PER_SEC*3;
  _cmd.stat|= STAT_SHELL_OPEN;
  update_timing_event ();
  
  return ret;
  
} // end PSX_set_disc


void
PSX_cd_set_mode_trace (
        	       const bool val
        	       )
{
  
  if ( val && _cd_cmd_trace != NULL )
    _run_cmd= run_cmd_trace;
  else
    _run_cmd= run_cmd;
      
} // end PSX_cd_set_mode_trace


bool
PSX_cd_dma_sync (
        	 const uint32_t nwords
        	 )
{
  // NOTA!! Mode0, no cal fer res.
  return true;
} // end PSX_cd_dma_sync


void
PSX_cd_dma_write (
        	  uint32_t data
        	  )
{
  _warning ( _udata, "CD (DMA3) write: el canal és sols de lectura" );
} // end PSX_cd_dma_write


uint32_t
PSX_cd_dma_read (void)
{

  uint32_t ret;
  

  clock ( false );
  
  // Comprovacions.
  if ( _fifod.N == 0 )
    {
      _warning ( _udata,
        	 "CD (DMA3): no hi han dades disponibles" );
      ret= 0xFF00FF00;
    }
  // Llig
  else if ( _fifod.N >= 4 )
    {
      ret=
        ((uint32_t) _fifod.v[_fifod.p]) |
        (((uint32_t) _fifod.v[_fifod.p+1])<<8) |
        (((uint32_t) _fifod.v[_fifod.p+2])<<16) |
        (((uint32_t) _fifod.v[_fifod.p+3])<<24);
      _fifod.p+= 4;
      _fifod.N-= 4;
    }
  else
    {
      ret= (uint32_t) _fifod.v[_fifod.p++];
      if ( --_fifod.N )
        {
          ret|= ((uint32_t) _fifod.v[_fifod.p++])<<8;
          if ( --_fifod.N )
            {
              ret|= ((uint32_t) _fifod.v[_fifod.p++])<<16;
              --_fifod.N;
            }
        }
    }
  update_timing_event ();
  
  return ret;
  
} // end PSX_cd_dma_read


void
PSX_cd_next_sound_sample (
        		  int16_t *l,
        		  int16_t *r
        		  )
{
  
  CD_Position pos;
  const adpcm_buf_t *buf;
  
  
  if ( _disc.current == NULL ||
       (!_audio.playing && !_cmd.mode.xa_adpcm_enabled) ||
       _audio.mute )
    {
      *l= *r= 0;
      return;
    }
  
   // Uncompressed CD-Audio
  if ( _audio.playing )
    {
      if ( _audio.p >= 0x930/2 ) // End of sector
        {
          if ( _cmd.mode.double_speed )
            _warning ( _udata,
        	       "CD (play): double speed not implemented" );
          if ( _audio.backward_mode && // Backward mode / Begining of track
               _audio.remaining_sectors >= _audio.total_sectors ) 
            {
              if ( _audio.track == 1 ) // Continue in play mode
        	{
        	  _audio.backward_mode= false;
        	  _audio.inc= 1;
        	  CD_disc_move_to_track ( _disc.current, 1 );
        	}
              else
        	{
        	  assert ( _audio.track > 1 );
        	  --_audio.track;
        	  pos= _disc.info->tracks[_audio.track-1].pos_last_sector;
        	  CD_disc_seek ( _disc.current, BCD2DEC(pos.mm),
        			 BCD2DEC(pos.ss), BCD2DEC(pos.sec) );
        	}
              play_init ();
            }
          else if ( !_audio.backward_mode &&
        	    _audio.remaining_sectors <= 0 ) // End of track
            {
              // Stop playing.
              if ( _cmd.mode.audio_pause ||
        	   _audio.track == _disc.info->ntracks )
        	{
        	  _cmd.stat&= ~STAT_PLAY;
        	  _fifor.v[0]= _cmd.stat;
        	  _fifor.N= 1;
                  _ints.v= (_ints.v&(~0xF)) | 4;
                  check_irq ();
        	  _audio.playing= false;
        	  // Move the position to the last sector of the
        	  // curren track.
        	  pos= _disc.info->tracks[_audio.track-1].pos_last_sector;
        	  CD_disc_seek ( _disc.current, BCD2DEC(pos.mm),
        			 BCD2DEC(pos.ss), BCD2DEC(pos.sec) );
        	  *l= *r= 0; // <-- Torna quelcom
        	  return;
        	}
              else // Continue on next track
        	{
        	  CD_disc_move_to_track ( _disc.current, _audio.track+1 );
        	  play_init ();
        	}
            }
          else
            {
              play_read_next_sector ();
              _audio.remaining_sectors-=
        	_audio.backward_mode ? -_audio.inc : _audio.inc;
            }
        }
      *l= _audio.buf[_audio.p++];
      *r= _audio.buf[_audio.p++];
      apply_volume ( l, r );
    }

  // ADPCM
  else // _cmd.mode.xa_adpcm_enabled
    {
      if ( _audio.adpcm.N == 0 ) { *l= *r= 0; return; }
      buf= &(_audio.adpcm.v[_audio.adpcm.current]);
      *l= buf->left[_audio.adpcm.p];
      *r= buf->right[_audio.adpcm.p];
      if ( ++_audio.adpcm.p == buf->length )
        {
          --_audio.adpcm.N;
          _audio.adpcm.current= (_audio.adpcm.current+1)%ADPCM_NBUFS;
          _audio.adpcm.p= 0;
        }
      if ( !_audio.adpcm.demute ) *l= *r= 0;
      else apply_volume ( l, r );
    }
  
} // end PSX_cd_next_sound_sample


void
PSX_cd_reset (void)
{

  stop_waiting ();
  
  // FIFOs
  _fifop.N= 0;
  _fifor.N= 0;
  _fifor.p= 0;
  _fifod.N= 0;
  _fifod.p= 0;

  // Comandaments.
  _cmd.ack= true;
  _cmd.stat= 0;
  _cmd.paused= false;

  // Request.
  _request.smen= false;
  _request.bfwr= false;
  _request.bfrd= false;
  
  // Buffer lectura.
  _bread.p1= _bread.N1= 0;
  _bread.p2= _bread.N2= 0;
  _bread.last_header_ok= false;
  _bread.counter= 0;

  // Vols CD.
  _audio.vol_l2l= 0x80; _audio.vol_l2r= 0x00;
  _audio.vol_r2l= 0x00; _audio.vol_r2r= 0x80;

  // Interrupcions.
  memset ( &_ints, 0, sizeof(_ints) );
  check_irq ();
  
} // end PSX_cd_reset

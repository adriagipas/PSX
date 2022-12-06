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
 *  gpu.c - Implementació del mòdul que simula el xip gràfic.
 *
 */
/*
 *  NOTES:
 *
 *  - No implemente la cache.
 *
 *  - Tinc molts dubtes de si en el texpage dels polígons cal incloure
 *    el mode textura (4bit, 8bit, etc...) i el tipus de
 *    transparència.
 *
 *  - Finalment, he decidit implementar la FIFO i que les instruccions
 *    de renderitzat tarden. Açò últim és important per què la GPU
 *    puga tardar entre sincronització i sincronització del
 *    DMA2. Anant tan apresa com puga fa per exemple que no funcione
 *    bé la BIOS. Respecte als temps, és un misteri, no estan en
 *    NOCASH, però em basaré en els de mednafen, això sí, els
 *    aproximaré amb cicles de GPU per simplificar. Per cert, al ficar
 *    temps ja té sentit que hi hasca una FIFO emulada, per exemple,
 *    ve un comandament de dibuixar un triangle i s'executa,
 *    inmediatament ve un altre i com no es pot executar s'encola,
 *    però en seguida ve un E0 i s'executa sense encolar-se.
 *
 *  - Nocash no sap si quan el display està actiu es fa Vsync o
 *    no. Aposte perque sí.
 *
 *  - No sé que és reverseflag i no l'implemente.
 *
 *  - IMPORTANT!!! Apunts sobre el bit 10 de DrawMode (Drawing to
 *    display area). En la meu primera implementació havia
 *    malinterpretat el seu significat. Havia entès que si estava
 *    desactivat no es podia dibuixar en el frame buffer. En realitat
 *    el bit parla de Display Area!! És a dir, de l'àrea del frame
 *    buffer que s'està mostrant per pantalla. De totes maneres, el
 *    mednafen, que és un emulador que estic gastant de referència
 *    quan estic molt perdut, sols el té en compte quan el Interlace
 *    està activat, i en aquest cas ho fa per a pintar línies
 *    imparells o parells (segons on estiga l'inici del display
 *    àrea). Cal recordar que en Interlace on, teoricament en cada
 *    actualització sols es mostren o les par o impar de cada frame
 *    480. Total!! que no acave de veure la importància d'emular açò
 *    amb interlace i a més és més costós, per tant, a falta de trovar
 *    un efecte gràfic que ho requerisca, de moment simplement vaig a
 *    ignorar aquest bit en tots els casos.
 */


#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "PSX.h"




/**********/
/* MACROS */
/**********/

// La grandària de la FIFO real és 16, però mednafen gasta 32 (té un
// comentari explicant per què però no acave d'entendre-ho).
#define FIFO_SIZE 32
//#define FIFO_SIZE 16

#define FB_WIDTH 1024
#define FB_HEIGHT 512

#define ACK_IRQ (_display.irq_enabled= false)

#define UNLOCK_RENDERER        			\
  if ( _renderer_locked )        		\
    {        					\
      _renderer_locked= false;        		\
      _renderer->unlock ( _renderer, _fb );        \
    }

#define LOCK_RENDERER        			\
  if ( !_renderer_locked )        		\
    {        					\
      _renderer_locked= true;        		\
      _renderer->lock ( _renderer, _fb );        \
    }

#define TORGB15b(R,G,B)                         \
  (((uint16_t) ((R)>>3)) |                      \
   (((uint16_t) ((G)>>3))<<5) |                 \
   (((uint16_t) ((B)>>3))<<10))

#define FIFO_BUF(IND) _fifo.v[(_fifo.p+(IND))%FIFO_SIZE]

#define INSERT_SHORT_CMD(CMD) \
  do {        		      \
    fifo_push ( (CMD) );      \
    ++_fifo.nactions;              \
    run_fifo_cmds ();              \
  } while(0)

#define INSERT_LONG_CMD(CMD,NWORDS) \
  do {        			    \
    fifo_push ( (CMD) );            \
    _render.state= WAIT_WORDS;            \
    _render.nwords= (NWORDS);            \
  } while(0)

#define NWORDS_FILL 2
#define NWORDS_MPOL3 3
#define NWORDS_TPOL3 6
#define NWORDS_SPOL3 5
#define NWORDS_STPOL3 8
#define NWORDS_MPOL4 4
#define NWORDS_TPOL4 8
#define NWORDS_SPOL4 7
#define NWORDS_STPOL4 11
#define NWORDS_MLINE 2
#define NWORDS_SLINE 3
#define NWORDS_MREC 1
#define NWORDS_TREC 2
#define NWORDS_MREC_VAR 2
#define NWORDS_TREC_VAR 3
#define NWORDS_VRAM2VRAM 3




/*********/
/* TIPUS */
/*********/

enum
  {
    HRES_256= 0,
    HRES_320= 1,
    HRES_512= 2,
    HRES_640= 3,
    HRES_368= 4,
    HRES_SENTINEL= 5
  };

enum
  {
    VRES_240= 0,
    VRES_480= 1,
    VRES_SENTINEL= 2
  };

enum
  {
    NTSC= 0,
    PAL= 1
  };

enum
  {
    TEX_SET_CLUT,
    TEX_SET_PAGE,
    TEX_SET_NONE
  };




/*************/
/* CONSTANTS */
/*************/

/* NOTES!!!! En realitat per a 368 deuria de ser el float
   256.0/368.0*10.0 (6.956521739130435) Però en Nocash fiquen que és
   7. :S. De moment faig cas i ho deixe amb sencers. */
static const int CYCLES_PER_PIXEL[HRES_SENTINEL]= { 10, 8, 5, 4, 7 };

// En realitat la resolució es el número de píxels que es pinten.
//static const int WIDTH[HRES_SENTINEL]= { 256, 320, 512, 640, 368 };
//static const int HEIGHT[VRES_SENTINEL]= { 240, 480 };

/*
 * NOTA!! Les línies per a NTSC i PAL són 480 (240) i 576 (288).
 */
static const int MAX_LINES[2]=
  {
    240, /* NTSC */
    288  /* PAL */
  };

// Valors copiats de mednafen
static const int FIRST_LINE_VISIBLE[2]=
  {
   16,
   20
  };

// Per als temps m'he basat en mednafen. Aquest calcula els temps en
// clocks de sistema, de fet per cada clock es fan 2 de
// renderitzat. No obstant, jo vaig a aproximar els temps a clocks de
// GPU, per tant abans de convertir-ho a clocks de GPU fare una
// correcció.
static const double RENDER_CC_CORRECTION= (11.0/7.0)/2.0;




/*********/
/* ESTAT */
/*********/

/* Callbacks. */
static PSX_Renderer *_renderer;
static PSX_Warning *_warning;
static void *_udata;

/* Frame buffer. */
static uint16_t _fb[FB_WIDTH*FB_HEIGHT];
static bool _renderer_locked;

/* Display. */
static struct
{

  bool enabled;
  bool irq_enabled;
  enum
  {
    TM_OFF= 0,
    TM_FIFO= 1,
    TM_DMA_WRITE= 2,
    TM_DMA_READ= 3
  }    transfer_mode; /* Mode de transferència. En realitat sols
        		 afecta al DMA i a al status. */
  int  x,y; /* Offset dins de fb que es dibuixa. */
  uint32_t x1,x2; /* Horizontal display range registers. */
  double screen_x0,screen_x1; /* Per a una tele 4:3, valors
        			 normalitzats [0,1]. Pot ser
        			 negatiu. */
  uint32_t y1,y2; /* Vertical display range registers. */
  double screen_y0,screen_y1; /* Per a una tele 4:3, valors
        			 normalitzats [0,1]. Pot ser
        			 negatiu. */
  int  hres;
  int  fb_line_width;
  int  vres;
  int  vres_original;
  bool vertical_interlace;
  int  interlace_field;
  bool color_depth_24bit;
  bool reverseflag;
  int  tv_mode;
  bool texture_disable;
  
} _display;

/* Estat per a renderitzar. */
static struct
{

  enum {
    WAIT_CMD= 0,
    WAIT_WORDS,
    WAIT_V1_POLY_MLINE,
    WAIT_V2_POLY_MLINE,
    WAIT_VN_POLY_MLINE,
    WAIT_C1_POLY_SLINE,
    WAIT_V1_POLY_SLINE,
    WAIT_C2_POLY_SLINE,
    WAIT_V2_POLY_SLINE,
    WAIT_CN_POLY_SLINE,
    WAIT_VN_POLY_SLINE,
    WAIT_WRITE_XY_COPY,
    WAIT_WRITE_WIDTH_HEIGHT_COPY,
    WAIT_WRITE_DATA_COPY,
    WAIT_READ_XY_COPY,
    WAIT_READ_WIDTH_HEIGHT_COPY,
    WAIT_READ_DATA_COPY
  }                state;
  int              nwords; // Paraules que s'esperen per al següent cmd.
  PSX_RendererArgs args;
  PSX_RendererArgs def_args;
  bool             drawing_da_enabled;
  bool             texture_disabled;
  int              off_x,off_y;
  uint32_t         e2_info;
  uint32_t         e3_info;
  uint32_t         e4_info;
  uint32_t         e5_info;
  bool             is_pol4;
  bool             is_poly;
  int              rec_w,rec_h; /* Rectangles, fill i copy. */
  int              min_x,max_x;
  int              min_y,max_y;
  bool             copy_mode_write;
  
} _render;


/* Estat per a la operació copy. */
static struct
{

  int x,y;
  int r,c;
  int end_r,end_c;
  
} _copy;


/* Gpuread. */
static struct
{

  uint32_t data;
  bool     vram_transfer;
  
} _read;

// FIFO
static struct
{
  uint32_t v[FIFO_SIZE];
  int      p; // Posició.
  int      N; // Número de paraules
  int      nactions; // Número d'accions esperant a ser executades.
  enum {
    FIFO_WAIT_CMD,
    FIFO_WAIT_POLY_MLINE,
    FIFO_WAIT_POLY_SLINE,
    FIFO_WAIT_READ_DATA_COPY,
    FIFO_WAIT_WRITE_DATA_COPY
  }        state;
  bool     busy;
} _fifo;

/* Per a controlar els temps. Els cicles són CPU*7. */
static struct
{

  int  cc;
  int  cc_used;
  bool enabled_VBlank;
  bool enabled_HBlank;
  bool signal_HBlank;
  int  cctoVBlankIn;
  int  cctoVBlankOut;
  int  cctoHBlankIn;
  int  cctoHBlankOut;
  int  cctoEndFrame;
  int  cctoEvent;
  int  cctoIdle; // Cicles que falten per què acabe el comandament actual.
  int  line; /* Línia actual. */
  int  ccline; /* Cicles en la línia actual. */
  int  ccperline; /* Depen de si és PAL o NTSC */
  int  nlines; /* Depen de si és PAL o NTSC */
  bool update_timing_event;
  
} _timing;

// DMA sync pendent.
static struct
{
  bool request;
} _dma_sync;

// Callbacks per als commandaments.
static PSX_GPUCmdTrace *_gpu_cmd_trace;
static void (*_gp0_cmd) (const uint32_t cmd);
static void (*_gp1_cmd) (const uint32_t cmd);
static void (*_run_fifo_cmd)(void);




/***************/
/* DEFINICIONS */
/***************/

static void
run_fifo_cmds (void);




/*********************/
/* FUNCIONS PRIVADES */
/*********************/

// Aquesta funció simplement comprova si es cumpleixen les condicions
// per a acceptar un sync o no. No fa res més. Tampoc fa comprovacions
// d'anidaments.
static bool
check_dma_sync (void)
{
  
  // Si no està en mode DMA torna cert, però sabent que s'ignorarà tot.
  if ( _display.transfer_mode == TM_OFF || _display.transfer_mode == TM_FIFO  )
    {
      _warning ( _udata,
                 "GPU (DMA2) sync: el canal està desactivat i totes les"
                 " peticions de transferència seran ignorades" );
      return true;
    }
  
  // Si és una lectura accepta.
  if ( _display.transfer_mode == TM_DMA_READ ) return true;

  // Si no està busy.
  if ( !_fifo.busy ) return true;
  /* PAREIX QUE HI HAGUENT ESPAI NO FA RES SI LA FIFO ESTA BUSY
  // Si és una escriptura accepta en les següents condicions.
  if ( // Si hi ha espai <-- En dubte !!!!!!
      ((_fifo.N+nwords) <= FIFO_SIZE) ||
      // Si la FIFO està a meitat d'una transferència o polilinia
      (_fifo.state == FIFO_WAIT_WRITE_DATA_COPY) ||
      (_fifo.state == FIFO_WAIT_POLY_MLINE) ||
      (_fifo.state == FIFO_WAIT_POLY_SLINE) ||
      // Cas no desitjable i pot comú que pot acabar mal. Si encara
      // que no cap ni estem a meitat d'una transferència, en el gp0
      // estem a meitat d'un comandament i la FIFO no està ocupada. És
      // possible que açò acabe en paraules descartades.
      (!_fifo.busy && _render.state!=WAIT_CMD &&
       _render.state!=WAIT_READ_DATA_COPY)
       )
    return true;
  */
  
  return false;
  
} // end check_dma_sync


static void
update_dma_sync (void)
{

  if ( !_dma_sync.request ) return;
  if ( check_dma_sync () )
    {
      _dma_sync.request= false;
      PSX_dma_active_channel ( 2 );
    }
  
} // end update_dma_sync


static void
update_timing_event (void)
{

  int tmp;


  if ( !_timing.update_timing_event ) return;
  
  // Actualitza cctoEvent
  _timing.cctoEvent= 0;
  if ( _fifo.busy &&
       (!_timing.cctoEvent || _timing.cctoIdle < _timing.cctoEvent) )
    _timing.cctoEvent= _timing.cctoIdle;
  if ( _timing.signal_HBlank && _timing.enabled_HBlank )
    {
      if ( !_timing.cctoEvent || _timing.cctoHBlankIn < _timing.cctoEvent )
        _timing.cctoEvent= _timing.cctoHBlankIn;
      if ( !_timing.cctoEvent || _timing.cctoHBlankOut < _timing.cctoEvent )
        _timing.cctoEvent= _timing.cctoHBlankOut;
    }
  if ( _timing.enabled_VBlank )
    {
      if ( !_timing.cctoEvent || _timing.cctoVBlankIn < _timing.cctoEvent )
        _timing.cctoEvent= _timing.cctoVBlankIn;
      if ( !_timing.cctoEvent || _timing.cctoVBlankOut < _timing.cctoEvent )
        _timing.cctoEvent= _timing.cctoVBlankOut;
    }
  else if ( !_timing.cctoEvent || _timing.cctoEndFrame < _timing.cctoEvent )
    _timing.cctoEvent= _timing.cctoEndFrame;
  
  // Update PSX_NextEventCC
  tmp= PSX_gpu_next_event_cc ();
  if ( tmp != -1 )
    {
      tmp+= PSX_Clock;
      if ( tmp < PSX_NextEventCC )
        PSX_NextEventCC= tmp;
    }

} // end update_timing_event


static void
update_timing_vblank (void)
{

  int gpucc;

  
  if ( _display.y1 >= _display.y2 || // <-- Açò no deuria de passar.
       _display.y2 >= (uint32_t) (_timing.nlines) )
    _timing.enabled_VBlank= false;
  else
    {
      assert ( _timing.line < _timing.nlines );
      _timing.enabled_VBlank= true;
      // NOTA!!! No tinc gens clar com funciona el tema del VBlank
      // ací, lo correcte és que hi haguera un número de línies
      // visibles fixes, però Nocash dona a entendre que no, que depen
      // de la zona visible.
      // In.
      if ( ((uint32_t) _timing.line) >= _display.y2 ) // Esperar pròxim frame.
        gpucc=
          ((_timing.nlines-_timing.line) + // El que falta en línies
           _display.y2 // El següent frame.
           ) * _timing.ccperline -
          _timing.ccline; // <-- Resta cicles actuals
      else
        gpucc=
          (_display.y2-_timing.line) * _timing.ccperline - _timing.ccline;
      _timing.cctoVBlankIn= gpucc*7;
      // Out.
      if ( ((uint32_t) _timing.line) >= _display.y1 ) // Esperar pròxim frame.
        gpucc=
          ((_timing.nlines-_timing.line) + // El que falta en línies
           _display.y1 // El següent frame.
           ) * _timing.ccperline -
          _timing.ccline; // <-- Resta cicles actuals
      else
        gpucc=
          (_display.y1-_timing.line) * _timing.ccperline - _timing.ccline;
      _timing.cctoVBlankOut= gpucc*7;
    }
  
} // end update_timing_vblank


static void
update_timing_hblank (void)
{

  int gpucc;

  
  if ( !_timing.signal_HBlank ||
       _display.x1 >= _display.x2 || /* <-- Açò no deuria de passar. */
       _display.x2 >= (uint32_t) (_timing.ccperline-1) )
    _timing.enabled_HBlank= false;
  else
    {
      _timing.enabled_HBlank= true;
      /* NOTA!!! No tinc gens clar com funciona el tema del HBlank
         ací, faré com el VBlank i asumiré que es genera quan estic
         fora del rang [x1,x2]. Així i tot es genera en totes les
         línies. */
      /* In. */
      if ( (uint32_t) _timing.ccline < _display.x2 ) /* Espera x2. */
        gpucc= _display.x2+1 - _timing.ccline;
      else
        gpucc= (_display.x2+1) + (_timing.ccperline-_timing.ccline);
      _timing.cctoHBlankIn= gpucc*7;
      /* Out. */
      if ( (uint32_t) _timing.ccline >= _display.x1 ) // Esperar pròxima línia.
        gpucc= (_timing.ccperline - _timing.ccline) + _display.x1;
      else
        gpucc= _display.x1 - _timing.ccline;
      _timing.cctoHBlankOut= gpucc*7;
    }
  
} /* end update_timing_hblank */


static void
update_timing_end_frame (void)
{

  int gpucc;


  gpucc= (_timing.nlines-_timing.line)*_timing.ccperline - _timing.ccline;
  _timing.cctoEndFrame= gpucc*7;
  
} /* end update_timing_end_frame */


// Es crida cada vegada que canvia algun aspecte de la configuració
// que afecte al timing.
static void
update_timing (void)
{

  // Fixa constants.
  // Frames rates segons NOCASH:
  //   PAL:  53.222400MHz/314/3406 = ca. 49.76 Hz (ie. almost 50Hz)
  //   NTSC: 53.222400MHz/263/3413 = ca. 59.29 Hz (ie. almost 60Hz)
  if ( _display.tv_mode == PAL )
    {
      _timing.nlines= 314;
      _timing.ccperline= 3406;
    }
  else
    {
      _timing.nlines= 263;
      _timing.ccperline= 3413;
    }
  
  // Actualitza línia.
  if ( _timing.ccline >= _timing.ccperline )
    {
      _timing.line+= _timing.ccline/_timing.ccperline;
      _timing.ccline%= _timing.ccperline;
    }
  if ( _timing.line >= _timing.nlines )
    _timing.line%= _timing.nlines;

  // Actualitza comptadors.
  update_timing_vblank ();
  update_timing_hblank ();
  update_timing_end_frame ();
  update_timing_event ();
  
} // end update_timing


static void
run (
     const int line_b,
     const int ccline_b,
     const int line_e,
     const int ccline_e
     )
{

  PSX_FrameGeometry g;
  
  
  // NOTA!! De moment ací sols m'encarregue de generar el frame.
  // CAL MIRAR EL TEMA DEL INTERLACE !!!!! I ALTRES COSES !!!!!!
  // PANTLLA HABILITADA, GEOMETRIA DISPLAY EN FRAME, ETC !!!!
  // ATENCIÓ!!! y2 és la primera línea fora de la pantalla.
  if ( (uint32_t) line_b < _display.y2 )
    {
      if ( (uint32_t) line_e >= _display.y2 )
        {
          // NOTA!!! Quan siga 480 vaig a mostrar tots els frames !!!!!
          if ( /*_display.vres == VRES_240 || !_timing.even_frame*/ true )
            {
              UNLOCK_RENDERER;
              g.x= _display.x; g.y= _display.y;
              //g.width= WIDTH[_display.hres];
              g.width= _display.fb_line_width;
              g.height= (_display.y2-_display.y1)<<_display.vres;
              if ( g.height > 480 ) g.height= 480;
              g.is15bit= !_display.color_depth_24bit;
              g.d_x0= _display.screen_x0; g.d_x1= _display.screen_x1;
              g.d_y0= _display.screen_y0; g.d_y1= _display.screen_y1;
              _renderer->draw ( _renderer, &g );
            }
          if ( _display.vertical_interlace ) _display.interlace_field^= 1;
          else                               _display.interlace_field= 0;
        }
    }
  
} // end run


/* En principi no és molt important però crec que mentres
   transferència de DMA pot ser que es produisca un Vsync. En
   qualsevol cas millor així. */
static void
clock (void)
{
  
  int cc,gpucc,ccused,new_line,new_ccline;
  bool update;
  

  cc= PSX_Clock-_timing.cc_used;
  if ( cc > 0 ) { _timing.cc+= 11*cc; _timing.cc_used+= cc; }
  
  /* Cicles a processar. */
  gpucc= _timing.cc/7;
  ccused= gpucc*7;
  _timing.cc%= 7;
  
  /* Actualitza comptadors. */
  _timing.cctoVBlankIn-= ccused;
  _timing.cctoVBlankOut-= ccused;
  _timing.cctoHBlankIn-= ccused;
  _timing.cctoHBlankOut-= ccused;
  _timing.cctoEndFrame-= ccused;
  
  // Comandaments.
  if ( _fifo.busy )
    {
      _timing.cctoIdle-= ccused;
      if ( _timing.cctoIdle <= 0 )
        {
          _timing.update_timing_event= false;
          _timing.cctoIdle= 0;
          _fifo.busy= false;
          run_fifo_cmds ();
          update_dma_sync ();
          _timing.update_timing_event= true;
        }
    }
  
  /* Calcula nou valors i executa. */
  new_line= _timing.line + gpucc/_timing.ccperline;
  new_ccline= _timing.ccline + gpucc%_timing.ccperline;
  if ( new_ccline >= _timing.ccperline )
    {
      ++new_line;
      new_ccline-= _timing.ccperline;
      assert ( new_ccline < _timing.ccperline );
    }
  while ( new_line >= _timing.nlines )
    {
      run ( _timing.line, _timing.ccline, _timing.nlines, 0 );
      _timing.line= 0; _timing.ccline= 0;
      new_line-= _timing.nlines;
    }
  run ( _timing.line, _timing.ccline, new_line, new_ccline );
  _timing.line= new_line;
  _timing.ccline= new_ccline;
  
  /* Processa comptadors VBlank. */
  if ( _timing.enabled_VBlank )
    {
      update= false;
      if ( _timing.cctoVBlankIn <= 0 )
        {
          update= true;
          PSX_int_interruption ( PSX_INT_VBLANK );
          PSX_timers_vblank_in ();
        }
      if ( _timing.cctoVBlankOut <= 0 )
        {
          update= true;
          PSX_timers_vblank_out ();
        }
      if ( update ) update_timing_vblank ();
    }
  
  /* Processa comptadors HBlank. */
  if ( _timing.enabled_HBlank )
    {
      update= false;
      if ( _timing.cctoHBlankIn <= 0 )
        {
          update= true;
          if ( _timing.signal_HBlank ) PSX_timers_hblank_in ();
        }
      if ( _timing.cctoHBlankOut <= 0 )
        {
          update= true;
          if ( _timing.signal_HBlank ) PSX_timers_hblank_out ();
        }
      if ( update ) update_timing_hblank ();
    }
  
  /* Fi de frame. */
  if ( _timing.cctoEndFrame <= 0 )
    update_timing_end_frame ();
  
  update_timing_event ();
  
} /* end clock */


static void
enable_display (
        	const bool enable
        	)
{
  
  _display.enabled= enable;
  _renderer->enable_display ( _renderer, enable );
  
} /* end enable_display */


static void
update_screen_x0_x1 (void)
{

  const int CCVIS= 2800; // Este número l'he tret de mednafen però no
                         // tinc gens clar que siga cert !!!!! No
                         // entenc que siga igual per a NTSC i PAL.
  int off;
  

  // Amplaria vertadera en píxels que es volen pintar.
  _display.fb_line_width=
    (((_display.x2-_display.x1)/CYCLES_PER_PIXEL[_display.hres]) + 2)&(~3);
  if ( _display.fb_line_width < 0 )
    _display.fb_line_width= 1;
  off= _display.tv_mode==PAL ? 560 : 520; // De nou números de mednafen.
  _display.screen_x0= (((double)_display.x1)-off) / CCVIS;
  _display.screen_x1= (((double)_display.x2)-off) / CCVIS;
  
  /*
  const int OFF= 0x260;

  int mwidth,width;
  

  mwidth= WIDTH[_display.hres] * CYCLES_PER_PIXEL[_display.hres];
  width= (((_display.x2-_display.x1)/CYCLES_PER_PIXEL[_display.hres]) + 2)&(~3);
  _display.screen_x0= (((double)_display.x1)-OFF) / (double) mwidth;
  _display.screen_x1= (double) width / (double) WIDTH[_display.hres];
  printf("W:%d %d %d %d\n",width,WIDTH[_display.hres],_display.x1,_display.x2);
  */
  
} // end update_screen_x0_x1


static void
set_x1_x2 (
           uint32_t cmd
           )
{

  uint32_t x1,x2;

  
  x1= cmd&0xFFF;
  x2= (cmd>>12)&0xFFF;
  if ( x1 >= x2 )
    {
      _warning ( _udata,
        	 "GPU: el valor de X1 (%lu) és major o igual que el"
        	 " de X2 (%lu), i per tant s'ignorarà",
        	 x1, x2 );
      return;
    }
  _display.x1= x1;
  _display.x2= x2;
  update_screen_x0_x1 ();
  
} /* end set_x1_x2 */


static void
set_x1_x2_cmd (
               uint32_t cmd
               )
{

  set_x1_x2 ( cmd );
  update_timing_hblank ();
  update_timing_event ();
  
} // end set_x1_x2_cmd


static void
update_screen_y0_y1 (void)
{

  int mheight;
  int firstline;

  
  mheight= MAX_LINES[_display.tv_mode] - 1;
  firstline= FIRST_LINE_VISIBLE[_display.tv_mode];
  _display.screen_y0= (((double) _display.y1) - firstline) / (double) mheight;
  _display.screen_y1=
    (((double) _display.y2) - firstline - 1) / (double) mheight;
  if ( _display.screen_y1 < 0 ) _display.screen_y1= 0;
  
} // end update_screen_y0_y1


static void
set_y1_y2 (
           uint32_t cmd
           )
{

  uint32_t y1,y2;

  
  y1= cmd&0x3FF;
  y2= (cmd>>10)&0x3FF;
  if ( y1 >= y2 )
    {
      _warning ( _udata,
        	 "GPU: el valor de Y1 (%lu) és major o igual que el"
        	 " de Y2 (%lu), i per tant s'ignorarà",
        	 y1, y2 );
      return;
    }
  _display.y1= y1;
  _display.y2= y2;
  update_screen_y0_y1 ();
  
} /* end set_y1_y2 */


static void
set_y1_y2_cmd (
               uint32_t cmd
               )
{

  set_y1_y2 ( cmd );
  update_timing_vblank ();
  update_timing_event ();
  
} // end set_y1_y2_cmd


static void
set_display_mode (
        	  const uint32_t cmd
        	  )
{
  
  // Display mode.
  _display.tv_mode= (cmd&0x8) ? PAL : NTSC;
  _display.color_depth_24bit= ((cmd&0x10)!=0);
  _display.vertical_interlace= ((cmd&0x20)!=0);
  _display.reverseflag= ((cmd&0x80)!=0);
  _display.vres_original= (cmd&0x4)>>2;
  if ( _display.vertical_interlace )
    _display.vres= _display.vres_original ? VRES_480 : VRES_240;
  else _display.vres= VRES_240;
  if ( cmd&0x40 ) _display.hres= HRES_368;
  else
    {
      switch ( cmd&0x3 )
        {
        case 0: _display.hres= HRES_256; break;
        case 1: _display.hres= HRES_320; break;
        case 2: _display.hres= HRES_512; break;
        case 3: _display.hres= HRES_640; break;
        }
    }
  PSX_timers_set_dot_gpucc ( CYCLES_PER_PIXEL[_display.hres] );
  
  // Actualitza valors derivats.
  update_screen_x0_x1 ();
  update_screen_y0_y1 ();
  update_timing ();
  
} // end set_display_mode


static void
set_draw_mode (
               const uint32_t cmd
               )
{

  _render.def_args.texpage_x= cmd&0xF;
  _render.def_args.texpage_y= (cmd>>4)&0x1;
  _render.def_args.transparency= (cmd>>5)&0x3;
  _render.def_args.texture_mode= (cmd>>7)&0x3;
  _render.def_args.dithering= (((cmd>>9)&0x1)==0x1);
  _render.drawing_da_enabled= (((cmd>>10)&0x1)==0x1);
  _render.texture_disabled= (((cmd>>11)&0x1)==0x1);
  _render.def_args.texflip_x= (((cmd>>12)&0x1)==0x1);
  _render.def_args.texflip_y= (((cmd>>13)&0x1)==0x1);
  
} /* end set_draw_mode */


static void
set_texture_window (
        	    const uint32_t cmd
        	    )
{

  uint32_t mask_x,mask_y;


  _render.e2_info= cmd&0xFFFFF;
  mask_x= (cmd&0x1F);
  mask_y= ((cmd>>5)&0x1F);
  _render.def_args.texwinmask_x= ~((uint8_t) (mask_x<<3));
  _render.def_args.texwinmask_y= ~((uint8_t) (mask_y<<3));
  _render.def_args.texwinoff_x= (uint8_t) ((((cmd>>10)&0x1F)&mask_x)<<3);
  _render.def_args.texwinoff_y= (uint8_t) ((((cmd>>15)&0x1F)&mask_y)<<3);
  
} /* end set_texture_window */


static void
set_draw_area_top_left (
        		const uint32_t cmd
        		)
{

  /* NOTA: Com són comuns a tots els fique directament en args !!! */
  
  _render.e3_info= cmd&0xFFFFF;
  _render.args.clip_x1= cmd&0x3FF;
  _render.args.clip_y1= (cmd>>10)&0x3FF;
  if ( _render.args.clip_y1 > 511 )
    _render.args.clip_y1= 511;
  
} /* end set_draw_area_top_left */


static void
set_draw_area_bottom_right (
        		    const uint32_t cmd
        		    )
{

  /* NOTA: Com són comuns a tots els fique directament en args !!! */
  
  _render.e4_info= cmd&0xFFFFF;
  _render.args.clip_x2= cmd&0x3FF;
  _render.args.clip_y2= (cmd>>10)&0x3FF;
  if ( _render.args.clip_y2 > 511 )
    _render.args.clip_y2= 511;
  
} /* end set_draw_area_bottom_right */


static void
set_drawing_offset (
        	    const uint32_t cmd
        	    )
{

  _render.e5_info= cmd&0x3FFFFF;
  _render.off_x= ((int16_t) ((uint16_t) ((cmd&0x7FF)<<5)))>>5;
  _render.off_y= ((int16_t) ((uint16_t) (((cmd>>11)&0x7FF)<<5)))>>5;
  
} /* end set_drawing_offset */


static void
set_mask_bit (
              const uint32_t cmd
              )
{

  /* NOTA!!! Com és comu a tots ho fique directament en args. */
  /* NOTA!! També ho gasten alguns comandament per a omplir. */
  
  _render.args.set_mask= ((cmd&0x1)==0x1);
  _render.args.check_mask= ((cmd&0x2)==0x2);
  
} /* end set_mask_bit */


static void
reset_render (void)
{

  /* Ací reseteje el atributs per defecte. Els valors són els de ficar
     0 en tots els atributs. */
  /* GP0(E1h..E6h) ;rendering attributes (0) */
  set_draw_mode ( 0xE1000000 );
  set_texture_window ( 0xE2000000 );
  set_draw_area_top_left ( 0xE3000000 );
  set_draw_area_bottom_right ( 0xE4000000 );
  set_drawing_offset ( 0xE5000000 );
  set_mask_bit ( 0xE6000000 );
  _render.state= WAIT_CMD; /* <-- ??? */
  
} /* end reset_render */


static void
reset_cmd_buffer (void)
{
  
  _render.state= WAIT_CMD;
  _render.nwords= 0;
  _fifo.p= 0;
  _fifo.N= 0;
  _fifo.nactions= 0;
  _fifo.state= FIFO_WAIT_CMD;
  _fifo.busy= false; // <-- ??? Inspirat per mednafen
  _timing.cctoIdle= 0; // <-- ????? Inspirat per mednafen
  update_dma_sync (); // <-- Al resetejar igual ara cap.
  update_timing_event ();
  
} // end reset_cmd_buffer


static void
reset_cmd (void)
{
  
  ACK_IRQ;
  enable_display ( false );
  _display.transfer_mode= TM_OFF;
  _display.x= 0;
  _display.y= 0;
  set_x1_x2 ( 0x200 | ((0x200 + 256*10)<<12) );
  set_y1_y2 ( 0x010 | ((0x010+240)<<10) );
  set_display_mode ( 0 ); // 0 ???!!! El nochas es contradictori amb
                          // el que diu.
  reset_render ();
  reset_cmd_buffer ();
  
} // end reset_cmd


static void
get_gpu_info (
              const uint32_t cmd
              )
{

  _read.vram_transfer= false;
  switch ( cmd&0xF )
    {
    case 0x0: /* Returns Nothing (old value in GPUREAD remains unchanged) */
    case 0x1:
      break;
    case 0x2: /* Read Texture Window setting; GP0(E2h) ;20bit/MSBs=Nothing */
      _read.data= _render.e2_info;
      break;
    case 0x3: /* Read Draw area top left; GP0(E3h) ;20bit/MSBs=Nothing */
      _read.data= _render.e3_info;
      break;
    case 0x4: /* Read Draw area bottom right; GP0(E4h) ;20bit/MSBs=Nothing */
      _read.data= _render.e4_info;
      break;
    case 0x5: /* Read Draw offset; GP0(E5h) ;22bit */
      _read.data= _render.e5_info;
      break;
    case 0x6: /* Returns Nothing (old value in GPUREAD remains unchanged) */
      break;
    case 0x7:  /* GPU Versions */
      _read.data= 2;
      break;
    case 0x8: /* Unknown (Returns 00000000h) */
      _read.data= 0;
      break;
    default: /* Returns Nothing (old value in GPUREAD remains unchanged) */
      break;
    }
  
} /* end get_gpu_info */


static void
set_vertex_xy (
               const int      v,
               const uint32_t arg
               )
{

  int x,y;


  x= _render.off_x + (((int16_t) ((uint16_t) ((arg&0x7FF)<<5)))>>5);
  y= _render.off_y + (((int16_t) ((uint16_t) (((arg>>16)&0x7FF)<<5)))>>5);
  if ( x < _render.min_x ) _render.min_x= x;
  else if ( x > _render.max_x ) _render.max_x= x;
  if ( y < _render.min_y ) _render.min_y= y;
  else if ( y > _render.max_y ) _render.max_y= y;
  _render.args.v[v].x= x;
  _render.args.v[v].y= y;
  
} /* end set_vertex_xy */


static void
set_vertex_rec (
        	const uint32_t arg
        	)
{

  _render.args.v[0].x=
    _render.off_x + (((int16_t) ((uint16_t) ((arg&0x7FF)<<5)))>>5);
  _render.args.v[0].y=
    _render.off_y + (((int16_t) ((uint16_t) (((arg>>16)&0x7FF)<<5)))>>5);
  
} /* end set_vertex_rec */


static void
set_rec_width_height (
        	      const uint32_t arg
        	      )
{

  _render.rec_w= (arg&0x3FF);
  _render.rec_h= (arg>>16)&0x1FF;
  
} /* end set_vertex_rec */


static void
set_vertex_txy (
        	const int      v,
        	const uint32_t arg,
        	const int      mode
        	)
{

  _render.args.v[v].u= (uint8_t) (arg&0xFF);
  _render.args.v[v].v= (uint8_t) ((arg>>8)&0xFF);
  if ( mode == TEX_SET_CLUT )
    {
      _render.args.texclut_x= (arg>>16)&0x3F;
      _render.args.texclut_y= (arg>>22)&0x1FF;
    }
  else if ( mode == TEX_SET_PAGE )
    {
      // NOTA!!! Aparentment açò sobreescriu els valors per
      // defecte. Jo pensava que eren locals al comandament però
      // pareix que no. En el Final Fantasy II és la diferència de que
      // en la introducció es veja bé o no el PUSH START.
      _render.def_args.texpage_x= _render.args.texpage_x= (arg>>16)&0xF;
      _render.def_args.texpage_y= _render.args.texpage_y= (arg>>20)&0x1;
      _render.def_args.transparency= (arg>>(5+16))&0x3;
      if ( _render.args.transparency != PSX_TR_NONE )
        _render.args.transparency= _render.def_args.transparency;
      _render.def_args.texture_mode=
        _render.args.texture_mode= (arg>>(7+16))&0x3;
    }
  
} // end set_vertex_txy


static void
init_maxmin_xy (void)
{
  
  _render.min_x= _render.min_y= 2000;
  _render.max_x= _render.max_y= -2000;
  
} /* end init_maxmin_xy */


static void
set_color (
           const uint32_t arg
           )
{

  _render.args.r= arg&0xFF;
  _render.args.g= (arg>>8)&0xFF;
  _render.args.b= (arg>>16)&0xFF;
  
} /* end set_color */


static void
set_vertex_color (
        	  const int      v,
        	  const uint32_t arg
        	  )
{

  _render.args.v[v].r= arg&0xFF;
  _render.args.v[v].g= (arg>>8)&0xFF;
  _render.args.v[v].b= (arg>>16)&0xFF;
  
} /* end set_vertex_color */


static void
calc_timing_draw_pol (
        	      const PSX_RendererStats *stats
        	      )
{

  int gpucc,extra;


  // NOTA: Temps un poc arreu basant-me en mednafen.

  // Base.
  gpucc= 64 + 18 + 2;
  if ( _render.args.gouraud && _render.args.texture_mode != PSX_TEX_NONE )
    extra= 150*3;
  else if ( _render.args.gouraud )
    extra= 96*3;
  else if ( _render.args.texture_mode != PSX_TEX_NONE )
    extra= 60*3;
  else extra= 0;
  gpucc+= extra;
  if ( _render.is_pol4 )
    gpucc+= extra + 28 + 18;
  
  // Línies.
  gpucc+= stats->nlines*2;
  
  // Pixels.
  if ( _render.args.gouraud || _render.args.texture_mode != PSX_TEX_NONE )
    gpucc+= stats->npixels*2;
  else if ( _render.args.transparency != PSX_TR_NONE ||
            _render.args.check_mask )
    gpucc+= (int) (stats->npixels*1.5 + 0.5); // Super aproximat !!!
  else gpucc+= stats->npixels;

  // Fixa.
  /*
  if ( !_render.drawing_da_enabled && _display.vres == VRES_480 )
    gpucc/= 2; // <-- Inventada meua, basada en que sosl es dibuixen la meitat.
  */
  _timing.cctoIdle+= 7 * ((int) (gpucc*RENDER_CC_CORRECTION + 0.5));
  _fifo.busy= (_timing.cctoIdle>0);
  update_timing_event ();
  
} // end calc_timing_draw_pol


static void
draw_mpol (void)
{

  PSX_RendererStats stats;

  
  if ( (_render.max_x-_render.min_x) > 1023 ||
       (_render.max_y-_render.min_y) > 511 )
    return;
  
  _render.args.gouraud= false;
  _render.args.texture_mode= PSX_TEX_NONE;
  _render.args.dithering= false; // No afecta polígons mono !!!!
  UNLOCK_RENDERER;
  if ( _render.is_pol4 ) _renderer->pol4 ( _renderer, &(_render.args), &stats );
  else                   _renderer->pol3 ( _renderer, &(_render.args), &stats );

  // Timing.
  calc_timing_draw_pol ( &stats        );
  
} // end draw_mpol


static void
draw_tpol (void)
{

  PSX_RendererStats stats;

  
  if ( (_render.max_x-_render.min_x) > 1023 ||
       (_render.max_y-_render.min_y) > 511 )
    return;
  
  _render.args.gouraud= false;
  _render.args.dithering= _render.def_args.dithering;
  _render.args.texwinmask_x= _render.def_args.texwinmask_x;
  _render.args.texwinmask_y= _render.def_args.texwinmask_y;
  _render.args.texwinoff_x= _render.def_args.texwinoff_x;
  _render.args.texwinoff_y= _render.def_args.texwinoff_y;
  _render.args.texflip_x= false;
  _render.args.texflip_y= false;
  if ( _display.texture_disable && _render.texture_disabled )
    _render.args.texture_mode= PSX_TEX_NONE;
  /*
  else // <-- ¿¿Cal comentar?? Depen de que inclou TEXPAGE
    _render.args.texture_mode= _render.def_args.texture_mode;
  */
  UNLOCK_RENDERER;
  if ( _render.is_pol4 ) _renderer->pol4 ( _renderer, &(_render.args), &stats );
  else                   _renderer->pol3 ( _renderer, &(_render.args), &stats );

  // Timing.
  calc_timing_draw_pol ( &stats        );
  
} // end draw_tpol


static void
draw_stpol (void)
{
  
  PSX_RendererStats stats;
  
  
  if ( (_render.max_x-_render.min_x) > 1023 ||
       (_render.max_y-_render.min_y) > 511 )
    return;
  
  _render.args.gouraud= true;
  _render.args.dithering= _render.def_args.dithering;
  _render.args.texwinmask_x= _render.def_args.texwinmask_x;
  _render.args.texwinmask_y= _render.def_args.texwinmask_y;
  _render.args.texwinoff_x= _render.def_args.texwinoff_x;
  _render.args.texwinoff_y= _render.def_args.texwinoff_y;
  _render.args.texflip_x= false;
  _render.args.texflip_y= false;
  if ( _display.texture_disable && _render.texture_disabled )
    _render.args.texture_mode= PSX_TEX_NONE;
  /*
  else // ¿¿¿Cal comentar??? Depén de què inclou TEXPAGE.
    _render.args.texture_mode= _render.def_args.texture_mode;
  */
  UNLOCK_RENDERER;
  if ( _render.is_pol4 ) _renderer->pol4 ( _renderer, &(_render.args), &stats );
  else                   _renderer->pol3 ( _renderer, &(_render.args), &stats );

  // Timing.
  calc_timing_draw_pol ( &stats        );
  
} // end draw_stpol


static void
draw_spol (void)
{

  PSX_RendererStats stats;

  
  if ( (_render.max_x-_render.min_x) > 1023 ||
       (_render.max_y-_render.min_y) > 511 )
    return;
  
  _render.args.gouraud= true;
  _render.args.texture_mode= PSX_TEX_NONE;
  _render.args.dithering= _render.def_args.dithering;
  UNLOCK_RENDERER;
  if ( _render.is_pol4 ) _renderer->pol4 ( _renderer, &(_render.args), &stats );
  else                   _renderer->pol3 ( _renderer, &(_render.args), &stats );

  // Timing.
  calc_timing_draw_pol ( &stats        );
  
} // end draw_spol


static void
prepare_next_line (void)
{

  _render.args.v[0]= _render.args.v[1];
  _render.min_x= _render.max_x= _render.args.v[0].x;
  _render.min_y= _render.max_y= _render.args.v[0].y;
  
} /* end prepare_next_line */


static void
calc_timing_draw_line (
        	       const PSX_RendererStats *stats
        	       )
{

  int gpucc;


  // NOTA: Temps un poc arreu basant-me en mednafen.

  gpucc= 2 + 16 + stats->npixels*2;
  /*
  if ( !_render.drawing_da_enabled && _display.vres == VRES_480 )
    gpucc/= 2; // <-- Inventada meua, basada en que sosl es dibuixen la meitat.
  */
  _timing.cctoIdle+= 7 * ((int) (gpucc*RENDER_CC_CORRECTION));
  _fifo.busy= (_timing.cctoIdle>0);
  update_timing_event ();
  
} // end calc_timing_draw_line


static void
draw_mline (void)
{
  
  PSX_RendererStats stats;

  
  if ( (_render.max_x-_render.min_x) > 1023 ||
       (_render.max_y-_render.min_y) > 511 )
    return;
  
  _render.args.gouraud= false;
  _render.args.dithering= _render.def_args.dithering;
  UNLOCK_RENDERER;
  _renderer->line ( _renderer, &(_render.args), &stats );

  // Timing.
  calc_timing_draw_line ( &stats );
  
} // end draw_mline


static void
draw_sline (void)
{

  PSX_RendererStats stats;

  
  if ( (_render.max_x-_render.min_x) > 1023 ||
       (_render.max_y-_render.min_y) > 511 )
    return;
  
  _render.args.gouraud= true;
  _render.args.dithering= _render.def_args.dithering;
  UNLOCK_RENDERER;
  _renderer->line ( _renderer, &(_render.args), &stats );

  // Timing.
  calc_timing_draw_line ( &stats );
  
} // end draw_sline


static void
calc_timing_draw_rec (
        	      const PSX_RendererStats *stats
        	      )
{
  
  int gpucc;


  // NOTA: Temps un poc arreu basant-me en mednafen.
  
  gpucc= 16 + 2;
  if ( _render.rec_w == 0 )
    gpucc+= _render.rec_h>>1;
  else
    {
      gpucc+= stats->npixels;
      if ( _render.args.transparency != PSX_TR_NONE ||
           _render.args.check_mask )
        gpucc+= (int) (stats->npixels*0.5);
    }
  /*
  if ( !_render.drawing_da_enabled && _display.vres == VRES_480 )
    gpucc/= 2; // <-- Inventada meua, basada en que sosl es dibuixen la meitat.
  */
  _timing.cctoIdle+= 7 * ((int) (gpucc*RENDER_CC_CORRECTION));
  _fifo.busy= (_timing.cctoIdle>0);
  update_timing_event ();
  
} // end calc_timing_draw_rec


static void
draw_mrec (void)
{

  PSX_RendererStats stats;

  
  _render.args.gouraud= false;
  _render.args.texture_mode= PSX_TEX_NONE;
  _render.args.dithering= false;
  UNLOCK_RENDERER;
  _renderer->rect ( _renderer, &(_render.args), _render.rec_w, _render.rec_h,
        	    &stats );

  // Timing.
  calc_timing_draw_rec ( &stats );
  
} // end draw_mrec


static void
draw_trec (void)
{
  
  PSX_RendererStats stats;
  
  
  _render.args.gouraud= false;
  _render.args.texpage_x= _render.def_args.texpage_x;
  _render.args.texpage_y= _render.def_args.texpage_y;
  _render.args.dithering= false;
  _render.args.texwinmask_x= _render.def_args.texwinmask_x;
  _render.args.texwinmask_y= _render.def_args.texwinmask_y;
  _render.args.texwinoff_x= _render.def_args.texwinoff_x;
  _render.args.texwinoff_y= _render.def_args.texwinoff_y;
  _render.args.texflip_x= _render.def_args.texflip_x;
  _render.args.texflip_y= _render.def_args.texflip_y;
  if ( _display.texture_disable && _render.texture_disabled )
    _render.args.texture_mode= PSX_TEX_NONE;
  else
    _render.args.texture_mode= _render.def_args.texture_mode;
  UNLOCK_RENDERER;
  _renderer->rect ( _renderer, &(_render.args), _render.rec_w, _render.rec_h,
        	    &stats );

  // Timing.
  calc_timing_draw_rec ( &stats );
  
} // end draw_trec


static void
fill_rec (void)
{

  int x,y,width,height,r,c,end_x,end_y,gpucc;
  uint16_t *line,color;

  
  // Prepara.
  x= _render.args.v[0].x; y= _render.args.v[0].y;
  width= _render.rec_w; height= _render.rec_h;
  end_x= x + width; end_y= y + height;
  color= TORGB15b ( _render.args.r, _render.args.g, _render.args.b );
  
  // Emplena.
  LOCK_RENDERER;
  for ( r= y; r < end_y; ++r )
    {
      line= &(_fb[(r&0x1FF)*FB_WIDTH]);
      for ( c= x; c < end_x; ++c )
        line[c&0x3FF]= color;
    }

  // Timing. Aparentment dibuixa 16 pixels de colp, hi han també unes
  // constants rares que no sé d'in ixen.
  gpucc= ((width>>3) + 9)*height + 46 + 2;
  /*
  if ( !_render.drawing_da_enabled && _display.vres == VRES_480 )
    gpucc/= 2; // <-- Inventada meua, basada en que sosl es dibuixen la meitat.
  */
  _timing.cctoIdle+= 7 * ((int) (gpucc*RENDER_CC_CORRECTION));
  _fifo.busy= (_timing.cctoIdle>0);
  update_timing_event ();
  
} // end fill_rec


static void
copy_vram2vram (void)
{
  
  int x0,y0,x1,y1,width,height,r0,c0,r1,c1,end_x0,end_y0,pos,gpucc,npixels;
  const uint16_t *line_src;
  uint16_t *line_dst;
  
  
  // Prepara.
  width= _render.rec_w; height= _render.rec_h;
  x0= _render.args.v[0].x; y0= _render.args.v[0].y;
  end_x0= x0 + width; end_y0= y0 + height;
  x1= _render.args.v[1].x; y1= _render.args.v[1].y;
  npixels= 0;
  
  // Copia.
  LOCK_RENDERER;
  for ( r0= y0, r1= y1; r0 < end_y0; ++r0, ++r1 )
    {
      line_src= &(_fb[(r0&0x1FF)*FB_WIDTH]);
      line_dst= &(_fb[(r1&0x1FF)*FB_WIDTH]);
      for ( c0= x0, c1= x1; c0 < end_x0; ++c0, ++c1 )
        {
          pos= c1&0x3FF;
          if ( _render.args.check_mask && line_dst[pos]&0x8000 )
            continue;
          line_dst[pos]= line_src[c0&0x3FF];
          if ( _render.args.set_mask ) line_dst[pos]|= 0x8000;
          ++npixels;
        }
    }
  
  // Timing inspirat per mednafen
  //gpucc= npixels>>3;
  gpucc= 2 + (int)(npixels*2);
  /*
  if ( !_render.drawing_da_enabled && _display.vres == VRES_480 )
    gpucc/= 2; // <-- Inventada meua, basada en que sosl es dibuixen la meitat.
  */
  _timing.cctoIdle+= 7 * ((int) (gpucc*RENDER_CC_CORRECTION));
  _fifo.busy= (_timing.cctoIdle>0);
  update_timing_event ();
  
} // end copy_vram2vram


static void
copy_cpu2vram (
               const uint32_t arg
               )
{
  
  int pos;
  uint16_t *line;
  
  
  LOCK_RENDERER;
  
  // Primera paraula.
  line= &(_fb[(_copy.r&0x1FF)*FB_WIDTH]);
  pos= _copy.c&0x3FF;
  if ( !_render.args.check_mask || !(line[pos]&0x8000) )
    {
      line[pos]= arg&0xFFFF;
      if ( _render.args.set_mask ) line[pos]|= 0x8000;
    }
  if ( ++_copy.c == _copy.end_c )
    {
      if ( ++_copy.r < _copy.end_r )
        {
          line= &(_fb[(_copy.r&0x1FF)*FB_WIDTH]);
          _copy.c= _copy.x;
        }
      else goto end;
    }

  // Segona paraula.
  pos= _copy.c&0x3FF;
  if ( !_render.args.check_mask || !(line[pos]&0x8000) )
    {
      line[pos]= arg>>16;
      if ( _render.args.set_mask ) line[pos]|= 0x8000;
    }
  if ( ++_copy.c == _copy.end_c )
    {
      if ( ++_copy.r < _copy.end_r ) _copy.c= _copy.x;
      else goto end;
    }
  
  return;
  
 end:
  _fifo.state= FIFO_WAIT_CMD;
  
} // end copy_cpu2vram


static uint32_t
copy_vram2cpu (void)
{

  uint32_t ret;
  int pos;
  const uint16_t *line;


  LOCK_RENDERER;
  
  // Primera paraula.
  line= &(_fb[(_copy.r&0x1FF)*FB_WIDTH]);
  pos= _copy.c&0x3FF;
  ret= (uint32_t) line[pos];
  if ( ++_copy.c == _copy.end_c )
    {
      if ( ++_copy.r < _copy.end_r )
        {
          line= &(_fb[(_copy.r&0x1FF)*FB_WIDTH]);
          _copy.c= _copy.x;
        }
      else goto end;
    }

  // Segona paraula.
  pos= _copy.c&0x3FF;
  ret|= (((uint32_t) line[pos])<<16);
  if ( ++_copy.c == _copy.end_c )
    {
      if ( ++_copy.r < _copy.end_r ) _copy.c= _copy.x;
      else goto end;
    }
  
  return ret;
  
 end:
  _fifo.state= FIFO_WAIT_CMD;
  _render.state= WAIT_CMD;
  _read.vram_transfer= false;
  return ret;
  
} // end copy_vram2cpu


// S'espera que estiga ple
static uint32_t
fifo_pop (void)
{

  uint32_t val;


  val= _fifo.v[_fifo.p];
  _fifo.p= (_fifo.p+1)%FIFO_SIZE;
  --_fifo.N;

  return val;
  
} // end fifo_pop


static void
run_fifo_cmd_mpol (void)
{

  uint32_t cmd;


  cmd= fifo_pop ();
  set_vertex_xy ( 0, cmd );
  cmd= fifo_pop ();
  set_vertex_xy ( 1, cmd );
  cmd= fifo_pop ();
  set_vertex_xy ( 2, cmd );
  if ( _render.is_pol4 )
    {
      cmd= fifo_pop ();
      set_vertex_xy ( 3, cmd );
    }
  draw_mpol ();
  
} // end run_fifo_cmd_mpol


static void
run_fifo_cmd_tpol (void)
{

  uint32_t cmd;


  cmd= fifo_pop ();
  set_vertex_xy ( 0, cmd );
  cmd= fifo_pop ();
  set_vertex_txy ( 0, cmd, TEX_SET_CLUT );
  cmd= fifo_pop ();
  set_vertex_xy ( 1, cmd );
  cmd= fifo_pop ();
  set_vertex_txy ( 1, cmd, TEX_SET_PAGE );
  cmd= fifo_pop ();
  set_vertex_xy ( 2, cmd );
  cmd= fifo_pop ();
  set_vertex_txy ( 2, cmd, TEX_SET_NONE );
  if ( _render.is_pol4 )
    {
      cmd= fifo_pop ();
      set_vertex_xy ( 3, cmd );
      cmd= fifo_pop ();
      set_vertex_txy ( 3, cmd, TEX_SET_NONE );
    }
  draw_tpol ();
  
} // end run_fifo_cmd_tpol


static void
run_fifo_cmd_spol (void)
{

  uint32_t cmd;


  cmd= fifo_pop ();
  set_vertex_xy ( 0, cmd );
  cmd= fifo_pop ();
  set_vertex_color ( 1, cmd );
  cmd= fifo_pop ();
  set_vertex_xy ( 1, cmd );
  cmd= fifo_pop ();
  set_vertex_color ( 2, cmd );
  cmd= fifo_pop ();
  set_vertex_xy ( 2, cmd );
  if ( _render.is_pol4 )
    {
      cmd= fifo_pop ();
      set_vertex_color ( 3, cmd );
      cmd= fifo_pop ();
      set_vertex_xy ( 3, cmd );
    }
  draw_spol ();
  
} // end run_fifo_cmd_spol


static void
run_fifo_cmd_stpol (void)
{
  
  uint32_t cmd;


  cmd= fifo_pop ();
  set_vertex_xy ( 0, cmd );
  cmd= fifo_pop ();
  set_vertex_txy ( 0, cmd, TEX_SET_CLUT );
  cmd= fifo_pop ();
  set_vertex_color ( 1, cmd );
  cmd= fifo_pop ();
  set_vertex_xy ( 1, cmd );
  cmd= fifo_pop ();
  set_vertex_txy ( 1, cmd, TEX_SET_PAGE );
  cmd= fifo_pop ();
  set_vertex_color ( 2, cmd );
  cmd= fifo_pop ();
  set_vertex_xy ( 2, cmd );
  cmd= fifo_pop ();
  set_vertex_txy ( 2, cmd, TEX_SET_NONE );
  if ( _render.is_pol4 )
    {
      cmd= fifo_pop ();
      set_vertex_color ( 3, cmd );
      cmd= fifo_pop ();
      set_vertex_xy ( 3, cmd );
      cmd= fifo_pop ();
      set_vertex_txy ( 3, cmd, TEX_SET_NONE );
    }
  draw_stpol ();
  
} // end run_fifo_cmd_stpol


static void
run_fifo_cmd_mline (void)
{
  
  uint32_t cmd;


  cmd= fifo_pop ();
  set_vertex_xy ( 0, cmd );
  cmd= fifo_pop ();
  set_vertex_xy ( 1, cmd );
  draw_mline ();
  if ( _render.is_poly ) _fifo.state= FIFO_WAIT_POLY_MLINE;
  
} // end run_fifo_cmd_mline


static void
run_fifo_cmd_sline (void)
{
  
  uint32_t cmd;


  cmd= fifo_pop ();
  set_vertex_xy ( 0, cmd );
  cmd= fifo_pop ();
  set_vertex_color ( 1, cmd );
  cmd= fifo_pop ();
  set_vertex_xy ( 1, cmd );
  draw_sline ();
  if ( _render.is_poly ) _fifo.state= FIFO_WAIT_POLY_SLINE;
  
} // end run_fifo_cmd_sline


static void
run_fifo_cmd_mrec (void)
{
  
  uint32_t cmd;

  
  cmd= fifo_pop ();
  set_vertex_rec ( cmd );
  if ( _render.rec_w == -1 )
    {
      cmd= fifo_pop ();
      set_rec_width_height ( cmd );
    }
  draw_mrec ();
  
} // end run_fifo_cmd_mrec


static void
run_fifo_cmd_trec (void)
{
  
  uint32_t cmd;


  cmd= fifo_pop ();
  set_vertex_rec ( cmd );
  cmd= fifo_pop ();
  set_vertex_txy ( 0, cmd, TEX_SET_CLUT );
  if ( _render.rec_w == -1 )
    {
      cmd= fifo_pop ();
      set_rec_width_height ( cmd );
    }
  draw_trec ();
  
} // end run_fifo_cmd_trec


static void
run_fifo_cmd_copy (void)
{
  
  uint32_t cmd;


  cmd= fifo_pop ();
  _copy.c= _copy.x= cmd&0x3FF;
  _copy.r= _copy.y= (cmd>>16)&0x1FF;
  cmd= fifo_pop ();
  _copy.end_c= _copy.x + (((cmd&0x3FF)-1)&0x3FF) + 1;
  _copy.end_r= _copy.y + ((((cmd>>16)&0x1FF)-1)&0x1FF) + 1;
  
  if ( !_render.copy_mode_write )
    {
      _fifo.state= FIFO_WAIT_READ_DATA_COPY;
      _read.vram_transfer= true;
    }
  else _fifo.state= FIFO_WAIT_WRITE_DATA_COPY;
  
} // end run_fifo_cmd_copy


static void
run_fifo_cmd (void)
{

  uint32_t cmd;

  
  switch ( _fifo.state )
    {
      
      // A meitat de poli-línia mono.
    case FIFO_WAIT_POLY_MLINE:
      cmd= fifo_pop ();
      if ( cmd != 0x55555555 && cmd != 0x50005000 )
        {
          prepare_next_line ();
          set_vertex_xy ( 1, cmd );
          draw_mline ();
        }
      else _fifo.state= FIFO_WAIT_CMD;
      break;

      // A meitat de poli-linea shaded.
    case FIFO_WAIT_POLY_SLINE:
      cmd= fifo_pop ();
      if ( cmd != 0x55555555 && cmd != 0x50005000 )
        {
          prepare_next_line ();
          set_vertex_color ( 1, cmd );
          cmd= fifo_pop ();
          set_vertex_xy ( 1, cmd );
          draw_sline ();
        }
      else _fifo.state= FIFO_WAIT_CMD;
      break;

      // Copiant dades CPU2VRAM
    case FIFO_WAIT_WRITE_DATA_COPY:
      cmd= fifo_pop ();
      copy_cpu2vram ( cmd );
      break;

      // Llegint dades VRAM2CPU
    case FIFO_WAIT_READ_DATA_COPY:
      cmd= fifo_pop ();
      _warning ( _udata,
                 "GPU FIFO: s'ignorarà la paraula %X perquè actualment s'està"
                 " fent una transferència VRAM a CPU.", cmd );
      break;
      
      // Cas normal.
    case FIFO_WAIT_CMD:
    default:
      cmd= fifo_pop ();
      switch ( cmd>>24 )
        {
        case 0x01: // ¿¿Clear Cache ?? <-- No implemente cache.
          break;
        case 0x02: // Fill Rectangle in VRAM
          set_color ( cmd );
          cmd= fifo_pop ();
          _render.args.v[0].x= cmd&0x3F0;
          _render.args.v[0].y= (cmd>>16)&0x1FF;
          cmd= fifo_pop ();
          _render.rec_w= ((cmd&0x3FF)+0xF) & (~0xF);
          _render.rec_h= (cmd>>16)&0x1FF;
          fill_rec ();
          break;
        case 0x03: break; // Desconegut
        case 0x1F: // Interrupt Request (IRQ1)
          if ( _display.irq_enabled )
            PSX_int_interruption ( PSX_INT_GPU );
          break;
        case 0x20: // Monochrome three-point polygon, opaque
        case 0x21:
          init_maxmin_xy ();
          set_color ( cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.is_pol4= false;
          run_fifo_cmd_mpol ();
          break;
        case 0x22: // Monochrome three-point polygon, semi-transparent
        case 0x23:
          init_maxmin_xy ();
          set_color ( cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.is_pol4= false;
          run_fifo_cmd_mpol ();
          break;
        case 0x24: // Textured three-point polygon, opaque, texture-blending
          init_maxmin_xy ();
          set_color ( cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.is_pol4= false;
          _render.args.modulate_texture= true;
          run_fifo_cmd_tpol ();
          break;
        case 0x25: // Textured three-point polygon, opaque, raw-texture
          init_maxmin_xy ();
          _render.args.transparency= PSX_TR_NONE;
          _render.is_pol4= false;
          _render.args.modulate_texture= false;
          run_fifo_cmd_tpol ();
          break;
        case 0x26: // Textured three-point polygon, semi-transparent,
        	   // texture-blending
          init_maxmin_xy ();
          set_color ( cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.is_pol4= false;
          _render.args.modulate_texture= true;
          run_fifo_cmd_tpol ();
          break;
        case 0x27: // Textured three-point polygon, semi-transparent,
        	   // raw-texture
          init_maxmin_xy ();
          _render.args.transparency= _render.def_args.transparency;
          _render.is_pol4= false;
          _render.args.modulate_texture= false;
          run_fifo_cmd_tpol ();
          break;
        case 0x28: // Monochrome four-point polygon, opaque
        case 0x29:
          init_maxmin_xy ();
          set_color ( cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.is_pol4= true;
          run_fifo_cmd_mpol ();
          break;
        case 0x2A: // Monochrome four-point polygon, semi-transparent
        case 0x2B:
          init_maxmin_xy ();
          set_color ( cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.is_pol4= true;
          run_fifo_cmd_mpol ();
          break;
        case 0x2C: // Textured four-point polygon, opaque, texture-blending
          init_maxmin_xy ();
          set_color ( cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.is_pol4= true;
          _render.args.modulate_texture= true;
          run_fifo_cmd_tpol ();
          break;
        case 0x2D: // Textured four-point polygon, opaque, raw-texture
          init_maxmin_xy ();
          _render.args.transparency= PSX_TR_NONE;
          _render.is_pol4= true;
          _render.args.modulate_texture= false;
          run_fifo_cmd_tpol ();
          break;
        case 0x2E: // Textured four-point polygon, semi-transparent,
        	   // texture-blending
          init_maxmin_xy ();
          set_color ( cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.is_pol4= true;
          _render.args.modulate_texture= true;
          run_fifo_cmd_tpol ();
          break;
        case 0x2F: // Textured four-point polygon, semi-transparent,
        	   // raw-texture
          init_maxmin_xy ();
          _render.args.transparency= _render.def_args.transparency;
          _render.is_pol4= true;
          _render.args.modulate_texture= false;
          run_fifo_cmd_tpol ();
          break;
        case 0x30: // Shaded three-point polygon, opaque
        case 0x31:
          init_maxmin_xy ();
          set_vertex_color ( 0, cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.is_pol4= false;
          run_fifo_cmd_spol ();
          break;
        case 0x32: // Shaded three-point polygon, semi-transparent
        case 0x33:
          init_maxmin_xy ();
          set_vertex_color ( 0, cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.is_pol4= false;
          run_fifo_cmd_spol ();
          break;
        case 0x34: // Shaded Textured three-point polygon, opaque,
        	   // texture-blending
          init_maxmin_xy ();
          set_vertex_color ( 0, cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.is_pol4= false;
          _render.args.modulate_texture= true;
          run_fifo_cmd_stpol ();
          break;
        case 0x35: // Shaded Textured three-point polygon, opaque,
        	   // raw-texture ¿¿??
          init_maxmin_xy ();
          set_vertex_color ( 0, cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.is_pol4= false;
          _render.args.modulate_texture= false;
          run_fifo_cmd_stpol ();
          break;
        case 0x36: // Shaded Textured three-point polygon,
        	   // semi-transparent, texture-blending
          init_maxmin_xy ();
          set_vertex_color ( 0, cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.is_pol4= false;
          _render.args.modulate_texture= true;
          run_fifo_cmd_stpol ();
          break;
        case 0x37: // Shaded Textured three-point polygon,
        	   // semi-transparent, raw-texture ¿¿??
          init_maxmin_xy ();
          set_vertex_color ( 0, cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.is_pol4= false;
          _render.args.modulate_texture= false;
          run_fifo_cmd_stpol ();
          break;
        case 0x38: // Shaded four-point polygon, opaque
        case 0x39:
          init_maxmin_xy ();
          set_vertex_color ( 0, cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.is_pol4= true;
          run_fifo_cmd_spol ();
          break;
        case 0x3A: // Shaded four-point polygon, semi-transparent
        case 0x3B:
          init_maxmin_xy ();
          set_vertex_color ( 0, cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.is_pol4= true;
          run_fifo_cmd_spol ();
          break;
        case 0x3C: // Shaded Textured four-point polygon, opaque,
        	   // texture-blending
          init_maxmin_xy ();
          set_vertex_color ( 0, cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.is_pol4= true;
          _render.args.modulate_texture= true;
          run_fifo_cmd_stpol ();
          break;
        case 0x3D: // Shaded Textured four-point polygon, opaque,
        	   // raw-texture ¿¿??
          init_maxmin_xy ();
          set_vertex_color ( 0, cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.is_pol4= true;
          _render.args.modulate_texture= false;
          run_fifo_cmd_stpol ();
          break;
        case 0x3E: // Shaded Textured four-point polygon,
        	   // semi-transparent, texture-blending
          init_maxmin_xy ();
          set_vertex_color ( 0, cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.is_pol4= true;
          _render.args.modulate_texture= true;
          run_fifo_cmd_stpol ();
          break;
        case 0x3F: // Shaded Textured four-point polygon,
        	   // semi-transparent, raw-texture ¿¿??
          init_maxmin_xy ();
          set_vertex_color ( 0, cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.is_pol4= true;
          _render.args.modulate_texture= false;
          run_fifo_cmd_stpol ();
          break;
        case 0x40: // Monochrome line, opaque
        case 0x41:
          init_maxmin_xy ();
          set_color ( cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.is_poly= false;
          run_fifo_cmd_mline ();
          break;
        case 0x42: // Monochrome line, semi-transparent
        case 0x43:
          init_maxmin_xy ();
          set_color ( cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.is_poly= false;
          run_fifo_cmd_mline ();
          break;

        case 0x48: // Monochrome Poly-line, opaque
        case 0x49:
        case 0x4C:
          init_maxmin_xy ();
          set_color ( cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.is_poly= true;
          run_fifo_cmd_mline ();
          break;
        case 0x4A: // Monochrome Poly-line, semi-transparent
        case 0x4B:
          init_maxmin_xy ();
          set_color ( cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.is_poly= true;
          run_fifo_cmd_mline ();
          break;

        case 0x50: // Shaded line, opaque
        case 0x51:
        case 0x55:
          init_maxmin_xy ();
          set_vertex_color ( 0, cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.is_poly= false;
          run_fifo_cmd_sline ();
          break;
        case 0x52: // Shaded line, semi-transparent
        case 0x53:
          init_maxmin_xy ();
          set_vertex_color ( 0, cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.is_poly= false;
          run_fifo_cmd_sline ();
          break;

        case 0x58: // Shaded Poly-line, opaque
        case 0x59:
          init_maxmin_xy ();
          set_vertex_color ( 0, cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.is_poly= true;
          run_fifo_cmd_sline ();
          break;
        case 0x5A: // Shaded Poly-line, semi-transparent
        case 0x5B:
        case 0x5E:
          init_maxmin_xy ();
          set_vertex_color ( 0, cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.is_poly= true;
          run_fifo_cmd_sline ();
          break;

        case 0x60: // Monochrome Rectangle (variable size) (opaque)
          set_color ( cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.rec_w= _render.rec_h= -1; // <- Variable.
          run_fifo_cmd_mrec ();
          break;
          
        case 0x62: // Monochrome Rectangle (variable size) (semi-transparent)
          set_color ( cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.rec_w= _render.rec_h= -1; // <- Variable.
          run_fifo_cmd_mrec ();
          break;
          
        case 0x64: // Textured Rectangle, variable size, opaque,
        	   // texture-blending
          set_color ( cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.rec_w= _render.rec_h= -1; // <- Variable.
          _render.args.modulate_texture= true;
          run_fifo_cmd_trec ();
          break;
        case 0x65: // Textured Rectangle, variable size, opaque, raw-texture
          set_color ( cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.rec_w= _render.rec_h= -1; // <- Variable.
          _render.args.modulate_texture= false;
          run_fifo_cmd_trec ();
          break;
        case 0x66: // Textured Rectangle, variable size, semi-transp,
        	   // texture-blending
          set_color ( cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.rec_w= _render.rec_h= -1; // <- Variable.
          _render.args.modulate_texture= true;
          run_fifo_cmd_trec ();
          break;
        case 0x67: // Textured Rectangle, variable size, semi-transp,
        	   // raw-texture */
          set_color ( cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.rec_w= _render.rec_h= -1; // <- Variable.
          _render.args.modulate_texture= false;
          run_fifo_cmd_trec ();
          break;
        case 0x68: // Monochrome Rectangle (1x1) (Dot) (opaque)
          set_color ( cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.rec_w= _render.rec_h= 1;
          run_fifo_cmd_mrec ();
          break;

        case 0x6A: // Monochrome Rectangle (1x1) (Dot)
        	   // (semi-transparent)
          set_color ( cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.rec_w= _render.rec_h= 1;
          run_fifo_cmd_mrec ();
          break;

        case 0x6C: // Textured Rectangle, 1x1 (nonsense), opaque,
        	   // texture-blending
          set_color ( cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.rec_w= _render.rec_h= 1;
          _render.args.modulate_texture= true;
          run_fifo_cmd_trec ();
          break;
        case 0x6D: // Textured Rectangle, 1x1 (nonsense), opaque,
        	   // raw-texture
          set_color ( cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.rec_w= _render.rec_h= 1;
          _render.args.modulate_texture= false;
          run_fifo_cmd_trec ();
          break;
        case 0x6E: // Textured Rectangle, 1x1 (nonsense), semi-transp,
        	   // texture-blending
          set_color ( cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.rec_w= _render.rec_h= 1;
          _render.args.modulate_texture= true;
          run_fifo_cmd_trec ();
          break;
        case 0x6F: // Textured Rectangle, 1x1 (nonsense), semi-transp,
        	   // raw-texture
          set_color ( cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.rec_w= _render.rec_h= 1;
          _render.args.modulate_texture= false;
          run_fifo_cmd_trec ();
          break;
        case 0x70: // Monochrome Rectangle (8x8) (opaque)
          set_color ( cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.rec_w= _render.rec_h= 8;
          run_fifo_cmd_mrec ();
          break;

        case 0x72: // Monochrome Rectangle (8x8) (semi-transparent)
          set_color ( cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.rec_w= _render.rec_h= 8;
          run_fifo_cmd_mrec ();
          break;

        case 0x74: // Textured Rectangle, 8x8, opaque, texture-blending
          set_color ( cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.rec_w= _render.rec_h= 8;
          _render.args.modulate_texture= true;
          run_fifo_cmd_trec ();
          break;
        case 0x75: // Textured Rectangle, 8x8, opaque, raw-texture
          set_color ( cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.rec_w= _render.rec_h= 8;
          _render.args.modulate_texture= false;
          run_fifo_cmd_trec ();
          break;
        case 0x76: // Textured Rectangle, 8x8, semi-transparent,
        	   // texture-blending
          set_color ( cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.rec_w= _render.rec_h= 8;
          _render.args.modulate_texture= true;
          run_fifo_cmd_trec ();
          break;
        case 0x77: // Textured Rectangle, 8x8, semi-transparent,
        	   // raw-texture
          set_color ( cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.rec_w= _render.rec_h= 8;
          _render.args.modulate_texture= false;
          run_fifo_cmd_trec ();
          break;
        case 0x78: // Monochrome Rectangle (16x16) (opaque)
          set_color ( cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.rec_w= _render.rec_h= 16;
          run_fifo_cmd_mrec ();
          break;
          
        case 0x7A: // Monochrome Rectangle (16x16) (semi-transparent)
          set_color ( cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.rec_w= _render.rec_h= 16;
          run_fifo_cmd_mrec ();
          break;

        case 0x7C: // Textured Rectangle, 16x16, opaque,
        	   // texture-blending
          set_color ( cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.rec_w= _render.rec_h= 16;
          _render.args.modulate_texture= true;
          run_fifo_cmd_trec ();
          break;
        case 0x7D: // Textured Rectangle, 16x16, opaque, raw-texture
          set_color ( cmd );
          _render.args.transparency= PSX_TR_NONE;
          _render.rec_w= _render.rec_h= 16;
          _render.args.modulate_texture= false;
          run_fifo_cmd_trec ();
          break;
        case 0x7E: // Textured Rectangle, 16x16, semi-transparent,
        	   // texture-blending
          set_color ( cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.rec_w= _render.rec_h= 16;
          _render.args.modulate_texture= true;
          run_fifo_cmd_trec ();
          break;
        case 0x7F: // Textured Rectangle, 16x16, semi-transparent,
        	   // raw-texture
          set_color ( cmd );
          _render.args.transparency= _render.def_args.transparency;
          _render.rec_w= _render.rec_h= 16;
          _render.args.modulate_texture= false;
          run_fifo_cmd_trec ();
          break;
        case 0x80 ... 0x9F: // Copy Rectangle (VRAM to VRAM)
          cmd= fifo_pop ();
          _render.args.v[0].x= cmd&0x3FF;
          _render.args.v[0].y= (cmd>>16)&0x1FF;
          cmd= fifo_pop ();
          _render.args.v[1].x= cmd&0x3FF;
          _render.args.v[1].y= (cmd>>16)&0x1FF;
          cmd= fifo_pop ();
          _render.rec_w= (((cmd&0x3FF)-1)&0x3FF) + 1;
          _render.rec_h= ((((cmd>>16)&0x1FF)-1)&0x1FF) + 1;
          copy_vram2vram ();
          break;
        case 0xA0 ... 0xBF: // Copy Rectangle (CPU to VRAM)
          _render.copy_mode_write= true;
          run_fifo_cmd_copy ();
          break;
        case 0xC0 ... 0xDF: // Copy Rectangle (VRAM to CPU)
          _render.copy_mode_write= false;
          run_fifo_cmd_copy ();
          break;
        case 0xE1: set_draw_mode ( cmd ); break;
        case 0xE2: set_texture_window ( cmd ); break;
        case 0xE6: set_mask_bit ( cmd ); break;
        default:
          _warning ( _udata,
        	     "GPU (FIFO): comandament desconegut %02X",
        	     cmd>>24 );
          break;
        }
      break;
      
    }
  --_fifo.nactions;
  
} // end run_fifo_cmd


static void
set_vertex_xy_trace (
        	     const int       v,
        	     const uint32_t  arg,
        	     PSX_GPUCmd     *cmd
        	     )
{
  
  cmd->v[v].x= (int) (((int16_t) ((uint16_t) ((arg&0x7FF)<<5)))>>5);
  cmd->v[v].y= (int) (((int16_t) ((uint16_t) (((arg>>16)&0x7FF)<<5)))>>5);
  ++(cmd->Nv);
  
} // end set_vertex_xy_trace


static void
set_vertex_txy_trace (
        	      const int       v,
        	      const uint32_t  arg,
        	      PSX_GPUCmd     *cmd
        	      )
{

  cmd->v[v].u= (uint8_t) (arg&0xFF);
  cmd->v[v].v= (uint8_t) ((arg>>8)&0xFF);
  
} // end set_vertex_txy_trace


static void
set_texclut_trace (
        	   const uint32_t  arg,
        	   PSX_GPUCmd     *cmd
        	   )
{

  cmd->texclut_x= (arg>>16)&0x3F;
  cmd->texclut_y= (arg>>22)&0x1FF;
  
} // end set_textclut_trace


static void
set_texpage_trace (
        	   const uint32_t  arg,
        	   PSX_GPUCmd     *cmd
        	   )
{
  
  cmd->texpage_x= (arg>>16)&0xF;
  cmd->texpage_y= (arg>>20)&0x1;
  cmd->tex_pol_transparency= (arg>>(5+16))&0x3;
  cmd->tex_pol_mode= (arg>>(7+16))&0x3;
  
} // end set_texpage_trace


static void
set_vertex_color_trace (
        		const int       v,
        		const uint32_t  arg,
        		PSX_GPUCmd     *cmd
        		)
{

  cmd->v[v].r= arg&0xFF;
  cmd->v[v].g= (arg>>8)&0xFF;
  cmd->v[v].b= (arg>>16)&0xFF;
  
} // end set_vertex_color_trace


static void
set_rec_width_height_trace (
        		    const uint32_t  arg,
        		    PSX_GPUCmd     *cmd
        		    )
{

  cmd->width= (arg&0x3FF);
  cmd->height= (arg>>16)&0x1FF;
  
} // end set_rec_width_height_trace


static bool
run_fifo_cmd_mpol_trace (
        		 PSX_GPUCmd *cmd
        		 )
{

  uint32_t real_cmd;

  
  real_cmd= FIFO_BUF(1);
  set_vertex_xy_trace ( 0, real_cmd, cmd );
  real_cmd= FIFO_BUF(2);
  set_vertex_xy_trace ( 1, real_cmd, cmd );
  real_cmd= FIFO_BUF(3);
  set_vertex_xy_trace ( 2, real_cmd, cmd );
  if ( cmd->name == PSX_GP0_POL3 ) return true;
  real_cmd= FIFO_BUF(4);
  set_vertex_xy_trace ( 3, real_cmd, cmd );
  
  return true;
  
} // end run_fifo_cmd_mpol_trace


static bool
run_fifo_cmd_tpol_trace (
        		 PSX_GPUCmd *cmd
        		 )
{

  uint32_t real_cmd;

  
  real_cmd= FIFO_BUF(1);
  set_vertex_xy_trace ( 0, real_cmd, cmd );
  real_cmd= FIFO_BUF(2);
  set_vertex_txy_trace ( 0, real_cmd, cmd );
  set_texclut_trace ( real_cmd, cmd );
  real_cmd= FIFO_BUF(3);
  set_vertex_xy_trace ( 1, real_cmd, cmd );
  real_cmd= FIFO_BUF(4);
  set_vertex_txy_trace ( 1, real_cmd, cmd );
  set_texpage_trace ( real_cmd, cmd );
  real_cmd= FIFO_BUF(5);
  set_vertex_xy_trace ( 2, real_cmd, cmd );
  real_cmd= FIFO_BUF(6);
  set_vertex_txy_trace ( 2, real_cmd, cmd );
  if ( cmd->name == PSX_GP0_POL3 ) return true;
  real_cmd= FIFO_BUF(7);
  set_vertex_xy_trace ( 3, real_cmd, cmd );
  real_cmd= FIFO_BUF(8);
  set_vertex_txy_trace ( 3, real_cmd, cmd );

  return true;
  
} // end run_fifo_cmd_tpol_trace


static bool
run_fifo_cmd_spol_trace (
        		 PSX_GPUCmd *cmd
        		 )
{

  uint32_t real_cmd;


  real_cmd= FIFO_BUF(1);
  set_vertex_xy_trace ( 0, real_cmd, cmd );
  real_cmd= FIFO_BUF(2);
  set_vertex_color_trace ( 1, real_cmd, cmd );
  real_cmd= FIFO_BUF(3);
  set_vertex_xy_trace ( 1, real_cmd, cmd );
  real_cmd= FIFO_BUF(4);
  set_vertex_color_trace ( 2, real_cmd, cmd );
  real_cmd= FIFO_BUF(5);
  set_vertex_xy_trace ( 2, real_cmd, cmd );
  if ( cmd->name == PSX_GP0_POL3 ) return true;
  real_cmd= FIFO_BUF(6);
  set_vertex_color_trace ( 3, real_cmd, cmd );
  real_cmd= FIFO_BUF(7);
  set_vertex_xy_trace ( 3, real_cmd, cmd );

  return true;
  
} // end run_fifo_cmd_spol_trace


static bool
run_fifo_cmd_stpol_trace (
        		  PSX_GPUCmd *cmd
        		  )
{

  uint32_t real_cmd;


  real_cmd= FIFO_BUF(1);
  set_vertex_xy_trace ( 0, real_cmd, cmd );
  real_cmd= FIFO_BUF(2);
  set_vertex_txy_trace ( 0, real_cmd, cmd );
  set_texclut_trace ( real_cmd, cmd );
  real_cmd= FIFO_BUF(3);
  set_vertex_color_trace ( 1, real_cmd, cmd );
  real_cmd= FIFO_BUF(4);
  set_vertex_xy_trace ( 1, real_cmd, cmd );
  real_cmd= FIFO_BUF(5);
  set_vertex_txy_trace ( 1, real_cmd, cmd );
  set_texpage_trace ( real_cmd, cmd );
  real_cmd= FIFO_BUF(6);
  set_vertex_color_trace ( 2, real_cmd, cmd );
  real_cmd= FIFO_BUF(7);
  set_vertex_xy_trace ( 2, real_cmd, cmd );
  real_cmd= FIFO_BUF(8);
  set_vertex_txy_trace ( 2, real_cmd, cmd );
  if ( cmd->name == PSX_GP0_POL3 ) return true;
  real_cmd= FIFO_BUF(9);
  set_vertex_color_trace ( 3, real_cmd, cmd );
  real_cmd= FIFO_BUF(10);
  set_vertex_xy_trace ( 3, real_cmd, cmd );
  real_cmd= FIFO_BUF(11);
  set_vertex_txy_trace ( 3, real_cmd, cmd );

  return true;
  
} // end run_fifo_cmd_stpol_trace


static bool
run_fifo_cmd_mline_trace (
        		  PSX_GPUCmd *cmd
        		  )
{
  
  uint32_t real_cmd;
  

  real_cmd= FIFO_BUF(1);
  set_vertex_xy_trace ( 0, real_cmd, cmd );
  real_cmd= FIFO_BUF(2);
  set_vertex_xy_trace ( 1, real_cmd, cmd );

  return true;
  
} // end run_fifo_cmd_mline_trace


static bool
run_fifo_cmd_sline_trace (
        		  PSX_GPUCmd *cmd
        		  )
{
  
  uint32_t real_cmd;


  real_cmd= FIFO_BUF(1);
  set_vertex_xy_trace ( 0, real_cmd, cmd );
  real_cmd= FIFO_BUF(2);
  set_vertex_color_trace ( 1, real_cmd, cmd );
  real_cmd= FIFO_BUF(3);
  set_vertex_xy_trace ( 1, real_cmd, cmd );

  return true;
  
} // end run_fifo_cmd_sline_trace


static bool
run_fifo_cmd_mrec_trace (
        		 PSX_GPUCmd *cmd
        		 )
{
  
  uint32_t real_cmd;


  real_cmd= FIFO_BUF(1);
  set_vertex_xy_trace ( 0, real_cmd, cmd );
  if ( cmd->width != -1 ) return true;
  real_cmd= FIFO_BUF(2);
  set_rec_width_height_trace ( real_cmd, cmd );

  return true;
  
} // end run_fifo_cmd_mrec_trace


static bool
run_fifo_cmd_trec_trace (
        		 PSX_GPUCmd *cmd
        		 )
{
  
  uint32_t real_cmd;
  
  
  real_cmd= FIFO_BUF(1);
  set_vertex_xy_trace ( 0, real_cmd, cmd );
  real_cmd= FIFO_BUF(2);
  set_vertex_txy_trace ( 0, real_cmd, cmd );
  set_texclut_trace ( real_cmd, cmd );
  if ( cmd->width != -1 ) return true;
  real_cmd= FIFO_BUF(3);
  set_rec_width_height_trace ( real_cmd, cmd );

  return true;
  
} // end run_fifo_cmd_trec_trace


static bool
run_fifo_cmd_copy_trace (
        		 PSX_GPUCmd *cmd
        		 )
{

  uint32_t real_cmd;


  real_cmd= FIFO_BUF(1);
  cmd->v[0].x= real_cmd&0x3FF;
  cmd->v[0].y= (real_cmd>>16)&0x1FF;
  cmd->Nv= 1;
  real_cmd= FIFO_BUF(2);
  cmd->width= (((real_cmd&0x3FF)-1)&0x3FF) + 1;
  cmd->height= ((((real_cmd>>16)&0x1FF)-1)&0x1FF) + 1;

  return true;
      
} // end run_fifo_cmd_copy_trace


static void
run_fifo_cmd_trace (void)
{

  uint32_t real_cmd;
  static PSX_GPUCmd cmd;
  bool ready;
  
  
  ready= false;
  switch ( _fifo.state )
    {
      
      // Cas poli-línia mono
    case FIFO_WAIT_POLY_MLINE:
      real_cmd= FIFO_BUF(0);
      if ( real_cmd != 0x55555555 && real_cmd != 0x50005000 )
        {
          cmd.word= PSX_GP0_POLYLINE_CONT;
          cmd.Nv= 0;
          set_vertex_xy_trace ( 0, real_cmd, &cmd );
          ready= true;
        }
      break;

      // Cas poli-línea shaded.
    case FIFO_WAIT_POLY_SLINE:
      real_cmd= FIFO_BUF(0);
      if ( real_cmd != 0x55555555 && real_cmd != 0x50005000 )
        {
          cmd.word= PSX_GP0_POLYLINE_CONT;
          cmd.Nv= 0;
          set_vertex_color_trace ( 0, real_cmd, &cmd );
          real_cmd= FIFO_BUF(1);
          set_vertex_xy_trace ( 0, real_cmd, &cmd );
          ready= true;
        }
      break;

    case FIFO_WAIT_WRITE_DATA_COPY:
    case FIFO_WAIT_READ_DATA_COPY:
      break;
      
      // Cas normal
    case FIFO_WAIT_CMD:
    default:
      real_cmd= FIFO_BUF(0);
      cmd.word= real_cmd;
      cmd.ops= 0;
      cmd.Nv= 0;
      cmd.width= cmd.height= -1;
      switch ( real_cmd>>24 )
        {
        case 0x01: // ¿¿Clear Cache ?? <-- No implemente cache.
          break;
        case 0x02: // Fill Rectangle in VRAM
          cmd.name= PSX_GP0_FILL;
          cmd.ops|= PSX_GP_COLOR;
          real_cmd= FIFO_BUF(1);
          cmd.v[0].x= real_cmd&0x3F0;
          cmd.v[0].y= (real_cmd>>16)&0x1FF;
          cmd.Nv= 1;
          real_cmd= FIFO_BUF(2);
          cmd.width= ((real_cmd&0x3FF)+0xF) & (~0xF);
          cmd.height= (real_cmd>>16)&0x1FF;
          ready= true;
          break;
        case 0x1F: // Interrupt Request (IRQ1)
          cmd.name= PSX_GP0_IRQ1;
          ready=true;
          break;
        case 0x20: // Monochrome three-point polygon, opaque
        case 0x21:
          cmd.name= PSX_GP0_POL3;
          cmd.ops|= PSX_GP_COLOR;
          ready= run_fifo_cmd_mpol_trace ( &cmd );
          break;
        case 0x22: // Monochrome three-point polygon, semi-transparent
        case 0x23:
          cmd.name= PSX_GP0_POL3;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TRANSPARENCY;
          ready= run_fifo_cmd_mpol_trace ( &cmd );
          break;
        case 0x24: // Textured three-point polygon, opaque, texture-blending
          cmd.name= PSX_GP0_POL3;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TEXT_BLEND;
          ready= run_fifo_cmd_tpol_trace ( &cmd );
          break;
        case 0x25: // Textured three-point polygon, opaque, raw-texture
          cmd.name= PSX_GP0_POL3;
          cmd.ops|= PSX_GP_RAW_TEXT;
          ready= run_fifo_cmd_tpol_trace ( &cmd );
          break;
        case 0x26: // Textured three-point polygon, semi-transparent,
          // texture-blending
          cmd.name= PSX_GP0_POL3;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TRANSPARENCY|PSX_GP_TEXT_BLEND;
          ready= run_fifo_cmd_tpol_trace ( &cmd );
          break;
        case 0x27: // Textured three-point polygon, semi-transparent,
          // raw-texture
          cmd.name= PSX_GP0_POL3;
          cmd.ops|= PSX_GP_TRANSPARENCY|PSX_GP_RAW_TEXT;
          ready= run_fifo_cmd_tpol_trace ( &cmd );
          break;
        case 0x28: // Monochrome four-point polygon, opaque
        case 0x29:
          cmd.name= PSX_GP0_POL4;
          cmd.ops|= PSX_GP_COLOR;
          ready= run_fifo_cmd_mpol_trace ( &cmd );
          break;
        case 0x2A: // Monochrome four-point polygon, semi-transparent
        case 0x2B:
          cmd.name= PSX_GP0_POL4;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TRANSPARENCY;
          ready= run_fifo_cmd_mpol_trace ( &cmd );
          break;
        case 0x2C: // Textured four-point polygon, opaque, texture-blending
          cmd.name= PSX_GP0_POL4;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TEXT_BLEND;
          ready= run_fifo_cmd_tpol_trace ( &cmd );
          break;
        case 0x2D: // Textured four-point polygon, opaque, raw-texture
          cmd.name= PSX_GP0_POL4;
          cmd.ops|= PSX_GP_RAW_TEXT;
          ready= run_fifo_cmd_tpol_trace ( &cmd );
          break;
        case 0x2E: // Textured four-point polygon, semi-transparent,
        	   // texture-blending
          cmd.name= PSX_GP0_POL4;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TRANSPARENCY|PSX_GP_TEXT_BLEND;
          ready= run_fifo_cmd_tpol_trace ( &cmd );
          break;
        case 0x2F: // Textured four-point polygon, semi-transparent,
        	   // raw-texture
          cmd.name= PSX_GP0_POL4;
          cmd.ops|= PSX_GP_TRANSPARENCY|PSX_GP_RAW_TEXT;
          ready= run_fifo_cmd_tpol_trace ( &cmd );
          break;
        case 0x30: // Shaded three-point polygon, opaque
        case 0x31:
          cmd.name= PSX_GP0_POL3;
          cmd.ops|= PSX_GP_V_COLOR;
          set_vertex_color_trace ( 0, real_cmd, &cmd );
          ready= run_fifo_cmd_spol_trace ( &cmd );
          break;
        case 0x32: // Shaded three-point polygon, semi-transparent
        case 0x33:
          cmd.name= PSX_GP0_POL3;
          cmd.ops|= PSX_GP_V_COLOR|PSX_GP_TRANSPARENCY;
          set_vertex_color_trace ( 0, real_cmd, &cmd );
          ready= run_fifo_cmd_spol_trace ( &cmd );
          break;
        case 0x34: // Shaded Textured three-point polygon, opaque,
        	   // texture-blending
          cmd.name= PSX_GP0_POL3;
          cmd.ops|= PSX_GP_V_COLOR|PSX_GP_TEXT_BLEND;
          set_vertex_color_trace ( 0, real_cmd, &cmd );
          ready= run_fifo_cmd_stpol_trace ( &cmd );
          break;
        case 0x35: // Shaded Textured three-point polygon, opaque,
        	   // raw-texture ¿¿??
          cmd.name= PSX_GP0_POL3;
          cmd.ops|= PSX_GP_V_COLOR|PSX_GP_RAW_TEXT;
          set_vertex_color_trace ( 0, real_cmd, &cmd );
          ready= run_fifo_cmd_stpol_trace ( &cmd );
          break;
        case 0x36: // Shaded Textured three-point polygon,
        	   // semi-transparent, texture-blending
          cmd.name= PSX_GP0_POL3;
          cmd.ops|= PSX_GP_V_COLOR|PSX_GP_TRANSPARENCY|PSX_GP_TEXT_BLEND;
          set_vertex_color_trace ( 0, real_cmd, &cmd );
          ready= run_fifo_cmd_stpol_trace ( &cmd );
          break;
        case 0x37: // Shaded Textured three-point polygon,
        	   // semi-transparent, raw-texture ¿¿??
          cmd.name= PSX_GP0_POL3;
          cmd.ops|= PSX_GP_V_COLOR|PSX_GP_TRANSPARENCY|PSX_GP_RAW_TEXT;
          set_vertex_color_trace ( 0, real_cmd, &cmd );
          ready= run_fifo_cmd_stpol_trace ( &cmd );
          break;
        case 0x38: // Shaded four-point polygon, opaque
        case 0x39:
          cmd.name= PSX_GP0_POL4;
          cmd.ops|= PSX_GP_V_COLOR;
          set_vertex_color_trace ( 0, real_cmd, &cmd );
          ready= run_fifo_cmd_spol_trace ( &cmd );
          break;
        case 0x3A: // Shaded four-point polygon, semi-transparent
        case 0x3B:
          cmd.name= PSX_GP0_POL4;
          cmd.ops|= PSX_GP_V_COLOR|PSX_GP_TRANSPARENCY;
          set_vertex_color_trace ( 0, real_cmd, &cmd );
          ready= run_fifo_cmd_spol_trace ( &cmd );
          break;
        case 0x3C: // Shaded Textured four-point polygon, opaque,
        	   // texture-blending
          cmd.name= PSX_GP0_POL4;
          cmd.ops|= PSX_GP_V_COLOR|PSX_GP_TEXT_BLEND;
          set_vertex_color_trace ( 0, real_cmd, &cmd );
          ready= run_fifo_cmd_stpol_trace ( &cmd );
          break;
        case 0x3D: // Shaded Textured four-point polygon, opaque,
        	   // raw-texture ¿¿??
          cmd.name= PSX_GP0_POL4;
          cmd.ops|= PSX_GP_V_COLOR|PSX_GP_RAW_TEXT;
          set_vertex_color_trace ( 0, real_cmd, &cmd );
          ready= run_fifo_cmd_stpol_trace ( &cmd );
          break;
        case 0x3E: // Shaded Textured four-point polygon,
        	   // semi-transparent, texture-blending
          cmd.name= PSX_GP0_POL4;
          cmd.ops|= PSX_GP_V_COLOR|PSX_GP_TRANSPARENCY|PSX_GP_TEXT_BLEND;
          set_vertex_color_trace ( 0, real_cmd, &cmd );
          ready= run_fifo_cmd_stpol_trace ( &cmd );
          break;
        case 0x3F: // Shaded Textured four-point polygon,
        	   // semi-transparent, raw-texture ¿¿??
          cmd.name= PSX_GP0_POL4;
          cmd.ops|= PSX_GP_V_COLOR|PSX_GP_TRANSPARENCY|PSX_GP_RAW_TEXT;
          set_vertex_color_trace ( 0, real_cmd, &cmd );
          ready= run_fifo_cmd_stpol_trace ( &cmd );
          break;
        case 0x40: // Monochrome line, opaque
        case 0x41:
          cmd.name= PSX_GP0_LINE;
          cmd.ops|= PSX_GP_COLOR;
          ready= run_fifo_cmd_mline_trace ( &cmd );
          break;
        case 0x42: // Monochrome line, semi-transparent
        case 0x43:
          cmd.name= PSX_GP0_LINE;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TRANSPARENCY;
          ready= run_fifo_cmd_mline_trace ( &cmd );
          break;

        case 0x48: // Monochrome Poly-line, opaque
        case 0x49:
        case 0x4C:
          cmd.name= PSX_GP0_POLYLINE;
          cmd.ops|= PSX_GP_COLOR;
          ready= run_fifo_cmd_mline_trace ( &cmd );
          break;
        case 0x4A: // Monochrome Poly-line, semi-transparent
        case 0x4B:
          cmd.name= PSX_GP0_POLYLINE;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TRANSPARENCY;
          ready= run_fifo_cmd_mline_trace ( &cmd );
          break;

        case 0x50: // Shaded line, opaque
        case 0x51:
        case 0x55:
          cmd.name= PSX_GP0_LINE;
          cmd.ops|= PSX_GP_V_COLOR;
          set_vertex_color_trace ( 0, real_cmd, &cmd );
          ready= run_fifo_cmd_sline_trace ( &cmd );
          break;
        case 0x52: // Shaded line, semi-transparent
        case 0x53:
          cmd.name= PSX_GP0_LINE;
          cmd.ops|= PSX_GP_V_COLOR|PSX_GP_TRANSPARENCY;
          set_vertex_color_trace ( 0, real_cmd, &cmd );
          ready= run_fifo_cmd_sline_trace ( &cmd );
          break;

        case 0x58: // Shaded Poly-line, opaque
        case 0x59:
          cmd.name= PSX_GP0_POLYLINE;
          cmd.ops|= PSX_GP_V_COLOR;
          set_vertex_color_trace ( 0, real_cmd, &cmd );
          ready= run_fifo_cmd_sline_trace ( &cmd );
          break;
        case 0x5A: // Shaded Poly-line, semi-transparent
        case 0x5B:
        case 0x5E:
          cmd.name= PSX_GP0_POLYLINE;
          cmd.ops|= PSX_GP_V_COLOR|PSX_GP_TRANSPARENCY;
          set_vertex_color_trace ( 0, real_cmd, &cmd );
          ready= run_fifo_cmd_sline_trace ( &cmd );
          break;

        case 0x60: // Monochrome Rectangle (variable size) (opaque)
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR;
          ready= run_fifo_cmd_mrec_trace ( &cmd );
          break;

        case 0x62: // Monochrome Rectangle (variable size)
                   // (semi-transparent)
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TRANSPARENCY;
          ready= run_fifo_cmd_mrec_trace ( &cmd );
          break;

        case 0x64: // Textured Rectangle, variable size, opaque,
        	   // texture-blending
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TEXT_BLEND;
          ready= run_fifo_cmd_trec_trace ( &cmd );
          break;
        case 0x65: // Textured Rectangle, variable size, opaque,
        	   // raw-texture
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_RAW_TEXT;
          ready= run_fifo_cmd_trec_trace ( &cmd );
          break;
        case 0x66: // Textured Rectangle, variable size, semi-transp,
                   // texture-blending
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TRANSPARENCY|PSX_GP_TEXT_BLEND;
          ready= run_fifo_cmd_trec_trace ( &cmd );
          break;
        case 0x67: // Textured Rectangle, variable size, semi-transp,
                   // raw-texture
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TRANSPARENCY|PSX_GP_RAW_TEXT;
          ready= run_fifo_cmd_trec_trace ( &cmd );
          break;
        case 0x68: // Monochrome Rectangle (1x1) (Dot) (opaque)
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR;
          cmd.width= 1;
          cmd.height= 1;
          ready= run_fifo_cmd_mrec_trace ( &cmd );
          break;

        case 0x6A: // Monochrome Rectangle (1x1) (Dot)
        	   // (semi-transparent)
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TRANSPARENCY;
          cmd.width= 1;
          cmd.height= 1;
          ready= run_fifo_cmd_mrec_trace ( &cmd );
          break;

        case 0x6C: // Textured Rectangle, 1x1 (nonsense), opaque,
                   // texture-blending
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TEXT_BLEND;
          cmd.width= 1;
          cmd.height= 1;
          ready= run_fifo_cmd_trec_trace ( &cmd );
          break;
        case 0x6D: // Textured Rectangle, 1x1 (nonsense), opaque,
                   // raw-texture
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_RAW_TEXT;
          cmd.width= 1;
          cmd.height= 1;
          ready= run_fifo_cmd_trec_trace ( &cmd );
          break;
        case 0x6E: // Textured Rectangle, 1x1 (nonsense), semi-transp,
                   // texture-blending
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TRANSPARENCY|PSX_GP_TEXT_BLEND;
          cmd.width= 1;
          cmd.height= 1;
          ready= run_fifo_cmd_trec_trace ( &cmd );
          break;
        case 0x6F: // Textured Rectangle, 1x1 (nonsense), semi-transp,
                   // raw-texture
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TRANSPARENCY|PSX_GP_RAW_TEXT;
          cmd.width= 1;
          cmd.height= 1;
          ready= run_fifo_cmd_trec_trace ( &cmd );
          break;
        case 0x70: // Monochrome Rectangle (8x8) (opaque)
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR;
          cmd.width= 8;
          cmd.height= 8;
          ready= run_fifo_cmd_mrec_trace ( &cmd );
          break;

        case 0x72: // Monochrome Rectangle (8x8) (semi-transparent)
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TRANSPARENCY;
          cmd.width= 8;
          cmd.height= 8;
          ready= run_fifo_cmd_mrec_trace ( &cmd );
          break;

        case 0x74: // Textured Rectangle, 8x8, opaque,
                   // texture-blending
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TEXT_BLEND;
          cmd.width= 8;
          cmd.height= 8;
          ready= run_fifo_cmd_trec_trace ( &cmd );
          break;
        case 0x75: // Textured Rectangle, 8x8, opaque, raw-texture
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_RAW_TEXT;
          cmd.width= 8;
          cmd.height= 8;
          ready= run_fifo_cmd_trec_trace ( &cmd );
          break;
        case 0x76: // Textured Rectangle, 8x8, semi-transparent,
                   // texture-blending
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TRANSPARENCY|PSX_GP_TEXT_BLEND;
          cmd.width= 8;
          cmd.height= 8;
          ready= run_fifo_cmd_trec_trace ( &cmd );
          break;
        case 0x77: // Textured Rectangle, 8x8, semi-transparent,
                   // raw-texture
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TRANSPARENCY|PSX_GP_RAW_TEXT;
          cmd.width= 8;
          cmd.height= 8;
          ready= run_fifo_cmd_trec_trace ( &cmd );
          break;
        case 0x78: // Monochrome Rectangle (16x16) (opaque)
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR;
          cmd.width= 16;
          cmd.height= 16;
          ready= run_fifo_cmd_mrec_trace ( &cmd );
          break;

        case 0x7A: // Monochrome Rectangle (16x16) (semi-transparent)
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TRANSPARENCY;
          cmd.width= 16;
          cmd.height= 16;
          ready= run_fifo_cmd_mrec_trace ( &cmd );
          break;

        case 0x7C: // Textured Rectangle, 16x16, opaque,
                   // texture-blending
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TEXT_BLEND;
          cmd.width= 16;
          cmd.height= 16;
          ready= run_fifo_cmd_trec_trace ( &cmd );
          break;
        case 0x7D: // Textured Rectangle, 16x16, opaque, raw-texture
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_RAW_TEXT;
          cmd.width= 16;
          cmd.height= 16;
          ready= run_fifo_cmd_trec_trace ( &cmd );
          break;
        case 0x7E: // Textured Rectangle, 16x16, semi-transparent,
                   // texture-blending
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TRANSPARENCY|PSX_GP_TEXT_BLEND;
          cmd.width= 16;
          cmd.height= 16;
          ready= run_fifo_cmd_trec_trace ( &cmd );
          break;
        case 0x7F: // Textured Rectangle, 16x16, semi-transparent,
                   // raw-texture
          cmd.name= PSX_GP0_RECT;
          cmd.ops|= PSX_GP_COLOR|PSX_GP_TRANSPARENCY|PSX_GP_RAW_TEXT;
          cmd.width= 16;
          cmd.height= 16;
          ready= run_fifo_cmd_trec_trace ( &cmd );
          break;
        case 0x80 ... 0x9F: // Copy Rectangle (VRAM to VRAM)
          cmd.name= PSX_GP0_COPY_VRAM2VRAM;
          ready= true;
          break;
        case 0xA0 ... 0xBF: // Copy Rectangle (CPU to VRAM)
          cmd.name= PSX_GP0_COPY_CPU2VRAM;
          ready= run_fifo_cmd_copy_trace ( &cmd );
          break;
        case 0xC0 ... 0xDF: // Copy Rectangle (VRAM to CPU)
          cmd.name= PSX_GP0_COPY_VRAM2CPU;
          ready= run_fifo_cmd_copy_trace ( &cmd );
          break;
        case 0xE1: cmd.name= PSX_GP0_SET_DRAW_MODE; ready= true; break;
        case 0xE2: cmd.name= PSX_GP0_SET_TEXT_WIN; ready= true; break;
        case 0xE6: cmd.name= PSX_GP0_SET_MASK_BIT; ready= true; break;
        default:
          cmd.name= PSX_GP0_UNK;
          ready= true;
          break;
        }
      break;
      
    }
  if ( ready ) _gpu_cmd_trace ( &cmd, _udata );
  
  // Executa el comandament.
  run_fifo_cmd ();
  
} // end run_fifo_cmd_trace


static void
run_fifo_cmds (void)
{
  
  while ( _fifo.nactions && !_fifo.busy )
    _run_fifo_cmd ();
  
} // end run_fifo_cmds


static void
fifo_push (
           const uint32_t data
           )
{

  if ( _fifo.N == FIFO_SIZE )
    {
      _warning ( _udata,
        	 "GPU (FIFO PUSH): la cola està plena, i per tant"
        	 " %08X es descartarà", data );
      return;
    }
  _fifo.v[(_fifo.p+_fifo.N)%FIFO_SIZE]= data;
  ++_fifo.N;
  
} // end fifo_push


// NOTA: No inclou el clock.
static void
gp0_cmd (
         const uint32_t cmd
         )
{

  int w,h,size;

  
  switch ( _render.state )
    {

      // Espera comandament.
    case WAIT_CMD:
      switch ( cmd>>24 )
        {
        case 0x00: break; // Nop.
        case 0x01: // ¿¿Clear Cache ??
          INSERT_SHORT_CMD ( cmd );
          break;
        case 0x02: // Fill Rectangle in VRAM
          INSERT_LONG_CMD ( cmd, NWORDS_FILL );
          break;
        case 0x03: // Desconegut.
          INSERT_SHORT_CMD ( cmd );
          break;
        case 0x04 ... 0x1E: // Nop mirror.
          break; 
        case 0x1F: // Interrupt Request (IRQ1)
          INSERT_SHORT_CMD ( cmd );
          break;
        case 0x20: // Monochrome three-point polygon, opaque
        case 0x21:
          INSERT_LONG_CMD ( cmd, NWORDS_MPOL3 );
          break;
        case 0x22: // Monochrome three-point polygon, semi-transparent
        case 0x23:
          INSERT_LONG_CMD ( cmd, NWORDS_MPOL3 );
          break;
        case 0x24: // Textured three-point polygon, opaque, texture-blending
          INSERT_LONG_CMD ( cmd, NWORDS_TPOL3 );
          break;
        case 0x25: // Textured three-point polygon, opaque, raw-texture
          INSERT_LONG_CMD ( cmd, NWORDS_TPOL3 );
          break;
        case 0x26: // Textured three-point polygon, semi-transparent,
        	   // texture-blending
          INSERT_LONG_CMD ( cmd, NWORDS_TPOL3 );
          break;
        case 0x27: // Textured three-point polygon, semi-transparent,
        	   // raw-texture
          INSERT_LONG_CMD ( cmd, NWORDS_TPOL3 );
          break;
        case 0x28: // Monochrome four-point polygon, opaque
        case 0x29:
          INSERT_LONG_CMD ( cmd, NWORDS_MPOL4 );
          break;
        case 0x2A: // Monochrome four-point polygon, semi-transparent
        case 0x2B:
          INSERT_LONG_CMD ( cmd, NWORDS_MPOL4 );
          break;
        case 0x2C: // Textured four-point polygon, opaque, texture-blending
          INSERT_LONG_CMD ( cmd, NWORDS_TPOL4 );
          break;
        case 0x2D: // Textured four-point polygon, opaque, raw-texture
          INSERT_LONG_CMD ( cmd, NWORDS_TPOL4 );
          break;
        case 0x2E: // Textured four-point polygon, semi-transparent,
        	   // texture-blending
          INSERT_LONG_CMD ( cmd, NWORDS_TPOL4 );
          break;
        case 0x2F: // Textured four-point polygon, semi-transparent,
        	   // raw-texture
          INSERT_LONG_CMD ( cmd, NWORDS_TPOL4 );
          break;
        case 0x30: // Shaded three-point polygon, opaque
        case 0x31:
          INSERT_LONG_CMD ( cmd, NWORDS_SPOL3 );
          break;
        case 0x32: // Shaded three-point polygon, semi-transparent
        case 0x33:
          INSERT_LONG_CMD ( cmd, NWORDS_SPOL3 );
          break;
        case 0x34: // Shaded Textured three-point polygon, opaque,
        	   // texture-blending
          INSERT_LONG_CMD ( cmd, NWORDS_STPOL3 );
          break;
        case 0x35: // Shaded Textured three-point polygon, opaque,
        	   // raw-texture ¿¿??
          INSERT_LONG_CMD ( cmd, NWORDS_STPOL3 );
          break;
        case 0x36: // Shaded Textured three-point polygon,
        	   // semi-transparent, texture-blending
          INSERT_LONG_CMD ( cmd, NWORDS_STPOL3 );
          break;
        case 0x37: // Shaded Textured three-point polygon,
        	   // semi-transparent, raw-texture ¿¿??
          INSERT_LONG_CMD ( cmd, NWORDS_STPOL3 );
          break;
        case 0x38: // Shaded four-point polygon, opaque
        case 0x39:
          INSERT_LONG_CMD ( cmd, NWORDS_SPOL4 );
          break;
        case 0x3A: // Shaded four-point polygon, semi-transparent
        case 0x3B:
          INSERT_LONG_CMD ( cmd, NWORDS_SPOL4 );
          break;
        case 0x3C: // Shaded Textured four-point polygon, opaque,
        	   // texture-blending
          INSERT_LONG_CMD ( cmd, NWORDS_STPOL4 );
          break;
        case 0x3D: // Shaded Textured four-point polygon, opaque,
        	   // raw-texture ¿¿??
          INSERT_LONG_CMD ( cmd, NWORDS_STPOL4 );
          break;
        case 0x3E: // Shaded Textured four-point polygon,
        	   // semi-transparent, texture-blending
          INSERT_LONG_CMD ( cmd, NWORDS_STPOL4 );
          break;
        case 0x3F: // Shaded Textured four-point polygon,
        	   // semi-transparent, raw-texture ¿¿??
          INSERT_LONG_CMD ( cmd, NWORDS_STPOL4 );
          break;
        case 0x40: // Monochrome line, opaque
        case 0x41:
          INSERT_LONG_CMD ( cmd, NWORDS_MLINE );
          break;
        case 0x42: // Monochrome line, semi-transparent
        case 0x43:
          INSERT_LONG_CMD ( cmd, NWORDS_MLINE );
          break;

        case 0x48: // Monochrome Poly-line, opaque
        case 0x49:
        case 0x4C:
          fifo_push ( cmd );
          _render.state= WAIT_V1_POLY_MLINE;
          break;
        case 0x4A: // Monochrome Poly-line, semi-transparent
        case 0x4B:
          fifo_push ( cmd );
          _render.state= WAIT_V1_POLY_MLINE;
          break;

        case 0x50: // Shaded line, opaque
        case 0x51:
        case 0x55:
          INSERT_LONG_CMD ( cmd, NWORDS_SLINE );
          break;
        case 0x52: // Shaded line, semi-transparent
        case 0x53:
          INSERT_LONG_CMD ( cmd, NWORDS_SLINE );
          break;

        case 0x58: // Shaded Poly-line, opaque
        case 0x59:
          fifo_push ( cmd );
          _render.state= WAIT_V1_POLY_SLINE;
          break;
        case 0x5A: // Shaded Poly-line, semi-transparent
        case 0x5B:
        case 0x5E:
          fifo_push ( cmd );
          _render.state= WAIT_V1_POLY_SLINE;
          break;
          
        case 0x60: // Monochrome Rectangle (variable size) (opaque)
          INSERT_LONG_CMD ( cmd, NWORDS_MREC_VAR );
          break;

        case 0x62: // Monochrome Rectangle (variable size)
        	   // (semi-transparent)
          INSERT_LONG_CMD ( cmd, NWORDS_MREC_VAR );
          break;

        case 0x64: // Textured Rectangle, variable size, opaque,
        	   // texture-blending
          INSERT_LONG_CMD ( cmd, NWORDS_TREC_VAR );
          break;
        case 0x65: // Textured Rectangle, variable size, opaque, raw-texture
          INSERT_LONG_CMD ( cmd, NWORDS_TREC_VAR );
          break;
        case 0x66: // Textured Rectangle, variable size, semi-transp,
        	   // texture-blending
          INSERT_LONG_CMD ( cmd, NWORDS_TREC_VAR );
          break;
        case 0x67: // Textured Rectangle, variable size, semi-transp,
        	   // raw-texture
          INSERT_LONG_CMD ( cmd, NWORDS_TREC_VAR );
          break;
        case 0x68: // Monochrome Rectangle (1x1) (Dot) (opaque)
          INSERT_LONG_CMD ( cmd, NWORDS_MREC );
          break;

        case 0x6A: // Monochrome Rectangle (1x1) (Dot) (semi-transparent)
          INSERT_LONG_CMD ( cmd, NWORDS_MREC );
          break;

        case 0x6C: // Textured Rectangle, 1x1 (nonsense), opaque,
        	   // texture-blending
          INSERT_LONG_CMD ( cmd, NWORDS_TREC );
          break;
        case 0x6D: // Textured Rectangle, 1x1 (nonsense), opaque,
        	   // raw-texture
          INSERT_LONG_CMD ( cmd, NWORDS_TREC );
          break;
        case 0x6E: // Textured Rectangle, 1x1 (nonsense), semi-transp,
        	   // texture-blending
          INSERT_LONG_CMD ( cmd, NWORDS_TREC );
          break;
        case 0x6F: // Textured Rectangle, 1x1 (nonsense), semi-transp,
        	   // raw-texture
          INSERT_LONG_CMD ( cmd, NWORDS_TREC );
          break;
        case 0x70: // Monochrome Rectangle (8x8) (opaque)
          INSERT_LONG_CMD ( cmd, NWORDS_MREC );
          break;

        case 0x72: // Monochrome Rectangle (8x8) (semi-transparent)
          INSERT_LONG_CMD ( cmd, NWORDS_MREC );
          break;

        case 0x74: // Textured Rectangle, 8x8, opaque,
        	   // texture-blending
          INSERT_LONG_CMD ( cmd, NWORDS_TREC );
          break;
        case 0x75: // Textured Rectangle, 8x8, opaque, raw-texture
          INSERT_LONG_CMD ( cmd, NWORDS_TREC );
          break;
        case 0x76: // Textured Rectangle, 8x8, semi-transparent,
        	   // texture-blending
          INSERT_LONG_CMD ( cmd, NWORDS_TREC );
          break;
        case 0x77: // Textured Rectangle, 8x8, semi-transparent,
        	   // raw-texture
          INSERT_LONG_CMD ( cmd, NWORDS_TREC );
          break;
        case 0x78: // Monochrome Rectangle (16x16) (opaque)
          INSERT_LONG_CMD ( cmd, NWORDS_MREC );
          break;

        case 0x7A: // Monochrome Rectangle (16x16) (semi-transparent)
          INSERT_LONG_CMD ( cmd, NWORDS_MREC );
          break;

        case 0x7C: // Textured Rectangle, 16x16, opaque,
        	   // texture-blending
          INSERT_LONG_CMD ( cmd, NWORDS_TREC );
          break;
        case 0x7D: // Textured Rectangle, 16x16, opaque, raw-texture
          INSERT_LONG_CMD ( cmd, NWORDS_TREC );
          break;
        case 0x7E: // Textured Rectangle, 16x16, semi-transparent,
        	   // texture-blending
          INSERT_LONG_CMD ( cmd, NWORDS_TREC );
          break;
        case 0x7F: // Textured Rectangle, 16x16, semi-transparent,
        	   // raw-texture
          INSERT_LONG_CMD ( cmd, NWORDS_TREC );
          break;
        case 0x80 ... 0x9F: // Copy Rectangle (VRAM to VRAM)
          INSERT_LONG_CMD ( cmd, NWORDS_VRAM2VRAM );
          break;
        case 0xA0 ... 0xBF: // Copy Rectangle (CPU to VRAM)
          fifo_push ( cmd );
          _render.state= WAIT_WRITE_XY_COPY;
          break;
        case 0xC0 ... 0xDF: // Copy Rectangle (VRAM to CPU)
          fifo_push ( cmd );
          _render.state= WAIT_READ_XY_COPY;
          break;
        case 0xE0: break; // Nop mirror.
        case 0xE1: // Set Draw mode.
          INSERT_SHORT_CMD ( cmd );
          break;
        case 0xE2: // Set Texture Window
          INSERT_SHORT_CMD ( cmd );
          break;
        case 0xE3: set_draw_area_top_left ( cmd ); break;
        case 0xE4: set_draw_area_bottom_right ( cmd ); break;
        case 0xE5: set_drawing_offset ( cmd ); break;
        case 0xE6:
          INSERT_SHORT_CMD ( cmd );
          break;
        case 0xE7 ... 0xEF: // Nop mirror.
          break;
          
        default:
          _warning ( _udata,
        	     "GPU (GP0): comandament desconegut %02X",
        	     cmd>>24 );
          break;
        }
      break;

      // Espera paraules comandament llarg.
    case WAIT_WORDS:
      fifo_push ( cmd );
      if ( --_render.nwords == 0 )
        {
          _render.state= WAIT_CMD;
          ++_fifo.nactions;
          run_fifo_cmds ();
        }
      break;

      // Monochrome Poly-line
    case WAIT_V1_POLY_MLINE:
      fifo_push ( cmd );
      _render.state= WAIT_V2_POLY_MLINE;
      break;
    case WAIT_V2_POLY_MLINE:
      fifo_push ( cmd );
      ++_fifo.nactions;
      run_fifo_cmds (); // <-- Dibuixa cada nou punt
      _render.state= WAIT_VN_POLY_MLINE;
      break;
    case WAIT_VN_POLY_MLINE:
      fifo_push ( cmd );
      ++_fifo.nactions;
      run_fifo_cmds (); // Acabar també és una acció.
      if ( cmd == 0x55555555 || cmd == 0x50005000 )
        _render.state= WAIT_CMD;
      break;

      // Shaded Poly-line
    case WAIT_V1_POLY_SLINE:
      fifo_push ( cmd );
      _render.state= WAIT_C2_POLY_SLINE;
      break;
    case WAIT_C2_POLY_SLINE:
      fifo_push ( cmd );
      _render.state= WAIT_V2_POLY_SLINE;
      break;
    case WAIT_V2_POLY_SLINE:
      fifo_push ( cmd );
      ++_fifo.nactions;
      run_fifo_cmds (); // <-- Dibuixa cada nou punt
      _render.state= WAIT_CN_POLY_SLINE;
      break;
    case WAIT_CN_POLY_SLINE:
      fifo_push ( cmd );
      if ( cmd != 0x55555555 && cmd != 0x50005000 )
        _render.state= WAIT_VN_POLY_SLINE;
      else
        {
          ++_fifo.nactions;
          run_fifo_cmds (); // Acabar també és una acció.
          _render.state= WAIT_CMD;
        }
      break;
    case WAIT_VN_POLY_SLINE:
      fifo_push ( cmd );
      ++_fifo.nactions;
      run_fifo_cmds (); // <-- Dibuixa cada nou punt
      _render.state= WAIT_CN_POLY_SLINE;
      break;

      // Copy Rectangle (CPU -> VRAM).
    case WAIT_WRITE_XY_COPY:
      fifo_push ( cmd );
      _render.state= WAIT_WRITE_WIDTH_HEIGHT_COPY;
      break;
    case WAIT_WRITE_WIDTH_HEIGHT_COPY:
      // Calcula paraules
      w= (((cmd&0x3FF)-1)&0x3FF) + 1;
      h= ((((cmd>>16)&0x1FF)-1)&0x1FF) + 1;
      size= h*w;
      _render.nwords= size/2;
      if ( size%2 ) ++_render.nwords;
      // Continua normalment.
      fifo_push ( cmd );
      ++_fifo.nactions;
      run_fifo_cmds (); // <-- Inicialitzem la copia.
      _render.state= _render.nwords>0 ? WAIT_WRITE_DATA_COPY : WAIT_CMD;
      break;
    case WAIT_WRITE_DATA_COPY:
      fifo_push ( cmd );
      ++_fifo.nactions;
      run_fifo_cmds (); // <-- Cada nova paraula és una acció.
      if ( --_render.nwords == 0 )
        _render.state= WAIT_CMD;
      break;

      // Copy Rectangle.
    case WAIT_READ_XY_COPY:
      fifo_push ( cmd );
      _render.state= WAIT_READ_WIDTH_HEIGHT_COPY;
      break;
    case WAIT_READ_WIDTH_HEIGHT_COPY:
      fifo_push ( cmd );
      ++_fifo.nactions;
      run_fifo_cmds (); // <-- Inicialitzem la copia.
      _render.state= WAIT_READ_DATA_COPY;
      break;
    case WAIT_READ_DATA_COPY:
      _warning ( _udata,
        	 "GPU GP0: s'ignorarà la paraula %X perquè actualment s'està"
        	 " fent una transferència VRAM a CPU." );
      break;
      
    default: break;
      
    }
  
} /* end gp0_cmd */


static void
gp0_cmd_trace (
               const uint32_t real_cmd
               )
{

  static PSX_GPUCmd cmd;

  bool ready;
  
  
  // Desenssambla.
  ready= false;
  switch ( _render.state )
    {
      
      // Espera comandament.
    case WAIT_CMD:
      cmd.word= real_cmd;
      cmd.ops= 0;
      cmd.Nv= 0;
      cmd.width= cmd.height= -1;
      switch ( real_cmd>>24 )
        {
        case 0x00: // Nop.
          cmd.name= PSX_GP0_NOP;
          ready= true;
          break;
        case 0x01: break; // Clear Cache??? <-- No implementat.
        case 0x02: break; // Fill rectangle in VRAM
        case 0x03: break; // Desconegut.
        case 0x04 ... 0x1E: // Nop mirror
          cmd.name= PSX_GP0_NOP;
          ready= true;
          break;
        case 0x1F: break; // Interrupt Request (IRQ1)
        case 0x20: // Monochrome three-point polygon, opaque
        case 0x21: break;
        case 0x22: // Monochrome three-point polygon, semi-transparent
        case 0x23: break;
        case 0x24: // Textured three-point polygon, opaque, texture-blending
          break;
        case 0x25: // Textured three-point polygon, opaque, raw-texture
          break;
        case 0x26: // Textured three-point polygon, semi-transparent,
        	   // texture-blending
          break;
        case 0x27: // Textured three-point polygon, semi-transparent,
        	   // raw-texture
          break;
        case 0x28: // Monochrome four-point polygon, opaque
        case 0x29: break;
        case 0x2A: // Monochrome four-point polygon, semi-transparent
        case 0x2B: break;
        case 0x2C: // Textured four-point polygon, opaque, texture-blending
          break;
        case 0x2D: // Textured four-point polygon, opaque, raw-texture
          break;
        case 0x2E: // Textured four-point polygon, semi-transparent,
        	   // texture-blending
          break;
        case 0x2F: // Textured four-point polygon, semi-transparent,
        	   // raw-texture
          break;
        case 0x30: // Shaded three-point polygon, opaque
        case 0x31:
          break;
        case 0x32: // Shaded three-point polygon, semi-transparent
        case 0x33: break;
        case 0x34: // Shaded Textured three-point polygon, opaque,
        	   // texture-blending
          break;
        case 0x35: // Shaded Textured three-point polygon, opaque,
        	   // raw-texture ¿¿??
          break;
        case 0x36: // Shaded Textured three-point polygon,
        	   // semi-transparent, texture-blending
          break;
        case 0x37: // Shaded Textured three-point polygon,
        	   // semi-transparent, raw-texture ¿¿??
          break;
        case 0x38: // Shaded four-point polygon, opaque
        case 0x39: break;
        case 0x3A: // Shaded four-point polygon, semi-transparent
        case 0x3B: break;
        case 0x3C: // Shaded Textured four-point polygon, opaque,
        	   // texture-blending
          break;
        case 0x3D: // Shaded Textured four-point polygon, opaque,
        	   // raw-texture ¿¿??
          break;
        case 0x3E: // Shaded Textured four-point polygon,
        	   // semi-transparent, texture-blending
          break;
        case 0x3F: // Shaded Textured four-point polygon,
        	   // semi-transparent, raw-texture ¿¿??
          break;
        case 0x40: // Monochrome line, opaque
        case 0x41: break;
        case 0x42: // Monochrome line, semi-transparent
        case 0x43: break;
          
        case 0x48: // Monochrome Poly-line, opaque
        case 0x49:
        case 0x4C: break;
        case 0x4A: // Monochrome Poly-line, semi-transparent
        case 0x4B: break;
          
        case 0x50: // Shaded line, opaque
        case 0x51:
        case 0x55: break;
        case 0x52: // Shaded line, semi-transparent
        case 0x53: break;
          
        case 0x58: // Shaded Poly-line, opaque
        case 0x59: break;
        case 0x5A: // Shaded Poly-line, semi-transparent
        case 0x5B:
        case 0x5E: break;
          
        case 0x60: // Monochrome Rectangle (variable size) (opaque)
          break;
          
        case 0x62: // Monochrome Rectangle (variable size)
        	   // (semi-transparent)
          break;
          
        case 0x64: // Textured Rectangle, variable size, opaque,
        	   // texture-blending
          break;
        case 0x65: // Textured Rectangle, variable size, opaque,
        	   // raw-texture
          break;
        case 0x66: // Textured Rectangle, variable size, semi-transp,
        	   // texture-blending
          break;
        case 0x67: // Textured Rectangle, variable size, semi-transp,
        	   // raw-texture
          break;
        case 0x68: // Monochrome Rectangle (1x1) (Dot) (opaque)
          break;
          
        case 0x6A: // Monochrome Rectangle (1x1) (Dot)
        	   // (semi-transparent)
          break;
          
        case 0x6C: // Textured Rectangle, 1x1 (nonsense), opaque,
        	   // texture-blending
          break;
        case 0x6D: // Textured Rectangle, 1x1 (nonsense), opaque,
        	   // raw-texture
          break;
        case 0x6E: // Textured Rectangle, 1x1 (nonsense), semi-transp,
        	   // texture-blending
          break;
        case 0x6F: // Textured Rectangle, 1x1 (nonsense), semi-transp,
        	   // raw-texture
          break;
        case 0x70: // Monochrome Rectangle (8x8) (opaque)
          break;
          
        case 0x72: // Monochrome Rectangle (8x8) (semi-transparent)
          break;
          
        case 0x74: // Textured Rectangle, 8x8, opaque,
        	   // texture-blending
          break;
        case 0x75: // Textured Rectangle, 8x8, opaque, raw-texture
          break;
        case 0x76: // Textured Rectangle, 8x8, semi-transparent,
        	   // texture-blending
          break;
        case 0x77: // Textured Rectangle, 8x8, semi-transparent,
        	   // raw-texture
          break;
        case 0x78: // Monochrome Rectangle (16x16) (opaque)
          break;
          
        case 0x7A: // Monochrome Rectangle (16x16) (semi-transparent)
          break;
          
        case 0x7C: // Textured Rectangle, 16x16, opaque,
        	   // texture-blending
          break;
        case 0x7D: // Textured Rectangle, 16x16, opaque, raw-texture
          break;
        case 0x7E: // Textured Rectangle, 16x16, semi-transparent,
        	   // texture-blending
          break;
        case 0x7F: // Textured Rectangle, 16x16, semi-transparent,
        	   // raw-texture
          break;
        case 0x80 ... 0x9F: // Copy Rectangle (VRAM to VRAM)
          break;
        case 0xA0 ... 0xBF: // Copy Rectangle (CPU to VRAM)
          break;
        case 0xC0 ... 0xDF: // Copy Rectangle (VRAM to CPU)
          break;
        case 0xE0: // Nop mirror.
          cmd.name= PSX_GP0_NOP;
          ready= true;
          break;
        case 0xE1: break; // Set Draw Mode
        case 0xE2: break; // Set Texture Window
        case 0xE3: cmd.name= PSX_GP0_SET_TOP_LEFT; ready= true; break;
        case 0xE4: cmd.name= PSX_GP0_SET_BOTTOM_RIGHT; ready= true; break;
        case 0xE5: cmd.name= PSX_GP0_SET_OFFSET; ready= true; break;
        case 0xE6: break; // Mask Bit Setting
        case 0xE7 ... 0xEF: // Nop mirror.
          cmd.name= PSX_GP0_NOP;
          ready= true;
          break;

        default:
          cmd.name= PSX_GP0_UNK;
          ready= true;
          break;
        }
      break;

      // Espera paraules comandament llarg.
    case WAIT_WORDS: break;

      // Monochrome Poly-line
    case WAIT_V1_POLY_MLINE: break;
    case WAIT_V2_POLY_MLINE: break;
    case WAIT_VN_POLY_MLINE: break;

      // Shaded Poly-line
    case WAIT_V1_POLY_SLINE: break;
    case WAIT_C2_POLY_SLINE: break;
    case WAIT_V2_POLY_SLINE: break;
    case WAIT_CN_POLY_SLINE: break;
    case WAIT_VN_POLY_SLINE: break;

      // Copy Rectangle (CPU -> VRAM).
    case WAIT_WRITE_XY_COPY: break;
    case WAIT_WRITE_WIDTH_HEIGHT_COPY: break;
    case WAIT_WRITE_DATA_COPY: break;

      // Copy Rectangle.
    case WAIT_READ_XY_COPY: break;
    case WAIT_READ_WIDTH_HEIGHT_COPY: break;
    case WAIT_READ_DATA_COPY: break;
      
    default: break;
      
    }
  if ( ready ) _gpu_cmd_trace ( &cmd, _udata );
  
  // Executa de veritat el comandament.
  gp0_cmd ( real_cmd );
  
} // end gp0_cmd_trace


static void
gp1_cmd (
         const uint32_t cmd
         )
{
  
  switch ( (cmd>>24)&0x3F )
    {
    case 0x00: reset_cmd (); break;
    case 0x01: reset_cmd_buffer (); break;
    case 0x02: ACK_IRQ; break;
    case 0x03: enable_display ( (cmd&0x1)==0 ); break;
    case 0x04: /* DMA Direction / Data Request. */
      _display.transfer_mode= cmd&0x3;
      update_dma_sync ();
      break;
    case 0x05: /* Start of display area. */
      _display.x= cmd&0x3FE; // Segonds mednafen és 0x3fe cmd&0x3FF;
      _display.y= (cmd>>10)&0x1FF;
      break;
    case 0x06: set_x1_x2_cmd ( cmd ); break;
    case 0x07: set_y1_y2_cmd ( cmd ); break;
    case 0x08: set_display_mode ( cmd ); break;
    case 0x09: _display.texture_disable= ((cmd&0x1)!=0); break;
    case 0x0A: break; /* Not used? */
    case 0x0B: break; /* Unknown/Internal? */
    case 0x0C:
    case 0x0D:
    case 0x0E:
    case 0x0F: break;
    case 0x10: /* Get GPU info */
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x1B:
    case 0x1C:
    case 0x1D:
    case 0x1E:
    case 0x1F: get_gpu_info ( cmd ); break;
    case 0x20: break; /* Ancient Texture Disable */
    default: /* Not used? */
      break;
    }
  
} // end gp1_cmd


static void
gp1_cmd_trace (
               const uint32_t arg
               )
{
  
  static PSX_GPUCmd cmd;


  // Traça.
  cmd.Nv= 0;
  cmd.ops= 0;
  cmd.word= arg;
  cmd.width= cmd.height= -1;
  switch ( (arg>>24)&0x3F )
    {
    case 0x00: cmd.name= PSX_GP1_RESET; break;
    case 0x01: cmd.name= PSX_GP1_RESET_BUFFER; break;
    case 0x02: cmd.name= PSX_GP1_ACK; break;
    case 0x03: cmd.name= PSX_GP1_ENABLE; break;
    case 0x04: // DMA Direction / Data Request.
      cmd.name= PSX_GP1_DATA_REQUEST;
      break;
    case 0x05: // Start of display area.
      cmd.name= PSX_GP1_START_DISP;
      cmd.v[0].x= arg&0x3FF;
      cmd.v[0].y= (arg>>10)&0x1FF;
      cmd.Nv= 1;
      break;
    case 0x06:
      cmd.name= PSX_GP1_HOR_DISP_RANGE;
      cmd.v[0].x= arg&0xFFF;
      cmd.v[0].y= (arg>>12)&0xFFF;
      cmd.Nv= 1;
      break;
    case 0x07:
      cmd.name= PSX_GP1_VER_DISP_RANGE;
      cmd.v[0].x= arg&0x3FF;
      cmd.v[0].y= (arg>>10)&0x3FF;
      cmd.Nv= 1;
      break;
    case 0x08: cmd.name= PSX_GP1_SET_DISP_MODE; break;
    case 0x09: cmd.name= PSX_GP1_TEXT_DISABLE; break;
    case 0x0A: cmd.name= PSX_GP1_UNK; break; // Not used?
    case 0x0B: cmd.name= PSX_GP1_UNK; break; // Unknown/Internal?
    case 0x0C:
    case 0x0D:
    case 0x0E:
    case 0x0F: cmd.name= PSX_GP1_UNK; break;
    case 0x10: // Get GPU info
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x1B:
    case 0x1C:
    case 0x1D:
    case 0x1E:
    case 0x1F: cmd.name= PSX_GP1_GET_INFO; break;
    case 0x20:
      cmd.name= PSX_GP1_OLD_TEXT_DISABLE;
      break; // Ancient Texture Disable
    default: // Not used?
      cmd.name= PSX_GP1_UNK;
      break;
    }
  _gpu_cmd_trace ( &cmd, _udata );
  
  // Executa de veritat el comandament.
  gp1_cmd ( arg );
  
} // end gp1_cmd_trace


/* No inclou el clock. */
static uint32_t
gpu_read (void)
{
  return _read.vram_transfer ? copy_vram2cpu () : _read.data;
} /* end gpu_read */




/**********************/
/* FUNCIONS PÚBLIQUES */
/**********************/

void
PSX_gpu_end_iter (void)
{

  int cc;

  
  /* NOTA:
   * 1 cicle GPU = 7/11 * cicles CPU
   *
   * En els cc (els que es gasten directament ací, es representen amb
   * fraccions 1/11 de CPU, representades com 11*cicle de cpu, es a
   * dir, 1-> 1/11, 2->2/11,...,11 -> 11/11 (1) )
   *
   * Per tant per a calcular quants cc de GPU tinc a partir de cc's
   * tinc que dividir entre 7, es a dir, cada 7 fraccions 1/11 tinc un
   * cicle de GPU.
   */
  cc= PSX_Clock-_timing.cc_used;
  if ( cc > 0 )
    {
      _timing.cc+= cc*11;
      _timing.cc_used+= cc;
      if ( _timing.cctoEvent && _timing.cc >= _timing.cctoEvent )
        clock ();
    }
  _timing.cc_used= 0;
  
} // end PSX_gpu_end_iter


int
PSX_gpu_next_event_cc (void)
{
  
  int cc,ret;
  
  
  if ( _timing.cctoEvent )
    {
      cc= _timing.cctoEvent - _timing.cc;
      assert ( cc >= 0 );
      ret= cc/11 + ((cc%11) != 0);
    }
  else ret= -1;

  return ret;
  
} // end PSX_gpu_next_event_cc


void
PSX_gpu_init (
              PSX_Renderer    *renderer,
              PSX_GPUCmdTrace *gpu_cmd,
              PSX_Warning     *warning,
              void            *udata
              )
{

  /* Callbacks. */
  _renderer= renderer;
  _gpu_cmd_trace= gpu_cmd;
  _warning= warning;
  _udata= udata;

  /* Frame buffer. */
  memset ( _fb, 0, sizeof(_fb) );
  _renderer_locked= true; // Assegurem que s'inicialitze almenys una vegada
  UNLOCK_RENDERER;

  /* Display. */
  _display.enabled= false;
  _display.irq_enabled= false;
  _display.transfer_mode= TM_OFF;
  _display.x= 0;
  _display.y= 0;
  _display.x1= 0;
  _display.x2= 1;
  _display.screen_x0= 0.0;
  _display.screen_x1= 1.0;
  _display.y1= 0;
  _display.y2= 1;
  _display.screen_y0= 0.0;
  _display.screen_y1= 1.0;
  _display.hres= HRES_256;
  _display.vres= VRES_240;
  _display.fb_line_width= 256;
  _display.reverseflag= false;
  _display.tv_mode= NTSC;
  _display.vertical_interlace= false;
  _display.color_depth_24bit= false;
  _display.texture_disable= false;
  _display.interlace_field= 0;

  /* GPU read. */
  _read.data= 0;
  _read.vram_transfer= false;

  // Fifo
  _fifo.p= 0;
  _fifo.N= 0;
  _fifo.nactions= 0;
  _fifo.state= FIFO_WAIT_CMD;
  _fifo.busy= false;
  
  /* Inicialitza timing. Té que estar al final !!!! */
  _timing.cc= 0;
  _timing.cc_used= 0;
  _timing.line= 0;
  _timing.ccline= 0;
  _timing.signal_HBlank= false;
  _timing.cctoIdle= 0;
  _timing.update_timing_event= true;
  update_timing ();

  /* Inicialitza rendering. */
  _render.state= WAIT_CMD;
  _render.nwords= 0;
  memset ( &_render.args, 0, sizeof(_render.args) );
  memset ( &_render.def_args, 0, sizeof(_render.def_args) );
  reset_render ();

  // DMA.
  _dma_sync.request= false;
  
  // Inicialitza estat tracer.
  PSX_gpu_set_mode_trace ( false );
  
} /* end PSX_gpu_init */


void
PSX_gpu_gp0 (
             const uint32_t cmd
             )
{

  clock ();
  
  _gp0_cmd ( cmd );
  update_dma_sync (); // <-- Pot ser al afegir una paraula s'ha buidat
        	      // la FIFO.
  
} /* end PSX_gpu_gp0 */


void
PSX_gpu_gp1 (
             const uint32_t cmd
             )
{
  
  clock ();
  _gp1_cmd ( cmd );
  
} // end PSX_gpu_gp1


uint32_t
PSX_gpu_read (void)
{

  clock ();

  return gpu_read ();
  
} /* end PSX_gpu_read */


uint32_t
PSX_gpu_stat (void)
{

  bool exec_cmd_busy,ready_vram2cpu,dma_info,interlace_odd;
  uint32_t ret;
  
  
  clock ();

  // NOTA! L'antiga implementació de 'exec_cmd_busy' és més correcta
  // pel que fa a NOCASH, però crec que la implementació actual
  // funcionarà.
  exec_cmd_busy= (_fifo.busy || _render.state>=WAIT_VN_POLY_SLINE);
  //exec_cmd_busy= (_render.state>=WAIT_V1_MPOL &&
  //                _render.state<=WAIT_VN_SLINE);
  ready_vram2cpu= _render.state==WAIT_READ_DATA_COPY;
  switch ( _display.transfer_mode )
    {
    case TM_OFF: dma_info= false; break;
    case TM_FIFO: dma_info= _fifo.N!=FIFO_SIZE; break;
    case TM_DMA_WRITE: dma_info= !exec_cmd_busy; break;
    case TM_DMA_READ: dma_info= ready_vram2cpu; break;
    default: dma_info= false; break; // <-- CALLA !!!
    }
  if ( ((uint32_t) _timing.line >= _display.y1 &&
        (uint32_t) _timing.line < _display.y2) )
    {
      if ( _display.vres==VRES_240 ) interlace_odd= ((_timing.line%2)==1);
      else interlace_odd= (_display.interlace_field!=0);
    }
  else interlace_odd= false;
  
  ret=
    // GP0(E1h)
    _render.def_args.texpage_x |
    (_render.def_args.texpage_y<<4) |
    (_render.def_args.transparency<<5) |
    (_render.def_args.texture_mode<<7) |
    ((_render.def_args.dithering?0x1:0x0)<<9) |
    ((_render.drawing_da_enabled?0x1:0x0)<<10) |
    ((_render.texture_disabled?0x1:0x0)<<15) |
    // GP0(E6h)
    ((_render.args.set_mask?0x1:0x0)<<11) |
    ((_render.args.check_mask?0x1:0x0)<<12) |
    // Interlace field
    // OJO!!! Segons nochas si el vertical interlace està desactivat
    // açò deuria de ser sempre 1. Però mednafen ho fa al revés.
    (((/*!_display.vertical_interlace||*/_display.interlace_field)?0x1:0x0)<<13) |
    // GP1(08h)
    ((_display.reverseflag?0x1:0x0)<<14) |
    (((_display.hres==HRES_368)?0x1:0x0)<<16) |
    (_display.hres<<17) |
    (_display.vres_original<<19) |
    ((_display.tv_mode==PAL)<<20) |
    ((_display.color_depth_24bit?0x1:0x0)<<21) |
    ((_display.vertical_interlace?0x1:0x0)<<22) |
    // GP1(03h)
    ((_display.enabled?0x0:0x1)<<23) |
    // GP0(1Fh)/GP1(02h)
    ((_display.irq_enabled?0x1:00)<<24) |
    // GP1(04h)
    (_display.transfer_mode<<29) |
    // Altres bits
    ((dma_info?0x1:0x0)<<25) |
    (((!exec_cmd_busy && _render.state!=WAIT_WRITE_DATA_COPY)?0x1:0x0)<<26) |
    //  <--- Alternativa: _render.state==WAIT_CMD
    ((ready_vram2cpu?0x1:0x0)<<27) |
    ((exec_cmd_busy?0x0:0x1)<<28) |
    ((interlace_odd?0x1:0x0)<<31)
    ;
  
  return ret;
  
} // end PSX_gpu_stat


bool
PSX_gpu_dma_sync (
                  const uint32_t nwords
                  )
{

  bool ret;

  
  // NOTA!!! Els canals en mode 1 i 2 tenen que explicitament activar
  // el canal a més de tornar si l'accepten o no.
  if ( _dma_sync.request )
    {
      /*
      <-- No faig el warning perque al canviar el dpcr es sol
         tornar a preguntar el sync, inclús quan s'ha denegat
         previament.
      _warning ( _udata,
                 "GPU (DMA2) sync: s'ha produït un anidament"
                 " de syncs inesperat" );
      */
      return false; // Important denegar si ja ho estava
    }
  
  ret= check_dma_sync ();
  if ( ret ) PSX_dma_active_channel ( 2 );
  else _dma_sync.request= true;
  
  return ret;
  
} // end PSX_gpu_dma_sync


void
PSX_gpu_dma_write (
                   uint32_t data
                   )
{

  if ( _display.transfer_mode != TM_DMA_WRITE )
    {
      _warning ( _udata,
        	 "GPU (DMA2) write: el canal no està en mode escriptura" );
      return;
    }
  
  // NOTA!! No cridem update_dma_sync perquè estem dins del DMA!!!
  clock ();
  
  _gp0_cmd ( data );
  
} // end PSX_gpu_dma_write


uint32_t
PSX_gpu_dma_read (void)
{

  uint32_t ret;

  
  if ( _display.transfer_mode != TM_DMA_READ )
    {
      _warning ( _udata, "GPU (DMA2) read: el canal no està en mode lectura" );
      return 0xFF00FF00;
    }

  clock ();
  
  ret= gpu_read ();

  return ret;
  
} // end PSX_gpu_dma_read


void
PSX_gpu_signal_hblank (
        	       const bool enable
        	       )
{

  clock ();
  
  _timing.signal_HBlank= enable;
  update_timing ();
  
} /* end PSX_gpu_signal_hblank */


const uint16_t *
PSX_gpu_get_frame_buffer (void)
{

  LOCK_RENDERER;
  
  return &(_fb[0]);
  
} // end PSX_gpu_get_frame_buffer


void
PSX_gpu_set_mode_trace (
        		const bool val
        		)
{

  if ( val && _gpu_cmd_trace != NULL )
    {
      _gp0_cmd= gp0_cmd_trace;
      _gp1_cmd= gp1_cmd_trace;
      _run_fifo_cmd= run_fifo_cmd_trace;
    }
  else
    {
      _gp0_cmd= gp0_cmd;
      _gp1_cmd= gp1_cmd;
      _run_fifo_cmd= run_fifo_cmd;
    }
  
} // end PSX_gpu_set_mode_trace


void
PSX_gpu_reset (void)
{

  // Fifo.
  _fifo.p= 0;
  _fifo.N= 0;
  _fifo.nactions= 0;
  _fifo.state= FIFO_WAIT_CMD;
  _fifo.busy= false;

  // DMA.
  _dma_sync.request= false;
  
  // Command
  reset_cmd ();

  // Timing (pot ser redundant)
  _timing.cc= 0;
  _timing.cc_used= 0;
  _timing.line= 0;
  _timing.ccline= 0;
  _timing.signal_HBlank= false;
  _timing.cctoIdle= 0;
  _timing.update_timing_event= true;
  update_timing ();
  
} // end PSX_gpu_reset

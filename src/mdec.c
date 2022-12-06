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
 *  mdec.c - Implementació del mòdul MDEC.
 *
 */
/*
 *  NOTES!!!
 *
 *  En aquesta nova implementació vaig a:
 *
 *   - Emprar explícitament FIFOs d'entrada i d'eixida.
 *
 *   - Totes les peticions d'escriptura s'acceptaran immediatament.
 *
 *   - Sobre el DMA:
 *
 *      * Si vols escriure s'accepta sempre sense comprovar espai.
 *
 *      * Si vol llegir es comprova que hi han prou dades en la FIFO d'eixida.
 *
 *   - Cada vegada que s'esciuen dades en la FIFO d'entrada s'intenta
 *     processar el més ràpid que es puga.
 *
 *   - En QT i ST no hi huarà cap tipus d'espera.
 *
 *   - En Decode, des de que s'inicialitza un macroblock fins que es
 *     genera el resultat final han de passar almenys un número de
 *     cicles. Ja sé que la generació del macrobloc es pot interrompre
 *     per algun altre tipus d'operació, en eixe cas el bloc es
 *     generarà més ràpid. Però no vaig a calfar-me el cap perquè no
 *     crec que siga habitual. Si al acabar el bloc encara no han
 *     passat els cicles, es tindrà que esperar.
 *
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "PSX.h"




/**********/
/* MACROS */
/**********/

// Segons
// https://wiki.multimedia.cx/index.php/PlayStation_Motion_Decoder
// "The unit is designed to decode 9000 macroblocks per second." Per
// tant calcule que cada macroblock tindrà aproximadament els següents
// cicles: 33868800/9000 -> 3763
#define CCMACROBLOCK 3763
#define CCMAX 100000

#define FIFO_SIZE 0x40000 // Tamany potència de 2, que càpiga una imatge.

#define QT_L (_qt[0])
#define QT_C (_qt[1])

#define BIN_SIZE 4 // Important que siga una potencia de 2.

// Codis retornars per run_decode
#define BIN_WAIT -1
#define BOUT_FULL -2

// Codis current-block.
#define CB_Y1 0
#define CB_Y2 1
#define CB_Y3 2
#define CB_Y4 3
#define CB_CR 4
#define CB_CB 5
#define CB_Y_MONO 4

// Signed10.
#define SIGNED10BIT(VAL)                                                \
  (((VAL)&0x200) ? ((int) ((VAL)&0x3FF))-1024 : (int) ((VAL)&0x3FF))




/*********/
/* TIPUS */
/*********/

typedef struct
{
  uint32_t v[FIFO_SIZE];
  int      p;
  int      N;
} fifo_t;




/*************/
/* CONSTANTS */
/*************/

static const int16_t DEFAULT_ST[64]=
  {
    0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82,
    0x7D8A, 0x6A6D, 0x471C, 0x18F8, 0xE707, 0xB8E3, 0x9592, 0x8275,
    0x7641, 0x30FB, 0xCF04, 0x89BE, 0x89BE, 0xCF04, 0x30FB, 0x7641,
    0x6A6D, 0xE707, 0x8275, 0xB8E3, 0x471C, 0x7D8A, 0x18F8, 0x9592,
    0x5A82, 0xA57D, 0xA57D, 0x5A82, 0x5A82, 0xA57D, 0xA57D, 0x5A82,
    0x471C, 0x8275, 0x18F8, 0x6A6D, 0x9592, 0xE707, 0x7D8A, 0xB8E3,
    0x30FB, 0x89BE, 0x7641, 0xCF04, 0xCF04, 0x7641, 0x89BE, 0x30FB,
    0x18F8, 0xB8E3, 0x6A6D, 0x8275, 0x7D8A, 0x9592, 0x471C, 0xE707
  };

// Taules amb constants.
static const double SCALEFACTOR[8]=
  {
    1.000000000, 1.387039845, 1.306562965, 1.175875602,
    1.000000000, 0.785694958, 0.541196100, 0.275899379
  };

static const int ZIGZAG[64]=
  {
    0 , 1 , 5 , 6 , 14, 15, 27, 28,
    2 , 4 , 7 , 13, 16, 26, 29, 42,
    3 , 8 , 12, 17, 25, 30, 41, 43,
    9 , 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63
  };




/*********/
/* ESTAT */
/*********/

// Callbacks.
static PSX_Warning *_warning;
static void *_udata;

// Taules de quantificació.
static uint8_t _qt[2][64];

// Taula d'escalat.
static struct
{
  double   v[64];
  uint64_t diff;    // Si és 0 aleshores és igual al per defecte.
} _st;

static double _scalezag[64];
static int _zagzig[64];

// Fifos
static fifo_t _fifo_in;
static fifo_t _fifo_out;

// Timing.
static struct
{
  int  cc;
  int  cc_used;
  int  cc_current_macroblock;
  int  cctoWriteMacroblock;
  int  cctoEvent;
} _timing;

// Estat.
static struct
{
  int      data_out_depth; // 0=4bit, 1=8bit, 2=24bit, 3=15bit
  bool     data_out_signed;
  bool     data_out_bit15_set;
  uint16_t remaining_words; // Per al status.
  uint32_t current_block;
  bool     waiting_write_macroblock;
  enum
    {
     DECODE,
     SET_QT,
     SET_ST,
     NONE
  }        cmd;
  union
  {
    struct
    {
      bool end; // Indica que s'ha acabat.
      int  cr_state; // Estat de la corutina.
      int  rldb_state; // Estat de la corutina rl_decode_block
      bool fast_idct;
    } decode;
    struct
    {
      int pos;    // Següent posició a escriure.
      int N;      // Número total de bytes a escriure.
    } set_qt;
    struct
    {
      int      pos;     // Següent posició a escriure.
      uint64_t mask;    // Utilitzat per a calcular diff.
    } set_st;
  } var;
} _state;

// Variables funcions estàtiques.
static struct
{

  // rl_decode_block.
  int32_t rldb_n;
  int rldb_k,rldb_q_scale;
  bool rldb_stop;

  // run_decode blocks.
  double  crblk[64];
  double  cbblk[64];
  double  yblk[64];
  uint8_t fb[16*16*3 /*Grandària màxima 24b*/];
  int     fb_N; // Paraules que s'han escrit.
  
} _v;

// Buffer d'entrada per al decode.
static struct
{
  uint16_t v[BIN_SIZE]; // No s'ha de plenar mai.
  int      p;    // Posició del primer valor.
  int      N;    // Número de valors.
} _bin;

// Control del DMA.
static struct
{
  bool in_enabled;
  bool out_enabled;
  bool out_waiting; // Hi ha un sync pendent.
  int  out_waiting_nwords;
} _dma;




/*********************/
/* FUNCIONS PRIVADES */
/*********************/

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


static void
swap_words (
            uint32_t  *buffer,
            const int  nwords
            )
{

  int n;
  uint32_t tmp,tmp2;
  

  for ( n= 0; n < nwords; ++n )
    {
      tmp= buffer[n];
      ((uint8_t *) &tmp2)[0]= ((const uint8_t *) &tmp)[2];
      ((uint8_t *) &tmp2)[1]= ((const uint8_t *) &tmp)[3];
      ((uint8_t *) &tmp2)[2]= ((const uint8_t *) &tmp)[0];
      ((uint8_t *) &tmp2)[3]= ((const uint8_t *) &tmp)[1];
      buffer[n]= tmp2;
    }
  
} // end swap_words
#endif


static void
update_timing_event (void)
{

  int tmp;

  
  // Actualitza cctoEvent
  _timing.cctoEvent= CCMAX;
  if ( _state.waiting_write_macroblock )
    _timing.cctoEvent= _timing.cctoWriteMacroblock;

  // Update PSX_NextEventCC
  tmp= PSX_mdec_next_event_cc ();
  if ( tmp != -1 )
    {
      tmp+= PSX_Clock;
      if ( tmp < PSX_NextEventCC )
        PSX_NextEventCC= tmp;
    }
  
} // end update_timing_event


static void
fifo_clear (
            fifo_t *f
            )
{

  f->p= 0;
  f->N= 0;
  
} // end fifo_clear


static void
init_fifo (
           fifo_t *f
           )
{

  fifo_clear ( f );
  memset ( f->v, 0, sizeof(f->v) );
  
} // end init_fifo


static void
init_st (void)
{
  
  int i;
  
  
  for ( i= 0; i < 64; ++i )
    _st.v[i]= DEFAULT_ST[i]/(/*16384.0*/8192.0*8);
  _st.diff= 0;
  
} // end init_st


static void
init_scalezag (void)
{

  int y,x;


  for ( y= 0; y < 8; ++y )
    for ( x= 0; x < 8; ++x )
      _scalezag[ZIGZAG[x+y*8]]= (SCALEFACTOR[x] * SCALEFACTOR[y])/8.0;
  
} // end init_scalezag


static void
init_zagzig (void)
{

  int i;


  for ( i= 0; i < 64; ++i )
    _zagzig[ZIGZAG[i]]= i;
  
} // end init_zagzig


static void
real_idct_core (
                double blk[64]
                )
{

  double *dst,*aux,*src,sum;
  double tmp[64];
  int p,x,y,z;

  
  src= &(blk[0]); dst= &(tmp[0]);
  for ( p= 0; p < 2; ++p )
    {
      for ( x= 0; x < 8; ++x )
        for ( y= 0; y < 8; ++y )
          {
            sum= 0;
            for ( z= 0; z < 8; ++z )
              sum+= src[y+z*8]*_st.v[x+z*8]; // El /8 està ja calculat.
            //dst[x+y*8]= sum; /* com treballe amb doubles no cal redondejar. */
            dst[x+y*8]= (double) ((int) (sum+0.5));
          }
      aux= dst; dst= src; src= aux;
    }
  
} // end real_idct_core


#if 0
static void
fast_idct_core (
                double blk[64]
                )
{

  double *dst,*aux,*src;
  double z10,z11,z12,z13,tmp0,tmp1,tmp2,tmp3,tmp4,tmp5,tmp6,tmp7,z5;
  int p,i,j;
  bool all_zero;
  

  src= &(blk[0]); dst= &(_v.idct_tmp[0]);
  for ( p= 0; p < 2; ++p )
    {
      for ( i= 0; i < 8; ++i )
        {

          /* Comprova que 1_7 són 0. En eixe cas omplit ràpid. */
          all_zero= true;
          for ( j= 1; j < 8; ++j )
            if ( src[j*8+i] != 0.0 )
              {
                all_zero= false;
                break;
              }
          if ( all_zero )
            {
              for ( j= 0; j < 8; ++j )
                dst[i*8+j]= src[0*8+i];
              continue; /* Següent. */
            }

          /* Si no és tot 0. */
          z10= src[0*8+i]+src[4*8+i]; z11= src[0*8+i]-src[4*8+i];
          z13= src[2*8+i]+src[6*8+i]; z12= src[2*8+i]-src[6*8+i];
          z12= 1.414213562*z12 - z13; /* sqrt(2) */
          tmp0= z10+z13; tmp3= z10-z13; tmp1= z11+z12; tmp2= z11-z12;
          z13= src[3*8+i]+src[5*8+i]; z10= src[3*8+i]-src[5*8+i];
          z11= src[1*8+i]+src[7*8+i]; z12= src[1*8+i]-src[7*8+i];
          z5= (1.847759065*(z12-z10)); /* sqrt(2)*scalefactor[2] */
          tmp7= z11+z13;
          tmp6= (2.613125930*(z10))+z5-tmp7; /* scalefactor[2]*2 */
          tmp5= (1.414213562*(z11-z13))-tmp6; /* sqrt(2) */
          tmp4= (1.082392200*(z12))-z5+tmp5; /* sqrt(2)/scalefactor[2] */
          dst[i*8+0]= tmp0+tmp7; dst[i*8+7]= tmp0-tmp7;
          dst[i*8+1]= tmp1+tmp6; dst[i*8+6]= tmp1-tmp6;
          dst[i*8+2]= tmp2+tmp5; dst[i*8+5]= tmp2-tmp5;
          dst[i*8+4]= tmp3+tmp4; dst[i*8+3]= tmp3-tmp4;
          
        }
      aux= dst; dst= src; src= aux;
    }
  
} /* end fast_idct_core */
#endif


static int32_t
read_bin (void)
{

  int32_t ret;

  
  // Si el buffer està buit.
  if ( _bin.N == 0 ) return BIN_WAIT;
  
  // Retorna el valor.
  ret= (int32_t) ((uint32_t) _bin.v[_bin.p++]);
  _bin.p&= (BIN_SIZE-1);
  --_bin.N;
  
  return ret;
  
} // end read_bin

#define BEGIN_RLDB switch ( _state.var.decode.rldb_state ) { case 0:
#define RETURN_RLDB(CODE)                                       \
  do { _state.var.decode.rldb_state= __LINE__; return CODE;        \
  case __LINE__:; } while (0)
#define END_RLDB } _state.var.decode.rldb_state= 0; return 0;


// Torna 0 si tot ha anat bé.
static int
rl_decode_block (
                 double        blk[64],
                 const uint8_t qt[64]
                 )
{
  
  int i,qt_aux;
  double val;
  

  BEGIN_RLDB;
  
  // Inicialització.
  for ( i= 0; i < 64; ++i ) blk[i]= 0.0;
  _v.rldb_k= 0;
  
  // Llig el primer valor.
  do {
    while ( (_v.rldb_n= read_bin ()) < 0 ) RETURN_RLDB(_v.rldb_n);
  } while ( _v.rldb_n == 0xFE00 );
  _v.rldb_q_scale= (_v.rldb_n>>10)&0x3F; // DCT.Q (Quantization factor)
  qt_aux= (int) ((unsigned int) qt[_v.rldb_k]);
  val= SIGNED10BIT(_v.rldb_n)*qt_aux; // DCT.DC (Direct Current reference)
  
  // Bucle.
  _v.rldb_stop= false;_state.var.decode.fast_idct= false; // DESHABILITAT!!!!!
  while ( !_v.rldb_stop )
    {
      
      // Inserta valor previ
      if ( _v.rldb_q_scale == 0 ) val= SIGNED10BIT(_v.rldb_n)*2;
      if ( val < -1024 ) val= -1024;
      else if ( val > 1023 ) val= 1023;
      //if ( _state.var.decode.fast_idct ) val= _v.rldb_val*scalezag[i]
      if ( _v.rldb_q_scale == 0 ) blk[_v.rldb_k]= val;
      else                        blk[_zagzig[_v.rldb_k]]= val;
      
      // Calcula valor per al següent
      while ( (_v.rldb_n= read_bin ()) < 0 ) RETURN_RLDB(_v.rldb_n);
      _v.rldb_k+= ((_v.rldb_n>>10)&0x3F)+1;
      _v.rldb_stop=  (_v.rldb_k > 63);
      if ( !_v.rldb_stop )
        {
          qt_aux= (int) ((unsigned int) qt[_v.rldb_k]);
          val= ((double) (SIGNED10BIT(_v.rldb_n)*qt_aux*_v.rldb_q_scale+4))/8.0;
        }
    }
  
  // IDCT.
  //if ( _state.var.decode.fast_idct ) fast_idct_core ( blk );
  /*else*/                               real_idct_core ( blk );
  
  END_RLDB;
  
} // end rl_decode_block


static void
yuv_to_rgb_24b (
                const double  crblk[64],
                const double  cbblk[64],
                const double  yblk[64],
                uint8_t      *fb,
                const int     xx,
                const int     yy
                )
{

  int row,col;
  double r,g,b,y;
  uint8_t *base_p,*p;
  const double *y_p;
  

  base_p= fb + (xx + yy*16)*3;
  y_p= &(yblk[0]);
  for ( row= 0; row < 8; ++row )
    {
      p= base_p;
      for ( col= 0; col < 8; ++col )
        {
          r= crblk[((xx+col)/2) + ((yy+row)/2)*8];
          b= cbblk[((xx+col)/2) + ((yy+row)/2)*8];
          g= -0.3437*b + -0.7143*r; r= 1.402*r; b= 1.772*b;
          y= *(y_p++);
          r+= y; if ( r < -128 ) r= -128; else if ( r > 127 ) r= 127;
          g+= y; if ( g < -128 ) g= -128; else if ( g > 127 ) g= 127;
          b+= y; if ( b < -128 ) b= -128; else if ( b > 127 ) b= 127;
          if ( !_state.data_out_signed )
            {
              *(p++)= ((uint8_t) ((int8_t) r))^0x80;
              *(p++)= ((uint8_t) ((int8_t) g))^0x80;
              *(p++)= ((uint8_t) ((int8_t) b))^0x80;
            }
          else
            {
              *(p++)= (uint8_t) ((int8_t) r);
              *(p++)= (uint8_t) ((int8_t) g);
              *(p++)= (uint8_t) ((int8_t) b);
            }
        }
      base_p+= 16*3;
    }
  
} // end yuv_to_rgb_24b


static void
yuv_to_rgb_15b (
                const double  crblk[64],
                const double  cbblk[64],
                const double  yblk[64],
                uint16_t     *fb,
                const int     xx,
                const int     yy
                )
{

  static const double factor= 31.0/255.0;
  
  int row,col;
  double r,g,b,y;
  uint16_t *base_p,*p,bit15,val;
  const double *y_p;
  
  
  base_p= fb + xx + yy*16;
  y_p= &(yblk[0]);
  bit15= _state.data_out_bit15_set ? 0x8000 : 0x0000;
  for ( row= 0; row < 8; ++row )
    {
      p= base_p;
      for ( col= 0; col < 8; ++col )
        {
          r= crblk[((xx+col)/2) + ((yy+row)/2)*8];
          b= cbblk[((xx+col)/2) + ((yy+row)/2)*8];
          g= -0.3437*b + -0.7143*r; r= 1.402*r; b= 1.772*b;
          y= *(y_p++);
          r+= y + 128.0; if ( r < 0 ) r= 0; else if ( r > 255 ) r= 255;
          g+= y + 128.0; if ( g < 0 ) g= 0; else if ( g > 255 ) g= 255;
          b+= y + 128.0; if ( b < 0 ) b= 0; else if ( b > 255 ) b= 255;
          val=
            bit15 |
            (((uint16_t) ((b*factor)+0.5))<<10) |
            (((uint16_t) ((g*factor)+0.5))<<5) |
            (((uint16_t) ((r*factor)+0.5))<<0);
          if ( _state.data_out_signed ) val^= 0x4210;
          *(p++)= val;
        }
      base_p+= 16;
    }
  
} // end yuv_to_rgb_15b


static void
yuv_to_mono_8b (
                const double  yblk[64],
                uint8_t      *fb
                )
{

  int i;
  double val;
  

  for ( i= 0; i < 64; ++i )
    {
      val= yblk[i];
      if      ( val < -128 ) val= -128;
      else if ( val > 127 ) val= 127;
      *(fb++)=
        _state.data_out_signed ?
        ((uint8_t) ((int8_t) (val+0.5))) :
        ((uint8_t) (val+128+0.5));
    }
  
} // end yuv_to_mono_8b


static void
yuv_to_mono_4b (
                const double  yblk[64],
                uint8_t      *fb
                )
{

  static const double factor= 15.0/255.0;
  
  int i,flag;
  double val;
  uint8_t aux;
  
  
  flag= 0; aux= 0;
  for ( i= 0; i < 64; ++i )
    {
      val= yblk[i]+128;
      if      ( val < 0 ) val= 0;
      else if ( val > 255 ) val= 255;
      if ( flag ) aux= (((uint8_t) (val*factor+0.5))<<4);
      else
        {
          aux|= (uint8_t) (val*factor+0.5);
          if ( _state.data_out_signed ) aux^= 0x88;
          *(fb++)= aux;
        }
      flag^= 1;
    }
  
} // end yuv_to_mono_4b


#define BEGIN_CR switch ( _state.var.decode.cr_state ) { case 0:
#define RETURN_CR(CODE)                                         \
  do { _state.var.decode.cr_state= __LINE__; return CODE;        \
  case __LINE__:; } while (0)
#define END_CR } _state.var.decode.cr_state= 0; return 0;

static int
run_decode_24b (void)
{

  int ret;

  
  BEGIN_CR;
  
  _state.current_block= CB_CR;
  while ( (ret= rl_decode_block ( _v.crblk, _qt[1] )) < 0 ) RETURN_CR(ret);
  _state.current_block= CB_CB;
  while ( (ret= rl_decode_block ( _v.cbblk, _qt[1] )) < 0 ) RETURN_CR(ret);
  _state.current_block= CB_Y1;
  while ( (ret= rl_decode_block ( _v.yblk, _qt[0] )) < 0 ) RETURN_CR(ret);
  yuv_to_rgb_24b ( _v.crblk, _v.cbblk, _v.yblk, _v.fb, 0, 0 );
  _state.current_block= CB_Y2;
  while ( (ret= rl_decode_block ( _v.yblk, _qt[0] )) < 0 ) RETURN_CR(ret);
  yuv_to_rgb_24b ( _v.crblk, _v.cbblk, _v.yblk, _v.fb, 8, 0 );
  _state.current_block= CB_Y3;
  while ( (ret= rl_decode_block ( _v.yblk, _qt[0] )) < 0 ) RETURN_CR(ret);
  yuv_to_rgb_24b ( _v.crblk, _v.cbblk, _v.yblk, _v.fb, 0, 8 );
  _state.current_block= CB_Y4;
  while ( (ret= rl_decode_block ( _v.yblk, _qt[0] )) < 0 ) RETURN_CR(ret);
  yuv_to_rgb_24b ( _v.crblk, _v.cbblk, _v.yblk, _v.fb, 8, 8 );
  
  _v.fb_N= (16*16*3)/4;
  
  END_CR;
  
} // end run_decode_24b


static int
run_decode_15b (void)
{
  
  int ret;
  
  
  BEGIN_CR;

  _state.current_block= CB_CR;
  while ( (ret= rl_decode_block ( _v.crblk, _qt[1] )) < 0 ) RETURN_CR(ret);
  _state.current_block= CB_CB;
  while ( (ret= rl_decode_block ( _v.cbblk, _qt[1] )) < 0 ) RETURN_CR(ret);
  _state.current_block= CB_Y1;
  while ( (ret= rl_decode_block ( _v.yblk, _qt[0] )) < 0 ) RETURN_CR(ret);
  yuv_to_rgb_15b ( _v.crblk, _v.cbblk, _v.yblk, (uint16_t *) _v.fb, 0, 0 );
  _state.current_block= CB_Y2;
  while ( (ret= rl_decode_block ( _v.yblk, _qt[0] )) < 0 ) RETURN_CR(ret);
  yuv_to_rgb_15b ( _v.crblk, _v.cbblk, _v.yblk, (uint16_t *) _v.fb, 8, 0 );
  _state.current_block= CB_Y3;
  while ( (ret= rl_decode_block ( _v.yblk, _qt[0] )) < 0 ) RETURN_CR(ret);
  yuv_to_rgb_15b ( _v.crblk, _v.cbblk, _v.yblk, (uint16_t *) _v.fb, 0, 8 );
  _state.current_block= CB_Y4;
  while ( (ret= rl_decode_block ( _v.yblk, _qt[0] )) < 0 ) RETURN_CR(ret);
  yuv_to_rgb_15b ( _v.crblk, _v.cbblk, _v.yblk, (uint16_t *) _v.fb, 8, 8 );

  _v.fb_N= (16*16*2)/4;
  
  END_CR;
  
} // end run_decode_15b


static int
run_decode_8b (void)
{

  int ret;

  
  BEGIN_CR;

  _state.current_block= CB_Y_MONO;
  while ( (ret= rl_decode_block ( _v.yblk, _qt[0] )) < 0 ) RETURN_CR(ret);
  yuv_to_mono_8b ( _v.yblk, _v.fb );

  _v.fb_N= (8*8)/4;
  
  END_CR;
  
} // end run_decode_8b


static int
run_decode_4b (void)
{
  
  int ret;

  
  BEGIN_CR;

  _state.current_block= CB_Y_MONO;
  while ( (ret= rl_decode_block ( _v.yblk, _qt[0] )) < 0 ) RETURN_CR(ret);
  yuv_to_mono_4b ( _v.yblk, _v.fb );

  _v.fb_N= (8*8)/(2*4);
  
  END_CR;
  
} // end run_decode_4b


static void
write_macroblock (void)
{

  int n,pos;

  
  // Prepara dades
#ifdef PSX_BE
  // Swap abans d'escriure.
  swap_bytes ( (uint32_t *) _v.fb, _v.fb_N );
#endif

  // Escriu dades
  if ( _v.fb_N+_fifo_out.N > FIFO_SIZE )
    {
      _warning ( _udata,
                 "MDEC::write_macroblock: la FIFO d'exida està plena, es"
                 " van a descartar %d paraules",
                 FIFO_SIZE-(_v.fb_N+_fifo_out.N) );
      _v.fb_N= FIFO_SIZE-_fifo_out.N; // Reajusta
    }
  pos= _fifo_out.p + _fifo_out.N;
  for ( n= 0; n < _v.fb_N; ++n )
    _fifo_out.v[(pos+n)&(FIFO_SIZE-1)]= ((const uint32_t *) _v.fb)[n];
  _fifo_out.N+= _v.fb_N;

  // Comprovacions de final de comandament.
  if ( _state.remaining_words == 0xFFFF )
    _state.cmd= NONE;

  // Desperta DMA.
  if ( _dma.out_waiting && _dma.out_waiting_nwords <= _fifo_out.N )
    {
      _dma.out_waiting= false;
      PSX_dma_active_channel ( 1 );
    }
  
} // end write_macroblock


static void
run_decode (
            const uint32_t data
            )
{

  int ret;


  // Inserta dades en el bin.
  assert ( _bin.N <= 2 );
  _bin.v[(_bin.p+_bin.N)&(BIN_SIZE-1)]= (uint16_t) (data&0xFFFF);
  _bin.v[(_bin.p+_bin.N+1)&(BIN_SIZE-1)]= (uint16_t) ((data>>16)&0xFFFF);
  _bin.N+= 2;
  --_state.remaining_words; // Paraula processada.
  
  // Descodifica macroblocks.
  do {

    // Descodica següent macrobloc.
    switch ( _state.data_out_depth )
      {
      case 0: ret= run_decode_4b (); break;
      case 1: ret= run_decode_8b (); break;
      case 2: ret= run_decode_24b (); break;
      case 3: ret= run_decode_15b (); break;
      default: printf("MDEC::run_decode - WTF!!!\n");
      }

    // Si hem descodificat un macrobloc.
    if ( ret == 0 )
      {
        // Es pot escriure immediatament.
        if ( _timing.cc_current_macroblock >= CCMACROBLOCK )
          {
            // Cicles per al següent bloc si és que hi ha.
            _timing.cc_current_macroblock-= CCMACROBLOCK;
            write_macroblock ();
          }
        // Espera!!!!!
        else
          {
            _timing.cctoWriteMacroblock=
              CCMACROBLOCK-_timing.cc_current_macroblock;
            _state.waiting_write_macroblock= true;
            update_timing_event ();
          }
      }
      
  } while ( ret == 0 &&
            !_state.waiting_write_macroblock &&
            _state.cmd == DECODE );

  // Com teòricament pot ser que s'acave el comandament sense tindre
  // un macroblock sencer, ací es fa una comprovació per a decidir si
  // acavem o no comandament.
  if ( !_state.waiting_write_macroblock &&
       _state.remaining_words == 0xFFFF )
    _state.cmd= NONE;
  
} // end run_decode


static void
new_command (
             const uint32_t data
             )
{

  // Bits 23-26. (Bit25-28 are copied to STAT.23-26). Necessari sempre.
  _state.data_out_depth= (data>>27)&0x3;
  _state.data_out_signed= (data&0x04000000)!=0;
  _state.data_out_bit15_set= (data&0x02000000)!=0;
  
  // Estat comandament.
  switch ( data>>29 )
    {
    case 1: // Decode Macroblock(s)
      _state.cmd= DECODE;
      _state.var.decode.cr_state= 0;
      _state.var.decode.rldb_state= 0;
      _state.var.decode.fast_idct= (_st.diff==0);
      _state.remaining_words= (data&0xFFFF)-1;
      _bin.p= 0; _bin.N= 0;
      _timing.cc_current_macroblock= 0;
      break;
    case 2: // Set Quant Table(s)
      _state.cmd= SET_QT;
      _state.var.set_qt.pos= 0;
      _state.var.set_qt.N= (data&0x1) ? 128 : 64;
      _state.remaining_words= ((uint16_t) (_state.var.set_qt.N>>2))-1;
      break;
    case 3: // Set Scale Table
      _state.cmd= SET_ST;
      _state.var.set_st.pos= 0;
      _state.var.set_st.mask= 0x1;
      _state.remaining_words= ((uint16_t) (64>>1))-1;
      break;
    default:
      _warning ( _udata,
        	 "MDEC::new_command:"
                 " commandament desconegut: %X", data>>29 );
      _state.cmd= NONE;
      _state.remaining_words= data&0xFFFF;
    }
  
} // end new_command


static void
write_qt (
          const uint32_t word
          )
{

  // Escriu.
  _qt[0][_state.var.set_qt.pos++]= (uint8_t) (word&0xFF);
  _qt[0][_state.var.set_qt.pos++]= (uint8_t) ((word>>8)&0xFF);
  _qt[0][_state.var.set_qt.pos++]= (uint8_t) ((word>>16)&0xFF);
  _qt[0][_state.var.set_qt.pos++]= (uint8_t) ((word>>24)&0xFF);
  
  // Comprova finalització.
  --_state.remaining_words;
  if ( _state.remaining_words == 0xFFFF )
    {
      assert ( _state.var.set_qt.pos == _state.var.set_qt.N );
      _state.cmd= NONE;
    }
  
} // end write_qt


static void
write_st (
          const uint32_t word
          )
{

  int16_t val;

  
  // Escriu primer valor.
  val= (int16_t) (word&0xFFFF);
  if ( val == DEFAULT_ST[_state.var.set_st.pos] )
    _st.diff&= ~_state.var.set_st.mask;
  else
    _st.diff|= _state.var.set_st.mask;
  // ¿¿14 o 13??? bit fractional part 2**14, i el /8 que adelante.
  _st.v[_state.var.set_st.pos++]= val/(/*16384.0*/8192.0*8);
  _state.var.set_st.mask<<= 1;

  // Escriu segon valor.
  val= (int16_t) ((word>>16)&0xFFFF);
  if ( val == DEFAULT_ST[_state.var.set_st.pos] )
    _st.diff&= ~_state.var.set_st.mask;
  else
    _st.diff|= _state.var.set_st.mask;
  // ¿¿14 o 13??? bit fractional part 2**14, i el /8 que adelante.
  _st.v[_state.var.set_st.pos++]= val/(/*16384.0*/8192.0*8);
  _state.var.set_st.mask<<= 1;

  // Comprova finalització.
  --_state.remaining_words;
  if ( _state.remaining_words == 0xFFFF )
    {
      assert ( _state.var.set_st.pos == 64 );
      _state.cmd= NONE;
    }
  
} // end write_st


static void
process_fifo_in (void)
{

  uint32_t word;

  
  while ( _fifo_in.N && !_state.waiting_write_macroblock )
    {

      // Llig següent paraula
      word= _fifo_in.v[_fifo_in.p];
      _fifo_in.p= (_fifo_in.p+1)&(FIFO_SIZE-1);
      --_fifo_in.N;

      // Command.
      switch ( _state.cmd )
        {
        case NONE:
          new_command ( word );
          break;
        case DECODE:
          run_decode ( word );
          break;
        case SET_QT:
          write_qt ( word );
          break;
        case SET_ST:
          write_st ( word );
          break;
        }
      
    }
  
} // end process_fifo_in


static void
reset_state (void)
{
  
  _state.cmd= NONE;
  _state.data_out_depth= 0;
  _state.data_out_signed= false;
  _state.data_out_bit15_set= false;
  _state.remaining_words= 0xFFFF;
  _state.current_block= 0;
  _state.waiting_write_macroblock= false;
  fifo_clear ( &_fifo_in );
  fifo_clear ( &_fifo_out );
  update_timing_event ();
  //_dma.in_enabled= false; <-- Ho faig en control
  //_dma.out_enabled= false;
  
} // end reset_state


static void
clock (void)
{

  int cc,CC;


  cc= PSX_Clock-_timing.cc_used;
  if ( cc > 0 ) { _timing.cc+= cc; _timing.cc_used+= cc;}
  if ( _timing.cc == 0 ) return;
  
  CC= _timing.cc;
  _timing.cc= 0;
  
  if ( _state.waiting_write_macroblock )
    {
      _timing.cctoWriteMacroblock-= CC;
      if ( _timing.cctoWriteMacroblock <= 0 )
        {
          // Cicles restants per al següent macroblock, si és que hi ha.
          _timing.cc_current_macroblock= -_timing.cctoWriteMacroblock;
          _timing.cctoWriteMacroblock= 0;
          _state.waiting_write_macroblock= false;
          write_macroblock ();
          // Pot ser que hi hasquen coses pendents de processar en el
          // decode.
          process_fifo_in ();
        }
    }
  else if ( _state.cmd == DECODE )
    _timing.cc_current_macroblock+= CC;
  
  update_timing_event ();
  
} // end clock




/**********************/
/* FUNCIONS PÚBLIQUES */
/**********************/

void
PSX_mdec_init (
               PSX_Warning *warning,
               void        *udata
               )
{

  // Callbacks.
  _warning= warning;
  _udata= udata;
  
  // Taules.
  init_scalezag ();
  init_zagzig ();
  memset ( _qt, 0, sizeof(_qt) );
  init_st ();
  
  // Fifos
  init_fifo ( &_fifo_in );
  init_fifo ( &_fifo_out );

  // Timing.
  _timing.cc= 0;
  _timing.cc_used= 0;
  _timing.cc_current_macroblock= 0;
  _timing.cctoWriteMacroblock= 0;
  _timing.cctoEvent= 0;

  // DMA.
  _dma.in_enabled= false;
  _dma.out_enabled= false;
  _dma.out_waiting= false;
  
  // State.
  reset_state ();
  
} // end PSX_mdec_init


void
PSX_mdec_end_iter(void)
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
  
} // end PSX_mdec_end_iter


int
PSX_mdec_next_event_cc (void)
{
  
  int ret;
  
  
  ret= _timing.cctoEvent - _timing.cc;
  assert ( ret >= 0 );
  
  return ret;
  
} // end PSX_mdec_next_event_cc


uint32_t
PSX_mdec_data_read (void)
{

  uint32_t ret;

  
  clock ();

  if ( _fifo_out.N == 0 )
    {
      _warning ( _udata,
        	 "PSX_mdec_data_read: no es poden llegir més"
                 " dades perquè la FIFO d'eixida està buida" );
      return 0;
    }

  ret= _fifo_out.v[_fifo_out.p++];
  _fifo_out.p&= (FIFO_SIZE-1);
  --_fifo_out.N;

  return ret;
  
} // end PSX_mdec_data_read


void
PSX_mdec_data_write (
        	     const uint32_t data
        	     )
{

  int pos;

  
  clock ();

  if ( _fifo_in.N == FIFO_SIZE )
    {
      _warning ( _udata,
        	 "PSX_mdec_data_write: no es poden escriure més"
                 " dades perquè la FIFO està plena" );
      return;
    }

  pos= (_fifo_in.p + _fifo_in.N)&(FIFO_SIZE-1);
  _fifo_in.v[pos]= data;
  ++_fifo_in.N;

  process_fifo_in ();
  
} // end PSX_mdec_data_write


uint32_t
PSX_mdec_status (void)
{

  uint32_t ret;


  clock ();
  
  /*
   * N1: Nocash diu 'Full, or Last word received', en realitat end es
   *     fica a true quan s'han intentat llegir dades de bin i estava
   *     buit.
   * N2: Sempre està disponible.
   * N3: Ignore de moment el comportament extrany de data out request.
   */
  ret=
    ((_fifo_out.N==0)<<31) |
    ((_fifo_in.N==FIFO_SIZE || _state.remaining_words==0xFFFF)<<30) | // N1
    ((_state.cmd!=NONE)<<29) | // N2.
    (_dma.in_enabled<<28) |
    (_dma.out_enabled<<27) | // N3.
    (_state.data_out_depth<<25) |
    (_state.data_out_signed<<24) |
    (_state.data_out_bit15_set<<23) |
    (_state.current_block<<16) |
    ((uint32_t) _state.remaining_words);
  
  return ret;
  
} // end PSX_mdec_status


void
PSX_mdec_control (
        	  const uint32_t data
        	  )
{


  // NOTA!! Per si de cas sempre que canvia l'estat d'un canal DMA
  // elmine transferències pendents.
  
  bool aux;
  

  clock ();
  
  if ( data&0x80000000 ) reset_state ();
  aux= (data&0x40000000)!=0;
  _dma.in_enabled= aux;
  aux= (data&0x20000000)!=0;
  if ( aux != _dma.out_enabled ) _dma.out_waiting= false;
  _dma.out_enabled= aux;
  
} // end PSX_mdec_control


bool
PSX_mdec_in_sync (
        	  const uint32_t nwords_m1
        	  )
{

  clock ();
  
  // NOTA: Si està deshabilitat o no s'espera res accepte la petició
  // inmediatament i deixe que siguen ignorades.
  if ( !_dma.in_enabled )
    _warning ( _udata,
               "MDECIN (DMA0) sync: el canal està desactivat i totes les"
               " peticions de transferència seran ignorades" );
  
  PSX_dma_active_channel ( 0 );

  
  return true;
  
} // end PSX_mdec_in_sync


void
PSX_mdec_in_write (
        	   uint32_t data
        	   )
{

  int pos;

  
  clock ();

  if ( !_dma.in_enabled )
    {
      _warning ( _udata, "MDECIN (DMA0) write: el canal està desactivat" );
      return;
    }
  
  if ( _fifo_in.N == FIFO_SIZE )
    {
      _warning ( _udata,
        	 "PSX_mdec_in_write: no es poden escriure més"
                 " dades perquè la FIFO està plena" );
      return;
    }

  pos= (_fifo_in.p + _fifo_in.N)&(FIFO_SIZE-1);
  _fifo_in.v[pos]= data;
  ++_fifo_in.N;
  
  process_fifo_in ();
  
} // end PSX_mdec_in_write


uint32_t
PSX_mdec_in_read (void)
{
  
  _warning ( _udata, "MDECIN (DMA0) read: el canal és sols d'escriptura" );
  
  return 0xFF00FF00;
  
} // end PSX_mdec_in_read


bool
PSX_mdec_out_sync (
        	   const uint32_t nwords_m1
        	   )
{

  bool ret;
  

  clock ();
  
  // NOTA: Si està deshabilitat o no s'espera res accepte la petició
  // inmediatament i deixe que siguen ignorades.
  
  if ( !_dma.out_enabled )
    {
      _warning ( _udata,
                 "MDECOUT (DMA1) sync: el canal està desactivat i totes les"
                 " peticions de transferència seran ignorades" );
      ret= true;
    }
  
  else if ( _dma.out_waiting )
    {
      _warning ( _udata,
                 "MDECOUT (DMA1) sync: s'ha produït un anidament"
                 " de syncs inesperat" );
      ret= false; // Penja la transferència!!!
    }

  // Petició pendent de que es plene el buffer
  else if ( nwords_m1 > (uint32_t) _fifo_out.N )
    {
      _dma.out_waiting= true;
      _dma.out_waiting_nwords= nwords_m1;
      ret= false;
    }

  // Petició acceptada.
  else ret= true;
  
  if ( ret ) PSX_dma_active_channel ( 1 );
  
  return ret;
  
} // end PSX_mdec_out_sync


void
PSX_mdec_out_write (
        	    uint32_t data
        	    )
{
  _warning ( _udata, "MDECOUT (DMA1) write: el canal és sols de lectura" );
} // end PSX_mdec_out_write


uint32_t
PSX_mdec_out_read (void)
{
  
  uint32_t ret;

  
  clock ();

  if ( !_dma.out_enabled )
    {
      _warning ( _udata, "MDECOUT (DMA1) read: el canal està desactivat" );
      return 0xFF00FF00;
    }
  
  if ( _fifo_out.N == 0 )
    {
      _warning ( _udata,
        	 "PSX_mdec_out_read: no es poden llegir més"
                 " dades perquè la FIFO d'eixida està buida" );
      return 0;
    }

  ret= _fifo_out.v[_fifo_out.p++];
  _fifo_out.p&= (FIFO_SIZE-1);
  --_fifo_out.N;
  
  return ret;
  
} // end PSX_mdec_out_read


void
PSX_mdec_reset (void)
{

  // Fifos
  init_fifo ( &_fifo_in );
  init_fifo ( &_fifo_out );

  // Timing.
  _timing.cc_current_macroblock= 0;
  _timing.cctoWriteMacroblock= 0;
  _timing.cctoEvent= 0;
  
  // DMA.
  _dma.in_enabled= false;
  _dma.out_enabled= false;
  _dma.out_waiting= false;
  
  // State.
  reset_state ();

  update_timing_event ();
  
} // end PSX_mdec_reset

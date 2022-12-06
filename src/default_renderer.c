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
 *  default_renderer.c - Implementació de PSX_Renderer en C.
 *
 */


#include <assert.h>
#include <math.h>
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

#define DR(ptr) ((default_renderer_t *) (ptr))

#define MAXSLNODES 4

#define NLINES 512
#define NCOLS 1024

#define MAXWIDTH 640
#define MAXHEIGHT 480

#define SWITCH_VERTEX(TMP,A,B)        					\
  do {        								\
    (TMP)= (A); (A)= (B); (B)= (TMP);        				\
  } while(0)

#define TORGB15b(R,G,B)        			\
  (((uint16_t) ((R)>>3)) |        		\
   (((uint16_t) ((G)>>3))<<5) |        		\
   (((uint16_t) ((B)>>3))<<10))

// NOTA!! Açò del round_double pretenia truncar els resultats de
// l'ecuació lineal per fer que resultats que són distints quan es
// canvien d'ordre els operants per qüestió de precissió foren igual,
// pero probablement mareja més que ajuda, així que ho he desactivat.
//#define ROUND_DOUBLE(VAL) (((int64_t) ((VAL)*100000)) / (double) 100000.0)
#define ROUND_DOUBLE(VAL) (VAL)

//#define TOINT(VAL) ((int) ((VAL) + 0.5))

// Aparentment el mapeig que fa de reals a sencers és el següent:
//
//  0    -> 0
// ]0,1] -> 1
// ]1,2] -> 2
//  ...
//
// La manera de aproximar açò és molt cutre, però pareix que
// funciona. Molt de compte que la coma real juga males
// passades. Idealment estaria bé moure a coma fixa amb sencers.
#define TOINT(VAL) ((int) ((VAL) + 0.999))

//#define TOINT(VAL) (((int) (VAL)) + (((VAL)!=((int) (VAL))) ? 1 : 0))





/*********/
/* TIPUS */
/*********/

typedef struct
{

  int    y_max;
  int    y_min;
  double x;
  double slope;
  
} edge_t;

typedef struct
{

  int r0,r1; // Primera i última fila.
  struct
  {
    bool enabled;
    struct
    {
      int  c; // Columna
    } v[2]; // Valors 0 -> esquerra 1 -> dreta 
  } p[NLINES]; // Valors per a cada píxel
  
} pol_state_t;
  
typedef struct
{
  
  PSX_RENDERER_CLASS;
  uint16_t            *fb;
  uint8_t              out_fb[MAXWIDTH*MAXHEIGHT*4];
  void                *udata;
  PSX_UpdateScreen    *update_screen;
  bool                 display_enabled;
  pol_state_t          pol;
  
} default_renderer_t;

// Per a dibuixar la textura d'un polígon
typedef struct
{
  
  bool            tex_enabled;
  bool            gouraud_enabled;
  bool            raw_texture;
  double          a,b,c; // Per a u.
  double          d,e,f; // Per a v.
  double          r_a,r_b,r_c; // Per a r.
  double          g_a,g_b,g_c; // Per a g.
  double          b_a,b_b,b_c; // Per a b.
  const uint16_t *clut;
  const uint16_t *page;
  
} pol_tex_t;





/*************/
/* CONSTANTS */
/*************/

static const int DITHERING[4][4]=
  {
    {-4,+0,-3,-1},
    {+2,-2,+3,-1},
    {-3,+1,-4,+0},
    {+3,-1,+2,-2}
  };




/*********************/
/* FUNCIONS PRIVADES */
/*********************/

/* Memòria ********************************************************************/
static void *
mem_alloc_ (
            size_t nbytes
            )
{

  void *new;
  
  
  new= malloc ( nbytes );
  if ( new == NULL )
    {
      fprintf ( stderr, "[EE] [PSX] cannot allocate memory\n" );
      exit ( EXIT_FAILURE );
    }
  
  return new;
  
} /* end mem_alloc_ */


#define mem_alloc(type,size)                            \
  ((type *) mem_alloc_ ( sizeof(type) * (size) ))


static uint16_t
apply_dithering (
        	 const int offset,
        	 uint8_t   r,
        	 uint8_t   g,
        	 uint8_t   b
        	 )
{

  int tmp;
  
  
  tmp= ((int) r) + offset;
  if ( tmp < 0 )        r= 0x00;
  else if ( tmp > 255 ) r= 0xff;
  else                  r= (uint8_t) tmp;
  tmp= ((int) g) + offset;
  if ( tmp < 0 )        g= 0x00;
  else if ( tmp > 255 ) g= 0xff;
  else                  g= (uint8_t) tmp;
  tmp= ((int) b) + offset;
  if ( tmp < 0 )        b= 0x00;
  else if ( tmp > 255 ) b= 0xff;
  else                  b= (uint8_t) tmp;
  
  
  return TORGB15b ( r, g, b );
  
} /* end apply_dithering */


static uint16_t
apply_color_blending (
        	      const int mode,
        	      const int old,
        	      const int new
        	      )
{

  int r,g,b;

  
  switch ( mode )
    {
    case PSX_TR_MODE0: // D/2 + S/2
      r= (((int) (old&0x001F)) + ((int) (new&0x001F)))>>1;
      if ( r > 0x1F ) r= 0x1F;
      g= ((((int) (old&0x03E0)) + ((int) (new&0x03E0)))>>1)&(~0x1F);
      if ( g > 0x3E0 ) g= 0x3E0;
      b= ((((int) (old&0x7C00)) + ((int) (new&0x7C00)))>>1)&(~0x3FF);
      if ( b > 0x7C00 ) b= 0x7C00;
      return (uint16_t)
        ((new&0x8000) |
         ((int16_t) r) |
         ((int16_t) g) |
         ((int16_t) b));
      
    case PSX_TR_MODE1: // D + S
      r= ((int) (old&0x001F)) + ((int) (new&0x001F));
      if ( r > 0x1F ) r= 0x1F;
      g= ((int) (old&0x03E0)) + ((int) (new&0x03E0));
      if ( g > 0x3E0 ) g= 0x3E0;
      b= ((int) (old&0x7C00)) + ((int) (new&0x7C00));
      if ( b > 0x7C00 ) b= 0x7C00;
      return (uint16_t)
        ((new&0x8000) |
         ((int16_t) r) |
         ((int16_t) g) |
         ((int16_t) b));
      
    case PSX_TR_MODE2: // D - S
      r= ((int) (old&0x001F)) - ((int) (new&0x001F));
      if ( r < 0 ) r= 0;
      g= ((int) (old&0x03E0)) - ((int) (new&0x03E0));
      if ( g < 0 ) g= 0;
      b= ((int) (old&0x7C00)) - ((int) (new&0x7C00));
      if ( b < 0 ) b= 0;
      return (uint16_t)
        ((new&0x8000) |
         ((int16_t) r) |
         ((int16_t) g) |
         ((int16_t) b));
      
    case PSX_TR_MODE3: // D + S/4
      r= ((((int) (old&0x001F))<<2) + ((int) (new&0x001F)))>>2;
      if ( r > 0x1F ) r= 0x1F;
      g= (((((int) (old&0x03E0))<<2) + ((int) (new&0x03E0)))>>2)&(~0x1F);
      if ( g > 0x3E0 ) g= 0x3E0;
      b= (((((int) (old&0x7C00))<<2) + ((int) (new&0x7C00)))>>2)&(~0x3FF);
      if ( b > 0x7C00 ) b= 0x7C00;
      return (uint16_t)
        ((new&0x8000) |
         ((int16_t) r) |
         ((int16_t) g) |
         ((int16_t) b));
      
    default: return new;
      
    }
  
} // end apply_color_blending


static uint16_t
read_tex_color (
        	const int       u,
        	const int       v,
        	const int       mode,
        	const uint16_t *page,
        	const uint16_t *clut
        	)
{

  uint16_t ind,color;

  
  switch ( mode )
    {
    case PSX_TEX_4b:
      ind= page[v*1024 + (u>>2)];
      color= clut[(ind>>(4*(u&0x3)))&0xF];
      break;
    case PSX_TEX_8b:
      ind= page[v*1024 + (u>>1)]; // ACÍ !!!! Use of uninitialised value of size 8
      color= clut[u&0x1 ? ind>>8 : ind&0xFF];
      break;
    case PSX_TEX_15b:
    default:
      color= page[v*1024 + u];
      break;
    }

  return color;
  
} /* end read_tex_color */


static uint16_t
tex_get_color (
               const pol_tex_t        *tex,
               double                 *uf,
               double                 *vf,
               const PSX_RendererArgs *a
               )
{

  int u,v;
  uint16_t color;
  

  // NOTA!!! Aquest redondeig no és baladí. Bàsicament, en l'espai
  // real el 0 és el centre del píxel 0, l'1 és el centre del píxel 1,
  // etc. Això vol dir que [-0.5,0.5[ -> píxel 0, [0.5,1.5[ -> píxel
  // 1, etc.
  u= (int) (*uf+0.5); u= ((u&a->texwinmask_x) | a->texwinoff_x);
  v= (int) (*vf+0.5); v= ((v&a->texwinmask_y) | a->texwinoff_y);
  color= read_tex_color ( u, v, a->texture_mode, tex->page, tex->clut );
  (*uf)+= tex->a;
  (*vf)+= tex->d;

  return color;
  
} // end tex_get_color


static void
modulate_color (
        	uint8_t        *r,
        	uint8_t        *g,
        	uint8_t        *b,
        	const uint16_t  color
        	)
{

  int lr,lb,lg;

  lr= (((int) *r) * ((int) ((color&0x1F)<<3)))>>7;
  if ( lr > 255 ) lr= 255;
  lg= (((int) *g) * ((int) (((color>>5)&0x1F)<<3)))>>7;
  if ( lg > 255 ) lg= 255;
  lb= (((int) *b) * ((int) (((color>>10)&0x1F)<<3)))>>7;
  if ( lb > 255 ) lb= 255;
  *r= (uint8_t) lr;
  *g= (uint8_t) lg;
  *b= (uint8_t) lb;

} // end modulate_color


static void
pol_tex_gouraud_init (
                     pol_tex_t              *tex,
                     const uint16_t         *fb,
                     const PSX_VertexInfo   *v0,
                     const PSX_VertexInfo   *v1,
                     const PSX_VertexInfo   *v2,
                     const PSX_RendererArgs *a
                     )
{

  double vals[8*3],tmp;
  double *m[3];
  double *swt;
  bool tex_enabled;


  assert ( fb != NULL );
  tex_enabled= (a->texture_mode!=PSX_TEX_NONE);
  tex->gouraud_enabled= false;
  tex->tex_enabled= false;
  tex->raw_texture= false;
  if ( !tex_enabled && !a->gouraud ) return;
  
  /* Transformació afí.
   *
   * u= a*x + b*y + c
   * v= d*x + e*y + f
   */

  // Plena matriu
  m[0]= &vals[0]; m[1]= &vals[8]; m[2]= &vals[16];
  m[0][0]= (double) v0->x; m[0][1]= (double) v0->y; m[0][2]= 1.0;
  m[1][0]= (double) v1->x; m[1][1]= (double) v1->y; m[1][2]= 1.0;
  m[2][0]= (double) v2->x; m[2][1]= (double) v2->y; m[2][2]= 1.0;
  if ( tex_enabled )
    {
      m[0][3]= (double) v0->u; m[0][4]= (double) v0->v;
      m[1][3]= (double) v1->u; m[1][4]= (double) v1->v;
      m[2][3]= (double) v2->u; m[2][4]= (double) v2->v;
    }
  if ( a->gouraud )
    {
      m[0][5]= (double) v0->r; m[0][6]= (double) v0->g; m[0][7]= (double) v0->b;
      m[1][5]= (double) v1->r; m[1][6]= (double) v1->g; m[1][7]= (double) v1->b;
      m[2][5]= (double) v2->r; m[2][6]= (double) v2->g; m[2][7]= (double) v2->b;
    }

  // Primer pivot
  // --> Reordena si cal
  if ( m[0][0] == 0.0 )
    {
      if ( m[1][0] != 0.0 ) { swt= m[1]; m[1]= m[0]; }
      else if ( m[2][0] != 0.0 ) { swt= m[2]; m[2]= m[0]; }
      else return; // No es pot!!!
      m[0]= swt;
    }
  // --> Normalitza
  tmp= m[0][0]; m[0][1]/= tmp; m[0][2]/= tmp;
  if ( tex_enabled ) { m[0][3]/= tmp; m[0][4]/= tmp; }
  if ( a->gouraud ) { m[0][5]/= tmp; m[0][6]/= tmp; m[0][7]/= tmp; }
  // --> Elimina pivot (no cal en el primer)
  if ( (tmp= m[1][0]) != 0.0 )
    {
      m[1][1]-= tmp*m[0][1]; m[1][2]-= tmp*m[0][2];
      if ( tex_enabled ) { m[1][3]-= tmp*m[0][3]; m[1][4]-= tmp*m[0][4]; }
      if ( a->gouraud )
        {
          m[1][5]-= tmp*m[0][5]; m[1][6]-= tmp*m[0][6]; m[1][7]-= tmp*m[0][7];
        }
    }
  if ( (tmp= m[2][0]) != 0.0 )
    {
      m[2][1]-= tmp*m[0][1]; m[2][2]-= tmp*m[0][2];
      if ( tex_enabled ) { m[2][3]-= tmp*m[0][3]; m[2][4]-= tmp*m[0][4]; }
      if ( a->gouraud )
        {
          m[2][5]-= tmp*m[0][5]; m[2][6]-= tmp*m[0][6]; m[2][7]-= tmp*m[0][7];
        }
    }
  
  // Segon pivot
  // --> Reordena si cal
  if ( m[1][1] == 0.0 )
    {
      if ( m[2][1] != 0.0 ) { swt= m[2]; m[2]= m[1]; }
      else return; // No es pot!!!
      m[1]= swt;
    }
  // --> Normalitza
  tmp= m[1][1]; m[1][2]/= tmp;
  if ( tex_enabled ) { m[1][3]/= tmp; m[1][4]/= tmp; }
  if ( a->gouraud ) { m[1][5]/= tmp; m[1][6]/= tmp; m[1][7]/= tmp; }
  // --> Elimina pivot (no cal en el primer)
  if ( (tmp= m[2][1]) != 0.0 )
    {
      m[2][2]-= tmp*m[1][2];
      if ( tex_enabled ) { m[2][3]-= tmp*m[1][3]; m[2][4]-= tmp*m[1][4]; }
      if ( a->gouraud )
        {
          m[2][5]-= tmp*m[1][5]; m[2][6]-= tmp*m[1][6]; m[2][7]-= tmp*m[1][7];
        }
    }
  
  // Tercer pivot
  if ( m[2][2] == 0.0 ) return; // No es pot!!!
  tmp= m[2][2];
  if ( tex_enabled ) { m[2][3]/= tmp; m[2][4]/= tmp; }
  if ( a->gouraud ) { m[2][5]/= tmp; m[2][6]/= tmp; m[2][7]/= tmp; }

  // Calcula coeficients
  if ( tex_enabled )
    {
      tex->c= ROUND_DOUBLE(m[2][3]);
      tex->b= ROUND_DOUBLE(m[1][3] - tex->c*m[1][2]);
      tex->a= ROUND_DOUBLE(m[0][3] - tex->c*m[0][2] - tex->b*m[0][1]);
      tex->f= ROUND_DOUBLE(m[2][4]);
      tex->e= ROUND_DOUBLE(m[1][4] - tex->f*m[1][2]);
      tex->d= ROUND_DOUBLE(m[0][4] - tex->f*m[0][2] - tex->e*m[0][1]);

      // clut i page.
      tex->clut= &(fb[a->texclut_y*1024 + a->texclut_x*16]);
      tex->page= &(fb[a->texpage_y*256*1024 + a->texpage_x*64]);
    }
  if ( a->gouraud )
    {
      tex->r_c= ROUND_DOUBLE(m[2][5]);
      tex->r_b= ROUND_DOUBLE(m[1][5] - tex->r_c*m[1][2]);
      tex->r_a= ROUND_DOUBLE(m[0][5] - tex->r_c*m[0][2] - tex->r_b*m[0][1]);
      tex->g_c= ROUND_DOUBLE(m[2][6]);
      tex->g_b= ROUND_DOUBLE(m[1][6] - tex->g_c*m[1][2]);
      tex->g_a= ROUND_DOUBLE(m[0][6] - tex->g_c*m[0][2] - tex->g_b*m[0][1]);
      tex->b_c= ROUND_DOUBLE(m[2][7]);
      tex->b_b= ROUND_DOUBLE(m[1][7] - tex->b_c*m[1][2]);
      tex->b_a= ROUND_DOUBLE(m[0][7] - tex->b_c*m[0][2] - tex->b_b*m[0][1]);
      /*
      tex->r_c= (double) v0->r;
      tex->r_b= 0;
      tex->r_a= 0;
      tex->g_c= (double) v0->g;
      tex->g_b= 0;
      tex->g_a= 0;
      tex->b_c= (double) v0->b;
      tex->b_b= 0;
      tex->b_a= 0;
      */
    }
  tex->gouraud_enabled= a->gouraud;
  tex->tex_enabled= tex_enabled;
  tex->raw_texture=
    (tex_enabled && (a->transparency==PSX_TR_NONE) && !a->modulate_texture);
  
} // end pol_tex_gouraud_init


static void
add_edge (
          const PSX_VertexInfo *a,
          const PSX_VertexInfo *b,
          edge_t               *get,
          int                  *N
          )
{

  double slope;
  int n,n2;
  const PSX_VertexInfo *tmp;
    

  // Ordena els vertex de manera que a siga el primer. 
  if ( a->y == b->y ) return;
  else if ( b->y < a->y ) { tmp= b; b= a; a= tmp; }

  slope= (b->x-a->x) / (double) (b->y-a->y);
  for ( n= 0;
        n < *N &&
          (get[n].y_min < a->y ||
           (get[n].y_min == a->y &&
            (get[n].x < a->x))) 
          ;
        ++n );
  for ( n2= *N; n2 > n; --n2 )
    get[n2]= get[n2-1];
  get[n].y_min= a->y;
  get[n].y_max= b->y;
  get[n].x= (double) a->x;
  get[n].slope= slope;
  ++(*N);
  
} // end add_edge


// Torna el nou p_aet
static int
remove_edges_aet (
                  int        p_aet,
                  const int  p_get,
                  edge_t    *edges
                  )
{


  int p,q;


  for ( p= p_aet; p != p_get; ++p )
    if ( edges[p].y_min >= edges[p].y_max )
      {
        for ( q= p; q > p_aet; --q )
          edges[q]= edges[q-1];
        ++p_aet;
      }
  
  return p_aet;
  
} // end remove_edges_aet


static void
swap_edges (
            const int  a,
            const int  b,
            edge_t    *edges
            )
{

  edge_t tmp;


  tmp= edges[a];
  edges[a]= edges[b];
  edges[b]= tmp;
  
} // end swap_edges


static void
sort_edges_aet (
                const int  p_aet,
                const int  p_get,
                edge_t    *edges
                )
{

  int N;

  

  N= p_get-p_aet;
  if ( N == 1 ) return;
  else if ( N == 2 )
    {
      if ( edges[p_aet].x > edges[p_aet+1].x )
        swap_edges ( p_aet, p_aet+1, edges );
    }
  else
    {
      assert ( N == 3 );
      if ( edges[p_aet].x > edges[p_aet+1].x )
        swap_edges ( p_aet, p_aet+1, edges );
      if ( edges[p_aet+1].x > edges[p_aet+2].x )
        {
          swap_edges ( p_aet+1, p_aet+2, edges );
          if ( edges[p_aet].x > edges[p_aet+1].x )
            swap_edges ( p_aet, p_aet+1, edges );
        }
    }
  
} // end sort_edges_aet


static void
draw_triangle_fill_line (
                         default_renderer_t     *renderer,
                         const PSX_RendererArgs *a,
                         const int               row,
                         const int               col0,
                         const int               col1,
                         const pol_tex_t        *tex,
                         PSX_RendererStats      *stats
                         )
{
  
  int c0,c1,c;
  uint16_t icolor,color;
  const int *d_row;
  uint16_t *line;
  double uf,vf,rf,gf,bf;
  uint8_t r,g,b;

  
  assert ( renderer->fb != NULL );
  if ( row < 0 || row >= NLINES ) return;
  if ( row < a->clip_y1 || row > a->clip_y2 ) return;
  ++stats->nlines;
  
  // Prepara.
  c0= col0;
  if ( c0 < a->clip_x1 ) c0= a->clip_x1; // OJO!!!
  c1= col1;
  if ( c1 > a->clip_x2 ) c1= a->clip_x2; // OJO!!!!
  if ( c0 > c1 ) return;
  d_row= &(DITHERING[row&0x3][0]);
  line= renderer->fb + row*NCOLS;
  
  // Textura plana
  if ( tex->raw_texture )
    {
      uf= ((double) c0)*tex->a + ((double) row)*tex->b + tex->c;
      vf= ((double) c0)*tex->d + ((double) row)*tex->e + tex->f;
      for ( c= c0; c <= c1; ++c )
        {
          assert ( tex->tex_enabled );
          color= tex_get_color ( tex, &uf, &vf, a );
          if ( color == 0 ) continue;
          if ( a->check_mask && (line[c]&0x8000) ) continue;
          if ( a->set_mask ) color|= 0x8000;
          line[c]= color;
          ++(stats->npixels);
        }
    }

  // Color sòlid fons
  else if ( !tex->gouraud_enabled )
    {
      if ( tex->tex_enabled )
        {
          uf= ((double) c0)*tex->a + ((double) row)*tex->b + tex->c;
          vf= ((double) c0)*tex->d + ((double) row)*tex->e + tex->f;
          icolor= 0;
        }
      else icolor= TORGB15b ( a->r, a->g, a->b );
      for ( c= c0; c <= c1; ++c )
        {
          if ( tex->tex_enabled )
            {
              color= tex_get_color ( tex, &uf, &vf, a );
              if ( color == 0 ) continue;
              if ( a->modulate_texture )
                {
                  r= a->r; g= a->g; b= a->b;
                  modulate_color ( &r, &g, &b, color );
                  color=
                    (color&0x8000) |
                    (a->dithering ?
                     apply_dithering ( d_row[c&0x3], r, g, b ) :
                     TORGB15b ( r, g, b ));
                }
              if ( a->transparency != PSX_TR_NONE && (color&0x8000) )
                color= apply_color_blending ( a->transparency,
                                              line[c], color );
              if ( a->check_mask && (line[c]&0x8000) ) continue;
              if ( a->set_mask ) color|= 0x8000;
              line[c]= color;
              ++(stats->npixels);
            }
          else
            {
              if ( a->dithering )
                color= apply_dithering ( d_row[c&0x3], a->r, a->g, a->b );
              else color= icolor;
              if ( a->transparency != PSX_TR_NONE )
                color= apply_color_blending ( a->transparency,
                                              line[c], color );
              if ( a->check_mask && (line[c]&0x8000) ) continue;
              if ( a->set_mask ) color|= 0x8000;
              line[c]= color;
              ++(stats->npixels);
            }
        }
    }
  
  // Gouraud
  else
    {
      rf= ((double) c0)*tex->r_a + ((double) row)*tex->r_b + tex->r_c;
      gf= ((double) c0)*tex->g_a + ((double) row)*tex->g_b + tex->g_c;
      bf= ((double) c0)*tex->b_a + ((double) row)*tex->b_b + tex->b_c;
      if ( tex->tex_enabled )
        {
          uf= ((double) c0)*tex->a + ((double) row)*tex->b + tex->c;
          vf= ((double) c0)*tex->d + ((double) row)*tex->e + tex->f;
        }
      for ( c= c0; c <= c1; ++c )
        {
          if ( rf < 0.0 ) r= 0;
          else if ( rf > 255.0 ) r= 255;
          else r= (uint8_t) (rf + 0.5);
          if ( gf < 0.0 ) g= 0;
          else if ( gf > 255.0 ) g= 255;
          else g= (uint8_t) (gf + 0.5);
          if ( bf < 0.0 ) b= 0;
          else if ( bf > 255.0 ) b= 255;
          else b= (uint8_t) (bf + 0.5);
          rf+= tex->r_a; gf+= tex->g_a; bf+= tex->b_a;
          if ( tex->tex_enabled )
            {
              color= tex_get_color ( tex, &uf, &vf, a );
              if ( color == 0 ) continue;
              if ( a->modulate_texture )
                {
                  modulate_color ( &r, &g, &b, color );
                  color=
                    (color&0x8000) |
                    (a->dithering ?
                     apply_dithering ( d_row[c&0x3], r, g, b ) :
                     TORGB15b ( r, g, b ));
                }
              if ( a->transparency != PSX_TR_NONE && (color&0x8000) )
                color= apply_color_blending ( a->transparency,
                                              line[c], color );
              if ( a->check_mask && (line[c]&0x8000) ) continue;
              if ( a->set_mask ) color|= 0x8000;
              line[c]= color;
              ++(stats->npixels);
            }
          else
            {
              if ( a->dithering )
                color= apply_dithering ( d_row[c&0x3], r, g, b );
              else color= TORGB15b ( r, g, b );
              if ( a->transparency != PSX_TR_NONE )
                color= apply_color_blending ( a->transparency,
                                              line[c], color );
              if ( a->check_mask && (line[c]&0x8000) ) continue;
              if ( a->set_mask ) color|= 0x8000;
              line[c]= color;
              ++(stats->npixels);
            }
        }
    }
  
} // end draw_triangle_fill_line


static void
draw_triangle (
               default_renderer_t     *renderer,
               const PSX_RendererArgs *a,
               const PSX_VertexInfo   *v0,
               const PSX_VertexInfo   *v1,
               const PSX_VertexInfo   *v2,
               const pol_tex_t        *tex,
               PSX_RendererStats      *stats
               )
{

  edge_t edges[3];
  int N,row,p_aet,p_get,p;
  int col0,col1;

  
  // Inicialitza la Global Edge Table. De manera que no cal mai
  // reodernar per X.
  N= 0;
  add_edge ( v0, v1, edges, &N );
  add_edge ( v1, v2, edges, &N );
  add_edge ( v2, v0, edges, &N );
  if ( N <= 1 ) return; // Res a dibuixar !!!

  // Prepara renderitzat.
  row= edges[0].y_min;
  p_aet= 0; // Posició Active Edge Table
  for ( p_get= 0; p_get < N && edges[p_get].y_min == row; ++p_get );
  
  // Renderitza.
  while ( p_aet < N )
    {
      
      // Obté columnes. Típicament sols hi han 2, però hi ha un cas
      // especial amb un únic vertex.
      assert ( p_aet+1 != p_get );
      col0= TOINT(edges[p_aet].x);
      col1= TOINT(edges[p_aet+1].x);
      --col1;
      
      // Renderitza
      if ( col0 <= col1 )
        draw_triangle_fill_line ( renderer, a, row, col0, col1, tex, stats );
      
      // Prepara següent.
      ++row;
      // --> Actualitza valors
      for ( p= p_aet; p != p_get; ++p )
        {
          ++edges[p].y_min;
          edges[p].x+= edges[p].slope;
        }
      // --> Elimina arestes
      p_aet= remove_edges_aet ( p_aet, p_get, edges );
      // --> Afegeix arestes noves
      for ( ;
            p_get < N && edges[p_get].y_min == row;
            ++p_get );
      // --> Reordena X si cal. Com sols són 2 com a molt és fàcil.
      if ( p_aet != p_get ) sort_edges_aet ( p_aet, p_get, edges );
      
    }
  
} // end draw_triangle




/***********/
/* MÈTODES */
/***********/

static void
free_ (
       PSX_Renderer *rend
       )
{
  free ( rend );
} /* end free_ */


static void
lock (
      PSX_Renderer *renderer,
      uint16_t     *fb
      )
{
  DR(renderer)->fb= NULL;
} /* end lock */


static void
unlock (
        PSX_Renderer *renderer,
        uint16_t     *fb
        )
{
  DR(renderer)->fb= fb;
} /* end unlock */


static void
pol3 (
      PSX_Renderer      *renderer,
      PSX_RendererArgs  *a,
      PSX_RendererStats *stats
      )
{
  
  const PSX_VertexInfo *v0,*v1,*v2;
  pol_tex_t tex;
  
  
  stats->npixels= 0;
  stats->nlines= 0;
  v0= &(a->v[0]); v1= &(a->v[1]); v2= &(a->v[2]);
  pol_tex_gouraud_init ( &tex, DR(renderer)->fb, v0, v1, v2, a );
  draw_triangle ( DR(renderer), a, v0, v1, v2, &tex, stats );
  
} // end pol3


static void
pol4 (
      PSX_Renderer      *renderer,
      PSX_RendererArgs  *a,
      PSX_RendererStats *stats
      )
{

  const PSX_VertexInfo *va0,*va1,*va2,*vb0,*vb1,*vb2;
  pol_tex_t tex;

  
  stats->npixels= 0;
  stats->nlines= 0;
  va0= &(a->v[0]); va1= &(a->v[1]); va2= &(a->v[2]);
  pol_tex_gouraud_init ( &tex, DR(renderer)->fb, va0, va1, va2, a );
  draw_triangle ( DR(renderer), a, va0, va1, va2, &tex, stats );
  vb0= &(a->v[1]); vb1= &(a->v[2]); vb2= &(a->v[3]);
  pol_tex_gouraud_init ( &tex, DR(renderer)->fb, vb0, vb1, vb2, a );
  draw_triangle ( DR(renderer), a, vb0, vb1, vb2, &tex, stats );
  
} // end pol4


static void
rect (
      PSX_Renderer      *renderer,
      PSX_RendererArgs  *a,
      const int          width,
      const int          height,
      PSX_RendererStats *stats
      )
{

  uint16_t *off,*p,color;
  int r,c,cx1,cx2,cy1,cy2;
  uint8_t u,v,R,g,b;
  bool tex_enabled;
  const uint16_t *clut;
  const uint16_t *page;
  
  
  stats->npixels= 0;
  stats->nlines= 0;
  tex_enabled= (a->texture_mode!=PSX_TEX_NONE);
  if ( tex_enabled )
    {
      clut= &(DR(renderer)->fb[a->texclut_y*1024 + a->texclut_x*16]);
      page= &(DR(renderer)->fb[a->texpage_y*256*1024 + a->texpage_x*64]);
    }
  else page= clut= NULL; /* CALLA!!! */
  off= &(DR(renderer)->fb[a->v[0].y*1024 + a->v[0].x]);
  v= a->texflip_y ? (a->v[0].v-1) : a->v[0].v;
  cy1= a->clip_y1 - a->v[0].y; cy2= a->clip_y2 - a->v[0].y;
  cx1= a->clip_x1 - a->v[0].x; cx2= a->clip_x2 - a->v[0].x;
  for ( r= 0; r < height; ++r )
    {
      u= a->texflip_x ? (a->v[0].u-1) : a->v[0].u;
      v= (v&a->texwinmask_y) | a->texwinoff_y;
      if ( r >= cy1 && r <= cy2 )
        {
          for ( p= off, c= 0; c < width; ++c, ++p )
            {
              if ( tex_enabled ) /* Textura, sense dithering però amb flip. */
        	{
        	  u= (u&a->texwinmask_x) | a->texwinoff_x;
        	  color= read_tex_color ( u, v, a->texture_mode, page, clut );
        	  if ( a->texflip_x ) --u; else ++u;
                  if ( color == 0 ) continue;
        	  if ( a->modulate_texture )
        	    {
        	      R= a->r; g= a->g; b= a->b;
        	      modulate_color ( &R, &g, &b, color );
        	      color= (color&0x8000) | TORGB15b ( R, g, b );
        	    }
        	}
              else color= TORGB15b ( a->r, a->g, a->b );
              if ( c < cx1 || c > cx2 ) continue;
              if ( a->transparency != PSX_TR_NONE &&
        	   (!tex_enabled || (color&0x8000) ) )
        	color= apply_color_blending ( a->transparency, *p, color );
              if ( a->check_mask && (*p&0x8000) ) continue;
              if ( a->set_mask ) color|= 0x8000;
              *p= color;
              ++(stats->npixels);
            }
        }
      if ( a->texflip_y ) --v; else ++v;
      off+= 1024;
    }
  
} /* end rect */


static void
line (
      PSX_Renderer      *renderer,
      PSX_RendererArgs  *a,
      PSX_RendererStats *stats
      )
{

  bool changed;
  int dx,dy,signx,signy,tmp,i,x,y,e;
  double dr,dg,db,aux,rf,gf,bf;
  uint16_t color,*pixel;
  uint8_t r,g,b;
  
  
  stats->npixels= 0;
  stats->nlines= 0;
  
  /* Prepara. */
  dx= a->v[1].x - a->v[0].x;
  if ( dx < 0 ) { dx= -dx; signx= -1; }
  else signx= 1;
  
  dy= a->v[1].y - a->v[0].y;
  if ( dy < 0 ) { dy= -dy; signy= -1; }
  else signy= 1;

  if ( dy > dx )
    {
      tmp= dx; dx= dy; dy= tmp;
      changed= true;
    }
  else changed= false;

  /* Renderitza. */
  if ( a->gouraud )
    {
      aux= 1.0 / ((double) dx);
      dr= (((int) a->v[1].r) - ((int) a->v[0].r)) * aux;
      dg= (((int) a->v[1].g) - ((int) a->v[0].g)) * aux;
      db= (((int) a->v[1].b) - ((int) a->v[0].b)) * aux;
      rf= (double) a->v[0].r;
      gf= (double) a->v[0].g;
      bf= (double) a->v[0].b;
    }
  else { rf= gf= bf= dr= dg= db= 0.0; }
  e= 2*dy - dx;
  x= a->v[0].x; y= a->v[0].y;
  for ( i= 0; i <= dx; ++i )
    {

      /* Dibuixa. */
      if ( a->gouraud )
        {
          r= (uint8_t) (rf + 0.5);
          g= (uint8_t) (gf + 0.5);
          b= (uint8_t) (bf + 0.5);
          rf+= dr; gf+= dg; bf+= db;
        }
      else { r= a->r; g= a->g; b= a->b; }
      if ( y >= a->clip_y1 && y <= a->clip_y2 &&
           x >= a->clip_x1 && x <= a->clip_x2 )
        {
          pixel= &(DR(renderer)->fb[y*1024 + x]);
          if ( a->dithering )
            color= apply_dithering ( DITHERING[y&0x3][x&0x3], r, g, b );
          else color= TORGB15b ( r, g, b );
          if ( a->transparency != PSX_TR_NONE )
            color= apply_color_blending ( a->transparency, *pixel, color );
          if ( !a->check_mask || !((*pixel)&0x8000) )
            {
              if ( a->set_mask ) color|= 0x8000;
              *pixel= color;
              ++(stats->npixels);
            }
        }
      
      /* Actualitza. */
      if ( e > 0 )
        {
          if ( changed ) x+= signx;
          else           y+= signy;
          e-= 2*dx;
        }
      if ( changed ) y+= signy;
      else           x+= signx;
      e+= 2*dy;
      
    }
  
} /* end line */


static void
draw_15bit (
            default_renderer_t      *renderer,
            const PSX_FrameGeometry *g
            )
{

  /*
   * NOTA!! De moment estic ignorant açò:
   *
   *  Ie. on the PSX, the intensity increases steeply from 0 to 15,
   *  and less steeply from 16 to 31.
   */

  const double FACTOR= 255.0/31.0;
  
  uint8_t *p;
  const uint16_t *line,*q;
  int r,c;
  uint16_t color;

  
  p= &(renderer->out_fb[0]);
  line= &(renderer->fb[g->y*NCOLS + g->x]);
  for ( r= 0; r < g->height; ++r )
    {
      q= line;
      for ( c= 0; c < g->width; ++c )
        {
          color= *(q++);
          p[0]= (uint8_t) ((color&0x1f)*FACTOR + 0.5);
          p[1]= (uint8_t) (((color>>5)&0x1f)*FACTOR + 0.5);
          p[2]= (uint8_t) (((color>>10)&0x1f)*FACTOR + 0.5);
          p[3]= 0xff;
          p+= 4;
        }
      line+= NCOLS;
    }
  
} /* end draw_15bit */


static void
draw_24bit (
            default_renderer_t      *renderer,
            const PSX_FrameGeometry *g
            )
{

  /*
   * NOTA!! De moment estic ignorant açò:
   *
   *  Ie. on the PSX, the intensity increases steeply from 0 to 15,
   *  and less steeply from 16 to 31.
   */
  uint8_t *p;
  const uint8_t *line,*q;
  int r,c;
  
  
  p= &(renderer->out_fb[0]);
  line= (const uint8_t *) &(renderer->fb[g->y*NCOLS + g->x]);
  for ( r= 0; r < g->height; ++r )
    {
      q= line;
      for ( c= 0; c < g->width; ++c )
        {
          p[0]= q[0];
          p[1]= q[1];
          p[2]= q[2];
          p[3]= 0xff;
          p+= 4; q+= 3;
        }
      line+= NCOLS*2; /* Incremente com si foren línies de 15 bits. */
    }
  
} /* end draw_24bit */


static void
draw_blank_screen (
        	   default_renderer_t *dr
        	   )
{
  
  PSX_UpdateScreenGeometry gg;


  gg.width= 320;
  gg.height= 240;
  gg.x0= 0; gg.x1= 1;
  gg.y0= 0; gg.y1= 1;
  memset ( dr->out_fb, 0, sizeof(uint32_t)*gg.width*gg.height );
  dr->update_screen  ( (const uint32_t *) &(dr->out_fb[0]),
        	       &gg, dr->udata );
  
} /* end draw_blank_renderer */


/* NOTA!!! Ho dibuixe tot en RGBA. */
static void
draw (
      PSX_Renderer            *renderer,
      const PSX_FrameGeometry *g
      )
{

  PSX_UpdateScreenGeometry gg;

  
  if ( !(DR(renderer)->display_enabled) )
    {
      draw_blank_screen ( DR(renderer) );
      return;
    }
  
  /* Ompli el out_fb. */
  if ( g->is15bit ) draw_15bit ( DR(renderer), g );
  else              draw_24bit ( DR(renderer), g );

  /* Actualitza la pantalla. */
  gg.width= g->width;
  gg.height= g->height;
  gg.x0= g->d_x0; gg.x1= g->d_x1;
  gg.y0= g->d_y0; gg.y1= g->d_y1;
  DR(renderer)->update_screen  ( (const uint32_t *) &(DR(renderer)->out_fb[0]),
        			 &gg, DR(renderer)->udata );
  
} /* end draw */


static void
enable_display (
        	PSX_Renderer *renderer,
        	const bool    enable
        	)
{
  DR(renderer)->display_enabled= enable;
} /* end enable_display */




/**********************/
/* FUNCIONS PÚBLIQUES */
/**********************/

PSX_Renderer *
PSX_create_default_renderer (
        		     PSX_UpdateScreen *update_screen,
        		     void             *udata
        		     )
{

  default_renderer_t *new;
  int r;
  

  new= mem_alloc ( default_renderer_t, 1 );
  
  /* Mètodes. */
  new->free= free_;
  new->lock= lock;
  new->unlock= unlock;
  new->pol3= pol3;
  new->pol4= pol4;
  new->rect= rect;
  new->line= line;
  new->draw= draw;
  new->enable_display= enable_display;
  
  /* Altres. */
  new->fb= NULL;
  new->udata= udata;
  new->update_screen= update_screen;
  new->display_enabled= false;
  for ( r= 0; r < NLINES; ++r )
    new->pol.p[r].enabled= false;
  new->pol.r0= NLINES;
  new->pol.r1= -1;
  
  return PSX_RENDERER(new);
  
} // end PSX_create_default_renderer

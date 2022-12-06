/*
 * Copyright 2018-2022 Adrià Giménez Pastor.
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
 *  stats_renderer.c - Implementació de PSX_Renderer per a calcular
 *                     estimacions del número de pixels dibuixats.
 *
 */


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

#define DR(ptr) ((stats_renderer_t *) (ptr))

#define MAXSLNODES 4

#define NLINES 512
#define NCOLS 1024

#define MAXWIDTH 640
#define MAXHEIGHT 480

#define SWITCH_VERTEX(TMP,A,B)        					\
  do {        								\
    (TMP)= (A); (A)= (B); (B)= (TMP);        				\
  } while(0)




/*********/
/* TIPUS */
/*********/

typedef struct
{
  
  PSX_RENDERER_CLASS;
  
} stats_renderer_t;

// Per a fill_flat_triangle
typedef struct
{

  int  c,x;
  int  dx,dy;
  int  inc_x;
  bool changed;
  bool left_line;
  int  e;
  
} fftri_line_t;

typedef struct
{

  int  dx,dy;
  
} fftri_extra_t;




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
  
} // end mem_alloc_


#define mem_alloc(type,size)                            \
  ((type *) mem_alloc_ ( sizeof(type) * (size) ))


/* Triangles ******************************************************************/

// NOTA!!! Si detecta que els tres vertex tenen la mateixa y para
// d'ordenar i torna false. En qualsevol altre cas torna true.
static bool
sort_coords_pol3 (
        	  const PSX_VertexInfo **v0,
        	  const PSX_VertexInfo **v1,
        	  const PSX_VertexInfo **v2
        	  )
{

  const PSX_VertexInfo *tmp;
  
  
  if ( (*v0)->y == (*v1)->y )
    {
      // No té sentit continuar ordenant.
      if ( (*v0)->y == (*v2)->y ) return false;
      else if ( (*v0)->y < (*v2)->y )
        {
          if ( (*v1)->x < (*v0)->x ) SWITCH_VERTEX ( tmp, *v0, *v1 );
        }
      
      else // if ( *y0 > *y2 )
        {
          SWITCH_VERTEX ( tmp, *v0, *v2 );
          if ( (*v2)->x < (*v1)->x ) SWITCH_VERTEX ( tmp, *v1, *v2 );
        }
    }

  else if ( (*v0)->y > (*v1)->y )
    {
      if ( (*v0)->y < (*v2)->y ) SWITCH_VERTEX ( tmp, *v0, *v1 );
      else if ( (*v0)->y == (*v2)->y )
        {
          SWITCH_VERTEX ( tmp, *v0, *v1 );
          if ( (*v2)->x < (*v1)->x ) SWITCH_VERTEX ( tmp, *v1, *v2 );
        }
      else // if ( *y0 > *y2 )
        {
          SWITCH_VERTEX ( tmp, *v0, *v2 );
          if ( (*v1)->y < (*v0)->y ||
               ((*v1)->y == (*v0)->y && (*v1)->x < (*v0)->x) )
            SWITCH_VERTEX ( tmp, *v0, *v1 );
        }
    }
  
  else // if ( *y0 < *y1 )
    {
      if ( (*v0)->y > (*v2)->y ) // v2 < v0 < v1
        {
          SWITCH_VERTEX ( tmp, *v1, *v2 );
          SWITCH_VERTEX ( tmp, *v0, *v1 );
        }
      else if ( (*v0)->y < (*v2)->y )
        {
          if ( (*v2)->y < (*v1)->y ||
               ((*v1)->y == (*v2)->y && (*v2)->x < (*v1)->x)  )
            SWITCH_VERTEX ( tmp, *v1, *v2 );
        }
      else  // if ( *y0 == *y2 )
        {
          SWITCH_VERTEX ( tmp, *v1, *v2 ); // v0 < v2 < v1
          if ( (*v1)->x < (*v0)->x ) // v2 < v0 < v1
            SWITCH_VERTEX ( tmp, *v0, *v1 );
        }
    }

  return true;
  
} // end sort_coords_pol3


static void
fftri_line_init (
        	 const int               dy, // Absolute.
        	 const PSX_VertexInfo   *v0,
        	 const PSX_VertexInfo   *v1,
        	 fftri_line_t           *line,
        	 const PSX_RendererArgs *a,
        	 const fftri_extra_t    *extra,
        	 const bool              left_line
        	 )
{

  int tmp_dy;
  
  
  // Algorisme dibuixat triangle.
  line->left_line= left_line;
  line->c= line->x= v0->x;
  if ( v1->x == -1 )
    {
      line->dx= extra->dx;
      tmp_dy= extra->dy;
    }
  else
    {
      line->dx= v1->x - v0->x;
      tmp_dy= dy;
    }
  if ( line->dx < 0 )
    {
      line->dx= -line->dx;
      line->inc_x= -1;
    }
  else line->inc_x= 1;
  if ( tmp_dy > line->dx )
    {
      line->dy= line->dx;
      line->dx= tmp_dy;
      line->changed= true;
    }
  else
    {
      line->dy= tmp_dy;
      line->changed= false;
    }
  line->e= 2*line->dy - line->dx;
  
} // end fftri_line_init


static void
fftri_line_update (
        	   fftri_line_t *line
        	   )
{

  bool stop;
  
  
  stop= false;
  line->c= line->left_line ? 99999 : -1;
  do {
    if ( line->left_line ) { if ( line->x < line->c ) line->c= line->x; }
    else                   { if ( line->x > line->c ) line->c= line->x; }
    if ( line->e >= 0 )
      {
        if ( line->changed ) line->x+= line->inc_x;
        else stop= true;
        line->e-= 2*line->dx;
      }
    if ( line->changed ) stop= true;
    else line->x+= line->inc_x;
    line->e+= 2*line->dy;
  } while ( !stop );
  
} // end fftri_line_update


static void
fill_line (
           const fftri_line_t     *lineA,
           const fftri_line_t     *lineB,
           const int               row,
           const PSX_RendererArgs *a,
           PSX_RendererStats      *stats,
           const bool              dummy
           )
{

  int beg,end,npixels;
  

  if ( dummy ) return;
  if ( row < a->clip_y1 || row > a->clip_y2 ) return;
  beg= a->clip_x1 > lineA->c ? a->clip_x1 : lineA->c;
  end= a->clip_x2 < lineB->c ? a->clip_x2 : lineB->c;
  npixels= end-beg;
  if ( npixels > 0 ) stats->npixels+= npixels;
  
} // end fill_line


// Pot tractar amb triangles 'top' i 'bottom'. Es supossa v0 vertex
// superior (o inferior), v1->x < v2->x.
static void
fill_flat_triangle (
        	    stats_renderer_t       *renderer,
        	    const PSX_RendererArgs *a,
        	    const PSX_VertexInfo   *v0,
        	    const PSX_VertexInfo   *v1,
        	    const PSX_VertexInfo   *v2,
        	    const fftri_extra_t    *extra,
        	    PSX_RendererStats      *stats
        	    )
{
  
  // A -> (x0,y0) -> (x1,y1)
  // B -> (x0,y0) -> (x2,y2)
  
  int dy,i,row,inc_row;
  fftri_line_t lineA,lineB;
  bool dummy_begin;
  
  
  // Inicialitza.
  dy= v1->y - v0->y;
  if ( dy < 0 ) { dy= -dy; inc_row= -1; dummy_begin= true; }
  else { inc_row= 1; dummy_begin= false; }
  fftri_line_init ( dy, v0, v1, &lineA, a, extra, true );
  fftri_line_init ( dy, v0, v2, &lineB, a, extra, false );
  if ( !dummy_begin && (--dy) == 0 ) return;
  
  // Dibuixa el triangle.
  for ( i= 0, row= v0->y; i <= dy; ++i, row+= inc_row )
    {
      fftri_line_update ( &lineA );
      fftri_line_update ( &lineB );
      fill_line ( &lineA, &lineB, row, a, stats, dummy_begin );
      dummy_begin= false;
    }
  
} // end fill_flat_triangle


static void
fill_triangle (
               stats_renderer_t     *renderer,
               const PSX_VertexInfo *v0,
               const PSX_VertexInfo *v1,
               const PSX_VertexInfo *v2,
               PSX_RendererArgs     *a,
               PSX_RendererStats    *stats
               )
{
  
  const PSX_VertexInfo *v3;
  PSX_VertexInfo v;
  fftri_extra_t extra;
  int x;

  
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
  // Lower right corner is excluded.  Aposte per no dibuixar el píxel
  // de la dreta de cada línea, ni l'última línea per tant no cal fer
  // res al respecte.
  if ( v1->y == v2->y )
    fill_flat_triangle ( renderer, a, v0, v1, v2, &extra, stats );
  else if ( v0->y == v1->y )
    fill_flat_triangle ( renderer, a, v2, v0, v1, &extra, stats );
  else
    {
      v.x= -1;
      x=
        (int) ((v0->x
        	+ (((double) (v1->y-v0->y)) / ((double) (v2->y-v0->y)))
        	* (v2->x-v0->x)) + 0.5);
      v.y= v1->y;
      if ( x < v1->x ) { v3= v1; v1= &v; }
      else               v3= &v;
      extra.dy= v2->y - v0->y;
      extra.dx= v2->x - v0->x;
      fill_flat_triangle ( renderer, a, v0, v1, v3, &extra, stats );
      extra.dx= -extra.dx;
      fill_flat_triangle ( renderer, a, v2, v1, v3, &extra, stats );
    }
#pragma GCC diagnostic pop
  
} // end fill_triangle




/***********/
/* MÈTODES */
/***********/

static void
free_ (
       PSX_Renderer *rend
       )
{
  free ( rend );
} // end free_


static void
lock (
      PSX_Renderer *renderer,
      uint16_t     *fb
      )
{
} // end lock


static void
unlock (
        PSX_Renderer *renderer,
        uint16_t     *fb
        )
{
} // end unlock


static void
pol3 (
      PSX_Renderer      *renderer,
      PSX_RendererArgs  *a,
      PSX_RendererStats *stats
      )
{
  
  const PSX_VertexInfo *v0,*v1,*v2;


  stats->npixels= 0;
  v0= &(a->v[0]); v1= &(a->v[1]); v2= &(a->v[2]);
  if ( !sort_coords_pol3 ( &v0, &v1, &v2 ) )
    return;
  fill_triangle ( DR(renderer), v0, v1, v2, a, stats );
  
} // end pol3


static void
pol4 (
      PSX_Renderer      *renderer,
      PSX_RendererArgs  *a,
      PSX_RendererStats *stats
      )
{

  const PSX_VertexInfo *va0,*va1,*va2,*vb0,*vb1,*vb2;


  stats->npixels= 0;
  va0= &(a->v[0]); va1= &(a->v[1]); va2= &(a->v[2]);
  vb0= &(a->v[1]); vb1= &(a->v[2]); vb2= &(a->v[3]);
  if ( !sort_coords_pol3 ( &va0, &va1, &va2 ) ) 
    {  // Si el primer no és un triangle.
      if ( !sort_coords_pol3 ( &vb0, &vb1, &vb2 ) )
        return;
      fill_triangle ( DR(renderer), vb0, vb1, vb2, a, stats );
    }
  else
    {
      // El segon no és un triangle
      if ( !sort_coords_pol3 ( &vb0, &vb1, &vb2 ) )
        fill_triangle ( DR(renderer), va0, va1, va2, a, stats );
      else
        {
          fill_triangle ( DR(renderer), va0, va1, va2, a, stats );
          fill_triangle ( DR(renderer), vb0, vb1, vb2, a, stats );
        }
    }
  
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

  int cx1,cx2,cy1,cy2,beg,end;
  int real_width, real_height;
  

  stats->npixels= 0;
  cy1= a->clip_y1 - a->v[0].y; cy2= a->clip_y2 - a->v[0].y;
  cx1= a->clip_x1 - a->v[0].x; cx2= a->clip_x2 - a->v[0].x;
  beg= cy1>0 ? cy1 : 0;
  end= cy2<height ? cy2 : height-1;
  real_height= end-beg+1;
  if ( real_height < 0 ) { stats->npixels= 0; return; }
  beg= cx1>0 ? cx1 : 0;
  end= cx2<width ? cx2 : width-1;
  real_width= end-beg+1;
  if ( real_width < 0 ) { stats->npixels= 0; return; }
  stats->npixels= real_width*real_height;
  
} // end rect


static void
line (
      PSX_Renderer      *renderer,
      PSX_RendererArgs  *a,
      PSX_RendererStats *stats
      )
{
  
  bool changed;
  int dx,dy,signx,signy,tmp,i,x,y,e;
  

  stats->npixels= 0;
  
  // Prepara.
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

  // Renderitza.
  e= 2*dy - dx;
  x= a->v[0].x; y= a->v[0].y;
  for ( i= 0; i <= dx; ++i )
    {

      // Dibuixa.
      if ( y >= a->clip_y1 && y <= a->clip_y2 &&
           x >= a->clip_x1 && x <= a->clip_x2 )
        {
          //if ( !a->check_mask || !((*pixel)&0x8000) )
          ++(stats->npixels);
        }
      
      // Actualitza.
      if ( e >= 0.0 )
        {
          if ( changed ) x+= signx;
          else           y+= signy;
          e-= 2*dx;
        }
      if ( changed ) y+= signy;
      else           x+= signx;
      e+= 2*dy;
      
    }
  
} // end line


static void
draw (
      PSX_Renderer            *renderer,
      const PSX_FrameGeometry *g
      )
{
} // end draw


static void
enable_display (
        	PSX_Renderer *renderer,
        	const bool    enable
        	)
{
} // end enable_display




/**********************/
/* FUNCIONS PÚBLIQUES */
/**********************/

PSX_Renderer *
PSX_create_stats_renderer (void)
{

  stats_renderer_t *new;
  

  new= mem_alloc ( stats_renderer_t, 1 );
  
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
  
  return PSX_RENDERER(new);
  
} // end PSX_create_stats_renderer

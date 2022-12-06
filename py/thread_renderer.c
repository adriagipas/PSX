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
 *  thread_renderer.c - Implementació de 'thread_renderer.h'.
 *
 */


#include <glib.h>
#include <stddef.h>
#include <stdlib.h>

#include "thread_renderer.h"




/*********************/
/* FUNCIONS PRIVADES */
/*********************/

static gpointer
loop (
      gpointer data
      )
{

  ThreadRenderer *self;


  self= (ThreadRenderer *) data;
  g_mutex_lock ( &(self->mutex) );
  while ( !self->stop )
    {
      
      while ( self->N==0 && !self->stop )
        g_cond_wait ( &(self->buffer_not_empty), &(self->mutex) );
      if ( self->stop ) break;

      // Process comand
      switch ( self->buffer[self->p].cmd )
        {
        case THREAD_RENDERER_POL3:
          self->renderer->pol3 ( self->renderer,);
          break;
        }
    }
  g_mutex_unlock ( &(self->mutex) );

  return NULL;
  
} // end loop




/**********************/
/* FUNCIONS PÚBLIQUES */
/**********************/

PSX_Renderer *
thread_renderer_new (
        	     PSX_Renderer *renderer
        	     )
{

  ThreadRenderer *new;


  // Reserva i inicialitza.
  new= g_new ( ThreadRenderer, 1 );
  new->renderer= renderer;
  new->thread= NULL;
  new->stop= false;
  new->N= 0;
  new->p= 0;

  // Mètodes.
  
  // Inicialitza thread.
  g_mutex_init ( &(new->mutex) );
  g_cond_init ( &(new->buffer_not_empty) );
  new->thread= g_thread_new ( "renderer", loop, new );

  return (PSX_Renderer *) new;
  
} // end thread_renderer_new

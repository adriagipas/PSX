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
 * thread_renderer.h - PSX_Renderer executant-se en un altre thread.
 *
 */

#ifndef __THREAD_RENDERER_H__
#define __THREAD_RENDERER_H__

#include <glib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "PSX.h"

#define THREAD_RENDERER_BSIZE 100

typedef struct
{

  PSX_RENDERER_CLASS;

  PSX_Renderer *renderer;
  GThread      *thread;
  GMutex        mutex;
  GCond         buffer_not_empty;
  bool          stop;
  struct
  {
    enum {
      THREAD_RENDERER_POL3,
      THREAD_RENDERER_POL4,
      THREAD_RENDERER_RECT,
      THREAD_RENDERER_LINE
    }                 cmd;
    PSX_RendererArgs  args;
    PSX_RendererStats stats;
    int               width;
    int               height;
  }             buffer[THREAD_RENDERER_BSIZE];
  int           p;
  int           N;
  
} ThreadRenderer;

// Nota el renderer baseline no es alliberat.
PSX_Renderer *
thread_renderer_new (
        	     PSX_Renderer *renderer
        	     );

#endif // __THREAD_RENDERER_H__

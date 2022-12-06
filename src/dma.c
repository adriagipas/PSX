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
 *  dma.c - Implementació del mòdul de DMA.
 *
 *  NOTA!!! Inspirat per mednafen he implementat un
 *  PSX_BUS_OWNER_CPU_DMA, és a dir, s'executa CPU i DMA al mateix
 *  temps. No obstant, he decidit de moment desacativar-lo, ja que
 *  Bloody Roar dona problemes. Ho deixe per si en el futur ho
 *  reprenc.
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

#define NUM_CHANS 7

#define START_BUSY_1 0x01000000
#define START_BUSY_2 0x10000000

#define CHN_GT(A,B)        						\
  ((A)->prio < (B)->prio || ((A)->prio == (B)->prio && (A)->id > (B)->id))

#define MAXBUF 0x10000




/*********/
/* TIPUS */
/*********/

typedef struct channel channel_t;

struct channel
{

  // Estat transfer_data
  int td_addr;
  int td_nwords;
  int td_p;
  
  // Registres
  uint32_t madr;
  uint32_t bcr;
  uint32_t chcr;

  // En mode 1 i 2, número paraules següent petició.
  uint32_t nwords_sync;

  // Estat
  int ccperword;
  int id;
  bool is_otc;  // Indica que és el canal especial 6 per a OTC.
  bool active;  // Si està insertat en actives.
  bool running;  // Indica que està en execució. Si està running però
                 // deshabilitat aleshores es considera en execució
                 // però està esperant a que l'habiliten per a poder
                 // fer coses.
  bool enabled;  // Indica que està habilitat.
  int mode;  // 0-3. El 3 no s'utilitza.
  int prio;  // Prioritat 0-7. 0 és la més alta.
  bool toram;
  int  inc;
  union
  {
    struct
    {
      uint32_t addr;  // Adreça actual, s'usa amb chopping.
      int      cc;  // Cicles actuals d'espera. Si chopping i
                    // current_cc<=0 s'ha de ficar altra vegada a
                    // enviar dades.
      uint32_t nwords;  // Paraules que falten en total per a enviar.
      bool     chopping;   // Si té que fer parades o no.
      uint32_t chop_ws;  // Paraules que té que copiar abans de descansar.
      uint32_t chop_cc;  // Cicles que té que esperar en cada descans.
    } m0;
    struct
    {
      uint32_t bsize;
      uint32_t nblocks;
    } m1;
    struct
    {
      uint32_t bsize; // Tamany següent block.
      uint32_t addr;  // Adreça inicial següent block.
      uint32_t next_addr; // Següent adreça llista.
      bool     bad; // A cert indica que l'estat està mal.
    } m2;
  } v;
  
  // Callbacks
  
  // En els mode 1 i 2, li indica al mòdul que es vol transferir el
  // següent bloc. En aquestos modes els mòduls tenen que activar la
  // transferència. Torna cert si la transferència s'activat abans de
  // tornar de la funció, o false si s'ha ajornat per a més
  // endavant. En el mode 1 i 2 'nwords_sync' la grandària de la
  // petició.
  bool (*sync) (uint32_t nwords_sync);
  // Escriu dades al mòdul.
  void (*write) (uint32_t data);
  // Llig dades del buffer.
  uint32_t (*read) (void);
  
};

// Gestiona cicles pendents i utilitzats.
struct
{
  bool waiting_event; // Esperant a que es deperte el canal en mode 0.
  int cc_used;
  int cc;
} _timing;




/*********/
/* ESTAT */
/*********/

/* Callbacks. */
static PSX_DMATransfer *_dma_transfer;
static PSX_Warning *_warning;
static void *_udata;

static bool (*_transfer_data) (channel_t *chn);

/* Canals. */
static channel_t _chans[NUM_CHANS];

/* Canals actius. */
static struct
{
  channel_t *v[NUM_CHANS];
  int N;
} _actives;

// Canal que s'està executant actualment.
static channel_t *_current_chn;

/* Altres registres. */
static uint32_t _dpcr;
static uint32_t _dicr;




/******************/
/* JUNK CALLBACKS */
/******************/

static bool
empty_sync (
            const uint32_t nwords_sync
            )
{
  _warning ( _udata, "No s'ha implementat 'sync'" );
  return false;
} /* end empty_sync */


static void
empty_write (
             uint32_t data
             )
{
  _warning ( _udata, "No s'ha implementat 'write'" );
} // end empty_write


static uint32_t
empty_read (void)
{
  _warning ( _udata, "No s'ha implementat 'read'" );
  return 0xFF00FF00;
} // end empty_read




/*********************/
/* FUNCIONS PRIVADES */
/*********************/

static void
update_dma_running_mode (void)
{

  int tmp;
  const channel_t *top;
  

  // Canal top
  if ( _current_chn != NULL ) top= _current_chn;
  else if ( _actives.N > 0 ) top= _actives.v[0];
  else top= NULL;
  
  // Actualitza cctoEvent
  _timing.waiting_event= (top!=NULL &&
                          top->mode == 0 &&
                          top->v.m0.cc);
  
  // Update PSX_NextEventCC
  tmp= PSX_dma_next_event_cc ();
  if ( tmp != -1 )
    {
      tmp+= PSX_Clock;
      if ( tmp < PSX_NextEventCC )
        PSX_NextEventCC= tmp;
    }

  // Take control of the bus.
  if ( top==NULL || _timing.waiting_event ) // El timing és sempre mode 0 chop.
    PSX_BusOwner= PSX_BUS_OWNER_CPU;
  /*
  else if ( top->mode == 1 )
    PSX_BusOwner= PSX_BUS_OWNER_CPU_DMA;
  */
  else
    PSX_BusOwner= PSX_BUS_OWNER_DMA;
  
} // end update_dma_running_mode


static void
check_interrupts (void)
{

  bool cb31,nb31,b15,b23;


  cb31= (_dicr&0x80000000)!=0;
  b15= (_dicr&0x00008000)!=0;
  b23= (_dicr&0x00800000)!=0;
  nb31= b15 || (b23 && ((_dicr>>16)&(_dicr>>24)&0x7F));
  if ( nb31 ) _dicr|= 0x80000000;
  else        _dicr&= 0x7FFFFFFF;
  if ( !cb31 && nb31 )
    PSX_int_interruption ( PSX_INT_DMA );
  
} /* end check_interrupts */


static void
set_irq_flag (
              const int id
              )
{
  
  _dicr|= ((1<<(24+id))&(_dicr<<8)); /* Sols es fixa si està habilitat
        				en Bit(16+n). */
  check_interrupts ();
  
} /* end set_irq_flag */


/* No hi ha problemes de desbordament. */
static void
actives_add (
             channel_t *chn
             )
{

  int p,q;


  p= _actives.N++;
  while ( p > 0 )
    {
      q= (p-1)/2;
      if ( CHN_GT(_actives.v[q],chn) )
        break;
      _actives.v[p]= _actives.v[q];
      p= q;
    }
  _actives.v[p]= chn;
  
} /* end actives_add */


static void
actives_pop (void)
{

  channel_t *chn;
  int p,q,q2;
  
  
  if ( --_actives.N == 0 ) return;
  chn= _actives.v[_actives.N];
  p= 0;
  for (;;)
    {
      q= 2*p+1;
      if ( q >= _actives.N ) break;
      q2= q+1;
      if ( q2 < _actives.N && CHN_GT(_actives.v[q2],_actives.v[q]) )
        q= q2;
      if ( CHN_GT(chn,_actives.v[q]) )
        break;
      _actives.v[p]= _actives.v[q];
      p= q;
    }
  _actives.v[p]= chn;
  
} /* end actives_pop */


static void
init_mode2 (
            channel_t *chn
            )
{

  uint32_t tmp;

  
  // Si per algún motiu l'adreça inicial ja és 0x00FFFFFF acaba.
  if ( chn->madr == 0x00FFFFFF )
    {
      /* <-- Sol passar, no mostre warnings.
      _warning ( _udata,
        	 "DMA (CHN%d): s'ha intentat fer una transferència en"
        	 " mode2 començant en 00FFFFFF", chn->id );
      */
      chn->v.m2.bad= true;
      return;
    }
  
  // Llig la capçalera.
  if ( !PSX_mem_read ( chn->madr, &tmp ) )
    {
      _warning ( _udata,
        	 "DMA (CHN%d): Error de bus al intentar llegir de"
        	 " l'adreça de memòria %08X el punter al següent node de"
        	 " la llista", chn->id, chn->madr );
      chn->v.m2.bad= true;
      return;
    }
  
  // Obté informació.
  chn->v.m2.addr= chn->madr+chn->inc;
  chn->v.m2.bsize= tmp>>24;
  chn->v.m2.next_addr= tmp&0x00FFFFFF;
  chn->v.m2.bad= false;
  
} // end init_mode2


static void
update_state_channel (
        	      channel_t *chn
        	      )
{
  uint32_t bsize;

  
  chn->active= false;
  chn->running= false;
  chn->mode= (chn->chcr>>9)&0x3;
  chn->toram= ((chn->chcr&0x1)==0);
  chn->inc= (chn->chcr&0x2) ? -4 : 4;
  chn->nwords_sync= 0;
  switch ( chn->mode )
    {
    case 0:
      chn->v.m0.addr= chn->madr;
      chn->v.m0.cc= 0;
      chn->v.m0.nwords= chn->bcr&0xFFFF;
      if ( chn->v.m0.nwords == 0 ) chn->v.m0.nwords= 0x10000;
      chn->v.m0.chopping= ((chn->chcr&0x100)!=0);
      chn->v.m0.chop_ws= 1<<((chn->chcr>>16)&0x7);
      chn->v.m0.chop_cc= 1<<((chn->chcr>>20)&0x7);
      if ( chn->v.m0.chopping )
        {
          chn->td_addr= chn->v.m0.addr;
          if ( chn->v.m0.chop_ws > chn->v.m0.nwords )
            {
              bsize= chn->v.m0.nwords;
              chn->v.m0.nwords= 0;
            }
          else
            {
              bsize= chn->v.m0.chop_ws;
              chn->v.m0.nwords-= bsize;
            }
          chn->td_nwords= bsize;
          chn->td_p= 0;
        }
      else // mode0 normal
        {
          chn->td_addr= chn->madr;
          chn->td_nwords= chn->v.m0.nwords;
          chn->td_p= 0;
        }
      break;
    case 1:
      chn->v.m1.bsize= chn->bcr&0xFFFF;
      if ( chn->v.m1.bsize == 0 ) chn->v.m1.bsize= 0x10000;
      chn->v.m1.nblocks= chn->bcr>>16;
      if ( chn->v.m1.nblocks == 0 ) chn->v.m1.bsize= 0x10000;
      chn->nwords_sync= chn->v.m1.bsize;
      chn->td_addr= chn->madr;
      chn->td_nwords= chn->v.m1.bsize;
      chn->td_p= 0;
      break;
    case 2:
      init_mode2 ( chn );
      chn->nwords_sync= chn->v.m2.bsize;
      chn->td_addr= chn->v.m2.addr;
      chn->td_nwords= chn->v.m2.bsize;
      if ( chn->v.m2.bad )
        chn->td_nwords= 0; // Force obviar el següent bloc. Vull que
                           // es processe el bad.
      chn->td_p= 0;
    default: break;
    }
  
} // end update_state_channel


static void
dpcr_changed (void)
{

  int i;
  uint32_t aux,tmp;
  channel_t *chn;
  
  
  // Reseteja actius i current.
  _actives.N= 0;
  
  // Desactiva i habilita si és el cas.
  aux= _dpcr;
  for ( i= 0; i < NUM_CHANS; ++i )
    {
      tmp= aux&0xF; aux>>= 4;
      chn= &(_chans[i]);
      chn->prio= tmp&0x7;
      if ( tmp>>3 ) // Habilita el canal.
        {
          chn->enabled= true;
          if ( chn->running ) // Ja estava running. No sé si són
                              // posibles tots els casos considerats.
            {
              if ( chn != _current_chn && // Si està processant-se no face res
                   (chn->active || chn->mode == 0) )
        	{
        	  chn->active= false;
        	  PSX_dma_active_channel ( i );
        	}
              // NOTA!! Si no estava actiu el deixe desactivat.
            }
        }
      else // Deshabilita canal.
        {
          chn->enabled= false;
          chn->active= false;
          if ( chn->running ) // Força aturada.
            {
              chn->running= false;
              _warning ( _udata,
        		 "S'ha deshabilitat el canal DMA %d mentre"
        		 " es trobava en execució", i );
              if ( _current_chn == chn )
                {
                  _current_chn= NULL;
                  _warning ( _udata,
                             "De fet estava realitzant-se ara mateixa"
                             " una transferència");
                }
            }
        }
    }

  update_dma_running_mode ();
  
} // end dpcr_changed


// Torna cert si encara queden paraules que transmetre.
// Tots els paràmetres s'han d'inicialitzar abans.
static bool
transfer_data (
               channel_t *chn
               )
{

  uint32_t word;

  
  if ( chn->toram )
    {
      word= chn->read ();
      if ( !PSX_mem_write ( chn->td_addr, word ) )
        _warning ( _udata,
                   "Error de bus al intentar el canal DMA %d escriure en"
                   " l'adreça de memòria %08X", chn->id, chn->td_addr );
      chn->td_addr+= chn->inc;
      ++(chn->td_p);
    }
  else // From RAM
    {
      if ( !PSX_mem_read ( chn->td_addr, &word ) )
        _warning ( _udata,
                   "Error de bus al intentar el canal DMA %d llegir de"
                   " l'adreça de memòria %08X", chn->id, chn->td_addr );
      chn->write ( word );
      chn->td_addr+= chn->inc;
      ++(chn->td_p);
    }
  
  return chn->td_p != chn->td_nwords;
  
} // end transfer_data


static bool
transfer_data_trace (
        	     channel_t *chn
        	     )
{

  _dma_transfer ( (int) (chn-&_chans[0]), chn->toram, chn->td_addr, _udata );
  return transfer_data ( chn );
  
} // end transfer_data_trace


static void
end_transfer_mode0 (
                    channel_t *chn
                    )
{
  
  chn->chcr&= ~(START_BUSY_1|START_BUSY_2);
  chn->active= false;
  chn->running= false;
  set_irq_flag ( chn->id );
  
} // end end_transfer_mode0


// NOTA!!! Açò sols s'executa quan no està esperant.
static void
end_transfer_mode0_chop (
                         channel_t *chn
                         )
{
  
  uint32_t bsize;

  
  // NOTA!!! Assumisc que la MADR sols s'actualitza en cada
  // espera.
  chn->v.m0.addr= chn->madr= chn->td_addr;
  if ( chn->v.m0.nwords == 0 ) // Final
    {
      chn->chcr&= ~(START_BUSY_1|START_BUSY_2);
      chn->active= false;
      chn->running= false;
      chn->bcr&= 0xFFFF; // Amb chopping el BA es fica a 0.
      set_irq_flag ( chn->id );
    }
  else // Descansa
    {
      chn->v.m0.cc= chn->v.m0.chop_cc;
      if ( chn->v.m0.chop_ws > chn->v.m0.nwords )
        {
          bsize= chn->v.m0.nwords;
          chn->v.m0.nwords= 0;
        }
      else
        {
          bsize= chn->v.m0.chop_ws;
          chn->v.m0.nwords-= bsize;
        }
      chn->td_nwords= bsize;
      chn->td_p= 0;
      chn->td_addr= chn->v.m0.addr;
      update_dma_running_mode (); // Passa control a CPU!!!!
    }
  
} // end end_transfer_mode0_chop


static void
end_transfer_mode1 (
                    channel_t *chn
                    )
{

  // Actualitza adreça i BCR
  chn->madr= chn->td_addr;
  chn->madr&= 0x00FFFFFF;
  --(chn->v.m1.nblocks);
  chn->bcr= (chn->bcr&0xFFFF) | (chn->v.m1.nblocks<<16);

  // Comprova si queden blocs o no
  if ( chn->v.m1.nblocks == 0 ) // Final
    {
      chn->active= false;
      chn->running= false;
      chn->chcr&= ~START_BUSY_1;
      set_irq_flag ( chn->id );
    }
  else // Prepara següent bloc
    {
      chn->td_addr= chn->madr;
      chn->td_nwords= chn->v.m1.bsize;
      chn->td_p= 0;
      // Si el dispositiu està preparat per a més dades s'activarà
      // immediatament, si no aleshores es quedarà inactiu esperant a
      // que el dispositiu el reactive.
      chn->active= chn->sync ( chn->nwords_sync );
    }
  
} // end end_transfer_mode1


static void
finish_mode2 (
              channel_t *chn
              )
{
  
  chn->active= false;
  chn->running= false;
  chn->chcr&= ~START_BUSY_1;
  set_irq_flag ( chn->id );
  
} // end finish_mode2


static void
end_transfer_mode2 (
                    channel_t *chn
                    )
{

  uint32_t tmp;

  
  // Algun error ha ocorregut en el bloc anterior.
  if ( chn->v.m2.bad ) { finish_mode2 ( chn ); return; }
  
  // Actualitza maddr i comprova finalització.
  // --> NOTA! El codi de loop està mal, però en dos emuladors he vist
  //     quelcom de detectar loops. Tal vegada cal pensar alguna cosa
  //     en el futur?
  //if ( chn->v.m2.next_addr > chn->madr ) goto end; // Loop detectat.¿¿?????
  chn->madr= chn->v.m2.next_addr;
  if ( chn->madr == 0x00FFFFFF ) { finish_mode2 ( chn ); return; }

  // Adelanta la lectura de la capçalera.
  if ( !PSX_mem_read ( chn->madr, &tmp ) )
    {
      _warning ( _udata,
                 "DMA (CHN%d): Error de bus al intentar llegir de"
                 " l'adreça de memòria %08X el punter al següent node de"
                 " la llista", chn->id, chn->madr );
      chn->v.m2.bad= true;
      chn->nwords_sync= 0;
      chn->td_nwords= 0; // Force a que en el següent bloc no es
                         // processen bytes. Vull que es processe el
                         // 'bad'.
    }
  else
    {
      chn->v.m2.addr= chn->madr+chn->inc;
      chn->v.m2.bsize= tmp>>24;
      chn->v.m2.next_addr= tmp&0x00FFFFFF;
      chn->nwords_sync= chn->v.m2.bsize;
      chn->td_nwords= chn->v.m2.bsize;
    }
  chn->td_addr= chn->v.m2.addr;
  chn->td_p= 0;
  // Si és un node buit no cal preguntar al dispositiu.
  chn->active= chn->td_nwords==0 ? true : chn->sync ( chn->nwords_sync );
  
} // end end_transfer_mode2


// Si la transferència acava el flag active es desactiva. Retorna el
// número de cicles emprats.
static int
channel_run (
             channel_t *chn
             )
{
  
  int ret;
  

  ret= chn->ccperword;
  // NOTA!! un bloc de 0 paraules indica que s'ha de botar
  // immediatament. Açò sols pot passar en mode2. En eixe cas tornem
  // els cicles com si s'haguera transmès 1 paraules, en qualsevol cas
  // tot costa temps.
  if ( chn->td_nwords==0 || !_transfer_data ( chn ) )
    {
      switch ( chn->mode )
        {
        case 0:
          if ( chn->v.m0.chopping )  end_transfer_mode0_chop ( chn );
          else                       end_transfer_mode0 ( chn );
          break;
        case 1: end_transfer_mode1 ( chn ); break;
        case 2: end_transfer_mode2 ( chn ); break;
        default:
          printf ( "[DMA] channel_run - WTF!!\n");
        }
    }
  
  return ret;
  
} // end channel_run


static bool
otc_dma_sync (
              const uint32_t nwords
              )
{
  return true;
} // end otc_dma_sync


static void
otc_dma_write (
               uint32_t data
               )
{

  _warning ( _udata,
             "OTC (DMA6) write: el canal no està disponible"
             " en mode escriptura" );
  return;
  
} // end otc_dma_write


static uint32_t
otc_dma_read (void)
{

  const channel_t *chn;
  uint32_t ret;


  chn= &(_chans[6]);
  if ( chn->td_p == chn->td_nwords-1 ) ret= 0x00FFFFFF;
  else
    {
      ret= chn->madr-(4*(chn->td_p+1));
      ret&= 0x00FFFFFF;
    }

  return ret;
  
} // end otc_dma_read


static void
clock (void)
{

  int cc;
  channel_t *chn;
  

  cc= PSX_Clock-_timing.cc_used;
  if ( cc > 0 ) _timing.cc_used+= cc;
  else return;
  if ( !_timing.waiting_event ) return;

  // L'únic event possible és que el canal actiu siga un canal de
  // mode0 que estiguera esperant i el desperten.
  assert ( _current_chn != NULL && _current_chn->mode == 0 );
  chn= _current_chn;
  chn->v.m0.cc-= cc;
  if ( chn->v.m0.cc <= 0 ) // Desperta canal en mode0 chopping
    {
      chn->v.m0.cc= 0;
      // NOTA!! No cal fer res més. L'estat intern ja està preparat
      // per a iniciar la següent transferència. I
      // update_dma_running_event s'encarregarà de recuperar el
      // control.
    }

  update_dma_running_mode ();
  
} // end clock




/**********************/
/* FUNCIONS PÚBLIQUES */
/**********************/

void
PSX_dma_init (
              PSX_DMATransfer *dma_transfer,
              PSX_Warning     *warning,
              void            *udata
              )
{

  /* Callbacks. */
  _dma_transfer= dma_transfer;
  _warning= warning;
  _udata= udata;

  // Trace
  _transfer_data= transfer_data;
  
  /* Inicialitza els canals. */
  memset ( _chans, 0, sizeof(_chans) );

  // Timing.
  _timing.waiting_event= 0;
  _timing.cc_used= 0;
  _timing.cc= 0;
  
  /* DMA 0. */
  _chans[0].id= 0;
  _chans[0].sync= PSX_mdec_in_sync;
  _chans[0].write= PSX_mdec_in_write;
  _chans[0].read= PSX_mdec_in_read;
  _chans[0].ccperword= 1;
  
  /* DMA 1. */
  _chans[1].id= 1;
  _chans[1].sync= PSX_mdec_out_sync;
  _chans[1].write= PSX_mdec_out_write;
  _chans[1].read= PSX_mdec_out_read;
  _chans[1].ccperword= 1;
  
  /* DMA 2. */
  _chans[2].id= 2;
  _chans[2].sync= PSX_gpu_dma_sync;
  _chans[2].write= PSX_gpu_dma_write;
  _chans[2].read= PSX_gpu_dma_read;
  _chans[2].ccperword= 1;

  /* DMA 3. */
  _chans[3].id= 3;
  _chans[3].sync= PSX_cd_dma_sync;
  _chans[3].write= PSX_cd_dma_write;
  _chans[3].read= PSX_cd_dma_read;
  _chans[3].ccperword= 24; // NOCASH diu que es pot canviar entre 34 i
        		   // 40, però no sé com!!!
  
  /* DMA 4. */
  _chans[4].id= 4;
  _chans[4].sync= PSX_spu_dma_sync;
  _chans[4].write= PSX_spu_dma_write;
  _chans[4].read= PSX_spu_dma_read;
  // NOTA: Segons NOCASH es pot configurar, peròooo.. ho vaig a deixar fixe.
  _chans[4].ccperword= 4; // Segons NOCASH és possible que siga més.
  
  /* DMA 5. */
  _chans[5].id= 5;
  _chans[5].sync= empty_sync;
  _chans[5].write= empty_write;
  _chans[5].read= empty_read;
  _chans[5].ccperword= 1; /* CAL CANVIAR !!!! */
  
  /* DMA 6. */
  _chans[6].id= 6;
  _chans[6].sync= otc_dma_sync;
  _chans[6].write= otc_dma_write;
  _chans[6].read= otc_dma_read;
  _chans[6].ccperword= 1;
  _chans[6].is_otc= true;
  
  /* Actius, current i altres registres. */
  //_dpcr= 0x07654321;
  _dpcr= 0; // <-- Com Mednafen
  _dicr= 0;
  _actives.N= 0;
  _current_chn= NULL;
  dpcr_changed ();
  
} /* end PSX_dma_init */


int
PSX_dma_run (void)
{

  int ret;
  
  
  // Selecciona canal.
  if ( _current_chn == NULL )
    {
      assert ( _actives.N > 0 );
      _current_chn= _actives.v[0];
      actives_pop ();
    }

  // Executa.
  ret= channel_run ( _current_chn );
  if ( !_current_chn->active )
    {
      _current_chn= NULL;
      update_dma_running_mode ();
    }
  
  return ret;
  
} // end PSX_dma_run


void
PSX_dma_run_cc (
                const int cc
                )
{

  // Selecciona canal.
  if ( _current_chn == NULL )
    {
      assert ( _actives.N > 0 );
      _current_chn= _actives.v[0];
      actives_pop ();
    }
  
  assert ( _current_chn->mode == 1 && PSX_BusOwner == PSX_BUS_OWNER_CPU_DMA );
  
  // Executa.
  _timing.cc+= cc;
  while ( PSX_BusOwner == PSX_BUS_OWNER_CPU_DMA &&
          _timing.cc >= _current_chn->ccperword )
    {

      // Executa
      channel_run ( _current_chn );
      _timing.cc-= _current_chn->ccperword;
      _timing.cc_used+= _current_chn->ccperword;

      // Comprova si el canal continua actiu.
      if ( !_current_chn->active )
        {

          // Elimina i recalcula estat bus.
          _current_chn= NULL;
          update_dma_running_mode ();

          // Torna a hi haure un canal mode 1.
          if ( PSX_BusOwner == PSX_BUS_OWNER_CPU_DMA )
            {
              assert ( _actives.N > 0 );
              _current_chn= _actives.v[0];
              actives_pop ();
              assert ( _current_chn->mode == 1 );
            }
          // En cas contrari allibere cicles no emprats i para el bucle.
          else _timing.cc= 0;
          
        }
      
    }
  
} // end PSX_dma_run_cc


void
PSX_dma_end_iter (void)
{

  if ( _timing.waiting_event )
    clock ();
  _timing.cc_used= 0;
  
} // end PSX_dma_end_iter


int
PSX_dma_next_event_cc (void)
{

  int ret;
  

  ret= _timing.waiting_event ? _actives.v[0]->v.m0.cc : -1;
  
  return ret;
  
} // end PSX_dma_next_event_cc


void
PSX_dma_active_channel (
        		const int num
        		)
{

  channel_t *chn;


  clock (); // Pot parar l'espera d'un canal mode0 al activar un canal
            // nou (tant des de la CPU com des del DMA)
  
  chn= &(_chans[num]);
  if ( chn->active ) return; // Per al sync durant el run.
  actives_add ( chn );
  chn->active= true;

  update_dma_running_mode ();
  
} // end PSX_dma_active_channel


void
PSX_dma_madr_write (
        	    const int      chn,
        	    const uint32_t data
        	    )
{
  
  if ( _chans[chn].running )
    {
      _warning ( _udata,
        	 "S'ha intentat modificar el registre MADR del canal"
        	 " de DMA %d, però ha sigut ignorat perquè el canal ja"
        	 " està en execució", chn );
      return;
    }
  _chans[chn].madr= data&0x00FFFFFF;

} // end PSX_dma_madr_write


uint32_t
PSX_dma_madr_read (
        	   const int chn
        	   )
{
  return _chans[chn].madr; 
} // end PSX_dma_madr_read


void
PSX_dma_bcr_write (
        	   const int      chn,
        	   const uint32_t data
        	   )
{
  
  if ( _chans[chn].running )
    {
      _warning ( _udata,
        	 "S'ha intentat modificar el registre BCR del canal"
        	 " de DMA %d, però ha sigut ignorat perquè el canal ja"
        	 " està en execució", chn );
      return;
    }
  _chans[chn].bcr= data;
  
} // end PSX_dma_bcr_write


uint32_t
PSX_dma_bcr_read (
        	  const int chn
        	  )
{
  return _chans[chn].bcr;
} // end PSX_dma_bcr_read


void
PSX_dma_chcr_write (
        	    const int      num,
        	    const uint32_t data
        	    )
{

  channel_t *chn;
  
  
  clock (); // Des de la CPU podem amb aquesta fució parar l'espera
            // d'un canal en mode0 chopping i ficar un altre en
            // execució. Per tant millor actualitzar cicles.
  
  chn= &(_chans[num]);
  if ( chn->running )
    {
      _warning ( _udata,
        	 "S'ha intentat modificar el registre CHCR del canal"
        	 " de DMA %d, però ha sigut ignorat perquè el canal ja"
        	 " està en execució", num );
      return;
    }
  if ( !chn->is_otc )
    {
      chn->chcr= data&0x717707FF;
      update_state_channel ( chn );
      if ( chn->mode == 3 )
        {
          _warning ( _udata,
        	     "S'ha configurat el canal de DMA %d per a funcionar"
        	     " en mode 3, el mode 3 no està suportat", num );
          return;
        }
      // El tema de START_BUSY_2 en realitat no tinc clar com funciona.
      chn->running=
        ((data&START_BUSY_1) || (chn->mode==0 && (data&START_BUSY_2)));
      if ( chn->enabled && chn->running )
        {
          if ( chn->mode == 0 ) PSX_dma_active_channel ( num );
          else                  chn->sync ( chn->nwords_sync );
        }
    }
  else // El canal OTC és un poc especial, sols es contempla el mode 0.
    {
      chn->chcr= (data&0x51000000)|0x2;
      update_state_channel ( chn );
      chn->running= ((data&START_BUSY_1) && (data&START_BUSY_2));
      if ( chn->enabled && chn->running )
        PSX_dma_active_channel ( num );
    }

  update_dma_running_mode ();
  
} // end PSX_dma_chcr_write


uint32_t
PSX_dma_chcr_read (
        	   const int chn
        	   )
{
  return _chans[chn].chcr;
} // end PSX_dma_chcr_read


void
PSX_dma_dpcr_write (
        	    const uint32_t data
        	    )
{

  clock (); // Des de la CPU també podem interrompre, en favor d'un
            // altre, l'espera d'un canal en mode0 chopping. Millor
            // actualitzar comptadors.
  
  _dpcr= data;
  dpcr_changed ();
  
} // end PSX_dma_dpcr_write


uint32_t
PSX_dma_dpcr_read (void)
{
  return _dpcr;
} // end PSX_dma_dpcr_read


void
PSX_dma_dicr_write (
        	    const uint32_t data
        	    )
{
  
  _dicr=
    (_dicr&0x80000000) | // b31
    (data&0x00FF803F) | // writable bits
    (_dicr&(~data)&0x7F000000); // Reseteja flags si 1.
  check_interrupts ();
  
} // end PSX_dma_dicr_write


uint32_t
PSX_dma_dicr_read (void)
{
  return _dicr;
} // end PSX_dma_dicr_read


uint32_t
PSX_dma_unk1_read (void)
{
  return 0x7FFAC68B;
} // end PSX_dma_unk1_read


uint32_t
PSX_dma_unk2_read (void)
{
  return 0x00FFFFF7;
} // end PSX_dma_unk2_read


void
PSX_dma_set_mode_trace (
        		const bool enable
        		)
{

  if ( _dma_transfer != NULL && enable )
    _transfer_data= transfer_data_trace;
  else
    _transfer_data= transfer_data;
  
} // end PSX_dma_set_mode_trace


void
PSX_dma_reset (void)
{

  // Desactiva.
  _dpcr= 0; // <-- Com Mednafen
  _dicr= 0;
  dpcr_changed ();
  
} // end PSX_dma_reset

/*
 * Copyright 2015-2025 Adrià Giménez Pastor.
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
 *  PSX.h - Simulador de PlayStation escrit en C.
 *
 *  NOTES:
 *
 * - Sobre el TLB i la cache del R3000A: Per a fer l'intèrpret més
 *   ràpid vaig a assumir que no es gasta mai el TLB i que la D-CACHE
 *   sempre es gasta com scratchpad. La única cosa que faré és mostrar
 *   avisos i ignorar les escriptures a memòria quan estiga el
 *   scratchpad desactivat i la cau desactivada. També ignorare i/o a
 *   scratchpad quan estiga desactivada. La scratchpad la implemente
 *   en el mapa de memòria, en la CPU controle la posibilitat
 *   d'accedir.
 *
 * - SWR, SWL, LWR i LWL són sospitoses de no estar ben implementades.
 *
 * - De moment no implemente la funcionalitat del registre de debug
 *   DCIC. No pareix que es vaja a gastar en els jocs, però donaré
 *   avisos.
 *
 */

#ifndef __PSX_H__
#define __PSX_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "CD.h"

#ifdef __LITTLE_ENDIAN__
#define PSX_LE
#elif defined __BIG_ENDIAN__
#define PSX_BE
#else
#error Per favor defineix __LITTLE_ENDIAN__ o __BIG_ENDIAN__
#endif


/*********/
/* TIPUS */
/*********/

#ifdef PSX_LE
typedef union
{
  uint32_t                        v;
  struct { uint16_t v0,v1; }      w;
  struct { uint8_t v0,v1,v2,v3; } b;
} PSX_GPRegister;
#else /* PSX_BE */
typedef union
{
  uint32_t                        v;
  struct { uint16_t v1,v0; }      w;
  struct { uint8_t v3,v2,v1,v0; } b;
} PSX_GPRegister;
#endif

typedef PSX_GPRegister PSX_Word;

/* Funció per a emetre avísos. */
typedef void 
(PSX_Warning) (
               void       *udata,
               const char *format,
               ...
               );


/*******/
/* CPU */
/*******/
/* Mòdul que simula el processador R3000A. El GTE s'implementa a banda. */

/* Tipus per a l'estat del processador. */
typedef struct
{

  /* General purpose registers (GPRs) */
  PSX_GPRegister gpr[32]; /* ATENCIÓ!! El 0 sempre és 0!. */

  /* A pair of special-purpose registers to hold the results of
   * integer multiply, divide, and multiply-accumulate operations.
   */
  uint32_t hi,lo;

  /* Program counter (PC). */
  uint32_t pc;

  
  /* Registres COP0. */
  
  /* Reg 3 - Breakpoint on execute (R/W) */
  uint32_t cop0r3_bpc;
  
  /* Reg 5 - Breakpoint on data access (R/W) */
  uint32_t cop0r5_bda;
  
  /* Reg 6 - Randomly memorized jump address (R) */
  /*uint32_t cop0r6_jumpdest;*/
  
  /* Reg 7 - Breakpoint control (R/W) */
  uint32_t cop0r7_dcic;

  /* Reg 8 - Bad Virtual Address (R) */
  uint32_t cop0r8_bad_vaddr;

  /* Reg 9 - Data Access breakpoint mask (R/W) */
  uint32_t cop0r9_bdam;

  /* Reg 11 - Execute breakpoint mask (R/W) */
  uint32_t cop0r11_bpcm;

  /* Reg 12 - System status register (R/W) */
  uint32_t cop0r12_sr;

  /* Reg 13 - Describes the most recently recognised exception (R) */
  uint32_t cop0r13_cause;

  /* Reg 14 - Return Address from Trap (R) */
  uint32_t cop0r14_epc;

  
  /* Registre extra mapejat en FFFE0130h que controla la cache. */
  uint32_t cache_control;
  
} PSX_CPU;

/* Estat compartit de la CPU (regitres) per les diferents
 * implementacions.
 */
extern PSX_CPU PSX_cpu_regs;

/* Mnemonics. */
typedef enum
  {
    PSX_UNK= 0,
    PSX_ADD,
    PSX_ADDI,
    PSX_ADDIU,
    PSX_ADDU,
    PSX_AND,
    PSX_ANDI,
    PSX_BEQ,
    PSX_BGEZ,
    PSX_BGEZAL,
    PSX_BGTZ,
    PSX_BLEZ,
    PSX_BLTZ,
    PSX_BLTZAL,
    PSX_BNE,
    PSX_BREAK,
    PSX_CFC2,
    PSX_COP0_RFE,
    PSX_COP0_TLBP,
    PSX_COP0_TLBR,
    PSX_COP0_TLBWI,
    PSX_COP0_TLBWR,
    PSX_COP2_RTPS,
    PSX_COP2_RTPT,
    PSX_COP2_NCLIP,
    PSX_COP2_AVSZ3,
    PSX_COP2_AVSZ4,
    PSX_COP2_MVMVA,
    PSX_COP2_SQR,
    PSX_COP2_OP,
    PSX_COP2_NCS,
    PSX_COP2_NCT,
    PSX_COP2_NCCS,
    PSX_COP2_NCCT,
    PSX_COP2_NCDS,
    PSX_COP2_NCDT,
    PSX_COP2_CC,
    PSX_COP2_CDP,
    PSX_COP2_DCPL,
    PSX_COP2_DPCS,
    PSX_COP2_DPCT,
    PSX_COP2_INTPL,
    PSX_COP2_GPF,
    PSX_COP2_GPL,
    PSX_CTC2,
    PSX_DIV,
    PSX_DIVU,
    PSX_J,
    PSX_JAL,
    PSX_JALR,
    PSX_JR,
    PSX_LB,
    PSX_LBU,
    PSX_LH,
    PSX_LHU,
    PSX_LUI,
    PSX_LW,
    PSX_LWC2,
    PSX_LWL,
    PSX_LWR,
    PSX_MFC0,
    PSX_MFC2,
    PSX_MFHI,
    PSX_MFLO,
    PSX_MTC0,
    PSX_MTC2,
    PSX_MTHI,
    PSX_MTLO,
    PSX_MULT,
    PSX_MULTU,
    PSX_NOR,
    PSX_OR,
    PSX_ORI,
    PSX_SB,
    PSX_SH,
    PSX_SLL,
    PSX_SLLV,
    PSX_SLT,
    PSX_SLTI,
    PSX_SLTIU,
    PSX_SLTU,
    PSX_SRA,
    PSX_SRAV,
    PSX_SRL,
    PSX_SRLV,
    PSX_SUB,
    PSX_SUBU,
    PSX_SW,
    PSX_SWC2,
    PSX_SWL,
    PSX_SWR,
    PSX_SYSCALL,
    PSX_XOR,
    PSX_XORI
  } PSX_Mnemonic;

/* Tipus d'operador. */
typedef enum
  {
    PSX_NONE= 0,
    PSX_RD,
    PSX_RS,
    PSX_RT,
    PSX_IMMEDIATE,
    PSX_OFFSET,
    PSX_ADDR,
    PSX_OFFSET_BASE,
    PSX_SA,
    PSX_COP2_SF,
    PSX_COP2_MX_V_CV,
    PSX_COP2_LM,
    PSX_COP0_REG,
    PSX_COP2_REG,
    PSX_COP2_REG_CTRL
  } PSX_OpType;

/* Estructura per a desar tota la informació relativa a una
 * instrucció.
 */
typedef struct
{
  
  uint32_t     word;    /* Instrucció en memòria. */
  PSX_Mnemonic name;    /* nom. */
  PSX_OpType   op1;
  PSX_OpType   op2;
  PSX_OpType   op3;
  struct {
    int      rd; /* RD, COP0_REG, COP2_REG, COP2_REG_CTRL */
    int      rs; /* RS i BASE */
    int      rt; /* RT */
    uint32_t imm; /* IMMEDIATE i ADDR */
    int32_t  off; /* OFFSET */
    int      sa; /* SA */
    int      cop2_sf; /* COP2_SF */
    bool     cop2_lm_is_0; /* COP2_LM */
    int      cop2_mx; /* COP2_MX_V_CV ;RT/LLM/LCM/Garbage */
    int      cop2_v;  /* COP2_MX_V_CV ;V0, V1, V2, or [IR1,IR2,IR3] */
    int      cop2_cv; /* COP2_MX_V_CV ;TR or BK or Bugged/FC, or None */
  }            extra;
  
} PSX_Inst;

/* Inicialitza els registres. */
void
PSX_cpu_init_regs (void);

/* Descodifica en INST la instrucció de l'adreça indicada. Aquesta
 * funció pot accedir a tot el mapa de memòria, si accedeix a una zona
 * no mapejada o no aliniada torna false.
 */
bool
PSX_cpu_decode (
        	const uint32_t  addr,
        	PSX_Inst       *inst
        	);

/* Interpreta la següent instrucció. Aquest mètode implementa
 * l'intèrpret. Torna els cicles executats.
 */
int
PSX_cpu_next_inst (void);

/* Inícia l'estat de l'intèrpret. Aquest mètode crida a
 * 'PSX_cpu_init_regs' i a 'PSX_cpu_reset'.
 */
void
PSX_cpu_init (
              PSX_Warning *warning,
              void        *udata
              );

// Canvia l'estat d'una senyal d'interrupció. id ha de ser 0..5.
void
PSX_cpu_set_int (
                 const int  id,
                 const bool active
                 );

/* Força l'execució d'una excepció reset en l'intèrpret. */
void
PSX_cpu_reset (void);

/* Si es modifica l'estat dels registres de la CPU des de fora de
 * l'intèrpret cal cridar a aquesta funció per a actualitzar l'estat
 * intern.
 */
void
PSX_cpu_update_state_interpreter (void);


/*******/
/* GTE */
/*******/
/* Mòdul que simula el "Geometry Transformation Engine". */

typedef enum
  {
    PSX_GTE_RTPS,
    PSX_GTE_NCLIP,
    PSX_GTE_OP,
    PSX_GTE_DPCS,
    PSX_GTE_INTPL,
    PSX_GTE_MVMVA,
    PSX_GTE_NCDS,
    PSX_GTE_CDP,
    PSX_GTE_NCDT,
    PSX_GTE_NCCS,
    PSX_GTE_CC,
    PSX_GTE_NCS,
    PSX_GTE_NCT,
    PSX_GTE_SQR,
    PSX_GTE_DCPL,
    PSX_GTE_DPCT,
    PSX_GTE_AVSZ3,
    PSX_GTE_AVSZ4,
    PSX_GTE_RTPT,
    PSX_GTE_GPF,
    PSX_GTE_GPL,
    PSX_GTE_NCCT,
    PSX_GTE_UNK
  } PSX_GTECmd;

typedef void (PSX_GTECmdTrace) (
        			uint32_t  regs_prev[64],
        			uint32_t  regs_after[64],
        			void     *udata
        			);

typedef void (PSX_GTEMemAccess) (
        			 const bool      read, // false
        			 const int       reg,
        			 const uint32_t  val,
        			 const bool      ok,
        			 void           *udata
        			 );

// Processa cicles de la UCP pendents.
void
PSX_gte_end_iter (void);

/* Executa un comandament. Torna número de cicles emprats.
 */
int
PSX_gte_execute (
        	 const uint32_t cmd
        	 );

/* Inicialitza el mòdul. */
void
PSX_gte_init (
              PSX_Warning      *warning,
              PSX_GTECmdTrace  *cmd_trace,
              PSX_GTEMemAccess *mem_access,
              void             *udata
              );

/* Llig un el contingut d'un registre. Torna número de cicles
 * emprats. 64 registres en total, els 32 primers son de dades, els 32
 * següents de control.
 */
int
PSX_gte_read (
              const int  nreg,
              uint32_t  *dst
              );

/* Escriu en un registre. 64 registres en total, els 32 primers son de
 * dades, els 32 següents de control. Aparentment no es bloqueja!
 */
void
PSX_gte_write (
               const int      nreg,
               const uint32_t data
               );

void
PSX_gte_set_mode_trace (
        		const bool enable
        		);


/*******/
/* MEM */
/*******/
/* Mòdul que simula el mapa físic de memòria. */

/* Tipus per a obtindre la configuració de la memòria. */
typedef struct
{
  struct
  {
    uint32_t end_ram;
    uint32_t end_hz;
    bool     locked_00800000;
  } ram;
} PSX_MemMap;

/* Tipus d'accessos a memòria. */
typedef enum
  {
    PSX_READ,
    PSX_WRITE
  } PSX_MemAccessType;

/* Tipus de la funció per a indicar que s'ha modificat la configuració
 * del mapa de memòria.
 */
typedef void (PSX_MemChanged) (void *udata);

/* Tipus de la funció per a fer una traça dels accessos a
 * memòria. Cada vegada que es produeix un accés a memòria física es
 * crida.
 */
typedef void (PSX_MemAccess) (
        		      const PSX_MemAccessType  type, /* Tipus */
        		      const uint32_t           addr, /* Adreça */
        		      const uint32_t           data, /* Dades */
        		      const bool               error,
        		      void                    *udata
        		      );

/* Tipus de la funció per a fer una traça dels accessos a memòria a
 * nivell de paraula. Cada vegada que es produeix un accés a memòria
 * física es crida.
 */
typedef void (PSX_MemAccess16) (
        			const PSX_MemAccessType  type, /* Tipus */
        			const uint32_t           addr, /* Adreça */
        			const uint16_t           data, /* Dades */
        			const bool               error,
        			void                    *udata
        			);

/* Tipus de la funció per a fer una traça dels accessos a memòria a
 * nivell de byte. Cada vegada que es produeix un accés a memòria
 * física es crida.
 */
typedef void (PSX_MemAccess8) (
        		       const PSX_MemAccessType  type, /* Tipus */
        		       const uint32_t           addr, /* Adreça */
        		       const uint8_t            data, /* Dades */
        		       const bool               error,
        		       void                    *udata
        		       );

#define PSX_BIOS_SIZE (512*1024)

void
PSX_mem_init (
              const uint8_t    bios[PSX_BIOS_SIZE],
              PSX_MemChanged  *mem_changed, /* Pot ser NULL */
              PSX_MemAccess   *mem_access, /* Pot ser NULL */
              PSX_MemAccess16 *mem_access16, /* Pot ser NULL */
              PSX_MemAccess8  *mem_access8, /* Pot ser NULL */
              void            *udata
              );

/* Llig una paraula de l'adreça especificada. Torna false en cas
 * d'error de bus. Adreça màxima 1FFFFFFF.
 */
bool
PSX_mem_read (
              const uint32_t  addr,
              uint32_t       *dst
              );

/* Llig mitja paraula de l'adreça especificada. Indica l'endianisme al
 * accedir. Torna false en cas d'error de bus. Adreça màxima 1FFFFFFF.
 */
bool
PSX_mem_read16 (
        	const uint32_t  addr,
        	uint16_t       *dst,
        	const bool      is_le
        	);

/* Llig un byte de l'adreça especificada. Indica l'endianisme al
 * accedir. Torna false en cas d'error de bus. Adreça màxima 1FFFFFFF.
 */
bool
PSX_mem_read8 (
               const uint32_t  addr,
               uint8_t        *dst,
               const bool      is_le
               );

/* Escriu una paraula en l'adreça especificada. Torna false en cas
 * d'error de bus. Adreça màxima 1FFFFFFF.
 */
bool
PSX_mem_write (
               const uint32_t addr,
               const uint32_t data
               );

/* Escriu mitja paraula en l'adreça especificada. Indica l'endianisme
 * al accedir. Torna false en cas d'error de bus. Adreça màxima
 * 1FFFFFFF.
 */
bool
PSX_mem_write16 (
        	 const uint32_t addr,
        	 const uint16_t data,
        	 const bool     is_le
        	 );

/* Escriu mitja paraula en l'adreça especificada. Indica l'endianisme
 * al accedir. Torna false en cas d'error de bus. Adreça màxima
 * 1FFFFFFF. El tema de data16 és perquè en el cas de la SPU el bus en
 * realitat és de 16 i funciona com si fóra de 16.
 */
bool
PSX_mem_write8 (
        	const uint32_t addr,
        	const uint8_t  data,
        	const uint16_t data16,
        	const bool     is_le
        	);

void
PSX_mem_set_mode_trace (
        		const bool val
        		);

void
PSX_mem_get_map (
        	 PSX_MemMap *map
        	 );

/*******/
/* INT */
/*******/
/* Mòdul que implementa la gestió d'interrupcions. */

/* Tipus d'interrupcions que poden ser cridades per altres mòduls. */
typedef enum
  {
    PSX_INT_VBLANK= 0x001,
    PSX_INT_GPU= 0x002,
    PSX_INT_CDROM= 0x004,
    PSX_INT_DMA= 0x008,
    PSX_INT_TMR0= 0x010,
    PSX_INT_TMR1= 0x020,
    PSX_INT_TMR2= 0x040,
    PSX_INT_IRQ7= 0x080, /* Controller and Memory Card - Byte Received
        		    Interrupt */
    PSX_INT_SIO= 0x100,
    PSX_INT_SPU= 0x200,
    PSX_INT_IRQ10= 0x400 /* Controller - Lightpen Interrupt */
  } PSX_Interruption;

/* Tipus de la funció per a fer una traça de les peticions
 * d'interrupció. Cada vegada que es produeix una petició
 * d'interrupció a la UCP o d'ACK per part del subsistema encarregat
 * de gestionar les interrupcions externes es crida.
 */
typedef void (PSX_IntTrace) (
        		     const bool      is_ack, /* ACK/Request*/
        		     const uint32_t  old_i_stat,
        		     const uint32_t  new_i_stat,
        		     const uint32_t  i_mask,
        		     void           *udata
        		     );

/* Inicialitza el mòdul. */
void
PSX_int_init (
              PSX_IntTrace *int_trace,
              void         *udata
              );

// Per a indicar que hem acabat una iteració.
void
PSX_int_end_iter (void);

void
PSX_int_set_mode_trace (
        		const bool val
        		);

/* Fixa el bit d'interrupció en I-STAT. */
void
PSX_int_interruption (
        	      const PSX_Interruption flag,
                      const bool             value
        	      );

/* Llig l'estat de les interrupcions. */
uint32_t
PSX_int_read_state (void);

/* Fa un 'acknowledge' d'una interrupció. */
void
PSX_int_ack (
             const uint32_t data
             );

/* Llig el I-MASK. */
uint32_t
PSX_int_read_imask (void);

/* Escriu el I-MASK. */
void
PSX_int_write_imask (
        	     const uint32_t data
        	     );

/*******/
/* DMA */
/*******/
/* Mòdul que implementa el DMA. */

/* Tipus de la funció per a fer una traça de les transferència de
 * DMA. Cada vegada que es produeix una transferència de DMA es crida.
 */
typedef void (PSX_DMATransfer) (
        			const int       channel,
        			const bool      to_ram, // false indica from_ram
        			const uint32_t  addr,
        			void           *udata
        			);

/* Inicialització. */
void
PSX_dma_init (
              PSX_DMATransfer *dma_transfer,
              PSX_Warning     *warning,
              void            *udata
              );

void
PSX_dma_reset (void);

void
PSX_dma_set_mode_trace (
        		const bool val
        		);

// Executa la següent transferència de dades i torna els cicles emprats.
int
PSX_dma_run (void);

// Com PSX_dma_run, però consumeix els cicles que se li passen en
// compte de generar cicles.
void
PSX_dma_run_cc (
                const int cc
                );

// Processa cicles pendents (sols té sentit quan el choping del mode0
// està activat)
void
PSX_dma_end_iter (void);

// Torna els cicles que queden per al proper event o -1 si no s'espera
// un event.
int
PSX_dma_next_event_cc (void);

/* Per a activar la transferència d'un canal, és emprat per els altres
 * mòduls com a resposta a un sync en el mode 1 o 2.
 */
void
PSX_dma_active_channel (
        		const int chn
        		);

/* Escriu en el registre MADR del canal indicat (0-6). */
void
PSX_dma_madr_write (
        	    const int      chn,
        	    const uint32_t data
        	    );

/* Llig el registre MADR del canal indicat (0-6). */
uint32_t
PSX_dma_madr_read (
        	   const int chn
        	   );

/* Escriu en el registre BCR del canal indicat (0-6). */
void
PSX_dma_bcr_write (
        	   const int      chn,
        	   const uint32_t data
        	   );

/* Llig el registre MADR del canal indicat (0-6). */
uint32_t
PSX_dma_bcr_read (
        	  const int chn
        	  );

/* Escriu en el registre CHCR del canal indicat (0-6). */
void
PSX_dma_chcr_write (
        	    const int      chn,
        	    const uint32_t data
        	    );

/* Llig el registre CHCR del canal indicat (0-6). */
uint32_t
PSX_dma_chcr_read (
        	   const int chn
        	   );

/* Escriu en el registre DPCR. */
void
PSX_dma_dpcr_write (
        	    const uint32_t data
        	    );

/* Escriu el registre DPCR. */
uint32_t
PSX_dma_dpcr_read (void);

/* Escriu en el registre DICR. */
void
PSX_dma_dicr_write (
        	    const uint32_t data
        	    );

/* Escriu el registre DICR. */
uint32_t
PSX_dma_dicr_read (void);

/* Registres desconeguts. */
uint32_t
PSX_dma_unk1_read (void);

uint32_t
PSX_dma_unk2_read (void);


/********/
/* MDEC */
/********/
/* Mòdul que implementa el MDEC. */

void
PSX_mdec_reset (void);

// Inicialització.
void
PSX_mdec_init (
               PSX_Warning *warning,
               void        *udata
               );

uint32_t
PSX_mdec_data_read (void);

void
PSX_mdec_data_write (
        	     const uint32_t data
        	     );

uint32_t
PSX_mdec_status (void);

void
PSX_mdec_control (
        	  const uint32_t data
        	  );

/* Per a DMA0. */
bool
PSX_mdec_in_sync (
        	  const uint32_t nwords_m1
        	  );

/* Per a DMA0. */
void
PSX_mdec_in_write (
        	   uint32_t data
        	   );

/* Per a DMA0. */
uint32_t
PSX_mdec_in_read (void);

/* Per a DMA1. */
bool
PSX_mdec_out_sync (
        	   const uint32_t nwords_m1
        	   );

/* Per a DMA1. */
void
PSX_mdec_out_write (
        	    uint32_t data
        	    );

/* Per a DMA1. */
uint32_t
PSX_mdec_out_read (void);

// Processa cicles pendents.
void
PSX_mdec_end_iter(void);

// Torna els cicles que queden per al proper event. Mai torna -1.
int
PSX_mdec_next_event_cc (void);

/************/
/* RENDERER */
/************/
/* Renderitzador. */

typedef struct
{

  int     x,y; /* Coordenades. */
  uint8_t r,g,b; /* Color per a gouraud. */
  uint8_t u,v; /* Coordenades textura. */
  
} PSX_VertexInfo;

typedef struct
{

  PSX_VertexInfo v[4];
  int            clip_x1,clip_x2; /* 0..1023. */
  int            clip_y1,clip_y2; /* 0..511. */
  uint8_t        r,g,b; /* Color per no gouraud. */
  enum {
    PSX_TR_MODE0= 0, // Fins a 3 Coincideix amb el valor de la GPU.
    PSX_TR_MODE1= 1,
    PSX_TR_MODE2= 2,
    PSX_TR_MODE3= 3,
    PSX_TR_NONE= 4
  }              transparency;
  bool           dithering;
  bool           gouraud;
  enum {
    PSX_TEX_4b= 0,
    PSX_TEX_8b= 1,
    PSX_TEX_15b= 2,
    PSX_TEX_NONE= 3 /* Gaste el reserved com a NONE!!! */
  }              texture_mode;
  int            texpage_x; /* 0..15 (x64) */
  int            texpage_y; /* 0..1 (x256) */
  int            texclut_x; /* 0..63 (x16) */
  int            texclut_y; /* 0..511 */
  bool           modulate_texture;
  /* Finciona així!!
   *  u= u&texwinmask_x | texwinoff_x
   *  v= v&texwinmask_y | texwinoff_y
   */
  uint8_t        texwinmask_x,texwinmask_y;
  uint8_t        texwinoff_x,texwinoff_y;
  bool           texflip_x,texflip_y; /* Sols per a rectangles. */
  bool           set_mask; /* Fixa el bit15 a 1. */
  bool           check_mask; /* No permet sobreescriure un píxel amb
        			el bit15 a 1. */
  
} PSX_RendererArgs;

// Torna estadístics del rendering per a poder calcular temps.
typedef struct
{
  int npixels;
  int nlines; // Important per a pol3, pol4
} PSX_RendererStats;

/* Paràmetres per a dibuixar un frame. */
typedef struct
{

  int  x,y; /* Coordenades en el fb. */
  int  width;
  int  height;
  bool is15bit;
  double d_x0,d_x1; /* Columnes visibles. Per a una tele 4:3, valors
        	       normalitzats [0,1]. Pot ser negatiu. */
  double d_y0,d_y1; /* Línies visibles. Per a una tele 4:3, valors
        	       normalitzats [0,1]. Pot ser negatiu. */
  
} PSX_FrameGeometry;

/* NOTES:
 *
 *  - Les dimensions del framebuffer són 1024x512.
 *  - Cada píxel és un uint16_t.
 *  - Les coordenades del vertex poden superar l'àrea del dibuix. En
 *    qualsevol cas hi ha una clip àrea.
 */
#define PSX_RENDERER_CLASS        					\
  /* Free. */        							\
  void (*free) (struct PSX_Renderer_ *);        			\
  /* Habilita/Deshabilta la pantalla. Quan està descativada la        	\
     pantalla és negra. */        					\
  void (*enable_display) (struct PSX_Renderer_ *,        		\
        		  const bool enable);				\
  /* Renderer -> fb. */        						\
  void (*lock) (struct PSX_Renderer_ *,uint16_t *fb);        		\
  /* fb -> Renderer. */        						\
  void (*unlock) (struct PSX_Renderer_ *,uint16_t *fb);        		\
  /* Dibuixa un frame a partir del fb. S'assumeix        		\
     que les coordenades estan dins del fb. */        			\
  void (*draw) (struct PSX_Renderer_ *,        				\
        	const PSX_FrameGeometry *g);				\
  /* Three-point polygon (triangle). */        				\
  void (*pol3) (struct PSX_Renderer_ *,        				\
        	PSX_RendererArgs  *args,				\
        	PSX_RendererStats *stats);				\
  /* Four-point polygon. */        					\
  void (*pol4) (struct PSX_Renderer_ *,        				\
        	PSX_RendererArgs  *args,				\
        	PSX_RendererStats *stats);				\
  /* Rectangle. Origen en v[0]. */        				\
  void (*rect) (struct PSX_Renderer_ *,        				\
        	PSX_RendererArgs  *args,				\
        	const int          width,				\
        	const int          height,				\
        	PSX_RendererStats *stats);				\
  /* Line. Sols torna npixels en stats. */        			\
  void (*line) (struct PSX_Renderer_ *,        				\
        	PSX_RendererArgs  *args,				\
        	PSX_RendererStats *stats);

typedef struct PSX_Renderer_ PSX_Renderer;

struct PSX_Renderer_
{
  PSX_RENDERER_CLASS;
};

#define PSX_RENDERER(ptr) ((PSX_Renderer *) (ptr))

#define PSX_renderer_free(ptr)        		\
  PSX_RENDERER(ptr)->free ( PSX_RENDERER(ptr) )

typedef struct
{

  int    width;
  int    height;
  double x0,x1; // Columnes visibles. Per a una tele 4:3, valors
                // normalitzats [0,1]. Pot ser negatiu o major que 1.
  double y0,y1; // Línies visibles. Per a una tele 4:3, valors
                // normalitzats [0,1]. Pot ser negatiu o major que 1.
  
} PSX_UpdateScreenGeometry;

/* Tipus de la funció utilitzat per el renderitzador per defecte per a
 * actualitzr la pantalla real. Cada píxel és un valor de 32 bits RGBA
 * (en eixe ordre quan s'interpreta com uint8_t).
 */
typedef void (PSX_UpdateScreen) (
        			 const uint32_t                 *fb,
        			 const PSX_UpdateScreenGeometry *g,
        			 void                           *udata
        			 );


/**********/
/* TIMERS */
/**********/
/* Comptadors. */

/*
// Processa cicles d'UCP. S'ha d'indicar si estem o no en el final de
// la iteració.
void
PSX_timers_clock (
        	  const bool end_iter
        	  );
*/

// Processa cicles pendents i possibles events.
void
PSX_timers_end_iter (void);

// Torna els cicles que queden per al proper event. Mai torna -1.
int
PSX_timers_next_event_cc (void);

/* Inicialitza el mòdul. */
void
PSX_timers_init (void);

/* Emprat per la GPU per a indicar que comença el HBlank. */
void
PSX_timers_hblank_in (void);

/* Emprat per la GPU per a indicar que s'ha acabat el HBlank. */
void
PSX_timers_hblank_out (void);

/* Emprat per la GPU per a indicar que comença el VBlank. */
void
PSX_timers_vblank_in (void);

/* Emprat per la GPU per a indicar que s'ha acabat el VBlank. */
void
PSX_timers_vblank_out (void);

/* Set timer current counter value. */
void
PSX_timers_set_counter_value (
        		      const uint32_t data,
        		      const int      timer
        		      );

/* Get timer current counter value. */
uint32_t
PSX_timers_get_counter_value (
        		      const int timer
        		      );

/* Set timer counter mode. */
void
PSX_timers_set_counter_mode (
        		     const uint32_t data,
        		     const int      timer
        		     );

/* Get timer counter mode. */
uint32_t
PSX_timers_get_counter_mode (
        		     const int timer
        		     );

/* Set timer counter target value. */
void
PSX_timers_set_target_value (
        		     const uint32_t data,
        		     const int      timer
        		     );

/* Get timer counter target value. */
uint32_t
PSX_timers_get_target_value (
        		     const int timer
        		     );

/* Es cridada per la GPU cada vegada que canvia la resolució
 * horitzontal per a indicar quants cicles de GPU és consumeixen per
 * cada píxel.
 */
void
PSX_timers_set_dot_gpucc (
        		  const int gpucc
        		  );


/*******/
/* GPU */
/*******/
/* Xip gràfic. */

/* Mnemonics GPU. */
typedef enum
  {
    PSX_GP0_POL3,
    PSX_GP0_POL4,
    PSX_GP0_LINE,
    PSX_GP0_POLYLINE,
    PSX_GP0_POLYLINE_CONT,
    PSX_GP0_RECT,
    PSX_GP0_SET_DRAW_MODE,
    PSX_GP0_SET_TEXT_WIN,
    PSX_GP0_SET_TOP_LEFT,
    PSX_GP0_SET_BOTTOM_RIGHT,
    PSX_GP0_SET_OFFSET,
    PSX_GP0_SET_MASK_BIT,
    PSX_GP0_CLEAR_CACHE,
    PSX_GP0_FILL,
    PSX_GP0_COPY_VRAM2VRAM,
    PSX_GP0_COPY_CPU2VRAM,
    PSX_GP0_COPY_VRAM2CPU,
    PSX_GP0_IRQ1,
    PSX_GP0_NOP,
    PSX_GP0_UNK,

    PSX_GP1_RESET,
    PSX_GP1_RESET_BUFFER,
    PSX_GP1_ACK,
    PSX_GP1_ENABLE,
    PSX_GP1_DATA_REQUEST,
    PSX_GP1_START_DISP,
    PSX_GP1_HOR_DISP_RANGE,
    PSX_GP1_VER_DISP_RANGE,
    PSX_GP1_SET_DISP_MODE,
    PSX_GP1_TEXT_DISABLE,
    PSX_GP1_GET_INFO,
    PSX_GP1_OLD_TEXT_DISABLE,
    PSX_GP1_UNK
  } PSX_GPUMnemonic;

// 'Flags' per a saber si té o no una operació.
#define PSX_GP_COLOR 0x01
#define PSX_GP_TRANSPARENCY 0x02
#define PSX_GP_TEXT_BLEND 0x04
#define PSX_GP_V_COLOR 0x08
#define PSX_GP_RAW_TEXT 0x10

/* Estructura per a desar tota la informació relativa a un comandament
 * de GPU.
 */
typedef struct
{

  uint32_t        word; // Inclou el (v0) color: CCBBGGRR
  PSX_GPUMnemonic name;
  long            ops;
  int             width,height;
  int             Nv;
  struct
  {
    int     x,y; // No inclou l'offset.
    uint8_t u,v; // Per a les textures.
    uint8_t r,g,b;
  }               v[4];
  int             texclut_x,texclut_y;
  int             texpage_x,texpage_y;
  int             tex_pol_transparency; // Sols si està activada i per a pol
  int             tex_pol_mode; // Sols per a pol
  
} PSX_GPUCmd;

/* Tipus de funció per a saber quin a sigut l'últim comandament de GPU
 * executat.
 */
typedef void (PSX_GPUCmdTrace) (
        			const PSX_GPUCmd *cmd, // Comandament.
        			void             *udata
        			);

void
PSX_gpu_reset (void);
  
// Processa cicles pendents i possibles events.
void
PSX_gpu_end_iter (void);

// Torna els cicles que queden per al proper event o -1 si no s'espera
// un event.
int
PSX_gpu_next_event_cc (void);

/* Inicialització. */
void
PSX_gpu_init (
              PSX_Renderer    *renderer,
              PSX_GPUCmdTrace *gpu_cmd,
              PSX_Warning     *warning,
              void            *udata
              );

/* Comandaments GP0 (Renderitzat i VRAM). */
void
PSX_gpu_gp0 (
             const uint32_t cmd
             );

/* Comandaments GP1 (Display control). */
void
PSX_gpu_gp1 (
             const uint32_t cmd
             );

/* Llig respostes a comandaments de GP0 i GP1. */
uint32_t
PSX_gpu_read (void);

/* Registre d'estat. */
uint32_t
PSX_gpu_stat (void);

/* Per a DMA2. */
bool
PSX_gpu_dma_sync (
                  const uint32_t nwords
                  );

/* Per a DMA2. */
void
PSX_gpu_dma_write (
                   uint32_t data
                   );

/* Per a DMA2. */
uint32_t
PSX_gpu_dma_read (void);

/* Utilitzat per timers per a indicar a la GPU que té que avisar de la
 * entrada o eixida en el HBlank.
 */
void
PSX_gpu_signal_hblank (
        	       const bool enable
        	       );

const uint16_t *
PSX_gpu_get_frame_buffer (void);

void
PSX_gpu_set_mode_trace (
        		const bool val
        		);


/******/
/* CD */
/******/
// CD-Rom

// Mnemonics CD
typedef enum
  {
    PSX_CD_SYNC,
    PSX_CD_SET_MODE,
    PSX_CD_INIT,
    PSX_CD_RESET,
    PSX_CD_MOTOR_ON,
    PSX_CD_STOP,
    PSX_CD_PAUSE,
    PSX_CD_SETLOC,
    PSX_CD_SEEKL,
    PSX_CD_SEEKP,
    PSX_CD_SET_SESSION,
    PSX_CD_READN,
    PSX_CD_READS,
    PSX_CD_READ_TOC,
    PSX_CD_GET_STAT,
    PSX_CD_GET_PARAM,
    PSX_CD_GET_LOC_L,
    PSX_CD_GET_LOC_P,
    PSX_CD_GET_TN,
    PSX_CD_GET_TD,
    PSX_CD_GET_Q,
    PSX_CD_GET_ID,
    PSX_CD_TEST,
    PSX_CD_MUTE,
    PSX_CD_DEMUTE,
    PSX_CD_PLAY,
    PSX_CD_FORWARD,
    PSX_CD_BACKWARD,
    PSX_CD_SET_FILTER,
    PSX_CD_UNK
  } PSX_CDMnemonic;

// Estructura per a desar tota la informació relativa a un comandament
// del CD-ROM.
typedef struct
{

  uint8_t        cmd;
  PSX_CDMnemonic name;
  struct {
    uint8_t v[16];
    int     N;
  }              args;
} PSX_CDCmd;

// Tipus de funció per a saber quin a sigut l'últim comandament de CD
// executat.
typedef void (PSX_CDCmdTrace) (
        		       const PSX_CDCmd *cmd, // Comandament.
        		       void            *udata
        		       );

void
PSX_cd_reset (void);

// Processa cicles pendents i possibles events. ¿¿¿Pel tema de l'àudio
// cal executar açò abans de la SPU.???
void
PSX_cd_end_iter (void);

// Torna els cicles que queden per al proper event. Mai torna -1.
int
PSX_cd_next_event_cc (void);

// Inicialització.
void
PSX_cd_init (
             PSX_CDCmdTrace *cd_cmd,
             PSX_Warning    *warning,
             void           *udata
             );

// Fixa l'índex del registre a emprar.
void
PSX_cd_set_index (
        	  const uint8_t data
        	  );

// Torna l'estat.
uint8_t
PSX_cd_status (void);

// Escriu en el port1.
void
PSX_cd_port1_write (
        	    const uint8_t data
        	    );

// Escriu en el port2.
void
PSX_cd_port2_write (
        	    const uint8_t data
        	    );

// Escriu en el port3.
void
PSX_cd_port3_write (
        	    const uint8_t data
        	    );

// Llig del port 1.
uint8_t
PSX_cd_port1_read (void);

// Llig del port 2.
uint8_t
PSX_cd_port2_read (void);

// Llig del port 3.
uint8_t
PSX_cd_port3_read (void);

void
PSX_cd_set_mode_trace (
        	       const bool val
        	       );

// Per a DMA3
bool
PSX_cd_dma_sync (
        	 const uint32_t nwords
        	 );

// Per a DMA3
void
PSX_cd_dma_write (
        	  uint32_t data
        	  );

// Per a DMA3.
uint32_t
PSX_cd_dma_read (void);

void
PSX_cd_next_sound_sample (
        		  int16_t *l,
        		  int16_t *r
        		  );


/*******/
/* SPU */
/*******/
// Xip de so.

// Poc més de mijta centèssima de segon
#define PSX_AUDIO_BUFFER_SIZE 256

/* Tipus de la funció que actualitza es crida per a reproduir so. Es
 * proporcionen dos canals intercalats (esquerra/dreta). Cada mostra
 * està codificada com un valor de 16 bits amb signe amb una
 * freqüència de 44100Hz.
 */
typedef void (PSX_PlaySound) (
        		      const int16_t  samples[PSX_AUDIO_BUFFER_SIZE*2],
        		      void          *udata
        		      );

void
PSX_spu_reset (void);

// Processa cicles pendents i possibles events. ¿¿¿Pel tema de l'àudio
// cal executar açò desprès del CD.???
void
PSX_spu_end_iter (void);

// Torna els cicles que queden per al proper event. Mai torna -1.
int
PSX_spu_next_event_cc (void);

// Inicialització.
void
PSX_spu_init (
              PSX_PlaySound *play_sound,
              PSX_Warning   *warning,
              void          *udata
              );

// Torna l'adreça inicial d'una veu.
uint16_t
PSX_spu_voice_get_start_addr (
        		      const int voice
        		      );

// Fixa l'adreça inicial per a una veu.
void
PSX_spu_voice_set_start_addr (
        		      const int      voice,
        		      const uint16_t val
        		      );

// Torna l'adreça de repetició d'una veu.
uint16_t
PSX_spu_voice_get_repeat_addr (
        		       const int voice
        		       );

// Fixa l'adreça de repetició per a una veu.
void
PSX_spu_voice_set_repeat_addr (
        		       const int      voice,
        		       const uint16_t val
        		       );

// Torna el 'rate' d'una veu.
uint16_t
PSX_spu_voice_get_sample_rate (
        		       const int voice
        		       );

// Fixa el 'rate' per a una veu.
void
PSX_spu_voice_set_sample_rate (
        		       const int      voice,
        		       const uint16_t val
        		       );

// Fixa els 'flags' de Pitch Modulation.
void
PSX_spu_set_pmon_lo (
        	     const uint16_t data
        	     );

void
PSX_spu_set_pmon_up (
        	     const uint16_t data
        	     );

// Retorna el contingut actual del registre.
uint32_t
PSX_spu_get_pmon (void);

// Configura el ADSR per a una veu (part baixa).
void
PSX_spu_voice_set_adsr_lo (
        		   const int      voice,
        		   const uint16_t val
        		   );

// Configura el ADSR per a una veu (part alta).
void
PSX_spu_voice_set_adsr_up (
        		   const int      voice,
        		   const uint16_t val
        		   );

// Torna el valor del registre ADSR per a una veu.
uint32_t
PSX_spu_voice_get_adsr (
        		const int voice
        		);

// Activa veus.
void
PSX_spu_key_on_lo (
        	   const uint16_t data
        	   );

void
PSX_spu_key_on_up (
        	   const uint16_t data
        	   );

// Desactiva veus.
void
PSX_spu_key_off_lo (
        	    const uint16_t data
        	    );

void
PSX_spu_key_off_up (
        	    const uint16_t data
        	    );

uint32_t
PSX_spu_get_kon (void);

uint32_t
PSX_spu_get_koff (void);

// Fixa ENDX (Stat ON/OFF de les veus)
void
PSX_spu_set_endx_lo (
                     const uint16_t data
                     );

void
PSX_spu_set_endx_up (
                     const uint16_t data
                     );

// Consulta ENDX (Stat ON/OFF de les veus)
uint32_t
PSX_spu_get_endx (void);

// Fixa el mode Noise/ADPCM
void
PSX_spu_set_non_lo (
        	    const uint16_t data
        	    );

void
PSX_spu_set_non_up (
        	    const uint16_t data
        	    );

uint32_t
PSX_spu_get_non (void);

// Fixa el volum ADSR actual (En realitat és un poc trellat, segons
// NOCASH al següent step la SPU el sobreescriu)
void
PSX_spu_voice_set_cur_vol (
        		   const int      voice,
        		   const uint16_t val
        		   );

// Torna el volum actual ADSR per a una veu.
uint16_t
PSX_spu_voice_get_cur_vol (
        		   const int voice
        		   );

// Fixa el volum esquerre per a una veu.
void
PSX_spu_voice_set_left_vol (
        		    const int      voice,
        		    const uint16_t val
        		    );

// Fixa el volum dret per a una veu.
void
PSX_spu_voice_set_right_vol (
        		     const int      voice,
        		     const uint16_t val
        		     );

// Torna el registre del volum esquerre per a una veu.
uint16_t
PSX_spu_voice_get_left_vol (
        		    const int voice
        		    );

// Torna el registre del volum dret per a una veu.
uint16_t
PSX_spu_voice_get_right_vol (
        		     const int voice
        		     );

// Torna el valor actual del volum esquerra/dreta.
uint32_t
PSX_spu_voice_get_cur_vol_lr (
        		      const int voice
        		      );

// Fixa volum principal esquerre.
void
PSX_spu_set_left_vol (
        	      const uint16_t data
        	      );

// Fixa volum principal dret.
void
PSX_spu_set_right_vol (
        	       const uint16_t data
        	       );

// Torna volum principal esquerre.
uint16_t
PSX_spu_get_left_vol (void);

// Torna volum principal dret.
uint16_t
PSX_spu_get_right_vol (void);

// Torna el valor actual del volum principal esquerra/dreta.
uint32_t
PSX_spu_get_cur_vol_lr (void);

// Fixa el volum per al CD.
void
PSX_spu_set_cd_vol_l (
        	      const uint16_t data
        	      );

void
PSX_spu_set_cd_vol_r (
        	      const uint16_t data
        	      );

// Fixa el volum per al Ext.
void
PSX_spu_set_ext_vol_l (
        	       const uint16_t data
        	       );

void
PSX_spu_set_ext_vol_r (
        	       const uint16_t data
        	       );

// Torna el volum per al CD.
uint32_t
PSX_spu_get_cd_vol (void);

// Torna el volum per al Ext.
uint32_t
PSX_spu_get_ext_vol (void);

// Configuració general de la SPU (SPUCNT).
void
PSX_spu_set_control (
        	     const uint16_t data
        	     );

uint16_t
PSX_spu_get_control (void);

// Fixa l'adreça de transferència.
void
PSX_spu_set_addr (
        	  const uint16_t data
        	  );

uint16_t
PSX_spu_get_addr (void);

void
PSX_spu_write (
               const uint16_t data
               );

// Fixa el tipus de transferència FIFO<->SPU_RAM
void
PSX_spu_set_transfer_type (
        		   const uint16_t data
        		   );

uint16_t
PSX_spu_get_transfer_type (void);

// Per a DMA4.
bool
PSX_spu_dma_sync (
                  const uint32_t nwords
                  );

// Per a DMA4.
void
PSX_spu_dma_write (
                   uint32_t data
                   );

// Per a DMA4.
uint32_t
PSX_spu_dma_read (void);

// Fixa el volum d'eixida esquerre reverb.
void
PSX_spu_reverb_set_vlout (
        		  const uint16_t data
        		  );

// Fixa el volum d'eixida dret reverb.
void
PSX_spu_reverb_set_vrout (
        		  const uint16_t data
        		  );

// Fixa l'adreça mBase per a reverb.
void
PSX_spu_reverb_set_mbase (
        		  const uint16_t data
        		  );

uint16_t
PSX_spu_reverb_get_vlout (void);

uint16_t
PSX_spu_reverb_get_vrout (void);

uint16_t
PSX_spu_reverb_get_mbase (void);

// Fixa valor registre reverberació.
void
PSX_spu_reverb_set_reg (
        		const int      reg,
        		const uint16_t data
        		);

uint16_t
PSX_spu_reverb_get_reg (
        		const int reg
        		);

// Habilita echo (reverb) en els canals.
void
PSX_spu_set_eon_lo (
        	    const uint16_t data
        	    );

void
PSX_spu_set_eon_up (
        	    const uint16_t data
        	    );

uint32_t
PSX_spu_get_eon (void);

// Fixa l'adreça de memòria que al llegir/esciure provoca
// interrupcions.
void
PSX_spu_set_irq_addr (
        	      const uint16_t data
        	      );

uint16_t
PSX_spu_get_irq_addr (void);

// Llig l'estat de la SPU (SPUSTAT).
uint16_t
PSX_spu_get_status (void);

void
PSX_spu_set_unk_da0 (
        	     const uint16_t data
        	     );

uint16_t
PSX_spu_get_unk_da0 (void);

void
PSX_spu_set_unk_dbc (
        	     const int      ind,
        	     const uint16_t data
        	     );

uint16_t
PSX_spu_get_unk_dbc (
        	     const int ind
        	     );

void
PSX_spu_set_unk_e60 (
        	     const int      reg,
        	     const uint16_t data
        	     );

uint16_t
PSX_spu_get_unk_e60 (
        	     const int reg
        	     );


/**************************/
/* JOYSTICKS/MEMORY CARDS */
/**************************/

typedef enum
  {
    PSX_CONTROLLER_STANDARD,
    PSX_CONTROLLER_NONE
  } PSX_Controller;

typedef enum
  {
    PSX_BUTTON_SELECT=   0x0001,
    PSX_BUTTON_START=    0x0008,
    PSX_BUTTON_UP=       0x0010,
    PSX_BUTTON_RIGHT=    0x0020,
    PSX_BUTTON_DOWN=     0x0040,
    PSX_BUTTON_LEFT=     0x0080,
    PSX_BUTTON_L2=       0x0100,
    PSX_BUTTON_R2=       0x0200,
    PSX_BUTTON_L1=       0x0400,
    PSX_BUTTON_R1=       0x0800,
    PSX_BUTTON_TRIANGLE= 0x1000,
    PSX_BUTTON_CIRCLE=   0x2000,
    PSX_BUTTON_CROSS=    0x4000,
    PSX_BUTTON_SQUARE=   0x8000
  } PSX_Button;

typedef struct
{
  
  uint16_t buttons; // Mascara de botons activats, veure PSX_Button
  
} PSX_ControllerState;

typedef const PSX_ControllerState * (PSX_GetControllerState) (
        						      const int  joy,
        						      void      *udata
        						      );

void
PSX_joy_init (
              PSX_Warning            *warning,
              PSX_GetControllerState *get_controller_state,
              void                   *udata
              );

void
PSX_joy_tx_data (
        	 const uint32_t data
        	 );

uint32_t
PSX_joy_rx_data (void);

uint32_t
PSX_joy_stat (void);

void
PSX_joy_mode_write (
        	    const uint16_t data
        	    );

uint16_t
PSX_joy_mode_read (void);

void
PSX_joy_ctrl_write (
        	    const uint16_t data
        	    );

uint16_t
PSX_joy_ctrl_read (void);

void
PSX_joy_baud_write (
        	    const uint16_t data
        	    );

uint16_t
PSX_joy_baud_read (void);

// Processa cicles pendents i possibles events.
void
PSX_joy_end_iter (void);

// Torna els cicles que queden per al proper event. Mai torna -1.
int
PSX_joy_next_event_cc (void);


/********/
/* MAIN */
/********/
/* Funcions principals del simulador. */

/* Número de cicles per segon. */
#define PSX_CYCLES_PER_SEC 33868800

// Número de cicles per instrucció
#define PSX_CYCLES_INST 2

// Clocks que es porten executats en l'actual iteració. Pot anar
// canviant durant la iteració.
extern int PSX_Clock;

// Cicles interns fins al següent event.
extern int PSX_NextEventCC;

typedef enum {
      PSX_BUS_OWNER_CPU= 0,
      PSX_BUS_OWNER_DMA,
      PSX_BUS_OWNER_CPU_DMA
} PSX_BusOwnerType;

extern PSX_BusOwnerType PSX_BusOwner;

/* Tipus de funció amb la que el 'frontend' indica a la llibreria si
 * s'ha produït alguna senyal. A més esta funció pot ser emprada per
 * el frontend per a tractar els events pendents.
 */
typedef void (PSX_CheckSignals) (
        			 bool *stop,
        			 bool *reset,
        			 void *udata
        			 );

/* Tipus de funció per a saber quin a sigut l'últim pas d'execució de
 * la UCP.
 */
typedef void (PSX_CPUInst) (
        		    const PSX_Inst *inst, /* Punter a pas
        					     d'execuió. */
        		    const uint32_t  addr, /* Adreça de
        					     memòria. */
        		    void           *udata
        		    );

/* Els camps poden ser distint de NULL. */
typedef struct
{

  PSX_MemChanged  *mem_changed;       /* Es crida cada vegada que es
        				 modifica el mapa de la
        				 memòria física. */
  PSX_MemAccess   *mem_access;        /* Es crida cada vegada que es
                                         produeïx un accés a
                                         memòria física. */
  PSX_MemAccess16 *mem_access16;       /* Es crida cada vegada que es
        				  produeïx un accés a memòria
        				  física a nivell de
        				  paraula. */
  PSX_MemAccess8  *mem_access8;       /* Es crida cada vegada que es
        				 produeïx un accés a memòria a
        				 nivell de byte. */
  PSX_CPUInst     *cpu_inst; /* Es crida en cada pas de la UCP. */
  PSX_GPUCmdTrace *gpu_cmd; // Es crida cada vegada que s'executa un
        		    // commandament de GPU.
  PSX_CDCmdTrace  *cd_cmd; // Es crida cada vegada que s'executa un
        		   // comandament de CD.
  PSX_IntTrace    *int_trace; // Es crida cada vegada que es fa una
        		      // petició d'interrupció a la CPU per part
        		      // del mòdul INT, o un ACK.
  PSX_DMATransfer *dma_transfer; // Es crida cada vegada que es
        			 // produeix una transferència per
        			 // DMA.
  PSX_GTECmdTrace *gte_cmd_trace; // Es crida cada vegada que
        			  // s'executa un comandament GTE.
  PSX_GTEMemAccess *gte_mem_access; // Es crida cada vegada que
        			    // s'escriu/llig un registre del
        			    // GTE.
  
} PSX_TraceCallbacks;

/* Conté informació necessària per a comunicar-se amb el
 * 'frontend'. El rendereri altres plugins van a banda.
 */
typedef struct
{
  
  PSX_Warning              *warning;  /* Funció per a mostrar avisos. */
  PSX_CheckSignals         *check;    /* Comprova si ha de parar i
        				 events externs. Pot ser NULL,
        				 en eixe cas el simulador
        				 s'executarà fins que es cride
        				 a 'PSX_stop'. */
  PSX_PlaySound            *play_sound; // Es crida per a reproduir el so.
  PSX_GetControllerState   *get_ctrl_state; // Es crida per consultar
        				    // l'estat del
        				    // controlador.
  const PSX_TraceCallbacks *trace;    /* Pot ser NULL si no es van a
        				 gastar les funcions per a fer
        				 una traça. */
  
} PSX_Frontend;

/* Crea el renderer per defecte implementat en C. */
PSX_Renderer *
PSX_create_default_renderer (
        		     PSX_UpdateScreen *update_screen,
        		     void             *udata
        		     );

/* Renderització que no renderitza, sols proporciona estimacions dels
 * píxels que es van a dibuixar, malauradament no és tan precissa com
 * la del default_renderer. La diferència radica en que aquest
 * renderer també compta els píxels que finalment no es dibuixen per
 * temes de màscares, color transparent o el que siga.
 */
PSX_Renderer *
PSX_create_stats_renderer (void);

/* Inicialitza la llibreria. */
void
PSX_init (
          const uint8_t       bios[PSX_BIOS_SIZE],
          const PSX_Frontend *frontend,       /* Frontend. */
          void               *udata,          /* Dades frontend. */
          PSX_Renderer       *renderer
          );

// Modifica la bios.
void
PSX_change_bios (
                 const uint8_t bios[PSX_BIOS_SIZE]
                 );

/* Executa cicles de la PlayStation. Aquesta funció executa almenys
 * els cicles indicats (probablement n'executarà uns pocs més). Si
 * CHECKSIGNALS en el fronted és no NULL aleshores al final de cada
 * execució crida a CHECKSIGNALS.  La senyal stop de CHECKSIGNALS és
 * llegit en STOP si es crida a CHECKSIGNALS. Torna el número de
 * cicles executats.
 */
int
PSX_iter (
          const int  cc,
          bool      *stop
          );

/* Fa un reset quan no hi ha checksignals. */
void
PSX_reset (void);

/* Executa els següent pas de UCP en mode traça. Tots aquelles
 * funcions de 'callback' que no són nul·les es cridaran si és el
 * cas. Torna el clocks de rellotge executats en l'últim pas. Torna en
 * 'pc' el comptador de programa abans d'executar la instrucció.
 */
int
PSX_trace (void);

// Aquesta funció emula que s'obri el CD-Rom i s'inserta (o es lleva
// si és NULL) un nou disc. S'emula perquè tarde 3 segons. Si a meitat
// es torna a cridar es reinicia el procés. Torna el disc anterior
// (podria ser NULL), no l'esborra.
CD_Disc *
PSX_set_disc (
              CD_Disc *disc // Pot ser NULL
              );

void
PSX_plug_controllers (
        	      const PSX_Controller ctrl1,
        	      const PSX_Controller ctrl2
        	      );

// Cada memory card és un punter a un array de 128KB. NULL desconecta
// el memory card. Si per a una memory card el punter és igual al ja
// conectat aquesta funció no te cap efecte per a eixa memory card.
void
PSX_plug_mem_cards (
        	    uint8_t *memc1,
        	    uint8_t *memc2
        	    );

#endif /* __PSX_H__*/

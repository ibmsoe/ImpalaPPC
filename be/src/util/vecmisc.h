/******************************************************************************/
/*                                                                            */
/* Licensed Materials - Property of IBM                                       */
/*                                                                            */
/* IBM Power Vector Intrinisic Functions version 1.0.2                        */
/*                                                                            */
/* Copyright IBM Corp. 2014,2015                                              */
/* US Government Users Restricted Rights - Use, duplication or                */
/* disclosure restricted by GSA ADP Schedule Contract with IBM Corp.          */
/*                                                                            */
/* See the licence in the license subdirectory.                               */
/*                                                                            */
/* More information on this software is available on the IBM DeveloperWorks   */
/* website at                                                                 */
/*  https://www.ibm.com/developerworks/community/groups/community/powerveclib */
/*                                                                            */
/******************************************************************************/

#ifndef _H_VECMISC
#define _H_VECMISC

#include <stdio.h>
#include <altivec.h>
#include "veclib_types.h"


/************************************************************** Floating-Point Control and Status Register *************************/

/* Get exception mask bits from MXCSR register */
VECLIB_INLINE unsigned int vec_getfpexceptionmask (void);

/* Get exception state bits from MXCSR register */
VECLIB_INLINE unsigned int vec_getfpexceptionstate (void);

/* Get flush zero bits from MXCSR register */
VECLIB_INLINE unsigned int vec_getfpflushtozeromode (void);

/* Get rounding mode bits from MXCSR register */
VECLIB_INLINE unsigned int vec_getfproundingmode (void);

/* Get all bits from MXCSR register */
VECLIB_INLINE unsigned int vec_getfpallbits (void);

/* Set exception mask bits in MXCSR register */
VECLIB_INLINE void vec_setfpexceptionmask (unsigned int mask);

/* Set exception state bits in MXCSR register */
VECLIB_INLINE void vec_setfpexceptionstate (unsigned int mask); 

/* Set flush zero bits in MXCSR register */
VECLIB_INLINE void vec_setfpflushtozeromode (unsigned int mask); 

/* Set rounding mode bits in MXCSR register */
VECLIB_INLINE void vec_setfproundingmode (unsigned int mask);

/* Set all bits in MXCSR register */
VECLIB_INLINE void vec_setfpallbits (unsigned int mask);

/************************************************************** Miscellaneous ******************************************************/
#define _MM_HINT_T0 0
#define _MM_HINT_T1 0
#define _MM_HINT_T2 0
#define _MM_HINT_NTA 0

/* vec_prefetch hint */
typedef enum vec_prefetch_hint
{
  vec_HINT_NTA = 0,
  vec_HINT_T2  = 1,
  vec_HINT_T1  = 2,
  vec_HINT_T0  = 3
} vec_prefetch_hint;

/* Prefetch cache line with hint */
VECLIB_INLINE void vec_prefetch (void const* address, vec_prefetch_hint hint);

/* Zero upper half of all 8 or 16 YMMM registers */
void vec_zeroallupper (void);  

/* Serialize previous stores before following stores */
VECLIB_INLINE void vec_fence (void);

/********************************************************* Malloc/Free ***********************************************************/

/* Allocate aligned vector memory block */
VECLIB_INLINE void* vec_malloc(size_t size, size_t align);

/* Free aligned vector memory block */
VECLIB_INLINE void vec_free(void *ptr);

/* Pause spin-wait loop */
VECLIB_INLINE void vec_pause (void);

/* Serialize previous loads and stores before following loads and stores */
VECLIB_INLINE void vec_fencestoreloads (void);

/********************************************************* Boolean ***********************************************************/

/* Population Count unsigned long long */
VECLIB_INLINE int vec_popcount1uw (unsigned long long a);

/********************************************************* CRC ***********************************************************/

static inline unsigned long long veclib_bitreverse32 (unsigned int input);

static inline unsigned long long veclib_crc_mod2 (unsigned long long dividend);

/* Accumulate CRC32 from unsigned char */
VECLIB_INLINE unsigned int vec_crc321ub (unsigned int crc, unsigned char next);

/* Accumulate CRC32 from unsigned int */
VECLIB_INLINE unsigned int vec_crc324ub (unsigned int crc, unsigned int next);

#endif

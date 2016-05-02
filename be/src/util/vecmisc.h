/******************************************************************************/
/*                                                                            */
/* Licensed Materials - Property of IBM                                       */
/*                                                                            */
/* IBM Power Vector Intrinisic Functions version 1.0.4                        */
/*                                                                            */
/* Copyright IBM Corp. 2014,2016                                              */
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
#include "vec128int.h"

/************************************************* Floating-Point Control and Status Register *************************/

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

/****************************************************** Miscellaneous *************************************************/

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
VECLIB_INLINE void vec_zeroallupper (void);

/* Serialize previous stores before following stores */
VECLIB_INLINE void vec_fence (void);

/*************************************************** Malloc/Free ******************************************************/

/* Allocate aligned vector memory block */
VECLIB_INLINE void* vec_malloc (size_t size, size_t align);

/* Free aligned vector memory block */
VECLIB_INLINE void vec_free (void *ptr);

/* Pause spin-wait loop */
VECLIB_INLINE void vec_pause (void);

/* Serialize previous loads and stores before following loads and stores */
VECLIB_INLINE void vec_fencestoreloads (void);

/***************************************************** Boolean ********************************************************/

/* Population Count unsigned long long */
VECLIB_INLINE int vec_popcount1uw (unsigned long long a); 

/******************************************************* CRC **********************************************************/

  #ifdef __LITTLE_ENDIAN__
    static const int upper64 = 1;  /* subscript for upper half of vector register */
    static const int lower64 = 0;  /* subscript for lower half of vector register */
  #elif __BIG_ENDIAN__
    static const int upper64 = 0;  /* subscript for upper half of vector register */
    static const int lower64 = 1;  /* subscript for lower half of vector register */
  #endif

  #ifdef __LITTLE_ENDIAN__
    static const int upper32       = 3;  /* subscript for upper quarter of vector register */
    static const int uppermiddle32 = 2;  /* subscript for upper middle quarter of vector register */
    static const int lowermiddle32 = 1;  /* subscript for lower middle quarter of vector register */
    static const int lower32       = 0;  /* subscript for lower quarter of vector register */
  #elif __BIG_ENDIAN__
    static const int upper32       = 0;  /* subscript for upper quarter of vector register */
    static const int uppermiddle32 = 1;  /* subscript for upper middle quarter of vector register */
    static const int lowermiddle32 = 2;  /* subscript for lower middle quarter of vector register */
    static const int lower32       = 3;  /* subscript for lower quarter of vector register */
  #endif

static inline unsigned long long veclib_bitreverse16 (unsigned short input);

static inline unsigned long long veclib_bitreverse32 (unsigned int input);

static inline unsigned long long veclib_bitreverse64 (unsigned long long source);

static inline vector unsigned long long veclib_vec_cntlz_int128 (vector unsigned long long v);

static inline unsigned long long veclib_crc_mod2 (unsigned long long dividend);

static inline vector unsigned char veclib_int96_crc_mod2 (vector unsigned char dividend);

/* Accumulate CRC32C (Castagnoli) from unsigned char */
VECLIB_INLINE unsigned int vec_crc321ub (unsigned int crc, unsigned char next);


/* Accumulate CRC32C (Castagnoli) from unsigned short */
VECLIB_INLINE unsigned int vec_crc321uh (unsigned int crc, unsigned short next);

/* Accumulate CRC32C (Castagnoli) from unsigned int */
VECLIB_INLINE unsigned int vec_crc321uw (unsigned int crc, unsigned int next);


/* Accumulate CRC32 from unsigned int - deprecated - use previous function */
VECLIB_INLINE unsigned int vec_crc324ub (unsigned int crc, unsigned int next);

/* Accumulate CRC32C (Castagnoli) from unsigned long long */
VECLIB_INLINE unsigned long long vec_crc321ud (unsigned long long crc, unsigned long long next);

#endif

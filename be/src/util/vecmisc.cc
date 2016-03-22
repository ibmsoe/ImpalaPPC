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

#include <stdio.h>
#include <stdlib.h>
#include <altivec.h>
#include "veclib_types.h"
#include "vecmisc.h"

/************************************************************** Floating-Point Control and Status Register *************************/

/* Get exception mask bits from MXCSR register */
VECLIB_INLINE unsigned int vec_getfpexceptionmask (void)
{
  #ifdef __ibmxl__
    unsigned long long exception_mask_mask = 0x1F000002;
    double FPSCR_content = __readflm();
    unsigned long long *upper_32bits = (unsigned long long *) &FPSCR_content;
    *upper_32bits = *upper_32bits & 0xFFFFFFFF;
    return (unsigned int) (*upper_32bits & exception_mask_mask);
  #else
    /* do nothing for now */
  #endif
}

/* Get exception state bits from MXCSR register */
VECLIB_INLINE unsigned int vec_getfpexceptionstate (void)
{
  #ifdef __ibmxl__
    unsigned long long exception_state_mask = 0xE01FFD;
    double FPSCR_content = __readflm();
    unsigned long long *upper_32bits = (unsigned long long *) &FPSCR_content;
    *upper_32bits = *upper_32bits & 0xFFFFFFFF;
    return (unsigned int) (*upper_32bits & exception_state_mask);
  #else
    /* do nothing for now */
  #endif
}

/* Get flush zero bits from MXCSR register */
VECLIB_INLINE unsigned int vec_getfpflushtozeromode (void)
{
  /* do nothing for now */
  return 0;
}

/* Get rounding mode bits from MXCSR register */
VECLIB_INLINE unsigned int vec_getfproundingmode (void) 
{
  #ifdef __ibmxl__
    unsigned long long rounding_mode_mask = 0x00000003;
    double FPSCR_content = __readflm();
    unsigned long long *upper_32bits = (unsigned long long *) &FPSCR_content;
    *upper_32bits = *upper_32bits & 0xFFFFFFFF;
    return (unsigned int) (*upper_32bits & rounding_mode_mask);
  #else
    /* do nothing for now */
  #endif
}

/* Get all bits from MXCSR register */
VECLIB_INLINE unsigned int vec_getfpallbits (void)
{
  #ifdef __ibmxl__
    unsigned long long all_bits_mask = 0xFFFFFFFF;
    double FPSCR_content = __readflm();
    unsigned long long *upper_32bits = (unsigned long long *) &FPSCR_content;
    *upper_32bits = *upper_32bits & 0xFFFFFFFF;
    return (unsigned int) (*upper_32bits & all_bits_mask);
  #else
    /* do nothing for now */
  #endif
}

/* Set exception mask bits in MXCSR register */
VECLIB_INLINE void vec_setfpexceptionmask (unsigned int mask) 
{
  #ifdef __ibmxl__
    unsigned long long exception_mask_mask = 0x1F000002;
    unsigned long long exception_mask = (unsigned long long) mask & exception_mask_mask;
    double *exception_mask_bits;
    exception_mask_bits  = (double*) &exception_mask;
    __setflm (*exception_mask_bits);
  #else
    /* do nothing for now */
  #endif
}

/* Set exception state bits in MXCSR register */
VECLIB_INLINE void vec_setfpexceptionstate (unsigned int mask) 
{
  #ifdef __ibmxl__
    unsigned long long exception_state_mask = 0xE01FFD;
    unsigned long long exception_state = (unsigned long long) mask & exception_state_mask;
    double* exception_state_bits;
    exception_state_bits = (double*) &exception_state;
    __setflm (*exception_state_bits);
  #else
    /* do nothing for now */
  #endif
}

/* Set flush zero bits in MXCSR register */
VECLIB_INLINE void vec_setfpflushtozeromode (unsigned int mask) 
{
  /* do nothing for now */
}

/* Set rounding mode bits in MXCSR register */
VECLIB_INLINE void vec_setfproundingmode (unsigned int mask)  
{
  #ifdef __ibmxl__
    unsigned int rounding_mode_mask = 0x3;
    unsigned int rounding_mode = mask & rounding_mode_mask;
    __setrnd (rounding_mode);
  #else
    /* do nothing for now */
  #endif
}

/* Set all bits in MXCSR register */
VECLIB_INLINE void vec_setfpallbits (unsigned int mask)
{
  #ifdef __ibmxl__
    /* set bits from 63:32 */
    unsigned long long all_bits_mask = 0xFFFFFFFF;
    unsigned long long all_bits = (unsigned long long) mask & all_bits_mask;
    double* fpscr_all_bits = (double*) &all_bits;
    __setflm (*fpscr_all_bits);
  #else
    /* do nothing for now */
  #endif
}

/************************************************************** Miscellaneous ******************************************************/

/* Prefetch cache line with hint */
VECLIB_INLINE void vec_prefetch (void const* address, vec_prefetch_hint hint)
{
  #ifdef __ibmxl__
    __dcbt ((void*) address);
  #else
    /* do nothing for now */
  #endif
}

/* Zero upper half of all 8 or 16 YMMM registers */
void vec_zeroallupper (void)  
{
  /* no-op */
}

/* Serialize previous stores before following stores */
VECLIB_INLINE void vec_fence (void)  
{
  #ifdef __ibmxl__
    __fence ();
  #else
    /* do nothing for now */
  #endif
}

/********************************************************* Malloc/Free ***********************************************************/

/* Allocate aligned vector memory block */
VECLIB_INLINE void* vec_malloc(size_t size, size_t align) {

    void *result;
    #ifdef _MSC_VER 
    result = _aligned_malloc(size, align);
    #else 
     if(posix_memalign(&result, align, size)) result = 0;
    #endif
    return result;
}

/* Free aligned vector memory block */
VECLIB_INLINE void vec_free(void *ptr) {

    #ifdef _MSC_VER 
        _aligned_free(ptr);
    #else 
      free(ptr);
    #endif
}

/* Pause spin-wait loop */
VECLIB_INLINE void vec_pause (void){}

/* Serialize previous loads and stores before following loads and stores */
VECLIB_INLINE void vec_fencestoreloads (void){
  #ifdef __ibmxl__
    __sync();
  #else
    __atomic_thread_fence( 5 );
  #endif
}

/********************************************************* Boolean ***********************************************************/

/* Population Count unsigned long long */
VECLIB_INLINE int vec_popcount1uw (unsigned long long a) {
  int result;
  #ifdef __ibmxl__
    result = __popcnt8 (a);
  #else
    /* gcc */
    asm("   popcntd %0, %1"
    :   "=r"     (result)
    :   "r"      (a)
    );
  #endif
  return result;
}

/********************************************************* CRC ***********************************************************/

static inline unsigned long long veclib_bitreverse32 (unsigned int input)
{
  /* Reverse the bits of a 32-bit input */
  long long source = (long long) input;
  unsigned long long result = 0;
  
  /* bit permute selector for reversing 0th..3rd byte of the input */ 
  const long long bit_selector0 = 0x3F3E3D3C3B3A3938;
  const long long bit_selector1 = 0x3736353433323130;
  const long long bit_selector2 = 0x2F2E2D2C2B2A2928;
  const long long bit_selector3 = 0x2726252423222120;
  /* reverse 0th..3rd byte of the input, the result is in the lower 8 bits */
  #ifdef __ibmxl__
    /* xlc */
    long long reverse_byte0 = __bpermd (bit_selector0, source);
    long long reverse_byte1 = __bpermd (bit_selector1, source);
    long long reverse_byte2 = __bpermd (bit_selector2, source);
    long long reverse_byte3 = __bpermd (bit_selector3, source);
  #else
    /* gcc */
    long long reverse_byte0 = __builtin_bpermd (bit_selector0, source);
    long long reverse_byte1 = __builtin_bpermd (bit_selector1, source);
    long long reverse_byte2 = __builtin_bpermd (bit_selector2, source);
    long long reverse_byte3 = __builtin_bpermd (bit_selector3, source);
  #endif
  
  /* rotate and insert reverse_byte0..3 to the 3rd..0th byte of the result */
  #ifdef __ibmxl__
    /* xlc */
    result = __rlwimi ((unsigned int) reverse_byte0, (unsigned int) result, 24, 0xFF000000);
    result = __rlwimi ((unsigned int) reverse_byte1, (unsigned int) result, 16, 0xFF0000);
    result = __rlwimi ((unsigned int) reverse_byte2, (unsigned int) result, 8, 0xFF00);
    result = __rlwimi ((unsigned int) reverse_byte3, (unsigned int) result, 0, 0xFF);    
  #else
    reverse_byte0 = (reverse_byte0 << 24) & 0xFF000000;
    reverse_byte1 = (reverse_byte1 << 16) & 0xFF0000;
    reverse_byte2 = (reverse_byte2 << 8) & 0xFF00;
    reverse_byte3 = (reverse_byte3 << 0) & 0xFF; 
    unsigned long long reverse_byte01 = reverse_byte0 | reverse_byte1;
    unsigned long long reverse_byte23 = reverse_byte2 | reverse_byte3;
    result = reverse_byte01 | reverse_byte23;
  #endif
  
  return result;
}

static inline unsigned long long veclib_crc_mod2 (unsigned long long dividend)
{
  /* Compute the crc by modulo 2 polynomial long division */
  unsigned long long divisor = 0x11EDC6F41;
  if (dividend < 0x100000000) {
    /* dividend is less than 33 bit */
    return dividend;
  }
  unsigned int leading_zeros = 0;
  #ifdef __ibmxl__
    /* xlc */
    leading_zeros = __cntlz8 (dividend);
  #else
    /* gcc */
    asm("   cntlzd %0, %1"
    :   "=r"     (leading_zeros)
    :   "r"      (dividend)
    );
  #endif
  divisor = divisor << (31 - leading_zeros);
  dividend = dividend ^ divisor;
  dividend = veclib_crc_mod2 (dividend);
}

/* Accumulate CRC32 from unsigned char */
VECLIB_INLINE unsigned int vec_crc321ub (unsigned int crc, unsigned char next)
{
  unsigned long long reversed_crc = veclib_bitreverse32 (crc & 0xFFFFFFFF);
  unsigned long long shifted_crc = reversed_crc << 8;
  
  #ifdef __ibmxl__
    /* xlc */
    unsigned long long reversed_next = (unsigned long long) __bpermd (0x3F3E3D3C3B3A3938, (long long) (next & 0xFF));
  #else
    /* gcc */
    unsigned long long reversed_next = (unsigned long long) __builtin_bpermd (0x3F3E3D3C3B3A3938, (long long) (next & 0xFF));
  #endif
 
  unsigned long long shifted_next = reversed_next << 32;

  unsigned long long merged = shifted_crc ^ shifted_next;
  unsigned int reversed_new_crc = (unsigned int) (veclib_crc_mod2 (merged) & 0xFFFFFFFF);
  return veclib_bitreverse32 (reversed_new_crc);
}

/* Accumulate CRC32 from unsigned int */
VECLIB_INLINE unsigned int vec_crc324ub (unsigned int crc, unsigned int next)
{
  unsigned long long reversed_crc = veclib_bitreverse32 (crc & 0xFFFFFFFF);
  unsigned long long shifted_crc = reversed_crc << 32;

  unsigned long long reversed_next = veclib_bitreverse32 (next & 0xFFFFFFFF);
  unsigned long long shifted_next = reversed_next << 32;

  unsigned long long merged = shifted_crc ^ shifted_next;
  unsigned int reversed_new_crc = (unsigned int) (veclib_crc_mod2 (merged) & 0xFFFFFFFF);
  return veclib_bitreverse32 (reversed_new_crc);
}

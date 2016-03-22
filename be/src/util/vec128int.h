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

#ifndef _H_VEC128INT
#define _H_VEC128INT

#include <altivec.h>
#include "veclib_types.h"

/************************************************************ Load ************************************************************/

/* Load 128-bits of integer data, aligned */
VECLIB_INLINE __m128i vec_load1q (__m128i const* address);

/* Load 128-bits of integer data, unaligned */
VECLIB_INLINE __m128i vec_loadu1q (__m128i const* address);

/* Load 128-bits of integer data, unaligned  - deprecated - use previous function */
VECLIB_INLINE __m128i vec_load1qu (__m128i const* address);

/* load 64-bits of integer data to lower part and zero upper part */
VECLIB_INLINE __m128i vec_loadlower1sd (__m128i const* from);

/************************************************************ Set ************************************************************/

/* Set 128 integer bits to zero */
VECLIB_INLINE __m128i vec_zero1q (void);

/* Splat 8-bit char to 16 8-bit chars */
VECLIB_INLINE __m128i vec_splat16sb (char scalar);

/* Splat 16-bit short to 8 16-bit shorts */
VECLIB_INLINE __m128i vec_splat8sh (short scalar);

/* Splat 32-bit int to 4 32-bit ints */
VECLIB_INLINE __m128i vec_splat4sw (int scalar);

/* Splat 64-bit long long to 2 64-bit long longs */
VECLIB_INLINE __m128i vec_splat2sd (long long scalar);

/* Set 16 8-bit chars */
VECLIB_INLINE __m128i vec_set16sb (char c15, char c14, char c13, char c12, char c11, char c10, char c9, char c8, char c7, char c6, char c5, char c4, char c3, char c2, char c1, char c0);

/* Set  8 16-bit shorts */
VECLIB_INLINE __m128i vec_set8sh (short s7, short s6, short s5, short s4, short s3, short s2, short s1, short s0);

/* Set 4 32-bit ints */
VECLIB_INLINE __m128i vec_set4sw (int i3, int i2, int i1, int i0);

/* Set 2 64-bit long longs */
VECLIB_INLINE __m128i vec_set2sd (__m64 LL1, __m64 LL0);

/* Set 16 8-bit chars reversed */
VECLIB_INLINE __m128i vec_setreverse16sb (char c15, char c14, char c13, char c12, char c11, char c10, char c9, char c8, char c7, char c6, char c5, char c4, char c3, char c2, char c1, char c0);

/* Set 8 16-bit shorts reversed */
VECLIB_INLINE __m128i vec_setreverse8sh (short s7, short s6, short s5, short s4, short s3, short s2, short s1, short s0);

/* Set 4 32-bit ints reversed */
VECLIB_INLINE __m128i vec_setreverse4sw (int i3, int i2, int i1, int i0);

/* Set 2 64-bit long longs reversed */
VECLIB_INLINE __m128i vec_setreverse2sd (__m64 v1, __m64 v0);

/* Set lower 64-bits of integer data and zero upper part */
VECLIB_INLINE __m128i vec_Zerouppersd (__m128i v);

/************************************************************ Store ************************************************************/

/* Store 128-bits integer, aligned */
VECLIB_INLINE void vec_store1q (__m128i* address, __m128i v);

/* Store 128-bits integer, unaligned */
VECLIB_INLINE void vec_storeu1q (__m128i* to, __m128i from);

/* Store 128-bits integer, unaligned  - deprecated - use previous function */
VECLIB_INLINE void vec_store1qu (__m128i* to, __m128i from);

/* Store lower 64-bit long long */
VECLIB_INLINE void vec_storelower1sdof2sd (__m128i* to, __m128i from);

/************************************************************ Insert ************************************************************/

/* Insert 32-bit int */
VECLIB_INLINE __m128i vec_insert4sw (__m128i into, int from, const intlit2 element_from_right);

/* Insert 64-bit long long */
VECLIB_INLINE __m128i vec_insert2sd (__m128i into, long long from, const intlit1 element_from_right);

/* Insert 32-bit int, zeroing upper */
VECLIB_INLINE __m128i vec_convert1swto1uq (int from);

/* Insert 16-bit short into one of 8 16-bit shorts */
VECLIB_INLINE __m128i vec_insert8sh (__m128i v, int scalar, intlit3 element_from_right);

/* Insert 8-bit unsigned char into one of 16 bytes */
VECLIB_INLINE __m128i vec_insert16ub (__m128i v, int scalar, intlit4 element_from_right);

/************************************************************ Extract ************************************************************/

/* Extract 32-bit int */
VECLIB_INLINE int vec_extract1swfrom4sw (__m128i from, const intlit2 element_from_right);

/* Extract 64-bit long long */
VECLIB_INLINE long long vec_extract1sdfrom2sd (__m128i from, const intlit1 element_from_right);

/* Extract 16-bit short from one of 8 16-bit shorts */
VECLIB_INLINE int vec_extract8sh (__m128i v, intlit3 element_from_right);

/* Extract upper bit of 16 8-bit chars */
VECLIB_INLINE int vec_extractupperbit16sb (__m128i v);

/* Extract upper bit of 2 64-bit doubles */
VECLIB_INLINE int vec_extractupperbit2dp (__m128d v);

/* Extract lower 32-bit int */
VECLIB_INLINE int     vec_extractlowersw  (__m128i from);

/******************************************************** Convert integer to integer ***************************************************/

/* Convert 8+8 16-bit shorts to 16 8-bit chars with signed saturation */
VECLIB_INLINE __m128i vec_packs8hto16sb (__m128i left, __m128i right);

/* Convert 8+8 16-bit shorts to 16 8-bit chars with signed saturation  - deprecated - use previous function */
VECLIB_INLINE __m128i vec_packs88hto16sb (__m128i left, __m128i right);

/* Convert 4+4 32-bit ints to 8 16-bit shorts with signed saturation */
VECLIB_INLINE __m128i vec_packs4wto8sh (__m128i left, __m128i right);

/* Convert 4+4 32-bit ints to 8 16-bit shorts with signed saturation  - deprecated - use previous function */
VECLIB_INLINE __m128i vec_packs44wto8sh (__m128i left, __m128i right);

/***************************************************** Convert floating-point to integer ***********************************************/

/* Convert 4 32-bit floats to 4 32-bit ints with truncation */
VECLIB_INLINE __m128i vec_converttruncating4spto4sw (__m128 a);

/* Convert 4 32-bit floats to 4 32-bit ints */
VECLIB_INLINE __m128i vec_convert4spto4sw (__m128 from);

/* Convert 2     64-bit doubles to       2 32-bit ints      with truncation */
#ifdef VECLIB_VSX
VECLIB_INLINE __m128i vec_Convert2dpto2sw (__m128d from);
#endif

/************************************************************** Arithmetic **************************************************************/

/* Add 16 8-bit chars */
VECLIB_INLINE __m128i vec_add16sb (__m128i left, __m128i right);

/* Add 8 16-bit shorts */
VECLIB_INLINE __m128i vec_add8sh (__m128i left, __m128i right);

/* Add 4 32-bit ints */
VECLIB_INLINE __m128i vec_add4sw (__m128i left, __m128i right);

/* Add 2 64-bit long longs */
VECLIB_INLINE __m128i vec_add2sd (__m128i left, __m128i right);

/* Add 16 8-bit chars with unsigned saturation */
VECLIB_INLINE __m128i vec_addsaturating16ub (__m128i left, __m128i right);

/* Add 16 8-bit chars with signed saturation */
VECLIB_INLINE __m128i vec_addsaturating16sb (__m128i left, __m128i right);

/* Add 8 16-bit shorts with signed saturation */
VECLIB_INLINE __m128i vec_addsaturating8sh (__m128i left, __m128i right);

/* Add 8 16-bit shorts with unsigned saturation */
VECLIB_INLINE __m128i vec_addsaturating8uh (__m128i left, __m128i right);

/* Subtract 16 8-bit chars */
VECLIB_INLINE __m128i vec_subtract16sb (__m128i left, __m128i right);

/* Subtract 8 16-bit shorts */
VECLIB_INLINE __m128i vec_subtract8sh (__m128i left, __m128i right);

/* Subtract 4 32-bit ints */
VECLIB_INLINE __m128i vec_subtract4sw (__m128i left, __m128i right);

/* Subtract 2 64-bit long longs*/
VECLIB_INLINE __m128i vec_subtract2sd (__m128i left, __m128i right);

/* Subtract 16 8-bit chars with unsigned saturation */
VECLIB_INLINE __m128i vec_subtractsaturating16ub (__m128i left, __m128i right);

/* Subtract 16 8-bit chars with signed saturation */
VECLIB_INLINE __m128i vec_subtractsaturating16sb (__m128i left, __m128i right);

/* Subtract 8 16-bit shorts with signed saturation */
VECLIB_INLINE __m128i vec_subtractsaturating8sh (__m128i left, __m128i right);

/* Subtract 8 16-bit shorts with unsigned saturation */
VECLIB_INLINE __m128i vec_subtractsaturating8uh (__m128i left, __m128i right);

/* Multiply lower 32-bit unsigned ints producing 2 64-bit unsigned long longs */
VECLIB_INLINE __m128i vec_multiplylower2uwto2ud (__m128i left, __m128i right);

/* Multiply 8 16-bit signed shorts */
VECLIB_INLINE __m128i vec_multiply8sh (__m128i left, __m128i right);

/* Average 16 8-bit unsigned chars */
VECLIB_INLINE __m128i vec_average16ub (__m128i left, __m128i right);

/* Average 8 16-bit unsigned shorts */
VECLIB_INLINE __m128i vec_average8uh (__m128i left, __m128i right);

/* Max 8 16-bit shorts */
VECLIB_INLINE __m128i vec_max8sh (__m128i left, __m128i right);

/* Max 16 8-bit unsigned chars */
VECLIB_INLINE __m128i vec_max16ub (__m128i left, __m128i right);

/* Min 16 8-bit unsigned chars */
VECLIB_INLINE __m128i vec_min16ub (__m128i left, __m128i right);

/* Min 8 16-bit shorts */
VECLIB_INLINE __m128i vec_min8sh (__m128i left, __m128i right);

/* Sum 2 octets of absolute differences of 16 8-bit unsigned chars into 2 64-bit long longs */
VECLIB_INLINE __m128i vec_sumabsdiffs16ub (__m128i left, __m128i right);

/* Multiply 4 16-bit shorts then add adjacent pairs with saturation to 4 32-bit ints */
VECLIB_INLINE __m128i vec_summultiply4sh (__m128i left, __m128i right);

/* Absolute value 16  8-bit chars */
VECLIB_INLINE __m128i vec_Abs16sb  (__m128i a);

/* Absolute value  8 16-bit shorts */
VECLIB_INLINE __m128i vec_Abs8sh (__m128i a);

/* Absolute value  4 32-bit ints */
VECLIB_INLINE __m128i vec_Abs4sw (__m128i a);

#if defined(__LITTLE_ENDIAN__) || defined(__LITTLE_ENDIAN) || defined(_LITTLE_ENDIAN)
  #define LEleft_BEright left
  #define LEright_BEleft right
#elif __BIG_ENDIAN__
  #define LEleft_BEright right
  #define LEright_BEleft left
#endif

/* Horizontally add 4+4 adjacent pairs of 16-bit shorts to 8 16-bit shorts */ /* {a0+a1, a2+a3, a4+a5, a6+a7, b0+b1, b2+b3, b4+b5, b6+b7} */
VECLIB_INLINE __m128i vec_horizontalAdd8sh  (__m128i left, __m128i right);

/* Horizontally add 2+2 adjacent pairs of 32-bit ints to 4 32-bit ints */
VECLIB_INLINE __m128i vec_partialhorizontaladd2sw (__m128i left, __m128i right);

/* Horizontally add 4+4 adjacent pairs of 16-bit shorts to 8 16-bit shorts with saturation */ /* {a0+a1, a2+a3, a4+a5, a6+a7, b0+b1, b2+b3, b4+b5, b6+b7} with saturation */
VECLIB_INLINE __m128i vec_horizontalAddsaturating8sh (__m128i left, __m128i right);

/* Horizontally subtract 4+4 adjacent pairs of 16-bit shorts to 8 16-bit shorts */ /* { a0-a1, a2-a3, a4-a5, a6-a7, b0-b1, b2-b3, b4-b5, b6-b7 } */
VECLIB_INLINE __m128i vec_horizontalSub8sh  (__m128i left, __m128i right);

/* Horizontally subtract 2+2 adjacent pairs of 32-bit ints to 4 32-bit ints */
VECLIB_INLINE __m128i vec_partialhorizontalsubtract2sw (__m128i left, __m128i right);

/* Horizontally subtract 4+4 adjacent pairs of 16-bit shorts to 8 16-bit shorts with saturation */ /* { a0-a1, a2-a3, a4-a5, a6-a7, b0-b1, b2-b3, b4-b5, b6-b7 } with saturation */
VECLIB_INLINE __m128i vec_horizontalSubtractsaturating8sh (__m128i left, __m128i right);

/* Multiply 16 8-bit u*s chars then add adjacent 16-bit products with signed saturation */
VECLIB_INLINE __m128i vec_Multiply16sbthenhorizontalAddsaturating8sh (__m128i left, __m128i right);

/* Multiply 8 16-bit shorts, shift right 14, add 1 and shift right 1 to 8 16-bit shorts */
VECLIB_INLINE __m128i vec_Multiply8shExtractUpper (__m128i left, __m128i right);

/* Negate 16  8-bit chars  when mask is negative, zero when zero, else copy */
VECLIB_INLINE __m128i vec_conditionalNegate16sb  (__m128i left, __m128i right);

/* Negate  8 16-bit shorts when mask is negative, zero when zero, else copy */
VECLIB_INLINE __m128i vec_conditionalNegate8sh (__m128i left, __m128i right);

/* Negate  4 32-bit ints   when mask is negative, zero when zero, else copy */
VECLIB_INLINE __m128i vec_conditionalNegate4sw (__m128i left, __m128i right);

/* Multiply 4 32-bit signed ints */
VECLIB_INLINE __m128i vec_multiply4sw (__m128i left, __m128i right);

/* Max  4 32-bit signed ints */
__m128i vec_Max4sw (__m128i left, __m128i right);

/* Min  4 32-bit signed ints */
__m128i vec_Min4sw (__m128i left, __m128i right);


/************************************************************************ Boolean *********************************************************************/

/* Bitwise 128-bit and */
VECLIB_INLINE __m128i vec_bitand1q (__m128i left, __m128i right);

/* Bitwise 128-bit and not (reversed) */
VECLIB_INLINE __m128i vec_bitandnotleft1q (__m128i left, __m128i right);

/* Bitwise 128-bit or */
VECLIB_INLINE __m128i vec_bitor1q (__m128i left, __m128i right);

/* Bitwise 128-bit xor */
VECLIB_INLINE __m128i vec_bitxor1q (__m128i left, __m128i right);

/****************************************************************** Unpack ************************************************************************/

/* Unpack 8+8 8-bit chars from high halves and interleave */
VECLIB_INLINE __m128i vec_unpackhigh8sb (__m128i left, __m128i right);

/* Unpack 8+8 8-bit chars from high halves and interleave  - deprecated - use previous function */
VECLIB_INLINE __m128i vec_unpackhigh88sb (__m128i left, __m128i right);

/* Unpack 4+4 16-bit shorts from high halves and interleave */
VECLIB_INLINE __m128i vec_unpackhigh4sh (__m128i left, __m128i right);

/* Unpack 4+4 16-bit shorts from high halves and interleave  - deprecated - use previous function */
VECLIB_INLINE __m128i vec_unpackhigh44sh (__m128i left, __m128i right);

/* Unpack 8+8 8-bit chars from low halves and interleave */
VECLIB_INLINE __m128i vec_unpacklow8sb (__m128i left, __m128i right);

/* Unpack 8+8 8-bit chars from low halves and interleave  - deprecated - use previous function */
VECLIB_INLINE __m128i vec_unpacklow88sb (__m128i left, __m128i right);

/* Unpack 4+4 16-bit shorts from low halves and interleave */
VECLIB_INLINE __m128i vec_unpacklow4sh (__m128i left, __m128i right);

/* Unpack 4+4 16-bit shorts from low halves and interleave  - deprecated - use previous function */
VECLIB_INLINE __m128i vec_unpacklow44sh (__m128i left, __m128i right);

/* Unpack 2+2 32-bit ints from low halves and interleave */
VECLIB_INLINE __m128i vec_unpacklow2sw (__m128i to_even, __m128i to_odd);

/* Unpack 2+2 32-bit ints from high halves and interleave */
VECLIB_INLINE __m128i vec_unpackhigh2sw (__m128i to_even, __m128i to_odd);

/* Unpack 1+1 64-bit long longs from low halves and interleave */
VECLIB_INLINE __m128i vec_unpacklow1sd (__m128i to_even, __m128i to_odd);

/* Unpack 1+1 64-bit long longs from high halves and interleave */
VECLIB_INLINE __m128i vec_unpackhigh1sd (__m128i to_even, __m128i to_odd);

/*************************************************************** Shift *****************************************************************/

/*- SSE2 shifts >= 32 produce 0; Altivec/MMX shifts by count%count_size. */
/*- The Altivec spec says the element shift count vector register must have a shift count in each element */
/*- and the shift counts may be different for each element. */
/*- The PowerPC Architecture says all elements must contain the same shift count. That wins. */
/*- The element shift count_size is: byte shift: 3 bits (0-7), halfword: 4 bits (0-15), word: 5 bits (0-31). */
/*- For full vector shifts the Altivec/PowerPC bit shift count is in the rightmost 7 bits, */
/*- with a 4 bit slo/sro byte shift count then a 3 bit sll/srl bit shift count. */

/* Shift left */

/* Shift 8 16-bit shorts left logical */
VECLIB_INLINE __m128i vec_shiftleft8sh (__m128i v, __m128i count);

/* Shift 4 32-bit ints left logical */
VECLIB_INLINE __m128i vec_shiftleft4sw (__m128i v, __m128i count);

/* Shift 2 64-bit long longs left logical */
VECLIB_INLINE __m128i vec_shiftleft2sd (__m128i v, __m128i count);

/* Shift 8 16-bit shorts left logical immediate */
VECLIB_INLINE __m128i vec_shiftleftimmediate8sh (__m128i v, intlit8 count);

/* Shift 4 32-bit ints left logical immediate */
VECLIB_INLINE __m128i vec_shiftleftimmediate4sw (__m128i v, intlit8 count);

/* Shift 2 64-bit long longs left logical immediate */
VECLIB_INLINE __m128i vec_shiftleftimmediate2sd (__m128i v, intlit8 count);

/* Shift 128-bits left logical immediate by bytes */
VECLIB_INLINE __m128i vec_shiftleftbytes1q (__m128i v, intlit8 bytecount);

/* Shift right */

/* Shift 8 16-bit shorts right logical immediate */
VECLIB_INLINE __m128i vec_shiftrightimmediate8sh (__m128i v, intlit8 count);

/* Shift 4 32-bit ints right logical immediate */
VECLIB_INLINE __m128i vec_shiftrightimmediate4sw (__m128i v, intlit8 count);

/* Shift 128-bits right logical immediate by bytes */
VECLIB_INLINE __m128i vec_shiftrightbytes1q (__m128i v, intlit8 bytecount);

/* Shift 4 32-bit ints right arithmetic */
VECLIB_INLINE __m128i vec_shiftrightarithmetic4wimmediate (__m128i v, intlit8 count);

/* Shift 2 64-bit long longs right logical immediate */
VECLIB_INLINE __m128i vec_shiftrightlogical2dimmediate (__m128i v, intlit8 count);

/* Shift 8 16-bit shorts right arithmetic */
VECLIB_INLINE __m128i vec_shiftrightarithmetic8himmediate (__m128i v, intlit8 count);

/* Shift 4 32-bit ints left logical */
VECLIB_INLINE __m128i vec_shiftrightarithmetic4sw (__m128i v, __m128i count);

/* Shift 128+128-bits right into 128-bits */
VECLIB_INLINE __m128i vec_shiftright2dqw (__m128i left, __m128i right, int const  count);

/***************************************************************** Permute *****************************************************************/

/* Shuffle lower 4 16-bit shorts using mask */
VECLIB_INLINE __m128i vec_permutelower4sh (__m128i v, intlit8 element_selectors);
/* Leaves upper half unchanged */

/* Shuffle 4 32-bit ints using mask */
VECLIB_INLINE __m128i vec_permute4sw (__m128i v, intlit8 element_selectors);

/* Shuffle 16 8-bit chars using mask */
VECLIB_INLINE __m128i vec_permute16sb (__m128i v, __m128i mask);

/***************************************************************** Compare *****************************************************************/

/* Compare eq */

/* Compare 16 8-bit chars for == to vector mask */
VECLIB_INLINE __m128i vec_compareeq16sb (__m128i left, __m128i right);

/* Compare 8 16-bit shorts for == to vector mask */
VECLIB_INLINE __m128i vec_compareeq8sh (__m128i left, __m128i right);

/* Compare 4 32-bit ints for == to vector mask */
VECLIB_INLINE __m128i vec_compare4sw (__m128i left, __m128i right);

/* Compare lt */

/* Compare 16 8-bit chars for < to mask */
VECLIB_INLINE __m128i vec_comparelt16sb (__m128i left, __m128i right);

/* Compare 8 16-bit shorts for < to vector mask */
VECLIB_INLINE __m128i vec_comparelt8sh (__m128i left, __m128i right);

/* Compare 4 32-bit ints for < to vector mask */
VECLIB_INLINE __m128i vec_comparelt4sw (__m128i left, __m128i right);

/* Compare gt */

/* Compare 16 8-bit signed chars for > to vector mask */
VECLIB_INLINE __m128i vec_comparegt16sb (__m128i left, __m128i right);

/* Compare 8 16-bit shorts for > to vector mask */
VECLIB_INLINE __m128i vec_comparegt8sh (__m128i left, __m128i right);

/* Compare 4 32-bit ints for > to vector mask */
VECLIB_INLINE __m128i vec_comparegt4sw (__m128i left, __m128i right);

/***************************************************************** Type Cast *****************************************************************/

/* Cast type __m128 to __m128i */
VECLIB_INLINE __m128i vec_cast4spto1q (__m128 v);

/* Cast __m128d to __m128i */
VECLIB_INLINE __m128i vec_Cast2dpto4sw (__m128d from);
#endif

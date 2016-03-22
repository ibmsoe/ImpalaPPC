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

#ifndef _H_VEC128STR
#define _H_VEC128STR

#include <altivec.h>
#include "veclib_types.h"

/***************************************************************** Compare String *****************************************************************/

/* String Control */                           /* 76543210 */
/* 1:0 Character Type */
#define  _SIDD_UBYTE_OPS                 0x00  /* ------00 unsigned  8-bit characters */
#define  _SIDD_UWORD_OPS                 0x01  /* ------01 unsigned 16-bit characters */
#define  _SIDD_SBYTE_OPS                 0x02  /* ------10 signed  8-bit characters */
#define  _SIDD_SWORD_OPS                 0x03  /* ------11 signed 16-bit characters */
/* 3:2 Compare Operation */
#define  _SIDD_CMP_EQUAL_ANY                0  /* ----00-- compare right equal to any left */
#define  _SIDD_CMP_RANGES                0x04  /* ----01-- compare right to two ranges in left */
#define  _SIDD_CMP_EQUAL_EACH            0x08  /* ----10-- compare each right equals corresponding left */
#define  _SIDD_CMP_EQUAL_ORDERED         0x0C  /* ----11-- compare right contains left */
/* 5:4 Mask Polarity bits */
#define  _SIDD_POSITIVE_POLARITY            0  /* ---0---- positive result mask */
#define  _SIDD_NEGATIVE_POLARITY         0x10  /* --01---- negate result mask */
#define  _SIDD_MASKED_NEGATIVE_POLARITY  0x30  /* --11---- negate result mask only before end of string */
/* 6 Index Element */
#define  _SIDD_LEAST_SIGNIFICANT            0  /* -0------ (index only: return least significant mask element index) */
#define  _SIDD_MOST_SIGNIFICANT          0x40  /* -1------ (index only: return most  significant mask element index) */
/* 6 Mask Type */
#define  _SIDD_BIT_MASK                     0  /* -0------ mask only: return mask of single bits */
#define  _SIDD_UNIT_MASK                 0x40  /* -1------ mask only: return mask of 8-bit or 16-bit elements */
/* 7 unused - use 0 */


static inline vector bool char veclib_valid_chars_mask (vector unsigned char input);

static inline vector bool short veclib_valid_shorts_mask (vector unsigned short input);

/* Compare zero terminated strings in various ways, giving vector or bit mask */
VECLIB_INLINE __m128i vec_comparestringstomask1q (__m128i left, __m128i right, intlit8 control);

/* Compare length terminated strings in various ways, giving vector or bit mask */
VECLIB_INLINE __m128i vec_comparelengthstringstomask1q (__m128i left, intlit5 leftlen, __m128i right, intlit5 rightlen, intlit8 control);

/* Compare length terminated strings in various ways, giving index */
VECLIB_INLINE int vec_comparelengthstringstoindex1q (__m128i left, intlit5 leftlen, __m128i right, intlit5 rightlen, intlit8 control);

#endif

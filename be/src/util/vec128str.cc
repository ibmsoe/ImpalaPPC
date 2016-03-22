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

#include <altivec.h>
#include "veclib_types.h"
#include "vec128str.h"

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


static inline vector bool char veclib_valid_chars_mask (vector unsigned char input) {
  vector unsigned char zeros = (vector unsigned char) { 0, 0, 0, 0 };
  vector bool char invalid_mask = vec_cmpeq (input, zeros);  /* zero == 0xFF, other = 0x00 */
  /* remove any trailing invalid */
  vector unsigned char shift_count = vec_splats ((unsigned char) 8);
  int i;
  for (i = 0; i < 15; ++i) {
    #ifdef __LITTLE_ENDIAN__
      /* in little endian any invalid characters would be at the left end, so must shift the 0xFF for zero (if any) left */
      invalid_mask = vec_or (invalid_mask,
                             (vector bool char) vec_slo ((vector unsigned char) invalid_mask, shift_count));
    #elif __BIG_ENDIAN__
     /* in big endian any invalid characters would be at the right end, so must shift the 0xFF for zero (if any) right */
      invalid_mask = vec_or (invalid_mask,
                             (vector bool char) vec_sro ((vector unsigned char) invalid_mask, shift_count));
    #endif
  }
  return vec_nor (invalid_mask, invalid_mask);  /* return valid mask */
}


static inline vector bool short veclib_valid_shorts_mask (vector unsigned short input) {
  vector unsigned short zeros = (vector unsigned short) { 0, 0, 0, 0 };
  vector bool short invalid_mask = vec_cmpeq (input, zeros);  /* zero == 0xFFFF, other == 0x0000 */
  /* remove any trailing invalid */
  vector unsigned char shift_count = vec_splats ((unsigned char) 16);
  int i;
  for (i = 0; i < 7; ++i) {
    #ifdef __LITTLE_ENDIAN__
      /* in little endian any invalid characters would be at the left end, so must shift the 0xFFFF for zero (if any) left */
      invalid_mask = (vector bool short) vec_or ((vector unsigned short) invalid_mask,
                                                 (vector unsigned short) vec_slo ((vector unsigned char) invalid_mask, shift_count));
    #elif __BIG_ENDIAN__
      /* in big endian any invalid characters would be at the right end, so must shift the 0xFFFF for zero (if any) right */
      invalid_mask = (vector bool short) vec_or ((vector unsigned short) invalid_mask,
                                                 (vector unsigned short) vec_sro ((vector unsigned char) invalid_mask, shift_count));
    #endif
  }
  return vec_nor (invalid_mask, invalid_mask);  /* return valid mask */
}


/* Compare zero terminated strings in various ways, giving vector or bit mask */
VECLIB_INLINE __m128i vec_comparestringstomask1q (__m128i left, __m128i right, intlit8 control)
  /* See "String Control" above. */
  /* Except for ranges, a zero terminator and all characters after it are invalid.  Characters before that are valid. */
  /* _SIDD_CMP_EQUAL_ANY:     Each valid character in right is compared to every valid character in left.             */
  /*                          Result mask is true if it matches any of them, false if not or if it is invalid.        */
  /* _SIDD_CMP_RANGES:        Each valid character in right is compared to the two ranges in left.                    */
  /*                          Left contains four characters: lower bound 1, upper bound 1, lower 2, upper 2.          */
  /*                          Result mask is true if it is within either bound, false if not or if it is invalid.     */
  /* _SIDD_CMP_EQUAL_EACH:    Each valid character in right is compared to the corresponding valid character in left. */
  /*                          Result mask is true if they match, false if not or if either is invalid.                */
  /* _SIDD_CMP_EQUAL_ORDERED: Each sequence of valid characters in right is compared to the the valid string in left. */
  /*                          Result mask is true if the sequence starting at that character matches all of left,     */
  /*                          false if not or if it is invalid.                                                       */
{
  vector unsigned char zeros = (vector unsigned char) {0, 0, 0, 0};
  vector bool char result_mask;

  unsigned int operation = (control >> 2) & 0x3;  /* bits 3:2 */
  unsigned int element_type = control & 0x3;  /* bit 1:0 */

  /* create valid character mask excluding zero string terminator and any characters after it */
  vector bool char left_valid_byte_mask;
  vector bool char right_valid_byte_mask;
  vector bool char result_valid_byte_mask;
  vector unsigned char valid_left_unsigned;
  vector unsigned char valid_right_unsigned;
  vector signed char valid_left_signed;
  vector signed char valid_right_signed;

  /* clear invalid characters */
  if ((element_type & 0x1) == 0) {
    left_valid_byte_mask = veclib_valid_chars_mask (left);
    right_valid_byte_mask = veclib_valid_chars_mask (right);
  } else /* (element_type & 0x1) == 1 */ {
    left_valid_byte_mask  = (vector bool char) veclib_valid_shorts_mask ((vector unsigned short) left);
    right_valid_byte_mask = (vector bool char) veclib_valid_shorts_mask ((vector unsigned short) right);
  }
  
  /* truncate the last element in the left_valid_byte_mask if the length of it is odd if _SIDD_CMP_RANGES*/
  if (operation == 1) {
    /* _SIDD_CMP_RANGES */
    if ((element_type & 0x1) == 0) {
      vector unsigned short odd_length_compare_mask = vec_splats ((unsigned short) 0xFF);
      vector bool short truncate_odd_length_mask = vec_cmpgt ((vector unsigned short) left_valid_byte_mask, odd_length_compare_mask);
      left_valid_byte_mask = vec_and ((vector bool char) truncate_odd_length_mask, left_valid_byte_mask);
    } else /* (element_type & 0x1) == 1 */ {
      vector unsigned int odd_length_compare_mask = vec_splats ((unsigned int) 0xFFFF);
      vector bool int truncate_odd_length_mask = vec_cmpgt ((vector unsigned int) left_valid_byte_mask, odd_length_compare_mask);
      left_valid_byte_mask = vec_and ((vector bool char) truncate_odd_length_mask, left_valid_byte_mask);
    }
  }
  
  if (((element_type == 2) || (element_type == 3)) && ((operation == 1) || (operation == 3))) {
    /* signed */
    valid_left_signed  = vec_and ((vector signed char) left,  left_valid_byte_mask);
    valid_right_signed = vec_and ((vector signed char) right, right_valid_byte_mask);
  } else /* unsigned */ {
    valid_left_unsigned  = vec_and ((vector unsigned char) left,  left_valid_byte_mask);
    valid_right_unsigned = vec_and ((vector unsigned char) right, right_valid_byte_mask);
  }
  
  /* compare operation */
  if (operation == 0) {
    /* _SIDD_CMP_EQUAL_ANY */
    /* compare each valid character in right to every valid character in left */
    /* sign does not matter */
    if ((element_type & 0x1) == 0) {
      /* compare each valid char in right to every valid char in left */
      /* first rotate left to all positions */
      vector unsigned char left0  = (vector unsigned char) valid_left_unsigned;
      vector unsigned char left1  = vec_sld (left0, left0, 1);
      vector unsigned char left2  = vec_sld (left0, left0, 2);
      vector unsigned char left3  = vec_sld (left0, left0, 3);
      vector unsigned char left4  = vec_sld (left0, left0, 4);
      vector unsigned char left5  = vec_sld (left0, left0, 5);
      vector unsigned char left6  = vec_sld (left0, left0, 6);
      vector unsigned char left7  = vec_sld (left0, left0, 7);
      vector unsigned char left8  = vec_sld (left0, left0, 8);
      vector unsigned char left9  = vec_sld (left0, left0, 9);
      vector unsigned char left10 = vec_sld (left0, left0, 10);
      vector unsigned char left11 = vec_sld (left0, left0, 11);
      vector unsigned char left12 = vec_sld (left0, left0, 12);
      vector unsigned char left13 = vec_sld (left0, left0, 13);
      vector unsigned char left14 = vec_sld (left0, left0, 14);
      vector unsigned char left15 = vec_sld (left0, left0, 15);
      vector bool char match0  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left0);
      vector bool char match1  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left1);
      vector bool char match2  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left2);
      vector bool char match3  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left3);
      vector bool char match4  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left4);
      vector bool char match5  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left5);
      vector bool char match6  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left6);
      vector bool char match7  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left7);
      vector bool char match8  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left8);
      vector bool char match9  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left9);
      vector bool char match10 = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left10);
      vector bool char match11 = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left11);
      vector bool char match12 = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left12);
      vector bool char match13 = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left13);
      vector bool char match14 = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left14);
      vector bool char match15 = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left15);
      vector bool char match01   = vec_or (match0,  match1);
      vector bool char match23   = vec_or (match2,  match3);
      vector bool char match45   = vec_or (match4,  match5);
      vector bool char match67   = vec_or (match6,  match7);
      vector bool char match89   = vec_or (match8,  match9);
      vector bool char match1011 = vec_or (match10, match11);
      vector bool char match1213 = vec_or (match12, match13);
      vector bool char match1415 = vec_or (match14, match15);
      vector bool char match0to3   = vec_or (match01,   match23);
      vector bool char match4to7   = vec_or (match45,   match67);
      vector bool char match8to11  = vec_or (match89,   match1011);
      vector bool char match12to15 = vec_or (match1213, match1415);
      vector bool char match0to7  = vec_or (match0to3,  match4to7);
      vector bool char match8to15 = vec_or (match8to11, match12to15);
      vector bool char match0to15 = vec_or (match0to7, match8to15);
      result_mask = vec_and ((vector bool char) match0to15, right_valid_byte_mask);
    }

    else /* (element_type & 0x1) == 1 */ {
      /* compare each valid short in right to every valid short in left */
      /* first rotate left to all positions */
      vector unsigned short left0 = (vector unsigned short) valid_left_unsigned;
      vector unsigned short left1 = vec_sld (left0, left0, 2);
      vector unsigned short left2 = vec_sld (left0, left0, 4);
      vector unsigned short left3 = vec_sld (left0, left0, 6);
      vector unsigned short left4 = vec_sld (left0, left0, 8);
      vector unsigned short left5 = vec_sld (left0, left0, 10);
      vector unsigned short left6 = vec_sld (left0, left0, 12);
      vector unsigned short left7 = vec_sld (left0, left0, 14);
      vector bool short match0 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left0);
      vector bool short match1 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left1);
      vector bool short match2 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left2);
      vector bool short match3 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left3);
      vector bool short match4 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left4);
      vector bool short match5 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left5);
      vector bool short match6 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left6);
      vector bool short match7 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left7);
      vector bool short match01 = vec_or (match0, match1);
      vector bool short match23 = vec_or (match2, match3);
      vector bool short match45 = vec_or (match4, match5);
      vector bool short match67 = vec_or (match6, match7);
      vector bool short match0to3 = vec_or (match01, match23);
      vector bool short match4to7 = vec_or (match45, match67);
      vector bool short match0to7 = vec_or (match0to3, match4to7);
      result_mask = vec_and ((vector bool char) match0to7, right_valid_byte_mask);
    }
    result_valid_byte_mask = right_valid_byte_mask;
  }

  else if (operation == 1) {
    /* _SIDD_CMP_RANGES */
    /* check within range for each valid character in right to a pair of adjacent characters in left */
    /* sign does matter */
    if (element_type == 0) {
      /* unsigned char */

      /* splat each of the 16 elements to create 8 upper and lower bounds */
      vector unsigned char lower_bound0 = vec_splat ((vector unsigned char) valid_left_unsigned, 0);
      vector unsigned char upper_bound0 = vec_splat ((vector unsigned char) valid_left_unsigned, 1);
      vector unsigned char lower_bound1 = vec_splat ((vector unsigned char) valid_left_unsigned, 2);
      vector unsigned char upper_bound1 = vec_splat ((vector unsigned char) valid_left_unsigned, 3);
      vector unsigned char lower_bound2 = vec_splat ((vector unsigned char) valid_left_unsigned, 4);
      vector unsigned char upper_bound2 = vec_splat ((vector unsigned char) valid_left_unsigned, 5);
      vector unsigned char lower_bound3 = vec_splat ((vector unsigned char) valid_left_unsigned, 6);
      vector unsigned char upper_bound3 = vec_splat ((vector unsigned char) valid_left_unsigned, 7);
      vector unsigned char lower_bound4 = vec_splat ((vector unsigned char) valid_left_unsigned, 8);
      vector unsigned char upper_bound4 = vec_splat ((vector unsigned char) valid_left_unsigned, 9);
      vector unsigned char lower_bound5 = vec_splat ((vector unsigned char) valid_left_unsigned, 10);
      vector unsigned char upper_bound5 = vec_splat ((vector unsigned char) valid_left_unsigned, 11);
      vector unsigned char lower_bound6 = vec_splat ((vector unsigned char) valid_left_unsigned, 12);
      vector unsigned char upper_bound6 = vec_splat ((vector unsigned char) valid_left_unsigned, 13);
      vector unsigned char lower_bound7 = vec_splat ((vector unsigned char) valid_left_unsigned, 14);
      vector unsigned char upper_bound7 = vec_splat ((vector unsigned char) valid_left_unsigned, 15);
  
      /* compare valid_right_unsigned to each of the 8 upper and lower bounds */
      #ifdef __ibmxl__
        /* xlc */
        vector bool char cmplow0 = vec_cmpge (valid_right_unsigned, lower_bound0);
        vector bool char cmphi0 = vec_cmpge (upper_bound0, valid_right_unsigned);
        vector bool char cmplow1 = vec_cmpge (valid_right_unsigned, lower_bound1);
        vector bool char cmphi1 = vec_cmpge (upper_bound1, valid_right_unsigned);
        vector bool char cmplow2 = vec_cmpge (valid_right_unsigned, lower_bound2);
        vector bool char cmphi2 = vec_cmpge (upper_bound2, valid_right_unsigned);
        vector bool char cmplow3 = vec_cmpge (valid_right_unsigned, lower_bound3);
        vector bool char cmphi3 = vec_cmpge (upper_bound3, valid_right_unsigned);
        vector bool char cmplow4 = vec_cmpge (valid_right_unsigned, lower_bound4);
        vector bool char cmphi4 = vec_cmpge (upper_bound4, valid_right_unsigned);
        vector bool char cmplow5 = vec_cmpge (valid_right_unsigned, lower_bound5);
        vector bool char cmphi5 = vec_cmpge (upper_bound5, valid_right_unsigned);
        vector bool char cmplow6 = vec_cmpge (valid_right_unsigned, lower_bound6);
        vector bool char cmphi6 = vec_cmpge (upper_bound6, valid_right_unsigned);
        vector bool char cmplow7 = vec_cmpge (valid_right_unsigned, lower_bound7);
        vector bool char cmphi7 = vec_cmpge (upper_bound7, valid_right_unsigned);
      #else 
        /* gcc */
        vector bool char cmpgtlow0 = vec_cmpgt (valid_right_unsigned, lower_bound0);
        vector bool char cmpgthi0 = vec_cmpgt (upper_bound0, valid_right_unsigned);
        vector bool char cmpeqlow0 = vec_cmpeq (valid_right_unsigned, lower_bound0);
        vector bool char cmpeqhi0 = vec_cmpeq (upper_bound0, valid_right_unsigned);
        vector bool char cmplow0 = vec_or (cmpgtlow0, cmpeqlow0);
        vector bool char cmphi0 = vec_or (cmpgthi0, cmpeqhi0);
        
        vector bool char cmpgtlow1 = vec_cmpgt (valid_right_unsigned, lower_bound1);
        vector bool char cmpgthi1 = vec_cmpgt (upper_bound1, valid_right_unsigned);    
        vector bool char cmpeqlow1 = vec_cmpeq (valid_right_unsigned, lower_bound1);
        vector bool char cmpeqhi1 = vec_cmpeq (upper_bound1, valid_right_unsigned);
        vector bool char cmplow1 = vec_or (cmpgtlow1, cmpeqlow1);
        vector bool char cmphi1 = vec_or (cmpgthi1, cmpeqhi1);
        
        vector bool char cmpgtlow2 = vec_cmpgt (valid_right_unsigned, lower_bound2);
        vector bool char cmpgthi2 = vec_cmpgt (upper_bound2, valid_right_unsigned);
        vector bool char cmpeqlow2 = vec_cmpeq (valid_right_unsigned, lower_bound2);
        vector bool char cmpeqhi2 = vec_cmpeq (upper_bound2, valid_right_unsigned);
        vector bool char cmplow2 = vec_or (cmpgtlow2, cmpeqlow2);
        vector bool char cmphi2 = vec_or (cmpgthi2, cmpeqhi2);
        
        vector bool char cmpgtlow3 = vec_cmpgt (valid_right_unsigned, lower_bound3);
        vector bool char cmpgthi3 = vec_cmpgt (upper_bound3, valid_right_unsigned);    
        vector bool char cmpeqlow3 = vec_cmpeq (valid_right_unsigned, lower_bound3);
        vector bool char cmpeqhi3 = vec_cmpeq (upper_bound3, valid_right_unsigned);
        vector bool char cmplow3 = vec_or (cmpgtlow3, cmpeqlow3);
        vector bool char cmphi3 = vec_or (cmpgthi3, cmpeqhi3);
        
        vector bool char cmpgtlow4 = vec_cmpgt (valid_right_unsigned, lower_bound4);
        vector bool char cmpgthi4 = vec_cmpgt (upper_bound4, valid_right_unsigned);
        vector bool char cmpeqlow4 = vec_cmpeq (valid_right_unsigned, lower_bound4);
        vector bool char cmpeqhi4 = vec_cmpeq (upper_bound4, valid_right_unsigned);
        vector bool char cmplow4 = vec_or (cmpgtlow4, cmpeqlow4);
        vector bool char cmphi4 = vec_or (cmpgthi4, cmpeqhi4);
        
        vector bool char cmpgtlow5 = vec_cmpgt (valid_right_unsigned, lower_bound5);
        vector bool char cmpgthi5 = vec_cmpgt (upper_bound5, valid_right_unsigned);    
        vector bool char cmpeqlow5 = vec_cmpeq (valid_right_unsigned, lower_bound5);
        vector bool char cmpeqhi5 = vec_cmpeq (upper_bound5, valid_right_unsigned);
        vector bool char cmplow5 = vec_or (cmpgtlow5, cmpeqlow5);
        vector bool char cmphi5 = vec_or (cmpgthi5, cmpeqhi5);
        
        vector bool char cmpgtlow6 = vec_cmpgt (valid_right_unsigned, lower_bound6);
        vector bool char cmpgthi6 = vec_cmpgt (upper_bound6, valid_right_unsigned);
        vector bool char cmpeqlow6 = vec_cmpeq (valid_right_unsigned, lower_bound6);
        vector bool char cmpeqhi6 = vec_cmpeq (upper_bound6, valid_right_unsigned);
        vector bool char cmplow6 = vec_or (cmpgtlow6, cmpeqlow6);
        vector bool char cmphi6 = vec_or (cmpgthi6, cmpeqhi6);
        
        vector bool char cmpgtlow7 = vec_cmpgt (valid_right_unsigned, lower_bound7);
        vector bool char cmpgthi7 = vec_cmpgt (upper_bound7, valid_right_unsigned);    
        vector bool char cmpeqlow7 = vec_cmpeq (valid_right_unsigned, lower_bound7);
        vector bool char cmpeqhi7 = vec_cmpeq (upper_bound7, valid_right_unsigned);
        vector bool char cmplow7 = vec_or (cmpgtlow7, cmpeqlow7);
        vector bool char cmphi7 = vec_or (cmpgthi7, cmpeqhi7);
      #endif
      
      /* AND the result of all the comparisons for within range compares */
      vector bool char range0 = vec_and (cmplow0, cmphi0);
      vector bool char range1 = vec_and (cmplow1, cmphi1);
      vector bool char range2 = vec_and (cmplow2, cmphi2);
      vector bool char range3 = vec_and (cmplow3, cmphi3);
      vector bool char range4 = vec_and (cmplow4, cmphi4);
      vector bool char range5 = vec_and (cmplow5, cmphi5);
      vector bool char range6 = vec_and (cmplow6, cmphi6);
      vector bool char range7 = vec_and (cmplow7, cmphi7);
      
      /* OR the result of range compares */
      vector bool char range01 = vec_or (range0, range1);
      vector bool char range23 = vec_or (range2, range3);
      vector bool char range45 = vec_or (range4, range5);
      vector bool char range67 = vec_or (range6, range7);
      vector bool char range0to3 = vec_or (range01, range23);
      vector bool char range4to7 = vec_or (range45, range67);
      vector bool char range0to7 = vec_or (range0to3, range4to7);
      
      result_mask = vec_and (range0to7, right_valid_byte_mask);
    }
    else if (element_type == 1) {
      /* unsigned short */
      
      /* splat each of the 16 elements to create 8 upper and lower bounds */
      vector unsigned short lower_bound0 = vec_splat ((vector unsigned short) valid_left_unsigned, 0);
      vector unsigned short upper_bound0 = vec_splat ((vector unsigned short) valid_left_unsigned, 1);
      vector unsigned short lower_bound1 = vec_splat ((vector unsigned short) valid_left_unsigned, 2);
      vector unsigned short upper_bound1 = vec_splat ((vector unsigned short) valid_left_unsigned, 3);
      vector unsigned short lower_bound2 = vec_splat ((vector unsigned short) valid_left_unsigned, 4);
      vector unsigned short upper_bound2 = vec_splat ((vector unsigned short) valid_left_unsigned, 5);
      vector unsigned short lower_bound3 = vec_splat ((vector unsigned short) valid_left_unsigned, 6);
      vector unsigned short upper_bound3 = vec_splat ((vector unsigned short) valid_left_unsigned, 7);

      /* compare valid_right_unsigned to each of the 8 upper and lower bounds */
      #ifdef __ibmxl__
        /* xlc */
        vector bool short cmplow0 = vec_cmpge ((vector unsigned short) (vector unsigned short) valid_right_unsigned, lower_bound0);
        vector bool short cmphi0 = vec_cmpge (upper_bound0, (vector unsigned short) (vector unsigned short) valid_right_unsigned);
        vector bool short cmplow1 = vec_cmpge ((vector unsigned short) valid_right_unsigned, lower_bound1);
        vector bool short cmphi1 = vec_cmpge (upper_bound1, (vector unsigned short) valid_right_unsigned);
        vector bool short cmplow2 = vec_cmpge ((vector unsigned short) valid_right_unsigned, lower_bound2);
        vector bool short cmphi2 = vec_cmpge (upper_bound2, (vector unsigned short) valid_right_unsigned);
        vector bool short cmplow3 = vec_cmpge ((vector unsigned short) valid_right_unsigned, lower_bound3);
        vector bool short cmphi3 = vec_cmpge (upper_bound3, (vector unsigned short) valid_right_unsigned);
      #else 
        /* gcc */
        vector bool short cmpgtlow0 = vec_cmpgt ((vector unsigned short) valid_right_unsigned, lower_bound0);
        vector bool short cmpgthi0 = vec_cmpgt (upper_bound0, (vector unsigned short) valid_right_unsigned);
        vector bool short cmpeqlow0 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, lower_bound0);
        vector bool short cmpeqhi0 = vec_cmpeq (upper_bound0, (vector unsigned short) valid_right_unsigned);
        vector bool short cmplow0 = vec_or (cmpgtlow0, cmpeqlow0);
        vector bool short cmphi0 = vec_or (cmpgthi0, cmpeqhi0);
        
        vector bool short cmpgtlow1 = vec_cmpgt ((vector unsigned short) valid_right_unsigned, lower_bound1);
        vector bool short cmpgthi1 = vec_cmpgt (upper_bound1, (vector unsigned short) valid_right_unsigned);    
        vector bool short cmpeqlow1 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, lower_bound1);
        vector bool short cmpeqhi1 = vec_cmpeq (upper_bound1, (vector unsigned short) valid_right_unsigned);
        vector bool short cmplow1 = vec_or (cmpgtlow1, cmpeqlow1);
        vector bool short cmphi1 = vec_or (cmpgthi1, cmpeqhi1);
        
        vector bool short cmpgtlow2 = vec_cmpgt ((vector unsigned short) valid_right_unsigned, lower_bound2);
        vector bool short cmpgthi2 = vec_cmpgt (upper_bound2, (vector unsigned short) valid_right_unsigned);
        vector bool short cmpeqlow2 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, lower_bound2);
        vector bool short cmpeqhi2 = vec_cmpeq (upper_bound2, (vector unsigned short) valid_right_unsigned);
        vector bool short cmplow2 = vec_or (cmpgtlow2, cmpeqlow2);
        vector bool short cmphi2 = vec_or (cmpgthi2, cmpeqhi2);
        
        vector bool short cmpgtlow3 = vec_cmpgt ((vector unsigned short) valid_right_unsigned, lower_bound3);
        vector bool short cmpgthi3 = vec_cmpgt (upper_bound3, (vector unsigned short) valid_right_unsigned);    
        vector bool short cmpeqlow3 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, lower_bound3);
        vector bool short cmpeqhi3 = vec_cmpeq (upper_bound3, (vector unsigned short) valid_right_unsigned);
        vector bool short cmplow3 = vec_or (cmpgtlow3, cmpeqlow3);
        vector bool short cmphi3 = vec_or (cmpgthi3, cmpeqhi3);
      #endif
      
      /* AND the result of all the comparisons for within range compares */
      vector bool short range0 = vec_and (cmplow0, cmphi0);
      vector bool short range1 = vec_and (cmplow1, cmphi1);
      vector bool short range2 = vec_and (cmplow2, cmphi2);
      vector bool short range3 = vec_and (cmplow3, cmphi3);
      
      /* OR the result of range compares */
      vector bool short range01 = vec_or (range0, range1);
      vector bool short range23 = vec_or (range2, range3);
      vector bool short range0to3 = vec_or (range01, range23);
      
      result_mask = vec_and((vector bool char) range0to3, right_valid_byte_mask);
    }
    else if (element_type == 2) {
      /* signed char */
      
      /* splat each of the 16 elements to create 8 upper and lower bounds */
      vector signed char lower_bound0 = vec_splat ((vector signed char) valid_left_signed, 0);
      vector signed char upper_bound0 = vec_splat ((vector signed char) valid_left_signed, 1);
      vector signed char lower_bound1 = vec_splat ((vector signed char) valid_left_signed, 2);
      vector signed char upper_bound1 = vec_splat ((vector signed char) valid_left_signed, 3);
      vector signed char lower_bound2 = vec_splat ((vector signed char) valid_left_signed, 4);
      vector signed char upper_bound2 = vec_splat ((vector signed char) valid_left_signed, 5);
      vector signed char lower_bound3 = vec_splat ((vector signed char) valid_left_signed, 6);
      vector signed char upper_bound3 = vec_splat ((vector signed char) valid_left_signed, 7);
      vector signed char lower_bound4 = vec_splat ((vector signed char) valid_left_signed, 8);
      vector signed char upper_bound4 = vec_splat ((vector signed char) valid_left_signed, 9);
      vector signed char lower_bound5 = vec_splat ((vector signed char) valid_left_signed, 10);
      vector signed char upper_bound5 = vec_splat ((vector signed char) valid_left_signed, 11);
      vector signed char lower_bound6 = vec_splat ((vector signed char) valid_left_signed, 12);
      vector signed char upper_bound6 = vec_splat ((vector signed char) valid_left_signed, 13);
      vector signed char lower_bound7 = vec_splat ((vector signed char) valid_left_signed, 14);
      vector signed char upper_bound7 = vec_splat ((vector signed char) valid_left_signed, 15);
      
      /* compare valid_right_signed to each of the 8 upper and lower bounds */
      #ifdef __ibmxl__
        /* xlc */
        vector bool char cmplow0 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound0);
        vector bool char cmphi0 = vec_cmpge (upper_bound0, (vector signed char) valid_right_signed);
        vector bool char cmplow1 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound1);
        vector bool char cmphi1 = vec_cmpge (upper_bound1, (vector signed char) valid_right_signed);
        vector bool char cmplow2 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound2);
        vector bool char cmphi2 = vec_cmpge (upper_bound2, (vector signed char) valid_right_signed);
        vector bool char cmplow3 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound3);
        vector bool char cmphi3 = vec_cmpge (upper_bound3, (vector signed char) valid_right_signed);
        vector bool char cmplow4 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound4);
        vector bool char cmphi4 = vec_cmpge (upper_bound4, (vector signed char) valid_right_signed);
        vector bool char cmplow5 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound5);
        vector bool char cmphi5 = vec_cmpge (upper_bound5, (vector signed char) valid_right_signed);
        vector bool char cmplow6 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound6);
        vector bool char cmphi6 = vec_cmpge (upper_bound6, (vector signed char) valid_right_signed);
        vector bool char cmplow7 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound7);
        vector bool char cmphi7 = vec_cmpge (upper_bound7, (vector signed char) valid_right_signed);
      #else 
        /* gcc */
        vector bool char cmpgtlow0 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound0);
        vector bool char cmpgthi0 = vec_cmpgt (upper_bound0, (vector signed char) valid_right_signed);
        vector bool char cmpeqlow0 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound0);
        vector bool char cmpeqhi0 = vec_cmpeq (upper_bound0, (vector signed char) valid_right_signed);
        vector bool char cmplow0 = vec_or (cmpgtlow0, cmpeqlow0);
        vector bool char cmphi0 = vec_or (cmpgthi0, cmpeqhi0);
        
        vector bool char cmpgtlow1 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound1);
        vector bool char cmpgthi1 = vec_cmpgt (upper_bound1, (vector signed char) valid_right_signed);    
        vector bool char cmpeqlow1 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound1);
        vector bool char cmpeqhi1 = vec_cmpeq (upper_bound1, (vector signed char) valid_right_signed);
        vector bool char cmplow1 = vec_or (cmpgtlow1, cmpeqlow1);
        vector bool char cmphi1 = vec_or (cmpgthi1, cmpeqhi1);
        
        vector bool char cmpgtlow2 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound2);
        vector bool char cmpgthi2 = vec_cmpgt (upper_bound2, (vector signed char) valid_right_signed);
        vector bool char cmpeqlow2 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound2);
        vector bool char cmpeqhi2 = vec_cmpeq (upper_bound2, (vector signed char) valid_right_signed);
        vector bool char cmplow2 = vec_or (cmpgtlow2, cmpeqlow2);
        vector bool char cmphi2 = vec_or (cmpgthi2, cmpeqhi2);
        
        vector bool char cmpgtlow3 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound3);
        vector bool char cmpgthi3 = vec_cmpgt (upper_bound3, (vector signed char) valid_right_signed);    
        vector bool char cmpeqlow3 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound3);
        vector bool char cmpeqhi3 = vec_cmpeq (upper_bound3, (vector signed char) valid_right_signed);
        vector bool char cmplow3 = vec_or (cmpgtlow3, cmpeqlow3);
        vector bool char cmphi3 = vec_or (cmpgthi3, cmpeqhi3);
        
        vector bool char cmpgtlow4 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound4);
        vector bool char cmpgthi4 = vec_cmpgt (upper_bound4, (vector signed char) valid_right_signed);
        vector bool char cmpeqlow4 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound4);
        vector bool char cmpeqhi4 = vec_cmpeq (upper_bound4, (vector signed char) valid_right_signed);
        vector bool char cmplow4 = vec_or (cmpgtlow4, cmpeqlow4);
        vector bool char cmphi4 = vec_or (cmpgthi4, cmpeqhi4);
        
        vector bool char cmpgtlow5 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound5);
        vector bool char cmpgthi5 = vec_cmpgt (upper_bound5, (vector signed char) valid_right_signed);    
        vector bool char cmpeqlow5 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound5);
        vector bool char cmpeqhi5 = vec_cmpeq (upper_bound5, (vector signed char) valid_right_signed);
        vector bool char cmplow5 = vec_or (cmpgtlow5, cmpeqlow5);
        vector bool char cmphi5 = vec_or (cmpgthi5, cmpeqhi5);
        
        vector bool char cmpgtlow6 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound6);
        vector bool char cmpgthi6 = vec_cmpgt (upper_bound6, (vector signed char) valid_right_signed);
        vector bool char cmpeqlow6 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound6);
        vector bool char cmpeqhi6 = vec_cmpeq (upper_bound6, (vector signed char) valid_right_signed);
        vector bool char cmplow6 = vec_or (cmpgtlow6, cmpeqlow6);
        vector bool char cmphi6 = vec_or (cmpgthi6, cmpeqhi6);
        
        vector bool char cmpgtlow7 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound7);
        vector bool char cmpgthi7 = vec_cmpgt (upper_bound7, (vector signed char) valid_right_signed);    
        vector bool char cmpeqlow7 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound7);
        vector bool char cmpeqhi7 = vec_cmpeq (upper_bound7, (vector signed char) valid_right_signed);
        vector bool char cmplow7 = vec_or (cmpgtlow7, cmpeqlow7);
        vector bool char cmphi7 = vec_or (cmpgthi7, cmpeqhi7);
      #endif
      
      /* AND the result of all the comparisons for within range compares */
      vector bool char range0 = vec_and (cmplow0, cmphi0);
      vector bool char range1 = vec_and (cmplow1, cmphi1);
      vector bool char range2 = vec_and (cmplow2, cmphi2);
      vector bool char range3 = vec_and (cmplow3, cmphi3);
      vector bool char range4 = vec_and (cmplow4, cmphi4);
      vector bool char range5 = vec_and (cmplow5, cmphi5);
      vector bool char range6 = vec_and (cmplow6, cmphi6);
      vector bool char range7 = vec_and (cmplow7, cmphi7);
      
      /* OR the result of range compares */
      vector bool char range01 = vec_or (range0, range1);
      vector bool char range23 = vec_or (range2, range3);
      vector bool char range45 = vec_or (range4, range5);
      vector bool char range67 = vec_or (range6, range7);
      vector bool char range0to3 = vec_or (range01, range23);
      vector bool char range4to7 = vec_or (range45, range67);
      vector bool char range0to7 = vec_or (range0to3, range4to7);
      
      result_mask = vec_and (range0to7, right_valid_byte_mask);
    }
    else if (element_type == 3) {
      /* signed short */
      
      /* splat each of the 16 elements to create 8 upper and lower bounds */
      vector signed short lower_bound0 = vec_splat ((vector signed short) valid_left_signed, 0);
      vector signed short upper_bound0 = vec_splat ((vector signed short) valid_left_signed, 1);
      vector signed short lower_bound1 = vec_splat ((vector signed short) valid_left_signed, 2);
      vector signed short upper_bound1 = vec_splat ((vector signed short) valid_left_signed, 3);
      vector signed short lower_bound2 = vec_splat ((vector signed short) valid_left_signed, 4);
      vector signed short upper_bound2 = vec_splat ((vector signed short) valid_left_signed, 5);
      vector signed short lower_bound3 = vec_splat ((vector signed short) valid_left_signed, 6);
      vector signed short upper_bound3 = vec_splat ((vector signed short) valid_left_signed, 7);

      /* compare valid_right_signed to each of the 8 upper and lower bounds */
      #ifdef __ibmxl__
        /* xlc */
        vector bool short cmplow0 = vec_cmpge ((vector signed short) valid_right_signed, lower_bound0);
        vector bool short cmphi0 = vec_cmpge (upper_bound0, (vector signed short) valid_right_signed);
        vector bool short cmplow1 = vec_cmpge ((vector signed short) valid_right_signed, lower_bound1);
        vector bool short cmphi1 = vec_cmpge (upper_bound1, (vector signed short) valid_right_signed);
        vector bool short cmplow2 = vec_cmpge ((vector signed short) valid_right_signed, lower_bound2);
        vector bool short cmphi2 = vec_cmpge (upper_bound2, (vector signed short) valid_right_signed);
        vector bool short cmplow3 = vec_cmpge ((vector signed short) valid_right_signed, lower_bound3);
        vector bool short cmphi3 = vec_cmpge (upper_bound3, (vector signed short) valid_right_signed);
      #else 
        /* gcc */
        vector bool short cmpgtlow0 = vec_cmpgt ((vector signed short) valid_right_signed, lower_bound0);
        vector bool short cmpgthi0 = vec_cmpgt (upper_bound0, (vector signed short) valid_right_signed);
        vector bool short cmpeqlow0 = vec_cmpeq ((vector signed short) valid_right_signed, lower_bound0);
        vector bool short cmpeqhi0 = vec_cmpeq (upper_bound0, (vector signed short) valid_right_signed);
        vector bool short cmplow0 = vec_or (cmpgtlow0, cmpeqlow0);
        vector bool short cmphi0 = vec_or (cmpgthi0, cmpeqhi0);
        
        vector bool short cmpgtlow1 = vec_cmpgt ((vector signed short) valid_right_signed, lower_bound1);
        vector bool short cmpgthi1 = vec_cmpgt (upper_bound1, (vector signed short) valid_right_signed);    
        vector bool short cmpeqlow1 = vec_cmpeq ((vector signed short) valid_right_signed, lower_bound1);
        vector bool short cmpeqhi1 = vec_cmpeq (upper_bound1, (vector signed short) valid_right_signed);
        vector bool short cmplow1 = vec_or (cmpgtlow1, cmpeqlow1);
        vector bool short cmphi1 = vec_or (cmpgthi1, cmpeqhi1);
        
        vector bool short cmpgtlow2 = vec_cmpgt ((vector signed short) valid_right_signed, lower_bound2);
        vector bool short cmpgthi2 = vec_cmpgt (upper_bound2, (vector signed short) valid_right_signed);
        vector bool short cmpeqlow2 = vec_cmpeq ((vector signed short) valid_right_signed, lower_bound2);
        vector bool short cmpeqhi2 = vec_cmpeq (upper_bound2, (vector signed short) valid_right_signed);
        vector bool short cmplow2 = vec_or (cmpgtlow2, cmpeqlow2);
        vector bool short cmphi2 = vec_or (cmpgthi2, cmpeqhi2);
        
        vector bool short cmpgtlow3 = vec_cmpgt ((vector signed short) valid_right_signed, lower_bound3);
        vector bool short cmpgthi3 = vec_cmpgt (upper_bound3, (vector signed short) valid_right_signed);    
        vector bool short cmpeqlow3 = vec_cmpeq ((vector signed short) valid_right_signed, lower_bound3);
        vector bool short cmpeqhi3 = vec_cmpeq (upper_bound3, (vector signed short) valid_right_signed);
        vector bool short cmplow3 = vec_or (cmpgtlow3, cmpeqlow3);
        vector bool short cmphi3 = vec_or (cmpgthi3, cmpeqhi3);
      #endif
      
      /* AND the result of all the comparisons for within range compares */
      vector bool short range0 = vec_and (cmplow0, cmphi0);
      vector bool short range1 = vec_and (cmplow1, cmphi1);
      vector bool short range2 = vec_and (cmplow2, cmphi2);
      vector bool short range3 = vec_and (cmplow3, cmphi3);
      
      /* OR the result of range compares */
      vector bool short range01 = vec_or (range0, range1);
      vector bool short range23 = vec_or (range2, range3);
      vector bool short range0to3 = vec_or (range01, range23);
      
      result_mask = vec_and((vector bool char) range0to3, right_valid_byte_mask);
    }
    result_valid_byte_mask = right_valid_byte_mask;
  }

  else if (operation == 2) {
    /* _SIDD_CMP_EQUAL_EACH */
    /* compare each valid character in right equal to corresponding valid character in left */

    if ((element_type & 0x1) == 0) {
    /* signed char or unsigned char */
      result_mask = vec_cmpeq ((vector unsigned char) valid_left_unsigned, (vector unsigned char) valid_right_unsigned);
    }

    else /* (element_type & 0x1) == 1 */ {
      /* signed short or unsigned short */
      result_mask = (vector bool char) vec_cmpeq ((vector unsigned short) valid_left_unsigned, (vector unsigned short) valid_right_unsigned);
    }
    result_valid_byte_mask = right_valid_byte_mask;
  }
  
  else /* operation == 3 */ {
    /* _SIDD_CMP_EQUAL_ORDERED */
    /* compare each valid character in right to start of valid substring in left */
    /* sign does matter */
    #ifdef __LITTLE_ENDIAN__
      if (element_type == 0) {
        /* unsigned char */
        
        /* shift valid_right_unsigned to right to all positions */
        vector unsigned char valid_right_shift_right0 = valid_right_unsigned;
        vector unsigned char valid_right_shift_right1 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x8));
        vector unsigned char valid_right_shift_right2 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x10));
        vector unsigned char valid_right_shift_right3 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x18));
        vector unsigned char valid_right_shift_right4 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x20));
        vector unsigned char valid_right_shift_right5 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x28));
        vector unsigned char valid_right_shift_right6 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x30));
        vector unsigned char valid_right_shift_right7 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x38));
        vector unsigned char valid_right_shift_right8 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x40));
        vector unsigned char valid_right_shift_right9 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x48));
        vector unsigned char valid_right_shift_right10 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x50));
        vector unsigned char valid_right_shift_right11 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x58));
        vector unsigned char valid_right_shift_right12 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x60));
        vector unsigned char valid_right_shift_right13 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x68));
        vector unsigned char valid_right_shift_right14 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x70));
        vector unsigned char valid_right_shift_right15 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x78));
        
        /* compare valid_right_shift_right0..15 to valid_left_unsigned */
        vector bool char compare_valid_left_shift_right0 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right0);
        vector bool char compare_valid_left_shift_right1 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right1);
        vector bool char compare_valid_left_shift_right2 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right2);
        vector bool char compare_valid_left_shift_right3 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right3);
        vector bool char compare_valid_left_shift_right4 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right4);
        vector bool char compare_valid_left_shift_right5 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right5);
        vector bool char compare_valid_left_shift_right6 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right6);
        vector bool char compare_valid_left_shift_right7 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right7);
        vector bool char compare_valid_left_shift_right8 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right8);
        vector bool char compare_valid_left_shift_right9 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right9);
        vector bool char compare_valid_left_shift_right10 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right10);
        vector bool char compare_valid_left_shift_right11 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right11);
        vector bool char compare_valid_left_shift_right12 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right12);
        vector bool char compare_valid_left_shift_right13 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right13);
        vector bool char compare_valid_left_shift_right14 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right14);
        vector bool char compare_valid_left_shift_right15 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right15);
        
        /* and compare_valid_left_shift_right0..15 with left_valid_byte_mask to clear invalid bytes */
        compare_valid_left_shift_right0 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right0);
        compare_valid_left_shift_right1 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right1);
        compare_valid_left_shift_right2 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right2);
        compare_valid_left_shift_right3 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right3);
        compare_valid_left_shift_right4 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right4);
        compare_valid_left_shift_right5 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right5);
        compare_valid_left_shift_right6 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right6);
        compare_valid_left_shift_right7 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right7);
        compare_valid_left_shift_right8 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right8);
        compare_valid_left_shift_right9 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right9);
        compare_valid_left_shift_right10 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right10);
        compare_valid_left_shift_right11 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right11);
        compare_valid_left_shift_right12 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right12);
        compare_valid_left_shift_right13 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right13);
        compare_valid_left_shift_right14 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right14);
        compare_valid_left_shift_right15 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right15);
        
        /* gather bits by bytes compare_valid_left_shift_right0..15 */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb0 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right7);
          vector unsigned char gbb8 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right8);
          vector unsigned char gbb9 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right9);
          vector unsigned char gbb10 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right10);
          vector unsigned char gbb11 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right11);
          vector unsigned char gbb12 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right12);
          vector unsigned char gbb13 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right13);
          vector unsigned char gbb14 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right14);
          vector unsigned char gbb15 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right15);
        #else
          /* gcc */
          vector unsigned char gbb0 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right7);
          vector unsigned char gbb8 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right8);
          vector unsigned char gbb9 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right9);
          vector unsigned char gbb10 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right10);
          vector unsigned char gbb11 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right11);
          vector unsigned char gbb12 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right12);
          vector unsigned char gbb13 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right13);
          vector unsigned char gbb14 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right14);
          vector unsigned char gbb15 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right15);
        #endif
        
        /* permute gbb0-15 to put the 0th and 8th bytes together and pack them in binary tree order to enable parallelism */
        vector unsigned char gbb_permute_mask01 = (vector unsigned char) { 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask23 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask45 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask67 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18 };
        vector unsigned char gbb_permute_mask89 = (vector unsigned char) { 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask1011 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask1213 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask1415 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18 };
        vector unsigned char gbb_permute_mask0to3 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x14, 0x15, 0x16,0x17, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask4to7 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x08,0x09, 0x0A,0x0B, 0x1C, 0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask8to11 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x14, 0x15, 0x16,0x17, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask12to15 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x08,0x09, 0x0A,0x0B, 0x1C, 0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask0to7 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x04, 0x05, 0x06,0x07, 0x18,0x19, 0x1A,0x1B, 0x1C,0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask8to15 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x04, 0x05, 0x06,0x07, 0x18,0x19, 0x1A,0x1B, 0x1C,0x1D, 0x1E,0x1F };
        vector unsigned char pack_gbb01 = vec_perm (gbb0, gbb1, gbb_permute_mask01);
        vector unsigned char pack_gbb23 = vec_perm (gbb2, gbb3, gbb_permute_mask23);
        vector unsigned char pack_gbb45 = vec_perm (gbb4, gbb5, gbb_permute_mask45);
        vector unsigned char pack_gbb67 = vec_perm (gbb6, gbb7, gbb_permute_mask67);
        vector unsigned char pack_gbb89 = vec_perm (gbb8, gbb9, gbb_permute_mask89);
        vector unsigned char pack_gbb1011 = vec_perm (gbb10, gbb11, gbb_permute_mask1011);
        vector unsigned char pack_gbb1213 = vec_perm (gbb12, gbb13, gbb_permute_mask1213);
        vector unsigned char pack_gbb1415 = vec_perm (gbb14, gbb15, gbb_permute_mask1415);
        vector unsigned char pack_gbb0to3 = vec_perm (pack_gbb01, pack_gbb23, gbb_permute_mask0to3);
        vector unsigned char pack_gbb4to7 = vec_perm (pack_gbb45, pack_gbb67, gbb_permute_mask4to7);
        vector unsigned char pack_gbb8to11 = vec_perm (pack_gbb89, pack_gbb1011, gbb_permute_mask8to11);
        vector unsigned char pack_gbb12to15 = vec_perm (pack_gbb1213, pack_gbb1415, gbb_permute_mask12to15);
        vector unsigned char pack_gbb0to7 = vec_perm (pack_gbb0to3, pack_gbb4to7, gbb_permute_mask0to7);
        vector unsigned char pack_gbb8to15 = vec_perm (pack_gbb8to11, pack_gbb12to15, gbb_permute_mask8to15);
        
        /* calculate mask for substring match */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb_substring = (vector unsigned char) vec_gbb ((vector unsigned long long) left_valid_byte_mask);
        #else 
          /* gcc */
          vector unsigned char gbb_substring = vec_vgbbd ((vector unsigned char) left_valid_byte_mask);
        #endif
        /* puts the byte 0 of gbb_substring into even bytes and byte 8 into odd bytes */
        vector unsigned char substring_permute_mask = (vector unsigned char) { 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08 };
        vector unsigned char substring_mask = vec_perm (gbb_substring, gbb_substring, substring_permute_mask);
        
        /* compare substring_mask to pack_gbb0to7 and pack_gbb8to15 to find matches */
        vector bool short compare_gbb0to7 = vec_cmpeq ((vector unsigned short) pack_gbb0to7, (vector unsigned short) substring_mask);
        vector bool short compare_gbb8to15 = vec_cmpeq ((vector unsigned short) pack_gbb8to15, (vector unsigned short) substring_mask);
        
        /* turn compare_gbb0to7 and compare_gbb8to15 into a mask of 8 bits per element */
        vector bool char compare_gbb0to15 = vec_pack ((vector bool short) compare_gbb0to7, (vector bool short) compare_gbb8to15);
        
        result_mask = compare_gbb0to15;
      }
      else if (element_type == 1) {
        /* unsigned short */
        
        /* shift valid_right_unsigned to right to all positions */
        vector unsigned short valid_right_shift_right0 = (vector unsigned short) valid_right_unsigned;
        vector unsigned short valid_right_shift_right1 = vec_sro ((vector unsigned short) valid_right_unsigned, vec_splats((unsigned char) 0x10));
        vector unsigned short valid_right_shift_right2 = vec_sro ((vector unsigned short) valid_right_unsigned, vec_splats((unsigned char) 0x20));
        vector unsigned short valid_right_shift_right3 = vec_sro ((vector unsigned short) valid_right_unsigned, vec_splats((unsigned char) 0x30));
        vector unsigned short valid_right_shift_right4 = vec_sro ((vector unsigned short) valid_right_unsigned, vec_splats((unsigned char) 0x40));
        vector unsigned short valid_right_shift_right5 = vec_sro ((vector unsigned short) valid_right_unsigned, vec_splats((unsigned char) 0x50));
        vector unsigned short valid_right_shift_right6 = vec_sro ((vector unsigned short) valid_right_unsigned, vec_splats((unsigned char) 0x60));
        vector unsigned short valid_right_shift_right7 = vec_sro ((vector unsigned short) valid_right_unsigned, vec_splats((unsigned char) 0x70));
        
        /* compare valid_right_shift_right0..7 to valid_left_unsigned */
        vector bool short compare_valid_left_shift_right0 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right0);
        vector bool short compare_valid_left_shift_right1 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right1);
        vector bool short compare_valid_left_shift_right2 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right2);
        vector bool short compare_valid_left_shift_right3 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right3);
        vector bool short compare_valid_left_shift_right4 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right4);
        vector bool short compare_valid_left_shift_right5 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right5);
        vector bool short compare_valid_left_shift_right6 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right6);
        vector bool short compare_valid_left_shift_right7 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right7);
        
        /* and compare_valid_left_shift_right0..7 with left_valid_byte_mask to clear invalid bytes */
        compare_valid_left_shift_right0 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right0);
        compare_valid_left_shift_right1 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right1);
        compare_valid_left_shift_right2 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right2);
        compare_valid_left_shift_right3 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right3);
        compare_valid_left_shift_right4 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right4);
        compare_valid_left_shift_right5 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right5);
        compare_valid_left_shift_right6 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right6);
        compare_valid_left_shift_right7 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right7);
        
        /* gather bits by bytes compare_valid_left_shift_right0..7 */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb0 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right7);
        #else
          /* gcc */
          vector unsigned char gbb0 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right7);
        #endif
        
        /* permute gbb0-7 to put the 0th and 4th shorts together and pack them in binary tree order to enable parallelism */

        vector unsigned char gbb_permute_mask01 = (vector unsigned char) { 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask23 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask45 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask67 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18 };
        vector unsigned char gbb_permute_mask0to3 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x14, 0x15, 0x16,0x17, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask4to7 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x08,0x09, 0x0A,0x0B, 0x1C, 0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask0to7 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x04, 0x05, 0x06,0x07, 0x18,0x19, 0x1A,0x1B, 0x1C,0x1D, 0x1E,0x1F };
        vector unsigned char pack_gbb01 = vec_perm (gbb0, gbb1, gbb_permute_mask01);
        vector unsigned char pack_gbb23 = vec_perm (gbb2, gbb3, gbb_permute_mask23);
        vector unsigned char pack_gbb45 = vec_perm (gbb4, gbb5, gbb_permute_mask45);
        vector unsigned char pack_gbb67 = vec_perm (gbb6, gbb7, gbb_permute_mask67);
        vector unsigned char pack_gbb0to3 = vec_perm (pack_gbb01, pack_gbb23, gbb_permute_mask0to3);
        vector unsigned char pack_gbb4to7 = vec_perm (pack_gbb45, pack_gbb67, gbb_permute_mask4to7);
        vector unsigned char pack_gbb0to7 = vec_perm (pack_gbb0to3, pack_gbb4to7, gbb_permute_mask0to7);
        
        /* calculate mask for substring match */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb_substring = (vector unsigned char) vec_gbb ((vector unsigned long long) left_valid_byte_mask);
        #else 
          /* gcc */
          vector unsigned char gbb_substring = vec_vgbbd ((vector unsigned char) left_valid_byte_mask);
        #endif
        /* puts the byte 0 of gbb_substring into even bytes and byte 8 into odd bytes */
        vector unsigned char substring_permute_mask = (vector unsigned char) { 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08 };
        vector unsigned char substring_mask = vec_perm (gbb_substring, gbb_substring, substring_permute_mask);
        
        /* compare substring_mask to pack_gbb0to7 to find matches */
        vector bool short compare_gbb0to7 = vec_cmpeq ((vector unsigned short) pack_gbb0to7, (vector unsigned short) substring_mask);

        result_mask = (vector bool char) compare_gbb0to7;
      }
      if (element_type == 2) {
        /* signed char */
        
        /* shift valid_right_unsigned to right to all positions */
        vector signed char valid_right_shift_right0 = (vector signed char) valid_right_signed;
        vector signed char valid_right_shift_right1 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x8));
        vector signed char valid_right_shift_right2 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x10));
        vector signed char valid_right_shift_right3 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x18));
        vector signed char valid_right_shift_right4 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x20));
        vector signed char valid_right_shift_right5 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x28));
        vector signed char valid_right_shift_right6 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x30));
        vector signed char valid_right_shift_right7 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x38));
        vector signed char valid_right_shift_right8 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x40));
        vector signed char valid_right_shift_right9 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x48));
        vector signed char valid_right_shift_right10 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x50));
        vector signed char valid_right_shift_right11 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x58));
        vector signed char valid_right_shift_right12 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x60));
        vector signed char valid_right_shift_right13 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x68));
        vector signed char valid_right_shift_right14 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x70));
        vector signed char valid_right_shift_right15 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x78));
        
        /* compare valid_right_shift_right0..15 to valid_left_unsigned */
        vector bool char compare_valid_left_shift_right0 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right0);
        vector bool char compare_valid_left_shift_right1 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right1);
        vector bool char compare_valid_left_shift_right2 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right2);
        vector bool char compare_valid_left_shift_right3 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right3);
        vector bool char compare_valid_left_shift_right4 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right4);
        vector bool char compare_valid_left_shift_right5 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right5);
        vector bool char compare_valid_left_shift_right6 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right6);
        vector bool char compare_valid_left_shift_right7 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right7);
        vector bool char compare_valid_left_shift_right8 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right8);
        vector bool char compare_valid_left_shift_right9 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right9);
        vector bool char compare_valid_left_shift_right10 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right10);
        vector bool char compare_valid_left_shift_right11 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right11);
        vector bool char compare_valid_left_shift_right12 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right12);
        vector bool char compare_valid_left_shift_right13 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right13);
        vector bool char compare_valid_left_shift_right14 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right14);
        vector bool char compare_valid_left_shift_right15 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right15);
        
        /* and compare_valid_left_shift_right0..15 with left_valid_byte_mask to clear invalid bytes */
        compare_valid_left_shift_right0 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right0);
        compare_valid_left_shift_right1 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right1);
        compare_valid_left_shift_right2 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right2);
        compare_valid_left_shift_right3 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right3);
        compare_valid_left_shift_right4 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right4);
        compare_valid_left_shift_right5 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right5);
        compare_valid_left_shift_right6 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right6);
        compare_valid_left_shift_right7 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right7);
        compare_valid_left_shift_right8 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right8);
        compare_valid_left_shift_right9 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right9);
        compare_valid_left_shift_right10 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right10);
        compare_valid_left_shift_right11 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right11);
        compare_valid_left_shift_right12 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right12);
        compare_valid_left_shift_right13 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right13);
        compare_valid_left_shift_right14 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right14);
        compare_valid_left_shift_right15 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right15);
        
        /* gather bits by bytes compare_valid_left_shift_right0..15 */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb0 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right7);
          vector unsigned char gbb8 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right8);
          vector unsigned char gbb9 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right9);
          vector unsigned char gbb10 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right10);
          vector unsigned char gbb11 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right11);
          vector unsigned char gbb12 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right12);
          vector unsigned char gbb13 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right13);
          vector unsigned char gbb14 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right14);
          vector unsigned char gbb15 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right15);
        #else
          /* gcc */
          vector unsigned char gbb0 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right7);
          vector unsigned char gbb8 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right8);
          vector unsigned char gbb9 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right9);
          vector unsigned char gbb10 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right10);
          vector unsigned char gbb11 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right11);
          vector unsigned char gbb12 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right12);
          vector unsigned char gbb13 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right13);
          vector unsigned char gbb14 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right14);
          vector unsigned char gbb15 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right15);
        #endif
        
        /* permute gbb0-15 to put the 0th and 8th bytes together and pack them in binary tree order to enable parallelism */
        vector unsigned char gbb_permute_mask01 = (vector unsigned char) { 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask23 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask45 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask67 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18 };
        vector unsigned char gbb_permute_mask89 = (vector unsigned char) { 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask1011 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask1213 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask1415 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18 };
        vector unsigned char gbb_permute_mask0to3 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x14, 0x15, 0x16,0x17, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask4to7 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x08,0x09, 0x0A,0x0B, 0x1C, 0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask8to11 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x14, 0x15, 0x16,0x17, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask12to15 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x08,0x09, 0x0A,0x0B, 0x1C, 0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask0to7 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x04, 0x05, 0x06,0x07, 0x18,0x19, 0x1A,0x1B, 0x1C,0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask8to15 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x04, 0x05, 0x06,0x07, 0x18,0x19, 0x1A,0x1B, 0x1C,0x1D, 0x1E,0x1F };
        vector unsigned char pack_gbb01 = vec_perm (gbb0, gbb1, gbb_permute_mask01);
        vector unsigned char pack_gbb23 = vec_perm (gbb2, gbb3, gbb_permute_mask23);
        vector unsigned char pack_gbb45 = vec_perm (gbb4, gbb5, gbb_permute_mask45);
        vector unsigned char pack_gbb67 = vec_perm (gbb6, gbb7, gbb_permute_mask67);
        vector unsigned char pack_gbb89 = vec_perm (gbb8, gbb9, gbb_permute_mask89);
        vector unsigned char pack_gbb1011 = vec_perm (gbb10, gbb11, gbb_permute_mask1011);
        vector unsigned char pack_gbb1213 = vec_perm (gbb12, gbb13, gbb_permute_mask1213);
        vector unsigned char pack_gbb1415 = vec_perm (gbb14, gbb15, gbb_permute_mask1415);
        vector unsigned char pack_gbb0to3 = vec_perm (pack_gbb01, pack_gbb23, gbb_permute_mask0to3);
        vector unsigned char pack_gbb4to7 = vec_perm (pack_gbb45, pack_gbb67, gbb_permute_mask4to7);
        vector unsigned char pack_gbb8to11 = vec_perm (pack_gbb89, pack_gbb1011, gbb_permute_mask8to11);
        vector unsigned char pack_gbb12to15 = vec_perm (pack_gbb1213, pack_gbb1415, gbb_permute_mask12to15);
        vector unsigned char pack_gbb0to7 = vec_perm (pack_gbb0to3, pack_gbb4to7, gbb_permute_mask0to7);
        vector unsigned char pack_gbb8to15 = vec_perm (pack_gbb8to11, pack_gbb12to15, gbb_permute_mask8to15);
        
        /* calculate mask for substring match */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb_substring = (vector unsigned char) vec_gbb ((vector unsigned long long) left_valid_byte_mask);
        #else 
          /* gcc */
          vector unsigned char gbb_substring = vec_vgbbd ((vector unsigned char) left_valid_byte_mask);
        #endif
        /* puts the byte 0 of gbb_substring into even bytes and byte 8 into odd bytes */
        vector unsigned char substring_permute_mask = (vector unsigned char) { 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08 };
        vector unsigned char substring_mask = vec_perm (gbb_substring, gbb_substring, substring_permute_mask);
        
        /* compare substring_mask to pack_gbb0to7 and pack_gbb8to15 to find matches */
        vector bool short compare_gbb0to7 = vec_cmpeq ((vector unsigned short) pack_gbb0to7, (vector unsigned short) substring_mask);
        vector bool short compare_gbb8to15 = vec_cmpeq ((vector unsigned short) pack_gbb8to15, (vector unsigned short) substring_mask);
        
        /* turn compare_gbb0to7 and compare_gbb8to15 into a mask of 8 bits per element */
        vector bool char compare_gbb0to15 = vec_pack ((vector bool short) compare_gbb0to7, (vector bool short) compare_gbb8to15);
        
        result_mask = compare_gbb0to15;
      }
      else if (element_type == 3) {
        /* signed short */
        
        /* shift valid_right_unsigned to right to all positions */
        vector signed short valid_right_shift_right0 = (vector signed short) valid_right_signed;
        vector signed short valid_right_shift_right1 = vec_sro ((vector signed short) valid_right_signed, vec_splats((unsigned char) 0x10));
        vector signed short valid_right_shift_right2 = vec_sro ((vector signed short) valid_right_signed, vec_splats((unsigned char) 0x20));
        vector signed short valid_right_shift_right3 = vec_sro ((vector signed short) valid_right_signed, vec_splats((unsigned char) 0x30));
        vector signed short valid_right_shift_right4 = vec_sro ((vector signed short) valid_right_signed, vec_splats((unsigned char) 0x40));
        vector signed short valid_right_shift_right5 = vec_sro ((vector signed short) valid_right_signed, vec_splats((unsigned char) 0x50));
        vector signed short valid_right_shift_right6 = vec_sro ((vector signed short) valid_right_signed, vec_splats((unsigned char) 0x60));
        vector signed short valid_right_shift_right7 = vec_sro ((vector signed short) valid_right_signed, vec_splats((unsigned char) 0x70));
        
        /* compare valid_right_shift_right0..7 to valid_left_unsigned */
        vector bool short compare_valid_left_shift_right0 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right0);
        vector bool short compare_valid_left_shift_right1 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right1);
        vector bool short compare_valid_left_shift_right2 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right2);
        vector bool short compare_valid_left_shift_right3 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right3);
        vector bool short compare_valid_left_shift_right4 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right4);
        vector bool short compare_valid_left_shift_right5 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right5);
        vector bool short compare_valid_left_shift_right6 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right6);
        vector bool short compare_valid_left_shift_right7 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right7);
        
        /* and compare_valid_left_shift_right0..7 with left_valid_byte_mask to clear invalid bytes */
        compare_valid_left_shift_right0 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right0);
        compare_valid_left_shift_right1 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right1);
        compare_valid_left_shift_right2 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right2);
        compare_valid_left_shift_right3 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right3);
        compare_valid_left_shift_right4 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right4);
        compare_valid_left_shift_right5 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right5);
        compare_valid_left_shift_right6 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right6);
        compare_valid_left_shift_right7 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right7);
        
        /* gather bits by bytes compare_valid_left_shift_right0..7 */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb0 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right7);
        #else
          /* gcc */
          vector unsigned char gbb0 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right7);
        #endif
        
        /* permute gbb0-7 to put the 0th and 4th shorts together and pack them in binary tree order to enable parallelism */
        vector unsigned char gbb_permute_mask01 = (vector unsigned char) { 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask23 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask45 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask67 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18 };
        vector unsigned char gbb_permute_mask0to3 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x14, 0x15, 0x16,0x17, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask4to7 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x08,0x09, 0x0A,0x0B, 0x1C, 0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask0to7 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x04, 0x05, 0x06,0x07, 0x18,0x19, 0x1A,0x1B, 0x1C,0x1D, 0x1E,0x1F };
        vector unsigned char pack_gbb01 = vec_perm (gbb0, gbb1, gbb_permute_mask01);
        vector unsigned char pack_gbb23 = vec_perm (gbb2, gbb3, gbb_permute_mask23);
        vector unsigned char pack_gbb45 = vec_perm (gbb4, gbb5, gbb_permute_mask45);
        vector unsigned char pack_gbb67 = vec_perm (gbb6, gbb7, gbb_permute_mask67);
        vector unsigned char pack_gbb0to3 = vec_perm (pack_gbb01, pack_gbb23, gbb_permute_mask0to3);
        vector unsigned char pack_gbb4to7 = vec_perm (pack_gbb45, pack_gbb67, gbb_permute_mask4to7);
        vector unsigned char pack_gbb0to7 = vec_perm (pack_gbb0to3, pack_gbb4to7, gbb_permute_mask0to7);
        
        /* calculate mask for substring match */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb_substring = (vector unsigned char) vec_gbb ((vector unsigned long long) left_valid_byte_mask);
        #else 
          /* gcc */
          vector unsigned char gbb_substring = vec_vgbbd ((vector unsigned char) left_valid_byte_mask);
        #endif
        /* puts the byte 0 of gbb_substring into even bytes and byte 8 into odd bytes */
        vector unsigned char substring_permute_mask = (vector unsigned char) { 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08 };
        vector unsigned char substring_mask = vec_perm (gbb_substring, gbb_substring, substring_permute_mask);
        
        /* compare substring_mask to pack_gbb0to7 to find matches */
        vector bool short compare_gbb0to7 = vec_cmpeq ((vector unsigned short) pack_gbb0to7, (vector unsigned short) substring_mask);

        result_mask = (vector bool char) compare_gbb0to7;
      }
    #elif __BIG_ENDIAN__
      #error _SIDD_CMP_EQUAL_ORDERED is not supported on machines lower than POWER8.
    #endif
    
    result_valid_byte_mask = right_valid_byte_mask;
  }

  /* mask polarity */
  unsigned int polarity = (control >> 4) & 0x3;  /* bits 5:4 */
  if (polarity == 1) {
    /* _SIDD_NEGATIVE_POLARITY */
    /* complement result mask */
    result_mask = vec_nor (result_mask, result_mask);
  }

  else if (polarity == 3) {
    /* _SIDD_MASKED_NEGATIVE_POLARITY */
    /* complement result mask only before end of string */
    result_mask = vec_sel (/* if invalid */ result_mask,
                           /* if valid */   vec_nor (result_mask, result_mask),
                           result_valid_byte_mask);
  }

  else /* polarity == 0 or 2 */ {
    /* _SIDD_POSITIVE_POLARITY */
    /* leave result mask unchanged */
  }

  /* Return bit mask or element mask */
  unsigned int bit_or_element = (control >> 6) & 0x1;  /* bit 6 */

  if (bit_or_element == 0) {
    /* _SIDD_BIT_MASK - return mask of single bits */
    vector bool char result_mask_to_transpose = result_mask;
    if ((element_type & 0x1) == 1) {
      /* signed short or unsigned short */
      /* extract one byte of each short (both bytes are the same; extracting low byte is simpler) */
      result_mask_to_transpose = (vector bool char) vec_pack ((vector unsigned short) result_mask,
                                                              (vector unsigned short) zeros);
    }

    vector unsigned char bit_transposed_mask;
    /* bit-transpose each half of result_mask, using Power8 vector gather bits by bytes by doubleword */
    #ifdef _ARCH_PWR8
      #ifdef __ibmxl__
        /* xlc */
        bit_transposed_mask = (vector unsigned char) vec_gbb ((vector unsigned long long) result_mask_to_transpose);
      #else
        /* gcc */
        bit_transposed_mask = vec_vgbbd ((vector unsigned char) result_mask_to_transpose);
      #endif
    #else
      /* emulate vector gather bits by bytes by doublewords for one bit only of each byte */
      /* all bits of each byte are identical, so extract the one that avoids bit shifting */
      vector unsigned char mask_bits_to_extract = (vector unsigned char)
          { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,  0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 };
      vector unsigned char extracted_mask_bits = vec_and (result_mask_to_transpose, mask_bits_to_extract);
      /* gather extracted bits for each doubleword into its byte 0 (leaving garbage in its other bytes) */
      vector unsigned char btmbit0 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 7*8));
      vector unsigned char btmbit1 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 6*8));
      vector unsigned char btmbit2 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 5*8));
      vector unsigned char btmbit3 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 4*8));
      vector unsigned char btmbit4 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 3*8));
      vector unsigned char btmbit5 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 2*8));
      vector unsigned char btmbit6 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 1*8));
      vector unsigned char btmbit7 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 0*8));
      vector unsigned char btmbits01 = vec_or (btmbit0, btmbit1);
      vector unsigned char btmbits23 = vec_or (btmbit2, btmbit3);
      vector unsigned char btmbits45 = vec_or (btmbit4, btmbit5);
      vector unsigned char btmbits67 = vec_or (btmbit6, btmbit7);
      vector unsigned char btmbits0to3 = vec_or (btmbits01, btmbits23);
      vector unsigned char btmbits4to7 = vec_or (btmbits45, btmbits67);
      vector unsigned char btmbits0to7 = vec_or (btmbits0to3, btmbits4to7);
      bit_transposed_mask = btmbits0to7;
    #endif

    vector unsigned char select_transposed_upper_bits;
    if ((element_type & 0x1) == 0) {
      /* signed char or unsigned char */
      /* extract leftmost bit of each byte, to two bytes with other 14 bytes zeroed */
      select_transposed_upper_bits = (vector unsigned char)  /* need leftmost byte of each doubleword */
      #ifdef __LITTLE_ENDIAN__
          /* for little endian these two bytes must be right justified in the VR to be stored in the first short element */
          /* the mask for the right (low address) half is stored in byte 0 */
          /* the following will be in the reverse order in the VR */
          { 0x10, 0x18, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 };
      #elif __BIG_ENDIAN__
          /* for big endian these two bytes must be left justified in the VR to be stored in the first short element */
          /* caution: some applications might access it as an int or long long? */
          /* the mask for the left (low address) half is stored in byte 0 */
          { 0x10, 0x18, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 };
      #endif
    }

    else /* (element_type & 0x1) == 1 */ {
      /* signed short or unsigned short */
      /* extract leftmost bit of one byte of each short, to one byte with other 15 bytes zeroed */
      select_transposed_upper_bits = (vector unsigned char)  /* need leftmost byte of left doubleword */
      #ifdef __LITTLE_ENDIAN__
          /* for little endian this byte must be right justified to be stored in the first char element */
          // { 0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0x10 };
          { 0x10, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 };
      #elif __BIG_ENDIAN__
          /* for big endian this byte must be left justified to be stored in the first char element */
          { 0x10, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 };
      #endif
    }

    /* extract leftmost byte of each doubleword, to 2 or 1 bytes with other 14 or 15 bytes zeroed */
    result_mask = (vector bool char) vec_perm (zeros, bit_transposed_mask, select_transposed_upper_bits);
  }

  else /* bit_or_element == 1 */ {
    /* _SIDD_UNIT_MASK - return mask of 8-bit or 16-bit elements */
    /* leave result_mask unchanged */
  }

  return (__m128i) result_mask;
}

/* Compare length terminated strings in various ways, giving vector or bit mask */
VECLIB_INLINE __m128i vec_comparelengthstringstomask1q (__m128i left, intlit5 leftlen, __m128i right, intlit5 rightlen, intlit8 control)
  /* See "String Control" above. */
  /* _SIDD_CMP_EQUAL_ANY:     Each valid character in right is compared to every valid character in left.             */
  /*                          Result mask is true if it matches any of them, false if not or if it is invalid.        */
  /* _SIDD_CMP_RANGES:        Each valid character in right is compared to the two ranges in left.                    */
  /*                          Left contains four characters: lower bound 1, upper bound 1, lower 2, upper 2.          */
  /*                          Result mask is true if it is within either bound, false if not or if it is invalid.     */
  /* _SIDD_CMP_EQUAL_EACH:    Each valid character in right is compared to the corresponding valid character in left. */
  /*                          Result mask is true if they match, false if not or if either is invalid.                */
  /* _SIDD_CMP_EQUAL_ORDERED: Each sequence of valid characters in right is compared to the the valid string in left. */
  /*                          Result mask is true if the sequence starting at that character matches all of left,     */
  /*                          false if not or if it is invalid.                                                       */
{
  vector unsigned char zeros = (vector unsigned char) {0, 0, 0, 0};
  vector bool char result_mask;

  unsigned int operation = (control >> 2) & 0x3;  /* bits 3:2 */
  unsigned int element_type = control & 0x3;  /* bit 0 */

  vector bool char left_valid_byte_mask;
  vector bool char right_valid_byte_mask;
  vector bool char result_valid_byte_mask;
  vector unsigned char valid_left_unsigned;
  vector unsigned char valid_right_unsigned;
  vector signed char valid_left_signed;
  vector signed char valid_right_signed;

  /* clear invalid bytes */
  /* create mask based on the length argument */
  if ((element_type & 0x01) == 0) /* element_type 00 10 */ {
    static const vector bool char valid_byte_mask_selector[17] = {
        { 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 0 */
        { 0xFF,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 1 */
        { 0xFF,0xFF, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 2 */
        { 0xFF,0xFF, 0xFF,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 3 */
        { 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 4 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 5 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 6 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 7 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 8 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 9 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 10 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0x00, 0x00,0x00, 0x00,0x00 }, /* length 11 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x00,0x00 }, /* length 12 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0x00, 0x00,0x00 }, /* length 13 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00 }, /* length 14 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0x00 }, /* length 15 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF }  /* length 16 */
    };
    
    /* truncate the last element in the left_valid_byte_mask if the length of it is odd if _SIDD_CMP_RANGES*/
    if (operation == 1) {
      /* _SIDD_CMP_RANGES */
      left_valid_byte_mask = valid_byte_mask_selector[(leftlen / 2) * 2];
    } else /* operation 0 2 3 */ {
      left_valid_byte_mask = valid_byte_mask_selector[leftlen];
    }
    right_valid_byte_mask = valid_byte_mask_selector[rightlen];
  } else /* element_type 01 11*/ {
    static const vector bool char valid_short_mask_selector[9] = {
        { 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 0 */
        { 0xFF,0xFF, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 1 */
        { 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 2 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 3 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 4 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 5 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x00,0x00 }, /* length 6 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00 }, /* length 7 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF }  /* length 8 */
    };
    
    /* truncate the last element in the left_valid_byte_mask if the length of it is odd if _SIDD_CMP_RANGES*/
    if (operation == 1) {
      /* _SIDD_CMP_RANGES */
      left_valid_byte_mask = valid_short_mask_selector[(leftlen / 2) * 2];
    } else /* operation 0 2 3 */ {
      left_valid_byte_mask = valid_short_mask_selector[leftlen];
    }
    right_valid_byte_mask = valid_short_mask_selector[rightlen];    
  }

  /* clear invalid characters */
  if (((element_type == 2) || (element_type == 3)) && ((operation == 1) || (operation == 3))) {
    /* signed and _SIDD_CMP_EQUAL_ORDERED */
    valid_left_signed  = vec_and ((vector signed char) left,  left_valid_byte_mask);
    valid_right_signed = vec_and ((vector signed char) right, right_valid_byte_mask);
  }
  else /* unsigned */ {
    valid_left_unsigned  = vec_and ((vector unsigned char) left,  left_valid_byte_mask);
    valid_right_unsigned = vec_and ((vector unsigned char) right, right_valid_byte_mask);
  }
  
  /* compare operation */
  if (operation == 0) {
    /* _SIDD_CMP_EQUAL_ANY */
    /* compare each valid character in right to every valid character in left */
    /* sign does not matter */
    if ((element_type & 0x1) == 0) {
      /* compare each valid char in right to every valid char in left */
      /* first rotate left to all positions */
      vector unsigned char left0  = (vector unsigned char) valid_left_unsigned;
      vector unsigned char left1  = vec_sld (left0, left0, 1);
      vector unsigned char left2  = vec_sld (left0, left0, 2);
      vector unsigned char left3  = vec_sld (left0, left0, 3);
      vector unsigned char left4  = vec_sld (left0, left0, 4);
      vector unsigned char left5  = vec_sld (left0, left0, 5);
      vector unsigned char left6  = vec_sld (left0, left0, 6);
      vector unsigned char left7  = vec_sld (left0, left0, 7);
      vector unsigned char left8  = vec_sld (left0, left0, 8);
      vector unsigned char left9  = vec_sld (left0, left0, 9);
      vector unsigned char left10 = vec_sld (left0, left0, 10);
      vector unsigned char left11 = vec_sld (left0, left0, 11);
      vector unsigned char left12 = vec_sld (left0, left0, 12);
      vector unsigned char left13 = vec_sld (left0, left0, 13);
      vector unsigned char left14 = vec_sld (left0, left0, 14);
      vector unsigned char left15 = vec_sld (left0, left0, 15);
      vector bool char match0  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left0);
      vector bool char match1  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left1);
      vector bool char match2  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left2);
      vector bool char match3  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left3);
      vector bool char match4  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left4);
      vector bool char match5  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left5);
      vector bool char match6  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left6);
      vector bool char match7  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left7);
      vector bool char match8  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left8);
      vector bool char match9  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left9);
      vector bool char match10 = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left10);
      vector bool char match11 = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left11);
      vector bool char match12 = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left12);
      vector bool char match13 = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left13);
      vector bool char match14 = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left14);
      vector bool char match15 = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left15);
      vector bool char match01   = vec_or (match0,  match1);
      vector bool char match23   = vec_or (match2,  match3);
      vector bool char match45   = vec_or (match4,  match5);
      vector bool char match67   = vec_or (match6,  match7);
      vector bool char match89   = vec_or (match8,  match9);
      vector bool char match1011 = vec_or (match10, match11);
      vector bool char match1213 = vec_or (match12, match13);
      vector bool char match1415 = vec_or (match14, match15);
      vector bool char match0to3   = vec_or (match01,   match23);
      vector bool char match4to7   = vec_or (match45,   match67);
      vector bool char match8to11  = vec_or (match89,   match1011);
      vector bool char match12to15 = vec_or (match1213, match1415);
      vector bool char match0to7  = vec_or (match0to3,  match4to7);
      vector bool char match8to15 = vec_or (match8to11, match12to15);
      vector bool char match0to15 = vec_or (match0to7, match8to15);
      result_mask = vec_and ((vector bool char) match0to15, right_valid_byte_mask);
    }

    else /* (element_type & 0x1) == 1 */ {
      /* compare each valid short in right to every valid short in left */
      /* first rotate left to all positions */
      vector unsigned short left0 = (vector unsigned short) valid_left_unsigned;
      vector unsigned short left1 = vec_sld (left0, left0, 2);
      vector unsigned short left2 = vec_sld (left0, left0, 4);
      vector unsigned short left3 = vec_sld (left0, left0, 6);
      vector unsigned short left4 = vec_sld (left0, left0, 8);
      vector unsigned short left5 = vec_sld (left0, left0, 10);
      vector unsigned short left6 = vec_sld (left0, left0, 12);
      vector unsigned short left7 = vec_sld (left0, left0, 14);
      vector bool short match0 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left0);
      vector bool short match1 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left1);
      vector bool short match2 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left2);
      vector bool short match3 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left3);
      vector bool short match4 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left4);
      vector bool short match5 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left5);
      vector bool short match6 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left6);
      vector bool short match7 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left7);
      vector bool short match01 = vec_or (match0, match1);
      vector bool short match23 = vec_or (match2, match3);
      vector bool short match45 = vec_or (match4, match5);
      vector bool short match67 = vec_or (match6, match7);
      vector bool short match0to3 = vec_or (match01, match23);
      vector bool short match4to7 = vec_or (match45, match67);
      vector bool short match0to7 = vec_or (match0to3, match4to7);
      result_mask = vec_and ((vector bool char) match0to7, right_valid_byte_mask);
    }
    result_valid_byte_mask = right_valid_byte_mask;
  }

  else if (operation == 1) {
    /* _SIDD_CMP_RANGES */
    /* check within range for each valid character in right to a pair of adjacent characters in left */
    /* sign does matter */
    if (element_type == 0x0) {
      /* unsigned char */
      
      /* splat each of the 16 elements to create 8 upper and lower bounds */
      vector unsigned char lower_bound0 = vec_splat ((vector unsigned char) valid_left_unsigned, 0);
      vector unsigned char upper_bound0 = vec_splat ((vector unsigned char) valid_left_unsigned, 1);
      vector unsigned char lower_bound1 = vec_splat ((vector unsigned char) valid_left_unsigned, 2);
      vector unsigned char upper_bound1 = vec_splat ((vector unsigned char) valid_left_unsigned, 3);
      vector unsigned char lower_bound2 = vec_splat ((vector unsigned char) valid_left_unsigned, 4);
      vector unsigned char upper_bound2 = vec_splat ((vector unsigned char) valid_left_unsigned, 5);
      vector unsigned char lower_bound3 = vec_splat ((vector unsigned char) valid_left_unsigned, 6);
      vector unsigned char upper_bound3 = vec_splat ((vector unsigned char) valid_left_unsigned, 7);
      vector unsigned char lower_bound4 = vec_splat ((vector unsigned char) valid_left_unsigned, 8);
      vector unsigned char upper_bound4 = vec_splat ((vector unsigned char) valid_left_unsigned, 9);
      vector unsigned char lower_bound5 = vec_splat ((vector unsigned char) valid_left_unsigned, 10);
      vector unsigned char upper_bound5 = vec_splat ((vector unsigned char) valid_left_unsigned, 11);
      vector unsigned char lower_bound6 = vec_splat ((vector unsigned char) valid_left_unsigned, 12);
      vector unsigned char upper_bound6 = vec_splat ((vector unsigned char) valid_left_unsigned, 13);
      vector unsigned char lower_bound7 = vec_splat ((vector unsigned char) valid_left_unsigned, 14);
      vector unsigned char upper_bound7 = vec_splat ((vector unsigned char) valid_left_unsigned, 15);
      
      /* compare valid_right_unsigned to each of the 8 upper and lower bounds */
      #ifdef __ibmxl__
        /* xlc */
        vector bool char cmplow0 = vec_cmpge (valid_right_unsigned, lower_bound0);
        vector bool char cmphi0 = vec_cmpge (upper_bound0, valid_right_unsigned);
        vector bool char cmplow1 = vec_cmpge (valid_right_unsigned, lower_bound1);
        vector bool char cmphi1 = vec_cmpge (upper_bound1, valid_right_unsigned);
        vector bool char cmplow2 = vec_cmpge (valid_right_unsigned, lower_bound2);
        vector bool char cmphi2 = vec_cmpge (upper_bound2, valid_right_unsigned);
        vector bool char cmplow3 = vec_cmpge (valid_right_unsigned, lower_bound3);
        vector bool char cmphi3 = vec_cmpge (upper_bound3, valid_right_unsigned);
        vector bool char cmplow4 = vec_cmpge (valid_right_unsigned, lower_bound4);
        vector bool char cmphi4 = vec_cmpge (upper_bound4, valid_right_unsigned);
        vector bool char cmplow5 = vec_cmpge (valid_right_unsigned, lower_bound5);
        vector bool char cmphi5 = vec_cmpge (upper_bound5, valid_right_unsigned);
        vector bool char cmplow6 = vec_cmpge (valid_right_unsigned, lower_bound6);
        vector bool char cmphi6 = vec_cmpge (upper_bound6, valid_right_unsigned);
        vector bool char cmplow7 = vec_cmpge (valid_right_unsigned, lower_bound7);
        vector bool char cmphi7 = vec_cmpge (upper_bound7, valid_right_unsigned);
      #else 
        /* gcc */
        vector bool char cmpgtlow0 = vec_cmpgt (valid_right_unsigned, lower_bound0);
        vector bool char cmpgthi0 = vec_cmpgt (upper_bound0, valid_right_unsigned);
        vector bool char cmpeqlow0 = vec_cmpeq (valid_right_unsigned, lower_bound0);
        vector bool char cmpeqhi0 = vec_cmpeq (upper_bound0, valid_right_unsigned);
        vector bool char cmplow0 = vec_or (cmpgtlow0, cmpeqlow0);
        vector bool char cmphi0 = vec_or (cmpgthi0, cmpeqhi0);
        
        vector bool char cmpgtlow1 = vec_cmpgt (valid_right_unsigned, lower_bound1);
        vector bool char cmpgthi1 = vec_cmpgt (upper_bound1, valid_right_unsigned);    
        vector bool char cmpeqlow1 = vec_cmpeq (valid_right_unsigned, lower_bound1);
        vector bool char cmpeqhi1 = vec_cmpeq (upper_bound1, valid_right_unsigned);
        vector bool char cmplow1 = vec_or (cmpgtlow1, cmpeqlow1);
        vector bool char cmphi1 = vec_or (cmpgthi1, cmpeqhi1);
        
        vector bool char cmpgtlow2 = vec_cmpgt (valid_right_unsigned, lower_bound2);
        vector bool char cmpgthi2 = vec_cmpgt (upper_bound2, valid_right_unsigned);
        vector bool char cmpeqlow2 = vec_cmpeq (valid_right_unsigned, lower_bound2);
        vector bool char cmpeqhi2 = vec_cmpeq (upper_bound2, valid_right_unsigned);
        vector bool char cmplow2 = vec_or (cmpgtlow2, cmpeqlow2);
        vector bool char cmphi2 = vec_or (cmpgthi2, cmpeqhi2);
        
        vector bool char cmpgtlow3 = vec_cmpgt (valid_right_unsigned, lower_bound3);
        vector bool char cmpgthi3 = vec_cmpgt (upper_bound3, valid_right_unsigned);    
        vector bool char cmpeqlow3 = vec_cmpeq (valid_right_unsigned, lower_bound3);
        vector bool char cmpeqhi3 = vec_cmpeq (upper_bound3, valid_right_unsigned);
        vector bool char cmplow3 = vec_or (cmpgtlow3, cmpeqlow3);
        vector bool char cmphi3 = vec_or (cmpgthi3, cmpeqhi3);
        
        vector bool char cmpgtlow4 = vec_cmpgt (valid_right_unsigned, lower_bound4);
        vector bool char cmpgthi4 = vec_cmpgt (upper_bound4, valid_right_unsigned);
        vector bool char cmpeqlow4 = vec_cmpeq (valid_right_unsigned, lower_bound4);
        vector bool char cmpeqhi4 = vec_cmpeq (upper_bound4, valid_right_unsigned);
        vector bool char cmplow4 = vec_or (cmpgtlow4, cmpeqlow4);
        vector bool char cmphi4 = vec_or (cmpgthi4, cmpeqhi4);
        
        vector bool char cmpgtlow5 = vec_cmpgt (valid_right_unsigned, lower_bound5);
        vector bool char cmpgthi5 = vec_cmpgt (upper_bound5, valid_right_unsigned);    
        vector bool char cmpeqlow5 = vec_cmpeq (valid_right_unsigned, lower_bound5);
        vector bool char cmpeqhi5 = vec_cmpeq (upper_bound5, valid_right_unsigned);
        vector bool char cmplow5 = vec_or (cmpgtlow5, cmpeqlow5);
        vector bool char cmphi5 = vec_or (cmpgthi5, cmpeqhi5);
        
        vector bool char cmpgtlow6 = vec_cmpgt (valid_right_unsigned, lower_bound6);
        vector bool char cmpgthi6 = vec_cmpgt (upper_bound6, valid_right_unsigned);
        vector bool char cmpeqlow6 = vec_cmpeq (valid_right_unsigned, lower_bound6);
        vector bool char cmpeqhi6 = vec_cmpeq (upper_bound6, valid_right_unsigned);
        vector bool char cmplow6 = vec_or (cmpgtlow6, cmpeqlow6);
        vector bool char cmphi6 = vec_or (cmpgthi6, cmpeqhi6);
        
        vector bool char cmpgtlow7 = vec_cmpgt (valid_right_unsigned, lower_bound7);
        vector bool char cmpgthi7 = vec_cmpgt (upper_bound7, valid_right_unsigned);    
        vector bool char cmpeqlow7 = vec_cmpeq (valid_right_unsigned, lower_bound7);
        vector bool char cmpeqhi7 = vec_cmpeq (upper_bound7, valid_right_unsigned);
        vector bool char cmplow7 = vec_or (cmpgtlow7, cmpeqlow7);
        vector bool char cmphi7 = vec_or (cmpgthi7, cmpeqhi7);
      #endif
      
      /* AND the result of all the comparisons for within range compares */
      vector bool char range0 = vec_and (cmplow0, cmphi0);
      vector bool char range1 = vec_and (cmplow1, cmphi1);
      vector bool char range2 = vec_and (cmplow2, cmphi2);
      vector bool char range3 = vec_and (cmplow3, cmphi3);
      vector bool char range4 = vec_and (cmplow4, cmphi4);
      vector bool char range5 = vec_and (cmplow5, cmphi5);
      vector bool char range6 = vec_and (cmplow6, cmphi6);
      vector bool char range7 = vec_and (cmplow7, cmphi7);
      
      /* OR the result of range compares */
      vector bool char range01 = vec_or (range0, range1);
      vector bool char range23 = vec_or (range2, range3);
      vector bool char range45 = vec_or (range4, range5);
      vector bool char range67 = vec_or (range6, range7);
      vector bool char range0to3 = vec_or (range01, range23);
      vector bool char range4to7 = vec_or (range45, range67);
      vector bool char range0to7 = vec_or (range0to3, range4to7);
      
      result_mask = vec_and (range0to7, right_valid_byte_mask);
    }
    else if (element_type == 0x1) {
      /* unsigned short */
      
      /* splat each of the 16 elements to create 8 upper and lower bounds */
      vector unsigned short lower_bound0 = vec_splat ((vector unsigned short) valid_left_unsigned, 0);
      vector unsigned short upper_bound0 = vec_splat ((vector unsigned short) valid_left_unsigned, 1);
      vector unsigned short lower_bound1 = vec_splat ((vector unsigned short) valid_left_unsigned, 2);
      vector unsigned short upper_bound1 = vec_splat ((vector unsigned short) valid_left_unsigned, 3);
      vector unsigned short lower_bound2 = vec_splat ((vector unsigned short) valid_left_unsigned, 4);
      vector unsigned short upper_bound2 = vec_splat ((vector unsigned short) valid_left_unsigned, 5);
      vector unsigned short lower_bound3 = vec_splat ((vector unsigned short) valid_left_unsigned, 6);
      vector unsigned short upper_bound3 = vec_splat ((vector unsigned short) valid_left_unsigned, 7);

      /* compare valid_right_unsigned to each of the 8 upper and lower bounds */
      #ifdef __ibmxl__
        /* xlc */
        vector bool short cmplow0 = vec_cmpge ((vector unsigned short) (vector unsigned short) valid_right_unsigned, lower_bound0);
        vector bool short cmphi0 = vec_cmpge (upper_bound0, (vector unsigned short) (vector unsigned short) valid_right_unsigned);
        vector bool short cmplow1 = vec_cmpge ((vector unsigned short) valid_right_unsigned, lower_bound1);
        vector bool short cmphi1 = vec_cmpge (upper_bound1, (vector unsigned short) valid_right_unsigned);
        vector bool short cmplow2 = vec_cmpge ((vector unsigned short) valid_right_unsigned, lower_bound2);
        vector bool short cmphi2 = vec_cmpge (upper_bound2, (vector unsigned short) valid_right_unsigned);
        vector bool short cmplow3 = vec_cmpge ((vector unsigned short) valid_right_unsigned, lower_bound3);
        vector bool short cmphi3 = vec_cmpge (upper_bound3, (vector unsigned short) valid_right_unsigned);
      #else 
        /* gcc */
        vector bool short cmpgtlow0 = vec_cmpgt ((vector unsigned short) valid_right_unsigned, lower_bound0);
        vector bool short cmpgthi0 = vec_cmpgt (upper_bound0, (vector unsigned short) valid_right_unsigned);
        vector bool short cmpeqlow0 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, lower_bound0);
        vector bool short cmpeqhi0 = vec_cmpeq (upper_bound0, (vector unsigned short) valid_right_unsigned);
        vector bool short cmplow0 = vec_or (cmpgtlow0, cmpeqlow0);
        vector bool short cmphi0 = vec_or (cmpgthi0, cmpeqhi0);
        
        vector bool short cmpgtlow1 = vec_cmpgt ((vector unsigned short) valid_right_unsigned, lower_bound1);
        vector bool short cmpgthi1 = vec_cmpgt (upper_bound1, (vector unsigned short) valid_right_unsigned);    
        vector bool short cmpeqlow1 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, lower_bound1);
        vector bool short cmpeqhi1 = vec_cmpeq (upper_bound1, (vector unsigned short) valid_right_unsigned);
        vector bool short cmplow1 = vec_or (cmpgtlow1, cmpeqlow1);
        vector bool short cmphi1 = vec_or (cmpgthi1, cmpeqhi1);
        
        vector bool short cmpgtlow2 = vec_cmpgt ((vector unsigned short) valid_right_unsigned, lower_bound2);
        vector bool short cmpgthi2 = vec_cmpgt (upper_bound2, (vector unsigned short) valid_right_unsigned);
        vector bool short cmpeqlow2 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, lower_bound2);
        vector bool short cmpeqhi2 = vec_cmpeq (upper_bound2, (vector unsigned short) valid_right_unsigned);
        vector bool short cmplow2 = vec_or (cmpgtlow2, cmpeqlow2);
        vector bool short cmphi2 = vec_or (cmpgthi2, cmpeqhi2);
        
        vector bool short cmpgtlow3 = vec_cmpgt ((vector unsigned short) valid_right_unsigned, lower_bound3);
        vector bool short cmpgthi3 = vec_cmpgt (upper_bound3, (vector unsigned short) valid_right_unsigned);    
        vector bool short cmpeqlow3 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, lower_bound3);
        vector bool short cmpeqhi3 = vec_cmpeq (upper_bound3, (vector unsigned short) valid_right_unsigned);
        vector bool short cmplow3 = vec_or (cmpgtlow3, cmpeqlow3);
        vector bool short cmphi3 = vec_or (cmpgthi3, cmpeqhi3);
      #endif
      
      /* AND the result of all the comparisons for within range compares */
      vector bool short range0 = vec_and (cmplow0, cmphi0);
      vector bool short range1 = vec_and (cmplow1, cmphi1);
      vector bool short range2 = vec_and (cmplow2, cmphi2);
      vector bool short range3 = vec_and (cmplow3, cmphi3);
      
      /* OR the result of range compares */
      vector bool short range01 = vec_or (range0, range1);
      vector bool short range23 = vec_or (range2, range3);
      vector bool short range0to3 = vec_or (range01, range23);
      
      result_mask = vec_and((vector bool char) range0to3, right_valid_byte_mask);
    }
    else if (element_type == 0x2) {
      /* signed char */
      
      /* splat each of the 16 elements to create 8 upper and lower bounds */
      vector signed char lower_bound0 = vec_splat ((vector signed char) valid_left_signed, 0);
      vector signed char upper_bound0 = vec_splat ((vector signed char) valid_left_signed, 1);
      vector signed char lower_bound1 = vec_splat ((vector signed char) valid_left_signed, 2);
      vector signed char upper_bound1 = vec_splat ((vector signed char) valid_left_signed, 3);
      vector signed char lower_bound2 = vec_splat ((vector signed char) valid_left_signed, 4);
      vector signed char upper_bound2 = vec_splat ((vector signed char) valid_left_signed, 5);
      vector signed char lower_bound3 = vec_splat ((vector signed char) valid_left_signed, 6);
      vector signed char upper_bound3 = vec_splat ((vector signed char) valid_left_signed, 7);
      vector signed char lower_bound4 = vec_splat ((vector signed char) valid_left_signed, 8);
      vector signed char upper_bound4 = vec_splat ((vector signed char) valid_left_signed, 9);
      vector signed char lower_bound5 = vec_splat ((vector signed char) valid_left_signed, 10);
      vector signed char upper_bound5 = vec_splat ((vector signed char) valid_left_signed, 11);
      vector signed char lower_bound6 = vec_splat ((vector signed char) valid_left_signed, 12);
      vector signed char upper_bound6 = vec_splat ((vector signed char) valid_left_signed, 13);
      vector signed char lower_bound7 = vec_splat ((vector signed char) valid_left_signed, 14);
      vector signed char upper_bound7 = vec_splat ((vector signed char) valid_left_signed, 15);
      
      /* compare valid_right_signed to each of the 8 upper and lower bounds */
      #ifdef __ibmxl__
        /* xlc */
        vector bool char cmplow0 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound0);
        vector bool char cmphi0 = vec_cmpge (upper_bound0, (vector signed char) valid_right_signed);
        vector bool char cmplow1 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound1);
        vector bool char cmphi1 = vec_cmpge (upper_bound1, (vector signed char) valid_right_signed);
        vector bool char cmplow2 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound2);
        vector bool char cmphi2 = vec_cmpge (upper_bound2, (vector signed char) valid_right_signed);
        vector bool char cmplow3 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound3);
        vector bool char cmphi3 = vec_cmpge (upper_bound3, (vector signed char) valid_right_signed);
        vector bool char cmplow4 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound4);
        vector bool char cmphi4 = vec_cmpge (upper_bound4, (vector signed char) valid_right_signed);
        vector bool char cmplow5 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound5);
        vector bool char cmphi5 = vec_cmpge (upper_bound5, (vector signed char) valid_right_signed);
        vector bool char cmplow6 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound6);
        vector bool char cmphi6 = vec_cmpge (upper_bound6, (vector signed char) valid_right_signed);
        vector bool char cmplow7 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound7);
        vector bool char cmphi7 = vec_cmpge (upper_bound7, (vector signed char) valid_right_signed);
      #else 
        /* gcc */
        vector bool char cmpgtlow0 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound0);
        vector bool char cmpgthi0 = vec_cmpgt (upper_bound0, (vector signed char) valid_right_signed);
        vector bool char cmpeqlow0 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound0);
        vector bool char cmpeqhi0 = vec_cmpeq (upper_bound0, (vector signed char) valid_right_signed);
        vector bool char cmplow0 = vec_or (cmpgtlow0, cmpeqlow0);
        vector bool char cmphi0 = vec_or (cmpgthi0, cmpeqhi0);
        
        vector bool char cmpgtlow1 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound1);
        vector bool char cmpgthi1 = vec_cmpgt (upper_bound1, (vector signed char) valid_right_signed);    
        vector bool char cmpeqlow1 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound1);
        vector bool char cmpeqhi1 = vec_cmpeq (upper_bound1, (vector signed char) valid_right_signed);
        vector bool char cmplow1 = vec_or (cmpgtlow1, cmpeqlow1);
        vector bool char cmphi1 = vec_or (cmpgthi1, cmpeqhi1);
        
        vector bool char cmpgtlow2 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound2);
        vector bool char cmpgthi2 = vec_cmpgt (upper_bound2, (vector signed char) valid_right_signed);
        vector bool char cmpeqlow2 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound2);
        vector bool char cmpeqhi2 = vec_cmpeq (upper_bound2, (vector signed char) valid_right_signed);
        vector bool char cmplow2 = vec_or (cmpgtlow2, cmpeqlow2);
        vector bool char cmphi2 = vec_or (cmpgthi2, cmpeqhi2);
        
        vector bool char cmpgtlow3 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound3);
        vector bool char cmpgthi3 = vec_cmpgt (upper_bound3, (vector signed char) valid_right_signed);    
        vector bool char cmpeqlow3 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound3);
        vector bool char cmpeqhi3 = vec_cmpeq (upper_bound3, (vector signed char) valid_right_signed);
        vector bool char cmplow3 = vec_or (cmpgtlow3, cmpeqlow3);
        vector bool char cmphi3 = vec_or (cmpgthi3, cmpeqhi3);
        
        vector bool char cmpgtlow4 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound4);
        vector bool char cmpgthi4 = vec_cmpgt (upper_bound4, (vector signed char) valid_right_signed);
        vector bool char cmpeqlow4 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound4);
        vector bool char cmpeqhi4 = vec_cmpeq (upper_bound4, (vector signed char) valid_right_signed);
        vector bool char cmplow4 = vec_or (cmpgtlow4, cmpeqlow4);
        vector bool char cmphi4 = vec_or (cmpgthi4, cmpeqhi4);
        
        vector bool char cmpgtlow5 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound5);
        vector bool char cmpgthi5 = vec_cmpgt (upper_bound5, (vector signed char) valid_right_signed);    
        vector bool char cmpeqlow5 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound5);
        vector bool char cmpeqhi5 = vec_cmpeq (upper_bound5, (vector signed char) valid_right_signed);
        vector bool char cmplow5 = vec_or (cmpgtlow5, cmpeqlow5);
        vector bool char cmphi5 = vec_or (cmpgthi5, cmpeqhi5);
        
        vector bool char cmpgtlow6 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound6);
        vector bool char cmpgthi6 = vec_cmpgt (upper_bound6, (vector signed char) valid_right_signed);
        vector bool char cmpeqlow6 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound6);
        vector bool char cmpeqhi6 = vec_cmpeq (upper_bound6, (vector signed char) valid_right_signed);
        vector bool char cmplow6 = vec_or (cmpgtlow6, cmpeqlow6);
        vector bool char cmphi6 = vec_or (cmpgthi6, cmpeqhi6);
        
        vector bool char cmpgtlow7 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound7);
        vector bool char cmpgthi7 = vec_cmpgt (upper_bound7, (vector signed char) valid_right_signed);    
        vector bool char cmpeqlow7 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound7);
        vector bool char cmpeqhi7 = vec_cmpeq (upper_bound7, (vector signed char) valid_right_signed);
        vector bool char cmplow7 = vec_or (cmpgtlow7, cmpeqlow7);
        vector bool char cmphi7 = vec_or (cmpgthi7, cmpeqhi7);
      #endif
      
      /* AND the result of all the comparisons for within range compares */
      vector bool char range0 = vec_and (cmplow0, cmphi0);
      vector bool char range1 = vec_and (cmplow1, cmphi1);
      vector bool char range2 = vec_and (cmplow2, cmphi2);
      vector bool char range3 = vec_and (cmplow3, cmphi3);
      vector bool char range4 = vec_and (cmplow4, cmphi4);
      vector bool char range5 = vec_and (cmplow5, cmphi5);
      vector bool char range6 = vec_and (cmplow6, cmphi6);
      vector bool char range7 = vec_and (cmplow7, cmphi7);
      
      /* OR the result of range compares */
      vector bool char range01 = vec_or (range0, range1);
      vector bool char range23 = vec_or (range2, range3);
      vector bool char range45 = vec_or (range4, range5);
      vector bool char range67 = vec_or (range6, range7);
      vector bool char range0to3 = vec_or (range01, range23);
      vector bool char range4to7 = vec_or (range45, range67);
      vector bool char range0to7 = vec_or (range0to3, range4to7);
      
      result_mask = vec_and (range0to7, right_valid_byte_mask);
    }
    else if (element_type == 0x3) {
      /* signed short */
      
      /* splat each of the 16 elements to create 8 upper and lower bounds */
      vector signed short lower_bound0 = vec_splat ((vector signed short) valid_left_signed, 0);
      vector signed short upper_bound0 = vec_splat ((vector signed short) valid_left_signed, 1);
      vector signed short lower_bound1 = vec_splat ((vector signed short) valid_left_signed, 2);
      vector signed short upper_bound1 = vec_splat ((vector signed short) valid_left_signed, 3);
      vector signed short lower_bound2 = vec_splat ((vector signed short) valid_left_signed, 4);
      vector signed short upper_bound2 = vec_splat ((vector signed short) valid_left_signed, 5);
      vector signed short lower_bound3 = vec_splat ((vector signed short) valid_left_signed, 6);
      vector signed short upper_bound3 = vec_splat ((vector signed short) valid_left_signed, 7);

      /* compare valid_right_signed to each of the 8 upper and lower bounds */
      #ifdef __ibmxl__
        /* xlc */
        vector bool short cmplow0 = vec_cmpge ((vector signed short) valid_right_signed, lower_bound0);
        vector bool short cmphi0 = vec_cmpge (upper_bound0, (vector signed short) valid_right_signed);
        vector bool short cmplow1 = vec_cmpge ((vector signed short) valid_right_signed, lower_bound1);
        vector bool short cmphi1 = vec_cmpge (upper_bound1, (vector signed short) valid_right_signed);
        vector bool short cmplow2 = vec_cmpge ((vector signed short) valid_right_signed, lower_bound2);
        vector bool short cmphi2 = vec_cmpge (upper_bound2, (vector signed short) valid_right_signed);
        vector bool short cmplow3 = vec_cmpge ((vector signed short) valid_right_signed, lower_bound3);
        vector bool short cmphi3 = vec_cmpge (upper_bound3, (vector signed short) valid_right_signed);
      #else 
        /* gcc */
        vector bool short cmpgtlow0 = vec_cmpgt ((vector signed short) valid_right_signed, lower_bound0);
        vector bool short cmpgthi0 = vec_cmpgt (upper_bound0, (vector signed short) valid_right_signed);
        vector bool short cmpeqlow0 = vec_cmpeq ((vector signed short) valid_right_signed, lower_bound0);
        vector bool short cmpeqhi0 = vec_cmpeq (upper_bound0, (vector signed short) valid_right_signed);
        vector bool short cmplow0 = vec_or (cmpgtlow0, cmpeqlow0);
        vector bool short cmphi0 = vec_or (cmpgthi0, cmpeqhi0);
        
        vector bool short cmpgtlow1 = vec_cmpgt ((vector signed short) valid_right_signed, lower_bound1);
        vector bool short cmpgthi1 = vec_cmpgt (upper_bound1, (vector signed short) valid_right_signed);    
        vector bool short cmpeqlow1 = vec_cmpeq ((vector signed short) valid_right_signed, lower_bound1);
        vector bool short cmpeqhi1 = vec_cmpeq (upper_bound1, (vector signed short) valid_right_signed);
        vector bool short cmplow1 = vec_or (cmpgtlow1, cmpeqlow1);
        vector bool short cmphi1 = vec_or (cmpgthi1, cmpeqhi1);
        
        vector bool short cmpgtlow2 = vec_cmpgt ((vector signed short) valid_right_signed, lower_bound2);
        vector bool short cmpgthi2 = vec_cmpgt (upper_bound2, (vector signed short) valid_right_signed);
        vector bool short cmpeqlow2 = vec_cmpeq ((vector signed short) valid_right_signed, lower_bound2);
        vector bool short cmpeqhi2 = vec_cmpeq (upper_bound2, (vector signed short) valid_right_signed);
        vector bool short cmplow2 = vec_or (cmpgtlow2, cmpeqlow2);
        vector bool short cmphi2 = vec_or (cmpgthi2, cmpeqhi2);
        
        vector bool short cmpgtlow3 = vec_cmpgt ((vector signed short) valid_right_signed, lower_bound3);
        vector bool short cmpgthi3 = vec_cmpgt (upper_bound3, (vector signed short) valid_right_signed);    
        vector bool short cmpeqlow3 = vec_cmpeq ((vector signed short) valid_right_signed, lower_bound3);
        vector bool short cmpeqhi3 = vec_cmpeq (upper_bound3, (vector signed short) valid_right_signed);
        vector bool short cmplow3 = vec_or (cmpgtlow3, cmpeqlow3);
        vector bool short cmphi3 = vec_or (cmpgthi3, cmpeqhi3);
      #endif
      
      /* AND the result of all the comparisons for within range compares */
      vector bool short range0 = vec_and (cmplow0, cmphi0);
      vector bool short range1 = vec_and (cmplow1, cmphi1);
      vector bool short range2 = vec_and (cmplow2, cmphi2);
      vector bool short range3 = vec_and (cmplow3, cmphi3);
      
      /* OR the result of range compares */
      vector bool short range01 = vec_or (range0, range1);
      vector bool short range23 = vec_or (range2, range3);
      vector bool short range0to3 = vec_or (range01, range23);
      
      result_mask = vec_and((vector bool char) range0to3, right_valid_byte_mask);
    }
    result_valid_byte_mask = right_valid_byte_mask;
  }

  else if (operation == 2) {
    /* _SIDD_CMP_EQUAL_EACH */
    /* compare each valid character in right equal to corresponding valid character in left */

    if ((element_type & 0x1) == 0) {
    /* signed char or unsigned char */
      result_mask = vec_cmpeq ((vector unsigned char) valid_left_unsigned, (vector unsigned char) valid_right_unsigned);
    }

    else /* (element_type & 0x1) == 1 */ {
      /* signed short or unsigned short */
      result_mask = (vector bool char) vec_cmpeq ((vector unsigned short) valid_left_unsigned, (vector unsigned short) valid_right_unsigned);
    }
    result_valid_byte_mask = right_valid_byte_mask;
  }
  
  else /* operation == 3 */ {
    /* _SIDD_CMP_EQUAL_ORDERED */
    /* compare each valid character in right to start of valid substring in left */
    /* sign does matter */
    #ifdef __LITTLE_ENDIAN__
      if (element_type == 0) {
        /* unsigned char */
        
        /* shift valid_right_unsigned to right to all positions */
        vector unsigned char valid_right_shift_right0 = valid_right_unsigned;
        vector unsigned char valid_right_shift_right1 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x8));
        vector unsigned char valid_right_shift_right2 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x10));
        vector unsigned char valid_right_shift_right3 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x18));
        vector unsigned char valid_right_shift_right4 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x20));
        vector unsigned char valid_right_shift_right5 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x28));
        vector unsigned char valid_right_shift_right6 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x30));
        vector unsigned char valid_right_shift_right7 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x38));
        vector unsigned char valid_right_shift_right8 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x40));
        vector unsigned char valid_right_shift_right9 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x48));
        vector unsigned char valid_right_shift_right10 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x50));
        vector unsigned char valid_right_shift_right11 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x58));
        vector unsigned char valid_right_shift_right12 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x60));
        vector unsigned char valid_right_shift_right13 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x68));
        vector unsigned char valid_right_shift_right14 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x70));
        vector unsigned char valid_right_shift_right15 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x78));
        
        /* compare valid_right_shift_right0..15 to valid_left_unsigned */
        vector bool char compare_valid_left_shift_right0 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right0);
        vector bool char compare_valid_left_shift_right1 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right1);
        vector bool char compare_valid_left_shift_right2 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right2);
        vector bool char compare_valid_left_shift_right3 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right3);
        vector bool char compare_valid_left_shift_right4 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right4);
        vector bool char compare_valid_left_shift_right5 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right5);
        vector bool char compare_valid_left_shift_right6 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right6);
        vector bool char compare_valid_left_shift_right7 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right7);
        vector bool char compare_valid_left_shift_right8 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right8);
        vector bool char compare_valid_left_shift_right9 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right9);
        vector bool char compare_valid_left_shift_right10 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right10);
        vector bool char compare_valid_left_shift_right11 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right11);
        vector bool char compare_valid_left_shift_right12 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right12);
        vector bool char compare_valid_left_shift_right13 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right13);
        vector bool char compare_valid_left_shift_right14 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right14);
        vector bool char compare_valid_left_shift_right15 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right15);
        
        /* and compare_valid_left_shift_right0..15 with left_valid_byte_mask to clear invalid bytes */
        compare_valid_left_shift_right0 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right0);
        compare_valid_left_shift_right1 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right1);
        compare_valid_left_shift_right2 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right2);
        compare_valid_left_shift_right3 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right3);
        compare_valid_left_shift_right4 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right4);
        compare_valid_left_shift_right5 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right5);
        compare_valid_left_shift_right6 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right6);
        compare_valid_left_shift_right7 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right7);
        compare_valid_left_shift_right8 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right8);
        compare_valid_left_shift_right9 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right9);
        compare_valid_left_shift_right10 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right10);
        compare_valid_left_shift_right11 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right11);
        compare_valid_left_shift_right12 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right12);
        compare_valid_left_shift_right13 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right13);
        compare_valid_left_shift_right14 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right14);
        compare_valid_left_shift_right15 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right15);
        
        /* gather bits by bytes compare_valid_left_shift_right0..15 */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb0 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right7);
          vector unsigned char gbb8 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right8);
          vector unsigned char gbb9 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right9);
          vector unsigned char gbb10 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right10);
          vector unsigned char gbb11 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right11);
          vector unsigned char gbb12 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right12);
          vector unsigned char gbb13 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right13);
          vector unsigned char gbb14 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right14);
          vector unsigned char gbb15 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right15);
        #else
          /* gcc */
          vector unsigned char gbb0 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right7);
          vector unsigned char gbb8 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right8);
          vector unsigned char gbb9 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right9);
          vector unsigned char gbb10 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right10);
          vector unsigned char gbb11 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right11);
          vector unsigned char gbb12 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right12);
          vector unsigned char gbb13 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right13);
          vector unsigned char gbb14 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right14);
          vector unsigned char gbb15 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right15);
        #endif
        
        /* permute gbb0-15 to put the 0th and 8th bytes together and pack them in binary tree order to enable parallelism */
        vector unsigned char gbb_permute_mask01 = (vector unsigned char) { 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask23 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask45 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask67 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18 };
        vector unsigned char gbb_permute_mask89 = (vector unsigned char) { 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask1011 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask1213 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask1415 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18 };
        vector unsigned char gbb_permute_mask0to3 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x14, 0x15, 0x16,0x17, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask4to7 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x08,0x09, 0x0A,0x0B, 0x1C, 0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask8to11 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x14, 0x15, 0x16,0x17, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask12to15 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x08,0x09, 0x0A,0x0B, 0x1C, 0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask0to7 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x04, 0x05, 0x06,0x07, 0x18,0x19, 0x1A,0x1B, 0x1C,0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask8to15 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x04, 0x05, 0x06,0x07, 0x18,0x19, 0x1A,0x1B, 0x1C,0x1D, 0x1E,0x1F };
        vector unsigned char pack_gbb01 = vec_perm (gbb0, gbb1, gbb_permute_mask01);
        vector unsigned char pack_gbb23 = vec_perm (gbb2, gbb3, gbb_permute_mask23);
        vector unsigned char pack_gbb45 = vec_perm (gbb4, gbb5, gbb_permute_mask45);
        vector unsigned char pack_gbb67 = vec_perm (gbb6, gbb7, gbb_permute_mask67);
        vector unsigned char pack_gbb89 = vec_perm (gbb8, gbb9, gbb_permute_mask89);
        vector unsigned char pack_gbb1011 = vec_perm (gbb10, gbb11, gbb_permute_mask1011);
        vector unsigned char pack_gbb1213 = vec_perm (gbb12, gbb13, gbb_permute_mask1213);
        vector unsigned char pack_gbb1415 = vec_perm (gbb14, gbb15, gbb_permute_mask1415);
        vector unsigned char pack_gbb0to3 = vec_perm (pack_gbb01, pack_gbb23, gbb_permute_mask0to3);
        vector unsigned char pack_gbb4to7 = vec_perm (pack_gbb45, pack_gbb67, gbb_permute_mask4to7);
        vector unsigned char pack_gbb8to11 = vec_perm (pack_gbb89, pack_gbb1011, gbb_permute_mask8to11);
        vector unsigned char pack_gbb12to15 = vec_perm (pack_gbb1213, pack_gbb1415, gbb_permute_mask12to15);
        vector unsigned char pack_gbb0to7 = vec_perm (pack_gbb0to3, pack_gbb4to7, gbb_permute_mask0to7);
        vector unsigned char pack_gbb8to15 = vec_perm (pack_gbb8to11, pack_gbb12to15, gbb_permute_mask8to15);
        
        /* calculate mask for substring match */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb_substring = (vector unsigned char) vec_gbb ((vector unsigned long long) left_valid_byte_mask);
        #else 
          /* gcc */
          vector unsigned char gbb_substring = vec_vgbbd ((vector unsigned char) left_valid_byte_mask);
        #endif
        /* puts the byte 0 of gbb_substring into even bytes and byte 8 into odd bytes */
        vector unsigned char substring_permute_mask = (vector unsigned char) { 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08 };
        vector unsigned char substring_mask = vec_perm (gbb_substring, gbb_substring, substring_permute_mask);
        
        /* compare substring_mask to pack_gbb0to7 and pack_gbb8to15 to find matches */
        vector bool short compare_gbb0to7 = vec_cmpeq ((vector unsigned short) pack_gbb0to7, (vector unsigned short) substring_mask);
        vector bool short compare_gbb8to15 = vec_cmpeq ((vector unsigned short) pack_gbb8to15, (vector unsigned short) substring_mask);
        
        /* turn compare_gbb0to7 and compare_gbb8to15 into a mask of 8 bits per element */
        vector bool char compare_gbb0to15 = vec_pack ((vector bool short) compare_gbb0to7, (vector bool short) compare_gbb8to15);
        
        result_mask = compare_gbb0to15;
      }
      else if (element_type == 1) {
        /* unsigned short */
        
        /* shift valid_right_unsigned to right to all positions */
        vector unsigned short valid_right_shift_right0 = (vector unsigned short) valid_right_unsigned;
        vector unsigned short valid_right_shift_right1 = vec_sro ((vector unsigned short) valid_right_unsigned, vec_splats((unsigned char) 0x10));
        vector unsigned short valid_right_shift_right2 = vec_sro ((vector unsigned short) valid_right_unsigned, vec_splats((unsigned char) 0x20));
        vector unsigned short valid_right_shift_right3 = vec_sro ((vector unsigned short) valid_right_unsigned, vec_splats((unsigned char) 0x30));
        vector unsigned short valid_right_shift_right4 = vec_sro ((vector unsigned short) valid_right_unsigned, vec_splats((unsigned char) 0x40));
        vector unsigned short valid_right_shift_right5 = vec_sro ((vector unsigned short) valid_right_unsigned, vec_splats((unsigned char) 0x50));
        vector unsigned short valid_right_shift_right6 = vec_sro ((vector unsigned short) valid_right_unsigned, vec_splats((unsigned char) 0x60));
        vector unsigned short valid_right_shift_right7 = vec_sro ((vector unsigned short) valid_right_unsigned, vec_splats((unsigned char) 0x70));
        
        /* compare valid_right_shift_right0..7 to valid_left_unsigned */
        vector bool short compare_valid_left_shift_right0 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right0);
        vector bool short compare_valid_left_shift_right1 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right1);
        vector bool short compare_valid_left_shift_right2 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right2);
        vector bool short compare_valid_left_shift_right3 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right3);
        vector bool short compare_valid_left_shift_right4 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right4);
        vector bool short compare_valid_left_shift_right5 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right5);
        vector bool short compare_valid_left_shift_right6 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right6);
        vector bool short compare_valid_left_shift_right7 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right7);
        
        /* and compare_valid_left_shift_right0..7 with left_valid_byte_mask to clear invalid bytes */
        compare_valid_left_shift_right0 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right0);
        compare_valid_left_shift_right1 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right1);
        compare_valid_left_shift_right2 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right2);
        compare_valid_left_shift_right3 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right3);
        compare_valid_left_shift_right4 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right4);
        compare_valid_left_shift_right5 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right5);
        compare_valid_left_shift_right6 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right6);
        compare_valid_left_shift_right7 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right7);
        
        /* gather bits by bytes compare_valid_left_shift_right0..7 */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb0 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right7);
        #else
          /* gcc */
          vector unsigned char gbb0 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right7);
        #endif
        
        /* permute gbb0-7 to put the 0th and 4th shorts together and pack them in binary tree order to enable parallelism */
        vector unsigned char gbb_permute_mask01 = (vector unsigned char) { 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask23 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask45 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask67 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18 };
        vector unsigned char gbb_permute_mask0to3 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x14, 0x15, 0x16,0x17, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask4to7 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x08,0x09, 0x0A,0x0B, 0x1C, 0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask0to7 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x04, 0x05, 0x06,0x07, 0x18,0x19, 0x1A,0x1B, 0x1C,0x1D, 0x1E,0x1F };
        vector unsigned char pack_gbb01 = vec_perm (gbb0, gbb1, gbb_permute_mask01);
        vector unsigned char pack_gbb23 = vec_perm (gbb2, gbb3, gbb_permute_mask23);
        vector unsigned char pack_gbb45 = vec_perm (gbb4, gbb5, gbb_permute_mask45);
        vector unsigned char pack_gbb67 = vec_perm (gbb6, gbb7, gbb_permute_mask67);
        vector unsigned char pack_gbb0to3 = vec_perm (pack_gbb01, pack_gbb23, gbb_permute_mask0to3);
        vector unsigned char pack_gbb4to7 = vec_perm (pack_gbb45, pack_gbb67, gbb_permute_mask4to7);
        vector unsigned char pack_gbb0to7 = vec_perm (pack_gbb0to3, pack_gbb4to7, gbb_permute_mask0to7);
        
        /* calculate mask for substring match */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb_substring = (vector unsigned char) vec_gbb ((vector unsigned long long) left_valid_byte_mask);
        #else 
          /* gcc */
          vector unsigned char gbb_substring = vec_vgbbd ((vector unsigned char) left_valid_byte_mask);
        #endif
        /* puts the byte 0 of gbb_substring into even bytes and byte 8 into odd bytes */
        vector unsigned char substring_permute_mask = (vector unsigned char) { 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08 };
        vector unsigned char substring_mask = vec_perm (gbb_substring, gbb_substring, substring_permute_mask);
        
        /* compare substring_mask to pack_gbb0to7 to find matches */
        vector bool short compare_gbb0to7 = vec_cmpeq ((vector unsigned short) pack_gbb0to7, (vector unsigned short) substring_mask);

        result_mask = (vector bool char) compare_gbb0to7;
      }
      if (element_type == 2) {
        /* signed char */
        
        /* shift valid_right_unsigned to right to all positions */
        vector signed char valid_right_shift_right0 = (vector signed char) valid_right_signed;
        vector signed char valid_right_shift_right1 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x8));
        vector signed char valid_right_shift_right2 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x10));
        vector signed char valid_right_shift_right3 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x18));
        vector signed char valid_right_shift_right4 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x20));
        vector signed char valid_right_shift_right5 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x28));
        vector signed char valid_right_shift_right6 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x30));
        vector signed char valid_right_shift_right7 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x38));
        vector signed char valid_right_shift_right8 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x40));
        vector signed char valid_right_shift_right9 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x48));
        vector signed char valid_right_shift_right10 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x50));
        vector signed char valid_right_shift_right11 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x58));
        vector signed char valid_right_shift_right12 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x60));
        vector signed char valid_right_shift_right13 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x68));
        vector signed char valid_right_shift_right14 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x70));
        vector signed char valid_right_shift_right15 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x78));
        
        /* compare valid_right_shift_right0..15 to valid_left_unsigned */
        vector bool char compare_valid_left_shift_right0 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right0);
        vector bool char compare_valid_left_shift_right1 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right1);
        vector bool char compare_valid_left_shift_right2 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right2);
        vector bool char compare_valid_left_shift_right3 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right3);
        vector bool char compare_valid_left_shift_right4 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right4);
        vector bool char compare_valid_left_shift_right5 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right5);
        vector bool char compare_valid_left_shift_right6 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right6);
        vector bool char compare_valid_left_shift_right7 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right7);
        vector bool char compare_valid_left_shift_right8 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right8);
        vector bool char compare_valid_left_shift_right9 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right9);
        vector bool char compare_valid_left_shift_right10 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right10);
        vector bool char compare_valid_left_shift_right11 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right11);
        vector bool char compare_valid_left_shift_right12 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right12);
        vector bool char compare_valid_left_shift_right13 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right13);
        vector bool char compare_valid_left_shift_right14 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right14);
        vector bool char compare_valid_left_shift_right15 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right15);
        
        /* and compare_valid_left_shift_right0..15 with left_valid_byte_mask to clear invalid bytes */
        compare_valid_left_shift_right0 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right0);
        compare_valid_left_shift_right1 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right1);
        compare_valid_left_shift_right2 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right2);
        compare_valid_left_shift_right3 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right3);
        compare_valid_left_shift_right4 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right4);
        compare_valid_left_shift_right5 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right5);
        compare_valid_left_shift_right6 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right6);
        compare_valid_left_shift_right7 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right7);
        compare_valid_left_shift_right8 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right8);
        compare_valid_left_shift_right9 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right9);
        compare_valid_left_shift_right10 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right10);
        compare_valid_left_shift_right11 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right11);
        compare_valid_left_shift_right12 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right12);
        compare_valid_left_shift_right13 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right13);
        compare_valid_left_shift_right14 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right14);
        compare_valid_left_shift_right15 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right15);
        
        /* gather bits by bytes compare_valid_left_shift_right0..15 */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb0 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right7);
          vector unsigned char gbb8 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right8);
          vector unsigned char gbb9 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right9);
          vector unsigned char gbb10 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right10);
          vector unsigned char gbb11 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right11);
          vector unsigned char gbb12 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right12);
          vector unsigned char gbb13 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right13);
          vector unsigned char gbb14 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right14);
          vector unsigned char gbb15 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right15);
        #else
          /* gcc */
          vector unsigned char gbb0 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right7);
          vector unsigned char gbb8 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right8);
          vector unsigned char gbb9 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right9);
          vector unsigned char gbb10 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right10);
          vector unsigned char gbb11 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right11);
          vector unsigned char gbb12 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right12);
          vector unsigned char gbb13 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right13);
          vector unsigned char gbb14 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right14);
          vector unsigned char gbb15 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right15);
        #endif
        
        /* permute gbb0-15 to put the 0th and 8th bytes together and pack them in binary tree order to enable parallelism */
        vector unsigned char gbb_permute_mask01 = (vector unsigned char) { 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask23 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask45 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask67 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18 };
        vector unsigned char gbb_permute_mask89 = (vector unsigned char) { 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask1011 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask1213 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask1415 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18 };
        vector unsigned char gbb_permute_mask0to3 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x14, 0x15, 0x16,0x17, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask4to7 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x08,0x09, 0x0A,0x0B, 0x1C, 0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask8to11 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x14, 0x15, 0x16,0x17, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask12to15 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x08,0x09, 0x0A,0x0B, 0x1C, 0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask0to7 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x04, 0x05, 0x06,0x07, 0x18,0x19, 0x1A,0x1B, 0x1C,0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask8to15 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x04, 0x05, 0x06,0x07, 0x18,0x19, 0x1A,0x1B, 0x1C,0x1D, 0x1E,0x1F };
        vector unsigned char pack_gbb01 = vec_perm (gbb0, gbb1, gbb_permute_mask01);
        vector unsigned char pack_gbb23 = vec_perm (gbb2, gbb3, gbb_permute_mask23);
        vector unsigned char pack_gbb45 = vec_perm (gbb4, gbb5, gbb_permute_mask45);
        vector unsigned char pack_gbb67 = vec_perm (gbb6, gbb7, gbb_permute_mask67);
        vector unsigned char pack_gbb89 = vec_perm (gbb8, gbb9, gbb_permute_mask89);
        vector unsigned char pack_gbb1011 = vec_perm (gbb10, gbb11, gbb_permute_mask1011);
        vector unsigned char pack_gbb1213 = vec_perm (gbb12, gbb13, gbb_permute_mask1213);
        vector unsigned char pack_gbb1415 = vec_perm (gbb14, gbb15, gbb_permute_mask1415);
        vector unsigned char pack_gbb0to3 = vec_perm (pack_gbb01, pack_gbb23, gbb_permute_mask0to3);
        vector unsigned char pack_gbb4to7 = vec_perm (pack_gbb45, pack_gbb67, gbb_permute_mask4to7);
        vector unsigned char pack_gbb8to11 = vec_perm (pack_gbb89, pack_gbb1011, gbb_permute_mask8to11);
        vector unsigned char pack_gbb12to15 = vec_perm (pack_gbb1213, pack_gbb1415, gbb_permute_mask12to15);
        vector unsigned char pack_gbb0to7 = vec_perm (pack_gbb0to3, pack_gbb4to7, gbb_permute_mask0to7);
        vector unsigned char pack_gbb8to15 = vec_perm (pack_gbb8to11, pack_gbb12to15, gbb_permute_mask8to15);
        
        /* calculate mask for substring match */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb_substring = (vector unsigned char) vec_gbb ((vector unsigned long long) left_valid_byte_mask);
        #else 
          /* gcc */
          vector unsigned char gbb_substring = vec_vgbbd ((vector unsigned char) left_valid_byte_mask);
        #endif
        /* puts the byte 0 of gbb_substring into even bytes and byte 8 into odd bytes */
        vector unsigned char substring_permute_mask = (vector unsigned char) { 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08 };
        vector unsigned char substring_mask = vec_perm (gbb_substring, gbb_substring, substring_permute_mask);
        
        /* compare substring_mask to pack_gbb0to7 and pack_gbb8to15 to find matches */
        vector bool short compare_gbb0to7 = vec_cmpeq ((vector unsigned short) pack_gbb0to7, (vector unsigned short) substring_mask);
        vector bool short compare_gbb8to15 = vec_cmpeq ((vector unsigned short) pack_gbb8to15, (vector unsigned short) substring_mask);
        
        /* turn compare_gbb0to7 and compare_gbb8to15 into a mask of 8 bits per element */
        vector bool char compare_gbb0to15 = vec_pack ((vector bool short) compare_gbb0to7, (vector bool short) compare_gbb8to15);
        
        result_mask = compare_gbb0to15;
      }
      else if (element_type == 3) {
        /* signed short */
        
        /* shift valid_right_unsigned to right to all positions */
        vector signed short valid_right_shift_right0 = (vector signed short) valid_right_signed;
        vector signed short valid_right_shift_right1 = vec_sro ((vector signed short) valid_right_signed, vec_splats((unsigned char) 0x10));
        vector signed short valid_right_shift_right2 = vec_sro ((vector signed short) valid_right_signed, vec_splats((unsigned char) 0x20));
        vector signed short valid_right_shift_right3 = vec_sro ((vector signed short) valid_right_signed, vec_splats((unsigned char) 0x30));
        vector signed short valid_right_shift_right4 = vec_sro ((vector signed short) valid_right_signed, vec_splats((unsigned char) 0x40));
        vector signed short valid_right_shift_right5 = vec_sro ((vector signed short) valid_right_signed, vec_splats((unsigned char) 0x50));
        vector signed short valid_right_shift_right6 = vec_sro ((vector signed short) valid_right_signed, vec_splats((unsigned char) 0x60));
        vector signed short valid_right_shift_right7 = vec_sro ((vector signed short) valid_right_signed, vec_splats((unsigned char) 0x70));
        
        /* compare valid_right_shift_right0..7 to valid_left_unsigned */
        vector bool short compare_valid_left_shift_right0 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right0);
        vector bool short compare_valid_left_shift_right1 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right1);
        vector bool short compare_valid_left_shift_right2 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right2);
        vector bool short compare_valid_left_shift_right3 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right3);
        vector bool short compare_valid_left_shift_right4 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right4);
        vector bool short compare_valid_left_shift_right5 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right5);
        vector bool short compare_valid_left_shift_right6 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right6);
        vector bool short compare_valid_left_shift_right7 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right7);
        
        /* and compare_valid_left_shift_right0..7 with left_valid_byte_mask to clear invalid bytes */
        compare_valid_left_shift_right0 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right0);
        compare_valid_left_shift_right1 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right1);
        compare_valid_left_shift_right2 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right2);
        compare_valid_left_shift_right3 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right3);
        compare_valid_left_shift_right4 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right4);
        compare_valid_left_shift_right5 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right5);
        compare_valid_left_shift_right6 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right6);
        compare_valid_left_shift_right7 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right7);
        
        /* gather bits by bytes compare_valid_left_shift_right0..7 */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb0 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right7);
        #else
          /* gcc */
          vector unsigned char gbb0 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right7);
        #endif
        
        /* permute gbb0-7 to put the 0th and 4th shorts together and pack them in binary tree order to enable parallelism */
        vector unsigned char gbb_permute_mask01 = (vector unsigned char) { 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask23 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask45 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask67 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18 };
        vector unsigned char gbb_permute_mask0to3 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x14, 0x15, 0x16,0x17, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask4to7 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x08,0x09, 0x0A,0x0B, 0x1C, 0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask0to7 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x04, 0x05, 0x06,0x07, 0x18,0x19, 0x1A,0x1B, 0x1C,0x1D, 0x1E,0x1F };
        vector unsigned char pack_gbb01 = vec_perm (gbb0, gbb1, gbb_permute_mask01);
        vector unsigned char pack_gbb23 = vec_perm (gbb2, gbb3, gbb_permute_mask23);
        vector unsigned char pack_gbb45 = vec_perm (gbb4, gbb5, gbb_permute_mask45);
        vector unsigned char pack_gbb67 = vec_perm (gbb6, gbb7, gbb_permute_mask67);
        vector unsigned char pack_gbb0to3 = vec_perm (pack_gbb01, pack_gbb23, gbb_permute_mask0to3);
        vector unsigned char pack_gbb4to7 = vec_perm (pack_gbb45, pack_gbb67, gbb_permute_mask4to7);
        vector unsigned char pack_gbb0to7 = vec_perm (pack_gbb0to3, pack_gbb4to7, gbb_permute_mask0to7);
        
        /* calculate mask for substring match */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb_substring = (vector unsigned char) vec_gbb ((vector unsigned long long) left_valid_byte_mask);
        #else 
          /* gcc */
          vector unsigned char gbb_substring = vec_vgbbd ((vector unsigned char) left_valid_byte_mask);
        #endif
        /* puts the byte 0 of gbb_substring into even bytes and byte 8 into odd bytes */
        vector unsigned char substring_permute_mask = (vector unsigned char) { 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08 };
        vector unsigned char substring_mask = vec_perm (gbb_substring, gbb_substring, substring_permute_mask);
        
        /* compare substring_mask to pack_gbb0to7 to find matches */
        vector bool short compare_gbb0to7 = vec_cmpeq ((vector unsigned short) pack_gbb0to7, (vector unsigned short) substring_mask);

        result_mask = (vector bool char) compare_gbb0to7;
      }
    #elif __BIG_ENDIAN__
      #error _SIDD_CMP_EQUAL_ORDERED is not supported on machines lower than POWER8.
    #endif
    
    result_valid_byte_mask = right_valid_byte_mask;
  }

  /* mask polarity */
  unsigned int polarity = (control >> 4) & 0x3;  /* bits 5:4 */
  if (polarity == 1) {
    /* _SIDD_NEGATIVE_POLARITY */
    /* complement result mask */
    result_mask = vec_nor (result_mask, result_mask);
  }

  else if (polarity == 3) {
    /* _SIDD_MASKED_NEGATIVE_POLARITY */
    /* complement result mask only before end of string */
    result_mask = vec_sel (/* if invalid */ result_mask,
                           /* if valid */   vec_nor (result_mask, result_mask),
                           result_valid_byte_mask);
  }

  else /* polarity == 0 or 2 */ {
    /* _SIDD_POSITIVE_POLARITY */
    /* leave result mask unchanged */
  }

  /* Return bit mask or element mask */
  unsigned int bit_or_element = (control >> 6) & 0x1;  /* bit 6 */

  if (bit_or_element == 0) {
    /* _SIDD_BIT_MASK - return mask of single bits */
    vector bool char result_mask_to_transpose = result_mask;
    if ((element_type & 0x1) == 1) {
      /* signed short or unsigned short */
      /* extract one byte of each short (both bytes are the same; extracting low byte is simpler) */
      result_mask_to_transpose = (vector bool char) vec_pack ((vector unsigned short) result_mask,
                                                              (vector unsigned short) zeros);
    }

    vector unsigned char bit_transposed_mask;
    /* bit-transpose each half of result_mask, using Power8 vector gather bits by bytes by doubleword */
    #ifdef _ARCH_PWR8
      #ifdef __ibmxl__
        /* xlc */
        bit_transposed_mask = (vector unsigned char) vec_gbb ((vector unsigned long long) result_mask_to_transpose);
      #else
        /* gcc */
        bit_transposed_mask = vec_vgbbd ((vector unsigned char) result_mask_to_transpose);
      #endif
    #else
      /* emulate vector gather bits by bytes by doublewords for one bit only of each byte */
      /* all bits of each byte are identical, so extract the one that avoids bit shifting */
      vector unsigned char mask_bits_to_extract = (vector unsigned char)
          { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,  0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 };
      vector unsigned char extracted_mask_bits = vec_and (result_mask_to_transpose, mask_bits_to_extract);
      /* gather extracted bits for each doubleword into its byte 0 (leaving garbage in its other bytes) */
      vector unsigned char btmbit0 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 7*8));
      vector unsigned char btmbit1 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 6*8));
      vector unsigned char btmbit2 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 5*8));
      vector unsigned char btmbit3 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 4*8));
      vector unsigned char btmbit4 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 3*8));
      vector unsigned char btmbit5 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 2*8));
      vector unsigned char btmbit6 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 1*8));
      vector unsigned char btmbit7 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 0*8));
      vector unsigned char btmbits01 = vec_or (btmbit0, btmbit1);
      vector unsigned char btmbits23 = vec_or (btmbit2, btmbit3);
      vector unsigned char btmbits45 = vec_or (btmbit4, btmbit5);
      vector unsigned char btmbits67 = vec_or (btmbit6, btmbit7);
      vector unsigned char btmbits0to3 = vec_or (btmbits01, btmbits23);
      vector unsigned char btmbits4to7 = vec_or (btmbits45, btmbits67);
      vector unsigned char btmbits0to7 = vec_or (btmbits0to3, btmbits4to7);
      bit_transposed_mask = btmbits0to7;
    #endif

    vector unsigned char select_transposed_upper_bits;
    if ((element_type & 0x1) == 0) {
      /* signed char or unsigned char */
      /* extract leftmost bit of each byte, to two bytes with other 14 bytes zeroed */
      select_transposed_upper_bits = (vector unsigned char)  /* need leftmost byte of each doubleword */
      #ifdef __LITTLE_ENDIAN__
          /* for little endian these two bytes must be right justified in the VR to be stored in the first short element */
          /* the mask for the right (low address) half is stored in byte 0 */
          /* the following will be in the reverse order in the VR */
          { 0x10, 0x18, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 };
      #elif __BIG_ENDIAN__
          /* for big endian these two bytes must be left justified in the VR to be stored in the first short element */
          /* caution: some applications might access it as an int or long long? */
          /* the mask for the left (low address) half is stored in byte 0 */
          { 0x10, 0x18, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 };
      #endif
    }

    else /* (element_type & 0x1) == 1 */ {
      /* signed short or unsigned short */
      /* extract leftmost bit of one byte of each short, to one byte with other 15 bytes zeroed */
      select_transposed_upper_bits = (vector unsigned char)  /* need leftmost byte of left doubleword */
      #ifdef __LITTLE_ENDIAN__
          /* for little endian this byte must be right justified to be stored in the first char element */
          // { 0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0x10 };
          { 0x10, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 };
      #elif __BIG_ENDIAN__
          /* for big endian this byte must be left justified to be stored in the first char element */
          { 0x10, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 };
      #endif
    }

    /* extract leftmost byte of each doubleword, to 2 or 1 bytes with other 14 or 15 bytes zeroed */
    result_mask = (vector bool char) vec_perm (zeros, bit_transposed_mask, select_transposed_upper_bits);
  }

  else /* bit_or_element == 1 */ {
    /* _SIDD_UNIT_MASK - return mask of 8-bit or 16-bit elements */
    /* leave result_mask unchanged */
  }

  return (__m128i) result_mask;
}

/* Compare length terminated strings in various ways, giving index */
VECLIB_INLINE int vec_comparelengthstringstoindex1q (__m128i left, intlit5 leftlen, __m128i right, intlit5 rightlen, intlit8 control)
  /* See "String Control" above. */
  /* Except for ranges, a zero terminator and all characters after it are invalid.  Characters before that are valid. */
  /* _SIDD_CMP_EQUAL_ANY:     Each valid character in right is compared to every valid character in left.             */
  /*                          Result mask is true if it matches any of them, false if not or if it is invalid.        */
  /* _SIDD_CMP_RANGES:        Each valid character in right is compared to the two ranges in left.                    */
  /*                          Left contains four characters: lower bound 1, upper bound 1, lower 2, upper 2.          */
  /*                          Result mask is true if it is within either bound, false if not or if it is invalid.     */
  /* _SIDD_CMP_EQUAL_EACH:    Each valid character in right is compared to the corresponding valid character in left. */
  /*                          Result mask is true if they match, false if not or if either is invalid.                */
  /* _SIDD_CMP_EQUAL_ORDERED: Each sequence of valid characters in right is compared to the the valid string in left. */
  /*                          Result mask is true if the sequence starting at that character matches all of left,     */
  /*                          false if not or if it is invalid.                                                       */
{
  vector unsigned char zeros = (vector unsigned char) {0, 0, 0, 0};
  vector bool char result_mask;
  int index;
  
  unsigned int operation = (control >> 2) & 0x3;  /* bits 3:2 */
  unsigned int element_type = control & 0x3;  /* bit 0 */

  vector bool char left_valid_byte_mask;
  vector bool char right_valid_byte_mask;
  vector bool char result_valid_byte_mask;
  vector unsigned char valid_left_unsigned;
  vector unsigned char valid_right_unsigned;
  vector signed char valid_left_signed;
  vector signed char valid_right_signed;

  /* clear invalid bytes */
  /* create mask based on the length argument */
  if ((element_type & 0x01) == 0) /* element_type 00 10 */ {
    static const vector bool char valid_byte_mask_selector[17] = {
        { 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 0 */
        { 0xFF,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 1 */
        { 0xFF,0xFF, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 2 */
        { 0xFF,0xFF, 0xFF,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 3 */
        { 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 4 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 5 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 6 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 7 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 8 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 9 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 10 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0x00, 0x00,0x00, 0x00,0x00 }, /* length 11 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x00,0x00 }, /* length 12 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0x00, 0x00,0x00 }, /* length 13 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00 }, /* length 14 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0x00 }, /* length 15 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF }  /* length 16 */
    };
    
    /* truncate the last element in the left_valid_byte_mask if the length of it is odd if _SIDD_CMP_RANGES*/
    if (operation == 1) {
      /* _SIDD_CMP_RANGES */
      left_valid_byte_mask = valid_byte_mask_selector[(leftlen / 2) * 2];
    } else /* operation 0 2 3 */ {
      left_valid_byte_mask = valid_byte_mask_selector[leftlen];
    }
    right_valid_byte_mask = valid_byte_mask_selector[rightlen];
  } else /* element_type 01 11*/ {
    static const vector bool char valid_short_mask_selector[9] = {
        { 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 0 */
        { 0xFF,0xFF, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 1 */
        { 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 2 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 3 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 4 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x00,0x00, 0x00,0x00 }, /* length 5 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x00,0x00 }, /* length 6 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0x00,0x00 }, /* length 7 */
        { 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF }  /* length 8 */
    };
    /* truncate the last element in the left_valid_byte_mask if the length of it is odd if _SIDD_CMP_RANGES*/
    if (operation == 1) {
      /* _SIDD_CMP_RANGES */
      left_valid_byte_mask = valid_short_mask_selector[(leftlen / 2) * 2];
    } else /* operation 0 2 3 */ {
      left_valid_byte_mask = valid_short_mask_selector[leftlen];
    }
    right_valid_byte_mask = valid_short_mask_selector[rightlen];    
  }

  /* clear invalid characters */
  if (((element_type == 2) || (element_type == 3)) && ((operation == 1) || (operation == 3))) {
    /* signed and _SIDD_CMP_EQUAL_ORDERED */
    valid_left_signed  = vec_and ((vector signed char) left,  left_valid_byte_mask);
    valid_right_signed = vec_and ((vector signed char) right, right_valid_byte_mask);
  }
  else /* unsigned */ {
    valid_left_unsigned  = vec_and ((vector unsigned char) left,  left_valid_byte_mask);
    valid_right_unsigned = vec_and ((vector unsigned char) right, right_valid_byte_mask);
  }
  
  /* compare operation */
  if (operation == 0) {
    /* _SIDD_CMP_EQUAL_ANY */
    /* compare each valid character in right to every valid character in left */
    /* sign does not matter */
    if ((element_type & 0x1) == 0) {
      /* compare each valid char in right to every valid char in left */
      /* first rotate left to all positions */
      vector unsigned char left0  = (vector unsigned char) valid_left_unsigned;
      vector unsigned char left1  = vec_sld (left0, left0, 1);
      vector unsigned char left2  = vec_sld (left0, left0, 2);
      vector unsigned char left3  = vec_sld (left0, left0, 3);
      vector unsigned char left4  = vec_sld (left0, left0, 4);
      vector unsigned char left5  = vec_sld (left0, left0, 5);
      vector unsigned char left6  = vec_sld (left0, left0, 6);
      vector unsigned char left7  = vec_sld (left0, left0, 7);
      vector unsigned char left8  = vec_sld (left0, left0, 8);
      vector unsigned char left9  = vec_sld (left0, left0, 9);
      vector unsigned char left10 = vec_sld (left0, left0, 10);
      vector unsigned char left11 = vec_sld (left0, left0, 11);
      vector unsigned char left12 = vec_sld (left0, left0, 12);
      vector unsigned char left13 = vec_sld (left0, left0, 13);
      vector unsigned char left14 = vec_sld (left0, left0, 14);
      vector unsigned char left15 = vec_sld (left0, left0, 15);
      vector bool char match0  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left0);
      vector bool char match1  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left1);
      vector bool char match2  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left2);
      vector bool char match3  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left3);
      vector bool char match4  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left4);
      vector bool char match5  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left5);
      vector bool char match6  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left6);
      vector bool char match7  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left7);
      vector bool char match8  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left8);
      vector bool char match9  = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left9);
      vector bool char match10 = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left10);
      vector bool char match11 = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left11);
      vector bool char match12 = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left12);
      vector bool char match13 = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left13);
      vector bool char match14 = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left14);
      vector bool char match15 = vec_cmpeq ((vector unsigned char) valid_right_unsigned, left15);
      vector bool char match01   = vec_or (match0,  match1);
      vector bool char match23   = vec_or (match2,  match3);
      vector bool char match45   = vec_or (match4,  match5);
      vector bool char match67   = vec_or (match6,  match7);
      vector bool char match89   = vec_or (match8,  match9);
      vector bool char match1011 = vec_or (match10, match11);
      vector bool char match1213 = vec_or (match12, match13);
      vector bool char match1415 = vec_or (match14, match15);
      vector bool char match0to3   = vec_or (match01,   match23);
      vector bool char match4to7   = vec_or (match45,   match67);
      vector bool char match8to11  = vec_or (match89,   match1011);
      vector bool char match12to15 = vec_or (match1213, match1415);
      vector bool char match0to7  = vec_or (match0to3,  match4to7);
      vector bool char match8to15 = vec_or (match8to11, match12to15);
      vector bool char match0to15 = vec_or (match0to7, match8to15);
      result_mask = vec_and ((vector bool char) match0to15, right_valid_byte_mask);
    }

    else /* (element_type & 0x1) == 1 */ {
      /* compare each valid short in right to every valid short in left */
      /* first rotate left to all positions */
      vector unsigned short left0 = (vector unsigned short) valid_left_unsigned;
      vector unsigned short left1 = vec_sld (left0, left0, 2);
      vector unsigned short left2 = vec_sld (left0, left0, 4);
      vector unsigned short left3 = vec_sld (left0, left0, 6);
      vector unsigned short left4 = vec_sld (left0, left0, 8);
      vector unsigned short left5 = vec_sld (left0, left0, 10);
      vector unsigned short left6 = vec_sld (left0, left0, 12);
      vector unsigned short left7 = vec_sld (left0, left0, 14);
      vector bool short match0 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left0);
      vector bool short match1 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left1);
      vector bool short match2 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left2);
      vector bool short match3 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left3);
      vector bool short match4 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left4);
      vector bool short match5 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left5);
      vector bool short match6 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left6);
      vector bool short match7 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, left7);
      vector bool short match01 = vec_or (match0, match1);
      vector bool short match23 = vec_or (match2, match3);
      vector bool short match45 = vec_or (match4, match5);
      vector bool short match67 = vec_or (match6, match7);
      vector bool short match0to3 = vec_or (match01, match23);
      vector bool short match4to7 = vec_or (match45, match67);
      vector bool short match0to7 = vec_or (match0to3, match4to7);
      result_mask = vec_and ((vector bool char) match0to7, right_valid_byte_mask);
    }
    result_valid_byte_mask = right_valid_byte_mask;
  }

  else if (operation == 1) {
    /* _SIDD_CMP_RANGES */
    /* check within range for each valid character in right to a pair of adjacent characters in left */
    /* sign does matter */
    if (element_type == 0x0) {
      /* unsigned char */
      
      /* splat each of the 16 elements to create 8 upper and lower bounds */
      vector unsigned char lower_bound0 = vec_splat ((vector unsigned char) valid_left_unsigned, 0);
      vector unsigned char upper_bound0 = vec_splat ((vector unsigned char) valid_left_unsigned, 1);
      vector unsigned char lower_bound1 = vec_splat ((vector unsigned char) valid_left_unsigned, 2);
      vector unsigned char upper_bound1 = vec_splat ((vector unsigned char) valid_left_unsigned, 3);
      vector unsigned char lower_bound2 = vec_splat ((vector unsigned char) valid_left_unsigned, 4);
      vector unsigned char upper_bound2 = vec_splat ((vector unsigned char) valid_left_unsigned, 5);
      vector unsigned char lower_bound3 = vec_splat ((vector unsigned char) valid_left_unsigned, 6);
      vector unsigned char upper_bound3 = vec_splat ((vector unsigned char) valid_left_unsigned, 7);
      vector unsigned char lower_bound4 = vec_splat ((vector unsigned char) valid_left_unsigned, 8);
      vector unsigned char upper_bound4 = vec_splat ((vector unsigned char) valid_left_unsigned, 9);
      vector unsigned char lower_bound5 = vec_splat ((vector unsigned char) valid_left_unsigned, 10);
      vector unsigned char upper_bound5 = vec_splat ((vector unsigned char) valid_left_unsigned, 11);
      vector unsigned char lower_bound6 = vec_splat ((vector unsigned char) valid_left_unsigned, 12);
      vector unsigned char upper_bound6 = vec_splat ((vector unsigned char) valid_left_unsigned, 13);
      vector unsigned char lower_bound7 = vec_splat ((vector unsigned char) valid_left_unsigned, 14);
      vector unsigned char upper_bound7 = vec_splat ((vector unsigned char) valid_left_unsigned, 15);
      
      /* compare valid_right_unsigned to each of the 8 upper and lower bounds */
      #ifdef __ibmxl__
        /* xlc */
        vector bool char cmplow0 = vec_cmpge (valid_right_unsigned, lower_bound0);
        vector bool char cmphi0 = vec_cmpge (upper_bound0, valid_right_unsigned);
        vector bool char cmplow1 = vec_cmpge (valid_right_unsigned, lower_bound1);
        vector bool char cmphi1 = vec_cmpge (upper_bound1, valid_right_unsigned);
        vector bool char cmplow2 = vec_cmpge (valid_right_unsigned, lower_bound2);
        vector bool char cmphi2 = vec_cmpge (upper_bound2, valid_right_unsigned);
        vector bool char cmplow3 = vec_cmpge (valid_right_unsigned, lower_bound3);
        vector bool char cmphi3 = vec_cmpge (upper_bound3, valid_right_unsigned);
        vector bool char cmplow4 = vec_cmpge (valid_right_unsigned, lower_bound4);
        vector bool char cmphi4 = vec_cmpge (upper_bound4, valid_right_unsigned);
        vector bool char cmplow5 = vec_cmpge (valid_right_unsigned, lower_bound5);
        vector bool char cmphi5 = vec_cmpge (upper_bound5, valid_right_unsigned);
        vector bool char cmplow6 = vec_cmpge (valid_right_unsigned, lower_bound6);
        vector bool char cmphi6 = vec_cmpge (upper_bound6, valid_right_unsigned);
        vector bool char cmplow7 = vec_cmpge (valid_right_unsigned, lower_bound7);
        vector bool char cmphi7 = vec_cmpge (upper_bound7, valid_right_unsigned);
      #else 
        /* gcc */
        vector bool char cmpgtlow0 = vec_cmpgt (valid_right_unsigned, lower_bound0);
        vector bool char cmpgthi0 = vec_cmpgt (upper_bound0, valid_right_unsigned);
        vector bool char cmpeqlow0 = vec_cmpeq (valid_right_unsigned, lower_bound0);
        vector bool char cmpeqhi0 = vec_cmpeq (upper_bound0, valid_right_unsigned);
        vector bool char cmplow0 = vec_or (cmpgtlow0, cmpeqlow0);
        vector bool char cmphi0 = vec_or (cmpgthi0, cmpeqhi0);
        
        vector bool char cmpgtlow1 = vec_cmpgt (valid_right_unsigned, lower_bound1);
        vector bool char cmpgthi1 = vec_cmpgt (upper_bound1, valid_right_unsigned);    
        vector bool char cmpeqlow1 = vec_cmpeq (valid_right_unsigned, lower_bound1);
        vector bool char cmpeqhi1 = vec_cmpeq (upper_bound1, valid_right_unsigned);
        vector bool char cmplow1 = vec_or (cmpgtlow1, cmpeqlow1);
        vector bool char cmphi1 = vec_or (cmpgthi1, cmpeqhi1);
        
        vector bool char cmpgtlow2 = vec_cmpgt (valid_right_unsigned, lower_bound2);
        vector bool char cmpgthi2 = vec_cmpgt (upper_bound2, valid_right_unsigned);
        vector bool char cmpeqlow2 = vec_cmpeq (valid_right_unsigned, lower_bound2);
        vector bool char cmpeqhi2 = vec_cmpeq (upper_bound2, valid_right_unsigned);
        vector bool char cmplow2 = vec_or (cmpgtlow2, cmpeqlow2);
        vector bool char cmphi2 = vec_or (cmpgthi2, cmpeqhi2);
        
        vector bool char cmpgtlow3 = vec_cmpgt (valid_right_unsigned, lower_bound3);
        vector bool char cmpgthi3 = vec_cmpgt (upper_bound3, valid_right_unsigned);    
        vector bool char cmpeqlow3 = vec_cmpeq (valid_right_unsigned, lower_bound3);
        vector bool char cmpeqhi3 = vec_cmpeq (upper_bound3, valid_right_unsigned);
        vector bool char cmplow3 = vec_or (cmpgtlow3, cmpeqlow3);
        vector bool char cmphi3 = vec_or (cmpgthi3, cmpeqhi3);
        
        vector bool char cmpgtlow4 = vec_cmpgt (valid_right_unsigned, lower_bound4);
        vector bool char cmpgthi4 = vec_cmpgt (upper_bound4, valid_right_unsigned);
        vector bool char cmpeqlow4 = vec_cmpeq (valid_right_unsigned, lower_bound4);
        vector bool char cmpeqhi4 = vec_cmpeq (upper_bound4, valid_right_unsigned);
        vector bool char cmplow4 = vec_or (cmpgtlow4, cmpeqlow4);
        vector bool char cmphi4 = vec_or (cmpgthi4, cmpeqhi4);
        
        vector bool char cmpgtlow5 = vec_cmpgt (valid_right_unsigned, lower_bound5);
        vector bool char cmpgthi5 = vec_cmpgt (upper_bound5, valid_right_unsigned);    
        vector bool char cmpeqlow5 = vec_cmpeq (valid_right_unsigned, lower_bound5);
        vector bool char cmpeqhi5 = vec_cmpeq (upper_bound5, valid_right_unsigned);
        vector bool char cmplow5 = vec_or (cmpgtlow5, cmpeqlow5);
        vector bool char cmphi5 = vec_or (cmpgthi5, cmpeqhi5);
        
        vector bool char cmpgtlow6 = vec_cmpgt (valid_right_unsigned, lower_bound6);
        vector bool char cmpgthi6 = vec_cmpgt (upper_bound6, valid_right_unsigned);
        vector bool char cmpeqlow6 = vec_cmpeq (valid_right_unsigned, lower_bound6);
        vector bool char cmpeqhi6 = vec_cmpeq (upper_bound6, valid_right_unsigned);
        vector bool char cmplow6 = vec_or (cmpgtlow6, cmpeqlow6);
        vector bool char cmphi6 = vec_or (cmpgthi6, cmpeqhi6);
        
        vector bool char cmpgtlow7 = vec_cmpgt (valid_right_unsigned, lower_bound7);
        vector bool char cmpgthi7 = vec_cmpgt (upper_bound7, valid_right_unsigned);    
        vector bool char cmpeqlow7 = vec_cmpeq (valid_right_unsigned, lower_bound7);
        vector bool char cmpeqhi7 = vec_cmpeq (upper_bound7, valid_right_unsigned);
        vector bool char cmplow7 = vec_or (cmpgtlow7, cmpeqlow7);
        vector bool char cmphi7 = vec_or (cmpgthi7, cmpeqhi7);
      #endif
      
      /* AND the result of all the comparisons for within range compares */
      vector bool char range0 = vec_and (cmplow0, cmphi0);
      vector bool char range1 = vec_and (cmplow1, cmphi1);
      vector bool char range2 = vec_and (cmplow2, cmphi2);
      vector bool char range3 = vec_and (cmplow3, cmphi3);
      vector bool char range4 = vec_and (cmplow4, cmphi4);
      vector bool char range5 = vec_and (cmplow5, cmphi5);
      vector bool char range6 = vec_and (cmplow6, cmphi6);
      vector bool char range7 = vec_and (cmplow7, cmphi7);
      
      /* OR the result of range compares */
      vector bool char range01 = vec_or (range0, range1);
      vector bool char range23 = vec_or (range2, range3);
      vector bool char range45 = vec_or (range4, range5);
      vector bool char range67 = vec_or (range6, range7);
      vector bool char range0to3 = vec_or (range01, range23);
      vector bool char range4to7 = vec_or (range45, range67);
      vector bool char range0to7 = vec_or (range0to3, range4to7);
      
      result_mask = vec_and (range0to7, right_valid_byte_mask);
    }
    else if (element_type == 0x1) {
      /* unsigned short */
      
      /* splat each of the 16 elements to create 8 upper and lower bounds */
      vector unsigned short lower_bound0 = vec_splat ((vector unsigned short) valid_left_unsigned, 0);
      vector unsigned short upper_bound0 = vec_splat ((vector unsigned short) valid_left_unsigned, 1);
      vector unsigned short lower_bound1 = vec_splat ((vector unsigned short) valid_left_unsigned, 2);
      vector unsigned short upper_bound1 = vec_splat ((vector unsigned short) valid_left_unsigned, 3);
      vector unsigned short lower_bound2 = vec_splat ((vector unsigned short) valid_left_unsigned, 4);
      vector unsigned short upper_bound2 = vec_splat ((vector unsigned short) valid_left_unsigned, 5);
      vector unsigned short lower_bound3 = vec_splat ((vector unsigned short) valid_left_unsigned, 6);
      vector unsigned short upper_bound3 = vec_splat ((vector unsigned short) valid_left_unsigned, 7);

      /* compare valid_right_unsigned to each of the 8 upper and lower bounds */
      #ifdef __ibmxl__
        /* xlc */
        vector bool short cmplow0 = vec_cmpge ((vector unsigned short) (vector unsigned short) valid_right_unsigned, lower_bound0);
        vector bool short cmphi0 = vec_cmpge (upper_bound0, (vector unsigned short) (vector unsigned short) valid_right_unsigned);
        vector bool short cmplow1 = vec_cmpge ((vector unsigned short) valid_right_unsigned, lower_bound1);
        vector bool short cmphi1 = vec_cmpge (upper_bound1, (vector unsigned short) valid_right_unsigned);
        vector bool short cmplow2 = vec_cmpge ((vector unsigned short) valid_right_unsigned, lower_bound2);
        vector bool short cmphi2 = vec_cmpge (upper_bound2, (vector unsigned short) valid_right_unsigned);
        vector bool short cmplow3 = vec_cmpge ((vector unsigned short) valid_right_unsigned, lower_bound3);
        vector bool short cmphi3 = vec_cmpge (upper_bound3, (vector unsigned short) valid_right_unsigned);
      #else 
        /* gcc */
        vector bool short cmpgtlow0 = vec_cmpgt ((vector unsigned short) valid_right_unsigned, lower_bound0);
        vector bool short cmpgthi0 = vec_cmpgt (upper_bound0, (vector unsigned short) valid_right_unsigned);
        vector bool short cmpeqlow0 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, lower_bound0);
        vector bool short cmpeqhi0 = vec_cmpeq (upper_bound0, (vector unsigned short) valid_right_unsigned);
        vector bool short cmplow0 = vec_or (cmpgtlow0, cmpeqlow0);
        vector bool short cmphi0 = vec_or (cmpgthi0, cmpeqhi0);
        
        vector bool short cmpgtlow1 = vec_cmpgt ((vector unsigned short) valid_right_unsigned, lower_bound1);
        vector bool short cmpgthi1 = vec_cmpgt (upper_bound1, (vector unsigned short) valid_right_unsigned);    
        vector bool short cmpeqlow1 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, lower_bound1);
        vector bool short cmpeqhi1 = vec_cmpeq (upper_bound1, (vector unsigned short) valid_right_unsigned);
        vector bool short cmplow1 = vec_or (cmpgtlow1, cmpeqlow1);
        vector bool short cmphi1 = vec_or (cmpgthi1, cmpeqhi1);
        
        vector bool short cmpgtlow2 = vec_cmpgt ((vector unsigned short) valid_right_unsigned, lower_bound2);
        vector bool short cmpgthi2 = vec_cmpgt (upper_bound2, (vector unsigned short) valid_right_unsigned);
        vector bool short cmpeqlow2 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, lower_bound2);
        vector bool short cmpeqhi2 = vec_cmpeq (upper_bound2, (vector unsigned short) valid_right_unsigned);
        vector bool short cmplow2 = vec_or (cmpgtlow2, cmpeqlow2);
        vector bool short cmphi2 = vec_or (cmpgthi2, cmpeqhi2);
        
        vector bool short cmpgtlow3 = vec_cmpgt ((vector unsigned short) valid_right_unsigned, lower_bound3);
        vector bool short cmpgthi3 = vec_cmpgt (upper_bound3, (vector unsigned short) valid_right_unsigned);    
        vector bool short cmpeqlow3 = vec_cmpeq ((vector unsigned short) valid_right_unsigned, lower_bound3);
        vector bool short cmpeqhi3 = vec_cmpeq (upper_bound3, (vector unsigned short) valid_right_unsigned);
        vector bool short cmplow3 = vec_or (cmpgtlow3, cmpeqlow3);
        vector bool short cmphi3 = vec_or (cmpgthi3, cmpeqhi3);
      #endif
      
      /* AND the result of all the comparisons for within range compares */
      vector bool short range0 = vec_and (cmplow0, cmphi0);
      vector bool short range1 = vec_and (cmplow1, cmphi1);
      vector bool short range2 = vec_and (cmplow2, cmphi2);
      vector bool short range3 = vec_and (cmplow3, cmphi3);
      
      /* OR the result of range compares */
      vector bool short range01 = vec_or (range0, range1);
      vector bool short range23 = vec_or (range2, range3);
      vector bool short range0to3 = vec_or (range01, range23);
      
      result_mask = vec_and((vector bool char) range0to3, right_valid_byte_mask);
    }
    else if (element_type == 0x2) {
      /* signed char */
      
      /* splat each of the 16 elements to create 8 upper and lower bounds */
      vector signed char lower_bound0 = vec_splat ((vector signed char) valid_left_signed, 0);
      vector signed char upper_bound0 = vec_splat ((vector signed char) valid_left_signed, 1);
      vector signed char lower_bound1 = vec_splat ((vector signed char) valid_left_signed, 2);
      vector signed char upper_bound1 = vec_splat ((vector signed char) valid_left_signed, 3);
      vector signed char lower_bound2 = vec_splat ((vector signed char) valid_left_signed, 4);
      vector signed char upper_bound2 = vec_splat ((vector signed char) valid_left_signed, 5);
      vector signed char lower_bound3 = vec_splat ((vector signed char) valid_left_signed, 6);
      vector signed char upper_bound3 = vec_splat ((vector signed char) valid_left_signed, 7);
      vector signed char lower_bound4 = vec_splat ((vector signed char) valid_left_signed, 8);
      vector signed char upper_bound4 = vec_splat ((vector signed char) valid_left_signed, 9);
      vector signed char lower_bound5 = vec_splat ((vector signed char) valid_left_signed, 10);
      vector signed char upper_bound5 = vec_splat ((vector signed char) valid_left_signed, 11);
      vector signed char lower_bound6 = vec_splat ((vector signed char) valid_left_signed, 12);
      vector signed char upper_bound6 = vec_splat ((vector signed char) valid_left_signed, 13);
      vector signed char lower_bound7 = vec_splat ((vector signed char) valid_left_signed, 14);
      vector signed char upper_bound7 = vec_splat ((vector signed char) valid_left_signed, 15);
      
      /* compare valid_right_signed to each of the 8 upper and lower bounds */
      #ifdef __ibmxl__
        /* xlc */
        vector bool char cmplow0 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound0);
        vector bool char cmphi0 = vec_cmpge (upper_bound0, (vector signed char) valid_right_signed);
        vector bool char cmplow1 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound1);
        vector bool char cmphi1 = vec_cmpge (upper_bound1, (vector signed char) valid_right_signed);
        vector bool char cmplow2 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound2);
        vector bool char cmphi2 = vec_cmpge (upper_bound2, (vector signed char) valid_right_signed);
        vector bool char cmplow3 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound3);
        vector bool char cmphi3 = vec_cmpge (upper_bound3, (vector signed char) valid_right_signed);
        vector bool char cmplow4 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound4);
        vector bool char cmphi4 = vec_cmpge (upper_bound4, (vector signed char) valid_right_signed);
        vector bool char cmplow5 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound5);
        vector bool char cmphi5 = vec_cmpge (upper_bound5, (vector signed char) valid_right_signed);
        vector bool char cmplow6 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound6);
        vector bool char cmphi6 = vec_cmpge (upper_bound6, (vector signed char) valid_right_signed);
        vector bool char cmplow7 = vec_cmpge ((vector signed char) valid_right_signed, lower_bound7);
        vector bool char cmphi7 = vec_cmpge (upper_bound7, (vector signed char) valid_right_signed);
      #else 
        /* gcc */
        vector bool char cmpgtlow0 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound0);
        vector bool char cmpgthi0 = vec_cmpgt (upper_bound0, (vector signed char) valid_right_signed);
        vector bool char cmpeqlow0 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound0);
        vector bool char cmpeqhi0 = vec_cmpeq (upper_bound0, (vector signed char) valid_right_signed);
        vector bool char cmplow0 = vec_or (cmpgtlow0, cmpeqlow0);
        vector bool char cmphi0 = vec_or (cmpgthi0, cmpeqhi0);
        
        vector bool char cmpgtlow1 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound1);
        vector bool char cmpgthi1 = vec_cmpgt (upper_bound1, (vector signed char) valid_right_signed);    
        vector bool char cmpeqlow1 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound1);
        vector bool char cmpeqhi1 = vec_cmpeq (upper_bound1, (vector signed char) valid_right_signed);
        vector bool char cmplow1 = vec_or (cmpgtlow1, cmpeqlow1);
        vector bool char cmphi1 = vec_or (cmpgthi1, cmpeqhi1);
        
        vector bool char cmpgtlow2 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound2);
        vector bool char cmpgthi2 = vec_cmpgt (upper_bound2, (vector signed char) valid_right_signed);
        vector bool char cmpeqlow2 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound2);
        vector bool char cmpeqhi2 = vec_cmpeq (upper_bound2, (vector signed char) valid_right_signed);
        vector bool char cmplow2 = vec_or (cmpgtlow2, cmpeqlow2);
        vector bool char cmphi2 = vec_or (cmpgthi2, cmpeqhi2);
        
        vector bool char cmpgtlow3 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound3);
        vector bool char cmpgthi3 = vec_cmpgt (upper_bound3, (vector signed char) valid_right_signed);    
        vector bool char cmpeqlow3 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound3);
        vector bool char cmpeqhi3 = vec_cmpeq (upper_bound3, (vector signed char) valid_right_signed);
        vector bool char cmplow3 = vec_or (cmpgtlow3, cmpeqlow3);
        vector bool char cmphi3 = vec_or (cmpgthi3, cmpeqhi3);
        
        vector bool char cmpgtlow4 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound4);
        vector bool char cmpgthi4 = vec_cmpgt (upper_bound4, (vector signed char) valid_right_signed);
        vector bool char cmpeqlow4 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound4);
        vector bool char cmpeqhi4 = vec_cmpeq (upper_bound4, (vector signed char) valid_right_signed);
        vector bool char cmplow4 = vec_or (cmpgtlow4, cmpeqlow4);
        vector bool char cmphi4 = vec_or (cmpgthi4, cmpeqhi4);
        
        vector bool char cmpgtlow5 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound5);
        vector bool char cmpgthi5 = vec_cmpgt (upper_bound5, (vector signed char) valid_right_signed);    
        vector bool char cmpeqlow5 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound5);
        vector bool char cmpeqhi5 = vec_cmpeq (upper_bound5, (vector signed char) valid_right_signed);
        vector bool char cmplow5 = vec_or (cmpgtlow5, cmpeqlow5);
        vector bool char cmphi5 = vec_or (cmpgthi5, cmpeqhi5);
        
        vector bool char cmpgtlow6 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound6);
        vector bool char cmpgthi6 = vec_cmpgt (upper_bound6, (vector signed char) valid_right_signed);
        vector bool char cmpeqlow6 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound6);
        vector bool char cmpeqhi6 = vec_cmpeq (upper_bound6, (vector signed char) valid_right_signed);
        vector bool char cmplow6 = vec_or (cmpgtlow6, cmpeqlow6);
        vector bool char cmphi6 = vec_or (cmpgthi6, cmpeqhi6);
        
        vector bool char cmpgtlow7 = vec_cmpgt ((vector signed char) valid_right_signed, lower_bound7);
        vector bool char cmpgthi7 = vec_cmpgt (upper_bound7, (vector signed char) valid_right_signed);    
        vector bool char cmpeqlow7 = vec_cmpeq ((vector signed char) valid_right_signed, lower_bound7);
        vector bool char cmpeqhi7 = vec_cmpeq (upper_bound7, (vector signed char) valid_right_signed);
        vector bool char cmplow7 = vec_or (cmpgtlow7, cmpeqlow7);
        vector bool char cmphi7 = vec_or (cmpgthi7, cmpeqhi7);
      #endif
      
      /* AND the result of all the comparisons for within range compares */
      vector bool char range0 = vec_and (cmplow0, cmphi0);
      vector bool char range1 = vec_and (cmplow1, cmphi1);
      vector bool char range2 = vec_and (cmplow2, cmphi2);
      vector bool char range3 = vec_and (cmplow3, cmphi3);
      vector bool char range4 = vec_and (cmplow4, cmphi4);
      vector bool char range5 = vec_and (cmplow5, cmphi5);
      vector bool char range6 = vec_and (cmplow6, cmphi6);
      vector bool char range7 = vec_and (cmplow7, cmphi7);
      
      /* OR the result of range compares */
      vector bool char range01 = vec_or (range0, range1);
      vector bool char range23 = vec_or (range2, range3);
      vector bool char range45 = vec_or (range4, range5);
      vector bool char range67 = vec_or (range6, range7);
      vector bool char range0to3 = vec_or (range01, range23);
      vector bool char range4to7 = vec_or (range45, range67);
      vector bool char range0to7 = vec_or (range0to3, range4to7);
      
      result_mask = vec_and (range0to7, right_valid_byte_mask);
    }
    else if (element_type == 0x3) {
      /* signed short */
      
      /* splat each of the 16 elements to create 8 upper and lower bounds */
      vector signed short lower_bound0 = vec_splat ((vector signed short) valid_left_signed, 0);
      vector signed short upper_bound0 = vec_splat ((vector signed short) valid_left_signed, 1);
      vector signed short lower_bound1 = vec_splat ((vector signed short) valid_left_signed, 2);
      vector signed short upper_bound1 = vec_splat ((vector signed short) valid_left_signed, 3);
      vector signed short lower_bound2 = vec_splat ((vector signed short) valid_left_signed, 4);
      vector signed short upper_bound2 = vec_splat ((vector signed short) valid_left_signed, 5);
      vector signed short lower_bound3 = vec_splat ((vector signed short) valid_left_signed, 6);
      vector signed short upper_bound3 = vec_splat ((vector signed short) valid_left_signed, 7);

      /* compare valid_right_signed to each of the 8 upper and lower bounds */
      #ifdef __ibmxl__
        /* xlc */
        vector bool short cmplow0 = vec_cmpge ((vector signed short) valid_right_signed, lower_bound0);
        vector bool short cmphi0 = vec_cmpge (upper_bound0, (vector signed short) valid_right_signed);
        vector bool short cmplow1 = vec_cmpge ((vector signed short) valid_right_signed, lower_bound1);
        vector bool short cmphi1 = vec_cmpge (upper_bound1, (vector signed short) valid_right_signed);
        vector bool short cmplow2 = vec_cmpge ((vector signed short) valid_right_signed, lower_bound2);
        vector bool short cmphi2 = vec_cmpge (upper_bound2, (vector signed short) valid_right_signed);
        vector bool short cmplow3 = vec_cmpge ((vector signed short) valid_right_signed, lower_bound3);
        vector bool short cmphi3 = vec_cmpge (upper_bound3, (vector signed short) valid_right_signed);
      #else 
        /* gcc */
        vector bool short cmpgtlow0 = vec_cmpgt ((vector signed short) valid_right_signed, lower_bound0);
        vector bool short cmpgthi0 = vec_cmpgt (upper_bound0, (vector signed short) valid_right_signed);
        vector bool short cmpeqlow0 = vec_cmpeq ((vector signed short) valid_right_signed, lower_bound0);
        vector bool short cmpeqhi0 = vec_cmpeq (upper_bound0, (vector signed short) valid_right_signed);
        vector bool short cmplow0 = vec_or (cmpgtlow0, cmpeqlow0);
        vector bool short cmphi0 = vec_or (cmpgthi0, cmpeqhi0);
        
        vector bool short cmpgtlow1 = vec_cmpgt ((vector signed short) valid_right_signed, lower_bound1);
        vector bool short cmpgthi1 = vec_cmpgt (upper_bound1, (vector signed short) valid_right_signed);    
        vector bool short cmpeqlow1 = vec_cmpeq ((vector signed short) valid_right_signed, lower_bound1);
        vector bool short cmpeqhi1 = vec_cmpeq (upper_bound1, (vector signed short) valid_right_signed);
        vector bool short cmplow1 = vec_or (cmpgtlow1, cmpeqlow1);
        vector bool short cmphi1 = vec_or (cmpgthi1, cmpeqhi1);
        
        vector bool short cmpgtlow2 = vec_cmpgt ((vector signed short) valid_right_signed, lower_bound2);
        vector bool short cmpgthi2 = vec_cmpgt (upper_bound2, (vector signed short) valid_right_signed);
        vector bool short cmpeqlow2 = vec_cmpeq ((vector signed short) valid_right_signed, lower_bound2);
        vector bool short cmpeqhi2 = vec_cmpeq (upper_bound2, (vector signed short) valid_right_signed);
        vector bool short cmplow2 = vec_or (cmpgtlow2, cmpeqlow2);
        vector bool short cmphi2 = vec_or (cmpgthi2, cmpeqhi2);
        
        vector bool short cmpgtlow3 = vec_cmpgt ((vector signed short) valid_right_signed, lower_bound3);
        vector bool short cmpgthi3 = vec_cmpgt (upper_bound3, (vector signed short) valid_right_signed);    
        vector bool short cmpeqlow3 = vec_cmpeq ((vector signed short) valid_right_signed, lower_bound3);
        vector bool short cmpeqhi3 = vec_cmpeq (upper_bound3, (vector signed short) valid_right_signed);
        vector bool short cmplow3 = vec_or (cmpgtlow3, cmpeqlow3);
        vector bool short cmphi3 = vec_or (cmpgthi3, cmpeqhi3);
      #endif
      
      /* AND the result of all the comparisons for within range compares */
      vector bool short range0 = vec_and (cmplow0, cmphi0);
      vector bool short range1 = vec_and (cmplow1, cmphi1);
      vector bool short range2 = vec_and (cmplow2, cmphi2);
      vector bool short range3 = vec_and (cmplow3, cmphi3);
      
      /* OR the result of range compares */
      vector bool short range01 = vec_or (range0, range1);
      vector bool short range23 = vec_or (range2, range3);
      vector bool short range0to3 = vec_or (range01, range23);
      
      result_mask = vec_and((vector bool char) range0to3, right_valid_byte_mask);
    }
    result_valid_byte_mask = right_valid_byte_mask;
  }

  else if (operation == 2) {
    /* _SIDD_CMP_EQUAL_EACH */
    /* compare each valid character in right equal to corresponding valid character in left */

    if ((element_type & 0x1) == 0) {
    /* signed char or unsigned char */
      result_mask = vec_cmpeq ((vector unsigned char) valid_left_unsigned, (vector unsigned char) valid_right_unsigned);
    }

    else /* (element_type & 0x1) == 1 */ {
      /* signed short or unsigned short */
      result_mask = (vector bool char) vec_cmpeq ((vector unsigned short) valid_left_unsigned, (vector unsigned short) valid_right_unsigned);
    }
    result_valid_byte_mask = right_valid_byte_mask;
  }
  
  else /* operation == 3 */ {
    /* _SIDD_CMP_EQUAL_ORDERED */
    /* compare each valid character in right to start of valid substring in left */
    /* sign does matter */
    #ifdef __LITTLE_ENDIAN__
      if (element_type == 0) {
        /* unsigned char */
        
        /* shift valid_right_unsigned to right to all positions */
        vector unsigned char valid_right_shift_right0 = valid_right_unsigned;
        vector unsigned char valid_right_shift_right1 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x8));
        vector unsigned char valid_right_shift_right2 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x10));
        vector unsigned char valid_right_shift_right3 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x18));
        vector unsigned char valid_right_shift_right4 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x20));
        vector unsigned char valid_right_shift_right5 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x28));
        vector unsigned char valid_right_shift_right6 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x30));
        vector unsigned char valid_right_shift_right7 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x38));
        vector unsigned char valid_right_shift_right8 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x40));
        vector unsigned char valid_right_shift_right9 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x48));
        vector unsigned char valid_right_shift_right10 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x50));
        vector unsigned char valid_right_shift_right11 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x58));
        vector unsigned char valid_right_shift_right12 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x60));
        vector unsigned char valid_right_shift_right13 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x68));
        vector unsigned char valid_right_shift_right14 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x70));
        vector unsigned char valid_right_shift_right15 = vec_sro (valid_right_unsigned, vec_splats((unsigned char) 0x78));
        
        /* compare valid_right_shift_right0..15 to valid_left_unsigned */
        vector bool char compare_valid_left_shift_right0 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right0);
        vector bool char compare_valid_left_shift_right1 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right1);
        vector bool char compare_valid_left_shift_right2 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right2);
        vector bool char compare_valid_left_shift_right3 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right3);
        vector bool char compare_valid_left_shift_right4 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right4);
        vector bool char compare_valid_left_shift_right5 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right5);
        vector bool char compare_valid_left_shift_right6 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right6);
        vector bool char compare_valid_left_shift_right7 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right7);
        vector bool char compare_valid_left_shift_right8 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right8);
        vector bool char compare_valid_left_shift_right9 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right9);
        vector bool char compare_valid_left_shift_right10 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right10);
        vector bool char compare_valid_left_shift_right11 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right11);
        vector bool char compare_valid_left_shift_right12 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right12);
        vector bool char compare_valid_left_shift_right13 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right13);
        vector bool char compare_valid_left_shift_right14 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right14);
        vector bool char compare_valid_left_shift_right15 = vec_cmpeq (valid_left_unsigned, valid_right_shift_right15);
        
        /* and compare_valid_left_shift_right0..15 with left_valid_byte_mask to clear invalid bytes */
        compare_valid_left_shift_right0 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right0);
        compare_valid_left_shift_right1 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right1);
        compare_valid_left_shift_right2 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right2);
        compare_valid_left_shift_right3 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right3);
        compare_valid_left_shift_right4 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right4);
        compare_valid_left_shift_right5 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right5);
        compare_valid_left_shift_right6 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right6);
        compare_valid_left_shift_right7 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right7);
        compare_valid_left_shift_right8 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right8);
        compare_valid_left_shift_right9 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right9);
        compare_valid_left_shift_right10 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right10);
        compare_valid_left_shift_right11 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right11);
        compare_valid_left_shift_right12 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right12);
        compare_valid_left_shift_right13 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right13);
        compare_valid_left_shift_right14 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right14);
        compare_valid_left_shift_right15 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right15);
        
        /* gather bits by bytes compare_valid_left_shift_right0..15 */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb0 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right7);
          vector unsigned char gbb8 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right8);
          vector unsigned char gbb9 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right9);
          vector unsigned char gbb10 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right10);
          vector unsigned char gbb11 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right11);
          vector unsigned char gbb12 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right12);
          vector unsigned char gbb13 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right13);
          vector unsigned char gbb14 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right14);
          vector unsigned char gbb15 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right15);
        #else
          /* gcc */
          vector unsigned char gbb0 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right7);
          vector unsigned char gbb8 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right8);
          vector unsigned char gbb9 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right9);
          vector unsigned char gbb10 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right10);
          vector unsigned char gbb11 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right11);
          vector unsigned char gbb12 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right12);
          vector unsigned char gbb13 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right13);
          vector unsigned char gbb14 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right14);
          vector unsigned char gbb15 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right15);
        #endif
        
        /* permute gbb0-15 to put the 0th and 8th bytes together and pack them in binary tree order to enable parallelism */
        vector unsigned char gbb_permute_mask01 = (vector unsigned char) { 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask23 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask45 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask67 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18 };
        vector unsigned char gbb_permute_mask89 = (vector unsigned char) { 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask1011 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask1213 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask1415 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18 };
        vector unsigned char gbb_permute_mask0to3 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x14, 0x15, 0x16,0x17, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask4to7 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x08,0x09, 0x0A,0x0B, 0x1C, 0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask8to11 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x14, 0x15, 0x16,0x17, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask12to15 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x08,0x09, 0x0A,0x0B, 0x1C, 0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask0to7 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x04, 0x05, 0x06,0x07, 0x18,0x19, 0x1A,0x1B, 0x1C,0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask8to15 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x04, 0x05, 0x06,0x07, 0x18,0x19, 0x1A,0x1B, 0x1C,0x1D, 0x1E,0x1F };
        vector unsigned char pack_gbb01 = vec_perm (gbb0, gbb1, gbb_permute_mask01);
        vector unsigned char pack_gbb23 = vec_perm (gbb2, gbb3, gbb_permute_mask23);
        vector unsigned char pack_gbb45 = vec_perm (gbb4, gbb5, gbb_permute_mask45);
        vector unsigned char pack_gbb67 = vec_perm (gbb6, gbb7, gbb_permute_mask67);
        vector unsigned char pack_gbb89 = vec_perm (gbb8, gbb9, gbb_permute_mask89);
        vector unsigned char pack_gbb1011 = vec_perm (gbb10, gbb11, gbb_permute_mask1011);
        vector unsigned char pack_gbb1213 = vec_perm (gbb12, gbb13, gbb_permute_mask1213);
        vector unsigned char pack_gbb1415 = vec_perm (gbb14, gbb15, gbb_permute_mask1415);
        vector unsigned char pack_gbb0to3 = vec_perm (pack_gbb01, pack_gbb23, gbb_permute_mask0to3);
        vector unsigned char pack_gbb4to7 = vec_perm (pack_gbb45, pack_gbb67, gbb_permute_mask4to7);
        vector unsigned char pack_gbb8to11 = vec_perm (pack_gbb89, pack_gbb1011, gbb_permute_mask8to11);
        vector unsigned char pack_gbb12to15 = vec_perm (pack_gbb1213, pack_gbb1415, gbb_permute_mask12to15);
        vector unsigned char pack_gbb0to7 = vec_perm (pack_gbb0to3, pack_gbb4to7, gbb_permute_mask0to7);
        vector unsigned char pack_gbb8to15 = vec_perm (pack_gbb8to11, pack_gbb12to15, gbb_permute_mask8to15);
        
        /* calculate mask for substring match */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb_substring = (vector unsigned char) vec_gbb ((vector unsigned long long) left_valid_byte_mask);
        #else 
          /* gcc */
          vector unsigned char gbb_substring = vec_vgbbd ((vector unsigned char) left_valid_byte_mask);
        #endif
        /* puts the byte 0 of gbb_substring into even bytes and byte 8 into odd bytes */
        vector unsigned char substring_permute_mask = (vector unsigned char) { 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08 };
        vector unsigned char substring_mask = vec_perm (gbb_substring, gbb_substring, substring_permute_mask);
        
        /* compare substring_mask to pack_gbb0to7 and pack_gbb8to15 to find matches */
        vector bool short compare_gbb0to7 = vec_cmpeq ((vector unsigned short) pack_gbb0to7, (vector unsigned short) substring_mask);
        vector bool short compare_gbb8to15 = vec_cmpeq ((vector unsigned short) pack_gbb8to15, (vector unsigned short) substring_mask);
        
        /* turn compare_gbb0to7 and compare_gbb8to15 into a mask of 8 bits per element */
        vector bool char compare_gbb0to15 = vec_pack ((vector bool short) compare_gbb0to7, (vector bool short) compare_gbb8to15);
        
        result_mask = compare_gbb0to15;
      }
      else if (element_type == 1) {
        /* unsigned short */
        
        /* shift valid_right_unsigned to right to all positions */
        vector unsigned short valid_right_shift_right0 = (vector unsigned short) valid_right_unsigned;
        vector unsigned short valid_right_shift_right1 = vec_sro ((vector unsigned short) valid_right_unsigned, vec_splats((unsigned char) 0x10));
        vector unsigned short valid_right_shift_right2 = vec_sro ((vector unsigned short) valid_right_unsigned, vec_splats((unsigned char) 0x20));
        vector unsigned short valid_right_shift_right3 = vec_sro ((vector unsigned short) valid_right_unsigned, vec_splats((unsigned char) 0x30));
        vector unsigned short valid_right_shift_right4 = vec_sro ((vector unsigned short) valid_right_unsigned, vec_splats((unsigned char) 0x40));
        vector unsigned short valid_right_shift_right5 = vec_sro ((vector unsigned short) valid_right_unsigned, vec_splats((unsigned char) 0x50));
        vector unsigned short valid_right_shift_right6 = vec_sro ((vector unsigned short) valid_right_unsigned, vec_splats((unsigned char) 0x60));
        vector unsigned short valid_right_shift_right7 = vec_sro ((vector unsigned short) valid_right_unsigned, vec_splats((unsigned char) 0x70));
        
        /* compare valid_right_shift_right0..7 to valid_left_unsigned */
        vector bool short compare_valid_left_shift_right0 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right0);
        vector bool short compare_valid_left_shift_right1 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right1);
        vector bool short compare_valid_left_shift_right2 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right2);
        vector bool short compare_valid_left_shift_right3 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right3);
        vector bool short compare_valid_left_shift_right4 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right4);
        vector bool short compare_valid_left_shift_right5 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right5);
        vector bool short compare_valid_left_shift_right6 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right6);
        vector bool short compare_valid_left_shift_right7 = vec_cmpeq ((vector unsigned short) valid_left_unsigned, valid_right_shift_right7);
        
        /* and compare_valid_left_shift_right0..7 with left_valid_byte_mask to clear invalid bytes */
        compare_valid_left_shift_right0 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right0);
        compare_valid_left_shift_right1 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right1);
        compare_valid_left_shift_right2 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right2);
        compare_valid_left_shift_right3 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right3);
        compare_valid_left_shift_right4 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right4);
        compare_valid_left_shift_right5 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right5);
        compare_valid_left_shift_right6 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right6);
        compare_valid_left_shift_right7 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right7);
        
        /* gather bits by bytes compare_valid_left_shift_right0..7 */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb0 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right7);
        #else
          /* gcc */
          vector unsigned char gbb0 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right7);
        #endif
        
        /* permute gbb0-7 to put the 0th and 4th shorts together and pack them in binary tree order to enable parallelism */
        vector unsigned char gbb_permute_mask01 = (vector unsigned char) { 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask23 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask45 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask67 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18 };
        vector unsigned char gbb_permute_mask0to3 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x14, 0x15, 0x16,0x17, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask4to7 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x08,0x09, 0x0A,0x0B, 0x1C, 0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask0to7 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x04, 0x05, 0x06,0x07, 0x18,0x19, 0x1A,0x1B, 0x1C,0x1D, 0x1E,0x1F };
        vector unsigned char pack_gbb01 = vec_perm (gbb0, gbb1, gbb_permute_mask01);
        vector unsigned char pack_gbb23 = vec_perm (gbb2, gbb3, gbb_permute_mask23);
        vector unsigned char pack_gbb45 = vec_perm (gbb4, gbb5, gbb_permute_mask45);
        vector unsigned char pack_gbb67 = vec_perm (gbb6, gbb7, gbb_permute_mask67);
        vector unsigned char pack_gbb0to3 = vec_perm (pack_gbb01, pack_gbb23, gbb_permute_mask0to3);
        vector unsigned char pack_gbb4to7 = vec_perm (pack_gbb45, pack_gbb67, gbb_permute_mask4to7);
        vector unsigned char pack_gbb0to7 = vec_perm (pack_gbb0to3, pack_gbb4to7, gbb_permute_mask0to7);
        
        /* calculate mask for substring match */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb_substring = (vector unsigned char) vec_gbb ((vector unsigned long long) left_valid_byte_mask);
        #else 
          /* gcc */
          vector unsigned char gbb_substring = vec_vgbbd ((vector unsigned char) left_valid_byte_mask);
        #endif
        /* puts the byte 0 of gbb_substring into even bytes and byte 8 into odd bytes */
        vector unsigned char substring_permute_mask = (vector unsigned char) { 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08 };
        vector unsigned char substring_mask = vec_perm (gbb_substring, gbb_substring, substring_permute_mask);
        
        /* compare substring_mask to pack_gbb0to7 to find matches */
        vector bool short compare_gbb0to7 = vec_cmpeq ((vector unsigned short) pack_gbb0to7, (vector unsigned short) substring_mask);

        result_mask = (vector bool char) compare_gbb0to7;
      }
      if (element_type == 2) {
        /* signed char */
        
        /* shift valid_right_unsigned to right to all positions */
        vector signed char valid_right_shift_right0 = (vector signed char) valid_right_signed;
        vector signed char valid_right_shift_right1 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x8));
        vector signed char valid_right_shift_right2 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x10));
        vector signed char valid_right_shift_right3 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x18));
        vector signed char valid_right_shift_right4 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x20));
        vector signed char valid_right_shift_right5 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x28));
        vector signed char valid_right_shift_right6 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x30));
        vector signed char valid_right_shift_right7 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x38));
        vector signed char valid_right_shift_right8 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x40));
        vector signed char valid_right_shift_right9 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x48));
        vector signed char valid_right_shift_right10 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x50));
        vector signed char valid_right_shift_right11 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x58));
        vector signed char valid_right_shift_right12 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x60));
        vector signed char valid_right_shift_right13 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x68));
        vector signed char valid_right_shift_right14 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x70));
        vector signed char valid_right_shift_right15 = vec_sro ((vector signed char) valid_right_signed, vec_splats((unsigned char) 0x78));
        
        /* compare valid_right_shift_right0..15 to valid_left_unsigned */
        vector bool char compare_valid_left_shift_right0 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right0);
        vector bool char compare_valid_left_shift_right1 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right1);
        vector bool char compare_valid_left_shift_right2 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right2);
        vector bool char compare_valid_left_shift_right3 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right3);
        vector bool char compare_valid_left_shift_right4 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right4);
        vector bool char compare_valid_left_shift_right5 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right5);
        vector bool char compare_valid_left_shift_right6 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right6);
        vector bool char compare_valid_left_shift_right7 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right7);
        vector bool char compare_valid_left_shift_right8 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right8);
        vector bool char compare_valid_left_shift_right9 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right9);
        vector bool char compare_valid_left_shift_right10 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right10);
        vector bool char compare_valid_left_shift_right11 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right11);
        vector bool char compare_valid_left_shift_right12 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right12);
        vector bool char compare_valid_left_shift_right13 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right13);
        vector bool char compare_valid_left_shift_right14 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right14);
        vector bool char compare_valid_left_shift_right15 = vec_cmpeq ((vector signed char) valid_left_signed, valid_right_shift_right15);
        
        /* and compare_valid_left_shift_right0..15 with left_valid_byte_mask to clear invalid bytes */
        compare_valid_left_shift_right0 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right0);
        compare_valid_left_shift_right1 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right1);
        compare_valid_left_shift_right2 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right2);
        compare_valid_left_shift_right3 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right3);
        compare_valid_left_shift_right4 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right4);
        compare_valid_left_shift_right5 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right5);
        compare_valid_left_shift_right6 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right6);
        compare_valid_left_shift_right7 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right7);
        compare_valid_left_shift_right8 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right8);
        compare_valid_left_shift_right9 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right9);
        compare_valid_left_shift_right10 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right10);
        compare_valid_left_shift_right11 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right11);
        compare_valid_left_shift_right12 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right12);
        compare_valid_left_shift_right13 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right13);
        compare_valid_left_shift_right14 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right14);
        compare_valid_left_shift_right15 = vec_and (left_valid_byte_mask, compare_valid_left_shift_right15);
        
        /* gather bits by bytes compare_valid_left_shift_right0..15 */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb0 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right7);
          vector unsigned char gbb8 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right8);
          vector unsigned char gbb9 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right9);
          vector unsigned char gbb10 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right10);
          vector unsigned char gbb11 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right11);
          vector unsigned char gbb12 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right12);
          vector unsigned char gbb13 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right13);
          vector unsigned char gbb14 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right14);
          vector unsigned char gbb15 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right15);
        #else
          /* gcc */
          vector unsigned char gbb0 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right7);
          vector unsigned char gbb8 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right8);
          vector unsigned char gbb9 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right9);
          vector unsigned char gbb10 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right10);
          vector unsigned char gbb11 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right11);
          vector unsigned char gbb12 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right12);
          vector unsigned char gbb13 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right13);
          vector unsigned char gbb14 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right14);
          vector unsigned char gbb15 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right15);
        #endif
        
        /* permute gbb0-15 to put the 0th and 8th bytes together and pack them in binary tree order to enable parallelism */
        vector unsigned char gbb_permute_mask01 = (vector unsigned char) { 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask23 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask45 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask67 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18 };
        vector unsigned char gbb_permute_mask89 = (vector unsigned char) { 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask1011 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask1213 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask1415 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18 };
        vector unsigned char gbb_permute_mask0to3 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x14, 0x15, 0x16,0x17, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask4to7 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x08,0x09, 0x0A,0x0B, 0x1C, 0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask8to11 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x14, 0x15, 0x16,0x17, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask12to15 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x08,0x09, 0x0A,0x0B, 0x1C, 0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask0to7 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x04, 0x05, 0x06,0x07, 0x18,0x19, 0x1A,0x1B, 0x1C,0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask8to15 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x04, 0x05, 0x06,0x07, 0x18,0x19, 0x1A,0x1B, 0x1C,0x1D, 0x1E,0x1F };
        vector unsigned char pack_gbb01 = vec_perm (gbb0, gbb1, gbb_permute_mask01);
        vector unsigned char pack_gbb23 = vec_perm (gbb2, gbb3, gbb_permute_mask23);
        vector unsigned char pack_gbb45 = vec_perm (gbb4, gbb5, gbb_permute_mask45);
        vector unsigned char pack_gbb67 = vec_perm (gbb6, gbb7, gbb_permute_mask67);
        vector unsigned char pack_gbb89 = vec_perm (gbb8, gbb9, gbb_permute_mask89);
        vector unsigned char pack_gbb1011 = vec_perm (gbb10, gbb11, gbb_permute_mask1011);
        vector unsigned char pack_gbb1213 = vec_perm (gbb12, gbb13, gbb_permute_mask1213);
        vector unsigned char pack_gbb1415 = vec_perm (gbb14, gbb15, gbb_permute_mask1415);
        vector unsigned char pack_gbb0to3 = vec_perm (pack_gbb01, pack_gbb23, gbb_permute_mask0to3);
        vector unsigned char pack_gbb4to7 = vec_perm (pack_gbb45, pack_gbb67, gbb_permute_mask4to7);
        vector unsigned char pack_gbb8to11 = vec_perm (pack_gbb89, pack_gbb1011, gbb_permute_mask8to11);
        vector unsigned char pack_gbb12to15 = vec_perm (pack_gbb1213, pack_gbb1415, gbb_permute_mask12to15);
        vector unsigned char pack_gbb0to7 = vec_perm (pack_gbb0to3, pack_gbb4to7, gbb_permute_mask0to7);
        vector unsigned char pack_gbb8to15 = vec_perm (pack_gbb8to11, pack_gbb12to15, gbb_permute_mask8to15);
        
        /* calculate mask for substring match */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb_substring = (vector unsigned char) vec_gbb ((vector unsigned long long) left_valid_byte_mask);
        #else 
          /* gcc */
          vector unsigned char gbb_substring = vec_vgbbd ((vector unsigned char) left_valid_byte_mask);
        #endif
        /* puts the byte 0 of gbb_substring into even bytes and byte 8 into odd bytes */
        vector unsigned char substring_permute_mask = (vector unsigned char) { 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08 };
        vector unsigned char substring_mask = vec_perm (gbb_substring, gbb_substring, substring_permute_mask);
        
        /* compare substring_mask to pack_gbb0to7 and pack_gbb8to15 to find matches */
        vector bool short compare_gbb0to7 = vec_cmpeq ((vector unsigned short) pack_gbb0to7, (vector unsigned short) substring_mask);
        vector bool short compare_gbb8to15 = vec_cmpeq ((vector unsigned short) pack_gbb8to15, (vector unsigned short) substring_mask);
        
        /* turn compare_gbb0to7 and compare_gbb8to15 into a mask of 8 bits per element */
        vector bool char compare_gbb0to15 = vec_pack ((vector bool short) compare_gbb0to7, (vector bool short) compare_gbb8to15);
        
        result_mask = compare_gbb0to15;
      }
      else if (element_type == 3) {
        /* signed short */
        
        /* shift valid_right_unsigned to right to all positions */
        vector signed short valid_right_shift_right0 = (vector signed short) valid_right_signed;
        vector signed short valid_right_shift_right1 = vec_sro ((vector signed short) valid_right_signed, vec_splats((unsigned char) 0x10));
        vector signed short valid_right_shift_right2 = vec_sro ((vector signed short) valid_right_signed, vec_splats((unsigned char) 0x20));
        vector signed short valid_right_shift_right3 = vec_sro ((vector signed short) valid_right_signed, vec_splats((unsigned char) 0x30));
        vector signed short valid_right_shift_right4 = vec_sro ((vector signed short) valid_right_signed, vec_splats((unsigned char) 0x40));
        vector signed short valid_right_shift_right5 = vec_sro ((vector signed short) valid_right_signed, vec_splats((unsigned char) 0x50));
        vector signed short valid_right_shift_right6 = vec_sro ((vector signed short) valid_right_signed, vec_splats((unsigned char) 0x60));
        vector signed short valid_right_shift_right7 = vec_sro ((vector signed short) valid_right_signed, vec_splats((unsigned char) 0x70));
        
        /* compare valid_right_shift_right0..7 to valid_left_unsigned */
        vector bool short compare_valid_left_shift_right0 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right0);
        vector bool short compare_valid_left_shift_right1 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right1);
        vector bool short compare_valid_left_shift_right2 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right2);
        vector bool short compare_valid_left_shift_right3 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right3);
        vector bool short compare_valid_left_shift_right4 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right4);
        vector bool short compare_valid_left_shift_right5 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right5);
        vector bool short compare_valid_left_shift_right6 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right6);
        vector bool short compare_valid_left_shift_right7 = vec_cmpeq ((vector signed short) valid_left_signed, valid_right_shift_right7);
        
        /* and compare_valid_left_shift_right0..7 with left_valid_byte_mask to clear invalid bytes */
        compare_valid_left_shift_right0 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right0);
        compare_valid_left_shift_right1 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right1);
        compare_valid_left_shift_right2 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right2);
        compare_valid_left_shift_right3 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right3);
        compare_valid_left_shift_right4 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right4);
        compare_valid_left_shift_right5 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right5);
        compare_valid_left_shift_right6 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right6);
        compare_valid_left_shift_right7 = vec_and ((vector bool short) left_valid_byte_mask, compare_valid_left_shift_right7);
        
        /* gather bits by bytes compare_valid_left_shift_right0..7 */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb0 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = (vector unsigned char) vec_gbb ((vector unsigned long long) compare_valid_left_shift_right7);
        #else
          /* gcc */
          vector unsigned char gbb0 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right0);
          vector unsigned char gbb1 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right1);
          vector unsigned char gbb2 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right2);
          vector unsigned char gbb3 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right3);
          vector unsigned char gbb4 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right4);
          vector unsigned char gbb5 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right5);
          vector unsigned char gbb6 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right6);
          vector unsigned char gbb7 = vec_vgbbd ((vector unsigned char) compare_valid_left_shift_right7);
        #endif
        
        /* permute gbb0-7 to put the 0th and 4th shorts together and pack them in binary tree order to enable parallelism */
        vector unsigned char gbb_permute_mask01 = (vector unsigned char) { 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask23 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask45 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask67 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x00,0x08, 0x10,0x18 };
        vector unsigned char gbb_permute_mask0to3 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x14, 0x15, 0x16,0x17, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F };
        vector unsigned char gbb_permute_mask4to7 = (vector unsigned char) { 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x0F,0x0F, 0x08,0x09, 0x0A,0x0B, 0x1C, 0x1D, 0x1E,0x1F };
        vector unsigned char gbb_permute_mask0to7 = (vector unsigned char) { 0x00,0x01, 0x02,0x03, 0x04, 0x05, 0x06,0x07, 0x18,0x19, 0x1A,0x1B, 0x1C,0x1D, 0x1E,0x1F };
        vector unsigned char pack_gbb01 = vec_perm (gbb0, gbb1, gbb_permute_mask01);
        vector unsigned char pack_gbb23 = vec_perm (gbb2, gbb3, gbb_permute_mask23);
        vector unsigned char pack_gbb45 = vec_perm (gbb4, gbb5, gbb_permute_mask45);
        vector unsigned char pack_gbb67 = vec_perm (gbb6, gbb7, gbb_permute_mask67);
        vector unsigned char pack_gbb0to3 = vec_perm (pack_gbb01, pack_gbb23, gbb_permute_mask0to3);
        vector unsigned char pack_gbb4to7 = vec_perm (pack_gbb45, pack_gbb67, gbb_permute_mask4to7);
        vector unsigned char pack_gbb0to7 = vec_perm (pack_gbb0to3, pack_gbb4to7, gbb_permute_mask0to7);
        
        /* calculate mask for substring match */
        #ifdef __ibmxl__
          /* xlc */
          vector unsigned char gbb_substring = (vector unsigned char) vec_gbb ((vector unsigned long long) left_valid_byte_mask);
        #else 
          /* gcc */
          vector unsigned char gbb_substring = vec_vgbbd ((vector unsigned char) left_valid_byte_mask);
        #endif
        /* puts the byte 0 of gbb_substring into even bytes and byte 8 into odd bytes */
        vector unsigned char substring_permute_mask = (vector unsigned char) { 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08, 0x00,0x08 };
        vector unsigned char substring_mask = vec_perm (gbb_substring, gbb_substring, substring_permute_mask);
        
        /* compare substring_mask to pack_gbb0to7 to find matches */
        vector bool short compare_gbb0to7 = vec_cmpeq ((vector unsigned short) pack_gbb0to7, (vector unsigned short) substring_mask);

        result_mask = (vector bool char) compare_gbb0to7;
      }
    #elif __BIG_ENDIAN__
      #error _SIDD_CMP_EQUAL_ORDERED is not supported on machines lower than POWER8.
    #endif
    
    result_valid_byte_mask = right_valid_byte_mask;
  }

  /* mask polarity */
  unsigned int polarity = (control >> 4) & 0x3;  /* bits 5:4 */
  if (polarity == 1) {
    /* _SIDD_NEGATIVE_POLARITY */
    /* complement result mask */
    result_mask = vec_nor (result_mask, result_mask);
  }

  else if (polarity == 3) {
    /* _SIDD_MASKED_NEGATIVE_POLARITY */
    /* complement result mask only before end of string */
    result_mask = vec_sel (/* if invalid */ result_mask,
                           /* if valid */   vec_nor (result_mask, result_mask),
                           result_valid_byte_mask);
  }

  else /* polarity == 0 or 2 */ {
    /* _SIDD_POSITIVE_POLARITY */
    /* leave result mask unchanged */
  }

  /* Return least or most significant bit index */
  unsigned int least_or_most = (control >> 6) & 0x1;  /* bit 6 */

  /* Prepare mask vector to extract bits */
  vector bool char result_mask_to_transpose = result_mask;
  if ((element_type & 0x1) == 0) {
    /* signed char or unsigned char */
    /* leave result_mask_to_transpose as it is */ 
     
  } else /* (element_type & 0x1) == 1 */ {
    /* signed short or unsigned short */
    /* extract one byte of each short (both bytes are the same; extracting low byte is simpler) */
    result_mask_to_transpose = (vector bool char) vec_pack ((vector unsigned short) result_mask,
                                                              (vector unsigned short) zeros);
  }
  if (least_or_most == 0) {
    /* _SIDD_LEAST_SIGNIFICANT - return the index of the least significant bit */
    #ifdef __LITTLE_ENDIAN__
      /* reverse the element of the result_mask_to_transpose */
      #ifdef __ibmxl__
        /* xlc */
        result_mask_to_transpose = (vector bool char) vec_reve ((vector unsigned char) result_mask_to_transpose);
      #else
        /* gcc */
        vector unsigned char reverse_mask = { 0x0F,0x0E, 0x0D,0x0C, 0x0B,0x0A, 0x09,0x08, 0x07,0x06, 0x05,0x04, 0x03,0x02, 0x01,0x00 };
        result_mask_to_transpose = vec_perm (result_mask_to_transpose, result_mask_to_transpose, reverse_mask);
      #endif
    #elif __BIG_ENDIAN__
      /* leave result_mask_to_transpose as it is */
    #endif    
  } else /* least_or_most == 1 */ {
    /* _SIDD_MOST_SIGNIFICANT - return the index of the most significant bit */
    #ifdef __LITTLE_ENDIAN__
      /* leave result_mask_to_transpose as it is */
    #elif __BIG_ENDIAN__
      /* reverse the element of the result_mask_to_transpose */
      result_mask_to_transpose = (vector bool char) vec_reve ((vector unsigned char) result_mask_to_transpose);
    #endif
  }
  
  /* bit-transpose each half of result_mask, using Power8 vector gather bits by bytes by doubleword */
  vector unsigned char bit_transposed_mask;
  #ifdef _ARCH_PWR8
    #ifdef __ibmxl__
      /* xlc */
      bit_transposed_mask = (vector unsigned char) vec_gbb ((vector unsigned long long) result_mask_to_transpose);
    #else
      /* gcc */
      bit_transposed_mask = vec_vgbbd ((vector unsigned char) result_mask_to_transpose);
    #endif
  #else
    /* emulate vector gather bits by bytes by doublewords for one bit only of each byte */
    /* all bits of each byte are identical, so extract the one that avoids bit shifting */
    vector unsigned char mask_bits_to_extract = (vector unsigned char)
          { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,  0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 };
    vector unsigned char extracted_mask_bits = vec_and (result_mask_to_transpose, mask_bits_to_extract);
    /* gather extracted bits for each doubleword into its byte 0 (leaving garbage in its other bytes) */
    vector unsigned char btmbit0 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 7*8));
    vector unsigned char btmbit1 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 6*8));
    vector unsigned char btmbit2 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 5*8));
    vector unsigned char btmbit3 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 4*8));
    vector unsigned char btmbit4 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 3*8));
    vector unsigned char btmbit5 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 2*8));
    vector unsigned char btmbit6 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 1*8));
    vector unsigned char btmbit7 = vec_slo (extracted_mask_bits, (vector unsigned char) vec_splats ((unsigned char) 0*8));
    vector unsigned char btmbits01 = vec_or (btmbit0, btmbit1);
    vector unsigned char btmbits23 = vec_or (btmbit2, btmbit3);
    vector unsigned char btmbits45 = vec_or (btmbit4, btmbit5);
    vector unsigned char btmbits67 = vec_or (btmbit6, btmbit7);
    vector unsigned char btmbits0to3 = vec_or (btmbits01, btmbits23);
    vector unsigned char btmbits4to7 = vec_or (btmbits45, btmbits67);
    vector unsigned char btmbits0to7 = vec_or (btmbits0to3, btmbits4to7);
    bit_transposed_mask = btmbits0to7;
  #endif
  
  /* extract leftmost bit to form the result_mask */
  vector unsigned char select_transposed_upper_bits;
  if (least_or_most == 0) {
    if ((element_type & 0x1) == 0) {
      /* signed char or unsigned char */
      /* extract leftmost bit of each byte, to two bytes with other 14 bytes zeroed */
      select_transposed_upper_bits = (vector unsigned char)  /* need leftmost byte of each doubleword */
      #ifdef __LITTLE_ENDIAN__
        /* for little endian these two bytes must be right justified in the VR to be stored in the first short element */
        /* the mask for the right (low address) half is stored in byte 0 */
        /* the following will be in the reverse order in the VR */
        { 0x17, 0x1F, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 };
      #elif __BIG_ENDIAN__
        /* for big endian these two bytes must be left justified in the VR to be stored in the first short element */
        /* caution: some applications might access it as an int or long long? */
        /* the mask for the left (low address) half is stored in byte 0 */
        { 0x10, 0x18, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 };
      #endif
    } else /* (element_type & 0x1) == 1 */ {
      /* signed short or unsigned short */
      /* extract leftmost bit of one byte of each short, to one byte with other 15 bytes zeroed */
      select_transposed_upper_bits = (vector unsigned char)  /* need leftmost byte of left doubleword */
      #ifdef __LITTLE_ENDIAN__
          /* for little endian this byte must be right justified to be stored in the first char element */
          { 0x18, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 };
      #elif __BIG_ENDIAN__
          /* for big endian this byte must be left justified to be stored in the first char element */
          { 0x10, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 };
      #endif
    }
  } else /* least_or_most == 1 */ {
    if ((element_type & 0x1) == 0) {
      /* signed char or unsigned char */
      /* extract leftmost bit of each byte, to two bytes with other 14 bytes zeroed */
      select_transposed_upper_bits = (vector unsigned char)  /* need leftmost byte of each doubleword */
      #ifdef __LITTLE_ENDIAN__
        /* for little endian these two bytes must be right justified in the VR to be stored in the first short element */
        /* the mask for the right (low address) half is stored in byte 0 */
        /* the following will be in the reverse order in the VR */
        { 0x10, 0x18, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 };
      #elif __BIG_ENDIAN__
        /* for big endian these two bytes must be left justified in the VR to be stored in the first short element */
        /* caution: some applications might access it as an int or long long? */
        /* the mask for the left (low address) half is stored in byte 0 */
        { 0x17, 0x1F, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 };
      #endif
    } else /* (element_type & 0x1) == 1 */ {
      /* signed short or unsigned short */
      /* extract leftmost bit of one byte of each short, to one byte with other 15 bytes zeroed */
      select_transposed_upper_bits = (vector unsigned char)  /* need leftmost byte of left doubleword */
      #ifdef __LITTLE_ENDIAN__
          /* for little endian this byte must be right justified to be stored in the first char element */
          { 0x10, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 };
      #elif __BIG_ENDIAN__
          /* for big endian this byte must be left justified to be stored in the first char element */
          { 0x18, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 };
      #endif
    }
  }

  /* extract leftmost byte of each doubleword, to 2 or 1 bytes with other 14 or 15 bytes zeroed */
  result_mask = (vector bool char) vec_perm (zeros, bit_transposed_mask, select_transposed_upper_bits);
  
  /* compute the least/most significant bit index */
  unsigned long long result;
  /* extract the lower half of the result_mask */
  #ifdef __LITTLE_ENDIAN__
    result = vec_extract ((vector unsigned long long) result_mask, 0);
  #elif __BIG_ENDIAN__
    result = vec_extract ((vector unsigned long long) result_mask, 1);
  #endif
  
  int leading_zeros;
  #ifdef __ibmxl__
    /* xlc */
    leading_zeros = __cntlz8 (result);
  #else
    /* gcc */
    asm("   cntlzd %0, %1"
    :   "=r"     (leading_zeros)
    :   "r"      (result)
    );
  #endif
  
  if (least_or_most == 0) {
    #ifdef __LITTLE_ENDIAN__
      if ((element_type & 0x1) == 0) {
        /* signed or unsigned char */
        index = leading_zeros - 48;
      } else /* (element_type & 0x1) == 1 */ {
        /* signed or unsigned short */
        index = leading_zeros - 56;
      }
    #elif __BIG_ENDIAN__
      if ((element_type & 0x1) == 0) {
        /* signed or unsigned char */
        index = 15 - (leading_zeros - 48);
      } else /* (element_type & 0x1) == 1 */ {
        /* signed or unsigned short */
        index = 7 - (leading_zeros - 56);
      }
    #endif
  } else /* least_or_most == 1 */ {
    #ifdef __LITTLE_ENDIAN__
      if ((element_type & 0x1) == 0) {
        /* signed or unsigned char */
        index = 15 - (leading_zeros - 48);
      } else /* (element_type & 0x1) == 1 */ {
        /* signed or unsigned short */
        index = 7 - (leading_zeros - 56);
      }
    #elif __BIG_ENDIAN__
      if ((element_type & 0x1) == 0) {
        /* signed or unsigned char */
        index = leading_zeros - 48;
      } else /* (element_type & 0x1) == 1 */ {
        /* signed or unsigned short */
        index = leading_zeros - 56;
      }
    #endif
  }
  
  if (index == 0xFFFFFFFF) {
    if ((element_type & 0x1) == 0) {
      /* signed or unsigned char */
      index = 16;
    } else /* (element_type & 0x1) == 1 */ {
      /* signed or unsigned short */
      index = 8;
    }
  }
  return index;
}

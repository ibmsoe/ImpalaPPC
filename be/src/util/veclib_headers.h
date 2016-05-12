#ifndef _H_VECLIB_HEADERS
#define _H_VECLIB_HEADERS


#ifdef __ALTIVEC__
#define VECLIB_VSX
#include <altivec.h>
#include "util/veclib_types.h"
#include "util/vec128str.h"
#include "util/vec128int.h"
#include "util/vecmisc.h"

#define _mm_crc32_u8 vec_crc321ub
#define _mm_crc32_u32 vec_crc324ub
//#define POPCNT_popcnt_u64 vec_popcount1uw
#define _mm_loadu_si128 vec_load1q
#define _mm_loadl_epi64 vec_loadlower1sd
#define _mm_extract_epi16 vec_extract8sh
#endif
#endif

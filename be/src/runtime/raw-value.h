// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#ifndef IMPALA_RUNTIME_RAW_VALUE_H
#define IMPALA_RUNTIME_RAW_VALUE_H

#include <sstream>
#include <string>

#include "runtime/types.h"

namespace impala {

class MemPool;
class SlotDescriptor;
class Tuple;

/// Useful utility functions for runtime values (which are passed around as void*).
class RawValue {
 public:
  /// Ascii output precision for double/float
  static const int ASCII_PRECISION;

  /// Convert 'value' into ascii and write to 'stream'. NULL turns into "NULL". 'scale'
  /// determines how many digits after the decimal are printed for floating point numbers,
  /// -1 indicates to use the stream's current formatting.
  /// TODO: for string types, we just print the result regardless of whether or not it
  /// ascii. This could be undesirable.
  static void PrintValue(const void* value, const ColumnType& type, int scale,
                         std::stringstream* stream);

  /// Write ascii value to string instead of stringstream.
  static void PrintValue(const void* value, const ColumnType& type, int scale,
                         std::string* str);

  /// Writes the byte representation of a value to a stringstream character-by-character
  static void PrintValueAsBytes(const void* value, const ColumnType& type,
                                std::stringstream* stream);

  /// Returns hash value for 'v' interpreted as 'type'.  The resulting hash value
  /// is combined with the seed value.
  static inline uint32_t GetHashValue(const void* v, const ColumnType& type,
      uint32_t seed = 0);

  /// Templatized version of GetHashValue, use if type is known ahead. GetHashValue
  /// handles nulls.
  template<typename T>
  static inline uint32_t GetHashValue(const T* v, const ColumnType& type,
      uint32_t seed = 0);

  /// Returns hash value for non-nullable 'v' for type T. GetHashValueNonNull doesn't
  /// handle nulls.
  template<typename T>
  static inline uint32_t GetHashValueNonNull(const T* v, const ColumnType& type,
      uint32_t seed = 0);

  /// Get a 32-bit hash value using the FNV hash function.
  /// Using different seeds with FNV results in different hash functions.
  /// GetHashValue() does not have this property and cannot be safely used as the first
  /// step in data repartitioning. However, GetHashValue() can be significantly faster.
  /// TODO: fix GetHashValue
  static inline uint32_t GetHashValueFnv(const void* v, const ColumnType& type,
      uint32_t seed);

  /// Compares both values.
  /// Return value is < 0  if v1 < v2, 0 if v1 == v2, > 0 if v1 > v2.
  static int Compare(const void* v1, const void* v2, const ColumnType& type);

  /// Writes the bytes of a given value into the slot of a tuple.
  /// For string values, the string data is copied into memory allocated from 'pool'
  /// only if pool is non-NULL.
  static void Write(const void* value, Tuple* tuple, const SlotDescriptor* slot_desc,
                    MemPool* pool);

  /// Writes 'src' into 'dst' for type.
  /// For string values, the string data is copied into 'pool' if pool is non-NULL.
  /// src must be non-NULL.
  static void Write(const void* src, void* dst, const ColumnType& type, MemPool* pool);

  /// Writes 'src' into 'dst' for type.
  /// String values are copied into *buffer and *buffer is updated by the length. *buf
  /// must be preallocated to be large enough.
  static void Write(const void* src, const ColumnType& type, void* dst, uint8_t** buf);

  /// Returns true if v1 == v2.
  /// This is more performant than Compare() == 0 for string equality, mostly because of
  /// the length comparison check.
  static inline bool Eq(const void* v1, const void* v2, const ColumnType& type);
};

}

#endif

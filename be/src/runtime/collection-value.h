// Copyright 2015 Cloudera Inc.
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

#ifndef IMPALA_RUNTIME_COLLECTION_VALUE_H
#define IMPALA_RUNTIME_COLLECTION_VALUE_H

#include "runtime/mem-pool.h"
#include "runtime/descriptors.h"

namespace impala {

/// The in-memory representation of a collection-type slot. Note that both arrays and maps
/// are represented in memory as arrays of tuples. After being read from the on-disk data,
/// arrays and maps are effectively indistinguishable; a map can be thought of as an array
/// of key/value structs (and neither of these fields are necessarily materialized in the
/// item tuples).
struct CollectionValue {
  /// Pointer to buffer containing item tuples.
  uint8_t* ptr;

  /// The number of item tuples.
  int num_tuples;

  CollectionValue() : ptr(NULL), num_tuples(0) {}

  /// Returns the size of this collection in bytes, i.e. the number of bytes written to
  /// ptr.
  inline int64_t ByteSize(const TupleDescriptor& item_tuple_desc) const {
    return num_tuples * item_tuple_desc.byte_size();
  }
};

}

#endif

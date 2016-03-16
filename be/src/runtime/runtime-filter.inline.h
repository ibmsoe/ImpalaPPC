// Copyright 2016 Cloudera Inc.
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


#ifndef IMPALA_RUNTIME_RUNTIME_FILTER_INLINE_H
#define IMPALA_RUNTIME_RUNTIME_FILTER_INLINE_H

#include "runtime/runtime-filter.h"

#include <boost/thread.hpp>

#include "runtime/raw-value.inline.h"
#include "util/bloom-filter.h"
#include "util/time.h"

namespace impala {

inline const RuntimeFilter* RuntimeFilterBank::GetRuntimeFilter(uint32_t filter_id) {
  boost::lock_guard<SpinLock> l(runtime_filter_lock_);
  RuntimeFilterMap::iterator it = consumed_filters_.find(filter_id);
  if (it == consumed_filters_.end()) return NULL;
  return it->second;
}

inline void RuntimeFilter::SetBloomFilter(BloomFilter* bloom_filter) {
  DCHECK(bloom_filter_ == NULL);
  // TODO: Barrier required here to ensure compiler does not both inline and re-order
  // this assignment. Not an issue for correctness (as assignment is atomic), but
  // potentially confusing.
  bloom_filter_ = bloom_filter;
  arrival_time_ = MonotonicMillis();
}

template<typename T>
inline bool RuntimeFilter::Eval(T* val, const ColumnType& col_type) const {
  // Safe to read bloom_filter_ concurrently with any ongoing SetBloomFilter() thanks
  // to a) the atomicity of / pointer assignments and b) the x86 TSO memory model.
  if (bloom_filter_ == NULL) return true;

  uint32_t h = RawValue::GetHashValue(val, col_type,
      RuntimeFilterBank::DefaultHashSeed());
  return bloom_filter_->Find(h);
}

}

#endif

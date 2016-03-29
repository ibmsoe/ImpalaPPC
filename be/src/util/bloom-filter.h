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

#ifndef IMPALA_UTIL_BLOOM_H
#define IMPALA_UTIL_BLOOM_H

#include <math.h>
#include <stdint.h>

#include <limits>

#include "gutil/macros.h"

#include "gen-cpp/ImpalaInternalService_types.h"
#include "runtime/buffered-block-mgr.h"

namespace impala {

/// A BloomFilter stores sets of items and offers a query operation indicating whether or
/// not that item is in the set.  BloomFilters use much less space than other compact data
/// structures, but they are less accurate: for a small percentage of elements, the query
/// operation incorrectly returns true even when the item is not in the set.
///
/// When talking about Bloom filter size, rather than talking about 'size', which might be
/// ambiguous, we distinguish two different quantities:
///
/// 1. Space: the amount of heap memory used
///
/// 2. NDV: the number of unique items that have been inserted
///
/// BloomFilter is implemented using block Bloom filters from Putze et al.'s "Cache-,
/// Hash- and Space-Efficient Bloom Filters". The basic idea is to hash the item to a
/// single cache line and then treat that cache line like a Bloom filter.  This
/// implementation sets 8 bits in each cache-line-sized Bloom filter. This provides a
/// false positive rate near optimal for between 5 and 15 bits per distinct value, which
/// corresponds to false positive probabilities between 0.1% (for 15 bits) and 10% (for 5
/// bits).
class BloomFilter {
 public:
  /// Consumes at most (1 << log_heap_space) bytes on the heap.
  BloomFilter(const int log_heap_space);
  BloomFilter(const TBloomFilter& thrift);
  ~BloomFilter();

  /// Representation of a filter which allows all elements to pass.
  static BloomFilter* ALWAYS_TRUE_FILTER;

  /// Converts 'filter' to its corresponding Thrift representation. If the first argument
  /// is NULL, it is interpreted as a complete filter which contains all elements.
  static void ToThrift(const BloomFilter* filter, TBloomFilter* thrift);

  /// Adds an element to the BloomFilter. The function used to generate 'hash' need not
  /// have good uniformity, but it should have low collision probability. For instance, if
  /// the set of values is 32-bit ints, the identity function is a valid hash function for
  /// this Bloom filter, since the collision probability (the probability that two
  /// non-equal values will have the same hash value) is 0.
  void Insert(const uint32_t hash);

  /// Finds an element in the BloomFilter, returning true if it is found and false (with
  /// high probabilty) if it is not.
  bool Find(const uint32_t hash) const;

  /// Computes the logical OR of this filter with 'other' and stores the result in 'this'.
  void Or(const BloomFilter& other);

  /// As more distinct items are inserted into a BloomFilter, the false positive rate
  /// rises. MaxNdv() returns the NDV (number of distinct values) at which a BloomFilter
  /// constructed with (1 << log_heap_space) bytes of heap space hits false positive
  /// probabilty fpp.
  static size_t MaxNdv(const int log_heap_space, const double fpp);

  /// If we expect to fill a Bloom filter with 'ndv' different unique elements and we
  /// want a false positive probabilty of less than 'fpp', then this is the log (base 2)
  /// of the minimum number of bytes we need.
  static int MinLogSpace(const size_t ndv, const double fpp);

  /// Returns the expected false positive rate for the given ndv and log_heap_space
  static double FalsePositiveProb(const size_t ndv, const int log_heap_space);

  /// Returns amount of heap space used, in bytes
  int64_t GetHeapSpaceUsed() const { return sizeof(Bucket) * (1LL << log_num_buckets_); }

  static int64_t GetExpectedHeapSpaceUsed(uint32_t log_heap_size) {
    DCHECK_GE(log_heap_size, LOG_BUCKET_WORD_BITS);
    return sizeof(Bucket) * (1LL << (log_heap_size - LOG_BUCKET_WORD_BITS));
  }

 private:
  /// log_directory_space_ is the log (base 2) of the number of buckets in the directory.
  const int log_num_buckets_;

  /// directory_mask_ is (1 << log_num_buckets_) - 1. It is precomputed for
  /// efficiency reasons.
  const uint32_t directory_mask_;

  /// The BloomFilter is divided up into Buckets, each of which is a cache line.
  static const uint64_t BUCKET_WORDS = 8;
  typedef uint64_t BucketWord;

  // log2(number of bits in a BucketWord)
  // TODO: Use BitUtil::Log2(numeric_limits<BucketWord>::digits) once we enable C++14 for
  // codegen.
  static const int LOG_BUCKET_WORD_BITS = 6;
  static const BucketWord BUCKET_WORD_MASK = 63; // 2^LOG_BUCKET_WORD_BITS - 1

  /// log2(number of bytes in a bucket)
  static const int LOG_BUCKET_BYTE_SIZE = 6;

  // TODO: Re-enable static_asserts when C++14 is enabled.
  //static_assert((1 << LOG_BUCKET_WORD_BITS) == std::numeric_limits<BucketWord>::digits,
  // "BucketWord must have a bit-width that is be a power of 2, like 64 for uint64_t.");

  typedef BucketWord Bucket[BUCKET_WORDS];
  Bucket* directory_;

  int64_t directory_size() const {
    return 1uLL << (log_num_buckets_ + LOG_BUCKET_BYTE_SIZE);
  }

  /// Serializes this filter as Thrift.
  void ToThrift(TBloomFilter* thrift) const;

  DISALLOW_COPY_AND_ASSIGN(BloomFilter);
};

inline void BloomFilter::Insert(const uint32_t hash) {
  const uint32_t bucket_idx = HashUtil::Rehash32to32(hash) & directory_mask_;
  uint64_t bits_to_set = HashUtil::Rehash32to64(hash);
  // To set 8 bits in an 64-byte cache line, we set one bit in each 64-bit uint64_t in
  // that cache line. This is a "split Bloom filter", and it has approximately the same
  // false positive probability as standard a Bloom filter; See Mitzenmacher's "Bloom
  // Filters and Such". It also has the advantage of requiring fewer random bits for the
  // 2016-era Intel cache line sizes and machine word sizes: log2(64) * 8 = 6 * 8 = 48
  // random bits for a split Bloom filter, but log2(512) * 8 = 72 random bits for a
  // standard Bloom filter. In fact, this leaves the most significant 16 bits of
  // bits_to_set unused.
  DCHECK_GE(std::numeric_limits<uint64_t>::digits, LOG_BUCKET_WORD_BITS * BUCKET_WORDS)
      << "bits_to_set must have enough bits to index into all the bucket words";
  for (int i = 0; i < BUCKET_WORDS; ++i) {
    // Use LOG_BUCKET_WORD_BITS bits of hash data to index into a BucketWord and set one
    // of its bits.
    directory_[bucket_idx][i] |= static_cast<BucketWord>(1)
        << (bits_to_set & BUCKET_WORD_MASK);
    bits_to_set >>= LOG_BUCKET_WORD_BITS;
  }
}

inline bool BloomFilter::Find(const uint32_t hash) const {
  const uint32_t bucket_idx = HashUtil::Rehash32to32(hash) & directory_mask_;
  uint64_t bits_to_set = HashUtil::Rehash32to64(hash);
  for (int i = 0; i < BUCKET_WORDS; ++i) {
    if (!(directory_[bucket_idx][i] &
            (static_cast<BucketWord>(1) << (bits_to_set & BUCKET_WORD_MASK)))) {
      return false;
    }
    bits_to_set >>= LOG_BUCKET_WORD_BITS;
  }
  return true;
}

}  // namespace impala

#endif  // IMPALA_UTIL_BLOOM_H

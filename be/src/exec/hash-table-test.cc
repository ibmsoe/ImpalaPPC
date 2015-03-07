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

#include <stdlib.h>
#include <stdio.h>
#include <iostream>
#include <vector>

#include <gtest/gtest.h>

#include "common/compiler-util.h"
#include "exec/hash-table.inline.h"
#include "exprs/expr.h"
#include "exprs/expr-context.h"
#include "exprs/slot-ref.h"
#include "runtime/mem-pool.h"
#include "runtime/mem-tracker.h"
#include "runtime/string-value.h"
#include "runtime/mem-tracker.h"
#include "util/cpu-info.h"
#include "util/runtime-profile.h"

using namespace std;

namespace impala {

class HashTableTest : public testing::Test {
 public:
  HashTableTest() : mem_pool_(&tracker_) {}

 protected:
  ObjectPool pool_;
  MemTracker tracker_;
  MemPool mem_pool_;
  vector<ExprContext*> build_expr_ctxs_;
  vector<ExprContext*> probe_expr_ctxs_;

  virtual void SetUp() {
    RowDescriptor desc;
    Status status;

    // Not very easy to test complex tuple layouts so this test will use the
    // simplest.  The purpose of these tests is to exercise the hash map
    // internals so a simple build/probe expr is fine.
    Expr* expr = pool_.Add(new SlotRef(TYPE_INT, 0));
    build_expr_ctxs_.push_back(pool_.Add(new ExprContext(expr)));
    status = Expr::Prepare(build_expr_ctxs_, NULL, desc, &tracker_);
    EXPECT_TRUE(status.ok());
    status = Expr::Open(build_expr_ctxs_, NULL);
    EXPECT_TRUE(status.ok());

    expr = pool_.Add(new SlotRef(TYPE_INT, 0));
    probe_expr_ctxs_.push_back(pool_.Add(new ExprContext(expr)));
    status = Expr::Prepare(probe_expr_ctxs_, NULL, desc, &tracker_);
    EXPECT_TRUE(status.ok());
    status = Expr::Open(probe_expr_ctxs_, NULL);
    EXPECT_TRUE(status.ok());
  }

  virtual void TearDown() {
    Expr::Close(build_expr_ctxs_, NULL);
    Expr::Close(probe_expr_ctxs_, NULL);
  }

  TupleRow* CreateTupleRow(int32_t val) {
    uint8_t* tuple_row_mem = mem_pool_.Allocate(sizeof(int32_t*));
    Tuple* tuple_mem = Tuple::Create(sizeof(int32_t), &mem_pool_);
    *reinterpret_cast<int32_t*>(tuple_mem) = val;
    TupleRow* row = reinterpret_cast<TupleRow*>(tuple_row_mem);
    row->SetTuple(0, tuple_mem);
    return row;
  }

  // Wrapper to call private methods on HashTable
  // TODO: understand google testing, there must be a more natural way to do this
  void ResizeTable(HashTable* table, int64_t new_size) {
    table->ResizeBuckets(new_size);
  }

  // Do a full table scan on table.  All values should be between [min,max).  If
  // all_unique, then each key(int value) should only appear once.  Results are
  // stored in results, indexed by the key.  Results must have been preallocated to
  // be at least max size.
  void FullScan(HashTable* table, HashTableCtx* ht_ctx, int min, int max,
      bool all_unique, TupleRow** results, TupleRow** expected) {
    HashTable::Iterator iter = table->Begin(ht_ctx);
    while (iter != table->End()) {
      TupleRow* row = iter.GetRow();
      int32_t val = *reinterpret_cast<int32_t*>(build_expr_ctxs_[0]->GetValue(row));
      EXPECT_GE(val, min);
      EXPECT_LT(val, max);
      if (all_unique) EXPECT_TRUE(results[val] == NULL);
      EXPECT_EQ(row->GetTuple(0), expected[val]->GetTuple(0));
      results[val] = row;
      iter.Next<false>(ht_ctx);
    }
  }

  // Validate that probe_row evaluates overs probe_exprs is equal to build_row
  // evaluated over build_exprs
  void ValidateMatch(TupleRow* probe_row, TupleRow* build_row) {
    EXPECT_TRUE(probe_row != build_row);
    int32_t build_val =
        *reinterpret_cast<int32_t*>(build_expr_ctxs_[0]->GetValue(probe_row));
    int32_t probe_val =
        *reinterpret_cast<int32_t*>(probe_expr_ctxs_[0]->GetValue(build_row));
    EXPECT_EQ(build_val, probe_val);
  }

  struct ProbeTestData {
    TupleRow* probe_row;
    vector<TupleRow*> expected_build_rows;
  };

  void ProbeTest(HashTable* table, HashTableCtx* ht_ctx,
      ProbeTestData* data, int num_data, bool scan) {
    uint32_t hash = 0;
    for (int i = 0; i < num_data; ++i) {
      TupleRow* row = data[i].probe_row;

      HashTable::Iterator iter;
      if (ht_ctx->EvalAndHashProbe(row, &hash)) continue;
      iter = table->Find(ht_ctx, hash);

      if (data[i].expected_build_rows.size() == 0) {
        EXPECT_TRUE(iter == table->End());
      } else {
        if (scan) {
          map<TupleRow*, bool> matched;
          while (iter != table->End()) {
            EXPECT_TRUE(matched.find(iter.GetRow()) == matched.end());
            matched[iter.GetRow()] = true;
            iter.Next<true>(ht_ctx);
          }
          EXPECT_EQ(matched.size(), data[i].expected_build_rows.size());
          for (int j = 0; i < data[j].expected_build_rows.size(); ++j) {
            EXPECT_TRUE(matched[data[i].expected_build_rows[j]]);
          }
        } else {
          EXPECT_EQ(data[i].expected_build_rows.size(), 1);
          EXPECT_TRUE(
              data[i].expected_build_rows[0]->GetTuple(0) == iter.GetRow()->GetTuple(0));
          ValidateMatch(row, iter.GetRow());
        }
      }
    }
  }

  void ScanTestImpl(int initial_size, int rows_to_insert, int additional_rows) {
    int total_rows = rows_to_insert + additional_rows;
    int target_size = BitUtil::NextPowerOfTwo(initial_size);
    HashTable hash_table(&mem_pool_, target_size);
    HashTableCtx ht_ctx(build_expr_ctxs_, probe_expr_ctxs_, false, false, 1, 0, 1);

    // Add 1 row with val 1, 2 with val 2, etc
    vector<TupleRow*> build_rows;
    ProbeTestData* probe_rows = new ProbeTestData[total_rows];
    probe_rows[0].probe_row = CreateTupleRow(0);
    uint32_t hash = 0;
    for (int val = 1; val <= rows_to_insert; ++val) {
      probe_rows[val].probe_row = CreateTupleRow(val);
      for (int i = 0; i < val; ++i) {
        TupleRow* row = CreateTupleRow(val);
        if (!ht_ctx.EvalAndHashBuild(row, &hash)) continue;
        hash_table.Insert(&ht_ctx, row->GetTuple(0), hash);
        build_rows.push_back(row);
        probe_rows[val].expected_build_rows.push_back(row);
      }
    }

    // Add some more probe rows that aren't there.
    for (int val = rows_to_insert; val < rows_to_insert + additional_rows; ++val) {
      probe_rows[val].probe_row = CreateTupleRow(val);
    }

    // Test that all the builds were found.
    ProbeTest(&hash_table, &ht_ctx, probe_rows, total_rows, true);

    // Resize and try again.
    target_size = BitUtil::NextPowerOfTwo(2 * total_rows);
    ResizeTable(&hash_table, target_size);
    EXPECT_EQ(hash_table.num_buckets(), target_size);
    ProbeTest(&hash_table, &ht_ctx, probe_rows, total_rows, true);

    target_size = BitUtil::NextPowerOfTwo(total_rows + 1);
    ResizeTable(&hash_table, target_size);
    EXPECT_EQ(hash_table.num_buckets(), target_size);
    ProbeTest(&hash_table, &ht_ctx, probe_rows, total_rows, true);

    target_size = BitUtil::NextPowerOfTwo(total_rows / 2);
    ResizeTable(&hash_table, target_size);
    EXPECT_EQ(hash_table.num_buckets(), target_size);
    ProbeTest(&hash_table, &ht_ctx, probe_rows, total_rows, true);

    delete [] probe_rows;
    hash_table.Close();
    mem_pool_.FreeAll();
  }
};

TEST_F(HashTableTest, SetupTest) {
  TupleRow* build_row1 = CreateTupleRow(1);
  TupleRow* build_row2 = CreateTupleRow(2);
  TupleRow* probe_row3 = CreateTupleRow(3);
  TupleRow* probe_row4 = CreateTupleRow(4);

  int32_t* val_row1 =
      reinterpret_cast<int32_t*>(build_expr_ctxs_[0]->GetValue(build_row1));
  EXPECT_EQ(*val_row1, 1);
  int32_t* val_row2 =
      reinterpret_cast<int32_t*>(build_expr_ctxs_[0]->GetValue(build_row2));
  EXPECT_EQ(*val_row2, 2);
  int32_t* val_row3 =
      reinterpret_cast<int32_t*>(probe_expr_ctxs_[0]->GetValue(probe_row3));
  EXPECT_EQ(*val_row3, 3);
  int32_t* val_row4 =
      reinterpret_cast<int32_t*>(probe_expr_ctxs_[0]->GetValue(probe_row4));
  EXPECT_EQ(*val_row4, 4);

  mem_pool_.FreeAll();
}

// This tests inserts the build rows [0->5) to hash table.  It validates that they
// are all there using a full table scan.  It also validates that Find() is correct
// testing for probe rows that are both there and not.
// The hash table is rehashed a few times and the scans/finds are tested again.
TEST_F(HashTableTest, BasicTest) {
  TupleRow* build_rows[5];
  TupleRow* scan_rows[5] = {0};
  for (int i = 0; i < 5; ++i) {
    build_rows[i] = CreateTupleRow(i);
  }

  ProbeTestData probe_rows[10];
  for (int i = 0; i < 10; ++i) {
    probe_rows[i].probe_row = CreateTupleRow(i);
    if (i < 5) {
      probe_rows[i].expected_build_rows.push_back(build_rows[i]);
    }
  }

  // Create the hash table and insert the build rows
  HashTable hash_table(&mem_pool_);
  HashTableCtx ht_ctx(build_expr_ctxs_, probe_expr_ctxs_, false, false, 1, 0, 1);

  uint32_t hash = 0;
  for (int i = 0; i < 5; ++i) {
    if (!ht_ctx.EvalAndHashBuild(build_rows[i], &hash)) continue;
    hash_table.Insert(&ht_ctx, build_rows[i]->GetTuple(0), hash);
  }
  EXPECT_EQ(hash_table.size(), 5);

  // Do a full table scan and validate returned pointers
  FullScan(&hash_table, &ht_ctx, 0, 5, true, scan_rows, build_rows);
  ProbeTest(&hash_table, &ht_ctx, probe_rows, 10, false);

  // Resize and scan again
  ResizeTable(&hash_table, 64);
  EXPECT_EQ(hash_table.num_buckets(), 64);
  EXPECT_EQ(hash_table.size(), 5);
  memset(scan_rows, 0, sizeof(scan_rows));
  FullScan(&hash_table, &ht_ctx, 0, 5, true, scan_rows, build_rows);
  ProbeTest(&hash_table, &ht_ctx, probe_rows, 10, false);

  // Resize to two and cause some collisions
  ResizeTable(&hash_table, 2);
  EXPECT_EQ(hash_table.num_buckets(), 2);
  EXPECT_EQ(hash_table.size(), 5);
  memset(scan_rows, 0, sizeof(scan_rows));
  FullScan(&hash_table, &ht_ctx, 0, 5, true, scan_rows, build_rows);
  ProbeTest(&hash_table, &ht_ctx, probe_rows, 10, false);

  // Resize to one and turn it into a linked list
  ResizeTable(&hash_table, 1);
  EXPECT_EQ(hash_table.num_buckets(), 1);
  EXPECT_EQ(hash_table.size(), 5);
  memset(scan_rows, 0, sizeof(scan_rows));
  FullScan(&hash_table, &ht_ctx, 0, 5, true, scan_rows, build_rows);
  ProbeTest(&hash_table, &ht_ctx, probe_rows, 10, false);

  hash_table.Close();
  mem_pool_.FreeAll();
}

// This test makes sure we can scan ranges of buckets.
TEST_F(HashTableTest, ScanTest) {
  ScanTestImpl(1024, 10, 5);
  ScanTestImpl(1024, 1000, 5);
  ScanTestImpl(1024, 1000, 500);
}

// This test continues adding to the hash table to trigger the resize code paths
TEST_F(HashTableTest, GrowTableTest) {
  int num_to_add = 4;
  int expected_size = 0;

  // Allocate a new pool to test OOM.
  MemTracker tracker(100 * 1024 * 1024);
  MemPool pool(&tracker);
  HashTable hash_table(&pool, num_to_add);
  HashTableCtx ht_ctx(build_expr_ctxs_, probe_expr_ctxs_, false, false, 1, 0, 1);

  // This inserts about 5M entries
  int build_row_val = 0;
  uint32_t hash = 0;
  for (int i = 0; i < 20; ++i) {
    for (int j = 0; j < num_to_add; ++build_row_val, ++j) {
      TupleRow* row = CreateTupleRow(build_row_val);
      if (!ht_ctx.EvalAndHashBuild(row, &hash)) continue;
      if (!hash_table.Insert(&ht_ctx, row->GetTuple(0), hash)) goto done_inserting;
    }
    expected_size += num_to_add;
    num_to_add *= 2;
  }
done_inserting:
  EXPECT_TRUE(tracker.LimitExceeded());

  // Validate that we can find the entries before we went over the limit
  for (int i = 0; i < expected_size * 5; i += 100000) {
    TupleRow* probe_row = CreateTupleRow(i);
    if (!ht_ctx.EvalAndHashProbe(probe_row, &hash)) continue;
    HashTable::Iterator iter = hash_table.Find(&ht_ctx, hash);
    if (i < hash_table.size()) {
      EXPECT_TRUE(iter != hash_table.End());
      ValidateMatch(probe_row, iter.GetRow());
    } else {
      EXPECT_TRUE(iter == hash_table.End());
    }
  }
  hash_table.Close();
  pool.FreeAll();
  mem_pool_.FreeAll();
}

}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  impala::CpuInfo::Init();
  return RUN_ALL_TESTS();
}

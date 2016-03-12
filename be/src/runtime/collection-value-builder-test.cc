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

#include <gtest/gtest.h>

#include "runtime/collection-value-builder.h"
#include "testutil/desc-tbl-builder.h"
#include "testutil/gtest-util.h"

#include "common/names.h"

using namespace impala;

TEST(CollectionValueBuilderTest, MaxBufferSize) {
  ObjectPool obj_pool;
  DescriptorTblBuilder builder(&obj_pool);
  builder.DeclareTuple() << TYPE_TINYINT << TYPE_TINYINT << TYPE_TINYINT;
  DescriptorTbl* desc_tbl = builder.Build();
  vector<TupleDescriptor*> descs;
  desc_tbl->GetTupleDescs(&descs);
  ASSERT_EQ(descs.size(), 1);
  const TupleDescriptor& tuple_desc = *descs[0];
  // Tuple includes a null byte (4 = 1 + 3).
  ASSERT_EQ(tuple_desc.byte_size(), 4);

  // Create CollectionValue with buffer size of slightly more than INT_MAX / 2
  CollectionValue coll_value;
  int64_t initial_capacity = (INT_MAX / 8) + 1;
  int64_t mem_limit = initial_capacity * 4 * 4;
  MemTracker tracker(mem_limit, mem_limit);
  MemPool pool(&tracker);
  CollectionValueBuilder coll_value_builder(
      &coll_value, tuple_desc, &pool, initial_capacity);
  EXPECT_EQ(tracker.consumption(), initial_capacity * 4);

  // Attempt to double the buffer so it goes over 32-bit INT_MAX.
  ASSERT_GT(tracker.consumption(), INT_MAX / 2);
  coll_value_builder.CommitTuples(initial_capacity);
  Tuple* tuple_mem;
  int num_tuples;
  EXPECT_OK(coll_value_builder.GetFreeMemory(&tuple_mem, &num_tuples));
  EXPECT_EQ(num_tuples, initial_capacity);
  EXPECT_EQ(tracker.consumption(), initial_capacity * (1 + 2) * 4);

  // Attempt to double the buffer again but it will fail as it exceeds
  // the memory limit.
  coll_value_builder.CommitTuples(initial_capacity);
  Status status = coll_value_builder.GetFreeMemory(&tuple_mem, &num_tuples);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(num_tuples, 0);
  EXPECT_EQ(tracker.consumption(), initial_capacity * (1 + 2) * 4);

  pool.FreeAll();
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}


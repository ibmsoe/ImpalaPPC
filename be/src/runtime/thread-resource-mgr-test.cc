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

#include <string>
#include <boost/bind.hpp>
#include <gtest/gtest.h>

#include "runtime/thread-resource-mgr.h"
#include "util/cpu-info.h"

#include "common/names.h"

namespace impala {

class NotifiedCounter {
 public:
  NotifiedCounter() : counter_(0) {
  }

  void Notify(ThreadResourceMgr::ResourcePool* consumer) {
    ASSERT_TRUE(consumer != NULL);
    ASSERT_LT(consumer->num_threads(), consumer->quota());
    ++counter_;
  }

  int counter() const { return counter_; }

 private:
  int counter_;
};

TEST(ThreadResourceMgr, BasicTest) {
  ThreadResourceMgr mgr(5);
  NotifiedCounter counter1, counter2;

  ThreadResourceMgr::ResourcePool* c1 = mgr.RegisterPool();
  int callback1 = c1->AddThreadAvailableCb(bind<void>(mem_fn(&NotifiedCounter::Notify),
      &counter1, _1));
  c1->AcquireThreadToken();
  c1->AcquireThreadToken();
  c1->AcquireThreadToken();
  EXPECT_EQ(c1->num_threads(), 3);
  EXPECT_EQ(c1->num_required_threads(), 3);
  EXPECT_EQ(c1->num_optional_threads(), 0);
  EXPECT_EQ(counter1.counter(), 0);
  c1->ReleaseThreadToken(true);
  EXPECT_EQ(c1->num_threads(), 2);
  EXPECT_EQ(c1->num_required_threads(), 2);
  EXPECT_EQ(c1->num_optional_threads(), 0);
  EXPECT_EQ(counter1.counter(), 1);
  bool is_reserved = false;
  c1->ReserveOptionalTokens(1);
  EXPECT_TRUE(c1->TryAcquireThreadToken(&is_reserved));
  EXPECT_TRUE(is_reserved);
  EXPECT_TRUE(c1->TryAcquireThreadToken(&is_reserved));
  EXPECT_FALSE(is_reserved);
  EXPECT_TRUE(c1->TryAcquireThreadToken(&is_reserved));
  EXPECT_FALSE(is_reserved);
  EXPECT_FALSE(c1->TryAcquireThreadToken(&is_reserved));
  EXPECT_FALSE(is_reserved);
  EXPECT_EQ(c1->num_threads(), 5);
  EXPECT_EQ(c1->num_required_threads(), 2);
  EXPECT_EQ(c1->num_optional_threads(), 3);
  c1->ReleaseThreadToken(true);
  c1->ReleaseThreadToken(false);
  EXPECT_EQ(counter1.counter(), 3);

  // Register a new consumer, quota is cut in half
  ThreadResourceMgr::ResourcePool* c2 = mgr.RegisterPool();
  int callback2 = c2->AddThreadAvailableCb(bind<void>(mem_fn(&NotifiedCounter::Notify),
      &counter2, _1));
  EXPECT_FALSE(c1->TryAcquireThreadToken());
  EXPECT_EQ(c1->num_threads(), 3);
  c1->AcquireThreadToken();
  EXPECT_EQ(c1->num_threads(), 4);
  EXPECT_EQ(c1->num_required_threads(), 2);
  EXPECT_EQ(c1->num_optional_threads(), 2);

  c1->RemoveThreadAvailableCb(callback1);
  mgr.UnregisterPool(c1);
  c2->RemoveThreadAvailableCb(callback2);
  mgr.UnregisterPool(c2);
  EXPECT_EQ(counter1.counter(), 3);
  EXPECT_EQ(counter2.counter(), 1);
}

TEST(ThreadResourceMgr, MultiCallbacks) {
  ThreadResourceMgr mgr(6);
  NotifiedCounter counter1, counter2, counter3;

  ThreadResourceMgr::ResourcePool* c1 = mgr.RegisterPool();
  int callback1 = c1->AddThreadAvailableCb(
      bind<void>(mem_fn(&NotifiedCounter::Notify), &counter1, _1));
  int callback2 = c1->AddThreadAvailableCb(
      bind<void>(mem_fn(&NotifiedCounter::Notify), &counter2, _1));

  // Round-robin between two callbacks.
  c1->AcquireThreadToken();
  c1->AcquireThreadToken();
  c1->AcquireThreadToken();

  c1->ReleaseThreadToken(true);
  EXPECT_EQ(counter1.counter(), 1);
  EXPECT_EQ(counter2.counter(), 1);
  c1->ReleaseThreadToken(true);
  EXPECT_EQ(counter1.counter(), 2);
  EXPECT_EQ(counter2.counter(), 2);
  c1->ReleaseThreadToken(true);
  EXPECT_EQ(counter1.counter(), 3);
  EXPECT_EQ(counter2.counter(), 3);

  // Remove callback 2. Only callback 1 will be called.
  c1->AcquireThreadToken();
  c1->AcquireThreadToken();
  c1->AcquireThreadToken();
  c1->RemoveThreadAvailableCb(callback2);

  c1->ReleaseThreadToken(true);
  EXPECT_EQ(counter1.counter(), 4);
  EXPECT_EQ(counter2.counter(), 3);
  c1->ReleaseThreadToken(true);
  EXPECT_EQ(counter1.counter(), 5);
  EXPECT_EQ(counter2.counter(), 3);
  c1->ReleaseThreadToken(true);
  EXPECT_EQ(counter1.counter(), 6);
  EXPECT_EQ(counter2.counter(), 3);

  // Also remove callback 1. No callback will be called.
  c1->AcquireThreadToken();
  c1->AcquireThreadToken();
  c1->AcquireThreadToken();
  c1->RemoveThreadAvailableCb(callback1);

  c1->ReleaseThreadToken(true);
  EXPECT_EQ(counter1.counter(), 6);
  EXPECT_EQ(counter2.counter(), 3);
  c1->ReleaseThreadToken(true);
  EXPECT_EQ(counter1.counter(), 6);
  EXPECT_EQ(counter2.counter(), 3);
  c1->ReleaseThreadToken(true);
  EXPECT_EQ(counter1.counter(), 6);
  EXPECT_EQ(counter2.counter(), 3);

  // Also verify UnregisterPool() will invoke the callback.
  ThreadResourceMgr::ResourcePool* c2 = mgr.RegisterPool();
  c2->AddThreadAvailableCb(
      bind<void>(mem_fn(&NotifiedCounter::Notify), &counter3, _1));
  EXPECT_EQ(counter3.counter(), 0);
  mgr.UnregisterPool(c1);
  EXPECT_EQ(counter3.counter(), 1);
}

}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  impala::CpuInfo::Init();
  return RUN_ALL_TESTS();
}

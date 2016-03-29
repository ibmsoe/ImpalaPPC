# Copyright (c) 2016 Cloudera, Inc. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

import time

from tests.common.test_vector import *
from tests.common.impala_test_suite import *

class TestRuntimeFilters(ImpalaTestSuite):
  @classmethod
  def get_workload(cls):
    return 'functional-query'

  @classmethod
  def add_test_dimensions(cls):
    super(TestRuntimeFilters, cls).add_test_dimensions()
    # Runtime filters are disabled on HBase
    cls.TestMatrix.add_constraint(
      lambda v: v.get_value('table_format').file_format != 'hbase')

  def test_basic_filters(self, vector):
    self.run_test_case('QueryTest/runtime_filters', vector)

  def test_wait_time(self, vector):
    """Test that a query that has global filters does not wait for them if run in LOCAL
    mode"""
    now = time.time()
    self.run_test_case('QueryTest/runtime_filters_wait', vector)
    duration = time.time() - now
    assert duration < 60, \
      "Query took too long (%ss, possibly waiting for missing filters?)" % str(duration)


class TestRuntimeRowFilters(ImpalaTestSuite):
  @classmethod
  def get_workload(cls):
    return 'functional-query'

  @classmethod
  def add_test_dimensions(cls):
    super(TestRuntimeRowFilters, cls).add_test_dimensions()
    cls.TestMatrix.add_constraint(lambda v:
        v.get_value('table_format').file_format in ['parquet'])

  def test_row_filters(self, vector):
    self.run_test_case('QueryTest/runtime_row_filters', vector)

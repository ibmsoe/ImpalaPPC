# Copyright (c) 2015 Cloudera, Inc. All rights reserved.
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

import logging
import pytest
from copy import deepcopy
from tests.common.custom_cluster_test_suite import CustomClusterTestSuite

class TestAllocFail(CustomClusterTestSuite):
  """Tests for handling malloc() failure for UDF/UDA"""

  @classmethod
  def get_workload(self):
    return 'functional-query'

  @pytest.mark.execute_serially
  @CustomClusterTestSuite.with_args("--stress_free_pool_alloc=1")
  def test_alloc_fail_init(self, vector):
    self.run_test_case('QueryTest/alloc-fail-init', vector)

  @pytest.mark.execute_serially
  @CustomClusterTestSuite.with_args("--stress_free_pool_alloc=3")
  def test_alloc_fail_update(self, vector):
    self.run_test_case('QueryTest/alloc-fail-update', vector)

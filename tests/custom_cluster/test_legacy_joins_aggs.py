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

class TestLegacyJoinsAggs(CustomClusterTestSuite):
  """Tests the behavior of the legacy join and agg nodes with nested types."""

  @classmethod
  def get_workload(self):
    return 'functional-query'

  @CustomClusterTestSuite.with_args(
      impalad_args=('--enable_partitioned_hash_join=false '
                    '--enable_partitioned_aggregation=false'),
      catalogd_args='--load_catalog_in_background=false')
  def test_nested_types(self, vector):
    self.run_test_case('QueryTest/legacy-joins-aggs', vector,
      use_db='functional_parquet')

#!/usr/bin/env python
# Copyright (c) 2012 Cloudera, Inc. All rights reserved.
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

import os
import pytest
import re
import uuid
from subprocess import call
from tests.common.test_vector import TestDimension
from tests.common.impala_test_suite import ImpalaTestSuite
from tests.common.skip import *

# Number of tables to create per thread
NUM_TBLS_PER_THREAD = 10

# Each client will get a different test id.
TEST_IDS = xrange(0, 10)

# Simple stress test for DDL operations. Attempts to create, cache,
# uncache, then drop many different tables in parallel.
class TestDdlStress(ImpalaTestSuite):
  @classmethod
  def get_workload(self):
    return 'targeted-stress'

  @classmethod
  def add_test_dimensions(cls):
    super(TestDdlStress, cls).add_test_dimensions()
    cls.TestMatrix.add_dimension(TestDimension('test_id', *TEST_IDS))
    cls.TestMatrix.add_constraint(lambda v: v.get_value('exec_option')['batch_size'] == 0)

    cls.TestMatrix.add_constraint(lambda v:\
        v.get_value('table_format').file_format == 'text' and\
        v.get_value('table_format').compression_codec == 'none')

  @skip_if_s3_caching
  @pytest.mark.stress
  def test_create_cache_many_tables(self, vector):
    self.client.set_configuration(vector.get_value('exec_option'))
    self.client.execute("create database if not exists ddl_stress_testdb")
    self.client.execute("use ddl_stress_testdb")
    tbl_uniquifier = str(uuid.uuid4()).replace('-', '')
    for i in xrange(NUM_TBLS_PER_THREAD):
      tbl_name = "tmp_%s_%s" % (tbl_uniquifier, i)
      # Create a partitioned and unpartitioned table
      self.client.execute("create table %s (i int)" % tbl_name)
      self.client.execute("create table %s_part (i int) partitioned by (j int)" %\
          tbl_name)
      # Add some data to each
      self.client.execute("insert overwrite table %s select int_col from "\
          "functional.alltypestiny" % tbl_name)
      self.client.execute("insert overwrite table %s_part partition(j) "\
          "values (1, 1), (2, 2), (3, 3), (4, 4), (4, 4)" % tbl_name)

      # Cache the data the unpartitioned table
      self.client.execute("alter table %s set cached in 'testPool'" % tbl_name)
      # Cache, uncache, then re-cache the data in the partitioned table.
      self.client.execute("alter table %s_part set cached in 'testPool'" % tbl_name)
      self.client.execute("alter table %s_part set uncached" % tbl_name)
      self.client.execute("alter table %s_part set cached in 'testPool'" % tbl_name)
      # Drop the tables, this should remove the cache requests.
      self.client.execute("drop table %s" % tbl_name)
      self.client.execute("drop table %s_part" % tbl_name)

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
#
import os
import pytest
from tests.common.skip import SkipIfS3
from tests.common.custom_cluster_test_suite import CustomClusterTestSuite
from tests.util.hdfs_util import HdfsConfig, get_hdfs_client, get_hdfs_client_from_conf


@SkipIfS3.insert
class TestInsertBehaviourCustomCluster(CustomClusterTestSuite):
  def check_partition_perms(self, part, perms):
    ls = self.hdfs_client.get_file_dir_status(
      "test-warehouse/functional.db/insert_inherit_permission/%s" % part)
    assert ls['FileStatus']['permission'] == perms

  @classmethod
  def setup_class(cls):
    if pytest.config.option.namenode_http_address is None:
      hdfs_conf = HdfsConfig(pytest.config.option.minicluster_xml_conf)
      cls.hdfs_client = get_hdfs_client_from_conf(hdfs_conf)
    else:
      host, port = pytest.config.option.namenode_http_address.split(":")
      cls.hdfs_client = get_hdfs_client(host, port)

  @pytest.mark.execute_serially
  @CustomClusterTestSuite.with_args("--insert_inherit_permissions=true")
  def test_insert_inherit_permission(self):
    """Create a table with three partition columns to test permission inheritance"""
    impalad = self.cluster.get_any_impalad()
    client = impalad.service.create_beeswax_client()

    self.execute_query_expect_success(client, "DROP TABLE IF EXISTS"
                                      " functional.insert_inherit_permission")
    self.execute_query_expect_success(client, "CREATE TABLE "
                                      "functional.insert_inherit_permission (col int)"
                                      " PARTITIONED BY (p1 int, p2 int, p3 int)")

    self.execute_query_expect_success(client, "ALTER TABLE "
                                      "functional.insert_inherit_permission ADD PARTITION"
                                      "(p1=1, p2=1, p3=1)")
    self.hdfs_client.chmod(
      "test-warehouse/functional.db/insert_inherit_permission/p1=1/", "777")

    # 1. INSERT that creates two new directories gets permissions from parent
    self.execute_query_expect_success(client, "INSERT INTO "
                                      "functional.insert_inherit_permission "
                                      "PARTITION(p1=1, p2=2, p3=2) VALUES(1)")
    self.check_partition_perms("p1=1/p2=2/", "777")
    self.check_partition_perms("p1=1/p2=2/p3=2/", "777")

    # 2. INSERT that creates one new directory gets permissions from parent
    self.execute_query_expect_success(client, "INSERT INTO "
                                      "functional.insert_inherit_permission "
                                      "PARTITION(p1=1, p2=2, p3=3) VALUES(1)")
    self.check_partition_perms("p1=1/p2=2/p3=3/", "777")

    # 3. INSERT that creates no new directories keeps standard permissions
    self.hdfs_client.chmod(
      "test-warehouse/functional.db/insert_inherit_permission/p1=1/p2=2", "644")
    self.execute_query_expect_success(client, "INSERT INTO "
                                      "functional.insert_inherit_permission "
                                      "PARTITION(p1=1, p2=2, p3=3) VALUES(1)")
    self.check_partition_perms("p1=1/p2=2/", "644")
    self.check_partition_perms("p1=1/p2=2/p3=3/", "777")


  @pytest.mark.execute_serially
  @CustomClusterTestSuite.with_args("--insert_inherit_permissions=false")
  def test_insert_inherit_permission_disabled(self):
    """Check that turning off insert permission inheritance works correctly."""
    impalad = self.cluster.get_any_impalad()
    client = impalad.service.create_beeswax_client()

    self.execute_query_expect_success(client, "DROP TABLE IF EXISTS"
                                      " functional.insert_inherit_permission")
    self.execute_query_expect_success(client, "CREATE TABLE "
                                      "functional.insert_inherit_permission (col int)"
                                      " PARTITIONED BY (p1 int, p2 int, p3 int)")

    self.execute_query_expect_success(client, "ALTER TABLE "
                                      "functional.insert_inherit_permission ADD PARTITION"
                                      "(p1=1, p2=1, p3=1)")
    ls = self.hdfs_client.get_file_dir_status(
      "test-warehouse/functional.db/insert_inherit_permission/p1=1/")
    default_perms = ls['FileStatus']['permission']

    self.hdfs_client.chmod(
      "test-warehouse/functional.db/insert_inherit_permission/p1=1/", "777")

    self.execute_query_expect_success(client, "INSERT INTO "
                                      "functional.insert_inherit_permission "
                                      "PARTITION(p1=1, p2=3, p3=4) VALUES(1)")
    # Would be 777 if inheritance was enabled
    self.check_partition_perms("p1=1/p2=3/", default_perms)

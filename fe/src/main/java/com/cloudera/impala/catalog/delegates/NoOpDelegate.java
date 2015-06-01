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

package com.cloudera.impala.catalog.delegates;

import org.apache.hadoop.hive.metastore.api.Table;

import com.cloudera.impala.common.ImpalaRuntimeException;
import com.cloudera.impala.thrift.TAlterTableParams;

/**
 * Empty implementation for the DdlDelegate interface that does nothing.
 */
public class NoOpDelegate implements DdlDelegate {

  @Override
  public void createTable(Table msTbl) throws ImpalaRuntimeException { }

  @Override
  public void dropTable(Table msTbl) throws ImpalaRuntimeException { }

  @Override
  public boolean alterTable(Table msTbl, TAlterTableParams params)
      throws ImpalaRuntimeException {
    return true;
  }

  @Override
  public boolean canHandle(Table table) { return false; }

}

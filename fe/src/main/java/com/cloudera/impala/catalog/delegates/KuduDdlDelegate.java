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

import java.util.ArrayList;
import java.util.HashSet;

import org.apache.hadoop.hive.metastore.TableType;
import org.apache.hadoop.hive.metastore.api.FieldSchema;
import org.apache.log4j.Logger;
import org.kududb.ColumnSchema;
import org.kududb.ColumnSchema.ColumnSchemaBuilder;
import org.kududb.Schema;
import org.kududb.Type;
import org.kududb.client.CreateTableBuilder;
import org.kududb.client.KuduClient;

import com.cloudera.impala.catalog.KuduTable;
import com.cloudera.impala.common.ImpalaRuntimeException;
import com.cloudera.impala.thrift.TAlterTableParams;
import com.cloudera.impala.util.KuduUtil;
import com.google.common.collect.Lists;

/**
 * Implementation of the Kudu DDL Delegate. Propagates create and drop table statements to
 * Kudu.
 */
public class KuduDdlDelegate implements DdlDelegate {

  private static final Logger LOG = Logger.getLogger(KuduDdlDelegate.class);

  /**
   * Creates the Kudu table if it does not exist and returns true. If the table exists and
   * the table is not a managed table ignore and return false, otherwise throw an
   * exception.
   */
  @Override
  public void createTable(org.apache.hadoop.hive.metastore.api.Table msTbl)
      throws ImpalaRuntimeException {

    String kuduTableName = msTbl.getParameters().get(KuduTable.KEY_TABLE_NAME);
    String kuduMasters = msTbl.getParameters().get(KuduTable.KEY_MASTER_ADDRESSES);

    // Can be optional for un-managed tables
    String kuduKeyCols = msTbl.getParameters().get(KuduTable.KEY_KEY_COLUMNS);

    try {
      KuduClient client = new KuduClient(KuduUtil.stringToHostAndPort(kuduMasters));

      // TODO should we throw if the table does not exist when its an external table?
      if (client.tableExists(kuduTableName)) {
        if (msTbl.getTableType().equals(TableType.MANAGED_TABLE.toString())) {
          throw new ImpalaRuntimeException(String.format(
              "Table %s already exists in Kudu master %s.", kuduTableName, kuduMasters));
        }

        // Check if the external table matches the schema
        org.kududb.client.KuduTable kuduTable = client.openTable(kuduTableName);
        if (!KuduUtil.compareSchema(msTbl, kuduTable)) {
          throw new ImpalaRuntimeException(String.format(
              "Table %s (%s) has a different schema in Kudu than in Hive.",
              msTbl.getTableName(), kuduTableName));
        }
        return;
      }

      HashSet<String> keyCols = KuduUtil.parseKeyColumns(kuduKeyCols);

      // Create a new Schema and map the types accordingly
      ArrayList<ColumnSchema> columns = Lists.newArrayList();
      for (FieldSchema fieldSchema: msTbl.getSd().getCols()) {
        com.cloudera.impala.catalog.Type catalogType = com.cloudera.impala.catalog.Type
            .parseColumnType(fieldSchema);
        if (catalogType == null) {
          throw new ImpalaRuntimeException(String.format(
              "Could not parse column type %s.", fieldSchema.getType()));
        }
        Type t = KuduUtil.fromImpalaType(catalogType);
        // Create the actual column and check if the column is a key column
        ColumnSchemaBuilder csb = new ColumnSchemaBuilder(fieldSchema.getName(), t);
        boolean isKeyColumn = keyCols.contains(fieldSchema.getName());
        csb.key(isKeyColumn);
        csb.nullable(!isKeyColumn);
        columns.add(csb.build());
      }

      Schema schema = new Schema(columns);
      CreateTableBuilder ctb = new CreateTableBuilder();
      client.createTable(kuduTableName, schema, ctb);
    } catch (ImpalaRuntimeException e) {
      throw e;
    } catch (Exception e) {
      throw new ImpalaRuntimeException("Error creating Kudu table", e);
    }
  }

  @Override
  public void dropTable(org.apache.hadoop.hive.metastore.api.Table msTbl)
      throws ImpalaRuntimeException {
    // If table is an external table, do not delete the data
    if (msTbl.getTableType().equals(TableType.EXTERNAL_TABLE.toString())) return;

    String kuduTableName = msTbl.getParameters().get(KuduTable.KEY_TABLE_NAME);
    String kuduMasters = msTbl.getParameters().get(KuduTable.KEY_MASTER_ADDRESSES);

    try {
      KuduClient client = new KuduClient(KuduUtil.stringToHostAndPort(kuduMasters));
      if (!client.tableExists(kuduTableName)) {
        throw new ImpalaRuntimeException(String.format(
            "Table %s does not exist in Kudu master %s", kuduTableName, kuduMasters));
      }
      client.deleteTable(kuduTableName);
      return;
    } catch (ImpalaRuntimeException e) {
      throw e;
    } catch (Exception e) {
      throw new ImpalaRuntimeException("Error dropping Kudu table", e);
    }
  }

  @Override
  public boolean canHandle(org.apache.hadoop.hive.metastore.api.Table msTable) {
    return KuduTable.isKuduTable(msTable);
  }

  @Override
  public boolean alterTable(org.apache.hadoop.hive.metastore.api.Table msTbl,
      TAlterTableParams params) throws ImpalaRuntimeException {
    throw new ImpalaRuntimeException(
        "Alter table operations are not supported for Kudu tables.");
  }

}

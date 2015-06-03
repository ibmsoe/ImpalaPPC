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

package com.cloudera.impala.catalog;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import org.apache.hadoop.hive.metastore.HiveMetaStoreClient;
import org.apache.hadoop.hive.metastore.TableType;
import org.apache.hadoop.hive.metastore.api.FieldSchema;
import org.apache.hadoop.hive.metastore.api.hive_metastoreConstants;
import org.apache.log4j.Logger;

import com.cloudera.impala.thrift.TCatalogObjectType;
import com.cloudera.impala.thrift.TKuduTable;
import com.cloudera.impala.thrift.TTable;
import com.cloudera.impala.thrift.TTableDescriptor;
import com.cloudera.impala.thrift.TTableType;
import com.cloudera.impala.util.KuduUtil;
import com.google.common.base.Preconditions;
import com.google.common.base.Splitter;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.ImmutableSet;
import com.google.common.collect.Iterables;
import com.google.common.collect.Sets;
import com.google.common.net.HostAndPort;

/**
 * Impala representation of a Kudu table.
 *
 * The Kudu-related metadata is stored in the Metastore table's table properties.
 */
public class KuduTable extends Table {
  private static final Logger LOG = Logger.getLogger(Table.class);

  // Alias to the string key that identifies the storage handler of a particular table
  public static final String KEY_STORAGE_HANDLER =
      hive_metastoreConstants.META_TABLE_STORAGE;

  // Key to access the table name from the table properties
  public static final String KEY_TABLE_NAME = "kudu.table_name";

  // Key to access the columns used to build the (composite) key of the table.
  // The order of the keys is important.
  public static final String KEY_KEY_COLUMNS = "kudu.key_columns";

  // Key to access the master address from the table properties
  // TODO we should have something like KuduConfig.getDefaultConfig()
  public static final String KEY_MASTER_ADDRESSES = "kudu.master_addresses";

  // Value used to identify a Kudu table by a specific storage handler property
  public static final String KUDU_STORAGE_HANDLER =
      "com.cloudera.kudu.hive.KuduStorageHandler";

  public static final long KUDU_RPC_TIMEOUT_MS = 50000;

  // The name of the table in Kudu.
  private String kuduTableName_;

  // The set of Kudu masters.
  private List<HostAndPort> kuduMasters_;

  // The set of columns that are key columns in Kudu.
  private List<String> kuduKeyColumnNames_;

  protected KuduTable(TableId id, org.apache.hadoop.hive.metastore.api.Table msTable,
      Db db, String name, String owner) {
    super(id, msTable, db, name, owner);
  }

  TKuduTable getKuduTable() {
    Preconditions.checkNotNull(kuduKeyColumnNames_);
    TKuduTable tbl = new TKuduTable();
    tbl.setKey_columns(kuduKeyColumnNames_);
    tbl.setMaster_addresses(KuduUtil.hostAndPortToString(kuduMasters_));
    tbl.setTable_name(kuduTableName_);
    return tbl;
  }

  @Override
  public TTableDescriptor toThriftDescriptor(Set<Long> referencedPartitions) {
    TTableDescriptor desc = new TTableDescriptor(id_.asInt(), TTableType.KUDU_TABLE,
        getColumns().size(), numClusteringCols_, kuduTableName_, db_.getName());
    desc.setKuduTable(getKuduTable());
    desc.setColNames(getColumnNames());
    return desc;
  }

  @Override
  public TCatalogObjectType getCatalogObjectType() { return TCatalogObjectType.TABLE; }

  @Override
  public String getStorageHandlerClassName() { return KUDU_STORAGE_HANDLER; }

  /**
   * Returns the columns in the order they have been created
   */
  @Override
  public ArrayList<Column> getColumnsInHiveOrder() { return getColumns(); }

  public static boolean isKuduTable(org.apache.hadoop.hive.metastore.api.Table mstbl) {
    return TableType.valueOf(mstbl.getTableType()) != TableType.VIRTUAL_VIEW
        && TableType.valueOf(mstbl.getTableType()) != TableType.INDEX_TABLE
        && KUDU_STORAGE_HANDLER.equals(mstbl.getParameters().get(KEY_STORAGE_HANDLER));
  }

  /**
   * Load the columns from the schema list
   */
  private void loadColumns(List<FieldSchema> schema, HiveMetaStoreClient client,
      Set<String> keyColumns)
      throws TableLoadingException {

    if (keyColumns.size() == 0 || keyColumns.size() > schema.size()) {
      throw new TableLoadingException("Kudu tables must have key columns between one and"
          + "the total number of columns in the schema.");
    }

    Set<String> columnNames = new HashSet<>();

    int pos = 0;
    for (FieldSchema field: schema) {
      com.cloudera.impala.catalog.Type type = parseColumnType(field);
      // TODO: Check for decimal types?
      Column col = new Column(field.getName(), type, field.getComment(), pos);
      columnNames.add(col.getName());
      addColumn(col);
      ++pos;
    }

    if (Sets.intersection(columnNames, keyColumns).size() < keyColumns.size()) {
      throw new TableLoadingException(String.format("Some key columns were not found in"
              + " the set of columns. List of column names: %s, List of key column names:"
              + " %s", Iterables.toString(columnNames), Iterables.toString(keyColumns)));
    }

    kuduKeyColumnNames_ = ImmutableList.copyOf(keyColumns);

    loadAllColumnStats(client);
  }

  @Override
  public void load(Table cachedEntry, HiveMetaStoreClient client,
      org.apache.hadoop.hive.metastore.api.Table msTbl) throws TableLoadingException {
    // TODO reuse cachedEntry on load
    if (getMetaStoreTable() == null || !validTable(msTbl.getParameters())) {
      throw new TableLoadingException(String.format(
          "Cannot load Kudu table %s, table is corrupt.", cachedEntry.getFullName()));
    }

    kuduTableName_ = msTbl.getParameters().get(KEY_TABLE_NAME);
    kuduMasters_ = KuduUtil.stringToHostAndPort(msTbl.getParameters().get(
        KEY_MASTER_ADDRESSES));

    Set<String> keyColumns = ImmutableSet.copyOf(Splitter.on(",").trimResults().split(
        Preconditions.checkNotNull(msTbl.getParameters().get(KEY_KEY_COLUMNS),
            "'kudu.key_columns' cannot be null.")));

    // Load the rest of the data from the table parameters directly
    loadColumns(msTbl.getSd().getCols(), client, keyColumns);

    // TODO Revisit, when we allow predicate pushdown
    numClusteringCols_ = 0;

    // Get row count from stats
    numRows_ = getRowCount(getMetaStoreTable().getParameters());
  }

  @Override
  public TTable toThrift() {
    TTable table = super.toThrift();
    table.setTable_type(TTableType.KUDU_TABLE);
    table.setKudu_table(getKuduTable());
    return table;
  }

  @Override
  protected void loadFromThrift(TTable thriftTable) throws TableLoadingException {
    super.loadFromThrift(thriftTable);
    TKuduTable tkudu = thriftTable.getKudu_table();
    kuduTableName_ = tkudu.getTable_name();
    kuduMasters_ = KuduUtil.stringToHostAndPort(tkudu.getMaster_addresses());
    kuduKeyColumnNames_ = tkudu.getKey_columns();
  }

  public String getKuduTableName() { return kuduTableName_; }

  public List<HostAndPort> getKuduMasterAddresses() { return kuduMasters_; }

  /**
   * Returns true if all required parameters are present in the given table properties
   * map.
   */
  public static boolean validTable(Map<String, String> params) {
    return params.get(KEY_TABLE_NAME) != null && params.get(KEY_MASTER_ADDRESSES) != null
        && params.get(KEY_KEY_COLUMNS) != null;
  }

  /**
   * The number of nodes is not know ahead of time and will be updated during computeStats
   * in the scan node.
   */
  @Override
  public int getNumNodes() { return -1; }

}

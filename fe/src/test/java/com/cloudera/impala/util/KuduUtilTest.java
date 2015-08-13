// Copyright (c) 2015 Cloudera, Inc. All rights reserved.

package com.cloudera.impala.util;

import com.cloudera.impala.common.ImpalaRuntimeException;
import com.google.common.collect.ImmutableList;
import org.junit.Test;
import org.kududb.ColumnSchema;
import org.kududb.ColumnSchema.ColumnSchemaBuilder;
import org.kududb.Schema;
import org.kududb.Type;
import org.kududb.client.PartialRow;

import java.util.List;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

public class KuduUtilTest {

  private ColumnSchema newKeyColumn(String name, Type type) {
    ColumnSchemaBuilder csb = new ColumnSchemaBuilder(name, type);
    csb.key(true);
    csb.nullable(false);
    return csb.build();
  }

  // Tests that we're able to parse splits for a single key of 'string' type.
  // TODO: this test doesn't make assertions on the resulting encoded key as that
  // api is not public in Kudu. Maybe we should make it public? It's hard to test
  // otherwise.
  @Test
  public void testParseSplitKeysSingleKeyStringColumn() throws ImpalaRuntimeException {
    String json = "[[\"a\"],[\"b\"]]";
    List<ColumnSchema> keyCols = ImmutableList.of(newKeyColumn("col1", Type.STRING));
    Schema schema = new Schema(keyCols);
    List<PartialRow> splitRows = KuduUtil.parseSplits(schema.getRowKeyProjection(), json);
    assertEquals(splitRows.size(), 2);
  }

  // Tests that we're able to parse splits for a single key of 'bigint' type.
  // TODO: this test doesn't make assertions on the resulting encoded key as that
  // api is not public in Kudu. Maybe we should make it public? It's hard to test
  // otherwise.
  @Test
  public void testParseSplitKeysSingleKeyIntColumn() throws ImpalaRuntimeException {
    String json = "[[10], [100]]";
    List<ColumnSchema> keyCols = ImmutableList.of(newKeyColumn("col1", Type.INT64));
    Schema schema = new Schema(keyCols);
    List<PartialRow> splitRows = KuduUtil.parseSplits(schema.getRowKeyProjection(), json);
    assertEquals(splitRows.size(), 2);
  }

  // Tests that we're able to parse splits for a compound key of 'string', 'bigint' type.
  // TODO: this test doesn't make assertions on the resulting encoded key as that
  // api is not public in Kudu. Maybe we should make it public? It's hard to test
  // otherwise.
  @Test
  public void testParseSplitKeysCompoundStringAndIntColumns()
      throws ImpalaRuntimeException {
    String json = "[[\"a\", 100], [\"b\", 1000]]";
    List<ColumnSchema> keyCols = ImmutableList.of(newKeyColumn("col1", Type.STRING),
        newKeyColumn("col2", Type.INT64));
    Schema schema = new Schema(keyCols);
    List<PartialRow> splitRows = KuduUtil.parseSplits(schema.getRowKeyProjection(), json);
    assertEquals(splitRows.size(), 2);
  }

  // Tests that we get an exception of the splits keys are of the wrong type, for a single
  // key column.
  @Test
  public void testFailToParseKeysWrongType() {
    String json = "[[\"a\"]]";
    List<ColumnSchema> keyCols = ImmutableList.of(newKeyColumn("col1", Type.INT64),
        newKeyColumn("col2", Type.INT64));
    Schema schema = new Schema(keyCols);
    Exception caught = null;
    try {
      KuduUtil.parseSplits(schema.getRowKeyProjection(), json);
    } catch (Exception e) {
      caught = e;
    }
    assertNotNull(caught);
    assertTrue(caught instanceof ImpalaRuntimeException);
  }

  // Tests that we get an exception of the splits keys have the wrong number of
  // keys.
  @Test
  public void testFailToParseKeysWrongNumber() {
    String json = "[[\"a\", 10]]";
    List<ColumnSchema> keyCols = ImmutableList.of(newKeyColumn("col1", Type.INT64),
        newKeyColumn("col2", Type.INT64));
    Schema schema = new Schema(keyCols);
    Exception caught = null;
    try {
      KuduUtil.parseSplits(schema.getRowKeyProjection(), json);
    } catch (Exception e) {
      caught = e;
    }
    assertNotNull(caught);
    assertTrue(caught instanceof ImpalaRuntimeException);
  }
}

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

package com.cloudera.impala.analysis;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import com.cloudera.impala.catalog.ColumnStats;
import com.cloudera.impala.catalog.Table;
import com.cloudera.impala.thrift.TTupleDescriptor;
import com.google.common.base.Joiner;
import com.google.common.base.Objects;
import com.google.common.base.Preconditions;
import com.google.common.collect.Lists;

public class TupleDescriptor {
  private final TupleId id_;
  private final String debugName_;  // debug-only
  private final ArrayList<SlotDescriptor> slots_;

  // Underlying table, if there is one.
  private Table table_;

  // Explicit or fully-qualified implicit alias.
  private String alias_;

  // True if alias_ is an explicit alias.
  private boolean hasExplicitAlias_;

  // if false, this tuple doesn't need to be materialized
  private boolean isMaterialized_ = true;

  private int byteSize_;  // of all slots plus null indicators
  private int numNullBytes_;

  // if true, computeMemLayout() has been called and we can't add any additional
  // slots
  private boolean hasMemLayout_ = false;

  private float avgSerializedSize_;  // in bytes; includes serialization overhead

  TupleDescriptor(TupleId id, String debugName) {
    id_ = id;
    debugName_ = debugName;
    slots_ = new ArrayList<SlotDescriptor>();
  }

  public void addSlot(SlotDescriptor desc) {
    Preconditions.checkState(!hasMemLayout_);
    slots_.add(desc);
  }

  public TupleId getId() { return id_; }
  public ArrayList<SlotDescriptor> getSlots() { return slots_; }
  public Table getTable() { return table_; }
  public TableName getTableName() {
    if (table_ == null) return null;
    return new TableName(
        table_.getDb() != null ? table_.getDb().getName() : null, table_.getName());
  }
  public void setTable(Table tbl) { table_ = tbl; }
  public int getByteSize() { return byteSize_; }
  public float getAvgSerializedSize() { return avgSerializedSize_; }
  public boolean isMaterialized() { return isMaterialized_; }
  public void setIsMaterialized(boolean value) { isMaterialized_ = value; }
  public String getAlias() { return alias_; }
  public boolean hasExplicitAlias() { return hasExplicitAlias_; }
  public void setAlias(String alias, boolean isExplicit) {
    alias_ = alias;
    hasExplicitAlias_ = isExplicit;
  }

  public TableName getAliasAsName() {
    if (hasExplicitAlias_) return new TableName(null, alias_);
    return getTableName();
  }

  public String debugString() {
    String tblStr = (table_ == null ? "null" : table_.getFullName());
    List<String> slotStrings = Lists.newArrayList();
    for (SlotDescriptor slot : slots_) {
      slotStrings.add(slot.debugString());
    }
    return Objects.toStringHelper(this)
        .add("id", id_.asInt())
        .add("name", debugName_)
        .add("tbl", tblStr)
        .add("byte_size", byteSize_)
        .add("is_materialized", isMaterialized_)
        .add("slots", "[" + Joiner.on(", ").join(slotStrings) + "]")
        .toString();
  }

  /**
   * Materialize all slots.
   */
  public void materializeSlots() {
    for (SlotDescriptor slot: slots_) {
      slot.setIsMaterialized(true);
    }
  }

  public TTupleDescriptor toThrift() {
    TTupleDescriptor ttupleDesc =
        new TTupleDescriptor(id_.asInt(), byteSize_, numNullBytes_);
    // do not set the table id for virtual tables such as views and inline views
    if (table_ != null && !table_.isVirtualTable()) {
      ttupleDesc.setTableId(table_.getId().asInt());
    }
    return ttupleDesc;
  }

  public void computeMemLayout() {
    if (hasMemLayout_) return;
    hasMemLayout_ = true;

    // sort slots by size
    Map<Integer, List<SlotDescriptor>> slotsBySize =
        new HashMap<Integer, List<SlotDescriptor>>();

    // populate slotsBySize; also compute avgSerializedSize
    int numNullableSlots = 0;
    for (SlotDescriptor d: slots_) {
      if (!d.isMaterialized()) continue;
      ColumnStats stats = d.getStats();
      if (stats.hasAvgSerializedSize()) {
        avgSerializedSize_ += d.getStats().getAvgSerializedSize();
      } else {
        // TODO: for computed slots, try to come up with stats estimates
        avgSerializedSize_ += d.getType().getSlotSize();
      }
      if (!slotsBySize.containsKey(d.getType().getSlotSize())) {
        slotsBySize.put(d.getType().getSlotSize(), new ArrayList<SlotDescriptor>());
      }
      slotsBySize.get(d.getType().getSlotSize()).add(d);
      if (d.getIsNullable()) ++numNullableSlots;
    }
    // we shouldn't have anything of size <= 0
    Preconditions.checkState(!slotsBySize.containsKey(0));
    Preconditions.checkState(!slotsBySize.containsKey(-1));

    // assign offsets to slots in order of ascending size
    numNullBytes_ = (numNullableSlots + 7) / 8;
    int offset = numNullBytes_;
    int nullIndicatorByte = 0;
    int nullIndicatorBit = 0;
    // slotIdx is the index into the resulting tuple struct.  The first (smallest) field
    // is 0, next is 1, etc.
    int slotIdx = 0;
    List<Integer> sortedSizes = new ArrayList<Integer>(slotsBySize.keySet());
    Collections.sort(sortedSizes);
    for (int slotSize: sortedSizes) {
      if (slotsBySize.get(slotSize).isEmpty()) continue;
      if (slotSize > 1) {
        // insert padding
        int alignTo = Math.min(slotSize, 8);
        offset = (offset + alignTo - 1) / alignTo * alignTo;
      }

      for (SlotDescriptor d: slotsBySize.get(slotSize)) {
        Preconditions.checkState(d.isMaterialized());
        d.setByteSize(slotSize);
        d.setByteOffset(offset);
        d.setSlotIdx(slotIdx++);
        offset += slotSize;

        // assign null indicator
        if (d.getIsNullable()) {
          d.setNullIndicatorByte(nullIndicatorByte);
          d.setNullIndicatorBit(nullIndicatorBit);
          nullIndicatorBit = (nullIndicatorBit + 1) % 8;
          if (nullIndicatorBit == 0) ++nullIndicatorByte;
        } else {
          // Non-nullable slots will have 0 for the byte offset and -1 for the bit mask
          d.setNullIndicatorBit(-1);
          d.setNullIndicatorByte(0);
        }
      }
    }

    this.byteSize_ = offset;
  }

  /**
   * Returns true if tuples of type 'this' can be assigned to tuples of type 'desc'
   * (checks that both have the same number of slots and that slots are of the same type)
   */
  public boolean isCompatible(TupleDescriptor desc) {
    if (slots_.size() != desc.slots_.size()) return false;
    for (int i = 0; i < slots_.size(); ++i) {
      if (!slots_.get(i).getType().equals(desc.slots_.get(i).getType())) return false;
    }
    return true;
  }
}

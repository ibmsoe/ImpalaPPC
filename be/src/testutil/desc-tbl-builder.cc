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

#include "testutil/desc-tbl-builder.h"
#include "util/bit-util.h"


#include "runtime/descriptors.h"

#include "common/names.h"

namespace impala {

DescriptorTblBuilder::DescriptorTblBuilder(ObjectPool* obj_pool) : obj_pool_(obj_pool) {
}

TupleDescBuilder& DescriptorTblBuilder::DeclareTuple() {
  TupleDescBuilder* tuple_builder = obj_pool_->Add(new TupleDescBuilder());
  tuples_descs_.push_back(tuple_builder);
  return *tuple_builder;
}

static TSlotDescriptor MakeSlotDescriptor(int id, int parent_id,
    const TupleDescBuilder::Slot& slot, int slot_idx, int byte_offset) {
  int null_byte = slot_idx / 8;
  int null_bit = slot_idx % 8;
  TSlotDescriptor slot_desc;
  slot_desc.__set_id(id);
  slot_desc.__set_parent(parent_id);
  slot_desc.__set_slotType(slot.slot_type.ToThrift());
  slot_desc.__set_columnPath(vector<int>(1, slot_idx));
  slot_desc.__set_byteOffset(byte_offset);
  slot_desc.__set_nullIndicatorByte(null_byte);
  slot_desc.__set_nullIndicatorBit(null_bit);
  slot_desc.__set_slotIdx(slot_idx);
  slot_desc.__set_isMaterialized(slot.materialized);
  return slot_desc;
}

static TTupleDescriptor MakeTupleDescriptor(int id, int byte_size, int num_null_bytes,
    int table_id = -1) {
  TTupleDescriptor tuple_desc;
  tuple_desc.__set_id(id);
  tuple_desc.__set_byteSize(byte_size);
  tuple_desc.__set_numNullBytes(num_null_bytes);
  if (table_id != -1) tuple_desc.__set_tableId(table_id);
  return tuple_desc;
}

void DescriptorTblBuilder::SetTableDescriptor(const TTableDescriptor& table_desc) {
  DCHECK(thrift_desc_tbl_.tableDescriptors.empty())
      << "Only one TableDescriptor can be set.";
  thrift_desc_tbl_.tableDescriptors.push_back(table_desc);
}

DescriptorTbl* DescriptorTblBuilder::Build() {
  DescriptorTbl* desc_tbl;
  int slot_id = tuples_descs_.size(); // First ids reserved for TupleDescriptors

  for (int i = 0; i < tuples_descs_.size(); ++i) {
    vector<TupleDescBuilder::Slot> slots = tuples_descs_[i]->slots();
    int num_null_bytes = BitUtil::Ceil(slots.size(), 8);
    int byte_offset = num_null_bytes;
    int tuple_id = i;

    for(int j = 0; j < slots.size(); ++j) {
      thrift_desc_tbl_.slotDescriptors.push_back(
          MakeSlotDescriptor(++slot_id, tuple_id, slots[j], j, byte_offset));

      int byte_size = slots[j].slot_type.GetByteSize();
      if (byte_size == 0) {
        // can only handle strings right now
        DCHECK(slots[j].slot_type.type == TYPE_STRING
               || slots[j].slot_type.type == TYPE_VARCHAR);
        byte_size = 16;
      }
      byte_offset += byte_size;
    }

    // If someone set a table descriptor pass that id along to the tuple descriptor.
    if (thrift_desc_tbl_.tableDescriptors.empty()) {
      thrift_desc_tbl_.tupleDescriptors.push_back(
          MakeTupleDescriptor(tuple_id, byte_offset, num_null_bytes));
    } else {
      thrift_desc_tbl_.tupleDescriptors.push_back(
          MakeTupleDescriptor(tuple_id, byte_offset, num_null_bytes,
              thrift_desc_tbl_.tableDescriptors[0].id));
    }
  }

  Status status = DescriptorTbl::Create(obj_pool_, thrift_desc_tbl_, &desc_tbl);
  DCHECK(status.ok());
  return desc_tbl;
}

}

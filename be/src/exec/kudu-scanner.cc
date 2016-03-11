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

#ifdef USE_KUDU

#include "exec/kudu-scanner.h"

#include <boost/foreach.hpp>
#include <kudu/client/row_result.h>
#include <thrift/protocol/TDebugProtocol.h>
#include <vector>

#include "exprs/expr.h"
#include "exprs/expr-context.h"
#include "exec/kudu-util.h"
#include "runtime/mem-pool.h"
#include "runtime/runtime-state.h"
#include "runtime/row-batch.h"
#include "runtime/string-value.h"
#include "runtime/tuple-row.h"
#include "gutil/gscoped_ptr.h"
#include "gutil/strings/substitute.h"
#include "util/jni-util.h"
#include "util/periodic-counter-updater.h"
#include "util/runtime-profile.h"

#include "common/names.h"

using kudu::client::KuduClient;
using kudu::client::KuduColumnSchema;
using kudu::client::KuduPredicate;
using kudu::client::KuduScanBatch;
using kudu::client::KuduSchema;
using kudu::client::KuduTable;
using strings::Substitute;

DEFINE_bool(pick_only_leaders_for_tests, false,
            "Whether to pick only leader replicas, for tests purposes only.");
DEFINE_int32(kudu_scanner_keep_alive_period_sec, 15,
    "The period at which Kudu Scanners should send keep-alive requests to the tablet "
    "server to ensure that scanners do not time out.");

namespace impala {

namespace {

// Sets up the scan range predicate on the scanner, i.e. the partition start/stop keys
// of the partition the scanner is supposed to scan.
Status SetupScanRangePredicate(const TKuduKeyRange& key_range,
    kudu::client::KuduScanner* scanner) {
  if (key_range.range_start_key.empty() && key_range.range_stop_key.empty()) {
    return Status::OK();
  }

  if (!key_range.range_start_key.empty()) {
    KUDU_RETURN_IF_ERROR(scanner->AddLowerBoundPartitionKeyRaw(
            key_range.range_start_key), "adding scan range lower bound");
  }
  if (!key_range.range_stop_key.empty()) {
    KUDU_RETURN_IF_ERROR(scanner->AddExclusiveUpperBoundPartitionKeyRaw(
            key_range.range_stop_key), "adding scan range upper bound");
  }
  return Status::OK();
}

} // anonymous namespace

KuduScanner::KuduScanner(KuduScanNode* scan_node, RuntimeState* state)
  : scan_node_(scan_node),
    state_(state),
    rows_scanned_current_block_(0),
    last_alive_time_micros_(0) {
}

Status KuduScanner::Open() {
  tuple_byte_size_ = scan_node_->tuple_desc()->byte_size();
  tuple_num_null_bytes_ = scan_node_->tuple_desc()->num_null_bytes();

  // Store columns that need relocation when materialized into the
  // destination row batch.
  for (int i = 0; i < scan_node_->tuple_desc_->slots().size(); ++i) {
    if (scan_node_->tuple_desc_->slots()[i]->type().type == TYPE_STRING) {
      string_slots_.push_back(scan_node_->tuple_desc_->slots()[i]);
    }
  }
  return scan_node_->GetConjunctCtxs(&conjunct_ctxs_);
}

Status KuduScanner::KeepKuduScannerAlive() {
  if (scanner_ == NULL) return Status::OK();
  int64_t now = MonotonicMicros();
  int64_t keepalive_us = FLAGS_kudu_scanner_keep_alive_period_sec * 1e6;
  if (now < last_alive_time_micros_ + keepalive_us) {
    return Status::OK();
  }
  KUDU_RETURN_IF_ERROR(scanner_->KeepAlive(), "Unable to keep the "
      "Kudu scanner alive.");
  last_alive_time_micros_ = now;
  return Status::OK();
}

Status KuduScanner::GetNext(RowBatch* row_batch, bool* eos) {
  int tuple_buffer_size = row_batch->capacity() * tuple_byte_size_;
  void* tuple_buffer = row_batch->tuple_data_pool()->TryAllocate(tuple_buffer_size);
  if (tuple_buffer_size > 0 && tuple_buffer == NULL) {
    Status s = Status::MemLimitExceeded();
    s.AddDetail("Could not allocate memory for a tuple buffer.");
    return s;
  }
  Tuple* tuple = reinterpret_cast<Tuple*>(tuple_buffer);

  // Main scan loop:
  // Tries to fill 'row_batch' with rows from the last fetched block.
  // If there are no rows to decode tries to get the next block from kudu.
  // If there are no more blocks in the current range tries to get the next range.
  // If there aren't any more rows, blocks or ranges then we're done.
  while(true) {
    RETURN_IF_CANCELLED(state_);
    // If the last fetched block has more rows, decode and if we filled up the batch
    // return.
    if (CurrentBlockHasMoreRows()) {
      bool batch_done = false;
      RETURN_IF_ERROR(DecodeRowsIntoRowBatch(row_batch, &tuple, &batch_done));
      if (batch_done) return Status::OK();
    }
    // If the current scanner has more blocks, fetch them.
    if (CurrentRangeHasMoreBlocks()) {
      RETURN_IF_ERROR(GetNextBlock());
      continue;
    }
    // No more blocks in the current scanner, close it.
    CloseCurrentRange();
    // No more rows or blocks, we're done.
    *eos = true;
    return Status::OK();
  }
  return Status::OK();
}

void KuduScanner::Close() {
  if (scanner_) CloseCurrentRange();
  Expr::Close(conjunct_ctxs_, state_);
}

Status KuduScanner::OpenNextRange(const TKuduKeyRange& key_range)  {
  DCHECK(scanner_ == NULL);
  scanner_.reset(new kudu::client::KuduScanner(scan_node_->kudu_table()));
  KUDU_RETURN_IF_ERROR(scanner_->SetProjectedColumns(scan_node_->projected_columns()),
      "Unable to set projected columns");

  RETURN_IF_ERROR(SetupScanRangePredicate(key_range, scanner_.get()));

  vector<KuduPredicate*> predicates;
  scan_node_->ClonePredicates(&predicates);
  BOOST_FOREACH(KuduPredicate* predicate, predicates) {
    KUDU_RETURN_IF_ERROR(scanner_->AddConjunctPredicate(predicate),
                         "Unable to add conjunct predicate.");
  }

  if (UNLIKELY(FLAGS_pick_only_leaders_for_tests)) {
    KUDU_RETURN_IF_ERROR(scanner_->SetSelection(kudu::client::KuduClient::LEADER_ONLY),
                         "Could not set replica selection.");
  }

  {
    SCOPED_TIMER(scan_node_->kudu_read_timer());
    KUDU_RETURN_IF_ERROR(scanner_->Open(), "Unable to open scanner");
  }
  return Status::OK();
}

void KuduScanner::CloseCurrentRange() {
  DCHECK_NOTNULL(scanner_.get());
  scanner_->Close();
  scanner_.reset();
  ExprContext::FreeLocalAllocations(conjunct_ctxs_);
}

Status KuduScanner::HandleEmptyProjection(RowBatch* row_batch, bool* batch_done) {
  int rem_in_block = cur_kudu_batch_.NumRows() - rows_scanned_current_block_;
  int rows_to_add = std::min(row_batch->capacity() - row_batch->num_rows(),
      rem_in_block);
  rows_scanned_current_block_ += rows_to_add;
  row_batch->CommitRows(rows_to_add);
  // If we've reached the capacity, or the LIMIT for the scan, return.
  if (row_batch->AtCapacity() || scan_node_->ReachedLimit()) {
    *batch_done = true;
  }
  return Status::OK();
}

Status KuduScanner::DecodeRowsIntoRowBatch(RowBatch* row_batch,
    Tuple** tuple_mem, bool* batch_done) {

  // Short-circuit the count(*) case.
  if (scan_node_->tuple_desc_->slots().empty()) return HandleEmptyProjection(row_batch, batch_done);

  // TODO consider consolidating the tuple creation/initialization here with the version that
  // happens inside the loop.
  int idx = row_batch->AddRow();
  TupleRow* row = row_batch->GetRow(idx);
  (*tuple_mem)->Init(scan_node_->tuple_desc()->num_null_bytes());
  row->SetTuple(tuple_idx(), *tuple_mem);

  int num_rows = cur_kudu_batch_.NumRows();
  // Now iterate through the Kudu rows.
  for (int krow_idx = rows_scanned_current_block_; krow_idx < num_rows; ++krow_idx) {
    // Clear any NULL indicators set by a previous iteration.
    (*tuple_mem)->Init(tuple_num_null_bytes_);

    // Transform a Kudu row into an Impala row.
    KuduScanBatch::RowPtr krow = cur_kudu_batch_.Row(krow_idx);
    RETURN_IF_ERROR(KuduRowToImpalaTuple(krow, row_batch, *tuple_mem));
    ++rows_scanned_current_block_;

    // Evaluate the conjuncts that haven't been pushed down to Kudu.
    if (conjunct_ctxs_.empty() ||
        ExecNode::EvalConjuncts(&conjunct_ctxs_[0], conjunct_ctxs_.size(), row)) {
      // Materialize those slots that require auxiliary memory
      RETURN_IF_ERROR(RelocateValuesFromKudu(*tuple_mem, row_batch->tuple_data_pool()));
      // If the conjuncts pass on the row commit it.
      row_batch->CommitLastRow();
      // If we've reached the capacity, or the LIMIT for the scan, return.
      if (row_batch->AtCapacity() || scan_node_->ReachedLimit()) {
        *batch_done = true;
        break;
      }
      // Add another row.
      idx = row_batch->AddRow();

      // Move to the next tuple in the tuple buffer.
      *tuple_mem = next_tuple(*tuple_mem);
      (*tuple_mem)->Init(tuple_num_null_bytes_);
      // Make 'row' point to the new row.
      row = row_batch->GetRow(idx);
      row->SetTuple(tuple_idx(), *tuple_mem);
    }
  }
  return Status::OK();
}

void KuduScanner::SetSlotToNull(Tuple* tuple, const SlotDescriptor& slot) {
  DCHECK(slot.is_nullable());
  tuple->SetNull(slot.null_indicator_offset());
}

bool KuduScanner::IsSlotNull(Tuple* tuple, const SlotDescriptor& slot) {
  return slot.is_nullable() && tuple->IsNull(slot.null_indicator_offset());
}

Status KuduScanner::RelocateValuesFromKudu(Tuple* tuple, MemPool* mem_pool) {
  for (int i = 0; i < string_slots_.size(); ++i) {
    const SlotDescriptor* slot = string_slots_[i];
    // NULL handling was done in KuduRowToImpalaTuple.
    if (IsSlotNull(tuple, *slot)) continue;

    // Extract the string value.
    void* slot_ptr = tuple->GetSlot(slot->tuple_offset());
    DCHECK(slot->type().IsStringType());

    // The string value of the slot has a pointer to memory from the Kudu row.
    StringValue* val = reinterpret_cast<StringValue*>(slot_ptr);
    char* old_buf = val->ptr;
    // Kudu never returns values larger than 8MB
    DCHECK_LE(val->len, 8 * (1 << 20));
    val->ptr = reinterpret_cast<char*>(mem_pool->TryAllocate(val->len));
    if (UNLIKELY(val->ptr == NULL)) {
      if (UNLIKELY(val->len > 0)) {
        Status s = Status::MemLimitExceeded();
        s.AddDetail("Could not allocate memory for string.");
      }
    } else {
      DCHECK(val->len > 0);
      memcpy(val->ptr, old_buf, val->len);
    }
  }
  return Status::OK();
}


Status KuduScanner::KuduRowToImpalaTuple(const KuduScanBatch::RowPtr& row,
    RowBatch* row_batch, Tuple* tuple) {
  for (int i = 0; i < scan_node_->tuple_desc_->slots().size(); ++i) {
    const SlotDescriptor* info = scan_node_->tuple_desc_->slots()[i];
    void* slot = tuple->GetSlot(info->tuple_offset());

    if (row.IsNull(i)) {
      SetSlotToNull(tuple, *info);
      continue;
    }

    int max_len = -1;
    switch (info->type().type) {
      case TYPE_VARCHAR:
        max_len = info->type().len;
        DCHECK_GT(max_len, 0);
        // Fallthrough intended.
      case TYPE_STRING: {
        // For types with auxiliary memory (String, Binary,...) store the original memory
        // location in the tuple to avoid the copy when the conjuncts do not pass. Relocate
        // the memory into the row batch's memory in a later step.
        kudu::Slice slice;
        KUDU_RETURN_IF_ERROR(row.GetString(i, &slice),
            "Error getting column value from Kudu.");
        StringValue* sv = reinterpret_cast<StringValue*>(slot);
        sv->ptr = const_cast<char*>(reinterpret_cast<const char*>(slice.data()));
        sv->len = static_cast<int>(slice.size());
        if (max_len > 0) sv->len = std::min(sv->len, max_len);
        break;
      }
      case TYPE_TINYINT:
        KUDU_RETURN_IF_ERROR(row.GetInt8(i, reinterpret_cast<int8_t*>(slot)),
            "Error getting column value from Kudu.");
        break;
      case TYPE_SMALLINT:
        KUDU_RETURN_IF_ERROR(row.GetInt16(i, reinterpret_cast<int16_t*>(slot)),
            "Error getting column value from Kudu.");
        break;
      case TYPE_INT:
        KUDU_RETURN_IF_ERROR(row.GetInt32(i, reinterpret_cast<int32_t*>(slot)),
            "Error getting column value from Kudu.");
        break;
      case TYPE_BIGINT:
        KUDU_RETURN_IF_ERROR(row.GetInt64(i, reinterpret_cast<int64_t*>(slot)),
            "Error getting column value from Kudu.");
        break;
      case TYPE_FLOAT:
        KUDU_RETURN_IF_ERROR(row.GetFloat(i, reinterpret_cast<float*>(slot)),
            "Error getting column value from Kudu.");
        break;
      case TYPE_DOUBLE:
        KUDU_RETURN_IF_ERROR(row.GetDouble(i, reinterpret_cast<double*>(slot)),
            "Error getting column value from Kudu.");
        break;
      case TYPE_BOOLEAN:
        KUDU_RETURN_IF_ERROR(row.GetBool(i, reinterpret_cast<bool*>(slot)),
            "Error getting column value from Kudu.");
        break;
      default:
        DCHECK(false) << "Impala type unsupported in Kudu: "
            << TypeToString(info->type().type);
        return Status(TErrorCode::IMPALA_KUDU_TYPE_MISSING,
            TypeToString(info->type().type));
    }
  }
  return Status::OK();
}


Status KuduScanner::GetNextBlock() {
  SCOPED_TIMER(scan_node_->kudu_read_timer());
  int64_t now = MonotonicMicros();
  KUDU_RETURN_IF_ERROR(scanner_->NextBatch(&cur_kudu_batch_), "Unable to advance iterator");
  COUNTER_ADD(scan_node_->kudu_round_trips(), 1);
  rows_scanned_current_block_ = 0;
  COUNTER_ADD(scan_node_->rows_read_counter(), cur_kudu_batch_.NumRows());
  last_alive_time_micros_ = now;
  return Status::OK();
}

}  // namespace impala

#endif

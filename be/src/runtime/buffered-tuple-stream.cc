// Copyright 2013 Cloudera Inc.
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

#include "runtime/buffered-tuple-stream.inline.h"

#include <boost/bind.hpp>
#include <gutil/strings/substitute.h>

#include "runtime/collection-value.h"
#include "runtime/descriptors.h"
#include "runtime/row-batch.h"
#include "runtime/string-value.h"
#include "runtime/tuple-row.h"
#include "util/bit-util.h"
#include "util/debug-util.h"

#include "common/names.h"

using namespace impala;
using namespace strings;

// The first NUM_SMALL_BLOCKS of the tuple stream are made of blocks less than the
// IO size. These blocks never spill.
// TODO: Consider adding a 4MB in-memory buffer that would split the gap between the
// 512KB in-memory buffer and the 8MB (IO-sized) spillable buffer.
static const int64_t INITIAL_BLOCK_SIZES[] = { 64 * 1024, 512 * 1024 };
static const int NUM_SMALL_BLOCKS = sizeof(INITIAL_BLOCK_SIZES) / sizeof(int64_t);

string BufferedTupleStream::RowIdx::DebugString() const {
  stringstream ss;
  ss << "RowIdx block=" << block() << " offset=" << offset() << " idx=" << idx();
  return ss.str();
}

BufferedTupleStream::BufferedTupleStream(RuntimeState* state,
    const RowDescriptor& row_desc, BufferedBlockMgr* block_mgr,
    BufferedBlockMgr::Client* client, bool use_initial_small_buffers, bool read_write,
    const set<SlotId>& ext_varlen_slots)
  : use_small_buffers_(use_initial_small_buffers),
    delete_on_read_(false),
    read_write_(read_write),
    state_(state),
    desc_(row_desc),
    has_nullable_tuple_(row_desc.IsAnyTupleNullable()),
    block_mgr_(block_mgr),
    block_mgr_client_(client),
    total_byte_size_(0),
    read_tuple_idx_(-1),
    read_ptr_(NULL),
    read_end_ptr_(NULL),
    write_tuple_idx_(-1),
    write_ptr_(NULL),
    write_end_ptr_(NULL),
    rows_returned_(0),
    read_block_idx_(-1),
    write_block_(NULL),
    num_pinned_(0),
    num_small_blocks_(0),
    closed_(false),
    num_rows_(0),
    pinned_(true),
    pin_timer_(NULL),
    unpin_timer_(NULL),
    get_new_block_timer_(NULL) {
  read_block_null_indicators_size_ = -1;
  write_block_null_indicators_size_ = -1;
  max_null_indicators_size_ = -1;
  read_block_ = blocks_.end();
  fixed_tuple_row_size_ = 0;
  for (int i = 0; i < desc_.tuple_descriptors().size(); ++i) {
    const TupleDescriptor* tuple_desc = desc_.tuple_descriptors()[i];
    const int tuple_byte_size = tuple_desc->byte_size();
    fixed_tuple_sizes_.push_back(tuple_byte_size);
    fixed_tuple_row_size_ += tuple_byte_size;

    vector<SlotDescriptor*> tuple_string_slots;
    vector<SlotDescriptor*> tuple_coll_slots;
    for (int j = 0; j < tuple_desc->slots().size(); ++j) {
      SlotDescriptor* slot = tuple_desc->slots()[j];
      if (!slot->type().IsVarLenType()) continue;
      if (ext_varlen_slots.find(slot->id()) == ext_varlen_slots.end()) {
        if (slot->type().IsVarLenStringType()) {
          tuple_string_slots.push_back(slot);
        } else {
          DCHECK(slot->type().IsCollectionType());
          tuple_coll_slots.push_back(slot);
        }
      }
    }
    if (!tuple_string_slots.empty()) {
      inlined_string_slots_.push_back(make_pair(i, tuple_string_slots));
    }

    if (!tuple_coll_slots.empty()) {
      inlined_coll_slots_.push_back(make_pair(i, tuple_coll_slots));
    }
  }
}

BufferedTupleStream::~BufferedTupleStream() {
  DCHECK(closed_);
}

// Returns the number of pinned blocks in the list. Only called in DCHECKs to validate
// num_pinned_.
int NumPinned(const list<BufferedBlockMgr::Block*>& blocks) {
  int num_pinned = 0;
  for (list<BufferedBlockMgr::Block*>::const_iterator it = blocks.begin();
      it != blocks.end(); ++it) {
    if ((*it)->is_pinned() && (*it)->is_max_size()) ++num_pinned;
  }
  return num_pinned;
}

string BufferedTupleStream::DebugString() const {
  stringstream ss;
  ss << "BufferedTupleStream num_rows=" << num_rows_ << " rows_returned="
     << rows_returned_ << " pinned=" << (pinned_ ? "true" : "false")
     << " delete_on_read=" << (delete_on_read_ ? "true" : "false")
     << " closed=" << (closed_ ? "true" : "false")
     << " num_pinned=" << num_pinned_
     << " write_block=" << write_block_ << " read_block_=";
  if (read_block_ == blocks_.end()) {
    ss << "<end>";
  } else {
    ss << *read_block_;
  }
  ss << " blocks=[\n";
  for (list<BufferedBlockMgr::Block*>::const_iterator it = blocks_.begin();
      it != blocks_.end(); ++it) {
    ss << "{" << (*it)->DebugString() << "}";
    if (*it != blocks_.back()) ss << ",\n";
  }
  ss << "]";
  return ss.str();
}

Status BufferedTupleStream::Init(int node_id, RuntimeProfile* profile, bool pinned) {
  if (profile != NULL) {
    pin_timer_ = ADD_TIMER(profile, "PinTime");
    unpin_timer_ = ADD_TIMER(profile, "UnpinTime");
    get_new_block_timer_ = ADD_TIMER(profile, "GetNewBlockTime");
  }

  max_null_indicators_size_ = ComputeNumNullIndicatorBytes(block_mgr_->max_block_size());
  if (UNLIKELY(max_null_indicators_size_ < 0)) {
    // The block cannot even fit in a row of tuples so just assume there is one row.
    int null_indicators_size = BitUtil::RoundUpNumi64(desc_.tuple_descriptors().size()) * 8;
    return Status(TErrorCode::BTS_BLOCK_OVERFLOW,
        PrettyPrinter::Print(fixed_tuple_row_size_, TUnit::BYTES),
        PrettyPrinter::Print(null_indicators_size,  TUnit::BYTES));
  }

  if (block_mgr_->max_block_size() < INITIAL_BLOCK_SIZES[0]) {
    use_small_buffers_ = false;
  }

  bool got_block;
  RETURN_IF_ERROR(NewWriteBlockForRow(fixed_tuple_row_size_, &got_block));
  if (!got_block) return block_mgr_->MemLimitTooLowError(block_mgr_client_, node_id);
  DCHECK(write_block_ != NULL);
  if (!pinned) RETURN_IF_ERROR(UnpinStream());
  return Status::OK();
}

Status BufferedTupleStream::SwitchToIoBuffers(bool* got_buffer) {
  if (!use_small_buffers_) {
    *got_buffer = (write_block_ != NULL);
    return Status::OK();
  }
  use_small_buffers_ = false;
  Status status =
      NewWriteBlock(block_mgr_->max_block_size(), max_null_indicators_size_, got_buffer);
  // IMPALA-2330: Set the flag using small buffers back to false in case it failed to
  // got a buffer.
  DCHECK(status.ok() || !*got_buffer) << status.ok() << " " << *got_buffer;
  use_small_buffers_ = !*got_buffer;
  return status;
}

void BufferedTupleStream::Close() {
  for (list<BufferedBlockMgr::Block*>::iterator it = blocks_.begin();
      it != blocks_.end(); ++it) {
    (*it)->Delete();
  }
  blocks_.clear();
  num_pinned_ = 0;
  DCHECK_EQ(num_pinned_, NumPinned(blocks_));
  closed_ = true;
}

int64_t BufferedTupleStream::bytes_in_mem(bool ignore_current) const {
  int64_t result = 0;
  for (list<BufferedBlockMgr::Block*>::const_iterator it = blocks_.begin();
      it != blocks_.end(); ++it) {
    if (!(*it)->is_pinned()) continue;
    if (!(*it)->is_max_size()) continue;
    if (*it == write_block_ && ignore_current) continue;
    result += (*it)->buffer_len();
  }
  return result;
}

Status BufferedTupleStream::UnpinBlock(BufferedBlockMgr::Block* block) {
  SCOPED_TIMER(unpin_timer_);
  DCHECK(block->is_pinned());
  if (!block->is_max_size()) return Status::OK();
  RETURN_IF_ERROR(block->Unpin());
  --num_pinned_;
  DCHECK_EQ(num_pinned_, NumPinned(blocks_));
  return Status::OK();
}

Status BufferedTupleStream::NewWriteBlock(int64_t block_len, int64_t null_indicators_size,
    bool* got_block) {
  DCHECK(!closed_);
  DCHECK_GE(null_indicators_size, 0);
  *got_block = false;

  BufferedBlockMgr::Block* unpin_block = write_block_;
  if (write_block_ != NULL) {
    DCHECK(write_block_->is_pinned());
    if (pinned_ || write_block_ == *read_block_ || !write_block_->is_max_size()) {
      // In these cases, don't unpin the current write block.
      unpin_block = NULL;
    }
  }

  BufferedBlockMgr::Block* new_block = NULL;
  {
    SCOPED_TIMER(get_new_block_timer_);
    RETURN_IF_ERROR(block_mgr_->GetNewBlock(
        block_mgr_client_, unpin_block, &new_block, block_len));
  }
  *got_block = new_block != NULL;

  if (!*got_block) {
    DCHECK(unpin_block == NULL);
    return Status::OK();
  }

  if (unpin_block != NULL) {
    DCHECK(unpin_block == write_block_);
    DCHECK(!write_block_->is_pinned());
    --num_pinned_;
    DCHECK_EQ(num_pinned_, NumPinned(blocks_));
  }

  // Mark the entire block as containing valid data to avoid updating it as we go.
  new_block->Allocate<uint8_t>(block_len);

  // Compute and allocate the block header with the null indicators.
  DCHECK_EQ(null_indicators_size, ComputeNumNullIndicatorBytes(block_len));
  write_block_null_indicators_size_ = null_indicators_size;
  write_tuple_idx_ = 0;
  write_ptr_ = new_block->buffer() + write_block_null_indicators_size_;
  write_end_ptr_ = new_block->buffer() + block_len;

  blocks_.push_back(new_block);
  block_start_idx_.push_back(new_block->buffer());
  write_block_ = new_block;
  DCHECK(write_block_->is_pinned());
  DCHECK_EQ(write_block_->num_rows(), 0);
  if (write_block_->is_max_size()) {
    ++num_pinned_;
    DCHECK_EQ(num_pinned_, NumPinned(blocks_));
  } else {
    ++num_small_blocks_;
  }
  total_byte_size_ += block_len;
  return Status::OK();
}

Status BufferedTupleStream::NewWriteBlockForRow(int64_t row_size, bool* got_block) {
  int64_t block_len;
  int64_t null_indicators_size;
  if (use_small_buffers_) {
    *got_block = false;
    if (blocks_.size() < NUM_SMALL_BLOCKS) {
      block_len = INITIAL_BLOCK_SIZES[blocks_.size()];
      null_indicators_size = ComputeNumNullIndicatorBytes(block_len);
      // Use small buffer only if:
      // 1. the small buffer's size is smaller than the configured max block size.
      // 2. a single row of tuples and null indicators (if any) fit in the small buffer.
      //
      // If condition 2 above is not met, we will bail. An alternative would be
      // to try the next larger small buffer.
      *got_block = block_len < block_mgr_->max_block_size() &&
          null_indicators_size >= 0 && row_size + null_indicators_size <= block_len;
    }
    // Do not switch to IO-buffers automatically. Do not get a buffer.
    if (!*got_block) return Status::OK();
  } else {
    DCHECK_GE(max_null_indicators_size_, 0);
    block_len = block_mgr_->max_block_size();
    null_indicators_size = max_null_indicators_size_;
    // Check if the size of row and null indicators exceeds the IO block size.
    if (UNLIKELY(row_size + null_indicators_size > block_len)) {
      return Status(TErrorCode::BTS_BLOCK_OVERFLOW,
          PrettyPrinter::Print(row_size, TUnit::BYTES),
          PrettyPrinter::Print(null_indicators_size, TUnit::BYTES));
    }
  }
  return NewWriteBlock(block_len, null_indicators_size, got_block);
}

Status BufferedTupleStream::NextReadBlock() {
  DCHECK(!closed_);
  DCHECK(read_block_ != blocks_.end());
  DCHECK_EQ(num_pinned_, NumPinned(blocks_)) << pinned_;

  // If non-NULL, this will be the current block if we are going to free it while
  // grabbing the next block. This will stay NULL if we don't want to free the
  // current block.
  BufferedBlockMgr::Block* block_to_free =
      (!pinned_ || delete_on_read_) ? *read_block_ : NULL;
  if (delete_on_read_) {
    DCHECK(read_block_ == blocks_.begin());
    DCHECK(*read_block_ != write_block_);
    blocks_.pop_front();
    read_block_ = blocks_.begin();
    read_block_idx_ = 0;
    if (block_to_free != NULL && !block_to_free->is_max_size()) {
      block_to_free->Delete();
      block_to_free = NULL;
      DCHECK_EQ(num_pinned_, NumPinned(blocks_)) << DebugString();
    }
  } else {
    ++read_block_;
    ++read_block_idx_;
    if (block_to_free != NULL && !block_to_free->is_max_size()) block_to_free = NULL;
  }

  bool pinned = false;
  if (read_block_ == blocks_.end() || (*read_block_)->is_pinned()) {
    // End of the blocks or already pinned, just handle block_to_free
    if (block_to_free != NULL) {
      SCOPED_TIMER(unpin_timer_);
      if (delete_on_read_) {
        block_to_free->Delete();
        --num_pinned_;
      } else {
        RETURN_IF_ERROR(UnpinBlock(block_to_free));
      }
    }
  } else {
    // Call into the block mgr to atomically unpin/delete the old block and pin the
    // new block.
    SCOPED_TIMER(pin_timer_);
    RETURN_IF_ERROR((*read_block_)->Pin(&pinned, block_to_free, !delete_on_read_));
    if (!pinned) {
      DCHECK(block_to_free == NULL) << "Should have been able to pin."
          << endl << block_mgr_->DebugString(block_mgr_client_);;
    }
    if (block_to_free == NULL && pinned) ++num_pinned_;
  }

  if (read_block_ != blocks_.end() && (*read_block_)->is_pinned()) {
    read_block_null_indicators_size_ =
        ComputeNumNullIndicatorBytes((*read_block_)->buffer_len());
    DCHECK_GE(read_block_null_indicators_size_, 0);
    read_tuple_idx_ = 0;
    read_ptr_ = (*read_block_)->buffer() + read_block_null_indicators_size_;
    read_end_ptr_ = (*read_block_)->buffer() + (*read_block_)->buffer_len();
  }
  DCHECK_EQ(num_pinned_, NumPinned(blocks_)) << DebugString();
  return Status::OK();
}

Status BufferedTupleStream::PrepareForRead(bool delete_on_read, bool* got_buffer) {
  DCHECK(!closed_);
  if (blocks_.empty()) return Status::OK();

  if (!read_write_ && write_block_ != NULL) {
    DCHECK(write_block_->is_pinned());
    if (!pinned_ && write_block_ != blocks_.front()) {
      RETURN_IF_ERROR(UnpinBlock(write_block_));
    }
    write_block_ = NULL;
  }

  // Walk the blocks and pin the first non-io sized block.
  for (list<BufferedBlockMgr::Block*>::iterator it = blocks_.begin();
      it != blocks_.end(); ++it) {
    if (!(*it)->is_pinned()) {
      SCOPED_TIMER(pin_timer_);
      bool current_pinned;
      RETURN_IF_ERROR((*it)->Pin(&current_pinned));
      if (!current_pinned) {
        DCHECK(got_buffer != NULL) << "Should have reserved enough blocks";
        *got_buffer = false;
        return Status::OK();
      }
      ++num_pinned_;
      DCHECK_EQ(num_pinned_, NumPinned(blocks_));
    }
    if ((*it)->is_max_size()) break;
  }

  read_block_ = blocks_.begin();
  DCHECK(read_block_ != blocks_.end());
  read_block_null_indicators_size_ =
      ComputeNumNullIndicatorBytes((*read_block_)->buffer_len());
  DCHECK_GE(read_block_null_indicators_size_, 0);
  read_tuple_idx_ = 0;
  read_ptr_ = (*read_block_)->buffer() + read_block_null_indicators_size_;
  read_end_ptr_ = (*read_block_)->buffer() + (*read_block_)->buffer_len();
  rows_returned_ = 0;
  read_block_idx_ = 0;
  delete_on_read_ = delete_on_read;
  if (got_buffer != NULL) *got_buffer = true;
  return Status::OK();
}

Status BufferedTupleStream::PinStream(bool already_reserved, bool* pinned) {
  DCHECK(!closed_);
  DCHECK(pinned != NULL);
  if (!already_reserved) {
    // If we can't get all the blocks, don't try at all.
    if (!block_mgr_->TryAcquireTmpReservation(block_mgr_client_, blocks_unpinned())) {
      *pinned = false;
      return Status::OK();
    }
  }

  for (list<BufferedBlockMgr::Block*>::iterator it = blocks_.begin();
      it != blocks_.end(); ++it) {
    if ((*it)->is_pinned()) continue;
    {
      SCOPED_TIMER(pin_timer_);
      RETURN_IF_ERROR((*it)->Pin(pinned));
    }
    if (!*pinned) {
      VLOG_QUERY << "Should have been reserved." << endl
                 << block_mgr_->DebugString(block_mgr_client_);
      return Status::OK();
    }
    ++num_pinned_;
    DCHECK_EQ(num_pinned_, NumPinned(blocks_));
  }

  if (!delete_on_read_) {
    // Populate block_start_idx_ on pin.
    DCHECK_EQ(block_start_idx_.size(), blocks_.size());
    block_start_idx_.clear();
    for (list<BufferedBlockMgr::Block*>::iterator it = blocks_.begin();
        it != blocks_.end(); ++it) {
      block_start_idx_.push_back((*it)->buffer());
    }
  }
  *pinned = true;
  pinned_ = true;
  return Status::OK();
}

Status BufferedTupleStream::UnpinStream(bool all) {
  DCHECK(!closed_);
  SCOPED_TIMER(unpin_timer_);

  BOOST_FOREACH(BufferedBlockMgr::Block* block, blocks_) {
    if (!block->is_pinned()) continue;
    if (!all && (block == write_block_ || (read_write_ && block == *read_block_))) {
      continue;
    }
    RETURN_IF_ERROR(UnpinBlock(block));
  }
  if (all) {
    read_block_ = blocks_.end();
    write_block_ = NULL;
  }
  pinned_ = false;
  return Status::OK();
}

int BufferedTupleStream::ComputeNumNullIndicatorBytes(int block_size) const {
  if (has_nullable_tuple_) {
    // We assume that all rows will use their max size, so we may be underutilizing the
    // space, i.e. we may have some unused space in case of rows with NULL tuples.
    const uint32_t tuples_per_row = desc_.tuple_descriptors().size();
    const uint32_t min_row_size_in_bits = 8 * fixed_tuple_row_size_ + tuples_per_row;
    const uint32_t block_size_in_bits = 8 * block_size;
    const uint32_t max_num_rows = block_size_in_bits / min_row_size_in_bits;
    if (UNLIKELY(max_num_rows == 0)) return -1;
    return BitUtil::RoundUpNumi64(max_num_rows * tuples_per_row) * 8;
  } else {
    // If there are no nullable tuples then no need to waste space for null indicators.
    return 0;
  }
}

Status BufferedTupleStream::GetRows(scoped_ptr<RowBatch>* batch, bool* got_rows) {
  RETURN_IF_ERROR(PinStream(false, got_rows));
  if (!*got_rows) return Status::OK();
  RETURN_IF_ERROR(PrepareForRead(false));
  batch->reset(
      new RowBatch(desc_, num_rows(), block_mgr_->get_tracker(block_mgr_client_)));
  bool eos = false;
  // Loop until GetNext fills the entire batch. Each call can stop at block
  // boundaries. We generally want it to stop, so that blocks can be freed
  // as we read. It is safe in this case because we pin the entire stream.
  while (!eos) {
    RETURN_IF_ERROR(GetNext(batch->get(), &eos));
  }
  return Status::OK();
}

Status BufferedTupleStream::GetNext(RowBatch* batch, bool* eos) {
  return GetNextInternal<false>(batch, eos, NULL);
}

Status BufferedTupleStream::GetNext(RowBatch* batch, bool* eos,
    vector<RowIdx>* indices) {
  return GetNextInternal<true>(batch, eos, indices);
}

template <bool FILL_INDICES>
Status BufferedTupleStream::GetNextInternal(RowBatch* batch, bool* eos,
    vector<RowIdx>* indices) {
  if (has_nullable_tuple_) {
    return GetNextInternal<FILL_INDICES, true>(batch, eos, indices);
  } else {
    return GetNextInternal<FILL_INDICES, false>(batch, eos, indices);
  }
}

template <bool FILL_INDICES, bool HAS_NULLABLE_TUPLE>
Status BufferedTupleStream::GetNextInternal(RowBatch* batch, bool* eos,
    vector<RowIdx>* indices) {
  DCHECK(!closed_);
  DCHECK(batch->row_desc().Equals(desc_));
  *eos = (rows_returned_ == num_rows_);
  if (*eos) return Status::OK();
  DCHECK_GE(read_block_null_indicators_size_, 0);

  const uint64_t tuples_per_row = desc_.tuple_descriptors().size();
  DCHECK_LE(read_tuple_idx_ / tuples_per_row, (*read_block_)->num_rows());
  DCHECK_EQ(read_tuple_idx_ % tuples_per_row, 0);
  int rows_returned_curr_block = read_tuple_idx_ / tuples_per_row;

  if (UNLIKELY(rows_returned_curr_block == (*read_block_)->num_rows())) {
    // Get the next block in the stream. We need to do this at the beginning of
    // the GetNext() call to ensure the buffer management semantics. NextReadBlock()
    // will recycle the memory for the rows returned from the *previous* call to
    // GetNext().
    RETURN_IF_ERROR(NextReadBlock());
    DCHECK(read_block_ != blocks_.end()) << DebugString();
    DCHECK_GE(read_block_null_indicators_size_, 0);
    rows_returned_curr_block = 0;
  }

  DCHECK(read_block_ != blocks_.end());
  DCHECK((*read_block_)->is_pinned()) << DebugString();
  DCHECK_GE(read_tuple_idx_, 0);

  int rows_left_in_block = (*read_block_)->num_rows() - rows_returned_curr_block;
  int rows_to_fill = std::min(batch->capacity() - batch->num_rows(), rows_left_in_block);
  DCHECK_GE(rows_to_fill, 1);
  batch->AddRows(rows_to_fill);
  uint8_t* tuple_row_mem = reinterpret_cast<uint8_t*>(batch->GetRow(batch->num_rows()));

  // Produce tuple rows from the current block and the corresponding position on the
  // null tuple indicator.
  if (FILL_INDICES) {
    DCHECK(indices != NULL);
    DCHECK(!delete_on_read_);
    DCHECK_EQ(batch->num_rows(), 0);
    indices->clear();
    indices->reserve(rows_to_fill);
  }

  uint8_t* null_word = NULL;
  uint32_t null_pos = 0;
  // Start reading from position read_tuple_idx_ in the block.
  // IMPALA-2256: Special case if there are no materialized slots.
  bool increment_row = RowConsumesMemory();
  uint64_t last_read_row = increment_row * (read_tuple_idx_ / tuples_per_row);
  for (int i = 0; i < rows_to_fill; ++i) {
    if (FILL_INDICES) {
      indices->push_back(RowIdx());
      DCHECK_EQ(indices->size(), i + 1);
      (*indices)[i].set(read_block_idx_, read_ptr_ - (*read_block_)->buffer(),
          last_read_row);
    }
    // Copy the row into the output batch.
    TupleRow* output_row = reinterpret_cast<TupleRow*>(tuple_row_mem);
    if (HAS_NULLABLE_TUPLE) {
      for (int j = 0; j < tuples_per_row; ++j) {
        // Stitch together the tuples from the block and the NULL ones.
        null_word = (*read_block_)->buffer() + (read_tuple_idx_ >> 3);
        null_pos = read_tuple_idx_ & 7;
        ++read_tuple_idx_;
        const bool is_not_null = ((*null_word & (1 << (7 - null_pos))) == 0);
        // Copy tuple and advance read_ptr_. If it is a NULL tuple, it calls SetTuple
        // with Tuple* being 0x0. To do that we multiply the current read_ptr_ with
        // false (0x0).
        output_row->SetTuple(j, reinterpret_cast<Tuple*>(
            reinterpret_cast<uint64_t>(read_ptr_) * is_not_null));
        read_ptr_ += fixed_tuple_sizes_[j] * is_not_null;
      }
    } else {
      // When we know that there are no nullable tuples we can skip null checks.
      for (int j = 0; j < tuples_per_row; ++j) {
        output_row->SetTuple(j, reinterpret_cast<Tuple*>(read_ptr_));
        read_ptr_ += fixed_tuple_sizes_[j];
      }
      read_tuple_idx_ += tuples_per_row;
    }
    tuple_row_mem += sizeof(Tuple*) * tuples_per_row;

    // Update string slot ptrs, skipping external strings.
    for (int j = 0; j < inlined_string_slots_.size(); ++j) {
      Tuple* tuple = output_row->GetTuple(inlined_string_slots_[j].first);
      if (HAS_NULLABLE_TUPLE && tuple == NULL) continue;
      FixUpStringsForRead(inlined_string_slots_[j].second, tuple);
    }

    // Update collection slot ptrs, skipping external collections. We traverse the
    // collection structure in the same order as it was written to the stream, allowing
    // us to infer the data layout based on the length of collections and strings.
    for (int j = 0; j < inlined_coll_slots_.size(); ++j) {
      Tuple* tuple = output_row->GetTuple(inlined_coll_slots_[j].first);
      if (HAS_NULLABLE_TUPLE && tuple == NULL) continue;
      FixUpCollectionsForRead(inlined_coll_slots_[j].second, tuple);
    }
    last_read_row += increment_row;
  }

  batch->CommitRows(rows_to_fill);
  rows_returned_ += rows_to_fill;
  *eos = (rows_returned_ == num_rows_);
  if ((!pinned_ || delete_on_read_) &&
      rows_returned_curr_block + rows_to_fill == (*read_block_)->num_rows()) {
    // No more data in this block. Mark this batch as needing to return so
    // the caller can pass the rows up the operator tree.
    batch->MarkNeedToReturn();
  }
  if (FILL_INDICES) DCHECK_EQ(indices->size(), rows_to_fill);
  DCHECK_LE(read_ptr_, read_end_ptr_);
  return Status::OK();
}

void BufferedTupleStream::FixUpStringsForRead(const vector<SlotDescriptor*>& string_slots,
    Tuple* tuple) {
  DCHECK(tuple != NULL);
  for (int i = 0; i < string_slots.size(); ++i) {
    const SlotDescriptor* slot_desc = string_slots[i];
    if (tuple->IsNull(slot_desc->null_indicator_offset())) continue;

    StringValue* sv = tuple->GetStringSlot(slot_desc->tuple_offset());
    DCHECK_LE(sv->len, read_block_bytes_remaining());
    sv->ptr = reinterpret_cast<char*>(read_ptr_);
    read_ptr_ += sv->len;
  }
}

void BufferedTupleStream::FixUpCollectionsForRead(const vector<SlotDescriptor*>& collection_slots,
    Tuple* tuple) {
  DCHECK(tuple != NULL);
  for (int i = 0; i < collection_slots.size(); ++i) {
    const SlotDescriptor* slot_desc = collection_slots[i];
    if (tuple->IsNull(slot_desc->null_indicator_offset())) continue;

    CollectionValue* cv = tuple->GetCollectionSlot(slot_desc->tuple_offset());
    const TupleDescriptor& item_desc = *slot_desc->collection_item_descriptor();
    int coll_byte_size = cv->num_tuples * item_desc.byte_size();
    DCHECK_LE(coll_byte_size, read_block_bytes_remaining());
    cv->ptr = reinterpret_cast<uint8_t*>(read_ptr_);
    read_ptr_ += coll_byte_size;

    if (!item_desc.HasVarlenSlots()) continue;
    uint8_t* coll_data = cv->ptr;
    for (int j = 0; j < cv->num_tuples; ++j) {
      Tuple* item = reinterpret_cast<Tuple*>(coll_data);
      FixUpStringsForRead(item_desc.string_slots(), item);
      FixUpCollectionsForRead(item_desc.collection_slots(), item);
      coll_data += item_desc.byte_size();
    }
  }
}

int64_t BufferedTupleStream::ComputeRowSize(TupleRow* row) const {
  int64_t size = 0;
  if (has_nullable_tuple_) {
    for (int i = 0; i < fixed_tuple_sizes_.size(); ++i) {
      if (row->GetTuple(i) != NULL) size += fixed_tuple_sizes_[i];
    }
  } else {
    size = fixed_tuple_row_size_;
  }
  for (int i = 0; i < inlined_string_slots_.size(); ++i) {
    Tuple* tuple = row->GetTuple(inlined_string_slots_[i].first);
    if (tuple == NULL) continue;
    const vector<SlotDescriptor*>& slots = inlined_string_slots_[i].second;
    for (auto it = slots.begin(); it != slots.end(); ++it) {
      if (tuple->IsNull((*it)->null_indicator_offset())) continue;
      size += tuple->GetStringSlot((*it)->tuple_offset())->len;
    }
  }

  for (int i = 0; i < inlined_coll_slots_.size(); ++i) {
    Tuple* tuple = row->GetTuple(inlined_coll_slots_[i].first);
    if (tuple == NULL) continue;
    const vector<SlotDescriptor*>& slots = inlined_coll_slots_[i].second;
    for (auto it = slots.begin(); it != slots.end(); ++it) {
      if (tuple->IsNull((*it)->null_indicator_offset())) continue;
      CollectionValue* cv = tuple->GetCollectionSlot((*it)->tuple_offset());
      const TupleDescriptor& item_desc = *(*it)->collection_item_descriptor();
      size += cv->num_tuples * item_desc.byte_size();

      if (!item_desc.HasVarlenSlots()) continue;
      for (int j = 0; j < cv->num_tuples; ++j) {
        Tuple* item = reinterpret_cast<Tuple*>(&cv->ptr[j * item_desc.byte_size()]);
        size += item->VarlenByteSize(item_desc);
      }
    }
  }
  return size;
}

bool BufferedTupleStream::DeepCopy(TupleRow* row) {
  if (has_nullable_tuple_) {
    return DeepCopyInternal<true>(row);
  } else {
    return DeepCopyInternal<false>(row);
  }
}

// TODO: this really needs codegen
// TODO: in case of duplicate tuples, this can redundantly serialize data.
template <bool HasNullableTuple>
bool BufferedTupleStream::DeepCopyInternal(TupleRow* row) {
  if (UNLIKELY(write_block_ == NULL)) return false;
  DCHECK_GE(write_block_null_indicators_size_, 0);
  DCHECK(write_block_->is_pinned()) << DebugString() << std::endl
      << write_block_->DebugString();

  const uint64_t tuples_per_row = desc_.tuple_descriptors().size();
  uint32_t bytes_remaining = write_block_bytes_remaining();
  if (UNLIKELY((bytes_remaining < fixed_tuple_row_size_) ||
              (HasNullableTuple &&
              (write_tuple_idx_ + tuples_per_row > write_block_null_indicators_size_ * 8)))) {
    return false;
  }

  // Copy the not NULL fixed len tuples. For the NULL tuples just update the NULL tuple
  // indicator.
  if (HasNullableTuple) {
    DCHECK_GT(write_block_null_indicators_size_, 0);
    uint8_t* null_word = NULL;
    uint32_t null_pos = 0;
    for (int i = 0; i < tuples_per_row; ++i) {
      null_word = write_block_->buffer() + (write_tuple_idx_ >> 3); // / 8
      null_pos = write_tuple_idx_ & 7;
      ++write_tuple_idx_;
      const int tuple_size = fixed_tuple_sizes_[i];
      Tuple* t = row->GetTuple(i);
      const uint8_t mask = 1 << (7 - null_pos);
      if (t != NULL) {
        *null_word &= ~mask;
        memcpy(write_ptr_, t, tuple_size);
        write_ptr_ += tuple_size;
      } else {
        *null_word |= mask;
      }
    }
    DCHECK_LE(write_tuple_idx_ - 1, write_block_null_indicators_size_ * 8);
  } else {
    // If we know that there are no nullable tuples no need to set the nullability flags.
    DCHECK_EQ(write_block_null_indicators_size_, 0);
    for (int i = 0; i < tuples_per_row; ++i) {
      const int tuple_size = fixed_tuple_sizes_[i];
      Tuple* t = row->GetTuple(i);
      // TODO: Once IMPALA-1306 (Avoid passing empty tuples of non-materialized slots)
      // is delivered, the check below should become DCHECK(t != NULL).
      DCHECK(t != NULL || tuple_size == 0);
      memcpy(write_ptr_, t, tuple_size);
      write_ptr_ += tuple_size;
    }
  }

  // Copy inlined string slots. Note: we do not need to convert the string ptrs to offsets
  // on the write path, only on the read. The tuple data is immediately followed
  // by the string data so only the len information is necessary.
  for (int i = 0; i < inlined_string_slots_.size(); ++i) {
    const Tuple* tuple = row->GetTuple(inlined_string_slots_[i].first);
    if (HasNullableTuple && tuple == NULL) continue;
    if (UNLIKELY(!CopyStrings(tuple, inlined_string_slots_[i].second))) return false;
  }

  // Copy inlined collection slots. We copy collection data in a well-defined order so
  // we do not need to convert pointers to offsets on the write path.
  for (int i = 0; i < inlined_coll_slots_.size(); ++i) {
    const Tuple* tuple = row->GetTuple(inlined_coll_slots_[i].first);
    if (HasNullableTuple && tuple == NULL) continue;
    if (UNLIKELY(!CopyCollections(tuple, inlined_coll_slots_[i].second))) return false;
  }

  write_block_->AddRow();
  ++num_rows_;
  return true;
}

bool BufferedTupleStream::CopyStrings(const Tuple* tuple,
    const vector<SlotDescriptor*>& string_slots) {
  for (int i = 0; i < string_slots.size(); ++i) {
    const SlotDescriptor* slot_desc = string_slots[i];
    if (tuple->IsNull(slot_desc->null_indicator_offset())) continue;
    const StringValue* sv = tuple->GetStringSlot(slot_desc->tuple_offset());
    if (LIKELY(sv->len > 0)) {
      if (UNLIKELY(write_block_bytes_remaining() < sv->len)) return false;

      memcpy(write_ptr_, sv->ptr, sv->len);
      write_ptr_ += sv->len;
    }
  }
  return true;
}

bool BufferedTupleStream::CopyCollections(const Tuple* tuple,
    const vector<SlotDescriptor*>& collection_slots) {
  for (int i = 0; i < collection_slots.size(); ++i) {
    const SlotDescriptor* slot_desc = collection_slots[i];
    if (tuple->IsNull(slot_desc->null_indicator_offset())) continue;
    const CollectionValue* cv = tuple->GetCollectionSlot(slot_desc->tuple_offset());
    const TupleDescriptor& item_desc = *slot_desc->collection_item_descriptor();
    if (LIKELY(cv->num_tuples > 0)) {
      int coll_byte_size = cv->num_tuples * item_desc.byte_size();
      if (UNLIKELY(write_block_bytes_remaining() < coll_byte_size)) return false;
      uint8_t* coll_data = write_ptr_;
      memcpy(coll_data, cv->ptr, coll_byte_size);
      write_ptr_ += coll_byte_size;

      if (!item_desc.HasVarlenSlots()) continue;
      // Copy variable length data when present in collection items.
      for (int j = 0; j < cv->num_tuples; ++j) {
        const Tuple* item = reinterpret_cast<Tuple*>(coll_data);
        if (UNLIKELY(!CopyStrings(item, item_desc.string_slots()))) return false;
        if (UNLIKELY(!CopyCollections(item, item_desc.collection_slots()))) return false;
        coll_data += item_desc.byte_size();
      }
    }
  }
  return true;
}

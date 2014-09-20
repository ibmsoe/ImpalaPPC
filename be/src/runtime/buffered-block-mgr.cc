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

#include "runtime/runtime-state.h"
#include "runtime/mem-tracker.h"
#include "runtime/mem-pool.h"
#include "runtime/buffered-block-mgr.h"
#include "runtime/tmp-file-mgr.h"
#include "util/runtime-profile.h"
#include "util/disk-info.h"
#include "util/filesystem-util.h"
#include "util/impalad-metrics.h"
#include "util/uid-util.h"

using namespace std;
using namespace boost;

namespace impala {

BufferedBlockMgr::BlockMgrsMap BufferedBlockMgr::query_to_block_mgrs_;
mutex BufferedBlockMgr::static_block_mgrs_lock_;

struct BufferedBlockMgr::Client {
  Client(BufferedBlockMgr* mgr, int num_reserved_buffers, MemTracker* tracker,
      RuntimeState* state)
    : mgr_(mgr),
      state_(state),
      tracker_(tracker),
      query_tracker_(mgr_->mem_tracker_->parent()),
      num_reserved_buffers_(num_reserved_buffers),
      num_pinned_buffers_(0) {
  }

  // Unowned.
  BufferedBlockMgr* mgr_;

  // Unowned.
  RuntimeState* state_;

  // Tracker for this client. Can be NULL. Unowned.
  // If this is set, when the client gets a buffer, we update the consumption on this
  // tracker. However, we don't want to transfer the buffer from the block mgr to the
  // client since (i.e. release from the block mgr), since the block mgr is where the
  // block mem usage limit is enforced. Even when we give a buffer to a client, the
  // buffer is still owned and counts against the block mgr tracker (i.e. there is a
  // fixed pool of buffers regardless of if they are in the block mgr or the clients).
  MemTracker* tracker_;

  // This is the common ancestor between the block mgr tracker and the client tracker.
  // When memory is transferred to the client, we want it to stop at this tracker.
  MemTracker* query_tracker_;

  // Number of buffers reserved by this client.
  int num_reserved_buffers_;

  // Number of buffers pinned by this client.
  int num_pinned_buffers_;

  void PinBuffer(BufferDescriptor* buffer) {
    DCHECK(buffer != NULL);
    ++num_pinned_buffers_;
    if (tracker_ != NULL) tracker_->ConsumeLocal(buffer->len, query_tracker_);
  }

  void UnpinBuffer(BufferDescriptor* buffer) {
    DCHECK(buffer != NULL);
    DCHECK_GT(num_pinned_buffers_, 0);
    --num_pinned_buffers_;
    if (tracker_ != NULL) tracker_->ReleaseLocal(buffer->len, query_tracker_);
  }
};

// BufferedBlockMgr::Block methods.
BufferedBlockMgr::Block::Block(BufferedBlockMgr* block_mgr)
  : buffer_desc_(NULL),
    block_mgr_(block_mgr),
    client_(NULL),
    write_range_(NULL),
    valid_data_len_(0) {
}

Status BufferedBlockMgr::Block::Pin(bool* pinned) {
  RETURN_IF_ERROR(block_mgr_->PinBlock(this, pinned));
  DCHECK(Validate()) << endl << DebugString();
  return Status::OK;
}

Status BufferedBlockMgr::Block::Unpin() {
  RETURN_IF_ERROR(block_mgr_->UnpinBlock(this));
  DCHECK(Validate()) << endl << DebugString();
  return Status::OK;
}

Status BufferedBlockMgr::Block::Delete() {
  RETURN_IF_ERROR(block_mgr_->DeleteBlock(this));
  DCHECK(Validate()) << endl << DebugString();
  return Status::OK;
}

void BufferedBlockMgr::Block::Init() {
  // No locks are taken because the block is new or has previously been deleted.
  is_pinned_ = false;
  in_write_ = false;
  is_deleted_ = false;
  valid_data_len_ = 0;
  client_ = NULL;
}

bool BufferedBlockMgr::Block::Validate() const {
  if (is_deleted_ && (is_pinned_ || (!in_write_ && buffer_desc_ != NULL))) {
    LOG(ERROR) << "Deleted block in use - " << DebugString();
    return false;
  }

  if (buffer_desc_ == NULL && (is_pinned_ || in_write_)) {
    LOG(ERROR) << "Block without buffer in use - " << DebugString();
    return false;
  }

  if (buffer_desc_ == NULL && block_mgr_->unpinned_blocks_.Contains(this)) {
    LOG(ERROR) << "Unpersisted block without buffer - " << DebugString();
    return false;
  }

  if (buffer_desc_ != NULL && (buffer_desc_->block != this)) {
    LOG(ERROR) << "Block buffer inconsistency - " << DebugString();
    return false;
  }

  return true;
}

string BufferedBlockMgr::Block::DebugString() const {
  stringstream ss;
  ss << "Block: " << this
     << " Buffer Desc: " << buffer_desc_
     << " Data Len: " << valid_data_len_
     << " Deleted: " << is_deleted_
     << " Pinned: " << is_pinned_
     << " Write Issued: " << in_write_
     << " Client Local: " << client_local_;
  return ss.str();
}

Status BufferedBlockMgr::Create(RuntimeState* state, MemTracker* parent,
    RuntimeProfile* profile, int64_t mem_limit, int64_t block_size,
    shared_ptr<BufferedBlockMgr>* block_mgr) {

  lock_guard<mutex> lock(static_block_mgrs_lock_);
  BlockMgrsMap::iterator it = query_to_block_mgrs_.find(state->query_id());
  if ((it != query_to_block_mgrs_.end())) {
    *block_mgr = it->second.lock();
    return Status::OK;
  }

  block_mgr->reset(new BufferedBlockMgr(state, parent, mem_limit, block_size));
  query_to_block_mgrs_[state->query_id()] = *block_mgr;

  // Initialize the tmp files and the initial file to use.
  int num_tmp_devices = TmpFileMgr::num_tmp_devices();
  (*block_mgr)->tmp_files_.reserve(num_tmp_devices);
  for (int i = 0; i < num_tmp_devices; ++i) {
    TmpFileMgr::File* tmp_file;
    RETURN_IF_ERROR(TmpFileMgr::GetFile(
        i, state->query_id(), state->fragment_instance_id(), &tmp_file));
    (*block_mgr)->tmp_files_.push_back(tmp_file);
  }
  (*block_mgr)->next_block_index_ = rand() % num_tmp_devices;
  (*block_mgr)->InitCounters(profile);
  if (mem_limit > 0) {
    (*block_mgr)->num_unreserved_buffers_ = mem_limit / block_size;
  } else {
    (*block_mgr)->num_unreserved_buffers_ = numeric_limits<int>::max();
  }
  return Status::OK;
}

Status BufferedBlockMgr::RegisterClient(int num_reserved_buffers, MemTracker* tracker,
    RuntimeState* state, Client** client) {
  DCHECK_GE(num_reserved_buffers, 0);
  *client = obj_pool_.Add(new Client(this, num_reserved_buffers, tracker, state));
  num_unreserved_buffers_ -= num_reserved_buffers;
  total_reserved_buffers_ += num_reserved_buffers;
  return Status::OK;
}

void BufferedBlockMgr::LowerBufferReservation(Client* client, int num_buffers) {
  lock_guard<mutex> lock(lock_);
  DCHECK_GE(client->num_reserved_buffers_, num_buffers);
  int delta = client->num_reserved_buffers_ - num_buffers;
  client->num_reserved_buffers_ = num_buffers;
  num_unreserved_buffers_ += delta;
  total_reserved_buffers_ -= delta;
}

void BufferedBlockMgr::Cancel() {
  lock_guard<mutex> lock(lock_);
  if (is_cancelled_) return;
  is_cancelled_ = true;

  // Cancel to the underlying io mgr to unblock any waiting threads.
  io_mgr_->CancelContext(io_request_context_);
}

Status BufferedBlockMgr::GetNewBlock(Client* client, Block* unpin_block, Block** block,
    int64_t len) {
  *block = NULL;
  Block* new_block = NULL;
  {
    lock_guard<mutex> lock(lock_);
    if (is_cancelled_) return Status::CANCELLED;
    new_block = GetUnusedBlock(client);
  }
  DCHECK_NOTNULL(new_block);
  DCHECK(new_block->client_ == client);

  if (len >= 0) {
    DCHECK(unpin_block == NULL);
    DCHECK_LT(len, max_block_size_) << "Cannot request blocks bigger than max_len";
    if (mem_tracker_->TryConsume(len)) {
      mem_used_counter_->Add(len);
      uint8_t* buffer = reinterpret_cast<uint8_t*>(malloc(len));
      new_block->buffer_desc_ = obj_pool_.Add(new BufferDescriptor(buffer, len));
      new_block->buffer_desc_->block = new_block;
      new_block->is_pinned_ = true;
      client->PinBuffer(new_block->buffer_desc_);
      ++num_unreserved_pinned_buffers_;
      *block = new_block;
      return Status::OK;
    } else {
      new_block->is_deleted_ = true;
      ReturnUnusedBlock(new_block);
      return Status::OK;
    }
  }

  bool in_mem;
  RETURN_IF_ERROR(FindBufferForBlock(new_block, &in_mem));
  DCHECK(!in_mem) << "A new block cannot start in mem.";

  if (!new_block->is_pinned()) {
    if (unpin_block == NULL) {
      // We couldn't get a new block and no unpin block was provided. Can't return
      // a block.
      new_block->is_deleted_ = true;
      ReturnUnusedBlock(new_block);
      new_block = NULL;
    } else {
      // We need to transfer the buffer from unpin_block to new_block.

      // First write out the old block.
      unpin_block->is_pinned_ = false;
      unpin_block->client_local_ = true;

      unique_lock<mutex> lock(lock_);
      RETURN_IF_ERROR(WriteUnpinnedBlock(unpin_block));

      // Wait for the write to complete.
      while (unpin_block->in_write_ && !is_cancelled_) {
        unpin_block->write_complete_cv_.wait(lock);
      }
      if (is_cancelled_) return Status::CANCELLED;

      // Assign the buffer to the new block.
      DCHECK(!unpin_block->is_pinned_);
      DCHECK(!unpin_block->in_write_);
      new_block->buffer_desc_ = unpin_block->buffer_desc_;
      new_block->buffer_desc_->block = new_block;
      unpin_block->buffer_desc_ = NULL;
      new_block->is_pinned_ = true;
    }
  } else if (unpin_block != NULL) {
    // Got a new block without needing to transfer. Just unpin this block.
    RETURN_IF_ERROR(unpin_block->Unpin());
  }

  if (new_block != NULL) DCHECK(new_block->is_pinned());
  *block = new_block;
  return Status::OK;
}

BufferedBlockMgr::~BufferedBlockMgr() {
  {
    lock_guard<mutex> lock(static_block_mgrs_lock_);
    DCHECK(query_to_block_mgrs_.find(query_id_) != query_to_block_mgrs_.end());
    query_to_block_mgrs_.erase(query_id_);
  }

  if (io_request_context_ != NULL) {
    io_mgr_->UnregisterContext(io_request_context_);
  }

  // Grab this lock to synchronize with io threads in WriteComplete(). We need those
  // to finish to ensure that memory buffers remain valid for any in-progress writes.
  lock_guard<mutex> lock(lock_);
  // Delete tmp files.
  BOOST_FOREACH(TmpFileMgr::File& file, tmp_files_) {
    file.Remove();
  }
  tmp_files_.clear();

  // Free memory resources.
  if (buffer_pool_ != NULL) {
    buffer_pool_->FreeAll();
    buffer_pool_.reset();
  }
  DCHECK_EQ(mem_tracker_->consumption(), 0);
  mem_tracker_->UnregisterFromParent();
  mem_tracker_.reset();
}

BufferedBlockMgr::BufferedBlockMgr(RuntimeState* state, MemTracker* parent,
    int64_t mem_limit, int64_t block_size)
  : max_block_size_(block_size),
    block_write_threshold_(TmpFileMgr::num_tmp_devices()),
    query_id_(state->query_id()),
    num_outstanding_writes_(0),
    io_mgr_(state->io_mgr()),
    is_cancelled_(false) {
  DCHECK(parent != NULL);
  // Create a new mem_tracker and allocate buffers.
  mem_tracker_.reset(new MemTracker(mem_limit, -1, "Block Manager", parent));
  buffer_pool_.reset(new MemPool(mem_tracker_.get(), block_size));
  state->io_mgr()->RegisterContext(NULL, &io_request_context_);
}

int64_t BufferedBlockMgr::bytes_allocated() const {
  return mem_tracker_->consumption();
}

int BufferedBlockMgr::num_pinned_buffers(Client* client) const {
  return client->num_pinned_buffers_;
}

int BufferedBlockMgr::num_reserved_buffers_remaining(Client* client) const {
  return max(client->num_reserved_buffers_ - client->num_pinned_buffers_, 0);
}

Status BufferedBlockMgr::PinBlock(Block* block, bool* pinned) {
  DCHECK(!block->is_deleted_);
  *pinned = false;
  {
    lock_guard<mutex> lock(lock_);
    if (is_cancelled_) return Status::CANCELLED;
  }

  if (block->is_pinned_) {
    *pinned = true;
    return Status::OK;
  }

  bool in_mem = false;
  RETURN_IF_ERROR(FindBufferForBlock(block, &in_mem));
  *pinned = block->is_pinned_;

  if (!block->is_pinned_ || in_mem || block->valid_data_len_ == 0) {
    // Either there was no memory for this block or the buffer was never evicted
    // and already contains the data. Either way, nothing left to do.
    return Status::OK;
  }

  // Read the block from disk if it was not in memory.
  DCHECK_NOTNULL(block->write_range_);
  SCOPED_TIMER(disk_read_timer_);
  // Create a ScanRange to perform the read.
  DiskIoMgr::ScanRange* scan_range =
      obj_pool_.Add(new DiskIoMgr::ScanRange());
  scan_range->Reset(block->write_range_->file(), block->write_range_->len(),
      block->write_range_->offset(), block->write_range_->disk_id(), false, block);
  vector<DiskIoMgr::ScanRange*> ranges(1, scan_range);
  RETURN_IF_ERROR(io_mgr_->AddScanRanges(io_request_context_, ranges, true));

  // Read from the io mgr buffer into the block's assigned buffer.
  int64_t offset = 0;
  DiskIoMgr::BufferDescriptor* io_mgr_buffer;
  do {
    RETURN_IF_ERROR(scan_range->GetNext(&io_mgr_buffer));
    memcpy(block->buffer() + offset, io_mgr_buffer->buffer(), io_mgr_buffer->len());
    offset += io_mgr_buffer->len();
    io_mgr_buffer->Return();
  } while (!io_mgr_buffer->eosr());
  DCHECK_EQ(offset, block->write_range_->len());
  return Status::OK;
}

Status BufferedBlockMgr::UnpinBlock(Block* block) {
  DCHECK(!block->is_deleted_) << "Unpin for deleted block.";
  lock_guard<mutex> unpinned_lock(lock_);
  if (is_cancelled_) return Status::CANCELLED;
  if (!block->is_pinned_) return Status::OK;
  DCHECK(Validate()) << endl << DebugInternal();
  DCHECK_EQ(block->buffer_desc_->len, max_block_size_) << "Can only unpin io blocks.";
  // Add 'block' to the list of unpinned blocks and set is_pinned_ to false.
  // Cache its position in the list for later removal.
  block->is_pinned_ = false;
  DCHECK(!unpinned_blocks_.Contains(block)) << " Unpin for block in unpinned list";
  DCHECK_GT(block->client_->num_pinned_buffers_, 0);
  if (!block->in_write_) unpinned_blocks_.Enqueue(block);
  if (block->client_->num_pinned_buffers_ > block->client_->num_reserved_buffers_) {
    --num_unreserved_pinned_buffers_;
  }
  block->client_->UnpinBuffer(block->buffer_desc_);
  RETURN_IF_ERROR(WriteUnpinnedBlocks());
  DCHECK(Validate()) << endl << DebugInternal();
  return Status::OK;
}

Status BufferedBlockMgr::WriteUnpinnedBlocks() {
  // Assumes block manager lock is already taken.
  while (num_outstanding_writes_ + free_io_buffers_.size() < block_write_threshold_) {
    if (unpinned_blocks_.empty()) break;
    // Pop a block from the back of the list (LIFO)
    Block* write_block = unpinned_blocks_.PopBack();
    write_block->client_local_ = false;
    RETURN_IF_ERROR(WriteUnpinnedBlock(write_block));
    ++num_outstanding_writes_;
  }
  DCHECK(Validate()) << endl << DebugInternal();
  return Status::OK;
}

Status BufferedBlockMgr::WriteUnpinnedBlock(Block* block) {
  // Assumes block manager lock is already taken.
  DCHECK(!block->is_pinned_) << block->DebugString();
  DCHECK(!block->in_write_) << block->DebugString();

  if (block->write_range_ == NULL) {
    // First time the block is being persisted. Find the next physical file in
    // round-robin order and create a write range for it.
    TmpFileMgr::File& tmp_file = tmp_files_[next_block_index_];
    next_block_index_ = (next_block_index_ + 1) % tmp_files_.size();
    int64_t file_offset;
    RETURN_IF_ERROR(tmp_file.AllocateSpace(max_block_size_, &file_offset));
    int disk_id = tmp_file.disk_id();
    if (disk_id < 0) {
      // Assign a valid disk id to the write range if the tmp file was not assigned one.
      static unsigned int next_disk_id = 0;
      disk_id = (++next_disk_id) % io_mgr_->num_disks();
    }
    disk_id %= io_mgr_->num_disks();
    DiskIoMgr::WriteRange::WriteDoneCallback callback =
        bind(mem_fn(&BufferedBlockMgr::WriteComplete), this, block, _1);
    block->write_range_ = obj_pool_.Add(new DiskIoMgr::WriteRange(
        tmp_file.path(), file_offset, disk_id, callback));
  }

  block->write_range_->SetData(block->buffer(), block->valid_data_len_);

  // Issue write through DiskIoMgr.
  RETURN_IF_ERROR(io_mgr_->AddWriteRange(io_request_context_, block->write_range_));
  block->in_write_ = true;
  DCHECK(block->Validate()) << endl << block->DebugString();
  outstanding_writes_counter_->Add(1);
  writes_issued_counter_->Add(1);
  if (writes_issued_counter_->value() == 1) {
    if (ImpaladMetrics::NUM_QUERIES_SPILLED != NULL) {
      ImpaladMetrics::NUM_QUERIES_SPILLED->Increment(1);
    }
  }
  return Status::OK;
}

void BufferedBlockMgr::WriteComplete(Block* block, const Status& write_status) {
  outstanding_writes_counter_->Add(-1);
  lock_guard<mutex> lock(lock_);
  DCHECK(Validate()) << endl << DebugInternal();
  DCHECK(block->in_write_) << "WriteComplete() for block not in write."
                           << endl << block->DebugString();
  if (!block->client_local_) {
    DCHECK_GT(num_outstanding_writes_, 0) << block->DebugString();
    --num_outstanding_writes_;
  }
  block->in_write_ = false;
  if (is_cancelled_) return;
  // Check for an error. Set cancelled and wake up waiting threads if an error occurred.
  if (!write_status.ok()) {
    block->client_->state_->LogError(write_status);
    is_cancelled_ = true;
    if (block->client_local_) {
      block->write_complete_cv_.notify_one();
    } else {
      buffer_available_cv_.notify_all();
    }
    return;
  }

  // If the block was re-pinned when it was in the IOMgr queue, don't free it.
  if (block->is_pinned_) {
    // The number of outstanding writes has decreased but the number of free buffers
    // hasn't.
    DCHECK(!block->client_local_)
        << "Client should be waiting, No one should have pinned this block.";
    WriteUnpinnedBlocks();
    DCHECK(Validate()) << endl << DebugInternal();
    return;
  }

  if (block->client_local_) {
    DCHECK(!block->is_deleted_)
        << "Client should be waiting. No one should have deleted this block.";
    block->write_complete_cv_.notify_one();
    return;
  }

  DCHECK_EQ(block->buffer_desc_->len, max_block_size_)
      << "Only io sized buffers should spill";
  free_io_buffers_.Enqueue(block->buffer_desc_);
  if (block->is_deleted_) {
    block->buffer_desc_->block = NULL;
    block->buffer_desc_ = NULL;
    ReturnUnusedBlock(block);
  }
  DCHECK(Validate()) << endl << DebugInternal();
  buffer_available_cv_.notify_one();
}

Status BufferedBlockMgr::DeleteBlock(Block* block) {
  DCHECK(!block->is_deleted_);
  lock_guard<mutex> lock(lock_);
  if (is_cancelled_) return Status::CANCELLED;
  DCHECK(block->Validate()) << endl << DebugInternal();

  block->is_deleted_ = true;
  if (block->is_pinned_) {
    block->is_pinned_ = false;
    if (block->client_->num_pinned_buffers_ > block->client_->num_reserved_buffers_) {
      --num_unreserved_pinned_buffers_;
    }
    block->client_->UnpinBuffer(block->buffer_desc_);
  } else if (unpinned_blocks_.Contains(block)) {
    // Remove block from unpinned list.
    unpinned_blocks_.Remove(block);
  }

  if (block->in_write_) {
    // If a write is still pending, return. Cleanup will be done in WriteComplete().
    return Status::OK;
  }

  if (block->buffer_desc_ != NULL) {
    if (block->buffer_desc_->len != max_block_size_) {
      // Just delete the block for now.
      free(block->buffer_desc_->buffer);
      mem_used_counter_->Add(-block->buffer_desc_->len);
      mem_tracker_->Release(block->buffer_desc_->len);
    } else if (!free_io_buffers_.Contains(block->buffer_desc_)) {
      free_io_buffers_.Enqueue(block->buffer_desc_);
      buffer_available_cv_.notify_one();
    }
    block->buffer_desc_->block = NULL;
    block->buffer_desc_ = NULL;
  }
  ReturnUnusedBlock(block);
  DCHECK(Validate()) << endl << DebugInternal();
  return Status::OK;
}

void BufferedBlockMgr::ReturnUnusedBlock(Block* block) {
  DCHECK(block->is_deleted_) << block->DebugString();
  DCHECK(!block->is_pinned_) << block->DebugString();;
  DCHECK(block->buffer_desc_ == NULL);
  block->Init();
  unused_blocks_.Enqueue(block);
}

Status BufferedBlockMgr::FindBufferForBlock(Block* block, bool* in_mem) {
  Client* client = block->client_;
  DCHECK(client != NULL);
  DCHECK(!block->is_pinned_);

  *in_mem = false;
  unique_lock<mutex> l(lock_);

  DCHECK(!block->is_pinned_ && !block->is_deleted_)
      << "FindBufferForBlock() " << endl << block->DebugString();
  DCHECK(Validate()) << endl << DebugInternal();

  bool is_optional_request = client->num_pinned_buffers_ >= client->num_reserved_buffers_;
  if (is_optional_request && num_unreserved_pinned_buffers_ >= num_unreserved_buffers_) {
    // The client already has its quota and there are no optional blocks left.
    return Status::OK;
  }

  if (block->buffer_desc_ != NULL) {
    // The block is in memory. It may be in 3 states
    // 1) In the unpinned list. The buffer will not be in the free list.
    // 2) Or, in_write_ = true. The buffer will not be in the free list.
    // 3) Or, the buffer is free, but hasn't yet been reassigned to a different block.
    DCHECK(unpinned_blocks_.Contains(block) ||
           block->in_write_ ||
           free_io_buffers_.Contains(block->buffer_desc_));
    if (unpinned_blocks_.Contains(block)) {
      unpinned_blocks_.Remove(block);
      DCHECK(!free_io_buffers_.Contains(block->buffer_desc_));
    } else if (block->in_write_) {
      DCHECK(block->in_write_ && !free_io_buffers_.Contains(block->buffer_desc_));
    } else {
      free_io_buffers_.Remove(block->buffer_desc_);
    }
    buffered_pin_counter_->Add(1);
    *in_mem = true;
  } else {
    // We need to find a new buffer for this block. We prefer getting this buffer in
    // this order:
    //  1. Allocate a new block if the number of free blocks is less than the write
    //     threshold, until we run out of memory.
    //  2. Pick a buffer from the free list.
    //  3. Wait and evict an unpinned buffer.
    BufferDescriptor* buffer_desc = NULL;
    if (free_io_buffers_.size() < block_write_threshold_) {
      uint8_t* new_buffer = buffer_pool_->TryAllocate(max_block_size_);
      if (new_buffer != NULL) {
        mem_used_counter_->Add(max_block_size_);
        buffer_desc = obj_pool_.Add(new BufferDescriptor(new_buffer, max_block_size_));
        all_io_buffers_.push_back(buffer_desc);
      }
    }
    if (buffer_desc == NULL) {
      if (free_io_buffers_.empty() && unpinned_blocks_.empty() &&
          num_outstanding_writes_ == 0) {
        // There are no free buffers or blocks we can evict. We need to fail this request.
        // If this is an optional request, return OK. If it is required, return OOM.
        if (is_optional_request) return Status::OK;
        Status status = Status::MEM_LIMIT_EXCEEDED;
        status.AddErrorMsg("Query did not have enough memory to get the minimum required "
            "buffers.");
        return status;
      }

      // At this point, this block needs to use a buffer that was unpinned from another
      // block. Get a free buffer from the front of the queue and assign it to the block.
      while (free_io_buffers_.empty()) {
        SCOPED_TIMER(buffer_wait_timer_);
        // Try to evict unpinned blocks before waiting.
        RETURN_IF_ERROR(WriteUnpinnedBlocks());
        buffer_available_cv_.wait(l);
        if (is_cancelled_) return Status::CANCELLED;
      }
      buffer_desc = free_io_buffers_.Dequeue();
    }

    DCHECK(buffer_desc != NULL);
    if (buffer_desc->block != NULL) {
      // This buffer was assigned to a block but now we are reusing it. Reset the
      // previous block->buffer link.
      DCHECK(buffer_desc->block->Validate()) << endl << buffer_desc->block->DebugString();
      buffer_desc->block->buffer_desc_ = NULL;
    }
    buffer_desc->block = block;
    block->buffer_desc_ = buffer_desc;
  }
  client->PinBuffer(block->buffer_desc_);
  if (is_optional_request) ++num_unreserved_pinned_buffers_;

  DCHECK_NOTNULL(block->buffer_desc_);
  block->is_pinned_ = true;
  DCHECK(block->Validate()) << endl << block->DebugString();
  // The number of free buffers has decreased. Write unpinned blocks if the number
  // of free buffers below the threshold is reached.
  RETURN_IF_ERROR(WriteUnpinnedBlocks());
  DCHECK(Validate()) << endl << DebugInternal();
  return Status::OK;
}

BufferedBlockMgr::Block* BufferedBlockMgr::GetUnusedBlock(Client* client) {
  DCHECK(client != NULL);
  Block* new_block;
  if (unused_blocks_.empty()) {
    new_block = obj_pool_.Add(new Block(this));
    new_block->Init();
    created_block_counter_->Add(1);
  } else {
    new_block = unused_blocks_.Dequeue();
    recycled_blocks_counter_->Add(1);
  }
  new_block->client_ = client;

  DCHECK_NOTNULL(new_block);
  DCHECK(new_block->Validate()) << endl << new_block->DebugString();
  return new_block;
}

bool BufferedBlockMgr::Validate() const {
  int num_free_io_buffers = 0;

  if (num_unreserved_pinned_buffers_ < 0) {
    LOG(ERROR) << "num_unreserved_pinned_buffers_ < 0: "
                 << num_unreserved_pinned_buffers_;
    return false;
  }

  BOOST_FOREACH(BufferDescriptor* buffer, all_io_buffers_) {
    bool is_free = free_io_buffers_.Contains(buffer);
    num_free_io_buffers += is_free;
    if (buffer->block == NULL && !is_free) {
      LOG(ERROR) << "Buffer with no block not in free list." << endl << DebugInternal();
      return false;
    }

    if (buffer->len != max_block_size_) {
      LOG(ERROR) << "Non-io sized buffers should not end up on free list.";
      return false;
    }

    if (buffer->block != NULL) {
      if (!buffer->block->Validate()) {
        LOG(ERROR) << "buffer->block inconsistent."
                      << endl << buffer->block->DebugString();
        return false;
      }

      if (is_free && (buffer->block->is_pinned_ || buffer->block->in_write_ ||
          unpinned_blocks_.Contains(buffer->block))) {
        LOG(ERROR) << "Block with buffer in free list and"
                      << " is_pinned_ = " << buffer->block->is_pinned_
                      << " in_write_ = " << buffer->block->in_write_
                      << " Unpinned_blocks_.Contains = "
                      << unpinned_blocks_.Contains(buffer->block)
                      << endl << buffer->block->DebugString();
        return false;
      }
    }
  }

  if (free_io_buffers_.size() != num_free_io_buffers) {
    LOG(ERROR) << "free_buffer_list_ inconsistency."
                  << " num_free_io_buffers = " << num_free_io_buffers
                  << " free_io_buffers_.size() = " << free_io_buffers_.size()
                  << endl << DebugInternal();
    return false;
  }

  Block* block = unpinned_blocks_.head();
  while (block != NULL) {
    if (!block->Validate()) {
      LOG(ERROR) << "Block inconsistent in unpinned list."
                    << endl << block->DebugString();
      return false;
    }

    if (block->in_write_ || free_io_buffers_.Contains(block->buffer_desc_)) {
      LOG(ERROR) << "Block in unpinned list with"
                    << " in_write_ = " << block->in_write_
                    << " free_io_buffers_.Contains = "
                    << free_io_buffers_.Contains(block->buffer_desc_)
                    << endl << block->DebugString();
      return false;
    }
    block = block->Next();
  }

  // Check if we're writing blocks when the number of free buffers falls below
  // threshold. We don't write blocks after cancellation.
  if (!is_cancelled_ && !unpinned_blocks_.empty() &&
      (free_io_buffers_.size() + num_outstanding_writes_ < block_write_threshold_)) {
    LOG(ERROR) << "Missed writing unpinned blocks";
    return false;
  }
  return true;
}

string BufferedBlockMgr::DebugString() {
  unique_lock<mutex> l(lock_);
  return DebugInternal();
}

string BufferedBlockMgr::DebugInternal() const {
  stringstream ss;
  ss << "Buffered block mgr" << endl
     << " Num writes outstanding " << outstanding_writes_counter_->value() << endl
     << " Num free io buffers " << free_io_buffers_.size() << endl
     << " Num unpinned blocks " << unpinned_blocks_.size() << endl
     << " Num unreserved buffers " << num_unreserved_buffers_ << endl
     << " Num unreserved pinned buffers " << num_unreserved_pinned_buffers_ << endl
     << " Remaining memory " << mem_tracker_->SpareCapacity() << endl
     << " Block write threshold " << block_write_threshold_;
  return ss.str();
}

void BufferedBlockMgr::InitCounters(RuntimeProfile* profile) {
  profile_.reset(new RuntimeProfile(&obj_pool_, "BlockMgr"));
  profile->AddChild(profile_.get());

  mem_limit_counter_ = ADD_COUNTER(profile_.get(), "MemoryLimit", TCounterType::BYTES);
  mem_limit_counter_->Set(mem_tracker_->limit());
  mem_used_counter_ = ADD_COUNTER(profile_.get(), "MemoryUsed", TCounterType::BYTES);
  block_size_counter_ = ADD_COUNTER(profile_.get(), "MaxBlockSize", TCounterType::BYTES);
  block_size_counter_->Set(max_block_size_);
  created_block_counter_ = ADD_COUNTER(
      profile_.get(), "BlocksCreated", TCounterType::UNIT);
  recycled_blocks_counter_ = ADD_COUNTER(
      profile_.get(), "BlocksRecycled", TCounterType::UNIT);
  writes_issued_counter_ = ADD_COUNTER(
      profile_.get(), "BlockWritesIssued", TCounterType::UNIT);
  outstanding_writes_counter_ =
      ADD_COUNTER(profile_.get(), "BlockWritesOutstanding", TCounterType::UNIT);
  buffered_pin_counter_ = ADD_COUNTER(profile_.get(), "BufferedPins", TCounterType::UNIT);
  disk_read_timer_ = ADD_TIMER(profile_.get(), "TotalReadBlockTime");
  buffer_wait_timer_ = ADD_TIMER(profile_.get(), "TotalBufferWaitTime");
}

} // namespace impala

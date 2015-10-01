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

#include "exec/hdfs-parquet-scanner.h"

#include <limits> // for std::numeric_limits
#include <queue>

#include <boost/algorithm/string.hpp>
#include <gflags/gflags.h>
#include <gutil/strings/substitute.h>

#include "common/object-pool.h"
#include "common/logging.h"
#include "exec/hdfs-scan-node.h"
#include "exec/scanner-context.inline.h"
#include "exec/read-write-util.h"
#include "exprs/expr.h"
#include "runtime/array-value-builder.h"
#include "runtime/descriptors.h"
#include "runtime/runtime-state.h"
#include "runtime/mem-pool.h"
#include "runtime/raw-value.h"
#include "runtime/row-batch.h"
#include "runtime/tuple-row.h"
#include "runtime/tuple.h"
#include "runtime/string-value.h"
#include "util/bitmap.h"
#include "util/bit-util.h"
#include "util/decompress.h"
#include "util/debug-util.h"
#include "util/error-util.h"
#include "util/dict-encoding.h"
#include "util/rle-encoding.h"
#include "util/runtime-profile.h"
#include "rpc/thrift-util.h"

#include "common/names.h"

using boost::algorithm::is_any_of;
using boost::algorithm::split;
using boost::algorithm::token_compress_on;
using namespace impala;
using namespace strings;

// Provide a workaround for IMPALA-1658.
DEFINE_bool(convert_legacy_hive_parquet_utc_timestamps, false,
    "When true, TIMESTAMPs read from files written by Parquet-MR (used by Hive) will "
    "be converted from UTC to local time. Writes are unaffected.");

// Max data page header size in bytes. This is an estimate and only needs to be an upper
// bound. It is theoretically possible to have a page header of any size due to string
// value statistics, but in practice we'll have trouble reading string values this large.
const int MAX_PAGE_HEADER_SIZE = 8 * 1024 * 1024;

// Max dictionary page header size in bytes. This is an estimate and only needs to be an
// upper bound.
const int MAX_DICT_HEADER_SIZE = 100;

// TODO: refactor error reporting across all scanners to be more consistent (e.g. make
// sure file name is always included, errors are reported exactly once)
// TODO: rename these macros so its easier to tell them apart

#define LOG_OR_ABORT(error_msg, runtime_state)                          \
  do {                                                                  \
    if (runtime_state->abort_on_error()) {                              \
      return Status(error_msg);                                         \
    } else {                                                            \
      runtime_state->LogError(error_msg);                               \
      return Status::OK();                                              \
    }                                                                   \
  } while (false)                                                       \


#define LOG_OR_RETURN_ON_ERROR(error_msg, runtime_state)                \
  do {                                                                  \
    if (runtime_state->abort_on_error()) {                              \
      return Status(error_msg.msg());                                   \
    }                                                                   \
    runtime_state->LogError(error_msg);                                 \
  } while (false)                                                       \

// FILE_CHECKs are conditions that we expect to be true but could fail due to a malformed
// input file. They differentiate these cases from DCHECKs, which indicate conditions that
// are true unless there's a bug in Impala. We would ideally always return a bad Status
// instead of failing a FILE_CHECK, but in many cases we use FILE_CHECK instead because
// there's a performance cost to doing the check in a release build, or just due to legacy
// code.
#define FILE_CHECK(a) DCHECK(a)
#define FILE_CHECK_EQ(a, b) DCHECK_EQ(a, b)
#define FILE_CHECK_NE(a, b) DCHECK_NE(a, b)
#define FILE_CHECK_GT(a, b) DCHECK_GT(a, b)
#define FILE_CHECK_LT(a, b) DCHECK_LT(a, b)
#define FILE_CHECK_GE(a, b) DCHECK_GE(a, b)
#define FILE_CHECK_LE(a, b) DCHECK_LE(a, b)

Status HdfsParquetScanner::IssueInitialRanges(HdfsScanNode* scan_node,
    const std::vector<HdfsFileDesc*>& files) {
  vector<DiskIoMgr::ScanRange*> footer_ranges;
  for (int i = 0; i < files.size(); ++i) {
    for (int j = 0; j < files[i]->splits.size(); ++j) {
      DiskIoMgr::ScanRange* split = files[i]->splits[j];

      // Since Parquet scanners always read entire files, only read a file if we're
      // assigned the first split to avoid reading multi-block files with multiple
      // scanners.
      if (split->offset() != 0) {
        // We are expecting each file to be one hdfs block (so all the scan range
        // offsets should be 0).  Having multiple blocks is not incorrect but is
        // nonoptimal so issue a warning.  However, if there is no impalad co-located
        // with the datanode holding the block (i.e. split->expected_local() is false),
        // then this may indicate a pseudo-HDFS system like Isilon where HDFS blocks
        // aren't meaningful from a locality standpoint.  In that case, the warning is
        // spurious so suppress it.
        if (split->expected_local()) {
          scan_node->runtime_state()->LogError(
              ErrorMsg(TErrorCode::PARQUET_MULTIPLE_BLOCKS, files[i]->filename));
        }
        // We assign the entire file to one scan range, so mark all but one split
        // (i.e. the first split) as complete
        scan_node->RangeComplete(THdfsFileFormat::PARQUET, THdfsCompression::NONE);
        continue;
      }

      // Compute the offset of the file footer
      DCHECK_GT(files[i]->file_length, 0);
      int64_t footer_size = min(static_cast<int64_t>(FOOTER_SIZE), files[i]->file_length);
      int64_t footer_start = files[i]->file_length - footer_size;

      ScanRangeMetadata* metadata =
          reinterpret_cast<ScanRangeMetadata*>(split->meta_data());
      DiskIoMgr::ScanRange* footer_range = scan_node->AllocateScanRange(
          files[i]->fs, files[i]->filename.c_str(), footer_size, footer_start,
          metadata->partition_id, split->disk_id(), split->try_cache(),
          split->expected_local(), files[i]->mtime);
      footer_ranges.push_back(footer_range);
    }
  }
  // Issue the footer ranges for all files. The same thread that processes the footer
  // will assemble the rows for this file, so mark these files added completely.
  RETURN_IF_ERROR(scan_node->AddDiskIoRanges(footer_ranges, files.size()));
  return Status::OK();
}

namespace impala {

HdfsParquetScanner::HdfsParquetScanner(HdfsScanNode* scan_node, RuntimeState* state)
    : HdfsScanner(scan_node, state),
      metadata_range_(NULL),
      dictionary_pool_(new MemPool(scan_node->mem_tracker())),
      assemble_rows_timer_(scan_node_->materialize_tuple_timer()) {
  assemble_rows_timer_.Stop();
}

HdfsParquetScanner::~HdfsParquetScanner() {
}

// TODO for 2.3: move column readers to separate file

/// Base class for reading a column. Reads a logical column, not necessarily a column
/// materialized in the file (e.g. arrays). The two subclasses are BaseScalarColumnReader
/// and CollectionColumnReader. Column readers read one def and rep level pair at a
/// time. The current def and rep level are exposed to the user, and the corresponding
/// value (if defined) can optionally be copied into a slot via ReadValue(). Can also
/// write position slots.
class HdfsParquetScanner::ColumnReader {
 public:
  virtual ~ColumnReader() { }

  int def_level() const { return def_level_; }
  int rep_level() const { return rep_level_; }

  const SlotDescriptor* slot_desc() const { return slot_desc_; }
  const parquet::SchemaElement& schema_element() const { return *node_.element; }
  int max_def_level() const { return node_.max_def_level; }
  int max_rep_level() const { return node_.max_rep_level; }
  int def_level_of_immediate_repeated_ancestor() const {
    return node_.def_level_of_immediate_repeated_ancestor;
  }
  const SlotDescriptor* pos_slot_desc() const { return pos_slot_desc_; }
  void set_pos_slot_desc(const SlotDescriptor* pos_slot_desc) {
    DCHECK(pos_slot_desc_ == NULL);
    pos_slot_desc_ = pos_slot_desc;
  }

  bool reading_collection() const { return node_.is_repeated(); }
  virtual const char* filename() const = 0;

  /// Read the current value (or null) into 'tuple' for this column. This should only be
  /// called when a value is defined, i.e., def_level() >=
  /// def_level_of_immediate_repeated_ancestor() (since empty or NULL arrays produce no
  /// output values), otherwise NextLevels() should be called instead.
  ///
  /// Advances this column reader to the next value (i.e. NextLevels() doesn't need to be
  /// called after calling ReadValue()).
  ///
  /// Returns false if execution should be aborted for some reason, e.g. parse_error_ is
  /// set, the query is cancelled, or the scan node limit was reached. Otherwise returns
  /// true.
  ///
  /// *conjuncts_passed is an in/out parameter. ReadValue() sets to false if the current
  /// row should be filtered based on this column's value. If already false on input, the
  /// row has already been filtered and ReadValue() only needs to advance the value.
  ///
  /// NextLevels() must be called on this reader before calling ReadValue() for the first
  /// time. This is to initialize the current value that ReadValue() will read.
  ///
  /// TODO: this is the function that needs to be codegen'd (e.g. CodegenReadValue())
  /// The codegened functions from all the materialized cols will then be combined
  /// into one function.
  /// TODO: another option is to materialize col by col for the entire row batch in
  /// one call.  e.g. MaterializeCol would write out 1024 values.  Our row batches
  /// are currently dense so we'll need to figure out something there.
  virtual bool ReadValue(MemPool* pool, Tuple* tuple, bool* conjuncts_passed) = 0;

  /// Advances this column reader's def and rep levels to the next logical value, i.e. to
  /// the next scalar value or the beginning of the next array, without attempting to read
  /// the value. This is used to skip past def/rep levels that don't materialize a value,
  /// such as the def/rep levels corresponding to an empty containing array.
  ///
  /// NextLevels() must be called on this reader before calling ReadValue() for the first
  /// time. This is to initialize the current value that ReadValue() will read.
  ///
  /// Returns false if execution should be aborted for some reason, e.g. parse_error_ is
  /// set, the query is cancelled, or the scan node limit was reached. Otherwise returns
  /// true.
  virtual bool NextLevels() = 0;

  /// Should only be called if pos_slot_desc_ is non-NULL. Writes pos_current_value_ to
  /// 'tuple' (i.e. "reads" the synthetic position field of the parent collection into
  /// 'tuple') and increments pos_current_value_.
  void ReadPosition(Tuple* tuple);

 protected:
  HdfsParquetScanner* parent_;
  const SchemaNode& node_;
  const SlotDescriptor* slot_desc_;

  /// The slot descriptor for the position field of the tuple, if there is one. NULL if
  /// there's not. Only one column reader for a given tuple desc will have this set.
  const SlotDescriptor* pos_slot_desc_;

  /// The next value to write into the position slot, if there is one. 64-bit int because
  /// the pos slot is always a BIGINT Set to -1 when this column reader does not have a
  /// current rep and def level (i.e. before the first NextLevels() call or after the last
  /// value in the column has been read).
  int64_t pos_current_value_;

  /// The current repetition and definition levels of this reader. Advanced via
  /// ReadValue() and NextLevels(). Set to -1 when this column reader does not have a
  /// current rep and def level (i.e. before the first NextLevels() call or after the last
  /// value in the column has been read).
  int rep_level_;
  int def_level_;

  ColumnReader(HdfsParquetScanner* parent, const SchemaNode& node,
      const SlotDescriptor* slot_desc)
    : parent_(parent),
      node_(node),
      slot_desc_(slot_desc),
      pos_slot_desc_(NULL),
      pos_current_value_(-1),
      rep_level_(-1),
      def_level_(-1) {
    DCHECK_GE(node_.max_def_level, 0);
    DCHECK_GE(node_.max_rep_level, 0);
  }
};

/// Collections are not materialized directly in parquet files; only scalar values appear
/// in the file. CollectionColumnReader uses the definition and repetition levels of child
/// column readers to figure out the boundaries of each collection in this column.
class HdfsParquetScanner::CollectionColumnReader :
      public HdfsParquetScanner::ColumnReader {
 public:
  CollectionColumnReader(HdfsParquetScanner* parent, const SchemaNode& node,
      const SlotDescriptor* slot_desc)
    : ColumnReader(parent, node, slot_desc) {
    DCHECK(node_.is_repeated());
    if (slot_desc != NULL) DCHECK(slot_desc->type().IsCollectionType());
  }

  virtual ~CollectionColumnReader() { }

  vector<ColumnReader*>* children() { return &children_; }

  virtual const char* filename() const {
    // TODO: this won't be completely accurate if/when we support columns in separate
    // files
    DCHECK(!children_.empty());
    return children_[0]->filename();
  }

  /// The repetition level indicating that the current value is the first in a new
  /// collection (meaning the last value read was the final item in the previous
  /// collection).
  int new_collection_rep_level() const { return max_rep_level() - 1; }

  /// Materializes ArrayValue into tuple slot (if materializing) and advances to next
  /// value.
  virtual bool ReadValue(MemPool* pool, Tuple* tuple, bool* conjuncts_passed);

  /// Advances all child readers to the beginning of the next collection and updates this
  /// reader's state.
  virtual bool NextLevels();

 private:
  /// Column readers of fields contained within this collection. There is at least one
  /// child reader per collection reader. Child readers either materialize slots in the
  /// collection item tuples, or there is a single child reader that does not materialize
  /// any slot and is only used by this reader to read def and rep levels.
  vector<ColumnReader*> children_;

  /// Updates this reader's def_level_, rep_level_, and pos_current_value_ based on child
  /// reader's state.
  void UpdateDerivedState();

  /// Recursively reads from children_ to assemble a single ArrayValue into *slot. Also
  /// advances rep_level_ and def_level_ via NextLevels().
  ///
  /// Returns false if execution should be aborted for some reason, e.g. parse_error_ is
  /// set, the query is cancelled, or the scan node limit was reached. Otherwise returns
  /// true.
  inline bool ReadSlot(void* slot, MemPool* pool, bool* conjuncts_passed);
};

/// Reader for a single column from the parquet file.  It's associated with a
/// ScannerContext::Stream and is responsible for decoding the data.  Super class for
/// per-type column readers. This contains most of the logic, the type specific functions
/// must be implemented in the subclass.
class HdfsParquetScanner::BaseScalarColumnReader :
      public HdfsParquetScanner::ColumnReader {
 public:
  BaseScalarColumnReader(HdfsParquetScanner* parent, const SchemaNode& node,
      const SlotDescriptor* slot_desc)
    : ColumnReader(parent, node, slot_desc),
      metadata_(NULL),
      stream_(NULL),
      decompressed_data_pool_(new MemPool(parent->scan_node_->mem_tracker())),
      num_buffered_values_(0),
      num_values_read_(0),
      bitmap_filter_(NULL),
      rows_returned_(0),
      bitmap_filter_rows_rejected_(0) {
    DCHECK_GE(node_.col_idx, 0);

    RuntimeState* state = parent_->scan_node_->runtime_state();
    hash_seed_ = state->fragment_hash_seed();
    // The bitmap filter is only valid for the top-level tuple
    if (slot_desc_ != NULL &&
        slot_desc_->parent() == parent_->scan_node_->tuple_desc()) {
      bitmap_filter_ = state->GetBitmapFilter(slot_desc_->id());
    }
  }

  virtual ~BaseScalarColumnReader() { }

  /// This is called once for each row group in the file.
  Status Reset(const parquet::ColumnMetaData* metadata, ScannerContext::Stream* stream) {
    DCHECK(stream != NULL);
    DCHECK(metadata != NULL);

    num_buffered_values_ = 0;
    data_ = NULL;
    stream_ = stream;
    metadata_ = metadata;
    dict_decoder_base_ = NULL;
    num_values_read_ = 0;
    def_level_ = -1;
    rep_level_ = -1;
    pos_current_value_ = -1;

    if (metadata_->codec != parquet::CompressionCodec::UNCOMPRESSED) {
      RETURN_IF_ERROR(Codec::CreateDecompressor(
          NULL, false, PARQUET_TO_IMPALA_CODEC[metadata_->codec], &decompressor_));
    }
    return Status::OK();
  }

  /// Called once when the scanner is complete for final cleanup.
  void Close() {
    if (decompressor_.get() != NULL) decompressor_->Close();
  }

  int64_t total_len() const { return metadata_->total_compressed_size; }
  int col_idx() const { return node_.col_idx; }
  THdfsCompression::type codec() const {
    if (metadata_ == NULL) return THdfsCompression::NONE;
    return PARQUET_TO_IMPALA_CODEC[metadata_->codec];
  }
  MemPool* decompressed_data_pool() const { return decompressed_data_pool_.get(); }

  virtual const char* filename() const { return stream_->filename(); }

  /// Reads the next definition and repetition levels for this column. Initializes the
  /// next data page if necessary.
  virtual bool NextLevels();

  // TODO: Some encodings might benefit a lot from a SkipValues(int num_rows) if
  // we know this row can be skipped. This could be very useful with stats and big
  // sections can be skipped. Implement that when we can benefit from it.

 protected:
  // Friend parent scanner so it can perform validation (e.g. ValidateEndOfRowGroup())
  friend class HdfsParquetScanner;

  const parquet::ColumnMetaData* metadata_;
  scoped_ptr<Codec> decompressor_;
  ScannerContext::Stream* stream_;

  /// Pool to allocate decompression buffers from.
  boost::scoped_ptr<MemPool> decompressed_data_pool_;

  /// Header for current data page.
  parquet::PageHeader current_page_header_;

  /// Num values remaining in the current data page
  int num_buffered_values_;

  /// Pointer to start of next value in data page
  uint8_t* data_;

  /// Decoder for definition levels. Only one of these is valid at a time, depending on
  /// the data page metadata.
  RleDecoder rle_def_levels_;
  BitReader bit_packed_def_levels_;

  /// Decoder for repetition levels. Only one of these is valid at a time, depending on
  /// the data page metadata.
  RleDecoder rle_rep_levels_;
  BitReader bit_packed_rep_levels_;

  /// Decoder for dictionary-encoded columns. Set by the subclass.
  DictDecoderBase* dict_decoder_base_;

  /// The number of values seen so far. Updated per data page.
  int64_t num_values_read_;

  /// Cache of the bitmap_filter_ (if any) for this slot.
  const Bitmap* bitmap_filter_;
  /// Cache of hash_seed_ to use with bitmap_filter_.
  uint32_t hash_seed_;

  /// Bitmap filters are optional (i.e. they can be ignored and the results will be
  /// correct). Keep track of stats to determine if the filter is not effective. If the
  /// number of rows filtered out is too low, this is not worth the cost.
  /// TODO: this should be cost based taking into account how much we save when we
  /// filter a row.
  int64_t rows_returned_;
  int64_t bitmap_filter_rows_rejected_;

  /// Read the next data page.  If a dictionary page is encountered, that will be read and
  /// this function will continue reading for the next data page.
  Status ReadDataPage();

  /// Returns the definition level for the next value
  /// Returns -1 and sets parse_status_ if there was a error parsing it.
  int ReadDefinitionLevel();

  /// Returns the repetition level for the next value
  /// Returns -1 and sets parse_status_ if there was a error parsing it.
  int ReadRepetitionLevel();

  /// Creates a dictionary decoder from values/size. Subclass must implement this
  /// and set dict_decoder_base_.
  virtual void CreateDictionaryDecoder(uint8_t* values, int size) = 0;

  /// Initializes the reader with the data contents. This is the content for the entire
  /// decompressed data page. Decoders can initialize state from here.
  virtual Status InitDataPage(uint8_t* data, int size) = 0;

 private:
  /// Helper method for creating definition and repetition level decoders
  Status InitLevelDecoders(parquet::Encoding::type encoding, int max_level,
      uint8_t** data, int* data_size, RleDecoder* rle_decoder, BitReader* bit_reader);

  /// Helper method for ReadDefinitionLevel() and ReadRepetitionLevel().
  int ReadLevel(parquet::Encoding::type encoding, RleDecoder* rle_decoder,
      BitReader* bit_reader);

  // Pull out slow-path Status construction code from ReadRepetitionLevel()/
  // ReadDefinitionLevel() for performance.
  void __attribute__((noinline)) SetLevelError(TErrorCode::type error_code) {
    parent_->parse_status_ =
        Status(error_code, num_buffered_values_, stream_->filename());
  }

  /// Writes the next value into *slot using pool if necessary. Also advances rep_level_
  /// and def_level_ via NextLevels().
  ///
  /// Returns false if execution should be aborted for some reason, e.g. parse_error_ is
  /// set, the query is cancelled, or the scan node limit was reached. Otherwise returns
  /// true.
  inline bool ReadSlot(void* slot, MemPool* pool, bool* conjuncts_passed);
};

/// Per column type reader. If MATERIALIZED is true, the column values are materialized
/// into the slot described by slot_desc. If MATERIALIZED is false, the column values
/// are not materialized, but the position can be accessed.
template<typename T, bool MATERIALIZED>
class HdfsParquetScanner::ScalarColumnReader :
      public HdfsParquetScanner::BaseScalarColumnReader {
 public:
  ScalarColumnReader(HdfsParquetScanner* parent, const SchemaNode& node,
      const SlotDescriptor* slot_desc)
    : BaseScalarColumnReader(parent, node, slot_desc) {
    if (!MATERIALIZED) {
      // We're not materializing any values, just counting them. No need (or ability) to
      // initialize state used to materialize values.
      DCHECK(slot_desc_ == NULL);
      return;
    }

    DCHECK(slot_desc_ != NULL);
    DCHECK_NE(slot_desc_->type().type, TYPE_BOOLEAN);
    if (slot_desc_->type().type == TYPE_DECIMAL) {
      fixed_len_size_ = ParquetPlainEncoder::DecimalSize(slot_desc_->type());
    } else if (slot_desc_->type().type == TYPE_VARCHAR) {
      fixed_len_size_ = slot_desc_->type().len;
    } else {
      fixed_len_size_ = -1;
    }
    needs_conversion_ = slot_desc_->type().type == TYPE_CHAR ||
        // TODO: Add logic to detect file versions that have unconverted TIMESTAMP
        // values. Currently all versions have converted values.
        (FLAGS_convert_legacy_hive_parquet_utc_timestamps &&
        slot_desc_->type().type == TYPE_TIMESTAMP &&
        parent->file_version_.application == "parquet-mr");
  }

  virtual ~ScalarColumnReader() { }

  virtual bool ReadValue(MemPool* pool, Tuple* tuple, bool* conjuncts_passed) {
    DCHECK_GE(rep_level_, 0);
    DCHECK_GE(def_level_, 0);
    DCHECK_GE(def_level_, def_level_of_immediate_repeated_ancestor()) <<
        "Caller should have called NextLevels() until we are ready to read a value";

    if (!MATERIALIZED) {
      return BaseScalarColumnReader::NextLevels();
    } else if (def_level_ >= max_def_level()) {
      return ReadSlot(tuple->GetSlot(slot_desc()->tuple_offset()), pool, conjuncts_passed);
    } else {
      // Null value
      tuple->SetNull(slot_desc()->null_indicator_offset());
      return BaseScalarColumnReader::NextLevels();
    }
  }

 protected:
  virtual void CreateDictionaryDecoder(uint8_t* values, int size) {
    dict_decoder_.reset(new DictDecoder<T>(values, size, fixed_len_size_));
    dict_decoder_base_ = dict_decoder_.get();
  }

  virtual Status InitDataPage(uint8_t* data, int size) {
    // If slot_desc_ is NULL, dict_decoder_ is uninitialized
    if (current_page_header_.data_page_header.encoding ==
          parquet::Encoding::PLAIN_DICTIONARY && slot_desc_ != NULL) {
      if (dict_decoder_.get() == NULL) {
        return Status("File corrupt. Missing dictionary page.");
      }
      dict_decoder_->SetData(data, size);
    }

    // Check if we should disable the bitmap filter. We'll do this if the filter
    // is not removing a lot of rows.
    // TODO: how to pick the selectivity?
    if (bitmap_filter_ != NULL && rows_returned_ > 10000 &&
        bitmap_filter_rows_rejected_ < rows_returned_ * .1) {
      bitmap_filter_ = NULL;
    }
    return Status::OK();
  }

 private:
  /// Writes the next value into *slot using pool if necessary. Also advances rep_level_
  /// and def_level_ via NextLevels().
  ///
  /// Returns false if execution should be aborted for some reason, e.g. parse_error_ is
  /// set, the query is cancelled, or the scan node limit was reached. Otherwise returns
  /// true.
  inline bool ReadSlot(void* slot, MemPool* pool, bool* conjuncts_passed) {
    // NextLevels() should have already been called and def and rep levels should be in
    // valid range.
    // TODO(skye): these could be DCHECKs if we check upper bound in Read*Level(). Add
    // check if not perf sensitive (here and other ReadSlot()/NextLevels() definitions)
    FILE_CHECK_GE(rep_level_, 0);
    FILE_CHECK_LE(rep_level_, max_rep_level());
    FILE_CHECK_GE(def_level_, 0);
    FILE_CHECK_LE(def_level_, max_def_level());

    parquet::Encoding::type page_encoding =
        current_page_header_.data_page_header.encoding;
    T val;
    T* val_ptr = needs_conversion_ ? &val : reinterpret_cast<T*>(slot);
    if (page_encoding == parquet::Encoding::PLAIN_DICTIONARY) {
      if (UNLIKELY(!dict_decoder_->GetValue(val_ptr))) {
        SetDictDecodeError();
        return false;
      }
    } else {
      FILE_CHECK_EQ(page_encoding, parquet::Encoding::PLAIN);
      data_ += ParquetPlainEncoder::Decode<T>(data_, fixed_len_size_, val_ptr);
    }
    if (needs_conversion_) ConvertSlot(&val, reinterpret_cast<T*>(slot), pool);
    ++rows_returned_;
    if (*conjuncts_passed && bitmap_filter_ != NULL) {
      uint32_t h = RawValue::GetHashValue(slot, slot_desc()->type(), hash_seed_);
      *conjuncts_passed = bitmap_filter_->Get<true>(h);
      ++bitmap_filter_rows_rejected_;
    }
    return BaseScalarColumnReader::NextLevels();
  }

  void CopySlot(T* slot, MemPool* pool) {
    // no-op for non-string columns.
  }

  /// Converts and writes src into dst based on desc_->type()
  void ConvertSlot(const T* src, T* dst, MemPool* pool) {
    DCHECK(false);
  }

  // Pull out slow-path Status construction code from ReadRepetitionLevel()/
  // ReadDefinitionLevel() for performance.
  void __attribute__((noinline)) SetDictDecodeError() {
    parent_->parse_status_ =
        Status(TErrorCode::PARQUET_DICT_DECODE_FAILURE, stream_->filename());
  }

  scoped_ptr<DictDecoder<T> > dict_decoder_;

  // true decoded values must be converted before being written to an output tuple
  bool needs_conversion_;

  // The size of this column with plain encoding for FIXED_LEN_BYTE_ARRAY, or
  // the max length for VARCHAR columns. Unused otherwise.
  int fixed_len_size_;
};

template<>
void HdfsParquetScanner::ScalarColumnReader<StringValue, true>::CopySlot(
    StringValue* slot, MemPool* pool) {
  if (slot->len == 0) return;
  uint8_t* buffer = pool->Allocate(slot->len);
  memcpy(buffer, slot->ptr, slot->len);
  slot->ptr = reinterpret_cast<char*>(buffer);
}

template<>
void HdfsParquetScanner::ScalarColumnReader<StringValue, true>::ConvertSlot(
    const StringValue* src, StringValue* dst, MemPool* pool) {
  DCHECK(slot_desc() != NULL);
  DCHECK(slot_desc()->type().type == TYPE_CHAR);
  int len = slot_desc()->type().len;
  StringValue sv;
  sv.len = len;
  if (slot_desc()->type().IsVarLenStringType()) {
    sv.ptr = reinterpret_cast<char*>(pool->Allocate(len));
  } else {
    sv.ptr = reinterpret_cast<char*>(dst);
  }
  int unpadded_len = min(len, src->len);
  memcpy(sv.ptr, src->ptr, unpadded_len);
  StringValue::PadWithSpaces(sv.ptr, len, unpadded_len);

  if (slot_desc()->type().IsVarLenStringType()) *dst = sv;
}

template<>
void HdfsParquetScanner::ScalarColumnReader<TimestampValue, true>::ConvertSlot(
    const TimestampValue* src, TimestampValue* dst, MemPool* pool) {
  // Conversion should only happen when this flag is enabled.
  DCHECK(FLAGS_convert_legacy_hive_parquet_utc_timestamps);
  *dst = *src;
  if (dst->HasDateAndTime()) dst->UtcToLocal();
}

class HdfsParquetScanner::BoolColumnReader :
      public HdfsParquetScanner::BaseScalarColumnReader {
 public:
  BoolColumnReader(HdfsParquetScanner* parent, const SchemaNode& node,
      const SlotDescriptor* slot_desc)
    : BaseScalarColumnReader(parent, node, slot_desc) {
    if (slot_desc_ != NULL) DCHECK_EQ(slot_desc_->type().type, TYPE_BOOLEAN);
  }

  virtual ~BoolColumnReader() { }

  virtual bool ReadValue(MemPool* pool, Tuple* tuple, bool* conjuncts_passed) {
    DCHECK(slot_desc_ != NULL);
    DCHECK_GE(rep_level_, 0);
    DCHECK_GE(def_level_, 0);
    DCHECK_GE(def_level_, def_level_of_immediate_repeated_ancestor()) <<
        "Caller should have called NextLevels() until we are ready to read a value";

    if (def_level_ >= max_def_level()) {
      return ReadSlot(tuple->GetSlot(slot_desc()->tuple_offset()), pool, conjuncts_passed);
    } else {
      // Null value
      tuple->SetNull(slot_desc()->null_indicator_offset());
      return BaseScalarColumnReader::NextLevels();
    }
  }
 protected:
  virtual void CreateDictionaryDecoder(uint8_t* values, int size) {
    DCHECK(false) << "Dictionary encoding is not supported for bools. Should never "
                  << "have gotten this far.";
  }

  virtual Status InitDataPage(uint8_t* data, int size) {
    // Initialize bool decoder
    bool_values_ = BitReader(data, size);
    return Status::OK();
  }

 private:
  /// Writes the next value into *slot using pool if necessary. Also advances rep_level_
  /// and def_level_ via NextLevels().
  ///
  /// Returns false if execution should be aborted for some reason, e.g. parse_error_ is
  /// set, the query is cancelled, or the scan node limit was reached. Otherwise returns
  /// true.
  inline bool ReadSlot(void* slot, MemPool* pool, bool* conjuncts_passed)  {
    // Def and rep levels should be in valid range.
    FILE_CHECK_GE(rep_level_, 0);
    FILE_CHECK_LE(rep_level_, max_rep_level());
    FILE_CHECK_GE(def_level_, 0);
    FILE_CHECK_LE(def_level_, max_def_level());

    if (!bool_values_.GetValue(1, reinterpret_cast<bool*>(slot))) {
      parent_->parse_status_ = Status("Invalid bool column.");
      return false;
    }
    return BaseScalarColumnReader::NextLevels();
  }

  BitReader bool_values_;
};

}

Status HdfsParquetScanner::Prepare(ScannerContext* context) {
  RETURN_IF_ERROR(HdfsScanner::Prepare(context));
  num_cols_counter_ =
      ADD_COUNTER(scan_node_->runtime_profile(), "NumColumns", TUnit::UNIT);

  scan_node_->IncNumScannersCodegenDisabled();
  return Status::OK();
}

void HdfsParquetScanner::Close() {
  vector<THdfsCompression::type> compression_types;

  // Visit each column reader, including collection reader children.
  stack<ColumnReader*> readers;
  BOOST_FOREACH(ColumnReader* r, column_readers_) readers.push(r);
  while (!readers.empty()) {
    ColumnReader* col_reader = readers.top();
    readers.pop();

    if (col_reader->reading_collection()) {
      CollectionColumnReader* collection_reader =
          static_cast<CollectionColumnReader*>(col_reader);
      BOOST_FOREACH(ColumnReader* r, *collection_reader->children()) readers.push(r);
      continue;
    }

    BaseScalarColumnReader* scalar_reader =
        static_cast<BaseScalarColumnReader*>(col_reader);
    if (scalar_reader->decompressed_data_pool() != NULL) {
      // No need to commit the row batches with the AttachPool() calls
      // since AddFinalRowBatch() already does below.
      AttachPool(scalar_reader->decompressed_data_pool(), false);
    }
    scalar_reader->Close();
    compression_types.push_back(scalar_reader->codec());
  }
  AttachPool(dictionary_pool_.get(), false);
  AddFinalRowBatch();

  // If this was a metadata only read (i.e. count(*)), there are no columns.
  if (compression_types.empty()) compression_types.push_back(THdfsCompression::NONE);
  scan_node_->RangeComplete(THdfsFileFormat::PARQUET, compression_types);
  assemble_rows_timer_.Stop();
  assemble_rows_timer_.ReleaseCounter();

  HdfsScanner::Close();
}

HdfsParquetScanner::ColumnReader* HdfsParquetScanner::CreateReader(
    const SchemaNode& node, const SlotDescriptor* slot_desc) {
  ColumnReader* reader = NULL;
  if (node.is_repeated()) {
    // Create collection reader (note this handles both NULL and non-NULL 'slot_desc')
    reader = new CollectionColumnReader(this, node, slot_desc);
  } else if (slot_desc != NULL) {
    // Create the appropriate ScalarColumnReader type to read values into 'slot_desc'
    switch (slot_desc->type().type) {
      case TYPE_BOOLEAN:
        reader = new BoolColumnReader(this, node, slot_desc);
        break;
      case TYPE_TINYINT:
        reader = new ScalarColumnReader<int8_t, true>(this, node, slot_desc);
        break;
      case TYPE_SMALLINT:
        reader = new ScalarColumnReader<int16_t, true>(this, node, slot_desc);
        break;
      case TYPE_INT:
        reader = new ScalarColumnReader<int32_t, true>(this, node, slot_desc);
        break;
      case TYPE_BIGINT:
        reader = new ScalarColumnReader<int64_t, true>(this, node, slot_desc);
        break;
      case TYPE_FLOAT:
        reader = new ScalarColumnReader<float, true>(this, node, slot_desc);
        break;
      case TYPE_DOUBLE:
        reader = new ScalarColumnReader<double, true>(this, node, slot_desc);
        break;
      case TYPE_TIMESTAMP:
        reader = new ScalarColumnReader<TimestampValue, true>(this, node, slot_desc);
        break;
      case TYPE_STRING:
      case TYPE_VARCHAR:
      case TYPE_CHAR:
        reader = new ScalarColumnReader<StringValue, true>(this, node, slot_desc);
        break;
      case TYPE_DECIMAL:
        switch (slot_desc->type().GetByteSize()) {
          case 4:
            reader = new ScalarColumnReader<Decimal4Value, true>(this, node, slot_desc);
            break;
          case 8:
            reader = new ScalarColumnReader<Decimal8Value, true>(this, node, slot_desc);
            break;
          case 16:
            reader = new ScalarColumnReader<Decimal16Value, true>(this, node, slot_desc);
            break;
        }
        break;
      default:
        DCHECK(false);
    }
  } else {
    // Special case for counting scalar values (e.g. count(*), no materialized columns in
    // the file, only materializing a position slot). We won't actually read any values,
    // only the rep and def levels, so it doesn't matter what kind of reader we make.
    reader = new ScalarColumnReader<int8_t, false>(this, node, slot_desc);
  }
  return scan_node_->runtime_state()->obj_pool()->Add(reader);
}

inline void HdfsParquetScanner::ColumnReader::ReadPosition(Tuple* tuple) {
  DCHECK(pos_slot_desc() != NULL);
  // NextLevels() should have already been called
  DCHECK_GE(rep_level_, 0);
  DCHECK_GE(def_level_, 0);
  DCHECK_GE(pos_current_value_, 0);
  DCHECK_GE(def_level_, def_level_of_immediate_repeated_ancestor()) <<
      "Caller should have called NextLevels() until we are ready to read a value";

  void* slot = tuple->GetSlot(pos_slot_desc()->tuple_offset());
  *reinterpret_cast<int64_t*>(slot) = pos_current_value_++;
}

// In 1.1, we had a bug where the dictionary page metadata was not set. Returns true
// if this matches those versions and compatibility workarounds need to be used.
static bool RequiresSkippedDictionaryHeaderCheck(
    const HdfsParquetScanner::FileVersion& v) {
  if (v.application != "impala") return false;
  return v.VersionEq(1,1,0) || (v.VersionEq(1,2,0) && v.is_impala_internal);
}

Status HdfsParquetScanner::BaseScalarColumnReader::ReadDataPage() {
  Status status;
  uint8_t* buffer;

  // We're about to move to the next data page.  The previous data page is
  // now complete, pass along the memory allocated for it.
  parent_->AttachPool(decompressed_data_pool_.get(), false);

  // Read the next data page, skipping page types we don't care about.
  // We break out of this loop on the non-error case (a data page was found or we read all
  // the pages).
  while (true) {
    DCHECK_EQ(num_buffered_values_, 0);
    if (num_values_read_ == metadata_->num_values) {
      // No more pages to read
      // TODO: should we check for stream_->eosr()?
      break;
    } else if (num_values_read_ > metadata_->num_values) {
      // The data pages contain more values than stated in the column metadata.
      return Status(TErrorCode::PARQUET_COLUMN_METADATA_INVALID,
         metadata_->num_values, num_values_read_, node_.element->name,
         stream_->filename());
    }

    int64_t buffer_size;
    RETURN_IF_ERROR(stream_->GetBuffer(true, &buffer, &buffer_size));
    if (buffer_size == 0) {
      // The data pages contain fewer values than stated in the column metadata.
      DCHECK(stream_->eosr());
      DCHECK_LT(num_values_read_, metadata_->num_values);
      // TODO for 2.3: node_.element->name isn't necessarily useful
      return Status(TErrorCode::PARQUET_COLUMN_METADATA_INVALID, metadata_->num_values,
          num_values_read_, node_.element->name, stream_->filename());
    }

    // We don't know the actual header size until the thrift object is deserialized.  Loop
    // until we successfully deserialize the header or exceed the maximum header size.
    uint32_t header_size;
    while (true) {
      header_size = buffer_size;
      status = DeserializeThriftMsg(
          buffer, &header_size, true, &current_page_header_);
      if (status.ok()) break;

      if (buffer_size >= MAX_PAGE_HEADER_SIZE) {
        stringstream ss;
        ss << "ParquetScanner: could not read data page because page header exceeded "
           << "maximum size of "
           << PrettyPrinter::Print(MAX_PAGE_HEADER_SIZE, TUnit::BYTES);
        status.AddDetail(ss.str());
        return status;
      }

      // Didn't read entire header, increase buffer size and try again
      Status status;
      int64_t new_buffer_size = max(buffer_size * 2, 1024L);
      bool success = stream_->GetBytes(
          new_buffer_size, &buffer, &new_buffer_size, &status, /* peek */ true);
      if (!success) {
        DCHECK(!status.ok());
        return status;
      }
      DCHECK(status.ok());

      if (buffer_size == new_buffer_size) {
        DCHECK_NE(new_buffer_size, 0);
        return Status(TErrorCode::PARQUET_HEADER_EOF, stream_->filename());
      }
      DCHECK_GT(new_buffer_size, buffer_size);
      buffer_size = new_buffer_size;
    }

    // Successfully deserialized current_page_header_
    if (!stream_->SkipBytes(header_size, &status)) return status;

    int data_size = current_page_header_.compressed_page_size;
    int uncompressed_size = current_page_header_.uncompressed_page_size;

    if (current_page_header_.type == parquet::PageType::DICTIONARY_PAGE) {
      if (slot_desc_ == NULL) {
        // Skip processing the dictionary page if we don't need to decode any values. In
        // addition to being unnecessary, we are likely unable to successfully decode the
        // dictionary values because we don't necessarily create the right type of scalar
        // reader if there's no slot to read into (see CreateReader()).
        if (!stream_->ReadBytes(data_size, &data_, &status)) return status;
        continue;
      }

      if (dict_decoder_base_ != NULL) {
        return Status("Column chunk should not contain two dictionary pages.");
      }
      if (node_.element->type == parquet::Type::BOOLEAN) {
        return Status("Unexpected dictionary page. Dictionary page is not"
            " supported for booleans.");
      }
      const parquet::DictionaryPageHeader* dict_header = NULL;
      if (current_page_header_.__isset.dictionary_page_header) {
        dict_header = &current_page_header_.dictionary_page_header;
      } else {
        if (!RequiresSkippedDictionaryHeaderCheck(parent_->file_version_)) {
          return Status("Dictionary page does not have dictionary header set.");
        }
      }
      if (dict_header != NULL &&
          dict_header->encoding != parquet::Encoding::PLAIN &&
          dict_header->encoding != parquet::Encoding::PLAIN_DICTIONARY) {
        return Status("Only PLAIN and PLAIN_DICTIONARY encodings are supported "
            "for dictionary pages.");
      }

      if (!stream_->ReadBytes(data_size, &data_, &status)) return status;

      uint8_t* dict_values = NULL;
      if (decompressor_.get() != NULL) {
        dict_values = parent_->dictionary_pool_->Allocate(uncompressed_size);
        RETURN_IF_ERROR(decompressor_->ProcessBlock32(true, data_size, data_,
            &uncompressed_size, &dict_values));
        VLOG_FILE << "Decompressed " << data_size << " to " << uncompressed_size;
        data_size = uncompressed_size;
      } else {
        FILE_CHECK_EQ(data_size, current_page_header_.uncompressed_page_size);
        // Copy dictionary from io buffer (which will be recycled as we read
        // more data) to a new buffer
        dict_values = parent_->dictionary_pool_->Allocate(data_size);
        memcpy(dict_values, data_, data_size);
      }

      CreateDictionaryDecoder(dict_values, data_size);
      if (dict_header != NULL &&
          dict_header->num_values != dict_decoder_base_->num_entries()) {
        return Status(Substitute(
            "Invalid dictionary. Expected $0 entries but data contained $1 entries",
            dict_header->num_values, dict_decoder_base_->num_entries()));
      }
      // Done with dictionary page, read next page
      continue;
    }

    if (current_page_header_.type != parquet::PageType::DATA_PAGE) {
      // We can safely skip non-data pages
      if (!stream_->SkipBytes(data_size, &status)) return status;
      continue;
    }

    // Read Data Page
    if (!stream_->ReadBytes(data_size, &data_, &status)) return status;
    num_buffered_values_ = current_page_header_.data_page_header.num_values;
    num_values_read_ += num_buffered_values_;

    if (decompressor_.get() != NULL) {
      SCOPED_TIMER(parent_->decompress_timer_);
      uint8_t* decompressed_buffer = decompressed_data_pool_->Allocate(uncompressed_size);
      RETURN_IF_ERROR(decompressor_->ProcessBlock32(true,
          current_page_header_.compressed_page_size, data_, &uncompressed_size,
          &decompressed_buffer));
      VLOG_FILE << "Decompressed " << current_page_header_.compressed_page_size
                << " to " << uncompressed_size;
      FILE_CHECK_EQ(current_page_header_.uncompressed_page_size, uncompressed_size);
      data_ = decompressed_buffer;
      data_size = current_page_header_.uncompressed_page_size;
    } else {
      DCHECK_EQ(metadata_->codec, parquet::CompressionCodec::UNCOMPRESSED);
      FILE_CHECK_EQ(current_page_header_.compressed_page_size, uncompressed_size);
    }

    if (max_rep_level() > 0) {
      // Initialize the repetition level data
      InitLevelDecoders(current_page_header_.data_page_header.repetition_level_encoding,
          max_rep_level(), &data_, &data_size, &rle_rep_levels_, &bit_packed_rep_levels_);
    }

    if (max_def_level() > 0) {
      // Initialize the definition level data
      InitLevelDecoders(current_page_header_.data_page_header.definition_level_encoding,
          max_def_level(), &data_, &data_size, &rle_def_levels_, &bit_packed_def_levels_);
    }

    // Data can be empty if the column contains all NULLs
    if (data_size != 0) RETURN_IF_ERROR(InitDataPage(data_, data_size));
    break;
  }

  return Status::OK();
}

Status HdfsParquetScanner::BaseScalarColumnReader::InitLevelDecoders(
    parquet::Encoding::type encoding, int max_level, uint8_t** data, int* data_size,
    RleDecoder* rle_decoder, BitReader* bit_reader) {
  int32_t num_bytes = 0;
  switch (encoding) {
    case parquet::Encoding::RLE: {
      Status status;
      if (!ReadWriteUtil::Read(data, data_size, &num_bytes, &status)) {
        return status;
      }
      if (num_bytes < 0) {
        return Status(TErrorCode::PARQUET_CORRUPT_VALUE, stream_->filename(), num_bytes);
      }
      int bit_width = BitUtil::Log2(max_level + 1);
      *rle_decoder = RleDecoder(*data, num_bytes, bit_width);
      break;
    }
    case parquet::Encoding::BIT_PACKED:
      num_bytes = BitUtil::Ceil(num_buffered_values_, 8);
      *bit_reader = BitReader(*data, num_bytes);
      break;
    default: {
      stringstream ss;
      ss << "Unsupported encoding: " << encoding;
      return Status(ss.str());
    }
  }
  DCHECK_GT(num_bytes, 0);
  *data += num_bytes;
  *data_size -= num_bytes;
  return Status::OK();
}

// TODO More codegen here as well.
inline int HdfsParquetScanner::BaseScalarColumnReader::ReadLevel(
    parquet::Encoding::type encoding, RleDecoder* rle_decoder, BitReader* bit_reader) {
  uint8_t level;
  bool valid = false;
  switch (encoding) {
    case parquet::Encoding::RLE:
      valid = rle_decoder->Get(&level);
      break;
    case parquet::Encoding::BIT_PACKED: {
      valid = bit_reader->GetValue(1, &level);
      break;
    }
    default:
      FILE_CHECK(false);
  }
  if (!valid) return -1;
  return level;
}

// TODO(skye): try reading + caching many levels at once to avoid error checking etc on
// each call (here and RLE decoder) for perf
inline int HdfsParquetScanner::BaseScalarColumnReader::ReadDefinitionLevel() {
  if (max_def_level() == 0) {
    // This column and any containing structs are required so there is nothing encoded for
    // the definition levels.
    return 0;
  }
  int def_level = ReadLevel(
      current_page_header_.data_page_header.definition_level_encoding,
      &rle_def_levels_, &bit_packed_def_levels_);

  if (UNLIKELY(def_level < 0)) {
    SetLevelError(TErrorCode::PARQUET_DEF_LEVEL_ERROR);
    return -1;
  }
  FILE_CHECK_LE(def_level_, max_def_level());
  return def_level;
}

inline int HdfsParquetScanner::BaseScalarColumnReader::ReadRepetitionLevel() {
  if (max_rep_level() == 0) {
    // This column is not nested in any collection types so there is nothing encoded for
    // the repetition levels.
    return 0;
  }
  int rep_level = ReadLevel(
      current_page_header_.data_page_header.repetition_level_encoding,
      &rle_rep_levels_, &bit_packed_rep_levels_);

  if (UNLIKELY(rep_level < 0)) {
    SetLevelError(TErrorCode::PARQUET_REP_LEVEL_ERROR);
    return -1;
  }
  FILE_CHECK_LE(rep_level, max_rep_level());
  return rep_level;
}

bool HdfsParquetScanner::BaseScalarColumnReader::NextLevels() {
  if (UNLIKELY(num_buffered_values_ == 0)) {
    parent_->assemble_rows_timer_.Stop();
    parent_->parse_status_ = ReadDataPage();
    if (!parent_->parse_status_.ok()) return false;
    if (num_buffered_values_ == 0) {
      rep_level_ = -1;
      def_level_ = -1;
      pos_current_value_ = -1;
      return true;
    }
    parent_->assemble_rows_timer_.Start();
  }
  --num_buffered_values_;

  rep_level_ = ReadRepetitionLevel();
  def_level_ = ReadDefinitionLevel();

  // Reset position counter if we are at the start of a new parent collection.
  if (rep_level_ <= max_rep_level() - 1) pos_current_value_ = 0;

  return parent_->parse_status_.ok();
}

bool HdfsParquetScanner::CollectionColumnReader::NextLevels() {
  DCHECK(!children_.empty());
  DCHECK_LE(rep_level_, new_collection_rep_level());
  for (int c = 0; c < children_.size(); ++c) {
    do {
      // TODO(skye): verify somewhere that all column readers are at end
      if (!children_[c]->NextLevels()) return false;
    } while (children_[c]->rep_level() > new_collection_rep_level());
  }
  UpdateDerivedState();
  return true;
}

bool HdfsParquetScanner::CollectionColumnReader::ReadValue(
    MemPool* pool, Tuple* tuple, bool* conjuncts_passed) {
  DCHECK_GE(rep_level_, 0);
  DCHECK_GE(def_level_, 0);
  DCHECK_GE(def_level_, def_level_of_immediate_repeated_ancestor()) <<
      "Caller should have called NextLevels() until we are ready to read a value";

  if (slot_desc_ == NULL) {
    return CollectionColumnReader::NextLevels();
  } if (def_level_ >= max_def_level()) {
    return ReadSlot(tuple->GetSlot(slot_desc()->tuple_offset()), pool, conjuncts_passed);
  } else {
    // Null value
    tuple->SetNull(slot_desc()->null_indicator_offset());
    return CollectionColumnReader::NextLevels();
  }
}

// TODO for 2.3: test query where *conjuncts_passed == false
bool HdfsParquetScanner::CollectionColumnReader::ReadSlot(
    void* slot, MemPool* pool, bool* conjuncts_passed) {
  DCHECK(!children_.empty());
  DCHECK_LE(rep_level_, new_collection_rep_level());

  // TODO: do something with conjuncts_passed? We still need to "read" the value in order
  // to advance children_ but we don't need to materialize the array.

  // Recursively read the collection into a new ArrayValue.
  ArrayValue* array_slot = reinterpret_cast<ArrayValue*>(slot);
  *array_slot = ArrayValue();
  ArrayValueBuilder builder(array_slot, *slot_desc_->collection_item_descriptor(), pool);
  bool continue_execution = parent_->AssembleRows<true, true>(
      slot_desc_->collection_item_descriptor(), children_, new_collection_rep_level(), -1,
      &builder);
  if (!continue_execution) return false;

  // AssembleRows() advances child readers, so we don't need to call NextLevels()
  UpdateDerivedState();
  return true;
}

void HdfsParquetScanner::CollectionColumnReader::UpdateDerivedState() {
  // We don't need to cap our def_level_ at max_def_level(). We always check def_level_
  // >= max_def_level() to check if the collection is defined.
  // TODO(skye): consider capping def_level_ at max_def_level()
  def_level_ = children_[0]->def_level();
  rep_level_ = children_[0]->rep_level();

  // All children should have been advanced to the beginning of the next collection
  for (int i = 0; i < children_.size(); ++i) {
    DCHECK_EQ(children_[i]->rep_level(), rep_level_);
    if (def_level_ < max_def_level()) {
      // Array not defined
      FILE_CHECK_EQ(children_[i]->def_level(), def_level_);
    } else {
      // Array is defined
      FILE_CHECK_GE(children_[i]->def_level(), max_def_level());
    }
  }

  if (rep_level_ == -1) {
    // No more values
    pos_current_value_ = -1;
  } else if (rep_level_ <= max_rep_level() - 2) {
    // Reset position counter if we are at the start of a new parent collection (i.e.,
    // the current collection is the first item in a new parent collection).
    pos_current_value_ = 0;
  }
}

Status HdfsParquetScanner::ProcessSplit() {
  // First process the file metadata in the footer
  bool eosr;
  RETURN_IF_ERROR(ProcessFooter(&eosr));
  if (eosr) return Status::OK();

  // We've processed the metadata and there are columns that need to be materialized.
  RETURN_IF_ERROR(CreateColumnReaders(*scan_node_->tuple_desc(), &column_readers_));

  // The scanner-wide stream was used only to read the file footer.  Each column has added
  // its own stream.
  stream_ = NULL;

  // Iterate through each row group in the file and read all the materialized columns
  // per row group.  Row groups are independent, so this this could be parallelized.
  // However, having multiple row groups per file should be seen as an edge case and
  // we can do better parallelizing across files instead.
  // TODO: not really an edge case since MR writes multiple row groups
  for (int i = 0; i < file_metadata_.row_groups.size(); ++i) {
    // Attach any resources and clear the streams before starting a new row group. These
    // streams could either be just the footer stream or streams for the previous row
    // group.
    context_->ReleaseCompletedResources(batch_, /* done */ true);
    // Commit the rows to flush the row batch from the previous row group
    CommitRows(0);

    RETURN_IF_ERROR(InitColumns(i, column_readers_));

    assemble_rows_timer_.Start();

    // Prepare column readers for first read
    bool continue_execution = true;
    BOOST_FOREACH(ColumnReader* col_reader, column_readers_) {
      continue_execution |= col_reader->NextLevels();
      if (!continue_execution) break;
    }

    bool in_collection =
      column_readers_[0]->def_level_of_immediate_repeated_ancestor() > 0;

    continue_execution = in_collection ?
        AssembleRows<true, false>(scan_node_->tuple_desc(), column_readers_, -1, i,
            NULL) :
        AssembleRows<false, false>(scan_node_->tuple_desc(), column_readers_, -1, i,
            NULL);
    assemble_rows_timer_.Stop();

    if (parse_status_.IsMemLimitExceeded()) return parse_status_;
    if (!parse_status_.ok()) LOG_OR_RETURN_ON_ERROR(parse_status_.msg(), state_);

    if (scan_node_->ReachedLimit()) return Status::OK();
    if (context_->cancelled()) return Status::OK();
    RETURN_IF_ERROR(state_->CheckQueryState());

    DCHECK(continue_execution);
    // We should be at the end of the the row group if we get this far
    DCHECK_EQ(column_readers_[0]->rep_level(), -1);
    // Reset parse_status_ for the next row group.
    parse_status_ = Status::OK();
  }
  return Status::OK();
}

// TODO: this needs to be codegen'd.  The ReadValue function needs to be codegen'd,
// specific to type and encoding and then inlined into AssembleRows().
template <bool IN_COLLECTION, bool MATERIALIZING_COLLECTION>
bool HdfsParquetScanner::AssembleRows(const TupleDescriptor* tuple_desc,
    const vector<ColumnReader*>& column_readers, int new_collection_rep_level,
    int row_group_idx, ArrayValueBuilder* array_value_builder) {
  DCHECK(!column_readers.empty());
  if (MATERIALIZING_COLLECTION) {
    DCHECK_GE(new_collection_rep_level, 0);
    DCHECK(array_value_builder != NULL);
  } else {
    DCHECK_EQ(new_collection_rep_level, -1);
    DCHECK(array_value_builder == NULL);
  }

  Tuple* template_tuple = template_tuple_map_[tuple_desc];
  const vector<ExprContext*> conjunct_ctxs = scanner_conjuncts_map_[tuple_desc->id()];

  int64_t rows_read = 0;
  bool continue_execution = !scan_node_->ReachedLimit() && !context_->cancelled();
  // Note that this will be set to true at the end of the row group or the end of the
  // current collection (if applicable).
  bool end_of_collection = column_readers[0]->rep_level() == -1;
  // We only initialize end_of_collection to true here if we're at the end of the row
  // group (otherwise it would always be true because we're on the "edge" of two
  // collections), and only ProcessSplit() should call AssembleRows() at the end of the
  // row group.
  if (array_value_builder != NULL) DCHECK(!end_of_collection);

  while (!end_of_collection && continue_execution) {
    MemPool* pool;
    Tuple* tuple;
    TupleRow* row = NULL;

    int64_t num_rows;
    if (!MATERIALIZING_COLLECTION) {
      // We're assembling the top-level tuples into row batches
      num_rows = static_cast<int64_t>(GetMemory(&pool, &tuple, &row));
    } else {
      // We're assembling item tuples into an ArrayValue
      num_rows = static_cast<int64_t>(
          GetCollectionMemory(array_value_builder, &pool, &tuple, &row));
      if (num_rows == 0) {
        DCHECK(!parse_status_.ok());
        continue_execution = false;
        break;
      }
      // 'num_rows' can be very high if we're writing to a large ArrayValue. Limit the
      // number of rows we read at one time so we don't spend too long in the 'num_rows'
      // loop below before checking for cancellation or limit reached.
      num_rows = std::min(
          num_rows, static_cast<int64_t>(scan_node_->runtime_state()->batch_size()));
    }
    int num_to_commit = 0;
    int row_idx = 0;
    for (row_idx = 0; row_idx < num_rows && !end_of_collection; ++row_idx) {
      DCHECK(continue_execution);
      // A tuple is produced iff the collection that contains its values is not empty and
      // non-NULL. (Empty or NULL collections produce no output values, whereas NULL is
      // output for the fields of NULL structs.)
      bool materialize_tuple = !IN_COLLECTION || column_readers[0]->def_level() >=
          column_readers[0]->def_level_of_immediate_repeated_ancestor();
      InitTuple(tuple_desc, template_tuple, tuple);
      continue_execution =
          ReadRow<IN_COLLECTION>(column_readers, tuple, pool, &materialize_tuple);
      if (UNLIKELY(!continue_execution)) break;
      end_of_collection = column_readers[0]->rep_level() <= new_collection_rep_level;

      if (materialize_tuple) {
        if (!MATERIALIZING_COLLECTION) row->SetTuple(scan_node_->tuple_idx(), tuple);
        if (ExecNode::EvalConjuncts(&conjunct_ctxs[0], conjunct_ctxs.size(), row)) {
          if (!MATERIALIZING_COLLECTION) row = next_row(row);
          tuple = next_tuple(tuple_desc->byte_size(), tuple);
          ++num_to_commit;
        }
      }
    }

    rows_read += row_idx;
    COUNTER_ADD(scan_node_->rows_read_counter(), row_idx);
    if (!MATERIALIZING_COLLECTION) {
      Status query_status = CommitRows(num_to_commit);
      if (!query_status.ok()) continue_execution = false;
    } else {
      array_value_builder->CommitTuples(num_to_commit);
    }
    continue_execution &= !scan_node_->ReachedLimit() && !context_->cancelled();
  }

  if (end_of_collection) {
    // All column readers should report the start of the same collection.
    for (int c = 1; c < column_readers.size(); ++c) {
      FILE_CHECK_EQ(column_readers[c]->rep_level(), column_readers[0]->rep_level());
    }
  }

  bool end_of_row_group = column_readers[0]->rep_level() == -1;
  if (end_of_row_group && parse_status_.ok()) {
    parse_status_ = ValidateEndOfRowGroup(column_readers, row_group_idx, rows_read);
    if (!parse_status_.ok()) continue_execution = false;
  }
  return continue_execution;
}

template <bool IN_COLLECTION>
inline bool HdfsParquetScanner::ReadRow(const vector<ColumnReader*>& column_readers,
    Tuple* tuple, MemPool* pool, bool* materialize_tuple) {
  DCHECK(!column_readers.empty());
  bool continue_execution = true;
  bool conjuncts_passed = true;
  for (int c = 0; c < column_readers.size(); ++c) {
    ColumnReader* col_reader = column_readers[c];
    if (!IN_COLLECTION) {
      DCHECK(*materialize_tuple);
      DCHECK(col_reader->pos_slot_desc() == NULL);
      // We found a value, read it
      continue_execution = col_reader->ReadValue(pool, tuple, &conjuncts_passed);
    } else if (*materialize_tuple) {
      // All column readers for this tuple should a value to materialize.
      FILE_CHECK_GE(col_reader->def_level(),
                    col_reader->def_level_of_immediate_repeated_ancestor());
      // Fill in position slot if applicable
      if (col_reader->pos_slot_desc() != NULL) col_reader->ReadPosition(tuple);
      continue_execution = col_reader->ReadValue(pool, tuple, &conjuncts_passed);
    } else {
      // A containing repeated field is empty or NULL
      FILE_CHECK_LT(col_reader->def_level(),
                    col_reader->def_level_of_immediate_repeated_ancestor());
      continue_execution = col_reader->NextLevels();
    }
    if (UNLIKELY(!continue_execution)) break;
  }
  *materialize_tuple &= conjuncts_passed;
  return continue_execution;
}

Status HdfsParquetScanner::ProcessFooter(bool* eosr) {
  *eosr = false;
  uint8_t* buffer;
  int64_t len;

  RETURN_IF_ERROR(stream_->GetBuffer(false, &buffer, &len));
  DCHECK(stream_->eosr());

  // Number of bytes in buffer after the fixed size footer is accounted for.
  int remaining_bytes_buffered = len - sizeof(int32_t) - sizeof(PARQUET_VERSION_NUMBER);

  // Make sure footer has enough bytes to contain the required information.
  if (remaining_bytes_buffered < 0) {
    return Status(Substitute("File $0 is invalid.  Missing metadata.",
        stream_->filename()));
  }

  // Validate magic file bytes are correct.
  uint8_t* magic_number_ptr = buffer + len - sizeof(PARQUET_VERSION_NUMBER);
  if (memcmp(magic_number_ptr, PARQUET_VERSION_NUMBER,
             sizeof(PARQUET_VERSION_NUMBER)) != 0) {
    return Status(Substitute("File $0 is invalid.  Invalid file footer: $1",
        stream_->filename(),
        string((char*)magic_number_ptr, sizeof(PARQUET_VERSION_NUMBER))));
  }

  // The size of the metadata is encoded as a 4 byte little endian value before
  // the magic number
  uint8_t* metadata_size_ptr = magic_number_ptr - sizeof(int32_t);
  uint32_t metadata_size = *reinterpret_cast<uint32_t*>(metadata_size_ptr);
  uint8_t* metadata_ptr = metadata_size_ptr - metadata_size;
  // If the metadata was too big, we need to stitch it before deserializing it.
  // In that case, we stitch the data in this buffer.
  vector<uint8_t> metadata_buffer;
  metadata_range_ = stream_->scan_range();

  if (UNLIKELY(metadata_size > remaining_bytes_buffered)) {
    // In this case, the metadata is bigger than our guess meaning there are
    // not enough bytes in the footer range from IssueInitialRanges().
    // We'll just issue more ranges to the IoMgr that is the actual footer.
    const HdfsFileDesc* file_desc = scan_node_->GetFileDesc(metadata_range_->file());
    DCHECK(file_desc != NULL);
    // The start of the metadata is:
    // file_length - 4-byte metadata size - footer-size - metadata size
    int64_t metadata_start = file_desc->file_length -
      sizeof(int32_t) - sizeof(PARQUET_VERSION_NUMBER) - metadata_size;
    int64_t metadata_bytes_to_read = metadata_size;
    if (metadata_start < 0) {
      return Status(Substitute("File $0 is invalid. Invalid metadata size in file "
          "footer: $1 bytes. File size: $2 bytes.", stream_->filename(), metadata_size,
          file_desc->file_length));
    }
    // IoMgr can only do a fixed size Read(). The metadata could be larger
    // so we stitch it here.
    // TODO: consider moving this stitching into the scanner context. The scanner
    // context usually handles the stitching but no other scanner need this logic
    // now.
    metadata_buffer.resize(metadata_size);
    metadata_ptr = &metadata_buffer[0];
    int64_t copy_offset = 0;
    DiskIoMgr* io_mgr = scan_node_->runtime_state()->io_mgr();

    while (metadata_bytes_to_read > 0) {
      int64_t to_read = ::min(static_cast<int64_t>(io_mgr->max_read_buffer_size()),
          metadata_bytes_to_read);
      DiskIoMgr::ScanRange* range = scan_node_->AllocateScanRange(
          metadata_range_->fs(), metadata_range_->file(), to_read,
          metadata_start + copy_offset, -1, metadata_range_->disk_id(),
          metadata_range_->try_cache(), metadata_range_->expected_local(),
          file_desc->mtime);

      DiskIoMgr::BufferDescriptor* io_buffer = NULL;
      RETURN_IF_ERROR(io_mgr->Read(scan_node_->reader_context(), range, &io_buffer));
      memcpy(metadata_ptr + copy_offset, io_buffer->buffer(), io_buffer->len());
      io_buffer->Return();

      metadata_bytes_to_read -= to_read;
      copy_offset += to_read;
    }
    DCHECK_EQ(metadata_bytes_to_read, 0);
  }
  // Deserialize file header
  // TODO: this takes ~7ms for a 1000-column table, figure out how to reduce this.
  Status status =
      DeserializeThriftMsg(metadata_ptr, &metadata_size, true, &file_metadata_);
  if (!status.ok()) {
    return Status(Substitute("File $0 has invalid file metadata at file offset $1. "
        "Error = $2.", stream_->filename(),
        metadata_size + sizeof(PARQUET_VERSION_NUMBER) + sizeof(uint32_t),
        status.GetDetail()));
  }

  RETURN_IF_ERROR(ValidateFileMetadata());
  // Parse file schema
  RETURN_IF_ERROR(CreateSchemaTree(file_metadata_.schema, &schema_));

  if (scan_node_->materialized_slots().empty() &&
      scan_node_->tuple_desc()->tuple_path().empty()) {
    // This is a count(*) over the table.  We can serve this query from just the file
    // metadata.  We don't need to read the column data.
    int64_t num_tuples = file_metadata_.num_rows;
    COUNTER_ADD(scan_node_->rows_read_counter(), num_tuples);

    while (num_tuples > 0) {
      MemPool* pool;
      Tuple* tuple;
      TupleRow* current_row;
      int max_tuples = GetMemory(&pool, &tuple, &current_row);
      max_tuples = min(static_cast<int64_t>(max_tuples), num_tuples);
      num_tuples -= max_tuples;

      int num_to_commit = WriteEmptyTuples(context_, current_row, max_tuples);
      RETURN_IF_ERROR(CommitRows(num_to_commit));
    }

    *eosr = true;
    return Status::OK();
  } else if (file_metadata_.num_rows == 0) {
    // Empty file
    *eosr = true;
    return Status::OK();
  }

  if (file_metadata_.row_groups.empty()) {
    return Status(Substitute("Invalid file. This file: $0 has no row groups",
                             stream_->filename()));
  }
  return Status::OK();
}

Status HdfsParquetScanner::ResolvePath(const SchemaPath& path, SchemaNode** node,
    bool* pos_field, bool* missing_field) {
  DCHECK(schema_.element != NULL)
      << "schema_ must be initialized before calling ResolvePath()";

  *pos_field = false;
  *missing_field = false;
  *node = &schema_;
  const ColumnType* col_type = NULL;

  // Traverse 'path' and resolve 'node' to the corresponding SchemaNode in 'schema_' (by
  // ordinal), or set 'node' to NULL if 'path' doesn't exist in this file's schema.
  for (int i = 0; i < path.size(); ++i) {
    int table_idx = path[i];
    // The first index in a path includes the table's partition keys
    int file_idx = i == 0 ? path[i] - scan_node_->num_partition_keys() : path[i];
    if ((*node)->children.size() <= file_idx) {
      // The selected field is not in the file
      VLOG_FILE << Substitute("File '$0' does not contain path '$1'",
          stream_->filename(), PrintPath(*scan_node_->hdfs_table(), path));
      *missing_field = true;
      *node = NULL;
      return Status::OK();
    }
    *node = &(*node)->children[file_idx];

    col_type = i == 0 ? &scan_node_->hdfs_table()->col_descs()[table_idx].type()
               : &col_type->children[table_idx];

    if (col_type->type == TYPE_ARRAY) {
      DCHECK_EQ(col_type->children.size(), 1);
      // According to the parquet spec, array columns are represented like:
      // <list-repetition> group <name> (LIST) {
      //   repeated group list {
      //     <element-repetition> <element-type> element;
      //   }
      // }
      // See https://github.com/apache/parquet-format/blob/master/LogicalTypes.md#lists
      // for more details.
      // TODO: implement support for all backwards-compat rules
      if ((*node)->children.size() != 1) {
        return Status(
            TErrorCode::PARQUET_UNRECOGNIZED_SCHEMA, "array", (*node)->DebugString());
      }
      SchemaNode* list = &(*node)->children[0];
      if (list->element->repetition_type != parquet::FieldRepetitionType::REPEATED ||
          list->children.empty()) {
        return Status(
            TErrorCode::PARQUET_UNRECOGNIZED_SCHEMA, "array", (*node)->DebugString());
      }
      *node = list;

      if (i + 1 < path.size()) {
        if (path[i + 1] == 1) {
          // The next index in 'path' is the artifical position field. Set 'pos_field' and
          // return the parent list node.
          DCHECK_EQ(path.size(), i + 2) << "position field cannot have children!";
          *pos_field = true;
          *node = NULL;
          return Status::OK();
        } else {
          // The next value in 'path' should be the item index
          DCHECK_EQ(path[i + 1], 0);
        }
      }

      if (list->children.size() != 1) {
        DCHECK_GT(list->children.size(), 1) << "Other collection representations NYI";
        // Backwards compatibility: some data does not contain the inner 'element' node if
        // the element is a struct. For example, the SQL schema
        // array_col array<struct<a: int, b:string>> should be encoded as:
        // <list-repetition> group array_col (LIST) {
        //   repeated group list {
        //     <element-repetition> group element {
        //       <a-repetition> int a;
        //       <b-repetition> binary b;
        //     }
        //   }
        // }
        //
        // However, it may be encoded as:
        // <list-repetition> group array_col (LIST) {
        //   repeated group list {
        //     <a-repetition> int a;
        //     <b-repetition> binary b;
        //   }
        // }
        //
        // In this case, we skip the item index in 'path'.
        col_type = &col_type->children[0];
        ++i;
      }
    } else if (col_type->type == TYPE_MAP) {
      DCHECK_EQ(col_type->children.size(), 2);
      // According to the parquet spec, map columns are represented like:
      // <map-repetition> group <name> (MAP) {
      //   repeated group key_value {
      //     required <key-type> key;
      //     <value-repetition> <value-type> value;
      //   }
      // }
      // See https://github.com/apache/parquet-format/blob/master/LogicalTypes.md#maps for
      // more details.
      // TODO: implement support for all backwards-compat rules
      if ((*node)->children.size() != 1) {
        return Status(
            TErrorCode::PARQUET_UNRECOGNIZED_SCHEMA, "map", (*node)->DebugString());
      }
      SchemaNode* key_value = &(*node)->children[0];
      if (key_value->element->repetition_type != parquet::FieldRepetitionType::REPEATED ||
          key_value->children.size() != 2) {
        return Status(
            TErrorCode::PARQUET_UNRECOGNIZED_SCHEMA, "map", (*node)->DebugString());
      }
      *node = key_value;

      // The next index in 'path' should be the key or the value. Note that map elements
      // do not have a synthetic position field.
      if (i + 1 < path.size()) DCHECK(path[i + 1] == 0 || path[i + 1] == 1);
    }
  }
  DCHECK(*node != NULL);
  return Status::OK();
}

Status HdfsParquetScanner::CreateColumnReaders(const TupleDescriptor& tuple_desc,
    vector<ColumnReader*>* column_readers) {
  DCHECK(column_readers != NULL);
  DCHECK(column_readers->empty());

  // Each tuple can have at most one position slot. We'll process this slot desc last.
  SlotDescriptor* pos_slot_desc = NULL;

  BOOST_FOREACH(SlotDescriptor* slot_desc, tuple_desc.slots()) {
    if (!slot_desc->is_materialized()) continue;
    // Skip partition columns
    if (&tuple_desc == scan_node_->tuple_desc() &&
        slot_desc->col_pos() < scan_node_->num_partition_keys()) continue;

    SchemaNode* node = NULL;
    bool pos_field;
    bool missing_field;
    RETURN_IF_ERROR(
        ResolvePath(slot_desc->col_path(), &node, &pos_field, &missing_field));

    if (missing_field) {
      // In this case, we are selecting a column that is not in the file.
      // Update the template tuple to put a NULL in this slot.
      Tuple** template_tuple = &template_tuple_map_[&tuple_desc];
      if (*template_tuple == NULL) {
        *template_tuple = scan_node_->InitEmptyTemplateTuple(tuple_desc);
      }
      (*template_tuple)->SetNull(slot_desc->null_indicator_offset());
      continue;
    }

    if (pos_field) {
      DCHECK(pos_slot_desc == NULL) << "There should only be one position slot per tuple";
      pos_slot_desc = slot_desc;
      continue;
    }

    ColumnReader* col_reader = CreateReader(*node, slot_desc);
    column_readers->push_back(col_reader);

    if (col_reader->reading_collection()) {
      // Recursively populate col_reader's children
      DCHECK(slot_desc->collection_item_descriptor() != NULL);
      const TupleDescriptor* item_tuple_desc = slot_desc->collection_item_descriptor();
      CollectionColumnReader* collection_reader =
          static_cast<CollectionColumnReader*>(col_reader);
      RETURN_IF_ERROR(CreateColumnReaders(
          *item_tuple_desc, collection_reader->children()));
    }
  }

  if (column_readers->empty()) {
    // This is either a count(*) over a collection type (count(*) over the table is
    // handled in ProcessFooter()), or no materialized columns appear in this file
    // (e.g. due to schema evolution, or if there's only a position slot). Create a single
    // column reader that we will use to count the number of tuples we should output. We
    // will not read any values from this reader.
    ColumnReader* reader;
    RETURN_IF_ERROR(CreateCountingReader(tuple_desc.tuple_path(), &reader));
    column_readers->push_back(reader);
  }

  if (pos_slot_desc != NULL) {
    // 'tuple_desc' has a position slot. Use an existing column reader to populate it.
    DCHECK(!column_readers->empty());
    (*column_readers)[0]->set_pos_slot_desc(pos_slot_desc);
  }

  COUNTER_ADD(num_cols_counter_, column_readers->size());
  return Status::OK();
}

Status HdfsParquetScanner::CreateCountingReader(
    const SchemaPath& parent_path, HdfsParquetScanner::ColumnReader** reader) {
  SchemaNode* parent_node;
  bool pos_field;
  bool missing_field;
  RETURN_IF_ERROR(ResolvePath(parent_path, &parent_node, &pos_field, &missing_field));

  if (missing_field) {
    // TODO: can we do anything else here?
    return Status(Substitute("Could not find '$0' in file.",
        PrintPath(*scan_node_->hdfs_table(), parent_path), stream_->filename()));
  }
  DCHECK(!pos_field);
  DCHECK(!parent_node->children.empty());
  DCHECK(parent_path.empty() || parent_node->is_repeated());

  // Find a non-struct (i.e. array or scalar) child of 'parent_node', which we will use to
  // create the item reader
  const SchemaNode* target_node = &parent_node->children[0];
  while (!target_node->children.empty() && !target_node->is_repeated()) {
    target_node = &target_node->children[0];
  }

  *reader = CreateReader(*target_node, NULL);

  if (target_node->is_repeated()) {
    // Find the closest scalar descendent of 'target_node' via breadth-first search, and
    // create scalar reader to drive 'reader'. We find the closest (i.e. least-nested)
    // descendent as a heuristic for picking a descendent with fewer values, so it's
    // faster to scan.
    // TODO: use different heuristic than least-nested? Fewest values?
    const SchemaNode* node = NULL;
    queue<const SchemaNode*> nodes;
    nodes.push(target_node);
    while (!nodes.empty()) {
      node = nodes.front();
      nodes.pop();
      if (node->children.size() > 0) {
        BOOST_FOREACH(const SchemaNode& child, node->children) nodes.push(&child);
      } else {
        // node is the least-nested scalar descendent of 'target_node'
        break;
      }
    }
    DCHECK(node->children.empty()) << node->DebugString();
    CollectionColumnReader* parent_reader = static_cast<CollectionColumnReader*>(*reader);
    parent_reader->children()->push_back(CreateReader(*node, NULL));
  }
  return Status::OK();
}

Status HdfsParquetScanner::InitColumns(
    int row_group_idx, const vector<ColumnReader*>& column_readers) {
  const HdfsFileDesc* file_desc = scan_node_->GetFileDesc(metadata_range_->file());
  DCHECK(file_desc != NULL);
  parquet::RowGroup& row_group = file_metadata_.row_groups[row_group_idx];

  // All the scan ranges (one for each column).
  vector<DiskIoMgr::ScanRange*> col_ranges;
  // Used to validate that the number of values in each reader in column_readers_ is the
  // same.
  int num_values = -1;
  // Used to validate we issued the right number of scan ranges
  int num_scalar_readers = 0;

  BOOST_FOREACH(ColumnReader* col_reader, column_readers) {
    if (col_reader->reading_collection()) {
      // Recursively init child readers
      CollectionColumnReader* collection_reader =
          static_cast<CollectionColumnReader*>(col_reader);
      RETURN_IF_ERROR(InitColumns(row_group_idx, *collection_reader->children()));
      continue;
    }
    ++num_scalar_readers;

    BaseScalarColumnReader* scalar_reader =
        static_cast<BaseScalarColumnReader*>(col_reader);
    const parquet::ColumnChunk& col_chunk = row_group.columns[scalar_reader->col_idx()];
    int64_t col_start = col_chunk.meta_data.data_page_offset;

    if (num_values == -1) {
      num_values = col_chunk.meta_data.num_values;
    } else if (col_chunk.meta_data.num_values != num_values) {
      // TODO for 2.3: improve this error message by saying which columns are different,
      // and also specify column in other error messages as appropriate
      return Status(TErrorCode::PARQUET_NUM_COL_VALS_ERROR, scalar_reader->col_idx(),
          col_chunk.meta_data.num_values, num_values, stream_->filename());
    }

    RETURN_IF_ERROR(ValidateColumn(*scalar_reader, row_group_idx));

    // If there is a dictionary page, the file format requires it to come before
    // any data pages.  We need to start reading the column from the data page.
    if (col_chunk.meta_data.__isset.dictionary_page_offset) {
      if (col_chunk.meta_data.dictionary_page_offset >= col_start) {
        stringstream ss;
        ss << "File " << file_desc->filename << ": metadata is corrupt. "
           << "Dictionary page (offset=" << col_chunk.meta_data.dictionary_page_offset
           << ") must come before any data pages (offset=" << col_start << ").";
        return Status(ss.str());
      }
      col_start = col_chunk.meta_data.dictionary_page_offset;
    }
    int64_t col_len = col_chunk.meta_data.total_compressed_size;
    int64_t col_end = col_start + col_len;
    if (col_end <= 0 || col_end > file_desc->file_length) {
      stringstream ss;
      ss << "File " << file_desc->filename << ": metadata is corrupt. "
         << "Column " << scalar_reader->col_idx() << " has invalid column offsets "
         << "(offset=" << col_start << ", size=" << col_len << ", "
         << "file_size=" << file_desc->file_length << ").";
      return Status(ss.str());
    }
    if (file_version_.application == "parquet-mr" && file_version_.VersionLt(1, 2, 9)) {
      // The Parquet MR writer had a bug in 1.2.8 and below where it didn't include the
      // dictionary page header size in total_compressed_size and total_uncompressed_size
      // (see IMPALA-694). We pad col_len to compensate.
      int64_t bytes_remaining = file_desc->file_length - col_end;
      int64_t pad = min(static_cast<int64_t>(MAX_DICT_HEADER_SIZE), bytes_remaining);
      col_len += pad;
    }

    // TODO: this will need to change when we have co-located files and the columns
    // are different files.
    if (!col_chunk.file_path.empty()) {
      FILE_CHECK_EQ(col_chunk.file_path, string(metadata_range_->file()));
    }

    DiskIoMgr::ScanRange* col_range = scan_node_->AllocateScanRange(
        metadata_range_->fs(), metadata_range_->file(), col_len, col_start,
        scalar_reader->col_idx(), metadata_range_->disk_id(),
        metadata_range_->try_cache(), metadata_range_->expected_local(),
        file_desc->mtime);
    col_ranges.push_back(col_range);

    // Get the stream that will be used for this column
    ScannerContext::Stream* stream = context_->AddStream(col_range);
    DCHECK(stream != NULL);

    RETURN_IF_ERROR(scalar_reader->Reset(&col_chunk.meta_data, stream));

    const SlotDescriptor* slot_desc = scalar_reader->slot_desc();
    if (slot_desc == NULL || !slot_desc->type().IsStringType() ||
        col_chunk.meta_data.codec != parquet::CompressionCodec::UNCOMPRESSED) {
      // Non-string types are always compact.  Compressed columns don't reference data in
      // the io buffers after tuple materialization.  In both cases, we can set compact to
      // true and recycle buffers more promptly.
      stream->set_contains_tuple_data(false);
    }
  }
  DCHECK_EQ(col_ranges.size(), num_scalar_readers);

  // Issue all the column chunks to the io mgr and have them scheduled immediately.
  // This means these ranges aren't returned via DiskIoMgr::GetNextRange and
  // instead are scheduled to be read immediately.
  RETURN_IF_ERROR(scan_node_->runtime_state()->io_mgr()->AddScanRanges(
      scan_node_->reader_context(), col_ranges, true));

  return Status::OK();
}

Status HdfsParquetScanner::CreateSchemaTree(const vector<parquet::SchemaElement>& schema,
    HdfsParquetScanner::SchemaNode* node) const {
  int idx = 0;
  int col_idx = 0;
  return CreateSchemaTree(schema, 0, 0, 0, &idx, &col_idx, node);
}

Status HdfsParquetScanner::CreateSchemaTree(
    const vector<parquet::SchemaElement>& schema, int max_def_level, int max_rep_level,
    int ira_def_level, int* idx, int* col_idx, HdfsParquetScanner::SchemaNode* node)
    const {
  if (*idx >= schema.size()) {
    return Status(Substitute("File $0 corrupt: could not reconstruct schema tree from "
            "flattened schema in file metadata", stream_->filename()));
  }
  node->element = &schema[*idx];
  ++(*idx);

  if (node->element->num_children == 0) {
    // node is a leaf node, meaning it's materialized in the file and appears in
    // file_metadata_.row_groups.columns
    node->col_idx = *col_idx;
    ++(*col_idx);
  }

  // def_level_of_immediate_repeated_ancestor does not include this node, so set before
  // updating ira_def_level
  node->def_level_of_immediate_repeated_ancestor = ira_def_level;

  if (node->element->repetition_type == parquet::FieldRepetitionType::OPTIONAL) {
    ++max_def_level;
  } else if (node->element->repetition_type == parquet::FieldRepetitionType::REPEATED) {
    ++max_rep_level;
    // Repeated fields add a definition level. This is used to distinguish between an
    // empty list and a list with an item in it.
    ++max_def_level;
    // node is the new most immediate repeated ancestor
    ira_def_level = max_def_level;
  }
  node->max_def_level = max_def_level;
  node->max_rep_level = max_rep_level;

  node->children.resize(node->element->num_children);
  for (int i = 0; i < node->element->num_children; ++i) {
    RETURN_IF_ERROR(CreateSchemaTree(schema, max_def_level, max_rep_level, ira_def_level,
        idx, col_idx, &node->children[i]));
  }
  return Status::OK();
}

HdfsParquetScanner::FileVersion::FileVersion(const string& created_by) {
  string created_by_lower = created_by;
  std::transform(created_by_lower.begin(), created_by_lower.end(),
      created_by_lower.begin(), ::tolower);
  is_impala_internal = false;

  vector<string> tokens;
  split(tokens, created_by_lower, is_any_of(" "), token_compress_on);
  // Boost always creates at least one token
  DCHECK_GT(tokens.size(), 0);
  application = tokens[0];

  if (tokens.size() >= 3 && tokens[1] == "version") {
    string version_string = tokens[2];
    // Ignore any trailing nodextra characters
    int n = version_string.find_first_not_of("0123456789.");
    string version_string_trimmed = version_string.substr(0, n);

    vector<string> version_tokens;
    split(version_tokens, version_string_trimmed, is_any_of("."));
    version.major = version_tokens.size() >= 1 ? atoi(version_tokens[0].c_str()) : 0;
    version.minor = version_tokens.size() >= 2 ? atoi(version_tokens[1].c_str()) : 0;
    version.patch = version_tokens.size() >= 3 ? atoi(version_tokens[2].c_str()) : 0;

    if (application == "impala") {
      if (version_string.find("-internal") != string::npos) is_impala_internal = true;
    }
  } else {
    version.major = 0;
    version.minor = 0;
    version.patch = 0;
  }
}

bool HdfsParquetScanner::FileVersion::VersionLt(int major, int minor, int patch) const {
  if (version.major < major) return true;
  if (version.major > major) return false;
  DCHECK_EQ(version.major, major);
  if (version.minor < minor) return true;
  if (version.minor > minor) return false;
  DCHECK_EQ(version.minor, minor);
  return version.patch < patch;
}

bool HdfsParquetScanner::FileVersion::VersionEq(int major, int minor, int patch) const {
  return version.major == major && version.minor == minor && version.patch == patch;
}

Status HdfsParquetScanner::ValidateFileMetadata() {
  if (file_metadata_.version > PARQUET_CURRENT_VERSION) {
    stringstream ss;
    ss << "File: " << stream_->filename() << " is of an unsupported version. "
       << "file version: " << file_metadata_.version;
    return Status(ss.str());
  }

  // Parse out the created by application version string
  if (file_metadata_.__isset.created_by) {
    file_version_ = FileVersion(file_metadata_.created_by);
  }
  return Status::OK();
}

bool IsEncodingSupported(parquet::Encoding::type e) {
  switch (e) {
    case parquet::Encoding::PLAIN:
    case parquet::Encoding::PLAIN_DICTIONARY:
    case parquet::Encoding::BIT_PACKED:
    case parquet::Encoding::RLE:
      return true;
    default:
      return false;
  }
}

Status HdfsParquetScanner::ValidateColumn(
    const BaseScalarColumnReader& col_reader, int row_group_idx) {
  int col_idx = col_reader.col_idx();
  const parquet::SchemaElement& schema_element = col_reader.schema_element();
  parquet::ColumnChunk& file_data =
      file_metadata_.row_groups[row_group_idx].columns[col_idx];

  // Check the encodings are supported.
  vector<parquet::Encoding::type>& encodings = file_data.meta_data.encodings;
  for (int i = 0; i < encodings.size(); ++i) {
    if (!IsEncodingSupported(encodings[i])) {
      stringstream ss;
      ss << "File '" << metadata_range_->file() << "' uses an unsupported encoding: "
         << PrintEncoding(encodings[i]) << " for column '" << schema_element.name
         << "'.";
      return Status(ss.str());
    }
  }

  // Check the compression is supported.
  if (file_data.meta_data.codec != parquet::CompressionCodec::UNCOMPRESSED &&
      file_data.meta_data.codec != parquet::CompressionCodec::SNAPPY &&
      file_data.meta_data.codec != parquet::CompressionCodec::GZIP) {
    stringstream ss;
    ss << "File '" << metadata_range_->file() << "' uses an unsupported compression: "
        << file_data.meta_data.codec << " for column '" << schema_element.name
        << "'.";
    return Status(ss.str());
  }

  // Check that this column is optional or required.
  if (schema_element.repetition_type != parquet::FieldRepetitionType::OPTIONAL &&
      schema_element.repetition_type != parquet::FieldRepetitionType::REQUIRED) {
    stringstream ss;
    ss << "File '" << metadata_range_->file() << "' column '" << schema_element.name
       << "' contains an unsupported column repetition type: "
       << schema_element.repetition_type;
    return Status(ss.str());
  }

  // Validation after this point is only if col_reader is reading values.
  const SlotDescriptor* slot_desc = col_reader.slot_desc();
  if (slot_desc == NULL) return Status::OK();

  // Check the type in the file is compatible with the catalog metadata.
  parquet::Type::type type = IMPALA_TO_PARQUET_TYPES[slot_desc->type().type];
  if (type != file_data.meta_data.type) {
    stringstream ss;
    ss << "File '" << metadata_range_->file() << "' has an incompatible type with the"
       << " table schema for column '" << schema_element.name << "'.  Expected type: "
       << type << ".  Actual type: " << file_data.meta_data.type;
    return Status(ss.str());
  }

  // Check the decimal scale in the file matches the metastore scale and precision.
  // We fail the query if the metadata makes it impossible for us to safely read
  // the file. If we don't require the metadata, we will fail the query if
  // abort_on_error is true, otherwise we will just log a warning.
  bool is_converted_type_decimal = schema_element.__isset.converted_type &&
      schema_element.converted_type == parquet::ConvertedType::DECIMAL;
  if (slot_desc->type().type == TYPE_DECIMAL) {
    // We require that the scale and byte length be set.
    if (schema_element.type != parquet::Type::FIXED_LEN_BYTE_ARRAY) {
      stringstream ss;
      ss << "File '" << metadata_range_->file() << "' column '" << schema_element.name
         << "' should be a decimal column encoded using FIXED_LEN_BYTE_ARRAY.";
      return Status(ss.str());
    }

    if (!schema_element.__isset.type_length) {
      stringstream ss;
      ss << "File '" << metadata_range_->file() << "' column '" << schema_element.name
         << "' does not have type_length set.";
      return Status(ss.str());
    }

    int expected_len = ParquetPlainEncoder::DecimalSize(slot_desc->type());
    if (schema_element.type_length != expected_len) {
      stringstream ss;
      ss << "File '" << metadata_range_->file() << "' column '" << schema_element.name
         << "' has an invalid type length. Expecting: " << expected_len
         << " len in file: " << schema_element.type_length;
      return Status(ss.str());
    }

    if (!schema_element.__isset.scale) {
      stringstream ss;
      ss << "File '" << metadata_range_->file() << "' column '" << schema_element.name
         << "' does not have the scale set.";
      return Status(ss.str());
    }

    if (schema_element.scale != slot_desc->type().scale) {
      // TODO: we could allow a mismatch and do a conversion at this step.
      stringstream ss;
      ss << "File '" << metadata_range_->file() << "' column '" << schema_element.name
         << "' has a scale that does not match the table metadata scale."
         << " File metadata scale: " << schema_element.scale
         << " Table metadata scale: " << slot_desc->type().scale;
      return Status(ss.str());
    }

    // The other decimal metadata should be there but we don't need it.
    if (!schema_element.__isset.precision) {
      ErrorMsg msg(TErrorCode::PARQUET_MISSING_PRECISION,
          metadata_range_->file(), schema_element.name);
      LOG_OR_RETURN_ON_ERROR(msg, state_);
    } else {
      if (schema_element.precision != slot_desc->type().precision) {
        // TODO: we could allow a mismatch and do a conversion at this step.
        ErrorMsg msg(TErrorCode::PARQUET_WRONG_PRECISION,
            metadata_range_->file(), schema_element.name,
            schema_element.precision, slot_desc->type().precision);
        LOG_OR_RETURN_ON_ERROR(msg, state_);
      }
    }

    if (!is_converted_type_decimal) {
      // TODO: is this validation useful? It is not required at all to read the data and
      // might only serve to reject otherwise perfectly readable files.
      ErrorMsg msg(TErrorCode::PARQUET_BAD_CONVERTED_TYPE,
          metadata_range_->file(), schema_element.name);
      LOG_OR_RETURN_ON_ERROR(msg, state_);
    }
  } else if (schema_element.__isset.scale || schema_element.__isset.precision ||
      is_converted_type_decimal) {
    ErrorMsg msg(TErrorCode::PARQUET_INCOMPATIBLE_DECIMAL,
        metadata_range_->file(), schema_element.name, slot_desc->type().DebugString());
    LOG_OR_RETURN_ON_ERROR(msg, state_);
  }
  return Status::OK();
}

Status HdfsParquetScanner::ValidateEndOfRowGroup(
    const vector<ColumnReader*>& column_readers, int row_group_idx, int64_t rows_read) {
  DCHECK(!column_readers.empty());
  DCHECK(parse_status_.ok()) << "Don't overwrite parse_status_";

  if (column_readers[0]->max_rep_level() == 0) {
    // These column readers materialize table-level values (vs. collection values). Test
    // if the expected number of rows from the file metadata matches the actual number of
    // rows read from the file.
    int64_t expected_rows_in_group = file_metadata_.row_groups[row_group_idx].num_rows;
    if (rows_read != expected_rows_in_group) {
      HdfsParquetScanner::ColumnReader* reader = column_readers_[0];
      return Status(TErrorCode::PARQUET_GROUP_ROW_COUNT_ERROR, reader->filename(),
          row_group_idx, expected_rows_in_group, rows_read);
    }
  }

  // Validate scalar column readers' state
  int num_values_read = -1;
  for (int c = 0; c < column_readers.size(); ++c) {
    if (column_readers[c]->reading_collection()) continue;
    BaseScalarColumnReader* reader =
        static_cast<BaseScalarColumnReader*>(column_readers[c]);
    // All readers should have exhausted the final data page. This could fail if one
    // column has more values than stated in the metadata, meaning the final data page
    // will still have unread values.
    // TODO for 2.3: make this a bad status
    FILE_CHECK_EQ(reader->num_buffered_values_, 0);
    // Sanity check that the num_values_read_ value is the same for all readers. All
    // readers should have been advanced in lockstep (the above check is more likely to
    // fail if this not the case though, since num_values_read_ is only updated at the end
    // of a data page).
    if (num_values_read == -1) num_values_read = reader->num_values_read_;
    DCHECK_EQ(reader->num_values_read_, num_values_read);
    // ReadDataPage() uses metadata_->num_values to determine when the column's done
    DCHECK_EQ(reader->num_values_read_, reader->metadata_->num_values);
  }
  return Status::OK();
}

string PrintRepetitionType(const parquet::FieldRepetitionType::type& t) {
  switch (t) {
    case parquet::FieldRepetitionType::REQUIRED: return "required";
    case parquet::FieldRepetitionType::OPTIONAL: return "optional";
    case parquet::FieldRepetitionType::REPEATED: return "repeated";
    default: return "<unknown>";
  }
}

string PrintParquetType(const parquet::Type::type& t) {
  switch (t) {
    case parquet::Type::BOOLEAN: return "boolean";
    case parquet::Type::INT32: return "int32";
    case parquet::Type::INT64: return "int64";
    case parquet::Type::INT96: return "int96";
    case parquet::Type::FLOAT: return "float";
    case parquet::Type::DOUBLE: return "double";
    case parquet::Type::BYTE_ARRAY: return "byte_array";
    case parquet::Type::FIXED_LEN_BYTE_ARRAY: return "fixed_len_byte_array";
    default: return "<unknown>";
  }
}

string HdfsParquetScanner::SchemaNode::DebugString(int indent) const {
  stringstream ss;
  for (int i = 0; i < indent; ++i) ss << " ";
  ss << PrintRepetitionType(element->repetition_type) << " ";
  if (element->num_children > 0) {
    ss << "struct";
  } else {
    ss << PrintParquetType(element->type);
  }
  ss << " " << element->name << " [i:" << col_idx << " d:" << max_def_level
     << " r:" << max_rep_level << "]";
  if (element->num_children > 0) {
    ss << " {" << endl;
    for (int i = 0; i < element->num_children; ++i) {
      ss << children[i].DebugString(indent + 2) << endl;
    }
    for (int i = 0; i < indent; ++i) ss << " ";
    ss << "}";
  }
  return ss.str();
}

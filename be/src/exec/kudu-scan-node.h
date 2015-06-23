// Copyright 2015 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http:///www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef IMPALA_EXEC_KUDU_SCAN_NODE_H_
#define IMPALA_EXEC_KUDU_SCAN_NODE_H_

#include <boost/scoped_ptr.hpp>
#include <kudu/client/client.h>

#include "exec/scan-node.h"
#include "runtime/descriptors.h"

namespace impala {

class KuduScanner;
class Tuple;

/// A scan node that scans Kudu TabletServers.
///
/// This scan node takes a set of ranges and uses a Kudu client to retrieve the data
/// belonging to those ranges from Kudu. The client's schema is rebuilt from the
/// TupleDescriptors forwarded by the frontend so that we're sure all the scan nodes
/// use the same schema, for the same scan.
///
/// What is implemented:
/// - Single threaded scans
/// - Scan node side conjunct eval
/// - Key predicate pushdowns
///
/// What is missing:
/// - Multi-threaded scans
/// - Column predicate pushdowns
/// - Memory transport
class KuduScanNode : public ScanNode {
 public:
  KuduScanNode(ObjectPool* pool, const TPlanNode& tnode, const DescriptorTbl& descs);

  ~KuduScanNode();

  /// Create Kudu schema and columns to slots mapping.
  virtual Status Prepare(RuntimeState* state);

  /// Start Kudu scan.
  virtual Status Open(RuntimeState* state);

  /// Fill the next row batch by fetching more data from the KuduScanner.
  virtual Status GetNext(RuntimeState* state, RowBatch* row_batch, bool* eos);

  /// Close connections to Kudu.
  virtual void Close(RuntimeState* state);

  /// Returns the set of materialized slots in the current schema.
  const std::vector<SlotDescriptor*>& materialized_slots() const {
    return materialized_slots_;
  }

  const kudu::client::KuduSchema& kudu_schema() const { return schema_; }

  const TupleDescriptor* tuple_desc() const { return tuple_desc_; }

 protected:
  /// Write debug string of this into out.
  virtual void DebugString(int indentation_level, std::stringstream* out) const;

 private:
  /// Friend so that scanners can call QueryMaintenance() on the scan node and access
  /// counters.
  friend class KuduScanner;

  /// Tuple id resolved in Prepare() to set tuple_desc_.
  TupleId tuple_id_;

  /// Descriptor of tuples read from Kudu table.
  const TupleDescriptor* tuple_desc_;

  /// The schema of the materialized slots (i.e projection)
  kudu::client::KuduSchema schema_;

  /// The Kudu client and table. Scanners share these instances.
  std::tr1::shared_ptr<kudu::client::KuduClient> client_;
  std::tr1::shared_ptr<kudu::client::KuduTable> table_;

  /// Set of ranges to be scanned.
  std::vector<TKuduKeyRange> scan_ranges_;

  /// Cached set of materialized slots in the tuple descriptor.
  std::vector<SlotDescriptor*> materialized_slots_;

  /// The (single) scanner used to perform scans on kudu.
  /// TODO a multi-threaded implementation will have more than one of these.
  boost::scoped_ptr<KuduScanner> scanner_;

  RuntimeProfile::Counter* kudu_read_timer_;
  RuntimeProfile::Counter* kudu_round_trips_;
  static const std::string KUDU_READ_TIMER;
  static const std::string KUDU_ROUND_TRIPS;
};

}

#endif

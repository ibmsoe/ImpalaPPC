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

#include "exec/kudu-table-sink.h"

#include <thrift/protocol/TDebugProtocol.h>

#include "exec/kudu-util.h"
#include "exprs/expr.h"
#include "exprs/expr-context.h"
#include "gen-cpp/ImpalaInternalService_constants.h"
#include "gutil/gscoped_ptr.h"

#include "common/names.h"

using kudu::client::KuduColumnSchema;
using kudu::client::KuduSchema;
using kudu::client::KuduClient;
using kudu::client::KuduRowResult;
using kudu::client::KuduTable;
using kudu::client::KuduInsert;
using kudu::client::KuduUpdate;
using kudu::client::KuduError;

static const char* KUDU_SINK_NAME = "KuduTableSink";

namespace impala {

const static string& ROOT_PARTITION_KEY =
    g_ImpalaInternalService_constants.ROOT_PARTITION_KEY;

KuduTableSink::KuduTableSink(const RowDescriptor& row_desc,
    const vector<TExpr>& select_list_texprs,
    const TDataSink& tsink)
    : table_id_(tsink.table_sink.target_table_id),
      row_desc_(row_desc),
      select_list_texprs_(select_list_texprs),
      sink_type_(tsink.table_sink.type),
      kudu_table_sink_(tsink.table_sink.kudu_table_sink) {
}

Status KuduTableSink::PrepareExprs(RuntimeState* state) {
  // From the thrift expressions create the real exprs.
  RETURN_IF_ERROR(Expr::CreateExprTrees(state->obj_pool(), select_list_texprs_,
                                        &output_expr_ctxs_));
  // Prepare the exprs to run.
  RETURN_IF_ERROR(
      Expr::Prepare(output_expr_ctxs_, state, row_desc_, expr_mem_tracker_.get()));
  return Status::OK();
}

Status KuduTableSink::Prepare(RuntimeState* state) {
  RETURN_IF_ERROR(DataSink::Prepare(state));
  runtime_profile_ = state->obj_pool()->Add(
      new RuntimeProfile(state->obj_pool(), KUDU_SINK_NAME));
  SCOPED_TIMER(runtime_profile_->total_time_counter());

  RETURN_IF_ERROR(PrepareExprs(state));


  // Get the kudu table descriptor.
  TableDescriptor* table_desc = state->desc_tbl().GetTableDescriptor(table_id_);
  DCHECK(table_desc != NULL);

  // In debug mode try a dynamic cast. If it fails it means that the
  // TableDescriptor is not an instance of KuduTableDescriptor.
  DCHECK(dynamic_cast<const KuduTableDescriptor*>(table_desc))
      << "TableDescriptor must be an instance KuduTableDescriptor.";

  table_desc_ = static_cast<const KuduTableDescriptor*>(table_desc);

  // Add a 'root partition' status in which to collect write statistics
  TInsertPartitionStatus root_status;
  root_status.__set_num_appended_rows(0L);
  root_status.__set_stats(TInsertStats());
  root_status.__set_id(-1L);
  state->per_partition_status()->insert(make_pair(ROOT_PARTITION_KEY, root_status));

  return Status::OK();
}

Status KuduTableSink::Open(RuntimeState* state) {
  RETURN_IF_ERROR(Expr::Open(output_expr_ctxs_, state));

  kudu::client::KuduClientBuilder b;
  BOOST_FOREACH(const string& address, table_desc_->kudu_master_addresses()) {
    b.add_master_server_addr(address);
  }

  KUDU_RETURN_IF_ERROR(b.Build(&client_), "Unable to create Kudu client");

  KUDU_RETURN_IF_ERROR(client_->OpenTable(table_desc_->table_name(), &table_),
      "Unable to open Kudu table");

  session_ = client_->NewSession();
  KUDU_RETURN_IF_ERROR(session_->SetFlushMode(
      kudu::client::KuduSession::MANUAL_FLUSH), "Unable to set flush mode");

  // TODO don't hardcode this timeout.
  session_->SetTimeoutMillis(10000);

  return Status::OK();
}

kudu::client::KuduWriteOperation* KuduTableSink::NewWriteOp() {
  if (sink_type_ == TTableSinkType::KUDU_INSERT) {
    return table_->NewInsert();
  } else if (sink_type_ == TTableSinkType::KUDU_UPDATE) {
    return table_->NewUpdate();
  } else {
    DCHECK(sink_type_ == TTableSinkType::KUDU_DELETE) << "Sink type not supported. "
                                                      << sink_type_;
    return table_->NewDelete();
  }
}

Status KuduTableSink::Send(RuntimeState* state, RowBatch* batch, bool eos) {
  SCOPED_TIMER(runtime_profile_->total_time_counter());
  ExprContext::FreeLocalAllocations(output_expr_ctxs_);
  RETURN_IF_ERROR(state->CheckQueryState());

  int rows_added = 0;
  // Since everything is set up just forward everything to the writer.
  for (int i = 0; i < batch->num_rows(); ++i) {
    TupleRow* current_row = batch->GetRow(i);
    gscoped_ptr<kudu::client::KuduWriteOperation> write(NewWriteOp());

    for (int j = 0; j < output_expr_ctxs_.size(); ++j) {
      int col = kudu_table_sink_.referenced_columns.empty() ?
          j : kudu_table_sink_.referenced_columns[j];

      void* value = output_expr_ctxs_[j]->GetValue(current_row);

      // If the value is NULL and no explicit column references are provided, the column
      // should be ignored, else it's explicitly set to NULL.
      if (value == NULL) {
        if (!kudu_table_sink_.referenced_columns.empty()) {
          KUDU_RETURN_IF_ERROR(write->mutable_row()->SetNull(col),
              "Could not add Kudu WriteOp.");
        }
        continue;
      }

      switch (output_expr_ctxs_[j]->root()->type().type) {
        case TYPE_STRING: {
          StringValue* sv = reinterpret_cast<StringValue*>(value);
          kudu::Slice slice(reinterpret_cast<uint8_t*>(sv->ptr), sv->len);
          KUDU_RETURN_IF_ERROR(write->mutable_row()->SetString(col, slice),
              "Could not add Kudu WriteOp.");
          break;
        }
        case TYPE_FLOAT:
          KUDU_RETURN_IF_ERROR(
              write->mutable_row()->SetFloat(col, *reinterpret_cast<float*>(value)),
              "Could not add Kudu WriteOp.");
          break;
        case TYPE_DOUBLE:
          KUDU_RETURN_IF_ERROR(
              write->mutable_row()->SetDouble(col, *reinterpret_cast<double*>(value)),
              "Could not add Kudu WriteOp.");
          break;
        case TYPE_BOOLEAN:
          KUDU_RETURN_IF_ERROR(
              write->mutable_row()->SetBool(col, *reinterpret_cast<bool*>(value)),
              "Could not add Kudu WriteOp.");
          break;
        case TYPE_TINYINT:
          KUDU_RETURN_IF_ERROR(
              write->mutable_row()->SetInt8(col, *reinterpret_cast<int8_t*>(value)),
              "Could not add Kudu WriteOp.");
          break;
        case TYPE_SMALLINT:
          KUDU_RETURN_IF_ERROR(
              write->mutable_row()->SetInt16(col, *reinterpret_cast<int16_t*>(value)),
              "Could not add Kudu WriteOp.");
          break;
        case TYPE_INT:
          KUDU_RETURN_IF_ERROR(
              write->mutable_row()->SetInt32(col, *reinterpret_cast<int32_t*>(value)),
              "Could not add Kudu WriteOp.");
          break;
        case TYPE_BIGINT:
          KUDU_RETURN_IF_ERROR(
              write->mutable_row()->SetInt64(col, *reinterpret_cast<int64_t*>(value)),
              "Could not add Kudu WriteOp.");
          break;
        default:
          DCHECK(false);
      }
    }

    KUDU_RETURN_IF_ERROR(session_->Apply(write.release()),
        "Error while applying Kudu session.");
    ++rows_added;
  }

  // TODO right now we always flush an entire row batch, if these are small we'll
  // be inefficient. Consider decoupling impala's batch size from kudu's
  if (!kudu_table_sink_.ignore_not_found_or_duplicate) {
    KUDU_RETURN_IF_ERROR(session_->Flush(), "Error while flushing Kudu session.");
  } else {
    // If the sink has the option "ignore_not_found_or_duplicate" set,
    // duplicate key or key already present errors from Kudu
    // in INSERT, UPDATE, or DELETE operations will be ignored.
    kudu::Status s = session_->Flush();
    if (UNLIKELY(!s.ok())) {
      vector<KuduError*> errors;
      bool overflowed = false;
      session_->GetPendingErrors(&errors, &overflowed);
      // The memory for the errors is manually manged. Iterate over all errors and delete
      // them accordingly.
      Status result = Status::OK();
      // Overflowed is set to true if the KuduSession could not hold on to all errors.
      // In this case we cannot guarantee that all errors are of type "NotFound".
      if (UNLIKELY(overflowed)) {
        result = Status("Error overflow in Kudu session, previous delete might be inconsistent.");
      }

      for (int i = 0; i < errors.size(); ++i) {
        kudu::Status e = errors[i]->status();
        // Only set the final status once, but continue iterating to clean up the memory
        if (result.ok() && (
                (sink_type_ == TTableSinkType::KUDU_DELETE && !e.IsNotFound()) ||
                (sink_type_ == TTableSinkType::KUDU_UPDATE && !e.IsNotFound()) ||
                (sink_type_ == TTableSinkType::KUDU_INSERT && !e.IsAlreadyPresent()))) {
          result = Status(strings::Substitute("$0: $1",
              "Error while flushing Kudu session", e.ToString()));
        }
        delete errors[i];
      }
      RETURN_IF_ERROR(result);
      rows_added -= errors.size();
    }
  }

  (*state->per_partition_status())[ROOT_PARTITION_KEY].num_appended_rows += rows_added;
  return Status::OK();
}

void KuduTableSink::Close(RuntimeState* state) {
  if (closed_) return;
  SCOPED_TIMER(runtime_profile_->total_time_counter());
  Expr::Close(output_expr_ctxs_, state);
  closed_ = true;
}

}  // namespace impala

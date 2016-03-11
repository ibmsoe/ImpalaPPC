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

package com.cloudera.impala.analysis;

import java.util.List;

import com.cloudera.impala.common.Pair;
import com.cloudera.impala.planner.DataSink;
import com.cloudera.impala.planner.KuduTableSink;
import com.cloudera.impala.planner.TableSink;
import com.google.common.base.Preconditions;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.Lists;

import static java.lang.String.format;

/**
 * Representation of an Update statement.
 *
 * Example UPDATE statement:
 *
 *     UPDATE target_table
 *       SET slotRef=expr, [slotRef=expr, ...]
 *       FROM table_ref_list
 *       WHERE conjunct_list
 *
 * An update statement consists of four major parts. First, the target table path,
 * second, the list of assignments, the optional FROM clause, and the optional where
 * clause. The type of the right-hand side of each assignments must be
 * assignment compatible with the left-hand side column type.
 *
 * Currently, only Kudu tables can be updated.
 */
public class UpdateStmt extends ModifyStmt {
  public UpdateStmt(List<String> targetTablePath,  FromClause tableRefs,
      List<Pair<SlotRef, Expr>> assignmentExprs,  Expr wherePredicate,
      boolean ignoreNotFound) {
    super(targetTablePath, tableRefs, assignmentExprs, wherePredicate, ignoreNotFound);
  }

  public UpdateStmt(UpdateStmt other) {
    super(other.targetTablePath_, other.fromClause_.clone(),
        Lists.<Pair<SlotRef, Expr>>newArrayList(), other.wherePredicate_,
        other.ignoreNotFound_);
  }

  /**
   * Return an instance of a KuduTableSink specialized as an Update operation.
   */
  public DataSink createDataSink() {
    // analyze() must have been called before.
    Preconditions.checkState(table_ != null);
    return TableSink.create(table_, TableSink.Op.UPDATE, ImmutableList.<Expr>of(),
        referencedColumns_, false, ignoreNotFound_);
  }

  @Override
  public UpdateStmt clone() {
    return new UpdateStmt(this);
  }

  @Override
  public String toSql() {
    StringBuilder b = new StringBuilder();
    b.append("UPDATE ");

    if (ignoreNotFound_) b.append("IGNORE ");

    if (fromClause_ == null) {
      b.append(targetTableRef_.toSql());
    } else {
      if (targetTableRef_.hasExplicitAlias()) {
        b.append(targetTableRef_.getExplicitAlias());
      } else {
        b.append(targetTableRef_.toSql());
      }
    }
    b.append(" SET");

    boolean first = true;
    for (Pair<SlotRef, Expr> i : assignments_) {
      if (!first) {
        b.append(",");
      } else {
        first = false;
      }
      b.append(format(" %s = %s",
          i.first.toSql(),
          i.second.toSql()));
    }

    b.append(fromClause_.toSql());

    if (wherePredicate_ != null) {
      b.append(" WHERE ");
      b.append(wherePredicate_.toSql());
    }
    return b.toString();
  }
}

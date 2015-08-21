// Copyright (c) 2015 Cloudera, Inc. All rights reserved.
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

package com.cloudera.impala.planner;

import java.util.ArrayList;

import com.cloudera.impala.analysis.Analyzer;
import com.cloudera.impala.analysis.CollectionTableRef;
import com.cloudera.impala.analysis.Expr;
import com.cloudera.impala.common.InternalException;
import com.cloudera.impala.thrift.TExplainLevel;
import com.cloudera.impala.thrift.TPlanNode;
import com.cloudera.impala.thrift.TPlanNodeType;
import com.cloudera.impala.thrift.TUnnestNode;
import com.google.common.base.Joiner;
import com.google.common.base.Preconditions;
import com.google.common.collect.Lists;

/**
 * An UnnestNode scans over a collection materialized in memory, and returns
 * one row per item in the collection.
 * An UnnestNode can only appear in the plan tree of a SubplanNode.
 */
public class UnnestNode extends PlanNode {
  private final SubplanNode parent_;
  private final CollectionTableRef tblRef_;
  private final Expr collectionExpr_;

  public UnnestNode(PlanNodeId id, SubplanNode parent, CollectionTableRef tblRef) {
    super(id, Lists.newArrayList(tblRef.getDesc().getId()), "UNNEST");
    parent_ = parent;
    tblRef_ = tblRef;
    collectionExpr_ = tblRef_.getCollectionExpr();
    // Assume the collection expr has been fully resolved in analysis.
    Preconditions.checkState(
        collectionExpr_.isBoundByTupleIds(parent.getChild(0).tupleIds_));
  }

  @Override
  public void init(Analyzer analyzer) throws InternalException {
    // TODO: Remove the scan predicates and predicates for enforcing slot
    // equivalences here when assigning them in the parent scan.
    ArrayList<Expr> scanPredicates = analyzer.getBoundPredicates(tupleIds_.get(0));
    conjuncts_.addAll(scanPredicates);
    super.init(analyzer);
    analyzer.createEquivConjuncts(tupleIds_.get(0), conjuncts_);

    // Unnest is like a scan and must materialize the slots of its conjuncts.
    markSlotsMaterialized(analyzer, conjuncts_);
    computeMemLayout(analyzer);
  }

  @Override
  public void computeStats(Analyzer analyzer) {
    super.computeStats(analyzer);
    cardinality_ = PlannerContext.AVG_COLLECTION_SIZE;
    numNodes_ = parent_.getNumNodes();
  }

  @Override
  protected String getNodeExplainString(String prefix, String detailPrefix,
      TExplainLevel detailLevel) {
    StringBuilder output = new StringBuilder();
    output.append(String.format("%s%s [%s]\n", prefix, getDisplayLabel(),
        getDisplayLabelDetail()));
    if (detailLevel.ordinal() >= TExplainLevel.EXTENDED.ordinal()) {
      output.append(
          String.format("%sparent-subplan=%s\n", detailPrefix, parent_.getId()));
    }
    if (detailLevel.ordinal() >= TExplainLevel.STANDARD.ordinal()) {
      if (!conjuncts_.isEmpty()) {
        output.append(
            detailPrefix + "predicates: " + getExplainString(conjuncts_) + "\n");
      }
    }
    return output.toString();
  }

  @Override
  protected String getDisplayLabelDetail() {
    StringBuilder strBuilder = new StringBuilder();
    strBuilder.append(Joiner.on(".").join(tblRef_.getPath()));
    if (tblRef_.hasExplicitAlias()) strBuilder.append(" " + tblRef_.getExplicitAlias());
    return strBuilder.toString();
  }

  @Override
  protected void toThrift(TPlanNode msg) {
    msg.node_type = TPlanNodeType.UNNEST_NODE;
    msg.setUnnest_node(new TUnnestNode(collectionExpr_.treeToThrift()));
  }
}

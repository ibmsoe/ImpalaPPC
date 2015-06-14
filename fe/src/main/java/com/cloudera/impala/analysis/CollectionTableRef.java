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

import com.cloudera.impala.common.AnalysisException;
import com.google.common.base.Preconditions;

/**
 * Reference to a MAP or ARRAY collection type that implies its
 * flattening during execution.
 */
public class CollectionTableRef extends TableRef {
  /////////////////////////////////////////
  // BEGIN: Members that need to be reset()

  // Expr that returns the referenced collection. Typically a SlotRef into the
  // parent scan's tuple. Result of analysis. Fully resolved against base tables.
  private Expr collectionExpr_;

  // True if this TableRef directly references a TableRef from an outer query block.
  private boolean isCorrelated_;

  // END: Members that need to be reset()
  /////////////////////////////////////////

  /**
   * Create a CollectionTableRef for the given collection type from the original
   * unresolved table ref. Sets the explicit alias and the join-related attributes
   * of the new collection table ref from the unresolved table ref.
   */
  public CollectionTableRef(TableRef tableRef) {
    super(tableRef);
    // Use the last path element as an implicit alias if no explicit alias was given.
    if (hasExplicitAlias()) return;
    String implicitAlias = rawPath_.get(rawPath_.size() - 1).toLowerCase();
    aliases_ = new String[] { implicitAlias };
  }

  /**
   * C'tor for cloning.
   */
  public CollectionTableRef(CollectionTableRef other) {
    super(other);
    collectionExpr_ =
        (other.collectionExpr_ != null) ? other.collectionExpr_.clone() : null;
    isCorrelated_ = other.isCorrelated_;
  }

  /**
   * Registers this table ref with the given analyzer and add a slot descriptor for
   * the materialized collection to be populated by parent scan. Also determines
   * whether this table ref is correlated or not.
   */
  @Override
  public void analyze(Analyzer analyzer) throws AnalysisException {
    if (isAnalyzed_) return;
    Preconditions.checkNotNull(getPrivilegeRequirement());
    desc_ = analyzer.registerTableRef(this);
    if (isRelativeRef()) {
      SlotDescriptor parentSlotDesc = analyzer.registerSlotRef(resolvedPath_);
      parentSlotDesc.setItemTupleDesc(desc_);
      collectionExpr_ = new SlotRef(parentSlotDesc);
      // Must always be materialized to ensure the correct cardinality after unnesting.
      analyzer.materializeSlots(collectionExpr_);
      Analyzer parentAnalyzer =
          analyzer.findAnalyzer(resolvedPath_.getRootDesc().getId());
      Preconditions.checkNotNull(parentAnalyzer);
      isCorrelated_ = parentAnalyzer != analyzer;
    }
    isAnalyzed_ = true;
    analyzeJoin(analyzer);
  }

  @Override
  public boolean isRelativeRef() {
    Preconditions.checkNotNull(resolvedPath_);
    return resolvedPath_.getRootDesc() != null;
  }

  @Override
  public boolean isCorrelated() { return isCorrelated_; }
  public Expr getCollectionExpr() { return collectionExpr_; }

  @Override
  protected CollectionTableRef clone() { return new CollectionTableRef(this); }

  @Override
  public void reset() {
    super.reset();
    collectionExpr_ = null;
    isCorrelated_ = false;
  }
}

// Copyright 2014 Cloudera Inc.
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

import java.math.BigDecimal;

import com.cloudera.impala.common.AnalysisException;
import com.cloudera.impala.common.InternalException;
import com.cloudera.impala.service.FeSupport;
import com.cloudera.impala.thrift.TAnalyticWindow;
import com.cloudera.impala.thrift.TAnalyticWindowBoundary;
import com.cloudera.impala.thrift.TAnalyticWindowBoundaryType;
import com.cloudera.impala.thrift.TAnalyticWindowType;
import com.cloudera.impala.thrift.TColumnValue;
import com.cloudera.impala.util.TColumnValueUtil;
import com.google.common.base.Preconditions;


/**
 * Windowing clause of an analytic expr
 * Both left and right boundaries are always non-null after analyze().
 */
public class AnalyticWindow {
  // default window used when an analytic expr was given an order by but no window
  public static final AnalyticWindow DEFAULT_WINDOW = new AnalyticWindow(Type.RANGE,
      new Boundary(BoundaryType.UNBOUNDED_PRECEDING, null),
      new Boundary(BoundaryType.CURRENT_ROW, null));

  enum Type {
    ROWS("ROWS"),
    RANGE("RANGE");

    private final String description_;

    private Type(String d) {
      description_ = d;
    }

    @Override
    public String toString() { return description_; }
    public TAnalyticWindowType toThrift() {
      return this == ROWS ? TAnalyticWindowType.ROWS : TAnalyticWindowType.RANGE;
    }
  }

  enum BoundaryType {
    UNBOUNDED_PRECEDING("UNBOUNDED PRECEDING"),
    UNBOUNDED_FOLLOWING("UNBOUNDED FOLLOWING"),
    CURRENT_ROW("CURRENT ROW"),
    PRECEDING("PRECEDING"),
    FOLLOWING("FOLLOWING");

    private final String description_;

    private BoundaryType(String d) {
      description_ = d;
    }

    @Override
    public String toString() { return description_; }
    public TAnalyticWindowBoundaryType toThrift() {
      Preconditions.checkState(!isAbsolutePos());
      if (this == CURRENT_ROW) {
        return TAnalyticWindowBoundaryType.CURRENT_ROW;
      } else if (this == PRECEDING) {
        return TAnalyticWindowBoundaryType.PRECEDING;
      } else if (this == FOLLOWING) {
        return TAnalyticWindowBoundaryType.FOLLOWING;
      }
      return null;
    }

    public boolean isAbsolutePos() {
      return this == UNBOUNDED_PRECEDING || this == UNBOUNDED_FOLLOWING;
    }

    public boolean isOffset() {
      return this == PRECEDING || this == FOLLOWING;
    }

    public boolean isPreceding() {
      return this == UNBOUNDED_PRECEDING || this == PRECEDING;
    }

    public boolean isFollowing() {
      return this == UNBOUNDED_FOLLOWING || this == FOLLOWING;
    }

    public BoundaryType converse() {
      switch (this) {
        case UNBOUNDED_PRECEDING: return UNBOUNDED_FOLLOWING;
        case UNBOUNDED_FOLLOWING: return UNBOUNDED_PRECEDING;
        case PRECEDING: return FOLLOWING;
        case FOLLOWING: return PRECEDING;
        default: return CURRENT_ROW;
      }
    }
  }

  public static class Boundary {
    private final BoundaryType type_;

    // Offset expr. Only set for PRECEDING/FOLLOWING. Needed for toSql().
    private final Expr expr_;

    // The offset value. Set during analysis after evaluating expr_. Integral valued
    // for ROWS windows.
    private BigDecimal offsetValue_;

    public BoundaryType getType() { return type_; }
    public Expr getExpr() { return expr_; }
    public BigDecimal getOffsetValue() { return offsetValue_; }

    public Boundary(BoundaryType type, Expr e) {
      this(type, e, null);
    }

    // c'tor used by clone()
    private Boundary(BoundaryType type, Expr e, BigDecimal offsetValue) {
      Preconditions.checkState(
        (type.isOffset() && e != null)
        || (!type.isOffset() && e == null));
      type_ = type;
      expr_ = e;
      offsetValue_ = offsetValue;
    }

    public String toSql() {
      StringBuilder sb = new StringBuilder();
      if (expr_ != null) sb.append(expr_.toSql()).append(" ");
      sb.append(type_.toString());
      return sb.toString();
    }

    public TAnalyticWindowBoundary toThrift(Type windowType) {
      TAnalyticWindowBoundary result = new TAnalyticWindowBoundary(type_.toThrift());
      if (type_.isOffset() && windowType == Type.ROWS) {
        result.setRows_offset_value(offsetValue_.longValue());
      }
      // TODO: range windows need range_offset_predicate
      return result;
    }

    @Override
    public boolean equals(Object obj) {
      if (obj == null) return false;
      if (obj.getClass() != this.getClass()) return false;
      Boundary o = (Boundary)obj;
      boolean exprEqual = (expr_ == null) == (o.expr_ == null);
      if (exprEqual && expr_ != null) exprEqual = expr_.equals(o.expr_);
      return type_ == o.type_ && exprEqual;
    }

    public Boundary converse() {
      Boundary result = new Boundary(type_.converse(),
          (expr_ != null) ? expr_.clone() : null);
      result.offsetValue_ = offsetValue_;
      return result;
    }

    @Override
    public Boundary clone() {
      return new Boundary(type_, expr_ != null ? expr_.clone() : null, offsetValue_);
    }

    public void analyze(Analyzer analyzer) throws AnalysisException {
      if (expr_ != null) expr_.analyze(analyzer);
    }
  }

  private final Type type_;
  private final Boundary leftBoundary_;
  private Boundary rightBoundary_;  // may be null before analyze()
  private String toSqlString_;  // cached after analysis

  public Type getType() { return type_; }
  public Boundary getLeftBoundary() { return leftBoundary_; }
  public Boundary getRightBoundary() { return rightBoundary_; }
  public Boundary setRightBoundary(Boundary b) { return rightBoundary_ = b; }

  public AnalyticWindow(Type type, Boundary b) {
    type_ = type;
    Preconditions.checkNotNull(b);
    leftBoundary_ = b;
    rightBoundary_ = null;
  }

  public AnalyticWindow(Type type, Boundary l, Boundary r) {
    type_ = type;
    Preconditions.checkNotNull(l);
    leftBoundary_ = l;
    Preconditions.checkNotNull(r);
    rightBoundary_ = r;
  }

  /**
   * Clone c'tor
   */
  private AnalyticWindow(AnalyticWindow other) {
    type_ = other.type_;
    Preconditions.checkNotNull(other.leftBoundary_);
    leftBoundary_ = other.leftBoundary_.clone();
    if (other.rightBoundary_ != null) {
      rightBoundary_ = other.rightBoundary_.clone();
    }
    toSqlString_ = other.toSqlString_;  // safe to share
  }

  public AnalyticWindow reverse() {
    Boundary newRightBoundary = leftBoundary_.converse();
    Boundary newLeftBoundary = null;
    if (rightBoundary_ == null) {
      newLeftBoundary = new Boundary(leftBoundary_.getType(), null);
    } else {
      newLeftBoundary = rightBoundary_.converse();
    }
    return new AnalyticWindow(type_, newLeftBoundary, newRightBoundary);
  }

  public String toSql() {
    if (toSqlString_ != null) return toSqlString_;
    StringBuilder sb = new StringBuilder();
    sb.append(type_.toString()).append(" ");
    if (rightBoundary_ == null) {
      sb.append(leftBoundary_.toSql());
    } else {
      sb.append("BETWEEN ").append(leftBoundary_.toSql()).append(" AND ");
      sb.append(rightBoundary_.toSql());
    }
    return sb.toString();
  }

  public TAnalyticWindow toThrift() {
    TAnalyticWindow result = new TAnalyticWindow(type_.toThrift());
    if (leftBoundary_.getType() != BoundaryType.UNBOUNDED_PRECEDING) {
      result.setWindow_start(leftBoundary_.toThrift(type_));
    }
    Preconditions.checkNotNull(rightBoundary_);
    if (rightBoundary_.getType() != BoundaryType.UNBOUNDED_FOLLOWING) {
      result.setWindow_end(rightBoundary_.toThrift(type_));
    }
    return result;
  }

  @Override
  public boolean equals(Object obj) {
    if (obj == null) return false;
    if (obj.getClass() != this.getClass()) return false;
    AnalyticWindow o = (AnalyticWindow)obj;
    boolean rightBoundaryEqual =
        (rightBoundary_ == null) == (o.rightBoundary_ == null);
    if (rightBoundaryEqual && rightBoundary_ != null) {
      rightBoundaryEqual = rightBoundary_.equals(o.rightBoundary_);
    }
    return type_ == o.type_
        && leftBoundary_.equals(o.leftBoundary_)
        && rightBoundaryEqual;
  }

  @Override
  public AnalyticWindow clone() { return new AnalyticWindow(this); }

  /**
   * Semantic analysis for expr of a PRECEDING/FOLLOWING clause.
   */
  private void checkOffsetExpr(Analyzer analyzer, Boundary boundary)
      throws AnalysisException {
    Preconditions.checkState(boundary.getType().isOffset());
    Expr e = boundary.getExpr();
    Preconditions.checkNotNull(e);
    boolean isPos = true;
    Double val = null;
    if (e.isConstant() && e.getType().isNumericType()) {
      try {
        val = TColumnValueUtil.getNumericVal(
            FeSupport.EvalConstExpr(e, analyzer.getQueryCtx()));
        if (val <= 0) isPos = false;
      } catch (InternalException exc) {
        throw new AnalysisException(
            "Couldn't evaluate PRECEDING/FOLLOWING expression: " + exc.getMessage());
      }
    }

    if (type_ == Type.ROWS) {
      if (!e.isConstant() || !e.getType().isIntegerType() || !isPos) {
        throw new AnalysisException(
            "For ROWS window, the value of a PRECEDING/FOLLOWING offset must be a "
              + "constant positive integer: " + boundary.toSql());
      }
      Preconditions.checkNotNull(val);
      boundary.offsetValue_ = new BigDecimal(val.longValue());
    } else {
      if (!e.isConstant() || !e.getType().isNumericType() || !isPos) {
        throw new AnalysisException(
            "For RANGE window, the value of a PRECEDING/FOLLOWING offset must be a "
              + "constant positive number: " + boundary.toSql());
      }
      boundary.offsetValue_ = new BigDecimal(val);
    }
  }

  /**
   * Check that b1 <= b2.
   */
  private void checkOffsetBoundaries(Analyzer analyzer, Boundary b1, Boundary b2)
      throws AnalysisException {
    Preconditions.checkState(b1.getType().isOffset());
    Preconditions.checkState(b2.getType().isOffset());
    Expr e1 = b1.getExpr();
    Preconditions.checkState(
        e1 != null && e1.isConstant() && e1.getType().isNumericType());
    Expr e2 = b2.getExpr();
    Preconditions.checkState(
        e2 != null && e2.isConstant() && e2.getType().isNumericType());

    try {
      TColumnValue val1 = FeSupport.EvalConstExpr(e1, analyzer.getQueryCtx());
      TColumnValue val2 = FeSupport.EvalConstExpr(e2, analyzer.getQueryCtx());
      double left = TColumnValueUtil.getNumericVal(val1);
      double right = TColumnValueUtil.getNumericVal(val2);
      if (left > right) {
        throw new AnalysisException(
            "Offset boundaries are in the wrong order: " + toSql());
      }
    } catch (InternalException exc) {
      throw new AnalysisException(
          "Couldn't evaluate PRECEDING/FOLLOWING expression: " + exc.getMessage());
    }

  }

  public void analyze(Analyzer analyzer) throws AnalysisException {
    leftBoundary_.analyze(analyzer);
    if (rightBoundary_ != null) rightBoundary_.analyze(analyzer);

    if (leftBoundary_.getType() == BoundaryType.UNBOUNDED_FOLLOWING) {
      throw new AnalysisException(
          leftBoundary_.getType().toString() + " is only allowed for upper bound of "
            + "BETWEEN");
    }
    if (rightBoundary_ != null
        && rightBoundary_.getType() == BoundaryType.UNBOUNDED_PRECEDING) {
      throw new AnalysisException(
          rightBoundary_.getType().toString() + " is only allowed for lower bound of "
            + "BETWEEN");
    }

    // TODO: Remove when RANGE windows with offset boundaries are supported.
    if (type_ == Type.RANGE) {
      if (leftBoundary_.type_.isOffset()
          || (rightBoundary_ != null && rightBoundary_.type_.isOffset())
          || (leftBoundary_.type_ == BoundaryType.CURRENT_ROW
              && (rightBoundary_ == null
                  || rightBoundary_.type_ == BoundaryType.CURRENT_ROW))) {
        throw new AnalysisException(
            "RANGE is only supported with both the lower and upper bounds UNBOUNDED or"
            + " one UNBOUNDED and the other CURRENT ROW.");
      }
    }

    if (rightBoundary_ == null && leftBoundary_.getType() == BoundaryType.FOLLOWING) {
      throw new AnalysisException(
          leftBoundary_.getType().toString() + " requires a BETWEEN clause");
    }

    if (leftBoundary_.getType().isOffset()) checkOffsetExpr(analyzer, leftBoundary_);
    if (rightBoundary_ == null) {
      // set right boundary to implied value, but make sure to cache toSql string
      // beforehand
      toSqlString_ = toSql();
      rightBoundary_ = new Boundary(BoundaryType.CURRENT_ROW, null);
      return;
    }
    if (rightBoundary_.getType().isOffset()) checkOffsetExpr(analyzer, rightBoundary_);

    if (leftBoundary_.getType() == BoundaryType.FOLLOWING) {
      if (rightBoundary_.getType() != BoundaryType.FOLLOWING
          && rightBoundary_.getType() != BoundaryType.UNBOUNDED_FOLLOWING) {
        throw new AnalysisException(
            "A lower window bound of " + BoundaryType.FOLLOWING.toString()
              + " requires that the upper bound also be "
              + BoundaryType.FOLLOWING.toString());
      }
      if (rightBoundary_.getType() != BoundaryType.UNBOUNDED_FOLLOWING) {
        checkOffsetBoundaries(analyzer, leftBoundary_, rightBoundary_);
      }
    }

    if (rightBoundary_.getType() == BoundaryType.PRECEDING) {
      if (leftBoundary_.getType() != BoundaryType.PRECEDING
          && leftBoundary_.getType() != BoundaryType.UNBOUNDED_PRECEDING) {
        throw new AnalysisException(
            "An upper window bound of " + BoundaryType.PRECEDING.toString()
              + " requires that the lower bound also be "
              + BoundaryType.PRECEDING.toString());
      }
      if (leftBoundary_.getType() != BoundaryType.UNBOUNDED_PRECEDING) {
        checkOffsetBoundaries(analyzer, rightBoundary_, leftBoundary_);
      }
    }
  }
}

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

package com.cloudera.impala.catalog;

import com.cloudera.impala.thrift.TColumn;

/**
 *  Describes a Kudu column mapped to a Hive column (as described in the metastore).
 *  This class extends Column with Kudu-specific information about whether it is part of a primary
 *  key, and whether it is nullable.
 */
public class KuduColumn extends Column {
  private final boolean isKey_;
  private final boolean isNullable_;

  public KuduColumn(String name, boolean isKey, boolean isNullable, Type type,
      String comment, int position) {
    super(name, type, comment, position);
    isKey_ = isKey;
    isNullable_ = isNullable;
  }

  public boolean isKey() { return isKey_; }
  public boolean isNullable() { return isNullable_; }

  @Override
  public TColumn toThrift() {
    TColumn colDesc = new TColumn(name_, type_.toThrift());
    if (comment_ != null) colDesc.setComment(comment_);
    colDesc.setCol_stats(getStats().toThrift());
    colDesc.setPosition(position_);
    colDesc.setIs_kudu_column(true);
    colDesc.setIs_key(isKey_);
    colDesc.setIs_nullable(isNullable_);
    return colDesc;
  }
}

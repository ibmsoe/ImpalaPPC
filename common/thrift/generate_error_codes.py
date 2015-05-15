#!/usr/bin/env python
# Copyright 2015 Cloudera Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


# For readability purposes we define the error codes and messages at the top of the
# file. New codes and messages must be added here. Old error messages MUST NEVER BE
# DELETED, but can be renamed. The tuple layout for a new entry is: error code enum name,
# numeric error code, format string of the message.
#
# TODO Add support for SQL Error Codes
#      https://msdn.microsoft.com/en-us/library/ms714687%28v=vs.85%29.aspx
error_codes = (
  ("OK", 1, ""),

  ("GENERAL", 2, "$0"),

  ("CANCELLED", 3, "$0"),

  ("ANALYSIS_ERROR", 4, "$0"),

  ("NOT_IMPLEMENTED_ERROR", 5, "$0"),

  ("RUNTIME_ERROR", 6, "$0"),

  ("MEM_LIMIT_EXCEEDED", 7, "$0"),

  ("INTERNAL_ERROR", 8, "$0"),

  ("RECOVERABLE_ERROR", 9, "$0"),

  ("PARQUET_MULTIPLE_BLOCKS", 10,
   "Parquet files should not be split into multiple hdfs-blocks. file=$0"),

  ("PARQUET_COLUMN_METADATA_INVALID", 11,
    "Column metadata states there are $0 values, but only read $1 values "
    "from column $2"),

  ("PARQUET_HEADER_PAGE_SIZE_EXCEEDED", 12,
    "ParquetScanner: could not read data page because page header exceeded "
    "maximum size of $0"),

  ("PARQUET_HEADER_EOF", 13,
    "ParquetScanner: reached EOF while deserializing data page header."),

  ("PARQUET_GROUP_ROW_COUNT_ERROR", 14,
    "Metadata states that in group $0($1) there are $2 rows, but only $3 "
    "rows were read."),

  ("PARQUET_GROUP_ROW_COUNT_OVERFLOW", 15,
    "Metadata states that in group $0($1) there are $2 rows, but there is at least one "
    "more row in the file."),

  ("PARQUET_MISSING_PRECISION", 16,
   "File '$0' column '$1' does not have the decimal precision set."),

  ("PARQUET_WRONG_PRECISION", 17,
    "File '$0' column '$1' has a precision that does not match the table metadata "
    " precision. File metadata precision: $2, table metadata precision: $3."),

  ("PARQUET_BAD_CONVERTED_TYPE", 18,
   "File '$0' column '$1' does not have converted type set to DECIMAL"),

  ("PARQUET_INCOMPATIBLE_DECIMAL", 19,
   "File '$0' column '$1' contains decimal data but the table metadata has type $2"),

  ("SEQUENCE_SCANNER_PARSE_ERROR", 20,
   "Problem parsing file $0 at $1$2"),

  ("SNAPPY_DECOMPRESS_INVALID_BLOCK_SIZE", 21,
   "Decompressor: block size is too big.  Data is likely corrupt. Size: $0"),

  ("SNAPPY_DECOMPRESS_INVALID_COMPRESSED_LENGTH", 22,
   "Decompressor: invalid compressed length.  Data is likely corrupt."),

  ("SNAPPY_DECOMPRESS_UNCOMPRESSED_LENGTH_FAILED", 23,
   "Snappy: GetUncompressedLength failed"),

  ("SNAPPY_DECOMPRESS_RAW_UNCOMPRESS_FAILED", 24,
   "SnappyBlock: RawUncompress failed"),

  ("SNAPPY_DECOMPRESS_DECOMPRESS_SIZE_INCORRECT", 25,
   "Snappy: Decompressed size is not correct."),

  ("HDFS_SCAN_NODE_UNKNOWN_DISK", 26, "Unknown disk id.  "
   "This will negatively affect performance. "
   "Check your hdfs settings to enable block location metadata."),

  ("FRAGMENT_EXECUTOR", 27, "Reserved resource size ($0) is larger than "
    "query mem limit ($1), and will be restricted to $1. Configure the reservation "
    "size by setting RM_INITIAL_MEM."),

  ("PARTITIONED_HASH_JOIN_MAX_PARTITION_DEPTH", 28,
   "Cannot perform join at hash join node with id $0."
   " The input data was partitioned the maximum number of $1 times."
   " This could mean there is significant skew in the data or the memory limit is"
   " set too low."),

  ("PARTITIONED_AGG_MAX_PARTITION_DEPTH", 29,
   "Cannot perform aggregation at hash aggregation node with id $0."
   " The input data was partitioned the maximum number of $1 times."
   " This could mean there is significant skew in the data or the memory limit is"
   " set too low."),

  ("MISSING_BUILTIN", 30, "Builtin '$0' with symbol '$1' does not exist. "
   "Verify that all your impalads are the same version."),

  ("RPC_GENERAL_ERROR", 31, "RPC Error: $0"),
  ("RPC_TIMEOUT", 32, "RPC timed out"),

  ("UDF_VERIFY_FAILED", 33,
   "Failed to verify function $0 from LLVM module $1, see log for more details."),
)

import sys
import os

# Verifies the uniqueness of the error constants and numeric error codes.
def check_duplicates(codes):
  constants = {}
  num_codes = {}
  for row in codes:
    if row[0] in constants:
      print("Constant %s already used, please check definition of '%s'!" % \
            (row[0], constants[row[0]]))
      exit(1)
    if row[1] in num_codes:
      print("Numeric error code %d already used, please check definition of '%s'!" % \
            (row[1], num_codes[row[1]]))
      exit(1)
    constants[row[0]] = row[2]
    num_codes[row[1]] = row[2]

preamble = """
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
//
//
// THIS FILE IS AUTO GENERATED BY generated_error_codes.py DO NOT MODIFY
// IT BY HAND.
//

namespace cpp impala
namespace java com.cloudera.impala.thrift

"""
# The script will always generate the file, CMake will take care of running it only if
# necessary.
target_file = "ErrorCodes.thrift"

# Check uniqueness of error constants and numeric codes
check_duplicates(error_codes)

fid = open(target_file, "w+")
try:
  fid.write(preamble)
  fid.write("""\nenum TErrorCode {\n""")
  fid.write(",\n".join(map(lambda x: "  %s" % x[0], error_codes)))
  fid.write("\n}")
  fid.write("\n")
  fid.write("const list<string> TErrorMessage = [\n")
  fid.write(",\n".join(map(lambda x: "  // %s\n  \"%s\"" %(x[0], x[2]), error_codes)))
  fid.write("\n]")
finally:
  fid.close()

print("%s created." % target_file)

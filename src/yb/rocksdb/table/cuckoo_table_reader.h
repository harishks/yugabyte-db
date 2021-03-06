//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef ROCKSDB_TABLE_CUCKOO_TABLE_READER_H
#define ROCKSDB_TABLE_CUCKOO_TABLE_READER_H

#ifndef ROCKSDB_LITE
#include <string>
#include <memory>
#include <utility>
#include <vector>

#include "yb/rocksdb/db/dbformat.h"
#include "yb/rocksdb/env.h"
#include "yb/rocksdb/options.h"
#include "yb/rocksdb/table/table_reader.h"
#include "yb/rocksdb/util/file_reader_writer.h"

namespace rocksdb {

class Arena;
class TableReader;
class InternalIterator;

class CuckooTableReader: public TableReader {
 public:
  CuckooTableReader(const ImmutableCFOptions& ioptions,
                    std::unique_ptr<RandomAccessFileReader>&& file,
                    uint64_t file_size, const Comparator* user_comparator,
                    uint64_t (*get_slice_hash)(const Slice&, uint32_t,
                                               uint64_t));
  ~CuckooTableReader() {}

  bool IsSplitSst() const override { return false; }

  void SetDataFileReader(unique_ptr<RandomAccessFileReader>&& data_file) override { assert(false); }

  std::shared_ptr<const TableProperties> GetTableProperties() const override {
    return table_props_;
  }

  Status status() const { return status_; }

  Status Get(const ReadOptions& read_options, const Slice& key,
             GetContext* get_context, bool skip_filters = false) override;

  InternalIterator* NewIterator(const ReadOptions&, Arena* arena = nullptr,
                                bool skip_filters = false) override;
  void Prepare(const Slice& target) override;

  // Report an approximation of how much memory has been used.
  size_t ApproximateMemoryUsage() const override;

  // Following methods are not implemented for Cuckoo Table Reader
  uint64_t ApproximateOffsetOf(const Slice& key) override { return 0; }
  void SetupForCompaction() override {}
  // End of methods not implemented.

 private:
  friend class CuckooTableIterator;
  void LoadAllKeys(std::vector<std::pair<Slice, uint32_t>>* key_to_bucket_id);
  std::unique_ptr<RandomAccessFileReader> file_;
  Slice file_data_;
  bool is_last_level_;
  bool identity_as_first_hash_;
  bool use_module_hash_;
  std::shared_ptr<const TableProperties> table_props_;
  Status status_;
  uint32_t num_hash_func_;
  std::string unused_key_;
  uint32_t key_length_;
  uint32_t user_key_length_;
  uint32_t value_length_;
  uint32_t bucket_length_;
  uint32_t cuckoo_block_size_;
  uint32_t cuckoo_block_bytes_minus_one_;
  uint64_t table_size_;
  const Comparator* ucomp_;
  uint64_t (*get_slice_hash_)(const Slice& s, uint32_t index,
      uint64_t max_num_buckets);
};

}  // namespace rocksdb
#endif  // ROCKSDB_LITE

#endif  // ROCKSDB_TABLE_CUCKOO_TABLE_READER_H

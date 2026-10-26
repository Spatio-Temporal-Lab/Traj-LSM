//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once

#include <mutex>
#include <vector>

#include "db/compaction/compaction_picker.h"

namespace ROCKSDB_NAMESPACE {
// Picking compactions for leveled compaction. See wiki page
// https://github.com/facebook/rocksdb/wiki/Leveled-Compaction
// for description of Leveled compaction.
class LevelCompactionPicker : public CompactionPicker {
 public:
  LevelCompactionPicker(const ImmutableOptions& ioptions,
                        const InternalKeyComparator* icmp)
      : CompactionPicker(ioptions, icmp) {}
  Compaction* PickCompaction(
      const std::string& cf_name, const MutableCFOptions& mutable_cf_options,
      const MutableDBOptions& mutable_db_options,
      const std::vector<SequenceNumber>& /* existing_snapshots */,
      const SnapshotChecker* /* snapshot_checker */,
      VersionStorageInfo* vstorage, LogBuffer* log_buffer,
      const std::string& full_history_ts_low,
      bool /*require_max_output_level*/ = false) override;

  bool NeedsCompaction(const VersionStorageInfo* vstorage) const override;
  bool NeedsCompaction(
      const VersionStorageInfo* vstorage,
      const MutableCFOptions& mutable_cf_options) const override;
  void ReleaseCompactionFiles(Compaction* c, const Status& status) override;
  bool HasPendingSTIncomingFile(int level) const;
  void ConsumePendingSTIncomingFile(int level);
  void ClearPendingSTIncomingFiles(int level) const;
  void RecordSTIncomingFiles(int level, size_t count);

 private:
  mutable std::mutex st_incoming_mutex_;
  mutable std::vector<size_t> st_pending_incoming_files_;
};

}  // namespace ROCKSDB_NAMESPACE

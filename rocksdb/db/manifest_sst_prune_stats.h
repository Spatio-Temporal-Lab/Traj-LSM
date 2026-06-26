//  Copyright (c) Meta Platforms, Inc. and affiliates.
//
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <cstdint>

#include "rocksdb/rocksdb_namespace.h"

namespace ROCKSDB_NAMESPACE {

// Per-MANIFEST-shard SST file counts after replaying the descriptor log.
struct ManifestSstPruneStats {
  uint64_t live_files = 0;
  uint64_t with_st_meta = 0;
  uint64_t missing_st_meta = 0;
  uint64_t range_del_blocked = 0;
  uint64_t pruned_disjoint = 0;
  uint64_t kept_intersect = 0;
};

}  // namespace ROCKSDB_NAMESPACE

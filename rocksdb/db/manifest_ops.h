//  Copyright (c) Meta Platforms, Inc. and affiliates.
//
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once
#include <limits>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "rocksdb/env.h"

namespace ROCKSDB_NAMESPACE {

// Parameter is_retry=true sets the verify_and_reconstruct_read flag.
// It comes handy when caller intends to re-read the data with much stronger
// data integrity checking - e.g. in case of a perceived file corruption.
Status GetCurrentManifestPath(const std::string& dbname, FileSystem* fs,
                              bool is_retry, std::string* manifest_path,
                              uint64_t* manifest_file_number);

// Returns one or more MANIFEST paths to replay, in replay order.
// CURRENT must point to "MANIFEST_SET-xxxxxx".
Status GetCurrentManifestPaths(const std::string& dbname, FileSystem* fs,
                               bool is_retry,
                               std::vector<std::string>* manifest_paths,
                               std::vector<uint64_t>* manifest_file_numbers,
                               uint64_t* manifest_set_generation = nullptr,
                               std::vector<uint64_t>* manifest_time_mins =
                                   nullptr,
                               std::vector<uint64_t>* manifest_time_maxs =
                                   nullptr,
                               std::vector<float>* manifest_mbr_x_mins =
                                   nullptr,
                               std::vector<float>* manifest_mbr_x_maxs =
                                   nullptr,
                               std::vector<float>* manifest_mbr_y_mins =
                                   nullptr,
                               std::vector<float>* manifest_mbr_y_maxs =
                                   nullptr,
                               std::vector<uint64_t>* manifest_master_counts =
                                   nullptr,
                               std::vector<uint64_t>* manifest_ghost_counts =
                                   nullptr);

// Query-time helper: returns MANIFEST shard paths whose [tmin,tmax] intersects
// [query_t_min, query_t_max]. Unknown-range shards are always kept. Returned
// vectors preserve replay order among selected shards.
Status GetCurrentManifestPathsFilteredByTimeRange(
    const std::string& dbname, FileSystem* fs, bool is_retry,
    uint64_t query_t_min, uint64_t query_t_max,
    std::vector<std::string>* filtered_manifest_paths,
    std::vector<uint64_t>* filtered_manifest_file_numbers,
    uint64_t* manifest_set_generation = nullptr,
    std::vector<uint64_t>* filtered_manifest_time_mins = nullptr,
    std::vector<uint64_t>* filtered_manifest_time_maxs = nullptr,
    std::vector<float>* filtered_manifest_mbr_x_mins = nullptr,
    std::vector<float>* filtered_manifest_mbr_x_maxs = nullptr,
    std::vector<float>* filtered_manifest_mbr_y_mins = nullptr,
    std::vector<float>* filtered_manifest_mbr_y_maxs = nullptr,
    std::vector<uint64_t>* filtered_manifest_master_counts = nullptr,
    std::vector<uint64_t>* filtered_manifest_ghost_counts = nullptr,
    bool enable_spatial_window = false,
    float query_x_min = 0.0f, float query_x_max = 0.0f,
    float query_y_min = 0.0f, float query_y_max = 0.0f);

// Query-time helper: combine prefilter + cached shard loads. Only selected
// shard contents are loaded into memory.
Status LoadCurrentManifestShardsByTimeRangeCached(
    const std::string& dbname, FileSystem* fs, bool is_retry,
    uint64_t query_t_min, uint64_t query_t_max,
    std::vector<std::string>* filtered_manifest_paths,
    std::vector<std::string>* filtered_manifest_contents,
    uint64_t* manifest_set_generation = nullptr);

// Query-time helper: prefilter shards by time (and optional spatial window) and
// aggregate per-shard global counters parsed from MANIFEST_SET entry metadata.
// When a shard has no explicit counters, it contributes 0.
Status GetCurrentManifestGlobalCountsFilteredByTimeRange(
    const std::string& dbname, FileSystem* fs, bool is_retry,
    uint64_t query_t_min, uint64_t query_t_max, uint64_t* total_masters,
    uint64_t* total_ghosts, uint64_t* manifest_set_generation = nullptr,
    bool enable_spatial_window = false, float query_x_min = 0.0f,
    float query_x_max = 0.0f, float query_y_min = 0.0f,
    float query_y_max = 0.0f);

// Best-effort: read a MANIFEST (descriptor log) into memory using a small LRU.
// This is intended for query-time/index builders that only need a subset of
// MANIFEST shards in memory at once.
Status ReadManifestShardToStringCached(FileSystem* fs, bool is_retry,
                                       const std::string& manifest_path,
                                       std::string* content);

// Select MANIFEST shard indices whose [tmin,tmax] overlaps [query_t_min,
// query_t_max]. Unknown shards (tmin==0 && tmax==max uint64_t, as parsed from
// plain MANIFEST-* lines) are always included.
void FilterManifestShardIndicesByTimeRange(
    uint64_t query_t_min, uint64_t query_t_max,
    const std::vector<uint64_t>& shard_t_mins,
    const std::vector<uint64_t>& shard_t_maxs,
    std::vector<size_t>* out_indices,
    const std::vector<float>* shard_x_mins = nullptr,
    const std::vector<float>* shard_x_maxs = nullptr,
    const std::vector<float>* shard_y_mins = nullptr,
    const std::vector<float>* shard_y_maxs = nullptr,
    bool enable_spatial_window = false,
    float query_x_min = 0.0f, float query_x_max = 0.0f,
    float query_y_min = 0.0f, float query_y_max = 0.0f);
}  // namespace ROCKSDB_NAMESPACE

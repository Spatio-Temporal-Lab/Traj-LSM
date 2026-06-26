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
#include <unordered_set>
#include <vector>

#include "db/manifest_sst_prune_stats.h"
#include "rocksdb/env.h"

namespace ROCKSDB_NAMESPACE {

// Parameter is_retry=true sets the verify_and_reconstruct_read flag.
// It comes handy when caller intends to re-read the data with much stronger
// data integrity checking - e.g. in case of a perceived file corruption.
Status GetCurrentManifestPath(const std::string& dbname, FileSystem* fs,
                              bool is_retry, std::string* manifest_path,
                              uint64_t* manifest_file_number);

// Returns one or more MANIFEST paths to replay, in replay order.
// CURRENT may point to "MANIFEST-xxxxxx" (single chain, default) or legacy
// "M_manager-xxxxxx" / "MANIFEST_SET-xxxxxx" (multi-shard index).
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
                                   nullptr,
                               std::vector<uint64_t>* manifest_host_buckets =
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
// aggregate per-shard global counters parsed from legacy MANIFEST_SET metadata.
// M_manager DBs return 0 (masters/ghosts deprecated). When a shard has no
// explicit counters, it contributes 0.
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
    float query_y_min = 0.0f, float query_y_max = 0.0f,
    uint32_t manifest_bucket_width_sec = 0,
    const std::vector<uint64_t>* shard_host_buckets = nullptr);

// Combined M_manager shard filter + SST file-level filter for one query window.
struct StPruneLayerStats {
  uint64_t m_manager_generation = 0;
  uint64_t manifest_shard_total = 0;
  uint64_t manifest_shard_selected = 0;
  ManifestSstPruneStats sst_all_shards;
  ManifestSstPruneStats sst_selected_shards;
};

// Replay MANIFEST shard(s) offline and count live SST files vs query window.
Status CountLiveSstFilesInManifestPath(
    const std::string& manifest_path, FileSystem* fs, bool is_retry,
    uint32_t column_family_id_filter, uint32_t query_t_min, uint32_t query_t_max,
    float query_x_min, float query_x_max, float query_y_min, float query_y_max,
    ManifestSstPruneStats* stats);

// Replay multiple shard logs in order (per-bucket incremental descriptors).
Status CountLiveSstFilesInManifestPaths(
    const std::vector<std::string>& manifest_paths, FileSystem* fs, bool is_retry,
    uint32_t column_family_id_filter, uint32_t query_t_min, uint32_t query_t_max,
    float query_x_min, float query_x_max, float query_y_min, float query_y_max,
    ManifestSstPruneStats* stats);

struct StSstLevelRow {
  int level = -1;
  uint64_t file_count = 0;
  uint64_t total_bytes = 0;
  uint64_t total_entries = 0;
};

struct StLiveSstLevelStats {
  uint64_t m_manager_shards = 0;
  uint64_t disk_sst_files = 0;
  uint64_t unique_live_files = 0;
  uint64_t manifest_refs_not_on_disk = 0;
  uint64_t disk_sst_without_manifest_level = 0;
  uint64_t with_st_file_meta = 0;
  uint64_t without_st_file_meta = 0;
  std::vector<StSstLevelRow> per_level;
};

// Replay all M_manager MANIFEST shards (no DB::Open) and aggregate live SST by level.
Status GetLiveSstLevelStatsFromMManager(const std::string& dbname, FileSystem* fs,
                                        bool is_retry, StLiveSstLevelStats* out);

// M_manager shard prefilter + per-shard SST stats (summed across shards).
Status GetStPruneLayerStats(
    const std::string& dbname, FileSystem* fs, bool is_retry,
    uint32_t query_t_min, uint32_t query_t_max, bool enable_spatial_window,
    float query_x_min, float query_x_max, float query_y_min, float query_y_max,
    uint32_t column_family_id_filter, StPruneLayerStats* out,
    std::vector<ManifestSstPruneStats>* per_selected_shard_stats = nullptr);

// Per-SST st_file_meta filter applied at manifest level.
// Uses Version's in-memory file metadata (sourced from Manifest st_file_meta).
// For each SST: time intersection AND spatial intersection must both hold.
// Files without st_file_meta are kept conservatively.
struct StManifestSstFilterResult {
  std::unordered_set<uint64_t> file_numbers;
  uint64_t total_files = 0;
  uint64_t time_disjoint = 0;
  uint64_t space_disjoint = 0;
  uint64_t missing_meta = 0;
  uint64_t passed = 0;
};

// Filter all SSTs in the Version by per-SST st_file_meta against query window.
// This is the Manifest-level coarse filter: m_manager selects manifest shards,
// then this function filters individual SSTs within selected shards.
StManifestSstFilterResult ManifestFilterSstByStFileMeta(
    const class VersionStorageInfo& vstorage, uint32_t q_t_min,
    uint32_t q_t_max, float q_x_min, float q_x_max, float q_y_min,
    float q_y_max);

}  // namespace ROCKSDB_NAMESPACE

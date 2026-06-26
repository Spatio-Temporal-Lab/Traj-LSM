#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rocksdb/status.h>

namespace rocksdb {
class DB;
class FileSystem;
class Slice;
struct LiveFileMetaData;

struct ManifestEntry {
  std::string manifest_name;
  uint32_t t_min = 0;
  uint32_t t_max = 0;
};

struct ManifestFileEntry {
  uint64_t file_number = 0;
  uint32_t level = 0;
  uint64_t file_size = 0;
  uint32_t t_min = 0;
  uint32_t t_max = 0;
  uint32_t t_end_max = 0;
  float x_min = 0.f;
  float x_max = 0.f;
  float y_min = 0.f;
  float y_max = 0.f;
  std::string smallest_key;
  std::string largest_key;
  bool has_time = false;
  bool has_space = false;
  bool has_key_range = false;
  bool is_conservative = true;
};

struct ManagerState {
  std::string current_manifest;
  std::vector<ManifestEntry> entries;
};

struct ManifestState {
  std::string manifest_name;
  std::vector<ManifestFileEntry> files;
};

struct ManifestSstPruneStats {
  uint64_t live_files = 0;
  uint64_t with_st_meta = 0;
  uint64_t missing_st_meta = 0;
  uint64_t range_del_blocked = 0;
  uint64_t kept_intersect = 0;
  uint64_t pruned_disjoint = 0;
};

struct StPruneLayerStats {
  uint64_t m_manager_generation = 0;
  uint64_t manifest_shard_total = 0;
  uint64_t manifest_shard_selected = 0;
  ManifestSstPruneStats sst_all_shards;
  ManifestSstPruneStats sst_selected_shards;
};

struct StLiveSstLevelStatsRow {
  uint32_t level = 0;
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
  std::vector<StLiveSstLevelStatsRow> per_level;
};

Status GetCurrentManifestPaths(
    const std::string& db_path, FileSystem* fs, bool is_retry,
    std::vector<std::string>* paths, std::vector<uint64_t>* numbers,
    uint64_t* generation, std::vector<uint64_t>* tmins,
    std::vector<uint64_t>* tmaxs, std::vector<float>* xmins,
    std::vector<float>* xmaxs, std::vector<float>* ymins,
    std::vector<float>* ymaxs, std::vector<uint64_t>* masters,
    std::vector<uint64_t>* ghosts);

void FilterManifestShardIndicesByTimeRange(
    uint64_t q_t_min, uint64_t q_t_max, const std::vector<uint64_t>& tmins,
    const std::vector<uint64_t>& tmaxs, std::vector<size_t>* selected,
    const std::vector<float>* xmins = nullptr,
    const std::vector<float>* xmaxs = nullptr,
    const std::vector<float>* ymins = nullptr,
    const std::vector<float>* ymaxs = nullptr,
    bool enable_spatial_window = false, float q_x_min = 0.f,
    float q_x_max = 0.f, float q_y_min = 0.f, float q_y_max = 0.f);

Status GetCurrentManifestGlobalCountsFilteredByTimeRange(
    const std::string& db_path, FileSystem* fs, bool is_retry,
    uint64_t q_t_min, uint64_t q_t_max, uint64_t* masters,
    uint64_t* ghosts, uint64_t* generation, bool enable_spatial_window = false,
    float q_x_min = 0.f, float q_x_max = 0.f, float q_y_min = 0.f,
    float q_y_max = 0.f);

Status GetStPruneLayerStats(
    const std::string& db_path, FileSystem* fs, bool is_retry,
    uint32_t q_t_min, uint32_t q_t_max, bool enable_spatial_window,
    float q_x_min, float q_x_max, float q_y_min, float q_y_max,
    uint32_t max_manifest_shards, StPruneLayerStats* out,
    std::vector<ManifestSstPruneStats>* per_shard = nullptr);

Status GetLiveSstLevelStatsFromMManager(const std::string& db_path,
                                        FileSystem* fs, bool is_retry,
                                        StLiveSstLevelStats* out);
}

namespace manifest_ops {

using ManifestEntry = rocksdb::ManifestEntry;
using ManifestFileEntry = rocksdb::ManifestFileEntry;
using ManagerState = rocksdb::ManagerState;
using ManifestState = rocksdb::ManifestState;

// Parse the on-disk CURRENT + M_manager chain in the DB directory.
bool LoadManagerState(const std::string& db_dir, ManagerState* out);

// Return the time range of all live manifest entries. False if nothing can be read.
bool GetManagerTimeRange(const std::string& db_dir, uint32_t* t_min, uint32_t* t_max);

// Return the live manifest name from CURRENT / M_manager.
bool GetCurrentManifestName(const std::string& db_dir, std::string* out);

// Parse live SST metadata from the DB using RocksDB's live file APIs.
bool LoadManifestState(rocksdb::DB* db, ManifestState* out);

// Best-effort build of live-file candidates directly from RocksDB live file metadata.
std::vector<ManifestFileEntry> BuildCandidateFilesFromLive(
    rocksdb::DB* db,
    uint32_t q_t_min, uint32_t q_t_max,
    float q_x_min, float q_x_max,
    float q_y_min, float q_y_max);

// Conservative candidate filter on manifest file entries.
std::vector<ManifestFileEntry> FilterCandidateFiles(
    const std::vector<ManifestFileEntry>& files,
    uint32_t q_t_min, uint32_t q_t_max,
    float q_x_min, float q_x_max,
    float q_y_min, float q_y_max);

// Check whether a user key falls into a candidate's best-effort key range.
bool CandidateMayContainKey(const ManifestFileEntry& entry,
                            const rocksdb::Slice& user_key);

// Build candidate file entries from live files and optional current query range.
std::vector<ManifestFileEntry> BuildCandidateFilesFromLive(
    rocksdb::DB* db,
    uint32_t q_t_min, uint32_t q_t_max,
    float q_x_min, float q_x_max,
    float q_y_min, float q_y_max);

}  // namespace manifest_ops

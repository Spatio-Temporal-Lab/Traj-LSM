// Batch spatial query mode for st_meta_read_bench (TMan SpatialQuery-aligned).
#pragma once

#include <cstdint>
#include <string>

namespace rocksdb {
class DB;
}

struct StMetaReadBenchBatchConfig {
  std::string query_batch_file;
  uint32_t envelope_t_min = 0;
  uint32_t envelope_t_max = 0;
  uint32_t bucket_id_min = 0;
  uint32_t bucket_id_max = 0;
  uint32_t bucket_width_sec = 3600;
  std::string prune_mode = "sst_manifest";
  bool verify_kv_results = true;
  uint32_t time_bucket_count = 32;
  uint32_t rtree_leaf_size = 8;
  bool sst_manifest_key_level = true;
  bool sst_manifest_adaptive_key_gate = false;
  bool sst_manifest_adaptive_block_gate = false;
  bool sst_manifest_key_level_boundary_only = false;
  bool vm_contains_batch_prewarm = true;
  float adaptive_overlap_threshold = 0.6f;
  float adaptive_block_overlap_threshold = 0.85f;
  bool virtual_merge_enable = false;
  bool virtual_merge_auto = false;
  uint32_t vm_time_span_sec_threshold = 6 * 3600;
  bool manifest_linear_file_skip_only = false;
  bool file_level_rtree_skip_ratio_gate = false;
  float file_level_rtree_min_skip_ratio = 0.2f;
  uint32_t debug_max_distinct_sst_files = 0;
};

// One DB::Open; run all queries in query_batch_file.
// Each token: minLng,minLat,maxLng,maxLat  OR  +startMs,endMs (TMan STQuery).
// Prints spatial_batch_query lines + spatial_batch_summary CSV rows.
int RunStMetaReadBenchSpatialBatch(rocksdb::DB* db, const StMetaReadBenchBatchConfig& cfg);

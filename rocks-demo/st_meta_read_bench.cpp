// Real RocksDB read benchmark on an EXISTING DB (e.g. verify_wuxi_segment_bucket3600_sst).
// Measures wall time + PerfContext block_read_count + IOStatsContext bytes/read_nanos
// for baseline vs experimental_st_prune_scan (same iterator API as st_meta_scan_bench).
//
// Default prune window = Wuxi wx_strat_narrow_01 (tools/st_validity_experiment_windows_wuxi_stratified12_n4m4w4.csv).
//
// Usage:
//   st_meta_read_bench --db D:/Project/data/verify_wuxi_segment_bucket3600_sst \
//     --prune-t-min 1596200000 --prune-t-max 1596203600 \
//     --prune-x-min 120.02 --prune-x-max 120.34 --prune-y-min 31.52 --prune-y-max 31.88
//
// Use --no-full-scan to only run the pruned pass (faster if you only care about query path).
// Use --no-prune-scan to only run the full-table pass (stock-like: no experimental_st_prune_scan).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/iostats_context.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>
#include <rocksdb/perf_context.h>
#include <rocksdb/perf_level.h>
#include <rocksdb/statistics.h>
#include <rocksdb/table.h>

#include "db/manifest_ops.h"
#include "segment_value_codec.hpp"
#include "segment_value_global.hpp"
#include "table/st_meta_user_key.h"
#include "util/hash.h"

#include "st_meta_read_bench_batch.h"

namespace {

using ROCKSDB_NAMESPACE::UserKeySpatioTemporalInWindow;
using ROCKSDB_NAMESPACE::Hash64;
using ROCKSDB_NAMESPACE::StQuerySegmentUidDedup;

constexpr unsigned char kMagicPointKey = 0xE5;
constexpr unsigned char kMagicSegmentLegacyKey = 0xE6;
constexpr unsigned char kMagicSegmentTzidKey = 0xE7;

static uint32_t CfEventTimeBucketWidthSec(rocksdb::DB* db) {
  const rocksdb::Options opts = db->GetOptions();
  const uint32_t w = opts.experimental_compaction_event_time_bucket_width;
  return w == 0 ? ROCKSDB_NAMESPACE::kSpatioTemporalE7DefaultBucketSeconds : w;
}

// Default RocksDB internal key: <user_key | 8-byte seq+type>.
inline rocksdb::Slice UserKeyFromInternalKey(const rocksdb::Slice& internal_key) {
  constexpr size_t kFooter = 8;
  if (internal_key.size() < kFooter) {
    return rocksdb::Slice();
  }
  return rocksdb::Slice(internal_key.data(),
                        internal_key.size() - kFooter);
}

inline bool LooksLikeStUserKey(const rocksdb::Slice& user_key) {
  if (user_key.size() < 1) {
    return false;
  }
  const unsigned char magic = static_cast<unsigned char>(user_key.data()[0]);
  if (magic == kMagicPointKey) {
    return user_key.size() >= 13;
  }
  if (magic == kMagicSegmentLegacyKey) {
    return user_key.size() >= 25;
  }
  if (magic == kMagicSegmentTzidKey) {
    return user_key.size() >= 31;
  }
  return false;
}

using ROCKSDB_NAMESPACE::UserKeyFromIteratorKey;

bool HasLegacyE6Keys(rocksdb::DB* db, size_t sample_limit = 100000) {
  if (db == nullptr) {
    return false;
  }
  rocksdb::ReadOptions ro;
  ro.total_order_seek = true;
  ro.fill_cache = false;
  std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro));
  size_t seen = 0;
  for (it->SeekToFirst(); it->Valid() && seen < sample_limit; it->Next(), ++seen) {
    const rocksdb::Slice uk = UserKeyFromIteratorKey(it->key());
    if (uk.size() > 0 &&
        static_cast<unsigned char>(uk.data()[0]) == kMagicSegmentLegacyKey) {
      return true;
    }
  }
  return false;
}

bool ParseU32(const char* s, uint32_t* out) {
  if (s == nullptr || out == nullptr) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const unsigned long v = std::strtoul(s, &end, 10);
  if (end == s || *end != '\0' || errno != 0) {
    return false;
  }
  if (v > static_cast<unsigned long>(std::numeric_limits<uint32_t>::max())) {
    return false;
  }
  *out = static_cast<uint32_t>(v);
  return true;
}

bool ParseFloat(const char* s, float* out) {
  if (s == nullptr || out == nullptr) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const double d = std::strtod(s, &end);
  if (end == s || *end != '\0' || errno != 0) {
    return false;
  }
  *out = static_cast<float>(d);
  return true;
}

struct PointTruthEval {
  bool supported = false;
  bool in_window = false;
  bool decode_failed = false;
  bool legacy_segment_value = false;
  bool ghost_segment_value = false;
  bool ghost_missing_master_key = false;
  bool ghost_master_lookup_failed = false;
  bool ghost_master_decode_failed = false;
  bool ghost_master_is_ghost = false;
  bool ghost_master_meta_mismatch = false;
};

PointTruthEval EvalPointTruthInWindow(rocksdb::DB* db,
                                      const rocksdb::ReadOptions& read_options,
                                      const rocksdb::Slice& user_key,
                                      const rocksdb::Slice& value,
                                      uint32_t t_min, uint32_t t_max,
                                      float x_min, float x_max, float y_min,
                                      float y_max) {
  PointTruthEval out;
  if (user_key.size() < 1) {
    return out;
  }
  const unsigned char magic =
      static_cast<unsigned char>(user_key.data()[0]);
  if (magic == kMagicPointKey) {
    out.supported = true;
    out.in_window = UserKeySpatioTemporalInWindow(user_key, t_min, t_max, x_min,
                                                  x_max, y_min, y_max);
    return out;
  }
  if (magic != kMagicSegmentLegacyKey && magic != kMagicSegmentTzidKey) {
    return out;
  }

  out.supported = true;
  segval::MaterializedSegmentValue materialized{};
  if (!segval::DecodeAndMaterializeSegmentValue(db, read_options, user_key, value,
                                                &materialized)) {
    out.decode_failed = true;
    return out;
  }
  const segval::DecodedSegmentValue& decoded = materialized.decoded;
  if (decoded.is_legacy) {
    out.legacy_segment_value = true;
    return out;
  }
  if (materialized.was_ghost) {
    out.ghost_segment_value = true;
    out.ghost_missing_master_key = decoded.master_key.empty();
    out.ghost_master_lookup_failed =
        !materialized.master_lookup_ok && !decoded.master_key.empty();
    out.ghost_master_decode_failed = materialized.master_decode_failed;
    out.ghost_master_is_ghost = materialized.master_is_ghost;
    out.ghost_master_meta_mismatch = materialized.master_meta_mismatch;
    if (out.ghost_missing_master_key || out.ghost_master_lookup_failed ||
        out.ghost_master_decode_failed || out.ghost_master_is_ghost ||
        out.ghost_master_meta_mismatch) {
      return out;
    }
  }
  for (const auto& p : decoded.points) {
    if (p.unix_s >= t_min && p.unix_s <= t_max && p.lon >= x_min &&
        p.lon <= x_max && p.lat >= y_min && p.lat <= y_max) {
      out.in_window = true;
      return out;
    }
  }
  return out;
}

inline bool IsSegmentUserKeyMagic(unsigned char magic) {
  return magic == kMagicSegmentLegacyKey || magic == kMagicSegmentTzidKey;
}

rocksdb::Options BenchOpenOptions(int block_cache_mb, bool large_cache,
                                  bool experimental_global_master_ghost,
                                  bool experimental_global_verify_ghost) {
  rocksdb::Options o;
  o.create_if_missing = false;
  o.statistics = rocksdb::CreateDBStatistics();
  o.experimental_global_master_ghost = experimental_global_master_ghost;
  o.experimental_global_verify_ghost = experimental_global_verify_ghost;
  rocksdb::BlockBasedTableOptions tb;
  tb.experimental_spatio_temporal_meta_in_index_value = true;
  const int mb =
      large_cache ? std::max(64, block_cache_mb) : std::max(1, block_cache_mb);
  tb.block_cache = rocksdb::NewLRUCache(static_cast<size_t>(mb) * size_t{1048576});
  tb.cache_index_and_filter_blocks = true;
  o.table_factory.reset(rocksdb::NewBlockBasedTableFactory(tb));
  return o;
}

struct IoSnap {
  uint64_t bytes_read = 0;
  uint64_t read_nanos = 0;
};

IoSnap IoStatsSnap() {
  IoSnap s;
  rocksdb::IOStatsContext* io = rocksdb::get_iostats_context();
  if (io != nullptr) {
    s.bytes_read = io->bytes_read;
    s.read_nanos = io->read_nanos;
  }
  return s;
}

void IoStatsDiff(const IoSnap& before, const char* label) {
  rocksdb::IOStatsContext* io = rocksdb::get_iostats_context();
  if (io == nullptr) {
    return;
  }
  const uint64_t br = io->bytes_read - before.bytes_read;
  const uint64_t rn = io->read_nanos - before.read_nanos;
  std::cout << label << " IOStatsContext delta: bytes_read=" << br
            << " read_nanos=" << rn << "\n";
}

bool ComputeGlobalCountsFromRawKv(
    const std::string& db_path, int block_cache_mb, bool large_cache,
    uint32_t t_min, uint32_t t_max, float x_min, float x_max, float y_min,
    float y_max, uint64_t* masters, uint64_t* ghosts) {
  if (masters == nullptr || ghosts == nullptr) {
    return false;
  }
  *masters = 0;
  *ghosts = 0;

  rocksdb::Options raw_opt =
      BenchOpenOptions(block_cache_mb, large_cache,
                       /*experimental_global_master_ghost=*/false,
                       /*experimental_global_verify_ghost=*/false);
  std::unique_ptr<rocksdb::DB> raw_db;
  rocksdb::Status s = rocksdb::DB::OpenForReadOnly(raw_opt, db_path, &raw_db);
  if (!s.ok()) {
    return false;
  }

  rocksdb::ReadOptions ro(rocksdb::Env::IOActivity::kDBIterator);
  ro.total_order_seek = true;
  ro.fill_cache = false;
  std::unique_ptr<rocksdb::Iterator> it(raw_db->NewIterator(ro));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const rocksdb::Slice uk = UserKeyFromIteratorKey(it->key());
    const rocksdb::Slice v = it->value();
    PointTruthEval pe = EvalPointTruthInWindow(raw_db.get(), ro, uk, v, t_min,
                                               t_max, x_min, x_max, y_min, y_max);
    if (!pe.supported || !pe.in_window) {
      continue;
    }
    segval::DecodedSegmentValue decoded{};
    if (!segval::DecodeSegmentValue(std::string(v.data(), v.size()), &decoded)) {
      continue;
    }
    if (decoded.is_ghost) {
      ++(*ghosts);
    } else {
      ++(*masters);
    }
  }
  return it->status().ok();
}

}  // namespace

int main(int argc, char** argv) {
  constexpr const char* kId = "st_meta_read_bench 2026-04-12 open-existing wuxi-default-prune-window";
  std::cout << kId << "\n";

  std::string db_path;
  // Default window: Wuxi stratified narrow_01 (st_validity_experiment_windows_wuxi_stratified12_n4m4w4.csv).
  uint32_t prune_t_min = 1596200000;
  uint32_t prune_t_max = 1596203600;
  float prune_x_min = 120.020000f;
  float prune_x_max = 120.340000f;
  float prune_y_min = 31.520000f;
  float prune_y_max = 31.880000f;
  bool run_full = true;
  bool run_prune = true;
  int block_cache_mb = 8;
  bool large_cache = false;
  // prune-mode controls which parts of our ST prune pipeline are enabled.
  // - sst:      SST-side only (block-level + key-level), no file-level skip
  // - manifest: Manifest-side only (file-level skip), no block/key pruning
  // - sst_manifest: all features (default; matches previous behavior)
  std::string prune_mode = "sst_manifest";
  // Baseline pass: "window" = full CF iterator + count only ST keys inside the same
  // axis-aligned window as prune_scan (brute-force query). "all_cf" = count every KV
  // (legacy Phase-A: whole column family).
  std::string full_scan_mode = "window";
  bool verify_kv_results = false;
  bool verify_kv_results_strict_exit = false;
  bool query_segment_uid_dedup = true;
  // External reference from segments_meta (time+MBR) / spatial-only window CSV.
  uint64_t ref_meta_st_segments = 0;
  uint64_t ref_spatial_segments = 0;
  float ref_segment_kv_min_ratio = 0.25f;
  float ref_segment_kv_max_ratio = 8.0f;
  bool experimental_global_master_ghost = false;
  bool experimental_global_verify_ghost = false;
  bool global_manifest_counts_fallback_scan = false;
  bool sst_manifest_key_level = true;
  bool sst_manifest_adaptive_key_gate = false;
  bool sst_manifest_adaptive_block_gate = false;
  bool sst_manifest_key_level_boundary_only = false;
  bool vm_contains_batch_prewarm = true;
  uint32_t time_bucket_count = 32;
  uint32_t rtree_leaf_size = 8;
  float adaptive_overlap_threshold = 0.6f;
  float adaptive_block_overlap_threshold = 0.85f;
  bool virtual_merge_enable = false;
  bool virtual_merge_auto = false;
  uint32_t vm_time_span_sec_threshold = 6 * 3600;
  // Override event_time_bucket_width_sec (0 = use DB option).
  uint32_t event_time_bucket_width_sec_override = 0;
  // manifest-only: use sequential disjoint checks only (debug / A-B vs bucket path).
  bool manifest_linear_file_skip_only = false;
  // When time-bucket R-tree file path is on: pre-scan eligible SSTs; if
  // disjoint/eligible < threshold, skip BVH and use linear disjoint only.
  bool file_level_rtree_skip_ratio_gate = false;
  float file_level_rtree_min_skip_ratio = 0.2f;
  bool allow_legacy_e6 = false;
  // Same DB + same ReadOptions: repeat NewIterator + full prune scan to amortize
  // Version-level ST meta cache (LevelIterator compact arrays) across iterators.
  int iterator_repeat = 1;
  // Scheme B (bench-only): stop after N distinct SST opens per level iterator
  // (forward SeekToFirst+Next). 0 = unlimited. Results are incomplete when hit.
  uint32_t debug_max_distinct_sst_files = 0;
  std::string query_batch_file;

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (std::strcmp(a, "--db") == 0 && i + 1 < argc) {
      db_path = argv[++i];
      continue;
    }
    if (std::strcmp(a, "--prune-t-min") == 0 && i + 1 < argc) {
      if (!ParseU32(argv[++i], &prune_t_min)) {
        std::cerr << "Invalid --prune-t-min\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--prune-t-max") == 0 && i + 1 < argc) {
      if (!ParseU32(argv[++i], &prune_t_max)) {
        std::cerr << "Invalid --prune-t-max\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--prune-x-min") == 0 && i + 1 < argc) {
      if (!ParseFloat(argv[++i], &prune_x_min)) {
        std::cerr << "Invalid --prune-x-min\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--prune-x-max") == 0 && i + 1 < argc) {
      if (!ParseFloat(argv[++i], &prune_x_max)) {
        std::cerr << "Invalid --prune-x-max\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--prune-y-min") == 0 && i + 1 < argc) {
      if (!ParseFloat(argv[++i], &prune_y_min)) {
        std::cerr << "Invalid --prune-y-min\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--prune-y-max") == 0 && i + 1 < argc) {
      if (!ParseFloat(argv[++i], &prune_y_max)) {
        std::cerr << "Invalid --prune-y-max\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--no-full-scan") == 0) {
      run_full = false;
      continue;
    }
    if (std::strcmp(a, "--no-prune-scan") == 0) {
      run_prune = false;
      continue;
    }
    if (std::strcmp(a, "--block-cache-mb") == 0 && i + 1 < argc) {
      errno = 0;
      const long v = std::strtol(argv[++i], nullptr, 10);
      if (errno != 0 || v < 1 || v > 4096) {
        std::cerr << "Invalid --block-cache-mb (1..4096)\n";
        return 1;
      }
      block_cache_mb = static_cast<int>(v);
      continue;
    }
    if (std::strcmp(a, "--large-cache") == 0) {
      large_cache = true;
      continue;
    }
    if (std::strcmp(a, "--prune-mode") == 0 && i + 1 < argc) {
      prune_mode = argv[++i];
      continue;
    }
    if (std::strcmp(a, "--full-scan-mode") == 0 && i + 1 < argc) {
      full_scan_mode = argv[++i];
      continue;
    }
    if (std::strcmp(a, "--verify-kv-results") == 0) {
      verify_kv_results = true;
      continue;
    }
    if (std::strcmp(a, "--verify-kv-results-strict-exit") == 0) {
      verify_kv_results = true;
      verify_kv_results_strict_exit = true;
      continue;
    }
    if (std::strcmp(a, "--no-query-segment-uid-dedup") == 0) {
      query_segment_uid_dedup = false;
      continue;
    }
    if (std::strcmp(a, "--ref-meta-st-segments") == 0 && i + 1 < argc) {
      char* end = nullptr;
      errno = 0;
      const unsigned long v = std::strtoul(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0' || errno != 0) {
        std::cerr << "Invalid --ref-meta-st-segments\n";
        return 1;
      }
      ref_meta_st_segments = static_cast<uint64_t>(v);
      continue;
    }
    if (std::strcmp(a, "--ref-spatial-segments") == 0 && i + 1 < argc) {
      char* end = nullptr;
      errno = 0;
      const unsigned long v = std::strtoul(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0' || errno != 0) {
        std::cerr << "Invalid --ref-spatial-segments\n";
        return 1;
      }
      ref_spatial_segments = static_cast<uint64_t>(v);
      continue;
    }
    if (std::strcmp(a, "--ref-segment-kv-min-ratio") == 0 && i + 1 < argc) {
      if (!ParseFloat(argv[++i], &ref_segment_kv_min_ratio) ||
          ref_segment_kv_min_ratio < 0.f) {
        std::cerr << "Invalid --ref-segment-kv-min-ratio\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--ref-segment-kv-max-ratio") == 0 && i + 1 < argc) {
      if (!ParseFloat(argv[++i], &ref_segment_kv_max_ratio) ||
          ref_segment_kv_max_ratio < ref_segment_kv_min_ratio) {
        std::cerr << "Invalid --ref-segment-kv-max-ratio\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--experimental-global-master-ghost") == 0) {
      experimental_global_master_ghost = true;
      continue;
    }
    if (std::strcmp(a, "--experimental-global-verify-ghost") == 0) {
      experimental_global_verify_ghost = true;
      continue;
    }
    if (std::strcmp(a, "--global-manifest-counts-fallback-scan") == 0) {
      global_manifest_counts_fallback_scan = true;
      continue;
    }
    if (std::strcmp(a, "--sst-manifest-key-level") == 0 && i + 1 < argc) {
      uint32_t v = 0;
      if (!ParseU32(argv[++i], &v) || (v != 0 && v != 1)) {
        std::cerr << "Invalid --sst-manifest-key-level (expected 0|1)\n";
        return 1;
      }
      sst_manifest_key_level = (v == 1);
      continue;
    }
    if (std::strcmp(a, "--sst-manifest-adaptive-key-gate") == 0) {
      sst_manifest_adaptive_key_gate = true;
      continue;
    }
    if (std::strcmp(a, "--sst-manifest-adaptive-block-gate") == 0) {
      sst_manifest_adaptive_block_gate = true;
      continue;
    }
    if (std::strcmp(a, "--sst-manifest-key-level-boundary-only") == 0 &&
        i + 1 < argc) {
      uint32_t v = 0;
      if (!ParseU32(argv[++i], &v) || (v != 0 && v != 1)) {
        std::cerr << "Invalid --sst-manifest-key-level-boundary-only "
                     "(expected 0|1)\n";
        return 1;
      }
      sst_manifest_key_level_boundary_only = (v == 1);
      continue;
    }
    if (std::strcmp(a, "--vm-contains-batch-prewarm") == 0 && i + 1 < argc) {
      uint32_t v = 0;
      if (!ParseU32(argv[++i], &v) || (v != 0 && v != 1)) {
        std::cerr << "Invalid --vm-contains-batch-prewarm (expected 0|1)\n";
        return 1;
      }
      vm_contains_batch_prewarm = (v == 1);
      continue;
    }
    if (std::strcmp(a, "--event-time-bucket-width-sec") == 0 && i + 1 < argc) {
      if (!ParseU32(argv[++i], &event_time_bucket_width_sec_override) ||
          event_time_bucket_width_sec_override == 0) {
        std::cerr << "Invalid --event-time-bucket-width-sec\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--time-bucket-count") == 0 && i + 1 < argc) {
      if (!ParseU32(argv[++i], &time_bucket_count) || time_bucket_count == 0) {
        std::cerr << "Invalid --time-bucket-count\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--rtree-leaf-size") == 0 && i + 1 < argc) {
      if (!ParseU32(argv[++i], &rtree_leaf_size) || rtree_leaf_size == 0) {
        std::cerr << "Invalid --rtree-leaf-size\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--adaptive-overlap-threshold") == 0 && i + 1 < argc) {
      if (!ParseFloat(argv[++i], &adaptive_overlap_threshold) ||
          adaptive_overlap_threshold < 0.0f ||
          adaptive_overlap_threshold > 1.0f) {
        std::cerr << "Invalid --adaptive-overlap-threshold (0..1)\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--adaptive-block-overlap-threshold") == 0 &&
        i + 1 < argc) {
      if (!ParseFloat(argv[++i], &adaptive_block_overlap_threshold) ||
          adaptive_block_overlap_threshold < 0.0f ||
          adaptive_block_overlap_threshold > 1.0f) {
        std::cerr << "Invalid --adaptive-block-overlap-threshold (0..1)\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--virtual-merge") == 0) {
      virtual_merge_enable = true;
      continue;
    }
    if (std::strcmp(a, "--virtual-merge-auto") == 0) {
      virtual_merge_auto = true;
      continue;
    }
    if (std::strcmp(a, "--vm-time-span-sec-threshold") == 0 && i + 1 < argc) {
      if (!ParseU32(argv[++i], &vm_time_span_sec_threshold) ||
          vm_time_span_sec_threshold == 0) {
        std::cerr << "Invalid --vm-time-span-sec-threshold\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--manifest-linear-file-skip") == 0) {
      manifest_linear_file_skip_only = true;
      continue;
    }
    if (std::strcmp(a, "--file-level-rtree-skip-ratio-gate") == 0) {
      file_level_rtree_skip_ratio_gate = true;
      continue;
    }
    if (std::strcmp(a, "--file-level-rtree-min-skip-ratio") == 0 &&
        i + 1 < argc) {
      if (!ParseFloat(argv[++i], &file_level_rtree_min_skip_ratio) ||
          file_level_rtree_min_skip_ratio < 0.0f ||
          file_level_rtree_min_skip_ratio > 1.0f) {
        std::cerr << "Invalid --file-level-rtree-min-skip-ratio (0..1)\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--iterator-repeat") == 0 && i + 1 < argc) {
      errno = 0;
      const long v = std::strtol(argv[++i], nullptr, 10);
      if (errno != 0 || v < 1 || v > 100000) {
        std::cerr << "Invalid --iterator-repeat (1..100000)\n";
        return 1;
      }
      iterator_repeat = static_cast<int>(v);
      continue;
    }
    if (std::strcmp(a, "--debug-max-distinct-sst-files") == 0 && i + 1 < argc) {
      if (!ParseU32(argv[++i], &debug_max_distinct_sst_files)) {
        std::cerr << "Invalid --debug-max-distinct-sst-files\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--allow-legacy-e6") == 0) {
      allow_legacy_e6 = true;
      continue;
    }
    if (std::strcmp(a, "--query-batch-file") == 0 && i + 1 < argc) {
      query_batch_file = argv[++i];
      continue;
    }
    if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
      std::cout
          << "st_meta_read_bench --db <existing_dir> [options]\n"
             "  Opens DB read-only path; does NOT create or destroy data.\n"
             "  Compares baseline vs experimental_st_prune_scan (same window).\n"
             "  Metrics: wall_us, perf_context block_read_count, IOStatsContext "
             "bytes_read/read_nanos.\n"
             "Options:\n"
             "  --prune-t-min/max <u32>  (default: Wuxi randcov_w01, see EXPERIMENTS_AND_SCRIPTS.md §0.5)\n"
             "  --prune-x-min/max --prune-y-min/max <float>\n"
             "  --full-scan-mode <window|all_cf>\n"
             "                          window (default): baseline = full CF scan but\n"
             "                          count only ST user keys inside the prune window;\n"
             "                          all_cf: legacy baseline = count every KV in CF.\n"
             "  --no-full-scan           only run pruned iterator (skip baseline)\n"
             "  --no-prune-scan          only run baseline pass (no ST prune)\n"
             "  --prune-mode <sst|manifest|manifest_timebucket_rtree|sst_manifest|sst_manifest_pipeline|sst_manifest_adaptive>\n"
             "                          prune sub-features: SST only (block+key),\n"
             "                          Manifest only (file-level; default uses time buckets + spatial index),\n"
             "                          or both (default sst_manifest).\n"
             "  --block-cache-mb N       LRU block cache size (default 8)\n"
             "  --large-cache            use at least 64MB block cache\n"
             "  --verify-kv-results     full vs prune: compare in-window (key,value) set\n"
             "  --verify-kv-results-strict-exit  return non-zero when verify result is FAIL\n"
             "  --experimental-global-master-ghost  preserve Global master/ghost options when reopening DB\n"
             "  --experimental-global-verify-ghost  reopen with Global ghost verification (requires master-ghost)\n"
             "  --global-manifest-counts-fallback-scan  when manifest counts are 0, compute in-window master/ghost from raw KV scan\n"
             "  --sst-manifest-key-level <0|1>  in sst_manifest mode, enable/disable key-level (default 1)\n"
             "  --sst-manifest-adaptive-key-gate  in sst_manifest mode, auto-disable key-level for high-overlap files\n"
             "  --sst-manifest-adaptive-block-gate  in sst_manifest mode, auto-disable block+key for very high-overlap files\n"
             "  --sst-manifest-key-level-boundary-only <0|1>  skip key-level ST checks when block index MBR is fully inside query\n"
             "  --time-bucket-count N   manifest / manifest_timebucket_rtree / virtual-merge: bucket count (default 32)\n"
             "  --rtree-leaf-size N     manifest / manifest_timebucket_rtree / virtual-merge: spatial index leaf fanout (default 8)\n"
             "  --manifest-linear-file-skip  manifest mode only: disable bucket/spatial index (linear file disjoint scan)\n"
             "  --file-level-rtree-skip-ratio-gate  when time-bucket R-tree path is on: skip BVH if disjoint/eligible < min ratio\n"
             "  --file-level-rtree-min-skip-ratio F  gate threshold in [0,1] (default 0.2; requires --file-level-rtree-skip-ratio-gate)\n"
             "  --adaptive-overlap-threshold F  sst_manifest_adaptive key-level off threshold (0..1, default 0.6)\n"
             "  --adaptive-block-overlap-threshold F  sst_manifest block-level off threshold (0..1, default 0.85)\n"
             "  --vm-contains-batch-prewarm <0|1>  enable/disable LevelIterator kContains batch prewarm (default 1)\n"
             "  --virtual-merge         force-enable timebucket+rtree file-level path when file-level prune is on\n"
             "  --virtual-merge-auto    auto-enable virtual-merge by query time span threshold (can combine with --virtual-merge)\n"
             "  --vm-time-span-sec-threshold N  auto threshold in seconds (default 21600)\n"
             "  --iterator-repeat N        run N sequential prune scans (new iterator each time;\n"
             "                             stresses Version-level ST meta cache vs cold per-iterator build)\n"
             "  --debug-max-distinct-sst-files N  bench-only: cap SST file opens per level (0=off).\n"
             "                             Stops iteration early — incomplete results; for locality A/B.\n"
             "  --allow-legacy-e6  allow running on legacy E6-key DBs (default: reject E6).\n"
             "  When prune runs: one-line BENCH_CONTRIB ... for grep/log (file/block/key counters).\n"
             "  --query-batch-file <path>  TMan-style batch: one DB::Open, many MBR queries\n"
             "                             (query_conditions.txt: ';'-separated or one/line).\n"
             "                             Use with --no-full-scan; per-query spatial_batch_query lines.\n";
      return 0;
    }
    std::cerr << "Unknown: " << a << "\n";
    return 1;
  }

  if (db_path.empty()) {
    std::cerr << "Required: --db <dir>\n";
    return 1;
  }
  if (!run_full && !run_prune) {
    std::cerr << "Cannot use both --no-full-scan and --no-prune-scan\n";
    return 1;
  }
  if (experimental_global_verify_ghost && !experimental_global_master_ghost) {
    std::cerr << "--experimental-global-verify-ghost requires "
                 "--experimental-global-master-ghost\n";
    return 1;
  }
  if (iterator_repeat != 1 && !run_prune) {
    std::cerr << "--iterator-repeat requires prune scan (omit --no-prune-scan)\n";
    return 1;
  }
  if (prune_t_min > prune_t_max) {
    std::cerr << "prune_t_min must be <= prune_t_max\n";
    return 1;
  }
  if (full_scan_mode != "window" && full_scan_mode != "all_cf") {
    std::cerr << "Invalid --full-scan-mode (expected window|all_cf)\n";
    return 1;
  }

  rocksdb::Options opt =
      BenchOpenOptions(block_cache_mb, large_cache,
                       experimental_global_master_ghost,
                       experimental_global_verify_ghost);
  std::unique_ptr<rocksdb::DB> db;
  rocksdb::Status s = rocksdb::DB::OpenForReadOnly(opt, db_path, &db);
  if (!s.ok()) {
    std::cerr << "DB::Open failed: " << s.ToString() << "\n";
    std::cerr << "Hint: DB must exist (e.g. st_meta_smoke --csv --st-keys). "
                 "Table options must include ST index tail.\n";
    return 1;
  }
  if (!allow_legacy_e6 && HasLegacyE6Keys(db.get())) {
    std::cerr
        << "Detected legacy E6 keys in DB. This benchmark now defaults to E7-only "
           "experiments.\n"
        << "If you intentionally need legacy compatibility, rerun with "
           "--allow-legacy-e6.\n";
    return 2;
  }
  const uint32_t cf_bucket_width_sec = CfEventTimeBucketWidthSec(db.get());

  if (!query_batch_file.empty()) {
    StMetaReadBenchBatchConfig bcfg;
    bcfg.query_batch_file = query_batch_file;
    bcfg.envelope_t_min = prune_t_min;
    bcfg.envelope_t_max = prune_t_max;
    bcfg.prune_mode = prune_mode;
    bcfg.query_segment_uid_dedup = query_segment_uid_dedup;
    bcfg.verify_kv_results = verify_kv_results;
    bcfg.time_bucket_count = time_bucket_count;
    bcfg.rtree_leaf_size = rtree_leaf_size;
    bcfg.sst_manifest_key_level = sst_manifest_key_level;
    bcfg.sst_manifest_adaptive_key_gate = sst_manifest_adaptive_key_gate;
    bcfg.sst_manifest_adaptive_block_gate = sst_manifest_adaptive_block_gate;
    bcfg.sst_manifest_key_level_boundary_only = sst_manifest_key_level_boundary_only;
    bcfg.vm_contains_batch_prewarm = vm_contains_batch_prewarm;
    bcfg.adaptive_overlap_threshold = adaptive_overlap_threshold;
    bcfg.adaptive_block_overlap_threshold = adaptive_block_overlap_threshold;
    bcfg.virtual_merge_enable = virtual_merge_enable;
    bcfg.virtual_merge_auto = virtual_merge_auto;
    bcfg.vm_time_span_sec_threshold = vm_time_span_sec_threshold;
    bcfg.manifest_linear_file_skip_only = manifest_linear_file_skip_only;
    bcfg.file_level_rtree_skip_ratio_gate = file_level_rtree_skip_ratio_gate;
    bcfg.file_level_rtree_min_skip_ratio = file_level_rtree_min_skip_ratio;
    bcfg.debug_max_distinct_sst_files = debug_max_distinct_sst_files;
    run_full = false;
    run_prune = false;
    rocksdb::SetPerfLevel(rocksdb::PerfLevel::kEnableTimeExceptForMutex);
    return RunStMetaReadBenchSpatialBatch(db.get(), bcfg);
  }

  rocksdb::SetPerfLevel(rocksdb::PerfLevel::kEnableTimeExceptForMutex);

  StQuerySegmentUidDedup query_dedup_full;
  StQuerySegmentUidDedup query_dedup_prune;
  query_dedup_full.bucket_width_sec = cf_bucket_width_sec;
  query_dedup_prune.bucket_width_sec = cf_bucket_width_sec;

  int64_t count_full = 0;
  int64_t count_full_scanned_total = 0;
  uint64_t br_full = 0;
  double us_full = 0;
  int64_t full_inwindow_kv = 0;
  int64_t full_inwindow_segment_kv = 0;
  uint64_t full_hash1 = 0;
  uint64_t full_hash2 = 0;
  uint64_t verify_full_decode_failed = 0;
  uint64_t verify_full_legacy_segment_value = 0;
  uint64_t verify_full_unsupported_key = 0;
  uint64_t verify_full_ghost_segment_value = 0;
  uint64_t verify_full_ghost_missing_master_key = 0;
  uint64_t verify_full_ghost_master_lookup_failed = 0;
  uint64_t verify_full_ghost_master_decode_failed = 0;
  uint64_t verify_full_ghost_master_is_ghost = 0;
  uint64_t verify_full_ghost_master_meta_mismatch = 0;

  if (run_full) {
    query_dedup_full.Reset();
    rocksdb::ReadOptions base(rocksdb::Env::IOActivity::kDBIterator);
    base.total_order_seek = true;
    base.fill_cache = false;

    if (auto* pc = rocksdb::get_perf_context(); pc != nullptr) {
      pc->Reset();
    }
    if (auto* io = rocksdb::get_iostats_context(); io != nullptr) {
      io->Reset();
    }
    uint64_t full_data_miss_before =
        db->GetOptions().statistics
            ? db->GetOptions().statistics->getAndResetTickerCount(
                  rocksdb::Tickers::BLOCK_CACHE_DATA_MISS)
            : 0;
    (void)full_data_miss_before;  // used after iterator
    const IoSnap io0 = IoStatsSnap();
    auto t0 = std::chrono::steady_clock::now();
    {
      std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(base));
      for (it->SeekToFirst(); it->Valid(); it->Next()) {
        ++count_full_scanned_total;
        bool inwindow = false;
        rocksdb::Slice uk;
        if (full_scan_mode == "all_cf") {
          ++count_full;
        } else {
          uk = UserKeyFromIteratorKey(it->key());
          inwindow =
              UserKeySpatioTemporalInWindow(uk, prune_t_min, prune_t_max, prune_x_min,
                                             prune_x_max, prune_y_min, prune_y_max);
          if (inwindow) {
            ++count_full;
          }
        }
        if (verify_kv_results) {
          if (uk.size() == 0) {
            uk = UserKeyFromIteratorKey(it->key());
          }
          const rocksdb::Slice v = it->value();
          const PointTruthEval pe = EvalPointTruthInWindow(
              db.get(), base, uk, v, prune_t_min, prune_t_max, prune_x_min, prune_x_max,
              prune_y_min, prune_y_max);
          if (!pe.supported) {
            ++verify_full_unsupported_key;
          }
          if (pe.decode_failed) {
            ++verify_full_decode_failed;
          }
          if (pe.legacy_segment_value) {
            ++verify_full_legacy_segment_value;
          }
          if (pe.ghost_segment_value) {
            ++verify_full_ghost_segment_value;
          }
          if (pe.ghost_missing_master_key) {
            ++verify_full_ghost_missing_master_key;
          }
          if (pe.ghost_master_lookup_failed) {
            ++verify_full_ghost_master_lookup_failed;
          }
          if (pe.ghost_master_decode_failed) {
            ++verify_full_ghost_master_decode_failed;
          }
          if (pe.ghost_master_is_ghost) {
            ++verify_full_ghost_master_is_ghost;
          }
          if (pe.ghost_master_meta_mismatch) {
            ++verify_full_ghost_master_meta_mismatch;
          }
          if (pe.in_window) {
            if (query_segment_uid_dedup &&
                !query_dedup_full.AcceptEmitUserKey(uk)) {
              continue;
            }
            const rocksdb::Slice v = it->value();
            // Use stable Hash64 in two independent seeds to avoid hash collisions.
            constexpr uint64_t kSeed1 = 0x123456789abcdef0ULL;
            constexpr uint64_t kSeed2 = 0x0fedcba987654321ULL;
            uint64_t h1 = Hash64(uk.data(), uk.size(), kSeed1);
            h1 = Hash64(v.data(), v.size(), h1);
            uint64_t h2 = Hash64(uk.data(), uk.size(), kSeed2);
            h2 = Hash64(v.data(), v.size(), h2);
            ++full_inwindow_kv;
            if (uk.size() > 0 &&
                IsSegmentUserKeyMagic(static_cast<unsigned char>(uk.data()[0]))) {
              ++full_inwindow_segment_kv;
            }
            full_hash1 += h1;
            full_hash2 += h2;
          }
        }
      }
      if (!it->status().ok()) {
        std::cerr << "Full iter: " << it->status().ToString() << "\n";
        return 1;
      }
    }
    auto t1 = std::chrono::steady_clock::now();
    br_full = rocksdb::get_perf_context()->block_read_count;
    us_full = std::chrono::duration<double, std::micro>(t1 - t0).count();
    uint64_t full_data_decomp =
        db->GetOptions().statistics
            ? db->GetOptions().statistics->getAndResetTickerCount(
                  rocksdb::Tickers::BLOCK_CACHE_DATA_MISS)
            : 0;
    IoStatsDiff(io0, "full_scan");
    std::cout << "full_scan mode=" << full_scan_mode
              << " keys=" << count_full;
    if (full_scan_mode == "window") {
      std::cout << " keys_scanned_total=" << count_full_scanned_total;
    }
    std::cout << " data_blocks_decompressed=" << full_data_decomp
              << " block_read_count=" << br_full << " wall_us=" << us_full << "\n";
  }

  int64_t count_prune = 0;
  int64_t count_prune_in_window = 0;
  uint64_t br_prune = 0;
  double us_prune = 0;
  int64_t prune_inwindow_kv = 0;
  int64_t prune_inwindow_segment_kv = 0;
  uint64_t prune_hash1 = 0;
  uint64_t prune_hash2 = 0;
  uint64_t verify_prune_decode_failed = 0;
  uint64_t verify_prune_legacy_segment_value = 0;
  uint64_t verify_prune_unsupported_key = 0;
  uint64_t verify_prune_ghost_segment_value = 0;
  uint64_t verify_prune_ghost_missing_master_key = 0;
  uint64_t verify_prune_ghost_master_lookup_failed = 0;
  uint64_t verify_prune_ghost_master_decode_failed = 0;
  uint64_t verify_prune_ghost_master_is_ghost = 0;
  uint64_t verify_prune_ghost_master_meta_mismatch = 0;

  if (run_prune) {
    uint64_t global_masters = 0;
    uint64_t global_ghosts = 0;
    uint64_t global_manifest_set_generation = 0;
    bool has_global_manifest_counts = false;
    {
      const rocksdb::Status gs =
          rocksdb::GetCurrentManifestGlobalCountsFilteredByTimeRange(
              db_path, opt.env->GetFileSystem().get(), /*is_retry=*/false,
              static_cast<uint64_t>(prune_t_min),
              static_cast<uint64_t>(prune_t_max), &global_masters,
              &global_ghosts, &global_manifest_set_generation,
              /*enable_spatial_window=*/true, prune_x_min, prune_x_max,
              prune_y_min, prune_y_max);
      if (gs.ok()) {
        has_global_manifest_counts = true;
        std::cout << "global_manifest_counts generation="
                  << global_manifest_set_generation << " masters="
                  << global_masters << " ghosts=" << global_ghosts << "\n";
        if (global_manifest_counts_fallback_scan && global_masters == 0 &&
            global_ghosts == 0) {
          uint64_t fb_masters = 0;
          uint64_t fb_ghosts = 0;
          const bool fb_ok = ComputeGlobalCountsFromRawKv(
              db_path, block_cache_mb, large_cache, prune_t_min, prune_t_max,
              prune_x_min, prune_x_max, prune_y_min, prune_y_max, &fb_masters,
              &fb_ghosts);
          if (fb_ok) {
            std::cout << "global_manifest_counts_fallback from_raw_kv masters="
                      << fb_masters << " ghosts=" << fb_ghosts
                      << " (point-in-window only; not \"ghost rows scanned\")\n";
          } else {
            std::cout << "global_manifest_counts_fallback status=FAILED\n";
          }
        }
      } else {
        std::cout << "global_manifest_counts status=" << gs.ToString() << "\n";
      }
    }

    rocksdb::ReadOptions prune(rocksdb::Env::IOActivity::kDBIterator);
    prune.total_order_seek = true;
    prune.fill_cache = false;
    prune.experimental_st_prune_scan.enable = true;
    const bool prune_iterator_segment_uid_dedup = query_segment_uid_dedup;
    if (prune_iterator_segment_uid_dedup) {
      prune.experimental_st_prune_scan.query_dedup_by_segment_uid_enable = true;
      prune.experimental_st_prune_scan.query_segment_uid_dedup = &query_dedup_prune;
    }
    if (event_time_bucket_width_sec_override > 0) {
      prune.experimental_st_prune_scan.event_time_bucket_width_sec =
          event_time_bucket_width_sec_override;
    } else {
      prune.experimental_st_prune_scan.event_time_bucket_width_sec =
          cf_bucket_width_sec;
    }
    prune.experimental_st_prune_scan.t_min = prune_t_min;
    prune.experimental_st_prune_scan.t_max = prune_t_max;
    prune.experimental_st_prune_scan.x_min = prune_x_min;
    prune.experimental_st_prune_scan.x_max = prune_x_max;
    prune.experimental_st_prune_scan.y_min = prune_y_min;
    prune.experimental_st_prune_scan.y_max = prune_y_max;
    uint64_t prune_files_skipped = 0;
    uint64_t prune_files_considered = 0;
    uint64_t prune_files_missing_meta = 0;
    uint64_t prune_files_range_del_blocked = 0;
    uint64_t prune_files_time_disjoint = 0;
    uint64_t prune_files_space_disjoint = 0;
    uint64_t prune_file_eval_count = 0;
    uint64_t prune_file_eval_ns = 0;
    uint64_t prune_block_index_examined = 0;
    uint64_t prune_block_index_skipped_disjoint = 0;
    uint64_t prune_block_index_stop_missing_meta = 0;
    uint64_t prune_block_index_prune_ns = 0;
    uint64_t prune_key_examined = 0;
    uint64_t prune_key_skipped_disjoint = 0;
    prune.experimental_st_prune_scan.file_level_files_skipped = &prune_files_skipped;
    prune.experimental_st_prune_scan.file_level_files_considered =
        &prune_files_considered;
    prune.experimental_st_prune_scan.file_level_files_missing_meta =
        &prune_files_missing_meta;
    prune.experimental_st_prune_scan.file_level_files_range_del_blocked =
        &prune_files_range_del_blocked;
    prune.experimental_st_prune_scan.file_level_files_time_disjoint =
        &prune_files_time_disjoint;
    prune.experimental_st_prune_scan.file_level_files_space_disjoint =
        &prune_files_space_disjoint;
    prune.experimental_st_prune_scan.file_level_eval_count =
        &prune_file_eval_count;
    prune.experimental_st_prune_scan.file_level_eval_ns = &prune_file_eval_ns;
    prune.experimental_st_prune_scan.block_level_index_entries_examined =
        &prune_block_index_examined;
    prune.experimental_st_prune_scan.block_level_index_entries_skipped_st_disjoint =
        &prune_block_index_skipped_disjoint;
    prune.experimental_st_prune_scan.block_level_index_stops_missing_meta =
        &prune_block_index_stop_missing_meta;
    prune.experimental_st_prune_scan.block_level_index_prune_ns =
        &prune_block_index_prune_ns;
    prune.experimental_st_prune_scan.debug_max_distinct_sst_files =
        debug_max_distinct_sst_files;
    prune.experimental_st_prune_scan.key_level_keys_examined =
        &prune_key_examined;
    prune.experimental_st_prune_scan.key_level_keys_skipped_disjoint =
        &prune_key_skipped_disjoint;

    // Strict ablation presets.
    // - sst: SST-side only (block-level + block内键过滤), no file-level skip
    // - manifest: Manifest-side only (file-level skip), no block/key pruning.
    //              By default uses the same time-bucket + spatial index file path
    //              as manifest_timebucket_rtree (override: --manifest-linear-file-skip).
    // - manifest_timebucket_rtree: same file-level path as manifest (explicit name).
    // - sst_manifest: both sides on (default; matches previous behavior)
    // - sst_manifest_pipeline: two-stage pipeline experiment:
    //     Global(file-level) + Local(block-level), key-level off to reduce
    //     duplicated fine-grained checks.
    if (prune_mode == "sst") {
      prune.experimental_st_prune_scan.file_level_enable = false;
      prune.experimental_st_prune_scan.block_level_enable = true;
      prune.experimental_st_prune_scan.key_level_enable = true;
    } else if (prune_mode == "manifest") {
      prune.experimental_st_prune_scan.file_level_enable = true;
      prune.experimental_st_prune_scan.block_level_enable = false;
      prune.experimental_st_prune_scan.key_level_enable = false;
      if (!manifest_linear_file_skip_only) {
        prune.experimental_st_prune_scan.file_level_time_bucket_rtree_enable =
            true;
        prune.experimental_st_prune_scan.file_level_time_bucket_count =
            time_bucket_count;
        prune.experimental_st_prune_scan.file_level_rtree_leaf_size =
            rtree_leaf_size;
      }
    } else if (prune_mode == "manifest_timebucket_rtree") {
      prune.experimental_st_prune_scan.file_level_enable = true;
      prune.experimental_st_prune_scan.block_level_enable = false;
      prune.experimental_st_prune_scan.key_level_enable = false;
      prune.experimental_st_prune_scan.file_level_time_bucket_rtree_enable =
          true;
      prune.experimental_st_prune_scan.file_level_time_bucket_count =
          time_bucket_count;
      prune.experimental_st_prune_scan.file_level_rtree_leaf_size =
          rtree_leaf_size;
    } else if (prune_mode == "sst_manifest_pipeline") {
      prune.experimental_st_prune_scan.file_level_enable = true;
      prune.experimental_st_prune_scan.block_level_enable = true;
      prune.experimental_st_prune_scan.key_level_enable = false;
    } else if (prune_mode == "sst_manifest_adaptive") {
      prune.experimental_st_prune_scan.file_level_enable = true;
      prune.experimental_st_prune_scan.block_level_enable = true;
      prune.experimental_st_prune_scan.key_level_enable = true;
      prune.experimental_st_prune_scan.key_level_adaptive_gate_enable = true;
      prune.experimental_st_prune_scan.key_level_disable_overlap_threshold =
          adaptive_overlap_threshold;
    } else if (prune_mode == "sst_manifest" || prune_mode == "both" ||
               prune_mode == "all") {
      prune.experimental_st_prune_scan.file_level_enable = true;
      prune.experimental_st_prune_scan.block_level_enable = true;
      prune.experimental_st_prune_scan.key_level_enable = sst_manifest_key_level;
      prune.experimental_st_prune_scan.block_level_adaptive_gate_enable =
          sst_manifest_adaptive_block_gate;
      prune.experimental_st_prune_scan.block_level_disable_overlap_threshold =
          adaptive_block_overlap_threshold;
      prune.experimental_st_prune_scan.key_level_adaptive_gate_enable =
          sst_manifest_adaptive_key_gate;
      prune.experimental_st_prune_scan.key_level_disable_overlap_threshold =
          adaptive_overlap_threshold;
      prune.experimental_st_prune_scan.key_level_boundary_blocks_only_enable =
          sst_manifest_key_level_boundary_only;
    } else {
      std::cerr << "Invalid --prune-mode: " << prune_mode << "\n";
      std::cerr << "Expected: sst | manifest | manifest_timebucket_rtree | "
                   "sst_manifest | sst_manifest_pipeline | "
                   "sst_manifest_adaptive\n";
      return 1;
    }

    if (prune.experimental_st_prune_scan.file_level_enable) {
      prune.experimental_st_prune_scan.file_level_contains_batch_prewarm_enable =
          vm_contains_batch_prewarm;
      const uint32_t span_sec = prune_t_max - prune_t_min + 1;
      const bool auto_hit =
          virtual_merge_auto && (span_sec >= vm_time_span_sec_threshold);
      // Combine manual and auto signals so they can reinforce each other.
      const bool enable_virtual_merge = virtual_merge_enable || auto_hit;
      if (enable_virtual_merge) {
        prune.experimental_st_prune_scan.file_level_time_bucket_rtree_enable =
            true;
        prune.experimental_st_prune_scan.file_level_time_bucket_count =
            time_bucket_count;
        prune.experimental_st_prune_scan.file_level_rtree_leaf_size =
            rtree_leaf_size;
      }
      const bool rtree_path =
          prune.experimental_st_prune_scan.file_level_time_bucket_rtree_enable;
      if (file_level_rtree_skip_ratio_gate && rtree_path) {
        prune.experimental_st_prune_scan.file_level_rtree_skip_ratio_gate_enable =
            true;
        prune.experimental_st_prune_scan.file_level_rtree_min_skip_ratio =
            file_level_rtree_min_skip_ratio;
      }
      std::cout << "file_level_time_bucket_rtree=" << (rtree_path ? 1 : 0)
                << " virtual_merge_gate=" << (enable_virtual_merge ? 1 : 0)
                << " manual_vm=" << (virtual_merge_enable ? 1 : 0)
                << " auto=" << (virtual_merge_auto ? 1 : 0)
                << " auto_hit=" << (auto_hit ? 1 : 0)
                << " span_sec=" << span_sec
                << " span_threshold_sec=" << vm_time_span_sec_threshold
                << " bucket_count="
                << prune.experimental_st_prune_scan.file_level_time_bucket_count
                << " leaf_size="
                << prune.experimental_st_prune_scan.file_level_rtree_leaf_size
                << " manifest_linear_only="
                << (manifest_linear_file_skip_only ? 1 : 0)
                << " rtree_skip_ratio_gate="
                << (prune.experimental_st_prune_scan
                        .file_level_rtree_skip_ratio_gate_enable
                    ? 1
                    : 0)
                << " rtree_min_skip_ratio="
                << prune.experimental_st_prune_scan.file_level_rtree_min_skip_ratio
                << "\n";
    }

    if (auto* pc = rocksdb::get_perf_context(); pc != nullptr) {
      pc->Reset();
    }
    if (auto* io = rocksdb::get_iostats_context(); io != nullptr) {
      io->Reset();
    }
    uint64_t prune_data_miss_before =
        db->GetOptions().statistics
            ? db->GetOptions().statistics->getAndResetTickerCount(
                  rocksdb::Tickers::BLOCK_CACHE_DATA_MISS)
            : 0;
    (void)prune_data_miss_before;
    const IoSnap io1 = IoStatsSnap();
    auto t2 = std::chrono::steady_clock::now();
    for (int rep = 0; rep < iterator_repeat; ++rep) {
      count_prune = 0;
      count_prune_in_window = 0;
      prune_files_skipped = 0;
      prune_files_considered = 0;
      prune_files_missing_meta = 0;
      prune_files_range_del_blocked = 0;
      prune_files_time_disjoint = 0;
      prune_files_space_disjoint = 0;
      prune_file_eval_count = 0;
      prune_file_eval_ns = 0;
      prune_block_index_examined = 0;
      prune_block_index_skipped_disjoint = 0;
      prune_block_index_stop_missing_meta = 0;
      prune_block_index_prune_ns = 0;
      prune_key_examined = 0;
      prune_key_skipped_disjoint = 0;
      if (verify_kv_results) {
        prune_inwindow_kv = 0;
        prune_inwindow_segment_kv = 0;
        prune_hash1 = 0;
        prune_hash2 = 0;
        verify_prune_decode_failed = 0;
        verify_prune_legacy_segment_value = 0;
        verify_prune_unsupported_key = 0;
        verify_prune_ghost_segment_value = 0;
        verify_prune_ghost_missing_master_key = 0;
        verify_prune_ghost_master_lookup_failed = 0;
        verify_prune_ghost_master_decode_failed = 0;
        verify_prune_ghost_master_is_ghost = 0;
        verify_prune_ghost_master_meta_mismatch = 0;
      }
      std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(prune));
      for (it->SeekToFirst(); it->Valid(); it->Next()) {
        ++count_prune;
        const rocksdb::Slice uk = UserKeyFromIteratorKey(it->key());
        const bool in_window =
            UserKeySpatioTemporalInWindow(uk, prune_t_min, prune_t_max,
                                          prune_x_min, prune_x_max, prune_y_min,
                                          prune_y_max);
        if (in_window) {
          ++count_prune_in_window;
        }
        if (verify_kv_results) {
          const rocksdb::Slice v = it->value();
          const PointTruthEval pe = EvalPointTruthInWindow(
              db.get(), prune, uk, v, prune_t_min, prune_t_max, prune_x_min, prune_x_max,
              prune_y_min, prune_y_max);
          if (!pe.supported) {
            ++verify_prune_unsupported_key;
          }
          if (pe.decode_failed) {
            ++verify_prune_decode_failed;
          }
          if (pe.legacy_segment_value) {
            ++verify_prune_legacy_segment_value;
          }
          if (pe.ghost_segment_value) {
            ++verify_prune_ghost_segment_value;
          }
          if (pe.ghost_missing_master_key) {
            ++verify_prune_ghost_missing_master_key;
          }
          if (pe.ghost_master_lookup_failed) {
            ++verify_prune_ghost_master_lookup_failed;
          }
          if (pe.ghost_master_decode_failed) {
            ++verify_prune_ghost_master_decode_failed;
          }
          if (pe.ghost_master_is_ghost) {
            ++verify_prune_ghost_master_is_ghost;
          }
          if (pe.ghost_master_meta_mismatch) {
            ++verify_prune_ghost_master_meta_mismatch;
          }
          if (!pe.in_window) {
            continue;
          }
          // Iterator already dedupes by segment_uid when enabled; do not call
          // AcceptEmitUserKey again or every emitted key is treated as duplicate.
          if (query_segment_uid_dedup && !prune_iterator_segment_uid_dedup &&
              !query_dedup_prune.AcceptEmitUserKey(uk)) {
            continue;
          }
          constexpr uint64_t kSeed1 = 0x123456789abcdef0ULL;
          constexpr uint64_t kSeed2 = 0x0fedcba987654321ULL;
          uint64_t h1 = Hash64(uk.data(), uk.size(), kSeed1);
          h1 = Hash64(v.data(), v.size(), h1);
          uint64_t h2 = Hash64(uk.data(), uk.size(), kSeed2);
          h2 = Hash64(v.data(), v.size(), h2);
          ++prune_inwindow_kv;
          if (uk.size() > 0 &&
              IsSegmentUserKeyMagic(static_cast<unsigned char>(uk.data()[0]))) {
            ++prune_inwindow_segment_kv;
          }
          prune_hash1 += h1;
          prune_hash2 += h2;
        }
      }
      if (!it->status().ok()) {
        std::cerr << "Prune iter: " << it->status().ToString() << "\n";
        return 1;
      }
    }
    auto t3 = std::chrono::steady_clock::now();
    br_prune = rocksdb::get_perf_context()->block_read_count;
    us_prune =
        std::chrono::duration<double, std::micro>(t3 - t2).count();
    uint64_t prune_data_decomp =
        db->GetOptions().statistics
            ? db->GetOptions().statistics->getAndResetTickerCount(
                  rocksdb::Tickers::BLOCK_CACHE_DATA_MISS)
            : 0;
    IoStatsDiff(io1, "prune_scan");

    std::cout << "prune_scan iterator_repeat=" << iterator_repeat
              << " keys(last_iter)=" << count_prune
              << " data_blocks_decompressed=" << prune_data_decomp
              << " block_read_count=" << br_prune << " wall_us_total=" << us_prune
              << " wall_us_per_iter_avg="
              << (us_prune / static_cast<double>(iterator_repeat))
              << " keys_in_window(last_iter)=" << count_prune_in_window << "\n";
    std::cout << "prune_file_skipped=" << prune_files_skipped << "\n";
    std::cout << "prune_file_diag considered=" << prune_files_considered
              << " missing_meta=" << prune_files_missing_meta
              << " range_del_blocked=" << prune_files_range_del_blocked
              << " time_disjoint=" << prune_files_time_disjoint
              << " space_disjoint=" << prune_files_space_disjoint << "\n";
    std::cout << "prune_file_eval count=" << prune_file_eval_count
              << " total_ns=" << prune_file_eval_ns << "\n";
    std::cout << "prune_block_diag index_examined=" << prune_block_index_examined
              << " index_skipped_st_disjoint=" << prune_block_index_skipped_disjoint
              << " index_stop_missing_meta=" << prune_block_index_stop_missing_meta
              << "\n";
    if (prune_block_index_examined > 0) {
      const double skip_rate_pct =
          100.0 * static_cast<double>(prune_block_index_skipped_disjoint) /
          static_cast<double>(prune_block_index_examined);
      const double prune_ns_per_index_entry =
          static_cast<double>(prune_block_index_prune_ns) /
          static_cast<double>(prune_block_index_examined);
      std::cout << "prune_block_metrics Block_Skip_Rate_pct=" << std::fixed
                << std::setprecision(4) << skip_rate_pct
                << " (index_skipped_st_disjoint/index_examined*100; block index "
                   "entries, not data blocks)\n";
      std::cout << "prune_block_metrics Pruning_Latency_Per_BlockIndexEntry_ns="
                << std::setprecision(4) << prune_ns_per_index_entry
                << " (block_level_index_prune_ns/index_examined)\n";
      std::cout << std::defaultfloat;
    } else {
      std::cout << "prune_block_metrics Block_Skip_Rate_pct=n/a "
                   "Pruning_Latency_Per_BlockIndexEntry_ns=n/a (no index "
                   "examinations)\n";
    }
    std::cout << "prune_key_diag keys_examined=" << prune_key_examined
              << " keys_skipped_disjoint=" << prune_key_skipped_disjoint << "\n";
    std::cout << "BENCH_CONTRIB"
              << " file_skipped=" << prune_files_skipped
              << " file_considered=" << prune_files_considered
              << " block_index_examined=" << prune_block_index_examined
              << " block_index_skipped_st_disjoint="
              << prune_block_index_skipped_disjoint
              << " block_index_stop_missing_meta="
              << prune_block_index_stop_missing_meta
              << " block_index_prune_ns=" << prune_block_index_prune_ns
              << " debug_max_distinct_sst_files=" << debug_max_distinct_sst_files
              << " key_examined=" << prune_key_examined
              << " key_skipped_disjoint=" << prune_key_skipped_disjoint
              << " global_manifest_counts_available="
              << (has_global_manifest_counts ? 1 : 0)
              << " global_manifest_generation=" << global_manifest_set_generation
              << " global_masters=" << global_masters
              << " global_ghosts=" << global_ghosts
              << " verify_prune_ghost_segment_value="
              << verify_prune_ghost_segment_value
              << " verify_prune_ghost_missing_master_key="
              << verify_prune_ghost_missing_master_key
              << " verify_prune_ghost_master_lookup_failed="
              << verify_prune_ghost_master_lookup_failed
              << " verify_prune_ghost_master_decode_failed="
              << verify_prune_ghost_master_decode_failed
              << " verify_prune_ghost_master_is_ghost="
              << verify_prune_ghost_master_is_ghost
              << " verify_prune_ghost_master_meta_mismatch="
              << verify_prune_ghost_master_meta_mismatch << "\n";
    std::cout << "prune window t=[" << prune_t_min << "," << prune_t_max << "] x=["
              << prune_x_min << "," << prune_x_max << "] y=[" << prune_y_min << ","
              << prune_y_max << "]\n";
  }

  if (query_segment_uid_dedup && run_prune) {
    std::cout << "query_result_dedup_count full_skipped="
              << query_dedup_full.dedup_skipped
              << " prune_skipped=" << query_dedup_prune.dedup_skipped
              << " full_unique_kv=" << full_inwindow_kv
              << " prune_unique_kv=" << prune_inwindow_kv << "\n";
  }

  if (verify_kv_results && run_prune && !run_full) {
    bool ref_ok = true;
    double ref_segment_ratio = 0.0;
    if (ref_meta_st_segments > 0) {
      ref_segment_ratio =
          static_cast<double>(prune_inwindow_segment_kv) /
          static_cast<double>(ref_meta_st_segments);
      const double min_allowed =
          static_cast<double>(ref_meta_st_segments) *
          static_cast<double>(ref_segment_kv_min_ratio);
      const double max_allowed =
          static_cast<double>(ref_meta_st_segments) *
          static_cast<double>(ref_segment_kv_max_ratio);
      if (static_cast<double>(prune_inwindow_segment_kv) < min_allowed ||
          static_cast<double>(prune_inwindow_segment_kv) > max_allowed) {
        ref_ok = false;
      }
    }
    const bool trivial_zero =
        (prune_inwindow_kv == 0 && ref_meta_st_segments > 0);
    if (trivial_zero) {
      ref_ok = false;
    }
    const bool strict_ok =
        ref_ok && verify_prune_decode_failed == 0 &&
        verify_prune_legacy_segment_value == 0 &&
        verify_prune_ghost_missing_master_key == 0 &&
        verify_prune_ghost_master_lookup_failed == 0 &&
        verify_prune_ghost_master_decode_failed == 0 &&
        verify_prune_ghost_master_is_ghost == 0 &&
        verify_prune_ghost_master_meta_mismatch == 0;
    std::cout << "verify_kv_results mode=prune_only correctness="
              << (strict_ok ? "OK" : "FAIL")
              << " ref_ok=" << (ref_ok ? "OK" : "FAIL")
              << " prune_inwindow_kv=" << prune_inwindow_kv
              << " prune_inwindow_segment_kv=" << prune_inwindow_segment_kv
              << " ref_meta_st_segments=" << ref_meta_st_segments
              << " ref_spatial_segments=" << ref_spatial_segments
              << " trivial_zero=" << (trivial_zero ? 1 : 0)
              << " ref_segment_ratio=" << std::fixed << std::setprecision(4)
              << ref_segment_ratio
              << " prune_decode_failed=" << verify_prune_decode_failed
              << " prune_legacy_segment_value=" << verify_prune_legacy_segment_value
              << " prune_unsupported_key=" << verify_prune_unsupported_key
              << "\n";
    if (verify_kv_results_strict_exit && !strict_ok) {
      return 2;
    }
  }

  if (verify_kv_results && run_full && !run_prune) {
    bool ref_ok = true;
    double ref_segment_ratio = 0.0;
    if (ref_meta_st_segments > 0) {
      ref_segment_ratio =
          static_cast<double>(full_inwindow_segment_kv) /
          static_cast<double>(ref_meta_st_segments);
      const double min_allowed =
          static_cast<double>(ref_meta_st_segments) *
          static_cast<double>(ref_segment_kv_min_ratio);
      const double max_allowed =
          static_cast<double>(ref_meta_st_segments) *
          static_cast<double>(ref_segment_kv_max_ratio);
      if (static_cast<double>(full_inwindow_segment_kv) < min_allowed ||
          static_cast<double>(full_inwindow_segment_kv) > max_allowed) {
        ref_ok = false;
      }
    }
  std::cout << "verify_kv_results mode=full_only correctness="
            << (ref_ok ? "OK" : "FAIL")
            << " ref_ok=" << (ref_ok ? "OK" : "FAIL")
            << " full_inwindow_kv=" << full_inwindow_kv
            << " full_inwindow_segment_kv=" << full_inwindow_segment_kv
            << " ref_meta_st_segments=" << ref_meta_st_segments
            << " ref_spatial_segments=" << ref_spatial_segments
            << " ref_segment_ratio=" << std::fixed << std::setprecision(4)
            << ref_segment_ratio
            << " full_decode_failed=" << verify_full_decode_failed
            << " full_legacy_segment_value=" << verify_full_legacy_segment_value
            << "\n";
  }

  if (verify_kv_results && run_full && run_prune) {
    const bool ok = (full_inwindow_kv == prune_inwindow_kv) &&
                     (full_inwindow_segment_kv == prune_inwindow_segment_kv) &&
                     (full_hash1 == prune_hash1) &&
                     (full_hash2 == prune_hash2);
    bool ref_ok = true;
    const bool trivial_zero =
        (full_inwindow_kv == 0 && prune_inwindow_kv == 0);
    if (trivial_zero &&
        (ref_meta_st_segments > 0 || ref_spatial_segments > 0)) {
      ref_ok = false;
    }
    double ref_segment_ratio = 0.0;
    if (ref_meta_st_segments > 0) {
      ref_segment_ratio =
          static_cast<double>(full_inwindow_segment_kv) /
          static_cast<double>(ref_meta_st_segments);
      const double min_allowed =
          static_cast<double>(ref_meta_st_segments) *
          static_cast<double>(ref_segment_kv_min_ratio);
      const double max_allowed =
          static_cast<double>(ref_meta_st_segments) *
          static_cast<double>(ref_segment_kv_max_ratio);
      if (static_cast<double>(full_inwindow_segment_kv) < min_allowed ||
          static_cast<double>(full_inwindow_segment_kv) > max_allowed) {
        ref_ok = false;
      }
    }
    const bool strict_ok = ok && ref_ok && verify_full_decode_failed == 0 &&
                           verify_prune_decode_failed == 0 &&
                           verify_full_legacy_segment_value == 0 &&
                           verify_prune_legacy_segment_value == 0 &&
                           verify_full_ghost_missing_master_key == 0 &&
                           verify_prune_ghost_missing_master_key == 0 &&
                           verify_full_ghost_master_lookup_failed == 0 &&
                           verify_prune_ghost_master_lookup_failed == 0 &&
                           verify_full_ghost_master_decode_failed == 0 &&
                           verify_prune_ghost_master_decode_failed == 0 &&
                           verify_full_ghost_master_is_ghost == 0 &&
                           verify_prune_ghost_master_is_ghost == 0 &&
                           verify_full_ghost_master_meta_mismatch == 0 &&
                           verify_prune_ghost_master_meta_mismatch == 0;
    std::cout << "verify_kv_results correctness="
              << (strict_ok ? "OK" : "FAIL")
              << " ref_ok=" << (ref_ok ? "OK" : "FAIL")
              << " full_inwindow_kv=" << full_inwindow_kv
              << " prune_inwindow_kv=" << prune_inwindow_kv
              << " full_inwindow_segment_kv=" << full_inwindow_segment_kv
              << " prune_inwindow_segment_kv=" << prune_inwindow_segment_kv
              << " ref_meta_st_segments=" << ref_meta_st_segments
              << " ref_spatial_segments=" << ref_spatial_segments
              << " trivial_zero=" << (trivial_zero ? 1 : 0)
              << " ref_segment_ratio=" << std::fixed << std::setprecision(4)
              << ref_segment_ratio
              << " full_decode_failed=" << verify_full_decode_failed
              << " prune_decode_failed=" << verify_prune_decode_failed
              << " full_legacy_segment_value=" << verify_full_legacy_segment_value
              << " prune_legacy_segment_value=" << verify_prune_legacy_segment_value
              << " full_ghost_segment_value=" << verify_full_ghost_segment_value
              << " prune_ghost_segment_value=" << verify_prune_ghost_segment_value
              << " full_ghost_missing_master_key="
              << verify_full_ghost_missing_master_key
              << " prune_ghost_missing_master_key="
              << verify_prune_ghost_missing_master_key
              << " full_ghost_master_lookup_failed="
              << verify_full_ghost_master_lookup_failed
              << " prune_ghost_master_lookup_failed="
              << verify_prune_ghost_master_lookup_failed
              << " full_ghost_master_decode_failed="
              << verify_full_ghost_master_decode_failed
              << " prune_ghost_master_decode_failed="
              << verify_prune_ghost_master_decode_failed
              << " full_ghost_master_is_ghost="
              << verify_full_ghost_master_is_ghost
              << " prune_ghost_master_is_ghost="
              << verify_prune_ghost_master_is_ghost
              << " full_ghost_master_meta_mismatch="
              << verify_full_ghost_master_meta_mismatch
              << " prune_ghost_master_meta_mismatch="
              << verify_prune_ghost_master_meta_mismatch
              << " full_unsupported_key=" << verify_full_unsupported_key
              << " prune_unsupported_key=" << verify_prune_unsupported_key
              << "\n";
    if (!ref_ok) {
      if (trivial_zero &&
          (ref_meta_st_segments > 0 || ref_spatial_segments > 0)) {
        std::cout << "verify_kv_results detail: trivial zero in-window KV while "
                     "ref_meta_st_segments or ref_spatial_segments > 0 "
                     "(full and prune both empty; not validated against source).\n";
      } else if (ref_meta_st_segments > 0) {
        std::cout << "verify_kv_results detail: in-window segment KV count "
                  << full_inwindow_segment_kv
                  << " outside [" << ref_segment_kv_min_ratio << ","
                  << ref_segment_kv_max_ratio << "] * ref_meta_st_segments="
                  << ref_meta_st_segments << ".\n";
      }
    }
    if (!strict_ok && ok && ref_ok &&
        (verify_full_decode_failed > 0 || verify_prune_decode_failed > 0 ||
         verify_full_legacy_segment_value > 0 ||
         verify_prune_legacy_segment_value > 0 ||
         verify_full_ghost_missing_master_key > 0 ||
         verify_prune_ghost_missing_master_key > 0 ||
         verify_full_ghost_master_lookup_failed > 0 ||
         verify_prune_ghost_master_lookup_failed > 0 ||
         verify_full_ghost_master_decode_failed > 0 ||
         verify_prune_ghost_master_decode_failed > 0 ||
         verify_full_ghost_master_is_ghost > 0 ||
         verify_prune_ghost_master_is_ghost > 0 ||
         verify_full_ghost_master_meta_mismatch > 0 ||
         verify_prune_ghost_master_meta_mismatch > 0)) {
      std::cout << "verify_kv_results detail: point-truth verification found "
                   "segment values that are undecodable, legacy(no point list), "
                   "or ghost follow-up failures/mismatches; rebuild segment DB "
                   "with Global master/ghost-compatible segment values for strict "
                   "correctness.\n";
    }
    if (verify_kv_results_strict_exit && !strict_ok) {
      return 2;
    }
  }

  if (run_full && run_prune && count_full > 0) {
    if (full_scan_mode == "window") {
      std::cout << "key_selectivity(in_window prune/full)="
                << std::fixed << std::setprecision(6)
                << (static_cast<double>(count_prune_in_window) /
                    static_cast<double>(count_full)) << "\n";
      if (count_prune != count_prune_in_window) {
        std::cout << "iterator_yield_ratio(valid_steps/baseline_in_window_keys)="
                  << std::setprecision(6)
                  << (static_cast<double>(count_prune) /
                      static_cast<double>(count_full))
                  << "  (extra_iter_keys=" << (count_prune - count_prune_in_window)
                  << ": non-ST or outside exact window but not key-pruned)\n";
      }
    } else {
      std::cout << "key_selectivity(prune/full)=" << std::fixed
                << std::setprecision(6)
                << (static_cast<double>(count_prune) /
                    static_cast<double>(count_full)) << "\n";
    }
  }
  if (run_full && run_prune && full_scan_mode == "window" &&
      count_full_scanned_total > 0) {
    std::cout << "query_vs_cf_selectivity(prune_in_window/total_cf)="
              << std::setprecision(6)
              << (static_cast<double>(count_prune_in_window) /
                  static_cast<double>(count_full_scanned_total)) << "\n";
  }
  if (run_full && run_prune && full_scan_mode == "window" && !verify_kv_results) {
    if (count_prune_in_window != count_full) {
      std::cout << "WARNING: prune keys_in_window=" << count_prune_in_window
                << " != baseline window keys=" << count_full << "\n";
    }
  }
  if (run_full && run_prune && br_full > 0) {
    std::cout << "block_read_ratio(prune/full)=" << std::setprecision(4)
              << (static_cast<double>(br_prune) / static_cast<double>(br_full)) << "\n";
  }
  if (run_full && run_prune && us_full > 0) {
    std::cout << "wall_time_ratio(prune/full)=" << std::setprecision(4)
              << (us_prune / us_full) << "\n";
  }

  if (opt.statistics) {
    using T = rocksdb::Tickers;
    rocksdb::Statistics& st = *opt.statistics;
    std::cout << "--- statistics (selected tickers) ---\n";
    std::cout << "BLOCK_CACHE_DATA_MISS " << st.getTickerCount(T::BLOCK_CACHE_DATA_MISS)
              << "\n";
    std::cout << "BLOCK_CACHE_DATA_HIT " << st.getTickerCount(T::BLOCK_CACHE_DATA_HIT)
              << "\n";
    std::cout << "BLOCK_CACHE_INDEX_MISS " << st.getTickerCount(T::BLOCK_CACHE_INDEX_MISS)
              << "\n";
    std::cout << "BLOCK_CACHE_INDEX_HIT " << st.getTickerCount(T::BLOCK_CACHE_INDEX_HIT)
              << "\n";
  }

  std::cout
      << "Note: First run after cold OS cache sees more read_nanos/bytes_read; "
         "repeat for trend. Ratios are the main A/B signal.\n"
         "Baseline default (--full-scan-mode window): same IO as full CF scan; "
         "keys= counts query answers (ST keys in window). Legacy: --full-scan-mode "
         "all_cf.\n";
  return 0;
}

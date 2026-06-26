// ST-Compaction Read/Write Throughput Benchmark (v2 — proper E7 data + pruned queries)
// Compares 7 compaction strategies: Leveled, Tiered(Real), Hybrid-L1, Hybrid-L1+L2,
// STFP-R4, STFP-R6, STFP-Rinf.
//
// Write phase:  batch-write all keys, Flush, CompactRange → measure write amp + time.
// Read phase:   pruned spatio-temporal query windows (file+block+key level pruning)
//               → measure real query throughput (result MB/s) and pruning effectiveness.
//
// Usage:
//   st_compaction_throughput_bench --num-keys 100000 --db-root D:/tmp/ctb
//   st_compaction_throughput_bench --num-keys 300000 --db-root D:/tmp/ctb --windows-csv tools/sample_data/query_windows.csv

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/iostats_context.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>
#include <rocksdb/perf_context.h>
#include <rocksdb/perf_level.h>
#include <rocksdb/statistics.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>

#include "segment_value_codec.hpp"
#include "st_segment_key.hpp"

namespace rst = ROCKSDB_NAMESPACE;

// ============================================================================
// Query window
// ============================================================================
struct QueryWindow {
  std::string label;
  uint32_t t_min = 0, t_max = 0;
  float x_min = 0, x_max = 0, y_min = 0, y_max = 0;
};

// ============================================================================
// Compaction strategy config
// ============================================================================
struct Config {
  const char* label;
  bool   tiered_on;       // experimental_st_compaction
  bool   real_tiered;     // experimental_st_compaction_real_tiered_l1_l2
  int    tiered_max_level; // -1 = all levels
  int    max_runs;         // tiered_max_runs_per_level (0=unlimited)
  bool   stfp_enable;      // experimental_st_compaction_stfp_enable
};

// ============================================================================
// Benchmark result
// ============================================================================
struct Result {
  std::string label;
  int64_t kv_bytes = 0;
  // Write
  double write_sec = 0;
  int64_t compact_read_mb = 0;
  int64_t compact_write_mb = 0;
  int l0 = 0, l1 = 0, l2 = 0, l3 = 0;
  // Read (pruned queries)
  int    num_queries = 0;
  double query_total_sec = 0;
  int64_t query_result_bytes = 0;
  uint64_t block_read_count = 0;
  int64_t prune_files_skipped = 0;
  int64_t prune_files_considered = 0;
  int64_t prune_block_index_examined = 0;
  int64_t prune_block_index_skipped = 0;
  int64_t prune_key_examined = 0;
  int64_t prune_key_skipped = 0;
  // Mixed workload (one entry per ratio point: 0%, 10%, ..., 100% read)
  std::vector<double> mixed_read_mbps;
  std::vector<double> mixed_write_mbps;
};

// ============================================================================
// Z-order helper (mirrors st_bucket_ingest_build.cpp MakeSimpleZOrderStart)
// ============================================================================
static uint64_t MakeZOrder(float lon, float lat) {
  uint32_t lon_bits = 0, lat_bits = 0;
  std::memcpy(&lon_bits, &lon, sizeof(lon_bits));
  std::memcpy(&lat_bits, &lat, sizeof(lat_bits));
  return (static_cast<uint64_t>(lon_bits) << 32) | static_cast<uint64_t>(lat_bits);
}

// ============================================================================
// Data generation: proper E7 keys + Segment V2 values
// Also computes ground-truth counts per query window for correctness verification.
// ============================================================================
struct GeneratedKV {
  std::string key;
  std::string value;
  uint32_t t_start = 0, t_end = 0;
  float x_min = 0, x_max = 0, y_min = 0, y_max = 0;
};

// Check if a segment's actual points fall within the query window (point-level truth)
static bool SegmentHasPointInWindow(
    const std::vector<segval::SegmentPoint>& points,
    const QueryWindow& w) {
  for (const auto& pt : points) {
    if (pt.unix_s >= w.t_min && pt.unix_s <= w.t_max &&
        pt.lon >= w.x_min && pt.lon <= w.x_max &&
        pt.lat >= w.y_min && pt.lat <= w.y_max) {
      return true;
    }
  }
  return false;
}

// Two spatio-temporal regions (aligned with query windows)
struct Region {
  float lon_min, lon_max, lat_min, lat_max;
  uint32_t t_min, t_max;  // Unix seconds
};

static const Region kRegionBeijing = {115.5f, 117.6f, 32.7f, 41.1f,
                                      1201930000u, 1202043000u};
static const Region kRegionChengdu = {103.5f, 104.7f, 29.8f, 31.1f,
                                      1406850000u, 1406895000u};

static void GenerateData(std::mt19937_64& rng, int total,
                         std::vector<GeneratedKV>& out,
                         int64_t& total_bytes,
                         const std::vector<QueryWindow>& windows,
                         std::vector<int64_t>& ground_truth_counts,
                         uint32_t bucket_sec = 3600) {
  out.clear();
  out.reserve(total);
  total_bytes = 0;

  // Assign half to Beijing, half to Chengdu
  for (int i = 0; i < total; ++i) {
    const Region& reg = (i < total / 2) ? kRegionBeijing : kRegionChengdu;

    // ── t_start, t_end ──
    uint32_t t_start = reg.t_min +
        std::uniform_int_distribution<uint32_t>(0, reg.t_max - reg.t_min)(rng);
    uint32_t duration = std::uniform_int_distribution<uint32_t>(60, 3600)(rng);
    uint32_t t_end = t_start + duration;

    // ── MBR center ──
    float cx = std::uniform_real_distribution<float>(reg.lon_min, reg.lon_max)(rng);
    float cy = std::uniform_real_distribution<float>(reg.lat_min, reg.lat_max)(rng);

    // ── Points ──
    uint16_t n_pts = static_cast<uint16_t>(
        std::uniform_int_distribution<int>(50, 200)(rng));
    std::vector<segval::SegmentPoint> points;
    points.reserve(n_pts);

    segval::SegmentValueHeader hdr{};
    hdr.t_start = t_start;
    hdr.t_end = t_end;
    hdr.point_count = n_pts;
    hdr.x_min = cx; hdr.x_max = cx;
    hdr.y_min = cy; hdr.y_max = cy;

    for (uint16_t j = 0; j < n_pts; ++j) {
      segval::SegmentPoint pt{};
      pt.unix_s = t_start + static_cast<uint32_t>(
          (static_cast<double>(j) / n_pts) * duration);
      pt.lon = cx + std::uniform_real_distribution<float>(-0.05f, 0.05f)(rng);
      pt.lat = cy + std::uniform_real_distribution<float>(-0.05f, 0.05f)(rng);
      hdr.x_min = std::min(hdr.x_min, pt.lon);
      hdr.x_max = std::max(hdr.x_max, pt.lon);
      hdr.y_min = std::min(hdr.y_min, pt.lat);
      hdr.y_max = std::max(hdr.y_max, pt.lat);
      points.push_back(pt);
    }

    // ── Value ──
    std::string val = segval::EncodeSegmentValueV2(hdr, points);
    if (val.empty()) {
      // fallback: simplest valid value
      std::cerr << "EncodeSegmentValueV2 failed, skip key " << i << "\n";
      continue;
    }

    // ── Key (E7) ──
    uint32_t bucket_id = t_start / bucket_sec;
    uint32_t t_start_offset = t_start - bucket_id * bucket_sec;
    uint64_t zorder = MakeZOrder(cx, cy);
    uint64_t vehicle_id = 20000000000000ULL + static_cast<uint64_t>(i);
    uint32_t segment_seq = static_cast<uint32_t>(i % 100000);
    uint16_t chunk_seq = 1;

    std::string key = StSegmentKeyTzidRaw(bucket_id, zorder, t_start_offset,
                                          vehicle_id, segment_seq, chunk_seq, 0);

    out.push_back({std::move(key), std::move(val),
                   t_start, t_end, hdr.x_min, hdr.x_max, hdr.y_min, hdr.y_max});
    total_bytes += static_cast<int64_t>(out.back().key.size() +
                                         out.back().value.size());

    // Ground truth: point-level check against each query window
    for (size_t wi = 0; wi < windows.size(); ++wi) {
      if (SegmentHasPointInWindow(points, windows[wi])) {
        ground_truth_counts[wi]++;
      }
    }
  }
}

// ============================================================================
// Built-in query windows (aligned with tools/sample_data/query_windows.csv)
// ============================================================================
static std::vector<QueryWindow> BuiltinQueryWindows() {
  return {
    {"beijing_narrow",  1201930244u, 1201933844u,
     116.308658f, 116.808658f, 39.544024f, 40.044024f},
    {"beijing_medium",  1201930244u, 1201943564u,
     115.508658f, 116.708658f, 39.444024f, 40.644024f},
    {"beijing_wide",    1201999002u, 1202042202u,
     115.610695f, 117.560222f, 32.764078f, 41.050735f},
    {"chengdu_narrow",  1406851200u, 1406854800u,
     104.111000f, 104.119000f, 30.686000f, 30.694000f},
    {"chengdu_medium",  1406851200u, 1406854800u,
     104.073090f, 104.098090f, 30.677160f, 30.702160f},
    {"chengdu_wide",    1406851200u, 1406854800u,
     104.060590f, 104.110590f, 30.664660f, 30.714660f},
  };
}

static std::vector<QueryWindow> LoadWindowsCSV(const std::string& path) {
  std::vector<QueryWindow> out;
  std::ifstream f(path);
  if (!f.is_open()) {
    std::cerr << "Cannot open windows CSV: " << path << "\n";
    return out;
  }
  std::string line;
  std::getline(f, line);  // skip header
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    std::vector<std::string> cols;
    std::istringstream ss(line);
    std::string col;
    while (std::getline(ss, col, ',')) cols.push_back(col);
    if (cols.size() < 7) continue;
    QueryWindow w;
    w.label = cols[0];
    w.t_min = static_cast<uint32_t>(std::strtoul(cols[1].c_str(), nullptr, 10));
    w.t_max = static_cast<uint32_t>(std::strtoul(cols[2].c_str(), nullptr, 10));
    w.x_min = std::strtof(cols[3].c_str(), nullptr);
    w.x_max = std::strtof(cols[4].c_str(), nullptr);
    w.y_min = std::strtof(cols[5].c_str(), nullptr);
    w.y_max = std::strtof(cols[6].c_str(), nullptr);
    if (w.t_min > 0 || w.t_max > 0) out.push_back(w);
  }
  return out;
}

// ============================================================================
// Build an E7 seek key for the given bucket_id (all other fields zero)
// ============================================================================
static std::string BuildE7SeekKey(uint32_t bucket_id) {
  std::string key(32, '\0');
  key[0] = static_cast<char>(0xE7);
  key[1] = 0;
  key[2] = static_cast<char>((bucket_id >> 24) & 0xFF);
  key[3] = static_cast<char>((bucket_id >> 16) & 0xFF);
  key[4] = static_cast<char>((bucket_id >> 8) & 0xFF);
  key[5] = static_cast<char>(bucket_id & 0xFF);
  return key;
}

// ============================================================================
// DB open + ST options
// ============================================================================
static void ApplyOptions(rst::Options& o, const Config& cfg,
                         std::shared_ptr<rst::Statistics>& stats_out) {
  o.create_if_missing = true;
  o.error_if_exists = false;
  o.paranoid_checks = false;
  o.max_background_jobs = 4;
  o.experimental_st_compaction = true;
  o.experimental_st_compaction_l1_l2_tiered = cfg.tiered_on;
  o.experimental_st_compaction_real_tiered_l1_l2 = cfg.real_tiered;
  // Set new options to defaults at Open (workaround OPTIONS file bug)
  o.experimental_st_compaction_tiered_max_level = 0;
  o.experimental_st_compaction_tiered_min_level = 0;
  o.experimental_st_compaction_real_tiered_l1_l2 = false;
  o.experimental_st_compaction_tiered_max_runs_per_level = cfg.max_runs;
  o.experimental_st_compaction_level_trigger_ratio = 0.9;
  o.experimental_global_verify_ghost = false;
  o.experimental_global_master_ghost = false;
  o.experimental_compaction_event_time_max_span = 86400;  // 24h, basically no time-cut
  o.experimental_compaction_event_time_split = true;       // enable SST key-count cut
  o.experimental_compaction_event_time_bucket_width = 3600;  // 1h bucket for split tracking
  o.experimental_compaction_event_time_max_point_keys = 0;  // no global cap, use per-level policy
  o.level_compaction_dynamic_level_bytes = false;  // required by event_time_split
  o.level0_file_num_compaction_trigger = 1;  // trigger L0→L1 immediately
  o.target_file_size_base = 64ULL << 20;     // 64 MB SST files
  o.max_bytes_for_level_base = 64ULL << 20;   // L1 = 64 MB
  o.max_bytes_for_level_multiplier = 4;       // L2=256MB, L3=1GB, L4=4GB
  o.num_levels = 7;
  o.compression = rst::kNoCompression;
  o.write_buffer_size = 64ULL << 20;   // 64MB memtable
  o.statistics = rst::CreateDBStatistics();
  stats_out = o.statistics;

  rst::BlockBasedTableOptions bt;
  bt.block_size = 16384;
  bt.format_version = 5;
  bt.filter_policy.reset(rst::NewBloomFilterPolicy(10, false));
  // Enable ST meta in index block values — REQUIRED for block-level pruning
  bt.experimental_spatio_temporal_meta_in_index_value = true;
  // Small block cache to make pruning effect measurable (cache misses → real IO)
  bt.block_cache = rst::NewLRUCache(64ULL << 20);  // 64 MB block cache
  bt.cache_index_and_filter_blocks = true;
  o.table_factory.reset(rst::NewBlockBasedTableFactory(bt));
}

// ============================================================================
// Run one configuration
// ============================================================================
static Result Run(const std::string& root, const Config& cfg,
                  std::mt19937_64& rng,
                  const std::vector<GeneratedKV>& data,
                  int64_t total_kv_bytes,
                  const std::vector<QueryWindow>& windows,
                  const std::vector<int64_t>& ground_truth_counts,
                  std::vector<int64_t>& query_result_counts,
                  uint32_t bucket_sec,
                  bool do_mixed_workload = false,
                  int mixed_ops_per_point = 500) {
  Result r;
  r.label = cfg.label;
  r.kv_bytes = total_kv_bytes;

  // Unique DB path
  auto ts = std::chrono::system_clock::now().time_since_epoch().count();
  std::string path = root + "/db_" + std::to_string(ts % 1000000);
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);

  // ── Open DB ──
  rst::Options o;
  std::shared_ptr<rst::Statistics> stats;
  ApplyOptions(o, cfg, stats);
  std::unique_ptr<rst::DB> db;
  rst::Status st = rst::DB::Open(o, path, &db);
  if (!st.ok()) {
    std::cerr << "  Open fail: " << st.ToString() << "\n";
    db.reset();
    std::filesystem::remove_all(path);
    return r;
  }

  // Apply actual tiered config via SetOptions (workaround OPTIONS file bug)
  std::unordered_map<std::string, std::string> sopts;
  if (cfg.tiered_on) {
    sopts["experimental_st_compaction_tiered_max_level"] =
        std::to_string(cfg.tiered_max_level);
    sopts["experimental_st_compaction_real_tiered_l1_l2"] =
        cfg.real_tiered ? "true" : "false";
    sopts["experimental_st_compaction_tiered_max_runs_per_level"] =
        std::to_string(cfg.max_runs);
    // STFP triggers only for STFP variants, not Hybrid
    sopts["experimental_st_compaction_stfp_enable"] =
        (cfg.stfp_enable ? "true" : "false");
  }
  if (!sopts.empty()) db->SetOptions(sopts);

  db->ResetStats();
  const int total = static_cast<int>(data.size());
  const int kBatchSize = 5000;
  rst::WriteOptions wo;
  wo.disableWAL = false;
  wo.sync = false;

  // ── Write Phase ──
  // Write all data; RocksDB auto-flushes memtable when write_buffer fills.
  // After write, drain L0 and wait for compactions to settle naturally.
  // No CompactRange — each compaction strategy produces its own layout.
  auto t0 = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < total; i += kBatchSize) {
    rst::WriteBatch batch;
    int end = std::min(i + kBatchSize, total);
    for (int j = i; j < end; ++j) {
      batch.Put(data[j].key, data[j].value);
    }
    db->Write(wo, &batch);
  }
  // Flush remaining memtable, then wait for auto-compaction to settle
  db->Flush(rst::FlushOptions());
  {
    int waited_ms = 0;
    const int kMaxWaitMs = 300000;  // 5-minute timeout for full compaction
    while (waited_ms < kMaxWaitMs) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      waited_ms += 500;
      std::string v;
      int l0_files = -1, running = -1;
      if (db->GetProperty("rocksdb.num-files-at-level0", &v)) {
        try { l0_files = std::stoi(v); } catch (...) {}
      }
      if (db->GetProperty("rocksdb.num-running-compactions", &v)) {
        try { running = std::stoi(v); } catch (...) {}
      }
      if (l0_files == 0 && running == 0) break;
    }
  }
  auto t1 = std::chrono::high_resolution_clock::now();

  using ns = std::chrono::nanoseconds;
  r.write_sec = std::chrono::duration<double>(t1 - t0).count();

  // Compaction IO
  auto GM = [&](rst::Tickers t) -> int64_t {
    return stats->getTickerCount(t);
  };
  r.compact_read_mb  = GM(rst::COMPACT_READ_BYTES) / 1048576;
  r.compact_write_mb = GM(rst::COMPACT_WRITE_BYTES) / 1048576;

  // Level file counts
  auto G = [&](const char* prop) {
    std::string v;
    if (db->GetProperty(prop, &v)) { try { return std::stoi(v); } catch (...) {} }
    return -1;
  };
  r.l0 = G("rocksdb.num-files-at-level0");
  r.l1 = G("rocksdb.num-files-at-level1");
  r.l2 = G("rocksdb.num-files-at-level2");
  r.l3 = G("rocksdb.num-files-at-level3");

  // ── Read Phase (pruned queries, 2 passes: warmup + measured) ──
  rst::SetPerfLevel(rst::PerfLevel::kEnableTimeExceptForMutex);
  r.num_queries = static_cast<int>(windows.size());
  int64_t total_result_bytes = 0;
  int64_t total_files_skipped = 0;
  int64_t total_files_considered = 0;
  int64_t total_block_examined = 0;
  int64_t total_block_skipped = 0;
  int64_t total_key_examined = 0;
  int64_t total_key_skipped = 0;
  uint64_t total_block_reads = 0;

  // Lambda: run all query windows once with full prune pipeline
  // query_result_counts[wi] = number of KV pairs returned for window wi
  std::vector<int64_t> qcounts(windows.size(), 0);
  auto RunQueryPass = [&]() {
    for (size_t wi = 0; wi < windows.size(); ++wi) {
      const auto& w = windows[wi];
      // Per-query prune counters
      uint64_t files_skipped = 0, files_considered = 0;
      uint64_t files_missing_meta = 0, files_range_del = 0;
      uint64_t files_time_disjoint = 0, files_space_disjoint = 0;
      uint64_t file_eval_count = 0, file_eval_ns = 0;
      uint64_t block_index_examined = 0, block_index_skipped = 0;
      uint64_t block_index_stop_missing = 0, block_index_prune_ns = 0;
      uint64_t key_examined = 0, key_skipped = 0;

      rst::ReadOptions ro;
      ro.total_order_seek = true;
      ro.fill_cache = false;
      ro.verify_checksums = false;
      ro.experimental_st_prune_scan.enable = true;
      ro.experimental_st_prune_scan.t_min = w.t_min;
      ro.experimental_st_prune_scan.t_max = w.t_max;
      ro.experimental_st_prune_scan.x_min = w.x_min;
      ro.experimental_st_prune_scan.x_max = w.x_max;
      ro.experimental_st_prune_scan.y_min = w.y_min;
      ro.experimental_st_prune_scan.y_max = w.y_max;
      ro.experimental_st_prune_scan.file_level_enable = true;
      ro.experimental_st_prune_scan.block_level_enable = true;
      ro.experimental_st_prune_scan.key_level_enable = true;
      ro.experimental_st_prune_scan.event_time_bucket_width_sec = bucket_sec;

      // Wire up prune counters
      ro.experimental_st_prune_scan.file_level_files_skipped = &files_skipped;
      ro.experimental_st_prune_scan.file_level_files_considered = &files_considered;
      ro.experimental_st_prune_scan.file_level_files_missing_meta = &files_missing_meta;
      ro.experimental_st_prune_scan.file_level_files_range_del_blocked = &files_range_del;
      ro.experimental_st_prune_scan.file_level_files_time_disjoint = &files_time_disjoint;
      ro.experimental_st_prune_scan.file_level_files_space_disjoint = &files_space_disjoint;
      ro.experimental_st_prune_scan.file_level_eval_count = &file_eval_count;
      ro.experimental_st_prune_scan.file_level_eval_ns = &file_eval_ns;
      ro.experimental_st_prune_scan.block_level_index_entries_examined = &block_index_examined;
      ro.experimental_st_prune_scan.block_level_index_entries_skipped_st_disjoint =
          &block_index_skipped;
      ro.experimental_st_prune_scan.block_level_index_stops_missing_meta =
          &block_index_stop_missing;
      ro.experimental_st_prune_scan.block_level_index_prune_ns = &block_index_prune_ns;
      ro.experimental_st_prune_scan.key_level_keys_examined = &key_examined;
      ro.experimental_st_prune_scan.key_level_keys_skipped_disjoint = &key_skipped;

      if (auto* pc = rst::get_perf_context(); pc != nullptr) {
        pc->Reset();
      }

      uint32_t seek_bucket = (w.t_min >= bucket_sec) ? (w.t_min / bucket_sec - 1) : 0;
      uint32_t max_bucket = w.t_max / bucket_sec + 1;
      std::string seek_key = BuildE7SeekKey(seek_bucket);

      std::unique_ptr<rst::Iterator> it(db->NewIterator(ro));
      int64_t query_bytes = 0;
      int64_t query_kv_count = 0;       // keys from prune scan
      int64_t query_hit_count = 0;      // keys passing point truth
      for (it->Seek(seek_key); it->Valid(); it->Next()) {
        if (it->key().size() >= 6) {
          const auto* d = reinterpret_cast<const uint8_t*>(it->key().data());
          uint32_t cur_bucket = (static_cast<uint32_t>(d[2]) << 24) |
                                (static_cast<uint32_t>(d[3]) << 16) |
                                (static_cast<uint32_t>(d[4]) << 8) |
                                static_cast<uint32_t>(d[5]);
          if (cur_bucket > max_bucket) break;
        }
        query_kv_count++;
        // Point-level truth: decode value, check each point against window
        std::string v(it->value().data(), it->value().size());
        segval::DecodedSegmentValue decoded;
        if (segval::DecodeSegmentValue(v, &decoded) && !decoded.is_legacy) {
          for (const auto& pt : decoded.points) {
            if (pt.unix_s >= w.t_min && pt.unix_s <= w.t_max &&
                pt.lon >= w.x_min && pt.lon <= w.x_max &&
                pt.lat >= w.y_min && pt.lat <= w.y_max) {
              query_bytes += it->key().size() + it->value().size();
              query_hit_count++;
              break;  // count each segment at most once
            }
          }
        }
      }
      if (!it->status().ok()) {
        std::cerr << "  Iterator error: " << it->status().ToString() << "\n";
      }

      qcounts[wi] = query_hit_count;  // point-truth verified count
      total_result_bytes += query_bytes;
      total_files_skipped += static_cast<int64_t>(files_skipped);
      total_files_considered += static_cast<int64_t>(files_considered);
      total_block_examined += static_cast<int64_t>(block_index_examined);
      total_block_skipped += static_cast<int64_t>(block_index_skipped);
      total_key_examined += static_cast<int64_t>(key_examined);
      total_key_skipped += static_cast<int64_t>(key_skipped);

      if (auto* pc = rst::get_perf_context(); pc != nullptr) {
        total_block_reads += pc->block_read_count;
      }
    }
  };

  // ── Pass 1: warmup (populate OS page cache, results discarded) ──
  {
    int64_t saved_bytes = total_result_bytes;
    RunQueryPass();
    total_result_bytes = saved_bytes;  // discard warmup byte counts
    // Reset prune counters after warmup
    total_files_skipped = 0;
    total_files_considered = 0;
    total_block_examined = 0;
    total_block_skipped = 0;
    total_key_examined = 0;
    total_key_skipped = 0;
    total_block_reads = 0;
  }

  // ── Pass 2: measured ──
  auto t_read_start = std::chrono::high_resolution_clock::now();
  RunQueryPass();
  auto t_read_end = std::chrono::high_resolution_clock::now();

  r.query_total_sec = std::chrono::duration<double>(t_read_end - t_read_start).count();
  r.query_result_bytes = total_result_bytes;
  r.block_read_count = total_block_reads;
  r.prune_files_skipped = total_files_skipped;
  r.prune_files_considered = total_files_considered;
  r.prune_block_index_examined = total_block_examined;
  r.prune_block_index_skipped = total_block_skipped;
  r.prune_key_examined = total_key_examined;
  r.prune_key_skipped = total_key_skipped;

  // ── Mixed workload phase (optional) ──
  // Run interleaved reads+writes at varying read ratios. Writes generate new
  // random segments; reads use the same pruned-query + point-truth pipeline.
  std::vector<double> mixed_read_mbps;   // one per ratio point
  std::vector<double> mixed_write_mbps;  // one per ratio point

  if (do_mixed_workload) {
    const int kMixedOps = mixed_ops_per_point;
    const int kRatioStep = 10;  // 0%, 10%, ..., 100% read
    std::mt19937_64 mix_rng(static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count()));

    // Pre-generate writes for the mixed phase (new segments, same regions)
    std::vector<GeneratedKV> mix_writes;
    mix_writes.reserve(kMixedOps);
    {
      int64_t dummy_bytes = 0;
      std::vector<QueryWindow> dummy_windows;  // no ground truth needed
      std::vector<int64_t> dummy_counts;
      GenerateData(mix_rng, kMixedOps, mix_writes, dummy_bytes, dummy_windows,
                   dummy_counts, bucket_sec);
    }

    // Pre-build seek keys per window
    std::vector<std::string> window_seek_keys;
    for (const auto& w : windows) {
      uint32_t sb = (w.t_min >= bucket_sec) ? (w.t_min / bucket_sec - 1) : 0;
      window_seek_keys.push_back(BuildE7SeekKey(sb));
    }

    std::cout << "  Mixed: ";
    for (int ratio_idx = 0; ratio_idx <= 10; ++ratio_idx) {
      int read_pct = ratio_idx * 10;  // 0, 10, ..., 100
      int read_ops = kMixedOps * read_pct / 100;
      int write_ops = kMixedOps - read_ops;

      // Build op list
      struct Op { bool is_read; int idx; };
      std::vector<Op> ops;
      for (int r = 0; r < read_ops; ++r) ops.push_back({true, r});
      for (int w = 0; w < write_ops; ++w) ops.push_back({false, w});
      std::shuffle(ops.begin(), ops.end(), mix_rng);

      uint64_t read_time_us = 0, write_time_us = 0;
      int64_t read_bytes = 0, write_bytes = 0;

      for (const auto& op : ops) {
        if (op.is_read) {
          int wi = op.idx % static_cast<int>(windows.size());
          const auto& w = windows[wi];
          auto tr0 = std::chrono::steady_clock::now();
          // Pruned query + point truth (simplified: no counters)
          rst::ReadOptions ro;
          ro.total_order_seek = true;
          ro.fill_cache = false;
          ro.experimental_st_prune_scan.enable = true;
          ro.experimental_st_prune_scan.t_min = w.t_min;
          ro.experimental_st_prune_scan.t_max = w.t_max;
          ro.experimental_st_prune_scan.x_min = w.x_min;
          ro.experimental_st_prune_scan.x_max = w.x_max;
          ro.experimental_st_prune_scan.y_min = w.y_min;
          ro.experimental_st_prune_scan.y_max = w.y_max;
          ro.experimental_st_prune_scan.file_level_enable = true;
          ro.experimental_st_prune_scan.block_level_enable = true;
          ro.experimental_st_prune_scan.key_level_enable = true;
          ro.experimental_st_prune_scan.event_time_bucket_width_sec = bucket_sec;

          uint32_t max_bucket = w.t_max / bucket_sec + 1;
          std::unique_ptr<rst::Iterator> it(db->NewIterator(ro));
          for (it->Seek(window_seek_keys[wi]); it->Valid(); it->Next()) {
            if (it->key().size() >= 6) {
              const auto* d = reinterpret_cast<const uint8_t*>(it->key().data());
              uint32_t cb = (static_cast<uint32_t>(d[2]) << 24) |
                            (static_cast<uint32_t>(d[3]) << 16) |
                            (static_cast<uint32_t>(d[4]) << 8) |
                            static_cast<uint32_t>(d[5]);
              if (cb > max_bucket) break;
            }
            // Point truth
            std::string v(it->value().data(), it->value().size());
            segval::DecodedSegmentValue dec;
            if (segval::DecodeSegmentValue(v, &dec) && !dec.is_legacy) {
              for (const auto& pt : dec.points) {
                if (pt.unix_s >= w.t_min && pt.unix_s <= w.t_max &&
                    pt.lon >= w.x_min && pt.lon <= w.x_max &&
                    pt.lat >= w.y_min && pt.lat <= w.y_max) {
                  read_bytes += it->key().size() + it->value().size();
                  break;
                }
              }
            }
          }
          auto tr1 = std::chrono::steady_clock::now();
          read_time_us += static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::microseconds>(tr1 - tr0).count());
        } else {
          // Write a new random segment
          int wi = op.idx % static_cast<int>(mix_writes.size());
          auto tw0 = std::chrono::steady_clock::now();
          rst::WriteOptions wo_mix;
          wo_mix.disableWAL = false;
          db->Put(wo_mix, mix_writes[wi].key, mix_writes[wi].value);
          auto tw1 = std::chrono::steady_clock::now();
          write_time_us += static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::microseconds>(tw1 - tw0).count());
          write_bytes += mix_writes[wi].key.size() + mix_writes[wi].value.size();
        }
      }
      double r_mbps = read_time_us > 0
          ? read_bytes / (read_time_us / 1e6) / 1e6 : 0;
      double w_mbps = write_time_us > 0
          ? write_bytes / (write_time_us / 1e6) / 1e6 : 0;
      mixed_read_mbps.push_back(r_mbps);
      mixed_write_mbps.push_back(w_mbps);
      std::cout << (read_pct == 0 ? "" : " │ ") << read_pct << "%R:"
                << std::fixed << std::setprecision(0)
                << "r=" << r_mbps << "MB/s w=" << w_mbps << "MB/s"
                << std::flush;
      // Flush writes to SSTs so next ratio point's reads see them
      if (write_ops > 0) {
        db->Flush(rst::FlushOptions());
      }
    }
    std::cout << "\n";
  }

  // ── Correctness verification: prune scan results vs ground truth ──
  query_result_counts = qcounts;  // copy to output parameter
  bool all_correct = true;
  for (size_t wi = 0; wi < windows.size() && wi < ground_truth_counts.size(); ++wi) {
    if (query_result_counts[wi] != ground_truth_counts[wi]) {
      all_correct = false;
      std::cerr << "  CORRECTNESS FAIL [" << windows[wi].label
                << "]: got=" << query_result_counts[wi]
                << " truth=" << ground_truth_counts[wi] << "\n";
    }
  }
  if (!all_correct) {
    std::cerr << "  CORRECTNESS: MISMATCH (prune scan + point truth != ground truth)!\n";
  }

  // Save mixed workload results
  r.mixed_read_mbps = std::move(mixed_read_mbps);
  r.mixed_write_mbps = std::move(mixed_write_mbps);

  // Cleanup
  db.reset();
  std::filesystem::remove_all(path);

  return r;
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char** argv) {
  int total_keys = 300000;
  int repeat = 10;
  bool do_mixed = false;
  int mixed_ops = 1000;
  std::string db_root = "D:/tmp/ctb";
  std::string windows_csv = "D:/Project/data/processed/unified/unified_stratified12_st_windows.csv";
  uint32_t bucket_sec = 3600;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--num-keys" && i + 1 < argc) {
      total_keys = std::atoi(argv[++i]);
    } else if (a == "--repeat" && i + 1 < argc) {
      repeat = std::atoi(argv[++i]);
    } else if (a == "--mixed-workload") {
      do_mixed = true;
    } else if (a == "--mixed-ops" && i + 1 < argc) {
      mixed_ops = std::atoi(argv[++i]);
    } else if (a == "--db-root" && i + 1 < argc) {
      db_root = argv[++i];
    } else if (a == "--windows-csv" && i + 1 < argc) {
      windows_csv = argv[++i];
    } else if (a == "--bucket-sec" && i + 1 < argc) {
      bucket_sec = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (a == "-h" || a == "--help") {
      std::cout
        << "st_compaction_throughput_bench — ST-Compaction Read/Write Throughput\n"
           "  --num-keys N         Total keys to generate (default 300000)\n"
           "  --repeat N           Repeat each config N times (default 1, use 3+ for paper)\n"
           "  --mixed-workload     Enable mixed R/W ratio sweep (0%→100% read)\n"
           "  --mixed-ops N        Ops per ratio point (default 500)\n"
           "  --db-root PATH       Temp DB directory (default D:/tmp/ctb)\n"
           "  --windows-csv PATH   Query windows CSV\n"
           "                       (default: unified_stratified12_st_windows.csv)\n"
           "  --bucket-sec N       E7 bucket width seconds (default 3600)\n";
      return 0;
    }
  }

  // ── Load query windows ──
  std::vector<QueryWindow> windows = LoadWindowsCSV(windows_csv);
  if (windows.empty()) {
    windows = BuiltinQueryWindows();
    std::cout << "Falling back to built-in windows\n";
  }
  std::cout << "=== ST-Compaction Read/Write Throughput Benchmark ===\n";
  std::cout << "Keys: " << total_keys << "  Root: " << db_root
            << "  Windows: " << windows.size() << "\n";
  for (const auto& w : windows) {
    std::cout << "  Window: " << w.label
              << " t=[" << w.t_min << "," << w.t_max << "]"
              << " x=[" << w.x_min << "," << w.x_max << "]"
              << " y=[" << w.y_min << "," << w.y_max << "]\n";
  }

  // ── Generate data once (same data for all configs) ──
  std::mt19937_64 rng(42);
  std::vector<GeneratedKV> data;
  int64_t total_kv_bytes = 0;
  std::vector<int64_t> ground_truth_counts(windows.size(), 0);
  std::cout << "\nGenerating " << total_keys << " keys... " << std::flush;
  GenerateData(rng, total_keys, data, total_kv_bytes, windows,
               ground_truth_counts, bucket_sec);
  std::cout << "done.  Data: " << (total_kv_bytes / 1e6) << " MB\n";

  // Shuffle write order (random interleaving of buckets → real key overlap)
  {
    std::mt19937_64 shuffle_rng(12345);
    std::shuffle(data.begin(), data.end(), shuffle_rng);
  }
  std::cout << "Data shuffled for random write order\n";

  // Print point-level ground truth
  std::cout << "Ground truth (point-level, segments with point in query window):\n";
  for (size_t wi = 0; wi < windows.size(); ++wi) {
    std::cout << "  " << windows[wi].label << ": " << ground_truth_counts[wi]
              << " segments\n";
  }

  // ── Compaction strategies ──
  Config cfgs[] = {
    // {label,              tiered_on, real_tiered, max_level, max_runs, stfp_enable}
    {"All-Leveled",         false,     false,        0,         0,         false},
    {"All-Tiered(R4)",      true,      true,        -1,         4,         false},
    {"Hybrid-L1(R4)",       true,      true,         1,         4,         false},
    {"Hybrid-L1+L2(R4)",    true,      true,         2,         4,         false},
    {"STFP-R4",             true,      true,        -1,         4,         true},
    {"STFP-R6",             true,      true,        -1,         6,         true},
  };
  const int N = sizeof(cfgs) / sizeof(cfgs[0]);
  // results[config_idx][repeat_idx]
  std::vector<std::vector<Result>> all_results(N);

  std::cout << "\n=== Running " << N << " configurations x " << repeat
            << " repeats ===\n";
  for (int i = 0; i < N; ++i) {
    all_results[i].reserve(repeat);
    for (int rep = 0; rep < repeat; ++rep) {
      std::cout << "[" << (i + 1) << "/" << N << "] " << cfgs[i].label
                << (repeat > 1 ? " rep=" + std::to_string(rep + 1) + "/" +
                                     std::to_string(repeat) : "")
                << " ... " << std::flush;
      std::vector<int64_t> query_result_counts;
      Result r = Run(db_root, cfgs[i], rng, data, total_kv_bytes, windows,
                     ground_truth_counts, query_result_counts, bucket_sec,
                     do_mixed, mixed_ops);
      all_results[i].push_back(r);
      double wmb = r.write_sec > 0 ? r.kv_bytes / r.write_sec / 1e6 : 0;
      double rmb = r.query_total_sec > 0
          ? r.query_result_bytes / r.query_total_sec / 1e6 : 0;
      // Per-window correctness
      bool correct = true;
      for (size_t wi = 0; wi < windows.size() && wi < ground_truth_counts.size(); ++wi) {
        int64_t got = wi < query_result_counts.size() ? query_result_counts[wi] : -1;
        if (got != ground_truth_counts[wi]) correct = false;
      }
      std::cout << "done  Write=" << std::fixed << std::setprecision(1)
                << wmb << "MB/s  Read=" << std::setprecision(1) << rmb << "MB/s"
                << "  Correct=" << (correct ? "OK" : "FAIL") << "\n";
    }
  }

  // ── Compute trimmed-mean results per config (drop 1 min + 1 max) ──
  auto TrimmedMean = [](std::vector<double> v) {
    if (v.empty()) return 0.0;
    if (v.size() <= 2) {
      double sum = 0; for (double x : v) sum += x;
      return sum / v.size();
    }
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (size_t k = 1; k < v.size() - 1; ++k) sum += v[k];
    return sum / (v.size() - 2);
  };
  auto TrimmedMeanI64 = [](std::vector<int64_t> v) {
    if (v.empty()) return (int64_t)0;
    if (v.size() <= 2) {
      int64_t sum = 0; for (int64_t x : v) sum += x;
      return sum / static_cast<int64_t>(v.size());
    }
    std::sort(v.begin(), v.end());
    int64_t sum = 0;
    for (size_t k = 1; k < v.size() - 1; ++k) sum += v[k];
    return sum / static_cast<int64_t>(v.size() - 2);
  };
  std::vector<Result> results;
  for (int i = 0; i < N; ++i) {
    Result mr;
    mr.label = cfgs[i].label;
    std::vector<double> wmb_vals, rmb_vals;
    std::vector<int64_t> block_vals;
    for (const auto& r : all_results[i]) {
      wmb_vals.push_back(r.write_sec > 0 ? r.kv_bytes / r.write_sec / 1e6 : 0);
      rmb_vals.push_back(r.query_total_sec > 0 ? r.query_result_bytes / r.query_total_sec / 1e6 : 0);
      block_vals.push_back(static_cast<int64_t>(r.block_read_count));
    }
    mr.kv_bytes = all_results[i][0].kv_bytes;
    mr.write_sec = all_results[i][0].kv_bytes / TrimmedMean(wmb_vals) / 1e6;
    mr.query_total_sec = all_results[i][0].query_result_bytes / TrimmedMean(rmb_vals) / 1e6;
    mr.query_result_bytes = all_results[i][0].query_result_bytes;
    mr.block_read_count = static_cast<uint64_t>(TrimmedMeanI64(block_vals));
    mr.compact_read_mb = TrimmedMeanI64({all_results[i][0].compact_read_mb});
    mr.compact_write_mb = TrimmedMeanI64({all_results[i][0].compact_write_mb});
    mr.l0 = all_results[i][0].l0; mr.l1 = all_results[i][0].l1;
    mr.l2 = all_results[i][0].l2; mr.l3 = all_results[i][0].l3;
    mr.num_queries = all_results[i][0].num_queries;
    mr.prune_files_skipped = TrimmedMeanI64({all_results[i][0].prune_files_skipped});
    mr.prune_files_considered = TrimmedMeanI64({all_results[i][0].prune_files_considered});
    mr.prune_block_index_examined = TrimmedMeanI64({all_results[i][0].prune_block_index_examined});
    mr.prune_block_index_skipped = TrimmedMeanI64({all_results[i][0].prune_block_index_skipped});
    mr.prune_key_examined = TrimmedMeanI64({all_results[i][0].prune_key_examined});
    mr.prune_key_skipped = TrimmedMeanI64({all_results[i][0].prune_key_skipped});
    // Mixed workload: trimmed mean across repeats for each ratio point
    if (do_mixed) {
      int n_ratios = static_cast<int>(all_results[i][0].mixed_read_mbps.size());
      mr.mixed_read_mbps.resize(n_ratios, 0);
      mr.mixed_write_mbps.resize(n_ratios, 0);
      for (int ri = 0; ri < n_ratios; ++ri) {
        std::vector<double> rv, wv;
        for (const auto& rr : all_results[i]) {
          if (ri < static_cast<int>(rr.mixed_read_mbps.size()))
            rv.push_back(rr.mixed_read_mbps[ri]);
          if (ri < static_cast<int>(rr.mixed_write_mbps.size()))
            wv.push_back(rr.mixed_write_mbps[ri]);
        }
        mr.mixed_read_mbps[ri] = TrimmedMean(rv);
        mr.mixed_write_mbps[ri] = TrimmedMean(wv);
      }
    }
    results.push_back(mr);
  }

  // ── Summary table ──
  std::cout << "\n";
  std::cout << "=== Write Throughput ===\n";
  std::cout << std::left
            << std::setw(18) << "Config"
            << std::right
            << std::setw(10) << "Write(s)"
            << std::setw(12) << "WriteMB/s"
            << std::setw(10) << "CptRdMB"
            << std::setw(10) << "CptWrMB"
            << std::setw(8) << "W-Amp"
            << std::setw(5) << "L0"
            << std::setw(5) << "L1"
            << std::setw(5) << "L2"
            << std::setw(5) << "L3"
            << "\n";
  std::cout << std::string(88, '-') << "\n";
  for (const auto& r : results) {
    double wmb = r.write_sec > 0 ? r.kv_bytes / r.write_sec / 1e6 : 0;
    double wamp = r.kv_bytes > 0
        ? static_cast<double>(r.compact_write_mb) * 1048576.0 / r.kv_bytes : 0;
    std::cout << std::left  << std::setw(18) << r.label
              << std::right << std::setw(10) << std::fixed << std::setprecision(2)
              << r.write_sec
              << std::setw(12) << std::setprecision(1) << wmb
              << std::setw(10) << r.compact_read_mb
              << std::setw(10) << r.compact_write_mb
              << std::setw(7) << std::setprecision(1) << wamp << "x"
              << std::setw(5) << r.l0
              << std::setw(5) << r.l1
              << std::setw(5) << r.l2
              << std::setw(5) << r.l3
              << "\n";
  }

  std::cout << "\n=== Read (Pruned Query) Throughput ===\n";
  std::cout << std::left
            << std::setw(18) << "Config"
            << std::right
            << std::setw(8) << "Queries"
            << std::setw(12) << "Time(s)"
            << std::setw(10) << "ResMB"
            << std::setw(12) << "ReadMB/s"
            << std::setw(12) << "BlockReads"
            << std::setw(12) << "FileSkip%"
            << std::setw(12) << "BlockSkip%"
            << std::setw(12) << "KeySkip%"
            << "\n";
  std::cout << std::string(108, '-') << "\n";
  for (const auto& r : results) {
    double rmb = r.query_total_sec > 0
        ? r.query_result_bytes / r.query_total_sec / 1e6 : 0;
    double file_skip_pct = r.prune_files_considered > 0
        ? 100.0 * r.prune_files_skipped / r.prune_files_considered : 0;
    double block_skip_pct = r.prune_block_index_examined > 0
        ? 100.0 * r.prune_block_index_skipped / r.prune_block_index_examined : 0;
    double key_skip_pct = r.prune_key_examined > 0
        ? 100.0 * r.prune_key_skipped / r.prune_key_examined : 0;
    std::cout << std::left  << std::setw(18) << r.label
              << std::right << std::setw(8) << r.num_queries
              << std::setw(12) << std::fixed << std::setprecision(3)
              << r.query_total_sec
              << std::setw(10) << std::setprecision(2)
              << (r.query_result_bytes / 1e6)
              << std::setw(12) << std::setprecision(2) << rmb
              << std::setw(12) << r.block_read_count
              << std::setw(11) << std::setprecision(1) << file_skip_pct << "%"
              << std::setw(11) << std::setprecision(1) << block_skip_pct << "%"
              << std::setw(11) << std::setprecision(1) << key_skip_pct << "%"
              << "\n";
  }

  // ── CSV output ──
  std::cout << "\n=== CSV ===\n";
  std::cout << "config,write_sec,write_mb_s,compact_read_mb,compact_write_mb,"
            << "write_amp,l0,l1,l2,l3,"
            << "num_queries,query_total_sec,query_result_mb,query_mb_s,"
            << "block_reads,file_skip_pct,block_skip_pct,key_skip_pct\n";
  for (const auto& r : results) {
    double wmb = r.write_sec > 0 ? r.kv_bytes / r.write_sec / 1e6 : 0;
    double wamp = r.kv_bytes > 0
        ? static_cast<double>(r.compact_write_mb) * 1048576.0 / r.kv_bytes : 0;
    double rmb = r.query_total_sec > 0
        ? r.query_result_bytes / r.query_total_sec / 1e6 : 0;
    double file_skip_pct = r.prune_files_considered > 0
        ? 100.0 * r.prune_files_skipped / r.prune_files_considered : 0;
    double block_skip_pct = r.prune_block_index_examined > 0
        ? 100.0 * r.prune_block_index_skipped / r.prune_block_index_examined : 0;
    double key_skip_pct = r.prune_key_examined > 0
        ? 100.0 * r.prune_key_skipped / r.prune_key_examined : 0;
    std::cout << std::fixed << std::setprecision(3)
              << r.label << ","
              << r.write_sec << ","
              << wmb << ","
              << r.compact_read_mb << ","
              << r.compact_write_mb << ","
              << wamp << ","
              << r.l0 << "," << r.l1 << "," << r.l2 << "," << r.l3 << ","
              << r.num_queries << ","
              << r.query_total_sec << ","
              << (r.query_result_bytes / 1e6) << ","
              << rmb << ","
              << r.block_read_count << ","
              << std::setprecision(1)
              << file_skip_pct << ","
              << block_skip_pct << ","
              << key_skip_pct
              << "\n";
  }

  // ── Mixed workload table ──
  if (do_mixed && !results.empty() && !results[0].mixed_read_mbps.empty()) {
    int n_ratios = static_cast<int>(results[0].mixed_read_mbps.size());
    std::cout << "\n=== Mixed Read/Write Throughput vs Read Ratio (trimmed mean of "
              << repeat << " repeats) ===\n";
    // Header
    std::cout << std::left << std::setw(18) << "Config";
    for (int ri = 0; ri < n_ratios; ++ri)
      std::cout << std::right << std::setw(9) << (ri * 10) << "%R";
    std::cout << "  (Read MB/s)\n";
    std::cout << std::string(18 + 9 * n_ratios, '-') << "\n";
    for (const auto& r : results) {
      std::cout << std::left << std::setw(18) << r.label;
      for (int ri = 0; ri < n_ratios; ++ri)
        std::cout << std::right << std::setw(8) << std::fixed << std::setprecision(1)
                  << r.mixed_read_mbps[ri] << " ";
      std::cout << "\n";
    }

    std::cout << "\n" << std::left << std::setw(18) << "Config";
    for (int ri = 0; ri < n_ratios; ++ri)
      std::cout << std::right << std::setw(9) << (ri * 10) << "%R";
    std::cout << "  (Write MB/s)\n";
    std::cout << std::string(18 + 9 * n_ratios, '-') << "\n";
    for (const auto& r : results) {
      std::cout << std::left << std::setw(18) << r.label;
      for (int ri = 0; ri < n_ratios; ++ri)
        std::cout << std::right << std::setw(8) << std::fixed << std::setprecision(1)
                  << r.mixed_write_mbps[ri] << " ";
      std::cout << "\n";
    }

    // CSV for mixed workload
    std::cout << "\n=== Mixed Workload CSV ===\n";
    std::cout << "config,ratio_pct,read_mbps,write_mbps\n";
    for (const auto& r : results) {
      for (int ri = 0; ri < n_ratios; ++ri) {
        std::cout << r.label << "," << (ri * 10) << ","
                  << std::fixed << std::setprecision(1)
                  << r.mixed_read_mbps[ri] << ","
                  << r.mixed_write_mbps[ri] << "\n";
      }
    }
  }

  std::cout << "\nDone.\n";
  return 0;
}

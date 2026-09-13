// TMan-aligned batch spatial queries: one DB session, many MBR windows sequentially.

#include "st_meta_read_bench_batch.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>
#include <rocksdb/iostats_context.h>
#include <rocksdb/perf_context.h>
#include <rocksdb/perf_level.h>
#include <rocksdb/statistics.h>
#include <rocksdb/table.h>

#include "db/manifest_ops.h"
#include "segment_value_codec.hpp"
#include "segment_value_global.hpp"
#include "table/st_meta_user_key.h"
#include "util/hash.h"

namespace {

using ROCKSDB_NAMESPACE::Hash64;
using ROCKSDB_NAMESPACE::StQuerySegmentUidDedup;
using ROCKSDB_NAMESPACE::kSpatioTemporalE7DefaultBucketSeconds;

static uint32_t CfEventTimeBucketWidthSec(rocksdb::DB* db) {
  const rocksdb::Options opts = db->GetOptions();
  const uint32_t w = opts.experimental_compaction_event_time_bucket_width;
  return w == 0 ? kSpatioTemporalE7DefaultBucketSeconds : w;
}

static uint32_t BucketIdForSec(uint32_t t_sec, uint32_t bucket_width_sec) {
  if (bucket_width_sec == 0) {
    return 0;
  }
  return t_sec / bucket_width_sec;
}

static void QueryWindowToBucketRange(uint32_t t_min_sec, uint32_t t_max_sec,
                                     uint32_t bucket_width_sec,
                                     uint32_t* bucket_min,
                                     uint32_t* bucket_max) {
  if (bucket_min == nullptr || bucket_max == nullptr) {
    return;
  }
  if (t_max_sec < t_min_sec) {
    *bucket_min = 0;
    *bucket_max = UINT32_MAX;
    return;
  }
  const uint32_t bw = bucket_width_sec == 0 ? kSpatioTemporalE7DefaultBucketSeconds
                                             : bucket_width_sec;
  // Use conservative lower bound (bucket 0) to avoid missing cross-bucket
  // segments whose canonical bucket_id is earlier than the query window.
  // The ST prune iterator (file-level st_file_meta + block-level index value
  // ST meta) provides hard filtering; this coarse bucket_range gate only needs
  // to avoid skipping segments that could potentially overlap.
  *bucket_min = 0;
  // Upper bound: query end bucket + 1 to include segments that start near the
  // end of the query window. UINT32_MAX for zero-width or safety.
  *bucket_max = (t_max_sec > 0) ? BucketIdForSec(t_max_sec, bw) + 1 : UINT32_MAX;
}

using ROCKSDB_NAMESPACE::UserKeySpatioTemporalInWindow;

constexpr unsigned char kMagicPointKey = 0xE5;
constexpr unsigned char kMagicSegmentLegacyKey = 0xE6;
constexpr unsigned char kMagicSegmentTzidKey = 0xE7;

struct SpatialWindow {
  float x_min = 0;
  float y_min = 0;
  float x_max = 0;
  float y_max = 0;
  uint32_t t_min = 0;
  uint32_t t_max = 0;
  bool has_time = false;
};

struct IoSnap {
  uint64_t bytes_read = 0;
  uint64_t read_nanos = 0;
};

IoSnap IoStatsSnap() {
  IoSnap s;
  if (rocksdb::IOStatsContext* io = rocksdb::get_iostats_context(); io != nullptr) {
    s.bytes_read = io->bytes_read;
    s.read_nanos = io->read_nanos;
  }
  return s;
}

struct QueryMetrics {
  double wall_us = 0;
  int64_t final_size = 0;
  int64_t keys_iterated = 0;
  int64_t keys_in_window = 0;
  uint64_t block_read = 0;
  uint64_t bytes_read = 0;
  uint64_t read_nanos = 0;
  uint64_t block_index_examined = 0;
  uint64_t block_index_skipped = 0;
  uint64_t file_considered = 0;
  uint64_t files_skipped = 0;
};

using ROCKSDB_NAMESPACE::UserKeyFromIteratorKey;

bool IsSegmentUserKeyMagic(unsigned char magic) {
  return magic == kMagicSegmentLegacyKey || magic == kMagicSegmentTzidKey;
}

static bool UserKeyBucketInRange(const rocksdb::Slice& user_key,
                                 uint32_t bucket_min, uint32_t bucket_max) {
  if (bucket_min > bucket_max) {
    return true;
  }
  if (user_key.size() < 2) {
    return true;
  }
  const unsigned char magic = static_cast<unsigned char>(user_key.data()[0]);
  if (magic == kMagicSegmentLegacyKey || magic == kMagicSegmentTzidKey) {
    if (user_key.size() < 6) {
      return true;
    }
    const unsigned char* p = reinterpret_cast<const unsigned char*>(user_key.data());
    const uint32_t bucket_id = (static_cast<uint32_t>(p[2]) << 24) |
                               (static_cast<uint32_t>(p[3]) << 16) |
                               (static_cast<uint32_t>(p[4]) << 8) |
                               static_cast<uint32_t>(p[5]);
    return bucket_id >= bucket_min && bucket_id <= bucket_max;
  }
  if (ROCKSDB_NAMESPACE::IsTmanStSegmentUserKey(user_key)) {
    if (user_key.size() < 2) {
      return true;
    }
    const unsigned char* p = reinterpret_cast<const unsigned char*>(user_key.data());
    const uint32_t period = (static_cast<uint32_t>(p[0]) << 8) |
                            static_cast<uint32_t>(p[1]);
    return period >= bucket_min && period <= bucket_max;
  }
  return true;
}

static bool BucketMetaMissingFallback(uint32_t bucket_min, uint32_t bucket_max) {
  return bucket_min > bucket_max;
}

static int SliceCompareString(const rocksdb::Slice& a, const std::string& b) {
  const size_t n = std::min(a.size(), b.size());
  const int cmp = std::memcmp(a.data(), b.data(), n);
  if (cmp != 0) {
    return cmp;
  }
  if (a.size() < b.size()) return -1;
  if (a.size() > b.size()) return 1;
  return 0;
}

struct PointTruthEval {
  bool supported = false;
  bool in_window = false;
};

PointTruthEval EvalPointTruthInWindow(rocksdb::DB* db,
                                      const rocksdb::ReadOptions& ro,
                                      const rocksdb::Slice& user_key,
                                      const rocksdb::Slice& value, uint32_t t_min,
                                      uint32_t t_max, float x_min, float x_max,
                                      float y_min, float y_max) {
  PointTruthEval out;
  (void)db;
  (void)ro;
  if (user_key.empty()) {
    return out;
  }
  const unsigned char magic = static_cast<unsigned char>(user_key.data()[0]);
  if (magic == kMagicPointKey) {
    out.supported = true;
    out.in_window = UserKeySpatioTemporalInWindow(user_key, t_min, t_max, x_min, x_max,
                                                  y_min, y_max);
    return out;
  }
  if (ROCKSDB_NAMESPACE::IsTmanStSegmentUserKey(user_key)) {
    out.supported = true;
    segval::DecodedSegmentValue decoded{};
    if (!segval::DecodeSegmentValue(std::string(value.data(), value.size()),
                                    &decoded)) {
      return out;
    }
    if (decoded.is_legacy) {
      return out;
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
  if (magic != kMagicSegmentLegacyKey && magic != kMagicSegmentTzidKey) {
    return out;
  }
  out.supported = true;
  segval::DecodedSegmentValue decoded{};
  if (!segval::DecodeSegmentValue(std::string(value.data(), value.size()),
                                  &decoded)) {
    return out;
  }
  if (decoded.is_legacy) {
    return out;
  }
  for (const auto& p : decoded.points) {
    if (p.unix_s >= t_min && p.unix_s <= t_max && p.lon >= x_min && p.lon <= x_max &&
        p.lat >= y_min && p.lat <= y_max) {
      out.in_window = true;
      return out;
    }
  }
  return out;
}

static std::string Trim(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) {
    ++b;
  }
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
    --e;
  }
  return s.substr(b, e - b);
}

static bool ParseQueryWindow(const std::string& token, SpatialWindow* out) {
  std::vector<double> vals;
  std::stringstream ss(token);
  std::string part;
  while (std::getline(ss, part, ',')) {
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(part.c_str(), &end);
    if (end == part.c_str() || errno != 0) {
      return false;
    }
    vals.push_back(v);
  }
  if (vals.size() != 4 && vals.size() != 6) {
    return false;
  }
  out->x_min = static_cast<float>(vals[0]);
  out->y_min = static_cast<float>(vals[1]);
  out->x_max = static_cast<float>(vals[2]);
  out->y_max = static_cast<float>(vals[3]);
  out->has_time = (vals.size() == 6);
  if (!out->has_time) {
    return true;
  }
  // TMan STQuery passes start/end in milliseconds; prune uses unix seconds.
  const double t0_ms = vals[4];
  const double t1_ms = vals[5];
  const double t0_s = t0_ms / 1000.0;
  const double t1_s = t1_ms / 1000.0;
  if (t0_s < 0.0 || t1_s < t0_s || t0_s > static_cast<double>(UINT32_MAX) ||
      t1_s > static_cast<double>(UINT32_MAX)) {
    return false;
  }
  out->t_min = static_cast<uint32_t>(t0_s);
  out->t_max = static_cast<uint32_t>(t1_s);
  return true;
}

static bool LoadSpatialQueryBatch(const std::string& path,
                                  std::vector<SpatialWindow>* out) {
  std::ifstream f(path);
  if (!f) {
    std::cerr << "Cannot open query batch file: " << path << "\n";
    return false;
  }
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  content = Trim(content);
  if (content.empty()) {
    return false;
  }
  std::vector<std::string> tokens;
  if (content.find(';') != std::string::npos) {
    std::stringstream ss(content);
    std::string tok;
    while (std::getline(ss, tok, ';')) {
      tok = Trim(tok);
      if (!tok.empty()) {
        tokens.push_back(tok);
      }
    }
  } else {
    std::stringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
      line = Trim(line);
      if (!line.empty()) {
        tokens.push_back(line);
      }
    }
  }
  out->clear();
  out->reserve(tokens.size());
  for (const auto& tok : tokens) {
    SpatialWindow w;
    if (!ParseQueryWindow(tok, &w)) {
      std::cerr << "Bad query window (expected 4 or 6 comma fields): " << tok
                << "\n";
      return false;
    }
    out->push_back(w);
  }
  return !out->empty();
}

static bool EnvPrefersTmanStKey() {
  const char* v = std::getenv("ROCKSDB_ST_USER_KEY_SCHEMA");
  return v == nullptr || *v == '\0' || std::strcmp(v, "tman_st") == 0;
}

static void DeriveBucketRange(const StMetaReadBenchBatchConfig& cfg,
                              uint32_t t_min_sec, uint32_t t_max_sec,
                              uint32_t* bucket_min, uint32_t* bucket_max) {
  const uint32_t bw = cfg.bucket_width_sec == 0 ? kSpatioTemporalE7DefaultBucketSeconds
                                                 : cfg.bucket_width_sec;
  if (t_max_sec < t_min_sec) {
    *bucket_min = 0;
    *bucket_max = UINT32_MAX;
    return;
  }
  *bucket_min = BucketIdForSec(t_min_sec, bw);
  *bucket_max = BucketIdForSec(t_max_sec, bw);
}

static void ApplyPruneMode(rocksdb::ReadOptions& prune,
                           const StMetaReadBenchBatchConfig& cfg) {
  auto& ps = prune.experimental_st_prune_scan;
  if (EnvPrefersTmanStKey()) {
    // Until rocksdb is rebuilt with StUserKeySchemaPrefersTmanSt() index Seek,
    // E7 lower-bound Seek skips all [period][spatial][tid] keys (0x00 prefix).
    ps.block_level_index_seek_lower_bound_enable = false;
  }
  if (cfg.prune_mode == "sst") {
    ps.file_level_enable = false;
    ps.block_level_enable = true;
    ps.key_level_enable = true;
  } else if (cfg.prune_mode == "manifest" || cfg.prune_mode == "manifest_timebucket_rtree") {
    ps.file_level_enable = true;
    ps.block_level_enable = false;
    ps.key_level_enable = false;
    ps.file_level_time_bucket_rtree_enable = true;
    ps.file_level_time_bucket_count = cfg.time_bucket_count;
    ps.file_level_rtree_leaf_size = cfg.rtree_leaf_size;
  } else if (cfg.prune_mode == "sst_manifest_pipeline") {
    ps.file_level_enable = true;
    ps.block_level_enable = true;
    ps.key_level_enable = false;
  } else {
    ps.file_level_enable = true;
    ps.block_level_enable = true;
    ps.key_level_enable = cfg.sst_manifest_key_level;
    ps.block_level_adaptive_gate_enable = cfg.sst_manifest_adaptive_block_gate;
    ps.block_level_disable_overlap_threshold = cfg.adaptive_block_overlap_threshold;
    ps.key_level_adaptive_gate_enable = cfg.sst_manifest_adaptive_key_gate;
    ps.key_level_disable_overlap_threshold = cfg.adaptive_overlap_threshold;
    ps.key_level_boundary_blocks_only_enable = cfg.sst_manifest_key_level_boundary_only;
  }
  if (ps.file_level_enable) {
    ps.file_level_contains_batch_prewarm_enable = cfg.vm_contains_batch_prewarm;
    const uint32_t span = cfg.envelope_t_max - cfg.envelope_t_min + 1;
    const bool auto_hit =
        cfg.virtual_merge_auto && (span >= cfg.vm_time_span_sec_threshold);
    if (cfg.virtual_merge_enable || auto_hit) {
      ps.file_level_time_bucket_rtree_enable = true;
      ps.file_level_time_bucket_count = cfg.time_bucket_count;
      ps.file_level_rtree_leaf_size = cfg.rtree_leaf_size;
    }
    if (cfg.file_level_rtree_skip_ratio_gate && ps.file_level_time_bucket_rtree_enable) {
      ps.file_level_rtree_skip_ratio_gate_enable = true;
      ps.file_level_rtree_min_skip_ratio = cfg.file_level_rtree_min_skip_ratio;
    }
  }
}

static void SummarizeAndPrint(const std::vector<double>& times_ms,
                              const std::vector<int64_t>& final_sizes) {
  auto summarize = [](const std::vector<double>& v) {
    struct S {
      double min_v, max_v, avg_v;
    } s{0, 0, 0};
    if (v.empty()) {
      return s;
    }
    s.min_v = v[0];
    s.max_v = v[0];
    double sum = 0;
    for (double x : v) {
      s.min_v = std::min(s.min_v, x);
      s.max_v = std::max(s.max_v, x);
      sum += x;
    }
    s.avg_v = sum / static_cast<double>(v.size());
    return s;
  };
  std::vector<double> fs;
  fs.reserve(final_sizes.size());
  for (int64_t x : final_sizes) {
    fs.push_back(static_cast<double>(x));
  }
  const auto st = summarize(times_ms);
  const auto ss = summarize(fs);
  std::cout << "spatial_batch_summary queries=" << times_ms.size() << "\n";
  std::cout << "type,min,max,avg,mid,per70,per80,per90\n";
  std::cout << "time," << st.min_v << "," << st.max_v << "," << st.avg_v << ",0,0,0,0\n";
  std::cout << "finalSize," << ss.min_v << "," << ss.max_v << "," << ss.avg_v
            << ",0,0,0,0\n";
}

static QueryMetrics RunOneQuery(rocksdb::DB* db, rocksdb::ReadOptions& prune,
                                StQuerySegmentUidDedup* dedup,
                                bool iterator_dedup, bool verify,
                                uint32_t t_min, uint32_t t_max,
                                uint32_t bucket_min, uint32_t bucket_max,
                                const SpatialWindow& w) {
  QueryMetrics m;
  auto& ps = prune.experimental_st_prune_scan;
  ps.t_min = t_min;
  ps.t_max = t_max;
  ps.x_min = w.x_min;
  ps.x_max = w.x_max;
  ps.y_min = w.y_min;
  ps.y_max = w.y_max;
  // block_level_enable/file_level_enable/key_level_enable already set by
  // ApplyPruneMode — do NOT override block_level_index_seek_lower_bound
  // which depends on EnvPrefersTmanStKey() matching the DB schema.

  std::vector<manifest_ops::ManifestFileEntry> candidates =
      manifest_ops::BuildCandidateFilesFromLive(db, t_min, t_max,
                                                w.x_min, w.x_max,
                                                w.y_min, w.y_max);
  const bool use_candidate_filter = !candidates.empty();
  std::vector<manifest_ops::ManifestFileEntry> local_candidates;
  local_candidates.reserve(candidates.size());
  for (const auto& c : candidates) {
    if (!c.has_key_range) {
      local_candidates.push_back(c);
      continue;
    }
    const rocksdb::Slice uk_min(reinterpret_cast<const char*>(c.smallest_key.data()),
                                c.smallest_key.size());
    const rocksdb::Slice uk_max(reinterpret_cast<const char*>(c.largest_key.data()),
                                c.largest_key.size());
    const bool lower_ok = SliceCompareString(uk_max, c.smallest_key) >= 0;
    (void)uk_min;
    if (lower_ok) {
      local_candidates.push_back(c);
    }
  }
  if (!local_candidates.empty()) {
    candidates.swap(local_candidates);
  }

  uint64_t files_skipped = 0, files_considered = 0, files_missing_meta = 0;
  uint64_t files_range_del = 0, files_time_disjoint = 0, files_space_disjoint = 0;
  uint64_t file_eval_count = 0, file_eval_ns = 0;
  uint64_t block_examined = 0, block_skipped = 0, block_stop_nometa = 0, block_prune_ns = 0;
  uint64_t key_examined = 0, key_skipped = 0;
  ps.file_level_files_skipped = &files_skipped;
  ps.file_level_files_considered = &files_considered;
  ps.file_level_files_missing_meta = &files_missing_meta;
  ps.file_level_files_range_del_blocked = &files_range_del;
  ps.file_level_files_time_disjoint = &files_time_disjoint;
  ps.file_level_files_space_disjoint = &files_space_disjoint;
  ps.file_level_eval_count = &file_eval_count;
  ps.file_level_eval_ns = &file_eval_ns;
  ps.block_level_index_entries_examined = &block_examined;
  ps.block_level_index_entries_skipped_st_disjoint = &block_skipped;
  ps.block_level_index_stops_missing_meta = &block_stop_nometa;
  ps.block_level_index_prune_ns = &block_prune_ns;
  ps.key_level_keys_examined = &key_examined;
  ps.key_level_keys_skipped_disjoint = &key_skipped;

  if (dedup != nullptr) {
    dedup->Reset();
  }
  if (auto* pc = rocksdb::get_perf_context(); pc != nullptr) {
    pc->Reset();
  }
  if (rocksdb::IOStatsContext* io = rocksdb::get_iostats_context(); io != nullptr) {
    io->Reset();
  }
  const IoSnap io0 = IoStatsSnap();

  int64_t count_prune = 0;
  int64_t count_in_window = 0;
  int64_t segment_kv = 0;

  const auto t0 = std::chrono::steady_clock::now();
  {
    std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(prune));
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      const rocksdb::Slice uk = UserKeyFromIteratorKey(it->key());
      if (!UserKeyBucketInRange(uk, bucket_min, bucket_max)) {
        continue;
      }
      if (!candidates.empty()) {
        bool candidate_hit = false;
        for (const auto& c : candidates) {
          if (c.is_conservative) {
            candidate_hit = true;
            break;
          }
          const bool time_ok = !(c.t_max < t_min || c.t_min > t_max);
          const bool space_ok = !(c.x_max < w.x_min || c.x_min > w.x_max ||
                                  c.y_max < w.y_min || c.y_min > w.y_max);
          const bool key_ok = manifest_ops::CandidateMayContainKey(c, uk);
          if (time_ok && space_ok && key_ok) {
            candidate_hit = true;
            break;
          }
        }
        if (!candidate_hit) {
          continue;
        }
      }
      ++count_prune;
      const bool in_win = UserKeySpatioTemporalInWindow(uk, t_min, t_max, w.x_min,
                                                        w.x_max, w.y_min, w.y_max);
      if (in_win) {
        ++count_in_window;
      }
      if (!verify) {
        continue;
      }
      // When key_level is enabled, the iterator already performed point-truth
      // verification (SegmentValueHasPointInWindow) before yielding each key.
      // Skip redundant EvalPointTruthInWindow to avoid divergence between the
      // two code paths. When key_level is off, perform the full check here.
      if (!prune.experimental_st_prune_scan.key_level_enable) {
        const PointTruthEval pe = EvalPointTruthInWindow(
            db, prune, uk, it->value(), t_min, t_max, w.x_min, w.x_max, w.y_min, w.y_max);
        if (!pe.in_window) {
          continue;
        }
      }
      if (!iterator_dedup && dedup != nullptr && !dedup->AcceptEmitUserKey(uk)) {
        continue;
      }
      ++segment_kv;
    }
    if (!it->status().ok()) {
      std::cerr << "Batch prune iter: " << it->status().ToString() << "\n";
    }
  }
  const auto t1 = std::chrono::steady_clock::now();
  m.wall_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
  m.final_size = segment_kv;
  m.keys_iterated = count_prune;
  m.keys_in_window = count_in_window;
  m.block_index_examined = block_examined;
  m.block_index_skipped = block_skipped;
  m.file_considered = files_considered;
  m.files_skipped = files_skipped;
  if (auto* pc = rocksdb::get_perf_context(); pc != nullptr) {
    m.block_read = pc->block_read_count;
  }
  const IoSnap io1 = IoStatsSnap();
  m.bytes_read = io1.bytes_read - io0.bytes_read;
  m.read_nanos = io1.read_nanos - io0.read_nanos;
  return m;
}

}  // namespace

int RunStMetaReadBenchSpatialBatch(rocksdb::DB* db,
                                   const StMetaReadBenchBatchConfig& cfg) {
  if (db == nullptr) {
    return 1;
  }
  std::vector<SpatialWindow> windows;
  if (!LoadSpatialQueryBatch(cfg.query_batch_file, &windows)) {
    return 1;
  }
  const bool any_per_query_time =
      std::any_of(windows.begin(), windows.end(),
                  [](const SpatialWindow& w) { return w.has_time; });
  if (!any_per_query_time && cfg.envelope_t_min > cfg.envelope_t_max) {
    std::cerr << "Invalid envelope time range\n";
    return 1;
  }

  std::cout << "spatial_batch_mode=1 queries=" << windows.size()
            << " per_query_time=" << (any_per_query_time ? 1 : 0)
            << " envelope_t=[" << cfg.envelope_t_min << "," << cfg.envelope_t_max
            << "] file=" << cfg.query_batch_file << "\n";

  // ── Total block stats from live SST metadata ──
  {
    std::vector<rocksdb::LiveFileMetaData> live;
    db->GetLiveFilesMetaData(&live);
    uint64_t total_sst = 0;
    uint64_t total_num_entries = 0;
    uint64_t total_compressed_bytes = 0;
    for (const auto& f : live) {
      if (f.level >= 0) {
        ++total_sst;
        total_num_entries += f.num_entries;
        total_compressed_bytes += f.size;
      }
    }
    std::cout << "db_total_blocks sst_count=" << total_sst
              << " total_num_entries=" << total_num_entries
              << " total_compressed_bytes=" << total_compressed_bytes << "\n";
  }

  rocksdb::ReadOptions prune(rocksdb::Env::IOActivity::kDBIterator);
  prune.total_order_seek = true;
  prune.fill_cache = false;
  prune.experimental_st_prune_scan.enable = true;
  const uint32_t cf_bucket_width_sec = CfEventTimeBucketWidthSec(db);
  prune.experimental_st_prune_scan.event_time_bucket_width_sec =
      cf_bucket_width_sec;
  StQuerySegmentUidDedup dedup;
  dedup.bucket_width_sec = cf_bucket_width_sec;
  const bool iterator_dedup = cfg.query_segment_uid_dedup;
  if (iterator_dedup) {
    prune.experimental_st_prune_scan.query_dedup_by_segment_uid_enable = true;
    prune.experimental_st_prune_scan.query_segment_uid_dedup = &dedup;
  }
  ApplyPruneMode(prune, cfg);

  std::vector<double> times_ms;
  std::vector<int64_t> final_sizes;
  times_ms.reserve(windows.size());
  final_sizes.reserve(windows.size());

  for (size_t i = 0; i < windows.size(); ++i) {
    const uint32_t t_min =
        windows[i].has_time ? windows[i].t_min : cfg.envelope_t_min;
    const uint32_t t_max =
        windows[i].has_time ? windows[i].t_max : cfg.envelope_t_max;
    uint32_t bucket_min = 0;
    uint32_t bucket_max = UINT32_MAX;
    QueryWindowToBucketRange(t_min, t_max, cf_bucket_width_sec, &bucket_min, &bucket_max);
    const QueryMetrics m =
        RunOneQuery(db, prune, &dedup, iterator_dedup, cfg.verify_kv_results,
                    t_min, t_max, bucket_min, bucket_max, windows[i]);
    times_ms.push_back(m.wall_us / 1000.0);
    final_sizes.push_back(m.final_size);
    std::cout << "spatial_batch_query id=" << i << " time_ms=" << std::fixed
              << std::setprecision(3) << (m.wall_us / 1000.0)
              << " finalSize=" << m.final_size
              << " keys_iterated=" << m.keys_iterated
              << " keys_in_window=" << m.keys_in_window
              << " block_read_count=" << m.block_read
              << " bytes_read=" << m.bytes_read
              << " read_nanos=" << m.read_nanos
              << " block_index_examined=" << m.block_index_examined
              << " block_index_skipped=" << m.block_index_skipped
              << " file_considered=" << m.file_considered
              << " files_skipped=" << m.files_skipped << std::defaultfloat
              << "\n";
  }

  SummarizeAndPrint(times_ms, final_sizes);
  return 0;
}

// Compare query efficiency across ST-compaction, Leveled, and Tiered/Universal
// compaction strategies. Runs each query window against each DB in both
// full-scan and prune-scan modes, measuring wall time / IO / block-reads.
//
// Usage:
//   st_compaction_bench \
//     --db-st       D:/Project/data/db/tdrive_st_st \
//     --db-leveled  D:/Project/data/db/tdrive_st_leveled \
//     --db-tiered   D:/Project/data/db/tdrive_st_tiered \
//     --windows-csv D:/Project/data/processed/unified/unified_stratified12_st_windows.csv

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
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
#include "table/st_meta_user_key.h"

namespace {

using ROCKSDB_NAMESPACE::UserKeySpatioTemporalInWindow;

constexpr unsigned char kMagicPointKey = 0xE5;
constexpr unsigned char kMagicSegmentLegacyKey = 0xE6;
constexpr unsigned char kMagicSegmentTzidKey = 0xE7;

// Default RocksDB internal key: <user_key | 8-byte seq+type>.
inline rocksdb::Slice UserKeyFromInternalKey(const rocksdb::Slice& internal_key) {
  constexpr size_t kFooter = 8;
  if (internal_key.size() < kFooter) return rocksdb::Slice();
  return rocksdb::Slice(internal_key.data(), internal_key.size() - kFooter);
}

inline bool LooksLikeStUserKey(const rocksdb::Slice& user_key) {
  if (user_key.size() < 1) return false;
  const unsigned char magic = static_cast<unsigned char>(user_key.data()[0]);
  if (magic == kMagicPointKey) return user_key.size() >= 13;
  if (magic == kMagicSegmentLegacyKey) return user_key.size() >= 25;
  if (magic == kMagicSegmentTzidKey) return user_key.size() >= 27;
  return false;
}

using ROCKSDB_NAMESPACE::UserKeyFromIteratorKey;

inline bool IsSegmentUserKeyMagic(unsigned char magic) {
  return magic == kMagicSegmentLegacyKey || magic == kMagicSegmentTzidKey;
}

bool ParseU32(const char* s, uint32_t* out) {
  if (s == nullptr || out == nullptr) return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long v = std::strtoul(s, &end, 10);
  if (end == s || *end != '\0' || errno != 0) return false;
  if (v > static_cast<unsigned long>(std::numeric_limits<uint32_t>::max())) return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

bool ParseFloat(const char* s, float* out) {
  if (s == nullptr || out == nullptr) return false;
  char* end = nullptr;
  errno = 0;
  const double d = std::strtod(s, &end);
  if (end == s || *end != '\0' || errno != 0) return false;
  *out = static_cast<float>(d);
  return true;
}

struct QueryWindow {
  std::string label;
  uint32_t t_min = 0, t_max = 0;
  float x_min = 0, x_max = 0, y_min = 0, y_max = 0;
  uint64_t ref_st_segments = 0;
};

std::vector<QueryWindow> LoadWindowsFromCsv(const std::string& path) {
  std::vector<QueryWindow> out;
  std::ifstream f(path);
  if (!f.is_open()) {
    std::cerr << "ERROR: cannot open windows CSV: " << path << "\n";
    return out;
  }
  std::string line;
  // Skip header
  std::getline(f, line);
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    // CSV: Label,TMin,TMax,XMin,XMax,YMin,YMax,STSegments,STSegmentsMBR,SpatialSegments,Note
    std::vector<std::string> cols;
    std::istringstream ss(line);
    std::string col;
    while (std::getline(ss, col, ',')) {
      cols.push_back(col);
    }
    if (cols.size() < 8) continue;
    QueryWindow w;
    w.label = cols[0];
    w.t_min = static_cast<uint32_t>(std::strtoul(cols[1].c_str(), nullptr, 10));
    w.t_max = static_cast<uint32_t>(std::strtoul(cols[2].c_str(), nullptr, 10));
    w.x_min = std::strtof(cols[3].c_str(), nullptr);
    w.x_max = std::strtof(cols[4].c_str(), nullptr);
    w.y_min = std::strtof(cols[5].c_str(), nullptr);
    w.y_max = std::strtof(cols[6].c_str(), nullptr);
    w.ref_st_segments = static_cast<uint64_t>(std::strtoull(cols[7].c_str(), nullptr, 10));
    if (w.t_min > 0 || w.t_max > 0) out.push_back(w);
  }
  return out;
}

struct ScanResult {
  uint64_t wall_us = 0;
  uint64_t block_read_count = 0;
  uint64_t block_read_byte = 0;
  uint64_t io_bytes_read = 0;
  uint64_t io_read_nanos = 0;
  uint64_t keys_scanned = 0;
  uint64_t result_count = 0;
  bool ok = false;
};

// Point-in-window evaluation for both point keys (0xE5) and segment keys (0xE7).
bool ValueInWindow(rocksdb::DB* db, const rocksdb::ReadOptions& ro,
                   const rocksdb::Slice& user_key, const rocksdb::Slice& value,
                   uint32_t t_min, uint32_t t_max,
                   float x_min, float x_max, float y_min, float y_max) {
  if (user_key.size() < 1) return false;
  const unsigned char magic = static_cast<unsigned char>(user_key.data()[0]);
  if (magic == kMagicPointKey) {
    return UserKeySpatioTemporalInWindow(user_key, t_min, t_max, x_min, x_max, y_min, y_max);
  }
  if (magic != kMagicSegmentLegacyKey && magic != kMagicSegmentTzidKey) return false;

  segval::DecodedSegmentValue decoded{};
  if (!segval::DecodeSegmentValue(std::string(value.data(), value.size()), &decoded))
    return false;
  if (decoded.is_legacy) return false;
  for (const auto& p : decoded.points) {
    if (p.unix_s >= t_min && p.unix_s <= t_max &&
        p.lon >= x_min && p.lon <= x_max &&
        p.lat >= y_min && p.lat <= y_max) {
      return true;
    }
  }
  return false;
}

rocksdb::Options OpenOptions(int block_cache_mb = 8) {
  rocksdb::Options o;
  o.create_if_missing = false;
  o.statistics = rocksdb::CreateDBStatistics();
  rocksdb::BlockBasedTableOptions tb;
  tb.experimental_spatio_temporal_meta_in_index_value = true;
  tb.block_cache = rocksdb::NewLRUCache(
      static_cast<size_t>(std::max(1, block_cache_mb)) * size_t{1048576});
  tb.cache_index_and_filter_blocks = true;
  o.table_factory.reset(rocksdb::NewBlockBasedTableFactory(tb));
  return o;
}

// Run full scan: SeekToFirst + Next, counting keys in window.
ScanResult RunFullScan(rocksdb::DB* db, const QueryWindow& w) {
  ScanResult r;
  rocksdb::ReadOptions ro(rocksdb::Env::IOActivity::kDBIterator);
  ro.total_order_seek = true;
  ro.fill_cache = false;

  rocksdb::SetPerfLevel(rocksdb::PerfLevel::kEnableTimeExceptForMutex);
  rocksdb::get_perf_context()->Reset();
  rocksdb::get_iostats_context()->Reset();

  auto t0 = std::chrono::steady_clock::now();
  std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro));

  uint64_t scanned = 0;
  uint64_t in_window = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const rocksdb::Slice uk = UserKeyFromIteratorKey(it->key());
    if (!LooksLikeStUserKey(uk)) continue;
    ++scanned;
    if (ValueInWindow(db, ro, uk, it->value(),
                      w.t_min, w.t_max, w.x_min, w.x_max, w.y_min, w.y_max)) {
      ++in_window;
    }
  }
  auto t1 = std::chrono::steady_clock::now();
  r.ok = it->status().ok();
  r.wall_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
  r.block_read_count = rocksdb::get_perf_context()->block_read_count;
  r.block_read_byte = rocksdb::get_perf_context()->block_read_byte;
  r.io_bytes_read = rocksdb::get_iostats_context()->bytes_read;
  r.io_read_nanos = rocksdb::get_iostats_context()->read_nanos;
  r.keys_scanned = scanned;
  r.result_count = in_window;
  return r;
}

// Run prune scan using experimental_st_prune_scan.
ScanResult RunPruneScan(rocksdb::DB* db, const QueryWindow& w) {
  ScanResult r;
  rocksdb::ReadOptions ro(rocksdb::Env::IOActivity::kDBIterator);
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
  // Set bucket width from DB options
  const rocksdb::Options opts = db->GetOptions();
  ro.experimental_st_prune_scan.event_time_bucket_width_sec =
      opts.experimental_compaction_event_time_bucket_width == 0
          ? 3600u
          : opts.experimental_compaction_event_time_bucket_width;

  rocksdb::SetPerfLevel(rocksdb::PerfLevel::kEnableTimeExceptForMutex);
  rocksdb::get_perf_context()->Reset();
  rocksdb::get_iostats_context()->Reset();

  auto t0 = std::chrono::steady_clock::now();
  std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro));

  uint64_t scanned = 0;
  uint64_t in_window = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const rocksdb::Slice uk = UserKeyFromIteratorKey(it->key());
    if (!LooksLikeStUserKey(uk)) continue;
    ++scanned;
    // With prune_scan, the iterator only yields keys in the window.
    // Still verify with point-truth check for correctness.
    if (ValueInWindow(db, ro, uk, it->value(),
                      w.t_min, w.t_max, w.x_min, w.x_max, w.y_min, w.y_max)) {
      ++in_window;
    }
  }
  auto t1 = std::chrono::steady_clock::now();
  r.ok = it->status().ok();
  r.wall_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
  r.block_read_count = rocksdb::get_perf_context()->block_read_count;
  r.block_read_byte = rocksdb::get_perf_context()->block_read_byte;
  r.io_bytes_read = rocksdb::get_iostats_context()->bytes_read;
  r.io_read_nanos = rocksdb::get_iostats_context()->read_nanos;
  r.keys_scanned = scanned;
  r.result_count = in_window;
  return r;
}

struct DbResult {
  std::string db_label;
  std::string window_label;
  int iter = 0;
  ScanResult full;
  ScanResult prune;
};

void PrintSeparator(int cols) {
  for (int i = 0; i < cols; ++i) std::cout << "+----------";
  std::cout << "+\n";
}

template<typename T>
T Median(std::vector<T> v) {
  if (v.empty()) return T{};
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

void PrintResults(const std::vector<DbResult>& results) {
  const int n = static_cast<int>(results.size());
  if (n == 0) return;

  // Collect window labels and db labels in order
  std::vector<std::string> db_labels;
  std::vector<std::string> win_labels;
  for (const auto& r : results) {
    if (std::find(db_labels.begin(), db_labels.end(), r.db_label) == db_labels.end())
      db_labels.push_back(r.db_label);
    if (std::find(win_labels.begin(), win_labels.end(), r.window_label) == win_labels.end())
      win_labels.push_back(r.window_label);
  }

  // Group results by (db, window) for median computation
  struct Key { std::string db; std::string win; };
  auto cmp = [](const Key& a, const Key& b) {
    if (a.win != b.win) return a.win < b.win;
    return a.db < b.db;
  };
  std::map<Key, std::vector<DbResult>, decltype(cmp)> groups(cmp);
  for (const auto& r : results) {
    groups[{r.db_label, r.window_label}].push_back(r);
  }

  const int repeat = results.empty() ? 1
      : static_cast<int>(groups.begin()->second.size());

  std::cout << "\n";
  std::cout << "=== QUERY BENCHMARK RESULTS (median of " << repeat
            << " runs) ===\n\n";

  // ---- Summary table ----
  std::cout << std::left;
  std::cout << std::setw(28) << "Window"
            << std::setw(14) << "DB"
            << std::setw(14) << "Full(μs)"
            << std::setw(14) << "Prune(μs)"
            << std::setw(12) << "Speedup"
            << std::setw(14) << "Full blk"
            << std::setw(14) << "Prune blk"
            << std::setw(12) << "Blk ratio"
            << std::setw(14) << "Full IO"
            << std::setw(14) << "Prune IO"
            << std::setw(14) << "Full res"
            << std::setw(14) << "Prune res"
            << "OK?\n";

  for (const auto& wl : win_labels) {
    bool first = true;
    for (const auto& dl : db_labels) {
      auto it = groups.find({dl, wl});
      if (it == groups.end()) continue;
      const auto& vec = it->second;

      // Compute medians
      std::vector<uint64_t> fw, pw, fb, pb, fi, pi, fr, pr;
      for (const auto& r : vec) {
        fw.push_back(r.full.wall_us);
        pw.push_back(r.prune.wall_us);
        fb.push_back(r.full.block_read_count);
        pb.push_back(r.prune.block_read_count);
        fi.push_back(r.full.io_bytes_read);
        pi.push_back(r.prune.io_bytes_read);
        fr.push_back(r.full.result_count);
        pr.push_back(r.prune.result_count);
      }
      const uint64_t m_fw = Median(fw), m_pw = Median(pw);
      const uint64_t m_fb = Median(fb), m_pb = Median(pb);
      const uint64_t m_fi = Median(fi), m_pi = Median(pi);
      const uint64_t m_fr = Median(fr), m_pr = Median(pr);

      const double speedup = m_fw > 0
          ? static_cast<double>(m_fw) / std::max(uint64_t{1}, m_pw) : 0.0;
      const double block_ratio = m_fb > 0
          ? static_cast<double>(m_pb) / m_fb : 0.0;
      const bool match = m_fr == m_pr;

      std::cout << std::setw(28) << (first ? wl : "")
                << std::setw(14) << dl
                << std::setw(14) << m_fw
                << std::setw(14) << m_pw
                << std::setw(11) << std::fixed << std::setprecision(2) << speedup << "x"
                << std::setw(14) << m_fb
                << std::setw(14) << m_pb
                << std::setw(11) << std::fixed << std::setprecision(3) << block_ratio
                << std::setw(14) << m_fi
                << std::setw(14) << m_pi
                << std::setw(14) << m_fr
                << std::setw(14) << m_pr
                << (match ? "OK" : "FAIL")
                << "\n";
      first = false;
    }
  }

  // ---- Per-window Prune Wall Time comparison across DBs ----
  std::cout << "\n=== PRUNE WALL TIME (μs, median) ===\n";
  std::cout << std::setw(28) << "Window";
  for (const auto& dl : db_labels) std::cout << std::setw(16) << dl;
  std::cout << std::setw(12) << "ST vs LV\n";
  for (const auto& wl : win_labels) {
    std::cout << std::setw(28) << wl;
    uint64_t st_val = 0, lv_val = 0;
    for (const auto& dl : db_labels) {
      auto it = groups.find({dl, wl});
      if (it == groups.end()) { std::cout << std::setw(16) << "-"; continue; }
      std::vector<uint64_t> vals;
      for (const auto& r : it->second) vals.push_back(r.prune.wall_us);
      uint64_t med = Median(vals);
      std::cout << std::setw(16) << med;
      if (dl == "ST") st_val = med;
      if (dl == "Leveled") lv_val = med;
    }
    if (st_val > 0 && lv_val > 0) {
      double delta = (static_cast<double>(lv_val) - st_val) / lv_val * 100;
      std::cout << std::setw(10) << std::fixed << std::setprecision(1)
                << (delta > 0 ? "ST+" : "ST") << std::abs(delta) << "%";
    }
    std::cout << "\n";
  }

  // ---- Per-window Prune IO comparison ----
  std::cout << "\n=== PRUNE IO BYTES (median) ===\n";
  std::cout << std::setw(28) << "Window";
  for (const auto& dl : db_labels) std::cout << std::setw(16) << dl;
  std::cout << "\n";
  for (const auto& wl : win_labels) {
    std::cout << std::setw(28) << wl;
    for (const auto& dl : db_labels) {
      auto it = groups.find({dl, wl});
      if (it == groups.end()) { std::cout << std::setw(16) << "-"; continue; }
      std::vector<uint64_t> vals;
      for (const auto& r : it->second) vals.push_back(r.prune.io_bytes_read);
      std::cout << std::setw(16) << Median(vals);
    }
    std::cout << "\n";
  }

  // ---- Correctness ----
  std::cout << "\n=== CORRECTNESS ===\n";
  bool all_ok = true;
  for (const auto& r : results) {
    if (r.full.result_count != r.prune.result_count) {
      all_ok = false;
      std::cout << "  MISMATCH: " << r.db_label << " / " << r.window_label
                << " iter=" << r.iter
                << " full=" << r.full.result_count
                << " prune=" << r.prune.result_count << "\n";
    }
  }
  if (all_ok) std::cout << "  All OK.\n";

  // ---- Average speedup by tier ----
  std::cout << "\n=== AVG SPEEDUP BY WINDOW TIER ===\n";
  for (const auto& dl : db_labels) {
    std::cout << dl << ": ";
    for (const auto& tier : {"narrow", "medium", "wide"}) {
      double sum = 0; int cnt = 0;
      for (const auto& wl : win_labels) {
        if (wl.find(tier) == std::string::npos) continue;
        auto it = groups.find({dl, wl});
        if (it == groups.end()) continue;
        std::vector<uint64_t> fw, pw;
        for (const auto& r : it->second) {
          fw.push_back(r.full.wall_us);
          pw.push_back(r.prune.wall_us);
        }
        uint64_t mf = Median(fw), mp = Median(pw);
        if (mf > 0) { sum += static_cast<double>(mf) / std::max(uint64_t{1}, mp); ++cnt; }
      }
      if (cnt > 0)
        std::cout << tier << "=" << std::fixed << std::setprecision(1)
                  << (sum / cnt) << "x ";
    }
    std::cout << "\n";
  }

  std::cout.flush();
}

}  // namespace

int main(int argc, char** argv) {
  std::string db_st, db_leveled, db_tiered;
  std::string windows_csv;
  int block_cache_mb = 8;
  int repeat = 1;
  bool skip_full = false;
  bool skip_prune = false;

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (std::strcmp(a, "--db-st") == 0 && i + 1 < argc) {
      db_st = argv[++i];
    } else if (std::strcmp(a, "--db-leveled") == 0 && i + 1 < argc) {
      db_leveled = argv[++i];
    } else if (std::strcmp(a, "--db-tiered") == 0 && i + 1 < argc) {
      db_tiered = argv[++i];
    } else if (std::strcmp(a, "--windows-csv") == 0 && i + 1 < argc) {
      windows_csv = argv[++i];
    } else if (std::strcmp(a, "--block-cache-mb") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const long v = std::strtol(argv[++i], &e, 10);
      if (e != argv[i] && *e == '\0' && v >= 1 && v <= 65536)
        block_cache_mb = static_cast<int>(v);
    } else if (std::strcmp(a, "--repeat") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const long v = std::strtol(argv[++i], &e, 10);
      if (e != argv[i] && *e == '\0' && v >= 1 && v <= 20)
        repeat = static_cast<int>(v);
    } else if (std::strcmp(a, "--skip-full") == 0) {
      skip_full = true;
    } else if (std::strcmp(a, "--skip-prune") == 0) {
      skip_prune = true;
    } else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
      std::cout
          << "st_compaction_bench — compare ST / Leveled / Tiered compaction\n"
             "  --db-st <path>          ST-compacted DB\n"
             "  --db-leveled <path>     Leveled-compacted DB\n"
             "  --db-tiered <path>      Tiered/Universal-compacted DB\n"
             "  --windows-csv <path>    Query windows CSV\n"
             "  --block-cache-mb N      Block cache size (default 8)\n"
             "  --repeat N              Repeat each query N times (default 1)\n"
             "  --skip-full             Skip full scan (prune only)\n"
             "  --skip-prune            Skip prune scan (full scan only)\n";
      return 0;
    } else {
      std::cerr << "Unknown: " << a << "\n";
      return 1;
    }
  }

  if (db_st.empty() || db_leveled.empty() || db_tiered.empty() || windows_csv.empty()) {
    std::cerr << "Required: --db-st --db-leveled --db-tiered --windows-csv\n";
    return 1;
  }

  auto windows = LoadWindowsFromCsv(windows_csv);
  if (windows.empty()) {
    std::cerr << "No windows loaded from " << windows_csv << "\n";
    return 1;
  }
  std::cout << "Loaded " << windows.size() << " windows from " << windows_csv << "\n";

  struct DbEntry {
    std::string label;
    std::string path;
  };
  std::vector<DbEntry> dbs = {
      {"ST", db_st},
      {"Leveled", db_leveled},
      {"Tiered", db_tiered},
  };

  std::vector<DbResult> all_results;

  for (const auto& db_entry : dbs) {
    std::cout << "\n=== Opening DB: " << db_entry.label
              << " (" << db_entry.path << ") ===\n";

    // Open fresh for each DB
    rocksdb::Options opt = OpenOptions(block_cache_mb);
    std::unique_ptr<rocksdb::DB> db;
    rocksdb::Status s = rocksdb::DB::OpenForReadOnly(opt, db_entry.path, &db);
    if (!s.ok()) {
      std::cerr << "ERROR opening " << db_entry.label << ": " << s.ToString() << "\n";
      // Still report what we can
      continue;
    }

    // Report SST layout
    std::vector<rocksdb::LiveFileMetaData> files;
    db->GetLiveFilesMetaData(&files);
    std::cout << "  Live SSTs: " << files.size() << "\n";
    for (const auto& f : files) {
      std::cout << "    L" << f.level << " " << f.name
                << " size=" << (f.size / 1048576) << "MB"
                << " entries=" << f.num_entries << "\n";
    }

    for (const auto& w : windows) {
      for (int iter = 0; iter < repeat; ++iter) {
        std::cout << "  Window: " << w.label
                  << " t=[" << w.t_min << "," << w.t_max << "]"
                  << " iter=" << (iter + 1) << "/" << repeat
                  << "\n";

        DbResult r;
        r.db_label = db_entry.label;
        r.window_label = w.label;
        r.iter = iter;

        if (!skip_full) {
          r.full = RunFullScan(db.get(), w);
          std::cout << "    full_scan: wall=" << r.full.wall_us << "us"
                    << " blocks=" << r.full.block_read_count
                    << " io_bytes=" << r.full.io_bytes_read
                    << " keys=" << r.full.keys_scanned
                    << " results=" << r.full.result_count
                    << (r.full.ok ? "" : " ERROR")
                    << "\n";
        }

        if (!skip_prune) {
          r.prune = RunPruneScan(db.get(), w);
          std::cout << "    prune_scan: wall=" << r.prune.wall_us << "us"
                    << " blocks=" << r.prune.block_read_count
                    << " io_bytes=" << r.prune.io_bytes_read
                    << " keys=" << r.prune.keys_scanned
                    << " results=" << r.prune.result_count
                    << (r.prune.ok ? "" : " ERROR")
                    << "\n";

          if (!skip_full && r.full.ok && r.prune.ok) {
            if (r.full.result_count != r.prune.result_count) {
              std::cerr << "    *** CORRECTNESS MISMATCH: full=" << r.full.result_count
                        << " prune=" << r.prune.result_count << " ***\n";
            }
          }
        }

        all_results.push_back(r);
      }
    }
  }

  PrintResults(all_results);

  return 0;
}

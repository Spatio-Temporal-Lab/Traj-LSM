// Mixed read/write throughput benchmark across compaction strategies.
// Prep phase builds differentiated DB structures. Then varies write ratio
// from 0%→100%, measures per-operation latency. Supports --repeat for noise reduction.
//
// Usage:
//   st_mixed_workload_bench \
//     --db-st       D:/Project/data/db/tdrive_st_st \
//     --db-leveled  D:/Project/data/db/tdrive_st_leveled \
//     --db-tiered   D:/Project/data/db/tdrive_st_tiered \
//     --windows-csv D:/Project/data/processed/unified/unified_stratified12_st_windows.csv \
//     --ops-per-point 1000 --repeat 5 --prep-entries 20000

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rocksdb/db.h>
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
#include "table/st_meta_user_key.h"

namespace {

struct QueryWindow {
  std::string label;
  uint32_t t_min = 0, t_max = 0;
  float x_min = 0, x_max = 0, y_min = 0, y_max = 0;
};

std::vector<QueryWindow> LoadWindows(const std::string& path) {
  std::vector<QueryWindow> out;
  std::ifstream f(path);
  if (!f.is_open()) return out;
  std::string line;
  std::getline(f, line);
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    std::vector<std::string> cols;
    std::istringstream ss(line);
    std::string col;
    while (std::getline(ss, col, ',')) cols.push_back(col);
    if (cols.size() < 8) continue;
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

std::string RandomE7Key(std::mt19937_64& rng, uint32_t bucket_sec,
                        uint64_t& out_vehicle_id) {
  static std::atomic<uint64_t> g_seq{1};
  const uint64_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
  uint32_t t_start;
  if (std::uniform_int_distribution<int>(0, 1)(rng) == 0) {
    t_start = 1201930000u + std::uniform_int_distribution<uint32_t>(0, 120000)(rng);
  } else {
    t_start = 1406851200u + std::uniform_int_distribution<uint32_t>(0, 43200)(rng);
  }
  const uint32_t bucket_id = t_start / bucket_sec;
  const uint32_t t_start_offset = t_start - bucket_id * bucket_sec;
  const uint64_t zorder = std::uniform_int_distribution<uint64_t>(
      0, (uint64_t{1} << 48) - 1)(rng);
  const uint64_t vehicle_id = 20000000000000ULL + seq;
  const uint32_t segment_seq = static_cast<uint32_t>(seq % 100000);
  out_vehicle_id = vehicle_id;
  return StSegmentKeyTzidRaw(bucket_id, zorder, t_start_offset,
                             vehicle_id, segment_seq, 0, 0);
}

std::string RandomSegmentValue(std::mt19937_64& rng, uint32_t t_start) {
  segval::SegmentValueHeader hdr{};
  hdr.t_start = t_start;
  hdr.t_end = t_start + std::uniform_int_distribution<uint32_t>(60, 3600)(rng);
  hdr.point_count = static_cast<uint16_t>(
      std::uniform_int_distribution<int>(50, 200)(rng));  // ~3-12KB values
  std::vector<segval::SegmentPoint> points;
  points.reserve(hdr.point_count);
  float x0 = 120.0f + std::uniform_real_distribution<float>(-0.5f, 0.8f)(rng);
  float y0 = 31.5f + std::uniform_real_distribution<float>(-0.5f, 0.5f)(rng);
  hdr.x_min = x0; hdr.x_max = x0; hdr.y_min = y0; hdr.y_max = y0;
  for (uint16_t i = 0; i < hdr.point_count; ++i) {
    segval::SegmentPoint p{};
    p.unix_s = t_start + static_cast<uint32_t>(i) * 60;
    p.lon = x0 + std::uniform_real_distribution<float>(-0.05f, 0.05f)(rng);
    p.lat = y0 + std::uniform_real_distribution<float>(-0.05f, 0.05f)(rng);
    hdr.x_min = std::min(hdr.x_min, p.lon);
    hdr.x_max = std::max(hdr.x_max, p.lon);
    hdr.y_min = std::min(hdr.y_min, p.lat);
    hdr.y_max = std::max(hdr.y_max, p.lat);
    points.push_back(p);
  }
  return segval::EncodeSegmentValueV2(hdr, points);
}

rocksdb::Options OpenOptions(int block_cache_mb, int level_bytes_mb) {
  rocksdb::Options o;
  o.create_if_missing = true;
  o.error_if_exists = false;
  o.statistics = rocksdb::CreateDBStatistics();
  o.info_log_level = rocksdb::InfoLogLevel::WARN_LEVEL;
  o.max_background_jobs = 4;
  o.max_bytes_for_level_base =
      static_cast<uint64_t>(std::max(1, level_bytes_mb)) * 1048576ull;
  o.level_compaction_dynamic_level_bytes = false;
  o.write_buffer_size = 64ULL << 20;  // 64MB memtable
  rocksdb::BlockBasedTableOptions tb;
  tb.experimental_spatio_temporal_meta_in_index_value = true;
  tb.block_cache = rocksdb::NewLRUCache(
      static_cast<size_t>(std::max(1, block_cache_mb)) * size_t{1048576});
  tb.cache_index_and_filter_blocks = true;
  o.table_factory.reset(rocksdb::NewBlockBasedTableFactory(tb));
  return o;
}

// Build an E7 seek key for the given bucket_id with all other fields zeroed.
// E7 key: [magic 0xE7][type 0x00][bucket_id u32 BE][zorder u64 BE]
//         [t_start_offset u32 BE][vehicle_id u64 BE][segment_seq u32 BE][chunk_seq u16 BE]
std::string BuildE7SeekKey(uint32_t bucket_id) {
  std::string key(32, '\0');
  key[0] = static_cast<char>(0xE7);
  key[1] = 0;
  key[2] = static_cast<char>((bucket_id >> 24) & 0xFF);
  key[3] = static_cast<char>((bucket_id >> 16) & 0xFF);
  key[4] = static_cast<char>((bucket_id >> 8) & 0xFF);
  key[5] = static_cast<char>(bucket_id & 0xFF);
  // zorder, t_start_offset, vehicle_id, segment_seq, chunk_seq all remain 0
  return key;
}

uint64_t RunOneQuery(rocksdb::Iterator* it, const QueryWindow& w,
                     uint32_t bucket_sec) {
  // Seek to bucket_id just before the query window (conservative look-back).
  uint32_t seek_bucket = (bucket_sec > 0 && w.t_min >= bucket_sec)
                             ? (w.t_min / bucket_sec) - 1
                             : 0;
  std::string seek_key = BuildE7SeekKey(seek_bucket);
  uint32_t max_bucket = (bucket_sec > 0) ? (w.t_max / bucket_sec) + 1 : UINT32_MAX;

  auto t0 = std::chrono::steady_clock::now();
  it->Seek(seek_key);
  while (it->Valid()) {
    if (it->key().size() >= 6) {
      const auto* d = reinterpret_cast<const uint8_t*>(it->key().data());
      uint32_t cur_bucket = (static_cast<uint32_t>(d[2]) << 24) |
                            (static_cast<uint32_t>(d[3]) << 16) |
                            (static_cast<uint32_t>(d[4]) << 8) |
                            static_cast<uint32_t>(d[5]);
      if (cur_bucket > max_bucket) break;
    }
    volatile auto v = it->value().size(); (void)v;
    it->Next();
  }
  auto t1 = std::chrono::steady_clock::now();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
}

// Pre-built iterator handle: created once per window, reused across reads.
struct WindowIterator {
  const QueryWindow* window;
  rocksdb::ReadOptions ro;
  std::unique_ptr<rocksdb::Iterator> it;
};

std::vector<WindowIterator> PrebuildIterators(
    rocksdb::DB* db, const std::vector<QueryWindow>& windows,
    uint32_t bucket_sec) {
  std::vector<WindowIterator> out;
  out.reserve(windows.size());
  for (const auto& w : windows) {
    WindowIterator wi;
    wi.window = &w;
    wi.ro.total_order_seek = true;
    wi.ro.fill_cache = false;
    wi.ro.experimental_st_prune_scan.enable = true;
    wi.ro.experimental_st_prune_scan.t_min = w.t_min;
    wi.ro.experimental_st_prune_scan.t_max = w.t_max;
    wi.ro.experimental_st_prune_scan.x_min = w.x_min;
    wi.ro.experimental_st_prune_scan.x_max = w.x_max;
    wi.ro.experimental_st_prune_scan.y_min = w.y_min;
    wi.ro.experimental_st_prune_scan.y_max = w.y_max;
    wi.ro.experimental_st_prune_scan.file_level_enable = true;
    wi.ro.experimental_st_prune_scan.block_level_enable = true;
    wi.ro.experimental_st_prune_scan.key_level_enable = true;
    wi.ro.experimental_st_prune_scan.event_time_bucket_width_sec =
        (bucket_sec > 0) ? bucket_sec : 3600u;
    wi.it.reset(db->NewIterator(wi.ro));
    out.push_back(std::move(wi));
  }
  return out;
}

struct RatioResult {
  int write_pct = 0;
  std::string db_label;
  int iter = 0;
  uint64_t write_ops = 0, read_ops = 0;
  uint64_t total_time_us = 0, write_time_us = 0, read_time_us = 0;
};

RatioResult RunRatioPoint(rocksdb::DB* db, const std::string& db_label,
                          const std::vector<QueryWindow>& windows,
                          const std::vector<WindowIterator>& prebuilt_iters,
                          int write_pct, int total_ops, uint32_t bucket_sec,
                          int iter) {
  RatioResult r;
  r.write_pct = write_pct;
  r.db_label = db_label;
  r.iter = iter;
  const int write_count = total_ops * write_pct / 100;
  const int read_count = total_ops - write_count;
  if (write_count == 0 && read_count == 0) return r;

  std::mt19937_64 rng(static_cast<uint64_t>(write_pct) * 12345 +
                      static_cast<uint64_t>(iter) * 999983 + 67890);

  struct WriteOp { std::string key; std::string value; };
  std::vector<WriteOp> writes;
  writes.reserve(write_count);
  for (int i = 0; i < write_count; ++i) {
    uint64_t vid = 0;
    std::string key = RandomE7Key(rng, bucket_sec, vid);
    uint32_t t_start = 1201930000u +
        std::uniform_int_distribution<uint32_t>(0, 120000)(rng);
    std::string value = RandomSegmentValue(rng, t_start);
    writes.push_back({std::move(key), std::move(value)});
  }

  std::vector<const QueryWindow*> reads;
  reads.reserve(read_count);
  for (int i = 0; i < read_count; ++i)
    reads.push_back(&windows[i % windows.size()]);

  struct Op { bool is_write; int idx; };
  std::vector<Op> ops;
  ops.reserve(write_count + read_count);
  for (int i = 0; i < write_count; ++i) ops.push_back({true, i});
  for (int i = 0; i < read_count; ++i) ops.push_back({false, i});
  std::shuffle(ops.begin(), ops.end(), rng);

  uint64_t write_time_us = 0, read_time_us = 0;
  rocksdb::WriteOptions wo;
  wo.disableWAL = false;

  auto t_total_start = std::chrono::steady_clock::now();
  for (const auto& op : ops) {
    if (op.is_write) {
      const auto& w = writes[op.idx];
      auto tw0 = std::chrono::steady_clock::now();
      rocksdb::Status s = db->Put(wo, w.key, w.value);
      auto tw1 = std::chrono::steady_clock::now();
      write_time_us += static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(tw1 - tw0).count());
      if (!s.ok()) std::cerr << "Put error: " << s.ToString() << "\n";
    } else {
      // Reuse pre-built iterator: pick the one matching this read's window.
      const auto* w = reads[op.idx];
      size_t wi_idx = op.idx % prebuilt_iters.size();
      auto tr0 = std::chrono::steady_clock::now();
      uint64_t qtime = RunOneQuery(prebuilt_iters[wi_idx].it.get(), *w,
                                   bucket_sec);
      auto tr1 = std::chrono::steady_clock::now();
      read_time_us += static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(tr1 - tr0).count());
      (void)qtime;
    }
  }
  auto t_total_end = std::chrono::steady_clock::now();
  r.total_time_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          t_total_end - t_total_start).count());
  r.write_ops = write_count;
  r.read_ops = read_count;
  r.write_time_us = write_time_us;
  r.read_time_us = read_time_us;
  return r;
}

// Write N entries in batches to build up level structure.
// Progress bar is printed to stderr so it's visible even when stdout is redirected.
void PrepPhase(rocksdb::DB* db, const std::string& label,
               int total_entries, uint32_t bucket_sec) {
  const int kBatchSize = 2000;
  std::mt19937_64 rng(static_cast<uint64_t>(label[0]) * 12345 + 67890);
  int written = 0;

  std::cerr << "  [" << label << "] Prep: " << total_entries << " entries\n";

  while (written < total_entries) {
    int batch = std::min(kBatchSize, total_entries - written);
    rocksdb::WriteBatch wb;
    for (int i = 0; i < batch; ++i) {
      uint64_t vid = 0;
      std::string key = RandomE7Key(rng, bucket_sec, vid);
      uint32_t t_start = 1201930000u +
          std::uniform_int_distribution<uint32_t>(0, 120000)(rng);
      std::string value = RandomSegmentValue(rng, t_start);
      wb.Put(key, value);
    }
    rocksdb::Status s = db->Write(rocksdb::WriteOptions(), &wb);
    if (!s.ok()) { std::cerr << "Prep write error: " << s.ToString() << "\n"; return; }
    written += batch;

    rocksdb::FlushOptions fo;
    fo.wait = true;
    s = db->Flush(fo);
    if (!s.ok()) { std::cerr << "Prep flush error: " << s.ToString() << "\n"; }

    // Progress bar to stderr every 2 batches
    if (written % (kBatchSize * 2) == 0 || written >= total_entries) {
      int pct = written * 100 / total_entries;
      int bar_width = 30;
      int filled = pct * bar_width / 100;
      std::cerr << "\r  [" << label << "] [";
      for (int b = 0; b < bar_width; ++b)
        std::cerr << (b < filled ? '=' : b == filled ? '>' : ' ');
      std::cerr << "] " << pct << "% (" << written << "/" << total_entries << ")"
                << std::flush;

      // Wait for L0 drain
      int l0_files = 0;
      int max_wait_ms = 15000, waited_ms = 0;
      while (waited_ms < max_wait_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        waited_ms += 500;
        std::vector<rocksdb::LiveFileMetaData> files;
        db->GetLiveFilesMetaData(&files);
        l0_files = 0;
        for (const auto& f : files) {
          if (f.level == 0) l0_files++;
        }
        if (l0_files <= 2) break;
      }
    }
  }
  std::cerr << "\n  [" << label << "] Prep complete: " << written << " entries\n";
}

void PrintSstLayout(rocksdb::DB* db, const std::string& label) {
  std::vector<rocksdb::LiveFileMetaData> files;
  db->GetLiveFilesMetaData(&files);
  // Count per level
  std::map<int, std::pair<int, uint64_t>> level_info;  // level -> (count, total_size)
  for (const auto& f : files) {
    level_info[f.level].first++;
    level_info[f.level].second += f.size;
  }
  std::cout << label << ": " << files.size() << " SSTs total\n";
  for (const auto& [L, info] : level_info) {
    std::cout << "  L" << L << ": " << info.first << " files, "
              << (info.second / 1048576) << "MB\n";
  }
}

// Median of a vector
template<typename T>
T Median(std::vector<T> v) {
  if (v.empty()) return T{};
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

template<typename T>
double Average(const std::vector<T>& v) {
  if (v.empty()) return 0;
  double sum = 0;
  for (const auto& x : v) sum += static_cast<double>(x);
  return sum / v.size();
}

void PrintResults(const std::vector<RatioResult>& all_results, int repeat,
                  bool use_average) {
  // Group by (write_pct, db) → compute median of per-op latencies
  struct Key { int pct; std::string db; };
  auto key_cmp = [](const Key& a, const Key& b) {
    if (a.pct != b.pct) return a.pct < b.pct;
    return a.db < b.db;
  };
  std::map<Key, std::vector<RatioResult>, decltype(key_cmp)> grouped(key_cmp);

  for (const auto& r : all_results) {
    Key k{r.write_pct, r.db_label};
    grouped[k].push_back(r);
  }

  // Collect sorted pcts and db labels
  std::vector<int> pcts;
  std::vector<std::string> labels;
  for (const auto& [k, _] : grouped) {
    if (std::find(pcts.begin(), pcts.end(), k.pct) == pcts.end())
      pcts.push_back(k.pct);
    if (std::find(labels.begin(), labels.end(), k.db) == labels.end())
      labels.push_back(k.db);
  }
  std::sort(pcts.begin(), pcts.end());

  // === Write throughput (ops/sec) ===
  std::cout << "\n=== WRITE THROUGHPUT (ops/s, "
            << (use_average ? "average" : "median") << " of " << repeat
            << " runs) ===\n";
  std::cout << std::setw(10) << "Write%";
  for (const auto& l : labels) std::cout << std::setw(16) << l;
  std::cout << "\n";
  for (int pct : pcts) {
    std::cout << std::setw(9) << (std::to_string(pct) + "%");
    for (const auto& l : labels) {
      Key k{pct, l};
      auto it = grouped.find(k);
      if (it == grouped.end()) { std::cout << std::setw(16) << "-"; continue; }
      std::vector<double> vals;
      for (const auto& r : it->second) {
        if (r.write_ops > 0 && r.write_time_us > 0)
          vals.push_back(1e6 * r.write_ops / r.write_time_us);
      }
      if (vals.empty()) { std::cout << std::setw(16) << "-"; continue; }
      std::cout << std::setw(15) << std::fixed << std::setprecision(1)
                << Median(vals);
    }
    std::cout << "\n";
  }

  // === Read throughput (ops/sec) ===
  std::cout << "\n=== READ THROUGHPUT (ops/s, median of " << repeat << " runs) ===\n";
  std::cout << std::setw(10) << "Write%";
  for (const auto& l : labels) std::cout << std::setw(16) << l;
  std::cout << "\n";
  for (int pct : pcts) {
    std::cout << std::setw(9) << (std::to_string(pct) + "%");
    for (const auto& l : labels) {
      Key k{pct, l};
      auto it = grouped.find(k);
      if (it == grouped.end()) { std::cout << std::setw(16) << "-"; continue; }
      std::vector<double> vals;
      for (const auto& r : it->second) {
        if (r.read_ops > 0 && r.read_time_us > 0)
          vals.push_back(1e6 * r.read_ops / r.read_time_us);
      }
      if (vals.empty()) { std::cout << std::setw(16) << "-"; continue; }
      std::cout << std::setw(15) << std::fixed << std::setprecision(1)
                << Median(vals);
    }
    std::cout << "\n";
  }

  // === CSV for plotting ===
  std::cout << "\n=== CSV ===\n";
  std::cout << "write_pct,db,write_ops_per_sec,read_ops_per_sec\n";
  for (int pct : pcts) {
    for (const auto& l : labels) {
      Key k{pct, l};
      auto it = grouped.find(k);
      if (it == grouped.end()) continue;
      std::vector<double> w_vals, r_vals;
      for (const auto& r : it->second) {
        if (r.write_ops > 0 && r.write_time_us > 0)
          w_vals.push_back(1e6 * r.write_ops / r.write_time_us);
        if (r.read_ops > 0 && r.read_time_us > 0)
          r_vals.push_back(1e6 * r.read_ops / r.read_time_us);
      }
      double w_med = w_vals.empty() ? 0 : Median(w_vals);
      double r_med = r_vals.empty() ? 0 : Median(r_vals);
      std::cout << pct << "," << l << ","
                << std::fixed << std::setprecision(1) << w_med << ","
                << r_med << "\n";
    }
  }
  std::cout.flush();
}

}  // namespace

int main(int argc, char** argv) {
  std::string db_leveled, db_tiered, db_hybrid1, db_hybrid2, db_stfr;
  std::string windows_csv;
  int total_ops_per_point = 1000;
  int repeat = 1;
  int prep_entries = 0;
  uint32_t bucket_sec = 3600;
  int block_cache_mb = 64;
  int level_bytes_mb = 10;

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (std::strcmp(a, "--db-leveled") == 0 && i + 1 < argc)
      db_leveled = argv[++i];
    else if (std::strcmp(a, "--db-tiered") == 0 && i + 1 < argc)
      db_tiered = argv[++i];
    else if (std::strcmp(a, "--db-hybrid1") == 0 && i + 1 < argc)
      db_hybrid1 = argv[++i];
    else if (std::strcmp(a, "--db-hybrid2") == 0 && i + 1 < argc)
      db_hybrid2 = argv[++i];
    else if (std::strcmp(a, "--db-stfr") == 0 && i + 1 < argc)
      db_stfr = argv[++i];
    else if (std::strcmp(a, "--windows-csv") == 0 && i + 1 < argc)
      windows_csv = argv[++i];
    else if (std::strcmp(a, "--ops-per-point") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const long v = std::strtol(argv[++i], &e, 10);
      if (e != argv[i] && *e == '\0' && v >= 100 && v <= 1000000)
        total_ops_per_point = static_cast<int>(v);
    } else if (std::strcmp(a, "--repeat") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const long v = std::strtol(argv[++i], &e, 10);
      if (e != argv[i] && *e == '\0' && v >= 1 && v <= 20)
        repeat = static_cast<int>(v);
    } else if (std::strcmp(a, "--prep-entries") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const long v = std::strtol(argv[++i], &e, 10);
      if (e != argv[i] && *e == '\0' && v >= 0 && v <= 10000000)
        prep_entries = static_cast<int>(v);
    } else if (std::strcmp(a, "--bucket-sec") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const unsigned long v = std::strtoul(argv[++i], &e, 10);
      if (e != argv[i] && *e == '\0' && v > 0)
        bucket_sec = static_cast<uint32_t>(v);
    } else if (std::strcmp(a, "--block-cache-mb") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const long v = std::strtol(argv[++i], &e, 10);
      if (e != argv[i] && *e == '\0' && v >= 1)
        block_cache_mb = static_cast<int>(v);
    } else if (std::strcmp(a, "--level-bytes-mb") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const long v = std::strtol(argv[++i], &e, 10);
      if (e != argv[i] && *e == '\0' && v >= 1 && v <= 1024)
        level_bytes_mb = static_cast<int>(v);
    } else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
      std::cout
          << "st_mixed_workload_bench — mixed R/W throughput vs write ratio\n"
             "  --db-st / --db-leveled / --db-tiered <path>\n"
             "  --windows-csv <path>     Query windows CSV\n"
             "  --ops-per-point N        Total ops per ratio point (default 1000)\n"
             "  --repeat N               Repeat each ratio point N times (default 1)\n"
             "  --prep-entries N         Write N entries in batches before benchmark\n"
             "                           to build up level structure (default 0)\n"
             "  --bucket-sec N           E7 bucket width seconds (default 3600)\n"
             "  --block-cache-mb N       Block cache MB (default 64)\n";
      return 0;
    } else {
      std::cerr << "Unknown: " << a << "\n";
      return 1;
    }
  }

  int db_count = !db_leveled.empty() + !db_tiered.empty() +
                 !db_hybrid1.empty() + !db_hybrid2.empty() + !db_stfr.empty();
  if (db_count < 2 || windows_csv.empty()) {
    std::cerr << "Need at least 2 --db-* paths and --windows-csv\n";
    return 1;
  }

  auto windows = LoadWindows(windows_csv);
  if (windows.empty()) { std::cerr << "No windows loaded\n"; return 1; }
  std::cout << "Loaded " << windows.size() << " windows\n";

  struct DbConfig {
    std::string label, path;
    bool tiered_on, real_tiered;
    int tiered_max_level;
    int tiered_max_runs;
  };

  std::vector<DbConfig> db_configs;
  if (!db_leveled.empty()) db_configs.push_back({"Leveled", db_leveled, false, false, 0, 0});
  if (!db_tiered.empty())  db_configs.push_back({"Tiered",  db_tiered,  true,  true,  0, 0});
  if (!db_hybrid1.empty()) db_configs.push_back({"Hybrid1", db_hybrid1, true,  true,  1, 0});
  if (!db_hybrid2.empty()) db_configs.push_back({"Hybrid2", db_hybrid2, true,  true,  2, 0});
  if (!db_stfr.empty())    db_configs.push_back({"STFR",    db_stfr,    true,  true,  0, 6});

  struct DbInfo {
    std::string label, path;
    std::unique_ptr<rocksdb::DB> db;
  };
  std::vector<DbInfo> dbs;

  for (const auto& cfg : db_configs) {
    DbInfo info;
    info.label = cfg.label;
    info.path = cfg.path;
    rocksdb::Options opt = OpenOptions(block_cache_mb, level_bytes_mb);
    rocksdb::Status s = rocksdb::DB::Open(opt, cfg.path, &info.db);
    if (!s.ok()) {
      std::cerr << "ERROR opening " << cfg.label << ": " << s.ToString() << "\n";
      return 1;
    }
    // Apply strategy-specific compaction options
    std::unordered_map<std::string, std::string> sopts;
    sopts["disable_auto_compactions"] = "false";
    sopts["experimental_st_compaction"] = "true";
    if (cfg.tiered_on) {
      sopts["experimental_st_compaction_l1_l2_tiered"] = "true";
      if (cfg.real_tiered) {
        sopts["experimental_st_compaction_real_tiered_l1_l2"] = "true";
      }
      sopts["experimental_st_compaction_tiered_max_level"] =
          std::to_string(cfg.tiered_max_level);
      sopts["experimental_st_compaction_tiered_max_runs_per_level"] =
          std::to_string(cfg.tiered_max_runs);
    }
    info.db->SetOptions(sopts);
    std::cout << "  " << cfg.label << " opened, options applied\n";
    dbs.push_back(std::move(info));
  }

  // ── Prep phase: build differentiated level structures ──
  if (prep_entries > 0) {
    std::cout << "\n=== PREP PHASE: writing " << prep_entries
              << " entries to each DB ===\n";
    for (auto& dbinfo : dbs) {
      std::cout << "\n-- " << dbinfo.label << " --\n";
      PrintSstLayout(dbinfo.db.get(), "BEFORE prep");
      PrepPhase(dbinfo.db.get(), dbinfo.label, prep_entries, bucket_sec);
      PrintSstLayout(dbinfo.db.get(), "AFTER prep");
    }
  }

  // Show final layout before benchmark
  std::cout << "\n=== SST LAYOUT BEFORE BENCHMARK ===\n";
  for (auto& dbinfo : dbs) {
    PrintSstLayout(dbinfo.db.get(), dbinfo.label);
  }

  // ── Benchmark ──
  std::vector<int> ratio_points;
  for (int p = 0; p <= 100; p += 10) ratio_points.push_back(p);

  std::vector<RatioResult> all_results;

  for (auto& dbinfo : dbs) {
    std::cout << "\n=== " << dbinfo.label << " ===\n";
    // Pre-build iterators: one per window, reused across all reads.
    std::vector<WindowIterator> prebuilt =
        PrebuildIterators(dbinfo.db.get(), windows, bucket_sec);
    for (int wp : ratio_points) {
      for (int iter = 0; iter < repeat; ++iter) {
        std::cout << "  write=" << wp << "% iter=" << (iter + 1) << "/" << repeat
                  << "... " << std::flush;
        RatioResult r = RunRatioPoint(dbinfo.db.get(), dbinfo.label,
                                       windows, prebuilt, wp,
                                       total_ops_per_point, bucket_sec, iter);
        const double w_tp = r.write_ops > 0 && r.write_time_us > 0
            ? 1e6 * r.write_ops / r.write_time_us : 0;
        const double r_tp = r.read_ops > 0 && r.read_time_us > 0
            ? 1e6 * r.read_ops / r.read_time_us : 0;
        std::cout << "w_tp=" << std::fixed << std::setprecision(0) << w_tp
                  << "/s r_tp=" << std::setprecision(0) << r_tp << "/s\n";
        all_results.push_back(r);
      }
    }
  }

  PrintResults(all_results, repeat, /*use_average=*/false);
  std::cout << "\nDone.\n";
  return 0;
}

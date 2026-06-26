// Offline pruning effectiveness report for an ST DB:
//   1) M_manager manifest-shard filter (selected vs total)
//   2) Per-shard MANIFEST SST file-level filter (kept vs live)
// Optional: --scan runs a read iterator with experimental_st_prune_scan counters.
//
// Usage:
//   st_prune_stats --db D:/Project/data/my_db \
//     --t-min 1596200000 --t-max 1596203600 \
//     --x-min 120.02 --x-max 120.34 --y-min 31.52 --y-max 31.88
//
//   st_prune_stats --db ... --time-only --t-min ... --t-max ...
//   st_prune_stats --db ... --scan   # also print iterator-level prune counters

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/env.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>

#include "db/manifest_ops.h"

namespace {

bool ParseU32(const char* s, uint32_t* out) {
  if (s == nullptr || out == nullptr) {
    return false;
  }
  char* end = nullptr;
  unsigned long v = std::strtoul(s, &end, 10);
  if (end == s || *end != '\0' || v > 0xffffffffUL) {
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
  *out = std::strtof(s, &end);
  return end != s && *end == '\0';
}

void PrintUsage() {
  std::cerr
      << "st_prune_stats --db <path> --t-min <u32> --t-max <u32> [options]\n"
      << "  --x-min / --x-max / --y-min / --y-max   spatial window (omit with "
         "--time-only)\n"
      << "  --time-only          M_manager time filter only (no spatial MBR)\n"
      << "  --per-shard          print per selected MANIFEST shard SST stats\n"
      << "  --scan               also run DB iterator with prune counters\n"
      << "  --cf <name>          column family (default: default)\n";
}

void PrintRatioLine(const char* label, uint64_t kept, uint64_t total) {
  const uint64_t pruned = total > kept ? total - kept : 0;
  const double keep_pct =
      total > 0 ? (100.0 * static_cast<double>(kept) / static_cast<double>(total))
                : 0.0;
  const double prune_pct =
      total > 0 ? (100.0 * static_cast<double>(pruned) / static_cast<double>(total))
                : 0.0;
  std::cout << label << " kept=" << kept << " total=" << total << " pruned=" << pruned
            << " keep=" << std::fixed << std::setprecision(2) << keep_pct << "%"
            << " prune=" << prune_pct << "%\n";
}

void PrintSstLayer(const char* title, const rocksdb::ManifestSstPruneStats& s) {
  std::cout << "\n[" << title << "]\n";
  std::cout << "  live_sst_files=" << s.live_files << " with_st_meta=" << s.with_st_meta
            << " missing_st_meta=" << s.missing_st_meta
            << " range_del_blocked=" << s.range_del_blocked << "\n";
  PrintRatioLine("  file_level_prune", s.kept_intersect, s.live_files);
  std::cout << "    pruned_disjoint=" << s.pruned_disjoint << "\n";
}

void RunScanPass(rocksdb::DB* db, uint32_t t_min, uint32_t t_max, float x_min,
                 float x_max, float y_min, float y_max) {
  uint64_t files_skipped = 0;
  uint64_t files_considered = 0;
  uint64_t files_missing_meta = 0;
  uint64_t files_range_del = 0;
  uint64_t files_time_disjoint = 0;
  uint64_t files_space_disjoint = 0;
  uint64_t block_examined = 0;
  uint64_t block_skipped = 0;
  uint64_t keys_examined = 0;
  uint64_t keys_skipped = 0;

  rocksdb::ReadOptions ro;
  ro.total_order_seek = true;
  ro.fill_cache = false;
  auto& q = ro.experimental_st_prune_scan;
  q.enable = true;
  q.file_level_enable = true;
  q.block_level_enable = true;
  q.key_level_enable = true;
  q.t_min = t_min;
  q.t_max = t_max;
  q.x_min = x_min;
  q.x_max = x_max;
  q.y_min = y_min;
  q.y_max = y_max;
  q.file_level_files_skipped = &files_skipped;
  q.file_level_files_considered = &files_considered;
  q.file_level_files_missing_meta = &files_missing_meta;
  q.file_level_files_range_del_blocked = &files_range_del;
  q.file_level_files_time_disjoint = &files_time_disjoint;
  q.file_level_files_space_disjoint = &files_space_disjoint;
  q.block_level_index_entries_examined = &block_examined;
  q.block_level_index_entries_skipped_st_disjoint = &block_skipped;
  q.key_level_keys_examined = &keys_examined;
  q.key_level_keys_skipped_disjoint = &keys_skipped;

  std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro));
  uint64_t keys_seen = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ++keys_seen;
  }
  const rocksdb::Status st = it->status();
  if (!st.ok()) {
    std::cerr << "iterator status: " << st.ToString() << "\n";
    return;
  }

  std::cout << "\n[iterator_scan experimental_st_prune_scan]\n";
  PrintRatioLine("  file_level_skip", files_considered > files_skipped ? files_considered - files_skipped : 0,
                 files_considered);
  std::cout << "    files_skipped=" << files_skipped << " missing_meta=" << files_missing_meta
            << " range_del_blocked=" << files_range_del
            << " time_disjoint=" << files_time_disjoint
            << " space_disjoint=" << files_space_disjoint << "\n";
  if (block_examined > 0) {
    PrintRatioLine("  block_index_skip", block_examined - block_skipped, block_examined);
  }
  if (keys_examined > 0) {
    PrintRatioLine("  key_level_skip", keys_examined - keys_skipped, keys_examined);
  }
  std::cout << "  keys_returned=" << keys_seen << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string db_path;
  std::string cf_name = "default";
  bool time_only = false;
  bool per_shard = false;
  bool do_scan = false;
  uint32_t t_min = 0;
  uint32_t t_max = 0;
  float x_min = 0.0f;
  float x_max = 0.0f;
  float y_min = 0.0f;
  float y_max = 0.0f;
  bool have_t_min = false;
  bool have_t_max = false;

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (std::strcmp(a, "--db") == 0 && i + 1 < argc) {
      db_path = argv[++i];
      continue;
    }
    if (std::strcmp(a, "--cf") == 0 && i + 1 < argc) {
      cf_name = argv[++i];
      continue;
    }
    if (std::strcmp(a, "--time-only") == 0) {
      time_only = true;
      continue;
    }
    if (std::strcmp(a, "--per-shard") == 0) {
      per_shard = true;
      continue;
    }
    if (std::strcmp(a, "--scan") == 0) {
      do_scan = true;
      continue;
    }
    if (std::strcmp(a, "--t-min") == 0 && i + 1 < argc) {
      if (!ParseU32(argv[++i], &t_min)) {
        std::cerr << "Invalid --t-min\n";
        return 1;
      }
      have_t_min = true;
      continue;
    }
    if (std::strcmp(a, "--t-max") == 0 && i + 1 < argc) {
      if (!ParseU32(argv[++i], &t_max)) {
        std::cerr << "Invalid --t-max\n";
        return 1;
      }
      have_t_max = true;
      continue;
    }
    if (std::strcmp(a, "--x-min") == 0 && i + 1 < argc) {
      if (!ParseFloat(argv[++i], &x_min)) {
        std::cerr << "Invalid --x-min\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--x-max") == 0 && i + 1 < argc) {
      if (!ParseFloat(argv[++i], &x_max)) {
        std::cerr << "Invalid --x-max\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--y-min") == 0 && i + 1 < argc) {
      if (!ParseFloat(argv[++i], &y_min)) {
        std::cerr << "Invalid --y-min\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--y-max") == 0 && i + 1 < argc) {
      if (!ParseFloat(argv[++i], &y_max)) {
        std::cerr << "Invalid --y-max\n";
        return 1;
      }
      continue;
    }
    std::cerr << "Unknown arg: " << a << "\n";
    PrintUsage();
    return 1;
  }

  if (db_path.empty() || !have_t_min || !have_t_max) {
    PrintUsage();
    return 1;
  }
  if (t_max < t_min) {
    std::cerr << "--t-max must be >= --t-min\n";
    return 1;
  }
  if (!time_only && (x_max < x_min || y_max < y_min)) {
    std::cerr << "spatial max must be >= min (or use --time-only)\n";
    return 1;
  }

  const bool enable_spatial = !time_only;
  if (time_only) {
    x_min = -std::numeric_limits<float>::max();
    x_max = std::numeric_limits<float>::max();
    y_min = -std::numeric_limits<float>::max();
    y_max = std::numeric_limits<float>::max();
  }

  rocksdb::FileSystem* fs = rocksdb::Env::Default()->GetFileSystem().get();
  rocksdb::StPruneLayerStats layer;
  std::vector<rocksdb::ManifestSstPruneStats> per_shard_stats;
  const rocksdb::Status s = rocksdb::GetStPruneLayerStats(
      db_path, fs, /*is_retry=*/false, t_min, t_max, enable_spatial, x_min, x_max,
      y_min, y_max, std::numeric_limits<uint32_t>::max(), &layer,
      per_shard ? &per_shard_stats : nullptr);
  if (!s.ok()) {
    std::cerr << "GetStPruneLayerStats: " << s.ToString() << "\n";
    return 1;
  }

  std::cout << "db=" << db_path << " query_t=[" << t_min << "," << t_max << "]";
  if (enable_spatial) {
    std::cout << " query_xy x=[" << x_min << "," << x_max << "] y=[" << y_min << ","
              << y_max << "]";
  } else {
    std::cout << " spatial=off(time-only)";
  }
  std::cout << " m_manager_generation=" << layer.m_manager_generation << "\n";

  std::cout << "\n[M_manager manifest shard filter]\n";
  PrintRatioLine("  shards", layer.manifest_shard_selected,
                 layer.manifest_shard_total);

  PrintSstLayer("all_manifest_shards SST file-level", layer.sst_all_shards);
  PrintSstLayer("selected_shards SST file-level (after M_manager)",
                layer.sst_selected_shards);

  const uint64_t all_live = layer.sst_all_shards.live_files;
  const uint64_t after_mgr =
      layer.manifest_shard_selected > 0 ? layer.sst_selected_shards.live_files : 0;
  const uint64_t after_file_prune = layer.sst_selected_shards.kept_intersect;
  std::cout << "\n[combined funnel]\n";
  PrintRatioLine("  after_M_manager_sst_live", after_mgr, all_live);
  PrintRatioLine("  after_file_meta_prune", after_file_prune, all_live);

  if (per_shard && !per_shard_stats.empty()) {
    std::cout << "\n[per selected shard]\n";
    for (size_t i = 0; i < per_shard_stats.size(); ++i) {
      const auto& sh = per_shard_stats[i];
      std::cout << "  shard[" << i << "] live=" << sh.live_files
                << " kept=" << sh.kept_intersect
                << " pruned_disjoint=" << sh.pruned_disjoint << "\n";
    }
  }

  if (do_scan) {
    rocksdb::Options options;
    options.create_if_missing = false;
    std::vector<rocksdb::ColumnFamilyDescriptor> cfs = {
        {cf_name, rocksdb::ColumnFamilyOptions()}};
    std::vector<rocksdb::ColumnFamilyHandle*> handles;
    std::unique_ptr<rocksdb::DB> db;
    const rocksdb::Status os = rocksdb::DB::OpenForReadOnly(
        options, db_path, cfs, &handles, &db,
        /*error_if_wal_file_exists=*/false);
    if (!os.ok()) {
      std::cerr << "DB::OpenForReadOnly: " << os.ToString() << "\n";
      return 1;
    }
    RunScanPass(db.get(), t_min, t_max, x_min, x_max, y_min, y_max);
    for (auto* h : handles) {
      db->DestroyColumnFamilyHandle(h);
    }
  }

  return 0;
}

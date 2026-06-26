// Inspect CURRENT -> M_manager (or legacy MANIFEST_SET): per-shard time bounds
// as stored on disk, plus optional query-window aggregation.
//
// Usage:
//   st_manifest_set_inspect --db D:/Project/data/my_db
//   st_manifest_set_inspect --db D:/path --query-window \
//     --t-min 1596200000 --t-max 1596203600 \
//     --x-min 120.02 --x-max 120.34 --y-min 31.52 --y-max 31.88

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <rocksdb/env.h>

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
      << "st_manifest_set_inspect --db <path> [options]\n"
      << "  --query-window          Also print filtered global counts (time + MBR)\n"
      << "  --t-min / --t-max       uint32 (required with --query-window)\n"
      << "  --x-min / --x-max       float (required with --query-window)\n"
      << "  --y-min / --y-max       float (required with --query-window)\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string db_path;
  bool query_window = false;
  uint32_t q_t_min = 0;
  uint32_t q_t_max = 0;
  float q_x_min = 0.0f;
  float q_x_max = 0.0f;
  float q_y_min = 0.0f;
  float q_y_max = 0.0f;

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (std::strcmp(a, "--db") == 0 && i + 1 < argc) {
      db_path = argv[++i];
      continue;
    }
    if (std::strcmp(a, "--query-window") == 0) {
      query_window = true;
      continue;
    }
    if (std::strcmp(a, "--t-min") == 0 && i + 1 < argc) {
      if (!ParseU32(argv[++i], &q_t_min)) {
        std::cerr << "Invalid --t-min\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--t-max") == 0 && i + 1 < argc) {
      if (!ParseU32(argv[++i], &q_t_max)) {
        std::cerr << "Invalid --t-max\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--x-min") == 0 && i + 1 < argc) {
      if (!ParseFloat(argv[++i], &q_x_min)) {
        std::cerr << "Invalid --x-min\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--x-max") == 0 && i + 1 < argc) {
      if (!ParseFloat(argv[++i], &q_x_max)) {
        std::cerr << "Invalid --x-max\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--y-min") == 0 && i + 1 < argc) {
      if (!ParseFloat(argv[++i], &q_y_min)) {
        std::cerr << "Invalid --y-min\n";
        return 1;
      }
      continue;
    }
    if (std::strcmp(a, "--y-max") == 0 && i + 1 < argc) {
      if (!ParseFloat(argv[++i], &q_y_max)) {
        std::cerr << "Invalid --y-max\n";
        return 1;
      }
      continue;
    }
    std::cerr << "Unknown arg: " << a << "\n";
    PrintUsage();
    return 1;
  }

  if (db_path.empty()) {
    PrintUsage();
    return 1;
  }

  if (query_window && (q_t_max < q_t_min)) {
    std::cerr << "--t-max must be >= --t-min\n";
    return 1;
  }

  rocksdb::FileSystem* fs = rocksdb::Env::Default()->GetFileSystem().get();
  std::vector<std::string> paths;
  std::vector<uint64_t> numbers;
  uint64_t generation = 0;
  std::vector<uint64_t> tmins;
  std::vector<uint64_t> tmaxs;
  std::vector<float> xm;
  std::vector<float> xM;
  std::vector<float> ym;
  std::vector<float> yM;
  std::vector<uint64_t> masters;
  std::vector<uint64_t> ghosts;

  const rocksdb::Status s = rocksdb::GetCurrentManifestPaths(
      db_path, fs, /*is_retry=*/false, &paths, &numbers, &generation, &tmins,
      &tmaxs, &xm, &xM, &ym, &yM, &masters, &ghosts);
  if (!s.ok()) {
    std::cerr << "GetCurrentManifestPaths: " << s.ToString() << "\n";
    return 1;
  }

  std::cout << "M_manager generation=" << generation
            << " shard_count=" << paths.size() << "\n";

  uint64_t sum_m = 0;
  uint64_t sum_g = 0;
  for (size_t i = 0; i < paths.size(); ++i) {
    const uint64_t m = i < masters.size() ? masters[i] : 0;
    const uint64_t g = i < ghosts.size() ? ghosts[i] : 0;
    sum_m += m;
    sum_g += g;
    const uint64_t t0 = i < tmins.size() ? tmins[i] : 0;
    const uint64_t t1 = i < tmaxs.size() ? tmaxs[i] : 0;
    const float xx0 = i < xm.size() ? xm[i] : std::numeric_limits<float>::quiet_NaN();
    const float xx1 = i < xM.size() ? xM[i] : std::numeric_limits<float>::quiet_NaN();
    const float yy0 = i < ym.size() ? ym[i] : std::numeric_limits<float>::quiet_NaN();
    const float yy1 = i < yM.size() ? yM[i] : std::numeric_limits<float>::quiet_NaN();
    std::cout << "  shard[" << i << "] file_number=" << numbers[i]
              << " masters=" << m << " ghosts=" << g << " t=[" << t0 << "," << t1
              << "]";
    if (!std::isnan(xx0) && !std::isnan(xx1) && !std::isnan(yy0) &&
        !std::isnan(yy1)) {
      std::cout << " mbr x=[" << xx0 << "," << xx1 << "] y=[" << yy0 << ","
                << yy1 << "]";
    } else {
      std::cout << " mbr=nan";
    }
    std::cout << " path=" << paths[i] << "\n";
  }
  std::cout << "unfiltered_sum masters=" << sum_m << " ghosts=" << sum_g << "\n";

  if (query_window) {
    std::vector<size_t> selected;
    rocksdb::FilterManifestShardIndicesByTimeRange(
        static_cast<uint64_t>(q_t_min), static_cast<uint64_t>(q_t_max), tmins,
        tmaxs, &selected, &xm, &xM, &ym, &yM,
        /*enable_spatial_window=*/true, q_x_min, q_x_max, q_y_min, q_y_max);
    std::cout << "filter_selected_shard_count=" << selected.size();
    if (!selected.empty()) {
      std::cout << " indices=";
      for (size_t k = 0; k < selected.size(); ++k) {
        std::cout << (k == 0 ? "" : ",") << selected[k];
      }
    }
    std::cout << "\n";
    uint64_t sum_selected_m = 0;
    uint64_t sum_selected_g = 0;
    for (size_t idx : selected) {
      if (idx < masters.size()) {
        sum_selected_m += masters[idx];
      }
      if (idx < ghosts.size()) {
        sum_selected_g += ghosts[idx];
      }
    }
    std::cout << "filter_sum_from_selected_shards masters=" << sum_selected_m
              << " ghosts=" << sum_selected_g
              << " (should match API below; 0 often means time/MBR disjoint "
                 "or stored counters are 0)\n";

    uint64_t fil_m = 0;
    uint64_t fil_g = 0;
    uint64_t gen2 = 0;
    const rocksdb::Status qs = rocksdb::GetCurrentManifestGlobalCountsFilteredByTimeRange(
        db_path, fs, /*is_retry=*/false, static_cast<uint64_t>(q_t_min),
        static_cast<uint64_t>(q_t_max), &fil_m, &fil_g, &gen2,
        /*enable_spatial_window=*/true, q_x_min, q_x_max, q_y_min, q_y_max);
    if (!qs.ok()) {
      std::cerr << "GetCurrentManifestGlobalCountsFilteredByTimeRange: "
                << qs.ToString() << "\n";
      return 1;
    }
    std::cout << "filtered_by_window t=[" << q_t_min << "," << q_t_max << "] x=["
              << q_x_min << "," << q_x_max << "] y=[" << q_y_min << "," << q_y_max
              << "] generation=" << gen2 << " masters=" << fil_m
              << " ghosts=" << fil_g << "\n";

    rocksdb::StPruneLayerStats layer;
    const rocksdb::Status ps = rocksdb::GetStPruneLayerStats(
        db_path, fs, /*is_retry=*/false, q_t_min, q_t_max,
        /*enable_spatial_window=*/true, q_x_min, q_x_max, q_y_min, q_y_max,
        std::numeric_limits<uint32_t>::max(), &layer);
    if (!ps.ok()) {
      std::cerr << "GetStPruneLayerStats: " << ps.ToString() << "\n";
      return 1;
    }
    std::cout << "prune_summary shards_selected=" << layer.manifest_shard_selected
              << " shards_total=" << layer.manifest_shard_total
              << " sst_live_all_shards=" << layer.sst_all_shards.live_files
              << " sst_live_selected=" << layer.sst_selected_shards.live_files
              << " sst_kept_after_file_prune="
              << layer.sst_selected_shards.kept_intersect
              << " sst_pruned_disjoint="
              << layer.sst_selected_shards.pruned_disjoint << "\n";
    std::cout << "(full report: st_prune_stats --db ... --t-min ... --t-max ... "
                 "--x-min ... --scan)\n";
  }

  return 0;
}

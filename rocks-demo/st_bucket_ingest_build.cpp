#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>

#include "segment_value_codec.hpp"
#include "st_segment_key.hpp"
#include "trajectory_kv.hpp"

namespace fs = std::filesystem;

namespace {

static std::vector<std::string> SplitAuto(const std::string& line) {
  if (line.find('\t') != std::string::npos) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
      if (c == '\t') {
        out.push_back(cur);
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    out.push_back(cur);
    return out;
  }
  return trajkv::SplitCsvLine(line);
}

rocksdb::Options MakeDbOptions(bool create_if_missing,
                               bool disable_auto_compactions,
                               bool experimental_global_master_ghost,
                               bool experimental_global_verify_ghost) {
  rocksdb::Options o;
  o.create_if_missing = create_if_missing;
  o.disable_auto_compactions = disable_auto_compactions;
  o.experimental_global_master_ghost = experimental_global_master_ghost;
  o.experimental_global_verify_ghost = experimental_global_verify_ghost;
  rocksdb::BlockBasedTableOptions tb;
  tb.experimental_spatio_temporal_meta_in_index_value = true;
  o.table_factory.reset(rocksdb::NewBlockBasedTableFactory(tb));
  return o;
}

struct Kv {
  std::string key;
  std::string val;
};

std::string EncodeSegmentValueForGlobalMode(
    const segval::SegmentValueHeader& head,
    const std::vector<segval::SegmentPoint>& points,
    bool experimental_global_master_ghost) {
  if (experimental_global_master_ghost) {
    return segval::EncodeSegmentValueV3(head, points, /*is_ghost=*/false,
                                        /*master_key=*/"");
  }
  return segval::EncodeSegmentValueV2(head, points);
}

}  // namespace

int main(int argc, char** argv) {
  std::string csv_path;
  std::string points_csv_path;
  std::string out_sst_dir;
  std::string target_db;
  uint32_t bucket_sec = 86400;
  bool reset_out_dir = false;
  bool reset_target_db = false;
  bool experimental_global_master_ghost = false;
  bool experimental_global_verify_ghost = false;
  int64_t max_rows = -1;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--segment-meta-csv" && i + 1 < argc) {
      csv_path = argv[++i];
    } else if (a == "--segment-points-csv" && i + 1 < argc) {
      points_csv_path = argv[++i];
    } else if (a == "--out-sst-dir" && i + 1 < argc) {
      out_sst_dir = argv[++i];
    } else if (a == "--target-db" && i + 1 < argc) {
      target_db = argv[++i];
    } else if (a == "--bucket-sec" && i + 1 < argc) {
      bucket_sec = static_cast<uint32_t>(std::stoul(argv[++i]));
    } else if (a == "--max-rows" && i + 1 < argc) {
      max_rows = std::stoll(argv[++i]);
    } else if (a == "--reset-out-sst-dir") {
      reset_out_dir = true;
    } else if (a == "--reset-target-db") {
      reset_target_db = true;
    } else if (a == "--experimental-global-master-ghost") {
      experimental_global_master_ghost = true;
    } else if (a == "--experimental-global-verify-ghost") {
      experimental_global_verify_ghost = true;
    } else if (a == "-h" || a == "--help") {
      std::cout
          << "st_bucket_ingest_build\n"
             "  --segment-meta-csv <segments_meta.csv>\n"
             "  --segment-points-csv <segments_points.csv>\n"
             "  --out-sst-dir <dir>\n"
             "  --target-db <empty_db_dir>\n"
             "  [--bucket-sec 86400]\n"
             "  [--max-rows -1]\n"
             "  [--reset-out-sst-dir]\n"
             "  [--reset-target-db]\n"
             "  [--experimental-global-master-ghost]\n"
             "  [--experimental-global-verify-ghost]\n";
      return 0;
    } else {
      std::cerr << "Unknown arg: " << a << "\n";
      return 1;
    }
  }

  if (csv_path.empty() || points_csv_path.empty() || out_sst_dir.empty() ||
      target_db.empty()) {
    std::cerr << "Required: --segment-meta-csv --segment-points-csv --out-sst-dir "
                 "--target-db\n";
    return 1;
  }
  if (bucket_sec == 0) {
    std::cerr << "--bucket-sec must be > 0\n";
    return 1;
  }
  if (experimental_global_verify_ghost && !experimental_global_master_ghost) {
    std::cerr << "--experimental-global-verify-ghost requires "
                 "--experimental-global-master-ghost\n";
    return 1;
  }

  if (reset_out_dir && fs::exists(out_sst_dir)) {
    fs::remove_all(out_sst_dir);
  }
  fs::create_directories(out_sst_dir);

  if (reset_target_db && fs::exists(target_db)) {
    fs::remove_all(target_db);
  }

  std::unordered_map<std::string, std::vector<segval::SegmentPoint>> by_segment;
  if (!segval::LoadSegmentPointsCsv(points_csv_path, &by_segment)) {
    return 1;
  }

  std::ifstream in(csv_path);
  if (!in) {
    std::cerr << "Cannot open: " << csv_path << "\n";
    return 1;
  }
  std::string header;
  if (!std::getline(in, header)) {
    std::cerr << "Empty csv.\n";
    return 1;
  }

  std::unordered_map<uint32_t, std::vector<Kv>> bucket_kvs;
  int64_t rows = 0;
  uint64_t id = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    if (max_rows >= 0 && rows >= max_rows) {
      break;
    }
    auto f = SplitAuto(line);
    if (f.size() < 9) {
      continue;
    }
    const uint32_t t_start = static_cast<uint32_t>(std::stoll(f[3]));
    const uint32_t t_end = static_cast<uint32_t>(std::stoll(f[4]));
    const float x_min = static_cast<float>(std::stod(f[5]));
    const float x_max = static_cast<float>(std::stod(f[6]));
    const float y_min = static_cast<float>(std::stod(f[7]));
    const float y_max = static_cast<float>(std::stod(f[8]));
    const uint32_t point_count = static_cast<uint32_t>(std::stoul(f[2]));
    const std::string segment_id = f[0];
    auto pit = by_segment.find(segment_id);
    if (pit == by_segment.end()) {
      std::cerr << "No points for segment_id=" << segment_id << "\n";
      return 1;
    }
    if (pit->second.size() != static_cast<size_t>(point_count)) {
      std::cerr << "point_count mismatch segment_id=" << segment_id << " meta="
                << point_count << " points=" << pit->second.size() << "\n";
      return 1;
    }
    const uint32_t b0 = t_start / bucket_sec;
    const uint32_t b1 = t_end / bucket_sec;

    segval::SegmentValueHeader head{};
    head.t_start = t_start;
    head.t_end = t_end;
    head.x_min = x_min;
    head.y_min = y_min;
    head.x_max = x_max;
    head.y_max = y_max;
    head.point_count = point_count;

    Kv kv;
    const uint64_t master_id = id++;
    kv.key = StSegmentKey(t_start, t_end, x_min, y_min, x_max, y_max, master_id);
    kv.val = EncodeSegmentValueForGlobalMode(
        head, pit->second, experimental_global_master_ghost);
    if (kv.val.empty()) {
      std::cerr << "EncodeSegmentValueForGlobalMode failed for segment_id="
                << segment_id
                << "\n";
      return 1;
    }
    const std::string master_key = kv.key;
    bucket_kvs[b0].push_back(std::move(kv));
    if (experimental_global_master_ghost && b1 > b0) {
      for (uint32_t ghost_bucket = b0 + 1; ghost_bucket <= b1; ++ghost_bucket) {
        Kv ghost_kv;
        ghost_kv.key = StSegmentGhostKey(t_start, t_end, x_min, y_min, x_max,
                                         y_max, master_id, ghost_bucket);
        ghost_kv.val = segval::EncodeGhostSegmentValueV3(head, master_key);
        if (ghost_kv.val.empty()) {
          std::cerr << "EncodeGhostSegmentValueV3 failed for segment_id="
                    << segment_id << " ghost_bucket=" << ghost_bucket << "\n";
          return 1;
        }
        bucket_kvs[ghost_bucket].push_back(std::move(ghost_kv));
      }
    }
    ++rows;
  }
  in.close();

  if (bucket_kvs.empty()) {
    std::cerr << "No rows parsed.\n";
    return 1;
  }

  std::vector<uint32_t> buckets;
  buckets.reserve(bucket_kvs.size());
  for (const auto& it : bucket_kvs) {
    buckets.push_back(it.first);
  }
  std::sort(buckets.begin(), buckets.end());

  // Write directly into target DB by bucket and flush once per bucket.
  // This follows the same metadata path as normal Put+Flush workloads, ensuring
  // file-level st_file_meta is materialized in VersionEdit.
  rocksdb::Options stage_opts =
      MakeDbOptions(true, true, experimental_global_master_ghost,
                    experimental_global_verify_ghost);
  std::unique_ptr<rocksdb::DB> stage_db;
  rocksdb::Status s = rocksdb::DB::Open(stage_opts, target_db, &stage_db);
  if (!s.ok()) {
    std::cerr << "Open target db failed: " << s.ToString() << "\n";
    return 1;
  }

  uint64_t total_entries = 0;
  uint64_t total_masters = 0;
  uint64_t total_ghosts = 0;
  for (uint32_t b : buckets) {
    auto& vec = bucket_kvs[b];
    std::sort(vec.begin(), vec.end(),
              [](const Kv& a, const Kv& c) { return a.key < c.key; });
    for (const auto& kv : vec) {
      s = stage_db->Put(rocksdb::WriteOptions(), kv.key, kv.val);
      if (!s.ok()) {
        std::cerr << "staging Put failed: " << s.ToString() << "\n";
        return 1;
      }
    }
    rocksdb::FlushOptions fo;
    fo.wait = true;
    s = stage_db->Flush(fo);
    if (!s.ok()) {
      std::cerr << "staging Flush failed: " << s.ToString() << "\n";
      return 1;
    }
    total_entries += static_cast<uint64_t>(vec.size());
    if (experimental_global_master_ghost) {
      for (const auto& kv : vec) {
        segval::DecodedSegmentValue decoded{};
        if (segval::DecodeSegmentValue(kv.val, &decoded) && decoded.is_ghost) {
          ++total_ghosts;
        } else {
          ++total_masters;
        }
      }
    } else {
      total_masters += static_cast<uint64_t>(vec.size());
    }
    std::cout << "Bucket " << b << " flushed entries=" << vec.size() << "\n";
  }
  stage_db.reset();
  rocksdb::Options db_opts =
      MakeDbOptions(false, true, experimental_global_master_ghost,
                    experimental_global_verify_ghost);
  std::unique_ptr<rocksdb::DB> db;
  s = rocksdb::DB::Open(db_opts, target_db, &db);
  if (!s.ok()) {
    std::cerr << "Reopen target db failed: " << s.ToString() << "\n";
    return 1;
  }
  std::vector<rocksdb::LiveFileMetaData> live;
  db->GetLiveFilesMetaData(&live);
  std::cout << "DONE buckets=" << buckets.size() << " rows=" << rows
            << " built_sst=" << buckets.size()
            << " ingested_live_sst=" << live.size()
            << " total_entries=" << total_entries
            << " masters=" << total_masters
            << " ghosts=" << total_ghosts << "\n";
  return 0;
}


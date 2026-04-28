// Offline compaction on an EXISTING ST-key DB (use a **copy** of precious data).
// After open: SetOptions(target_file_size_base) then CompactRange(full keyspace)
// on the default column family to increase SST count / tighten per-file bounds.
//
// Example:
//   robocopy D:\Project\data\verify_traj_st_full D:\Project\data\verify_traj_compact /E
//   st_meta_compact_existing.exe --db D:\Project\data\verify_traj_compact --target-file-mb 32

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/metadata.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>

namespace {

rocksdb::Options BenchCompatibleOpenOptions() {
  rocksdb::Options o;
  o.create_if_missing = false;
  // Ensure INFO-level diagnostics from compaction (e.g. ST_EVENT_SPLIT) are
  // visible in LOG. This tool is interactive/debug-oriented rather than perf
  // sensitive.
  o.info_log_level = rocksdb::InfoLogLevel::INFO_LEVEL;
  rocksdb::BlockBasedTableOptions tb;
  tb.experimental_spatio_temporal_meta_in_index_value = true;
  tb.block_cache = rocksdb::NewLRUCache(8ULL * 1048576);
  tb.cache_index_and_filter_blocks = true;
  o.table_factory.reset(rocksdb::NewBlockBasedTableFactory(tb));
  return o;
}

void ReportFiles(rocksdb::DB* db, const char* label) {
  std::vector<rocksdb::LiveFileMetaData> m;
  db->GetLiveFilesMetaData(&m);
  std::cout << label << " live_sst=" << m.size() << "\n";
  for (const auto& f : m) {
    std::cout << "  L" << f.level << " " << f.name
              << " num_entries=" << f.num_entries << "\n";
  }
}

void ProbeUserKeys(rocksdb::DB* db, size_t sample_limit = 4096) {
  rocksdb::ReadOptions ro;
  std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro));
  size_t seen = 0;
  size_t e6 = 0;
  size_t e5 = 0;
  size_t other = 0;
  uint32_t first_t = 0;
  uint32_t last_t = 0;
  bool have_t = false;
  for (it->SeekToFirst(); it->Valid() && seen < sample_limit; it->Next()) {
    const rocksdb::Slice k = it->key();
    if (k.size() == 0) {
      ++other;
      ++seen;
      continue;
    }
    const uint8_t magic = static_cast<uint8_t>(k.data()[0]);
    if (magic == 0xE6 && k.size() >= 5) {
      ++e6;
      const unsigned char* p =
          reinterpret_cast<const unsigned char*>(k.data() + 1);
      const uint32_t t = (static_cast<uint32_t>(p[0]) << 24) |
                         (static_cast<uint32_t>(p[1]) << 16) |
                         (static_cast<uint32_t>(p[2]) << 8) |
                         (static_cast<uint32_t>(p[3]));
      if (!have_t) {
        first_t = t;
        have_t = true;
      }
      last_t = t;
    } else if (magic == 0xE5) {
      ++e5;
    } else {
      ++other;
    }
    ++seen;
  }
  std::cout << "KEY_PROBE sample=" << seen << " e6=" << e6 << " e5=" << e5
            << " other=" << other;
  if (have_t) {
    std::cout << " e6_t_first=" << first_t << " e6_t_last=" << last_t;
  }
  std::cout << "\n";
}

void ReportStCompactionOptions(rocksdb::DB* db, const char* label) {
  if (db == nullptr) {
    return;
  }
  const rocksdb::Options o = db->GetOptions();
  std::cout << label
            << " st_event_time_split="
            << static_cast<int>(o.experimental_compaction_event_time_split)
            << " bucket_width=" << o.experimental_compaction_event_time_bucket_width
            << " min_keys_per_file="
            << o.experimental_compaction_event_time_min_keys_per_file
            << " max_span=" << o.experimental_compaction_event_time_max_span
            << " max_point_keys="
            << o.experimental_compaction_event_time_max_point_keys
            << " spatial_soft_ratio="
            << o.experimental_compaction_spatial_soft_expansion_ratio
            << " spatial_hard_ratio="
            << o.experimental_compaction_spatial_hard_expansion_ratio
            << " spatial_hard_min_file_size="
            << o.experimental_compaction_spatial_hard_min_file_size << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string db_path;
  int target_file_mb = 32;
  bool dry_run = false;
  bool st_time_split_enable = false;
  uint32_t st_bucket_sec = 0;
  uint32_t st_min_keys_per_file = 0;
  double st_spatial_soft_ratio = 0.0;
  double st_spatial_hard_ratio = 0.0;
  uint64_t st_spatial_hard_min_file_size = 0;
  bool read_repair_first = false;
  uint64_t read_repair_min_reads = 0;
  double read_repair_shape_weight = 1.0;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
      db_path = argv[++i];
      continue;
    }
    if (std::strcmp(argv[i], "--target-file-mb") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const long v = std::strtol(argv[++i], &e, 10);
      if (e == argv[i] || *e != '\0' || v < 1 || v > 2048) {
        std::cerr << "Invalid --target-file-mb (1..2048)\n";
        return 1;
      }
      target_file_mb = static_cast<int>(v);
      continue;
    }
    if (std::strcmp(argv[i], "--st-time-split") == 0) {
      st_time_split_enable = true;
      continue;
    }
    if (std::strcmp(argv[i], "--st-bucket-sec") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const unsigned long v = std::strtoul(argv[++i], &e, 10);
      if (e == argv[i] || *e != '\0' || v == 0 || v > 86400ul * 3650ul) {
        std::cerr << "Invalid --st-bucket-sec (1..315360000)\n";
        return 1;
      }
      st_bucket_sec = static_cast<uint32_t>(v);
      continue;
    }
    if (std::strcmp(argv[i], "--st-min-keys") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const unsigned long v = std::strtoul(argv[++i], &e, 10);
      if (e == argv[i] || *e != '\0' || v > 100000000ul) {
        std::cerr << "Invalid --st-min-keys (0..1e8)\n";
        return 1;
      }
      st_min_keys_per_file = static_cast<uint32_t>(v);
      continue;
    }
    if (std::strcmp(argv[i], "--dry-run") == 0) {
      dry_run = true;
      continue;
    }
    if (std::strcmp(argv[i], "--st-spatial-soft-ratio") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const double v = std::strtod(argv[++i], &e);
      if (e == argv[i] || *e != '\0' || v < 0.0) {
        std::cerr << "Invalid --st-spatial-soft-ratio (>=0)\n";
        return 1;
      }
      st_spatial_soft_ratio = v;
      continue;
    }
    if (std::strcmp(argv[i], "--st-spatial-hard-ratio") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const double v = std::strtod(argv[++i], &e);
      if (e == argv[i] || *e != '\0' || v < 0.0) {
        std::cerr << "Invalid --st-spatial-hard-ratio (>=0)\n";
        return 1;
      }
      st_spatial_hard_ratio = v;
      continue;
    }
    if (std::strcmp(argv[i], "--st-spatial-hard-min-file-mb") == 0 &&
        i + 1 < argc) {
      char* e = nullptr;
      const unsigned long long v = std::strtoull(argv[++i], &e, 10);
      if (e == argv[i] || *e != '\0' || v > 1024ull * 1024ull) {
        std::cerr << "Invalid --st-spatial-hard-min-file-mb\n";
        return 1;
      }
      st_spatial_hard_min_file_size = static_cast<uint64_t>(v) * 1048576ull;
      continue;
    }
    if (std::strcmp(argv[i], "--read-repair-first") == 0) {
      read_repair_first = true;
      continue;
    }
    if (std::strcmp(argv[i], "--read-repair-min-reads") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const unsigned long long v = std::strtoull(argv[++i], &e, 10);
      if (e == argv[i] || *e != '\0') {
        std::cerr << "Invalid --read-repair-min-reads\n";
        return 1;
      }
      read_repair_min_reads = static_cast<uint64_t>(v);
      continue;
    }
    if (std::strcmp(argv[i], "--read-repair-shape-weight") == 0 &&
        i + 1 < argc) {
      char* e = nullptr;
      const double v = std::strtod(argv[++i], &e);
      if (e == argv[i] || *e != '\0' || v < 0.0) {
        std::cerr << "Invalid --read-repair-shape-weight (>=0)\n";
        return 1;
      }
      read_repair_shape_weight = v;
      continue;
    }
    if (std::strcmp(argv[i], "-h") == 0 ||
        std::strcmp(argv[i], "--help") == 0) {
      std::cout
          << "st_meta_compact_existing --db <dir> [options]\n"
             "  Opens an existing DB (same ST table options as st_meta_read_bench).\n"
             "  **Copy the DB first** if the source directory is precious.\n"
             "  Lists live SSTs, sets target_file_size_base (default 32 MB), runs "
             "CompactRange on full keyspace.\n"
             "Options:\n"
             "  --target-file-mb N   passed to SetOptions before compact (default 32)\n"
             "  --st-time-split      enable ST event-time split during compaction\n"
             "  --st-bucket-sec W    split when floor(t_sec/W) changes (segment keys use t_start)\n"
             "  --st-min-keys K      require at least K ST keys in output before splitting on bucket change\n"
             "  --st-spatial-soft-ratio R  soft split trigger on MBR expansion ratio (>=0)\n"
             "  --st-spatial-hard-ratio R  hard split trigger on MBR expansion ratio (>=0)\n"
             "  --st-spatial-hard-min-file-mb M  hard split only if file size >= M MiB\n"
             "  --read-repair-first  enable read-repair-first file pick ordering\n"
             "  --read-repair-min-reads N  minimum sampled reads before read-repair scoring applies\n"
             "  --read-repair-shape-weight W  shape penalty weight for read-repair score\n"
             "  --dry-run            print live files only; no SetOptions/CompactRange\n";
      return 0;
    }
    std::cerr << "Unknown: " << argv[i] << "\n";
    return 1;
  }

  if (db_path.empty()) {
    std::cerr << "Required: --db <dir>\n";
    return 1;
  }

  rocksdb::Options opt = BenchCompatibleOpenOptions();
  std::unique_ptr<rocksdb::DB> db;
  rocksdb::Status s = rocksdb::DB::Open(opt, db_path, &db);
  if (!s.ok()) {
    std::cerr << "DB::Open: " << s.ToString() << "\n";
    return 1;
  }

  ReportFiles(db.get(), "BEFORE");
  ProbeUserKeys(db.get());
  ReportStCompactionOptions(db.get(), "BEFORE_OPTS");

  const uint64_t base_bytes =
      static_cast<uint64_t>(target_file_mb) * 1048576ull;
  std::unordered_map<std::string, std::string> cf_opts;
  cf_opts["target_file_size_base"] = std::to_string(base_bytes);
  if (st_time_split_enable || st_bucket_sec != 0 || st_min_keys_per_file != 0) {
    cf_opts["experimental_compaction_event_time_split"] = "true";
    if (st_bucket_sec != 0) {
      cf_opts["experimental_compaction_event_time_bucket_width"] =
          std::to_string(st_bucket_sec);
    }
    if (st_min_keys_per_file != 0) {
      cf_opts["experimental_compaction_event_time_min_keys_per_file"] =
          std::to_string(st_min_keys_per_file);
    }
  }
  if (st_spatial_soft_ratio > 0.0) {
    cf_opts["experimental_compaction_spatial_soft_expansion_ratio"] =
        std::to_string(st_spatial_soft_ratio);
  }
  if (st_spatial_hard_ratio > 0.0) {
    cf_opts["experimental_compaction_spatial_hard_expansion_ratio"] =
        std::to_string(st_spatial_hard_ratio);
  }
  if (st_spatial_hard_min_file_size > 0) {
    cf_opts["experimental_compaction_spatial_hard_min_file_size"] =
        std::to_string(st_spatial_hard_min_file_size);
  }
  if (read_repair_first) {
    cf_opts["experimental_compaction_read_repair_first"] = "true";
    cf_opts["experimental_compaction_read_repair_min_reads"] =
        std::to_string(read_repair_min_reads);
    cf_opts["experimental_compaction_read_repair_shape_weight"] =
        std::to_string(read_repair_shape_weight);
  }
  s = db->SetOptions(cf_opts);
  if (!s.ok()) {
    std::cerr << "SetOptions warning: " << s.ToString()
              << " (continuing with CompactRange)\n";
  } else {
    std::cout << "SetOptions target_file_size_base=" << base_bytes << " ok\n";
  }

  // NOTE: `DB::Open` does not automatically load mutable CF options from the
  // OPTIONS file. Reopening here would therefore drop the SetOptions() changes
  // and make the compaction behave as if event-time split was disabled.
  ReportStCompactionOptions(db.get(), "AFTER_SETOPTIONS_OPTS");
  if (dry_run) {
    // In dry-run mode we intentionally avoid CompactRange, but we still apply
    // SetOptions so callers can verify the effective mutable options without
    // rewriting SSTs.
    return 0;
  }

  rocksdb::CompactRangeOptions cro;
  // This DB is already fully in the bottommost level (common for offline-built
  // trajectory DBs). Without forcing bottommost-level compaction, CompactRange
  // can become a no-op and won't rewrite SSTs (thus no chance to apply
  // event-time split / target_file_size_base).
  cro.bottommost_level_compaction =
      rocksdb::BottommostLevelCompaction::kForce;
  std::cout << "CompactRange (full keyspace, may take a long time)...\n";
  std::cout.flush();
  s = db->CompactRange(cro, nullptr, nullptr);
  if (!s.ok()) {
    std::cerr << "CompactRange: " << s.ToString() << "\n";
    return 1;
  }

  ReportFiles(db.get(), "AFTER");
  std::cout
      << "Next: run st_meta_sst_diag.exe on each *.sst (or st_diag_align_read_bench "
         "after pointing SST paths / copy DB).\n";
  return 0;
}

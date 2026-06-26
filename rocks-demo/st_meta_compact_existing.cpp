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
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/metadata.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>

#include "segment_value_codec.hpp"

namespace {

constexpr uint8_t kE7TypeGhostBit = 0x01;

inline bool IsE7GhostByKeyType(const rocksdb::Slice& user_key) {
  if (user_key.size() < 32 ||
      static_cast<unsigned char>(user_key.data()[0]) != 0xE7) {
    return false;
  }
  return (static_cast<uint8_t>(user_key.data()[1]) & kE7TypeGhostBit) != 0;
}

std::string QuoteArg(const std::string& s) {
  if (s.find_first_of(" \t\"") == std::string::npos) {
    return s;
  }
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') {
      out += "\\\"";
    } else {
      out += c;
    }
  }
  out += "\"";
  return out;
}

std::string SiblingExePath(const char* argv0, const char* exe_name) {
  try {
    std::filesystem::path p = std::filesystem::absolute(argv0);
    return (p.parent_path() / exe_name).string();
  } catch (...) {
    return std::string(exe_name);
  }
}

int RunExternalCommand(const std::string& cmd) {
  std::cout << "[post-check] " << cmd << "\n";
  std::cout.flush();
  return std::system(cmd.c_str());
}

rocksdb::Options BenchCompatibleOpenOptions(
    rocksdb::CompactionStyle compaction_style =
        rocksdb::CompactionStyle::kCompactionStyleLevel,
    bool level_compaction_dynamic_level_bytes = true) {
  rocksdb::Options o;
  o.create_if_missing = false;
  o.compaction_style = compaction_style;
  o.level_compaction_dynamic_level_bytes = level_compaction_dynamic_level_bytes;
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

// Default RocksDB internal key: <user_key | 8-byte seq+type>.
inline rocksdb::Slice UserKeyFromInternalKey(const rocksdb::Slice& internal_key) {
  constexpr size_t kFooter = 8;
  if (internal_key.size() < kFooter) {
    return rocksdb::Slice();
  }
  return rocksdb::Slice(internal_key.data(), internal_key.size() - kFooter);
}

inline bool LooksLikeStUserKey(const rocksdb::Slice& user_key) {
  if (user_key.size() < 1) {
    return false;
  }
  const unsigned char magic = static_cast<unsigned char>(user_key.data()[0]);
  if (magic == 0xE5) {
    return user_key.size() >= 13;
  }
  if (magic == 0xE7) {
    // E7 layout is 31 bytes (legacy) or 32 bytes (with type_flags).
    return user_key.size() >= 31;
  }
  return false;
}

// DB iterators may yield either internal keys or user keys depending on path.
// Try stripping internal footer first; if that doesn't look like an ST key, fall
// back to the raw slice.
inline rocksdb::Slice UserKeyFromIteratorKey(const rocksdb::Slice& raw_key) {
  rocksdb::Slice uk = UserKeyFromInternalKey(raw_key);
  if (LooksLikeStUserKey(uk)) {
    return uk;
  }
  if (LooksLikeStUserKey(raw_key)) {
    return raw_key;
  }
  return uk.empty() ? raw_key : uk;
}

struct GhostCompactResult {
  uint64_t scanned = 0;
  uint64_t ghosts_seen = 0;
  uint64_t duplicate_merged = 0;
  uint64_t masters_ok = 0;
  uint64_t dangling = 0;
  uint64_t meta_mismatch = 0;
  uint64_t deletes = 0;
};

bool ParseBucketFromUserKey(const rocksdb::Slice& user_key, uint32_t bucket_width,
                            uint32_t* out_bucket) {
  if (out_bucket == nullptr || user_key.size() < 1) {
    return false;
  }
  const unsigned char magic = static_cast<unsigned char>(user_key.data()[0]);
  if (magic == 0xE7 && user_key.size() >= 5) {
    const size_t payload_offset = user_key.size() >= 32 ? 2 : 1;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(
        user_key.data() + payload_offset);
    *out_bucket = (static_cast<uint32_t>(p[0]) << 24) |
                  (static_cast<uint32_t>(p[1]) << 16) |
                  (static_cast<uint32_t>(p[2]) << 8) |
                  (static_cast<uint32_t>(p[3]));
    return true;
  }
  if (magic == 0xE6 && user_key.size() >= 5 && bucket_width > 0) {
    const unsigned char* p =
        reinterpret_cast<const unsigned char*>(user_key.data() + 1);
    const uint32_t t_start = (static_cast<uint32_t>(p[0]) << 24) |
                             (static_cast<uint32_t>(p[1]) << 16) |
                             (static_cast<uint32_t>(p[2]) << 8) |
                             (static_cast<uint32_t>(p[3]));
    *out_bucket = t_start / bucket_width;
    return true;
  }
  return false;
}

GhostCompactResult CompactGlobalGhosts(rocksdb::DB* db, bool verify_meta,
                                      bool merge_duplicates, bool dry_run, uint64_t max_probes,
                                      uint64_t max_deletes) {
  GhostCompactResult r;
  if (db == nullptr) {
    return r;
  }
  rocksdb::ReadOptions ro;
  ro.total_order_seek = true;
  std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro));
  rocksdb::WriteBatch batch;
  const rocksdb::WriteOptions wo;
  const rocksdb::Options db_opt = db->GetOptions();
  const uint32_t bucket_width =
      db_opt.experimental_compaction_event_time_bucket_width == 0
          ? 3600u
          : db_opt.experimental_compaction_event_time_bucket_width;
  std::unordered_set<std::string> seen_bucket_master;

  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    if (r.scanned >= max_probes) {
      break;
    }
    ++r.scanned;
    const rocksdb::Slice user_key = UserKeyFromIteratorKey(it->key());
    if (user_key.size() < 1) {
      continue;
    }
    const unsigned char magic = static_cast<unsigned char>(user_key.data()[0]);
    if (magic != 0xE6 && magic != 0xE7) {
      continue;
    }
    if (magic == 0xE7 && !IsE7GhostByKeyType(user_key)) {
      // V1: key-first ghost check for E7.
      continue;
    }
    segval::DecodedSegmentValue decoded{};
    const std::string value_blob(it->value().data(), it->value().size());
    if (!segval::DecodeSegmentValue(value_blob, &decoded) || decoded.is_legacy) {
      continue;
    }
    if (magic != 0xE7 && !decoded.is_ghost) {
      continue;
    }
    ++r.ghosts_seen;
    if (merge_duplicates && !decoded.master_key.empty()) {
      uint32_t bucket = 0;
      if (ParseBucketFromUserKey(user_key, bucket_width, &bucket)) {
        const std::string dedupe_key =
            std::to_string(bucket) + "|" + decoded.master_key;
        if (!seen_bucket_master.insert(dedupe_key).second) {
          ++r.duplicate_merged;
          if (!dry_run && r.deletes < max_deletes) {
            batch.Delete(user_key);
            ++r.deletes;
          }
          continue;
        }
      }
    }
    if (decoded.master_key.empty()) {
      ++r.dangling;
      if (!dry_run && r.deletes < max_deletes) {
        batch.Delete(user_key);
        ++r.deletes;
      }
      continue;
    }

    std::string master_value;
    rocksdb::Status s = db->Get(ro, decoded.master_key, &master_value);
    if (!s.ok()) {
      ++r.dangling;
      if (!dry_run && r.deletes < max_deletes) {
        batch.Delete(user_key);
        ++r.deletes;
      }
      continue;
    }
    ++r.masters_ok;

    if (verify_meta) {
      segval::DecodedSegmentValue master{};
      if (!segval::DecodeSegmentValue(master_value, &master) || master.is_legacy ||
          master.is_ghost) {
        ++r.meta_mismatch;
        if (!dry_run && r.deletes < max_deletes) {
          batch.Delete(user_key);
          ++r.deletes;
        }
        continue;
      }
      if (master.header.t_start != decoded.header.t_start ||
          master.header.t_end != decoded.header.t_end ||
          master.header.x_min != decoded.header.x_min ||
          master.header.x_max != decoded.header.x_max ||
          master.header.y_min != decoded.header.y_min ||
          master.header.y_max != decoded.header.y_max) {
        ++r.meta_mismatch;
        if (!dry_run && r.deletes < max_deletes) {
          batch.Delete(user_key);
          ++r.deletes;
        }
      }
    }
  }

  if (!dry_run && batch.GetDataSize() > 0) {
    (void)db->Write(wo, &batch);
  }
  return r;
}

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
    if (uk.size() > 0 && static_cast<unsigned char>(uk.data()[0]) == 0xE6) {
      return true;
    }
  }
  return false;
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
  size_t e7 = 0;
  size_t e6 = 0;
  size_t e5 = 0;
  size_t other = 0;
  uint32_t first_t_e7 = 0;
  uint32_t last_t_e7 = 0;
  bool have_t_e7 = false;
  uint32_t first_t_e6 = 0;
  uint32_t last_t_e6 = 0;
  bool have_t_e6 = false;
  const rocksdb::Options o = db->GetOptions();
  const uint32_t bucket_width =
      o.experimental_compaction_event_time_bucket_width == 0
          ? 3600u
          : o.experimental_compaction_event_time_bucket_width;
  for (it->SeekToFirst(); it->Valid() && seen < sample_limit; it->Next()) {
    const rocksdb::Slice k = UserKeyFromIteratorKey(it->key());
    if (k.size() == 0) {
      ++other;
      ++seen;
      continue;
    }
    const uint8_t magic = static_cast<uint8_t>(k.data()[0]);
    if (magic == 0xE7 && k.size() >= 17) {
      ++e7;
      const size_t payload_offset = k.size() >= 32 ? 2 : 1;
      const unsigned char* p =
          reinterpret_cast<const unsigned char*>(k.data() + payload_offset);
      const uint32_t bucket_id = (static_cast<uint32_t>(p[0]) << 24) |
                                 (static_cast<uint32_t>(p[1]) << 16) |
                                 (static_cast<uint32_t>(p[2]) << 8) |
                                 (static_cast<uint32_t>(p[3]));
      const unsigned char* offp =
          reinterpret_cast<const unsigned char*>(k.data() + payload_offset + 12);
      const uint32_t t_start_offset = (static_cast<uint32_t>(offp[0]) << 24) |
                                      (static_cast<uint32_t>(offp[1]) << 16) |
                                      (static_cast<uint32_t>(offp[2]) << 8) |
                                      (static_cast<uint32_t>(offp[3]));
      const uint32_t t = bucket_id * bucket_width + t_start_offset;
      if (!have_t_e7) {
        first_t_e7 = t;
        have_t_e7 = true;
      }
      last_t_e7 = t;
    } else if (magic == 0xE6 && k.size() >= 5) {
      ++e6;
      const unsigned char* p =
          reinterpret_cast<const unsigned char*>(k.data() + 1);
      const uint32_t t = (static_cast<uint32_t>(p[0]) << 24) |
                         (static_cast<uint32_t>(p[1]) << 16) |
                         (static_cast<uint32_t>(p[2]) << 8) |
                         (static_cast<uint32_t>(p[3]));
      if (!have_t_e6) {
        first_t_e6 = t;
        have_t_e6 = true;
      }
      last_t_e6 = t;
    } else if (magic == 0xE5) {
      ++e5;
    } else {
      ++other;
    }
    ++seen;
  }
  std::cout << "KEY_PROBE sample=" << seen << " e7=" << e7 << " e6=" << e6
            << " e5=" << e5
            << " other=" << other;
  if (have_t_e7) {
    std::cout << " e7_t_first=" << first_t_e7 << " e7_t_last=" << last_t_e7;
  }
  if (have_t_e6) {
    std::cout << " e6_t_first=" << first_t_e6 << " e6_t_last=" << last_t_e6;
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
            << o.experimental_compaction_spatial_hard_min_file_size
            << " st_compaction=" << static_cast<int>(o.experimental_st_compaction)
            << " st_level_trigger_ratio="
            << o.experimental_st_compaction_level_trigger_ratio
            << " st_l1_l2_tiered="
            << static_cast<int>(o.experimental_st_compaction_l1_l2_tiered)
            << " st_real_tiered="
            << static_cast<int>(o.experimental_st_compaction_real_tiered_l1_l2)
            << " st_tiered_max_runs="
            << o.experimental_st_compaction_tiered_max_runs_per_level
            << "\n";
  std::cout
      << label
      << " st_level_aware_policy "
      << "bucket_span(L1=2,L2-3=4,L4+=16)"
      << " time_span_factor(L1=0.5x,L2-3=1.0x,L4+=2.0x)"
      << " spatial_ratio_factor(L1=1.0x,L2-3=1.35x,L4+=1.8x)"
      << " spatial_hard_min_file_factor(L1=1.0x,L2-3=1.25x,L4+=1.75x)"
      << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string db_path;
  int target_file_mb = 32;
  bool dry_run = false;
  bool st_time_split_enable = false;
  bool st_compaction = false;
  bool st_l1_l2_tiered = false;
  bool st_real_tiered = false;
  int st_tiered_max_level = 0;
  int st_tiered_max_runs_per_level = 0;
  int max_level_bytes_mb = 0;
  bool no_force_bottommost = false;
  uint32_t st_bucket_sec = 0;
  uint32_t st_min_keys_per_file = 0;
  uint32_t st_max_span_sec = 0;
  uint32_t st_max_point_keys = 0;
  double st_spatial_soft_ratio = 0.0;
  double st_spatial_hard_ratio = 0.0;
  uint64_t st_spatial_hard_min_file_size = 0;
  bool read_repair_first = false;
  uint64_t read_repair_min_reads = 0;
  double read_repair_shape_weight = 1.0;
  std::string compaction_style_str = "leveled";
  bool global_ghost_compact = false;
  bool global_ghost_compact_dry_run = false;
  bool global_ghost_verify_meta = false;
  bool global_ghost_merge_duplicates = true;
  bool expect_no_ghost_values = false;
  uint64_t global_ghost_max_probes = 1'000'000;
  uint64_t global_ghost_max_deletes = 100'000;
  bool allow_legacy_e6 = false;
  bool post_verify_kv_vs_file_meta = false;
  bool post_read_bench = false;
  bool post_read_bench_global_counts_fallback_scan = false;
  bool post_read_bench_require_nonzero_global_counts = false;
  std::string post_verify_exe;
  std::string post_read_bench_exe;
  std::string post_prune_mode = "sst_manifest";
  uint32_t post_prune_t_min = 1596200000u;
  uint32_t post_prune_t_max = 1596203600u;
  float post_prune_x_min = 120.02f;
  float post_prune_x_max = 120.34f;
  float post_prune_y_min = 31.52f;
  float post_prune_y_max = 31.88f;

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
    if (std::strcmp(argv[i], "--st-compaction") == 0) {
      st_compaction = true;
      continue;
    }
    if (std::strcmp(argv[i], "--st-l1-l2-tiered") == 0) {
      st_l1_l2_tiered = true;
      continue;
    }
    if (std::strcmp(argv[i], "--st-real-tiered") == 0) {
      st_real_tiered = true;
      continue;
    }
    if (std::strcmp(argv[i], "--st-tiered-max-level") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const long v = std::strtol(argv[++i], &e, 10);
      if (e == argv[i] || *e != '\0' || v < 0 || v > 10) {
        std::cerr << "Invalid --st-tiered-max-level (0=adaptive, 1-10=fixed)\n";
        return 1;
      }
      st_tiered_max_level = static_cast<int>(v);
      continue;
    }
    if (std::strcmp(argv[i], "--st-tiered-max-runs-per-level") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const long v = std::strtol(argv[++i], &e, 10);
      if (e == argv[i] || *e != '\0' || v < 0 || v > 1000) {
        std::cerr << "Invalid --st-tiered-max-runs-per-level (0=unlimited)\n";
        return 1;
      }
      st_tiered_max_runs_per_level = static_cast<int>(v);
      continue;
    }
    if (std::strcmp(argv[i], "--max-level-bytes-mb") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const long v = std::strtol(argv[++i], &e, 10);
      if (e == argv[i] || *e != '\0' || v < 1 || v > 1048576) {
        std::cerr << "Invalid --max-level-bytes-mb (1..1048576)\n";
        return 1;
      }
      max_level_bytes_mb = static_cast<int>(v);
      continue;
    }
    if (std::strcmp(argv[i], "--no-force-bottommost") == 0) {
      no_force_bottommost = true;
      continue;
    }
    if (std::strcmp(argv[i], "--compaction-style") == 0 && i + 1 < argc) {
      compaction_style_str = argv[++i];
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
    if (std::strcmp(argv[i], "--st-max-span-sec") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const unsigned long v = std::strtoul(argv[++i], &e, 10);
      if (e == argv[i] || *e != '\0' || v > 86400ul * 3650ul) {
        std::cerr << "Invalid --st-max-span-sec (0..315360000)\n";
        return 1;
      }
      st_max_span_sec = static_cast<uint32_t>(v);
      continue;
    }
    if (std::strcmp(argv[i], "--st-max-point-keys") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const unsigned long v = std::strtoul(argv[++i], &e, 10);
      if (e == argv[i] || *e != '\0' || v > 100000000ul) {
        std::cerr << "Invalid --st-max-point-keys (0..1e8)\n";
        return 1;
      }
      st_max_point_keys = static_cast<uint32_t>(v);
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
    if (std::strcmp(argv[i], "--global-ghost-compact") == 0) {
      global_ghost_compact = true;
      continue;
    }
    if (std::strcmp(argv[i], "--global-ghost-compact-dry-run") == 0) {
      global_ghost_compact = true;
      global_ghost_compact_dry_run = true;
      continue;
    }
    if (std::strcmp(argv[i], "--global-ghost-verify-meta") == 0) {
      global_ghost_verify_meta = true;
      continue;
    }
    if (std::strcmp(argv[i], "--global-ghost-no-merge-duplicates") == 0) {
      global_ghost_merge_duplicates = false;
      continue;
    }
    if (std::strcmp(argv[i], "--expect-no-ghost-values") == 0) {
      expect_no_ghost_values = true;
      continue;
    }
    if (std::strcmp(argv[i], "--global-ghost-max-probes") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const unsigned long long v = std::strtoull(argv[++i], &e, 10);
      if (e == argv[i] || *e != '\0') {
        std::cerr << "Invalid --global-ghost-max-probes\n";
        return 1;
      }
      global_ghost_max_probes = static_cast<uint64_t>(v);
      continue;
    }
    if (std::strcmp(argv[i], "--global-ghost-max-deletes") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const unsigned long long v = std::strtoull(argv[++i], &e, 10);
      if (e == argv[i] || *e != '\0') {
        std::cerr << "Invalid --global-ghost-max-deletes\n";
        return 1;
      }
      global_ghost_max_deletes = static_cast<uint64_t>(v);
      continue;
    }
    if (std::strcmp(argv[i], "--allow-legacy-e6") == 0) {
      allow_legacy_e6 = true;
      continue;
    }
    if (std::strcmp(argv[i], "--post-verify-kv-vs-file-meta") == 0) {
      post_verify_kv_vs_file_meta = true;
      continue;
    }
    if (std::strcmp(argv[i], "--post-read-bench") == 0) {
      post_read_bench = true;
      continue;
    }
    if (std::strcmp(argv[i], "--post-read-bench-global-counts-fallback-scan") ==
        0) {
      post_read_bench = true;
      post_read_bench_global_counts_fallback_scan = true;
      continue;
    }
    if (std::strcmp(argv[i], "--post-read-bench-require-nonzero-global-counts") ==
        0) {
      post_read_bench = true;
      post_read_bench_require_nonzero_global_counts = true;
      continue;
    }
    if (std::strcmp(argv[i], "--post-verify-exe") == 0 && i + 1 < argc) {
      post_verify_exe = argv[++i];
      continue;
    }
    if (std::strcmp(argv[i], "--post-read-bench-exe") == 0 && i + 1 < argc) {
      post_read_bench_exe = argv[++i];
      continue;
    }
    if (std::strcmp(argv[i], "--post-prune-mode") == 0 && i + 1 < argc) {
      post_prune_mode = argv[++i];
      continue;
    }
    if (std::strcmp(argv[i], "--post-prune-t-min") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const unsigned long v = std::strtoul(argv[++i], &e, 10);
      if (e == argv[i] || *e != '\0') {
        std::cerr << "Invalid --post-prune-t-min\n";
        return 1;
      }
      post_prune_t_min = static_cast<uint32_t>(v);
      continue;
    }
    if (std::strcmp(argv[i], "--post-prune-t-max") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const unsigned long v = std::strtoul(argv[++i], &e, 10);
      if (e == argv[i] || *e != '\0') {
        std::cerr << "Invalid --post-prune-t-max\n";
        return 1;
      }
      post_prune_t_max = static_cast<uint32_t>(v);
      continue;
    }
    if (std::strcmp(argv[i], "--post-prune-x-min") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const double v = std::strtod(argv[++i], &e);
      if (e == argv[i] || *e != '\0') {
        std::cerr << "Invalid --post-prune-x-min\n";
        return 1;
      }
      post_prune_x_min = static_cast<float>(v);
      continue;
    }
    if (std::strcmp(argv[i], "--post-prune-x-max") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const double v = std::strtod(argv[++i], &e);
      if (e == argv[i] || *e != '\0') {
        std::cerr << "Invalid --post-prune-x-max\n";
        return 1;
      }
      post_prune_x_max = static_cast<float>(v);
      continue;
    }
    if (std::strcmp(argv[i], "--post-prune-y-min") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const double v = std::strtod(argv[++i], &e);
      if (e == argv[i] || *e != '\0') {
        std::cerr << "Invalid --post-prune-y-min\n";
        return 1;
      }
      post_prune_y_min = static_cast<float>(v);
      continue;
    }
    if (std::strcmp(argv[i], "--post-prune-y-max") == 0 && i + 1 < argc) {
      char* e = nullptr;
      const double v = std::strtod(argv[++i], &e);
      if (e == argv[i] || *e != '\0') {
        std::cerr << "Invalid --post-prune-y-max\n";
        return 1;
      }
      post_prune_y_max = static_cast<float>(v);
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
             "  --compaction-style <leveled|universal|fifo>  compaction style (default leveled)\n"
             "      NOTE: non-leveled styles rename old OPTIONS-* files so the DB opens with the new style.\n"
             "  --st-time-split      enable ST event-time split during compaction\n"
             "  --st-compaction      enable ST-compaction scheduler (Mind.md: L1+ at "
             "ratio*MaxBytes, whole-layer Lx->Lx+1, L0 output pinned L1)\n"
             "  --st-l1-l2-tiered    ST-Hybrid: use Tiered semantics for L1→L2, L2→L3\n"
             "                       (subset merge, lower write amp at shallow levels)\n"
             "  --st-bucket-sec W    split when floor(t_sec/W) changes (segment keys use t_start)\n"
             "  --st-min-keys K      require at least K ST keys in output before splitting on bucket change\n"
             "  --st-max-span-sec S  cut when (t_max - t_min) would exceed S (0=disable)\n"
             "  --st-max-point-keys N  cut after N ST keys written (0=disable)\n"
             "  --st-spatial-soft-ratio R  soft split trigger on MBR expansion ratio (>=0)\n"
             "  --st-spatial-hard-ratio R  hard split trigger on MBR expansion ratio (>=0)\n"
             "  --st-spatial-hard-min-file-mb M  hard split only if file size >= M MiB\n"
             "  --read-repair-first  enable read-repair-first file pick ordering\n"
             "  --read-repair-min-reads N  minimum sampled reads before read-repair scoring applies\n"
             "  --read-repair-shape-weight W  shape penalty weight for read-repair score\n"
             "  --global-ghost-compact  after CompactRange, scan & delete dangling/inconsistent ghosts (E7 V3)\n"
             "  --global-ghost-compact-dry-run  report only; do not delete\n"
             "  --global-ghost-verify-meta  delete ghosts when master meta mismatches\n"
             "  --global-ghost-no-merge-duplicates  disable dedupe by (bucket,master_key)\n"
             "  --expect-no-ghost-values  fail if any ghost value is observed during ghost scan\n"
             "  --global-ghost-max-probes N  cap scanned records (default 1e6)\n"
             "  --global-ghost-max-deletes N  cap deletes per run (default 1e5)\n"
             "  --allow-legacy-e6  allow running compaction on legacy E6 DBs (default: reject E6)\n"
             "  --post-verify-kv-vs-file-meta  run st_meta_compaction_verify after compaction\n"
             "  --post-read-bench  run st_meta_read_bench --verify-kv-results after compaction\n"
             "  --post-read-bench-global-counts-fallback-scan  add --global-manifest-counts-fallback-scan to post read-bench\n"
             "  --post-read-bench-require-nonzero-global-counts  fail post-check when 'global_manifest_counts ... masters=0 ghosts=0'\n"
             "  --post-verify-exe <path>  override st_meta_compaction_verify.exe path\n"
             "  --post-read-bench-exe <path>  override st_meta_read_bench.exe path\n"
             "  --post-prune-mode <mode>  read-bench prune mode (default sst_manifest)\n"
             "  --post-prune-t-min/max <u32> --post-prune-x-min/max --post-prune-y-min/max\n"
             "      window for --post-read-bench (defaults to wx_strat_narrow_01)\n"
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

  // Resolve compaction style.
  rocksdb::CompactionStyle comp_style =
      rocksdb::CompactionStyle::kCompactionStyleLevel;
  if (compaction_style_str == "universal") {
    comp_style = rocksdb::CompactionStyle::kCompactionStyleUniversal;
  } else if (compaction_style_str == "leveled") {
    comp_style = rocksdb::CompactionStyle::kCompactionStyleLevel;
  } else if (compaction_style_str == "fifo") {
    comp_style = rocksdb::CompactionStyle::kCompactionStyleFIFO;
  } else {
    std::cerr << "Invalid --compaction-style: " << compaction_style_str
              << " (expected leveled|universal|fifo)\n";
    return 1;
  }

  // When switching to a non-default compaction style or enabling ST time
  // split (which requires level_compaction_dynamic_level_bytes=false), the
  // immutable CF options stored in the OPTIONS file would conflict.
  // Rename the old OPTIONS file(s) so RocksDB adopts the options we pass.
  const bool need_new_options =
      compaction_style_str != "leveled" || st_time_split_enable;
  if (need_new_options) {
    try {
      for (const auto& entry :
           std::filesystem::directory_iterator(db_path)) {
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        if (name.size() >= 8 && name.substr(0, 8) == "OPTIONS-") {
          std::filesystem::path bak =
              entry.path().string() + ".bak_experiment";
          std::filesystem::rename(entry.path(), bak);
          std::cout << "Renamed " << entry.path().string() << " -> "
                    << bak.string() << "\n";
        }
      }
    } catch (const std::exception& e) {
      std::cerr << "Warning: failed to rename OPTIONS files: " << e.what()
                << "\n";
    }
  }

  const bool dynamic_level_bytes =
      st_time_split_enable ? false : true;
  rocksdb::Options opt =
      BenchCompatibleOpenOptions(comp_style, dynamic_level_bytes);
  std::unique_ptr<rocksdb::DB> db;
  rocksdb::Status s = rocksdb::DB::Open(opt, db_path, &db);
  if (!s.ok()) {
    std::cerr << "DB::Open: " << s.ToString() << "\n";
    return 1;
  }
  if (!allow_legacy_e6 && HasLegacyE6Keys(db.get())) {
    std::cerr
        << "Detected legacy E6 keys in DB. This compaction tool now defaults to "
           "E7-only experiments.\n"
        << "If you intentionally need legacy compatibility, rerun with "
           "--allow-legacy-e6.\n";
    return 2;
  }

  ReportFiles(db.get(), "BEFORE");
  ProbeUserKeys(db.get());
  ReportStCompactionOptions(db.get(), "BEFORE_OPTS");

  const uint64_t base_bytes =
      static_cast<uint64_t>(target_file_mb) * 1048576ull;
  std::unordered_map<std::string, std::string> cf_opts;
  cf_opts["target_file_size_base"] = std::to_string(base_bytes);
  if (st_compaction) {
    cf_opts["experimental_st_compaction"] = "true";
  }
  if (st_l1_l2_tiered) {
    cf_opts["experimental_st_compaction_l1_l2_tiered"] = "true";
    if (st_real_tiered) {
      cf_opts["experimental_st_compaction_real_tiered_l1_l2"] = "true";
    }
  }
  if (st_tiered_max_level > 0) {
    cf_opts["experimental_st_compaction_tiered_max_level"] =
        std::to_string(st_tiered_max_level);
  }
  if (st_tiered_max_runs_per_level >= 0 && st_real_tiered) {
    cf_opts["experimental_st_compaction_tiered_max_runs_per_level"] =
        std::to_string(st_tiered_max_runs_per_level);
  }
  if (max_level_bytes_mb > 0) {
    cf_opts["max_bytes_for_level_base"] =
        std::to_string(static_cast<uint64_t>(max_level_bytes_mb) * 1048576ull);
  }
  if (st_time_split_enable || st_bucket_sec != 0 || st_min_keys_per_file != 0 ||
      st_max_span_sec != 0 || st_max_point_keys != 0) {
    cf_opts["experimental_compaction_event_time_split"] = "true";
    if (st_bucket_sec != 0) {
      cf_opts["experimental_compaction_event_time_bucket_width"] =
          std::to_string(st_bucket_sec);
    }
    if (st_min_keys_per_file != 0) {
      cf_opts["experimental_compaction_event_time_min_keys_per_file"] =
          std::to_string(st_min_keys_per_file);
    }
    if (st_max_span_sec != 0) {
      cf_opts["experimental_compaction_event_time_max_span"] =
          std::to_string(st_max_span_sec);
    }
    if (st_max_point_keys != 0) {
      cf_opts["experimental_compaction_event_time_max_point_keys"] =
          std::to_string(st_max_point_keys);
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
  if (no_force_bottommost) {
    cro.bottommost_level_compaction =
        rocksdb::BottommostLevelCompaction::kSkip;
  } else {
    cro.bottommost_level_compaction =
        rocksdb::BottommostLevelCompaction::kForce;
  }
  std::cout << "CompactRange (full keyspace, may take a long time)...\n";
  std::cout.flush();
  s = db->CompactRange(cro, nullptr, nullptr);
  if (!s.ok()) {
    std::cerr << "CompactRange: " << s.ToString() << "\n";
    return 1;
  }

  ReportFiles(db.get(), "AFTER");
  if (global_ghost_compact) {
    std::cout << "GlobalGhostCompact (scan E6/E7 ghosts)...\n";
    GhostCompactResult gr = CompactGlobalGhosts(
        db.get(), global_ghost_verify_meta, global_ghost_merge_duplicates,
        global_ghost_compact_dry_run,
        global_ghost_max_probes, global_ghost_max_deletes);
    std::cout << "GlobalGhostCompact scanned=" << gr.scanned
              << " ghosts_seen=" << gr.ghosts_seen
              << " duplicate_merged=" << gr.duplicate_merged
              << " masters_ok=" << gr.masters_ok
              << " dangling=" << gr.dangling << " meta_mismatch=" << gr.meta_mismatch
              << " deletes=" << gr.deletes
              << (global_ghost_compact_dry_run ? " (dry-run)" : "") << "\n";
    if (expect_no_ghost_values && gr.ghosts_seen > 0) {
      std::cerr << "expect_no_ghost_values violated: ghosts_seen="
                << gr.ghosts_seen << "\n";
      return 5;
    }
  }
  if (post_verify_kv_vs_file_meta) {
    if (post_verify_exe.empty()) {
      post_verify_exe = SiblingExePath(argv[0], "st_meta_compaction_verify.exe");
    }
    std::ostringstream cmd;
    cmd << QuoteArg(post_verify_exe) << " --db " << QuoteArg(db_path)
        << " --verify-only --verify-kv-vs-file-meta --no-clean";
    const int rc = RunExternalCommand(cmd.str());
    if (rc != 0) {
      std::cerr << "post-check verify failed, exit_code=" << rc << "\n";
      return 3;
    }
  }
  if (post_read_bench) {
    if (post_read_bench_exe.empty()) {
      post_read_bench_exe = SiblingExePath(argv[0], "st_meta_read_bench.exe");
    }
    std::ostringstream cmd;
    cmd << QuoteArg(post_read_bench_exe) << " --db " << QuoteArg(db_path)
        << " --prune-mode " << post_prune_mode
        << " --verify-kv-results --verify-kv-results-strict-exit"
        << " --prune-t-min " << post_prune_t_min
        << " --prune-t-max " << post_prune_t_max
        << " --prune-x-min " << post_prune_x_min
        << " --prune-x-max " << post_prune_x_max
        << " --prune-y-min " << post_prune_y_min
        << " --prune-y-max " << post_prune_y_max;
    if (post_read_bench_global_counts_fallback_scan) {
      cmd << " --global-manifest-counts-fallback-scan";
    }
    if (allow_legacy_e6) {
      cmd << " --allow-legacy-e6";
    }
    std::string run_cmd = cmd.str();
    if (post_read_bench_require_nonzero_global_counts) {
      run_cmd +=
          " | findstr /R /C:\"global_manifest_counts generation=.* masters=[1-9]\"";
    }
    const int rc = RunExternalCommand(run_cmd);
    if (rc != 0) {
      std::cerr << "post-check read bench failed, exit_code=" << rc << "\n";
      return 4;
    }
  }
  std::cout
      << "Next: run st_meta_sst_diag.exe on each *.sst (or st_diag_align_read_bench "
         "after pointing SST paths / copy DB).\n";
  return 0;
}

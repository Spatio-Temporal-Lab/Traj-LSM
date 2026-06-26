// Validates experimental Compaction Event-Time splitting: compare SST counts /
// approximate event-time span across file boundaries (from smallest/largest
// user keys). With --split, uses CompactFiles (default allow_trivial_move=false)
// so compaction always merges SSTs (CompactRange can trivial-move and skip
// event-time logic). ST user keys 0xE5 (same layout as st_meta_scan_bench).

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <cmath>
#include <limits>

#include <rocksdb/db.h>
#include <rocksdb/metadata.h>
#include <rocksdb/options.h>
#include <rocksdb/sst_file_reader.h>
#include <rocksdb/table.h>

#include "table/index_value_codec.h"
#include "table/st_meta_index_extension.h"
#include "table/st_meta_user_key.h"

#include "st_user_key.hpp"

// Set by rocks-demo CMake: -DROCKS_DEMO_BT=Release (must match rocksdb.lib build).
#ifndef ROCKS_DEMO_BT
#define ROCKS_DEMO_BT unknown
#endif
#define ROCKS_DEMO_STR_(x) #x
#define ROCKS_DEMO_STR(x) ROCKS_DEMO_STR_(x)

namespace {

void Log(const char* msg) {
  std::fprintf(stdout, "%s\n", msg);
  std::fflush(stdout);
}

bool UserKeyEventTime(const std::string& uk, uint32_t* t_sec) {
  if (uk.size() < 5 || t_sec == nullptr) {
    return false;
  }
  if (static_cast<unsigned char>(uk[0]) != 0xE5) {
    return false;
  }
  std::memcpy(t_sec, uk.data() + 1, 4);
  return true;
}

std::vector<std::string> L0SstNamesForCompactFiles(
    const rocksdb::ColumnFamilyMetaData& cf) {
  std::vector<std::string> out;
  const rocksdb::LevelMetaData* l0 = nullptr;
  for (const auto& lv : cf.levels) {
    if (lv.level == 0) {
      l0 = &lv;
      break;
    }
  }
  if (l0 == nullptr) {
    return out;
  }
  for (const auto& f : l0->files) {
    if (!f.name.empty()) {
      out.push_back(f.name);
    } else if (!f.relative_filename.empty()) {
      out.push_back(std::string("/") + f.relative_filename);
    }
  }
  return out;
}

void ReportSstByLevel(rocksdb::DB* db, const char* label) {
  std::vector<rocksdb::LiveFileMetaData> files;
  db->GetLiveFilesMetaData(&files);
  constexpr int kMaxLevel = 64;
  int cnt[kMaxLevel] = {};
  int non_l0 = 0;
  uint32_t worst_span_non_l0 = 0;
  for (const auto& f : files) {
    const int L = f.level;
    if (L < 0 || L >= kMaxLevel) {
      continue;
    }
    cnt[L]++;
    if (L > 0) {
      non_l0++;
      uint32_t ts = 0, tl = 0;
      if (UserKeyEventTime(f.smallestkey, &ts) &&
          UserKeyEventTime(f.largestkey, &tl)) {
        const uint32_t lo = (std::min)(ts, tl);
        const uint32_t hi = (std::max)(ts, tl);
        worst_span_non_l0 =
            (std::max)(worst_span_non_l0, hi - lo);
      }
    }
  }
  std::cout << label << " total_sst=" << files.size()
            << " non_L0=" << non_l0
            << " max_t_span_endpoints(non_L0)=" << worst_span_non_l0
            << " per_level:";
  for (int L = 0; L < kMaxLevel; ++L) {
    if (cnt[L] != 0) {
      std::cout << " L" << L << "=" << cnt[L];
    }
  }
  std::cout << std::endl;
}

bool FileBoundsCoverScanned(const rocksdb::SpatioTemporalBlockMeta& f,
                             const rocksdb::SpatioTemporalBlockMeta& s,
                             float eps) {
  if (f.t_min > s.t_min || f.t_max < s.t_max) {
    return false;
  }
  const double el = static_cast<double>(eps);
  auto axis_ok = [&](float fl, float fh, float sl, float sh) -> bool {
    const double sd_lo = static_cast<double>(sl);
    const double sd_hi = static_cast<double>(sh);
    if (!std::isfinite(sd_lo) || !std::isfinite(sd_hi)) {
      return true;
    }
    const double fd_lo = static_cast<double>(fl);
    const double fd_hi = static_cast<double>(fh);
    const bool lo_ok = !std::isfinite(fd_lo) || fd_lo <= sd_lo + el;
    const bool hi_ok = !std::isfinite(fd_hi) || fd_hi >= sd_hi - el;
    return lo_ok && hi_ok;
  };
  return axis_ok(f.x_min, f.x_max, s.x_min, s.x_max) &&
         axis_ok(f.y_min, f.y_max, s.y_min, s.y_max);
}

// Phase-1 compaction check: SST table property `rocksdb.experimental.st_file_bounds`
// must envelop the union of decoded ST summaries from actual KV (same semantics as
// MergeSpatioTemporalMetaFromKeyValue). Bitmap is not recomputed here.
bool VerifyLiveSstsKvVsFileStBounds(rocksdb::DB* db,
                                    const rocksdb::Options& options,
                                    std::string* first_error) {
  std::vector<rocksdb::LiveFileMetaData> files;
  db->GetLiveFilesMetaData(&files);
  rocksdb::SstFileReader reader(options);

  constexpr float kAxisEps =
      1e-5f * 8.f;  // permissive vs float round-trip in SST aggregation

  size_t checked = 0;
  size_t skipped_no_st = 0;
  size_t processed = 0;
  {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "VerifyLiveSstsKvVsFileStBounds: start live_files_total=%llu",
                  static_cast<unsigned long long>(files.size()));
    Log(buf);
  }
  for (const rocksdb::LiveFileMetaData& f : files) {
    ++processed;
    if (processed == 1 || (processed % 50) == 0) {
      char buf[192];
      std::snprintf(buf, sizeof(buf),
                    "VerifyLiveSstsKvVsFileStBounds: progress processed=%llu/%llu "
                    "checked_ST_sst=%llu skipped_no_decodable_st=%llu",
                    static_cast<unsigned long long>(processed),
                    static_cast<unsigned long long>(files.size()),
                    static_cast<unsigned long long>(checked),
                    static_cast<unsigned long long>(skipped_no_st));
      Log(buf);
    }
    std::filesystem::path full = std::filesystem::path(f.directory) / f.relative_filename;
    rocksdb::Status os = reader.Open(full.string());
    if (!os.ok()) {
      if (first_error) {
        *first_error = "Open SST " + full.string() + ": " + os.ToString();
      }
      return false;
    }
    std::shared_ptr<const rocksdb::TableProperties> tp = reader.GetTableProperties();
    if (!tp) {
      if (first_error) {
        *first_error = "No table properties: " + full.string();
      }
      return false;
    }
    auto pit = tp->user_collected_properties.find(
        rocksdb::kExperimentalStFileBoundsPropertyName());
    if (pit == tp->user_collected_properties.end()) {
      if (first_error) {
        *first_error = "Missing " +
                       std::string(rocksdb::kExperimentalStFileBoundsPropertyName()) +
                       " in " + full.string();
      }
      return false;
    }
    rocksdb::Slice prop_slice(pit->second);
    rocksdb::SpatioTemporalBlockMeta file_bounds{};
    rocksdb::Status ds =
        rocksdb::DecodeSpatioTemporalIndexTail(&prop_slice, &file_bounds);
    if (!ds.ok() || !prop_slice.empty()) {
      if (first_error) {
        *first_error = "Decode file ST bounds " + full.string() + ": " +
                       ds.ToString();
      }
      return false;
    }

    rocksdb::SpatioTemporalBlockMeta scanned{};
    bool scanned_valid = false;

    std::unique_ptr<rocksdb::Iterator> it(reader.NewIterator(rocksdb::ReadOptions()));
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      const rocksdb::Slice uk = it->key();
      const rocksdb::Slice val = it->value();
      rocksdb::MergeSpatioTemporalMetaFromKeyValue(uk,
                                                   std::make_optional(val),
                                                   &scanned, &scanned_valid);
    }
    if (!it->status().ok()) {
      if (first_error) {
        *first_error = "SST KV scan failed " + full.string() + ": " +
                       it->status().ToString();
      }
      return false;
    }
    if (!scanned_valid) {
      ++skipped_no_st;
      continue;
    }
    ++checked;
    if (!FileBoundsCoverScanned(file_bounds, scanned, kAxisEps)) {
      if (first_error) {
        std::ostringstream o;
        o << "KV union not covered by rocksdb.experimental.st_file_bounds "
          << full.string() << " file_bounds t[" << file_bounds.t_min << ","
          << file_bounds.t_max << "] x[" << file_bounds.x_min << ","
          << file_bounds.x_max << "] y[" << file_bounds.y_min << ","
          << file_bounds.y_max << "] scanned t[" << scanned.t_min << ","
          << scanned.t_max << "] x[" << scanned.x_min << "," << scanned.x_max
          << "] y[" << scanned.y_min << "," << scanned.y_max << "]";
        *first_error = o.str();
      }
      return false;
    }
  }

  {
    char summary[192];
    std::snprintf(
        summary, sizeof(summary),
        "VerifyLiveSstsKvVsFileStBounds: ok  checked_ST_sst=%llu  "
        "skipped_no_decodable_st=%llu  live_files_total=%llu",
        static_cast<unsigned long long>(checked),
        static_cast<unsigned long long>(skipped_no_st),
        static_cast<unsigned long long>(files.size()));
    Log(summary);
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _DEBUG
  // Release rocksdb.lib (typical "ninja rocksdb") uses /MD; Debug demo uses /MDd.
  // Mixing heaps causes access violations inside DB::Open with no C++ exception.
  if (std::getenv("ROCKSDB_ALLOW_MSVC_DEBUG_DEMO") == nullptr) {
    std::fprintf(
        stderr,
        "ERROR: This program was built MSVC Debug (/MDd). Your rocksdb.lib is "
        "normally Release (/MD). That mix crashes inside DB::Open.\n\n"
        "Fix (recommended):\n"
        "  cd D:\\Project\\rocks-demo\\build\n"
        "  cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..\n"
        "  ninja st_meta_compaction_verify\n\n"
        "If you built RocksDB in Debug on purpose, rerun with:\n"
        "  set ROCKSDB_ALLOW_MSVC_DEBUG_DEMO=1\n");
    return 2;
  }
#endif
  bool split = false;
  bool no_clean = false;
  bool no_st_meta = false;
  bool minimal = false;
  bool verify_kv_vs_file_bounds = false;
  bool verify_only = false;
  int num_keys = 50000;
  uint32_t event_max_span = 80;
  uint32_t event_bucket_width = 0;
  uint32_t event_max_point_keys = 0;
  std::string db_path = "D:/Project/data/st_meta_compaction_verify";
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--split") == 0) {
      split = true;
    } else if (std::strcmp(argv[i], "--verify-only") == 0) {
      verify_only = true;
    } else if (std::strcmp(argv[i], "--no-st-meta") == 0) {
      no_st_meta = true;
    } else if (std::strcmp(argv[i], "--minimal") == 0) {
      minimal = true;
    } else if (std::strcmp(argv[i], "--verify-kv-vs-file-meta") == 0) {
      verify_kv_vs_file_bounds = true;
    } else if (std::strcmp(argv[i], "--no-clean") == 0) {
      no_clean = true;
    } else if (std::strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
      db_path = argv[++i];
    } else if (std::strcmp(argv[i], "--num") == 0 && i + 1 < argc) {
      char* end = nullptr;
      const long n = std::strtol(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0' || n < 100 || n > 10000000) {
        std::fprintf(stderr, "Invalid --num (use 100..10000000)\n");
        return 1;
      }
      num_keys = static_cast<int>(n);
    } else if (std::strcmp(argv[i], "--max-span") == 0 && i + 1 < argc) {
      char* end = nullptr;
      const unsigned long v = std::strtoul(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0' || v == 0 || v > 0xFFFFFFFFul) {
        std::fprintf(stderr, "Invalid --max-span (use 1..4294967295)\n");
        return 1;
      }
      event_max_span = static_cast<uint32_t>(v);
    } else if (std::strcmp(argv[i], "--bucket-width") == 0 && i + 1 < argc) {
      char* end = nullptr;
      const unsigned long v = std::strtoul(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0' || v > 0xFFFFFFFFul) {
        std::fprintf(stderr, "Invalid --bucket-width\n");
        return 1;
      }
      event_bucket_width = static_cast<uint32_t>(v);
    } else if (std::strcmp(argv[i], "--max-point-keys") == 0 && i + 1 < argc) {
      char* end = nullptr;
      const unsigned long v = std::strtoul(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0' || v > 0xFFFFFFFFul) {
        std::fprintf(stderr, "Invalid --max-point-keys\n");
        return 1;
      }
      event_max_point_keys = static_cast<uint32_t>(v);
    } else if (std::strcmp(argv[i], "-h") == 0 ||
               std::strcmp(argv[i], "--help") == 0) {
      std::cout
          << "st_meta_compaction_verify [--split] [--db <dir>] [--num N] "
             "[--no-clean] [--no-st-meta] [--minimal]\n"
             "            [--verify-kv-vs-file-meta] [--verify-only]\n"
             "            [--max-span N] [--bucket-width N] [--max-point-keys N]\n"
             "  Default --num=50000. Use --num 5000 for a quick run.\n"
             "  Cleans db dir via std::filesystem::remove_all (not DestroyDB) "
             "to avoid Windows hangs; --no-clean skips removal.\n"
             "  --verify-only: do NOT delete or write the DB; just open the\n"
             "    existing DB and run --verify-kv-vs-file-meta (read-only).\n"
             "  --minimal: only create_if_missing + disable_auto_compactions "
             "(default table factory); for diagnosing Open crashes.\n"
             "  --no-st-meta: plain BlockBasedTable (no ST index tail); use if "
             "Open crashes with experimental_spatio_temporal_meta_in_index_value.\n"
             "  --verify-kv-vs-file-meta: after compaction, check each live SST's "
             "`rocksdb.experimental.st_file_bounds` envelops the KV union (E5/E7).\n"
             "  Without --split: CompactRange (may trivial-move SSTs).\n"
             "  With --split: event-time split; default --max-span 80; compaction "
             "uses CompactFiles (real merge, no optional codecs). Keys sort by t "
             "then lon/lat/id; try --bucket-width 1 or small --max-span.\n";
      return 0;
    }
  }

  for (char& c : db_path) {
    if (c == '\\') {
      c = '/';
    }
  }

  Log((split ? "MODE split_ON" : "MODE split_OFF"));
  if (split) {
    char ev[160];
    std::snprintf(ev, sizeof(ev),
                  "event_time: max_span=%" PRIu32 " bucket_width=%" PRIu32
                  " max_point_keys=%" PRIu32,
                  event_max_span, event_bucket_width, event_max_point_keys);
    Log(ev);
  }
  {
    const std::string db_line = "db=" + db_path;
    Log(db_line.c_str());
  }
  {
    char btbuf[120];
    std::snprintf(btbuf, sizeof(btbuf), "rocks-demo CMAKE_BUILD_TYPE=%s",
                  ROCKS_DEMO_STR(ROCKS_DEMO_BT));
    Log(btbuf);
  }
  std::fprintf(
      stderr,
      "rocksdb.lib MUST be built with the same CMAKE_BUILD_TYPE. "
      "Upstream RocksDB defaults to Debug when .git exists and TYPE was unset.\n"
      "If DB::Open crashes, from rocksdb/build run:\n"
      "  cmake -G Ninja -DCMAKE_BUILD_TYPE=%s ..\n"
      "  ninja clean && ninja rocksdb\n"
      "Then rebuild st_meta_compaction_verify.\n",
      ROCKS_DEMO_STR(ROCKS_DEMO_BT));
  std::fflush(stderr);

  if (verify_only && !verify_kv_vs_file_bounds) {
    std::fprintf(stderr,
                 "ERROR: --verify-only requires --verify-kv-vs-file-meta\n");
    return 1;
  }
  if (verify_only && split) {
    std::fprintf(
        stderr,
        "ERROR: --verify-only is incompatible with --split (no compaction)\n");
    return 1;
  }
  if (verify_only && !no_clean) {
    // Be explicit: this tool used to delete db_path by default.
    Log("NOTE: --verify-only implies --no-clean (safety)");
    no_clean = true;
  }

  try {
    const std::filesystem::path p(db_path);
    if (p.has_parent_path()) {
      std::filesystem::create_directories(p.parent_path());
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "create_directories: %s\n", e.what());
    return 1;
  }

  if (!no_clean) {
    try {
      const std::filesystem::path dbp(db_path);
      if (std::filesystem::exists(dbp)) {
        Log("Removing existing db directory (remove_all)...");
        const auto n = std::filesystem::remove_all(dbp);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "remove_all ok (entries=%llu)",
                      static_cast<unsigned long long>(n));
        Log(buf);
      } else {
        Log("No existing db directory (fresh path).");
      }
    } catch (const std::exception& e) {
      std::fprintf(stderr, "remove_all failed: %s\n", e.what());
      return 1;
    }
  } else {
    Log("--no-clean: skip directory removal");
  }

  if (!verify_only) {
    try {
      std::filesystem::create_directories(std::filesystem::path(db_path));
    } catch (const std::exception& e) {
      std::fprintf(stderr, "create_directories(db): %s\n", e.what());
      return 1;
    }
    Log("db directory ready (empty or new)");
  }

  rocksdb::Options opt;
  opt.create_if_missing = !verify_only;
  opt.disable_auto_compactions = true;
  if (minimal) {
    Log("Options: --minimal (default table factory, no custom sizes)");
  } else {
    opt.write_buffer_size = 4 << 20;
    opt.target_file_size_base = 256ull << 20;
    opt.max_bytes_for_level_base = 256ull << 20;
    rocksdb::BlockBasedTableOptions tb;
    if (!no_st_meta) {
      tb.experimental_spatio_temporal_meta_in_index_value = true;
    }
    opt.table_factory.reset(rocksdb::NewBlockBasedTableFactory(tb));
    if (no_st_meta) {
      Log("Table: BlockBasedTable without ST index tail (--no-st-meta)");
    } else {
      Log("Table: BlockBasedTable with experimental ST index tail");
    }
  }

  if (split) {
    opt.experimental_compaction_event_time_split = true;
    opt.experimental_compaction_event_time_max_span = event_max_span;
    opt.experimental_compaction_event_time_bucket_width = event_bucket_width;
    opt.experimental_compaction_event_time_max_point_keys = event_max_point_keys;
    Log("Event-time verify: will use CompactFiles (not CompactRange); "
        "CompactionOptions::allow_trivial_move defaults false so SSTs are "
        "merged (no Snappy/Zlib required).");
  }

  Log("Open DB... (if this never finishes: close Explorer preview of this "
      "folder, kill other RocksDB processes, or exclude path from AV)");
  std::fprintf(stderr, "[trace] calling DB::Open...\n");
  std::fflush(stderr);

  std::unique_ptr<rocksdb::DB> db;
  rocksdb::Status s;
  const auto t_open0 = std::chrono::steady_clock::now();
  try {
    if (verify_only) {
      s = rocksdb::DB::OpenForReadOnly(opt, db_path, &db);
    } else {
      s = rocksdb::DB::Open(opt, db_path, &db);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "DB::Open std::exception: %s\n", e.what());
    return 1;
  } catch (...) {
    std::fprintf(stderr, "DB::Open: unknown C++ exception\n");
    return 1;
  }
  const auto open_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t_open0)
                           .count();
  std::fprintf(stderr, "[trace] DB::Open returned (C++ level)\n");
  std::fflush(stderr);

  {
    const std::string st = s.ToString();
    const std::string line = "Open returned after " +
                             std::to_string(static_cast<long long>(open_ms)) +
                             " ms: " + st;
    Log(line.c_str());
  }
  if (!s.ok()) {
    return 1;
  }

  if (verify_only) {
    if (verify_kv_vs_file_bounds) {
      std::string err;
      const bool ok = VerifyLiveSstsKvVsFileStBounds(db.get(), opt, &err);
      if (!ok) {
        std::cerr << err << "\n";
        return 1;
      }
    }
    return 0;
  }

  if (split) {
    const rocksdb::Options o = db->GetOptions();
    char ob[220];
    std::snprintf(
        ob, sizeof(ob),
        "GetOptions() after Open: experimental_compaction_event_time_split=%d "
        "max_span=%" PRIu32 " bucket_width=%" PRIu32 " max_point_keys=%" PRIu32,
        static_cast<int>(o.experimental_compaction_event_time_split),
        o.experimental_compaction_event_time_max_span,
        o.experimental_compaction_event_time_bucket_width,
        o.experimental_compaction_event_time_max_point_keys);
    Log(ob);
    if (!o.experimental_compaction_event_time_split) {
      Log("WARNING: CF options show event-time split OFF; compaction will not "
          "apply experimental_compaction_event_time_*.");
    } else if (o.experimental_compaction_event_time_max_span != event_max_span ||
               o.experimental_compaction_event_time_bucket_width !=
                   event_bucket_width ||
               o.experimental_compaction_event_time_max_point_keys !=
                   event_max_point_keys) {
      Log("WARNING: GetOptions() event-time fields differ from CLI (unexpected).");
    }
  }

  const std::string val(32, 'v');
  char put_msg[64];
  std::snprintf(put_msg, sizeof(put_msg), "Put keys=%d...", num_keys);
  Log(put_msg);
  for (int i = 0; i < num_keys; ++i) {
    const uint32_t t = static_cast<uint32_t>(i % 500);
    const float lon = -120.f + static_cast<float>((i % 200) * 0.01f);
    const float lat = 35.f + static_cast<float>((i % 150) * 0.01f);
    s = db->Put(rocksdb::WriteOptions(),
                StUserKey(t, lon, lat, static_cast<uint64_t>(i)), val);
    if (!s.ok()) {
      std::fprintf(stderr, "Put: %s\n", s.ToString().c_str());
      return 1;
    }
  }
  Log("Put ok");

  Log("Flush...");
  s = db->Flush(rocksdb::FlushOptions());
  if (!s.ok()) {
    std::fprintf(stderr, "Flush: %s\n", s.ToString().c_str());
    return 1;
  }
  Log("Flush ok");

  rocksdb::ColumnFamilyMetaData cf_meta;
  db->GetColumnFamilyMetaData(&cf_meta);
  const std::vector<std::string> l0_names = L0SstNamesForCompactFiles(cf_meta);
  if (l0_names.empty()) {
    std::fprintf(stderr, "No L0 SST files after flush; cannot compact.\n");
    return 1;
  }

  if (split) {
    Log("CompactFiles (may take a while): L0 -> last level, real merge...");
    rocksdb::CompactionOptions cfo;
    const int last_level =
        (std::max)(1, db->GetOptions().num_levels - 1);
    s = db->CompactFiles(cfo, l0_names, last_level);
    if (!s.ok()) {
      std::fprintf(stderr, "CompactFiles: %s\n", s.ToString().c_str());
      return 1;
    }
    Log("CompactFiles ok");
  } else {
    Log("CompactRange (may take a while)...");
    rocksdb::CompactRangeOptions cro;
    s = db->CompactRange(cro, nullptr, nullptr);
    if (!s.ok()) {
      std::fprintf(stderr, "CompactRange: %s\n", s.ToString().c_str());
      return 1;
    }
    Log("CompactRange ok");
  }

  Log("--- result ---");
  ReportSstByLevel(db.get(), split ? "split_ON" : "split_OFF");

  if (verify_kv_vs_file_bounds) {
    if (minimal || no_st_meta) {
      std::fprintf(
          stderr,
          "--verify-kv-vs-file-meta requires ST-enabled table (--no-st-meta and "
          "--minimal are incompatible).\n");
      return 2;
    }
    std::string verr;
    if (!VerifyLiveSstsKvVsFileStBounds(db.get(), db->GetOptions(), &verr)) {
      std::fprintf(stderr, "VERIFY FAILED: %s\n", verr.c_str());
      return 3;
    }
  }

  Log("exit 0");
  return 0;
}

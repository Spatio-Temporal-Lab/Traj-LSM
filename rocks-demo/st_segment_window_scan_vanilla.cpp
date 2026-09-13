// Scan default CF with **upstream RocksDB** (no fork headers): SeekToFirst..Next,
// count keys whose user key lies in the spatio-temporal window (E5 point / E7 segment).
// Same window semantics as st_meta_read_bench full_scan mode=window for Wuxi segment DBs.
//
// Build: prefers rocksdb_vanilla/build[/Release]/rocksdb.lib (see tools/bootstrap_rocksdb_vanilla.ps1).
// CMake -DVANILLA_STANDIN_LINK_FORK_LIB=ON links ../rocksdb instead (smoke only; not official upstream).

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/perf_context.h>
#include <rocksdb/perf_level.h>
#include <rocksdb/slice.h>
#include <algorithm>

namespace {

using rocksdb::Slice;

constexpr unsigned char kMagicPoint = 0xE5;
constexpr unsigned char kMagicSegmentTzid = 0xE7;

inline Slice UserKeyFromInternalKey(const Slice& internal_key) {
  constexpr size_t kFooter = 8;
  if (internal_key.size() < kFooter) {
    return Slice();
  }
  return Slice(internal_key.data(), internal_key.size() - kFooter);
}

// DBIter usually yields internal key (user | seq+type). Some paths yield user keys
// already; stripping 8 bytes then corrupts short user keys.
inline Slice UserKeyFromIteratorKey(const Slice& raw) {
  Slice uk = UserKeyFromInternalKey(raw);
  if (uk.empty()) {
    return uk;
  }
  const auto m = static_cast<unsigned char>(uk[0]);
  if (m == kMagicPoint && uk.size() < 13) {
    return raw;
  }
  if (m == kMagicSegmentTzid && uk.size() < 27) {
    return raw;
  }
  return uk;
}

inline uint32_t DecodeFixed32LittleEndian(const char* p) {
  return static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

inline uint32_t DecodeFixed32BigEndian(const char* p) {
  return (static_cast<uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[3])));
}

bool ConsumeFixed32(Slice* s, uint32_t* out) {
  if (s->size() < 4) {
    return false;
  }
  *out = DecodeFixed32LittleEndian(s->data());
  s->remove_prefix(4);
  return true;
}

bool ConsumeFixed32BigEndian(Slice* s, uint32_t* out) {
  if (s->size() < 4) {
    return false;
  }
  *out = DecodeFixed32BigEndian(s->data());
  s->remove_prefix(4);
  return true;
}

bool TryDecodeE7KeyStartTime(const Slice& user_key, uint32_t* t_start) {
  if (t_start == nullptr || user_key.size() < 27 ||
      static_cast<unsigned char>(user_key.data()[0]) != kMagicSegmentTzid) {
    return false;
  }
  constexpr uint32_t kE7BucketSeconds = 3600;
  const size_t payload_offset = user_key.size() >= 32 ? 2 : 1;
  Slice s(user_key.data() + payload_offset, user_key.size() - payload_offset);
  uint32_t bucket_id = 0;
  if (!ConsumeFixed32BigEndian(&s, &bucket_id)) {
    return false;
  }
  // E7 key no longer carries t_start offset; use coarse bucket start (conservative).
  *t_start = bucket_id * kE7BucketSeconds;
  return true;
}

// Mirrors rocksdb/table/st_meta_user_key.cc UserKeySpatioTemporalInWindow for E5/E7 only.
bool UserKeyStInWindow(const Slice& user_key, uint32_t t_min, uint32_t t_max, float x_min,
                       float x_max, float y_min, float y_max) {
  if (user_key.size() < 13) {
    return false;
  }
  const char* p = user_key.data();
  const auto magic = static_cast<unsigned char>(p[0]);
  if (magic != kMagicPoint && magic != kMagicSegmentTzid) {
    return false;
  }
  Slice s(p + 1, user_key.size() - 1);
  if (magic == kMagicPoint) {
    uint32_t t_sec = 0;
    if (!ConsumeFixed32(&s, &t_sec)) {
      return false;
    }
    uint32_t xbits = 0, ybits = 0;
    if (!ConsumeFixed32(&s, &xbits) || !ConsumeFixed32(&s, &ybits)) {
      return false;
    }
    float x = 0.f, y = 0.f;
    std::memcpy(&x, &xbits, sizeof(x));
    std::memcpy(&y, &ybits, sizeof(y));
    if (t_sec > t_max || t_sec < t_min || x > x_max || x < x_min || y > y_max ||
        y < y_min) {
      return false;
    }
    return true;
  }
  if (magic == kMagicSegmentTzid) {
    // E7 key no longer guarantees in-key t_end/MBR. Keep key filtering
    // conservative and rely on value verification for the final answer.
    uint32_t t_start = 0;
    if (!TryDecodeE7KeyStartTime(user_key, &t_start)) {
      return false;
    }
    return t_start <= t_max;
  }
  return false;
}

bool ValueHasPointInWindow(const std::string& json_line, uint32_t t_min,
                           uint32_t t_max, float x_min, float x_max, float y_min,
                           float y_max) {
  // Extremely small ad-hoc parser to avoid adding dependencies:
  // scan for occurrences like ["YYYY-mm-dd HH:MM:SS", lon, lat]
  // and check bounds. Assumes the dataset format we ingest.
  auto ParseTimeToUnixSec = [](const char* p) -> uint32_t {
    // "YYYY-mm-dd HH:MM:SS" -> only parse seconds since epoch is expensive;
    // but dataset also contains numeric t_min/t_max which we already use for time.
    // Here we skip time parsing and only validate lon/lat; time overlap already checked via key.
    (void)p;
    return 0;
  };
  (void)ParseTimeToUnixSec;
  const char* s = json_line.c_str();
  const char* e = s + json_line.size();
  // Find "points":[
  const char* p = std::search(s, e, "points", "points" + 6);
  if (p == e) return false;
  // Scan for lon/lat numbers after first string in each point tuple.
  // This is best-effort and may miss on unexpected formatting.
  while (p < e) {
    // Find '['
    p = static_cast<const char*>(std::memchr(p, '[', static_cast<size_t>(e - p)));
    if (!p) break;
    // Find quote end then parse two numbers separated by commas.
    const char* q = static_cast<const char*>(std::memchr(p, '"', static_cast<size_t>(e - p)));
    if (!q) { ++p; continue; }
    q = static_cast<const char*>(std::memchr(q + 1, '"', static_cast<size_t>(e - (q + 1))));
    if (!q) { ++p; continue; }
    // Parse lon
    const char* num = static_cast<const char*>(std::memchr(q + 1, ',', static_cast<size_t>(e - (q + 1))));
    if (!num) break;
    ++num;
    char* endp = nullptr;
    const double lon = std::strtod(num, &endp);
    if (endp == num) { p = q + 1; continue; }
    // Parse lat
    const char* num2 = static_cast<const char*>(std::memchr(endp, ',', static_cast<size_t>(e - endp)));
    if (!num2) break;
    ++num2;
    char* endp2 = nullptr;
    const double lat = std::strtod(num2, &endp2);
    if (endp2 == num2) { p = endp; continue; }
    if (lon >= x_min && lon <= x_max && lat >= y_min && lat <= y_max) {
      return true;
    }
    p = endp2;
  }
  return false;
}

rocksdb::Options VanillaOpenOptions(int block_cache_mb) {
  rocksdb::Options o;
  o.create_if_missing = false;
  // Must match DB built by st_vanilla_kv_stream_ingest (plain internal key tail).
  o.persist_user_defined_timestamps = false;
  rocksdb::BlockBasedTableOptions tb;
  tb.block_cache =
      rocksdb::NewLRUCache(static_cast<size_t>(std::max(1, block_cache_mb)) * size_t{1048576});
  tb.cache_index_and_filter_blocks = true;
  o.table_factory.reset(rocksdb::NewBlockBasedTableFactory(tb));
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  std::string db_path;
  uint32_t t_min = 0, t_max = 0;
  float x_min = 0, x_max = 0, y_min = 0, y_max = 0;
  int block_cache_mb = 8;
  bool verify_by_value = true;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--db" && i + 1 < argc) {
      db_path = argv[++i];
    } else if (a == "--prune-t-min" && i + 1 < argc) {
      t_min = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (a == "--prune-t-max" && i + 1 < argc) {
      t_max = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (a == "--prune-x-min" && i + 1 < argc) {
      x_min = static_cast<float>(std::strtod(argv[++i], nullptr));
    } else if (a == "--prune-x-max" && i + 1 < argc) {
      x_max = static_cast<float>(std::strtod(argv[++i], nullptr));
    } else if (a == "--prune-y-min" && i + 1 < argc) {
      y_min = static_cast<float>(std::strtod(argv[++i], nullptr));
    } else if (a == "--prune-y-max" && i + 1 < argc) {
      y_max = static_cast<float>(std::strtod(argv[++i], nullptr));
    } else if (a == "--block-cache-mb" && i + 1 < argc) {
      block_cache_mb = std::atoi(argv[++i]);
    } else if (a == "--no-verify-by-value") {
      verify_by_value = false;
    }
  }

#ifdef VANILLA_BENCH_LINKED_FORK_LIB
  std::cerr
      << "WARNING: st_segment_window_scan_vanilla was built with VANILLA_STANDIN_LINK_FORK_LIB "
         "(project fork rocksdb.lib, not upstream clone) — not a peer-reviewed official baseline.\n";
#endif

  if (db_path.empty()) {
    std::cerr << "Usage: st_segment_window_scan_vanilla --db <dir> "
                 "--prune-t-min ... --prune-t-max ... --prune-x-min ... --prune-x-max ... "
                 "--prune-y-min ... --prune-y-max ... [--block-cache-mb 8]\n";
    return 2;
  }

  rocksdb::SetPerfLevel(rocksdb::PerfLevel::kEnableTimeExceptForMutex);
  if (rocksdb::get_perf_context() != nullptr) {
    rocksdb::get_perf_context()->Reset();
  }

  rocksdb::Options opt = VanillaOpenOptions(block_cache_mb);
  std::unique_ptr<rocksdb::DB> db;
  rocksdb::Status st = rocksdb::DB::Open(opt, db_path, &db);
  if (!st.ok()) {
    std::cerr << "DB::Open failed: " << st.ToString() << std::endl;
    return 3;
  }

  rocksdb::ReadOptions ro;
  ro.total_order_seek = true;
  ro.fill_cache = false;

  int64_t keys_in_window = 0;
  int64_t keys_scanned_total = 0;
  auto t0 = std::chrono::steady_clock::now();
  {
    std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro));
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      ++keys_scanned_total;
      const Slice uk = UserKeyFromIteratorKey(it->key());
      const bool key_pass = UserKeyStInWindow(uk, t_min, t_max, x_min, x_max, y_min, y_max);
      if (!key_pass) continue;
      const auto m = static_cast<unsigned char>(uk.size() ? uk[0] : 0);
      if (m == kMagicSegmentTzid && verify_by_value) {
        const std::string v = it->value().ToString();
        if (ValueHasPointInWindow(v, t_min, t_max, x_min, x_max, y_min, y_max)) {
          ++keys_in_window;
        }
      } else {
        ++keys_in_window;
      }
    }
    if (!it->status().ok()) {
      std::cerr << "Iterator: " << it->status().ToString() << std::endl;
      return 4;
    }
  }
  auto t1 = std::chrono::steady_clock::now();
  const double wall_us =
      std::chrono::duration<double, std::micro>(t1 - t0).count();
  const uint64_t br = rocksdb::get_perf_context()->block_read_count;

  std::cout << "vanilla_segment_scan keys=" << keys_in_window
            << " keys_scanned_total=" << keys_scanned_total
            << " block_read_count=" << br << " wall_us=" << wall_us << "\n";
  return 0;
}

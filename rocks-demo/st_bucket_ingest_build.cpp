#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/metadata.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>

#include "segment_points_store.hpp"
#include "segment_value_codec.hpp"
#include "st_cross_bucket_ingest.hpp"
#include "st_segment_key.hpp"
#include "st_tman_locs_key.hpp"
#include "st_tman_st_key.hpp"
#include "trajectory_kv.hpp"

enum class UserKeySchema { kTmanSt, kTmanLocs, kE7 };

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

static void ClearLegacyManifestShardEnv() {
#if defined(_WIN32)
  _putenv_s("ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_ROUTE", "");
  _putenv_s("ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_BUCKET_WIDTH_SEC", "");
  _putenv_s("ROCKSDB_EXPERIMENTAL_MANIFEST_SYNC_FANOUT", "");
#else
  unsetenv("ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_ROUTE");
  unsetenv("ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_BUCKET_WIDTH_SEC");
  unsetenv("ROCKSDB_EXPERIMENTAL_MANIFEST_SYNC_FANOUT");
#endif
}

rocksdb::Options MakeDbOptions(bool create_if_missing,
                               bool disable_auto_compactions,
                               bool experimental_st_compaction,
                               uint32_t bucket_sec,
                               int max_level_bytes_mb = 0,
                               uint64_t block_size = 0) {
  rocksdb::Options o;
  o.create_if_missing = create_if_missing;
  o.disable_auto_compactions = disable_auto_compactions;
  o.experimental_st_compaction = experimental_st_compaction;
  if (bucket_sec > 0) {
    o.experimental_compaction_event_time_bucket_width = bucket_sec;
    if (experimental_st_compaction) {
      o.experimental_compaction_event_time_split = true;
      // Per-file time range cap: 120 days (bucket-span policy at lower
      // levels may cut tighter; this is the backstop for higher levels).
      o.experimental_compaction_event_time_max_span = 120 * 86400;
    }
  }
  if (experimental_st_compaction) {
    // Match VersionStorageInfo::CalculateBaseBytes static branch so ladder
    // thresholds agree with ST compaction scores.
    o.level_compaction_dynamic_level_bytes = false;
    o.max_bytes_for_level_base = 1ULL << 30;
    o.max_bytes_for_level_multiplier = 10;
    o.target_file_size_base = 64ULL << 20;
    o.target_file_size_multiplier = 1;
    // Do not drain every flushed memtable immediately.  ST ingest batches
    // several L0 files before the first L0->L1 compaction.
    o.level0_file_num_compaction_trigger = 4;
  }
  if (max_level_bytes_mb > 0) {
    o.max_bytes_for_level_base =
        static_cast<uint64_t>(max_level_bytes_mb) * 1048576ull;
  }
  rocksdb::BlockBasedTableOptions tb;
  tb.experimental_spatio_temporal_meta_in_index_value = true;
  tb.experimental_st_index_block_enable = true;
  tb.experimental_st_index_bucket_seconds = bucket_sec;
  if (block_size > 0) {
    tb.block_size = block_size;
  }
  o.table_factory.reset(rocksdb::NewBlockBasedTableFactory(tb));
  return o;
}

static uint64_t MultiplyUint64Cap(uint64_t a, uint64_t b) {
  if (a == 0 || b == 0) return 0;
  if (a > UINT64_MAX / b) return UINT64_MAX;
  return a * b;
}

// Mirrors VersionStorageInfo static branch: L0/L1 budget = base; L>1 chained.
static uint64_t StaticMaxBytesForLevel(const rocksdb::ColumnFamilyOptions& cf,
                                       int level) {
  if (level <= 1) return cf.max_bytes_for_level_base;
  uint64_t prev = cf.max_bytes_for_level_base;
  for (int i = 2; i <= level; ++i) {
    const uint64_t mul = static_cast<uint64_t>(cf.max_bytes_for_level_multiplier);
    uint64_t add = 1;
    const size_t idx = static_cast<size_t>(i - 1);
    if (idx < cf.max_bytes_for_level_multiplier_additional.size()) {
      add = cf.max_bytes_for_level_multiplier_additional[idx];
    }
    prev = MultiplyUint64Cap(prev, mul);
    prev = MultiplyUint64Cap(prev, add);
  }
  return prev;
}

static void PrintSstCountPerLevel(
    const std::vector<rocksdb::LiveFileMetaData>& live) {
  std::array<int, 8> cnt{};
  for (const auto& f : live) {
    if (f.level >= 0 && f.level < static_cast<int>(cnt.size())) {
      cnt[static_cast<size_t>(f.level)]++;
    }
  }
  std::cout << "sst_per_level:";
  for (size_t i = 0; i < cnt.size(); ++i) {
    if (cnt[i] > 0) {
      std::cout << " L" << i << "=" << cnt[i];
    }
  }
  std::cout << "\n";
}

static const rocksdb::LevelMetaData* FindLevelMeta(
    const rocksdb::ColumnFamilyMetaData& m, int level) {
  for (const auto& lv : m.levels) {
    if (lv.level == level) return &lv;
  }
  return nullptr;
}

static std::string FileNameForCompact(const rocksdb::SstFileMetaData& f) {
  if (!f.relative_filename.empty()) return f.relative_filename;
  if (!f.name.empty() && f.name[0] == '/') return f.name.substr(1);
  if (!f.name.empty()) return f.name;
  return std::string();
}

static rocksdb::Status StageDownNewestL0File(
    rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cf,
    uint64_t manifest_host_bucket = std::numeric_limits<uint64_t>::max());

// Mind.md L0: follow RocksDB file-count trigger (not L1+ 100% byte budget).
static rocksdb::Status DrainL0WhenAtCompactionTrigger(
    rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cf,
    const rocksdb::ColumnFamilyOptions& cf_opts, const char* reason,
    uint32_t max_rounds,
    uint64_t manifest_host_bucket = std::numeric_limits<uint64_t>::max()) {
  int trigger = cf_opts.level0_file_num_compaction_trigger;
  if (trigger < 1) {
    trigger = 1;
  }
  const size_t need_files = static_cast<size_t>(trigger);
  for (uint32_t round = 0; round < max_rounds; ++round) {
    rocksdb::ColumnFamilyMetaData cf_meta;
    db->GetColumnFamilyMetaData(cf, &cf_meta);
    const rocksdb::LevelMetaData* l0 = FindLevelMeta(cf_meta, 0);
    const size_t nf = (l0 != nullptr) ? l0->files.size() : 0;
    if (nf < need_files) {
      return rocksdb::Status::OK();
    }
    std::cout << "ST L0 drain (" << reason << "): files=" << nf
              << " trigger>=" << need_files << " round=" << round << "\n";
    rocksdb::Status s = StageDownNewestL0File(db, cf, manifest_host_bucket);
    if (!s.ok()) {
      return s;
    }
  }
  return rocksdb::Status::InvalidArgument(
      "ST L0 drain: round limit exceeded (L0 still at compaction trigger)");
}

static rocksdb::Status DrainL0AllFiles(
    rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cf, const char* reason,
    uint32_t max_rounds,
    uint64_t manifest_host_bucket = std::numeric_limits<uint64_t>::max()) {
  for (uint32_t round = 0; round < max_rounds; ++round) {
    rocksdb::ColumnFamilyMetaData cf_meta;
    db->GetColumnFamilyMetaData(cf, &cf_meta);
    const rocksdb::LevelMetaData* l0 = FindLevelMeta(cf_meta, 0);
    const size_t nf = (l0 != nullptr) ? l0->files.size() : 0;
    if (nf == 0) {
      return rocksdb::Status::OK();
    }
    std::cout << "ST L0 clear (" << reason << "): remaining_l0_files=" << nf
              << " round=" << round << "\n";
    rocksdb::Status s = StageDownNewestL0File(db, cf, manifest_host_bucket);
    if (!s.ok()) {
      return s;
    }
  }
  return rocksdb::Status::InvalidArgument(
      "ST L0 clear: round limit exceeded (L0 not empty)");
}

// Staged ST compaction (Mind.md):
// L>=1: compact whole level to L+1 when level_bytes >= MaxBytesForLevel(L).
// Callers drain L0 separately according to its count trigger, or fully at EOF.
// Uses CompactFiles (manual, synchronous) — no generic background draining.
static rocksdb::Status RunStCompactionFileLadder(
    rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cf,
    const rocksdb::Options& opts,
    uint64_t max_steps = 500000,
    uint64_t manifest_host_bucket = std::numeric_limits<uint64_t>::max()) {
  const rocksdb::ColumnFamilyOptions& cf_opts = opts;
  rocksdb::CompactionOptions copts;
  copts.output_file_size_limit = cf_opts.target_file_size_base;
  copts.st_manifest_host_bucket = manifest_host_bucket;
  const int max_level_index = static_cast<int>(cf_opts.num_levels) - 1;
  if (max_level_index < 1) {
    return rocksdb::Status::InvalidArgument("num_levels too small");
  }
  uint64_t steps = 0;

  while (++steps <= max_steps) {
    rocksdb::ColumnFamilyMetaData cf_meta;
    db->GetColumnFamilyMetaData(cf, &cf_meta);

    bool progressed = false;
    for (int lev = 1; lev < max_level_index; ++lev) {
      const rocksdb::LevelMetaData* lx = FindLevelMeta(cf_meta, lev);
      const uint64_t sz = (lx != nullptr) ? lx->size : 0;
      const size_t nf = (lx != nullptr) ? lx->files.size() : 0;
      if (nf == 0) continue;

      const uint64_t budget = StaticMaxBytesForLevel(cf_opts, lev);
      const uint64_t need = budget;
      if (sz < need) continue;

      std::vector<std::string> inputs;
      inputs.reserve(nf);
      for (const auto& f : lx->files) {
        std::string fn = FileNameForCompact(f);
        if (!fn.empty()) inputs.push_back(std::move(fn));
      }
      if (inputs.empty()) {
        return rocksdb::Status::Corruption("level has bytes but no filenames");
      }
      std::cout << "ST ladder: CompactFiles L" << lev << "->" << (lev + 1)
                << " inputs=" << inputs.size() << " level_bytes=" << sz
                << " threshold_bytes=" << need << " budget_bytes=" << budget
                << "\n";
      rocksdb::Status cs = db->CompactFiles(copts, cf, inputs, lev + 1);
      if (!cs.ok()) return cs;
      progressed = true;
      break;
    }

    if (!progressed) break;
  }

  if (max_steps > 0 && steps > max_steps) {
    return rocksdb::Status::InvalidArgument("ST compaction ladder: step limit exceeded");
  }
  return rocksdb::Status::OK();
}

// One-shot L1->L2 (or Lx->Lx+1) regardless of byte threshold — offline "L2 compact" round.
static rocksdb::Status ForceCompactWholeLevel(
    rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cf, int from_level,
    uint64_t manifest_host_bucket = std::numeric_limits<uint64_t>::max()) {
  if (from_level < 1) {
    return rocksdb::Status::InvalidArgument("from_level must be >= 1");
  }
  rocksdb::ColumnFamilyMetaData cf_meta;
  db->GetColumnFamilyMetaData(cf, &cf_meta);
  const rocksdb::LevelMetaData* lx = FindLevelMeta(cf_meta, from_level);
  if (lx == nullptr || lx->files.empty()) {
    std::cout << "ST force: L" << from_level << " empty, skip\n";
    return rocksdb::Status::OK();
  }
  std::vector<std::string> inputs;
  inputs.reserve(lx->files.size());
  for (const auto& f : lx->files) {
    std::string fn = FileNameForCompact(f);
    if (!fn.empty()) {
      inputs.push_back(std::move(fn));
    }
  }
  if (inputs.empty()) {
    return rocksdb::Status::Corruption("level has files but no filenames");
  }
  rocksdb::CompactionOptions copts;
  copts.output_file_size_limit =
      db->GetOptions(cf).target_file_size_base;
  copts.st_manifest_host_bucket = manifest_host_bucket;
  std::cout << "ST force: CompactFiles L" << from_level << "->" << (from_level + 1)
            << " inputs=" << inputs.size() << " level_bytes=" << lx->size << "\n";
  return db->CompactFiles(copts, cf, inputs, from_level + 1);
}

struct Kv {
  std::string key;
  std::string val;
};

uint64_t Fnv1a64SegmentId(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

bool ParseUnsignedDecimal(const std::string& text, uint64_t* out) {
  if (out == nullptr || text.empty()) {
    return false;
  }
  uint64_t value = 0;
  for (char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    value = value * 10 + static_cast<uint64_t>(ch - '0');
  }
  *out = value;
  return true;
}

void ParseSegmentIdForE7(const std::string& segment_id, uint64_t* vehicle_id,
                         uint32_t* segment_seq, uint16_t* chunk_seq) {
  if (vehicle_id == nullptr || segment_seq == nullptr || chunk_seq == nullptr) {
    return;
  }
  *vehicle_id = Fnv1a64SegmentId(segment_id);
  *segment_seq = 0;
  *chunk_seq = 0;

  // ── Extract phone / vehicle-id from between the last two colons ──
  //   wuxi:0086188613520182:000001_0  → phone=0086188613520182
  //   tdrive:3644:3644_1              → phone=3644
  //   geolife:000:000000              → phone=000
  //   cd:hexhash:000001               → phone=hexhash (non-decimal → skip)
  const size_t last_colon = segment_id.rfind(':');
  if (last_colon != std::string::npos) {
    const size_t prev_colon = segment_id.rfind(':', last_colon - 1);
    if (prev_colon != std::string::npos) {
      const std::string phone =
          segment_id.substr(prev_colon + 1, last_colon - prev_colon - 1);
      uint64_t parsed_phone = 0;
      if (ParseUnsignedDecimal(phone, &parsed_phone)) {
        *vehicle_id = parsed_phone;
      }
    }
  }

  // ── Parse tail (after last colon) for segment_seq + chunk ──
  const std::string tail =
      (last_colon == std::string::npos) ? segment_id
                                        : segment_id.substr(last_colon + 1);
  const size_t hash = tail.find('#');
  const std::string main_part = tail.substr(0, hash);
  const std::string chunk_part =
      (hash == std::string::npos) ? std::string() : tail.substr(hash + 1);

  //   wuxi:phone:000001_0  → main_part="000001_0"
  //     underscore splits into seq=1, chunk=0 (keep 0 — key distinctness)
  //   tdrive:3644:3644_1   → main_part="3644_1"
  //     underscore splits: seq=3644, chunk=1
  bool chunk_parsed = false;
  const size_t underscore = main_part.rfind('_');
  if (underscore != std::string::npos) {
    uint64_t parsed_seq = 0;
    if (ParseUnsignedDecimal(main_part.substr(0, underscore), &parsed_seq)) {
      *segment_seq = static_cast<uint32_t>(parsed_seq);
    }
    uint64_t parsed_chunk = 0;
    if (ParseUnsignedDecimal(main_part.substr(underscore + 1), &parsed_chunk)) {
      *chunk_seq = static_cast<uint16_t>(parsed_chunk);
      chunk_parsed = true;
    }
  } else if (!main_part.empty()) {
    // No underscore: entire tail is segment_seq (e.g. wuxi:phone:000001)
    uint64_t parsed_seq = 0;
    if (ParseUnsignedDecimal(main_part, &parsed_seq)) {
      *segment_seq = static_cast<uint32_t>(parsed_seq);
    }
  }
  if (chunk_part.size() >= 2 && chunk_part[0] == 'p') {
    uint64_t parsed_chunk = 0;
    if (ParseUnsignedDecimal(chunk_part.substr(1), &parsed_chunk)) {
      *chunk_seq = static_cast<uint16_t>(parsed_chunk);
      return;
    }
  }
  // Fallback: only normalize 0→1 when chunk was NOT parsed from data.
  if (*chunk_seq == 0 && !chunk_parsed) {
    *chunk_seq = 1;
  }
}

struct MetaRowFields {
  uint32_t t_start = 0;
  uint32_t t_end = 0;
  uint32_t point_count = 0;
  std::string segment_id;
  float x_min = 0;
  float x_max = 0;
  float y_min = 0;
  float y_max = 0;
  // Pre-computed E7 fields (optional, speeds up ingest)
  bool has_e7_precomputed = false;
  uint32_t e7_bucket_id = 0;
  uint64_t e7_zorder = 0;
  uint64_t e7_vehicle_id = 0;
  uint32_t e7_segment_seq = 0;
  uint16_t e7_chunk_seq = 0;
};

bool TryParseMetaLine(const std::string& line, MetaRowFields* out) {
  if (out == nullptr || line.empty()) {
    return false;
  }
  auto f = SplitAuto(line);
  if (f.size() < 9) {
    return false;
  }
  out->segment_id = f[0];
  out->point_count = static_cast<uint32_t>(std::stoul(f[2]));
  out->t_start = static_cast<uint32_t>(std::stoll(f[3]));
  out->t_end = static_cast<uint32_t>(std::stoll(f[4]));
  out->x_min = static_cast<float>(std::stod(f[5]));
  out->x_max = static_cast<float>(std::stod(f[6]));
  out->y_min = static_cast<float>(std::stod(f[7]));
  out->y_max = static_cast<float>(std::stod(f[8]));
  // Parse pre-computed E7 fields if available (columns 10-14)
  if (f.size() >= 15) {
    out->has_e7_precomputed = true;
    out->e7_bucket_id     = static_cast<uint32_t>(std::stoul(f[10]));
    out->e7_zorder        = std::stoull(f[11]);
    out->e7_vehicle_id    = std::stoull(f[12]);
    out->e7_segment_seq   = static_cast<uint32_t>(std::stoul(f[13]));
    out->e7_chunk_seq     = static_cast<uint16_t>(std::stoul(f[14]));
  }
  return true;
}

bool PeekRowMinHostBucket(const std::string& line, uint32_t bucket_sec,
                          uint32_t* min_host_bucket) {
  MetaRowFields row;
  if (!TryParseMetaLine(line, &row) || min_host_bucket == nullptr ||
      bucket_sec == 0) {
    return false;
  }
  *min_host_bucket = st_cross_bucket::CanonicalBucketId(row.t_start, bucket_sec);
  return true;
}

uint32_t RowMinHostBucket(const MetaRowFields& row, uint32_t bucket_sec) {
  return st_cross_bucket::CanonicalBucketId(row.t_start, bucket_sec);
}

uint32_t RowMaxHostBucket(const MetaRowFields& row, uint32_t bucket_sec) {
  return st_cross_bucket::CanonicalBucketId(row.t_start, bucket_sec);
}

rocksdb::Status BuildKvsForMetaRow(
    const MetaRowFields& row,
    const std::vector<segval::SegmentPoint>& points, uint32_t bucket_sec,
    UserKeySchema user_key_schema,
    const st_tman::StKeyBuildConfig* st_key_cfg,
    const st_tman::TmanLocsKeyTable* tman_locs_keys, std::vector<Kv>* kvs) {
  if (kvs == nullptr) {
    return rocksdb::Status::InvalidArgument("null kvs");
  }
  kvs->clear();
  if (points.size() != static_cast<size_t>(row.point_count)) {
    return rocksdb::Status::InvalidArgument("point_count mismatch segment_id=" +
                                          row.segment_id);
  }
  segval::SegmentValueHeader head{};
  head.t_start = row.t_start;
  head.t_end = row.t_end;
  head.x_min = row.x_min;
  head.y_min = row.y_min;
  head.x_max = row.x_max;
  head.y_max = row.y_max;
  head.point_count = row.point_count;

  uint64_t vehicle_id = 0;
  uint32_t segment_seq = 0;
  uint16_t chunk_seq = 0;
  uint64_t zorder_start = 0;
  if (row.has_e7_precomputed) {
    // Use pre-computed E7 fields from meta CSV — skip string parsing + points read
    vehicle_id  = row.e7_vehicle_id;
    segment_seq = row.e7_segment_seq;
    chunk_seq   = row.e7_chunk_seq;
    zorder_start = row.e7_zorder;
  } else {
    ParseSegmentIdForE7(row.segment_id, &vehicle_id, &segment_seq, &chunk_seq);
    zorder_start = MakeZOrderStart(points);
  }

  const std::string encoded = st_cross_bucket::EncodeFullSegmentValue(head, points);
  if (encoded.empty()) {
    return rocksdb::Status::Corruption("EncodeSegmentValueV2 failed");
  }

  Kv kv;
  if (user_key_schema == UserKeySchema::kTmanLocs) {
    if (tman_locs_keys == nullptr) {
      return rocksdb::Status::InvalidArgument("tman_locs keys table missing");
    }
    if (!tman_locs_keys->BuildRowKey(row.segment_id, &kv.key)) {
      return rocksdb::Status::NotFound("no TMan LocS key for segment_id=" +
                                       row.segment_id);
    }
  } else if (user_key_schema == UserKeySchema::kTmanSt) {
    if (st_key_cfg == nullptr) {
      return rocksdb::Status::InvalidArgument("st_key_cfg missing");
    }
    kv.key = st_tman::BuildNativeStRowKey(row.t_start, points, row.segment_id,
                                          *st_key_cfg);
    if (kv.key.empty()) {
      return rocksdb::Status::InvalidArgument("bad segment_id for ST key: " +
                                              row.segment_id);
    }
  } else {
    kv.key = st_cross_bucket::SegmentKeyCanonical(row.t_start, zorder_start, vehicle_id,
                                                  segment_seq, chunk_seq, bucket_sec);
  }
  kv.val = encoded;
  kvs->push_back(std::move(kv));
  return rocksdb::Status::OK();
}

// Stage-down only the newest L0 SST (the one just flushed). Avoids scanning/compacting
// every L0 file on each bucket window (manifest-shard + hundreds of L0s -> crashes).
static rocksdb::Status StageDownNewestL0File(
    rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cf,
    uint64_t manifest_host_bucket) {
  rocksdb::ColumnFamilyMetaData cf_meta;
  db->GetColumnFamilyMetaData(cf, &cf_meta);
  const rocksdb::LevelMetaData* l0 = FindLevelMeta(cf_meta, 0);
  if (l0 == nullptr || l0->files.empty()) {
    return rocksdb::Status::OK();
  }
  uint64_t best_num = 0;
  std::string best_fn;
  for (const auto& f : l0->files) {
    std::string fn = FileNameForCompact(f);
    if (fn.empty()) {
      continue;
    }
    uint64_t num = 0;
    const size_t slash = fn.find_last_of('/');
    const std::string base =
        (slash == std::string::npos) ? fn : fn.substr(slash + 1);
    if (base.size() > 4 && base.substr(base.size() - 4) == ".sst") {
      try {
        num = std::stoull(base.substr(0, base.size() - 4));
      } catch (...) {
        continue;
      }
    }
    if (num >= best_num) {
      best_num = num;
      best_fn = std::move(fn);
    }
  }
  if (best_fn.empty()) {
    return rocksdb::Status::OK();
  }
  rocksdb::CompactionOptions stage_opts;
  stage_opts.output_file_size_limit =
      db->GetOptions(cf).target_file_size_base;
  // Permit a metadata-only move when RocksDB's overlap/compression checks
  // allow it. This flag does not bypass merging with overlapping L1 files.
  stage_opts.allow_trivial_move = true;
  stage_opts.st_manifest_host_bucket = manifest_host_bucket;
  std::cout << "ST incremental: StageDown newest L0 file " << best_fn
            << " -> L1 (allow_trivial_move=true"
            << (manifest_host_bucket != std::numeric_limits<uint64_t>::max()
                    ? ", manifest_host_bucket=" +
                          std::to_string(manifest_host_bucket)
                    : "")
            << ")\n";
  return db->CompactFiles(stage_opts, cf, {best_fn}, 1);
}

// L0-only memtable flush when unified ingest fills the active memtable (no ST ladder).
static rocksdb::Status FlushMemtableIfPressure(
    rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cf, uint64_t manifest_host_bucket,
    uint64_t active_memtable_bytes_threshold, bool stage_down_newest_l0) {
  if (active_memtable_bytes_threshold == 0) {
    return rocksdb::Status::OK();
  }
  uint64_t mem = 0;
  if (!db->GetIntProperty(cf, rocksdb::DB::Properties::kCurSizeActiveMemTable, &mem)) {
    return rocksdb::Status::OK();
  }
  if (mem < active_memtable_bytes_threshold) {
    return rocksdb::Status::OK();
  }
  rocksdb::FlushOptions fo;
  fo.wait = true;
  fo.st_manifest_host_bucket = manifest_host_bucket;
  std::cout << "ST incremental: memtable pressure flush active_mem=" << mem
            << " threshold=" << active_memtable_bytes_threshold << "\n";
  rocksdb::Status s = db->Flush(fo);
  if (!s.ok()) {
    return s;
  }
  if (stage_down_newest_l0) {
    return StageDownNewestL0File(db, cf, manifest_host_bucket);
  }
  return rocksdb::Status::OK();
}

static size_t CountLiveL0Files(rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cf) {
  rocksdb::ColumnFamilyMetaData cf_meta;
  db->GetColumnFamilyMetaData(cf, &cf_meta);
  const rocksdb::LevelMetaData* l0 = FindLevelMeta(cf_meta, 0);
  return (l0 != nullptr) ? l0->files.size() : 0;
}

static rocksdb::Status VerifyIngestL0Empty(rocksdb::DB* db,
                                           rocksdb::ColumnFamilyHandle* cf) {
  const size_t l0_n = CountLiveL0Files(db, cf);
  if (l0_n == 0) {
    return rocksdb::Status::OK();
  }
  return rocksdb::Status::InvalidArgument(
      "ingest finished with L0 files=" + std::to_string(l0_n) +
      " (expected 0 after DrainL0AllFiles)");
}

static rocksdb::Status FlushAndMaybeRunStLadder(
    rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cf,
    const rocksdb::Options& opts, bool experimental_st_compaction,
    const char* reason, uint64_t ladder_max_steps, bool /*newest_l0_only*/,
    bool flush_only = false, uint64_t manifest_host_bucket =
        std::numeric_limits<uint64_t>::max()) {
  const auto effective_opts = db->GetOptions(cf);
  rocksdb::FlushOptions fo;
  fo.wait = effective_opts.disable_auto_compactions;
  fo.st_manifest_host_bucket = manifest_host_bucket;
  rocksdb::Status s = db->Flush(fo);
  if (!s.ok()) {
    return s;
  }
  if (flush_only) {
    std::cout << "ST incremental: Flush (" << reason << ") only (no CompactFiles)\n";
    return rocksdb::Status::OK();
  }
  if (!experimental_st_compaction) {
    return rocksdb::Status::OK();
  }
  if (!effective_opts.disable_auto_compactions) {
    return rocksdb::Status::OK();  // Background picker owns compaction.
  }
  const auto& cf_opts =
      static_cast<const rocksdb::ColumnFamilyOptions&>(effective_opts);
  s = DrainL0WhenAtCompactionTrigger(db, cf, cf_opts, reason,
                                     /*max_rounds=*/32, manifest_host_bucket);
  if (!s.ok()) {
    return s;
  }
  if (ladder_max_steps == 0) {
    return rocksdb::Status::OK();
  }
  // L1+ capacity work; sub-threshold L0 files remain until a later trigger/EOF.
  std::cout << "ST incremental: after Flush+L0 drain (" << reason
            << ") -> L1+ ladder max_steps=" << ladder_max_steps << "\n";
  return RunStCompactionFileLadder(db, cf, effective_opts, ladder_max_steps,
                                 manifest_host_bucket);
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
  bool experimental_st_compaction = false;
  bool stream_ingest = false;
  bool stream_ingest_flush_only = false;
  bool ingest_per_bucket_flush = false;
  bool batch_ingest = false;
  bool st_ladder_drain_only = false;
  bool force_l1_to_l2 = false;
  bool stream_flush_every_n_rows = false;
  bool stream_incremental_compact = false;
  int64_t flush_every_n_rows = 0;
  int ingest_threads = 4;
  bool st_l1_l2_tiered = false;
  bool st_real_tiered = false;
  int st_tiered_max_level = 0;
  double st_level_trigger_ratio = 0.8;
  std::vector<std::pair<std::string, std::string>> st_custom_opts;
  int max_level_bytes_mb = 0;
  uint64_t block_size = 0;
  int64_t max_rows = -1;
  UserKeySchema user_key_schema = UserKeySchema::kTmanSt;
  st_tman::StKeyBuildConfig st_key_cfg;
  std::string tman_st_keys_csv;
  st_tman::TmanLocsKeyTable tman_locs_keys;

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
    } else if (a == "--experimental-st-compaction") {
      experimental_st_compaction = true;
    } else if (a == "--st-l1-l2-tiered") {
      st_l1_l2_tiered = true;
    } else if (a == "--st-real-tiered") {
      st_real_tiered = true;
    } else if (a == "--st-tiered-max-level" && i + 1 < argc) {
      st_tiered_max_level = std::stoi(argv[++i]);
    } else if (a == "--st-set-opt" && i + 1 < argc) {
      std::string kv = argv[++i];
      size_t eq = kv.find('=');
      if (eq != std::string::npos) {
        st_custom_opts.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
      }
    } else if (a == "--st-level-trigger-ratio" && i + 1 < argc) {
      st_level_trigger_ratio = std::stod(argv[++i]);
    } else if (a == "--max-level-bytes-mb" && i + 1 < argc) {
      max_level_bytes_mb = std::stoi(argv[++i]);
    } else if (a == "--block-size" && i + 1 < argc) {
      block_size = std::stoull(argv[++i]);
    } else if (a == "--stream-ingest") {
      stream_ingest = true;
    } else if (a == "--ingest-threads" && i + 1 < argc) {
      ingest_threads = std::stoi(argv[++i]);
    } else if (a == "--stream-ingest-flush-only") {
      stream_ingest_flush_only = true;
    } else if (a == "--per-bucket-flush") {
      ingest_per_bucket_flush = true;
    } else if (a == "--batch-ingest") {
      batch_ingest = true;
    } else if (a == "--st-ladder-drain-only") {
      st_ladder_drain_only = true;
    } else if (a == "--force-l1-to-l2") {
      force_l1_to_l2 = true;
    } else if (a == "--flush-every-n-rows" && i + 1 < argc) {
      stream_flush_every_n_rows = true;
      flush_every_n_rows = std::stoll(argv[++i]);
    } else if (a == "--stream-incremental-compact") {
      stream_incremental_compact = true;
    } else if (a == "--user-key-schema" && i + 1 < argc) {
      const std::string schema = argv[++i];
      if (schema == "e7") {
        user_key_schema = UserKeySchema::kE7;
      } else if (schema == "tman_st") {
        user_key_schema = UserKeySchema::kTmanSt;
      } else if (schema == "tman_locs") {
        user_key_schema = UserKeySchema::kTmanLocs;
      } else {
        std::cerr << "Invalid --user-key-schema (expected tman_st|tman_locs|e7)\n";
        return 2;
      }
    } else if (a == "--tman-st-keys-csv" && i + 1 < argc) {
      tman_st_keys_csv = argv[++i];
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
             "  [--experimental-st-compaction]\n"
             "  [--st-level-trigger-ratio 0.8]  same-level merge trigger after upper-level SST arrival\n"
             "      ST ladder: synchronous CompactFiles (L0 per-file -> L1; Lx->Lx+1 when\n"
             "      level_bytes >= MaxBytesForLevel). Default: one memtable stream/batch,\n"
             "      EOF Flush + final ST L0 drain + L1+ ladder (no per-calendar-bucket SST).\n"
             "  [--stream-ingest]  row-by-row Put; default EOF unified flush (not per bucket)\n"
             "  [--ingest-threads 4]  bounded parallel trajectory encode/write workers\n"
             "  [--stream-ingest-flush-only]  Flush only at EOF; ST ladder at EOF only\n"
             "  [--per-bucket-flush]  legacy: Flush+ST after each calendar bucket window\n"
             "  [--batch-ingest]   explicit batch mode (default if neither stream flag)\n"
             "  [--user-key-schema tman_st|tman_locs|e7]  default tman_st\n"
             "  [--tman-st-keys-csv <segments_st_key.csv>]  required for tman_locs (LocSIndex from TMan-tsp)\n"
             "  [--st-ladder-drain-only]  open existing --target-db; L0 drain + L1+ ladder only\n"
             "  [--force-l1-to-l2]  with --st-ladder-drain-only: one L1->L2 CompactFiles first\n"
             "      L0 drain uses level0_file_num_compaction_trigger (Mind.md; not L1 byte capacity).\n"
             "      disable_auto_compactions stays true (no generic background drain).\n";
      return 0;
    } else {
      std::cerr << "Unknown arg: " << a << "\n";
      return 1;
    }
  }

  if (target_db.empty()) {
    std::cerr << "Required: --target-db\n";
    return 1;
  }
  if (st_ladder_drain_only) {
    if (!experimental_st_compaction) {
      std::cerr << "--st-ladder-drain-only requires --experimental-st-compaction\n";
      return 1;
    }
  } else if (csv_path.empty() || points_csv_path.empty() || out_sst_dir.empty()) {
    std::cerr << "Required: --segment-meta-csv --segment-points-csv --out-sst-dir "
                 "--target-db\n";
    return 1;
  }
  if (bucket_sec == 0) {
    std::cerr << "--bucket-sec must be > 0\n";
    return 1;
  }
  if (ingest_threads < 1 || ingest_threads > 32) {
    std::cerr << "--ingest-threads must be between 1 and 32\n";
    return 1;
  }
  if (stream_flush_every_n_rows && flush_every_n_rows <= 0) {
    std::cerr << "--flush-every-n-rows must be > 0\n";
    return 1;
  }
  if (stream_incremental_compact && !stream_ingest) {
    std::cerr << "--stream-incremental-compact requires --stream-ingest\n";
    return 1;
  }
  if (stream_incremental_compact && !stream_flush_every_n_rows) {
    std::cerr << "--stream-incremental-compact requires --flush-every-n-rows\n";
    return 1;
  }
  ClearLegacyManifestShardEnv();
  std::cout << "Manifest: single MANIFEST-* (CURRENT -> MANIFEST, per-SST st_file_meta)\n"
            << std::flush;
  if (user_key_schema == UserKeySchema::kTmanSt ||
      user_key_schema == UserKeySchema::kTmanLocs) {
    if (user_key_schema == UserKeySchema::kTmanLocs) {
      if (tman_st_keys_csv.empty()) {
        std::cerr << "--user-key-schema tman_locs requires --tman-st-keys-csv\n";
        return 1;
      }
      const rocksdb::Status load_st = tman_locs_keys.LoadCsv(tman_st_keys_csv);
      if (!load_st.ok()) {
        std::cerr << "Load tman ST keys CSV: " << load_st.ToString() << "\n";
        return 1;
      }
      std::cout << "User key: TMan LocSIndex [period][spatial][tid] from "
                << tman_st_keys_csv << " (n=" << tman_locs_keys.size() << ")\n"
                << std::flush;
    } else {
      std::cout << "User key: ST-RocksDB native [period][spatial][tid] "
                   "(C++ Morton centroid)\n"
                << std::flush;
    }
#ifdef _WIN32
    _putenv_s("ROCKSDB_ST_USER_KEY_SCHEMA", "tman_st");
#else
    setenv("ROCKSDB_ST_USER_KEY_SCHEMA", "tman_st", 1);
#endif
  } else {
#ifdef _WIN32
    _putenv_s("ROCKSDB_ST_USER_KEY_SCHEMA", "e7");
#else
    setenv("ROCKSDB_ST_USER_KEY_SCHEMA", "e7", 1);
#endif
    std::cout << "User key: E7 (legacy)\n" << std::flush;
  }
  if (ingest_per_bucket_flush) {
    std::cout << "WARN: --per-bucket-flush (legacy; can leave many L0 SSTs)\n";
  }

  if (!st_ladder_drain_only) {
    if (reset_out_dir && fs::exists(out_sst_dir)) {
      fs::remove_all(out_sst_dir);
    }
    fs::create_directories(out_sst_dir);
    if (reset_target_db && fs::exists(target_db)) {
      fs::remove_all(target_db);
    }
  }

  rocksdb::Options stage_opts = MakeDbOptions(
      !st_ladder_drain_only, false, experimental_st_compaction, bucket_sec,
      max_level_bytes_mb, block_size);
  stage_opts.max_background_jobs = std::max(4, ingest_threads);
  std::cout << "Opening staging DB (before points preload): " << target_db << "\n"
            << std::flush;
  std::unique_ptr<rocksdb::DB> stage_db;
  rocksdb::Status s = rocksdb::DB::Open(stage_opts, target_db, &stage_db);
  if (!s.ok()) {
    std::cerr << "Open target db failed: " << s.ToString() << "\n";
    return 1;
  }
  rocksdb::ColumnFamilyHandle* const cf = stage_db->DefaultColumnFamily();
  std::cout << "Staging DB open OK\n" << std::flush;
  // Mind.md ingest: L0 file-count drain, then bounded L1+ ladder (100% byte thresholds)
  // after each bucket boundary — not only at EOF.
  constexpr uint64_t kIngestPerBucketL1LadderSteps = 64;
  // Native RocksDB and tiered ST use background flush/compaction.
  // Non-tiered ST uses the synchronous ST ladder.
  const bool use_auto_compaction =
      !experimental_st_compaction ||
      (st_l1_l2_tiered && st_real_tiered);
  if (experimental_st_compaction) {
    std::unordered_map<std::string, std::string> st_opts;
    if (use_auto_compaction) {
      // Let RocksDB background compaction run — our picker is invoked
      // automatically via TryPickWholeLevelNonL0ST.
      st_opts["experimental_st_compaction"] = "true";
      st_opts["experimental_st_compaction_l1_l2_tiered"] = "true";
      st_opts["experimental_st_compaction_real_tiered_l1_l2"] = "true";
      st_opts["experimental_st_compaction_tiered_max_level"] =
          std::to_string(st_tiered_max_level);
      st_opts["experimental_st_compaction_level_trigger_ratio"] =
          std::to_string(st_level_trigger_ratio);
      st_opts["disable_auto_compactions"] = "false";
      st_opts["level0_file_num_compaction_trigger"] =
          std::to_string(stage_opts.level0_file_num_compaction_trigger);
    } else {
      st_opts["disable_auto_compactions"] = "true";
      st_opts["experimental_st_compaction"] = "true";
      if (st_l1_l2_tiered) {
        st_opts["experimental_st_compaction_l1_l2_tiered"] = "true";
        if (st_real_tiered) {
          st_opts["experimental_st_compaction_real_tiered_l1_l2"] = "true";
        }
      }
      if (st_real_tiered) {
        st_opts["experimental_st_compaction_level_trigger_ratio"] =
            std::to_string(st_level_trigger_ratio);
      }
    }
    for (const auto& [k, v] : st_custom_opts) {
      st_opts[k] = v;
    }
    s = stage_db->SetOptions(cf, st_opts);
    if (!s.ok()) {
      std::cerr << "SetOptions(ST at open) failed: " << s.ToString() << "\n";
      return 1;
    }
  }

  // Apply any custom options (e.g. for parameter sweeps)
  if (!st_custom_opts.empty()) {
    std::unordered_map<std::string, std::string> custom_map;
    for (const auto& [k, v] : st_custom_opts) {
      custom_map[k] = v;
      std::cout << "SetOption: " << k << "=" << v << "\n";
    }
    s = stage_db->SetOptions(cf, custom_map);
    if (!s.ok()) {
      std::cerr << "SetOptions(custom) failed: " << s.ToString() << "\n";
      return 1;
    }
  }

  if (experimental_st_compaction) {
    const auto effective_options = stage_db->GetOptions(cf);
    stage_opts = effective_options;
    std::cout << "ST capacity: base_bytes="
              << effective_options.max_bytes_for_level_base
              << " multiplier=" << effective_options.max_bytes_for_level_multiplier
              << " target_sst_bytes=" << effective_options.target_file_size_base
              << " write_buffer_bytes=" << effective_options.write_buffer_size
              << " level_trigger_ratio="
              << effective_options.experimental_st_compaction_level_trigger_ratio
              << " auto_compactions=" << !effective_options.disable_auto_compactions
              << " block_mbr_flush_enabled="
              << effective_options.experimental_st_block_mbr_flush_enable
              << " l0_trigger=" << effective_options.level0_file_num_compaction_trigger
              << "\n";
  } else {
    const auto effective_options = stage_db->GetOptions(cf);
    std::cout << "Compaction mode: RocksDB native; auto_compactions="
              << !effective_options.disable_auto_compactions
              << " write_buffer_bytes=" << effective_options.write_buffer_size
              << " target_sst_bytes=" << effective_options.target_file_size_base
              << " l0_trigger="
              << effective_options.level0_file_num_compaction_trigger
              << " block_mbr_flush_enabled="
              << effective_options.experimental_st_block_mbr_flush_enable
              << "\n";
  }

  if (st_ladder_drain_only) {
    const uint64_t host_bucket = std::numeric_limits<uint64_t>::max();
    if (force_l1_to_l2) {
      s = ForceCompactWholeLevel(stage_db.get(), cf, 1, host_bucket);
      if (!s.ok()) {
        std::cerr << "Force L1->L2 failed: " << s.ToString() << "\n";
        return 1;
      }
    } else {
      std::cout << "ST incremental: ladder-drain-only L0 clear + L1+ ladder\n";
      s = DrainL0AllFiles(stage_db.get(), cf, "offline_drain_l0",
                          /*max_rounds=*/100000, host_bucket);
      if (!s.ok()) {
        std::cerr << "DrainL0 failed: " << s.ToString() << "\n";
        return 1;
      }
      s = RunStCompactionFileLadder(stage_db.get(), cf, stage_opts,
                                    /*max_steps=*/500000, host_bucket);
      if (!s.ok()) {
        std::cerr << "ST ladder failed: " << s.ToString() << "\n";
        return 1;
      }
    }
    std::vector<rocksdb::LiveFileMetaData> live;
    stage_db->GetLiveFilesMetaData(&live);
    PrintSstCountPerLevel(live);
    s = stage_db->Close();
    if (!s.ok()) {
      std::cerr << "DB::Close failed: " << s.ToString() << "\n";
      return 1;
    }
    std::cout << "ST ladder-drain-only OK\n";
    return 0;
  }

  // Stream mode: preload meta (~tens of MB) before the ~GB points map to avoid
  // heap failure when allocating sorted_lines on a full RSS (16GB machines).
  std::vector<std::pair<uint32_t, std::string>> sorted_lines;
  std::ifstream meta_in;
  if (stream_ingest && stream_ingest_flush_only) {
    std::cout << "Stream ingest: --stream-ingest-flush-only (legacy: defer ST to EOF)\n";
  }
  const bool parallel_stream_ingest = stream_ingest &&
                                      !ingest_per_bucket_flush &&
                                      !stream_flush_every_n_rows &&
                                      !stream_incremental_compact;
  if (stream_ingest) {
    if (ingest_per_bucket_flush) {
      std::cout << "Ingest mode: stream (row-by-row Put; per-bucket Flush+ST; "
                   "L1+ ladder max_steps="
                << kIngestPerBucketL1LadderSteps << " per bucket)\n"
                << std::flush;
    } else {
      std::cout << "Ingest mode: stream ("
                << (parallel_stream_ingest
                        ? "parallel encode/write workers=" +
                              std::to_string(ingest_threads) +
                              "; WriteBatch=128 segments; "
                        : "serial encode/write; ")
                << "unified EOF Flush+ST; "
                << (use_auto_compaction ? "background flush+compaction"
                                        : "manual pressure flush at 192MiB");
      if (stream_flush_every_n_rows && flush_every_n_rows > 0) {
        std::cout << "; flush-every-n-rows=" << flush_every_n_rows;
      }
      std::cout << ")\n" << std::flush;
    }
    meta_in.open(csv_path);
    if (!meta_in) {
      std::cerr << "Cannot open: " << csv_path << "\n";
      return 1;
    }
    std::string header;
    if (!std::getline(meta_in, header)) {
      std::cerr << "Empty csv.\n";
      return 1;
    }
    sorted_lines.reserve(140000);
    std::string line;
    while (std::getline(meta_in, line)) {
      if (line.empty()) {
        continue;
      }
      MetaRowFields row;
      if (!TryParseMetaLine(line, &row)) {
        continue;
      }
      sorted_lines.emplace_back(row.t_start, std::move(line));
    }
    meta_in.close();
    std::sort(sorted_lines.begin(), sorted_lines.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::cout << "Stream: loaded " << sorted_lines.size()
              << " meta rows, sorted by t_start\n"
              << std::flush;
    if (sorted_lines.empty()) {
      std::cerr << "No rows parsed.\n";
      return 1;
    }
  } else {
    meta_in.open(csv_path);
    if (!meta_in) {
      std::cerr << "Cannot open: " << csv_path << "\n";
      return 1;
    }
    std::string header;
    if (!std::getline(meta_in, header)) {
      std::cerr << "Empty csv.\n";
      return 1;
    }
  }

  std::unordered_map<std::string, std::vector<segval::SegmentPoint>> by_segment;
  segval::SegmentPointsStore points_spill;
  const bool use_points_spill =
      fs::exists(points_csv_path) &&
      fs::file_size(points_csv_path) > (1024ull * 1024ull * 1024ull);
  if (use_points_spill) {
    std::cout << "Loading segment points CSV offset index (RSS cap): "
              << points_csv_path << "\n"
              << std::flush;
    if (!points_spill.BuildFromCsv(points_csv_path)) {
      return 1;
    }
  } else {
    std::cout << "Loading segment points CSV (full preload): " << points_csv_path
              << "\n"
              << std::flush;
    if (!segval::LoadSegmentPointsCsv(points_csv_path, &by_segment)) {
      return 1;
    }
    std::cout << "Loaded segment points: " << by_segment.size() << " segments\n"
              << std::flush;
  }

  auto lookup_points = [&](const std::string& segment_id,
                           std::vector<segval::SegmentPoint>* points) -> bool {
    if (use_points_spill) {
      return points_spill.Get(segment_id, points);
    }
    auto pit = by_segment.find(segment_id);
    if (pit == by_segment.end()) {
      return false;
    }
    *points = pit->second;
    return true;
  };

  int64_t rows = 0;
  uint64_t total_entries = 0;
  size_t bucket_flush_events = 0;
  std::vector<uint32_t> buckets_touched;

  auto persist_st_options = [&]() -> rocksdb::Status {
    if (!experimental_st_compaction) {
      return rocksdb::Status::OK();
    }
    std::unordered_map<std::string, std::string> cf_opts;
    cf_opts["disable_auto_compactions"] = "true";
    cf_opts["experimental_st_compaction"] = "true";
    return stage_db->SetOptions(cf, cf_opts);
  };

  auto final_auto_drain = [&]() -> rocksdb::Status {
    rocksdb::WaitForCompactOptions wait;
    wait.timeout = std::chrono::seconds(120);
    auto status = stage_db->WaitForCompact(wait);
    if (!status.ok()) return status;
    // Automatic L0 compaction starts at a file-count threshold. Drain the
    // sub-threshold tail explicitly, with no background compaction races.
    status = stage_db->SetOptions(cf, {{"disable_auto_compactions", "true"}});
    if (!status.ok()) return status;
    status = stage_db->WaitForCompact(wait);
    if (!status.ok()) return status;
    status = DrainL0AllFiles(stage_db.get(), cf, "eof_tail", 100000);
    if (!status.ok()) return status;
    status = stage_db->SetOptions(cf, {{"disable_auto_compactions", "false"}});
    if (!status.ok()) return status;
    status = stage_db->WaitForCompact(wait);
    if (!status.ok()) return status;
    return VerifyIngestL0Empty(stage_db.get(), cf);
  };

  auto wait_for_native_compaction = [&]() -> rocksdb::Status {
    rocksdb::WaitForCompactOptions wait;
    wait.timeout = std::chrono::minutes(10);
    return stage_db->WaitForCompact(wait);
  };

  auto final_st_drain = [&](uint64_t manifest_host_bucket) -> rocksdb::Status {
    if (!experimental_st_compaction) {
      return rocksdb::Status::OK();
    }
    std::cout << "ST incremental: final clear L0 then L1+ ladder\n";
    rocksdb::Status s = DrainL0AllFiles(stage_db.get(), cf, "eof_clear_l0",
                                        /*max_rounds=*/100000,
                                        manifest_host_bucket);
    if (!s.ok()) {
      return s;
    }
    return RunStCompactionFileLadder(stage_db.get(), cf, stage_opts,
                                   /*max_steps=*/500000, manifest_host_bucket);
  };

  if (stream_ingest) {
    const size_t n = sorted_lines.size();
    constexpr uint64_t kMemtablePressureBytes = 192ULL * 1024 * 1024;
    int64_t next_flush_at_row =
        stream_flush_every_n_rows && flush_every_n_rows > 0 ? flush_every_n_rows
                                                             : -1;
    uint32_t last_flush_bucket = UINT32_MAX;

    if (parallel_stream_ingest) {
      const size_t limit = max_rows >= 0
                               ? std::min(n, static_cast<size_t>(max_rows))
                               : n;
      const size_t worker_count = std::min<size_t>(ingest_threads, limit);
      std::atomic<size_t> next_index{0};
      std::atomic<int64_t> completed_rows{0};
      std::atomic<uint64_t> written_entries{0};
      std::atomic<bool> stop{false};
      std::mutex error_mutex;
      std::mutex progress_mutex;
      rocksdb::Status worker_error;
      const auto ingest_started = std::chrono::steady_clock::now();
      std::vector<std::thread> workers;
      std::vector<std::unordered_set<uint32_t>> worker_buckets(worker_count);
      workers.reserve(worker_count);
      for (size_t worker_id = 0; worker_id < worker_count; ++worker_id) {
        workers.emplace_back([&, worker_id]() {
          auto& local_buckets = worker_buckets[worker_id];
          std::ifstream points_input(points_csv_path, std::ios::binary);
          if (!points_input) {
            std::lock_guard<std::mutex> lock(error_mutex);
            if (worker_error.ok()) {
              worker_error = rocksdb::Status::IOError(
                  "cannot open points CSV in ingest worker");
            }
            stop.store(true, std::memory_order_relaxed);
            return;
          }
          rocksdb::WriteBatch batch;
          size_t batch_rows = 0;
          auto flush_batch = [&]() -> bool {
            if (batch_rows == 0) return true;
            const rocksdb::Status write_status = stage_db->Write(
                rocksdb::WriteOptions(), &batch);
            if (!write_status.ok()) {
              std::lock_guard<std::mutex> lock(error_mutex);
              if (worker_error.ok()) worker_error = write_status;
              stop.store(true, std::memory_order_relaxed);
              return false;
            }
            batch.Clear();
            batch_rows = 0;
            return true;
          };
          while (!stop.load(std::memory_order_relaxed)) {
            const size_t i = next_index.fetch_add(1, std::memory_order_relaxed);
            if (i >= limit) break;
            MetaRowFields row;
            if (!TryParseMetaLine(sorted_lines[i].second, &row)) continue;
            std::vector<segval::SegmentPoint> row_points;
            const bool found_points =
                use_points_spill
                    ? points_spill.Get(row.segment_id, &row_points,
                                       &points_input)
                    : lookup_points(row.segment_id, &row_points);
            if (!found_points) {
              std::lock_guard<std::mutex> lock(error_mutex);
              if (worker_error.ok()) {
                worker_error = rocksdb::Status::Corruption(
                    "cannot read points for segment_id=" + row.segment_id);
              }
              stop.store(true, std::memory_order_relaxed);
              break;
            }
            std::vector<Kv> row_kvs;
            const rocksdb::Status encode_status = BuildKvsForMetaRow(
                row, row_points, bucket_sec, user_key_schema, &st_key_cfg,
                &tman_locs_keys, &row_kvs);
            if (!encode_status.ok()) {
              std::lock_guard<std::mutex> lock(error_mutex);
              if (worker_error.ok()) worker_error = encode_status;
              stop.store(true, std::memory_order_relaxed);
              break;
            }
            for (const auto& kv : row_kvs) {
              const rocksdb::Status add_status = batch.Put(kv.key, kv.val);
              if (!add_status.ok()) {
                std::lock_guard<std::mutex> lock(error_mutex);
                if (worker_error.ok()) worker_error = add_status;
                stop.store(true, std::memory_order_relaxed);
                break;
              }
            }
            if (stop.load(std::memory_order_relaxed)) break;
            written_entries.fetch_add(row_kvs.size(), std::memory_order_relaxed);
            ++batch_rows;
            if (batch_rows >= 128 && !flush_batch()) break;
            local_buckets.insert(RowMinHostBucket(row, bucket_sec));
            const int64_t done = completed_rows.fetch_add(
                                     1, std::memory_order_relaxed) +
                                 1;
            if (done % 100000 == 0) {
              std::lock_guard<std::mutex> lock(progress_mutex);
              const double elapsed = std::chrono::duration<double>(
                  std::chrono::steady_clock::now() - ingest_started).count();
              std::cout << "Stream progress rows=" << done
                        << " total_entries="
                        << written_entries.load(std::memory_order_relaxed)
                        << " rows_per_sec=" << std::fixed
                        << std::setprecision(1)
                        << (elapsed > 0 ? done / elapsed : 0.0)
                        << " bucket_flushes=0\n"
                        << std::defaultfloat << std::flush;
            }
          }
          if (!stop.load(std::memory_order_relaxed)) flush_batch();
        });
      }
      for (auto& worker : workers) worker.join();
      if (!worker_error.ok()) {
        std::cerr << "Parallel stream ingest failed: "
                  << worker_error.ToString() << "\n";
        return 1;
      }
      rows = completed_rows.load(std::memory_order_relaxed);
      total_entries = written_entries.load(std::memory_order_relaxed);
      std::unordered_set<uint32_t> touched_buckets;
      for (const auto& local_buckets : worker_buckets) {
        touched_buckets.insert(local_buckets.begin(), local_buckets.end());
      }
      buckets_touched.assign(touched_buckets.begin(), touched_buckets.end());
      std::sort(buckets_touched.begin(), buckets_touched.end());
    } else {
    for (size_t i = 0; i < n; ++i) {
      if (max_rows >= 0 && rows >= max_rows) {
        break;
      }
      const std::string& line = sorted_lines[i].second;
      const bool more_rows = (i + 1 < n);

      MetaRowFields row;
      if (!TryParseMetaLine(line, &row)) {
        continue;
      }
      std::vector<segval::SegmentPoint> row_points;
      if (!lookup_points(row.segment_id, &row_points)) {
        std::cerr << "No points for segment_id=" << row.segment_id << "\n";
        return 1;
      }

      uint32_t next_min_b = UINT32_MAX;
      if (more_rows &&
          !PeekRowMinHostBucket(sorted_lines[i + 1].second, bucket_sec,
                                &next_min_b)) {
        next_min_b = UINT32_MAX;
      }

      std::vector<Kv> row_kvs;
      s = BuildKvsForMetaRow(row, row_points, bucket_sec, user_key_schema,
                             &st_key_cfg, &tman_locs_keys, &row_kvs);
      if (!s.ok()) {
        std::cerr << "BuildKvs failed segment_id=" << row.segment_id << ": "
                  << s.ToString() << "\n";
        return 1;
      }
      for (const auto& kv : row_kvs) {
        s = stage_db->Put(rocksdb::WriteOptions(), kv.key, kv.val);
        if (!s.ok()) {
          std::cerr << "staging Put failed: " << s.ToString() << "\n";
          return 1;
        }
      }
      total_entries += static_cast<uint64_t>(row_kvs.size());

      const uint32_t row_min_b =
          RowMinHostBucket(row, bucket_sec);
      const uint32_t row_max_b =
          RowMaxHostBucket(row, bucket_sec);
      // Flush when the next segment starts in a later host bucket (CSV sorted by
      // t_start). Use row_min_b, not row_max_b — cross-bucket replicas span far.
      if (buckets_touched.empty() ||
          std::find(buckets_touched.begin(), buckets_touched.end(), row_min_b) ==
              buckets_touched.end()) {
        buckets_touched.push_back(row_min_b);
      }
      const bool bucket_window_done = !more_rows || next_min_b > row_min_b;
      if (ingest_per_bucket_flush && bucket_window_done) {
        std::string reason = "bucket_start_" + std::to_string(row_min_b) + "_max_" +
                             std::to_string(row_max_b);
        s = FlushAndMaybeRunStLadder(
            stage_db.get(), cf, stage_opts, experimental_st_compaction,
            reason.c_str(), kIngestPerBucketL1LadderSteps,
            /*newest_l0_only=*/false, /*flush_only=*/stream_ingest_flush_only,
            /*manifest_host_bucket=*/static_cast<uint64_t>(row_min_b));
        if (!s.ok()) {
          std::cerr << "Flush/ST after bucket window failed: " << s.ToString()
                    << "\n";
          return 1;
        }
        ++bucket_flush_events;
        if (bucket_flush_events <= 5 || bucket_flush_events % 50 == 0) {
          std::cout << "Bucket flush at start_bucket=" << row_min_b
                    << " max_bucket=" << row_max_b
                    << " flush#" << bucket_flush_events << " row=" << rows
                    << " total_entries=" << total_entries << "\n";
        }
      } else if (!ingest_per_bucket_flush) {
        bool do_flush = false;
        std::string flush_reason;

        // Active intervention: flush every N rows to create many granular L1 SSTs.
        if (stream_flush_every_n_rows && rows >= next_flush_at_row) {
          do_flush = true;
          flush_reason = "row_count_" + std::to_string(next_flush_at_row);
          next_flush_at_row = rows + flush_every_n_rows;
        }

        // Automatic mode relies on RocksDB's write-buffer limit and background
        // flush. Do not poll and synchronously flush at a smaller hidden limit.
        if (!do_flush && !use_auto_compaction) {
          const uint64_t mem_before = [&]() {
            uint64_t m = 0;
            stage_db->GetIntProperty(
                cf, rocksdb::DB::Properties::kCurSizeActiveMemTable, &m);
            return m;
          }();
          if (mem_before >= kMemtablePressureBytes) {
            do_flush = true;
            flush_reason = "memtable_" + std::to_string(mem_before);
          }
        }

        if (do_flush) {
          auto t_flush_start = std::chrono::steady_clock::now();
          rocksdb::FlushOptions fo;
          fo.wait = !use_auto_compaction;
          fo.st_manifest_host_bucket = static_cast<uint64_t>(row_min_b);
          s = stage_db->Flush(fo);
          if (!s.ok()) {
            std::cerr << "Flush failed (" << flush_reason
                      << "): " << s.ToString() << "\n";
            return 1;
          }
          double flush_ms =
              std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - t_flush_start)
                  .count();

          double compact_ms = 0.0;
          size_t l0_before = CountLiveL0Files(stage_db.get(), cf);

          if (stream_incremental_compact && experimental_st_compaction &&
              !use_auto_compaction) {
            auto t_compact = std::chrono::steady_clock::now();
            {
              // ── Incremental compact: stage-down L0 + limited L1+ ladder ──
              const auto& cf_opts =
                  static_cast<const rocksdb::ColumnFamilyOptions&>(stage_opts);
              s = DrainL0WhenAtCompactionTrigger(
                  stage_db.get(), cf, cf_opts, "incr_l0_trigger",
                  /*max_rounds=*/32, static_cast<uint64_t>(row_min_b));
              if (s.ok()) {
                constexpr uint64_t kIncrementalLadderSteps = 8;
                s = RunStCompactionFileLadder(
                    stage_db.get(), cf, stage_opts,
                    kIncrementalLadderSteps, static_cast<uint64_t>(row_min_b));
              }
            }
            if (!s.ok()) {
              std::cerr << "Incremental ST compaction failed: " << s.ToString()
                        << "\n";
              return 1;
            }
            compact_ms =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t_compact)
                    .count();
          }

          ++bucket_flush_events;
          std::cout << "Flush #" << bucket_flush_events
                    << " reason=" << flush_reason
                    << " bucket=" << row_min_b
                    << " row=" << rows
                    << " flush_ms=" << std::fixed << std::setprecision(1) << flush_ms;
          if (stream_incremental_compact) {
            size_t l0_after = CountLiveL0Files(stage_db.get(), cf);
            std::cout << " compact_ms=" << compact_ms
                      << " l0_before=" << l0_before << " l0_after=" << l0_after;
          }
          std::cout << std::defaultfloat << "\n";
        }
      }
      if (rows > 0 && rows % 10000 == 0) {
        std::cout << "Stream progress rows=" << rows
                  << " total_entries=" << total_entries
                  << " bucket_flushes=" << bucket_flush_events << "\n";
      }
      sorted_lines[i].second.clear();
      sorted_lines[i].second.shrink_to_fit();
      ++rows;
    }
    }

    if (rows == 0) {
      std::cerr << "No rows parsed.\n";
      return 1;
    }
    sorted_lines.clear();
    sorted_lines.shrink_to_fit();
    const uint64_t eof_host_bucket =
        buckets_touched.empty()
            ? std::numeric_limits<uint64_t>::max()
            : static_cast<uint64_t>(*std::min_element(buckets_touched.begin(),
                                                        buckets_touched.end()));
    // ── EOF: flush remaining memtable, then compact ALL L0 files
    //     to L1 in one shot (bucket-span policy splits the output).
    {
      rocksdb::FlushOptions fo;
      fo.wait = true;
      fo.st_manifest_host_bucket = eof_host_bucket;
      s = stage_db->Flush(fo);
      if (!s.ok()) {
        std::cerr << "EOF Flush failed: " << s.ToString() << "\n";
        return 1;
      }
      ++bucket_flush_events;
    }

    if (experimental_st_compaction) {
      if (use_auto_compaction) {
        s = final_auto_drain();
        if (!s.ok()) {
          std::cerr << "EOF compaction/drain failed: " << s.ToString() << "\n";
          return 1;
        }
      } else {
        // Gather ALL live L0 files and compact them to L1 together.
        rocksdb::ColumnFamilyMetaData cf_meta;
        stage_db->GetColumnFamilyMetaData(cf, &cf_meta);
        const rocksdb::LevelMetaData* l0m = FindLevelMeta(cf_meta, 0);
        if (l0m != nullptr && !l0m->files.empty()) {
          std::vector<std::string> all_l0;
          all_l0.reserve(l0m->files.size());
          for (const auto& f : l0m->files) {
            std::string fn = FileNameForCompact(f);
            if (!fn.empty()) all_l0.push_back(std::move(fn));
          }
          std::cout << "EOF compact-all-L0: " << all_l0.size()
                    << " L0 files -> L1 (time-split enabled)\n";
          rocksdb::CompactionOptions copts;
          copts.output_file_size_limit = stage_opts.target_file_size_base;
          copts.st_manifest_host_bucket = eof_host_bucket;
          s = stage_db->CompactFiles(copts, cf, all_l0, /*output_level=*/1);
          if (!s.ok()) {
            std::cerr << "EOF CompactFiles L0->L1 failed: " << s.ToString() << "\n";
            return 1;
          }
        }
        // Now run L1+ ladder (L1→L2 with bucket-span splits).
        s = RunStCompactionFileLadder(stage_db.get(), cf, stage_opts,
                                      /*max_steps=*/500000, eof_host_bucket);
        if (!s.ok()) {
          std::cerr << "EOF L1+ ladder failed: " << s.ToString() << "\n";
          return 1;
        }
      }
    } else {
      s = wait_for_native_compaction();
      if (!s.ok()) {
        std::cerr << "EOF native compaction wait failed: " << s.ToString()
                  << "\n";
        return 1;
      }
    }
  } else {
    if (ingest_per_bucket_flush) {
      std::cout << "Ingest mode: batch (per-bucket Put+Flush+ST)\n";
    } else {
      std::cout << "Ingest mode: batch (unified sort+Put; EOF Flush+ST)\n";
    }
    std::unordered_map<uint32_t, std::vector<Kv>> bucket_kvs;
    std::string line;
    while (std::getline(meta_in, line)) {
      if (line.empty()) {
        continue;
      }
      if (max_rows >= 0 && rows >= max_rows) {
        break;
      }
      MetaRowFields row;
      if (!TryParseMetaLine(line, &row)) {
        continue;
      }
      std::vector<segval::SegmentPoint> row_points;
      if (!lookup_points(row.segment_id, &row_points)) {
        std::cerr << "No points for segment_id=" << row.segment_id << "\n";
        return 1;
      }
      std::vector<Kv> row_kvs;
      s = BuildKvsForMetaRow(row, row_points, bucket_sec, user_key_schema,
                             &st_key_cfg, &tman_locs_keys, &row_kvs);
      if (!s.ok()) {
        std::cerr << "BuildKvs failed segment_id=" << row.segment_id << ": "
                  << s.ToString() << "\n";
        return 1;
      }
      const uint32_t canonical_b =
          st_cross_bucket::CanonicalBucketId(row.t_start, bucket_sec);
      bucket_kvs[canonical_b].push_back(std::move(row_kvs[0]));
      ++rows;
    }
    meta_in.close();

    if (bucket_kvs.empty()) {
      std::cerr << "No rows parsed.\n";
      return 1;
    }

    buckets_touched.reserve(bucket_kvs.size());
    for (const auto& it : bucket_kvs) {
      buckets_touched.push_back(it.first);
    }
    std::sort(buckets_touched.begin(), buckets_touched.end());

    if (ingest_per_bucket_flush) {
      for (uint32_t b : buckets_touched) {
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
        std::string reason = "bucket_" + std::to_string(b);
        s = FlushAndMaybeRunStLadder(
            stage_db.get(), cf, stage_opts, experimental_st_compaction,
            reason.c_str(), kIngestPerBucketL1LadderSteps,
            /*newest_l0_only=*/false, /*flush_only=*/false,
            /*manifest_host_bucket=*/static_cast<uint64_t>(b));
        if (!s.ok()) {
          std::cerr << "Flush/ST failed bucket=" << b << ": " << s.ToString()
                    << "\n";
          return 1;
        }
        ++bucket_flush_events;
        total_entries += static_cast<uint64_t>(vec.size());
        std::cout << "Bucket " << b << " flushed entries=" << vec.size() << "\n";
      }
    } else {
      std::vector<Kv> all_kvs;
      size_t total_kv = 0;
      for (uint32_t b : buckets_touched) {
        total_kv += bucket_kvs[b].size();
      }
      all_kvs.reserve(total_kv);
      for (uint32_t b : buckets_touched) {
        auto& vec = bucket_kvs[b];
        for (auto& kv : vec) {
          all_kvs.push_back(std::move(kv));
        }
        vec.clear();
      }
      std::sort(all_kvs.begin(), all_kvs.end(),
                [](const Kv& a, const Kv& c) { return a.key < c.key; });
      for (const auto& kv : all_kvs) {
        s = stage_db->Put(rocksdb::WriteOptions(), kv.key, kv.val);
        if (!s.ok()) {
          std::cerr << "staging Put failed: " << s.ToString() << "\n";
          return 1;
        }
      }
      total_entries += static_cast<uint64_t>(all_kvs.size());
      const uint64_t eof_host_bucket =
          buckets_touched.empty()
              ? std::numeric_limits<uint64_t>::max()
              : static_cast<uint64_t>(buckets_touched.front());
      s = FlushAndMaybeRunStLadder(
          stage_db.get(), cf, stage_opts, experimental_st_compaction,
          "unified_batch_eof", kIngestPerBucketL1LadderSteps,
          /*newest_l0_only=*/false, /*flush_only=*/false,
          /*manifest_host_bucket=*/eof_host_bucket);
      if (!s.ok()) {
        std::cerr << "Unified batch Flush/ST failed: " << s.ToString() << "\n";
        return 1;
      }
      bucket_flush_events = 1;
      std::cout << "Unified batch flushed entries=" << all_kvs.size() << "\n";
    }
    if (experimental_st_compaction) {
      if (use_auto_compaction) {
        s = final_auto_drain();
        if (!s.ok()) {
          std::cerr << "EOF compaction/drain failed: " << s.ToString() << "\n";
          return 1;
        }
      } else {
        const uint64_t eof_host_bucket =
            buckets_touched.empty()
                ? std::numeric_limits<uint64_t>::max()
                : static_cast<uint64_t>(buckets_touched.front());
        s = final_st_drain(eof_host_bucket);
        if (!s.ok()) {
          std::cerr << "Final ST drain failed: " << s.ToString() << "\n";
          return 1;
        }
      }
    } else {
      s = wait_for_native_compaction();
      if (!s.ok()) {
        std::cerr << "EOF native compaction wait failed: " << s.ToString()
                  << "\n";
        return 1;
      }
    }
  }

  s = persist_st_options();
  if (!s.ok()) {
    std::cerr << "SetOptions(ST persist) failed: " << s.ToString() << "\n";
    return 1;
  }

  std::vector<rocksdb::LiveFileMetaData> live;
  stage_db->GetLiveFilesMetaData(&live);
  PrintSstCountPerLevel(live);
  if (experimental_st_compaction) {
    s = VerifyIngestL0Empty(stage_db.get(), cf);
    if (!s.ok()) {
      std::cerr << "Ingest L0 verify failed: " << s.ToString() << "\n";
      return 1;
    }
  }
  std::cout << "DONE mode=" << (stream_ingest ? "stream" : "batch")
            << (stream_ingest && stream_ingest_flush_only ? " flush_only=1" : "")
            << " buckets=" << buckets_touched.size()
            << " bucket_flush_events=" << bucket_flush_events << " rows=" << rows
            << " ingested_live_sst=" << live.size()
            << " total_entries=" << total_entries << "\n";

  s = stage_db->Close();
  if (!s.ok()) {
    std::cerr << "DB::Close failed: " << s.ToString() << "\n";
    return 1;
  }
  std::cout << "DB::Close OK\n";
  stage_db.reset();
  return 0;
}

#pragma once

#include <cstdlib>
#include <cstdint>
#include <string>

namespace ROCKSDB_NAMESPACE {

namespace {
inline uint32_t StEnvU32(const char* name, uint32_t def) {
  const char* v = std::getenv(name);
  if (v == nullptr || *v == '\0') return def;
  return static_cast<uint32_t>(std::stoul(v));
}
}  // namespace

// Mind.md (final): Level-aware ST compaction output policy.
//
// Physical compaction (SST layer) policy recap:
// - Level-aware relax for two constraint families:
//   1) Cross-bucket: unrestricted (no bucket-span output cut)
//   2) Time expansion: max_time_expansion[level]
//
// Hard vs soft constraints (enforced in CompactionOutputs):
// - Hard: time-span, max-keys (must cut)
// - Soft: bucket-span (may be gated by min_keys)
//
// This header centralizes the level-aware knobs used by both
// CompactionOutputs (output splitting) and the compaction picker preference
// logic, to avoid policy drift across call sites.
struct StEventSplitLevelPolicy {
  uint32_t max_bucket_span = UINT32_MAX;  // no cross-bucket output cut
  uint32_t effective_max_time_span = 0;   // seconds; derived from base max span
  uint32_t effective_max_keys = UINT32_MAX;  // per-file ST key count cap
};

// Level-aware SST size pyramid: upper levels → many small files (good prune
// locality); lower levels → few large files (sequential I/O). All dimensions
// scale together so output files naturally grow ~4× per level.
//
// Target example with 284K segments across 7535 daily buckets:
//   L1:  12000 keys,  64 buckets → ~24 SSTs, ~15.6MB each
//   L2:  20000 keys, 128 buckets → ~14 SSTs, ~26MB each
//   L3:  80000 keys, 256 buckets →  ~4 SSTs, ~64MB each (file-size bound)
//   L4+: no key cap,  512 buckets → ~1-2 SSTs, ~64MB each
inline StEventSplitLevelPolicy ComputeStEventSplitLevelPolicy(
    int output_level, uint32_t base_max_time_span) {
  StEventSplitLevelPolicy p{};

  // ── Per-file ST key cap (hard constraint, 4× per level) ──
  // Env overrides: ST_PYRAMID_L1_KEYS, ST_PYRAMID_L2_KEYS, ST_PYRAMID_L3_KEYS
  uint32_t l1_keys = StEnvU32("ST_PYRAMID_L1_KEYS", 12000);
  uint32_t l2_keys = StEnvU32("ST_PYRAMID_L2_KEYS", 20000);
  uint32_t l3_keys = StEnvU32("ST_PYRAMID_L3_KEYS", 80000);
  if (output_level <= 1) {
    p.effective_max_keys = l1_keys;
  } else if (output_level == 2) {
    p.effective_max_keys = l2_keys;
  } else if (output_level == 3) {
    p.effective_max_keys = l3_keys;
  } else {
    p.effective_max_keys = UINT32_MAX;  // L4+: no key-count split
  }

  // ── Cross-bucket span (soft constraint, 2× per level) ──
  // Env overrides: ST_PYRAMID_L1_BUCKETS, ST_PYRAMID_L2_BUCKETS, ST_PYRAMID_L3_BUCKETS
  uint32_t l1_buckets = StEnvU32("ST_PYRAMID_L1_BUCKETS", 64);
  uint32_t l2_buckets = StEnvU32("ST_PYRAMID_L2_BUCKETS", 128);
  uint32_t l3_buckets = StEnvU32("ST_PYRAMID_L3_BUCKETS", 256);
  if (output_level <= 1) {
    p.max_bucket_span = l1_buckets;
  } else if (output_level == 2) {
    p.max_bucket_span = l2_buckets;
  } else if (output_level == 3) {
    p.max_bucket_span = l3_buckets;
  } else {
    p.max_bucket_span = 512;
  }

  // ── Per-file time span (hard constraint; L4+ relaxes 2×) ──
  // Env override: ST_MAX_TIME_SPAN_DAYS (applied to base_max_time_span)
  uint32_t env_span_days = StEnvU32("ST_MAX_TIME_SPAN_DAYS", 0);
  uint32_t eff_base_span = env_span_days > 0 ? env_span_days * 86400 : base_max_time_span;
  p.effective_max_time_span = eff_base_span;
  if (eff_base_span > 0 && output_level >= 4) {
    const uint64_t widened = static_cast<uint64_t>(eff_base_span) * 2;
    p.effective_max_time_span =
        widened > static_cast<uint64_t>(UINT32_MAX)
            ? UINT32_MAX
            : static_cast<uint32_t>(widened);
  }

  return p;
}

inline uint32_t StEventMaxBucketSpanForLevel(int output_level) {
  return ComputeStEventSplitLevelPolicy(output_level, /*base_max_time_span=*/0)
      .max_bucket_span;
}

// True when compaction may emit cross-bucket output / uid-merge (always on).
inline bool StEventAllowsCrossBucketMerge(int /*output_level*/) {
  return true;
}

}  // namespace ROCKSDB_NAMESPACE


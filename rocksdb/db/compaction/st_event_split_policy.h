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

// Level-aware temporal ST compaction output policy.
//
// Physical compaction (SST layer) policy recap:
// - Level-aware relax for two constraint families:
//   1) Start-bucket span
//   2) Start-bucket timestamp span
//
// Hard vs soft constraints (enforced in CompactionOutputs):
// - Hard: time-span (must cut)
// - Soft: bucket-span (may be gated by min_keys)
//
// This header centralizes the level-aware knobs used by both
// CompactionOutputs (output splitting) and the compaction picker preference
// logic, to avoid policy drift across call sites.
struct StEventSplitLevelPolicy {
  uint32_t max_bucket_span = UINT32_MAX;
  uint32_t effective_max_time_span = 0;   // seconds; derived from base max span
};

// Level-aware temporal output limits. There are no per-level record-count
// limits; ordinary file-size policy controls SST size.
inline StEventSplitLevelPolicy ComputeStEventSplitLevelPolicy(
    int output_level, uint32_t base_max_time_span) {
  StEventSplitLevelPolicy p{};

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

}  // namespace ROCKSDB_NAMESPACE


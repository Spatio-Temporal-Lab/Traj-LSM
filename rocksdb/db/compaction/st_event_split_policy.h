#pragma once

#include <cstdint>

namespace ROCKSDB_NAMESPACE {

// Mind.md (final): Level-aware ST compaction output policy.
//
// Physical compaction (SST layer) policy recap:
// - Level-aware relax for three constraint families:
//   1) Cross-bucket: unrestricted (no bucket-span output cut)
//   2) Time expansion: max_time_expansion[level]
//   3) Spatial expansion: max_mbr_expansion[level] (soft/hard)
//
// Hard vs soft constraints (enforced in CompactionOutputs):
// - Hard: time-span, max-keys, hard-spatial (must cut)
// - Soft: bucket-span, soft-spatial (may be gated by min_keys)
//
// This header centralizes the level-aware knobs used by both
// CompactionOutputs (output splitting) and the compaction picker preference
// logic, to avoid policy drift across call sites.
struct StEventSplitLevelPolicy {
  uint32_t max_bucket_span = UINT32_MAX;  // no cross-bucket output cut
  uint32_t effective_max_time_span = 0;   // seconds; derived from base max span
  uint32_t effective_max_keys = UINT32_MAX;  // per-file ST key count cap
  double spatial_ratio_mul = 1.0;
  double hard_min_file_size_mul = 1.0;
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
  if (output_level <= 1) {
    p.effective_max_keys = 12000;
  } else if (output_level == 2) {
    p.effective_max_keys = 20000;
  } else if (output_level == 3) {
    p.effective_max_keys = 80000;
  } else {
    p.effective_max_keys = UINT32_MAX;  // L4+: no key-count split
  }

  // ── Cross-bucket span (soft constraint, 2× per level) ──
  if (output_level <= 1) {
    p.max_bucket_span = 64;
  } else if (output_level == 2) {
    p.max_bucket_span = 128;
  } else if (output_level == 3) {
    p.max_bucket_span = 256;
  } else {
    p.max_bucket_span = 512;
  }

  // ── Per-file time span (hard constraint; L4+ relaxes 2×) ──
  p.effective_max_time_span = base_max_time_span;
  if (base_max_time_span > 0 && output_level >= 4) {
    const uint64_t widened = static_cast<uint64_t>(base_max_time_span) * 2;
    p.effective_max_time_span =
        widened > static_cast<uint64_t>(UINT32_MAX)
            ? UINT32_MAX
            : static_cast<uint32_t>(widened);
  }

  // ── Spatial expansion relax ──
  if (output_level >= 4) {
    p.spatial_ratio_mul = 1.8;
    p.hard_min_file_size_mul = 1.75;
  } else if (output_level >= 2) {
    p.spatial_ratio_mul = 1.35;
    p.hard_min_file_size_mul = 1.25;
  } else {
    p.spatial_ratio_mul = 1.0;
    p.hard_min_file_size_mul = 1.0;
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


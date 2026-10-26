// SPDX-License-Identifier: Apache-2.0
// User-key prefix encoding for SpatioTemporalBlockMeta aggregation (experimental).

#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_set>
#include <vector>

#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "table/st_meta_index_extension.h"

namespace ROCKSDB_NAMESPACE {

// User key layout (little-endian), minimum length 13 + N:
//   [0]     magic 0xE5
//   [1:5)   uint32_t t_sec   (application-defined epoch seconds or bucket)
//   [5:9)   float x          (e.g. longitude)
//   [9:13)  float y          (e.g. latitude)
//   [13:..) opaque suffix     (uniqueness; not parsed here)
//
// Keys without this prefix are ignored for st-meta aggregation.

constexpr unsigned char kSpatioTemporalUserKeyMagic = 0xE5;
// Legacy segment key schema kept for runtime compatibility with existing DBs.
constexpr unsigned char kSpatioTemporalSegmentKeyLegacyMagic = 0xE6;
constexpr unsigned char kSpatioTemporalSegmentKeyTzidMagic = 0xE7;
// E7 key is 27 bytes: magic + bucket_id + zorder + vehicle_id + segment_seq + chunk_seq.
// No type byte and no in-key t_start offset (coarse bucket start only).
constexpr size_t kSpatioTemporalE7KeyBytes = 27;
constexpr uint32_t kSpatioTemporalE7DefaultBucketSeconds = 3600;
// event_time_bucket_lookback == this value => bucket_id 0 (maximally conservative).
constexpr uint32_t kSpatioTemporalE7PruneSeekLookbackAll = UINT32_MAX;

// TMan-tsp ST primary RowKey (no magic): [2B period BE][8B spatial BE][tid...]
constexpr size_t kTmanStKeyPrefixBytes = 10;

// Runtime schema hint used by lower-bound seek construction and query-side
// pruning. Default is tman_st unless ROCKSDB_ST_USER_KEY_SCHEMA=e7.
bool StUserKeySchemaPrefersTmanSt();

inline bool IsTmanStSegmentUserKey(const Slice& user_key) {
  if (user_key.size() < kTmanStKeyPrefixBytes + 1) {
    return false;
  }
  const unsigned char m = static_cast<unsigned char>(user_key.data()[0]);
  if (m == kSpatioTemporalUserKeyMagic ||
      m == kSpatioTemporalSegmentKeyLegacyMagic ||
      m == kSpatioTemporalSegmentKeyTzidMagic) {
    return false;
  }
  return true;
}

inline bool IsE7SegmentUserKey(const Slice& user_key) {
  return user_key.size() >= kSpatioTemporalE7KeyBytes &&
         static_cast<unsigned char>(user_key.data()[0]) ==
             kSpatioTemporalSegmentKeyTzidMagic;
}

// Mind.md / rocks-demo segment key (E7) or TMan ST RowKey.
inline bool IsStDesignSegmentUserKey(const Slice& user_key) {
  return IsE7SegmentUserKey(user_key) || IsTmanStSegmentUserKey(user_key);
}

struct SpatioTemporalPoint {
  float x = 0.f;
  float y = 0.f;
};

// Merges one user key into per-data-block accumulator (same semantics as
// widening t/MBR and unioning the bitmap summary).
void MergeSpatioTemporalMetaFromUserKey(const Slice& user_key,
                                        SpatioTemporalBlockMeta* acc,
                                        bool* acc_valid);

// Same as above, but allows segment-style keys (especially E7) to source
// t_end/MBR/bitmap from the value header when the user key no longer carries
// enough pruning metadata.
void MergeSpatioTemporalMetaFromKeyValue(const Slice& user_key,
                                         const std::optional<Slice>& value,
                                         SpatioTemporalBlockMeta* acc,
                                         bool* acc_valid);

// Appends exact points used to build a block-local bitmap. Returns
// *precise_points=false when this key/value can only provide coarse bounds, in
// which case callers should disable bitmap-based rejection for the block.
void CollectSpatioTemporalPointsFromKeyValue(
    const Slice& user_key, const std::optional<Slice>& value,
    std::vector<SpatioTemporalPoint>* out_points, bool* precise_points);

void BuildBitmapFromPoints(const std::vector<SpatioTemporalPoint>& points,
                           SpatioTemporalBlockMeta* meta);

// E7 host calendar bucket_id from key bytes [1:5) big-endian.
bool TryDecodeE7HostBucketId(const Slice& user_key, uint32_t* bucket_id);

bool TryDecodeSegmentValueStartTime(const Slice& value, uint32_t* t_start);


// Decodes Event Time `t_sec` from a spatio-temporal user key (magic 0xE5).
// Returns false if the key is too short or not an ST user key.
bool TryDecodeSpatioTemporalUserKeyTime(
    const Slice& user_key, uint32_t* t_sec,
    uint32_t bucket_width_sec = kSpatioTemporalE7DefaultBucketSeconds);

// True if the key decodes as an ST user key and its (t,x,y) point is
// axis-disjoint from the prune window (same test as block/file MBR vs window).
// False if not an ST key or if the point intersects the window (caller keeps key).
bool UserKeySpatioTemporalDisjointFromPruneScan(
    const Slice& user_key,
    const ReadOptions::ExperimentalSpatioTemporalPruneScan& q);

// True iff the key is a valid ST user key (magic 0xE5, decodable t,x,y) and the
// point lies inside the axis-aligned window [t_min,t_max]×[x_min,x_max]×[y_min,y_max].
// Used for brute-force baselines that scan the CF but count only query answers.
bool UserKeySpatioTemporalInWindow(const Slice& user_key, uint32_t t_min,
                                   uint32_t t_max, float x_min, float x_max,
                                   float y_min, float y_max);

// Union of per-data-block SpatioTemporal summaries into whole-file bounds for
// Manifest (min/max t, MBR union, bitmap OR). *acc_valid false => copy block.
void UnionSpatioTemporalBlockMeta(SpatioTemporalBlockMeta* acc, bool* acc_valid,
                                  const SpatioTemporalBlockMeta& block);

bool SpatioTemporalBitmapMayIntersectQuery(
    const SpatioTemporalBlockMeta& meta,
    const ReadOptions::ExperimentalSpatioTemporalPruneScan& q);

// True iff at least one trajectory point in value lies inside the prune window.
// Conservative on decode failure (returns true) to avoid false negatives.
bool KeyValueHasPointInSpatioTemporalWindow(
    const Slice& user_key, const Slice& value,
    const ReadOptions::ExperimentalSpatioTemporalPruneScan& q);

// Builds the minimum E7 user key (27B) for index Seek during prune scan:
// bucket_id = max(0, floor(q_t_min / bucket_width) - lookback), zorder/ids zeroed.
// Conservative: lookback covers cross-bucket replicas with host buckets before q_t_min.
void AppendE7PruneSeekLowerBoundUserKey(uint32_t q_t_min,
                                        uint32_t bucket_width_sec,
                                        uint32_t bucket_lookback,
                                        std::string* out_user_key);

bool TryDecodeTmanStPeriod(const Slice& user_key, int16_t* period);

// Conservative lower-bound start time (sec) from period field only.
bool TryDecodeTmanStKeyStartTimeSec(const Slice& user_key, uint32_t* t_start_sec,
                                    double time_bin, int time_bin_nums);

// Index Seek lower bound for TMan ST keys: period = max(0, period(q_t_min) - lookback).
void AppendTmanStPruneSeekLowerBoundUserKey(uint32_t q_t_min,
                                            double time_bin, int time_bin_nums,
                                            uint32_t period_lookback,
                                            std::string* out_user_key);

// Dispatches E7 vs TMan ST lower bound (auto-detect via time_bin / schema hint).
void AppendStPruneSeekLowerBoundUserKey(
    uint32_t q_t_min, uint32_t bucket_width_sec, uint32_t bucket_lookback,
    double tman_time_bin, int tman_time_bin_nums, uint32_t tman_period_lookback,
    bool prefer_tman_st_key, std::string* out_user_key);

// Extract user key from an iterator key (either internal key or raw user key).
// For DBs with index_key_is_user_key=1, index keys are already user keys.
inline Slice UserKeyFromIteratorKey(const Slice& raw_key) {
  // Try stripping 8-byte internal key footer first (seq+type).
  constexpr size_t kInternalKeyFooter = 8;
  if (raw_key.size() > kInternalKeyFooter) {
    Slice uk(raw_key.data(), raw_key.size() - kInternalKeyFooter);
    if (IsStDesignSegmentUserKey(uk)) {
      return uk;
    }
  }
  // Fallback: raw_key may already be a user key.
  if (IsStDesignSegmentUserKey(raw_key)) {
    return raw_key;
  }
  // Not an ST key: return best effort.
  return raw_key;
}

}  // namespace ROCKSDB_NAMESPACE

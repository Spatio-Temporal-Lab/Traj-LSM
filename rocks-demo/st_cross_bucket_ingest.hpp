// ST segment ingest helpers (Mind.md): canonical single-key ingest by default;
// IntersectingBucketRange retained for Manifest S(F) fan-out (not for KV replicate).

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "segment_value_codec.hpp"
#include "st_segment_key.hpp"

namespace st_cross_bucket {

inline uint32_t CanonicalBucketId(uint32_t t_start, uint32_t bucket_sec) {
  const uint32_t w = bucket_sec == 0 ? 3600u : bucket_sec;
  return t_start / w;
}

inline std::pair<uint32_t, uint32_t> IntersectingBucketRange(uint32_t t_start,
                                                             uint32_t t_end,
                                                             uint32_t bucket_sec) {
  if (bucket_sec == 0) {
    bucket_sec = 3600;
  }
  const uint32_t b0 = t_start / bucket_sec;
  const uint32_t b1 = t_end / bucket_sec;
  return {b0, b1};
}

inline std::string EncodeFullSegmentValue(const segval::SegmentValueHeader& head,
                                          const std::vector<segval::SegmentPoint>& points) {
  return segval::EncodeSegmentValueV2(head, points);
}

// E7 key with canonical bucket_id = floor(t_start / W).
inline std::string SegmentKeyCanonical(uint32_t t_start, uint64_t zorder_start,
                                       uint64_t vehicle_id, uint32_t segment_seq,
                                       uint16_t chunk_seq, uint32_t bucket_sec) {
  return StSegmentKeyTzid(t_start, zorder_start, vehicle_id, segment_seq, chunk_seq,
                          bucket_sec);
}

// Legacy cross-bucket replicate: one key per intersecting calendar bucket.
inline std::string SegmentKeyForHostBucket(uint32_t t_start, uint32_t host_bucket_id,
                                           uint64_t zorder_start, uint64_t vehicle_id,
                                           uint32_t segment_seq, uint16_t chunk_seq,
                                           uint32_t bucket_sec) {
  return StSegmentKeyTzidForHostBucket(t_start, host_bucket_id, zorder_start, vehicle_id,
                                       segment_seq, chunk_seq, bucket_sec);
}

inline size_t IntersectingBucketCount(uint32_t t_start, uint32_t t_end,
                                      uint32_t bucket_sec) {
  const auto [b0, b1] = IntersectingBucketRange(t_start, t_end, bucket_sec);
  return static_cast<size_t>(b1 - b0) + 1;
}

}  // namespace st_cross_bucket
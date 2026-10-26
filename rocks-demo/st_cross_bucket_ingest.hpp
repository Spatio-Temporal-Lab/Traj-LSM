// ST segment ingest helpers: canonical single-key ingest.

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

}  // namespace st_cross_bucket

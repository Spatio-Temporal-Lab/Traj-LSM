// Segment-level ST user key (E7, final canonical schema).

#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "segment_value_codec.hpp"

// Segment key layout (E7, 27 bytes, big-endian / memcmp sortable):
// [0]     magic 0xE7
// [1:5)   bucket_id_be(u32)   canonical calendar bucket floor(t_start / W)
// [5:13)  zorder_be(u64)      Morton interleave of centroid lon/lat float bits
// [13:21) vehicle_id_be(u64)
// [21:25) segment_seq_be(u32)
// [25:27) chunk_seq_be(u16)
//
// Canonical ingest: one key per segment_uid (canonical bucket_id), K ≈ N.
// segment_uid = (vehicle_id, segment_seq, chunk_seq) for idempotency / dedup.

// Z-order start: centroid of all points, Morton-interleaved lon/lat float bits.
// (True 2D space-filling order, not the legacy (lon<<32)|lat concatenation.)
inline uint64_t MakeZOrderStart(const std::vector<segval::SegmentPoint>& points) {
  if (points.empty()) {
    return 0;
  }
  double cx = 0.0, cy = 0.0;
  for (const auto& p : points) {
    cx += static_cast<double>(p.lon);
    cy += static_cast<double>(p.lat);
  }
  const double n = static_cast<double>(points.size());
  cx /= n;
  cy /= n;
  // Encode the full 32-bit IEEE-754 float bit pattern of the centroid, not the
  // low 4 bytes of the double (which drops sign/exponent and breaks monotonicity).
  const float flon = static_cast<float>(cx);
  const float flat = static_cast<float>(cy);
  uint32_t lon_bits = 0, lat_bits = 0;
  std::memcpy(&lon_bits, &flon, sizeof(lon_bits));
  std::memcpy(&lat_bits, &flat, sizeof(lat_bits));
  uint64_t code = 0;
  for (int i = 31; i >= 0; --i) {
    code = (code << 2) |
           ((static_cast<uint64_t>(lon_bits >> i) & 1U) << 1) |
           (static_cast<uint64_t>(lat_bits >> i) & 1U);
  }
  return code;
}

inline std::string StSegmentKeyTzidRaw(uint32_t bucket_id, uint64_t zorder_start,
                                       uint64_t vehicle_id, uint32_t segment_seq,
                                       uint16_t chunk_seq) {
  std::string k(27, '\0');
  k[0] = static_cast<char>(0xE7);
  k[1] = static_cast<char>((bucket_id >> 24) & 0xff);
  k[2] = static_cast<char>((bucket_id >> 16) & 0xff);
  k[3] = static_cast<char>((bucket_id >> 8) & 0xff);
  k[4] = static_cast<char>((bucket_id) & 0xff);
  for (int i = 0; i < 8; ++i) {
    k[5 + i] = static_cast<char>((zorder_start >> (56 - 8 * i)) & 0xff);
  }
  for (int i = 0; i < 8; ++i) {
    k[13 + i] = static_cast<char>((vehicle_id >> (56 - 8 * i)) & 0xff);
  }
  k[21] = static_cast<char>((segment_seq >> 24) & 0xff);
  k[22] = static_cast<char>((segment_seq >> 16) & 0xff);
  k[23] = static_cast<char>((segment_seq >> 8) & 0xff);
  k[24] = static_cast<char>((segment_seq) & 0xff);
  k[25] = static_cast<char>((chunk_seq >> 8) & 0xff);
  k[26] = static_cast<char>(chunk_seq & 0xff);
  return k;
}

inline std::string StSegmentKeyTzid(uint32_t t_start, uint64_t zorder_start,
                                    uint64_t vehicle_id, uint32_t segment_seq,
                                    uint16_t chunk_seq,
                                    uint32_t bucket_sec = 3600) {
  const uint32_t safe_bucket_sec = bucket_sec == 0 ? 3600u : bucket_sec;
  const uint32_t bucket_id = t_start / safe_bucket_sec;
  return StSegmentKeyTzidRaw(bucket_id, zorder_start, vehicle_id, segment_seq,
                             chunk_seq);
}

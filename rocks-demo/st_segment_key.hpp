// Shared segment-level ST user key:
// [0] 0xE6 + [1:5) t_start_be + [5:9) t_end_be + [9:13) x_min + [13:17) y_min +
// [17:21) x_max + [21:25) y_max + [25:33) id
//
// NOTE: t_start/t_end are encoded big-endian so that default RocksDB user key
// comparator (memcmp) orders keys by time, which is required for event-time
// compaction splitting to behave sensibly.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>

inline std::string StSegmentKey(uint32_t t_start, uint32_t t_end, float x_min,
                                float y_min, float x_max, float y_max,
                                uint64_t id) {
  std::string k(33, '\0');
  k[0] = static_cast<char>(0xE6);
  k[1] = static_cast<char>((t_start >> 24) & 0xff);
  k[2] = static_cast<char>((t_start >> 16) & 0xff);
  k[3] = static_cast<char>((t_start >> 8) & 0xff);
  k[4] = static_cast<char>((t_start) & 0xff);
  k[5] = static_cast<char>((t_end >> 24) & 0xff);
  k[6] = static_cast<char>((t_end >> 16) & 0xff);
  k[7] = static_cast<char>((t_end >> 8) & 0xff);
  k[8] = static_cast<char>((t_end) & 0xff);
  std::memcpy(&k[9], &x_min, 4);
  std::memcpy(&k[13], &y_min, 4);
  std::memcpy(&k[17], &x_max, 4);
  std::memcpy(&k[21], &y_max, 4);
  std::memcpy(&k[25], &id, 8);
  return k;
}

inline uint64_t StSegmentGhostId(uint64_t master_id, uint32_t target_bucket) {
  return master_id ^ (0x9e3779b97f4a7c15ULL +
                      static_cast<uint64_t>(target_bucket) * 0xbf58476d1ce4e5b9ULL);
}

inline std::string StSegmentGhostKey(uint32_t t_start, uint32_t t_end, float x_min,
                                     float y_min, float x_max, float y_max,
                                     uint64_t master_id,
                                     uint32_t target_bucket) {
  return StSegmentKey(t_start, t_end, x_min, y_min, x_max, y_max,
                      StSegmentGhostId(master_id, target_bucket));
}

// Segment key layout (time-range + Z-order + id):
// [0] 0xE7 + [1:5) t_start_be + [5:9) t_end_be + [9:17) zorder_start_be(u64) + [17:25) id
//
// NOTE: big-endian for ordering by memcmp (time, then zorder, then id).
inline std::string StSegmentKeyTzid(uint32_t t_start, uint32_t t_end,
                                    uint64_t zorder_start, uint64_t id) {
  std::string k(25, '\0');
  k[0] = static_cast<char>(0xE7);
  k[1] = static_cast<char>((t_start >> 24) & 0xff);
  k[2] = static_cast<char>((t_start >> 16) & 0xff);
  k[3] = static_cast<char>((t_start >> 8) & 0xff);
  k[4] = static_cast<char>((t_start) & 0xff);
  k[5] = static_cast<char>((t_end >> 24) & 0xff);
  k[6] = static_cast<char>((t_end >> 16) & 0xff);
  k[7] = static_cast<char>((t_end >> 8) & 0xff);
  k[8] = static_cast<char>((t_end) & 0xff);
  for (int i = 0; i < 8; ++i) {
    k[9 + i] = static_cast<char>((zorder_start >> (56 - 8 * i)) & 0xff);
  }
  std::memcpy(&k[17], &id, 8);
  return k;
}


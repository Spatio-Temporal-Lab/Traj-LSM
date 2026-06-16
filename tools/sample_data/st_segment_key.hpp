// Segment-level ST user key (E7, final schema; see Mind.md).

#pragma once

#include <cstdint>
#include <string>

// Segment key layout (Mind.md E7 v1, 32 bytes, big-endian / memcmp sortable):
// [0] 0xE7
// [1]     type_flags (reserved; MUST be 0 for new ingest — no master/ghost)
// [2:6)   bucket_id_be(u32)  canonical calendar bucket floor(t_start / W)
// [6:14)  zorder_start_be(u64)
// [14:18) t_start_offset_be(u32) = t_start - bucket_start(canonical)
// [18:26) vehicle_id_be(u64)
// [26:30) segment_seq_be(u32)
// [30:32) chunk_seq_be(u16)
//
// Default ingest: one key per segment_uid (canonical bucket_id). Legacy
// --cross-bucket-replicate used multiple keys differing by bucket_id.
// segment_uid = (vehicle_id, segment_seq, chunk_seq) for idempotency / dedup.
//
// Legacy: bit0=1 marked ghost keys (--legacy-ghost-ingest); 31-byte E7 without
// byte [1] may still exist in old DBs (RocksDB decodes both lengths).
constexpr uint8_t kE7KeyTypeGhostBit = 0x01;  // legacy ingest/read only

inline std::string StSegmentKeyTzidRaw(uint32_t bucket_id, uint64_t zorder_start,
                                       uint32_t t_start_offset, uint64_t vehicle_id,
                                       uint32_t segment_seq, uint16_t chunk_seq,
                                       uint8_t type_flags = 0) {
  std::string k(32, '\0');
  k[0] = static_cast<char>(0xE7);
  k[1] = static_cast<char>(type_flags);
  k[2] = static_cast<char>((bucket_id >> 24) & 0xff);
  k[3] = static_cast<char>((bucket_id >> 16) & 0xff);
  k[4] = static_cast<char>((bucket_id >> 8) & 0xff);
  k[5] = static_cast<char>((bucket_id) & 0xff);
  for (int i = 0; i < 8; ++i) {
    k[6 + i] = static_cast<char>((zorder_start >> (56 - 8 * i)) & 0xff);
  }
  k[14] = static_cast<char>((t_start_offset >> 24) & 0xff);
  k[15] = static_cast<char>((t_start_offset >> 16) & 0xff);
  k[16] = static_cast<char>((t_start_offset >> 8) & 0xff);
  k[17] = static_cast<char>((t_start_offset) & 0xff);
  for (int i = 0; i < 8; ++i) {
    k[18 + i] = static_cast<char>((vehicle_id >> (56 - 8 * i)) & 0xff);
  }
  k[26] = static_cast<char>((segment_seq >> 24) & 0xff);
  k[27] = static_cast<char>((segment_seq >> 16) & 0xff);
  k[28] = static_cast<char>((segment_seq >> 8) & 0xff);
  k[29] = static_cast<char>((segment_seq) & 0xff);
  k[30] = static_cast<char>((chunk_seq >> 8) & 0xff);
  k[31] = static_cast<char>(chunk_seq & 0xff);
  return k;
}

// E7 key for an explicit calendar bucket (legacy --cross-bucket-replicate only).
inline std::string StSegmentKeyTzidForHostBucket(uint32_t t_start,
                                                 uint32_t host_bucket_id,
                                                 uint64_t zorder_start,
                                                 uint64_t vehicle_id,
                                                 uint32_t segment_seq,
                                                 uint16_t chunk_seq,
                                                 uint32_t bucket_sec) {
  const uint32_t safe_bucket_sec = bucket_sec == 0 ? 3600u : bucket_sec;
  const uint32_t bucket_start = host_bucket_id * safe_bucket_sec;
  const uint32_t t_start_offset = t_start - bucket_start;
  return StSegmentKeyTzidRaw(host_bucket_id, zorder_start, t_start_offset,
                            vehicle_id, segment_seq, chunk_seq, /*type_flags=*/0);
}

inline std::string StSegmentKeyTzid(uint32_t t_start, uint64_t zorder_start,
                                    uint64_t vehicle_id, uint32_t segment_seq,
                                    uint16_t chunk_seq,
                                    uint32_t bucket_sec = 3600) {
  const uint32_t safe_bucket_sec = bucket_sec == 0 ? 3600u : bucket_sec;
  const uint32_t bucket_id = t_start / safe_bucket_sec;
  return StSegmentKeyTzidForHostBucket(t_start, bucket_id, zorder_start, vehicle_id,
                                       segment_seq, chunk_seq, safe_bucket_sec);
}

// Deprecated: ghost reference keys (--legacy-ghost-ingest only). New path uses
// StSegmentKeyTzidForHostBucket with full segment value in every intersecting bucket.
inline std::string StSegmentGhostKeyTzid(uint32_t target_bucket_id,
                                         uint64_t zorder_start,
                                         uint64_t vehicle_id,
                                         uint32_t segment_seq,
                                         uint16_t chunk_seq,
                                         uint32_t t_start_offset_hint = 0) {
  return StSegmentKeyTzidRaw(target_bucket_id, zorder_start, t_start_offset_hint,
                            vehicle_id, segment_seq, chunk_seq,
                            kE7KeyTypeGhostBit);
}


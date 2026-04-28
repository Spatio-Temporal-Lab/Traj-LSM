// SPDX-License-Identifier: Apache-2.0

#include "table/st_meta_user_key.h"

#include <algorithm>
#include <cstring>
#include <optional>

#include "util/coding.h"

namespace ROCKSDB_NAMESPACE {

namespace {

inline uint32_t DecodeFixed32BigEndian(const char* p) {
  return (static_cast<uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[3])));
}

inline bool ConsumeFixed32BigEndian(Slice* s, uint32_t* out) {
  if (s == nullptr || out == nullptr || s->size() < 4) {
    return false;
  }
  *out = DecodeFixed32BigEndian(s->data());
  s->remove_prefix(4);
  return true;
}

int LonLatToGridBit(float lon, float lat) {
  const int cx = static_cast<int>((lon + 180.f) / 360.f * 8.f);
  const int cy = static_cast<int>((lat + 90.f) / 180.f * 8.f);
  const int ix = std::max(0, std::min(7, cx));
  const int iy = std::max(0, std::min(7, cy));
  return iy * 8 + ix;
}

#pragma pack(push, 1)
struct SegmentValueHeaderForStMeta {
  uint32_t t_start = 0;
  uint32_t t_end = 0;
  float x_min = 0.f;
  float y_min = 0.f;
  float x_max = 0.f;
  float y_max = 0.f;
  uint32_t point_count = 0;
};
#pragma pack(pop)

static_assert(sizeof(SegmentValueHeaderForStMeta) == 28,
              "segment value header size");

constexpr uint32_t kSegmentValueMagicV2 = 0x32676553;  // 'Seg2'
constexpr uint32_t kSegmentValueMagicV3 = 0x33676553;  // 'Seg3'

bool TryDecodeSegmentValueHeader(const Slice& value,
                                 SegmentValueHeaderForStMeta* out) {
  if (out == nullptr) {
    return false;
  }
  *out = SegmentValueHeaderForStMeta{};
  if (value.size() == sizeof(SegmentValueHeaderForStMeta)) {
    std::memcpy(out, value.data(), sizeof(*out));
    return true;
  }
  if (value.size() < 4 + sizeof(SegmentValueHeaderForStMeta)) {
    return false;
  }
  uint32_t magic = 0;
  std::memcpy(&magic, value.data(), sizeof(magic));
  if (magic != kSegmentValueMagicV2 && magic != kSegmentValueMagicV3) {
    return false;
  }
  std::memcpy(out, value.data() + 4, sizeof(*out));
  return true;
}

void MergeSpatioTemporalMetaFromSegmentValue(
    const Slice& value, SpatioTemporalBlockMeta* acc, bool* acc_valid) {
  if (acc == nullptr || acc_valid == nullptr) {
    return;
  }
  SegmentValueHeaderForStMeta header{};
  if (!TryDecodeSegmentValueHeader(value, &header)) {
    return;
  }
  const float cx = (header.x_min + header.x_max) * 0.5f;
  const float cy = (header.y_min + header.y_max) * 0.5f;
  const uint64_t bit = 1ULL << LonLatToGridBit(cx, cy);
  if (!*acc_valid) {
    acc->t_min = header.t_start;
    acc->t_max = header.t_end;
    acc->x_min = header.x_min;
    acc->x_max = header.x_max;
    acc->y_min = header.y_min;
    acc->y_max = header.y_max;
    acc->bitmap = bit;
    *acc_valid = true;
  } else {
    acc->t_min = std::min(acc->t_min, header.t_start);
    acc->t_max = std::max(acc->t_max, header.t_end);
    acc->x_min = std::min(acc->x_min, header.x_min);
    acc->x_max = std::max(acc->x_max, header.x_max);
    acc->y_min = std::min(acc->y_min, header.y_min);
    acc->y_max = std::max(acc->y_max, header.y_max);
    acc->bitmap |= bit;
  }
}

}  // namespace

bool TryDecodeSpatioTemporalUserKeyTime(const Slice& user_key,
                                        uint32_t* t_sec) {
  if (t_sec == nullptr || user_key.size() < 5) {
    return false;
  }
  const char* p = user_key.data();
  const unsigned char magic = static_cast<unsigned char>(p[0]);
  if (magic != kSpatioTemporalUserKeyMagic &&
      magic != kSpatioTemporalSegmentKeyMagic &&
      magic != kSpatioTemporalSegmentKeyTzidMagic) {
    return false;
  }
  if (magic == kSpatioTemporalUserKeyMagic) {
    Slice s(p + 1, user_key.size() - 1);
    return GetFixed32(&s, t_sec);  // little-endian
  }
  // Segment keys encode t_start big-endian for memcmp ordering.
  Slice s(p + 1, user_key.size() - 1);
  return ConsumeFixed32BigEndian(&s, t_sec);
}

bool UserKeySpatioTemporalDisjointFromPruneScan(
    const Slice& user_key,
    const ReadOptions::ExperimentalSpatioTemporalPruneScan& q) {
  if (!q.enable || user_key.size() < 13) {
    return false;
  }
  const char* p = user_key.data();
  const unsigned char magic = static_cast<unsigned char>(p[0]);
  if (magic != kSpatioTemporalUserKeyMagic &&
      magic != kSpatioTemporalSegmentKeyMagic &&
      magic != kSpatioTemporalSegmentKeyTzidMagic) {
    return false;
  }
  Slice s(p + 1, user_key.size() - 1);
  if (magic == kSpatioTemporalUserKeyMagic) {
    uint32_t t_sec = 0;
    if (!GetFixed32(&s, &t_sec)) {
      return false;
    }
    uint32_t xbits = 0, ybits = 0;
    if (!GetFixed32(&s, &xbits) || !GetFixed32(&s, &ybits)) {
      return false;
    }
    float x = 0.f, y = 0.f;
    static_assert(sizeof(float) == sizeof(uint32_t), "float size");
    std::memcpy(&x, &xbits, sizeof(x));
    std::memcpy(&y, &ybits, sizeof(y));
    return t_sec > q.t_max || t_sec < q.t_min || x > q.x_max || x < q.x_min ||
           y > q.y_max || y < q.y_min;
  }
  if (magic == kSpatioTemporalSegmentKeyTzidMagic) {
    // E7 key no longer guarantees t_end/MBR in-key. Key-level pruning may only
    // reject records that start after the query window ends.
    if (user_key.size() < 5) {
      return false;
    }
    uint32_t t_start = 0;
    if (!ConsumeFixed32BigEndian(&s, &t_start)) {
      return false;
    }
    return t_start > q.t_max;
  }

  // Segment-key layout (minimum length 25 + suffix):
  // [0] magic 0xE6
  // [1:5)  t_start (u32)
  // [5:9)  t_end   (u32)
  // [9:13) x_min   (f32)
  // [13:17) y_min  (f32)
  // [17:21) x_max  (f32)
  // [21:25) y_max  (f32)
  if (user_key.size() < 25) {
    return false;
  }
  uint32_t t_start = 0, t_end = 0;
  if (!ConsumeFixed32BigEndian(&s, &t_start) ||
      !ConsumeFixed32BigEndian(&s, &t_end)) {
    return false;
  }
  uint32_t xmin_bits = 0, ymin_bits = 0, xmax_bits = 0, ymax_bits = 0;
  if (!GetFixed32(&s, &xmin_bits) || !GetFixed32(&s, &ymin_bits) ||
      !GetFixed32(&s, &xmax_bits) || !GetFixed32(&s, &ymax_bits)) {
    return false;
  }
  float x_min = 0.f, y_min = 0.f, x_max = 0.f, y_max = 0.f;
  std::memcpy(&x_min, &xmin_bits, sizeof(x_min));
  std::memcpy(&y_min, &ymin_bits, sizeof(y_min));
  std::memcpy(&x_max, &xmax_bits, sizeof(x_max));
  std::memcpy(&y_max, &ymax_bits, sizeof(y_max));
  const bool time_disjoint = (t_start > q.t_max) || (t_end < q.t_min);
  const bool space_disjoint =
      (x_min > q.x_max) || (x_max < q.x_min) || (y_min > q.y_max) ||
      (y_max < q.y_min);
  return time_disjoint || space_disjoint;
}

bool UserKeySpatioTemporalInWindow(const Slice& user_key, uint32_t t_min,
                                   uint32_t t_max, float x_min, float x_max,
                                   float y_min, float y_max) {
  if (user_key.size() < 13) {
    return false;
  }
  const char* p = user_key.data();
  const unsigned char magic = static_cast<unsigned char>(p[0]);
  if (magic != kSpatioTemporalUserKeyMagic &&
      magic != kSpatioTemporalSegmentKeyMagic &&
      magic != kSpatioTemporalSegmentKeyTzidMagic) {
    return false;
  }
  Slice s(p + 1, user_key.size() - 1);
  if (magic == kSpatioTemporalUserKeyMagic) {
    uint32_t t_sec = 0;
    if (!GetFixed32(&s, &t_sec)) {
      return false;
    }
    uint32_t xbits = 0, ybits = 0;
    if (!GetFixed32(&s, &xbits) || !GetFixed32(&s, &ybits)) {
      return false;
    }
    float x = 0.f, y = 0.f;
    static_assert(sizeof(float) == sizeof(uint32_t), "float size");
    std::memcpy(&x, &xbits, sizeof(x));
    std::memcpy(&y, &ybits, sizeof(y));
    if (t_sec > t_max || t_sec < t_min || x > x_max || x < x_min ||
        y > y_max || y < y_min) {
      return false;
    }
    return true;
  }
  if (magic == kSpatioTemporalSegmentKeyTzidMagic) {
    if (user_key.size() < 5) {
      return false;
    }
    uint32_t t_start = 0;
    if (!ConsumeFixed32BigEndian(&s, &t_start)) {
      return false;
    }
    // No t_end/MBR in key; keep this conservative and let value verification or
    // block-level metadata make the final decision.
    return t_start <= t_max;
  }

  if (user_key.size() < 25) {
    return false;
  }
  uint32_t t_start = 0, t_end = 0;
  if (!ConsumeFixed32BigEndian(&s, &t_start) ||
      !ConsumeFixed32BigEndian(&s, &t_end)) {
    return false;
  }
  uint32_t xmin_bits = 0, ymin_bits = 0, xmax_bits = 0, ymax_bits = 0;
  if (!GetFixed32(&s, &xmin_bits) || !GetFixed32(&s, &ymin_bits) ||
      !GetFixed32(&s, &xmax_bits) || !GetFixed32(&s, &ymax_bits)) {
    return false;
  }
  float kx_min = 0.f, ky_min = 0.f, kx_max = 0.f, ky_max = 0.f;
  std::memcpy(&kx_min, &xmin_bits, sizeof(kx_min));
  std::memcpy(&ky_min, &ymin_bits, sizeof(ky_min));
  std::memcpy(&kx_max, &xmax_bits, sizeof(kx_max));
  std::memcpy(&ky_max, &ymax_bits, sizeof(ky_max));
  const bool time_intersects = !(t_start > t_max || t_end < t_min);
  const bool space_intersects =
      !(kx_min > x_max || kx_max < x_min || ky_min > y_max || ky_max < y_min);
  return time_intersects && space_intersects;
}

void MergeSpatioTemporalMetaFromUserKey(const Slice& user_key,
                                        SpatioTemporalBlockMeta* acc,
                                        bool* acc_valid) {
  if (acc == nullptr || acc_valid == nullptr) {
    return;
  }
  if (user_key.size() < 13) {
    return;
  }
  const char* p = user_key.data();
  const unsigned char magic = static_cast<unsigned char>(p[0]);
  if (magic != kSpatioTemporalUserKeyMagic &&
      magic != kSpatioTemporalSegmentKeyMagic &&
      magic != kSpatioTemporalSegmentKeyTzidMagic) {
    return;
  }
  Slice s(p + 1, user_key.size() - 1);
  uint32_t t_min_v = 0, t_max_v = 0;
  float x_min_v = 0.f, y_min_v = 0.f, x_max_v = 0.f, y_max_v = 0.f;
  uint64_t bit = 0;

  if (magic == kSpatioTemporalUserKeyMagic) {
    uint32_t t_sec = 0;
    if (!GetFixed32(&s, &t_sec)) {
      return;
    }
    uint32_t xbits = 0, ybits = 0;
    if (!GetFixed32(&s, &xbits) || !GetFixed32(&s, &ybits)) {
      return;
    }
    float x = 0.f, y = 0.f;
    static_assert(sizeof(float) == sizeof(uint32_t), "float size");
    std::memcpy(&x, &xbits, sizeof(x));
    std::memcpy(&y, &ybits, sizeof(y));
    t_min_v = t_sec;
    t_max_v = t_sec;
    x_min_v = x_max_v = x;
    y_min_v = y_max_v = y;
    bit = 1ULL << LonLatToGridBit(x, y);
  } else {
    if (magic == kSpatioTemporalSegmentKeyTzidMagic) {
      // Can't widen MBR/bitmap without explicit spatial bounds. Skip meta merge
      // for now so existing ST-meta semantics remain sound.
      return;
    }
    if (user_key.size() < 25) {
      return;
    }
    if (!ConsumeFixed32BigEndian(&s, &t_min_v) ||
        !ConsumeFixed32BigEndian(&s, &t_max_v)) {
      return;
    }
    uint32_t xmin_bits = 0, ymin_bits = 0, xmax_bits = 0, ymax_bits = 0;
    if (!GetFixed32(&s, &xmin_bits) || !GetFixed32(&s, &ymin_bits) ||
        !GetFixed32(&s, &xmax_bits) || !GetFixed32(&s, &ymax_bits)) {
      return;
    }
    std::memcpy(&x_min_v, &xmin_bits, sizeof(x_min_v));
    std::memcpy(&y_min_v, &ymin_bits, sizeof(y_min_v));
    std::memcpy(&x_max_v, &xmax_bits, sizeof(x_max_v));
    std::memcpy(&y_max_v, &ymax_bits, sizeof(y_max_v));
    const int cx = LonLatToGridBit((x_min_v + x_max_v) * 0.5f,
                                   (y_min_v + y_max_v) * 0.5f);
    bit = 1ULL << cx;
  }

  if (!*acc_valid) {
    acc->t_min = t_min_v;
    acc->t_max = t_max_v;
    acc->x_min = x_min_v;
    acc->x_max = x_max_v;
    acc->y_min = y_min_v;
    acc->y_max = y_max_v;
    acc->bitmap = bit;
    *acc_valid = true;
  } else {
    acc->t_min = std::min(acc->t_min, t_min_v);
    acc->t_max = std::max(acc->t_max, t_max_v);
    acc->x_min = std::min(acc->x_min, x_min_v);
    acc->x_max = std::max(acc->x_max, x_max_v);
    acc->y_min = std::min(acc->y_min, y_min_v);
    acc->y_max = std::max(acc->y_max, y_max_v);
    acc->bitmap |= bit;
  }
}

void MergeSpatioTemporalMetaFromKeyValue(const Slice& user_key,
                                         const std::optional<Slice>& value,
                                         SpatioTemporalBlockMeta* acc,
                                         bool* acc_valid) {
  if (acc == nullptr || acc_valid == nullptr) {
    return;
  }
  if (user_key.size() < 1) {
    return;
  }
  const unsigned char magic = static_cast<unsigned char>(user_key.data()[0]);
  if (magic == kSpatioTemporalSegmentKeyTzidMagic && value.has_value()) {
    MergeSpatioTemporalMetaFromSegmentValue(value.value(), acc, acc_valid);
    return;
  }
  MergeSpatioTemporalMetaFromUserKey(user_key, acc, acc_valid);
}

void UnionSpatioTemporalBlockMeta(SpatioTemporalBlockMeta* acc, bool* acc_valid,
                                  const SpatioTemporalBlockMeta& block) {
  if (acc == nullptr || acc_valid == nullptr) {
    return;
  }
  if (!*acc_valid) {
    *acc = block;
    *acc_valid = true;
    return;
  }
  acc->t_min = std::min(acc->t_min, block.t_min);
  acc->t_max = std::max(acc->t_max, block.t_max);
  acc->x_min = std::min(acc->x_min, block.x_min);
  acc->x_max = std::max(acc->x_max, block.x_max);
  acc->y_min = std::min(acc->y_min, block.y_min);
  acc->y_max = std::max(acc->y_max, block.y_max);
  acc->bitmap |= block.bitmap;
}

}  // namespace ROCKSDB_NAMESPACE

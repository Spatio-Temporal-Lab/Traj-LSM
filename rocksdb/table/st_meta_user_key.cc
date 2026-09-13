// SPDX-License-Identifier: Apache-2.0

#include "table/st_meta_user_key.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

#include "table/tman_rocksdb_value.h"
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

void SetBitmapBit(SpatioTemporalBlockMeta* meta, uint32_t cell_index) {
  if (meta == nullptr || cell_index >= 256) {
    return;
  }
  const size_t word = cell_index / 64;
  const uint32_t bit = cell_index % 64;
  meta->bitmap_words[word] |= (uint64_t{1} << bit);
}

void OrBitmapWords(SpatioTemporalBlockMeta* acc,
                   const SpatioTemporalBlockMeta& block) {
  if (acc == nullptr) {
    return;
  }
  for (size_t i = 0; i < acc->bitmap_words.size(); ++i) {
    acc->bitmap_words[i] |= block.bitmap_words[i];
  }
}

uint32_t ClampBitmapCoord(float normalized) {
  if (normalized <= 0.0f) {
    return 0;
  }
  if (normalized >= 1.0f) {
    return kSpatioTemporalBitmapGridSize - 1;
  }
  const float scaled =
      normalized * static_cast<float>(kSpatioTemporalBitmapGridSize);
  uint32_t cell = static_cast<uint32_t>(scaled);
  if (cell >= kSpatioTemporalBitmapGridSize) {
    cell = kSpatioTemporalBitmapGridSize - 1;
  }
  return cell;
}

void AddBitmapPointNormalized(SpatioTemporalBlockMeta* meta, float x, float y) {
  if (meta == nullptr) {
    return;
  }
  const float w = meta->x_max - meta->x_min;
  const float h = meta->y_max - meta->y_min;
  if (!(w > 0.0f) || !(h > 0.0f)) {
    SetSpatioTemporalBitmapAll(meta);
    return;
  }
  const float nx = (x - meta->x_min) / w;
  const float ny = (y - meta->y_min) / h;
  const uint32_t ix = ClampBitmapCoord(nx);
  const uint32_t iy = ClampBitmapCoord(ny);
  SetBitmapBit(meta, iy * kSpatioTemporalBitmapGridSize + ix);
}

void BuildBitmapFromPointsImpl(const std::vector<SpatioTemporalPoint>& points,
                               SpatioTemporalBlockMeta* meta) {
  if (meta == nullptr) {
    return;
  }
  ClearSpatioTemporalBitmap(meta);
  if (points.empty()) {
    SetSpatioTemporalBitmapAll(meta);
    return;
  }
  const float w = meta->x_max - meta->x_min;
  const float h = meta->y_max - meta->y_min;
  if (!(w > 0.0f) || !(h > 0.0f)) {
    SetSpatioTemporalBitmapAll(meta);
    return;
  }
  for (const auto& p : points) {
    AddBitmapPointNormalized(meta, p.x, p.y);
  }
}

void SetSpatioTemporalMetaMatchAll(SpatioTemporalBlockMeta* meta,
                                   bool* meta_valid) {
  if (meta == nullptr || meta_valid == nullptr) {
    return;
  }
  meta->t_min = 0;
  meta->t_max = UINT32_MAX;
  meta->x_min = -std::numeric_limits<float>::infinity();
  meta->x_max = std::numeric_limits<float>::infinity();
  meta->y_min = -std::numeric_limits<float>::infinity();
  meta->y_max = std::numeric_limits<float>::infinity();
  SetSpatioTemporalBitmapAll(meta);
  *meta_valid = true;
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
constexpr uint8_t kSegmentValueFlagGhost = 0x01;

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
    // D (anti-false-negative): for E7, decode failure must not produce
    // partial/under-approximated bounds.
    SetSpatioTemporalMetaMatchAll(acc, acc_valid);
    return;
  }
  if (!*acc_valid) {
    acc->t_min = header.t_start;
    acc->t_max = header.t_end;
    acc->x_min = header.x_min;
    acc->x_max = header.x_max;
    acc->y_min = header.y_min;
    acc->y_max = header.y_max;
    SetSpatioTemporalBitmapAll(acc);
    *acc_valid = true;
  } else {
    acc->t_min = std::min(acc->t_min, header.t_start);
    acc->t_max = std::max(acc->t_max, header.t_end);
    acc->x_min = std::min(acc->x_min, header.x_min);
    acc->x_max = std::max(acc->x_max, header.x_max);
    acc->y_min = std::min(acc->y_min, header.y_min);
    acc->y_max = std::max(acc->y_max, header.y_max);
    SetSpatioTemporalBitmapAll(acc);
  }
}

}  // namespace

namespace {

bool E7HostBucketFromSegmentValue(const Slice& value, uint32_t bucket_width_sec,
                                   uint32_t* host_bucket) {
  if (host_bucket == nullptr) {
    return false;
  }
  SegmentValueHeaderForStMeta header{};
  if (!TryDecodeSegmentValueHeader(value, &header)) {
    return false;
  }
  const uint32_t bw = bucket_width_sec == 0 ? kSpatioTemporalE7DefaultBucketSeconds
                                            : bucket_width_sec;
  *host_bucket = header.t_start / bw;
  return true;
}

}  // namespace

bool StQuerySegmentUidDedup::AcceptEmitUserKey(const Slice& user_key,
                                               const Slice& value) {
  StSegmentUid uid;
  if (!TryDecodeE7SegmentUid(user_key, &uid)) {
    return true;
  }
  uint32_t cur_bucket = 0;
  if (!TryDecodeE7HostBucketId(user_key, &cur_bucket)) {
    return true;
  }
  uint32_t host_bucket = 0;
  bool cur_is_host = false;
  if (value.size() > 0 &&
      E7HostBucketFromSegmentValue(value, bucket_width_sec, &host_bucket)) {
    cur_is_host = (cur_bucket == host_bucket);
  }
  auto it = seen.find(uid);
  if (it == seen.end()) {
    seen.emplace(uid, Entry{cur_bucket, cur_is_host});
    return true;
  }
  if (it->second.kept_is_host) {
    ++dedup_skipped;
    return false;
  }
  if (cur_is_host) {
    it->second.kept_is_host = true;
    it->second.kept_bucket_id = cur_bucket;
    return true;
  }
  ++dedup_skipped;
  return false;
}

bool TryDecodeE7HostBucketFromValue(const Slice& value,
                                    uint32_t bucket_width_sec,
                                    uint32_t* host_bucket) {
  return E7HostBucketFromSegmentValue(value, bucket_width_sec, host_bucket);
}

bool TryDecodeE7HostBucketId(const Slice& user_key, uint32_t* bucket_id) {
  if (bucket_id == nullptr || user_key.size() < kSpatioTemporalE7KeyBytes ||
      static_cast<unsigned char>(user_key[0]) != kSpatioTemporalSegmentKeyTzidMagic) {
    return false;
  }
  constexpr size_t kPayloadOffset = 1;  // magic byte only
  if (user_key.size() < kPayloadOffset + 4) {
    return false;
  }
  const char* p = user_key.data() + kPayloadOffset;
  *bucket_id = (static_cast<uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
               (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
               (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 8) |
               static_cast<uint32_t>(static_cast<unsigned char>(p[3]));
  return true;
}

namespace {

int16_t TmanStPeriodFromUnixSec(uint32_t t_sec, double time_bin,
                                int time_bin_nums) {
  const int64_t period_ms = static_cast<int64_t>(
      time_bin * static_cast<double>(time_bin_nums) * 3600.0 * 1000.0);
  if (period_ms <= 0) {
    return 0;
  }
  const int64_t time_ms = static_cast<int64_t>(t_sec) * 1000;
  return static_cast<int16_t>(time_ms / period_ms);
}

void AppendInt16BE(std::string* out, int16_t v) {
  const uint16_t u = static_cast<uint16_t>(v);
  out->push_back(static_cast<char>((u >> 8) & 0xff));
  out->push_back(static_cast<char>(u & 0xff));
}

void AppendUInt64BE(std::string* out, uint64_t v) {
  for (int i = 7; i >= 0; --i) {
    out->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
  }
}

}  // namespace

bool StUserKeySchemaPrefersTmanSt() {
  const char* v = std::getenv("ROCKSDB_ST_USER_KEY_SCHEMA");
  if (v == nullptr || *v == '\0') {
    return true;
  }
  return std::strcmp(v, "tman_st") == 0;
}

bool TryDecodeTmanStPeriod(const Slice& user_key, int16_t* period) {
  if (period == nullptr || !IsTmanStSegmentUserKey(user_key)) {
    return false;
  }
  const char* p = user_key.data();
  *period = static_cast<int16_t>(
      (static_cast<uint16_t>(static_cast<unsigned char>(p[0])) << 8) |
      static_cast<unsigned char>(p[1]));
  return true;
}

bool TryDecodeTmanStKeyStartTimeSec(const Slice& user_key, uint32_t* t_start_sec,
                                    double time_bin, int time_bin_nums) {
  if (t_start_sec == nullptr) {
    return false;
  }
  int16_t period = 0;
  if (!TryDecodeTmanStPeriod(user_key, &period)) {
    return false;
  }
  const int64_t period_ms = static_cast<int64_t>(
      time_bin * static_cast<double>(time_bin_nums) * 3600.0 * 1000.0);
  if (period_ms <= 0 || period < 0) {
    return false;
  }
  const int64_t start_ms = static_cast<int64_t>(period) * period_ms;
  if (start_ms < 0 || start_ms > UINT32_MAX * 1000LL) {
    return false;
  }
  *t_start_sec = static_cast<uint32_t>(start_ms / 1000);
  return true;
}

void AppendTmanStPruneSeekLowerBoundUserKey(uint32_t q_t_min, double time_bin,
                                            int time_bin_nums,
                                            uint32_t period_lookback,
                                            std::string* out_user_key) {
  if (out_user_key == nullptr) {
    return;
  }
  const double tb = time_bin > 0 ? time_bin : 3600.0;
  const int tbn = time_bin_nums > 0 ? time_bin_nums : 149;
  int16_t period = TmanStPeriodFromUnixSec(q_t_min, tb, tbn);
  if (period_lookback == kSpatioTemporalE7PruneSeekLookbackAll) {
    period = 0;
  } else if (period_lookback > 0) {
    const int p = static_cast<int>(period);
    period = static_cast<int16_t>(
        (p >= static_cast<int>(period_lookback)) ? (p - static_cast<int>(period_lookback))
                                                 : 0);
  }
  out_user_key->clear();
  AppendInt16BE(out_user_key, period);
  AppendUInt64BE(out_user_key, 0);
}

void AppendStPruneSeekLowerBoundUserKey(
    uint32_t q_t_min, uint32_t bucket_width_sec, uint32_t bucket_lookback,
    double tman_time_bin, int tman_time_bin_nums, uint32_t tman_period_lookback,
    bool prefer_tman_st_key, std::string* out_user_key) {
  if (out_user_key == nullptr) {
    return;
  }
  const char* schema = std::getenv("ROCKSDB_ST_USER_KEY_SCHEMA");
  const bool use_tman =
      prefer_tman_st_key || StUserKeySchemaPrefersTmanSt();
  // The returned lower-bound key is intentionally conservative:
  // - it is used only to seek the index block iterator to the first
  //   candidate entry at or after the query's coarse lower bound;
  // - lookback keeps cross-boundary/long-segment candidates in play;
  // - the actual disjointness decision still happens on index-entry meta.
  if (use_tman && !(schema != nullptr && std::strcmp(schema, "e7") == 0)) {
    AppendTmanStPruneSeekLowerBoundUserKey(q_t_min, tman_time_bin, tman_time_bin_nums,
                                           tman_period_lookback, out_user_key);
    return;
  }
  AppendE7PruneSeekLowerBoundUserKey(q_t_min, bucket_width_sec, bucket_lookback,
                                     out_user_key);
}

void AppendE7PruneSeekLowerBoundUserKey(uint32_t q_t_min,
                                        uint32_t bucket_width_sec,
                                        uint32_t bucket_lookback,
                                        std::string* out_user_key) {
  if (out_user_key == nullptr) {
    return;
  }
  const uint32_t bw =
      bucket_width_sec == 0 ? kSpatioTemporalE7DefaultBucketSeconds
                            : bucket_width_sec;
  uint32_t bucket_id = q_t_min / bw;
  if (bucket_lookback == kSpatioTemporalE7PruneSeekLookbackAll) {
    bucket_id = 0;
  } else if (bucket_lookback > 0) {
    bucket_id = (bucket_id >= bucket_lookback) ? (bucket_id - bucket_lookback) : 0;
  }
  out_user_key->assign(kSpatioTemporalE7KeyBytes, '\0');
  (*out_user_key)[0] = static_cast<char>(kSpatioTemporalSegmentKeyTzidMagic);
  (*out_user_key)[1] = static_cast<char>((bucket_id >> 24) & 0xff);
  (*out_user_key)[2] = static_cast<char>((bucket_id >> 16) & 0xff);
  (*out_user_key)[3] = static_cast<char>((bucket_id >> 8) & 0xff);
  (*out_user_key)[4] = static_cast<char>(bucket_id & 0xff);
}

bool TryDecodeSpatioTemporalUserKeyTime(const Slice& user_key,
                                        uint32_t* t_sec) {
  if (t_sec == nullptr || user_key.size() < 5) {
    return false;
  }
  if (IsTmanStSegmentUserKey(user_key)) {
    return TryDecodeTmanStKeyStartTimeSec(user_key, t_sec, 3600.0, 149);
  }
  const char* p = user_key.data();
  const unsigned char magic = static_cast<unsigned char>(p[0]);
  if (magic != kSpatioTemporalUserKeyMagic &&
      magic != kSpatioTemporalSegmentKeyLegacyMagic &&
      magic != kSpatioTemporalSegmentKeyTzidMagic) {
    return false;
  }
  if (magic == kSpatioTemporalUserKeyMagic) {
    Slice s(p + 1, user_key.size() - 1);
    return GetFixed32(&s, t_sec);  // little-endian
  }
  if (magic == kSpatioTemporalSegmentKeyTzidMagic) {
    // E7 key carries only the coarse calendar bucket (no in-key t_start).
    // Return the bucket start as a conservative time estimate; callers only use
    // t_sec / bucket_width (=> bucket_id) for event-time split decisions, which
    // is invariant to whether we return bucket start or the exact t_start.
    uint32_t bucket_id = 0;
    if (!TryDecodeE7HostBucketId(user_key, &bucket_id)) {
      return false;
    }
    *t_sec = bucket_id * kSpatioTemporalE7DefaultBucketSeconds;
    return true;
  }
  // Legacy E6 segment keys encode t_start big-endian for memcmp ordering.
  Slice s(p + 1, user_key.size() - 1);
  return ConsumeFixed32BigEndian(&s, t_sec);
}

bool UserKeySpatioTemporalDisjointFromPruneScan(
    const Slice& user_key,
    const ReadOptions::ExperimentalSpatioTemporalPruneScan& q) {
  if (!q.enable) {
    return false;
  }
  if (IsTmanStSegmentUserKey(user_key)) {
    uint32_t t_start = 0;
    if (!TryDecodeTmanStKeyStartTimeSec(user_key, &t_start, 3600.0, 149)) {
      return false;
    }
    return t_start > q.t_max;
  }
  if (user_key.size() < 13) {
    return false;
  }
  const char* p = user_key.data();
  const unsigned char magic = static_cast<unsigned char>(p[0]);
  if (magic != kSpatioTemporalUserKeyMagic &&
      magic != kSpatioTemporalSegmentKeyLegacyMagic &&
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
    // reject records whose bucket starts after the query window ends
    // (conservative — no false negatives).
    uint32_t bucket_id = 0;
    const uint32_t bw = q.event_time_bucket_width_sec == 0
                            ? kSpatioTemporalE7DefaultBucketSeconds
                            : q.event_time_bucket_width_sec;
    if (!TryDecodeE7HostBucketId(user_key, &bucket_id)) {
      return false;
    }
    return bucket_id * bw > q.t_max;
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
  (void)x_min;
  (void)x_max;
  (void)y_min;
  (void)y_max;
  if (IsTmanStSegmentUserKey(user_key)) {
    uint32_t t_start = 0;
    if (!TryDecodeTmanStKeyStartTimeSec(user_key, &t_start, 3600.0, 149)) {
      return false;
    }
    return t_start <= t_max;
  }
  if (user_key.size() < 13) {
    return false;
  }
  const char* p = user_key.data();
  const unsigned char magic = static_cast<unsigned char>(p[0]);
  if (magic != kSpatioTemporalUserKeyMagic &&
      magic != kSpatioTemporalSegmentKeyLegacyMagic &&
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
    // Coarse calendar bucket only; keep conservative (bucket start <= t_max) and
    // let value verification / block-level metadata make the final decision.
    uint32_t bucket_id = 0;
    if (!TryDecodeE7HostBucketId(user_key, &bucket_id)) {
      return false;
    }
    return bucket_id * kSpatioTemporalE7DefaultBucketSeconds <= t_max;
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
      magic != kSpatioTemporalSegmentKeyLegacyMagic &&
      magic != kSpatioTemporalSegmentKeyTzidMagic) {
    return;
  }
  Slice s(p + 1, user_key.size() - 1);
  uint32_t t_min_v = 0, t_max_v = 0;
  float x_min_v = 0.f, y_min_v = 0.f, x_max_v = 0.f, y_max_v = 0.f;

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
  } else if (magic == kSpatioTemporalSegmentKeyLegacyMagic) {
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
  } else {
    // E7 no longer carries full spatial bounds in-key. For E7 segment keys, the
    // block/file meta must be derived from values (see MergeSpatioTemporalMetaFromKeyValue).
    return;
  }

  if (!*acc_valid) {
    acc->t_min = t_min_v;
    acc->t_max = t_max_v;
    acc->x_min = x_min_v;
    acc->x_max = x_max_v;
    acc->y_min = y_min_v;
    acc->y_max = y_max_v;
    SetSpatioTemporalBitmapAll(acc);
    *acc_valid = true;
  } else {
    acc->t_min = std::min(acc->t_min, t_min_v);
    acc->t_max = std::max(acc->t_max, t_max_v);
    acc->x_min = std::min(acc->x_min, x_min_v);
    acc->x_max = std::max(acc->x_max, x_max_v);
    acc->y_min = std::min(acc->y_min, y_min_v);
    acc->y_max = std::max(acc->y_max, y_max_v);
    SetSpatioTemporalBitmapAll(acc);
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
  if (magic == kSpatioTemporalSegmentKeyTzidMagic) {
    if (!value.has_value()) {
      SetSpatioTemporalMetaMatchAll(acc, acc_valid);
      return;
    }
    MergeSpatioTemporalMetaFromSegmentValue(value.value(), acc, acc_valid);
    return;
  }
  if (value.has_value() && value->size() >= 4 &&
      std::memcmp(value->data(), "TMRV", 4) == 0) {
    if (MergeTmanRocksStptBounds(value.value(), acc, acc_valid)) {
      return;
    }
    SetSpatioTemporalMetaMatchAll(acc, acc_valid);
    return;
  }
  MergeSpatioTemporalMetaFromUserKey(user_key, acc, acc_valid);
}

void CollectSpatioTemporalPointsFromKeyValue(
    const Slice& user_key, const std::optional<Slice>& value,
    std::vector<SpatioTemporalPoint>* out_points, bool* precise_points) {
  if (out_points == nullptr || precise_points == nullptr || user_key.size() < 1) {
    return;
  }
  const unsigned char magic = static_cast<unsigned char>(user_key.data()[0]);
  const char* p = user_key.data();
  Slice s(p + 1, user_key.size() - 1);
  if (magic == kSpatioTemporalUserKeyMagic) {
    uint32_t t_sec = 0, xbits = 0, ybits = 0;
    if (!GetFixed32(&s, &t_sec) || !GetFixed32(&s, &xbits) ||
        !GetFixed32(&s, &ybits)) {
      *precise_points = false;
      return;
    }
    (void)t_sec;
    SpatioTemporalPoint point{};
    std::memcpy(&point.x, &xbits, sizeof(point.x));
    std::memcpy(&point.y, &ybits, sizeof(point.y));
    out_points->push_back(point);
    return;
  }
  if (magic == kSpatioTemporalSegmentKeyTzidMagic) {
    if (!value.has_value()) {
      *precise_points = false;
      return;
    }
    SegmentValueHeaderForStMeta header{};
    if (!TryDecodeSegmentValueHeader(value.value(), &header)) {
      *precise_points = false;
      return;
    }
    if (value->size() == sizeof(SegmentValueHeaderForStMeta) ||
        value->size() < 4 + sizeof(SegmentValueHeaderForStMeta)) {
      *precise_points = false;
      return;
    }
    uint32_t value_magic = 0;
    std::memcpy(&value_magic, value->data(), sizeof(value_magic));
    if (value_magic == kSegmentValueMagicV2) {
      size_t off = 4 + sizeof(SegmentValueHeaderForStMeta);
      for (uint32_t i = 0; i < header.point_count; ++i) {
        if (off + 12 > value->size()) {
          *precise_points = false;
          out_points->clear();
          return;
        }
        SpatioTemporalPoint point{};
        std::memcpy(&point.x, value->data() + off + 4, sizeof(point.x));
        std::memcpy(&point.y, value->data() + off + 8, sizeof(point.y));
        out_points->push_back(point);
        off += 12;
      }
      return;
    }
    if (value_magic == kSegmentValueMagicV3) {
      size_t off = 4 + sizeof(SegmentValueHeaderForStMeta);
      if (off + 1 + 4 > value->size()) {
        *precise_points = false;
        return;
      }
      const uint8_t flags = static_cast<uint8_t>(value->data()[off]);
      off += 1;
      uint32_t master_key_len = 0;
      std::memcpy(&master_key_len, value->data() + off, sizeof(master_key_len));
      off += 4;
      if (off + static_cast<size_t>(master_key_len) > value->size()) {
        *precise_points = false;
        return;
      }
      off += master_key_len;
      const size_t points_bytes =
          static_cast<size_t>(header.point_count) * static_cast<size_t>(12);
      const size_t remaining = value->size() - off;
      const bool is_ghost = (flags & kSegmentValueFlagGhost) != 0;
      if (remaining != points_bytes || (is_ghost && remaining == 0)) {
        *precise_points = false;
        return;
      }
      for (uint32_t i = 0; i < header.point_count; ++i) {
        if (off + 12 > value->size()) {
          *precise_points = false;
          out_points->clear();
          return;
        }
        SpatioTemporalPoint point{};
        std::memcpy(&point.x, value->data() + off + 4, sizeof(point.x));
        std::memcpy(&point.y, value->data() + off + 8, sizeof(point.y));
        out_points->push_back(point);
        off += 12;
      }
      return;
    }
    *precise_points = false;
    return;
  }
  if (value.has_value() && value->size() >= 4 &&
      std::memcmp(value->data(), "TMRV", 4) == 0) {
    CollectTmanRocksStptPoints(value.value(), out_points, precise_points);
    return;
  }
  // Non-ST user keys without TMRV value: coarse bounds only.
  *precise_points = false;
}

void BuildBitmapFromPoints(const std::vector<SpatioTemporalPoint>& points,
                           SpatioTemporalBlockMeta* meta) {
  BuildBitmapFromPointsImpl(points, meta);
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
  OrBitmapWords(acc, block);
}

namespace {

bool PointInSpatioTemporalWindow(uint32_t t, float x, float y,
                                 const ReadOptions::ExperimentalSpatioTemporalPruneScan& q) {
  return t >= q.t_min && t <= q.t_max && x >= q.x_min && x <= q.x_max &&
         y >= q.y_min && y <= q.y_max;
}

bool SegmentValueHasPointInWindow(
    const Slice& value, const ReadOptions::ExperimentalSpatioTemporalPruneScan& q) {
  SegmentValueHeaderForStMeta header{};
  if (!TryDecodeSegmentValueHeader(value, &header)) {
    // Decode failure: value is malformed or uses a legacy format no longer
    // supported (e.g. raw header without magic). Treat as not-in-window.
    return false;
  }
  if (value.size() == sizeof(SegmentValueHeaderForStMeta)) {
    // Header-only value (no points): cannot verify, treat as not-in-window.
    return false;
  }
  if (value.size() < 4 + sizeof(SegmentValueHeaderForStMeta)) {
    // Value too small to contain magic + header + points.
    return false;
  }
  uint32_t value_magic = 0;
  std::memcpy(&value_magic, value.data(), sizeof(value_magic));
  if (value_magic == kSegmentValueMagicV2) {
    size_t off = 4 + sizeof(SegmentValueHeaderForStMeta);
    for (uint32_t i = 0; i < header.point_count; ++i) {
      if (off + 12 > value.size()) {
        // Buffer overflow: malformed value, no point in window.
        return false;
      }
      uint32_t t = 0;
      float x = 0.f, y = 0.f;
      std::memcpy(&t, value.data() + off, sizeof(t));
      std::memcpy(&x, value.data() + off + 4, sizeof(x));
      std::memcpy(&y, value.data() + off + 8, sizeof(y));
      if (PointInSpatioTemporalWindow(t, x, y, q)) {
        return true;
      }
      off += 12;
    }
    return false;
  }
  if (value_magic == kSegmentValueMagicV3) {
    size_t off = 4 + sizeof(SegmentValueHeaderForStMeta);
    if (off + 1 + 4 > value.size()) {
      return false;
    }
    // Skip flags byte (legacy ghost bit, no longer used).
    off += 1;
    uint32_t master_key_len = 0;
    std::memcpy(&master_key_len, value.data() + off, sizeof(master_key_len));
    off += 4;
    if (off + static_cast<size_t>(master_key_len) > value.size()) {
      return false;
    }
    off += master_key_len;
    const size_t points_bytes =
        static_cast<size_t>(header.point_count) * static_cast<size_t>(12);
    const size_t remaining = value.size() - off;
    if (remaining != points_bytes) {
      // Size mismatch: value does not contain exactly the expected points.
      return false;
    }
    for (uint32_t i = 0; i < header.point_count; ++i) {
      if (off + 12 > value.size()) {
        return false;
      }
      uint32_t t = 0;
      float x = 0.f, y = 0.f;
      std::memcpy(&t, value.data() + off, sizeof(t));
      std::memcpy(&x, value.data() + off + 4, sizeof(x));
      std::memcpy(&y, value.data() + off + 8, sizeof(y));
      if (PointInSpatioTemporalWindow(t, x, y, q)) {
        return true;
      }
      off += 12;
    }
    return false;
  }
  // Unknown value magic: unsupported format, treat as not-in-window.
  return false;
}

}  // namespace

bool KeyValueHasPointInSpatioTemporalWindow(
    const Slice& user_key, const Slice& value,
    const ReadOptions::ExperimentalSpatioTemporalPruneScan& q) {
  if (!q.enable) {
    return true;
  }
  if (user_key.size() < 1) {
    return false;
  }
  const unsigned char magic = static_cast<unsigned char>(user_key.data()[0]);
  if (magic == kSpatioTemporalUserKeyMagic) {
    return UserKeySpatioTemporalInWindow(user_key, q.t_min, q.t_max, q.x_min,
                                         q.x_max, q.y_min, q.y_max);
  }
  if (magic == kSpatioTemporalSegmentKeyLegacyMagic) {
    return UserKeySpatioTemporalInWindow(user_key, q.t_min, q.t_max, q.x_min,
                                         q.x_max, q.y_min, q.y_max);
  }
  if (magic == kSpatioTemporalSegmentKeyTzidMagic) {
    return SegmentValueHasPointInWindow(value, q);
  }
  if (value.size() >= 4 && std::memcmp(value.data(), "TMRV", 4) == 0) {
    return TmanRocksValueHasPointInSpatioTemporalWindow(value, q);
  }
  return true;
}

bool SpatioTemporalBitmapMayIntersectQuery(
    const SpatioTemporalBlockMeta& meta,
    const ReadOptions::ExperimentalSpatioTemporalPruneScan& q) {
  const float ox_min = std::max(meta.x_min, q.x_min);
  const float ox_max = std::min(meta.x_max, q.x_max);
  const float oy_min = std::max(meta.y_min, q.y_min);
  const float oy_max = std::min(meta.y_max, q.y_max);
  if (ox_min > ox_max || oy_min > oy_max) {
    return false;
  }
  const float w = meta.x_max - meta.x_min;
  const float h = meta.y_max - meta.y_min;
  if (!(w > 0.0f) || !(h > 0.0f)) {
    return true;
  }
  const auto begin_x = ClampBitmapCoord((ox_min - meta.x_min) / w);
  const auto end_x = ClampBitmapCoord((ox_max - meta.x_min) / w);
  const auto begin_y = ClampBitmapCoord((oy_min - meta.y_min) / h);
  const auto end_y = ClampBitmapCoord((oy_max - meta.y_min) / h);
  for (uint32_t iy = begin_y; iy <= end_y; ++iy) {
    for (uint32_t ix = begin_x; ix <= end_x; ++ix) {
      const uint32_t bit_index = iy * kSpatioTemporalBitmapGridSize + ix;
      const size_t word = bit_index / 64;
      const uint32_t bit = bit_index % 64;
      if ((meta.bitmap_words[word] & (uint64_t{1} << bit)) != 0) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace ROCKSDB_NAMESPACE

// Decode TMRV1 + STPT trailer written by TMan-RocksDB (Java TmanRocksValueCodec).

#include "table/tman_rocksdb_value.h"

#include <cstring>
#include <functional>
#include <limits>

namespace ROCKSDB_NAMESPACE {
namespace {

constexpr char kTmrvMagic[] = "TMRV";
constexpr char kStptMagic[] = "STPT";

inline uint32_t ReadBE32(const char* p) {
  return (static_cast<uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 8) |
         static_cast<uint32_t>(static_cast<unsigned char>(p[3]));
}

inline uint16_t ReadBE16(const char* p) {
  return static_cast<uint16_t>(
      (static_cast<unsigned char>(p[0]) << 8) |
      static_cast<unsigned char>(p[1]));
}

inline uint32_t ReadLE32(const char* p) {
  return static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

bool SkipTmrv1Header(const Slice& value, size_t* stpt_offset) {
  if (value.size() < 12 || std::memcmp(value.data(), kTmrvMagic, 4) != 0) {
    return false;
  }
  const char* p = value.data() + 4;
  const char* end = value.data() + value.size();
  p += 4;  // version
  if (p + 4 > end) {
    return false;
  }
  const uint32_t ncols = ReadBE32(p);
  p += 4;
  for (uint32_t i = 0; i < ncols; ++i) {
    if (p + 2 > end) {
      return false;
    }
    const uint16_t qlen = ReadBE16(p);
    p += 2;
    if (p + qlen + 4 > end) {
      return false;
    }
    p += qlen;
    const uint32_t vlen = ReadBE32(p);
    p += 4;
    if (p + vlen > end) {
      return false;
    }
    p += vlen;
  }
  *stpt_offset = static_cast<size_t>(p - value.data());
  return true;
}

bool PointInWindow(uint32_t t, float x, float y,
                   const ReadOptions::ExperimentalSpatioTemporalPruneScan& q) {
  return t >= q.t_min && t <= q.t_max && x >= q.x_min && x <= q.x_max &&
         y >= q.y_min && y <= q.y_max;
}

}  // namespace

bool TmanRocksValueHasPointInSpatioTemporalWindow(
    const Slice& value,
    const ReadOptions::ExperimentalSpatioTemporalPruneScan& q) {
  size_t off = 0;
  if (!SkipTmrv1Header(value, &off)) {
    return true;
  }
  if (off + 12 > value.size()) {
    return true;
  }
  if (std::memcmp(value.data() + off, kStptMagic, 4) != 0) {
    return true;
  }
  const char* p = value.data() + off + 4;
  const char* end = value.data() + value.size();
  if (p + 8 > end) {
    return true;
  }
  const uint32_t version = ReadLE32(p);
  p += 4;
  const uint32_t n = ReadLE32(p);
  p += 4;
  if (version != 1) {
    return true;
  }
  if (p + static_cast<size_t>(n) * 12 > end) {
    return true;
  }
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t t = 0;
    float x = 0.f;
    float y = 0.f;
    std::memcpy(&t, p, sizeof(t));
    p += 4;
    std::memcpy(&x, p, sizeof(x));
    p += 4;
    std::memcpy(&y, p, sizeof(y));
    p += 4;
    if (PointInWindow(t, x, y, q)) {
      return true;
    }
  }
  return false;
}

bool ForEachStptPoint(
    const Slice& value,
    const std::function<bool(uint32_t t, float x, float y)>& fn) {
  size_t off = 0;
  if (!SkipTmrv1Header(value, &off)) {
    return false;
  }
  if (off + 12 > value.size()) {
    return false;
  }
  if (std::memcmp(value.data() + off, kStptMagic, 4) != 0) {
    return false;
  }
  const char* p = value.data() + off + 4;
  const char* end = value.data() + value.size();
  if (p + 8 > end) {
    return false;
  }
  const uint32_t version = ReadLE32(p);
  p += 4;
  const uint32_t n = ReadLE32(p);
  p += 4;
  if (version != 1 || p + static_cast<size_t>(n) * 12 > end) {
    return false;
  }
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t t = 0;
    float x = 0.f;
    float y = 0.f;
    std::memcpy(&t, p, sizeof(t));
    p += 4;
    std::memcpy(&x, p, sizeof(x));
    p += 4;
    std::memcpy(&y, p, sizeof(y));
    p += 4;
    if (!fn(t, x, y)) {
      return false;
    }
  }
  return true;
}

bool MergeTmanRocksStptBounds(const Slice& value, SpatioTemporalBlockMeta* acc,
                              bool* acc_valid) {
  if (acc == nullptr || acc_valid == nullptr) {
    return false;
  }
  bool any = false;
  uint32_t t_min = UINT32_MAX;
  uint32_t t_max = 0;
  float x_min = 0.f, y_min = 0.f, x_max = 0.f, y_max = 0.f;
  const bool ok = ForEachStptPoint(value, [&](uint32_t t, float x, float y) {
    if (!any) {
      t_min = t_max = t;
      x_min = x_max = x;
      y_min = y_max = y;
      any = true;
    } else {
      t_min = std::min(t_min, t);
      t_max = std::max(t_max, t);
      x_min = std::min(x_min, x);
      x_max = std::max(x_max, x);
      y_min = std::min(y_min, y);
      y_max = std::max(y_max, y);
    }
    return true;
  });
  if (!ok || !any) {
    return false;
  }
  if (!*acc_valid) {
    acc->t_min = t_min;
    acc->t_max = t_max;
    acc->x_min = x_min;
    acc->x_max = x_max;
    acc->y_min = y_min;
    acc->y_max = y_max;
    *acc_valid = true;
  } else {
    acc->t_min = std::min(acc->t_min, t_min);
    acc->t_max = std::max(acc->t_max, t_max);
    acc->x_min = std::min(acc->x_min, x_min);
    acc->x_max = std::max(acc->x_max, x_max);
    acc->y_min = std::min(acc->y_min, y_min);
    acc->y_max = std::max(acc->y_max, y_max);
  }
  return true;
}

void CollectTmanRocksStptPoints(const Slice& value,
                                std::vector<SpatioTemporalPoint>* out_points,
                                bool* precise_points) {
  if (out_points == nullptr || precise_points == nullptr) {
    return;
  }
  const bool ok = ForEachStptPoint(value, [&](uint32_t, float x, float y) {
    out_points->push_back(SpatioTemporalPoint{x, y});
    return true;
  });
  *precise_points = ok;
}

}  // namespace ROCKSDB_NAMESPACE

// ST-RocksDB user key: [2B period BE][8B spatial BE][tid UTF-8]
// Built in st_bucket_ingest_build (C++ only). Value / Manifest / Local index unchanged.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "segment_value_codec.hpp"

namespace st_tman {

constexpr size_t kTmanStKeyPrefixBytes = 10;
constexpr int kDefaultResolution = 10;
constexpr int kDefaultAlpha = 4;
constexpr int kDefaultBeta = 4;
constexpr double kDefaultTimeBin = 3600.0;
constexpr int kDefaultTimeBinNums = 149;

struct StKeyBuildConfig {
  double xmin = 115.47173;
  double ymin = 39.04912;
  double xmax = 117.82745;
  double ymax = 41.21767;
  double time_bin = kDefaultTimeBin;
  int time_bin_nums = kDefaultTimeBinNums;
  int resolution = kDefaultResolution;
  int alpha = kDefaultAlpha;
  int beta = kDefaultBeta;
};

inline void WriteInt16BE(std::string* out, int16_t v) {
  const uint16_t u = static_cast<uint16_t>(v);
  out->push_back(static_cast<char>((u >> 8) & 0xff));
  out->push_back(static_cast<char>(u & 0xff));
}

inline void WriteUInt64BE(std::string* out, uint64_t v) {
  for (int i = 7; i >= 0; --i) {
    out->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
  }
}

inline int16_t PeriodNumberFromTimeMs(int64_t time_ms, double time_bin,
                                      int time_bin_nums) {
  const int64_t period_ms = static_cast<int64_t>(
      time_bin * static_cast<double>(time_bin_nums) * 3600.0 * 1000.0);
  if (period_ms <= 0) {
    return 0;
  }
  return static_cast<int16_t>(time_ms / period_ms);
}

inline int16_t PeriodNumberFromUnixSec(uint32_t t_sec, double time_bin,
                                       int time_bin_nums) {
  return PeriodNumberFromTimeMs(static_cast<int64_t>(t_sec) * 1000, time_bin,
                                time_bin_nums);
}

inline bool ParseTdriveSegmentTid(const std::string& segment_id, std::string* tid) {
  if (tid == nullptr) {
    return false;
  }
  const size_t c1 = segment_id.find(':');
  if (c1 == std::string::npos) {
    return false;
  }
  const size_t c2 = segment_id.find(':', c1 + 1);
  if (c2 == std::string::npos || c2 + 1 >= segment_id.size()) {
    return false;
  }
  *tid = segment_id.substr(c2 + 1);
  return !tid->empty();
}

inline std::string BuildTmanStRowKey(int16_t period, uint64_t spatial_index,
                                     const std::string& tid) {
  std::string k;
  k.reserve(kTmanStKeyPrefixBytes + tid.size());
  WriteInt16BE(&k, period);
  WriteUInt64BE(&k, spatial_index);
  k.append(tid);
  return k;
}

inline uint64_t InterleaveBits2D(uint32_t x, uint32_t y, int max_level) {
  uint64_t code = 0;
  for (int i = max_level - 1; i >= 0; --i) {
    const uint64_t x_bit = (x >> i) & 1U;
    const uint64_t y_bit = (y >> i) & 1U;
    code = (code << 2) | (x_bit << 1) | y_bit;
  }
  return code;
}

inline uint64_t ComputeNativeStSpatialIndex(
    const std::vector<segval::SegmentPoint>& points,
    const StKeyBuildConfig& cfg) {
  if (points.empty()) {
    return 0;
  }
  double cx = 0.0;
  double cy = 0.0;
  for (const auto& p : points) {
    cx += static_cast<double>(p.lon);
    cy += static_cast<double>(p.lat);
  }
  const double n = static_cast<double>(points.size());
  cx /= n;
  cy /= n;

  const double wx = std::max(cfg.xmax - cfg.xmin, 1e-9);
  const double wy = std::max(cfg.ymax - cfg.ymin, 1e-9);
  const int res = (std::max)(1, (std::min)(cfg.resolution, 16));
  const uint32_t max_cell = (1U << res) - 1U;
  const uint32_t ix = static_cast<uint32_t>((std::min)(
      1.0, (std::max)(0.0, (cx - cfg.xmin) / wx)) * static_cast<double>(max_cell));
  const uint32_t iy = static_cast<uint32_t>((std::min)(
      1.0, (std::max)(0.0, (cy - cfg.ymin) / wy)) * static_cast<double>(max_cell));

  const uint64_t location = InterleaveBits2D(ix, iy, res);
  const int move_bits = cfg.alpha * cfg.beta;
  return location << static_cast<unsigned>(move_bits);
}

inline std::string BuildNativeStRowKey(
    uint32_t t_start_sec, const std::vector<segval::SegmentPoint>& points,
    const std::string& segment_id, const StKeyBuildConfig& cfg) {
  std::string tid;
  if (!ParseTdriveSegmentTid(segment_id, &tid)) {
    return {};
  }
  const int16_t period =
      PeriodNumberFromUnixSec(t_start_sec, cfg.time_bin, cfg.time_bin_nums);
  const uint64_t spatial = ComputeNativeStSpatialIndex(points, cfg);
  return BuildTmanStRowKey(period, spatial, tid);
}

inline bool IsTmanStRowKeyLayout(const char* data, size_t size) {
  if (data == nullptr || size < kTmanStKeyPrefixBytes + 1) {
    return false;
  }
  const unsigned char m = static_cast<unsigned char>(data[0]);
  return m != 0xE5 && m != 0xE6 && m != 0xE7;
}

}  // namespace st_tman

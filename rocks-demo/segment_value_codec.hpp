// Segment KV value layout for --st-segment-keys ingest (st_meta_smoke, st_bucket_ingest_build).
// V2 (required for new ingests): magic + fixed header + packed points (t, lon, lat per point).
// Legacy (28 bytes, no magic): header only — still decoded for old DBs.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "trajectory_kv.hpp"

namespace segval {

static constexpr uint32_t kSegmentValueMagicV2 = 0x32676553;  // bytes 'S','e','g','2'
static constexpr uint32_t kSegmentValueMagicV3 = 0x33676553;  // bytes 'S','e','g','3'
static constexpr uint8_t kSegmentValueFlagGhost = 0x01;

#pragma pack(push, 1)
struct SegmentValueHeader {
  uint32_t t_start = 0;
  uint32_t t_end = 0;
  float x_min = 0.f;
  float y_min = 0.f;
  float x_max = 0.f;
  float y_max = 0.f;
  uint32_t point_count = 0;
};
#pragma pack(pop)

static_assert(sizeof(SegmentValueHeader) == 28, "segment header size");

struct SegmentPoint {
  uint32_t unix_s = 0;
  float lon = 0.f;
  float lat = 0.f;
};

inline std::string EncodeSegmentValueV2(const SegmentValueHeader& head,
                                       const std::vector<SegmentPoint>& points) {
  if (points.size() != static_cast<size_t>(head.point_count)) {
    return {};
  }
  std::string out;
  out.resize(4 + sizeof(SegmentValueHeader) + points.size() * 12);
  std::memcpy(out.data(), &kSegmentValueMagicV2, 4);
  std::memcpy(out.data() + 4, &head, sizeof(head));
  size_t off = 4 + sizeof(head);
  for (const auto& p : points) {
    std::memcpy(out.data() + off, &p.unix_s, 4);
    std::memcpy(out.data() + off + 4, &p.lon, 4);
    std::memcpy(out.data() + off + 8, &p.lat, 4);
    off += 12;
  }
  return out;
}

struct DecodedSegmentValue {
  SegmentValueHeader header{};
  std::vector<SegmentPoint> points;
  bool is_legacy = false;
  bool is_ghost = false;
  std::string master_key;
};

inline bool DecodeSegmentValue(const std::string& blob, DecodedSegmentValue* out) {
  if (out == nullptr) {
    return false;
  }
  out->points.clear();
  out->is_legacy = false;
  out->is_ghost = false;
  out->master_key.clear();
  if (blob.size() == sizeof(SegmentValueHeader)) {
    std::memcpy(&out->header, blob.data(), sizeof(SegmentValueHeader));
    out->is_legacy = true;
    return true;
  }
  if (blob.size() < 4 + sizeof(SegmentValueHeader)) {
    return false;
  }
  uint32_t magic = 0;
  std::memcpy(&magic, blob.data(), 4);
  if (magic != kSegmentValueMagicV2 && magic != kSegmentValueMagicV3) {
    return false;
  }
  if (magic == kSegmentValueMagicV2) {
    std::memcpy(&out->header, blob.data() + 4, sizeof(SegmentValueHeader));
    const size_t expected =
        4 + sizeof(SegmentValueHeader) +
        static_cast<size_t>(out->header.point_count) * 12;
    if (blob.size() != expected) {
      return false;
    }
    out->points.resize(out->header.point_count);
    size_t off = 4 + sizeof(SegmentValueHeader);
    for (uint32_t i = 0; i < out->header.point_count; ++i) {
      std::memcpy(&out->points[i].unix_s, blob.data() + off, 4);
      std::memcpy(&out->points[i].lon, blob.data() + off + 4, 4);
      std::memcpy(&out->points[i].lat, blob.data() + off + 8, 4);
      off += 12;
    }
    out->is_legacy = false;
    out->is_ghost = false;
    return true;
  }
  // V3 layout:
  // [magic(4)][header(28)][flags(1)][master_key_len(4)][master_key][points...]
  if (blob.size() < 4 + sizeof(SegmentValueHeader) + 1 + 4) {
    return false;
  }
  size_t off = 4;
  std::memcpy(&out->header, blob.data() + off, sizeof(SegmentValueHeader));
  off += sizeof(SegmentValueHeader);
  const uint8_t flags = static_cast<uint8_t>(blob[off]);
  off += 1;
  uint32_t master_key_len = 0;
  std::memcpy(&master_key_len, blob.data() + off, 4);
  off += 4;
  if (off + static_cast<size_t>(master_key_len) > blob.size()) {
    return false;
  }
  out->is_ghost = ((flags & kSegmentValueFlagGhost) != 0);
  if (master_key_len > 0) {
    out->master_key.assign(blob.data() + off, master_key_len);
  }
  off += master_key_len;
  const size_t points_bytes =
      static_cast<size_t>(out->header.point_count) * static_cast<size_t>(12);
  const size_t remaining = blob.size() - off;
  if (out->is_ghost) {
    // Ghost values may omit the point payload entirely and rely on master_key
    // follow-up for full materialization. We also accept fully materialized
    // ghosts for transition/debug use.
    if (remaining != 0 && remaining != points_bytes) {
      return false;
    }
  } else if (remaining != points_bytes) {
    return false;
  }
  if (remaining == points_bytes) {
    out->points.resize(out->header.point_count);
    for (uint32_t i = 0; i < out->header.point_count; ++i) {
      std::memcpy(&out->points[i].unix_s, blob.data() + off, 4);
      std::memcpy(&out->points[i].lon, blob.data() + off + 4, 4);
      std::memcpy(&out->points[i].lat, blob.data() + off + 8, 4);
      off += 12;
    }
  }
  if (out->is_ghost && out->master_key.empty()) {
    // Ghost entries must carry a master-key reference.
    return false;
  }
  out->is_legacy = false;
  return true;
}

inline std::string EncodeSegmentValueV3(const SegmentValueHeader& head,
                                        const std::vector<SegmentPoint>& points,
                                        bool is_ghost,
                                        const std::string& master_key) {
  const size_t expected_points = static_cast<size_t>(head.point_count);
  const bool points_match = (points.size() == expected_points);
  const bool ghost_without_payload = is_ghost && points.empty();
  if (!points_match && !ghost_without_payload) {
    return {};
  }
  if (is_ghost && master_key.empty()) {
    return {};
  }
  if (master_key.size() > static_cast<size_t>(UINT32_MAX)) {
    return {};
  }
  std::string out;
  out.resize(4 + sizeof(SegmentValueHeader) + 1 + 4 + master_key.size() +
             points.size() * 12);
  size_t off = 0;
  std::memcpy(out.data() + off, &kSegmentValueMagicV3, 4);
  off += 4;
  std::memcpy(out.data() + off, &head, sizeof(head));
  off += sizeof(head);
  const uint8_t flags = is_ghost ? kSegmentValueFlagGhost : 0;
  out[off] = static_cast<char>(flags);
  off += 1;
  const uint32_t master_key_len = static_cast<uint32_t>(master_key.size());
  std::memcpy(out.data() + off, &master_key_len, 4);
  off += 4;
  if (!master_key.empty()) {
    std::memcpy(out.data() + off, master_key.data(), master_key.size());
    off += master_key.size();
  }
  for (const auto& p : points) {
    std::memcpy(out.data() + off, &p.unix_s, 4);
    std::memcpy(out.data() + off + 4, &p.lon, 4);
    std::memcpy(out.data() + off + 8, &p.lat, 4);
    off += 12;
  }
  return out;
}

inline std::string EncodeGhostSegmentValueV3(const SegmentValueHeader& head,
                                             const std::string& master_key) {
  return EncodeSegmentValueV3(head, /*points=*/{}, /*is_ghost=*/true, master_key);
}

// segments_points.csv: segment_id, dataset, point_index, unix_time_s, lon, lat, ...
// Points per segment ordered by point_index.
inline bool LoadSegmentPointsCsv(
    const std::string& path,
    std::unordered_map<std::string, std::vector<SegmentPoint>>* by_segment) {
  if (by_segment == nullptr) {
    return false;
  }
  by_segment->clear();
  std::ifstream in(path);
  if (!in) {
    std::cerr << "Cannot open segment points CSV: " << path << "\n";
    return false;
  }
  std::string header;
  if (!std::getline(in, header)) {
    std::cerr << "Empty segment points CSV.\n";
    return false;
  }
  // Per-segment vectors avoid std::map node overhead (~4GB CSV -> much lower RSS).
  std::unordered_map<std::string, std::vector<std::pair<int, SegmentPoint>>> indexed;
  indexed.reserve(150000);
  std::string line;
  uint64_t lines = 0;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    auto f = trajkv::SplitCsvLine(line);
    if (f.size() < 6) {
      std::cerr << "Bad row in segment points CSV (need >= 6 fields)\n";
      return false;
    }
    const std::string& segment_id = f[0];
    const int point_index = std::stoi(f[2]);
    SegmentPoint p{};
    p.unix_s = static_cast<uint32_t>(std::stoul(f[3]));
    p.lon = static_cast<float>(std::stod(f[4]));
    p.lat = static_cast<float>(std::stod(f[5]));
    indexed[segment_id].emplace_back(point_index, p);
    if (++lines % 5000000 == 0) {
      std::cerr << "  points CSV lines=" << lines << " segments=" << indexed.size()
                << "\n";
    }
  }
  std::cerr << "  points CSV done lines=" << lines << " segments=" << indexed.size()
            << " sorting...\n";
  by_segment->reserve(indexed.size());
  for (auto& kv : indexed) {
    auto& pairs = kv.second;
    std::sort(pairs.begin(), pairs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<SegmentPoint> vec;
    vec.reserve(pairs.size());
    for (const auto& pr : pairs) {
      vec.push_back(pr.second);
    }
    (*by_segment)[std::move(kv.first)] = std::move(vec);
  }
  indexed.clear();
  indexed.rehash(0);
  return true;
}

}  // namespace segval

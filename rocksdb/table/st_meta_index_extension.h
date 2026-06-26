// SPDX-License-Identifier: Apache-2.0
// Spatio-temporal summary embedded after standard IndexValue bytes (experimental).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ROCKSDB_NAMESPACE {

constexpr uint32_t kSpatioTemporalBitmapGridSize = 16;
constexpr size_t kSpatioTemporalBitmapWords = 4;

struct SpatioTemporalBlockMeta {
  uint32_t t_min = 0;
  uint32_t t_max = 0;
  float x_min = 0.f;
  float y_min = 0.f;
  float x_max = 0.f;
  float y_max = 0.f;
  std::array<uint64_t, kSpatioTemporalBitmapWords> bitmap_words{};
};

constexpr size_t kSpatioTemporalIndexTailBytes() {
  return sizeof(uint32_t) * 2 + sizeof(float) * 4 +
         sizeof(uint64_t) * kSpatioTemporalBitmapWords;
}

inline void SetSpatioTemporalBitmapAll(SpatioTemporalBlockMeta* meta) {
  if (meta == nullptr) {
    return;
  }
  meta->bitmap_words.fill(UINT64_MAX);
}

inline void ClearSpatioTemporalBitmap(SpatioTemporalBlockMeta* meta) {
  if (meta == nullptr) {
    return;
  }
  meta->bitmap_words.fill(0);
}

// User property written to SST property block; flushed into MANIFEST via
// FileMetaData (experimental).
inline const char* kExperimentalStFileBoundsPropertyName() {
  return "rocksdb.experimental.st_file_bounds";
}

}  // namespace ROCKSDB_NAMESPACE

// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "table/format.h"

namespace ROCKSDB_NAMESPACE {

extern const char kStIndexBlockName[];

constexpr size_t kStIndexKeyBytes = 8;
constexpr size_t kStIndexHeaderBytes = 4;
constexpr uint32_t kStIndexDefaultBucketSeconds = 3600;

void EncodeStIndexHeader(uint32_t bucket_seconds, std::string* dst);
Status DecodeStIndexHeader(const Slice& input, uint32_t* bucket_seconds);
void EncodeStIndexKey(uint32_t bs, uint32_t pe, std::string* dst);
void EncodeStIndexValue(uint32_t min_timestamp, uint32_t max_timestamp,
                        float x_min, float y_min, float x_max, float y_max,
                        const BlockHandle& handle, std::string* dst);

struct StIndexEntry {
  uint32_t bs = 0;
  uint32_t pe = 0;
  uint32_t min_timestamp = 0;
  uint32_t max_timestamp = 0;
  float x_min = 0;
  float y_min = 0;
  float x_max = 0;
  float y_max = 0;
  BlockHandle block_handle;
};

class StIndexBlockBuilder {
 public:
  explicit StIndexBlockBuilder(uint32_t bucket_seconds);
  void Add(uint32_t bs, uint32_t be, uint32_t min_timestamp,
           uint32_t max_timestamp, float x_min, float y_min, float x_max,
           float y_max, const BlockHandle& block_handle);
  const std::vector<StIndexEntry>& entries() const { return entries_; }

 private:
  uint32_t bucket_seconds_;
  uint32_t prefix_max_be_ = 0;
  std::vector<StIndexEntry> entries_;
};

}  // namespace ROCKSDB_NAMESPACE

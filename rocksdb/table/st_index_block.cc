// SPDX-License-Identifier: Apache-2.0

#include "table/st_index_block.h"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "util/coding.h"

namespace ROCKSDB_NAMESPACE {

const char kStIndexBlockName[] = "st index block";

namespace {

void PutFixed32BE(char* dst, uint32_t value) {
  dst[0] = static_cast<char>(value >> 24);
  dst[1] = static_cast<char>(value >> 16);
  dst[2] = static_cast<char>(value >> 8);
  dst[3] = static_cast<char>(value);
}

void PutFloatLE(std::string* dst, float value) {
  uint32_t bits;
  static_assert(sizeof(bits) == sizeof(value), "float size");
  std::memcpy(&bits, &value, sizeof(bits));
  PutFixed32(dst, bits);
}

}  // namespace

void EncodeStIndexHeader(uint32_t bucket_seconds, std::string* dst) {
  PutFixed32(dst, bucket_seconds);
}

Status DecodeStIndexHeader(const Slice& input, uint32_t* bucket_seconds) {
  if (bucket_seconds == nullptr) {
    return Status::InvalidArgument("null ST index header output");
  }
  Slice encoded = input;
  if (!GetFixed32(&encoded, bucket_seconds) || *bucket_seconds == 0) {
    return Status::Corruption("invalid ST index header");
  }
  return Status::OK();
}

void EncodeStIndexKey(uint32_t bs, uint32_t pe, std::string* dst) {
  char key[kStIndexKeyBytes];
  PutFixed32BE(key, bs);
  PutFixed32BE(key + 4, pe);
  dst->append(key, sizeof(key));
}

void EncodeStIndexValue(uint32_t min_timestamp, uint32_t max_timestamp,
                        float x_min, float y_min, float x_max, float y_max,
                        const BlockHandle& handle, std::string* dst) {
  PutFixed32(dst, min_timestamp);
  PutFixed32(dst, max_timestamp);
  PutFloatLE(dst, x_min);
  PutFloatLE(dst, y_min);
  PutFloatLE(dst, x_max);
  PutFloatLE(dst, y_max);
  PutFixed64(dst, handle.offset());
  PutFixed64(dst, handle.size());
}

StIndexBlockBuilder::StIndexBlockBuilder(uint32_t bucket_seconds)
    : bucket_seconds_(bucket_seconds == 0 ? kStIndexDefaultBucketSeconds
                                          : bucket_seconds) {}

void StIndexBlockBuilder::Add(uint32_t bs, uint32_t be,
                              uint32_t min_timestamp,
                              uint32_t max_timestamp, float x_min,
                              float y_min, float x_max, float y_max,
                              const BlockHandle& block_handle) {
  assert(entries_.empty() || bs >= entries_.back().bs);
  prefix_max_be_ = std::max(prefix_max_be_, be);
  entries_.push_back({bs, prefix_max_be_, min_timestamp, max_timestamp,
                       x_min, y_min, x_max, y_max, block_handle});
}

}  // namespace ROCKSDB_NAMESPACE

// SPDX-License-Identifier: Apache-2.0

#include "table/index_value_codec.h"

#include <cstring>

#include "table/format.h"
#include "util/coding.h"

namespace ROCKSDB_NAMESPACE {

namespace {

void AppendFloatAsLittleEndianU32(std::string* dst, float v) {
  static_assert(sizeof(float) == sizeof(uint32_t), "float size");
  uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  PutFixed32(dst, bits);
}

bool ReadFloatFromLittleEndianU32(Slice* input, float* out) {
  if (input->size() < 4) {
    return false;
  }
  uint32_t bits = 0;
  if (!GetFixed32(input, &bits)) {
    return false;
  }
  std::memcpy(out, &bits, sizeof(*out));
  return true;
}

}  // namespace

void AppendSpatioTemporalIndexTail(std::string* dst,
                                   const SpatioTemporalBlockMeta& meta) {
  PutFixed32(dst, meta.t_min);
  PutFixed32(dst, meta.t_max);
  AppendFloatAsLittleEndianU32(dst, meta.x_min);
  AppendFloatAsLittleEndianU32(dst, meta.y_min);
  AppendFloatAsLittleEndianU32(dst, meta.x_max);
  AppendFloatAsLittleEndianU32(dst, meta.y_max);
  for (uint64_t word : meta.bitmap_words) {
    PutFixed64(dst, word);
  }
  PutFixed32(dst, meta.prefix_max_t_end);
}

Status DecodeSpatioTemporalIndexTail(Slice* input,
                                     SpatioTemporalBlockMeta* meta) {
  if (meta == nullptr) {
    return Status::InvalidArgument("null meta");
  }
  // Accept both legacy (56 bytes, no prefix_max_t_end) and current
  // (60 bytes, with prefix_max_t_end) formats.
  if (input->size() < kSpatioTemporalIndexTailBytesLegacy()) {
    return Status::Corruption("truncated spatio-temporal tail");
  }
  const bool has_prefix_max =
      input->size() >= kSpatioTemporalIndexTailBytes();
  uint32_t t_min = 0;
  uint32_t t_max = 0;
  if (!GetFixed32(input, &t_min) || !GetFixed32(input, &t_max)) {
    return Status::Corruption("bad T_min/T_max");
  }
  float x_min = 0, y_min = 0, x_max = 0, y_max = 0;
  if (!ReadFloatFromLittleEndianU32(input, &x_min) ||
      !ReadFloatFromLittleEndianU32(input, &y_min) ||
      !ReadFloatFromLittleEndianU32(input, &x_max) ||
      !ReadFloatFromLittleEndianU32(input, &y_max)) {
    return Status::Corruption("bad MBR floats");
  }
  for (size_t i = 0; i < meta->bitmap_words.size(); ++i) {
    if (!GetFixed64(input, &meta->bitmap_words[i])) {
      return Status::Corruption("bad bitmap");
    }
  }
  meta->t_min = t_min;
  meta->t_max = t_max;
  meta->x_min = x_min;
  meta->y_min = y_min;
  meta->x_max = x_max;
  meta->y_max = y_max;
  if (has_prefix_max) {
    if (!GetFixed32(input, &meta->prefix_max_t_end)) {
      return Status::Corruption("bad prefix_max_t_end");
    }
  } else {
    meta->prefix_max_t_end = 0;
  }
  return Status::OK();
}

Status EncodeSpatioTemporalIndexValue(std::string* dst, const BlockHandle& handle,
                                      const SpatioTemporalBlockMeta& meta) {
  handle.EncodeTo(dst);
  AppendSpatioTemporalIndexTail(dst, meta);
  return Status::OK();
}

Status DecodeSpatioTemporalIndexValue(Slice* input, BlockHandle* handle,
                                      SpatioTemporalBlockMeta* meta) {
  if (handle == nullptr || meta == nullptr) {
    return Status::InvalidArgument("null out-parameter");
  }
  Status hs = handle->DecodeFrom(input);
  if (!hs.ok()) {
    return Status::Corruption("bad BlockHandle in spatio-temporal index value");
  }
  return DecodeSpatioTemporalIndexTail(input, meta);
}

}  // namespace ROCKSDB_NAMESPACE

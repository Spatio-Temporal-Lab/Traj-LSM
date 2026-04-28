#pragma once

#include <cstring>
#include <string>
#include <utility>

#include <rocksdb/db.h>
#include <rocksdb/options.h>

#include "segment_value_codec.hpp"

namespace segval {

struct MaterializedSegmentValue {
  DecodedSegmentValue decoded{};
  bool was_ghost = false;
  bool master_lookup_attempted = false;
  bool master_lookup_ok = false;
  bool master_decode_failed = false;
  bool master_is_ghost = false;
  bool master_meta_mismatch = false;
};

inline bool SegmentMetaMatches(const SegmentValueHeader& a,
                               const SegmentValueHeader& b) {
  return a.t_start == b.t_start && a.t_end == b.t_end && a.point_count == b.point_count &&
         std::memcmp(&a.x_min, &b.x_min, sizeof(float)) == 0 &&
         std::memcmp(&a.y_min, &b.y_min, sizeof(float)) == 0 &&
         std::memcmp(&a.x_max, &b.x_max, sizeof(float)) == 0 &&
         std::memcmp(&a.y_max, &b.y_max, sizeof(float)) == 0;
}

inline bool DecodeAndMaterializeSegmentValue(rocksdb::DB* db,
                                             const rocksdb::ReadOptions& read_options,
                                             const rocksdb::Slice& raw_value,
                                             MaterializedSegmentValue* out) {
  if (db == nullptr || out == nullptr) {
    return false;
  }
  *out = MaterializedSegmentValue{};
  const std::string blob(raw_value.data(), raw_value.size());
  if (!DecodeSegmentValue(blob, &out->decoded)) {
    return false;
  }
  out->was_ghost = out->decoded.is_ghost;
  if (!out->decoded.is_ghost) {
    return true;
  }
  out->master_lookup_attempted = true;
  std::string master_blob;
  rocksdb::Status s =
      db->Get(read_options, out->decoded.master_key, &master_blob);
  if (!s.ok()) {
    rocksdb::ReadOptions ro = read_options;
    ro.total_order_seek = true;
    std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro));
    it->Seek(out->decoded.master_key);
    if (it->Valid() && it->key().compare(rocksdb::Slice(out->decoded.master_key)) == 0) {
      master_blob.assign(it->value().data(), it->value().size());
      s = rocksdb::Status::OK();
    } else {
      return true;
    }
  }
  out->master_lookup_ok = true;
  DecodedSegmentValue master{};
  if (!DecodeSegmentValue(master_blob, &master)) {
    out->master_decode_failed = true;
    return true;
  }
  if (master.is_ghost) {
    out->master_is_ghost = true;
    return true;
  }
  if (!SegmentMetaMatches(out->decoded.header, master.header)) {
    out->master_meta_mismatch = true;
    return true;
  }
  out->decoded.points = std::move(master.points);
  return true;
}

}  // namespace segval

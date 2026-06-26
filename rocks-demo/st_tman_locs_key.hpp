// TMan-tsp ST RowKey spatial from LocSIndex (precomputed CSV from EmitSegmentStKeys).

#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>

#include <rocksdb/status.h>

#include "st_tman_st_key.hpp"

namespace st_tman {

struct TmanLocsKeyFields {
  int16_t period = 0;
  uint64_t spatial_index = 0;
  std::string tid;
};

class TmanLocsKeyTable {
 public:
  rocksdb::Status LoadCsv(const std::string& path) {
    by_segment_.clear();
    std::ifstream in(path);
    if (!in) {
      return rocksdb::Status::IOError("Cannot open tman ST keys CSV: " + path);
    }
    std::string line;
    if (!std::getline(in, line)) {
      return rocksdb::Status::Corruption("empty tman ST keys CSV");
    }
    size_t loaded = 0;
    while (std::getline(in, line)) {
      if (line.empty()) {
        continue;
      }
      const size_t c1 = line.find(',');
      const size_t c2 = line.find(',', c1 + 1);
      const size_t c3 = line.find(',', c2 + 1);
      const size_t c4 = line.find(',', c3 + 1);
      if (c1 == std::string::npos || c2 == std::string::npos ||
          c3 == std::string::npos) {
        continue;
      }
      const std::string segment_id = line.substr(0, c1);
      TmanLocsKeyFields f;
      try {
        f.period = static_cast<int16_t>(std::stoi(line.substr(c1 + 1, c2 - c1 - 1)));
        f.spatial_index = static_cast<uint64_t>(
            std::stoull(line.substr(c2 + 1, c3 - c2 - 1)));
        if (c4 != std::string::npos) {
          f.tid = line.substr(c3 + 1, c4 - c3 - 1);
        } else {
          f.tid = line.substr(c3 + 1);
        }
      } catch (...) {
        return rocksdb::Status::Corruption("bad tman ST keys CSV row: " + line);
      }
      by_segment_[segment_id] = std::move(f);
      ++loaded;
    }
    if (loaded == 0) {
      return rocksdb::Status::Corruption("no rows in tman ST keys CSV");
    }
    return rocksdb::Status::OK();
  }

  size_t size() const { return by_segment_.size(); }

  bool BuildRowKey(const std::string& segment_id, std::string* out_key) const {
    if (out_key == nullptr) {
      return false;
    }
    const auto it = by_segment_.find(segment_id);
    if (it == by_segment_.end()) {
      return false;
    }
    *out_key = BuildTmanStRowKey(it->second.period, it->second.spatial_index,
                                 it->second.tid);
    return !out_key->empty();
  }

 private:
  std::unordered_map<std::string, TmanLocsKeyFields> by_segment_;
};

}  // namespace st_tman

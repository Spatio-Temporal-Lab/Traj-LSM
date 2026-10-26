// Bounded-RSS loader for segments_points.csv (multi-GB).
// Pass 1: scan CSV once, record byte offsets of each contiguous point run per segment_id.
// Pass 2 (ingest): Get() reads points directly from the original CSV (no duplicate .spill.bin).

#pragma once

#include <algorithm>
#include <filesystem>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "segment_value_codec.hpp"
#include "trajectory_kv.hpp"

namespace segval {

class SegmentPointsStore {
 public:
  bool BuildFromCsv(const std::string& csv_path);
  bool Get(const std::string& segment_id, std::vector<SegmentPoint>* out) const;
  bool Get(const std::string& segment_id, std::vector<SegmentPoint>* out,
           std::ifstream* input) const;

 private:
  struct CsvPointRun {
    uint64_t byte_offset = 0;
    uint32_t num_points = 0;
  };

  static constexpr char kIndexMagic[6] = {'S', 'T', 'P', 'X', '2', '\0'};

  std::string csv_path_;
  std::string index_path_;
  std::unordered_map<std::string, std::vector<CsvPointRun>> runs_;

  static bool ParsePointLine(const std::string& line, std::string* segment_id,
                             int* point_index, SegmentPoint* point);
  static void MergeSortedPointPairs(
      std::vector<std::pair<int, SegmentPoint>>* dst,
      const std::vector<std::pair<int, SegmentPoint>>& src);
  bool ReadRunFromCsv(const CsvPointRun& run, const std::string& expect_segment_id,
                      std::vector<std::pair<int, SegmentPoint>>* pairs,
                      std::ifstream* input) const;
  bool LoadIndexFile(uint64_t csv_size, int64_t csv_mtime);
  bool WriteIndexFile() const;
};

inline bool SegmentPointsStore::ParsePointLine(const std::string& line,
                                               std::string* segment_id,
                                               int* point_index,
                                               SegmentPoint* point) {
  if (segment_id == nullptr || point_index == nullptr || point == nullptr ||
      line.empty()) {
    return false;
  }
  auto f = trajkv::SplitCsvLine(line);
  if (f.size() < 6) {
    return false;
  }
  *segment_id = f[0];
  *point_index = std::stoi(f[2]);
  point->unix_s = static_cast<uint32_t>(std::stoul(f[3]));
  point->lon = static_cast<float>(std::stod(f[4]));
  point->lat = static_cast<float>(std::stod(f[5]));
  return true;
}

inline void SegmentPointsStore::MergeSortedPointPairs(
    std::vector<std::pair<int, SegmentPoint>>* dst,
    const std::vector<std::pair<int, SegmentPoint>>& src) {
  if (dst == nullptr || src.empty()) {
    return;
  }
  std::vector<std::pair<int, SegmentPoint>> merged;
  merged.reserve(dst->size() + src.size());
  size_t i = 0;
  size_t j = 0;
  while (i < dst->size() && j < src.size()) {
    if ((*dst)[i].first < src[j].first) {
      merged.push_back(std::move((*dst)[i++]));
    } else if (src[j].first < (*dst)[i].first) {
      merged.push_back(src[j++]);
    } else {
      merged.push_back(src[j++]);
      ++i;
    }
  }
  while (i < dst->size()) {
    merged.push_back(std::move((*dst)[i++]));
  }
  while (j < src.size()) {
    merged.push_back(src[j++]);
  }
  *dst = std::move(merged);
}

inline bool SegmentPointsStore::ReadRunFromCsv(
    const CsvPointRun& run, const std::string& expect_segment_id,
    std::vector<std::pair<int, SegmentPoint>>* pairs,
    std::ifstream* input) const {
  if (pairs == nullptr || input == nullptr || run.num_points == 0) {
    return false;
  }
  input->clear();
  input->seekg(static_cast<std::streamoff>(run.byte_offset));
  if (!*input) {
    return false;
  }
  pairs->reserve(pairs->size() + run.num_points);
  std::string line;
  for (uint32_t n = 0; n < run.num_points; ++n) {
    if (!std::getline(*input, line)) {
      return false;
    }
    std::string segment_id;
    int point_index = 0;
    SegmentPoint p{};
    if (!ParsePointLine(line, &segment_id, &point_index, &p)) {
      return false;
    }
    if (segment_id != expect_segment_id) {
      std::cerr << "CSV run corrupt at offset=" << run.byte_offset
                << " expected segment_id=" << expect_segment_id
                << " got=" << segment_id << "\n";
      return false;
    }
    pairs->emplace_back(point_index, p);
  }
  return true;
}

inline bool SegmentPointsStore::LoadIndexFile(uint64_t csv_size,
                                              int64_t csv_mtime) {
  std::ifstream idx(index_path_, std::ios::binary);
  if (!idx) return false;
  char magic[sizeof(kIndexMagic)]{};
  uint64_t stored_size = 0;
  int64_t stored_mtime = 0;
  uint64_t nseg = 0;
  idx.read(magic, sizeof(magic));
  idx.read(reinterpret_cast<char*>(&stored_size), sizeof(stored_size));
  idx.read(reinterpret_cast<char*>(&stored_mtime), sizeof(stored_mtime));
  idx.read(reinterpret_cast<char*>(&nseg), sizeof(nseg));
  if (!idx || !std::equal(std::begin(kIndexMagic), std::end(kIndexMagic), magic) ||
      stored_size != csv_size || stored_mtime != csv_mtime ||
      nseg > 100000000 || nseg > csv_size / 6) {
    return false;
  }
  std::unordered_map<std::string, std::vector<CsvPointRun>> loaded;
  loaded.reserve(static_cast<size_t>(nseg));
  for (uint64_t i = 0; i < nseg; ++i) {
    uint32_t id_len = 0;
    uint32_t nruns = 0;
    idx.read(reinterpret_cast<char*>(&id_len), sizeof(id_len));
    if (!idx || id_len == 0 || id_len > 4096) return false;
    std::string id(id_len, '\0');
    idx.read(id.data(), static_cast<std::streamsize>(id_len));
    idx.read(reinterpret_cast<char*>(&nruns), sizeof(nruns));
    if (!idx || nruns == 0 || nruns > 1000000) return false;
    auto& runs = loaded[id];
    runs.resize(nruns);
    for (auto& run : runs) {
      idx.read(reinterpret_cast<char*>(&run.byte_offset), sizeof(run.byte_offset));
      idx.read(reinterpret_cast<char*>(&run.num_points), sizeof(run.num_points));
      if (!idx || run.num_points == 0 || run.byte_offset >= csv_size) return false;
    }
  }
  if (idx.peek() != std::char_traits<char>::eof()) return false;
  runs_ = std::move(loaded);
  std::cerr << "  points CSV index cache hit segments=" << runs_.size()
            << " index=" << index_path_ << "\n";
  return true;
}

inline bool SegmentPointsStore::WriteIndexFile() const {
  std::ofstream idx(index_path_, std::ios::binary | std::ios::trunc);
  if (!idx) {
    return false;
  }
  idx.write(kIndexMagic, sizeof(kIndexMagic));
  const uint64_t csv_size = std::filesystem::file_size(csv_path_);
  const int64_t csv_mtime = static_cast<int64_t>(
      std::filesystem::last_write_time(csv_path_).time_since_epoch().count());
  idx.write(reinterpret_cast<const char*>(&csv_size), sizeof(csv_size));
  idx.write(reinterpret_cast<const char*>(&csv_mtime), sizeof(csv_mtime));
  const uint64_t nseg = static_cast<uint64_t>(runs_.size());
  idx.write(reinterpret_cast<const char*>(&nseg), sizeof(nseg));
  for (const auto& kv : runs_) {
    const uint32_t id_len = static_cast<uint32_t>(kv.first.size());
    const uint32_t nruns = static_cast<uint32_t>(kv.second.size());
    idx.write(reinterpret_cast<const char*>(&id_len), sizeof(id_len));
    idx.write(kv.first.data(), static_cast<std::streamsize>(id_len));
    idx.write(reinterpret_cast<const char*>(&nruns), sizeof(nruns));
    for (const CsvPointRun& run : kv.second) {
      idx.write(reinterpret_cast<const char*>(&run.byte_offset),
                sizeof(run.byte_offset));
      idx.write(reinterpret_cast<const char*>(&run.num_points),
                sizeof(run.num_points));
    }
  }
  return idx.good();
}

inline bool SegmentPointsStore::BuildFromCsv(const std::string& csv_path) {
  runs_.clear();
  csv_path_ = csv_path;
  index_path_ = csv_path + ".spill.idx";
  const uint64_t csv_size = std::filesystem::file_size(csv_path_);
  const int64_t csv_mtime = static_cast<int64_t>(
      std::filesystem::last_write_time(csv_path_).time_since_epoch().count());
  if (LoadIndexFile(csv_size, csv_mtime)) return true;

  std::ifstream in(csv_path_, std::ios::binary);
  if (!in) {
    std::cerr << "Cannot open segment points CSV: " << csv_path_ << "\n";
    return false;
  }
  std::string header;
  if (!std::getline(in, header)) {
    std::cerr << "Empty segment points CSV.\n";
    return false;
  }

  std::string line;
  std::string cur_segment_id;
  uint64_t cur_run_byte_off = 0;
  uint32_t cur_run_points = 0;
  uint64_t lines = 0;
  uint64_t multi_run_segments = 0;

  auto seal_run = [&]() {
    if (cur_segment_id.empty() || cur_run_points == 0) {
      return;
    }
    auto& runs = runs_[cur_segment_id];
    if (!runs.empty()) {
      ++multi_run_segments;
    }
    runs.push_back(CsvPointRun{cur_run_byte_off, cur_run_points});
    cur_run_points = 0;
  };

  while (true) {
    const uint64_t line_start = static_cast<uint64_t>(in.tellg());
    if (!std::getline(in, line)) {
      break;
    }
    if (line.empty()) {
      continue;
    }
    std::string segment_id;
    int point_index = 0;
    SegmentPoint p{};
    if (!ParsePointLine(line, &segment_id, &point_index, &p)) {
      std::cerr << "Bad row in segment points CSV at offset=" << line_start << "\n";
      return false;
    }
    (void)point_index;
    (void)p;

    if (segment_id != cur_segment_id) {
      seal_run();
      cur_segment_id = std::move(segment_id);
      cur_run_byte_off = line_start;
      cur_run_points = 1;
    } else {
      ++cur_run_points;
    }
    if (++lines % 5000000 == 0) {
      std::cerr << "  points index lines=" << lines << " segments=" << runs_.size()
                << "\n";
    }
  }
  seal_run();
  in.close();

  if (!WriteIndexFile()) {
    return false;
  }
  std::cerr << "  points CSV index done lines=" << lines << " segments=" << runs_.size()
            << " multi_run_segments=" << multi_run_segments
            << " index=" << index_path_ << " (reads original CSV)\n";
  return true;
}

inline bool SegmentPointsStore::Get(const std::string& segment_id,
                                    std::vector<SegmentPoint>* out) const {
  std::ifstream input(csv_path_, std::ios::binary);
  if (!input) return false;
  return Get(segment_id, out, &input);
}

inline bool SegmentPointsStore::Get(const std::string& segment_id,
                                    std::vector<SegmentPoint>* out,
                                    std::ifstream* input) const {
  if (out == nullptr || input == nullptr) {
    return false;
  }
  out->clear();
  const auto it = runs_.find(segment_id);
  if (it == runs_.end()) {
    return false;
  }
  std::vector<std::pair<int, SegmentPoint>> pairs;
  for (const CsvPointRun& run : it->second) {
    std::vector<std::pair<int, SegmentPoint>> chunk;
    if (!ReadRunFromCsv(run, segment_id, &chunk, input)) {
      return false;
    }
    MergeSortedPointPairs(&pairs, chunk);
  }
  out->reserve(pairs.size());
  for (const auto& pr : pairs) {
    out->push_back(pr.second);
  }
  return true;
}

}  // namespace segval

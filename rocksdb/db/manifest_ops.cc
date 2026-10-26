//  Copyright (c) Meta Platforms, Inc. and affiliates.
//
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "db/manifest_ops.h"

#include <cstdlib>
#include <cmath>
#include <limits>
#include <map>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "db/log_reader.h"
#include "db/version_edit_handler.h"
#include "file/sequence_file_reader.h"
#include "file/filename.h"

namespace ROCKSDB_NAMESPACE {
namespace {

const char* kManifestSetPrefix = "MANIFEST_SET-";
const char* kMManagerPrefix = "M_manager-";

// A tiny LRU cache for small text files (M_manager / MANIFEST_SET and MANIFEST*).
// This is intentionally simple: fixed capacity, whole-file caching, best-effort.
class SmallFileLRUCache {
 public:
  explicit SmallFileLRUCache(size_t capacity) : capacity_(capacity) {}

  Status Read(FileSystem* fs, const std::string& path, const IOOptions& opts,
              std::string* out) {
    if (out == nullptr) {
      return Status::InvalidArgument("out is null");
    }
    if (capacity_ == 0) {
      return ReadFileToString(fs, path, opts, out);
    }
    {
      std::lock_guard<std::mutex> lg(mu_);
      auto it = map_.find(path);
      if (it != map_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second.it);
        *out = it->second.content;
        return Status::OK();
      }
    }

    std::string content;
    Status s = ReadFileToString(fs, path, opts, &content);
    if (!s.ok()) {
      return s;
    }

    {
      std::lock_guard<std::mutex> lg(mu_);
      auto it = map_.find(path);
      if (it != map_.end()) {
        // Another thread populated it while we were reading.
        lru_.splice(lru_.begin(), lru_, it->second.it);
        *out = it->second.content;
        return Status::OK();
      }
      lru_.push_front(path);
      map_[path] = Entry{lru_.begin(), std::move(content)};
      if (map_.size() > capacity_) {
        const std::string& victim = lru_.back();
        map_.erase(victim);
        lru_.pop_back();
      }
      *out = map_[path].content;
    }
    return Status::OK();
  }

 private:
  struct Entry {
    std::list<std::string>::iterator it;
    std::string content;
  };

  const size_t capacity_;
  std::mutex mu_;
  std::list<std::string> lru_;
  std::unordered_map<std::string, Entry> map_;
};

[[maybe_unused]] SmallFileLRUCache& ManifestSmallFileCache() {
  // Default capacity: keep a small batch of recent manifest-related small files.
  // This matches the "keep a small batch in memory" goal without over-allocating.
  static SmallFileLRUCache cache(/*capacity=*/16);
  return cache;
}

SmallFileLRUCache& ManifestShardFileCache() {
  // In-memory LRU only (default 8). Eviction does not delete on-disk shards.
  static SmallFileLRUCache cache(/*capacity=*/8);
  return cache;
}

std::string MakeDbRelativePath(const std::string& dbname,
                               const std::string& relative_name);

Status ParseManifestSetFile(const std::string& dbname, FileSystem* fs,
                            bool is_retry, const std::string& manifest_set_name,
                            std::vector<std::string>* manifest_paths,
                            std::vector<uint64_t>* manifest_file_numbers,
                            uint64_t* manifest_set_generation,
                            std::vector<uint64_t>* manifest_time_mins,
                            std::vector<uint64_t>* manifest_time_maxs,
                            std::vector<float>* manifest_mbr_x_mins,
                            std::vector<float>* manifest_mbr_x_maxs,
                            std::vector<float>* manifest_mbr_y_mins,
                            std::vector<float>* manifest_mbr_y_maxs,
                            std::vector<uint64_t>* manifest_master_counts,
                            std::vector<uint64_t>* manifest_ghost_counts,
                            std::vector<uint64_t>* manifest_host_buckets);

Status ParseManifestSetContent(
    const std::string& dbname, const std::string& content,
    std::vector<std::string>* manifest_paths,
    std::vector<uint64_t>* manifest_file_numbers,
    uint64_t* manifest_set_generation,
    std::vector<uint64_t>* manifest_time_mins,
    std::vector<uint64_t>* manifest_time_maxs,
    std::vector<float>* manifest_mbr_x_mins,
    std::vector<float>* manifest_mbr_x_maxs,
    std::vector<float>* manifest_mbr_y_mins,
    std::vector<float>* manifest_mbr_y_maxs,
    std::vector<uint64_t>* manifest_master_counts,
    std::vector<uint64_t>* manifest_ghost_counts,
    std::vector<uint64_t>* manifest_host_buckets) {
  if (manifest_paths == nullptr || manifest_file_numbers == nullptr) {
    return Status::InvalidArgument("null input");
  }
  if (content.empty() || content.back() != '\n') {
    return Status::Corruption("MANIFEST_SET file does not end with newline");
  }
  if (manifest_set_generation != nullptr) {
    *manifest_set_generation = 0;
  }
  manifest_paths->clear();
  manifest_file_numbers->clear();
  if (manifest_time_mins != nullptr) {
    manifest_time_mins->clear();
  }
  if (manifest_time_maxs != nullptr) {
    manifest_time_maxs->clear();
  }
  if (manifest_mbr_x_mins != nullptr) {
    manifest_mbr_x_mins->clear();
  }
  if (manifest_mbr_x_maxs != nullptr) {
    manifest_mbr_x_maxs->clear();
  }
  if (manifest_mbr_y_mins != nullptr) {
    manifest_mbr_y_mins->clear();
  }
  if (manifest_mbr_y_maxs != nullptr) {
    manifest_mbr_y_maxs->clear();
  }
  if (manifest_master_counts != nullptr) {
    manifest_master_counts->clear();
  }
  if (manifest_ghost_counts != nullptr) {
    manifest_ghost_counts->clear();
  }
  if (manifest_host_buckets != nullptr) {
    manifest_host_buckets->clear();
  }

  size_t line_start = 0;
  constexpr uint64_t kTimeUnknownMin = 0;
  constexpr uint64_t kTimeUnknownMax = std::numeric_limits<uint64_t>::max();
  constexpr uint64_t kHostBucketUnknown = std::numeric_limits<uint64_t>::max();
  while (line_start < content.size()) {
    const size_t line_end = content.find('\n', line_start);
    if (line_end == std::string::npos) {
      break;
    }
    std::string line = content.substr(line_start, line_end - line_start);
    line_start = line_end + 1;
    if (line.empty() || line[0] == '#') {
      continue;
    }
    if (line.rfind("version=", 0) == 0 || line.rfind("format=", 0) == 0) {
      continue;
    }
    if (line.rfind("generation=", 0) == 0) {
      if (manifest_set_generation != nullptr) {
        const std::string generation_str =
            line.substr(std::string("generation=").size());
        char* end = nullptr;
        const uint64_t parsed =
            std::strtoull(generation_str.c_str(), &end, 10);
        if (generation_str.empty() || end == generation_str.c_str() ||
            *end != '\0') {
          return Status::Corruption("MANIFEST_SET generation parse failed");
        }
        *manifest_set_generation = parsed;
      }
      continue;
    }
    uint64_t tmin = kTimeUnknownMin;
    uint64_t tmax = kTimeUnknownMax;
    uint64_t host_bucket = kHostBucketUnknown;
    const float kMbrUnknown = std::numeric_limits<float>::quiet_NaN();
    float xmin = kMbrUnknown;
    float xmax = kMbrUnknown;
    float ymin = kMbrUnknown;
    float ymax = kMbrUnknown;
    uint64_t masters = 0;
    uint64_t ghosts = 0;
    if (line.rfind("entry=", 0) == 0) {
      std::string body = line.substr(std::string("entry=").size());
      std::string manifest_name;
      size_t comma = body.find(',');
      if (comma == std::string::npos) {
        manifest_name = body;
      } else {
        manifest_name = body.substr(0, comma);
        body = body.substr(comma + 1);
        while (!body.empty()) {
          size_t next = body.find(',');
          std::string kv = next == std::string::npos ? body : body.substr(0, next);
          if (next == std::string::npos) {
            body.clear();
          } else {
            body = body.substr(next + 1);
          }
          while (!kv.empty() && kv.front() == ' ') kv.erase(kv.begin());
          while (!kv.empty() && kv.back() == ' ') kv.pop_back();
          if (kv.rfind("bucket=", 0) == 0) {
            char* end = nullptr;
            const std::string v = kv.substr(7);
            const uint64_t parsed = std::strtoull(v.c_str(), &end, 10);
            if (!v.empty() && end != v.c_str() && *end == '\0') {
              host_bucket = parsed;
            }
          } else if (kv.rfind("tmin=", 0) == 0) {
            char* end = nullptr;
            const std::string v = kv.substr(5);
            const uint64_t parsed = std::strtoull(v.c_str(), &end, 10);
            if (!v.empty() && end != v.c_str() && *end == '\0') {
              tmin = parsed;
            }
          } else if (kv.rfind("tmax=", 0) == 0) {
            char* end = nullptr;
            const std::string v = kv.substr(5);
            const uint64_t parsed = std::strtoull(v.c_str(), &end, 10);
            if (!v.empty() && end != v.c_str() && *end == '\0') {
              tmax = parsed;
            }
          } else if (kv.rfind("xmin=", 0) == 0) {
            char* end = nullptr;
            const std::string v = kv.substr(5);
            const float parsed = std::strtof(v.c_str(), &end);
            if (!v.empty() && end != v.c_str() && *end == '\0') {
              xmin = parsed;
            }
          } else if (kv.rfind("xmax=", 0) == 0) {
            char* end = nullptr;
            const std::string v = kv.substr(5);
            const float parsed = std::strtof(v.c_str(), &end);
            if (!v.empty() && end != v.c_str() && *end == '\0') {
              xmax = parsed;
            }
          } else if (kv.rfind("ymin=", 0) == 0) {
            char* end = nullptr;
            const std::string v = kv.substr(5);
            const float parsed = std::strtof(v.c_str(), &end);
            if (!v.empty() && end != v.c_str() && *end == '\0') {
              ymin = parsed;
            }
          } else if (kv.rfind("ymax=", 0) == 0) {
            char* end = nullptr;
            const std::string v = kv.substr(5);
            const float parsed = std::strtof(v.c_str(), &end);
            if (!v.empty() && end != v.c_str() && *end == '\0') {
              ymax = parsed;
            }
          } else if (kv.rfind("masters=", 0) == 0) {
            char* end = nullptr;
            const std::string v = kv.substr(8);
            const uint64_t parsed = std::strtoull(v.c_str(), &end, 10);
            if (!v.empty() && end != v.c_str() && *end == '\0') {
              masters = parsed;
            }
          } else if (kv.rfind("ghosts=", 0) == 0) {
            char* end = nullptr;
            const std::string v = kv.substr(7);
            const uint64_t parsed = std::strtoull(v.c_str(), &end, 10);
            if (!v.empty() && end != v.c_str() && *end == '\0') {
              ghosts = parsed;
            }
          }
        }
      }
      line = manifest_name;
    }
    uint64_t num = 0;
    FileType type;
    const bool parse_ok = ParseFileName(line, &num, &type);
    if (!parse_ok || type != kDescriptorFile) {
      return Status::Corruption("MANIFEST_SET file corrupted");
    }
    manifest_file_numbers->push_back(num);
    manifest_paths->push_back(MakeDbRelativePath(dbname, line));
    if (manifest_time_mins != nullptr) {
      manifest_time_mins->push_back(tmin);
    }
    if (manifest_time_maxs != nullptr) {
      manifest_time_maxs->push_back(tmax);
    }
    if (manifest_mbr_x_mins != nullptr) {
      manifest_mbr_x_mins->push_back(xmin);
    }
    if (manifest_mbr_x_maxs != nullptr) {
      manifest_mbr_x_maxs->push_back(xmax);
    }
    if (manifest_mbr_y_mins != nullptr) {
      manifest_mbr_y_mins->push_back(ymin);
    }
    if (manifest_mbr_y_maxs != nullptr) {
      manifest_mbr_y_maxs->push_back(ymax);
    }
    if (manifest_master_counts != nullptr) {
      manifest_master_counts->push_back(masters);
    }
    if (manifest_ghost_counts != nullptr) {
      manifest_ghost_counts->push_back(ghosts);
    }
    if (manifest_host_buckets != nullptr) {
      manifest_host_buckets->push_back(host_bucket);
    }
  }
  if (manifest_paths->empty()) {
    return Status::Corruption("MANIFEST_SET file has no MANIFEST entries");
  }
  return Status::OK();
}

std::string MakeDbRelativePath(const std::string& dbname,
                               const std::string& relative_name) {
  std::string out = dbname;
  if (!out.empty() && out.back() != '/' && out.back() != kFilePathSeparator) {
    out.push_back(kFilePathSeparator);
  }
  out.append(relative_name);
  return out;
}

Status ParseMManagerFile(const std::string& dbname, FileSystem* fs, bool is_retry,
                       const std::string& manager_name,
                       std::vector<std::string>* manifest_paths,
                       std::vector<uint64_t>* manifest_file_numbers,
                       uint64_t* manifest_set_generation,
                       std::vector<uint64_t>* manifest_time_mins,
                       std::vector<uint64_t>* manifest_time_maxs,
                       std::vector<float>* manifest_mbr_x_mins,
                       std::vector<float>* manifest_mbr_x_maxs,
                       std::vector<float>* manifest_mbr_y_mins,
                       std::vector<float>* manifest_mbr_y_maxs,
                       std::vector<uint64_t>* manifest_master_counts,
                       std::vector<uint64_t>* manifest_ghost_counts,
                       std::vector<uint64_t>* manifest_host_buckets) {
  return ParseManifestSetFile(dbname, fs, is_retry, manager_name, manifest_paths,
                            manifest_file_numbers, manifest_set_generation,
                            manifest_time_mins, manifest_time_maxs,
                            manifest_mbr_x_mins, manifest_mbr_x_maxs,
                            manifest_mbr_y_mins, manifest_mbr_y_maxs,
                            manifest_master_counts, manifest_ghost_counts,
                            manifest_host_buckets);
}

Status ParseManifestSetFile(const std::string& dbname, FileSystem* fs,
                            bool is_retry,
                            const std::string& manifest_set_name,
                            std::vector<std::string>* manifest_paths,
                            std::vector<uint64_t>* manifest_file_numbers,
                            uint64_t* manifest_set_generation,
                            std::vector<uint64_t>* manifest_time_mins,
                            std::vector<uint64_t>* manifest_time_maxs,
                            std::vector<float>* manifest_mbr_x_mins,
                            std::vector<float>* manifest_mbr_x_maxs,
                            std::vector<float>* manifest_mbr_y_mins,
                            std::vector<float>* manifest_mbr_y_maxs,
                            std::vector<uint64_t>* manifest_master_counts,
                            std::vector<uint64_t>* manifest_ghost_counts,
                            std::vector<uint64_t>* manifest_host_buckets) {
  if (fs == nullptr || manifest_paths == nullptr ||
      manifest_file_numbers == nullptr) {
    return Status::InvalidArgument("null input");
  }
  IOOptions opts;
  if (is_retry) {
    opts.verify_and_reconstruct_read = true;
  }
  const std::string set_path = MakeDbRelativePath(dbname, manifest_set_name);
  std::string content;
  // MANIFEST_SET can be rewritten during reopen/recovery, so always re-read it
  // from disk instead of serving stale process-local cache contents.
  Status s = ReadFileToString(fs, set_path, opts, &content);
  if (!s.ok()) {
    return s;
  }
  s = ParseManifestSetContent(
      dbname, content, manifest_paths, manifest_file_numbers,
      manifest_set_generation, manifest_time_mins, manifest_time_maxs,
      manifest_mbr_x_mins, manifest_mbr_x_maxs, manifest_mbr_y_mins,
      manifest_mbr_y_maxs, manifest_master_counts, manifest_ghost_counts,
      manifest_host_buckets);
  if (!s.ok()) {
    return s;
  }

  // Query and operational robustness: it is possible that a MANIFEST_SET
  // references historical MANIFEST shards that are no longer present on disk
  // (e.g., a background manifest rewrite/rotation purged old shards).
  //
  // Dropping missing *historical* entries allows read-only queries/tools to
  // proceed without requiring out-of-band "repair" scripts. If the newest
  // (active) shard is missing, we must still fail.
  IOOptions exists_opts;
  std::vector<std::string> filtered_paths;
  std::vector<uint64_t> filtered_numbers;
  std::vector<uint64_t> filtered_tmins;
  std::vector<uint64_t> filtered_tmaxs;
  std::vector<float> filtered_xmins;
  std::vector<float> filtered_xmaxs;
  std::vector<float> filtered_ymins;
  std::vector<float> filtered_ymaxs;
  std::vector<uint64_t> filtered_masters;
  std::vector<uint64_t> filtered_ghosts;
  std::vector<uint64_t> filtered_host_buckets;
  filtered_paths.reserve(manifest_paths->size());
  filtered_numbers.reserve(manifest_file_numbers->size());
  if (manifest_time_mins != nullptr) {
    filtered_tmins.reserve(manifest_time_mins->size());
  }
  if (manifest_time_maxs != nullptr) {
    filtered_tmaxs.reserve(manifest_time_maxs->size());
  }
  if (manifest_mbr_x_mins != nullptr) {
    filtered_xmins.reserve(manifest_mbr_x_mins->size());
  }
  if (manifest_mbr_x_maxs != nullptr) {
    filtered_xmaxs.reserve(manifest_mbr_x_maxs->size());
  }
  if (manifest_mbr_y_mins != nullptr) {
    filtered_ymins.reserve(manifest_mbr_y_mins->size());
  }
  if (manifest_mbr_y_maxs != nullptr) {
    filtered_ymaxs.reserve(manifest_mbr_y_maxs->size());
  }
  if (manifest_master_counts != nullptr) {
    filtered_masters.reserve(manifest_master_counts->size());
  }
  if (manifest_ghost_counts != nullptr) {
    filtered_ghosts.reserve(manifest_ghost_counts->size());
  }
  if (manifest_host_buckets != nullptr) {
    filtered_host_buckets.reserve(manifest_host_buckets->size());
  }
  const size_t n = manifest_paths->size();
  for (size_t i = 0; i < n; ++i) {
    const std::string& p = (*manifest_paths)[i];
    const IOStatus es = fs->FileExists(p, exists_opts, nullptr);
    if (!es.ok()) {
      continue;
    }
    filtered_paths.push_back(p);
    filtered_numbers.push_back((*manifest_file_numbers)[i]);
    if (manifest_time_mins != nullptr) {
      filtered_tmins.push_back((*manifest_time_mins)[i]);
    }
    if (manifest_time_maxs != nullptr) {
      filtered_tmaxs.push_back((*manifest_time_maxs)[i]);
    }
    if (manifest_mbr_x_mins != nullptr) {
      filtered_xmins.push_back((*manifest_mbr_x_mins)[i]);
    }
    if (manifest_mbr_x_maxs != nullptr) {
      filtered_xmaxs.push_back((*manifest_mbr_x_maxs)[i]);
    }
    if (manifest_mbr_y_mins != nullptr) {
      filtered_ymins.push_back((*manifest_mbr_y_mins)[i]);
    }
    if (manifest_mbr_y_maxs != nullptr) {
      filtered_ymaxs.push_back((*manifest_mbr_y_maxs)[i]);
    }
    if (manifest_master_counts != nullptr) {
      filtered_masters.push_back((*manifest_master_counts)[i]);
    }
    if (manifest_ghost_counts != nullptr) {
      filtered_ghosts.push_back((*manifest_ghost_counts)[i]);
    }
    if (manifest_host_buckets != nullptr) {
      filtered_host_buckets.push_back((*manifest_host_buckets)[i]);
    }
  }
  if (filtered_paths.empty()) {
    return Status::NotFound(
        "all MANIFEST shards referenced by MANIFEST_SET are missing");
  }
  if (filtered_paths.size() != manifest_paths->size()) {
    *manifest_paths = std::move(filtered_paths);
    *manifest_file_numbers = std::move(filtered_numbers);
    if (manifest_time_mins != nullptr) {
      *manifest_time_mins = std::move(filtered_tmins);
    }
    if (manifest_time_maxs != nullptr) {
      *manifest_time_maxs = std::move(filtered_tmaxs);
    }
    if (manifest_mbr_x_mins != nullptr) {
      *manifest_mbr_x_mins = std::move(filtered_xmins);
    }
    if (manifest_mbr_x_maxs != nullptr) {
      *manifest_mbr_x_maxs = std::move(filtered_xmaxs);
    }
    if (manifest_mbr_y_mins != nullptr) {
      *manifest_mbr_y_mins = std::move(filtered_ymins);
    }
    if (manifest_mbr_y_maxs != nullptr) {
      *manifest_mbr_y_maxs = std::move(filtered_ymaxs);
    }
    if (manifest_master_counts != nullptr) {
      *manifest_master_counts = std::move(filtered_masters);
    }
    if (manifest_ghost_counts != nullptr) {
      *manifest_ghost_counts = std::move(filtered_ghosts);
    }
    if (manifest_host_buckets != nullptr) {
      *manifest_host_buckets = std::move(filtered_host_buckets);
    }
  }
  return Status::OK();
}

}  // namespace

Status GetCurrentManifestPath(const std::string& dbname, FileSystem* fs,
                              bool is_retry, std::string* manifest_path,
                              uint64_t* manifest_file_number) {
  std::vector<std::string> paths;
  std::vector<uint64_t> numbers;
  Status s = GetCurrentManifestPaths(dbname, fs, is_retry, &paths, &numbers);
  if (!s.ok()) {
    return s;
  }
  assert(!paths.empty());
  assert(!numbers.empty());
  // For backward compatibility with callers expecting a single "current"
  // MANIFEST, return the last replay entry.
  *manifest_path = paths.back();
  *manifest_file_number = numbers.back();
  return Status::OK();
}

Status GetCurrentManifestPaths(const std::string& dbname, FileSystem* fs,
                               bool is_retry,
                               std::vector<std::string>* manifest_paths,
                               std::vector<uint64_t>* manifest_file_numbers,
                               uint64_t* manifest_set_generation,
                               std::vector<uint64_t>* manifest_time_mins,
                               std::vector<uint64_t>* manifest_time_maxs,
                               std::vector<float>* manifest_mbr_x_mins,
                               std::vector<float>* manifest_mbr_x_maxs,
                               std::vector<float>* manifest_mbr_y_mins,
                               std::vector<float>* manifest_mbr_y_maxs,
                               std::vector<uint64_t>* manifest_master_counts,
                               std::vector<uint64_t>* manifest_ghost_counts,
                               std::vector<uint64_t>* manifest_host_buckets) {
  assert(fs != nullptr);
  assert(manifest_paths != nullptr);
  assert(manifest_file_numbers != nullptr);
  manifest_paths->clear();
  manifest_file_numbers->clear();
  if (manifest_set_generation != nullptr) {
    *manifest_set_generation = 0;
  }
  if (manifest_time_mins != nullptr) {
    manifest_time_mins->clear();
  }
  if (manifest_time_maxs != nullptr) {
    manifest_time_maxs->clear();
  }
  if (manifest_mbr_x_mins != nullptr) {
    manifest_mbr_x_mins->clear();
  }
  if (manifest_mbr_x_maxs != nullptr) {
    manifest_mbr_x_maxs->clear();
  }
  if (manifest_mbr_y_mins != nullptr) {
    manifest_mbr_y_mins->clear();
  }
  if (manifest_mbr_y_maxs != nullptr) {
    manifest_mbr_y_maxs->clear();
  }
  if (manifest_master_counts != nullptr) {
    manifest_master_counts->clear();
  }
  if (manifest_ghost_counts != nullptr) {
    manifest_ghost_counts->clear();
  }
  if (manifest_host_buckets != nullptr) {
    manifest_host_buckets->clear();
  }

  IOOptions opts;
  std::string fname;
  if (is_retry) {
    opts.verify_and_reconstruct_read = true;
  }
  Status s = ReadFileToString(fs, CurrentFileName(dbname), opts, &fname);
  if (!s.ok()) {
    return s;
  }
  if (fname.empty() || fname.back() != '\n') {
    return Status::Corruption("CURRENT file does not end with newline");
  }
  fname.resize(fname.size() - 1);

  if (fname.rfind(kMManagerPrefix, 0) == 0) {
    return ParseMManagerFile(dbname, fs, is_retry, fname, manifest_paths,
                           manifest_file_numbers, manifest_set_generation,
                           manifest_time_mins, manifest_time_maxs,
                           manifest_mbr_x_mins, manifest_mbr_x_maxs,
                           manifest_mbr_y_mins, manifest_mbr_y_maxs,
                           manifest_master_counts, manifest_ghost_counts,
                           manifest_host_buckets);
  }
  if (fname.rfind(kManifestSetPrefix, 0) == 0) {
    return ParseManifestSetFile(dbname, fs, is_retry, fname, manifest_paths,
                                manifest_file_numbers, manifest_set_generation,
                                manifest_time_mins, manifest_time_maxs,
                                manifest_mbr_x_mins, manifest_mbr_x_maxs,
                                manifest_mbr_y_mins, manifest_mbr_y_maxs,
                                manifest_master_counts, manifest_ghost_counts,
                                manifest_host_buckets);
  }
  uint64_t legacy_manifest_file_number = 0;
  FileType legacy_type;
  bool parse_legacy =
      ParseFileName(fname, &legacy_manifest_file_number, &legacy_type);
  if (parse_legacy && legacy_type == kDescriptorFile) {
    manifest_paths->push_back(MakeDbRelativePath(dbname, fname));
    manifest_file_numbers->push_back(legacy_manifest_file_number);
    if (manifest_set_generation != nullptr) {
      *manifest_set_generation = 0;
    }
    if (manifest_time_mins != nullptr) {
      manifest_time_mins->push_back(0);
    }
    if (manifest_time_maxs != nullptr) {
      manifest_time_maxs->push_back(std::numeric_limits<uint64_t>::max());
    }
    if (manifest_host_buckets != nullptr) {
      manifest_host_buckets->push_back(std::numeric_limits<uint64_t>::max());
    }
    return Status::OK();
  }
  return Status::Corruption(
      "CURRENT file corrupted (expect MANIFEST-*, M_manager-*, or "
      "legacy MANIFEST_SET-*)");
}

Status GetCurrentManifestPathsFilteredByTimeRange(
    const std::string& dbname, FileSystem* fs, bool is_retry,
    uint64_t query_t_min, uint64_t query_t_max,
    std::vector<std::string>* filtered_manifest_paths,
    std::vector<uint64_t>* filtered_manifest_file_numbers,
    uint64_t* manifest_set_generation,
    std::vector<uint64_t>* filtered_manifest_time_mins,
    std::vector<uint64_t>* filtered_manifest_time_maxs,
    std::vector<float>* filtered_manifest_mbr_x_mins,
    std::vector<float>* filtered_manifest_mbr_x_maxs,
    std::vector<float>* filtered_manifest_mbr_y_mins,
    std::vector<float>* filtered_manifest_mbr_y_maxs,
    std::vector<uint64_t>* filtered_manifest_master_counts,
    std::vector<uint64_t>* filtered_manifest_ghost_counts,
    bool enable_spatial_window, float query_x_min, float query_x_max,
    float query_y_min, float query_y_max) {
  assert(filtered_manifest_paths != nullptr);
  assert(filtered_manifest_file_numbers != nullptr);
  filtered_manifest_paths->clear();
  filtered_manifest_file_numbers->clear();
  if (filtered_manifest_time_mins != nullptr) {
    filtered_manifest_time_mins->clear();
  }
  if (filtered_manifest_time_maxs != nullptr) {
    filtered_manifest_time_maxs->clear();
  }
  if (filtered_manifest_mbr_x_mins != nullptr) {
    filtered_manifest_mbr_x_mins->clear();
  }
  if (filtered_manifest_mbr_x_maxs != nullptr) {
    filtered_manifest_mbr_x_maxs->clear();
  }
  if (filtered_manifest_mbr_y_mins != nullptr) {
    filtered_manifest_mbr_y_mins->clear();
  }
  if (filtered_manifest_mbr_y_maxs != nullptr) {
    filtered_manifest_mbr_y_maxs->clear();
  }
  if (filtered_manifest_master_counts != nullptr) {
    filtered_manifest_master_counts->clear();
  }
  if (filtered_manifest_ghost_counts != nullptr) {
    filtered_manifest_ghost_counts->clear();
  }

  std::vector<std::string> manifest_paths;
  std::vector<uint64_t> manifest_file_numbers;
  std::vector<uint64_t> manifest_time_mins;
  std::vector<uint64_t> manifest_time_maxs;
  std::vector<float> manifest_x_mins;
  std::vector<float> manifest_x_maxs;
  std::vector<float> manifest_y_mins;
  std::vector<float> manifest_y_maxs;
  std::vector<uint64_t> manifest_master_counts;
  std::vector<uint64_t> manifest_ghost_counts;
  Status s = GetCurrentManifestPaths(
      dbname, fs, is_retry, &manifest_paths, &manifest_file_numbers,
      manifest_set_generation, &manifest_time_mins, &manifest_time_maxs,
      &manifest_x_mins, &manifest_x_maxs, &manifest_y_mins, &manifest_y_maxs,
      &manifest_master_counts, &manifest_ghost_counts);
  if (!s.ok()) {
    return s;
  }
  std::vector<size_t> selected_indices;
  FilterManifestShardIndicesByTimeRange(query_t_min, query_t_max,
                                        manifest_time_mins, manifest_time_maxs,
                                        &selected_indices, &manifest_x_mins,
                                        &manifest_x_maxs, &manifest_y_mins,
                                        &manifest_y_maxs, enable_spatial_window,
                                        query_x_min, query_x_max, query_y_min,
                                        query_y_max);
  filtered_manifest_paths->reserve(selected_indices.size());
  filtered_manifest_file_numbers->reserve(selected_indices.size());
  if (filtered_manifest_time_mins != nullptr) {
    filtered_manifest_time_mins->reserve(selected_indices.size());
  }
  if (filtered_manifest_time_maxs != nullptr) {
    filtered_manifest_time_maxs->reserve(selected_indices.size());
  }
  if (filtered_manifest_mbr_x_mins != nullptr) {
    filtered_manifest_mbr_x_mins->reserve(selected_indices.size());
  }
  if (filtered_manifest_mbr_x_maxs != nullptr) {
    filtered_manifest_mbr_x_maxs->reserve(selected_indices.size());
  }
  if (filtered_manifest_mbr_y_mins != nullptr) {
    filtered_manifest_mbr_y_mins->reserve(selected_indices.size());
  }
  if (filtered_manifest_mbr_y_maxs != nullptr) {
    filtered_manifest_mbr_y_maxs->reserve(selected_indices.size());
  }
  if (filtered_manifest_master_counts != nullptr) {
    filtered_manifest_master_counts->reserve(selected_indices.size());
  }
  if (filtered_manifest_ghost_counts != nullptr) {
    filtered_manifest_ghost_counts->reserve(selected_indices.size());
  }
  for (size_t i = 0; i < selected_indices.size(); ++i) {
    const size_t ridx = selected_indices[i];
    filtered_manifest_paths->push_back(manifest_paths[ridx]);
    filtered_manifest_file_numbers->push_back(manifest_file_numbers[ridx]);
    if (filtered_manifest_time_mins != nullptr) {
      filtered_manifest_time_mins->push_back(manifest_time_mins[ridx]);
    }
    if (filtered_manifest_time_maxs != nullptr) {
      filtered_manifest_time_maxs->push_back(manifest_time_maxs[ridx]);
    }
    if (filtered_manifest_mbr_x_mins != nullptr) {
      filtered_manifest_mbr_x_mins->push_back(manifest_x_mins[ridx]);
    }
    if (filtered_manifest_mbr_x_maxs != nullptr) {
      filtered_manifest_mbr_x_maxs->push_back(manifest_x_maxs[ridx]);
    }
    if (filtered_manifest_mbr_y_mins != nullptr) {
      filtered_manifest_mbr_y_mins->push_back(manifest_y_mins[ridx]);
    }
    if (filtered_manifest_mbr_y_maxs != nullptr) {
      filtered_manifest_mbr_y_maxs->push_back(manifest_y_maxs[ridx]);
    }
    if (filtered_manifest_master_counts != nullptr) {
      filtered_manifest_master_counts->push_back(manifest_master_counts[ridx]);
    }
    if (filtered_manifest_ghost_counts != nullptr) {
      filtered_manifest_ghost_counts->push_back(manifest_ghost_counts[ridx]);
    }
  }
  return Status::OK();
}

Status LoadCurrentManifestShardsByTimeRangeCached(
    const std::string& dbname, FileSystem* fs, bool is_retry,
    uint64_t query_t_min, uint64_t query_t_max,
    std::vector<std::string>* filtered_manifest_paths,
    std::vector<std::string>* filtered_manifest_contents,
    uint64_t* manifest_set_generation) {
  assert(filtered_manifest_paths != nullptr);
  assert(filtered_manifest_contents != nullptr);
  filtered_manifest_paths->clear();
  filtered_manifest_contents->clear();
  std::vector<uint64_t> filtered_manifest_file_numbers;
  Status s = GetCurrentManifestPathsFilteredByTimeRange(
      dbname, fs, is_retry, query_t_min, query_t_max, filtered_manifest_paths,
      &filtered_manifest_file_numbers, manifest_set_generation,
      /*filtered_manifest_time_mins=*/nullptr,
      /*filtered_manifest_time_maxs=*/nullptr);
  if (!s.ok()) {
    return s;
  }
  filtered_manifest_contents->reserve(filtered_manifest_paths->size());
  for (size_t i = 0; i < filtered_manifest_paths->size(); ++i) {
    std::string content;
    s = ReadManifestShardToStringCached(fs, is_retry,
                                        (*filtered_manifest_paths)[i], &content);
    if (!s.ok()) {
      return s;
    }
    filtered_manifest_contents->push_back(std::move(content));
  }
  return Status::OK();
}

Status GetCurrentManifestGlobalCountsFilteredByTimeRange(
    const std::string& dbname, FileSystem* fs, bool is_retry,
    uint64_t query_t_min, uint64_t query_t_max, uint64_t* total_masters,
    uint64_t* total_ghosts, uint64_t* manifest_set_generation,
    bool enable_spatial_window, float query_x_min, float query_x_max,
    float query_y_min, float query_y_max) {
  if (total_masters == nullptr || total_ghosts == nullptr) {
    return Status::InvalidArgument("null global count output");
  }
  *total_masters = 0;
  *total_ghosts = 0;
  assert(fs != nullptr);
  IOOptions opts;
  std::string fname;
  if (is_retry) {
    opts.verify_and_reconstruct_read = true;
  }
  Status s = ReadFileToString(fs, CurrentFileName(dbname), opts, &fname);
  if (!s.ok()) {
    return s;
  }
  if (fname.empty() || fname.back() != '\n') {
    return Status::Corruption("CURRENT file does not end with newline");
  }
  fname.resize(fname.size() - 1);
  const bool is_m_manager = (fname.rfind(kMManagerPrefix, 0) == 0);
  const bool is_manifest_set = (fname.rfind(kManifestSetPrefix, 0) == 0);
  uint64_t legacy_manifest_file_number = 0;
  FileType legacy_type;
  const bool is_single_manifest =
      ParseFileName(fname, &legacy_manifest_file_number, &legacy_type) &&
      legacy_type == kDescriptorFile;
  if (!is_m_manager && !is_manifest_set && !is_single_manifest) {
    return Status::Corruption(
        "CURRENT file corrupted (expect MANIFEST-*, M_manager-*, or "
        "legacy MANIFEST_SET-*)");
  }
  if (is_single_manifest) {
    *total_masters = 0;
    *total_ghosts = 0;
    if (manifest_set_generation != nullptr) {
      *manifest_set_generation = 0;
    }
    return Status::OK();
  }
  const std::string index_path = MakeDbRelativePath(dbname, fname);
  std::string index_content;
  s = ReadFileToString(fs, index_path, opts, &index_content);
  if (!s.ok()) {
    return s;
  }

  std::vector<std::string> manifest_paths;
  std::vector<uint64_t> manifest_file_numbers;
  std::vector<uint64_t> manifest_time_mins;
  std::vector<uint64_t> manifest_time_maxs;
  std::vector<float> manifest_x_mins;
  std::vector<float> manifest_x_maxs;
  std::vector<float> manifest_y_mins;
  std::vector<float> manifest_y_maxs;
  std::vector<uint64_t> manifest_masters;
  std::vector<uint64_t> manifest_ghosts;
  s = ParseManifestSetContent(
      dbname, index_content, &manifest_paths, &manifest_file_numbers,
      manifest_set_generation, &manifest_time_mins, &manifest_time_maxs,
      &manifest_x_mins, &manifest_x_maxs, &manifest_y_mins, &manifest_y_maxs,
      is_m_manager ? nullptr : &manifest_masters,
      is_m_manager ? nullptr : &manifest_ghosts, /*manifest_host_buckets=*/nullptr);
  if (!s.ok()) {
    return s;
  }

  std::vector<size_t> selected_indices;
  const std::vector<float>* spatial_x_mins =
      (is_m_manager || !enable_spatial_window) ? nullptr : &manifest_x_mins;
  const std::vector<float>* spatial_x_maxs =
      (is_m_manager || !enable_spatial_window) ? nullptr : &manifest_x_maxs;
  const std::vector<float>* spatial_y_mins =
      (is_m_manager || !enable_spatial_window) ? nullptr : &manifest_y_mins;
  const std::vector<float>* spatial_y_maxs =
      (is_m_manager || !enable_spatial_window) ? nullptr : &manifest_y_maxs;
  FilterManifestShardIndicesByTimeRange(
      query_t_min, query_t_max, manifest_time_mins, manifest_time_maxs,
      &selected_indices, spatial_x_mins, spatial_x_maxs, spatial_y_mins,
      spatial_y_maxs, enable_spatial_window && !is_m_manager, query_x_min,
      query_x_max, query_y_min, query_y_max);
  if (!is_m_manager) {
    for (size_t i = 0; i < selected_indices.size(); ++i) {
      const size_t ridx = selected_indices[i];
      if (ridx < manifest_masters.size()) {
        *total_masters += manifest_masters[ridx];
      }
      if (ridx < manifest_ghosts.size()) {
        *total_ghosts += manifest_ghosts[ridx];
      }
    }
  }
  return Status::OK();
}

Status ReadManifestShardToStringCached(FileSystem* fs, bool is_retry,
                                       const std::string& manifest_path,
                                       std::string* content) {
  if (fs == nullptr || content == nullptr) {
    return Status::InvalidArgument("null input");
  }
  IOOptions opts;
  if (is_retry) {
    opts.verify_and_reconstruct_read = true;
  }
  return ManifestShardFileCache().Read(fs, manifest_path, opts, content);
}

void FilterManifestShardIndicesByTimeRange(
    uint64_t query_t_min, uint64_t query_t_max,
    const std::vector<uint64_t>& shard_t_mins,
    const std::vector<uint64_t>& shard_t_maxs,
    std::vector<size_t>* out_indices, const std::vector<float>* shard_x_mins,
    const std::vector<float>* shard_x_maxs, const std::vector<float>* shard_y_mins,
    const std::vector<float>* shard_y_maxs, bool enable_spatial_window,
    float query_x_min, float query_x_max, float query_y_min, float query_y_max,
    uint32_t manifest_bucket_width_sec,
    const std::vector<uint64_t>* shard_host_buckets) {
  assert(out_indices != nullptr);
  out_indices->clear();
  if (shard_t_mins.size() != shard_t_maxs.size()) {
    return;
  }
  constexpr uint64_t kTimeUnknownMin = 0;
  constexpr uint64_t kTimeUnknownMax = std::numeric_limits<uint64_t>::max();
  constexpr uint64_t kHostBucketUnknown = std::numeric_limits<uint64_t>::max();
  const uint64_t query_b0 =
      manifest_bucket_width_sec > 0 ? query_t_min / manifest_bucket_width_sec : 0;
  const uint64_t query_b1 = manifest_bucket_width_sec > 0
                                ? query_t_max / manifest_bucket_width_sec
                                : 0;
  for (size_t i = 0; i < shard_t_mins.size(); ++i) {
    const uint64_t tmin = shard_t_mins[i];
    const uint64_t tmax = shard_t_maxs[i];
    if (tmin == kTimeUnknownMin && tmax == kTimeUnknownMax) {
      out_indices->push_back(i);
      continue;
    }
    if (manifest_bucket_width_sec > 0) {
      const uint64_t span_buckets =
          (tmax / manifest_bucket_width_sec) -
          (tmin / manifest_bucket_width_sec) + 1;
      if (shard_host_buckets != nullptr && i < shard_host_buckets->size() &&
          (*shard_host_buckets)[i] != kHostBucketUnknown) {
        const uint64_t hb = (*shard_host_buckets)[i];
        if (hb < query_b0 || hb > query_b1) {
          continue;
        }
      } else if (span_buckets > 32) {
        // Legacy dataset-wide rows: do not treat as "keep all queries".
        if (tmax < query_t_min || tmin > query_t_max) {
          continue;
        }
      }
    }
    if (tmax < query_t_min || tmin > query_t_max) {
      continue;
    }
    if (enable_spatial_window && shard_x_mins != nullptr &&
        shard_x_maxs != nullptr && shard_y_mins != nullptr &&
        shard_y_maxs != nullptr && shard_x_mins->size() == shard_t_mins.size() &&
        shard_x_maxs->size() == shard_t_mins.size() &&
        shard_y_mins->size() == shard_t_mins.size() &&
        shard_y_maxs->size() == shard_t_mins.size()) {
      const float sxmin = (*shard_x_mins)[i];
      const float sxmax = (*shard_x_maxs)[i];
      const float symin = (*shard_y_mins)[i];
      const float symax = (*shard_y_maxs)[i];
      // Unknown MBR (NaN) stays conservative: keep shard.
      if (!(std::isnan(sxmin) || std::isnan(sxmax) || std::isnan(symin) ||
            std::isnan(symax))) {
        const bool spatial_disjoint =
            sxmax < query_x_min || sxmin > query_x_max || symax < query_y_min ||
            symin > query_y_max;
        if (spatial_disjoint) {
          continue;
        }
      }
    }
    out_indices->push_back(i);
  }
}

namespace {

struct ManifestLogReporter : public log::Reader::Reporter {
  Status* status = nullptr;
  void Corruption(size_t /*bytes*/, const Status& s,
                  uint64_t /*log_number*/ = kMaxSequenceNumber) override {
    if (status != nullptr && status->ok()) {
      *status = s;
    }
  }
};

[[maybe_unused]] void AddManifestSstPruneStats(
    ManifestSstPruneStats* acc, const ManifestSstPruneStats& delta) {
  if (acc == nullptr) {
    return;
  }
  acc->live_files += delta.live_files;
  acc->with_st_meta += delta.with_st_meta;
  acc->missing_st_meta += delta.missing_st_meta;
  acc->range_del_blocked += delta.range_del_blocked;
  acc->pruned_disjoint += delta.pruned_disjoint;
  acc->kept_intersect += delta.kept_intersect;
}

}  // namespace

Status CountLiveSstFilesInManifestPaths(
    const std::vector<std::string>& manifest_paths, FileSystem* fs,
    bool is_retry, uint32_t column_family_id_filter, uint32_t query_t_min,
    uint32_t query_t_max, float query_x_min, float query_x_max, float query_y_min,
    float query_y_max, ManifestSstPruneStats* stats) {
  if (fs == nullptr || stats == nullptr) {
    return Status::InvalidArgument("null input");
  }
  *stats = ManifestSstPruneStats{};
  if (manifest_paths.empty()) {
    return Status::OK();
  }
  const ReadOptions read_options;
  ManifestSstPruneStatsHandler handler(read_options, column_family_id_filter);
  Status s;
  for (const std::string& manifest_path : manifest_paths) {
    std::unique_ptr<FSSequentialFile> file;
    s = fs->NewSequentialFile(manifest_path, FileOptions(), &file, nullptr);
    if (!s.ok()) {
      return s;
    }
    std::unique_ptr<SequentialFileReader> file_reader =
        std::make_unique<SequentialFileReader>(std::move(file), manifest_path,
                                               /*io_tracer=*/nullptr);
    ManifestLogReporter reporter;
    reporter.status = &s;
    log::Reader reader(nullptr, std::move(file_reader), &reporter,
                     true /* checksum */, 0 /* log_number */);
    handler.Iterate(reader, &s);
    if (!handler.status().ok()) {
      return handler.status();
    }
    if (!s.ok()) {
      return s;
    }
  }
  handler.ComputeStats(query_t_min, query_t_max, query_x_min, query_x_max,
                       query_y_min, query_y_max, /*enable_spatial_window=*/true,
                       stats);
  return Status::OK();
}

Status CountLiveSstFilesInManifestPath(
    const std::string& manifest_path, FileSystem* fs, bool is_retry,
    uint32_t column_family_id_filter, uint32_t query_t_min, uint32_t query_t_max,
    float query_x_min, float query_x_max, float query_y_min, float query_y_max,
    ManifestSstPruneStats* stats) {
  if (manifest_path.empty()) {
    return Status::InvalidArgument("manifest_path is empty");
  }
  return CountLiveSstFilesInManifestPaths(
      std::vector<std::string>{manifest_path}, fs, is_retry,
      column_family_id_filter, query_t_min, query_t_max, query_x_min, query_x_max,
      query_y_min, query_y_max, stats);
}

Status GetLiveSstLevelStatsFromMManager(const std::string& dbname, FileSystem* fs,
                                        bool is_retry, StLiveSstLevelStats* out) {
  if (fs == nullptr || out == nullptr) {
    return Status::InvalidArgument("null input");
  }
  *out = StLiveSstLevelStats{};
  std::vector<std::string> manifest_paths;
  std::vector<uint64_t> manifest_numbers;
  Status s = GetCurrentManifestPaths(dbname, fs, is_retry, &manifest_paths,
                                     &manifest_numbers);
  if (!s.ok()) {
    return s;
  }
  out->m_manager_shards = manifest_paths.size();
  if (manifest_paths.empty()) {
    return Status::OK();
  }
  const ReadOptions read_options;
  ManifestSstPruneStatsHandler handler(read_options);
  for (const std::string& manifest_path : manifest_paths) {
    std::unique_ptr<FSSequentialFile> file;
    s = fs->NewSequentialFile(manifest_path, FileOptions(), &file, nullptr);
    if (!s.ok()) {
      return s;
    }
    std::unique_ptr<SequentialFileReader> file_reader =
        std::make_unique<SequentialFileReader>(std::move(file), manifest_path,
                                               /*io_tracer=*/nullptr);
    ManifestLogReporter reporter;
    reporter.status = &s;
    log::Reader reader(nullptr, std::move(file_reader), &reporter,
                       true /* checksum */, 0 /* log_number */);
    handler.Iterate(reader, &s);
    if (!handler.status().ok()) {
      return handler.status();
    }
    if (!s.ok()) {
      return s;
    }
  }

  std::unordered_set<uint64_t> disk_file_numbers;
  std::vector<std::string> children;
  s = fs->GetChildren(dbname, IOOptions(), &children, nullptr);
  if (!s.ok()) {
    return s;
  }
  for (const std::string& name : children) {
    uint64_t number = 0;
    FileType type;
    if (ParseFileName(name, &number, &type) && type == kTableFile) {
      disk_file_numbers.insert(number);
    }
  }
  out->disk_sst_files = disk_file_numbers.size();

  std::unordered_map<uint64_t, std::pair<int, const FileMetaData*>> manifest_live;
  handler.ForEachLiveFile([&](int level, const FileMetaData& f) {
    manifest_live[f.fd.GetNumber()] = {level, &f};
  });
  for (const auto& kv : manifest_live) {
    if (disk_file_numbers.find(kv.first) == disk_file_numbers.end()) {
      ++out->manifest_refs_not_on_disk;
    }
  }

  std::map<int, StSstLevelRow> by_level;
  for (const auto& kv : manifest_live) {
    if (disk_file_numbers.find(kv.first) == disk_file_numbers.end()) {
      continue;
    }
    const int level = kv.second.first;
    const FileMetaData& f = *kv.second.second;
    ++out->unique_live_files;
    if (f.has_st_file_meta) {
      ++out->with_st_file_meta;
    } else {
      ++out->without_st_file_meta;
    }
    StSstLevelRow& row = by_level[level];
    row.level = level;
    row.file_count++;
    row.total_bytes += f.fd.GetFileSize();
    row.total_entries += f.num_entries;
  }
  for (uint64_t num : disk_file_numbers) {
    if (manifest_live.find(num) == manifest_live.end()) {
      ++out->disk_sst_without_manifest_level;
    }
  }
  out->per_level.reserve(by_level.size());
  for (const auto& kv : by_level) {
    out->per_level.push_back(kv.second);
  }
  return Status::OK();
}

Status GetStPruneLayerStats(
    const std::string& dbname, FileSystem* fs, bool is_retry,
    uint32_t query_t_min, uint32_t query_t_max, bool enable_spatial_window,
    float query_x_min, float query_x_max, float query_y_min, float query_y_max,
    uint32_t column_family_id_filter, StPruneLayerStats* out,
    std::vector<ManifestSstPruneStats>* per_selected_shard_stats) {
  if (fs == nullptr || out == nullptr) {
    return Status::InvalidArgument("null input");
  }
  *out = StPruneLayerStats{};
  if (per_selected_shard_stats != nullptr) {
    per_selected_shard_stats->clear();
  }

  std::vector<std::string> manifest_paths;
  std::vector<uint64_t> manifest_file_numbers;
  std::vector<uint64_t> manifest_time_mins;
  std::vector<uint64_t> manifest_time_maxs;
  std::vector<uint64_t> manifest_host_buckets;
  std::vector<float> manifest_x_mins;
  std::vector<float> manifest_x_maxs;
  std::vector<float> manifest_y_mins;
  std::vector<float> manifest_y_maxs;
  Status s = GetCurrentManifestPaths(
      dbname, fs, is_retry, &manifest_paths, &manifest_file_numbers,
      &out->m_manager_generation, &manifest_time_mins, &manifest_time_maxs,
      &manifest_x_mins, &manifest_x_maxs, &manifest_y_mins, &manifest_y_maxs,
      /*master_counts=*/nullptr, /*ghost_counts=*/nullptr,
      &manifest_host_buckets);
  if (!s.ok()) {
    return s;
  }
  out->manifest_shard_total = manifest_paths.size();

  uint32_t bucket_width_sec = 0;
  const char* bw_env =
      std::getenv("ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_BUCKET_WIDTH_SEC");
  if (bw_env != nullptr && *bw_env != '\0') {
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(bw_env, &end, 10);
    if (end != bw_env && *end == '\0' && parsed > 0 &&
        parsed <= UINT32_MAX) {
      bucket_width_sec = static_cast<uint32_t>(parsed);
    }
  }

  std::vector<size_t> selected_indices;
  FilterManifestShardIndicesByTimeRange(
      query_t_min, query_t_max, manifest_time_mins, manifest_time_maxs,
      &selected_indices, &manifest_x_mins, &manifest_x_maxs, &manifest_y_mins,
      &manifest_y_maxs, enable_spatial_window, query_x_min, query_x_max,
      query_y_min, query_y_max, bucket_width_sec, &manifest_host_buckets);
  out->manifest_shard_selected = selected_indices.size();

  const float kFloatMax = (std::numeric_limits<float>::max)();
  const float sx_min = enable_spatial_window ? query_x_min : -kFloatMax;
  const float sx_max = enable_spatial_window ? query_x_max : kFloatMax;
  const float sy_min = enable_spatial_window ? query_y_min : -kFloatMax;
  const float sy_max = enable_spatial_window ? query_y_max : kFloatMax;

  // Live SST census: replay all per-bucket shard logs in M_manager order.
  if (!manifest_paths.empty()) {
    s = CountLiveSstFilesInManifestPaths(
        manifest_paths, fs, is_retry, column_family_id_filter, query_t_min,
        query_t_max, sx_min, sx_max, sy_min, sy_max, &out->sst_all_shards);
    if (!s.ok()) {
      return s;
    }
  }

  std::vector<std::string> selected_paths;
  selected_paths.reserve(selected_indices.size());
  for (size_t ridx : selected_indices) {
    if (ridx < manifest_paths.size()) {
      selected_paths.push_back(manifest_paths[ridx]);
    }
  }
  if (!selected_paths.empty()) {
    s = CountLiveSstFilesInManifestPaths(
        selected_paths, fs, is_retry, column_family_id_filter, query_t_min,
        query_t_max, sx_min, sx_max, sy_min, sy_max, &out->sst_selected_shards);
    if (!s.ok()) {
      return s;
    }
  }
  if (per_selected_shard_stats != nullptr) {
    for (const std::string& path : selected_paths) {
      ManifestSstPruneStats shard_stats;
      s = CountLiveSstFilesInManifestPath(
          path, fs, is_retry, column_family_id_filter, query_t_min, query_t_max,
          sx_min, sx_max, sy_min, sy_max, &shard_stats);
      if (!s.ok()) {
        return s;
      }
      per_selected_shard_stats->push_back(shard_stats);
    }
  }
  return Status::OK();
}

StManifestSstFilterResult ManifestFilterSstByStFileMeta(
    const VersionStorageInfo& vstorage, uint32_t q_t_min, uint32_t q_t_max,
    float q_x_min, float q_x_max, float q_y_min, float q_y_max) {
  StManifestSstFilterResult out;

  // Files across levels sorted by st_file_meta.t_min (built in
  // VersionStorageInfo::PrepareForVersionAppend). Binary-search the upper
  // bound instead of scanning every file.
  const auto& sorted = vstorage.StTimeSortedFiles();
  out.total_files = static_cast<uint64_t>(sorted.size());

  // First file with t_min > q_t_max; files after this start strictly after
  // the query window and are time-disjoint (skipped without per-file check).
  auto upper = std::upper_bound(
      sorted.begin(), sorted.end(), q_t_max,
      [](uint32_t t, const FileMetaData* f) {
        const uint32_t lower = (!f->has_st_file_meta || f->num_range_deletions > 0)
                                   ? 0 : f->st_file_meta.t_min;
        return t < lower;
      });
  out.time_disjoint += static_cast<uint64_t>(sorted.end() - upper);

  for (auto it = sorted.begin(); it != upper; ++it) {
    const FileMetaData* f = *it;
    if (!f->has_st_file_meta) {
      ++out.missing_meta;
      out.file_numbers.insert(f->fd.GetNumber());
      continue;
    }
    if (f->num_range_deletions > 0) {
      ++out.passed;
      out.file_numbers.insert(f->fd.GetNumber());
      continue;
    }
    const auto& m = f->st_file_meta;
    // t_min <= q_t_max already guaranteed by the bound; check t_max and MBR.
    const bool time_disjoint = (m.t_max < q_t_min);
    const bool space_disjoint =
        (m.x_max < q_x_min || m.x_min > q_x_max || m.y_max < q_y_min ||
         m.y_min > q_y_max);
    if (time_disjoint) ++out.time_disjoint;
    if (space_disjoint) ++out.space_disjoint;
    if (time_disjoint || space_disjoint) {
      continue;
    }
    ++out.passed;
    out.file_numbers.insert(f->fd.GetNumber());
  }
  return out;
}

}  // namespace ROCKSDB_NAMESPACE

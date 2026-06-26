#include "manifest_ops.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/file_system.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/table.h>

namespace fs = std::filesystem;

namespace manifest_ops {
using ManifestEntry = rocksdb::ManifestEntry;
using ManifestFileEntry = rocksdb::ManifestFileEntry;
using ManagerState = rocksdb::ManagerState;
using ManifestState = rocksdb::ManifestState;

namespace {

static std::string Trim(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) {
    ++b;
  }
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
    --e;
  }
  return s.substr(b, e - b);
}

static bool ReadFileText(const fs::path& p, std::string* out) {
  if (out == nullptr) return false;
  std::ifstream f(p, std::ios::in | std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

static bool ParseUInt64(const std::string& s, uint64_t* out) {
  if (out == nullptr) return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
  if (end == s.c_str() || errno != 0) return false;
  *out = static_cast<uint64_t>(v);
  return true;
}

static bool ParseUInt32(const std::string& s, uint32_t* out) {
  uint64_t v = 0;
  if (!ParseUInt64(s, &v) || v > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  *out = static_cast<uint32_t>(v);
  return true;
}

static bool ParseFloat(const std::string& s, float* out) {
  if (out == nullptr) return false;
  char* end = nullptr;
  errno = 0;
  const float v = std::strtof(s.c_str(), &end);
  if (end == s.c_str() || errno != 0) return false;
  *out = v;
  return true;
}

static bool ParseManagerLine(const std::string& line, ManifestEntry* e) {
  if (e == nullptr) return false;
  const std::string s = Trim(line);
  if (s.rfind("entry=", 0) != 0) return false;

  const std::string payload = s.substr(6);
  const auto comma = payload.find(',');
  if (comma == std::string::npos) return false;
  e->manifest_name = payload.substr(0, comma);
  const std::string rest = payload.substr(comma + 1);
  std::stringstream ss(rest);
  std::string kv;
  while (std::getline(ss, kv, ',')) {
    const auto eq = kv.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = Trim(kv.substr(0, eq));
    const std::string v = Trim(kv.substr(eq + 1));
    if (k == "tmin") {
      ParseUInt32(v, &e->t_min);
    } else if (k == "tmax") {
      ParseUInt32(v, &e->t_max);
    }
  }
  return !e->manifest_name.empty();
}

static bool ParseManifestMetaLine(const std::string& line,
                                  ManifestFileEntry* e) {
  if (e == nullptr) return false;
  const std::string s = Trim(line);
  if (s.empty() || s[0] == '#') return false;

  std::stringstream ss(s);
  std::string kv;
  bool matched = false;
  while (std::getline(ss, kv, ' ')) {
    const auto eq = kv.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = Trim(kv.substr(0, eq));
    const std::string v = Trim(kv.substr(eq + 1));
    if (k == "file" || k == "file_number") {
      matched = ParseUInt64(v, &e->file_number) || matched;
    } else if (k == "level") {
      matched = ParseUInt32(v, &e->level) || matched;
    } else if (k == "size" || k == "file_size") {
      matched = ParseUInt64(v, &e->file_size) || matched;
    } else if (k == "tmin") {
      matched = ParseUInt32(v, &e->t_min) || matched;
      e->has_time = true;
    } else if (k == "tmax") {
      matched = ParseUInt32(v, &e->t_max) || matched;
      e->t_end_max = e->t_max;
      e->has_time = true;
    } else if (k == "t_end_max") {
      matched = ParseUInt32(v, &e->t_end_max) || matched;
      e->has_time = true;
    } else if (k == "xmin") {
      matched = ParseFloat(v, &e->x_min) || matched;
      e->has_space = true;
    } else if (k == "xmax") {
      matched = ParseFloat(v, &e->x_max) || matched;
      e->has_space = true;
    } else if (k == "ymin") {
      matched = ParseFloat(v, &e->y_min) || matched;
      e->has_space = true;
    } else if (k == "ymax") {
      matched = ParseFloat(v, &e->y_max) || matched;
      e->has_space = true;
    } else if (k == "smallest_key") {
      e->smallest_key = v;
      e->has_key_range = true;
      matched = true;
    } else if (k == "largest_key") {
      e->largest_key = v;
      e->has_key_range = true;
      matched = true;
    }
  }
  return matched;
}

}  // namespace

bool GetCurrentManifestName(const std::string& db_dir, std::string* out) {
  if (out == nullptr) return false;
  std::string current;
  if (!ReadFileText(fs::path(db_dir) / "CURRENT", &current)) return false;
  current = Trim(current);
  if (current.empty()) return false;
  *out = current;
  return true;
}

bool LoadManagerState(const std::string& db_dir, ManagerState* out) {
  if (out == nullptr) return false;
  out->current_manifest.clear();
  out->entries.clear();

  std::string current;
  if (!ReadFileText(fs::path(db_dir) / "CURRENT", &current)) return false;
  current = Trim(current);
  if (current.empty()) return false;
  out->current_manifest = current;

  std::string mgr_text;
  if (!ReadFileText(fs::path(db_dir) / current, &mgr_text)) return false;

  std::istringstream iss(mgr_text);
  std::string line;
  while (std::getline(iss, line)) {
    ManifestEntry e;
    if (ParseManagerLine(line, &e)) {
      out->entries.push_back(e);
    }
  }

  if (out->entries.empty()) {
    uint32_t tmin = std::numeric_limits<uint32_t>::max();
    uint32_t tmax = 0;
    iss.clear();
    iss.seekg(0);
    while (std::getline(iss, line)) {
      const std::string s = Trim(line);
      const auto p1 = s.find("tmin=");
      const auto p2 = s.find("tmax=");
      if (p1 != std::string::npos) {
        uint32_t v = 0;
        if (ParseUInt32(s.substr(p1 + 5), &v)) tmin = std::min(tmin, v);
      }
      if (p2 != std::string::npos) {
        uint32_t v = 0;
        if (ParseUInt32(s.substr(p2 + 5), &v)) tmax = std::max(tmax, v);
      }
    }
    if (tmin != std::numeric_limits<uint32_t>::max() && tmax >= tmin) {
      out->entries.push_back({current, tmin, tmax});
    }
  }

  return !out->entries.empty();
}

bool GetManagerTimeRange(const std::string& db_dir, uint32_t* t_min,
                         uint32_t* t_max) {
  if (t_min == nullptr || t_max == nullptr) return false;
  ManagerState st;
  if (!LoadManagerState(db_dir, &st)) return false;
  uint32_t lo = std::numeric_limits<uint32_t>::max();
  uint32_t hi = 0;
  for (const auto& e : st.entries) {
    lo = std::min(lo, e.t_min);
    hi = std::max(hi, e.t_max);
  }
  if (lo == std::numeric_limits<uint32_t>::max() || hi < lo) return false;
  *t_min = lo;
  *t_max = hi;
  return true;
}

bool LoadManifestState(rocksdb::DB* db, ManifestState* out) {
  if (out == nullptr || db == nullptr) return false;
  out->manifest_name.clear();
  out->files.clear();

  std::vector<rocksdb::LiveFileMetaData> live_files;
  db->GetLiveFilesMetaData(&live_files);
  if (live_files.empty()) return false;

  out->files.reserve(live_files.size());
  for (const auto& lf : live_files) {
    ManifestFileEntry e;
    e.file_number = static_cast<uint64_t>(lf.file_number);
    e.level = static_cast<uint32_t>(lf.level);
    e.file_size = static_cast<uint64_t>(lf.size);
    e.is_conservative = true;
    if (!lf.smallestkey.empty()) {
      e.smallest_key.assign(lf.smallestkey.data(), lf.smallestkey.size());
      e.has_key_range = true;
    }
    if (!lf.largestkey.empty()) {
      e.largest_key.assign(lf.largestkey.data(), lf.largestkey.size());
      e.has_key_range = true;
    }
    out->files.push_back(std::move(e));
  }
  return true;
}

std::vector<ManifestFileEntry> FilterCandidateFiles(
    const std::vector<ManifestFileEntry>& files, uint32_t q_t_min,
    uint32_t q_t_max, float q_x_min, float q_x_max, float q_y_min,
    float q_y_max) {
  std::vector<ManifestFileEntry> out;
  for (const auto& f : files) {
    if (!f.has_time || !f.has_space) {
      out.push_back(f);
      continue;
    }
    const bool time_ok = !(f.t_max < q_t_min || f.t_min > q_t_max);
    const bool space_ok = !(f.x_max < q_x_min || f.x_min > q_x_max ||
                            f.y_max < q_y_min || f.y_min > q_y_max);
    if (time_ok && space_ok) out.push_back(f);
  }
  return out;
}

std::vector<ManifestFileEntry> BuildCandidateFilesFromLive(
    rocksdb::DB* db, uint32_t q_t_min, uint32_t q_t_max, float q_x_min,
    float q_x_max, float q_y_min, float q_y_max) {
  ManifestState st;
  if (!LoadManifestState(db, &st)) return {};
  auto cand = FilterCandidateFiles(st.files, q_t_min, q_t_max, q_x_min,
                                   q_x_max, q_y_min, q_y_max);
  if (cand.empty()) cand = st.files;
  return cand;
}

bool CandidateMayContainKey(const ManifestFileEntry& entry,
                            const rocksdb::Slice& user_key) {
  if (!entry.has_key_range) return true;
  if (entry.smallest_key.empty() || entry.largest_key.empty()) return true;
  const std::string uk(user_key.data(), user_key.size());
  return uk >= entry.smallest_key && uk <= entry.largest_key;
}

}  // namespace manifest_ops

namespace rocksdb {

Status GetCurrentManifestPaths(
    const std::string& db_path, FileSystem* /*fs*/, bool /*is_retry*/,
    std::vector<std::string>* paths, std::vector<uint64_t>* numbers,
    uint64_t* generation, std::vector<uint64_t>* tmins,
    std::vector<uint64_t>* tmaxs, std::vector<float>* xmins,
    std::vector<float>* xmaxs, std::vector<float>* ymins,
    std::vector<float>* ymaxs, std::vector<uint64_t>* masters,
    std::vector<uint64_t>* ghosts) {
  if (paths == nullptr || numbers == nullptr || generation == nullptr ||
      tmins == nullptr || tmaxs == nullptr || xmins == nullptr ||
      xmaxs == nullptr || ymins == nullptr || ymaxs == nullptr ||
      masters == nullptr || ghosts == nullptr) {
    return Status::InvalidArgument("null output in GetCurrentManifestPaths");
  }

  paths->clear();
  numbers->clear();
  tmins->clear();
  tmaxs->clear();
  xmins->clear();
  xmaxs->clear();
  ymins->clear();
  ymaxs->clear();
  masters->clear();
  ghosts->clear();
  *generation = 0;

  manifest_ops::ManagerState mgr;
  if (manifest_ops::LoadManagerState(db_path, &mgr)) {
    uint64_t idx = 0;
    for (const auto& e : mgr.entries) {
      paths->push_back(e.manifest_name);
      numbers->push_back(++idx);
      tmins->push_back(e.t_min);
      tmaxs->push_back(e.t_max);
      xmins->push_back(std::numeric_limits<float>::quiet_NaN());
      xmaxs->push_back(std::numeric_limits<float>::quiet_NaN());
      ymins->push_back(std::numeric_limits<float>::quiet_NaN());
      ymaxs->push_back(std::numeric_limits<float>::quiet_NaN());
      masters->push_back(0);
      ghosts->push_back(0);
    }
    *generation = idx;
    return Status::OK();
  }

  std::string cur;
  if (!manifest_ops::GetCurrentManifestName(db_path, &cur)) {
    return Status::NotFound("CURRENT/M_manager not found", db_path);
  }
  paths->push_back(cur);
  numbers->push_back(1);
  tmins->push_back(0);
  tmaxs->push_back(UINT64_MAX);
  xmins->push_back(std::numeric_limits<float>::quiet_NaN());
  xmaxs->push_back(std::numeric_limits<float>::quiet_NaN());
  ymins->push_back(std::numeric_limits<float>::quiet_NaN());
  ymaxs->push_back(std::numeric_limits<float>::quiet_NaN());
  masters->push_back(0);
  ghosts->push_back(0);
  *generation = 1;
  return Status::OK();
}

void FilterManifestShardIndicesByTimeRange(
    uint64_t q_t_min, uint64_t q_t_max, const std::vector<uint64_t>& tmins,
    const std::vector<uint64_t>& tmaxs, std::vector<size_t>* selected,
    const std::vector<float>* xmins, const std::vector<float>* xmaxs,
    const std::vector<float>* ymins, const std::vector<float>* ymaxs,
    bool enable_spatial_window, float q_x_min, float q_x_max, float q_y_min,
    float q_y_max) {
  if (selected == nullptr) return;
  selected->clear();
  const size_t n = std::min(tmins.size(), tmaxs.size());
  for (size_t i = 0; i < n; ++i) {
    if (tmaxs[i] < q_t_min || tmins[i] > q_t_max) continue;
    if (enable_spatial_window && xmins && xmaxs && ymins && ymaxs &&
        i < xmins->size() && i < xmaxs->size() && i < ymins->size() &&
        i < ymaxs->size()) {
      const float x0 = (*xmins)[i];
      const float x1 = (*xmaxs)[i];
      const float y0 = (*ymins)[i];
      const float y1 = (*ymaxs)[i];
      if (!std::isnan(x0) && !std::isnan(x1) && !std::isnan(y0) &&
          !std::isnan(y1) &&
          (x1 < q_x_min || x0 > q_x_max || y1 < q_y_min || y0 > q_y_max)) {
        continue;
      }
    }
    selected->push_back(i);
  }
}

#ifndef ROCKS_DEMO_SKIP_ROCKSDB_MANIFEST_STATS_IMPL
Status GetCurrentManifestGlobalCountsFilteredByTimeRange(
    const std::string& db_path, FileSystem* fs, bool is_retry,
    uint64_t q_t_min, uint64_t q_t_max, uint64_t* masters,
    uint64_t* ghosts, uint64_t* generation, bool enable_spatial_window,
    float q_x_min, float q_x_max, float q_y_min, float q_y_max) {
  if (masters == nullptr || ghosts == nullptr || generation == nullptr) {
    return Status::InvalidArgument("null output in manifest counts");
  }
  std::vector<std::string> paths;
  std::vector<uint64_t> numbers, tmins, tmaxs, m, g;
  std::vector<float> xmins, xmaxs, ymins, ymaxs;
  Status s = GetCurrentManifestPaths(db_path, fs, is_retry, &paths, &numbers,
                                     generation, &tmins, &tmaxs, &xmins,
                                     &xmaxs, &ymins, &ymaxs, &m, &g);
  if (!s.ok()) return s;
  std::vector<size_t> selected;
  FilterManifestShardIndicesByTimeRange(q_t_min, q_t_max, tmins, tmaxs,
                                        &selected, &xmins, &xmaxs, &ymins,
                                        &ymaxs, enable_spatial_window, q_x_min,
                                        q_x_max, q_y_min, q_y_max);
  *masters = 0;
  *ghosts = 0;
  for (size_t i : selected) {
    if (i < m.size()) *masters += m[i];
    if (i < g.size()) *ghosts += g[i];
  }
  return Status::OK();
}

Status GetStPruneLayerStats(
    const std::string& db_path, FileSystem* fs, bool is_retry,
    uint32_t q_t_min, uint32_t q_t_max, bool enable_spatial_window,
    float q_x_min, float q_x_max, float q_y_min, float q_y_max,
    uint32_t max_manifest_shards, StPruneLayerStats* out,
    std::vector<ManifestSstPruneStats>* per_shard) {
  if (out == nullptr) {
    return Status::InvalidArgument("null StPruneLayerStats output");
  }
  *out = StPruneLayerStats{};
  if (per_shard != nullptr) per_shard->clear();

  std::vector<std::string> paths;
  std::vector<uint64_t> numbers, tmins, tmaxs, masters, ghosts;
  std::vector<float> xmins, xmaxs, ymins, ymaxs;
  Status s = GetCurrentManifestPaths(db_path, fs, is_retry, &paths, &numbers,
                                     &out->m_manager_generation, &tmins,
                                     &tmaxs, &xmins, &xmaxs, &ymins, &ymaxs,
                                     &masters, &ghosts);
  if (!s.ok()) return s;
  out->manifest_shard_total = paths.size();

  std::vector<size_t> selected;
  FilterManifestShardIndicesByTimeRange(q_t_min, q_t_max, tmins, tmaxs,
                                        &selected, &xmins, &xmaxs, &ymins,
                                        &ymaxs, enable_spatial_window, q_x_min,
                                        q_x_max, q_y_min, q_y_max);
  if (selected.size() > max_manifest_shards) selected.resize(max_manifest_shards);
  out->manifest_shard_selected = selected.size();

  rocksdb::Options options;
  options.create_if_missing = false;
  std::unique_ptr<rocksdb::DB> db;
  Status open_s = rocksdb::DB::OpenForReadOnly(options, db_path, &db);
  if (!open_s.ok()) return open_s;

  manifest_ops::ManifestState st;
  if (!manifest_ops::LoadManifestState(db.get(), &st)) {
    return Status::OK();
  }

  auto fill_stats = [&](ManifestSstPruneStats* stats) {
    for (const auto& f : st.files) {
      ++stats->live_files;
      if (f.has_time || f.has_space) {
        ++stats->with_st_meta;
      } else {
        ++stats->missing_st_meta;
      }
      if (f.has_time && (f.t_max < q_t_min || f.t_min > q_t_max)) {
        ++stats->pruned_disjoint;
        continue;
      }
      if (enable_spatial_window && f.has_space &&
          (f.x_max < q_x_min || f.x_min > q_x_max || f.y_max < q_y_min ||
           f.y_min > q_y_max)) {
        ++stats->pruned_disjoint;
        continue;
      }
      ++stats->kept_intersect;
    }
  };

  fill_stats(&out->sst_all_shards);
  out->sst_selected_shards = out->sst_all_shards;
  if (per_shard != nullptr) {
    for (size_t i : selected) {
      (void)i;
      ManifestSstPruneStats shard;
      fill_stats(&shard);
      per_shard->push_back(shard);
    }
  }
  return Status::OK();
}

#endif  // ROCKS_DEMO_SKIP_ROCKSDB_MANIFEST_STATS_IMPL

#ifndef ROCKS_DEMO_SKIP_ROCKSDB_MANIFEST_LEVEL_STATS_IMPL
Status GetLiveSstLevelStatsFromMManager(const std::string& db_path,
                                        FileSystem* /*fs*/, bool /*is_retry*/,
                                        StLiveSstLevelStats* out) {
  if (out == nullptr) {
    return Status::InvalidArgument("null StLiveSstLevelStats output");
  }
  *out = StLiveSstLevelStats{};

  manifest_ops::ManagerState mgr;
  if (manifest_ops::LoadManagerState(db_path, &mgr)) {
    out->m_manager_shards = mgr.entries.size();
  }

  rocksdb::Options options;
  options.create_if_missing = false;
  std::unique_ptr<rocksdb::DB> db;
  Status s = rocksdb::DB::OpenForReadOnly(options, db_path, &db);
  if (!s.ok()) return s;

  std::vector<rocksdb::LiveFileMetaData> live;
  db->GetLiveFilesMetaData(&live);

  std::map<uint32_t, StLiveSstLevelStatsRow> levels;
  std::set<uint64_t> seen;
  for (const auto& lf : live) {
    seen.insert(static_cast<uint64_t>(lf.file_number));
    StLiveSstLevelStatsRow& row = levels[static_cast<uint32_t>(lf.level)];
    row.level = static_cast<uint32_t>(lf.level);
    ++row.file_count;
    row.total_bytes += static_cast<uint64_t>(lf.size);
    ++row.total_entries;
    ++out->without_st_file_meta;
  }
  out->disk_sst_files = live.size();
  out->unique_live_files = seen.size();
  for (const auto& kv : levels) {
    out->per_level.push_back(kv.second);
  }
  return Status::OK();
}

#endif  // ROCKS_DEMO_SKIP_ROCKSDB_MANIFEST_LEVEL_STATS_IMPL

}  // namespace rocksdb

//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/compaction/compaction_picker_level.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "db/compaction/st_event_split_policy.h"
#include "db/version_edit.h"
#include "logging/log_buffer.h"
#include "table/st_meta_index_extension.h"
#include "test_util/sync_point.h"

namespace ROCKSDB_NAMESPACE {

namespace {

double GetCompactionScoreForLevel(VersionStorageInfo* vstorage, int level,
                                  int num_sorted_slots) {
  for (int i = 0; i < num_sorted_slots; ++i) {
    if (vstorage->CompactionScoreLevel(i) == level) {
      return vstorage->CompactionScore(i);
    }
  }
  return 0.0;
}

int StCompactionL0OutputLevel(const MutableCFOptions& mopts,
                              VersionStorageInfo* vstorage) {
  if (mopts.experimental_st_compaction) {
    return 1;
  }
  return vstorage->base_level();
}

}  // namespace

bool LevelCompactionPicker::NeedsCompaction(
    const VersionStorageInfo* vstorage) const {
  if (!vstorage->ExpiredTtlFiles().empty()) {
    return true;
  }
  if (!vstorage->FilesMarkedForPeriodicCompaction().empty()) {
    return true;
  }
  if (!vstorage->BottommostFilesMarkedForCompaction().empty()) {
    return true;
  }
  if (!vstorage->FilesMarkedForCompaction().empty()) {
    return true;
  }
  if (!vstorage->FilesMarkedForForcedBlobGC().empty()) {
    return true;
  }
  if (!vstorage->ReadTriggeredCompactionFiles().empty()) {
    return true;
  }
  for (int i = 0; i <= vstorage->MaxInputLevel(); i++) {
    if (vstorage->CompactionScore(i) >= 1) {
      return true;
    }
  }
  return false;
}

namespace {

enum class CompactToNextLevel {
  kNo,   // compact to the same level as the input file
  kYes,  // compact to the next level except the last level to the same level
  kSkipLastLevel,  // compact to the next level but skip the last level
};

// Per-level spatiotemporal envelope: computes the bounding box covering
// the given level's files for S_hybrid normalization. Uses per-level
// (not global) envelope to avoid normalization collapse: a multi-year
// global span would flatten all local time differences to near-zero,
// making DeltaS meaningless for neighbor-pair selection.
struct GlobalEnvelope {
  double t_min = 0, t_max = 1;
  double x_min = 0, x_max = 1;
  double y_min = 0, y_max = 1;
  bool valid = false;
};

GlobalEnvelope ComputeLevelEnvelope(
    const std::vector<FileMetaData*>& files) {
  GlobalEnvelope e;
  double t_min = std::numeric_limits<double>::max();
  double t_max = std::numeric_limits<double>::lowest();
  double x_min = std::numeric_limits<double>::max();
  double x_max = std::numeric_limits<double>::lowest();
  double y_min = std::numeric_limits<double>::max();
  double y_max = std::numeric_limits<double>::lowest();
  bool any = false;
  for (const auto* f : files) {
    if (f == nullptr || !f->has_st_file_meta) continue;
    const auto& m = f->st_file_meta;
    t_min = std::min(t_min, static_cast<double>(m.t_min));
    t_max = std::max(t_max, static_cast<double>(m.t_max));
    x_min = std::min(x_min, static_cast<double>(m.x_min));
    x_max = std::max(x_max, static_cast<double>(m.x_max));
    y_min = std::min(y_min, static_cast<double>(m.y_min));
    y_max = std::max(y_max, static_cast<double>(m.y_max));
    any = true;
  }
  if (!any) return e;  // no ST meta at all
  constexpr double kEps = 1e-9;
  e.t_min = t_min;  e.t_max = t_max + kEps;
  e.x_min = x_min;  e.x_max = x_max + kEps;
  e.y_min = y_min;  e.y_max = y_max + kEps;
  e.valid = true;
  return e;
}

// One-dimensional ST projection: maps a file's (t_center, x_center, y_center)
// into a scalar S ∈ [0,1] using the global envelope for normalization.
//   S = 0.50 * t_norm + 0.25 * x_norm + 0.25 * y_norm
// Returns -1 if the file has no ST meta.
double ComputeSHybrid(const FileMetaData* f, const GlobalEnvelope& env) {
  if (!f->has_st_file_meta || !env.valid) return -1.0;
  const auto& m = f->st_file_meta;
  const double t_c = (static_cast<double>(m.t_min) + m.t_max) * 0.5;
  const double x_c = (static_cast<double>(m.x_min) + m.x_max) * 0.5;
  const double y_c = (static_cast<double>(m.y_min) + m.y_max) * 0.5;
  const double t_n = (t_c - env.t_min) / std::max(env.t_max - env.t_min, 1e-9);
  const double x_n = (x_c - env.x_min) / std::max(env.x_max - env.x_min, 1e-9);
  const double y_n = (y_c - env.y_min) / std::max(env.y_max - env.y_min, 1e-9);
  return 0.50 * t_n + 0.25 * x_n + 0.25 * y_n;
}

// Find the best pair of neighboring files to merge by scanning the level's
// files sorted by S_hybrid. Returns the pair with the smallest ΔS that is
// below the threshold, or empty if none qualify.
//   ΔS_threshold: below this value, files are "close enough" to merge.
constexpr double kDeltaSThreshold = 0.01;

void FindBestNeighborPair(const std::vector<FileMetaData*>& files,
                           const GlobalEnvelope& env,
                           std::vector<FileMetaData*>* out_pair) {
  out_pair->clear();
  // Collect files with valid S_hybrid.
  struct Scored {
    FileMetaData* file;
    double s;
  };
  std::vector<Scored> scored;
  scored.reserve(files.size());
  for (auto* f : files) {
    if (f == nullptr || f->being_compacted) continue;
    double s = ComputeSHybrid(f, env);
    if (s < 0.0) continue;  // no ST meta, skip
    scored.push_back({f, s});
  }
  if (scored.size() < 2) return;

  // Sort by S_hybrid ascending.
  std::sort(scored.begin(), scored.end(),
            [](const Scored& a, const Scored& b) { return a.s < b.s; });

  // Find the pair with smallest ΔS below threshold.
  size_t best_i = 0;
  double best_ds = std::numeric_limits<double>::max();
  for (size_t i = 0; i < scored.size() - 1; ++i) {
    double ds = scored[i + 1].s - scored[i].s;
    if (ds < best_ds) {
      best_ds = ds;
      best_i = i;
    }
  }
  if (best_ds < kDeltaSThreshold) {
    out_pair->push_back(scored[best_i].file);
    out_pair->push_back(scored[best_i + 1].file);
  }
}

double ComputeMbrArea(const SpatioTemporalBlockMeta& m);

// Check if a file is "bloated": large MBR area but low data density.
// Returns true if the file should be pushed down (trigger B).
double ComputeMbrArea(const SpatioTemporalBlockMeta& m) {
  const double w = std::max(0.0, static_cast<double>(m.x_max) -
                                     static_cast<double>(m.x_min));
  const double h = std::max(0.0, static_cast<double>(m.y_max) -
                                     static_cast<double>(m.y_min));
  return w * h;
}

SpatioTemporalBlockMeta UnionMbr(const SpatioTemporalBlockMeta& a,
                                const SpatioTemporalBlockMeta& b) {
  SpatioTemporalBlockMeta out = a;
  out.x_min = std::min(out.x_min, b.x_min);
  out.x_max = std::max(out.x_max, b.x_max);
  out.y_min = std::min(out.y_min, b.y_min);
  out.y_max = std::max(out.y_max, b.y_max);
  out.t_min = std::min(out.t_min, b.t_min);
  out.t_max = std::max(out.t_max, b.t_max);
  // bitmap_words not used for picker scoring; ignore.
  return out;
}

bool MbrIntersects(const SpatioTemporalBlockMeta& a,
                    const SpatioTemporalBlockMeta& b) {
  return !(a.x_max < b.x_min || a.x_min > b.x_max || a.y_max < b.y_min ||
           a.y_min > b.y_max);
}

double MbrIou(const SpatioTemporalBlockMeta& a,
               const SpatioTemporalBlockMeta& b) {
  const float ix_min = std::max(a.x_min, b.x_min);
  const float ix_max = std::min(a.x_max, b.x_max);
  const float iy_min = std::max(a.y_min, b.y_min);
  const float iy_max = std::min(a.y_max, b.y_max);
  const double iw = std::max(0.0, static_cast<double>(ix_max - ix_min));
  const double ih = std::max(0.0, static_cast<double>(iy_max - iy_min));
  const double inter = iw * ih;
  if (inter <= 0.0) {
    return 0.0;
  }
  const double area_a = std::max(ComputeMbrArea(a), 1e-12);
  const double area_b = std::max(ComputeMbrArea(b), 1e-12);
  const double uni = area_a + area_b - inter;
  return uni > 0.0 ? (inter / uni) : 0.0;
}

double EdgeDistance(const SpatioTemporalBlockMeta& a,
                     const SpatioTemporalBlockMeta& b) {
  const double dx = (a.x_max < b.x_min)
                         ? (static_cast<double>(b.x_min) - a.x_max)
                         : (a.x_min > b.x_max)
                               ? (static_cast<double>(a.x_min) - b.x_max)
                               : 0.0;
  const double dy = (a.y_max < b.y_min)
                         ? (static_cast<double>(b.y_min) - a.y_max)
                         : (a.y_min > b.y_max)
                               ? (static_cast<double>(a.y_min) - b.y_max)
                               : 0.0;
  return std::sqrt(dx * dx + dy * dy);
}

double TimeGapNormalized(const SpatioTemporalBlockMeta& a,
                          const SpatioTemporalBlockMeta& b,
                          uint32_t time_norm) {
  const uint32_t lo = std::min(a.t_min, a.t_max);
  const uint32_t hi = std::max(a.t_min, a.t_max);
  const uint32_t blo = std::min(b.t_min, b.t_max);
  const uint32_t bhi = std::max(b.t_min, b.t_max);
  uint32_t gap = 0;
  if (hi < blo) {
    gap = blo - hi;
  } else if (bhi < lo) {
    gap = lo - bhi;
  } else {
    gap = 0;
  }
  const double denom = time_norm > 0 ? static_cast<double>(time_norm) : 21600.0;
  return gap == 0 ? 0.0 : (static_cast<double>(gap) / denom);
}

double SpatialProximityFineScore(
    const FileMetaData* cand, const FileMetaData* overlap,
    uint32_t time_norm) {
  if (cand == nullptr || overlap == nullptr || !cand->has_st_file_meta ||
      !overlap->has_st_file_meta) {
    return std::numeric_limits<double>::infinity();
  }
  const auto& cm = cand->st_file_meta;
  const auto& om = overlap->st_file_meta;

  // Mind.md recommended weights.
  constexpr double wt = 0.35;
  constexpr double ws = 0.35;
  constexpr double we = 0.20;
  constexpr double wb = 0.10;

  const double d_t = TimeGapNormalized(cm, om, time_norm);

  const double iou = MbrIou(cm, om);
  double d_s = 0.0;
  if (iou > 0.0) {
    d_s = 1.0 - iou;
  } else {
    const double d_edge = EdgeDistance(cm, om);
    const double base = std::sqrt(std::max(ComputeMbrArea(cm), 1e-12));
    d_s = 1.0 + d_edge / std::max(base, 1e-6);
  }

  const SpatioTemporalBlockMeta merged = UnionMbr(cm, om);
  const double expansion =
      (ComputeMbrArea(merged) - ComputeMbrArea(cm)) /
      std::max(ComputeMbrArea(cm), 1e-12);

  const double cand_size = static_cast<double>(cand->fd.GetFileSize());
  const double overlap_size =
      static_cast<double>(overlap->fd.GetFileSize());
  const double avg = std::max((cand_size + overlap_size) * 0.5, 1.0);
  const double imbalance = std::abs(cand_size - overlap_size) / avg;

  const double score = wt * d_t + ws * d_s + we * expansion + wb * imbalance;
  return score;
}

double SpatialProximityCoarseEdgeThreshold(int out_level) {
  // Normalize edge distance by sqrt(area_base) inside the picker.
  if (out_level <= 1) {
    return 0.10;
  }
  if (out_level <= 3) {
    return 0.15;
  }
  return 0.25;
}

// Time IoU: intersection(t_A, t_B) / union(t_A, t_B).
// Returns 0 when time ranges are completely disjoint.
double IouTime(const SpatioTemporalBlockMeta& a,
               const SpatioTemporalBlockMeta& b) {
  const uint32_t a_lo = std::min(a.t_min, a.t_max);
  const uint32_t a_hi = std::max(a.t_min, a.t_max);
  const uint32_t b_lo = std::min(b.t_min, b.t_max);
  const uint32_t b_hi = std::max(b.t_min, b.t_max);
  const uint32_t i_lo = std::max(a_lo, b_lo);
  const uint32_t i_hi = std::min(a_hi, b_hi);
  if (i_hi <= i_lo) return 0.0;
  const double inter = static_cast<double>(i_hi - i_lo);
  const double uni =
      static_cast<double>(std::max(a_hi, b_hi) - std::min(a_lo, b_lo));
  return uni > 0.0 ? (inter / uni) : 0.0;
}

// ST-aware merge efficiency score for two runs (files) in a tiered level.
// Higher score → more beneficial to merge per I/O byte.
//   efficiency = (alpha * IouTime + beta * IouSpatial) / log2(1 + total_bytes)
// If both time AND space are disjoint, returns 0 (skip — no pruning benefit).
double MergePriorityScore(const FileMetaData* a, const FileMetaData* b) {
  if (a == nullptr || b == nullptr ||
      !a->has_st_file_meta || !b->has_st_file_meta) {
    return 0.01;  // no ST meta → low baseline, eligible but deprioritized
  }
  const auto& ma = a->st_file_meta;
  const auto& mb = b->st_file_meta;

  constexpr double alpha = 0.50;  // time overlap
  constexpr double beta  = 0.50;  // spatial overlap

  const double iou_t = IouTime(ma, mb);
  const double iou_s = MbrIou(ma, mb);

  // If no overlap in either dimension, merging has no pruning benefit.
  if (iou_t <= 0.0 && iou_s <= 0.0) return 0.0;

  // Size efficiency: prefer merging small files (higher score per I/O byte).
  const double sz_a = static_cast<double>(std::max<uint64_t>(a->fd.GetFileSize(), 1));
  const double sz_b = static_cast<double>(std::max<uint64_t>(b->fd.GetFileSize(), 1));
  const double total_mb = (sz_a + sz_b) / (1024.0 * 1024.0);
  const double size_den = std::log2(1.0 + total_mb);

  return (alpha * iou_t + beta * iou_s) / std::max(size_den, 1e-6);
}

// Select a batch of overlapping files to merge from a level with too many
// runs. Strategy: find the "center" file with the highest total merge
// benefit (sum of scores with all other files), then greedily add its best
// neighbors until enough runs are eliminated or no more good candidates.
// Merging a batch in one job reduces compaction job count vs pair-by-pair.
void SelectBestMergeBatch(const std::vector<FileMetaData*>& level_files,
                          int max_runs,
                          std::vector<FileMetaData*>* out_selected) {
  out_selected->clear();
  const size_t n = level_files.size();
  if (n < 2) return;

  // Count active (non-compacting) files.
  int active = 0;
  for (size_t i = 0; i < n; ++i) {
    if (level_files[i] != nullptr && !level_files[i]->being_compacted)
      ++active;
  }
  if (active <= max_runs) return;  // no merge needed

  // Compute total merge benefit per file (sum of scores with all others).
  struct Candidate {
    size_t index;
    double total_score;
    int    neighbor_count;  // files with score > 0
  };
  std::vector<Candidate> cands;
  cands.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    if (level_files[i] == nullptr || level_files[i]->being_compacted)
      continue;
    double sum = 0.0;
    int neighbors = 0;
    for (size_t j = 0; j < n; ++j) {
      if (i == j) continue;
      if (level_files[j] == nullptr || level_files[j]->being_compacted)
        continue;
      const double s = MergePriorityScore(level_files[i], level_files[j]);
      sum += s;
      if (s > 0.0) ++neighbors;
    }
    cands.push_back({i, sum, neighbors});
  }
  if (cands.empty()) return;

  // Pick the center: file with highest total_score (most overlap benefit).
  std::sort(cands.begin(), cands.end(),
            [](const Candidate& a, const Candidate& b) {
              return a.total_score > b.total_score;  // descending
            });

  // Build a merge batch around the center.  Include the center plus
  // enough neighbors to bring the run count down to max_runs.
  const int need = active - max_runs;  // runs to eliminate
  const size_t center = cands[0].index;
  out_selected->push_back(
      const_cast<FileMetaData*>(level_files[center]));

  // Score all files against the center, sort by score descending.
  std::vector<std::pair<double, size_t>> by_score;
  for (size_t j = 0; j < n; ++j) {
    if (j == center) continue;
    if (level_files[j] == nullptr || level_files[j]->being_compacted)
      continue;
    const double s = MergePriorityScore(level_files[center], level_files[j]);
    if (s > 0.0) {
      by_score.push_back({s, j});
    }
  }
  std::sort(by_score.begin(), by_score.end(),
            [](const auto& a, const auto& b) {
              return a.first > b.first;  // highest score first
            });

  // Add neighbors until we eliminate enough runs.
  // Each added file merges one extra run into the batch (eliminating it).
  int added = 0;
  for (const auto& p : by_score) {
    if (added >= need) break;
    out_selected->push_back(
        const_cast<FileMetaData*>(level_files[p.second]));
    ++added;
  }

  // If we still haven't met the target and there are files without ST meta
  // (score = 0.01), add those as a last resort.
  if (added < need) {
    for (size_t j = 0; j < n && added < need; ++j) {
      if (j == center) continue;
      if (level_files[j] == nullptr || level_files[j]->being_compacted)
        continue;
      // Check if already selected
      bool already = false;
      for (auto* f : *out_selected) {
        if (f == level_files[j]) { already = true; break; }
      }
      if (already) continue;
      const double s = MergePriorityScore(level_files[center],
                                           level_files[j]);
      if (s <= 0.0) {  // no-ST-meta files get 0.01
        out_selected->push_back(
            const_cast<FileMetaData*>(level_files[j]));
        ++added;
      }
    }
  }

  // If batch is still just 1 file (no neighbors), add at least one more.
  if (out_selected->size() < 2 && by_score.empty() && n >= 2) {
    for (size_t j = 0; j < n; ++j) {
      if (j != center && level_files[j] != nullptr &&
          !level_files[j]->being_compacted) {
        out_selected->push_back(
            const_cast<FileMetaData*>(level_files[j]));
        break;
      }
    }
  }
}

double ComputeMbrArea(const SpatioTemporalBlockMeta& m);
double EdgeDistance(const SpatioTemporalBlockMeta& a,
                    const SpatioTemporalBlockMeta& b);
double SpatialProximityFineScore(
    const FileMetaData* cand, const FileMetaData* overlap,
    uint32_t time_norm);
double SpatialProximityCoarseEdgeThreshold(int out_level);

// A class to build a leveled compaction step-by-step.
class LevelCompactionBuilder {
 public:
  LevelCompactionBuilder(const std::string& cf_name,
                         VersionStorageInfo* vstorage,
                         CompactionPicker* compaction_picker,
                         LogBuffer* log_buffer,
                         const MutableCFOptions& mutable_cf_options,
                         const ImmutableOptions& ioptions,
                         const MutableDBOptions& mutable_db_options,
                         const std::string& full_history_ts_low)
      : cf_name_(cf_name),
        vstorage_(vstorage),
        compaction_picker_(compaction_picker),
        log_buffer_(log_buffer),
        mutable_cf_options_(mutable_cf_options),
        ioptions_(ioptions),
        mutable_db_options_(mutable_db_options),
        full_history_ts_low_(full_history_ts_low) {}

  // Pick and return a compaction.
  Compaction* PickCompaction();

  // Pick the initial files to compact to the next level. (or together
  // in Intra-L0 compactions)
  void SetupInitialFiles();

  // If the initial files are from L0 level, pick other L0
  // files if needed.
  bool SetupOtherL0FilesIfNeeded();

  // Compaction with round-robin compaction priority allows more files to be
  // picked to form a large compaction
  void SetupOtherFilesWithRoundRobinExpansion();
  // Based on initial files, setup other files need to be compacted
  // in this compaction, accordingly.
  bool SetupOtherInputsIfNeeded();

  Compaction* GetCompaction();

  // From `start_level_`, pick files to compact to `output_level_`.
  // Returns false if there is no file to compact.
  // If it returns true, inputs->files.size() will be exactly one for
  // all compaction priorities except round-robin. For round-robin,
  // multiple consecutive files may be put into inputs->files.
  // If level is 0 and there is already a compaction on that level, this
  // function will return false.
  bool PickFileToCompact();

  // Return true if a L0 trivial move is picked up.
  bool TryPickL0TrivialMove();

  // For L0->L0, picks the longest span of files that aren't currently
  // undergoing compaction for which work-per-deleted-file decreases. The span
  // always starts from the newest L0 file.
  //
  // Intra-L0 compaction is independent of all other files, so it can be
  // performed even when L0->base_level compactions are blocked.
  //
  // Returns true if `inputs` is populated with a span of files to be compacted;
  // otherwise, returns false.
  bool PickIntraL0Compaction();

  // When total L0 size is small compared to Lbase, try to pick intra-L0
  // compaction starting from the newest L0 file. This helps to prevent
  // L0->Lbase compaction with large write-amp.
  //
  // Returns true iff an intra-L0 compaction is picked.
  // `start_level_inputs_` and `output_level_` will be updated accordingly if
  // a compaction is picked.
  bool PickSizeBasedIntraL0Compaction();

  // ST-compaction (Mind.md): all non-compacting SSTs on start_level_ (>0).
  bool TryPickWholeLevelNonL0ST();

  // Return true if TrivialMove is extended. `start_index` is the index of
  // the initial file picked, which should already be in `start_level_inputs_`.
  bool TryExtendNonL0TrivialMove(int start_index,
                                 bool only_expand_right = false);

  // Picks a file from level_files to compact.
  // level_files is a vector of (level, file metadata) in ascending order of
  // level. If compact_to_next_level is true, compact the file to the next
  // level, otherwise, compact to the same level as the input file.
  // If skip_last_level is true, skip the last level.
  void PickFileToCompact(
      const autovector<std::pair<int, FileMetaData*>>& level_files,
      CompactToNextLevel compact_to_next_level);

  const std::string& cf_name_;
  VersionStorageInfo* vstorage_;
  CompactionPicker* compaction_picker_;
  LogBuffer* log_buffer_;
  int start_level_ = -1;
  int output_level_ = -1;
  int parent_index_ = -1;
  int base_index_ = -1;
  double start_level_score_ = 0;
  bool is_l0_trivial_move_ = false;
  CompactionInputFiles start_level_inputs_;
  std::vector<CompactionInputFiles> compaction_inputs_;
  CompactionInputFiles output_level_inputs_;
  std::vector<FileMetaData*> grandparents_;
  CompactionReason compaction_reason_ = CompactionReason::kUnknown;

  const MutableCFOptions& mutable_cf_options_;
  const ImmutableOptions& ioptions_;
  const MutableDBOptions& mutable_db_options_;
  const std::string& full_history_ts_low_;
  // Pick a path ID to place a newly generated file, with its level
  static uint32_t GetPathId(const ImmutableCFOptions& ioptions,
                            const MutableCFOptions& mutable_cf_options,
                            int level);

  static const int kMinFilesForIntraL0Compaction = 4;
};

void LevelCompactionBuilder::PickFileToCompact(
    const autovector<std::pair<int, FileMetaData*>>& level_files,
    CompactToNextLevel compact_to_next_level) {
  // Optional ST-aware preference: if multiple candidates exist for the same
  // start/output level, prefer the one that is spatially closer to the
  // output-level overlaps (Mind.md coarse+fine heuristic).
  int preferred_idx = -1;
  double preferred_score = std::numeric_limits<double>::infinity();
  if (!level_files.empty()) {
    for (size_t i = 0; i < level_files.size(); ++i) {
      const int start_level = level_files[i].first;
      FileMetaData* f = level_files[i].second;
      if (f == nullptr || !f->has_st_file_meta) {
        continue;
      }
      int out_level = start_level;
      if (compact_to_next_level != CompactToNextLevel::kNo &&
          (start_level < vstorage_->num_non_empty_levels() - 1)) {
        out_level = (start_level == 0)
                        ? StCompactionL0OutputLevel(mutable_cf_options_,
                                                      vstorage_)
                        : start_level + 1;
      }
      if (out_level <= 0 || out_level >= vstorage_->num_levels()) {
        continue;
      }
      // Overlapping output-level files by key range (existing API), then
      // coarse+fine spatial proximity scoring based on file-level ST meta.
      std::vector<FileMetaData*> overlaps;
      vstorage_->GetOverlappingInputs(out_level, &f->smallest, &f->largest, &overlaps);
      if (overlaps.empty()) {
        continue;
      }

      const auto& fm = f->st_file_meta;
      const double base_edge = std::sqrt(std::max(ComputeMbrArea(fm), 1e-12));
      const double coarse_th = SpatialProximityCoarseEdgeThreshold(out_level);
      const uint32_t base_time_norm =
          mutable_cf_options_.experimental_compaction_event_time_max_span;
      const uint32_t time_norm = std::max<uint32_t>(
          1, ComputeStEventSplitLevelPolicy(out_level, base_time_norm)
                 .effective_max_time_span);

      double best_score = std::numeric_limits<double>::infinity();
      bool coarse_any = false;

      for (FileMetaData* g : overlaps) {
        if (g == nullptr || !g->has_st_file_meta) {
          continue;
        }
        const auto& gm = g->st_file_meta;

        bool coarse_ok = false;
        if (MbrIntersects(fm, gm)) {
          coarse_ok = true;
        } else {
          const double d_edge = EdgeDistance(fm, gm);
          const double d_norm = d_edge / std::max(base_edge, 1e-6);
          coarse_ok = (d_norm <= coarse_th);
        }
        if (!coarse_ok) {
          continue;
        }
        coarse_any = true;
        best_score = std::min(best_score, SpatialProximityFineScore(f, g, time_norm));
      }

      if (coarse_any && best_score < preferred_score) {
        preferred_score = best_score;
        preferred_idx = static_cast<int>(i);
      }
    }
  }

  auto try_pick_at = [&](size_t idx) -> bool {
    auto level_file = level_files[idx];
    // If it's being compacted it has nothing to do here.
    assert(!level_file.second->being_compacted);
    start_level_ = level_file.first;
    if ((compact_to_next_level == CompactToNextLevel::kSkipLastLevel &&
         start_level_ == vstorage_->num_non_empty_levels() - 1) ||
        (start_level_ == 0 &&
         !compaction_picker_->level0_compactions_in_progress()->empty())) {
      return false;
    }

    if (compact_to_next_level != CompactToNextLevel::kNo &&
        (start_level_ < vstorage_->num_non_empty_levels() - 1)) {
      output_level_ =
          (start_level_ == 0)
              ? StCompactionL0OutputLevel(mutable_cf_options_, vstorage_)
              : start_level_ + 1;
    } else {
      output_level_ = start_level_;
    }
    start_level_inputs_.files = {level_file.second};
    start_level_inputs_.level = start_level_;
    if (compaction_picker_->ExpandInputsToCleanCut(cf_name_, vstorage_,
                                                   &start_level_inputs_)) {
      return true;
    }
    return false;
  };

  if (preferred_idx >= 0) {
    if (try_pick_at(static_cast<size_t>(preferred_idx))) {
      return;
    }
    // Fall through to default order if preferred choice cannot form a clean cut.
    start_level_inputs_.files.clear();
  }

  for (auto& level_file : level_files) {
    // If it's being compacted it has nothing to do here.
    // If this assert() fails that means that some function marked some
    // files as being_compacted, but didn't call ComputeCompactionScore()
    assert(!level_file.second->being_compacted);
    start_level_ = level_file.first;
    if ((compact_to_next_level == CompactToNextLevel::kSkipLastLevel &&
         start_level_ == vstorage_->num_non_empty_levels() - 1) ||
        (start_level_ == 0 &&
         !compaction_picker_->level0_compactions_in_progress()->empty())) {
      continue;
    }

    // Compact to the next level only if the file is not in the last level and
    // compact_to_next_level is kYes or kSkipLastLevel.
    if (compact_to_next_level != CompactToNextLevel::kNo &&
        (start_level_ < vstorage_->num_non_empty_levels() - 1)) {
      output_level_ =
          (start_level_ == 0)
              ? StCompactionL0OutputLevel(mutable_cf_options_, vstorage_)
              : start_level_ + 1;
    } else {
      output_level_ = start_level_;
    }
    start_level_inputs_.files = {level_file.second};
    start_level_inputs_.level = start_level_;
    if (compaction_picker_->ExpandInputsToCleanCut(cf_name_, vstorage_,
                                                   &start_level_inputs_)) {
      return;
    }
  }
  start_level_inputs_.files.clear();
}

// Write-pressure signal for adaptive compaction scope.
// Uses L0 file count as a direct proxy: L0 files are produced by flushes,
// so L0 backlog == how much write pressure the system is under.
enum class WritePressure { IDLE, NORMAL, HIGH };

WritePressure ComputeWritePressure(const VersionStorageInfo* vstorage,
                                    int l0_compaction_trigger) {
  int l0_count = static_cast<int>(vstorage->LevelFiles(0).size());
  if (l0_count == 0) return WritePressure::IDLE;
  if (l0_compaction_trigger > 0 &&
      l0_count >= 2 * l0_compaction_trigger)
    return WritePressure::HIGH;
  return WritePressure::NORMAL;
}

int FindSTRunCountLevel(const VersionStorageInfo* vstorage,
                       const MutableCFOptions& options,
                       const ImmutableOptions& ioptions) {
  const int max_runs =
      options.experimental_st_compaction_tiered_max_runs_per_level;
  if (!options.experimental_st_compaction_l1_l2_tiered ||
      !options.experimental_st_compaction_real_tiered_l1_l2 || max_runs <= 0 ||
      ComputeWritePressure(vstorage,
                           options.level0_file_num_compaction_trigger) ==
          WritePressure::HIGH) {
    return -1;
  }
  const int min_level =
      std::max(1, options.experimental_st_compaction_tiered_min_level);
  int max_level = std::min(
      vstorage->MaxInputLevel(),
      vstorage->MaxOutputLevel(ioptions.cf_allow_ingest_behind ||
                              ioptions.allow_ingest_behind) - 1);
  if (options.experimental_st_compaction_tiered_max_level > 0) {
    max_level = std::min(
        max_level, options.experimental_st_compaction_tiered_max_level);
  }
  for (int level = min_level; level <= max_level; ++level) {
    int active_runs = 0;
    for (const auto* file : vstorage->LevelFiles(level)) {
      if (file != nullptr && !file->being_compacted && ++active_runs > max_runs) {
        return level;
      }
    }
  }
  return -1;
}

void LevelCompactionBuilder::SetupInitialFiles() {
  // Find the compactions by size on all levels.
  bool skipped_l0_to_base = false;
  const int num_score_slots = compaction_picker_->NumberLevels() - 1;

  // ST-compaction: if L0 still needs work but a deeper level sorts first by
  // score, prefer L0 before considering deeper levels.
  if (mutable_cf_options_.experimental_st_compaction &&
      ioptions_.compaction_style == kCompactionStyleLevel &&
      num_score_slots > 0) {
    const double l0_score =
        GetCompactionScoreForLevel(vstorage_, 0, num_score_slots);
    if (l0_score >= 1.0 && vstorage_->CompactionScoreLevel(0) != 0) {
      start_level_ = 0;
      start_level_score_ = l0_score;
      output_level_ =
          StCompactionL0OutputLevel(mutable_cf_options_, vstorage_);
      if (PickFileToCompact()) {
        compaction_reason_ = CompactionReason::kLevelL0FilesNum;
        return;
      }
      start_level_inputs_.clear();
      skipped_l0_to_base = true;
      if (PickIntraL0Compaction()) {
        output_level_ = 0;
        compaction_reason_ = CompactionReason::kLevelL0FilesNum;
        return;
      }
    }
  }

  // Write-pressure adaptive threshold: when idle, proactively compact
  // at a lower threshold. When under high write pressure, skip non-L0
  // compaction entirely — let L0 backlog clear first.
  const WritePressure wp = ComputeWritePressure(
      vstorage_,
      mutable_cf_options_.level0_file_num_compaction_trigger);
  constexpr double kIdleScoreThreshold = 0.7;
  const double effective_threshold =
      (wp == WritePressure::IDLE) ? kIdleScoreThreshold : 1.0;

  // ── STFR Trigger A: intra-level neighbor merge (ALL non-L0 levels) ──
  // Scan ALL non-L0, non-last levels for files with small ΔS (close in
  // spatiotemporal position). Merge them in-place (output = start_level)
  // to reduce run count without pushing data deeper.
  if (start_level_inputs_.empty() &&
      wp != WritePressure::HIGH &&
      mutable_cf_options_.experimental_st_compaction_l1_l2_tiered &&
      mutable_cf_options_.experimental_st_compaction_real_tiered_l1_l2 &&
      mutable_cf_options_.experimental_st_compaction_stfp_enable) {
    // Trigger A uses per-level envelope (not global) to avoid
    // normalization collapse: a 20-year global span would flatten all
    // local time differences to near-zero, making DeltaS meaningless.
    for (int lvl = 1; lvl < vstorage_->num_levels() - 1; ++lvl) {
      const auto& files = vstorage_->LevelFiles(lvl);
      if (files.size() < 2) continue;
      GlobalEnvelope env = ComputeLevelEnvelope(files);
      if (!env.valid) continue;
      std::vector<FileMetaData*> pair;
      FindBestNeighborPair(files, env, &pair);
      if (pair.size() >= 2) {
        start_level_inputs_.level = lvl;
        start_level_inputs_.files = std::move(pair);
        // Intra-level: merge neighbors back into the same level.
        output_level_ = lvl;
        start_level_ = lvl;
        compaction_reason_ = CompactionReason::kLevelMaxLevelSize;
        break;
      }
    }
  }

  // ── STFR Trigger B: bloated file push-down (IDLE only, ALL non-L0 levels) ──
  if (!start_level_inputs_.empty()) {
    return;
  }

  for (int i = 0; i < num_score_slots; i++) {
    start_level_score_ = vstorage_->CompactionScore(i);
    start_level_ = vstorage_->CompactionScoreLevel(i);
    assert(i == 0 || start_level_score_ <= vstorage_->CompactionScore(i - 1));

    // HIGH write pressure: skip non-L0 compaction (only L0→L1 allowed).
    if (wp == WritePressure::HIGH && start_level_ > 0) continue;

    if (start_level_score_ >= effective_threshold) {
      const int l0_dest_level =
          StCompactionL0OutputLevel(mutable_cf_options_, vstorage_);
      if (skipped_l0_to_base && start_level_ == l0_dest_level) {
        // If L0->dest compaction is pending, don't schedule further compaction
        // from the destination level. Otherwise L0->dest may starve.
        continue;
      }
      if (mutable_cf_options_.experimental_st_compaction &&
          ioptions_.compaction_style == kCompactionStyleLevel &&
          start_level_ > 0) {
        const int max_out = vstorage_->MaxOutputLevel(
            ioptions_.cf_allow_ingest_behind || ioptions_.allow_ingest_behind);
        output_level_ =
            (start_level_ >= max_out) ? start_level_ : start_level_ + 1;
      } else {
        output_level_ = (start_level_ == 0)
                            ? StCompactionL0OutputLevel(mutable_cf_options_,
                                                        vstorage_)
                            : start_level_ + 1;
      }
      bool picked_file_to_compact = false;
      if (mutable_cf_options_.experimental_st_compaction &&
          ioptions_.compaction_style == kCompactionStyleLevel &&
          start_level_ > 0) {
        picked_file_to_compact = TryPickWholeLevelNonL0ST();
      } else {
        picked_file_to_compact = PickFileToCompact();
      }
      TEST_SYNC_POINT_CALLBACK("PostPickFileToCompact",
                               &picked_file_to_compact);
      if (picked_file_to_compact) {
        // found the compaction!
        if (start_level_ == 0) {
          // L0 score = `num L0 files` / `level0_file_num_compaction_trigger`
          compaction_reason_ = CompactionReason::kLevelL0FilesNum;
        } else {
          // L1+ score = `Level files size` / `MaxBytesForLevel`
          compaction_reason_ = CompactionReason::kLevelMaxLevelSize;
        }
        break;
      } else {
        // didn't find the compaction, clear the inputs
        start_level_inputs_.clear();
        if (start_level_ == 0) {
          skipped_l0_to_base = true;
          // L0->base_level may be blocked due to ongoing L0->base_level
          // compactions. It may also be blocked by an ongoing compaction from
          // base_level downwards.
          //
          // In these cases, to reduce L0 file count and thus reduce likelihood
          // of write stalls, we can attempt compacting a span of files within
          // L0.
          if (PickIntraL0Compaction()) {
            output_level_ = 0;
            compaction_reason_ = CompactionReason::kLevelL0FilesNum;
            break;
          }
        }
      }
    } else {
      // Compaction scores are sorted in descending order, no further scores
      // will be >= effective_threshold.
      break;
    }
  }
  // ST run-count trigger: when an ST-tiered level has more files (runs)
  // than tiered_max_runs_per_level, select the best pair of runs to merge
  // based on time+spatial overlap (ST-aware merge selection). The merged
  // output goes to the next level (standard L(n)→L(n+1) tiered compaction),
  // which reduces the run count in this level.
  // Skip under HIGH write pressure — write throughput takes priority.
  // Run-count trigger: applies to ALL real_tiered strategies.
  // STFP additionally enables Trigger A + B via stfp_enable.
  if (wp != WritePressure::HIGH &&
      start_level_inputs_.empty() &&
      mutable_cf_options_.experimental_st_compaction_l1_l2_tiered &&
      mutable_cf_options_.experimental_st_compaction_real_tiered_l1_l2) {
    const int max_runs =
        mutable_cf_options_.experimental_st_compaction_tiered_max_runs_per_level;
    if (max_runs > 0) {  // 0 = unlimited, never trigger
      const int lvl =
          FindSTRunCountLevel(vstorage_, mutable_cf_options_, ioptions_);
      if (lvl >= 0) {
        const auto& files = vstorage_->LevelFiles(lvl);
        // Select a batch of runs to merge (one compaction job).
        // NOTE: batch may span multiple calendar buckets. Cross-bucket
        // SST coexistence is by design — st_file_meta time windows
        // guarantee recall regardless of which bucket a segment lives in.
        std::vector<FileMetaData*> merge_batch;
        SelectBestMergeBatch(files, max_runs, &merge_batch);
        if (merge_batch.size() < 2) return;

        // Found a level with too many runs. Compact the selected batch
        // to the next level in one job (tiered: no output-level files
        // are involved — enforced in SetupOtherInputsIfNeeded).
        start_level_inputs_.level = lvl;
        start_level_inputs_.files = std::move(merge_batch);
        const int max_out = vstorage_->MaxOutputLevel(
            ioptions_.cf_allow_ingest_behind || ioptions_.allow_ingest_behind);
        output_level_ = (lvl >= max_out) ? lvl : lvl + 1;
        start_level_ = lvl;
        compaction_reason_ = CompactionReason::kLevelMaxLevelSize;
      }
    }
  }
  if (!start_level_inputs_.empty()) {
    return;
  }

  // if we didn't find a compaction, check if there are any files marked for
  // compaction
  parent_index_ = base_index_ = -1;

  compaction_picker_->PickFilesMarkedForCompaction(
      cf_name_, vstorage_, &start_level_, &output_level_, &start_level_inputs_,
      /*skip_marked_file*/ [](const FileMetaData* /* file */) {
        return false;
      });
  if (!start_level_inputs_.empty()) {
    compaction_reason_ = CompactionReason::kFilesMarkedForCompaction;
    return;
  }

  // Bottommost Files Compaction on deleting tombstones
  PickFileToCompact(vstorage_->BottommostFilesMarkedForCompaction(),
                    CompactToNextLevel::kNo);
  if (!start_level_inputs_.empty()) {
    compaction_reason_ = CompactionReason::kBottommostFiles;
    return;
  }

  // TTL Compaction
  if (ioptions_.compaction_pri == kRoundRobin &&
      !vstorage_->ExpiredTtlFiles().empty()) {
    auto expired_files = vstorage_->ExpiredTtlFiles();
    // the expired files list should already be sorted by level
    start_level_ = expired_files.front().first;
#ifndef NDEBUG
    for (const auto& file : expired_files) {
      assert(start_level_ <= file.first);
    }
#endif
    if (start_level_ > 0) {
      output_level_ = start_level_ + 1;
      if (PickFileToCompact()) {
        compaction_reason_ = CompactionReason::kRoundRobinTtl;
        return;
      }
    }
  }

  PickFileToCompact(vstorage_->ExpiredTtlFiles(),
                    CompactToNextLevel::kSkipLastLevel);
  if (!start_level_inputs_.empty()) {
    compaction_reason_ = CompactionReason::kTtl;
    return;
  }

  // Periodic Compaction
  PickFileToCompact(vstorage_->FilesMarkedForPeriodicCompaction(),
                    ioptions_.level_compaction_dynamic_level_bytes
                        ? CompactToNextLevel::kYes
                        : CompactToNextLevel::kNo);
  if (!start_level_inputs_.empty()) {
    compaction_reason_ = CompactionReason::kPeriodicCompaction;
    return;
  }

  // Forced blob garbage collection
  PickFileToCompact(vstorage_->FilesMarkedForForcedBlobGC(),
                    CompactToNextLevel::kNo);
  if (!start_level_inputs_.empty()) {
    compaction_reason_ = CompactionReason::kForcedBlobGC;
    return;
  }

  // Read-triggered compaction
  PickFileToCompact(vstorage_->ReadTriggeredCompactionFiles(),
                    CompactToNextLevel::kYes);
  if (!start_level_inputs_.empty()) {
    compaction_reason_ = CompactionReason::kReadTriggered;
    return;
  }
}

bool LevelCompactionBuilder::SetupOtherL0FilesIfNeeded() {
  if (start_level_ == 0 && output_level_ != 0 && !is_l0_trivial_move_) {
    return compaction_picker_->GetOverlappingL0Files(
        vstorage_, &start_level_inputs_, output_level_, &parent_index_);
  }
  return true;
}

void LevelCompactionBuilder::SetupOtherFilesWithRoundRobinExpansion() {
  // We only expand when the start level is not L0 under round robin
  assert(start_level_ >= 1);

  // For round-robin compaction priority, we have 3 constraints when picking
  // multiple files.
  // Constraint 1: We can only pick consecutive files
  //  -> Constraint 1a: When a file is being compacted (or some input files
  //                    are being compacted after expanding, we cannot
  //                    choose it and have to stop choosing more files
  //  -> Constraint 1b: When we reach the last file (with largest keys), we
  //                    cannot choose more files (the next file will be the
  //                    first one)
  // Constraint 2: We should ensure the total compaction bytes (including the
  //               overlapped files from the next level) is no more than
  //               mutable_cf_options_.max_compaction_bytes
  // Constraint 3: We try our best to pick as many files as possible so that
  //               the post-compaction level size is less than
  //               MaxBytesForLevel(start_level_)
  // Constraint 4: We do not expand if it is possible to apply a trivial move
  // Constraint 5 (TODO): Try to pick minimal files to split into the target
  //               number of subcompactions
  TEST_SYNC_POINT("LevelCompactionPicker::RoundRobin");

  // Only expand the inputs when we have selected a file in start_level_inputs_
  if (start_level_inputs_.size() == 0) {
    return;
  }

  uint64_t start_lvl_bytes_no_compacting = 0;
  uint64_t curr_bytes_to_compact = 0;
  uint64_t start_lvl_max_bytes_to_compact = 0;
  const std::vector<FileMetaData*>& level_files =
      vstorage_->LevelFiles(start_level_);
  // Constraint 3 (pre-calculate the ideal max bytes to compact)
  for (auto f : level_files) {
    if (!f->being_compacted) {
      start_lvl_bytes_no_compacting += f->fd.GetFileSize();
    }
  }
  if (start_lvl_bytes_no_compacting >
      vstorage_->MaxBytesForLevel(start_level_)) {
    start_lvl_max_bytes_to_compact = start_lvl_bytes_no_compacting -
                                     vstorage_->MaxBytesForLevel(start_level_);
  }

  size_t start_index = vstorage_->FilesByCompactionPri(start_level_)[0];
  InternalKey smallest, largest;
  // Constraint 4 (No need to check again later)
  compaction_picker_->GetRange(start_level_inputs_, &smallest, &largest);
  CompactionInputFiles output_level_inputs;
  output_level_inputs.level = output_level_;
  vstorage_->GetOverlappingInputs(output_level_, &smallest, &largest,
                                  &output_level_inputs.files);
  if (output_level_inputs.empty()) {
    if (TryExtendNonL0TrivialMove((int)start_index,
                                  true /* only_expand_right */)) {
      return;
    }
  }
  // Constraint 3
  if (start_level_inputs_[0]->fd.GetFileSize() >=
      start_lvl_max_bytes_to_compact) {
    return;
  }
  CompactionInputFiles tmp_start_level_inputs;
  tmp_start_level_inputs = start_level_inputs_;
  // TODO (zichen): Future parallel round-robin may also need to update this
  // Constraint 1b (only expand till the end)
  size_t next_index = start_index + 1;
  const FileMetaData* last_file = start_level_inputs_.files.back();
  for (size_t j = next_index; j < level_files.size(); j++) {
    if (level_files[j] == last_file) {
      next_index = j + 1;
      break;
    }
  }
  for (size_t i = next_index; i < level_files.size(); i++) {
    auto* f = level_files[i];
    if (f->being_compacted) {
      // Constraint 1a
      return;
    }

    tmp_start_level_inputs.files.push_back(f);
    if (!compaction_picker_->ExpandInputsToCleanCut(cf_name_, vstorage_,
                                                    &tmp_start_level_inputs) ||
        compaction_picker_->FilesRangeOverlapWithCompaction(
            {tmp_start_level_inputs}, output_level_,
            Compaction::EvaluateProximalLevel(vstorage_, mutable_cf_options_,
                                              ioptions_, start_level_,
                                              output_level_))) {
      // Constraint 1a
      tmp_start_level_inputs.clear();
      return;
    }

    curr_bytes_to_compact = 0;
    for (auto start_lvl_f : tmp_start_level_inputs.files) {
      curr_bytes_to_compact += start_lvl_f->fd.GetFileSize();
    }

    // Check whether any output level files are locked
    compaction_picker_->GetRange(tmp_start_level_inputs, &smallest, &largest);
    vstorage_->GetOverlappingInputs(output_level_, &smallest, &largest,
                                    &output_level_inputs.files);
    if (!output_level_inputs.empty() &&
        !compaction_picker_->ExpandInputsToCleanCut(cf_name_, vstorage_,
                                                    &output_level_inputs)) {
      // Constraint 1a
      tmp_start_level_inputs.clear();
      return;
    }

    uint64_t start_lvl_curr_bytes_to_compact = curr_bytes_to_compact;
    for (auto output_lvl_f : output_level_inputs.files) {
      curr_bytes_to_compact += output_lvl_f->fd.GetFileSize();
    }
    if (curr_bytes_to_compact > mutable_cf_options_.max_compaction_bytes) {
      // Constraint 2
      tmp_start_level_inputs.clear();
      return;
    }

    start_level_inputs_.files = tmp_start_level_inputs.files;
    // Constraint 3
    if (start_lvl_curr_bytes_to_compact > start_lvl_max_bytes_to_compact) {
      return;
    }
  }
}

bool LevelCompactionBuilder::SetupOtherInputsIfNeeded() {
  // Setup input files from output level. For output to L0, we only compact
  // spans of files that do not interact with any pending compactions, so don't
  // need to consider other levels.
  if (output_level_ != 0) {
    // Unified Soft-Tiered: when real_tiered_l1_l2 is enabled, skip
    // output-level involvement entirely. Output files are NOT read or
    // rewritten, reducing write amp. Multiple sorted runs per level are
    // allowed — trajectory data has no duplicate keys, so version
    // resolution is unnecessary. The old Adaptive hysteresis and
    // per-level tiered cutoff are removed; all levels >= tiered_min
    // use tiered uniformly.
    bool is_real_tiered = false;
    if (mutable_cf_options_.experimental_st_compaction_l1_l2_tiered &&
        mutable_cf_options_.experimental_st_compaction_real_tiered_l1_l2 &&
        start_level_ >= 1) {
      int tiered_min =
          mutable_cf_options_.experimental_st_compaction_tiered_min_level;
      if (tiered_min <= 0) tiered_min = 1;
      int tiered_max =
          mutable_cf_options_.experimental_st_compaction_tiered_max_level;
      is_real_tiered = (start_level_ >= tiered_min &&
                        (tiered_max <= 0 || start_level_ <= tiered_max));
    }

    output_level_inputs_.level = output_level_;
    bool round_robin_expanding =
        ioptions_.compaction_pri == kRoundRobin &&
        compaction_reason_ == CompactionReason::kLevelMaxLevelSize;
    if (round_robin_expanding) {
      SetupOtherFilesWithRoundRobinExpansion();
    }
    if (!is_real_tiered && !is_l0_trivial_move_ &&
        !compaction_picker_->SetupOtherInputs(
            cf_name_, mutable_cf_options_, vstorage_, &start_level_inputs_,
            &output_level_inputs_, &parent_index_, base_index_,
            round_robin_expanding)) {
      return false;
    }

    compaction_inputs_.push_back(start_level_inputs_);
    if (!output_level_inputs_.empty()) {
      compaction_inputs_.push_back(output_level_inputs_);
    }

    // In some edge cases we could pick a compaction that will be compacting
    // a key range that overlap with another running compaction, and both
    // of them have the same output level. This could happen if
    // (1) we are running a non-exclusive manual compaction
    // (2) AddFile ingest a new file into the LSM tree
    // We need to disallow this from happening.
    if (compaction_picker_->FilesRangeOverlapWithCompaction(
            compaction_inputs_, output_level_,
            Compaction::EvaluateProximalLevel(vstorage_, mutable_cf_options_,
                                              ioptions_, start_level_,
                                              output_level_))) {
      // This compaction output could potentially conflict with the output
      // of a currently running compaction, we cannot run it.
      return false;
    }
    if (!is_real_tiered && !is_l0_trivial_move_) {
      compaction_picker_->GetGrandparents(vstorage_, start_level_inputs_,
                                          output_level_inputs_, &grandparents_);
    }
  } else {
    compaction_inputs_.push_back(start_level_inputs_);
  }
  return true;
}

Compaction* LevelCompactionBuilder::PickCompaction() {
  // Pick up the first file to start compaction. It may have been extended
  // to a clean cut.
  SetupInitialFiles();
  if (start_level_inputs_.empty()) {
    return nullptr;
  }
  assert(start_level_ >= 0 && output_level_ >= 0);

  // If it is a L0 -> base level compaction, we need to set up other L0
  // files if needed.
  if (!SetupOtherL0FilesIfNeeded()) {
    return nullptr;
  }

  // Pick files in the output level and expand more files in the start level
  // if needed.
  if (!SetupOtherInputsIfNeeded()) {
    return nullptr;
  }

  // Form a compaction object containing the files we picked.
  Compaction* c = GetCompaction();

  TEST_SYNC_POINT_CALLBACK("LevelCompactionPicker::PickCompaction:Return", c);

  return c;
}

Compaction* LevelCompactionBuilder::GetCompaction() {
  // TryPickL0TrivialMove() does not apply to the case when compacting L0 to an
  // empty output level. So L0 files is picked in PickFileToCompact() by
  // compaction score. We may still be able to do trivial move when this file
  // does not overlap with other L0s. This happens when
  // compaction_inputs_[0].size() == 1 since SetupOtherL0FilesIfNeeded() did not
  // pull in more L0s.
  assert(!compaction_inputs_.empty());
  bool l0_files_might_overlap =
      start_level_ == 0 && !is_l0_trivial_move_ &&
      (compaction_inputs_.size() > 1 || compaction_inputs_[0].size() > 1);
  auto c = new Compaction(
      vstorage_, ioptions_, mutable_cf_options_, mutable_db_options_,
      std::move(compaction_inputs_), output_level_,
      MaxFileSizeForLevel(mutable_cf_options_, output_level_,
                          ioptions_.compaction_style, vstorage_->base_level(),
                          ioptions_.level_compaction_dynamic_level_bytes),
      mutable_cf_options_.max_compaction_bytes,
      GetPathId(ioptions_, mutable_cf_options_, output_level_),
      GetCompressionType(vstorage_, mutable_cf_options_, output_level_,
                         vstorage_->base_level()),
      GetCompressionOptions(mutable_cf_options_, vstorage_, output_level_),
      Temperature::kUnknown,
      /* max_subcompactions */ 0, std::move(grandparents_),
      /* earliest_snapshot */ std::nullopt, /* snapshot_checker */ nullptr,
      compaction_reason_,
      /* trim_ts */ "", start_level_score_, l0_files_might_overlap);

  // If it's level 0 compaction, make sure we don't execute any other level 0
  // compactions in parallel
  compaction_picker_->RegisterCompaction(c);

  // Creating a compaction influences the compaction score because the score
  // takes running compactions into account (by skipping files that are already
  // being compacted). Since we just changed compaction score, we recalculate it
  // here
  vstorage_->ComputeCompactionScore(ioptions_, mutable_cf_options_,
                                    full_history_ts_low_);
  return c;
}

/*
 * Find the optimal path to place a file
 * Given a level, finds the path where levels up to it will fit in levels
 * up to and including this path
 */
uint32_t LevelCompactionBuilder::GetPathId(
    const ImmutableCFOptions& ioptions,
    const MutableCFOptions& mutable_cf_options, int level) {
  uint32_t p = 0;
  assert(!ioptions.cf_paths.empty());

  // size remaining in the most recent path
  uint64_t current_path_size = ioptions.cf_paths[0].target_size;

  uint64_t level_size;
  int cur_level = 0;

  // max_bytes_for_level_base denotes L1 size.
  // We estimate L0 size to be the same as L1.
  level_size = mutable_cf_options.max_bytes_for_level_base;

  // Last path is the fallback
  while (p < ioptions.cf_paths.size() - 1) {
    if (level_size <= current_path_size) {
      if (cur_level == level) {
        // Does desired level fit in this path?
        return p;
      } else {
        current_path_size -= level_size;
        if (cur_level > 0) {
          if (ioptions.level_compaction_dynamic_level_bytes) {
            // Currently, level_compaction_dynamic_level_bytes is ignored when
            // multiple db paths are specified. https://github.com/facebook/
            // rocksdb/blob/main/db/column_family.cc.
            // Still, adding this check to avoid accidentally using
            // max_bytes_for_level_multiplier_additional
            level_size = static_cast<uint64_t>(
                level_size * mutable_cf_options.max_bytes_for_level_multiplier);
          } else {
            level_size = static_cast<uint64_t>(
                level_size * mutable_cf_options.max_bytes_for_level_multiplier *
                mutable_cf_options.MaxBytesMultiplerAdditional(cur_level));
          }
        }
        cur_level++;
        continue;
      }
    }
    p++;
    current_path_size = ioptions.cf_paths[p].target_size;
  }
  return p;
}

bool LevelCompactionBuilder::TryPickL0TrivialMove() {
  if (vstorage_->base_level() <= 0) {
    return false;
  }
  if (start_level_ == 0 && mutable_cf_options_.compression_per_level.empty() &&
      !vstorage_->LevelFiles(output_level_).empty() &&
      ioptions_.db_paths.size() <= 1) {
    // Try to pick trivial move from L0 to L1. We start from the oldest
    // file. We keep expanding to newer files if it would form a
    // trivial move.
    // For now we don't support it with
    // mutable_cf_options_.compression_per_level to prevent the logic
    // of determining whether L0 can be trivial moved to the next level.
    // We skip the case where output level is empty, since in this case, at
    // least the oldest file would qualify for trivial move, and this would
    // be a surprising behavior with few benefits.

    // We search from the oldest file from the newest. In theory, there are
    // files in the middle can form trivial move too, but it is probably
    // uncommon and we ignore these cases for simplicity.
    const std::vector<FileMetaData*>& level_files =
        vstorage_->LevelFiles(start_level_);

    InternalKey my_smallest, my_largest;
    for (auto it = level_files.rbegin(); it != level_files.rend(); ++it) {
      CompactionInputFiles output_level_inputs;
      output_level_inputs.level = output_level_;
      FileMetaData* file = *it;
      if (it == level_files.rbegin()) {
        my_smallest = file->smallest;
        my_largest = file->largest;
      } else {
        if (compaction_picker_->icmp()->Compare(file->largest, my_smallest) <
            0) {
          my_smallest = file->smallest;
        } else if (compaction_picker_->icmp()->Compare(file->smallest,
                                                       my_largest) > 0) {
          my_largest = file->largest;
        } else {
          break;
        }
      }
      vstorage_->GetOverlappingInputs(output_level_, &my_smallest, &my_largest,
                                      &output_level_inputs.files);
      if (output_level_inputs.empty()) {
        assert(!file->being_compacted);
        start_level_inputs_.files.push_back(file);
      } else {
        break;
      }
    }
  }

  if (!start_level_inputs_.empty()) {
    // Sort files by key range. Not sure it's 100% necessary but it's cleaner
    // to always keep files sorted by key the key ranges don't overlap.
    std::sort(start_level_inputs_.files.begin(),
              start_level_inputs_.files.end(),
              [icmp = compaction_picker_->icmp()](FileMetaData* f1,
                                                  FileMetaData* f2) -> bool {
                return (icmp->Compare(f1->smallest, f2->smallest) < 0);
              });

    is_l0_trivial_move_ = true;
    return true;
  }
  return false;
}

bool LevelCompactionBuilder::TryExtendNonL0TrivialMove(int start_index,
                                                       bool only_expand_right) {
  if (start_level_inputs_.size() == 1 &&
      (ioptions_.db_paths.empty() || ioptions_.db_paths.size() == 1) &&
      (mutable_cf_options_.compression_per_level.empty())) {
    // Only file of `index`, and it is likely a trivial move. Try to
    // expand if it is still a trivial move, but not beyond
    // max_compaction_bytes or 4 files, so that we don't create too
    // much compaction pressure for the next level.
    // Ignore if there are more than one DB path, as it would be hard
    // to predict whether it is a trivial move.
    const std::vector<FileMetaData*>& level_files =
        vstorage_->LevelFiles(start_level_);
    const size_t kMaxMultiTrivialMove = 4;
    FileMetaData* initial_file = start_level_inputs_.files[0];
    size_t total_size = initial_file->fd.GetFileSize();
    CompactionInputFiles output_level_inputs;
    output_level_inputs.level = output_level_;
    // Expand towards right
    for (int i = start_index + 1;
         i < static_cast<int>(level_files.size()) &&
         start_level_inputs_.size() < kMaxMultiTrivialMove;
         i++) {
      FileMetaData* next_file = level_files[i];
      if (next_file->being_compacted) {
        break;
      }
      vstorage_->GetOverlappingInputs(output_level_, &(initial_file->smallest),
                                      &(next_file->largest),
                                      &output_level_inputs.files);
      if (!output_level_inputs.empty()) {
        break;
      }
      if (i < static_cast<int>(level_files.size()) - 1 &&
          compaction_picker_->icmp()
                  ->user_comparator()
                  ->CompareWithoutTimestamp(
                      next_file->largest.user_key(),
                      level_files[i + 1]->smallest.user_key()) == 0) {
        TEST_SYNC_POINT_CALLBACK(
            "LevelCompactionBuilder::TryExtendNonL0TrivialMove:NoCleanCut",
            nullptr);
        // Not a clean up after adding the next file. Skip.
        break;
      }
      total_size += next_file->fd.GetFileSize();
      if (total_size > mutable_cf_options_.max_compaction_bytes) {
        break;
      }
      start_level_inputs_.files.push_back(next_file);
    }
    // Expand towards left
    if (!only_expand_right) {
      for (int i = start_index - 1;
           i >= 0 && start_level_inputs_.size() < kMaxMultiTrivialMove; i--) {
        FileMetaData* next_file = level_files[i];
        if (next_file->being_compacted) {
          break;
        }
        vstorage_->GetOverlappingInputs(output_level_, &(next_file->smallest),
                                        &(initial_file->largest),
                                        &output_level_inputs.files);
        if (!output_level_inputs.empty()) {
          break;
        }
        if (i > 0 && compaction_picker_->icmp()
                             ->user_comparator()
                             ->CompareWithoutTimestamp(
                                 next_file->smallest.user_key(),
                                 level_files[i - 1]->largest.user_key()) == 0) {
          // Not a clean up after adding the next file. Skip.
          break;
        }
        total_size += next_file->fd.GetFileSize();
        if (total_size > mutable_cf_options_.max_compaction_bytes) {
          break;
        }
        // keep `files` sorted in increasing order by key range
        start_level_inputs_.files.insert(start_level_inputs_.files.begin(),
                                         next_file);
      }
    }
    return start_level_inputs_.size() > 1;
  }
  return false;
}

bool LevelCompactionBuilder::PickFileToCompact() {
  // level 0 files are overlapping. So we cannot pick more
  // than one concurrent compactions at this level. This
  // could be made better by looking at key-ranges that are
  // being compacted at level 0.
  if (start_level_ == 0 &&
      !compaction_picker_->level0_compactions_in_progress()->empty()) {
    if (PickSizeBasedIntraL0Compaction()) {
      return true;
    }
    TEST_SYNC_POINT("LevelCompactionPicker::PickCompactionBySize:0");
    return false;
  }

  start_level_inputs_.clear();
  start_level_inputs_.level = start_level_;

  assert(start_level_ >= 0);

  if (TryPickL0TrivialMove()) {
    return true;
  }
  if (start_level_ == 0 && PickSizeBasedIntraL0Compaction()) {
    return true;
  }

  const std::vector<FileMetaData*>& level_files =
      vstorage_->LevelFiles(start_level_);

  // Pick the file with the highest score in this level that is not already
  // being compacted.
  const std::vector<int>& file_scores =
      vstorage_->FilesByCompactionPri(start_level_);

  unsigned int cmp_idx;
  for (cmp_idx = vstorage_->NextCompactionIndex(start_level_);
       cmp_idx < file_scores.size(); cmp_idx++) {
    int index = file_scores[cmp_idx];
    auto* f = level_files[index];

    // do not pick a file to compact if it is being compacted
    // from n-1 level.
    if (f->being_compacted) {
      if (ioptions_.compaction_pri == kRoundRobin) {
        // TODO(zichen): this file may be involved in one compaction from
        // an upper level, cannot advance the cursor for round-robin policy.
        // Currently, we do not pick any file to compact in this case. We
        // should fix this later to ensure a compaction is picked but the
        // cursor shall not be advanced.
        return false;
      }
      continue;
    }

    start_level_inputs_.files.push_back(f);
    if (!compaction_picker_->ExpandInputsToCleanCut(cf_name_, vstorage_,
                                                    &start_level_inputs_) ||
        compaction_picker_->FilesRangeOverlapWithCompaction(
            {start_level_inputs_}, output_level_,
            Compaction::EvaluateProximalLevel(vstorage_, mutable_cf_options_,
                                              ioptions_, start_level_,
                                              output_level_))) {
      // A locked (pending compaction) input-level file was pulled in due to
      // user-key overlap.
      start_level_inputs_.clear();

      if (ioptions_.compaction_pri == kRoundRobin) {
        return false;
      }
      continue;
    }

    // Now that input level is fully expanded, we check whether any output
    // files are locked due to pending compaction.
    //
    // Note we rely on ExpandInputsToCleanCut() to tell us whether any output-
    // level files are locked, not just the extra ones pulled in for user-key
    // overlap.
    InternalKey smallest, largest;
    compaction_picker_->GetRange(start_level_inputs_, &smallest, &largest);
    CompactionInputFiles output_level_inputs;
    output_level_inputs.level = output_level_;
    vstorage_->GetOverlappingInputs(output_level_, &smallest, &largest,
                                    &output_level_inputs.files);
    if (output_level_inputs.empty()) {
      if (start_level_ > 0 &&
          TryExtendNonL0TrivialMove(index,
                                    ioptions_.compaction_pri ==
                                        kRoundRobin /* only_expand_right */)) {
        break;
      }
    } else {
      if (!compaction_picker_->ExpandInputsToCleanCut(cf_name_, vstorage_,
                                                      &output_level_inputs)) {
        start_level_inputs_.clear();
        if (ioptions_.compaction_pri == kRoundRobin) {
          return false;
        }
        continue;
      }
    }

    base_index_ = index;
    break;
  }

  // store where to start the iteration in the next call to PickCompaction
  if (ioptions_.compaction_pri != kRoundRobin) {
    vstorage_->SetNextCompactionIndex(start_level_, cmp_idx);
  }
  return start_level_inputs_.size() > 0;
}

bool LevelCompactionBuilder::PickIntraL0Compaction() {
  if (mutable_cf_options_.experimental_st_compaction &&
      ioptions_.compaction_style == kCompactionStyleLevel) {
    // ST-compaction contract (Mind.md): clear L0 only via L0->L1, not L0->L0.
    return false;
  }
  start_level_inputs_.clear();
  const std::vector<FileMetaData*>& level_files =
      vstorage_->LevelFiles(0 /* level */);
  if (level_files.size() <
          static_cast<size_t>(
              mutable_cf_options_.level0_file_num_compaction_trigger + 2) ||
      level_files[0]->being_compacted) {
    // If L0 isn't accumulating much files beyond the regular trigger, don't
    // resort to L0->L0 compaction yet.
    return false;
  }
  return PickCostBasedIntraL0Compaction(
      level_files, kMinFilesForIntraL0Compaction,
      std::numeric_limits<uint64_t>::max(),
      mutable_cf_options_.max_compaction_bytes, &start_level_inputs_);
}

bool LevelCompactionBuilder::PickSizeBasedIntraL0Compaction() {
  assert(start_level_ == 0);
  if (mutable_cf_options_.experimental_st_compaction &&
      ioptions_.compaction_style == kCompactionStyleLevel) {
    return false;
  }
  int base_level = vstorage_->base_level();
  if (base_level <= 0) {
    return false;
  }
  const std::vector<FileMetaData*>& l0_files =
      vstorage_->LevelFiles(/*level=*/0);
  size_t min_num_file =
      std::max(2, mutable_cf_options_.level0_file_num_compaction_trigger);
  if (l0_files.size() < min_num_file) {
    return false;
  }
  uint64_t l0_size = 0;
  for (const auto& file : l0_files) {
    assert(file->compensated_file_size >= file->fd.GetFileSize());
    // Compact down L0s with more deletions.
    l0_size += file->compensated_file_size;
  }

  // Avoid L0->Lbase compactions that are inefficient for write-amp.
  const double kMultiplier =
      std::max(10.0, mutable_cf_options_.max_bytes_for_level_multiplier) * 2;
  const uint64_t min_lbase_size = MultiplyCheckOverflow(l0_size, kMultiplier);
  assert(min_lbase_size >= l0_size);
  const std::vector<FileMetaData*>& lbase_files =
      vstorage_->LevelFiles(/*level=*/base_level);
  uint64_t lbase_size = 0;
  for (const auto& file : lbase_files) {
    lbase_size += file->fd.GetFileSize();
    if (lbase_size > min_lbase_size) {
      break;
    }
  }
  if (lbase_size <= min_lbase_size) {
    return false;
  }

  start_level_inputs_.clear();
  start_level_inputs_.level = 0;
  for (const auto& file : l0_files) {
    if (file->being_compacted) {
      break;
    }
    start_level_inputs_.files.push_back(file);
  }
  if (start_level_inputs_.files.size() < min_num_file) {
    start_level_inputs_.clear();
    return false;
  }
  output_level_ = 0;
  return true /* picked an intra-L0 compaction */;
}


// ST-aware level compaction: picks files from start_level_ for compaction.
// Despite the "WholeLevel" name, when ST metadata is present this method
// selects a spatially-proximate subset anchored at the earliest-t_min file,
// not necessarily every file in the level. Non-ST files or files without
// valid st_file_meta fall back to whole-level inclusion.
bool LevelCompactionBuilder::TryPickWholeLevelNonL0ST() {
  assert(start_level_ > 0);
  start_level_inputs_.clear();
  start_level_inputs_.level = start_level_;
  const std::vector<FileMetaData*>& level_files =
      vstorage_->LevelFiles(start_level_);
  std::vector<FileMetaData*> candidates;
  candidates.reserve(level_files.size());
  for (auto* f : level_files) {
    if (f != nullptr && !f->being_compacted) {
      candidates.push_back(f);
    }
  }
  if (candidates.empty()) {
    return false;
  }

  const int max_out = vstorage_->MaxOutputLevel(
      ioptions_.cf_allow_ingest_behind || ioptions_.allow_ingest_behind);
  const int out_level =
      (start_level_ >= max_out) ? start_level_ : start_level_ + 1;
  const uint32_t base_time_norm =
      mutable_cf_options_.experimental_compaction_event_time_max_span;
  const uint32_t time_norm = std::max<uint32_t>(
      1, ComputeStEventSplitLevelPolicy(out_level, base_time_norm)
             .effective_max_time_span);
  const double coarse_th = SpatialProximityCoarseEdgeThreshold(out_level);
  constexpr double kMaxNeighborFineScore = 2.5;

  FileMetaData* anchor = nullptr;
  uint32_t anchor_t_min = UINT32_MAX;
  for (FileMetaData* f : candidates) {
    if (!f->has_st_file_meta) {
      continue;
    }
    const uint32_t t = f->st_file_meta.t_min;
    if (anchor == nullptr || t < anchor_t_min ||
        (t == anchor_t_min &&
         ioptions_.internal_comparator.Compare(f->smallest, anchor->smallest) <
             0)) {
      anchor = f;
      anchor_t_min = t;
    }
  }
  if (anchor == nullptr) {
    anchor = candidates[0];
    for (FileMetaData* f : candidates) {
      if (ioptions_.internal_comparator.Compare(f->smallest, anchor->smallest) <
          0) {
        anchor = f;
      }
    }
  }

  std::vector<FileMetaData*> picked;
  picked.push_back(anchor);
  if (anchor->has_st_file_meta) {
    const auto& am = anchor->st_file_meta;
    const double base_edge = std::sqrt(std::max(ComputeMbrArea(am), 1e-12));
    for (FileMetaData* f : candidates) {
      if (f == anchor) {
        continue;
      }
      bool include = false;
      if (f->has_st_file_meta) {
        const auto& fm = f->st_file_meta;
        if (MbrIntersects(fm, am)) {
          include = true;
        } else {
          const double d_edge = EdgeDistance(fm, am);
          const double d_norm = d_edge / std::max(base_edge, 1e-6);
          include = (d_norm <= coarse_th);
        }
        if (include) {
          const double fine = SpatialProximityFineScore(f, anchor, time_norm);
          include = fine <= kMaxNeighborFineScore;
        }
      } else if (ioptions_.internal_comparator.Compare(f->smallest,
                                                         anchor->largest) <=
                     0 &&
                 ioptions_.internal_comparator.Compare(anchor->smallest,
                                                     f->largest) <= 0) {
        include = true;
      }
      if (include) {
        picked.push_back(f);
      }
    }
  } else {
    picked = candidates;
  }

  std::sort(picked.begin(), picked.end(),
            [&](const FileMetaData* a, const FileMetaData* b) {
              return ioptions_.internal_comparator.Compare(a->smallest,
                                                           b->smallest) < 0;
            });
  start_level_inputs_.files = std::move(picked);
  if (start_level_inputs_.files.empty()) {
    return false;
  }
  if (!compaction_picker_->ExpandInputsToCleanCut(cf_name_, vstorage_,
                                                  &start_level_inputs_)) {
    start_level_inputs_.clear();
    return false;
  }
  output_level_ = out_level;

  // Unified Soft-Tiered: when l1_l2_tiered + real_tiered_l1_l2 are both
  // enabled, all levels >= tiered_min use tiered compaction uniformly.
  // All picked files are kept (no 3-file soft cap). Output-level files
  // are NOT involved (enforced in SetupOtherInputsIfNeeded). The old
  // ComputeAdaptiveTieredMaxLevel hysteresis and per-level tiered_max
  // cutoff are removed. Run-count is controlled by the new option
  // experimental_st_compaction_tiered_max_runs_per_level.
  int tiered_min = mutable_cf_options_.experimental_st_compaction_tiered_min_level;
  if (tiered_min <= 0) tiered_min = 1;
  int tiered_max = mutable_cf_options_.experimental_st_compaction_tiered_max_level;
  bool is_tiered = (mutable_cf_options_.experimental_st_compaction_l1_l2_tiered &&
                    mutable_cf_options_.experimental_st_compaction_real_tiered_l1_l2 &&
                    start_level_ >= tiered_min &&
                    (tiered_max <= 0 || start_level_ <= tiered_max));
  if (is_tiered) {
    // Real Tiered: keep ALL files (whole-level compaction).
  } else if (mutable_cf_options_.experimental_st_compaction_l1_l2_tiered &&
             !mutable_cf_options_.experimental_st_compaction_real_tiered_l1_l2 &&
             start_level_ >= tiered_min) {
    // Legacy soft-tiered path: capped at 3 files (for A/B comparison).
    if (start_level_inputs_.files.size() > 3) {
      start_level_inputs_.files.resize(3);
    }
  }

  if (compaction_picker_->FilesRangeOverlapWithCompaction(
          {start_level_inputs_}, output_level_,
          Compaction::EvaluateProximalLevel(vstorage_, mutable_cf_options_,
                                            ioptions_, start_level_,
                                            output_level_))) {
    start_level_inputs_.clear();
    return false;
  }
  return true;
}
}  // namespace

bool LevelCompactionPicker::NeedsCompaction(
    const VersionStorageInfo* vstorage,
    const MutableCFOptions& mutable_cf_options) const {
  return NeedsCompaction(vstorage) ||
         FindSTRunCountLevel(vstorage, mutable_cf_options, ioptions_) >= 0;
}

Compaction* LevelCompactionPicker::PickCompaction(
    const std::string& cf_name, const MutableCFOptions& mutable_cf_options,
    const MutableDBOptions& mutable_db_options,
    const std::vector<SequenceNumber>& /*existing_snapshots */,
    const SnapshotChecker* /*snapshot_checker*/, VersionStorageInfo* vstorage,
    LogBuffer* log_buffer, const std::string& full_history_ts_low,
    bool /* require_max_output_level*/) {
  LevelCompactionBuilder builder(cf_name, vstorage, this, log_buffer,
                                 mutable_cf_options, ioptions_,
                                 mutable_db_options, full_history_ts_low);
  return builder.PickCompaction();
}
}  // namespace ROCKSDB_NAMESPACE

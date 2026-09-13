// SPDX-License-Identifier: Apache-2.0

#include "table/block_based/st_mbr_flush_block_policy.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "db/dbformat.h"
#include "options/cf_options.h"
#include "rocksdb/table.h"
#include "table/block_based/flush_block_policy_impl.h"
#include "table/st_meta_user_key.h"

namespace ROCKSDB_NAMESPACE {

namespace {

constexpr double kDefaultBlockMbrExpansionRatio = 2.5;
constexpr double kAreaEps = 1e-12;

double ComputeSpatialArea(const SpatioTemporalBlockMeta& m) {
  const double w = std::max(0.0, static_cast<double>(m.x_max) -
                                     static_cast<double>(m.x_min));
  const double h = std::max(0.0, static_cast<double>(m.y_max) -
                                     static_cast<double>(m.y_min));
  return w * h;
}

SpatioTemporalBlockMeta UnionMbr(const SpatioTemporalBlockMeta& a,
                                 const SpatioTemporalBlockMeta& b) {
  SpatioTemporalBlockMeta out = a;
  out.t_min = std::min(out.t_min, b.t_min);
  out.t_max = std::max(out.t_max, b.t_max);
  out.x_min = std::min(out.x_min, b.x_min);
  out.x_max = std::max(out.x_max, b.x_max);
  out.y_min = std::min(out.y_min, b.y_min);
  out.y_max = std::max(out.y_max, b.y_max);
  return out;
}

bool DecodeIncomingBlockMeta(const Slice& user_key, const Slice& value,
                             SpatioTemporalBlockMeta* meta) {
  if (meta == nullptr) {
    return false;
  }
  bool valid = false;
  MergeSpatioTemporalMetaFromKeyValue(user_key, std::optional<Slice>(value), meta,
                                    &valid);
  return valid;
}

bool DecodeStFlushPartitionId(const Slice& user_key, uint32_t* partition_id) {
  if (partition_id == nullptr) {
    return false;
  }
  if (TryDecodeE7HostBucketId(user_key, partition_id)) {
    return true;
  }
  int16_t period = 0;
  if (TryDecodeTmanStPeriod(user_key, &period)) {
    *partition_id = static_cast<uint32_t>(static_cast<uint16_t>(period));
    return true;
  }
  return false;
}

// Maximum consecutive calendar buckets allowed in one Data Block
// before forcing a flush on bucket change. Values of 2-3 let adjacent-bucket
// trajectories (e.g. 23:58 and 00:02 on a day boundary) stay in the same block.
// The block-level constraint is tighter than file-level max_bucket_span because
// Data Blocks are much smaller units — we only need to avoid fragmenting blocks
// on the very first bucket boundary.
constexpr uint32_t kBlockBucketSpanTolerance = 3;

class StMbrFlushBlockPolicy : public RetargetableFlushBlockPolicy {
 public:
  StMbrFlushBlockPolicy(const BlockBasedTableOptions& table_options,
                        const MutableCFOptions& moptions,
                        const BlockBuilder& data_block_builder,
                        std::unique_ptr<FlushBlockPolicy> size_policy)
      : RetargetableFlushBlockPolicy(data_block_builder),
        expansion_ratio_(ResolveExpansionRatio(moptions)),
        size_policy_(std::move(size_policy)) {
    (void)table_options;
  }

  bool Update(const Slice& key, const Slice& value) override {
    if (data_block_builder_->empty()) {
      tracker_valid_ = false;
      tracker_first_bucket_id_ = 0;
      tracker_last_bucket_id_ = 0;
      tracker_mbr_ = {};
      pending_first_valid_ = SeedPendingFirst(key, value);
      return false;
    }

    if (size_policy_ != nullptr && size_policy_->Update(key, value)) {
      pending_first_valid_ = false;
      SeedTrackerFromIncoming(key, value);
      return true;
    }

    const Slice user_key = ExtractUserKey(key);
    uint32_t bucket_id = 0;
    if (!DecodeStFlushPartitionId(user_key, &bucket_id)) {
      return false;
    }

    SpatioTemporalBlockMeta incoming{};
    if (!DecodeIncomingBlockMeta(user_key, value, &incoming)) {
      return false;
    }

    if (!tracker_valid_) {
      if (pending_first_valid_) {
        tracker_valid_ = true;
        tracker_first_bucket_id_ = pending_first_bucket_id_;
        tracker_last_bucket_id_ = pending_first_bucket_id_;
        tracker_mbr_ = pending_first_mbr_;
        pending_first_valid_ = false;
      } else {
        tracker_valid_ = true;
        tracker_first_bucket_id_ = bucket_id;
        tracker_last_bucket_id_ = bucket_id;
        tracker_mbr_ = incoming;
      }
    }

    // Bucket span tolerance: allow up to kBlockBucketSpanTolerance adjacent
    // buckets in the same block before forcing a flush. This avoids splitting
    // blocks on the very first boundary where trajectories (e.g. 23:58 and
    // 00:02) are physically adjacent despite different calendar buckets.
    if (bucket_id != tracker_last_bucket_id_) {
      const uint32_t first = std::min(tracker_first_bucket_id_, bucket_id);
      const uint32_t last  = std::max(tracker_first_bucket_id_, bucket_id);
      const uint32_t span = (last - first) + 1;
      if (span > kBlockBucketSpanTolerance) {
        pending_first_valid_ = false;
        SeedTrackerFromIncomingMeta(bucket_id, incoming);
        return true;
      }
      // Within tolerance: allow into current block, update last-seen bucket.
      tracker_last_bucket_id_ = bucket_id;
    }

    const SpatioTemporalBlockMeta merged = UnionMbr(tracker_mbr_, incoming);
    const double old_area = ComputeSpatialArea(tracker_mbr_);
    const double new_area = ComputeSpatialArea(merged);
    const double expansion =
        (new_area - old_area) / std::max(old_area, kAreaEps);
    if (expansion_ratio_ > 0.0 && expansion >= expansion_ratio_) {
      pending_first_valid_ = false;
      SeedTrackerFromIncomingMeta(bucket_id, incoming);
      return true;
    }

    tracker_mbr_ = merged;
    return false;
  }

 private:
  bool SeedPendingFirst(const Slice& key, const Slice& value) {
    const Slice user_key = ExtractUserKey(key);
    uint32_t bucket_id = 0;
    SpatioTemporalBlockMeta incoming{};
    if (!DecodeStFlushPartitionId(user_key, &bucket_id) ||
        !DecodeIncomingBlockMeta(user_key, value, &incoming)) {
      pending_first_valid_ = false;
      return false;
    }
    pending_first_valid_ = true;
    pending_first_bucket_id_ = bucket_id;
    pending_first_mbr_ = incoming;
    return true;
  }

  static double ResolveExpansionRatio(const MutableCFOptions& moptions) {
    if (moptions.experimental_st_block_compaction_ratio > 0.0) {
      return moptions.experimental_st_block_compaction_ratio;
    }
    return kDefaultBlockMbrExpansionRatio;
  }

  void SeedTrackerFromIncoming(const Slice& key, const Slice& value) {
    pending_first_valid_ = false;
    const Slice user_key = ExtractUserKey(key);
    uint32_t bucket_id = 0;
    SpatioTemporalBlockMeta incoming{};
    if (!DecodeStFlushPartitionId(user_key, &bucket_id) ||
        !DecodeIncomingBlockMeta(user_key, value, &incoming)) {
      tracker_valid_ = false;
      tracker_mbr_ = {};
      return;
    }
    SeedTrackerFromIncomingMeta(bucket_id, incoming);
  }

  void SeedTrackerFromIncomingMeta(uint32_t bucket_id,
                                   const SpatioTemporalBlockMeta& incoming) {
    tracker_valid_ = true;
    tracker_first_bucket_id_ = bucket_id;
    tracker_last_bucket_id_ = bucket_id;
    tracker_mbr_ = incoming;
  }

  const double expansion_ratio_;
  std::unique_ptr<FlushBlockPolicy> size_policy_;
  bool tracker_valid_ = false;
  uint32_t tracker_first_bucket_id_ = 0;
  uint32_t tracker_last_bucket_id_ = 0;
  SpatioTemporalBlockMeta tracker_mbr_{};
  bool pending_first_valid_ = false;
  uint32_t pending_first_bucket_id_ = 0;
  SpatioTemporalBlockMeta pending_first_mbr_{};
};

}  // namespace

std::unique_ptr<FlushBlockPolicy> NewStMbrCombinedFlushBlockPolicy(
    const BlockBasedTableOptions& table_options,
    const MutableCFOptions& moptions, const BlockBuilder& data_block_builder) {
  std::unique_ptr<FlushBlockPolicy> size_policy(
      table_options.flush_block_policy_factory->NewFlushBlockPolicy(
          table_options, data_block_builder));
  return std::unique_ptr<FlushBlockPolicy>(new StMbrFlushBlockPolicy(
      table_options, moptions, data_block_builder, std::move(size_policy)));
}

std::unique_ptr<FlushBlockPolicy> NewDefaultOrStMbrFlushBlockPolicy(
    const BlockBasedTableOptions& table_options,
    const MutableCFOptions& moptions, const BlockBuilder& data_block_builder) {
  const bool st_block_flush =
      table_options.experimental_spatio_temporal_meta_in_index_value &&
      (moptions.experimental_st_block_mbr_flush_enable ||
       moptions.experimental_st_block_compaction_ratio > 0.0);
  if (st_block_flush) {
    return NewStMbrCombinedFlushBlockPolicy(table_options, moptions,
                                            data_block_builder);
  }
  return std::unique_ptr<FlushBlockPolicy>(
      table_options.flush_block_policy_factory->NewFlushBlockPolicy(
          table_options, data_block_builder));
}

}  // namespace ROCKSDB_NAMESPACE

// TMan-RocksDB TMRV1 value + STPT point-truth trailer for ST fork prune.
#pragma once

#include <vector>

#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "table/st_meta_index_extension.h"
#include "table/st_meta_user_key.h"

namespace ROCKSDB_NAMESPACE {

bool TmanRocksValueHasPointInSpatioTemporalWindow(
    const Slice& value,
    const ReadOptions::ExperimentalSpatioTemporalPruneScan& q);

bool MergeTmanRocksStptBounds(const Slice& value, SpatioTemporalBlockMeta* acc,
                              bool* acc_valid);

void CollectTmanRocksStptPoints(const Slice& value,
                                std::vector<SpatioTemporalPoint>* out_points,
                                bool* precise_points);

}  // namespace ROCKSDB_NAMESPACE

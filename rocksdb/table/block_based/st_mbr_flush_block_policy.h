// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>

#include "rocksdb/flush_block_policy.h"
#include "table/block_based/block_builder.h"

namespace ROCKSDB_NAMESPACE {

struct MutableCFOptions;
struct BlockBasedTableOptions;

// Combined FlushBlockBySize + ST MBR / bucket_id block clustering (Mind.md).
std::unique_ptr<FlushBlockPolicy> NewStMbrCombinedFlushBlockPolicy(
    const BlockBasedTableOptions& table_options,
    const MutableCFOptions& moptions, const BlockBuilder& data_block_builder);

std::unique_ptr<FlushBlockPolicy> NewDefaultOrStMbrFlushBlockPolicy(
    const BlockBasedTableOptions& table_options,
    const MutableCFOptions& moptions, const BlockBuilder& data_block_builder);

}  // namespace ROCKSDB_NAMESPACE

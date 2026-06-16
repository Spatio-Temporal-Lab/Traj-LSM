# Spatio-Temporal RocksDB (ST-RocksDB) — TiDB/TiKV Integration Guide

## Overview

ST-RocksDB extends RocksDB with spatio-temporal indexing and pruning, targeting efficient
range queries over trajectory data (and more generally, any data with time + space dimensions).

### What you get

```
                     Without ST                     With ST
                     ══════════                     ═══════
Query: "cars in      Scan ALL SSTs →     File-level time+MBR pruning →
 area X between      read all blocks →   skip 80%+ SSTs →
 time T1-T2"         filter in memory     index-block hard filter →
                                          only read matching data
                                          
Query latency:       100% (baseline)      3-10x faster for selective queries
Write throughput:    100% (baseline)      ~90-100% (slight overhead for MBR tracking)
```

### Architecture: Three Layers

```
┌─────────────────────────────────────────────────────┐
│ GLOBAL INDEX (Manifest + File-level pruning)        │
│ • per-SST st_file_meta: [t_min, t_max] + MBR       │
│ • Query first checks each SST's time+space envelope │
│ • Skips SSTs that can't contain results             │
├─────────────────────────────────────────────────────┤
│ LOCAL INDEX (Index block metadata)                  │
│ • Each index block has [t_min, t_max] + MBR         │
│ • Hard filter: skip blocks that don't intersect     │
│ • Bucket_id in key for coarse positioning           │
├─────────────────────────────────────────────────────┤
│ POINT TRUTH (Value-level verification)              │
│ • Decode segment value, check individual points     │
│ • Final guarantee: no false negatives               │
└─────────────────────────────────────────────────────┘
```

## Part 1: Apply the Patch

### 1.1 Prerequisites

- RocksDB 11.2.0 (the fork is based on this version)
- Your existing RocksDB fork or a fresh clone
- C++20 compiler (MSVC 2022+ or GCC 13+ / Clang 16+)

### 1.2 Apply

```bash
# In your RocksDB repository:
cd /path/to/rocksdb
git checkout -b st-integration

# Apply the ST patch
git apply /path/to/rocksdb-st-full.patch

# Or if conflicts:
patch -p1 < /path/to/rocksdb-st-full.patch
```

### 1.3 Build

```bash
mkdir build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
ninja rocksdb
```

## Part 2: New Configuration Options

All new options are mutable (changeable via `SetOptions` at runtime).

### CF Options (in `advanced_options.h`)

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `experimental_st_compaction` | bool | false | Enable ST-aware compaction scheduling |
| `experimental_st_compaction_level_trigger_ratio` | double | 0.9 | Lx total bytes / MaxBytesForLevel trigger |
| `experimental_st_compaction_l1_l2_tiered` | bool | false | Use Tiered semantics for L1→L2, L2→L3 |
| `experimental_compaction_event_time_split` | bool | false | Enable time-bucket output SST splitting |
| `experimental_compaction_event_time_bucket_width` | uint32 | 0 | Seconds per time bucket (e.g. 3600 = 1 hour) |
| `experimental_compaction_event_time_max_span` | uint32 | 0 | Max time span per output SST (0 = unlimited) |
| `experimental_compaction_spatial_soft_expansion_ratio` | double | 0 | MBR expansion flush trigger (e.g. 2.5) |

### Table Options (in `table.h`)

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `experimental_spatio_temporal_meta_in_index_value` | bool | false | Write ST metadata into index block values |

### Read Options (in `options.h`)

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `experimental_st_prune_scan.enable` | bool | false | Enable ST prune scan |
| `experimental_st_prune_scan.t_min/t_max` | uint32 | 0 | Query time window |
| `experimental_st_prune_scan.x_min/x_max/y_min/y_max` | float | 0 | Query spatial window |
| `experimental_st_prune_scan.file_level_enable` | bool | false | Enable file-level pruning |
| `experimental_st_prune_scan.block_level_enable` | bool | false | Enable index-block-level pruning |
| `experimental_st_prune_scan.key_level_enable` | bool | false | Enable key-level point truth check |
| `experimental_st_prune_scan.event_time_bucket_width_sec` | uint32 | 3600 | Time bucket width for index seek |

## Part 3: Data Format

### 3.1 E7 Segment Key (32 bytes, big-endian, memcmp-sortable)

```
Offset  Size  Field            Description
────────────────────────────────────────────
[0]     1B    magic = 0xE7     Segment key identifier
[1]     1B    type_flags       Reserved, must be 0
[2:6)   4B    bucket_id        Calendar bucket = floor(t_start / bucket_sec)
[6:14)  8B    zorder           Morton spatial code of segment centroid
[14:18) 4B    t_start_offset   t_start - bucket_start (seconds)
[18:26) 8B    vehicle_id       Unique vehicle identifier
[26:30) 4B    segment_seq      Segment sequence number
[30:32) 2B    chunk_seq        Chunk sequence within segment
```

Key properties:
- **Sorted by bucket_id first** (coarse time ordering)
- **Then by zorder** (spatial locality within the same time bucket)
- **Then by t_start_offset, vehicle_id, segment_seq, chunk_seq** (tie-breaking)
- **segment_uid = (vehicle_id, segment_seq, chunk_seq)** uniquely identifies a segment

### 3.2 Segment Value (V2 format)

```
Offset  Size  Field
─────────────────────
[0:4)   4B    magic = 'Seg2' (0x32676553)
[4:8)   4B    t_start (uint32, unix seconds)
[8:12)  4B    t_end   (uint32)
[12:16) 4B    x_min   (float32)
[16:20) 4B    y_min   (float32)
[20:24) 4B    x_max   (float32)
[24:28) 4B    y_max   (float32)
[28:30) 2B    point_count (uint16)
[30:32) 2B    reserved
─────────────────────
Header: 32 bytes total

Followed by point_count × 12 bytes:
Offset  Size  Field
[0:4)   4B    unix_s  (uint32)
[4:8)   4B    lon     (float32)
[8:12)  4B    lat     (float32)
```

Helper functions in `segment_value_codec.hpp`:
```cpp
// Encode
std::string EncodeSegmentValueV2(const SegmentValueHeader& head,
                                  const std::vector<SegmentPoint>& points);

// Decode
bool DecodeSegmentValue(const std::string& blob, DecodedSegmentValue* out);
```

### 3.3 Key Generation (C++)

```cpp
#include "st_segment_key.hpp"

uint32_t bucket_sec = 3600;  // 1-hour buckets
uint32_t bucket_id = t_start / bucket_sec;
uint32_t t_start_offset = t_start - bucket_id * bucket_sec;
uint64_t zorder = ComputeMortonCode(lon, lat, resolution);

std::string key = StSegmentKeyTzidRaw(
    bucket_id, zorder, t_start_offset,
    vehicle_id, segment_seq, chunk_seq,
    /*type_flags=*/0);
```

### 3.4 Key Generation (Go, for TiKV)

```go
import "encoding/binary"

const E7KeySize = 32
const E7Magic = 0xE7

func EncodeE7Key(bucketID uint32, zorder uint64, tStartOffset uint32,
                  vehicleID uint64, segmentSeq uint32, chunkSeq uint16) []byte {
    key := make([]byte, E7KeySize)
    key[0] = E7Magic
    key[1] = 0 // type_flags
    binary.BigEndian.PutUint32(key[2:6], bucketID)
    binary.BigEndian.PutUint64(key[6:14], zorder)
    binary.BigEndian.PutUint32(key[14:18], tStartOffset)
    binary.BigEndian.PutUint64(key[18:26], vehicleID)
    binary.BigEndian.PutUint32(key[26:30], segmentSeq)
    binary.BigEndian.PutUint16(key[30:32], chunkSeq)
    return key
}
```

## Part 4: TiKV Integration Points

### 4.1 Where to touch in TiKV

```
TiKV Layer                         What to change
══════════                         ═══════════════
storage/txn/store.go              → Pass ReadOptions with prune_scan params
src/storage/mvcc/reader.rs        → NewIterator with ST-aware options
src/config/rocksdb.rs             → Add new CF options to config
src/server/service/kv.rs          → Extract time+space from coprocessor request
components/engine_rocks/          → SetOptions for ST compaction settings
```

### 4.2 Write Path

```
1. Trajectory data arrives at TiKV
2. Convert to E7 key format (see §3.4)
3. Encode segment value (see §3.2)
4. Put(key, value) into RocksDB
5. At memtable flush → Compaction uses ST options
   - Time-bucket output splitting
   - MBR-based block flush policy
   - st_file_meta written per SST
```

### 4.3 Query Path

```
1. Coprocessor request arrives with time window + spatial MBR
2. Create ReadOptions with:
   ro.experimental_st_prune_scan.enable = true
   ro.experimental_st_prune_scan.file_level_enable = true
   ro.experimental_st_prune_scan.block_level_enable = true
   ro.experimental_st_prune_scan.key_level_enable = true
   ro.experimental_st_prune_scan.t_min = q.t_min
   ro.experimental_st_prune_scan.t_max = q.t_max
   ro.experimental_st_prune_scan.x_min = q.x_min
   // ... etc
3. NewIterator(ro) → automatically prunes at all 3 levels
4. Iterate results, decode values, return to client
```

### 4.4 Compaction Configuration

For production deployment, start with ST-Leveled (all levels use Leveled):

```cpp
// DB open options
options.experimental_st_compaction = true;
options.experimental_compaction_event_time_split = true;
options.experimental_compaction_event_time_bucket_width = 3600;  // 1 hour
options.experimental_compaction_spatial_soft_expansion_ratio = 2.5;
options.experimental_spatio_temporal_meta_in_index_value = true;

// Required for ST: disable dynamic level bytes
options.level_compaction_dynamic_level_bytes = false;
```

For write-heavy workloads, switch to Hybrid (Tiered L1-L2):

```cpp
options.experimental_st_compaction_l1_l2_tiered = true;
```

ALL options are mutable — can change via `SetOptions` at runtime without restart.

## Part 5: Validation

### 5.1 Quick Smoke Test

```cpp
// 1. Open DB with ST options
Options options;
options.create_if_missing = true;
BlockBasedTableOptions tb;
tb.experimental_spatio_temporal_meta_in_index_value = true;
options.table_factory.reset(NewBlockBasedTableFactory(tb));
options.experimental_st_compaction = true;
options.experimental_compaction_event_time_split = true;
options.experimental_compaction_event_time_bucket_width = 3600;
options.level_compaction_dynamic_level_bytes = false;

DB* db;
DB::Open(options, "/tmp/test_st", &db);

// 2. Write sample data
string key = StSegmentKeyTzidRaw(bucket_id, zorder, t_start_offset,
                                  vehicle_id, segment_seq, chunk_seq, 0);
string value = EncodeSegmentValueV2(header, points);
db->Put(WriteOptions(), key, value);
db->Flush(FlushOptions());

// 3. Query with prune scan
ReadOptions ro;
ro.experimental_st_prune_scan.enable = true;
ro.experimental_st_prune_scan.file_level_enable = true;
ro.experimental_st_prune_scan.block_level_enable = true;
ro.experimental_st_prune_scan.key_level_enable = true;
ro.experimental_st_prune_scan.t_min = query_t_min;
ro.experimental_st_prune_scan.t_max = query_t_max;
ro.experimental_st_prune_scan.x_min = qx_min;
ro.experimental_st_prune_scan.x_max = qx_max;
ro.experimental_st_prune_scan.y_min = qy_min;
ro.experimental_st_prune_scan.y_max = qy_max;

unique_ptr<Iterator> it(db->NewIterator(ro));
for (it->SeekToFirst(); it->Valid(); it->Next()) {
    // Process result
}
```

### 5.2 Correctness Check

```cpp
// Verify: prune scan results == full scan results
int full_count = 0;
Iterator* full_it = db->NewIterator(ReadOptions()); // no pruning
for (full_it->SeekToFirst(); full_it->Valid(); full_it->Next()) {
    if (value_in_window(full_it->key(), full_it->value())) full_count++;
}

int prune_count = 0;
ReadOptions prune_ro;
prune_ro.experimental_st_prune_scan.enable = true; // ... set window
Iterator* prune_it = db->NewIterator(prune_ro);
for (prune_it->SeekToFirst(); prune_it->Valid(); prune_it->Next()) {
    prune_count++;
}

assert(full_count == prune_count); // Must be equal!
```

### 5.3 Sample Data & Query Windows

Included in the delivery package:
- `sample_data/` — 1000-segment sample in E7 format
- `query_windows.csv` — 6 validation windows with expected result counts
- `build_and_test.sh` — Automated build + smoke + validate script

## Part 6: Common Issues

### Q: SetOptions fails with "incompatible with dynamic_level_bytes"
```
→ Set level_compaction_dynamic_level_bytes=false in DB open options
  (this is an immutable option — cannot change via SetOptions)
```

### Q: Prune scan returns 0 results but full scan finds data
```
→ Check that experimental_spatio_temporal_meta_in_index_value=true 
  was set when the SST files were created. Old SSTs without metadata
  cannot be pruned.
→ Check st_file_meta is populated: use st_meta_sst_diag to inspect.
```

### Q: Compaction is very slow with ST options
```
→ Set target_file_size_base=32MB (or higher) for production
→ Reduce max_subcompactions if parallelism causes contention
```

### Q: Want to compare ST vs vanilla RocksDB
```
→ Use the benchmark tools in rocks-demo/:
  - st_compaction_bench: query latency comparison
  - st_mixed_workload_bench: mixed read/write throughput
```

## Part 7: File Index

```
Delivery package:
├── rocksdb-st-full.patch          ← All ST changes (apply to RocksDB 11.2.0)
├── TIDB_INTEGRATION_GUIDE.md      ← This document
├── sample_data/
│   ├── segments_meta.csv          ← 1000 sample segments metadata
│   ├── segments_points.csv        ← Corresponding trajectory points
│   └── query_windows.csv          ← Validation query windows
├── tools/
│   ├── build_and_test.sh          ← Automated build + smoke test
│   ├── st_segment_key.hpp         ← E7 key reference implementation
│   ├── segment_value_codec.hpp    ← Value encode/decode reference
│   └── run_validation.py          ← Python validation script
└── docs/
    └── architecture.md            ← Detailed architecture reference
```

## Appendix A: Morton Code (Z-order) Reference

```cpp
// Interleave bits of x and y to produce Morton code
inline uint64_t MortonEncode(uint32_t x, uint32_t y) {
    uint64_t z = 0;
    for (int i = 0; i < 32; i++) {
        z |= (uint64_t)((x >> i) & 1) << (2 * i);
        z |= (uint64_t)((y >> i) & 1) << (2 * i + 1);
    }
    return z;
}

// To map (lon, lat) to Morton grid at resolution r:
// grid_x = floor((lon - x_min) / (x_max - x_min) * (2^r - 1))
// grid_y = floor((lat - y_min) / (y_max - y_min) * (2^r - 1))
// zorder = MortonEncode(grid_x, grid_y)
```

## Appendix B: ST Level-Aware Policy

```
Level   max_bucket_span  time_span_factor  spatial_ratio
═════════════════════════════════════════════════════════
L1      2                0.5x              1.0x
L2-3    4                1.0x              1.35x
L4+     16               2.0x              1.8x
```

These control how aggressively the compaction splits output SSTs by
time bucket and spatial MBR at each level. Shallower levels are stricter,
deeper levels allow more merging.

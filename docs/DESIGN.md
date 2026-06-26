# ST-RocksDB 设计文档（终稿）

> 2026-06 · 唯一真相源 · 代码落地口径

---

## 一、系统总览

ST-RocksDB 是基于 RocksDB fork 的**时空轨迹数据存储引擎**，在 LSM-tree 上增加时空索引和 ST-compaction，支持 100% 召回的轨迹段时空查询。

```
                            QUERY PIPELINE

  M_manager ──→ MANIFEST-b shards ──→ SST st_file_meta ──→ Local Index ──→ Data Block
  时间+空间        per-bucket            file time+MBR        index block      点真判
  粗筛shard       descriptor replay      粗筛SST             value硬判定
```

**核心保证**：`false_negative_count = 0`

---

## 二、关键概念

### STFP-R4

**STFP = Spatio-Temporal Flush Policy**，ST-compaction 的输出策略，最终选型 **STFP-R4**（max_runs=4 + event_time_split）。

对比 7 种策略后选定：Read 577 MB/s（最高），Write 69 MB/s，所有 read ratio 下最稳定。STFP-R6（6 runs）被碾压——多 50% merge 开销，写放大更大，读反而更低。

### E7 Key

31 字节 user key：`[0xE7] [bucket_id:4B BE] [zorder:8B BE] [t_start_offset:4B BE] [vehicle_id:8B BE] [segment_seq:4B BE] [chunk_seq:2B BE]`

`bucket_id = floor(t_start / W)` 为 canonical 桶，用于排序和块断流，**不是**数据分区。

### segment_uid

`(vehicle_id, segment_seq, chunk_seq)` —— 每 uid 恰好一条 KV。

---

## 三、Global Index：M_manager + 多 MANIFEST-b

### 3.1 盘上结构

```
CURRENT → M_manager-NNNNNN
              ├── MANIFEST-b_B0  [host_bucket=B0, t_min..t_max, MBR]  ~200K segments
              ├── MANIFEST-b_B1  [host_bucket=B1, t_min..t_max, MBR]  ~200K segments
              └── ...
```

| 组件 | 职责 | 代码位置 |
|------|------|----------|
| `CURRENT` | 指向当前 M_manager | `version_set.cc:ProcessManifestWrites` |
| `M_manager` | 索引：所有 MANIFEST-b 的时间窗+MBR+host_bucket | `manifest_ops.h` |
| `MANIFEST-b` | per-bucket 增量 descriptor log | `version_set.cc` shard_route 路径 |
| `st_file_meta` | 每 SST 全局 `[t_min,t_max]`+MBR（Value 头全段并集）| `compaction_outputs.cc` 建表路径 |

### 3.2 Manifest 容量 & 分片触发

每个 MANIFEST-b 承载 **~200K** 轨迹段。限制来自 `max_manifest_file_size`（默认 4MB）：

```
200K segments × ~20 bytes/entry ≈ 4MB → 新建 MANIFEST-b
超限 → 新 M_manager → 追加 MANIFEST-b → 后续 SST 写入新 shard
```

总分片数上限：`ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_MAX`（默认 8192）。

> 代码：`version_set.cc:8485` `prev_manifest_file_size >= tuned_max_manifest_file_size_` 触发 rollover；`version_set.cc:8091` `kMaxManifestSetShards` 上限。

### 3.3 Global 查询路径

```
M_manager:          时间 AND 空间粗筛 relevant shards         ← manifest_ops.h:FilterManifestShardIndicesByTimeRange
    ↓
MANIFEST-b replay:  得 live SST 列表 + st_file_meta            ← manifest_ops.h:GetCurrentManifestPathsFilteredByTimeRange
    ↓
st_file_meta:       时间 AND 空间粗筛 SST                      ← manifest_ops.h:ManifestFilterSstByStFileMeta
    ↓
Local Index:        index block value 硬判定                   ← 第五节
```

| 阶段 | 防漏策略 |
|------|----------|
| M_manager | 时间/空间不相交 shard → 安全跳过 |
| st_file_meta | meta 缺失 → **直接报错**，不降级 |
| Local | 硬性条件，不满足跳过（见第五节） |

---

## 四、ST-Compaction（STFP-R4）

### 4.1 归并流

```
整层 Lx SST ──LevelIterator──┐
                              ├── CompactionMergingIterator（最小堆归并）
Lx+1 overlap SST ─LevelIterator┘
                              ↓
                       CompactionIterator（逐条输出）
                              ↓
                       TableBuilder
                         ├── StMbrFlushBlockPolicy   ← bucket_id 变化 或 MBR膨胀≥2.5× → Flush Data Block
                         ├── ShouldStopBeforeEventTime ← keys≥max_keys 或 time_span超限 → 切新 SST
                         └── 写 st_file_meta（全段时间窗+MBR）
                              ↓
                         新 SST → Manifest VersionEdit AddFile（同边界）
```

> Compaction picker: `compaction_picker.cc:TryPickWholeLevelNonL0ST`
> 输出切分: `compaction_outputs.cc:ShouldStopBeforeEventTime`
> 块级断流: `st_mbr_flush_block_policy.cc`

### 4.2 STFP-R4 配置

```
experimental_st_compaction                         = true
experimental_st_compaction_l1_l2_tiered            = true
experimental_st_compaction_real_tiered_l1_l2       = true
experimental_st_compaction_tiered_max_runs_per_level = 4        ← "R4"
experimental_st_compaction_stfp_enable             = true       ← "STFP"
experimental_compaction_event_time_split           = true
experimental_compaction_event_time_bucket_width    = 3600       ← 1h bucket
experimental_compaction_event_time_max_span        = 86400      ← 24h
```

### 4.3 层级策略

| 层 | 跨桶 | 空间临近 | 时间跨度 |
|----|------|----------|----------|
| L0→L1 | 下移 | — | — |
| L1 | 严格 | d_level 小 | 基准值 |
| L2–L3 | 中等 | 中等 | 基准值 |
| L4+ | 放宽 | d_level 放宽 | 2× 基准值 |

> 代码：`st_event_split_policy.h:ComputeStEventSplitLevelPolicy`

---

## 五、SST 层级段数限制（SST Size Pyramid）

### 5.1 当前参数（终稿）

| 参数 | 值 |
|------|-----|
| `target_file_size_base` | **64 MB** |
| `max_bytes_for_level_base` | **64 MB**（L1） |
| `max_bytes_for_level_multiplier` | **4**（L2=256MB, L3=1GB） |
| `write_buffer_size` | **64 MB** |

### 5.2 层级 SST 金字塔

`st_event_split_policy.h` 定义硬性 key cap（4× 每层放大）：

| Level | Keys/SST | 约大小 | L 容量 | SST 数 | 触发 compaction |
|-------|----------|--------|--------|--------|-----------------|
| L1 | **5,000** | ~6.5 MB | 64 MB | ~10 | max_runs=4 @ 第5个 SST |
| L2 | **20,000** | ~26 MB | 256 MB | ~10 | max_runs=4 @ 第5个 SST |
| L3 | **80,000** | 64 MB* | 1 GB | ~16 | 文件大小绑定（≈50K 段） |
| L4+ | ∞ | 64 MB | 4 GB | — | 大文件顺序 IO |

- L1/L2：key cap 绑定 → 小块高精度剪枝
- L3：文件大小绑定 → 过渡
- L4+：全放开 → 大文件顺序 IO
- 无全局 cap（`max_point_keys = 0`）

### 5.3 双重限制体系

```
查询窗口
    ↓
M_manager              ← 时间+空间粗筛（~200K segments/shard = max_manifest_file_size 4MB）
    ↓
MANIFEST-b shard       
    ↓
st_file_meta           ← 文件级时间+空间粗筛（[t_min,t_max]+MBR）
    ↓
SST 内部               ← L1=5K, L2=20K, L3=80K（st_event_split_policy.h）
    ↓
Data Block             ← MBR膨胀≥2.5× / 跨bucket_id → FlushBlockPolicy 断流
```

---

## 六、Local Index

### 6.1 Index Block 改造

RocksDB index block 的 **value** 中加入该 block 的全局时空信息：

| 字段 | 位置 | 用途 | 代码 |
|------|------|------|------|
| `bucket_id` | index block **key** | 粗定位 + 终止判定 | `st_segment_key.hpp` |
| `[t_min,t_max]`+MBR | index block **value** | 硬性条件判定 | `index_builder.cc:MergeSpatioTemporalMetaFromKeyValue` |

> 启用：`BlockBasedTableOptions::experimental_spatio_temporal_meta_in_index_value = true`

### 6.2 两阶段剪枝

**阶段 1 — 粗定位（key bucket_id）**：
- 查询窗 → bucket_id，用最小 bucket_id `Seek` 起始 block，**向后**遍历
- 当 key bucket_id < 查询最小 bucket_id → 安全终止（key 有序单增保证后面一定没有）
- **禁止**用 bucket_id 判定起始（跨桶长段可能从更早 bucket_id 开始）

**阶段 2 — 硬判定（value 时空信息）**：
- 检查 index block value 中 `[t_min,t_max]` + MBR
- 时间 AND 空间都相交 → 读 Data Block
- 不满足 → 跳过（安全，硬性条件，不降级）
- meta 缺失 / value 解码失败 → **直接报错**

### 6.3 点真判

Data Block 解压 → 遍历轨迹点 → 任一点落入查询窗才算命中（控 FP，与 FN 无关）。

---

## 七、存储语义

| 规则 | 说明 |
|------|------|
| 每 segment_uid 一条 KV | K ≈ N，无跨桶复制 |
| bucket_id = canonical `b* = floor(t_start/W)` | 排序 + 块断流 |
| Value 为完整 points | 含跨桶长段全程 |
| Ingest 结束 L0=0 | 数据全在 L1+（`DrainL0AllFiles`） |
| 跨桶召回 | 靠 st_file_meta 时间窗，不靠复制 |

---

## 八、代码指针速查

| 设计要素 | 代码位置 |
|----------|----------|
| **M_manager + MANIFEST-b** | `db/manifest_ops.h/cc`、`db/version_set.cc:ProcessManifestWrites` |
| **Manifest 分片触发** | `version_set.cc:8485`（file_size >= tuned_max） |
| **st_file_meta 写入** | `db/compaction/compaction_outputs.cc`（建表路径） |
| **st_file_meta 查询粗筛** | `db/manifest_ops.h:ManifestFilterSstByStFileMeta` |
| **SST 金字塔 key cap** | `db/compaction/st_event_split_policy.h:ComputeStEventSplitLevelPolicy` |
| **SST 切分触发** | `db/compaction/compaction_outputs.cc:ShouldStopBeforeEventTime` |
| **Data Block MBR 断流** | `table/block_based/st_mbr_flush_block_policy.cc` |
| **Index block value ST 元数据** | `table/block_based/index_builder.cc` |
| **Local 查询剪枝** | `table/block_based/block_based_table_reader.cc` |
| **E7 Key 格式** | `rocks-demo/st_segment_key.hpp` |
| **Ingest L0 清空** | `rocks-demo/st_bucket_ingest_build.cpp:final_st_drain` |
| **Compaction benchmark** | `rocks-demo/st_compaction_throughput_bench.cpp` |

---

## 九、关键指标

| 指标 | 目标 |
|------|------|
| `false_negative_count` | 0 |
| `ingest_l0_file_count` | 0 |
| `live_kv_count` | ≈ N |
| `query_result_dedup_count` | ≈0 |
| `cross_bucket_replicate_count` | 0 |
| STFP-R4 Read | 577 MB/s |
| STFP-R4 Write | 69 MB/s |

---

## 十、术语表

| 术语 | 定义 |
|------|------|
| **STFP** | Spatio-Temporal Flush Policy — ST-compaction 输出策略 |
| **STFP-R4** | STFP + max_runs=4，最终选型 |
| **E7** | 轨迹段 user key（0xE7, 31B） |
| **bucket_id** | `floor(t_start/W)`，排序用，非分区 |
| **host_bucket** | MANIFEST-b shard 所属 bucket |
| **segment_uid** | `(vehicle_id, segment_seq, chunk_seq)` |
| **st_file_meta** | 每 SST 全局 `[t_min,t_max]`+MBR |
| **M_manager** | 多 MANIFEST-b 的索引文件 |
| **MANIFEST-b** | per-bucket descriptor log |
| **event_time_split** | 文件级切分（time span + max_keys + bucket span） |
| **StMbrFlushBlockPolicy** | Data Block MBR 膨胀断流 |
| **CompactionMergingIterator** | 多路归并最小堆 |
| **LevelIterator** | 同层 SST 串联迭代器 |

─── 待决议问题清单 ───

1. [优先级] run-count vs score 调度次序
   当前: Trigger A → B → score → run-count
   争议: run-count 直接决定读性能（runs多→merge开销大），应优先于score（容量问题）
   位置: Algorithm 1 (PickForLevel) 和 SetupInitialFiles()

2. [已解决] S_hybrid 全局归一化 → 层级局部归一化
   修复: ComputeLevelEnvelope() 替代 ComputeGlobalEnvelope()
         Trigger A 使用本层文件范围做归一化，不再被20年全局跨度压扁
   位置: compaction_picker_level.cc: ComputeLevelEnvelope + Trigger A 调用处

3. [已解决] 跨bucket SST 共存
   代码已明确: Run-count trigger 和整层归并均可跨 bucket 选文件
         st_file_meta 时间窗保证跨 bucket 召回，无需按 bucket 独立
   位置: compaction_picker_level.cc: SelectBestMergeBatch 注释

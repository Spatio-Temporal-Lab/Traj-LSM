# ST-RocksDB 项目总览

> 本文档面向新加入的 AI Agent，一次性讲清楚我们在做什么、怎么做、做到哪了。

---

## 一、项目是什么

**ST-RocksDB** 是一个基于 Facebook RocksDB LSM-tree 存储引擎深度修改的**时空轨迹数据存储引擎**。目标：支持 100% 召回（零假阴性）的轨迹段时空查询。

核心改动：
- 在 RocksDB 上增加了**时空索引**（SST 文件级 + Data Block 级）
- 实现了**ST-compaction**（按层整层归并 + MBR 块断流）
- 实现了**多层查询剪枝**（Manifest → SST file meta → Index block value → 点真判）

---

## 二、仓库顶层结构

```
D:\Project\
├── rocksdb/              ← ST-RocksDB fork 源码（C++ 引擎，核心改动都在这里）
├── rocksdb_vanilla/      ← 未修改的原生 RocksDB（A/B 对照实验用）
├── rocks-demo/           ← C++ 实验/诊断工具（灌库、查询、compaction benchmark）
├── tools/                ← Python/PowerShell 脚本（数据生成、实验编排、画图）
├── scripts/              ← 快捷 PowerShell 脚本
├── data/                 ← 数据集 + 灌好的 DB（不入 git）
├── Result/               ← 实验结果 TSV + 图片（不入 git）
├── Tman-RocksDB/         ← Java 独立项目：TMan-RocksDB HTTP 服务端
├── TMan-spatial/         ← Java 独立项目：TMan 空间查询引擎
├── TMan-tsp/             ← Java 独立项目：TMan-tsp 时序空间处理
├── ST-RocksDB_TiDB_交付包/ ← 交付包：patch + 样本数据 + TiDB 集成指南
├── vcpkg/                ← C++ 包管理器（Snappy）
├── .claude/              ← Claude Code 配置
│
├── Mind.md               ← **设计定稿（最终口径）** — 最重要文档
├── MANUAL.md             ← 完整用户手册（编译/灌库/查询/实验）
├── README.md             ← 快速开始入口
└── CLAUDE.md             ← 本文件（Agent 总览）
```

**注意**：`Tman-RocksDB/`、`TMan-spatial/`、`TMan-tsp/` 是 Java 独立项目，与 C++ ST 引擎无编译依赖，仅通过文件/网络交互。

---

## 三、核心架构（定稿）

### 3.1 整体流程

```text
Ingest（CSV → SST → DB）
  → 每个 segment_uid 一条 E7 KV（canonical bucket_id + 完整 value）
  → unified memtable / EOF flush → L0=0，数据全在 L1+
  → K ≈ N（每 segment_uid 恰好一条 KV，无跨桶复制）
      ↓
ST-compaction（L1+ 整层归并）
  → 层间整理 / 块 MBR 断流 / 文件 meta
  → 删旧 SST、写新 SST
  → Version 边界 Manifest + st_file_meta 同步
      ↓
Query（多层剪枝，保证 100% 召回）
  → m_manager（仅选出 MANIFEST 文件）
  → MANIFEST replay → 按 SST st_file_meta 时间窗 AND 空间 MBR 粗筛
  → Local（查询窗→bucket_id Seek + index block value 时空硬判定）
  → KV 解压 + 点真判
```

### 3.2 核心设计原则

1. **单 Manifest**：`CURRENT` → `MANIFEST-*` 单链，与原生 RocksDB 一致。每个物理 SST 登记一次。
2. **无跨桶复制**：每 `segment_uid` 恰好一条 KV；不因段跨日历桶而写多份副本。
3. **Per-SST st_file_meta**：每个 SST 携带 `[t_min, t_max]` + 空间 MBR（来自 Value 头全段并集，非 key 的 bucket_id 推短窗）。
4. **100% 召回保证**：G1（Manifest）+ L1（file meta）+ L2（保守策略：元信息缺失直接报错）+ 点真判。
5. **E7 Key 格式**：Magic 0xE7，canonical `bucket_id = floor(t_start/W)`，z-order 空间曲线排序。
6. **m_manager 轻量化**：仅做 MANIFEST 文件选择，不做时空过滤，不持有空间信息。

### 3.3 不漏解充分条件（定稿）

对查询窗 `W_q` 与段 `s`，不漏解 ⟺ 下列 AND：

| 跳 | 合同 | 说明 |
|----|------|------|
| G1 Manifest | 单 MANIFEST replay 得全部 live SST；`st_file_meta` 时间相交 AND MBR 空间相交才进候选；meta 缺失 → 报错 | 一次 IO |
| L1 meta | `st_file_meta` / block tail 的 `[t_min,t_max]`、MBR 来自 Value 头全段，非仅用 key bucket_id 推短窗 | 跨桶长段不误剪 |
| L2 保守 | meta 缺失 / value 解码失败 → 直接报错（不降级为 match-all） | 架构高度依赖元信息 |
| 点真 | MBR 相交后解压 value，存在一点 ∈ W_q 才命中 | 控 FP，与 FN 无关 |

---

## 四、Key 格式（E7，定稿）

### E7 Key Layout（32 字节，big-endian，memcmp 可排序）

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1B | `magic` | 0xE7 |
| 1 | 1B | `type_flags` | 新库恒为 0（历史库 bit0=1 为 ghost key，已废弃）|
| 2 | 4B | `bucket_id` (u32 BE) | canonical 日历桶 `b* = floor(t_start/W)` |
| 6 | 8B | `zorder` (u64 BE) | 空间 Morton 编码（质心格网，用于排序聚簇）|
| 14 | 4B | `t_start_offset` (u32 BE) | 相对 canonical 桶起点秒数 |
| 18 | 8B | `vehicle_id` (u64 BE) | 车辆 ID |
| 26 | 4B | `segment_seq` (u32 BE) | 段序号 |
| 30 | 2B | `chunk_seq` (u16 BE) | 切分序号 |

排序语义：`bucket_id`（粗时间）→ `zorder`（空间）→ `t_start_offset`（细时间）→ id 字段打破平局。

### TMan ST Key（变长，新库默认）

```
[2B period BE][8B spatial BE][tid UTF-8...]
```
- `period` = `TimeRangeIndex.periodNumber(t_start_ms)`
- `spatial` = z_order Morton 编码（resolution=10, α=β=4）
- 与 TMan-tsp `TrajPutUtil` ST 主索引一致

### 逻辑段唯一 ID

`segment_uid = (vehicle_id, segment_seq, chunk_seq)` — 去重键，**不含 bucket_id**。

---

## 五、Global Index（单 Manifest + SST 级时间 meta）

### 组件

| 组件 | 职责 |
|------|------|
| `CURRENT` | 1 个文件，单行指针指向当前 `MANIFEST-*` |
| `MANIFEST-*` | 标准 descriptor log，增量追加 VersionEdit；replay 得全库 live SST |
| `st_file_meta` | 每个 SST 的全局 `[t_min,t_max]` + MBR；建表时从 Value 头全段并集 |
| `m_manager` | **仅负责选出 MANIFEST 文件**；不做时空过滤，不持有空间信息 |

### 查询链路（端到端）

| 阶段 | 输入 | 动作 | 漏召回策略 |
|------|------|------|------------|
| Global — m_manager | 无 | 仅选出 MANIFEST 文件 | 不持有空间信息 |
| Global — Manifest | q_start, q_end, 空间 MBR | replay → live SST；file meta 时间 AND 空间相交筛候选 | meta 缺失 → 报错 |
| 查询窗转换 | q_start, q_end | 窗口→bucket_id 范围 | — |
| Local — 粗定位 | 最小 bucket_id | index block Seek 起始位置；向后遍历 | 从最小 bucket_id 开始 |
| Local — 硬判定 | index block value | 检查全局时空信息 | 硬性条件，不满足则跳过 block |
| Local — 终止 | index block key bucket_id | key bucket_id < 最小查询 bucket_id → 安全停止 | key 有序单增 |
| KV | 候选 block | 解压 value；遍历轨迹点 | 相交即保留整段 |
| 后处理 | 命中 KV | 按 segment_uid 去重（异常兜底） | 定稿态应 ≈0 |

### 历史方案（已废弃）

- `CURRENT` → `M_manager-*` → 分片 `MANIFEST-b`：按日历桶切 Manifest，SST 经 S(F) 扇出到多分片。**定稿废弃**，迁移到单 Manifest。
- Master/Ghost + `experimental_global_*`：跨桶复制方案。**定稿废弃**。

---

## 六、Local Index（SST 内剪枝）

### Index Block 改造

在 index entry 的 **value** 中加入了该 block 的全局时空信息：

| 字段 | 位置 | 用途 |
|------|------|------|
| `bucket_id` | index block **key** | 粗粒度 Seek + 终止判定 |
| 全局 `[t_min, t_max]` + MBR | index block **value** | **硬性条件**：时间 AND 空间都相交才读 Data Block |

### 两阶段剪枝

**阶段 1 — 粗定位（bucket_id Seek）**：
- 查询窗转为 bucket_id 范围，用最小 bucket_id 做 Seek
- `bucket_id` 只能判定**终止位置**（key 有序单增，bucket_id < 查询最小 → 后面一定没有）
- `bucket_id` **不能**判定起始位置（跨桶长段可能从更早 bucket_id 开始）

**阶段 2 — 硬判定（index block value 时空信息）**：
- 对每个 index entry 检查其 value 中的全局 `[t_min, t_max]` + MBR
- 时间 AND 空间都相交 → 读 Data Block
- 不满足 → 安全跳过（硬性条件，不漏召回）

### 点真判

- 读 Segment value 头（28B：t_start/t_end/MBR）做轻量判断
- 仍不能排除时，解压完整 points，遍历所有轨迹点
- 任一点落入查询窗才计为命中

---

## 七、ST-Compaction

### 三条通道

1. **L0→L1 下移通道**：仅做阶段迁移（StageDown），不做跨文件归并。Ingest 完成后 L0 文件数必须为 0。
2. **ST-compaction 通道（L1+）**：物理 SST 归并，整层泄洪 + 时空近邻簇 + Data Block MBR 断流。
3. **Manifest 逻辑通道**：ST 完成条件 — 单 MANIFEST VersionEdit 与 st_file_meta 与 SST 变更对齐。

### 触发机制

- **L0→L1**：每次 Flush 产生 L0 SST 后，StageDown 到 L1（`StageDownNewestL0File`）
- **Lx→Lx+1（x≥1）**：当 `sum(file_size on Lx) ≥ 0.9 × MaxBytesForLevel(Lx)` 时触发整层归并

### 整层归并流

```text
整层 Lx SST ──LevelIterator──┐
                              ├── 最小堆 → CompactionIterator → TableBuilder（块级 MBR 断流 + 文件级大小/时间切分）
Lx+1 overlap SST ─LevelIterator┘                              → 新 SST
                                                                    ↓
                                                          Version 边界: Manifest + st_file_meta
```

### 金字塔输出策略（`st_event_split_policy.h`）

| 层 | max_keys/文件 | max_bucket_span | 说明 |
|----|--------------|-----------------|------|
| L1 | 10,000 | 64 | 小文件，好剪枝 |
| L2 | 40,000 | 128 | |
| L3 | 160,000 | 256 | |
| L4+ | 无上限 | 512 | 大文件，顺序 I/O |

### Data Block MBR 断流（`StMbrFlushBlockPolicy`）

在 `BlockBasedTableBuilder::Add` 流式写盘时判定是否 Flush Data Block：
- 跨 `bucket_id` → Flush
- 空间 MBR 膨胀率 ≥ `experimental_compaction_spatial_soft_expansion_ratio`（缺省 2.5）→ Flush
- 受标准 `FlushBlockBySizePolicy` 约束

**废弃**：文件级 MBR soft/hard 切分（破坏大文件连续写）。

### Ingest 完成条件

1. EOF Flush（最后一批 memtable）
2. `DrainL0AllFiles`：循环 StageDown 直至 **L0=0**
3. L1+ ST ladder（按层 90% 字节阈值）
4. 验收：`st_level_stats` L0=0；`false_negative_count=0`

---

## 八、关键数据结构

### `SpatioTemporalBlockMeta`（`rocksdb/table/st_meta_index_extension.h`）

```cpp
struct SpatioTemporalBlockMeta {
  uint32_t t_min = 0;
  uint32_t t_max = 0;
  float x_min = 0.f, y_min = 0.f;
  float x_max = 0.f, y_max = 0.f;
  std::array<uint64_t, 4> bitmap_words{};  // 16×16 网格 bitmap（查询不用，写全 1）
};
```

### `StSegmentUid`（`rocksdb/table/st_meta_user_key.h`）

```cpp
struct StSegmentUid {
  uint64_t vehicle_id = 0;
  uint32_t segment_seq = 0;
  uint16_t chunk_seq = 0;
};
```

### `StEventSplitLevelPolicy`（`rocksdb/db/compaction/st_event_split_policy.h`）

```cpp
struct StEventSplitLevelPolicy {
  uint32_t max_bucket_span = UINT32_MAX;
  uint32_t effective_max_time_span = 0;
  uint32_t effective_max_keys = UINT32_MAX;
  double spatial_ratio_mul = 1.0;
  double hard_min_file_size_mul = 1.0;
};
```

---

## 九、源码改动地图（ST 相关）

### rocksdb/include/rocksdb/ — 公开头文件

| 文件 | 改动 |
|------|------|
| `options.h` / `advanced_options.h` | 新增 ST 选项：`experimental_st_compaction`、`experimental_st_compaction_real_tiered_l1_l2`、`experimental_st_compaction_stfp_enable`、`experimental_st_block_mbr_flush_enable`、`experimental_st_prune_scan`（查询窗 t_min/t_max/x/y）、`has_st_file_meta` |
| `table.h` | 引用 `UserDefinedIndexFactory` |
| `db.h` | 基本不变，透传 ST 选项 |
| `sst_file_writer.h` | 支持 ST 元数据 |
| `table_properties.h` | ST 属性扩展 |
| `metadata.h` | `SpatioTemporalBlockMeta` 相关字段 |

### rocksdb/db/ — 数据库核心

| 文件 | 改动内容 |
|------|----------|
| `manifest_ops.cc/.h` | **核心**：多 shard manifest 管理、`ManifestFilterSstByStFileMeta`、`GetLiveSstLevelStatsFromMManager`、per-SST st_file_meta 过滤 |
| `version_edit.h` | `FileMetaData` 扩展：`has_st_file_meta`、`st_file_meta`（SpatioTemporalBlockMeta）、`st_segment_count` |
| `version_set.h` | 注释引用 per-bucket manifest routing |
| `version_set.cc` | LogAndApply / manifest 同步 |
| `version_builder.cc/.h` | 构建时设置 `meta->has_st_file_meta = true` |
| `version_edit_handler.cc/.h` | VersionEdit 处理 |
| `builder.cc` | 设置 `has_st_file_meta` + 复制 `st_file_meta` |
| `db_impl.cc/.h` | DB 打开/恢复逻辑 |
| `db_impl_compaction_flush.cc` | compaction/flush 调度 |
| `db_iter.cc/.h` | ST 感知的迭代器 |
| `column_family.cc` | ST 选项处理 |
| `external_sst_file_ingestion_job.cc` | 外部 SST ingest 时的 ST 元数据 |
| `repair.cc` | DB 修复 |
| `compaction/compaction_picker_level.cc` | **ST 整层 picker**：`TryPickWholeLevelNonL0ST`，ST 路由 vs 标准 picker |
| `compaction/compaction_outputs.cc/.h` | ST 输出切分：`st_compaction_uid_dropped_` |
| `compaction/st_event_split_policy.h` | **新增**：层感知 ST 输出策略、金字塔尺寸 |

### rocksdb/table/ — SST 表和索引

| 文件 | 改动内容 |
|------|----------|
| `st_meta_index_extension.h` | **新增**：`SpatioTemporalBlockMeta` 结构体、bitmap 操作 |
| `st_meta_user_key.h/.cc` | **新增**：E5/E6/E7 key 解析、TMan ST key 解析、block meta 聚合、Seek 下界构造、`StSegmentUid` 解码、`StQuerySegmentUidDedup` 去重 |
| `tman_rocksdb_value.h/.cc` | **新增**：TMan value 格式（含 STPT trailer） |
| `block_based/st_mbr_flush_block_policy.h/.cc` | **新增**：MBR 驱动的 FlushBlockPolicy |
| `block_based/index_builder.h/.cc` | 改动：合并时空 block meta 到 index entry |
| `block_based/block_based_table_builder.cc/.h` | 改动：调用 ST flush block policy + meta 收集 |
| `block_based/block_based_table_reader.cc/.h` | 改动：ST 读时剪枝 |
| `block_based/block_based_table_factory.cc/.h` | 改动：组装 ST 工厂 |
| `block_based/user_defined_index_wrapper.h` | 新增：用户自定义索引封装 |

### rocksdb/options/ — 选项系统

| 文件 | 改动 |
|------|------|
| `cf_options.cc/.h` | ST 选项的 column family 集成 |
| `options.cc` | ST 选项初始化和验证 |
| `options_helper.cc` | ST 选项的字符串/枚举映射 |

### rocksdb/file/ — 文件命名

| 文件 | 改动 |
|------|------|
| `filename.cc/.h` | MANIFEST / CURRENT / M_manager 文件命名规则 |

---

## 十、rocks-demo/ 工具集

### 构建

- CMake ≥ 3.20，C++20，MSVC Ninja，Release 模式
- 链接 `rocksdb.lib`（ST 引擎）+ `shlwapi` + `rpcrt4`
- 同时支持链接 vanilla RocksDB 做 A/B 对照

### 主要工具

| 工具 | 功能 |
|------|------|
| `st_bucket_ingest_build` | **灌库**：CSV → SST → DB。支持 `--user-key-schema e7|tman_st|tman_locs`、`--experimental-st-compaction`、EOF drain |
| `st_meta_read_bench` | **查询 benchmark**：时间窗 + 空间 MBR 剪枝查询 |
| `st_meta_read_bench_batch` | 批量查询 benchmark |
| `st_meta_compact_existing` | 对已有 DB 执行 ST compaction |
| `st_meta_compaction_verify` | 验证 compaction 正确性 |
| `st_compaction_bench` | 多 DB compaction 对比（ST vs Leveled vs Tiered） |
| `st_compaction_throughput_bench` | 5 策略吞吐量对比 |
| `st_mixed_workload_bench` | 混合读写吞吐量 benchmark |
| `st_meta_smoke` | 灌库验证 / 冒烟测试 |
| `st_level_stats` | 打印每层 SST 统计（文件数、大小、L0 文件数） |
| `st_prune_stats` | 打印剪枝统计 |
| `st_table_props` | 打印 SST table properties |
| `st_manifest_set_inspect` | 检查 manifest set 结构 |
| `st_global_ghost_compact` | Global ghost compaction |
| `traj_db_audit` | DB 审计（`CURRENT` → `MANIFEST-*`、live SST 与 Version 一致性） |
| `traj_db_repair` | DB 修复 |
| `trajectory_validate` | 轨迹数据验证 |
| `st_vanilla_kv_stream_ingest` | 原生 RocksDB ingest（A/B 对照） |
| `st_segment_window_scan_vanilla` | 原生 RocksDB 段窗口扫描 |
| `st_cross_bucket_ingest` | 跨桶 ingest 测试 |
| `manifest_bucket_rtree_validate` | Manifest bucket R-tree 验证 |

---

## 十一、关键配置选项

### ST Compaction 选项

| 选项 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `experimental_st_compaction` | bool | false | 启用 ST compaction |
| `experimental_st_compaction_l1_l2_tiered` | bool | false | L1+L2 tiered compaction |
| `experimental_st_compaction_real_tiered_l1_l2` | bool | false | 真 tiered（非 hybrid） |
| `experimental_st_compaction_stfp_enable` | bool | false | STFP 模式（spatio-temporal file placement） |
| `experimental_st_compaction_tiered_max_level` | int | ... | Tiered 最大层 |
| `experimental_st_compaction_tiered_min_level` | int | ... | Tiered 最小层 |
| `experimental_st_compaction_tiered_max_runs_per_level` | int | ... | 每层最大 run 数 |
| `experimental_st_block_mbr_flush_enable` | bool | false | MBR 驱动的 block flush |
| `experimental_st_compaction_level_trigger_ratio` | double | ... | 触发比率 |

### ST 查询选项（ReadOptions）

| 选项 | 说明 |
|------|------|
| `experimental_st_prune_scan` | 查询窗口：`t_min`、`t_max`、`x_min`、`x_max`、`y_min`、`y_max` |

---

## 十二、关键技术术语表

| 术语 | 含义 |
|------|------|
| **ST** | Spatio-Temporal，时空 |
| **STFP** | Spatio-Temporal File Placement，时空文件放置 |
| **ST-compaction** | 本项目定义的 L1+ SST 物理归并策略（整层、时空聚簇、MBR 断流） |
| **Global Index** | m_manager + Manifest 层：列出 live SST，用 st_file_meta 做 file 级时间+空间粗筛 |
| **Local Index** | SST 内 index block 层：利用 bucket_id Seek + index block value 时空信息做硬判定 |
| **E7** | 当前 segment key 格式（magic 0xE7，32B） |
| **TMan ST** | 新库默认 key 格式（变长，period + spatial + tid） |
| **canonical bucket_id** | `b* = floor(t_start/W)`，每 segment_uid 仅一条 key 用此 bucket_id |
| **segment_uid** | `(vehicle_id, segment_seq, chunk_seq)`，逻辑去重键 |
| **st_file_meta** | 每个 SST 的全局 `[t_min, t_max]` + MBR，用于 Manifest 级文件筛选 |
| **m_manager** | 轻量组件，仅选 MANIFEST 文件，不做时空过滤 |
| **Manifest** | RocksDB descriptor log（`MANIFEST-*`），存 VersionEdit 链 |
| **VersionEdit** | 一次 AddFile/DeleteFile/level 变更记录 |
| **MBR** | Minimum Bounding Rectangle，最小包围矩形 |
| **Block Meta** | Data Block 的全局时空元信息，嵌在 index block entry value 中 |
| **Uid-Merge** | 历史库去重机制（同 segment_uid 的重复 key 合并），新库不需要 |
| **L0 Drain** | Ingest 结束后将 L0 全部下移到 L1，确保 L0=0 |
| **Z-order / Morton** | 空间填充曲线编码，用于 key 内空间聚簇排序 |
| **W / bucket_seconds** | 日历桶宽度（默认 3600 秒=1 小时，或 86400 秒=1 天） |

---

## 十三、实现阶段与优先级

| 阶段 | 内容 | 状态 |
|------|------|------|
| **P0** | E7 定长 key；canonical bucket_id；单 key ingest（无跨桶复制）；ingest 幂等 | 基本完成 |
| **P1** | **单 Manifest**（`CURRENT` → `MANIFEST-*`）；per-SST st_file_meta；查询 file 时间筛 | **进行中**（当前代码仍是 M_manager 路径） |
| **P2** | Local：index block value 硬判定 + 点真判 | 已接入 |
| **P3** | ST-compaction + Manifest 同步 + 指标 | 物理 ST + 整层 picker 已有；单 Manifest 闭环待完成 |

### 当前代码与现实目标的差距

- **`CURRENT` 仍指向 `M_manager-*`**，尚未迁移到单 `MANIFEST-*`
- **S(F) 扇出**、**`st_manifest_host_bucket`** 等旧代码仍存在（历史兼容）
- **单 Manifest** 是当前首要实现目标

---

## 十四、指标（必须的验收指标）

| 指标 | 要求 | 说明 |
|------|------|------|
| `false_negative_count` | **恒为 0** | 假阴性数 |
| `live_kv_count` | ≈ N | 盘上 KV 条数 |
| `ingest_l0_file_count` | ingest 结束 = **0** | 用 `st_level_stats` 验收 |
| `query_result_dedup_count` | ≈ 0 | 定稿态无复制 |
| `cross_bucket_replicate_count` | 新库 = 0 | 跨桶复制计数 |
| `compaction_uid_dropped` | 新库 ≈ 0 | Uid-Merge 丢条数 |
| `manifest_sync_files_added/deleted` | — | Manifest 同步计数 |

---

## 十五、数据集

- **T-Drive**：北京出租车 GPS 轨迹
- **Geolife**：微软 Geolife 轨迹数据集
- **Wuxi**：无锡出租车数据

预处理流水线：
1. 原始数据 → `tools/unify_datasets*.py` → 统一格式 JSONL
2. JSONL → `tools/generate_*.py` → segments_meta.csv + segments_points.csv + segments_st_key.csv
3. CSV → `st_bucket_ingest_build` → RocksDB SST → DB

---

## 十六、重要文件速查

| 要查什么 | 看哪个文件 |
|----------|------------|
| 设计定稿（权威口径） | `Mind.md` |
| 用户手册（怎么跑） | `MANUAL.md` |
| 公开 ST 选项定义 | `rocksdb/include/rocksdb/options.h`、`advanced_options.h` |
| 时空 block meta 结构 | `rocksdb/table/st_meta_index_extension.h` |
| Key 格式/E7 解析/UID 解码 | `rocksdb/table/st_meta_user_key.h` |
| ST compaction 金字塔策略 | `rocksdb/db/compaction/st_event_split_policy.h` |
| 整层 ST picker | `rocksdb/db/compaction/compaction_picker_level.cc`（搜 `TryPickWholeLevelNonL0ST`） |
| Manifest 操作/ST 过滤 | `rocksdb/db/manifest_ops.cc` |
| FileMetaData（st_file_meta 字段） | `rocksdb/db/version_edit.h` |
| MBR flush block policy | `rocksdb/table/block_based/st_mbr_flush_block_policy.h` |
| 灌库工具 | `rocks-demo/st_bucket_ingest_build.cpp` |
| 查询 benchmark | `rocks-demo/st_meta_read_bench.cpp` |
| CMake 构建 | `rocks-demo/CMakeLists.txt` |
| 实验脚本 | `tools/` 目录 |

---

## 十七、Build 环境

- **OS**：Windows 10/11 x64
- **编译器**：Visual Studio 2022（MSVC）
- **构建系统**：CMake ≥ 3.20 + Ninja
- **C++ 标准**：C++20
- **依赖**：Snappy（通过 vcpkg）、shlwapi、rpcrt4
- **构建命令**：
  ```powershell
  cd D:\Project\rocks-demo\build
  cmake --build . --config Release
  ```

---

## 十八、废弃的概念（不要在新代码中使用）

| 废弃概念 | 原因 |
|----------|------|
| 跨桶完整 value 复制 | 写放大，定稿用 st_file_meta 全段窗替代 |
| Master/Ghost key | 旧跨桶复制方案，定稿废弃 |
| `type_flags` bit0 ghost 语义 | 新库恒为 0 |
| Per-bucket flush（`--per-bucket-flush`） | 会导致 L0 堆积 ~146 SST |
| `FlushOptions.st_manifest_host_bucket` | 单 Manifest 下无分片登记 |
| `experimental_global_master_ghost` | 全局 ghost 方案废弃 |
| `M_manager` + 分片 MANIFEST-b | 迁移到单 Manifest |
| S(F) fan-out | 每个 SST 向多分片重复登记，定稿废弃 |
| 文件级 MBR soft/hard 切分 | 已收拢到 Data Block FlushBlockPolicy |
| `MANIFEST_SET` | 更旧的混合分片方案 |

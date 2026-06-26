# ST-RocksDB 用户手册

> 版本：2026-06 · 适用于当前 `Global` 分支的 ST-RocksDB fork

---

## 目录

1. [概述](#1-概述)
2. [仓库结构](#2-仓库结构)
3. [环境与编译](#3-环境与编译)
4. [数据准备](#4-数据准备)
5. [灌库（Ingest）](#5-灌库ingest)
6. [查询与 Benchmark](#6-查询与-benchmark)
7. [Compaction](#7-compaction)
8. [数据库检查与诊断](#8-数据库检查与诊断)
9. [实验脚本](#9-实验脚本)
10. [配置参考](#10-配置参考)
11. [关键概念](#11-关键概念)
12. [故障排除](#12-故障排除)

---

## 1. 概述

ST-RocksDB 是一个基于 RocksDB 的**时空轨迹数据存储引擎**。它在 RocksDB 的 LSM-tree 基础上增加了：

- **时空索引**：SST 文件级和 block 级的时空元数据（`st_file_meta`、index block value），用于查询时剪枝
- **ST-compaction**：按层整层归并 + 时空近邻聚簇 + Data Block MBR 断流
- **单 Manifest**：标准 `MANIFEST-*` 文件，每个 SST 登记 `st_file_meta`（时间窗 + 空间 MBR）

核心设计文档见 `Mind.md`。

---

## 2. 仓库结构

```
D:\Project\
├── rocksdb/                ← ST-RocksDB fork 源码（引擎层）
│   ├── db/                 ←   数据库核心（version_set, compaction, manifest）
│   ├── table/              ←   SST 格式、index block ST 扩展
│   └── include/rocksdb/    ←   公开头文件（options, db）
│
├── rocks-demo/             ← ST 实验工具（应用层）
│   ├── CMakeLists.txt      ←   构建配置
│   ├── st_bucket_ingest_build.cpp   ← 灌库工具
│   ├── st_meta_read_bench.cpp       ← 查询 benchmark
│   ├── st_meta_compact_existing.cpp ← ST-compaction 工具
│   ├── st_level_stats.cpp           ← SST 层级统计
│   ├── st_meta_smoke.cpp            ← 灌库验证/冒烟测试
│   └── ...                          ← 其他诊断/辅助工具
│
├── rocksdb_vanilla/        ← 原生 RocksDB（A/B 对照，不参与构建）
│
├── tools/                  ← Python/PowerShell 辅助脚本
│   ├── generate_*.py       ←   查询窗口/数据集生成
│   ├── run_tdrive_*.py     ←   实验编排
│   ├── plot_*.py           ←   结果画图
│   ├── summarize_*.py      ←   结果汇总
│   ├── unify_datasets*.py  ←   数据预处理
│   ├── docker/             ←   Docker 构建配置
│   └── ...
│
├── scripts/                ← 快捷 PowerShell 脚本
│   ├── build-rocksdb.ps1
│   ├── ingest-tdrive.ps1
│   └── run-repeat-experiment.ps1
│
├── data/                   ← 数据集和数据库（不入 git）
│   ├── datasets/           ←   原始/中间数据集
│   ├── processed/          ←   预处理后的 CSV
│   └── db/                 ←   灌好的 RocksDB 数据库
│
├── Result/                 ← 实验输出（不入 git）
│   ├── TSV/                ←   benchmark TSV 结果
│   └── picture/            ←   图表
│
├── web/                    ← 实验看板 HTML
├── vcpkg/                  ← C++ 包管理器配置
├── .claude/                ← Claude Code 配置
│
├── Mind.md                 ← 设计定稿（实现口径）
├── MANUAL.md               ← 本文件
└── README.md               ← 入口（指向本文件）
```

### 独立子项目

位于仓库根目录的以下目录是**独立项目**，有自己的构建系统和仓库：

| 目录 | 语言 | 用途 |
|------|------|------|
| `Tman-RocksDB/` | Java | TMan-RocksDB 服务端（HTTP API 封装） |
| `TMan-spatial/` | Java | TMan 空间查询引擎 |
| `TMan-tsp/` | Java | TMan-tsp 时序空间处理 |

它们与 C++ ST 引擎**无直接编译依赖**，仅通过文件/网络交互。

---

## 3. 环境与编译

### 3.1 环境要求

| 组件 | 版本要求 |
|------|----------|
| Windows | 10/11 x64 |
| Visual Studio | 2022（含 MSVC v143 工具链） |
| CMake | ≥ 3.20 |
| Ninja | 推荐（VS 自带或单独安装） |
| Python | ≥ 3.9 |
| Git | ≥ 2.40 |

### 3.2 编译 RocksDB 静态库

```powershell
cd D:\Project\rocksdb
# 使用 VS 2022 打开 build_vs/rocksdb.sln，Release x64 编译
# 产物：D:\Project\rocksdb\build_vs\rocksdb.lib
```

### 3.3 编译 Demo 工具

```powershell
cd D:\Project

# 配置（只需一次）
cmake -S .\rocks-demo -B .\rocks-demo\build -G Ninja `
  -DROCKSDB_BUILD=D:\Project\rocksdb\build_vs

# 编译全部工具
cd .\rocks-demo\build
cmake --build . --config Release

# 或只编译单个目标
cmake --build . --config Release --target st_bucket_ingest_build
cmake --build . --config Release --target st_meta_read_bench
cmake --build . --config Release --target st_level_stats
```

编译成功后，可执行文件在 `D:\Project\rocks-demo\build\` 下。

### 3.4 所有构建目标一览

| 目标名称 | 分类 | 用途 |
|----------|------|------|
| `st_bucket_ingest_build` | **灌库** | 从 CSV 批量灌入轨迹段数据 |
| `st_meta_read_bench` | **查询** | 读查询 benchmark（支持多种 prune 模式） |
| `st_meta_scan_bench` | **查询** | 全表扫描 benchmark |
| `st_meta_bench` | **查询** | 轻量查询 benchmark |
| `st_meta_compact_existing` | **Compaction** | 对已有 DB 执行 ST-compaction |
| `st_meta_compaction_verify` | **Compaction** | compaction 后验证数据完整性 |
| `st_global_ghost_compact` | **Compaction** | 清理历史 ghost 数据 |
| `st_level_stats` | **诊断** | 查看 DB 的 SST 层级和 ST 元数据覆盖 |
| `st_prune_stats` | **诊断** | 查看 Manifest/SST 剪枝统计 |
| `st_manifest_set_inspect` | **诊断** | 查看 Manifest shard 选择情况 |
| `st_table_props` | **诊断** | 查看 SST 文件 properties |
| `st_kv_sample_probe` | **诊断** | 采样查看 KV 内容 |
| `st_meta_smoke` | **测试** | 灌库后冒烟测试（验证查询正确性） |
| `traj_db_audit` | **审计** | 审计 DB 内轨迹数据一致性 |
| `traj_db_repair` | **修复** | 修复 DB 元数据问题 |
| `manifest_bucket_rtree_validate` | **测试** | 验证 Manifest bucket R-tree 索引 |
| `vanilla_db_live_audit` | **审计** | 原生 RocksDB 审计 |
| `st_fork_kv_stream_dump` | **工具** | 导出 KV 流 |
| `st_segment_window_scan_vanilla` | **对照** | 原生 RocksDB 扫描对照 |
| `st_vanilla_kv_stream_ingest` | **对照** | 原生 RocksDB 灌库对照 |
| `rocks_demo` | **示例** | 基础 demo 入口 |

---

## 4. 数据准备

### 4.1 数据集

当前使用的主要数据集：

| 数据集 | 路径 | 规模 |
|--------|------|------|
| TDrive | `data/datasets/tdrive/` | ~10K 出租车轨迹 |
| Wuxi | `data/wuxi_daily_zips/` | 无锡出租车日轨迹 |
| Unified | 上述两者合并 | 通过 `tools/unify_datasets.py` 生成 |

### 4.2 预处理流程

从原始数据到可灌库的 CSV，使用 `tools/unify_datasets_streaming.py`：

```powershell
cd D:\Project
python tools/unify_datasets_streaming.py --help
```

产出文件（放在 `data/processed/<dataset>/` 下）：

| 文件 | 内容 |
|------|------|
| `segments_meta.csv` | 段级元数据（uid, t_min, t_max, MBR, point_count） |
| `segments_points.csv` | 轨迹点序列（uid, unix_s, lon, lat） |
| `segments_st_key.csv` | TMan-format ST key（`tman_locs` schema 需要） |

### 4.3 生成查询窗口

预生成查询窗口 CSV 供 benchmark 使用：

```powershell
# TDrive stratified 12 窗口（均匀 + 倾斜）
python tools/generate_tdrive_stratified12_windows.py

# TDrive dense20 窗口（10 轮重复实验用）
python tools/generate_tdrive_dense20_st_windows.py

# Unified 数据集窗口
python tools/generate_unified_stratified12_windows.py

# 随机窗口生成器
python tools/generate_random_query_windows.py
```

---

## 5. 灌库（Ingest）

### 5.1 基本命令

```powershell
cd D:\Project\rocks-demo\build

.\st_bucket_ingest_build.exe `
  --segment-meta-csv "D:\Project\data\processed\tdrive\segments_meta.csv" `
  --segment-points-csv "D:\Project\data\processed\tdrive\segments_points.csv" `
  --out-sst-dir "D:\Project\data\db\tdrive_st_sst" `
  --target-db "D:\Project\data\db\tdrive_st" `
  --bucket-sec 86400 `
  --reset-out-sst-dir `
  --reset-target-db `
  --batch-ingest `
  --experimental-st-compaction `
  --user-key-schema tman_locs `
  --tman-st-keys-csv "D:\Project\data\processed\tdrive\segments_st_key.csv"
```

### 5.2 关键参数

| 参数 | 说明 |
|------|------|
| `--segment-meta-csv` | 段元数据 CSV（必填） |
| `--segment-points-csv` | 轨迹点 CSV（必填） |
| `--target-db` | 目标 DB 路径 |
| `--bucket-sec` | 时间桶宽度（秒），默认 3600 |
| `--user-key-schema` | key 格式：`tman_locs`（推荐）/ `tman_st` / `e7` |
| `--tman-st-keys-csv` | TMan ST key CSV（`tman_locs` 须提供） |
| `--batch-ingest` | 批量 ingest 模式 |
| `--experimental-st-compaction` | 启用 ST-compaction |
| `--reset-target-db` | **危险**：删除并重建目标 DB |
| `--reset-out-sst-dir` | 删除并重建 SST 中间目录 |
| `--st-ladder-drain-only` | 仅执行 drain（不重新灌库） |

### 5.3 灌库流程

```
1. 预处理 CSV  ──→  2. Ingest（写 SST）  ──→  3. EOF Drain L0  ──→  4. L1+ ST Ladder
```

**Ingest 完成条件**：`st_level_stats` 显示 `L0=0`，数据全部在 L1+。

### 5.4 灌库后验证

```powershell
.\st_level_stats.exe --db "D:\Project\data\db\tdrive_st"
.\st_meta_smoke.exe --db "D:\Project\data\db\tdrive_st"
```

---

## 6. 查询与 Benchmark

### 6.1 `st_meta_read_bench`（主要查询 benchmark）

```powershell
cd D:\Project\rocks-demo\build

# 基本用法：对单个窗口做 prune vs full scan 对比
.\st_meta_read_bench.exe `
  --db "D:\Project\data\db\tdrive_st" `
  --prune-t-min 1201930245 --prune-t-max 1202016645 `
  --prune-x-min 116.0 --prune-x-max 117.0 `
  --prune-y-min 39.0 --prune-y-max 41.0

# 只跑 prune（跳过 full scan baseline）
.\st_meta_read_bench.exe --db "..." --no-full-scan ...

# 批量查询（从文件读多个窗口）
.\st_meta_read_bench.exe --db "..." --query-batch-file windows.csv

# 迭代重复（测试缓存热身后性能）
.\st_meta_read_bench.exe --db "..." --iterator-repeat 10 ...
```

### 6.2 Prune 模式

| `--prune-mode` | 说明 |
|----------------|------|
| `sst_manifest` | **默认**：file-level + block-level + key-level 全开 |
| `sst` | 仅 block-level + key-level（无 file 跳过） |
| `manifest` | 仅 file-level（时间桶 + R-tree 索引跳过） |
| `sst_manifest_pipeline` | file-level + block-level（关 key-level） |
| `sst_manifest_adaptive` | 自适应开关 key-level（高 overlap 时自动关） |

### 6.3 关键指标

| 指标 | 含义 |
|------|------|
| `block_read_count` | block 读取次数（越少越好） |
| `wall_us` | 端到端延迟（微秒） |
| `prune_file_skipped` | 被 file-level prune 跳过的 SST 数 |
| `block_index_skipped_st_disjoint` | 被 block-level prune 跳过的 index entry 数 |
| `key_selectivity` | prune keys ÷ full scan keys（越低 prune 效果越好） |
| `false_negative_count` | **必须为 0** |

### 6.4 实验脚本（推荐）

大多数情况下用 `tools/run_tdrive_st_query_bench.py` 编排批量实验：

```powershell
cd D:\Project
python tools/run_tdrive_st_query_bench.py `
  --db D:\Project\data\db\tdrive_st `
  --suites unified_12
```

---

## 7. Compaction

### 7.1 ST-compaction 触发

ST-compaction 由 `experimental_st_compaction` 选项控制。触发逻辑：

- **L0→L1**：ingest 完成后显式 StageDown（不依赖后台自动）
- **L1+**：当层字节数 ≥ `0.9 × MaxBytesForLevel(Lx)` 时触发整层归并

调度优先级：L0 清空 > L1+ 归并

### 7.2 `st_meta_compact_existing`（手动 compaction）

```powershell
cd D:\Project\rocks-demo\build

.\st_meta_compact_existing.exe `
  --db "D:\Project\data\db\tdrive_st" `
  --experimental-st-compaction `
  --max-background-jobs 4
```

### 7.3 Ghost 清理

```powershell
.\st_global_ghost_compact.exe `
  --db "D:\Project\data\db\tdrive_st" `
  --dry-run          # 先预览
```

去掉 `--dry-run` 执行实际清理。

---

## 8. 数据库检查与诊断

### 8.1 SST 层级统计

```powershell
.\st_level_stats.exe --db "D:\Project\data\db\tdrive_st"
```

输出：每个 level 的文件数、总字节、ST meta 覆盖率。

### 8.2 Prune 统计

```powershell
.\st_prune_stats.exe `
  --db "D:\Project\data\db\tdrive_st" `
  --t-min 1201930245 --t-max 1202016645 `
  --x-min 116.0 --x-max 117.0 --y-min 39.0 --y-max 41.0 `
  --per-shard
```

### 8.3 Manifest 检查

```powershell
.\st_manifest_set_inspect.exe `
  --db "D:\Project\data\db\tdrive_st" `
  --t-min 1201930245 --t-max 1202016645
```

### 8.4 KV 采样

```powershell
.\st_kv_sample_probe.exe --db "D:\Project\data\db\tdrive_st" --count 100
```

### 8.5 审计

```powershell
.\traj_db_audit.exe --db "D:\Project\data\db\tdrive_st"
.\traj_db_repair.exe --db "D:\Project\data\db\tdrive_st" --dry-run
```

---

## 9. 实验脚本

### 9.1 脚本组织

`tools/` 目录按功能分类：

| 前缀 | 用途 | 示例 |
|------|------|------|
| `generate_*` | 生成查询窗口/数据集 | `generate_tdrive_stratified12_windows.py` |
| `run_tdrive_*` / `run_unified_*` | 实验编排 | `run_tdrive_st_query_bench.py` |
| `run_tman_*` | TMan 相关实验 | `run_tman_spatial12_benchmark.sh` |
| `plot_*` | 画图 | `plot_tdrive_dense20_latest_bars.py` |
| `summarize_*` | 汇总统计 | `summarize_tdrive_dense20_10round_multi.py` |
| `verify_*` / `validate_*` | 验证 | `verify_tdrive_crossbucket_fn.py` |
| `unify_*` | 数据预处理 | `unify_datasets_streaming.py` |
| `emit_*` | Key 生成 | `emit_tdrive_tman_st_keys.py` |

### 9.2 常用实验流程

**重复实验（Repeat）**：
```powershell
cd D:\Project
# 1. 编译
cd rocks-demo\build; cmake --build . --config Release; cd D:\Project
# 2. 运行 benchmark
python tools/run_tdrive_st_query_bench.py --suites unified_12
# 3. 汇总结果
python tools/summarize_tdrive_dense20_10round_multi.py
# 4. 画图
python tools/plot_tdrive_dense20_latest_bars.py
```

**消融实验（Ablation）**：
```powershell
# Baseline（全功能）
python tools/run_tdrive_st_query_bench.py --suites unified_12 --prune-mode sst_manifest

# 关 block-level
python tools/run_tdrive_st_query_bench.py --suites unified_12 --prune-mode manifest

# 关 file-level
python tools/run_tdrive_st_query_bench.py --suites unified_12 --prune-mode sst
```

### 9.3 `scripts/` 快捷脚本

| 脚本 | 功能 |
|------|------|
| `build-rocksdb.ps1` | 一键编译 |
| `ingest-tdrive.ps1` | 一键灌库（TDrive） |
| `run-repeat-experiment.ps1` | 一键跑重复实验 |
| `run-ablation-experiment.ps1` | 一键跑消融实验 |
| `verify-repeat-results.ps1` | 验证实验结果 |

---

## 10. 配置参考

### 10.1 关键 RocksDB 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `experimental_st_compaction` | false | 启用 ST-compaction |
| `experimental_st_compaction_level_trigger_ratio` | 0.9 | 层字节触发阈值（90%） |
| `experimental_compaction_event_time_split` | false | 启用事件时间切分 |
| `experimental_compaction_event_time_bucket_width` | 0 | 时间桶宽度（秒） |
| `experimental_compaction_event_time_max_span` | 0 | SST 最大时间跨度（秒） |
| `experimental_compaction_event_time_max_point_keys` | 0 | SST 最大段数 |
| `experimental_compaction_spatial_soft_expansion_ratio` | 0 | Data Block MBR 膨胀阈值 |
| `experimental_global_master_ghost` | false | Global master/ghost 语义（废弃中） |
| `max_manifest_file_size` | 4 MB | Manifest 轮转硬下限 |
| `max_manifest_space_amp_pct` | 500 | Manifest 空间放大上限 (%) |

### 10.2 环境变量

| 变量 | 默认 | 说明 |
|------|------|------|
| `ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_ROUTE` | 空（关闭） | Manifest 分片路由（废弃中） |
| `ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_BUCKET_WIDTH_SEC` | 空（关闭） | 分片时间桶宽度 |
| `ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_MAX` | 8192 | 最大分片数 |

---

## 11. 关键概念

### 11.1 Manifest

RocksDB 的元数据日志，记录所有 live SST 文件、level、文件大小。ST 扩展在每个 SST 的 entry 中附加 `st_file_meta`（`[t_min, t_max]` + MBR）。

- **单 Manifest**：`CURRENT` → `MANIFEST-xxxxxx`（标准 RocksDB 方式）
- **轮转**：当 manifest 文件大小超过 `max(max_manifest_file_size, compacted_size × (1 + amp_pct/100))` 时自动压缩重写
- **查询路径**：replay Manifest → 对每个 SST 用 `st_file_meta` 做时间+空间粗筛 → 对候选 SST 做 Local 剪枝

### 11.2 时间桶（bucket_id）

每个 segment 的 canonical 时间桶 `b* = floor(t_start / W)`。用于：

- Key 排序（`bucket_id` 在 key 前缀）
- Data Block 断流（跨 `bucket_id` 时 Flush block）
- **非** Manifest 分片维度（单 Manifest 下不需要）

### 11.3 ST-compaction

- **整层输入**：某层触发时，该层**全部 SST** 进 job
- **输出**：优先单 SST，必要时按 `target_file_size` / 最大段数 / 最大时间跨度切开
- **块级 MBR**：`StMbrFlushBlockPolicy` 在 Data Block MBR 膨胀超过阈值时 Flush block
- **文件级 MBR 切分**：已废弃（仅块级）

### 11.4 查询剪枝链

```
m_manager（仅选 MANIFEST 文件）
  → Manifest replay → SST st_file_meta 时间+空间粗筛
    → Index block key bucket_id Seek 起始
      → Index block value 时空信息硬判定
        → Data Block → 解压 value → 点真判
```

---

## 12. 故障排除

### 12.1 编译问题

**`LNK2005` 重复定义**：
清理 CMake 缓存后重新配置：
```powershell
cd D:\Project\rocks-demo\build
cmake -S .. -B . -G Ninja -DROCKSDB_BUILD=D:\Project\rocksdb\build_vs
cmake --build . --config Release
```

**`LNK2019` 找不到符号**：
确认 `ROCKSDB_BUILD` 指向正确的目录，且 `rocksdb.lib` 在该目录下。

**找不到 MSVC 头文件**：
从 "x64 Native Tools Command Prompt for VS 2022" 执行编译。

### 12.2 运行时问题

**DB::Open 崩溃**：
检查编译类型：rocksdb.lib 是 Release，demo 也必须是 Release。Debug (/MDd) 与 Release (/MD) 混用会导致 CRT 不兼容。

**`false_negative_count > 0`**：
- 检查 Manifest 是否与 SST 同步（`st_level_stats` 看 `with_st_file_meta` 是否覆盖全部 SST）
- 检查 `st_file_meta` 的 `[t_min, t_max]` 是否来自 Value 头全段而非 key 的 `bucket_id`
- 确认 `experimental_st_compaction` 已启用

**L0 堆积**：
灌库完成后执行 `st_level_stats`，如果 `L0 > 0`：
```powershell
.\st_bucket_ingest_build.exe ... --st-ladder-drain-only
```

**灌库后查询慢**：
- 确认 block cache 大小合理（`--block-cache-mb`，建议 ≥ 64 MB）
- 重新打开 DB 后再跑查询（排除冷 OS 页缓存影响）

### 12.3 数据问题

**`with_st_file_meta=0`**：
说明 SST 在写入时未启用 ST 元数据。检查 `experimental_spatio_temporal_meta_in_index_value` table option 是否开启，然后重新灌库。

**manifest 太大**：
默认 `max_manifest_file_size=4MB`。如果 DB 有大量 SST，compacted manifest 可能较大，调大此值。反之，如果 compacted manifest 很小（<1MB），可以降到 1MB。

---

## 附录 A：常用路径速查

| 用途 | 路径 |
|------|------|
| RocksDB 源码 | `D:\Project\rocksdb\` |
| RocksDB 静态库 | `D:\Project\rocksdb\build_vs\rocksdb.lib` |
| Demo 源码 | `D:\Project\rocks-demo\` |
| Demo 构建目录 | `D:\Project\rocks-demo\build\` |
| Demo 可执行文件 | `D:\Project\rocks-demo\build\*.exe` |
| 辅助脚本 | `D:\Project\tools\` |
| TDrive 预处理数据 | `D:\Project\data\processed\tdrive\` |
| TDrive ST 数据库 | `D:\Project\data\db\tdrive_st\` |
| 实验输出 | `D:\Project\Result\` |

## 附录 B：设计文档

- `Mind.md` — 设计定稿（架构、查询语义、compaction 策略、Key 格式）
- `rocksdb/CLAUDE.md` — RocksDB 代码规范（上游）
- `rocksdb/HISTORY.md` — RocksDB 上游变更记录

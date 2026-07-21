# ST-RocksDB

基于 Facebook RocksDB LSM-tree 深度修改的**时空轨迹数据存储引擎**。支持 100% 召回（零假阴性）的轨迹段时空查询。

## 仓库结构

```
D:\Project\
├── rocksdb/              ← ST-RocksDB 源码（C++ 引擎）
├── rocksdb_vanilla/      ← 未修改的原生 RocksDB（A/B 对照）
├── rocks-demo/           ← C++ 工具集（灌库、查询、compaction benchmark）
├── TMan-tsp/             ← TMan 时空查询（HBase/Spark/Scala 后端）
├── TMan-spatial/         ← TMan 空间查询 benchmark 对比
├── code/                 ← 第三方源码（GeoMesa 等）
├── tools/                ← 工具脚本（分类到子目录）
│   ├── benchmark/        ← 查询 benchmark / 汇总 / 验证
│   ├── ingest/           ← 灌库脚本（ST-RocksDB / TMan）
│   ├── data/             ← 数据集预处理 / 转换 / 生成
│   ├── plot/             ← 画图脚本
│   ├── ablation/         ← 消融实验脚本
│   ├── paramsweep/       ← 参数扫描脚本
│   ├── report/           ← 报告生成
│   ├── server/           ← 服务器运维 / 部署
│   └── docker/           ← Docker 构建（rocksdbjni）
├── scripts/              ← PowerShell 快捷脚本
├── data/                 ← 数据集 + 灌好的 DB（不入 git）
│   ├── shared/           ← 共享查询窗口 / 样本数据
│   ├── processed/        ← 预处理后的数据集
│   ├── runs/             ← 灌库运行输出
│   ├── db/               ← 实验数据库（参数扫描等）
│   ├── sst/              ← SST 文件
│   └── raw/              ← 原始下载数据
├── Result/               ← 实验结果（不入 git）
│   ├── charts/           ← 所有图表 PNG/JPG
│   ├── ablation/         ← 消融实验结果
│   ├── paramsweep/       ← 参数扫描结果
│   ├── compare/          ← 对比实验结果
│   └── logs/             ← 实验日志
├── docs/                 ← 文档 / 报告 / 设计
├── vcpkg/                ← C++ 包管理器（Snappy）
├── build/                ← CMake 构建输出
└── .claude/              ← Claude Code 配置
```

## 核心架构

```
Ingest（CSV → SST → DB）
  → 每个 segment_uid 一条 E7 KV（canonical bucket_id + 完整 value）
  → L0=0，数据全在 L1+
  → K ≈ N（每 segment_uid 恰好一条 KV，无跨桶复制）
      ↓
ST-compaction（L1+ 整层归并）
  → 整层 SST 输入 + Lx+1 overlap SST
  → Data Block MBR 断流 + 金字塔输出策略
  → 新 SST 写盘 + Manifest 同步
      ↓
Query（多层剪枝，100% 召回）
  → Manifest replay → 按 SST st_file_meta 时间窗 AND 空间 MBR 粗筛
  → Local（bucket_id Seek + index block value 时空硬判定）
  → KV 解压 + 点真判
```

## Manifest 设计

- **按轨迹段数切分**：每个 Manifest 约 10 万段，超过则轮转新 Manifest
- CURRENT → M_manager 索引多个 MANIFEST-*
- 当前代码同时支持单 MANIFEST-* 链（过渡态）和 M_manager 多分片路径
- 旧方案（M_manager + per-bucket MANIFEST-b、MANIFEST_SET、S(F) 扇出）已废弃

## E7 Key 格式（32 字节，big-endian）

| 偏移 | 大小 | 字段 |
|------|------|------|
| 0 | 1B | magic 0xE7 |
| 1 | 1B | type_flags（新库恒为 0，bit0 旧为 ghost） |
| 2 | 4B | bucket_id (u32 BE) = floor(t_start / bucket_seconds) |
| 6 | 8B | zorder (u64 BE) Morton 空间编码 |
| 14 | 4B | t_start_offset (u32 BE) 相对 canonical 桶起点秒数 |
| 18 | 8B | vehicle_id (u64 BE) |
| 26 | 4B | segment_seq (u32 BE) |
| 30 | 2B | chunk_seq (u16 BE) |

排序：`bucket_id` → `zorder` → `t_start_offset` → id 字段

segment_uid = (vehicle_id, segment_seq, chunk_seq)，不含 bucket_id

## 关键数据结构

- **SpatioTemporalBlockMeta** (`rocksdb/table/st_meta_index_extension.h`)：t_min/t_max + MBR(x/y min/max) + bitmap
- **StSegmentUid** (`rocksdb/table/st_meta_user_key.h`)：vehicle_id + segment_seq + chunk_seq
- **StEventSplitLevelPolicy** (`rocksdb/db/compaction/st_event_split_policy.h`)：金字塔输出参数

## 源码地图

### rocksdb/include/rocksdb/ — 公开头文件
| 文件 | 内容 |
|------|------|
| `options.h` / `advanced_options.h` | ST 选项定义 |
| `table.h` / `db.h` | 接口透传 |

### rocksdb/db/ — 数据库核心
| 文件 | 内容 |
|------|------|
| `manifest_ops.cc/.h` | Manifest 读写、SST 时空过滤、m_manager 管理 |
| `version_edit.h` | FileMetaData 扩展：st_file_meta、st_segment_count |
| `version_set.h/.cc` | Version 管理、ProcessManifestWrites、Manifest 轮转 |
| `version_builder.cc` | 构建时设置 st_file_meta |
| `builder.cc` | SST 构建时 st_file_meta |
| `db_impl.cc` | DB 打开/恢复 |
| `compaction/compaction_picker_level.cc` | 整层 ST picker：TryPickWholeLevelNonL0ST |
| `compaction/compaction_outputs.cc` | ST 输出切分 |
| `compaction/st_event_split_policy.h` | 金字塔输出策略 |

### rocksdb/table/ — SST 表和索引
| 文件 | 内容 |
|------|------|
| `st_meta_index_extension.h` | SpatioTemporalBlockMeta、bitmap |
| `st_meta_user_key.h/.cc` | E7 key 解析、UID 解码、block meta 聚合 |
| `block_based/st_mbr_flush_block_policy.h/.cc` | MBR 驱动 Data Block 断流 |
| `block_based/index_builder.h/.cc` | 时空 block meta 写入 index entry |
| `block_based/block_based_table_builder.cc` | 建表调用 ST flush policy |
| `block_based/block_based_table_reader.cc` | 读时剪枝 |

### rocksdb/file/ — 文件命名
| 文件 | 内容 |
|------|------|
| `filename.h/.cc` | MANIFEST / CURRENT / M_manager 命名规则 |

## rocks-demo/ 主要工具

| 工具 | 功能 |
|------|------|
| `st_bucket_ingest_build` | 灌库：CSV → SST → DB |
| `st_meta_read_bench` | 查询 benchmark |
| `st_meta_compact_existing` | 对已有 DB 执行 ST compaction |
| `st_meta_compaction_verify` | 验证 compaction 正确性 |
| `st_level_stats` | 打印每层 SST 统计 |
| `st_table_props` | 打印 SST table properties（block 数、KV 数等） |
| `st_prune_stats` | 打印剪枝统计 |
| `traj_db_audit` | DB 审计 |
| `st_meta_smoke` | 冒烟测试 |

## 关键 ST 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `experimental_st_compaction` | false | 启用 ST compaction |
| `experimental_st_block_mbr_flush_enable` | true | MBR 驱动 block flush |
| `experimental_st_compaction_stfp_enable` | false | STFP 模式 |
| `experimental_st_compaction_level_trigger_ratio` | 0.9 | 层触发比率 |
| `experimental_st_prune_scan` | - | 查询窗 t_min/t_max/x/y |

## Build

- **OS**：Windows 10/11 x64
- **编译器**：Visual Studio 2022 (MSVC) + Ninja
- **C++ 标准**：C++20
- **依赖**：Snappy (vcpkg)
- **构建**：`cd rocks-demo/build && cmake --build . --config Release`

## 术语

| 术语 | 含义 |
|------|------|
| ST | Spatio-Temporal |
| MBR | Minimum Bounding Rectangle |
| bucket_id | canonical 时间桶 floor(t_start / bucket_seconds)，默认 3600s |
| segment_uid | (vehicle_id, segment_seq, chunk_seq) 逻辑去重键 |
| st_file_meta | 每 SST 的全局 [t_min,t_max] + MBR |
| ST-compaction | L1+ SST 整层归并 + 时空聚簇 + MBR 断流 |
| E7 | 当前 segment key 格式（magic 0xE7，32B） |
| Z-order / Morton | 空间填充曲线，key 内空间聚簇 |
| m_manager | Manifest 索引文件，管理多个 MANIFEST-* |
| L0 Drain | Ingest 后将 L0 全部下移到 L1 |
| 点真判 | 解压 value 遍历轨迹点，任一点落入查询窗才命中 |

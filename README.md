# ST-RocksDB

基于 RocksDB 的**时空轨迹数据存储引擎**。在 LSM-tree 上增加时空索引、ST-compaction 和多层查询剪枝，支持 100% 召回的轨迹段时空查询。

## 快速开始

```powershell
# 1. 编译
cd D:\Project\rocks-demo\build
cmake --build . --config Release

# 2. 灌库（TDrive 示例）
.\st_bucket_ingest_build.exe `
  --segment-meta-csv "D:\Project\data\processed\tdrive\segments_meta.csv" `
  --segment-points-csv "D:\Project\data\processed\tdrive\segments_points.csv" `
  --out-sst-dir "D:\Project\data\db\tdrive_st_sst" `
  --target-db "D:\Project\data\db\tdrive_st" `
  --bucket-sec 86400 `
  --reset-out-sst-dir --reset-target-db `
  --batch-ingest --experimental-st-compaction `
  --user-key-schema tman_locs `
  --tman-st-keys-csv "D:\Project\data\processed\tdrive\segments_st_key.csv"

# 3. 检查
.\st_level_stats.exe --db "D:\Project\data\db\tdrive_st"

# 4. 查询 benchmark
.\st_meta_read_bench.exe --db "D:\Project\data\db\tdrive_st" `
  --prune-t-min 1201930245 --prune-t-max 1202016645 `
  --prune-x-min 116.0 --prune-x-max 117.0 `
  --prune-y-min 39.0 --prune-y-max 41.0
```

## 文档

| 文档 | 内容 |
|------|------|
| **[docs/DESIGN.md](docs/DESIGN.md)** | 系统设计（架构·STFP定义·层级限制·术语表） |
| **[docs/MANUAL.md](docs/MANUAL.md)** | 完整用户手册（编译/灌库/查询/实验/故障排除） |
| **[docs/Mind.md](docs/Mind.md)** | 历史设计定稿（参考归档） |

## 仓库结构

| 目录 | 内容 |
|------|------|
| `rocksdb/` | ST-RocksDB fork 源码（C++ 引擎） |
| `rocks-demo/` | 实验工具 C++ 源码（灌库/查询/compaction/diagnostic） |
| `tools/` | Python/PowerShell 辅助脚本 + 画图脚本（`tools/plot/`） |
| `scripts/` | PowerShell 快捷脚本 |
| `docs/` | 设计文档与用户手册 |
| `Result/` | 实验输出（TSV 数据 + picture 图表） |
| `data/` | 数据集和数据库文件 |

## 环境

- Windows 10/11 x64 · Visual Studio 2022 · CMake >= 3.20 · Python >= 3.9

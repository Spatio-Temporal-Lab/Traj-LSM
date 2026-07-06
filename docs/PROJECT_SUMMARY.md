# ST-LSM 项目总结

> 2026-07-06 · 一次性说清设计、代码、数据、窗口、实验结果

---

## 一、整体设计

### 三层架构

```
QUERY:  M_manager ──→ Manifest ──→ SST st_file_meta ──→ Local Index ──→ Data Block ──→ 点真判
        选MANIFEST     replay       file time+MBR粗筛     index block      解压value     存在一点
                                                         value硬判定                      ∈窗口才命中
```

### 核心保证

`false_negative_count = 0`（100% 召回，零假阴性）

### 不漏解充分条件

| 层 | 条件 | 说明 |
|----|------|------|
| G1 Manifest | 单 MANIFEST replay 得全部 live SST；`st_file_meta` 时间+空间相交才进候选；meta 缺失 → 报错 | 一次 IO |
| L1 meta | `[t_min,t_max]`、MBR 来自 Value 头全段并集，非仅用 key bucket_id 推短窗 | 跨桶长段不误剪 |
| L2 保守 | meta 缺失 / value 解码失败 → 直接报错 | 不降级为 match-all |
| 点真 | MBR 相交后解压 value，存在一点 ∈ 窗口才命中 | 控 FP |

### Key 格式: E7 (32 bytes)

```
[0xE7][type_flags][bucket_id 4B BE][zorder 8B BE][t_start_offset 4B BE][vehicle_id 8B BE][segment_seq 4B BE][chunk_seq 2B BE]
```

- `bucket_id = floor(t_start / W)` — canonical 日历桶，粗排序 + 后截断
- `zorder` — Morton 空间编码，排序聚簇
- `segment_uid = (vehicle_id, segment_seq, chunk_seq)` — 逻辑去重键

### ST-Compaction: 整层归并 + 金字塔输出 + MBR 断流

| 层 | max_keys/文件 | max_bucket_span |
|----|--------------|-----------------|
| L1 | 10,000 | 64 |
| L2 | 40,000 | 128 |
| L3 | 160,000 | 256 |
| L4+ | 无上限 | 512 |

---

## 二、构建环境问题：两套 build 目录

**问题**: 修改 `rocksdb/` 源码后编译 `build/` 但链接的是 `build_vs/`。

```
CMakeCache:  ROCKSDB_BUILD = D:/Project/rocksdb/build_vs   ← 实际链接
我们编译的:   D:/Project/rocksdb/build/                      ← 改代码后编译到这里
```

**根因**: CMake cache 指向 `build_vs`，Ninja 链接 `rocksdb/build_vs/rocksdb.lib`。之前所有对 `block_based_table_iterator.cc` 的改动都无效，因为链接的 lib 从未被重编译。

**解决**: 在 `build_vs` 下编译：

```powershell
cd D:\Project\rocksdb\build_vs
"D:/Visio/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe" rocksdb
cd D:\Project\rocks-demo\build
del st_meta_read_bench.exe
cmake --build . --config Release
```

---

## 三、TiDB 集成：应该修改哪些文件

### 参考文档

| 文档 | 内容 |
|------|------|
| `docs/DESIGN.md` | **设计终稿**（唯一真相源） |
| `docs/TIDB_INTEGRATION_GUIDE.md` | TiDB 集成步骤指南 |
| `docs/MANUAL.md` | 完整用户手册（编译/灌库/查询/实验） |
| `CLAUDE.md` | 项目总览和水源改动地图 |

### 核心源码（需要移植到 TiDB/TiKV 的 RocksDB）

所有修改都在 `rocksdb/` 目录下：

| 目录 | 文件 | 改动内容 |
|------|------|----------|
| **include/rocksdb/** | `options.h`, `advanced_options.h` | ST 选项定义 |
| **db/** | `version_edit.h` | `FileMetaData` 扩展：`st_file_meta`, `has_st_file_meta` |
| | `version_set.cc/.h` | LevelIterator ST 过滤、Manifest 同步 |
| | `manifest_ops.cc/.h` | Manifest filter、`GetLiveSstLevelStats` |
| | `compaction/compaction_picker_level.cc` | **ST 整层 picker**: `TryPickWholeLevelNonL0ST` |
| | `compaction/st_event_split_policy.h` | **金字塔输出策略** |
| | `compaction/compaction_outputs.cc/.h` | ST 输出切分、uid_merge |
| | `db_impl.cc/.h`, `db_impl_compaction_flush.cc` | DB 打开/恢复/flush/compaction 调度 |
| **table/** | `st_meta_index_extension.h` | **SpatioTemporalBlockMeta** 结构体 |
| | `st_meta_user_key.h/.cc` | **E7 Key 解析**、Block meta 聚合、Seek 下界构造 |
| | `block_based/block_based_table_iterator.cc` | **前后截断**（此项消融的关键代码） |
| | `block_based/block_based_table_builder.cc` | MBR flush block policy |
| | `block_based/st_mbr_flush_block_policy.h/.cc` | **MBR 驱动 FlushPolicy** |
| | `block_based/index_builder.h/.cc` | 合并时空 meta 到 index entry |
| | `block_based/block_based_table_reader.cc` | ST 读时剪枝 |

> **总共 ~30 个文件，主要集中在 `rocksdb/db/`、`rocksdb/table/`、`rocksdb/include/`**

### 三个新增的关键文件

| 文件 | 作用 |
|------|------|
| `rocksdb/table/st_meta_index_extension.h` | SpatioTemporalBlockMeta 结构体，block MBR + time range |
| `rocksdb/table/st_meta_user_key.h/.cc` | E7 key 格式定义、编解码、Seek 下界、UID 去重 |
| `rocksdb/db/compaction/st_event_split_policy.h` | ST compaction 金字塔参数 |

---

## 四、数据集

### TDrive + Wuxi 数据

| 路径 | 内容 |
|------|------|
| `data/td_wx/segments_meta.csv` | 176,686 段元信息（id, dataset, t_start, t_end, MBR） |
| `data/td_wx/segments_points.csv` | 42,631,900 轨迹点（segment_id, lon, lat, time） |
| `data/td_wx/tdwx_trajs.txt` | TMan 格式轨迹文件（MULTIPOINT Z WKT） |
| `data/td_wx/tdwx_envelope.json` | TMan ingest 配置 |

### 数据集构成

| 来源 | 段数 | 时间范围 |
|------|:---:|------|
| TDrive | 47,102 | 2008-02-02 ~ 2008-02-08 |
| Wuxi | 129,584 | 2020-08-04 ~ 2020-08-17 |

### 灌好的 DB

| 路径 | 模式 | SST 组织 |
|------|------|----------|
| `data/runs/tdwx_stlsm` | ST compaction (E7) | 10 SST @ L2, 537 MB |
| `data/runs/tdwx_vanilla` | 原生 RocksDB (E7) | 8 SST @ L0, 537 MB |

---

## 五、查询窗口

| 窗口 | t_min | t_max | x_min | x_max | y_min | y_max | finalSize |
|------|-------|-------|-------|-------|-------|-------|:---:|
| td_narrow | 1201930244 | 1201933844 | 116.0 | 117.0 | 39.0 | 41.0 | 1385 |
| td_medium | 1201930244 | 1201943564 | 116.0 | 117.0 | 39.0 | 41.0 | 2501 |
| td_wide | 1201999002 | 1202042202 | 115.6 | 117.6 | 32.7 | 41.1 | 6028 |
| wx_narrow | 1596499200 | 1596502800 | 119.7 | 120.7 | 31.0 | 32.5 | 320 |
| wx_medium | 1596499200 | 1596512520 | 119.4 | 121.0 | 30.9 | 32.7 | 906 |
| wx_wide | 1596499200 | 1596542400 | 118.9 | 121.0 | 30.6 | 32.9 | 2726 |

窗口文件: `data/td_wx/windows6.csv`（秒）, `data/td_wx/conditions6.txt`（毫秒）

---

## 六、实验结果

| 结果 | 路径 |
|------|------|
| TMan vs ST-LSM 对比 | `Result/compare/tdwx_tman_vs_stlsm.md` |
| 消融实验 | `Result/compare/ablation/README.md` |
| 消融原始 JSON | `Result/compare/ablation/ablation_*.json` |

---

## 七、已修复 Bug

**uid_merge 误删段**: `ParseSegmentIdForE7` 对无 `_chunk` 后缀的 segment_id（如 `wuxi:phone:000001`）未解析 `segment_seq`，导致同一车辆的段 uid 碰撞，ST compaction 期间误删 59,439 条（34%）。已在 `rocks-demo/st_bucket_ingest_build.cpp` 修复。

---

## 八、消融实验关键代码位置

前后截断的控制点在 `rocksdb/table/block_based/block_based_table_iterator.cc`:

```cpp
// 后截断 (backward): bucket_id > max_query_bucket → stop
IndexBucketPastQueryWindow(index_iter->key(), q)  // 行 89

// 前截断 (forward): m.t_max < q.t_min → skip block  
m.t_max < q.t_min || m.t_min > q.t_max            // 行 115

// 环境变量控制（消融用）:
// ST_DISABLE_BACKWARD_TRUNCATION=1 → 关后截断
// ST_DISABLE_FORWARD_TRUNCATION=1  → 关前截断
```

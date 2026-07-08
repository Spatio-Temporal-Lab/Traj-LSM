# ST-LSM 参数实验清单

> 完整参数列表，按模块分类。标注 ⭐ 的是对性能/召回影响大的关键参数。

---

## 一、ST-Compaction 参数

### 1.1 金字塔输出策略 (`st_event_split_policy.h`)

| 参数 | 默认值 | 说明 | 实验建议 |
|------|--------|------|----------|
| ⭐ L1 max_keys | 12,000 | L1 每文件最大 key 数 | 6k/12k/24k |
| ⭐ L2 max_keys | 20,000 | | 10k/20k/40k |
| ⭐ L3 max_keys | 80,000 | | 40k/80k/160k |
| L4+ max_keys | unlimited | | — |
| L1 max_bucket_span | 64 | 每文件最大时间跨度(桶) | 32/64/128 |
| L2 max_bucket_span | 128 | | 64/128/256 |
| L3 max_bucket_span | 256 | | 128/256/512 |
| L4+ max_bucket_span | 512 | | — |
| spatial_ratio_mul | 1.0→1.8 | 空间膨胀倍率，层级越高越宽松 | 0.8/1.0/1.5 |

### 1.2 MBR 断流 (`advanced_options.h`)

| 参数 | 默认值 | 说明 | 实验建议 |
|------|--------|------|----------|
| ⭐ `experimental_compaction_spatial_soft_expansion_ratio` | 2.5 | MBR 软膨胀阈值（触发 block flush） | 1.5/2.0/2.5/3.0 |
| `experimental_compaction_spatial_hard_expansion_ratio` | 0 | 硬膨胀阈值 | 2.0/3.0/4.0 |
| `experimental_st_block_mbr_flush_enable` | true | 是否启用 MBR 驱动 block flush | on/off |

### 1.3 触发策略 (`advanced_options.h`)

| 参数 | 默认值 | 说明 | 实验建议 |
|------|--------|------|----------|
| ⭐ `experimental_st_compaction_level_trigger_ratio` | 0.9 | Level bytes >= ratio × max_bytes 时触发整层归并 | 0.7/0.8/0.9/0.95 |
| ⭐ `experimental_st_compaction_tiered_max_runs_per_level` | 4 | 每层最大 run 数（R4 vs R6） | **2/4/6/8** |
| `experimental_st_compaction_stfp_enable` | false | STFP 模式 | on/off |
| `experimental_st_compaction_l1_l2_tiered` | false | L1+L2 tiered | on/off |

### 1.4 时间切分 (`advanced_options.h`)

| 参数 | 默认值 | 说明 | 实验建议 |
|------|--------|------|----------|
| `experimental_compaction_event_time_split` | false | 时间维度切分 | on/off |
| `experimental_compaction_event_time_bucket_width` | 0 | 时间桶宽度(秒) | 3600/86400 |
| `experimental_compaction_event_time_max_span` | 0 | 文件最大时间跨度 | 21600/43200/86400 |

---

## 二、Local Index 参数

### 2.1 前后截断 (`block_based_table_iterator.cc`)

| 参数 | 默认值 | 说明 | 实验建议 |
|------|--------|------|----------|
| ⭐ 前截断 (forward) | 开 | index block value `t_max < q.t_min` 跳过 | **on/off** |
| ⭐ 后截断 (backward) | 开 | index key `bucket_id > max` 停止 | **on/off** |

### 2.2 索引选项 (`options.h ReadOptions`)

| 参数 | 默认值 | 说明 | 实验建议 |
|------|--------|------|----------|
| `block_level_enable` | true | 是否启用 index block 剪枝 | on/off |
| `block_level_index_seek_lower_bound_enable` | true | Seek 下界优化 | on/off |
| `event_time_bucket_width_sec` | 3600 | E7 key bucket 宽度 | 3600/86400 |
| `event_time_bucket_lookback` | UINT32_MAX | Seek 回退桶数（0=从bucket_id 0开始） | 0/10/100/all |
| `key_level_enable` | true | key 级过滤 | on/off |

---

## 三、Global Index 参数

### 3.1 Manifest 文件过滤 (`version_set.cc`)

| 参数 | 默认值 | 说明 | 实验建议 |
|------|--------|------|----------|
| ⭐ prune-mode | sst_manifest | 过滤层级 | sst/manifest/sst_manifest |
| `time-bucket-count` | 32 | 时间桶数（虚拟归并） | 16/32/64 |
| `rtree-leaf-size` | 8 | R-tree 叶子大小 | 4/8/16 |
| `vm-time-span-sec-threshold` | 21600 | 虚拟归并触发阈值(秒) | 3600/10800/21600 |

---

## 四、Ingest 参数

| 参数 | 默认值 | 说明 | 实验建议 |
|------|--------|------|----------|
| ⭐ `bucket-sec` | 3600 | 日历桶宽度(秒) | **3600/86400** |
| `user-key-schema` | e7 | key 格式 | e7/tman_st |
| `write_buffer_size` | 64MB | memtable 大小 | 32/64/128MB |

---

## 五、参数实验优先级

### P0（必须做）

| 实验 | 参数 | 对比维度 |
|------|------|----------|
| R4 vs R6 | `tiered_max_runs_per_level` = 4 vs 6 | compaction 策略 |
| 金字塔键数 | L1/L2/L3 max_keys | 文件大小 → 剪枝粒度 |
| Level 触发比 | `level_trigger_ratio` = 0.7/0.8/0.9 | compaction 频率 |
| MBR 膨胀比 | `spatial_soft_expansion_ratio` = 1.5~3.0 | block flush 频率 → block 质量 |
| 前后截断 | forward/backward on/off | query 时间 + block read |
| bucket 宽度 | 3600 vs 86400 | key 聚簇粒度 |

### P1（建议做）

| 实验 | 参数 |
|------|------|
| Global 过滤策略 | sst vs manifest vs sst_manifest |
| MBR flush 开关 | `st_block_mbr_flush_enable` on/off |
| Seek lookback | `event_time_bucket_lookback` 0 vs all |
| 时间切分 | `event_time_split` + `max_span` |
| max_bucket_span | 各层 32~512 |

### P2（可选）

| 实验 | 参数 |
|------|------|
| R-tree leaf size | 4/8/16 |
| Time bucket count | 16/32/64 |
| VM threshold | 3600~21600 |
| write_buffer_size | 32/64/128MB |

# ST-RocksDB TiDB 测试指南

## 前提

TiDB/TiKV 已集成 ST-RocksDB fork，编译通过。

## 第一步：烟雾测试（5 分钟）

验证 ST 功能基本可用：

```sql
-- 1. 创建表（使用 E7 key 格式需自定义编解码）
-- 如果 TiDB 侧已实现 E7 key codec，直接写入
-- 否则用 RawKV API 测试：

-- 2. 写入 100 条轨迹段数据
--    每条: E7 key (32B) + Segment Value (header 32B + points)

-- 3. 执行时空范围查询
--    查询窗: time=[T1, T2], MBR=[x1,x2; y1,y2]

-- 4. 验证三种模式结果一致：
--    a) 全扫描 (ST prune disabled)
--    b) File 级裁剪 (file_level_enable=true)
--    c) File+Block 级裁剪 (file+block enable)
```

### C++ 侧验证（Direct RocksDB）

```cpp
// 编译并运行
// 1. 写入测试数据
for (int i = 0; i < 100; i++) {
    std::string key = MakeE7Key(t_start[i], lon[i], lat[i]);
    std::string value = MakeSegmentValue(points[i]);
    db->Put(WriteOptions(), key, value);
}
db->Flush(FlushOptions());

// 2. 全扫描计数
int full_count = 0;
auto* full_it = db->NewIterator(ReadOptions());
for (full_it->SeekToFirst(); full_it->Valid(); full_it->Next()) {
    if (InWindow(full_it->key(), full_it->value(), query)) full_count++;
}

// 3. Prune 扫描计数
ReadOptions ro;
ro.experimental_st_prune_scan.enable = true;
ro.experimental_st_prune_scan.file_level_enable = true;
ro.experimental_st_prune_scan.block_level_enable = true;
ro.experimental_st_prune_scan.key_level_enable = true;
ro.experimental_st_prune_scan.t_min = q_t_min;
// ... set all window params

int prune_count = 0;
auto* prune_it = db->NewIterator(ro);
for (prune_it->SeekToFirst(); prune_it->Valid(); prune_it->Next()) {
    prune_count++;
}

assert(full_count == prune_count);  // 必须相等！
printf("PASS: full=%d prune=%d\n", full_count, prune_count);
```

## 第二步：正确性测试（15 分钟）

用附带的 12 个分层查询窗口验证：

```bash
# 在 ST-RocksDB 目录下
./build/st_meta_read_bench \
    --db /path/to/test_db \
    --prune-mode sst_manifest \
    --windows-csv unified_stratified12_st_windows.csv \
    --segment-meta-csv segments_meta.csv
```

**期望结果**：
- 每个窗口 `false_negative_count = 0`
- `query_result_dedup_count` ≈ 0
- 所有窗口 Correct=OK

## 第三步：性能基准测试（30 分钟）

### 写入吞吐

```bash
./build/st_compaction_throughput_bench \
    --num-keys 200000 \
    --repeat 3 \
    --stfp_r4
```

**期望**：Read > 500 MB/s, Write > 15 MB/s, Correct=OK

### 对比：ST 开 vs 关

| 配置 | Read MB/s | Write MB/s |
|------|:---------:|:----------:|
| ST disabled (vanilla) | ~550 | ~23 |
| STFP-R4 enabled | ~600 | ~26 |

## 第四步：Compaction 验证（10 分钟）

```bash
# 检查 SST 文件是否有 ST 元数据
./build/st_level_stats --db /path/to/db

# 期望输出:
#   with_st_file_meta=N  (N > 0)
#   without_st_file_meta=0
```

确认：
- `st_file_meta` 覆盖 100% 的 SST
- Compaction 后 L0=0
- `live_kv_count` ≈ 写入段数

## 第五步：TiKV 集成测试

如果 TiDB 侧实现了 E7 key codec：

```sql
-- 使用 TiDB 写入
INSERT INTO trajectory_segments VALUES (...);

-- 验证读
SELECT COUNT(*) FROM trajectory_segments 
WHERE t BETWEEN T1 AND T2 
  AND lon BETWEEN X1 AND X2 
  AND lat BETWEEN Y1 AND Y2;

-- 对比执行计划（explain analyze）查看是否走了 ST prune
```

### 确认 ST 生效的方法

在 TiKV 日志中搜索：
```
experimental_st_prune_scan
st_file_meta
prune_file_skipped
prune_block_diag
```

如果看到 `prune_file_skipped > 0`，说明文件级裁剪正在工作。

## 快速验证 Checklist

- [ ] Smoke test: full_count == prune_count
- [ ] 12 stratified windows: all Correct=OK, FN=0
- [ ] st_level_stats: with_st_file_meta > 0
- [ ] Compaction bench: Read > 500 MB/s
- [ ] TiKV logs show prune_file_skipped > 0 (if TiDB integrated)

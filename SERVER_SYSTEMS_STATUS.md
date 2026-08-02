# 服务器三系统状态文档

> 更新时间: 2026-07-28 20:35 CST  
> 服务器: 10.242.6.21 (startserver06), 10.242.1.36 (startserver07), 10.242.3.184 (startserver08)  
> 用户: lfh/lfh

---

## 集群基础设施

| 组件 | 主节点 | 节点列表 | 状态 |
|------|--------|----------|------|
| HDFS NameNode | 10.242.3.184:9000 | - | ✅ |
| HDFS DataNodes | - | 06, 07, 08 | ✅ |
| YARN ResourceManager | 10.242.3.184 | - | ✅ |
| YARN NodeManagers | - | 06, 07, 08 | ✅ |
| HBase Master | 10.242.6.21 | - | ✅ |
| HBase RegionServers | - | 06, 07, 08 | ✅ |
| Zookeeper | 10.242.6.21:2182 | - | ✅ |
| Redis | 10.242.6.21:6379 | - | ✅ |
| Spark | 10.242.6.21 | 2.3.3-bin-hadoop2.7 | ✅ |

### 路径速查
```bash
export JAVA_HOME=/home/lfh/apps/jdk1.8.0_202
export SPARK_HOME=/home/lfh/apps/spark-2.3.3-bin-hadoop2.7
export HADOOP_HOME=/home/lfh/apps/hadoop
export HBASE_HOME=/home/lfh/apps/hbase-2.1.2
export HADOOP_CONF_DIR=$HADOOP_HOME/etc/hadoop
```

---

## 系统一：TraSS (k-sim)
**类型**: Spark + HBase, XZ* 轨迹相似度查询  
**JAR**: `/home/lfh/Tman/k-sim/trass.jar`  
**数据表**: `trass_full` (HBase)  
**数据量**: 115,143 行 (tdrive + wuxi 5K 样本)  
**灌库命令**:
```bash
# 数据转换: oid-tid-WKT -> oid\t\tWKT (tab分隔)
python3 << 'PYEOF'
import glob
out = open("/tmp/trass_full/1/part-00000", "w")
for f in sorted(glob.glob("/home/lfh/Tman/k-sim/data/tdrive/1/part-*")):
    for line in open(f):
        line = line.strip()
        if not line: continue
        idx = line.rindex("-")
        prefix, wkt = line[:idx], line[idx+1:]
        out.write(prefix + "\t" + prefix + "\t" + wkt + "\n")
# wuxi data
for line in open("/home/lfh/Tman/k-sim/data/tdwx_test/1/part-00000"):
    line = line.strip()
    if not line: continue
    idx = line.rindex("-")
    prefix, wkt = line[:idx], line[idx+1:]
    out.write(prefix + "\t" + prefix + "\t" + wkt + "\n")
out.close()
PYEOF

# 灌库
$SPARK_HOME/bin/spark-submit \
  --class com.just.ksim.experiments.storing.StoringQuadTDriveToHBase \
  --master "local[4]" --driver-memory 8G \
  /home/lfh/Tman/k-sim/trass.jar \
  file:///tmp/trass_full trass_full file:///tmp/trass_ingest_time 8 1 1 16
```

**查询命令 (RangeQuery)**:
```bash
# 本地模式
$SPARK_HOME/bin/spark-submit \
  --class com.just.ksim.experiments.query.RangeQuery \
  --master "local[4]" --driver-memory 4G \
  /home/lfh/Tman/k-sim/trass.jar \
  "xmin,ymin,xmax,ymax;..." trass_full file:///tmp/result 8 16

# YARN分布式模式
$SPARK_HOME/bin/spark-submit \
  --class com.just.ksim.experiments.query.RangeQuery \
  --master yarn --deploy-mode client \
  --num-executors 4 --executor-memory 8G --executor-cores 2 \
  --conf spark.yarn.jars=hdfs://10.242.3.184:9000/user/lfh/spark-jars/* \
  /home/lfh/Tman/k-sim/trass.jar \
  "xmin,ymin,xmax,ymax;..." trass_full hdfs://10.242.3.184:9000/user/lfh/output/result 8 16
```

**查询类型**:
- `RangeQuery` — 空间范围查询 (MBR窗口)
- `SimilarityQuery` — 阈值相似度查询 (DTW/Hausdorff)
- `KNNQuery` — Top-k 相似轨迹查询

---

## 系统二：TMan-tsp (HBase)
**类型**: Java + HBase, 时空索引查询  
**JAR**: `/home/lfh/apps/hbase-2.1.2/lib/TMan-1.0-SNAPSHOT.jar` (已修复 warmup bug)  
**数据表**: `tman_full` + `tman_full_meta` (HBase)  
**数据量**: 223,788 行 (tdrive 47K + wuxi 177K)  
**索引类型**: SPATIAL 主索引 + TemporalFilter 后过滤 = 完整时空查询  
**查询验证**: 5,556 point-truth 结果, 1,490ms  
**灌库命令**:
```bash
java -cp /home/lfh/apps/hbase-2.1.2/lib/TMan-1.0-SNAPSHOT.jar:$HBASE_CP \
  huajunge.github.io.spatialman.experiments.tman.TMANStoring \
  /tmp/tdwx_full_tman.txt \
  10 1 1 3600.0 109932 \
  DELTA_COMPRESS_VGB \
  115.0 12.0 134.0 46.0 \
  8 SPATIAL tman_full 10.242.6.21 0 0 \
  /tmp/tman_ingest_time
```
> 数据准备: `cat /home/lfh/dataset/processed/tman_tdrive/tdrive_trajs.txt /home/lfh/dataset/processed/tdwx/tdwx_trajs.txt > /tmp/tdwx_full_tman.txt`  
> **注意**: 必须用 `SPATIAL` (不是 `ST`)，因为 SpatioTemporalFilter 缺 HBase 序列化。时空过滤由 SpatialFilter(RowKey剪枝) + TemporalFilter(cell过滤) 联合完成。

**查询命令 (STQuery - 时空)**:  
```bash
export HBASE_CP=$(/home/lfh/apps/hbase-2.1.2/bin/hbase classpath)

# 单窗口时空查询 (xmin,ymin,xmax,ymax,tmin_ms,tmax_ms)
java -cp /home/lfh/apps/hbase-2.1.2/lib/TMan-1.0-SNAPSHOT.jar:$HBASE_CP \
  huajunge.github.io.spatialman.experiments.tman.STQuery \
  tman_full "116.30,39.85,116.50,40.00,1201930244000,1202000000000" /tmp/result.csv
# 验证: ~5,556 point-truth results, ~1,490ms
```

**查询命令 (SpatialQuery - 纯空间)**:
```bash
export HBASE_CP=$(/home/lfh/apps/hbase-2.1.2/bin/hbase classpath)

# 空间查询
java -cp /home/lfh/apps/hbase-2.1.2/lib/TMan-1.0-SNAPSHOT.jar:$HBASE_CP \
  huajunge.github.io.spatialman.experiments.tman.SpatialQuery \
  tman_full "xmin,ymin,xmax,ymax" /tmp/result.csv

# 时空查询 (6参数)
java -cp /home/lfh/apps/hbase-2.1.2/lib/TMan-1.0-SNAPSHOT.jar:$HBASE_CP \
  huajunge.github.io.spatialman.experiments.tman.STQuery \
  tman_full "xmin,ymin,xmax,ymax,tmin_ms,tmax_ms" /tmp/result.csv

# 批量查询 (分号分隔多个条件)
java -cp ... STQuery tman_full "x1,y1,x2,y2,t1,t2;x1,y1,x2,y2,t1,t2;..." /tmp/result.csv
```

---

## 系统三：TMan-RocksDB
**类型**: Java + RocksDB JNI, 时空索引查询  
**JAR**: `/home/lfh/Tman/TMan-RocksDB/target/TMan-RocksDB-1.0-SNAPSHOT-jar-with-dependencies.jar`  
**RocksDB JNI**: `/home/lfh/Tman/rocksdb/build/java/rocksdbjni-11.2.0-linux64.jar`  
**数据位置**: 
- 07 (10.242.1.36): `/home/lfh/tman_rocks_db/full` — 111,894 行
- 08 (10.242.3.184): `/home/lfh/tman_rocks_db/full` — 111,894 行  
**总数据量**: 223,788 行 (分片，每节点一半)  

**灌库命令**:
```bash
# 分片数据
TOTAL=$(wc -l < /tmp/tdwx_full_tman.txt)
head -$((TOTAL/2)) /tmp/tdwx_full_tman.txt > /tmp/shard0.txt
tail -n +$((TOTAL/2+1)) /tmp/tdwx_full_tman.txt > /tmp/shard1.txt

# 在每个节点上灌库
CP="/home/lfh/Tman/TMan-RocksDB/target/classes:/home/lfh/Tman/TMan-RocksDB/target/TMan-RocksDB-1.0-SNAPSHOT-jar-with-dependencies.jar:/home/lfh/Tman/rocksdb/build/java/rocksdbjni-11.2.0-linux64.jar:/home/lfh/Tman/TMan-hehuajun-tsp/target/classes"

java -cp $CP huajunge.github.io.spatialman.experiments.rocksdb.TmanRocksStoring \
  --db /home/lfh/tman_rocks_db/full \
  --source /home/lfh/tman_data/full_shard0.txt \
  --table tman_full --primary ST \
  --redis-host 10.242.6.21 \
  --xmin 115.0 --ymin 12.0 --xmax 134.0 --ymax 46.0
```

**查询命令 (单节点 CLI)**:
```bash
java -cp $CP huajunge.github.io.spatialman.distributed.TmanRocksWorkerMain \
  --db /home/lfh/tman_rocks_db/full \
  --table tman_full \
  --condition "xmin,ymin,xmax,ymax,tmin,tmax"
# 输出: time_ms,finalSize,candidates,indexRanges
```

**查询命令 (SSH并行两节点)**:
```bash
export JAVA_HOME=/home/lfh/apps/jdk1.8.0_202
CP="/home/lfh/Tman/TMan-RocksDB/target/classes:/home/lfh/Tman/TMan-RocksDB/target/TMan-RocksDB-1.0-SNAPSHOT-jar-with-dependencies.jar:/home/lfh/Tman/rocksdb/build/java/rocksdbjni-11.2.0-linux64.jar:/home/lfh/Tman/TMan-hehuajun-tsp/target/classes"
Q="116.30,39.85,116.50,40.00,1201930244000,1202000000000"

# 并行查询
R07=$(ssh lfh@10.242.1.36 "java -cp $CP huajunge.github.io.spatialman.distributed.TmanRocksWorkerMain --db /home/lfh/tman_rocks_db/full --table tman_full --condition \"$Q\"")
R08=$(ssh lfh@10.242.3.184 "java -cp $CP huajunge.github.io.spatialman.distributed.TmanRocksWorkerMain --db /home/lfh/tman_rocks_db/full --table tman_full --condition \"$Q\"")
echo "07: $R07"
echo "08: $R08"
# 格式: time_ms,finalSize,candidates,indexRanges
# 合并: finalSize=sum, candidates=sum, time=max, indexRanges=sum
```

---

## 数据集说明

| 数据集 | 来源 | 行数 | 空间范围 | 时间范围 |
|--------|------|------|----------|----------|
| tdrive | 北京出租车GPS | 47,102 | lon 115-118, lat 39-42 | 2008-02 |
| wuxi | 无锡车辆轨迹 | 176,686 | lon 110-134, lat 12-46 | 2020-07 |
| **合并 (td+wx)** | - | **223,788** | China-wide | 2008-2020 |

---

## 启动/重启步骤

> **⚠️ 重启后 /tmp 会清空！需要重新准备数据文件：**
> ```bash
> # TMan-tsp + TMan-RocksDB 数据
> cat /home/lfh/dataset/processed/tman_tdrive/tdrive_trajs.txt /home/lfh/dataset/processed/tdwx/tdwx_trajs.txt > /tmp/tdwx_full_tman.txt
> # TraSS 数据转换（见系统一灌库命令中的 python3 脚本）
> ```

### 一键启动（在 10.242.6.21 上执行）
```bash
export JAVA_HOME=/home/lfh/apps/jdk1.8.0_202

# HDFS + YARN
ssh lfh@10.242.3.184 'export JAVA_HOME=/home/lfh/apps/jdk1.8.0_202 && /home/lfh/apps/hadoop/sbin/start-dfs.sh && /home/lfh/apps/hadoop/sbin/start-yarn.sh'
sleep 5

# HBase (06)
/home/lfh/apps/hbase-2.1.2/bin/start-hbase.sh
# HBase RegionServer on 07/08
ssh lfh@10.242.1.36 'export JAVA_HOME=/home/lfh/apps/jdk1.8.0_202 && /home/lfh/apps/hbase-2.1.2/bin/hbase-daemon.sh start regionserver' &
ssh lfh@10.242.3.184 'export JAVA_HOME=/home/lfh/apps/jdk1.8.0_202 && /home/lfh/apps/hbase-2.1.2/bin/hbase-daemon.sh start regionserver' &
wait
sleep 5

# YARN NodeManager on 07/08 (如果没自动启动)
ssh lfh@10.242.1.36 'export JAVA_HOME=/home/lfh/apps/jdk1.8.0_202 && /home/lfh/apps/hadoop/sbin/yarn-daemon.sh start nodemanager' &
ssh lfh@10.242.3.184 'export JAVA_HOME=/home/lfh/apps/jdk1.8.0_202 && /home/lfh/apps/hadoop/sbin/yarn-daemon.sh start nodemanager' &
wait

# Redis
/home/lfh/apps/redis/bin/redis-server --daemonize yes
/home/lfh/apps/redis/bin/redis-cli CONFIG SET protected-mode no
```

### 健康检查
```bash
echo "=== HDFS ===" && /home/lfh/apps/hadoop/bin/hdfs dfsadmin -report 2>/dev/null | grep -E "Live|Dead"
echo "=== YARN ===" && /home/lfh/apps/hadoop/bin/yarn node -list 2>/dev/null | grep RUNNING
echo "=== HBase ===" && echo 'status' | /home/lfh/apps/hbase-2.1.2/bin/hbase shell -n 2>/dev/null | tail -2
echo "=== Redis ===" && /home/lfh/apps/redis/bin/redis-cli ping
echo "=== Tables ===" && echo 'list' | /home/lfh/apps/hbase-2.1.2/bin/hbase shell -n 2>/dev/null | grep -E 'trass_full|tman_full'
```
应该看到: 3 DataNodes live, 3 NMs RUNNING, 3 servers 0 dead, PONG, trass_full+tman_full

### 快速验证三系统查询
```bash
# TraSS
$SPARK_HOME/bin/spark-submit --class com.just.ksim.experiments.query.RangeQuery --master "local[4]" --driver-memory 4G \
  /home/lfh/Tman/k-sim/trass.jar "116.3,39.85,116.5,40.0" trass_full file:///tmp/check 8 16
cat /tmp/check

# TMan-tsp
java -cp /home/lfh/apps/hbase-2.1.2/lib/TMan-1.0-SNAPSHOT.jar:$(/home/lfh/apps/hbase-2.1.2/bin/hbase classpath) \
  huajunge.github.io.spatialman.experiments.tman.STQuery \
  tman_full "116.30,39.85,116.50,40.00,1201930244000,1202000000000" /tmp/check2
cat /tmp/check2

# TMan-RocksDB (节点07)
ssh lfh@10.242.1.36 "java -cp /home/lfh/Tman/TMan-RocksDB/target/classes:/home/lfh/Tman/TMan-RocksDB/target/TMan-RocksDB-1.0-SNAPSHOT-jar-with-dependencies.jar:/home/lfh/Tman/rocksdb/build/java/rocksdbjni-11.2.0-linux64.jar:/home/lfh/Tman/TMan-hehuajun-tsp/target/classes \
  huajunge.github.io.spatialman.distributed.TmanRocksWorkerMain \
  --db /home/lfh/tman_rocks_db/full --table tman_full --condition '116.30,39.85,116.50,40.00,1201930244000,1202000000000'"
```

---

---
## TMan-tsp 内部：两套独立索引系统

TMan-tsp 实际上包含两套独立的时空索引体系，对应不同的灌库入口和查询 Filter 组合。**数据必须分开两次灌库**，存在不同的 HBase 表和 Redis key 中。

### 体系对比

| | TMan (传统) | TrajMesa / JUST |
|------|------|------|
| 灌库入口 | `TMANStoring` + `Loader` | `JUSTStoring` + `JUSTLoader` |
| 配置类 | `TableConfig` | `JUSTTableConfig`（多了 `timePeriod`） |
| **空间索引** | **TShape** = `LocSIndex`（`TShapeEE`） | **XZ2** = `XZIndex`（`XZ2EE`） |
| **时间索引** | **TRIndex** = `TimeRangeIndex` | **XZT** = `XZT` |
| 空间查询 Filter | `SpatialFilter` | `JUSTSpatialFilter` |
| 时间查询 Filter | `TemporalFilter` | `JUSTTemporalFilter` |
| 时空查询 Filter | `SpatialFilter` + `TemporalFilter` 联合 | `JUSTSpatialFilter` + `JUSTTemporalFilter` 联合 |
| 关键参数 | `IS_XZ=0`（控制空间索引用 TShape） | 自动使用 XZ2 + XZT |

### 索引能力边界

- **TShape / XZ2**：纯空间索引，API 为 `ranges(bbox)`，不能单独做时空查询
- **TRIndex / XZT**：纯时间索引，API 为 `ranges(startTime, endTime)` / `index(startTime, endTime)`，不能单独做时空查询
- **时空查询 = 空间 + 时间联合**：RowKey 前缀用空间范围 × 时间周期交叉覆盖，cell 级别再做时间/空间点真判

### TShape vs XZ2 区别

| | TShapeEE (`LocSIndex`) | XZ2EE (`XZIndex` / `XZLocSIndex`) |
|------|------|------|
| 四叉树元素 | `TShapeEE`（`XZSFC.scala:445`） | `XZ2EE`（`XZSFC.scala:734`） |
| 空间扩展倍数 | `alpha × width`，`beta × height` | 固定 **2×** width，**2×** height |
| 网格划分 | alpha×beta 子网格 | alpha×beta 子网格（在 2× 扩展区域内） |

### TRIndex vs XZT 区别

| | TRIndex (`TimeRangeIndex`) | XZT |
|------|------|------|
| 算法 | 网格化：`startBin × maxBinNumber + duration` | XZ-curve 思想应用到 1D 时间区间 |
| 粒度 | bin（小时间片）+ maxBinNumber（最大跨度） | g（分辨率参数）+ timePeriod（Day/Week/Month/Year） |
| 查询分解 | `intersects()` 返回多个 IndexRange（跨周期展开） | `ranges()` 层级四叉树分解，返回合并后的 IndexRange |

### 灌库命令模板

**TMan（TShape + TRIndex）**：
```bash
java -cp TMan-1.0-SNAPSHOT.jar:$HBASE_CP \
  huajunge.github.io.spatialman.experiments.tman.TMANStoring \
  <source_path> <resolution> <alpha> <beta> <timeBin> <timeBinNums> <compress> \
  <xmin> <ymin> <xmax> <ymax> \
  <shards> SPATIAL <table_name> <redis_host> 0 0 <result_path>
#                                              ↑ IS_XZ=0 → TShape
```

**TrajMesa（XZ2 + XZT）**：
```bash
java -cp TMan-1.0-SNAPSHOT.jar:$HBASE_CP \
  huajunge.github.io.spatialman.experiments.just.JUSTStoring \
  <source_path> <resolution> <alpha> <beta> <timeBin> <timeBinNums> <compress> \
  <xmin> <ymin> <xmax> <ymax> \
  <shards> SPATIAL <table_name> <redis_host> <timePeriod>
#  timePeriod: 1=Day, 2=Week, 3=Month, 4=Year
```

### Redis Key 命名规则

灌库后自动创建的 Redis key（zset 类型）：

| 索引类型 | Key 格式 | TMan 示例 | TrajMesa 示例 |
|------|------|------|------|
| 空间索引 | `<table_name>` | `tman_st` | `trajmesa_st` |
| 时间索引 | `<table_name>_temporal` | `tman_st_temporal` | `trajmesa_st_temporal` |
| OID 索引 | `<table_name>_oid` | `tman_st_oid` | `trajmesa_st_oid` |
| 元数据表 | `<table_name>_meta` (HBase) | `tman_st_meta` | `trajmesa_st_meta` |

### 当前集群已有表

| HBase 表 | 体系 | 空间索引 | 时间索引 | 备注 |
|------|------|------|------|------|
| `tdwx_st` | TMan (`IS_XZ=1`) | **XZLocSIndex**（非 TShape！） | TRIndex | 旧数据，不是纯 TMan 方案 |
| `tman_full` | TMan (`IS_XZ=0`) | **TShape** | TRIndex | ✅ 正确 TMan 方案 |
| `trass_full` | TraSS | - | - | 独立系统 |

> **注**：`tman_full` 的 `IS_XZ=0` 见灌库命令末尾 `0 0`（isXZ=0, tspEncoding=0），空间索引用 LocSIndex (TShape)。但 `tdwx_st` 用 `IS_XZ=1`，空间索引用的是 XZLocSIndex，属于混合方案。

### 待实施：两次独立灌库

要在三节点集群上部署两套完整的 TMan-tsp 索引系统：

```
灌库1 — TMan：      TMANStoring  IS_XZ=0  → 表名 tman_st      → TShape + TRIndex
灌库2 — TrajMesa：  JUSTStoring           → 表名 trajmesa_st  → XZ2 + XZT
```

- 数据源：`~/dataset/processed/tdwx/`（按节点分 shard0/1/2）
- 每个节点分别灌对应 shard
- 两套系统互不干扰：不同的 HBase 表 + 不同的 Redis key

---

## 已知问题

1. **YARN NodeManager 偶尔挂**: 08 NM 被 shutdown 后未自动重启。需要手动 `/home/lfh/apps/hadoop/sbin/yarn-daemon.sh start nodemanager`。
2. **TMan-tsp SpatioTemporalFilter 不完整**: 缺少 HBase 序列化方法(toByteArray/parseFrom)，因此不能直接作为 HBase Filter 使用。时空查询通过 SPATIAL 主索引 + TemporalFilter 后过滤实现，功能等价。
3. **SSH keys 失效**: 节点间 SSH 有时会断，从 06 `sshpass -p lfh ssh-copy-id lfh@$HOST` 修复。

# 上传到 startserver06 的 TMan-RocksDB 灌库包

## 压缩包内容

- rocksdbjni-*-linux64.jar  — ST fork JNI（Linux），由本机 Docker 编译
- SERVER_README.txt         — 本说明
- version_set-popcnt.patch  — 备用（服务器若从源码编 rocksdb 需打此补丁）

## 服务器步骤

### 1. 解压

```bash
mkdir -p ~/downloads/tman_rocksdb_bundle
cd ~/downloads/tman_rocksdb_bundle
# 把 zip 里的 jar 放这里
ls -lh rocksdbjni-*-linux64.jar
```

### 2. 安装 jni jar

```bash
mkdir -p ~/Tman/rocksdb/build/java
cp rocksdbjni-*-linux64.jar ~/Tman/rocksdb/build/java/
```

### 3. 离线 Maven 打 TMan-RocksDB 包

```bash
export JAVA_HOME=/usr/lib/jvm/java-8-openjdk-amd64
export PATH=$HOME/apps/apache-maven-3.9.6/bin:$PATH

# TMan-tsp 已在 ~/.m2 可跳过；若没有：
# mkdir -p ~/.m2/repository/org/example/TMan/1.0-SNAPSHOT
# cp ~/Tman/TMan-tsp/target/TMan-1.0-SNAPSHOT.jar ~/.m2/repository/org/example/TMan/1.0-SNAPSHOT/

cat > ~/.m2/settings-offline.xml <<'EOF'
<settings xmlns="http://maven.apache.org/SETTINGS/1.2.0">
  <offline>true</offline>
</settings>
EOF

JNI_JAR=$(ls ~/Tman/rocksdb/build/java/rocksdbjni-*-linux64.jar)
cd ~/Tman/TMan-RocksDB
mvn -o -s ~/.m2/settings-offline.xml package -DskipTests \
  "-Drocksdb.fork.jar.path=$JNI_JAR"

ls -lh target/TMan-RocksDB-1.0-SNAPSHOT-jar-with-dependencies.jar
```

### 4. Redis + 灌库（RocksDB，不是 HBase）

```bash
mkdir -p ~/data/redis ~/tmp
nohup ~/apps/redis/bin/redis-server ~/apps/redis.conf >> ~/data/redis/redis.log 2>&1 &
sleep 2
~/apps/redis/bin/redis-cli ping

bash ~/Tman/tools/run_tman_rocksdb_tdrive_ingest.sh z_order
```

### 5. 验证

```bash
ls ~/dataset/runs/tman_rocksdb/tdrive_z/CURRENT
~/apps/redis/bin/redis-cli zcard tdrive_z
```

日志中必须是 **TmanRocksStoring**，不是 TMANStoring。

### 6. dense20 查询实验（20 窗 × 2 suite，warmup + 10 轮）

需有查询集（若缺失，从本机拷 `tdrive_query_st/uniform_dense20` 与 `skewed_dense20`）：

```bash
ls ~/dataset/processed/tman_tdrive/tdrive_query_st/uniform_dense20/query_conditions_st.txt

chmod +x ~/Tman/tools/run_tman_rocksdb_tdrive_query.sh \
         ~/Tman/tools/run_tman_rocksdb_dense20_10round_server.sh

# 约 11 次 JVM（warmup + run_01..10），每轮 40 条 ST 查询（20+20）
bash ~/Tman/tools/run_tman_rocksdb_dense20_10round_server.sh

# 汇总（可在本机对拷回的 Result 目录跑）
# python tools/summarize_tman_rocksdb_dense20_10round.py --root Result/TSV/tman_rocksdb_tdrive_dense20_10round
```

结果目录：`~/Result/TSV/tman_rocksdb_tdrive_dense20_10round/{warmup,run_01..run_10}/z_order_*.csv`

## 数据（应已准备好）

- ~/dataset/processed/tman_tdrive/tdrive_trajs.txt
- ~/dataset/processed/tman_tdrive/tdrive_envelope.json
- ~/dataset/processed/tman_tdrive/tdrive_query_st/（dense20 查询）

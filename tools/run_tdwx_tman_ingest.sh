#!/usr/bin/env bash
# TMan-RocksDB ingest for TDrive+Wuxi (td_wx) dataset
set -euo pipefail

JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-8-openjdk-amd64}"
APP_JAR="$HOME/Tman/TMan-RocksDB/target/TMan-RocksDB-1.0-SNAPSHOT-jar-with-dependencies.jar"
JNI_JAR="$HOME/Tman/rocksdb/build/java/rocksdbjni-11.2.0-linux64.jar"
MAIN="huajunge.github.io.spatialman.experiments.rocksdb.TmanRocksStoring"

DB="$HOME/dataset/runs/tman_rocksdb/tdwx_st_z"
SRC="$HOME/dataset/processed/tdwx/tdwx_trajs.txt"
TABLE="tdwx_st_z"

echo "=== TMan-RocksDB td_wx ingest ==="
echo "Started at $(date)"
echo "DB=$DB"
echo "Source=$SRC"
echo "Table=$TABLE"

# Remove old DB if exists
if [ -f "$DB/CURRENT" ]; then
    echo "Removing old DB at $DB"
    rm -rf "$DB"
fi

# Ensure Redis
~/apps/redis/bin/redis-cli ping || { echo "ERROR: Redis not running"; exit 1; }

java -Djava.io.tmpdir="$HOME/tmp" \
  -cp "${JNI_JAR}:${APP_JAR}" \
  "${MAIN}" \
  --db "${DB}" \
  --source "${SRC}" \
  --table "${TABLE}" \
  --primary ST \
  --resolution 10 --alpha 4 --beta 4 \
  --time-bin 3600 --time-bin-nums 109932 \
  --xmin 76.779245 --ymin 12.1525216 --xmax 133.1606633 --ymax 45.0662583 \
  --compress DELTA_COMPRESS_VGB \
  --redis-host 127.0.0.1 --is-xz 0 --tsp-encoding 0

echo "=== Done at $(date) ==="

# Verify
echo "DB CURRENT check:"
ls -la "$DB/CURRENT"
echo "Redis zcard:"
~/apps/redis/bin/redis-cli zcard "${TABLE}"

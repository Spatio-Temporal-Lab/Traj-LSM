#!/usr/bin/env bash
# Build ST fork rocksdbjni (linux) + TMan-RocksDB on the server. No HBase involved.
set -euo pipefail

export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-8-openjdk-amd64}"
export PATH="${HOME}/apps/apache-maven-3.9.6/bin:${PATH}"

TMAN_ROOT="${TMAN_ROOT:-${HOME}/Tman}"
ROCKSDB_FORK="${ROCKSDB_FORK:-${TMAN_ROOT}/rocksdb}"
TMAN_TSP="${TMAN_TSP:-${TMAN_ROOT}/TMan-tsp}"
TMAN_ROCKS="${TMAN_ROCKS:-${TMAN_ROOT}/TMan-RocksDB}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

die() { echo "ERROR: $*" >&2; exit 1; }

[[ -d "${ROCKSDB_FORK}" ]] || die "missing fork tree: ${ROCKSDB_FORK}"
[[ -d "${TMAN_TSP}" ]] || die "missing TMan-tsp: ${TMAN_TSP}"
[[ -d "${TMAN_ROCKS}" ]] || die "missing TMan-RocksDB: ${TMAN_ROCKS}"
command -v cmake >/dev/null || die "cmake not installed"
command -v g++ >/dev/null || die "g++ not installed"

df -h "${HOME}" | tail -1
avail="$(df -k "${HOME}" | tail -1 | awk '{print $4}')"
[[ "${avail}" -gt 5242880 ]] || die "need >5GB free disk under HOME (df shows low space)"

echo "[build] TMan-tsp install ..."
(cd "${TMAN_TSP}" && mvn install -DskipTests -q)

BUILD_DIR="${ROCKSDB_FORK}/build"
mkdir -p "${BUILD_DIR}"
echo "[build] cmake rocksdb WITH_JNI=1 ..."
cmake -S "${ROCKSDB_FORK}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" -DWITH_JNI=1
cmake --build "${BUILD_DIR}" --target rocksdbjava -j "$(nproc)"

JNI_JAR="$(find "${BUILD_DIR}/java" -name 'rocksdbjni-*-linux64.jar' | sort | tail -1)"
[[ -n "${JNI_JAR}" ]] || die "rocksdbjni linux jar not found under ${BUILD_DIR}/java"

mvn install:install-file \
  -Dfile="${JNI_JAR}" \
  -DgroupId=org.rocksdb \
  -DartifactId=rocksdbjni \
  -Dversion=11.2.0-st-fork \
  -Dpackaging=jar \
  -DgeneratePom=true

echo "[build] TMan-RocksDB package ..."
(cd "${TMAN_ROCKS}" && mvn package -DskipTests -q \
  "-Drocksdb.fork.jar.path=${JNI_JAR}")

echo "[build] Done."
echo "  jni : ${JNI_JAR}"
echo "  app : ${TMAN_ROCKS}/target/TMan-RocksDB-1.0-SNAPSHOT-jar-with-dependencies.jar"

#!/usr/bin/env bash
# Build ST-fork rocksdbjni (linux64) inside Docker.
set -euo pipefail

SRC_MOUNT="${SRC_MOUNT:-/src/rocksdb}"
WORK_SRC="${WORK_SRC:-/work/rocksdb}"
OUT_DIR="${OUT_DIR:-/out}"
CCACHE_DIR="${CCACHE_DIR:-/ccache}"
LOG="${OUT_DIR}/cmake-build.log"
JOBS="${JOBS:-$(nproc)}"

export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-8-openjdk-amd64}"
export PATH="${JAVA_HOME}/bin:${PATH}"
export CCACHE_DIR
export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-5G}"

mkdir -p "${OUT_DIR}" "${CCACHE_DIR}"
: > "${LOG}"

echo "[build] jobs=${JOBS} ccache=${CCACHE_DIR}" | tee -a "${LOG}"

echo "[build] copy source -> ${WORK_SRC} ..." | tee -a "${LOG}"
rm -rf "${WORK_SRC}"
mkdir -p "$(dirname "${WORK_SRC}")"
cp -a "${SRC_MOUNT}/." "${WORK_SRC}/"

cd "${WORK_SRC}"
if [[ ! -d build ]] || [[ ! -f build/build.ninja ]]; then
  rm -rf build
  echo "[build] cmake configure (Ninja) ..." | tee -a "${LOG}"
  cmake -S . -B build -G Ninja \
    -DCMAKE_CXX_STANDARD=20 \
    -DCMAKE_BUILD_TYPE=Release \
    -DPORTABLE=1 \
    -DFAIL_ON_WARNINGS=OFF \
    -DROCKSDB_BUILD_SHARED=OFF \
    -DWITH_JNI=1 \
    -DWITH_GFLAGS=ON \
  -DWITH_LZ4=ON \
    -DWITH_BENCHMARK_TOOLS=OFF \
    -DWITH_CORE_TOOLS=OFF \
    -DWITH_TOOLS=OFF \
    -DWITH_TRACE_TOOLS=OFF \
    -DWITH_LIBURING=OFF 2>&1 | tee -a "${LOG}"
else
  echo "[build] reuse existing build/ (skip cmake)" | tee -a "${LOG}"
fi

echo "[build] compile rocksdbjava -j${JOBS} ..." | tee -a "${LOG}"
cmake --build build --target rocksdbjava -j "${JOBS}" 2>&1 | tee -a "${LOG}"

ccache -s 2>&1 | tee -a "${LOG}" || true

JAR="$(find build/java -name 'rocksdbjni-*-linux64.jar' | sort | tail -1)"
[[ -n "${JAR}" ]] || { echo "ERROR: rocksdbjni linux jar not found"; exit 1; }
cp -f "${JAR}" "${OUT_DIR}/"
ls -lh "${OUT_DIR}/"
echo "[build] OK: ${OUT_DIR}/$(basename "${JAR}")"

#!/usr/bin/env bash
# TMan spatial index variants × 12-spatial (MBR-only) query benchmark.
# Run on the TMan server (HBase + Redis), NOT on Windows dev machine.
#
# Prereqs on server:
#   - ~/Tman/TMan-spatial/target/TMan-spatial-1.0-SNAPSHOT-jar-with-dependencies.jar
#   - ~/dataset/processed/tman_wuxi/wuxi_trajs.txt
#   - ~/dataset/processed/tman_wuxi/wuxi_query_stratified12_spatial/query_conditions.txt
#   - bash tools/verify_tman_hbase.sh  (Redis PONG, HBase up)
#
# Sync from Project (local) once:
#   rsync -avP data/processed/tman_wuxi/wuxi_trajs.txt \
#     data/processed/tman_wuxi/wuxi_envelope.json \
#     data/processed/tman_wuxi/wuxi_query_stratified12_spatial/ \
#     USER@HOST:~/dataset/processed/tman_wuxi/
#
# Usage:
#   bash tools/run_tman_spatial12_benchmark.sh all4         # ingest + query 4 TMAN variants (no LETI)
#   bash tools/run_tman_spatial12_benchmark.sh all          # ingest + query all 6 variants (needs LETI order)
#   bash tools/run_tman_spatial12_benchmark.sh ingest       # ingest only
#   bash tools/run_tman_spatial12_benchmark.sh query          # query existing tables only
#   bash tools/run_tman_spatial12_benchmark.sh query xz_order
#
set -euo pipefail

MODE="${1:-all}"
ONLY_VARIANT="${2:-}"

export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-8-openjdk-amd64}"
export HBASE_CONF_DIR="${HBASE_CONF_DIR:-$HOME/apps/hbase-2.1.2/conf}"
export PATH="${HOME}/apps/redis/bin:${PATH}"
[[ -f "${HOME}/.tman_env" ]] && source "${HOME}/.tman_env"

HBASE_HOME="${HBASE_HOME:-$HOME/apps/hbase-2.1.2}"
JAR="${JAR:-$HOME/Tman/TMan-spatial/target/TMan-spatial-1.0-SNAPSHOT-jar-with-dependencies.jar}"

DATA_ROOT="${DATA_ROOT:-$HOME/dataset/processed/tman_wuxi}"
TRAJ="${TRAJ:-${DATA_ROOT}/wuxi_trajs.txt}"
QUERY_FILE="${QUERY_FILE:-${DATA_ROOT}/wuxi_query_stratified12_spatial/query_conditions.txt}"
OUT_ROOT="${OUT_ROOT:-$HOME/tman_benchmark/spatial12_$(date +%Y%m%d_%H%M%S)}"
RESULT_TSV_ROOT="${RESULT_TSV_ROOT:-$HOME/Result/TSV/tman_spatial12}"

# Wuxi envelope (from wuxi_envelope.json)
XMIN="${XMIN:-76.779245}"
YMIN="${YMIN:-12.1525216}"
XMAX="${XMAX:-133.1606633}"
YMAX="${YMAX:-45.0662583}"
TIME_BIN="${TIME_BIN:-3600}"
TIME_BIN_NUMS="${TIME_BIN_NUMS:-736}"
RESOLUTION="${RESOLUTION:-10}"
SHARDS="${SHARDS:-4}"
REDIS_HOST="${REDIS_HOST:-127.0.0.1}"

# LETI order JSON (Wuxi envelope, resolution/alpha/beta must match ingest params below).
# Generate offline first (see tools/TMAN_LETI_WUXI_ORDER.md); then either:
#   - filesystem: $HOME/Tman/TMan-spatial/leti/wuxi/skew_order.json
#   - classpath:    leti/wuxi/skew_order.json under src/main/resources/ + mvn package
LETI_ORDER="${LETI_ORDER:-$HOME/Tman/TMan-spatial/leti/wuxi/skew_order.json}"

HBASE_CP="$("${HBASE_HOME}/bin/hbase" classpath 2>/dev/null || true)"
if [[ -z "${HBASE_CP}" ]]; then
  HBASE_CP="${HBASE_CONF_DIR}"
fi
FULL_CP="${HBASE_CP}:${JAR}"

die() { echo "ERROR: $*" >&2; exit 1; }

require_files() {
  [[ -f "${JAR}" ]] || die "missing JAR: ${JAR}"
  [[ -f "${TRAJ}" ]] || die "missing trajectories: ${TRAJ}"
  [[ -f "${QUERY_FILE}" ]] || die "missing 12-spatial queries: ${QUERY_FILE}"
  "${HOME}/apps/redis/bin/redis-cli" ping | grep -q PONG || die "Redis not responding"
}

build_cp() {
  echo "JAVA_HOME=${JAVA_HOME}"
  echo "JAR=${JAR}"
  echo "TRAJ=${TRAJ}"
  echo "QUERY_FILE=${QUERY_FILE}"
  echo "OUT_ROOT=${OUT_ROOT}"
}

# variant_name:table_suffix:isXZ:alpha:beta:tspEncoding:loader:adaptivePartition
# README mapping (TMan-spatial):
#   Z-Order baseline  -> LocS without TSP (isXZ=0, tspEncoding=0)
#   LocS + TSP        -> TShape (isXZ=0, tspEncoding=1)
#   XZ-Order          -> XZPlus (isXZ=1, alpha=beta=4, tspEncoding=0)
#   XZ* (paper)       -> XZ_STAR (isXZ=1, alpha=beta=2, tspEncoding=0)
#   LETI + TSP        -> LetiStoring tsp default
#   adaptive partition-> LetiStoring adaptivePartition=1 (LETI only)
VARIANTS_ALL=(
  "z_order:wuxi_z:0:4:4:0:tman:0"
  "locs_tsp:wuxi_locs:0:4:4:1:tman:0"
  "xz_order:wuxi_xz:1:4:4:0:tman:0"
  "xz_star:wuxi_xzstar:1:2:2:0:tman:0"
  "leti:wuxi_leti:0:4:4:1:leti:0"
  "leti_adaptive:wuxi_leti_ap:0:4:4:1:leti:1"
)
VARIANTS_4=(
  "z_order:wuxi_z:0:4:4:0:tman:0"
  "locs_tsp:wuxi_locs:0:4:4:1:tman:0"
  "xz_order:wuxi_xz:1:4:4:0:tman:0"
  "xz_star:wuxi_xzstar:1:2:2:0:tman:0"
)
VARIANTS=("${VARIANTS_ALL[@]}")

should_run_variant() {
  local name="$1"
  [[ -z "${ONLY_VARIANT}" || "${ONLY_VARIANT}" == "${name}" ]]
}

ingest_tman() {
  local table="$1" is_xz="$2" alpha="$3" beta="$4" tsp="$5"
  local store_out="${OUT_ROOT}/store/${table}"
  mkdir -p "${store_out}"
  echo ""
  echo "=== INGEST TMANStoring table=${table} isXZ=${is_xz} tsp=${tsp} alpha=${alpha} beta=${beta} ==="
  java -cp "${FULL_CP}" experiments.tman.TMANStoring \
    "${TRAJ}" "${RESOLUTION}" "${alpha}" "${beta}" \
    "${TIME_BIN}" "${TIME_BIN_NUMS}" VGB \
    "${XMIN}" "${YMIN}" "${XMAX}" "${YMAX}" \
    "${SHARDS}" SPATIAL "${table}" "${REDIS_HOST}" \
    "${is_xz}" "${tsp}" "${store_out}"
  local zcard
  zcard="$("${HOME}/apps/redis/bin/redis-cli" zcard "${table}" 2>/dev/null || echo 0)"
  echo "Redis zcard ${table}=${zcard}"
  [[ "${zcard}" != "0" ]] || die "Redis index empty for ${table}"
}

ingest_leti() {
  local table="$1" alpha="$2" beta="$3" adaptive="$4"
  local store_out="${OUT_ROOT}/store/${table}"
  mkdir -p "${store_out}"
  if [[ ! -f "${LETI_ORDER}" ]]; then
    die "LETI order missing: ${LETI_ORDER}
Generate Wuxi skew_order.json first (see tools/TMAN_LETI_WUXI_ORDER.md).
Do NOT reuse leti/tdrive/skew_order.json (Beijing envelope)."
  fi
  echo ""
  echo "=== INGEST LetiStoring table=${table} adaptive=${adaptive} order=${LETI_ORDER} ==="
  java -cp "${FULL_CP}" experiments.tman.LetiStoring \
    "${TRAJ}" "${RESOLUTION}" "${alpha}" "${beta}" \
    "${TIME_BIN}" "${TIME_BIN_NUMS}" VGB \
    "${XMIN}" "${YMIN}" "${XMAX}" "${YMAX}" \
    "${SHARDS}" SPATIAL "${table}" "${REDIS_HOST}" \
    "${adaptive}" "${store_out}" "${LETI_ORDER}"
  local zcard
  zcard="$("${HOME}/apps/redis/bin/redis-cli" zcard "${table}" 2>/dev/null || echo 0)"
  echo "Redis zcard ${table}=${zcard}"
  [[ "${zcard}" != "0" ]] || die "Redis index empty for ${table}"
}

run_spatial_query() {
  local variant="$1" table="$2" loader="$3"
  local qpath="${OUT_ROOT}/query/${variant}.csv"
  mkdir -p "$(dirname "${qpath}")" "${RESULT_TSV_ROOT}"
  local conditions
  conditions="$(tr -d '\r\n' < "${QUERY_FILE}")"
  local query_main="experiments.tman.SpatialQuery"
  [[ "${loader}" == "leti" ]] && query_main="experiments.tman.LetiSpatialQuery"
  echo ""
  echo "=== QUERY ${variant} table=${table} -> ${qpath} ==="
  java -cp "${FULL_CP}" "${query_main}" \
    "${table}" "${conditions}" "${qpath}"
  cp -f "${qpath}" "${RESULT_TSV_ROOT}/${variant}_stratified12_spatial.csv"
  echo "Copied -> ${RESULT_TSV_ROOT}/${variant}_stratified12_spatial.csv"
  grep '^time,' "${qpath}" || true
}

parse_variant() {
  local spec="$1"
  IFS=':' read -r variant_name table_suffix is_xz alpha beta tsp loader adaptive <<< "${spec}"
  TABLE="${table_suffix}"
  VARIANT="${variant_name}"
  IS_XZ="${is_xz}"
  ALPHA="${alpha}"
  BETA="${beta}"
  TSP="${tsp}"
  LOADER="${loader}"
  ADAPTIVE="${adaptive}"
}

do_ingest() {
  for spec in "${VARIANTS[@]}"; do
    parse_variant "${spec}"
    should_run_variant "${VARIANT}" || continue
    if [[ "${LOADER}" == "leti" ]]; then
      ingest_leti "${TABLE}" "${ALPHA}" "${BETA}" "${ADAPTIVE}"
    else
      ingest_tman "${TABLE}" "${IS_XZ}" "${ALPHA}" "${BETA}" "${TSP}"
    fi
  done
}

do_query() {
  for spec in "${VARIANTS[@]}"; do
    parse_variant "${spec}"
    should_run_variant "${VARIANT}" || continue
    run_spatial_query "${VARIANT}" "${TABLE}" "${LOADER}"
  done
  echo ""
  echo "=== Summary (time row, avg ms) ==="
  for spec in "${VARIANTS[@]}"; do
    parse_variant "${spec}"
    should_run_variant "${VARIANT}" || continue
    local f="${RESULT_TSV_ROOT}/${VARIANT}_stratified12_spatial.csv"
    if [[ -f "${f}" ]]; then
      awk -F, '/^time,/{getline; print "'"${VARIANT}"' avg=" $4 " ms  (min=" $2 " max=" $3 ")"; exit}' "${f}"
    fi
  done
}

require_files
build_cp
mkdir -p "${OUT_ROOT}" "${RESULT_TSV_ROOT}"

case "${MODE}" in
  all4)
    VARIANTS=("${VARIANTS_4[@]}")
    echo "Mode all4: z_order, locs_tsp, xz_order, xz_star (no LETI)"
    do_ingest
    do_query
    ;;
  all)
    VARIANTS=("${VARIANTS_ALL[@]}")
    do_ingest
    do_query
    ;;
  ingest)
    do_ingest
    ;;
  query)
    do_query
    ;;
  *)
    die "unknown mode: ${MODE} (use all4|all|ingest|query)"
    ;;
esac

echo ""
echo "Done. Results under ${RESULT_TSV_ROOT}"

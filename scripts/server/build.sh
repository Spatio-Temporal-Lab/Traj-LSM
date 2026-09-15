#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
jobs="${JOBS:-4}"
mode="${1:-release}"
case "$mode" in
  release) build_type=Release; build_dir="$root/build-server" ;;
  test) build_type=Debug; build_dir="$root/build-server-tests" ;;
  *) printf 'Usage: bash scripts/server/build.sh [release|test]\n' >&2; exit 2 ;;
esac
cmake -S "$root/scripts/server" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE="$build_type" -DWITH_TESTS=ON
if [[ "$mode" == release ]]; then
  cmake --build "$build_dir" --target stlsm_tools --parallel "$jobs"
  printf 'Executables: %s/bin\n' "$build_dir"
else
  cmake --build "$build_dir" --target compaction_picker_test db_compaction_test --parallel "$jobs"
  export TEST_TMPDIR="$build_dir/test-data"
  mkdir -p "$TEST_TMPDIR"
  "$build_dir/bin/compaction_picker_test" '--gtest_filter=*NeedsCompaction*:*STRunCount*'
  "$build_dir/bin/db_compaction_test" '--gtest_filter=DBCompactionTest.STRunCountSchedulesBelowCapacity:DBCompactionTest.ManualCompactionUnknownOutputSize'
fi

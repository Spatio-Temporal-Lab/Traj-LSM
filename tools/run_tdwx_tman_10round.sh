#!/usr/bin/env bash
# TMan-RocksDB: 10-round repeat query for td_wx (6 windows, per-window stats)
# Each round runs all 6 windows in one JVM; per-query time/finalSize parsed from stdout.
# Usage: bash run_tdwx_tman_10round.sh
set -euo pipefail

JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-8-openjdk-amd64}"
APP_JAR="$HOME/Tman/TMan-RocksDB/target/TMan-RocksDB-1.0-SNAPSHOT-jar-with-dependencies.jar"
JNI_JAR="$HOME/Tman/rocksdb/build/java/rocksdbjni-11.2.0-linux64.jar"
MAIN="huajunge.github.io.spatialman.experiments.rocksdb.TmanRocksSTQuery"

DB="$HOME/dataset/runs/tman_rocksdb/tdwx_st_z"
TABLE="tdwx_st_z"
CONDITIONS="$HOME/dataset/processed/tdwx/tdwx_query_st.txt"
RESULT_DIR="$HOME/Result/tdwx_comparison/tman"

ROUNDS=10

# Verify prerequisites
[ -f "$DB/CURRENT" ] || { echo "ERROR: DB not found at $DB"; exit 1; }
[ -f "$CONDITIONS" ] || { echo "ERROR: conditions not found"; exit 1; }
~/apps/redis/bin/redis-cli ping | grep -q PONG || { echo "ERROR: Redis not running"; exit 1; }

# Window labels (must match order in conditions file)
WIN_LABELS=("td_narrow" "td_medium" "td_wide" "wx_narrow" "wx_medium" "wx_wide")

mkdir -p "$RESULT_DIR"

echo "=== TMan-RocksDB td_wx 10-round query ==="
echo "DB=$DB  Table=$TABLE"
echo "Conditions: $(wc -l < "$CONDITIONS") windows"
echo "Rounds: $ROUNDS"
echo "Output: $RESULT_DIR"
echo "Started at $(date)"

# Run 10 rounds
for rnd in $(seq -w 1 $ROUNDS); do
    out_csv="$RESULT_DIR/tman_r${rnd}.csv"
    out_log="$RESULT_DIR/tman_r${rnd}.log"
    echo ""
    echo "======== Round $rnd / $ROUNDS ========"
    java -Djava.io.tmpdir="$HOME/tmp" \
      -cp "${JNI_JAR}:${APP_JAR}" \
      "${MAIN}" \
      --db "${DB}" \
      --table "${TABLE}" \
      --conditions-file "${CONDITIONS}" \
      --out "${out_csv}" \
      2>&1 | tee "${out_log}"
    echo "Round $rnd done: $out_csv"
done

echo ""
echo "=== All rounds complete at $(date) ==="

# ── Parse per-round per-window stats from logs ──
echo ""
echo "=============================================="
echo "SUMMARY — Per-Window Trimmed Mean (drop max/min, avg of 8)"
echo "=============================================="

declare -A all_times all_sizes

for rnd in $(seq -w 1 $ROUNDS); do
    log="$RESULT_DIR/tman_r${rnd}.log"
    if [ -f "$log" ]; then
        # Extract per-query lines: [TmanRocksSTQuery] time=N ms ... finalSize=N
        # Map query index to window label
        qi=0
        while IFS= read -r line; do
            if [[ "$line" =~ time=([0-9]+)\ ms.*finalSize=([0-9]+) ]]; then
                t="${BASH_REMATCH[1]}"
                s="${BASH_REMATCH[2]}"
                label="${WIN_LABELS[$qi]:-q$qi}"
                all_times["${label}_${rnd}"]="$t"
                all_sizes["${label}_${rnd}"]="$s"
                qi=$((qi + 1))
            fi
        done < "$log"
    fi
done

# Print per-window summary
printf "%-15s %8s %10s %10s %8s %10s\n" "Window" "finalSz" "TrimAvg_ms" "Avg_ms" "Min_ms" "Max_ms"
echo "----------------------------------------------------------------"

for label in "${WIN_LABELS[@]}"; do
    times=()
    sizes=()
    for rnd in $(seq -w 1 $ROUNDS); do
        t="${all_times["${label}_${rnd}"]:-}"
        s="${all_sizes["${label}_${rnd}"]:-}"
        if [ -n "$t" ]; then
            times+=("$t")
            sizes+=("$s")
        fi
    done

    if [ ${#times[@]} -eq 0 ]; then
        echo "  $label: NO DATA"
        continue
    fi

    # Check size consistency
    size_ok=1
    first_s="${sizes[0]}"
    for s in "${sizes[@]}"; do
        [ "$s" == "$first_s" ] || size_ok=0
    done

    # Compute trimmed mean (bash sort + drop first/last)
    sorted_times=($(printf '%s\n' "${times[@]}" | sort -n))
    n=${#sorted_times[@]}
    if [ "$n" -ge 4 ]; then
        # Drop min and max
        sum=0; count=0
        for ((i=1; i<n-1; i++)); do
            sum=$((sum + sorted_times[i]))
            count=$((count + 1))
        done
        trim_avg=$((sum / count))
    else
        sum=0
        for t in "${sorted_times[@]}"; do sum=$((sum + t)); done
        trim_avg=$((sum / n))
    fi

    # Full average
    sum=0
    for t in "${times[@]}"; do sum=$((sum + t)); done
    full_avg=$((sum / ${#times[@]}))

    size_flag="OK"
    [ "$size_ok" -eq 0 ] && size_flag="MISMATCH"

    printf "%-15s %8s %10d %10d %8d %10d  %s\n" \
        "$label" "$first_s" "$trim_avg" "$full_avg" \
        "${sorted_times[0]}" "${sorted_times[$((n-1))]}" "$size_flag"
done

echo ""
echo "Raw logs: $RESULT_DIR/tman_r*.log"
echo "Done."

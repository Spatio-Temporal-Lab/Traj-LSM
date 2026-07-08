# Shared ablation experiment configuration

$ROOT   = "D:\Project"
$BUILD  = "$ROOT\rocks-demo\build"
$DB     = "$ROOT\data\db\tdrive_st"          # standard STFP-R4 DB
$WINDOWS_CSV = "$ROOT\data\processed\unified\unified_stratified12_st_windows.csv"
$META_CSV    = "$ROOT\data\processed\unified\segments_meta.csv"
$POINTS_CSV  = "$ROOT\data\processed\unified\segments_points.csv"
$OUT_DIR     = "$ROOT\Result\ablation"
$REPEATS     = 10
$BUCKET_SEC  = 86400

# Exe paths
$READ_BENCH      = "$BUILD\st_meta_read_bench.exe"
$COMPACTION_BENCH = "$BUILD\st_compaction_throughput_bench.exe"

# Common flags for st_meta_read_bench
$COMMON_FLAGS = @(
    "--db", $DB,
    "--windows-csv", $WINDOWS_CSV,
    "--segment-meta-csv", $META_CSV,
    "--segment-points-csv", $POINTS_CSV,
    "--bucket-width-sec", $BUCKET_SEC,
    "--block-cache-mb", "512",
    "--iterator-repeat", "3",
    "--no-full-scan"           # prune-scan only for ablation
)

# Ensure output directory
New-Item -ItemType Directory -Force -Path $OUT_DIR | Out-Null
Write-Output "Config loaded: DB=$DB, windows=$WINDOWS_CSV, out=$OUT_DIR"

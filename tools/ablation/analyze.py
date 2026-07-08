"""
ST-LSM Ablation Analysis & Plotting
====================================
Reads TSV output from ablation experiments and generates:
  1. Summary tables (mean, std across repeats)
  2. Comparison bar charts per experiment
  3. Speedup ratios (variant / baseline)
"""

import sys, os, glob, csv
from collections import defaultdict
import math

def load_tsv(path):
    """Load ablation TSV and return {experiment: {variant: {window: {metric: [values]}}}}"""
    data = defaultdict(lambda: defaultdict(lambda: defaultdict(lambda: defaultdict(list))))
    with open(path, "r") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            exp = row["experiment"]
            var = row["variant"]
            wid = row["window_id"]
            met = row["metric"]
            val = float(row["value"])
            data[exp][var][wid][met].append(val)
    return data

def summarize(data):
    """Print summary table for each experiment."""
    for exp, variants in data.items():
        print(f"\n{'='*60}")
        print(f"Experiment: {exp}")
        print(f"{'='*60}")
        
        # Find baseline variant
        baseline = None
        for v in variants:
            if "baseline" in v or "no_file" in v or "file_only" in v or "all_leveled" in v:
                baseline = v
                break
        if baseline is None:
            baseline = list(variants.keys())[0]
        
        # Key metrics to show
        key_metrics = [
            "prune_wall_ms", "files_skipped", "files_considered",
            "index_entries_examined", "index_entries_skipped",
            "data_blocks_read", "keys_examined", "false_negative_count"
        ]
        
        print(f"\nBaseline: {baseline}")
        print(f"{'variant':<25} {'metric':<30} {'mean':>10} {'std':>8} {'vs baseline':>12}")
        print("-" * 85)
        
        for variant in sorted(variants.keys()):
            for metric in key_metrics:
                all_vals = []
                for wid in variants[variant]:
                    all_vals.extend(variants[variant][wid].get(metric, []))
                if not all_vals:
                    continue
                
                mean = sum(all_vals) / len(all_vals)
                var = sum((x - mean)**2 for x in all_vals) / len(all_vals)
                std = math.sqrt(var)
                
                # Compare to baseline
                base_vals = []
                for wid in variants[baseline]:
                    base_vals.extend(variants[baseline][wid].get(metric, []))
                if base_vals:
                    base_mean = sum(base_vals) / len(base_vals)
                    ratio = (mean / base_mean - 1) * 100 if base_mean > 0 else 0
                    cmp = f"{ratio:+.1f}%"
                else:
                    cmp = "N/A"
                
                print(f"{variant:<25} {metric:<30} {mean:>10.1f} {std:>8.1f} {cmp:>12}")

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--dir", required=True, help="Ablation output directory")
    args = parser.parse_args()
    
    # Find all TSV files
    tsv_files = glob.glob(os.path.join(args.dir, "*.tsv"))
    if not tsv_files:
        # Try subdirectories
        for root, dirs, files in os.walk(args.dir):
            for f in files:
                if f.endswith(".tsv"):
                    tsv_files.append(os.path.join(root, f))
    
    if not tsv_files:
        print(f"No TSV files found in {args.dir}")
        sys.exit(1)
    
    print(f"Found {len(tsv_files)} TSV files")
    
    # Load and summarize
    all_data = defaultdict(lambda: defaultdict(lambda: defaultdict(lambda: defaultdict(list))))
    for tsv in tsv_files:
        data = load_tsv(tsv)
        for exp, variants in data.items():
            for var, windows in variants.items():
                for wid, metrics in windows.items():
                    for met, vals in metrics.items():
                        all_data[exp][var][wid][met].extend(vals)
    
    summarize(all_data)
    
    # Output summary TSV
    summary_path = os.path.join(args.dir, "ablation_summary.tsv")
    with open(summary_path, "w", newline="") as f:
        writer = csv.writer(f, delimiter="\t")
        writer.writerow(["experiment", "variant", "metric", "mean", "std", "n"])
        for exp, variants in all_data.items():
            for var, windows in variants.items():
                for met in key_metrics_for(exp):
                    all_vals = []
                    for wid in windows:
                        all_vals.extend(windows[wid].get(met, []))
                    if not all_vals:
                        continue
                    mean = sum(all_vals) / len(all_vals)
                    std = math.sqrt(sum((x-mean)**2 for x in all_vals) / len(all_vals))
                    writer.writerow([exp, var, met, f"{mean:.1f}", f"{std:.1f}", len(all_vals)])
    
    print(f"\nSummary saved to: {summary_path}")

def key_metrics_for(exp):
    base = ["prune_wall_ms", "files_skipped", "index_entries_examined",
            "index_entries_skipped", "data_blocks_read", "false_negative_count"]
    if "compaction" in exp.lower():
        base += ["read_throughput_mbs", "write_throughput_mbs", "compaction_write_amp"]
    if "cutoff" in exp.lower():
        base += ["block_level_index_prune_ns"]
    return base

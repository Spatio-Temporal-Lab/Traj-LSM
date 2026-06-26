#!/usr/bin/env python3
"""Compare TMan-RocksDB vs ST-RocksDB (E7) vs ST-RocksDB (TMan ST Key): latency + block prune."""

from __future__ import annotations

import argparse
import json
import re
import statistics
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

PROJECT = Path(__file__).resolve().parents[1]
SUITES = ("uniform_dense20", "skewed_dense20")
SUITE_LABELS = {"uniform_dense20": "Uniform dense20", "skewed_dense20": "Skewed dense20"}

# ST-Prune = sst_manifest (Local + Global)
MODE = "sst_manifest"

PAT = re.compile(
    r"spatial_batch_query id=\d+ time_ms=[\d.]+ finalSize=\d+"
    r"(?: keys_iterated=\d+)? keys_in_window=\d+"
    r" block_read_count=\d+ bytes_read=\d+ read_nanos=\d+"
    r" block_index_examined=(\d+) block_index_skipped=(\d+)"
    r" file_considered=(\d+) files_skipped=(\d+)"
)

DEFAULT_TMAN_ROOT = PROJECT / "Result/tman_tdrive_dense20_10round"
DEFAULT_TDRIVE_SUMMARY = (
    PROJECT / "Result/TSV/tdrive_dense20_TDrive_rerun_20260603/summary.json"
)
DEFAULT_ST_KEY_SUMMARY = (
    PROJECT / "Result/TSV/tdrive_tman_locs_dense20_10round/tman_locs_dense20_10round_summary.json"
)

SYSTEMS = [
    {
        "id": "tman_rocksdb",
        "label": "TMan-RocksDB",
        "color": "#4C72B0",
        "latency": "tman_root",
        "tman_root": DEFAULT_TMAN_ROOT,
        "log_glob": None,
    },
    {
        "id": "st_e7",
        "label": "ST-RocksDB (E7)",
        "color": "#FC8D62",
        "latency": "summary",
        "summary": DEFAULT_TDRIVE_SUMMARY,
        "log_root": PROJECT / "Result/TSV/tdrive_dense20_TDrive_rerun_20260603/ablation_10round",
        "log_rel": "{mode}/st_prune_{suite}.log",
    },
    {
        "id": "st_tman_key",
        "label": "ST-RocksDB (ST Key)",
        "color": "#1B9E77",
        "latency": "summary",
        "summary": DEFAULT_ST_KEY_SUMMARY,
        "log_roots": [
            PROJECT / "Result/TSV/tdrive_tman_locs_dense20_10round/ablation_10round",
            PROJECT / "Result/TSV/tdrive_tman_locs_dense20_10round/st_prune_10round",
        ],
        "log_rel": "{mode}/st_prune_{suite}.log",
        "log_rel_alt": "st_prune_{suite}.log",
    },
]


def load_summary(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def latency_from_summary(report: dict, suite: str) -> dict:
    s = report["suites"][suite][MODE]
    return {
        "avg_ms": s["avg_ms_mean"],
        "p90_ms": s["p90_ms_mean"],
        "n_rounds": s["n_rounds"],
        "mismatch": report.get("finalSize_mismatches", 0),
    }


def latency_from_tman_json(raw: dict, suite: str) -> dict:
    t = raw[suite]["TMan"]
    return {
        "avg_ms": t["avg_ms"],
        "p90_ms": t["p90_ms"],
        "n_rounds": t.get("rounds", 10),
        "mismatch": None,
    }


def parse_log_file(path: Path) -> list[dict]:
    if not path.is_file():
        return []
    text = path.read_text(encoding="utf-8", errors="replace")
    rows = []
    for line in text.splitlines():
        m = PAT.search(line)
        if not m:
            continue
        examined = int(m.group(1))
        skipped = int(m.group(2))
        rows.append(
            {
                "block_index_examined": examined,
                "block_index_skipped": skipped,
                "file_considered": int(m.group(3)),
                "files_skipped": int(m.group(4)),
            }
        )
    return rows


def collect_logs(root: Path, rel: str, rel_alt: str, suite: str) -> list[dict]:
    all_rows: list[dict] = []
    for run_dir in sorted(root.glob("run_*")):
        p = run_dir / rel.format(mode=MODE, suite=suite)
        if not p.is_file() and rel_alt:
            p = run_dir / rel_alt.format(suite=suite)
        all_rows.extend(parse_log_file(p))
    return all_rows


def summarize_prune(rows: list[dict]) -> dict:
    if not rows:
        return {}
    examined = [r["block_index_examined"] for r in rows]
    skipped = [r["block_index_skipped"] for r in rows]
    skip_pct = [
        100.0 * r["block_index_skipped"] / r["block_index_examined"]
        for r in rows
        if r["block_index_examined"] > 0
    ]
    return {
        "n_query_lines": len(rows),
        "block_index_examined_mean": statistics.mean(examined),
        "block_index_skipped_mean": statistics.mean(skipped),
        "block_index_skip_pct_mean": statistics.mean(skip_pct) if skip_pct else 0.0,
        "file_considered_mean": statistics.mean(r["file_considered"] for r in rows),
        "files_skipped_mean": statistics.mean(r["files_skipped"] for r in rows),
        "block_index_examined_sum": sum(examined),
        "block_index_skipped_sum": sum(skipped),
    }


def collect_tman_latency(tman_root: Path) -> dict[str, dict]:
    import csv

    out: dict[str, dict] = {s: {} for s in SUITES}
    for suite in SUITES:
        avgs, p90s = [], []
        for run_dir in sorted(tman_root.glob("run_*")):
            p = run_dir / f"z_order_{suite}.csv"
            if not p.is_file():
                continue
            with p.open(encoding="utf-8", newline="") as f:
                for row in csv.DictReader(f):
                    if row.get("type") == "time":
                        avgs.append(float(row["avg"]))
                        p90s.append(float(row.get("per90") or row.get("max") or 0))
        out[suite] = {
            "avg_ms": statistics.mean(avgs) if avgs else 0,
            "p90_ms": statistics.mean(p90s) if p90s else 0,
            "n_rounds": len(avgs),
            "mismatch": None,
        }
    return out


def build_report() -> dict:
    out: dict = {
        "protocol": "dense20 warmup+run_01..10; query=sst_manifest (ST-Prune L+G); point-truth finalSize",
        "validation": {
            "st_e7": "finalSize vs manifest truth: 0 mismatch (10 rounds)",
            "st_tman_key": "finalSize vs manifest truth: 0 mismatch (10 rounds)",
            "tman_rocksdb": "TMan z_order CSV 10-round (see Result/picture); no block logs in repo",
        },
        "systems": {},
    }

    for cfg in SYSTEMS:
        sid = cfg["id"]
        entry: dict = {"label": cfg["label"], "suites": {}}
        for suite in SUITES:
            suite_d: dict = {}
            if cfg["latency"] == "summary" and cfg["summary"].is_file():
                suite_d["latency"] = latency_from_summary(load_summary(cfg["summary"]), suite)
            elif cfg["latency"] == "tman_root":
                root = cfg.get("tman_root", DEFAULT_TMAN_ROOT)
                if Path(root).is_dir():
                    suite_d["latency"] = collect_tman_latency(Path(root))[suite]

            rows: list[dict] = []
            if sid == "st_e7" and cfg.get("log_root"):
                rows = collect_logs(cfg["log_root"], cfg["log_rel"], "", suite)
            elif sid == "st_tman_key":
                for root in cfg.get("log_roots", []):
                    rows.extend(
                        collect_logs(root, cfg["log_rel"], cfg.get("log_rel_alt", ""), suite)
                    )
            if rows:
                suite_d["prune"] = summarize_prune(rows)
            entry["suites"][suite] = suite_d
        out["systems"][sid] = entry
    return out


def plot_latency(report: dict, out_base: Path) -> None:
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.5), sharey=False)
    x = np.arange(len(SYSTEMS))
    w = 0.35
    ids = [c["id"] for c in SYSTEMS]

    for ax, suite in zip(axes, SUITES):
        avgs = [report["systems"][i]["suites"][suite]["latency"]["avg_ms"] for i in ids]
        p90s = [report["systems"][i]["suites"][suite]["latency"]["p90_ms"] for i in ids]
        colors = [c["color"] for c in SYSTEMS]
        b1 = ax.bar(x - w / 2, avgs, w, label="avg (ms)", color=colors, alpha=0.85)
        b2 = ax.bar(x + w / 2, p90s, w, label="P90 (ms)", color=colors, alpha=0.45, hatch="//")
        ax.set_xticks(x)
        ax.set_xticklabels([c["label"] for c in SYSTEMS], rotation=12, ha="right", fontsize=9)
        ax.set_ylabel("Query time (ms)")
        ax.set_title(SUITE_LABELS[suite])
        ax.grid(axis="y", alpha=0.3)
        for bars in (b1, b2):
            for bar in bars:
                h = bar.get_height()
                ax.text(
                    bar.get_x() + bar.get_width() / 2,
                    h,
                    f"{h:.0f}",
                    ha="center",
                    va="bottom",
                    fontsize=7,
                )
    axes[0].legend(loc="upper left", fontsize=8)
    fig.suptitle(
        "TDrive dense20 · 10 rounds · ST-Prune (sst_manifest)\n"
        "E7=TDrive rerun 20260603 · ST Key=tman_locs · TMan=z_order 10r (latest)",
        fontsize=10,
    )
    fig.tight_layout()
    fig.savefig(out_base.with_suffix(".png"), dpi=160)
    fig.savefig(out_base.with_suffix(".svg"))
    plt.close(fig)


def plot_prune(report: dict, out_base: Path) -> None:
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.5))
    # Only ST forks have prune logs
    st_ids = ["st_e7", "st_tman_key"]
    labels = [c["label"] for c in SYSTEMS if c["id"] in st_ids]
    colors = [c["color"] for c in SYSTEMS if c["id"] in st_ids]
    x = np.arange(len(st_ids))

    for ax, suite in zip(axes, SUITES):
        skipped = []
        skip_pct = []
        for sid in st_ids:
            pr = report["systems"][sid]["suites"][suite].get("prune", {})
            skipped.append(pr.get("block_index_skipped_mean", 0))
            skip_pct.append(pr.get("block_index_skip_pct_mean", 0))
        ax2 = ax.twinx()
        b = ax.bar(x, skipped, 0.5, color=colors, alpha=0.85, label="block_index_skipped (mean)")
        ax2.plot(x, skip_pct, "ko-", linewidth=2, markersize=8, label="skip %")
        ax.set_xticks(x)
        ax.set_xticklabels(
            ["ST-RocksDB (E7)", "ST-RocksDB (ST Key)"], rotation=10, ha="right", fontsize=9
        )
        ax.set_ylabel("Blocks skipped (index prune, mean/query)")
        ax2.set_ylabel("Skip ratio (% of examined)")
        ax.set_title(SUITE_LABELS[suite])
        ax.grid(axis="y", alpha=0.3)
        for bar, val in zip(b, skipped):
            ax.text(
                bar.get_x() + bar.get_width() / 2,
                bar.get_height(),
                f"{val:.0f}",
                ha="center",
                va="bottom",
                fontsize=8,
            )
        lines = [
            f"{labels[i]}: examined≈{report['systems'][st_ids[i]]['suites'][suite].get('prune', {}).get('block_index_examined_mean', 0):.0f}"
            for i in range(len(st_ids))
        ]
        ax.text(0.02, 0.98, "\n".join(lines), transform=ax.transAxes, va="top", fontsize=7)
    fig.suptitle(
        "Block-level ST prune (Local index): skipped disjoint blocks per query\n"
        "TMan-RocksDB: no st_meta_read_bench logs in workspace",
        fontsize=10,
    )
    fig.tight_layout()
    fig.savefig(out_base.with_suffix(".png"), dpi=160)
    fig.savefig(out_base.with_suffix(".svg"))
    plt.close(fig)


def write_markdown(report: dict, path: Path) -> None:
    lines = [
        "# dense20 三系统对比（10 轮）",
        "",
        "## 结果验证",
        "",
        "| 系统 | finalSize 校验 |",
        "|------|----------------|",
        f"| ST-RocksDB (E7) | {report['validation']['st_e7']} |",
        f"| ST-RocksDB (ST Key / LocS) | {report['validation']['st_tman_key']} |",
        f"| TMan-RocksDB | {report['validation']['tman_rocksdb']} |",
        "",
        "协议：warmup + run_01–10；查询模式 **sst_manifest**（Global Manifest + Local 块剪枝 + 点真值）。",
        "",
        "## 查询延迟 (ms)",
        "",
    ]
    for suite in SUITES:
        lines.append(f"### {SUITE_LABELS[suite]}")
        lines.append("")
        lines.append("| 系统 | avg | P90 | rounds |")
        lines.append("|------|-----|-----|--------|")
        for sid in [c["id"] for c in SYSTEMS]:
            lat = report["systems"][sid]["suites"][suite]["latency"]
            lines.append(
                f"| {report['systems'][sid]['label']} | {lat['avg_ms']:.1f} | {lat['p90_ms']:.1f} | {lat['n_rounds']} |"
            )
        lines.append("")

    lines.append("## 块剪枝（Block index skipped）")
    lines.append("")
    lines.append("统计自 `st_meta_read_bench` 日志 `block_index_examined` / `block_index_skipped`（所有 run_* × 20 查询均值）。")
    lines.append("")
    for suite in SUITES:
        lines.append(f"### {SUITE_LABELS[suite]}")
        lines.append("")
        lines.append("| 系统 | examined/query | skipped/query | skip % | files_considered | files_skipped |")
        lines.append("|------|----------------|---------------|--------|------------------|---------------|")
        for sid in ["st_e7", "st_tman_key"]:
            pr = report["systems"][sid]["suites"][suite].get("prune", {})
            if not pr:
                continue
            lines.append(
                f"| {report['systems'][sid]['label']} | "
                f"{pr.get('block_index_examined_mean', 0):.0f} | "
                f"{pr.get('block_index_skipped_mean', 0):.0f} | "
                f"{pr.get('block_index_skip_pct_mean', 0):.1f}% | "
                f"{pr.get('file_considered_mean', 0):.0f} | "
                f"{pr.get('files_skipped_mean', 0):.0f} |"
            )
        lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--out-dir",
        type=Path,
        default=PROJECT / "Result/picture/three_systems_dense20",
    )
    args = ap.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)

    report = build_report()
    json_path = args.out_dir / "three_systems_dense20_summary.json"
    json_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    write_markdown(report, args.out_dir / "three_systems_dense20_report.md")

    plot_latency(report, args.out_dir / "three_systems_dense20_latency")
    plot_prune(report, args.out_dir / "three_systems_dense20_block_prune")

    print(f"Wrote {json_path}")
    print(f"Wrote {args.out_dir / 'three_systems_dense20_latency.png'}")
    print(f"Wrote {args.out_dir / 'three_systems_dense20_block_prune.png'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

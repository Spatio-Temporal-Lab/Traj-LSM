#!/usr/bin/env python3
"""Summarize TDrive DB: M_manager index + per-level live SST via traj_db_audit."""
from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
DB = PROJECT / "data" / "runs" / "TDrive"
AUDIT = PROJECT / "rocks-demo" / "build" / "traj_db_audit.exe"
OUT = PROJECT / "Result" / "TSV" / "tdrive_db_level_summary.txt"


def main() -> int:
    current = (DB / "CURRENT").read_text(encoding="utf-8").strip()
    mgr_path = DB / current
    mgr_lines = mgr_path.read_text(encoding="utf-8").splitlines()
    entries = [ln for ln in mgr_lines if ln.startswith("entry=")]
    gen = next((ln for ln in mgr_lines if ln.startswith("generation=")), "?")

    lines: list[str] = []
    lines.append(f"db={DB}")
    lines.append(f"CURRENT -> {current}")
    lines.append(f"M_manager_file=1 ({mgr_path.name})")
    lines.append(f"m_manager_generation={gen}")
    lines.append(f"manifest_shard_entries={len(entries)}")
    lines.append("")

    if not AUDIT.is_file():
        lines.append(f"ERROR: missing {AUDIT}")
        OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print("\n".join(lines))
        return 1

    env = os.environ.copy()
    env["ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_ROUTE"] = "1"
    env["ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_BUCKET_WIDTH_SEC"] = "3600"
    env["ROCKSDB_EXPERIMENTAL_MANIFEST_SHARD_MAX"] = "8192"
    lock = DB / "LOCK"
    if lock.is_file():
        lock.unlink()

    proc = subprocess.run(
        [str(AUDIT), "--db", str(DB)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=env,
    )
    raw = proc.stdout + proc.stderr
    if proc.returncode != 0:
        lines.append(f"traj_db_audit failed exit={proc.returncode}")
        lines.append(raw[-2000:])
        OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print("\n".join(lines))
        return proc.returncode

    levels: dict[int, list[tuple[str, int, int]]] = {}
    for ln in raw.splitlines():
        m = re.match(r"\s+L(\d+) (\S+) size=(\d+) num_entries=(\d+)", ln)
        if m:
            lv = int(m.group(1))
            levels.setdefault(lv, []).append(
                (m.group(2), int(m.group(3)), int(m.group(4)))
            )

    m_count = re.search(r"live_sst count=(\d+)", raw)
    m_sum = re.search(r"sum_num_entries=(\d+) sum_file_size=(\d+)", raw)
    lines.append(f"live_sst_open={m_count.group(1) if m_count else '?'}")
    if m_sum:
        lines.append(
            f"sum_num_entries={m_sum.group(1)} sum_file_size={m_sum.group(2)}"
        )
    lines.append("")

    for lv in sorted(levels):
        files = sorted(levels[lv], key=lambda x: x[0])
        tb = sum(x[1] for x in files)
        te = sum(x[2] for x in files)
        lines.append(
            f"=== L{lv}: files={len(files)} bytes={tb} ({tb/1024/1024:.2f} MiB) "
            f"entries={te} avg_bytes={tb // len(files) if files else 0} ==="
        )
        for fn, sz, ent in files:
            lines.append(f"  {fn}  size={sz}  entries={ent}")
        lines.append("")

    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())

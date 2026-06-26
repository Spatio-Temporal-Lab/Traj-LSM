#!/usr/bin/env python3
import re
import statistics as st
import sys
from pathlib import Path

PAT = re.compile(
    r"spatial_batch_query id=\d+ time_ms=([\d.]+).*"
    r"block_read_count=(\d+) bytes_read=(\d+).*"
    r"block_index_examined=(\d+) block_index_skipped=(\d+).*"
    r"file_considered=(\d+) files_skipped=(\d+)"
)


def parse(path: Path) -> list[dict]:
    rows = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = PAT.search(line)
        if not m:
            continue
        time_ms, br, nbytes, bie, bis, fc, fs = map(float, m.groups())
        rows.append(
            {
                "time_ms": time_ms,
                "block_read": int(br),
                "bytes_read": int(nbytes),
                "block_index_examined": int(bie),
                "block_index_skipped": int(bis),
                "files_considered": int(fc),
                "files_skipped": int(fs),
                "files_opened": int(fc) - int(fs),
            }
        )
    return rows


def report(rows: list[dict], title: str) -> None:
    if not rows:
        print(f"{title}: (no rows)")
        return
    def avg(k):
        return st.mean(r[k] for r in rows)

    bie = sum(r["block_index_examined"] for r in rows)
    bis = sum(r["block_index_skipped"] for r in rows)
    fc = sum(r["files_considered"] for r in rows)
    fs = sum(r["files_skipped"] for r in rows)
    print(f"\n=== {title} (n={len(rows)}) ===")
    print(f"  time_ms avg={avg('time_ms'):.2f}")
    print(f"  block_read avg={avg('block_read'):.0f}")
    print(f"  bytes_read avg={avg('bytes_read')/1e6:.2f} MB")
    print(f"  files_considered={rows[0]['files_considered']}  files_opened avg={avg('files_opened'):.1f}")
    print(f"  file_skip_rate={100*fs/max(1,fc):.1f}%")
    print(f"  block_index_skip_rate={100*bis/max(1,bie):.1f}%")


if __name__ == "__main__":
    for p in sys.argv[1:]:
        report(parse(Path(p)), Path(p).parent.name + "/" + Path(p).name)

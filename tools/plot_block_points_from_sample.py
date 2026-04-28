import argparse
import json
import re
from pathlib import Path

import matplotlib.pyplot as plt


BLOCK_RE = re.compile(r"^Data Block #\s*(\d+)\b")
HEX_ROW_RE = re.compile(r"^\s*HEX\s+[0-9A-F]+:\s+([0-9A-F]+)\s*$")


def parse_block_hex_values(sample_txt: Path, block_id: int) -> list[str]:
    values: list[str] = []
    in_block = False
    with sample_txt.open("r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.rstrip("\n")
            m = BLOCK_RE.match(line)
            if m:
                cur = int(m.group(1))
                if in_block and cur != block_id:
                    break
                in_block = cur == block_id
                continue
            if not in_block:
                continue
            h = HEX_ROW_RE.match(line)
            if h:
                values.append(h.group(1))
    return values


def decode_points(value_hex: str) -> tuple[str, list[tuple[float, float]]]:
    data = bytes.fromhex(value_hex)
    obj = json.loads(data.decode("utf-8"))
    seg_id = str(obj.get("segment_id", "unknown"))
    points_raw = obj.get("points", [])
    points: list[tuple[float, float]] = []
    for p in points_raw:
        if not isinstance(p, list) or len(p) < 3:
            continue
        lon = float(p[1])
        lat = float(p[2])
        points.append((lon, lat))
    return seg_id, points


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Plot all trajectory points contained in one sampled Data Block."
    )
    ap.add_argument("--sample", required=True, help="Path to *_sampled_blocks.txt")
    ap.add_argument("--block-id", type=int, required=True, help="Data Block id to plot")
    ap.add_argument("--out", required=True, help="Output PNG path")
    args = ap.parse_args()

    sample_txt = Path(args.sample)
    out_png = Path(args.out)
    out_png.parent.mkdir(parents=True, exist_ok=True)

    value_hex_rows = parse_block_hex_values(sample_txt, args.block_id)
    if not value_hex_rows:
        raise SystemExit(f"No HEX rows found for Data Block #{args.block_id}")

    total_points = 0
    total_segments = 0
    min_lon = float("inf")
    max_lon = float("-inf")
    min_lat = float("inf")
    max_lat = float("-inf")

    plt.figure(figsize=(9, 8), dpi=140)
    for vh in value_hex_rows:
        seg_id, pts = decode_points(vh)
        if not pts:
            continue
        total_segments += 1
        total_points += len(pts)
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        min_lon = min(min_lon, min(xs))
        max_lon = max(max_lon, max(xs))
        min_lat = min(min_lat, min(ys))
        max_lat = max(max_lat, max(ys))
        plt.plot(xs, ys, linewidth=0.7, alpha=0.8)
        plt.scatter(xs, ys, s=6, alpha=0.85)

    if total_segments == 0:
        raise SystemExit(f"Block #{args.block_id} had rows but no valid points.")

    plt.xlabel("Longitude")
    plt.ylabel("Latitude")
    plt.title(
        f"Data Block #{args.block_id}: {total_segments} segments, {total_points} points"
    )
    plt.grid(True, linestyle="--", alpha=0.25)
    if min_lon < max_lon and min_lat < max_lat:
        dx = max(1e-6, (max_lon - min_lon) * 0.05)
        dy = max(1e-6, (max_lat - min_lat) * 0.05)
        plt.xlim(min_lon - dx, max_lon + dx)
        plt.ylim(min_lat - dy, max_lat + dy)
    plt.tight_layout()
    plt.savefig(out_png)
    plt.close()

    print(f"OK block={args.block_id} segments={total_segments} points={total_points}")
    print(f"PNG={out_png.as_posix()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


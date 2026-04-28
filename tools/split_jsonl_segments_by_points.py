import argparse
import json
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
CN_TZ = timezone(timedelta(hours=8))

from pathlib import Path
from typing import Any


@dataclass
class SplitConfig:
    min_points: int
    max_points: int
    target_points: int


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Split long trajectory segments in JSONL into chunks by point count."
    )
    p.add_argument("--input", required=True, help="Input JSONL path")
    p.add_argument("--output", required=True, help="Output JSONL path")
    p.add_argument("--min-points", type=int, default=200)
    p.add_argument("--max-points", type=int, default=300)
    p.add_argument("--target-points", type=int, default=250)
    return p.parse_args()


def to_unix_seconds(ts: Any) -> int:
    if isinstance(ts, (int, float)):
        return int(ts)
    if not isinstance(ts, str):
        raise ValueError(f"Unsupported timestamp type: {type(ts)}")
    # Expected format in current dataset: "YYYY-mm-dd HH:MM:SS"
    dt = datetime.strptime(ts, "%Y-%m-%d %H:%M:%S")
    # Source generated_trajs timestamps are local China time.
    return int(dt.replace(tzinfo=CN_TZ).timestamp())


def choose_chunk_count(n: int, cfg: SplitConfig) -> int:
    if n <= cfg.max_points:
        return 1
    k_min = (n + cfg.max_points - 1) // cfg.max_points  # ceil(n/max)
    k_max = n // cfg.min_points  # floor(n/min)
    if k_max < 1:
        return 1
    if k_min > k_max:
        return k_min
    ideal = max(1, round(n / cfg.target_points))
    return min(max(ideal, k_min), k_max)


def split_sizes(n: int, k: int) -> list[int]:
    base = n // k
    rem = n % k
    return [base + (1 if i < rem else 0) for i in range(k)]


def calc_mbr(points: list[list[Any]]) -> dict[str, float]:
    lons = [float(p[1]) for p in points]
    lats = [float(p[2]) for p in points]
    return {
        "min_lon": min(lons),
        "min_lat": min(lats),
        "max_lon": max(lons),
        "max_lat": max(lats),
    }


def build_chunk_record(src: dict[str, Any], points: list[list[Any]], chunk_idx: int) -> dict[str, Any]:
    t_vals = [to_unix_seconds(p[0]) for p in points]
    first = points[0]
    seg_id = str(src["segment_id"])
    out = dict(src)
    out["segment_id"] = f"{seg_id}#p{chunk_idx:03d}"
    out["points"] = points
    out["point_count"] = len(points)
    out["t_min"] = min(t_vals)
    out["t_max"] = max(t_vals)
    out["start_time"] = out["t_min"]
    out["start_lon"] = float(first[1])
    out["start_lat"] = float(first[2])
    out["mbr"] = calc_mbr(points)
    return out


def main() -> int:
    args = parse_args()
    cfg = SplitConfig(
        min_points=args.min_points,
        max_points=args.max_points,
        target_points=args.target_points,
    )
    if not (1 <= cfg.min_points <= cfg.target_points <= cfg.max_points):
        raise ValueError("Require: 1 <= min-points <= target-points <= max-points")

    src_path = Path(args.input)
    dst_path = Path(args.output)
    dst_path.parent.mkdir(parents=True, exist_ok=True)

    src_segments = 0
    out_segments = 0
    total_points = 0
    out_min = None
    out_max = 0

    with src_path.open("r", encoding="utf-8") as fin, dst_path.open(
        "w", encoding="utf-8", newline="\n"
    ) as fout:
        for line in fin:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            pts = rec.get("points", [])
            if not isinstance(pts, list) or len(pts) == 0:
                continue
            n = len(pts)
            src_segments += 1
            total_points += n

            k = choose_chunk_count(n, cfg)
            if k <= 1:
                out = build_chunk_record(rec, pts, 1)
                cnt = out["point_count"]
                out_min = cnt if out_min is None else min(out_min, cnt)
                out_max = max(out_max, cnt)
                fout.write(json.dumps(out, ensure_ascii=False) + "\n")
                out_segments += 1
                continue

            sizes = split_sizes(n, k)
            off = 0
            for i, sz in enumerate(sizes, start=1):
                chunk = pts[off : off + sz]
                off += sz
                out = build_chunk_record(rec, chunk, i)
                cnt = out["point_count"]
                out_min = cnt if out_min is None else min(out_min, cnt)
                out_max = max(out_max, cnt)
                fout.write(json.dumps(out, ensure_ascii=False) + "\n")
                out_segments += 1

    avg_before = (total_points / src_segments) if src_segments else 0.0
    avg_after = (total_points / out_segments) if out_segments else 0.0
    print(f"input_segments={src_segments}")
    print(f"output_segments={out_segments}")
    print(f"total_points={total_points}")
    print(f"avg_points_before={avg_before:.3f}")
    print(f"avg_points_after={avg_after:.3f}")
    print(f"output_min_points={out_min if out_min is not None else 0}")
    print(f"output_max_points={out_max}")
    print(f"output_path={dst_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


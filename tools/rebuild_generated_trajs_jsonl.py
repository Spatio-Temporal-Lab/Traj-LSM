import argparse
import json
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Iterable


CN_TZ = timezone(timedelta(hours=8))


@dataclass
class SegmentRecord:
    segment_id: str
    points: list[list]
    point_count: int
    t_min: int
    t_max: int
    start_time: int
    start_lon: float
    start_lat: float
    mbr: dict[str, float]
    zorder_start: int


def parse_time_cn_to_unix(ts: str) -> int:
    dt = datetime.strptime(ts, "%Y-%m-%d %H:%M:%S").replace(tzinfo=CN_TZ)
    return int(dt.timestamp())


def quantize_lon_lat_to_uint32(lon: float, lat: float) -> tuple[int, int]:
    # Map lon in [-180,180], lat in [-90,90] to uint32.
    x = (lon + 180.0) / 360.0
    y = (lat + 90.0) / 180.0
    x = min(1.0, max(0.0, x))
    y = min(1.0, max(0.0, y))
    qx = int(round(x * 4294967295.0))
    qy = int(round(y * 4294967295.0))
    return qx, qy


def morton64_from_uint32_xy(x: int, y: int) -> int:
    def part1by1(n: int) -> int:
        n &= 0xFFFFFFFF
        n = (n | (n << 16)) & 0x0000FFFF0000FFFF
        n = (n | (n << 8)) & 0x00FF00FF00FF00FF
        n = (n | (n << 4)) & 0x0F0F0F0F0F0F0F0F
        n = (n | (n << 2)) & 0x3333333333333333
        n = (n | (n << 1)) & 0x5555555555555555
        return n

    return (part1by1(y) << 1) | part1by1(x)


def parse_generated_trajs_line(line: str) -> tuple[str, list[list]]:
    # Expected shape per line:
    # [86627503122318_1,[["2020-07-18 09:07:17",119.6776716,31.27908],...]]
    line = line.strip()
    if not line or not line.startswith("[") or not line.endswith("]"):
        raise ValueError("bad line wrapper")
    marker = ",[["
    p = line.find(marker)
    if p <= 1:
        raise ValueError("cannot locate id/points split")
    raw_id = line[1:p]
    points_json = line[p + 1 : -1]  # keep [[...]] only
    pts = json.loads(points_json)
    if not isinstance(pts, list):
        raise ValueError("points is not list")
    return raw_id, pts


def build_record(src_date: str, src_seg_id: str, points: list[list]) -> SegmentRecord:
    if not points:
        raise ValueError("empty points")
    t_vals = [parse_time_cn_to_unix(str(p[0])) for p in points]
    lons = [float(p[1]) for p in points]
    lats = [float(p[2]) for p in points]
    start_lon = lons[0]
    start_lat = lats[0]
    qx, qy = quantize_lon_lat_to_uint32(start_lon, start_lat)
    z = morton64_from_uint32_xy(qx, qy)
    return SegmentRecord(
        # Keep source id semantics, but disambiguate cross-day repeats.
        segment_id=f"{src_date}:{src_seg_id}",
        points=points,
        point_count=len(points),
        t_min=min(t_vals),
        t_max=max(t_vals),
        start_time=t_vals[0],
        start_lon=start_lon,
        start_lat=start_lat,
        mbr={
            "min_lon": min(lons),
            "min_lat": min(lats),
            "max_lon": max(lons),
            "max_lat": max(lats),
        },
        zorder_start=z,
    )


def iter_input_files(in_dir: Path) -> Iterable[Path]:
    files = sorted(in_dir.glob("*.txt"))
    for p in files:
        yield p


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Rebuild generated_trajs_segments.jsonl from data/generated_trajs/*.txt"
    )
    ap.add_argument("--in-dir", default="data/generated_trajs")
    ap.add_argument("--out", default="data/generated_trajs_segments.jsonl")
    args = ap.parse_args()

    in_dir = Path(args.in_dir)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    n_in = 0
    n_out = 0
    n_bad = 0

    with out.open("w", encoding="utf-8", newline="\n") as fout:
        for fp in iter_input_files(in_dir):
            src_date = fp.stem
            print(f"processing_file={fp.name}")
            with fp.open("r", encoding="utf-8", errors="replace") as fin:
                for line in fin:
                    s = line.strip()
                    if not s:
                        continue
                    n_in += 1
                    try:
                        seg_id, pts = parse_generated_trajs_line(s)
                        rec = build_record(src_date, seg_id, pts)
                    except Exception:
                        n_bad += 1
                        continue
                    obj = {
                        "segment_id": rec.segment_id,
                        "point_count": rec.point_count,
                        "t_min": rec.t_min,
                        "t_max": rec.t_max,
                        "start_time": rec.start_time,
                        "start_lon": rec.start_lon,
                        "start_lat": rec.start_lat,
                        "mbr": rec.mbr,
                        "zorder_start": rec.zorder_start,
                        "points": rec.points,
                    }
                    fout.write(json.dumps(obj, ensure_ascii=False) + "\n")
                    n_out += 1
                    if (n_out % 100000) == 0:
                        print(f"progress_output_segments={n_out}")

    print(f"input_lines={n_in}")
    print(f"output_segments={n_out}")
    print(f"bad_lines={n_bad}")
    print(f"output_path={out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


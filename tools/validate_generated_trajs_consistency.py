import argparse
import json
import math
import re
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Any, Dict, Optional, Tuple


CN_TZ = timezone(timedelta(hours=8))


_VID_RE = re.compile(r"^\d{8}:(\d+)_")


def parse_vehicle_id(segment_id: str) -> Optional[int]:
    m = _VID_RE.match(segment_id)
    if not m:
        return None
    try:
        return int(m.group(1))
    except ValueError:
        return None


def to_unix_seconds_cn(ts: Any, cache: Dict[str, int]) -> int:
    if isinstance(ts, (int, float)):
        return int(ts)
    if not isinstance(ts, str):
        raise ValueError(f"Unsupported timestamp type: {type(ts)}")
    v = cache.get(ts)
    if v is not None:
        return v
    dt = datetime.strptime(ts, "%Y-%m-%d %H:%M:%S").replace(tzinfo=CN_TZ)
    u = int(dt.timestamp())
    cache[ts] = u
    return u


def haversine_m(lon1: float, lat1: float, lon2: float, lat2: float) -> float:
    # Earth radius
    r = 6371000.0
    phi1 = math.radians(lat1)
    phi2 = math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dl = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2.0) ** 2 + math.cos(phi1) * math.cos(phi2) * math.sin(dl / 2.0) ** 2
    c = 2.0 * math.atan2(math.sqrt(a), math.sqrt(1.0 - a))
    return r * c


@dataclass
class SeenPoint:
    lon: float
    lat: float
    segment_id: str
    point_idx: int


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Validate generated_trajs derived JSONL for impossible same-vehicle same-time positions and extreme speeds."
    )
    ap.add_argument("--input", default="data/generated_trajs_segments.jsonl")
    ap.add_argument("--same-sec-max-dist-m", type=float, default=1000.0)
    ap.add_argument("--max-speed-kmh", type=float, default=200.0)
    ap.add_argument("--max-report", type=int, default=50)
    args = ap.parse_args()

    inp = args.input
    same_sec_max_dist_m = float(args.same_sec_max_dist_m)
    max_speed_mps = float(args.max_speed_kmh) * 1000.0 / 3600.0

    # Global check: (vehicle_id, unix_sec) -> first seen point
    seen_same_sec: Dict[Tuple[int, int], SeenPoint] = {}

    # Per vehicle last point for speed check (based on encounter order; within a segment it's monotonic)
    last_point_by_vehicle: Dict[int, Tuple[int, float, float, str, int]] = {}

    ts_cache: Dict[str, int] = {}
    segments = 0
    points = 0
    bad_lines = 0
    bad_points = 0
    hit_same_sec = 0
    hit_speed = 0

    def report_same_sec(
        vid: int,
        t: int,
        a: SeenPoint,
        b_lon: float,
        b_lat: float,
        b_seg: str,
        b_idx: int,
        dist_m: float,
    ):
        nonlocal hit_same_sec
        hit_same_sec += 1
        print("ANOMALY_SAME_SECOND")
        print(f"  vehicle_id={vid} unix_s={t} dist_m={dist_m:.1f} threshold_m={same_sec_max_dist_m}")
        print(f"  A: lon={a.lon:.7f} lat={a.lat:.7f} seg={a.segment_id} idx={a.point_idx}")
        print(f"  B: lon={b_lon:.7f} lat={b_lat:.7f} seg={b_seg} idx={b_idx}")

    def report_speed(
        vid: int,
        a_t: int,
        a_lon: float,
        a_lat: float,
        a_seg: str,
        a_idx: int,
        b_t: int,
        b_lon: float,
        b_lat: float,
        b_seg: str,
        b_idx: int,
        dist_m: float,
        speed_mps: float,
    ):
        nonlocal hit_speed
        hit_speed += 1
        print("ANOMALY_SPEED")
        print(
            f"  vehicle_id={vid} dt_s={b_t - a_t} dist_m={dist_m:.1f} speed_kmh={speed_mps*3.6:.1f} threshold_kmh={args.max_speed_kmh}"
        )
        print(f"  A: t={a_t} lon={a_lon:.7f} lat={a_lat:.7f} seg={a_seg} idx={a_idx}")
        print(f"  B: t={b_t} lon={b_lon:.7f} lat={b_lat:.7f} seg={b_seg} idx={b_idx}")

    max_report = int(args.max_report)
    reports = 0

    with open(inp, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except Exception:
                bad_lines += 1
                continue

            seg_id = str(rec.get("segment_id", ""))
            vid = parse_vehicle_id(seg_id)
            if vid is None:
                bad_lines += 1
                continue

            pts = rec.get("points", [])
            if not isinstance(pts, list) or not pts:
                bad_points += 1
                continue

            segments += 1
            prev_t: Optional[int] = None
            prev_lon = 0.0
            prev_lat = 0.0
            for i, p in enumerate(pts):
                points += 1
                if not isinstance(p, list) or len(p) < 3:
                    bad_points += 1
                    continue
                try:
                    t = to_unix_seconds_cn(p[0], ts_cache)
                    lon = float(p[1])
                    lat = float(p[2])
                except Exception:
                    bad_points += 1
                    continue

                # Same-second global check
                key = (vid, t)
                sp = seen_same_sec.get(key)
                if sp is None:
                    seen_same_sec[key] = SeenPoint(lon=lon, lat=lat, segment_id=seg_id, point_idx=i)
                else:
                    d = haversine_m(sp.lon, sp.lat, lon, lat)
                    if d > same_sec_max_dist_m:
                        report_same_sec(vid, t, sp, lon, lat, seg_id, i, d)
                        reports += 1
                        if reports >= max_report:
                            print("STOP: reached --max-report")
                            break

                # Within-segment monotonic check / speed check
                if prev_t is not None:
                    dt_s = t - prev_t
                    if dt_s > 0:
                        d = haversine_m(prev_lon, prev_lat, lon, lat)
                        spd = d / float(dt_s)
                        if spd > max_speed_mps:
                            report_speed(
                                vid,
                                prev_t,
                                prev_lon,
                                prev_lat,
                                seg_id,
                                i - 1,
                                t,
                                lon,
                                lat,
                                seg_id,
                                i,
                                d,
                                spd,
                            )
                            reports += 1
                            if reports >= max_report:
                                print("STOP: reached --max-report")
                                break
                prev_t = t
                prev_lon = lon
                prev_lat = lat

                # Cross-segment speed check based on encounter order (best-effort).
                last = last_point_by_vehicle.get(vid)
                if last is not None:
                    last_t, last_lon, last_lat, last_seg, last_idx = last
                    dt_s = t - last_t
                    if dt_s > 0:
                        d = haversine_m(last_lon, last_lat, lon, lat)
                        spd = d / float(dt_s)
                        if spd > max_speed_mps:
                            report_speed(
                                vid,
                                last_t,
                                last_lon,
                                last_lat,
                                last_seg,
                                last_idx,
                                t,
                                lon,
                                lat,
                                seg_id,
                                i,
                                d,
                                spd,
                            )
                            reports += 1
                            if reports >= max_report:
                                print("STOP: reached --max-report")
                                break
                last_point_by_vehicle[vid] = (t, lon, lat, seg_id, i)

            if reports >= max_report:
                break

    print("SUMMARY")
    print(f"  input={inp}")
    print(f"  segments={segments} points={points}")
    print(f"  bad_lines={bad_lines} bad_points={bad_points}")
    print(f"  anomalies_same_second={hit_same_sec}")
    print(f"  anomalies_speed={hit_speed}")
    print(f"  same_sec_max_dist_m={same_sec_max_dist_m} max_speed_kmh={args.max_speed_kmh}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


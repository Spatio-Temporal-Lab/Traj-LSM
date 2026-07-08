"""Convert unified CSV to TMan single-line format: oid-tid-WKT"""
import csv, sys
from collections import defaultdict

def convert(meta_path, points_path, output_path):
    print("Loading points...", flush=True)
    points_by_seg = defaultdict(list)
    with open(points_path) as f:
        for i, row in enumerate(csv.DictReader(f)):
            sid = row["segment_id"]
            points_by_seg[sid].append(
                (float(row["lon"]), float(row["lat"]), int(row["unix_time_s"])))
            if i % 10000000 == 0:
                print(f"  points: {i:,}", flush=True)

    print("Converting...", flush=True)
    n = 0
    with open(meta_path) as mf, open(output_path, "w") as out:
        for i, row in enumerate(csv.DictReader(mf)):
            sid = row["segment_id"]
            pts = points_by_seg.get(sid, [])
            if not pts:
                continue
            pts.sort(key=lambda p: p[2])
            coords = ",".join(f"({x:.7f} {y:.7f} {t*1000})" for x, y, t in pts)
            out.write(f'{i}-{sid}-MULTIPOINT Z({coords})\n')
            n += 1
            if n % 10000 == 0:
                print(f"  converted: {n:,}", flush=True)
    print(f"Done: {n:,} segments -> {output_path}", flush=True)

if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("--meta", required=True)
    p.add_argument("--points", required=True)
    p.add_argument("--output", required=True)
    a = p.parse_args()
    convert(a.meta, a.points, a.output)

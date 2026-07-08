"""
Convert wuxi segments to TMan single-line format.
Exact format match with tdrive_trajs.txt:
  oid-tid-MULTIPOINT Z((lon lat time_ms), (lon lat time_ms), ...)
"""
import csv
from collections import defaultdict

def convert(meta_path, points_path, output_path):
    print("Loading wuxi points...")
    pts_by_seg = defaultdict(list)
    with open(points_path) as f:
        for i, row in enumerate(csv.DictReader(f)):
            pts_by_seg[row["segment_id"]].append(
                (float(row["lon"]), float(row["lat"]), int(row["unix_time_s"]) * 1000))
            if i % 5000000 == 0:
                print(f"  points: {i:,}")

    print("Converting wuxi segments...")
    n = 0
    with open(meta_path) as mf, open(output_path, "w") as out:
        for i, row in enumerate(csv.DictReader(mf)):
            pts = pts_by_seg.get(row["segment_id"], [])
            if not pts:
                continue
            pts.sort(key=lambda p: p[2])
            # TMan format: oid-tid-MULTIPOINT Z((x y z), ...)
            # tid: replace colons with underscores
            tid = row["segment_id"].replace(":", "_")
            coords = ",".join(f"{x:.7f} {y:.7f} {t}" for x, y, t in pts)
            out.write(f'{i}-{tid}-MULTIPOINT Z(({coords}))\n')
            n += 1
            if n % 10000 == 0:
                print(f"  converted: {n:,}")
    print(f"Done: {n:,} wuxi segments -> {output_path}")

if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("--meta", required=True)
    p.add_argument("--points", required=True)
    p.add_argument("--output", required=True)
    a = p.parse_args()
    convert(a.meta, a.points, a.output)

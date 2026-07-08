"""Filter unified CSV to tdrive + wuxi only"""
import csv

meta_in = r"D:\Project\data\processed\unified\segments_meta.csv"
points_in = r"D:\Project\data\processed\unified\segments_points.csv"
meta_out = r"D:\Project\data\td_wx\segments_meta.csv"
points_out = r"D:\Project\data\td_wx\segments_points.csv"

# Read meta, filter to tdrive+wuxi
print("Filtering meta...")
seg_ids = set()
with open(meta_in) as f, open(meta_out, "w", newline="") as out:
    reader = csv.DictReader(f)
    writer = csv.DictWriter(out, fieldnames=reader.fieldnames)
    writer.writeheader()
    for row in reader:
        if row["dataset"] in ("tdrive", "wuxi"):
            writer.writerow(row)
            seg_ids.add(row["segment_id"])
print(f"  {len(seg_ids)} segments (tdrive+wuxi)")

# Filter points
print("Filtering points...")
n = 0
with open(points_in) as f, open(points_out, "w", newline="") as out:
    reader = csv.DictReader(f)
    writer = csv.DictWriter(out, fieldnames=reader.fieldnames)
    writer.writeheader()
    for row in reader:
        if row["segment_id"] in seg_ids:
            writer.writerow(row)
            n += 1
            if n % 10000000 == 0:
                print(f"  {n:,} points")
print(f"Done: {n:,} points")

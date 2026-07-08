import csv

# Generate TMan query conditions from stratified12 windows
# TMan format: xMin,yMin,xMax,yMax,tMin(ms),tMax(ms)
# Use all 9 windows covering tdrive + wuxi time ranges

with open(r"D:\Project\data\processed\unified\unified_stratified12_st_windows.csv") as f:
    rows = list(csv.DictReader(f))

with open(r"D:\Project\data\server_upload\conditions_tdrive_wuxi.txt", "w") as out:
    for r in rows:
        tmin_ms = int(r["TMin"]) * 1000
        tmax_ms = int(r["TMax"]) * 1000
        out.write(f'{r["XMin"]},{r["YMin"]},{r["XMax"]},{r["YMax"]},{tmin_ms},{tmax_ms}\n')

print(f"Generated {len(rows)} query conditions")
for r in rows:
    print(f"  {r['Label']}: {r['STSegments']} segs, TMin={r['TMin']}")

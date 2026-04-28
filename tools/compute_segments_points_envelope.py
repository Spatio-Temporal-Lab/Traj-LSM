#!/usr/bin/env python3
"""
Compute (t,x,y) envelope from segments_points.csv (optionally capped).
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--csv", default="data/processed/generated/segments_points.csv")
    ap.add_argument("--max-rows", type=int, default=200000)
    args = ap.parse_args()

    p = Path(args.csv)
    n_cap = args.max_rows

    tmin = None
    tmax = None
    xmin = float("inf")
    xmax = float("-inf")
    ymin = float("inf")
    ymax = float("-inf")
    n = 0

    with p.open("r", encoding="utf-8", newline="") as f:
        r = csv.DictReader(f)
        for row in r:
            t = int(row["unix_time_s"])
            x = float(row["lon"])
            y = float(row["lat"])
            tmin = t if tmin is None else min(tmin, t)
            tmax = t if tmax is None else max(tmax, t)
            xmin = min(xmin, x)
            xmax = max(xmax, x)
            ymin = min(ymin, y)
            ymax = max(ymax, y)
            n += 1
            if n_cap >= 0 and n >= n_cap:
                break

    assert tmin is not None and tmax is not None
    print(f"rows={n}")
    print(f"t_min={tmin} t_max={tmax} t_span={tmax - tmin}")
    print(f"x_min={xmin:.7f} x_max={xmax:.7f} x_span={xmax - xmin:.7f}")
    print(f"y_min={ymin:.7f} y_max={ymax:.7f} y_span={ymax - ymin:.7f}")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())


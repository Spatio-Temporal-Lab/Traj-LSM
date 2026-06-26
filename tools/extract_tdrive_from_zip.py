#!/usr/bin/env python3
"""Extract datasets/tdrive and datasets/tdrive-query from datasets.zip (no unzip CLI)."""
from __future__ import annotations

import argparse
import os
import zipfile
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--zip", type=Path, required=True)
    ap.add_argument("--out-root", type=Path, required=True)
    args = ap.parse_args()

    prefixes = ("datasets/tdrive/", "datasets/tdrive-query/")
    args.out_root.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.zip) as z:
        n = 0
        for name in z.namelist():
            if any(name.startswith(p) for p in prefixes):
                z.extract(name, args.out_root)
                n += 1
    print(f"extracted {n} zip entries -> {args.out_root}")
    for p in prefixes:
        d = args.out_root / p.rstrip("/")
        print(f"  {d}: {'OK' if d.is_dir() else 'MISSING'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Precompute TMan-tsp ST RowKey fields for rocks-demo st_bucket_ingest_build (tman_st schema).

Uses TMan-tsp EmitSegmentStKeys (LocSIndex + TimeRangeIndex, same as HBase/TMan ingest).

Output: data/processed/tdrive/segments_st_key.csv
  segment_id,period,spatial_index,tid,key_len

Usage:
  python tools/emit_tdrive_tman_st_keys.py
  python tools/emit_tdrive_tman_st_keys.py --mvn D:\\apache-maven-3.9.16\\bin\\mvn.cmd
  python tools/emit_tdrive_tman_st_keys.py --skip-mvn   # classpath already built
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
TMAN = PROJECT / "TMan-tsp"
DEFAULT_META = PROJECT / "data" / "processed" / "tdrive" / "segments_meta.csv"
DEFAULT_POINTS = PROJECT / "data" / "processed" / "tdrive" / "segments_points.csv"
DEFAULT_OUT = PROJECT / "data" / "processed" / "tdrive" / "segments_st_key.csv"
ENVELOPE = PROJECT / "data" / "processed" / "tman_tdrive" / "tdrive_envelope.json"
EMIT_MAIN = "huajunge.github.io.spatialman.tools.EmitSegmentStKeys"


def _is_windows() -> bool:
    return sys.platform == "win32"


def _mvn_script_name() -> str:
    return "mvn.cmd" if _is_windows() else "mvn"


def find_mvn(explicit: str | None = None) -> Path | None:
    """Resolve mvn executable on Windows/Linux."""
    if explicit:
        p = Path(explicit)
        if p.is_file():
            return p
        print(f"ERROR: --mvn not found: {p}", file=sys.stderr)
        return None

    which = shutil.which("mvn")
    if which:
        return Path(which)

    home_keys = ("MAVEN_HOME", "M2_HOME", "MVN_HOME")
    for key in home_keys:
        home = os.environ.get(key)
        if not home:
            continue
        candidate = Path(home) / "bin" / _mvn_script_name()
        if candidate.is_file():
            return candidate

    # Common Windows install locations (PATH often missing \bin).
    if _is_windows():
        for candidate in (
            Path(r"D:\apache-maven-3.9.16\bin\mvn.cmd"),
            Path(r"D:\apache-maven\bin\mvn.cmd"),
            Path(r"C:\apache-maven\bin\mvn.cmd"),
            Path(r"C:\Program Files\Apache\maven\bin\mvn.cmd"),
            Path.home() / "apps" / "apache-maven-3.9.6" / "bin" / "mvn.cmd",
            Path.home() / "apps" / "apache-maven" / "bin" / "mvn.cmd",
        ):
            if candidate.is_file():
                return candidate

    return None


def find_java() -> str:
    java = shutil.which("java")
    if java:
        return java
    if _is_windows():
        for candidate in (
            Path(r"C:\Program Files\Common Files\Oracle\Java\javapath\java.exe"),
            Path(r"C:\Program Files\Java\jdk-17\bin\java.exe"),
            Path(r"C:\Program Files\Java\jdk-11\bin\java.exe"),
        ):
            if candidate.is_file():
                return str(candidate)
    print("ERROR: java not found in PATH", file=sys.stderr)
    sys.exit(1)


def emit_classes_dir() -> Path:
    return PROJECT / "rocks-demo" / "build" / "emit_classes"


def compile_emit_main(jar: Path) -> bool:
    """javac EmitSegmentStKeys against existing fat jar (when mvn scala fails)."""
    src = TMAN / "src" / "main" / "java" / "huajunge" / "github" / "io" / "spatialman" / "tools" / "EmitSegmentStKeys.java"
    if not src.is_file() or not jar.is_file():
        return False
    out = emit_classes_dir()
    out.mkdir(parents=True, exist_ok=True)
    javac = shutil.which("javac")
    if not javac and _is_windows():
        java = Path(find_java())
        javac_cand = java.parent / "javac.exe"
        if javac_cand.is_file():
            javac = str(javac_cand)
    if not javac:
        return False
    print(f">>> {javac} -cp {jar} EmitSegmentStKeys.java")
    rc = subprocess.run(
        [javac, "-encoding", "UTF-8", "-cp", str(jar), "-d", str(out), str(src)],
        cwd=str(PROJECT),
        check=False,
    ).returncode
    return rc == 0


def classpath_for_emit() -> str | None:
    """Return -cp string if EmitSegmentStKeys is already compiled."""
    fat = sorted((TMAN / "target").glob("*-jar-with-dependencies.jar"))
    emit_cls = (
        emit_classes_dir()
        / "huajunge"
        / "github"
        / "io"
        / "spatialman"
        / "tools"
        / "EmitSegmentStKeys.class"
    )
    if emit_cls.is_file() and fat:
        sep = ";" if _is_windows() else ":"
        return f"{emit_classes_dir()}{sep}{fat[-1]}"

    if fat:
        if compile_emit_main(fat[-1]) and emit_cls.is_file():
            sep = ";" if _is_windows() else ":"
            return f"{emit_classes_dir()}{sep}{fat[-1]}"
        if emit_cls.is_file():
            sep = ";" if _is_windows() else ":"
            return f"{emit_classes_dir()}{sep}{fat[-1]}"

    classes = TMAN / "target" / "classes"
    emit_class = classes / "huajunge" / "github" / "io" / "spatialman" / "tools" / "EmitSegmentStKeys.class"
    if not emit_class.is_file():
        return None

    lib = TMAN / "target" / "dependency"
    sep = ";" if _is_windows() else ":"
    parts: list[str] = [str(classes)]
    if lib.is_dir():
        parts.extend(str(j) for j in sorted(lib.glob("*.jar")))
    return sep.join(parts)


def run_mvn_package(mvn: Path) -> int:
    print(f">>> {mvn} -q -DskipTests package (TMan-tsp)")
    return subprocess.run(
        [str(mvn), "-q", "-DskipTests", "package"],
        cwd=str(TMAN),
        check=False,
    ).returncode


def print_mvn_help() -> None:
    print(
        "\nMaven (mvn) not found. Fix one of:\n"
        "  1) Add Maven bin to PATH, e.g. D:\\apache-maven-3.9.16\\bin\n"
        "  2) set MAVEN_HOME=D:\\apache-maven-3.9.16\n"
        "  3) python tools\\emit_tdrive_tman_st_keys.py --mvn D:\\apache-maven-3.9.16\\bin\\mvn.cmd\n"
        "  4) After manual 'mvn package' in TMan-tsp: --skip-mvn\n",
        file=sys.stderr,
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--meta", type=Path, default=DEFAULT_META)
    ap.add_argument("--points", type=Path, default=DEFAULT_POINTS)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--mvn", type=str, default="", help="Path to mvn or mvn.cmd")
    ap.add_argument("--skip-mvn", action="store_true", help="Skip mvn; use existing target/")
    args = ap.parse_args()

    if not args.meta.is_file() or not args.points.is_file():
        print(f"ERROR: missing meta/points under {args.meta.parent}", file=sys.stderr)
        return 1

    xmin, ymin, xmax, ymax = 115.47173, 39.04912, 117.82745, 41.21767
    time_bin, time_bin_nums = 3600.0, 149
    if ENVELOPE.is_file():
        import json

        e = json.loads(ENVELOPE.read_text(encoding="utf-8"))
        b = e["spatial_boundary"]
        h = e["tman_table_config_hint"]
        xmin, ymin, xmax, ymax = b["xmin"], b["ymin"], b["xmax"], b["ymax"]
        time_bin = float(h["timeBin"])
        time_bin_nums = int(h["timeBinNums"])

    cp = classpath_for_emit()
    if cp is None and not args.skip_mvn:
        mvn = find_mvn(args.mvn or None)
        if mvn is None:
            print_mvn_help()
            return 1
        rc = run_mvn_package(mvn)
        if rc != 0:
            return rc
        cp = classpath_for_emit()

    if cp is None:
        print(
            "ERROR: EmitSegmentStKeys not compiled. Run mvn package in TMan-tsp first.",
            file=sys.stderr,
        )
        print_mvn_help()
        return 1

    java = find_java()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        java,
        "-cp",
        cp,
        EMIT_MAIN,
        "--meta",
        str(args.meta),
        "--points",
        str(args.points),
        "--out",
        str(args.out),
        "--xmin",
        str(xmin),
        "--ymin",
        str(ymin),
        "--xmax",
        str(xmax),
        "--ymax",
        str(ymax),
        "--time-bin",
        str(time_bin),
        "--time-bin-nums",
        str(time_bin_nums),
    ]
    print(">>>", " ".join(cmd))
    r = subprocess.run(cmd, cwd=str(PROJECT), check=False)
    if r.returncode == 0:
        print(f"OK: {args.out}")
    return r.returncode


if __name__ == "__main__":
    raise SystemExit(main())

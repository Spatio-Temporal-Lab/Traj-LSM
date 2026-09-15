import argparse
import datetime
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def git(*arguments):
    return subprocess.check_output(["git", "-C", str(ROOT), *arguments])


def main():
    parser = argparse.ArgumentParser(description="Copy current ST-LSM sources and input data into an upload directory")
    parser.add_argument("--output", type=Path, default=ROOT / "server_upload")
    parser.add_argument("--refresh-code", action="store_true")
    parser.add_argument("--dataset", choices=["processed/base", "td_wx", "shared", "processed/expanded"], default="processed/base")
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists() and not args.refresh_code:
        parser.error(f"Refusing to overwrite existing upload directory: {output}")
    if output == ROOT or ROOT.is_relative_to(output) or output.is_relative_to(ROOT / "data"):
        parser.error("Upload directory must not overlap the input data or contain the repository")
    tracked = git("ls-files", "-z", "--", "rocksdb", "rocks-demo").decode("utf-8").split("\0")
    sources = [(ROOT / relative, Path("STLSM") / relative) for relative in tracked if relative]
    sources.extend((source, Path("STLSM") / source.relative_to(ROOT))
                   for source in sorted((ROOT / "scripts" / "server").rglob("*"))
                   if source.is_file() and "__pycache__" not in source.parts)
    sources.extend((source, Path("STLSM") / source.relative_to(ROOT))
                   for source in sorted((ROOT / "rocksdb" / "cmake").rglob("*"))
                   if source.is_file())
    notes = ROOT / "rocks-demo" / "ST_COMPACTION_CONFIG.md"
    if notes.is_file() and not any(source == notes for source, _ in sources):
        sources.append((notes, Path("STLSM/rocks-demo/ST_COMPACTION_CONFIG.md")))
    sources.append((ROOT / "scripts/server/README.md", Path("STLSM/README.md")))
    excluded = []
    previous = None
    if args.refresh_code:
        previous = json.loads((output / "STLSM/PACKAGE_MANIFEST.json").read_text(encoding="utf-8"))
    dataset = ROOT / "data" / args.dataset
    if not dataset.is_dir():
        parser.error(f"Missing dataset: {dataset}")
    for source in ([] if args.refresh_code else sorted(dataset.rglob("*"))):
        if not source.is_file():
            continue
        relative = source.relative_to(ROOT / "data")
        if relative.parts[0] in {"db", "sst"} or source.name.endswith((".spill.idx", ".log")) or source.name == "td_debug.kvs":
            excluded.append({"path": "data/" + relative.as_posix(), "bytes": source.stat().st_size})
        else:
            sources.append((source, Path("data") / relative))
    total = sum(source.stat().st_size for source, _ in sources)
    if shutil.disk_usage(output.parent).free < total + 1024 ** 3:
        parser.error("Insufficient free space for a separate upload copy")
    output.mkdir(parents=True, exist_ok=args.refresh_code)
    manifest = {"created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "base_commit": git("rev-parse", "HEAD").decode().strip(),
                "source": "current working tree, including uncommitted changes",
                "shell_line_endings": "LF", "dataset": args.dataset,
                "excluded_data": excluded, "files": []}
    if previous is not None:
        manifest["files"] = [entry for entry in previous["files"] if entry["path"].startswith("data/")]
        manifest["excluded_data"] = previous["excluded_data"]
        manifest["dataset"] = previous["dataset"]
    print(f"Copying {len(sources)} files, {total / 1024 ** 3:.2f} GiB -> {output}", flush=True)
    for source, relative in sources:
        before = source.stat()
        if source.is_symlink():
            raise RuntimeError(f"Unexpected symbolic link: {source}")
        destination = output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        digest = hashlib.sha256()
        written = 0
        next_report = 4 * 1024 ** 3
        with source.open("rb") as incoming, destination.open("wb" if args.refresh_code else "xb") as outgoing:
            if source.suffix == ".sh":
                block = incoming.read().replace(b"\r\n", b"\n")
                outgoing.write(block)
                digest.update(block)
                written = len(block)
            else:
                while block := incoming.read(8 * 1024 * 1024):
                    outgoing.write(block)
                    digest.update(block)
                    written += len(block)
                    if written >= next_report:
                        print(f"  {relative}: {written / 1024 ** 3:.1f} GiB", flush=True)
                        next_report += 4 * 1024 ** 3
        after = source.stat()
        if before.st_size != after.st_size or before.st_mtime_ns != after.st_mtime_ns:
            raise RuntimeError(f"Source changed while copying: {source}")
        manifest["files"].append({"path": relative.as_posix(), "bytes": written, "sha256": digest.hexdigest()})
        if written >= 1024 ** 3:
            print(f"  Finished {relative}", flush=True)
    patch = git("diff", "--binary", "HEAD", "--", "rocksdb", "rocks-demo")
    (output / "STLSM/LOCAL_CHANGES.patch").write_bytes(patch)
    manifest["files"].append({"path": "STLSM/LOCAL_CHANGES.patch", "bytes": len(patch), "sha256": hashlib.sha256(patch).hexdigest()})
    (output / "STLSM/PACKAGE_MANIFEST.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Complete: {len(manifest['files'])} files; manifest saved.", flush=True)


if __name__ == "__main__":
    main()

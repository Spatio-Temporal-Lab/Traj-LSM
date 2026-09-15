import argparse
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description="Verify uploaded files against the packaging manifest")
    parser.add_argument("--code-only", action="store_true")
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[2]
    root = project.parent.resolve()
    manifest = json.loads((project / "PACKAGE_MANIFEST.json").read_text(encoding="utf-8"))
    failures = []
    checked = 0
    for entry in manifest["files"]:
        if args.code_only and not entry["path"].startswith("STLSM/"):
            continue
        path = (root / entry["path"]).resolve()
        if not path.is_relative_to(root):
            raise ValueError(f"Path outside upload directory: {entry['path']}")
        if not path.is_file() or path.stat().st_size != entry["bytes"]:
            failures.append(entry["path"])
            continue
        digest = hashlib.sha256()
        with path.open("rb") as source:
            while block := source.read(8 * 1024 * 1024):
                digest.update(block)
        if digest.hexdigest() != entry["sha256"]:
            failures.append(entry["path"])
        checked += 1
        if entry["bytes"] >= 1024 ** 3:
            print(f"Checked {entry['path']}", flush=True)
    if failures:
        raise SystemExit("Verification FAILED:\n" + "\n".join(failures))
    print(f"Verified {checked} files (SHA-256).")


if __name__ == "__main__":
    main()

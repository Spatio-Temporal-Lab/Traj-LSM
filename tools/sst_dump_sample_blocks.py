import argparse
import json
import math
import os
import re
from typing import List, Tuple


_BLOCK_RE = re.compile(r"^Data Block #\s*(\d+)\b", re.MULTILINE)


def _find_block_spans(text: str) -> List[Tuple[int, int, int]]:
    """
    Returns list of (block_index, start_offset, end_offset) for each 'Data Block #N' section.
    end_offset is the start of next block or EOF.
    """
    hits = list(_BLOCK_RE.finditer(text))
    out: List[Tuple[int, int, int]] = []
    for i, m in enumerate(hits):
        idx = int(m.group(1))
        start = m.start()
        end = hits[i + 1].start() if (i + 1) < len(hits) else len(text)
        out.append((idx, start, end))
    return out


def _pick_evenly(block_ids: List[int], n: int) -> List[int]:
    if n <= 0 or not block_ids:
        return []
    if len(block_ids) <= n:
        return block_ids[:]
    # Evenly spaced indices over [0, len-1]
    picks = []
    for i in range(n):
        pos = int(round(i * (len(block_ids) - 1) / float(n - 1)))
        picks.append(block_ids[pos])
    # De-dup while preserving order
    seen = set()
    uniq = []
    for x in picks:
        if x not in seen:
            seen.add(x)
            uniq.append(x)
    return uniq


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Sample N Data Block sections from `sst_dump --command=raw` output."
    )
    ap.add_argument("--in", dest="inp", required=True, help="Input *_dump.txt from sst_dump --command=raw")
    ap.add_argument("--out", dest="out", required=True, help="Output text file to write sampled blocks")
    ap.add_argument("--count", type=int, default=10, help="How many blocks to sample (default: 10)")
    ap.add_argument(
        "--strategy",
        choices=["even"],
        default="even",
        help="Sampling strategy (default: even)",
    )
    ap.add_argument(
        "--meta-out",
        default="",
        help="Optional JSON file to write block index metadata",
    )
    args = ap.parse_args()

    with open(args.inp, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()

    spans = _find_block_spans(text)
    if not spans:
        raise SystemExit("No 'Data Block #' sections found. Did you run sst_dump with --command=raw?")

    block_ids = [b for (b, _, _) in spans]
    chosen = _pick_evenly(block_ids, args.count)
    chosen_set = set(chosen)

    # Build map id -> span
    span_by_id = {b: (s, e) for (b, s, e) in spans}
    chosen_spans = [(b, *span_by_id[b]) for b in chosen if b in span_by_id]

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as out:
        out.write(f"# source: {args.inp}\n")
        out.write(f"# total_blocks_found: {len(block_ids)}\n")
        out.write(f"# sampled_blocks: {json.dumps(chosen)}\n\n")
        for b, s, e in chosen_spans:
            out.write(text[s:e].rstrip())
            out.write("\n\n")

    if args.meta_out:
        meta = {
            "input": args.inp,
            "total_blocks_found": len(block_ids),
            "sampled_blocks": chosen,
        }
        os.makedirs(os.path.dirname(os.path.abspath(args.meta_out)), exist_ok=True)
        with open(args.meta_out, "w", encoding="utf-8") as mf:
            json.dump(meta, mf, indent=2)
            mf.write("\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())


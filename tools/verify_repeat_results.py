#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
DEFAULT_ROOT = PROJECT / 'Result' / 'TSV' / 'tdrive_st_query_repeat'
MANIFEST = PROJECT / 'data' / 'processed' / 'tman_tdrive' / 'tdrive_query_st' / 'unified_12' / 'queries_manifest_st.json'


def load_truth() -> list[dict]:
    data = json.loads(MANIFEST.read_text(encoding='utf-8'))
    return [r for r in data if isinstance(r, dict)]


def load_per_query(run_dir: Path) -> list[dict]:
    p = run_dir / 'st_prune_unified_12_per_query.json'
    if not p.is_file():
        raise FileNotFoundError(p)
    data = json.loads(p.read_text(encoding='utf-8'))
    if not isinstance(data, list):
        raise ValueError(f'invalid per_query payload: {p}')
    return data


def verify_run(run_dir: Path, truth: list[dict]) -> dict:
    actual = load_per_query(run_dir)
    truth_by_id = {int(r['id']): r for r in truth}
    actual_by_id = {int(r.get('query_id', r.get('id', -1))): r for r in actual}

    exact = 0
    fn = 0
    fp = 0
    details = []
    for qid, t in truth_by_id.items():
        a = actual_by_id.get(qid)
        t_hit = int(t.get('ref_points_in_mbr', 0))
        a_hit = int(a.get('finalSize', -1)) if a else -1
        if a is None:
            fn += t_hit
            details.append({'query_id': qid, 'status': 'FAIL', 'reason': 'missing actual result', 'truth': t_hit, 'actual': None})
            continue
        if a_hit == t_hit:
            exact += 1
            details.append({'query_id': qid, 'status': 'PASS', 'truth': t_hit, 'actual': a_hit})
        else:
            if a_hit < t_hit:
                fn += (t_hit - a_hit)
            else:
                fp += (a_hit - t_hit)
            details.append({'query_id': qid, 'status': 'FAIL', 'truth': t_hit, 'actual': a_hit})

    passed = fn == 0 and fp == 0 and exact == len(truth_by_id)
    out = {
        'run_dir': str(run_dir),
        'pass': passed,
        'false_negative_count': fn,
        'false_positive_count': fp,
        'exact_match_count': exact,
        'query_count': len(truth_by_id),
        'details': details,
    }
    (run_dir / 'verify_summary.json').write_text(json.dumps(out, indent=2) + '\n', encoding='utf-8')
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', type=Path, default=DEFAULT_ROOT)
    ap.add_argument('--round-dir', type=Path, default=None)
    args = ap.parse_args()

    truth = load_truth()
    if args.round_dir:
        result = verify_run(args.round_dir, truth)
        print(json.dumps(result, indent=2))
        return 0 if result['pass'] else 1

    runs = sorted([p for p in args.root.iterdir() if p.is_dir() and '_round_' in p.name])
    summaries = [verify_run(r, truth) for r in runs]
    overall = {
        'pass': all(s['pass'] for s in summaries),
        'false_negative_count': sum(s['false_negative_count'] for s in summaries),
        'false_positive_count': sum(s['false_positive_count'] for s in summaries),
        'exact_match_count': sum(s['exact_match_count'] for s in summaries),
        'run_count': len(summaries),
        'details': summaries,
    }
    (args.root / 'verify_summary.json').write_text(json.dumps(overall, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(overall, indent=2))
    return 0 if overall['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

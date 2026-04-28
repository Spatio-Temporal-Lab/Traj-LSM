import json
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: python tools/count_unique_source_keys.py <jsonl>")
        return 2
    p = sys.argv[1]
    total = 0
    uniq_sid = set()
    uniq_e6 = set()
    uniq_e7 = set()
    with open(p, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            o = json.loads(line)
            total += 1
            sid = str(o["segment_id"])
            uniq_sid.add(sid)
            t_min = int(o["t_min"])
            t_max = int(o["t_max"])
            z = int(o["zorder_start"])
            m = o["mbr"]
            uniq_e6.add(
                (
                    t_min,
                    t_max,
                    float(m["min_lon"]),
                    float(m["min_lat"]),
                    float(m["max_lon"]),
                    float(m["max_lat"]),
                    sid,
                )
            )
            uniq_e7.add((t_min, t_max, z, sid))
    print(f"total_lines={total}")
    print(f"unique_segment_id={len(uniq_sid)}")
    print(f"unique_e6_key_tuple={len(uniq_e6)}")
    print(f"unique_e7_key_tuple={len(uniq_e7)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


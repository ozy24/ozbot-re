#!/usr/bin/env python3
"""
Stranded-time readout: how much live bot time is spent with NO goal for longer
than --min seconds, and where.  A bot with no goal for many seconds is usually
standing in a nav sink -- a region with no outgoing link to the item graph
(e.g. a spawn point on a learned island), which no A* cost can fix.

    python3 tools/stranded.py <merged.jsonl> [...] [--min 5]
"""
import argparse
import collections
import json


def stranded(path, min_s):
    run = {}
    total = live = 0.0
    spots = collections.Counter()
    last_t = {}
    for line in open(path):
        r = json.loads(line)
        if r.get("type") != "tick":
            continue
        b, t = r["bot"], r["t"]
        dt = t - last_t.get(b, t)
        last_t[b] = t
        if r["dead"] or r["goal_node"] >= 0:
            seg = run.pop(b, None)
            if seg and seg[1] - seg[0] >= min_s:
                total += seg[1] - seg[0]
                spots[seg[2]] += seg[1] - seg[0]
            if not r["dead"]:
                live += dt
            continue
        live += dt
        k = (round(r["x"], -2), round(r["y"], -2), round(r["z"], -2))
        if b in run:
            run[b][1] = t
        else:
            run[b] = [t, t, k]
    for seg in run.values():
        if seg[1] - seg[0] >= min_s:
            total += seg[1] - seg[0]
            spots[seg[2]] += seg[1] - seg[0]
    return total, live, spots


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--min", type=float, default=5.0)
    a = ap.parse_args()
    for p in a.logs:
        tot, live, spots = stranded(p, a.min)
        print(f"{p.rsplit('/', 1)[-1]}: stranded {tot:.0f}s of {live:.0f}s live "
              f"({tot / max(live, 1):.1%})  top: "
              + ", ".join(f"{k}:{v:.0f}s" for k, v in spots.most_common(4)))

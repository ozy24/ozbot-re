#!/usr/bin/env python3
"""
Per-goal outcome breakdown from a run's telemetry: for every goal_item, how
it ended (pickup of the goal item / giveup / superseded by another goal /
death) and how long it took.  Compares A/B arms where pickups move but the
giveup taxonomy doesn't say why (e.g. a travel controller changing which
goals get picked and how fast they complete).

    python3 tools/goal_outcomes.py <merged.jsonl> [<merged.jsonl> ...]
"""
import collections
import json
import sys


def outcomes(path):
    open_goal = {}
    res = collections.Counter()
    dur = collections.defaultdict(list)
    per_item = collections.defaultdict(collections.Counter)
    side = 0
    for line in open(path):
        r = json.loads(line)
        if r.get("type") != "event":
            continue
        if "bot" not in r:
            continue
        b, ev, t = r["bot"], r["event"], r["t"]
        g = open_goal.get(b)

        def close(kind):
            res[kind] += 1
            dur[kind].append(t - g[1])
            per_item[g[0]][kind] += 1
            open_goal.pop(b, None)

        if ev == "pickup":
            if g and r.get("item") == g[0]:
                close("done")
            else:
                side += 1
        elif ev == "giveup" and g:
            close("giveup")
        elif ev == "death" and g:
            close("death")
        elif ev == "goal_item":
            if g:
                close("superseded")
            open_goal[b] = (r["item"], t)
    return res, dur, per_item, side


def med(xs):
    xs = sorted(xs)
    return xs[len(xs) // 2] if xs else 0


if __name__ == "__main__":
    for p in sys.argv[1:]:
        res, dur, per_item, side = outcomes(p)
        n = sum(res.values())
        print(f"== {p.rsplit('/', 1)[-1]}: {n} goals, side pickups {side}")
        for k in ("done", "giveup", "superseded", "death"):
            print(f"   {k:10s} {res[k]:5d} ({res[k] / max(n, 1):5.1%})  median {med(dur[k]):5.2f}s")
        top = sorted(per_item.items(), key=lambda kv: -sum(kv[1].values()))[:8]
        for item, c in top:
            print(f"     {item:18s} " + " ".join(f"{k}={c[k]}" for k in ("done", "giveup", "superseded", "death")))

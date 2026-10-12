#!/usr/bin/env python3
"""
Why do item goals get abandoned?  Giveup taxonomy over run telemetry.

Every giveup is a goal-budget expiry (bot_main.c); the question is what ate
the budget.  Needs a DLL whose giveup events carry the time accounting fields
(budget/elapsed/frozen/fight/noprog, added 2026-10-12).  Each giveup gets ONE
dominant cause, first match wins:

  at_node    reached the goal node, item not collected (last-leg failure)
  pb_abort   a playbook attempt aborted inside this goal attempt (ALIGN
             timeout / drift) -- the traversal ate the budget
  frozen     a traversal controller (lift/playbook replay) held the clock for
             most of the attempt
  fight      an enemy was present for >40% of the budget
  stalled    no 8u closer to the goal for most of the budget AND under half
             the route walked -- stuck or orbiting
  slow       still progressing when the budget ran out: budget too tight for
             the route as executed

Usage:
    py tools/mine_giveups.py <log.jsonl> [more logs...] [--top 8]
"""

import collections
import json
import statistics as st
import sys

CLASSES = ["at_node", "pb_abort", "frozen", "fight", "stalled", "slow"]


def classify(g, pb_aborts):
    if g["atnode"]:
        return "at_node"
    t0 = g["t"] - g["elapsed"]
    if any(t0 <= t <= g["t"] for t in pb_aborts.get(g["bot"], ())):
        return "pb_abort"
    if g["frozen"] > 0.5 * g["elapsed"]:
        return "frozen"
    if g["fight"] > 0.4 * g["budget"]:
        return "fight"
    if g["noprog"] > 0.5 * g["budget"] and g["pidx"] < 0.5 * max(1, g["plen"]):
        return "stalled"
    return "slow"


def load(path):
    gives, pb_aborts, last_item, mapname = [], collections.defaultdict(list), {}, "?"
    with open(path) as f:
        for line in f:
            if '"type": "run"' in line and mapname == "?":
                mapname = json.loads(line).get("map", "?")
            if '"type": "event"' not in line:
                continue
            r = json.loads(line)
            ev = r.get("event", "")
            if ev == "goal_item":
                last_item[r["bot"]] = r.get("item", "?")
            elif ev.startswith("pb_abort"):
                pb_aborts[r["bot"]].append(r["t"])
            elif ev == "giveup" and "budget" in r:
                r["item"] = last_item.get(r["bot"], "?")
                gives.append(r)
    return mapname, gives, pb_aborts


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    top = int(sys.argv[sys.argv.index("--top") + 1]) if "--top" in sys.argv else 6
    if "--top" in sys.argv:
        args.remove(str(top))
    if not args:
        sys.exit(__doc__)
    for path in args:
        mapname, gives, pb_aborts = load(path)
        if not gives:
            print(f"{path}: no giveups with time accounting (older DLL?)")
            continue
        by = collections.defaultdict(list)
        for g in gives:
            by[classify(g, pb_aborts)].append(g)
        print(f"\n== {mapname}: {len(gives)} giveups  ({path.split('/')[-1]})")
        print(f"  {'class':9s} {'n':>4s} {'share':>6s}  {'elapsed':>7s} {'frozen':>6s} {'fight':>5s} "
              f"{'noprog':>6s} {'gdist':>5s}  route  top items")
        for c in CLASSES:
            gs = by.get(c, [])
            if not gs:
                continue
            med = lambda k: st.median(x[k] for x in gs)
            items = collections.Counter(x["item"] for x in gs).most_common(3)
            print(f"  {c:9s} {len(gs):4d} {100 * len(gs) / len(gives):5.1f}%  {med('elapsed'):7.1f} "
                  f"{med('frozen'):6.1f} {med('fight'):5.1f} {med('noprog'):6.1f} {med('gdist'):5.0f}  "
                  f"{med('pidx'):.0f}/{med('plen'):.0f}  " + ", ".join(f"{i}x{n}" for i, n in items))
        cells = collections.Counter((round(g["x"] / 128) * 128, round(g["y"] / 128) * 128,
                                     round(g["z"] / 64) * 64) for g in gives)
        print("  hot spots (128u cells): " + "  ".join(f"{c}x{n}" for c, n in cells.most_common(top)))


if __name__ == "__main__":
    main()

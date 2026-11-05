#!/usr/bin/env python3
"""Add typed route links found outside the game (a simulator's walk/jump tests) to an ONAV graph.

A learned graph holds only the links its bots happened to walk, so it can send a
bot far round where a short walk, drop or jump would do (mm-ptrip: a hole in the
armor room's floor drops onto the large health; the graph went 2,800 units round).
The links come from a JSON file, one per map:

    {"links": [[from, to, type, cost, graph_cost, test], ...]}

against this graph's node ids, `type` a NAV_LINK_* (0 WALK, 1 FALL, 2 JUMP,
8 PUSH), graph_cost the old route's cost (-1: none). Each was checked by running
it: a plain walk at the node (FALL where it drops more than 40, PUSH where a jump
pad threw the player), or a jump the way Bot_Navigate takes a JUMP link (run at the
node, jump within 96 of it), from the node and from four starts off the line.

Links are added biggest shortcut first, priced as the game prices that type
(nav_edit.link_cost, the same as Nav_LinkCost), skipping pairs already linked,
and never filling a node past --cap links: Nav_AddLink silently refuses a 13th,
and the load-time seeding (item nodes, plats, teleporters, jump pads, playbooks)
needs room on the nodes it wires to.  The header flags (FROZEN) are kept.

Usage:
  python3 tools/nav_links_import.py <in.nav> <links.json> --out <out.nav> [--cap 9] [--min-gain U] [--max-add N] [--dry-run]
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nav_edit as ne  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("nav")
    ap.add_argument("links")
    ap.add_argument("--out")
    ap.add_argument("--cap", type=int, default=9, help="most links a node may hold after the import")
    ap.add_argument("--max-add", type=int, default=0, help="most links added (0: no limit)")
    ap.add_argument("--min-gain", type=float, default=0.0,
                    help="only links that shorten the old route by this much (no old route: always)")
    ap.add_argument("--types", default="0,1,2,8", help="link types to import")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    nodes, flags = ne.load(a.nav)
    want = {int(t) for t in a.types.split(",")}
    rows = json.load(open(a.links))["links"]
    have = {(i, l["to"]) for i, n in enumerate(nodes) for l in n["links"]}

    def gain(r):  # the old route's cost minus the link's; no old route first
        return 1e9 if r[4] < 0 else r[4] - r[3]

    added = collections.Counter()
    skipped = collections.Counter()
    for r in sorted(rows, key=gain, reverse=True):
        i, j, typ = int(r[0]), int(r[1]), int(r[2])
        if typ not in want:
            skipped["type"] += 1
        elif gain(r) < a.min_gain:
            skipped["small"] += 1
        elif not (0 <= i < len(nodes) and 0 <= j < len(nodes)) or i == j:
            skipped["bad node"] += 1
        elif (i, j) in have:
            skipped["linked"] += 1
        elif len(nodes[i]["links"]) >= min(a.cap, ne.NAV_MAX_LINKS):
            skipped["full"] += 1
        elif a.max_add and sum(added.values()) >= a.max_add:
            skipped["max"] += 1
        else:
            cost = ne.link_cost(nodes[i]["origin"], nodes[j]["origin"], typ)
            nodes[i]["links"].append({"to": j, "type": typ, "cost": cost})
            have.add((i, j))
            added[ne.LINK_NAMES.get(typ, str(typ))] += 1
    total = sum(len(n["links"]) for n in nodes)
    print(json.dumps({"nav": os.path.basename(a.nav), "added": sum(added.values()), "by_type": dict(added),
                      "skipped": dict(skipped), "links_after": total}))
    if not a.dry_run:
        ne.save(a.out or a.nav, nodes, flags)


if __name__ == "__main__":
    main()

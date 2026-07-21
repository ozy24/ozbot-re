#!/usr/bin/env python3
"""Integrate a capture-log trajectory into an ONAV nav graph as a node chain.

A recorded human/bot path is a sequence of positions the player physically
traversed -- so consecutive waypoints are, by construction, a walkable edge.
This decimates a `capture` log (per-frame usercmd + position, slot 0 by
default) to ~spacing-apart waypoints, snaps each to an existing nearby node
(same density/z rules as Nav_AddNode / nav_edit add-node: reuse within 96u &
|dz|<48) or creates a new one, and links consecutive waypoints with a
geometry-derived type:

  * WATER (4)  either endpoint's frame is in water  (+ NAV_FLAG_WATER on the
               submerged node -- water-ness is a NODE property, bot_nav.h)
  * JUMP  (2)  climb up   (dz > +Zthr) -- ladders/step-ups; bot_ladder is
               opportunistic and climbs whenever the next node is >32u up
  * FALL  (1)  drop down  (dz < -Zthr)
  * WALK  (0)  otherwise

WATER/WALK links are bidirectional (symmetric traversal). JUMP/FALL are added
only in the recorded direction of travel (the reverse of a real drop/climb
isn't guaranteed executable), with a complementary reverse link for ladders
(--ladder), which are climbable both ways.

Usage:
  py tools/traj_to_nav.py <nav> <log.jsonl> [--slot N] [--spacing U]
      [--zthr U] [--ladder] [--stitch U] [--apply] [--out FILE] [--verbose]

Dry-run by default: prints the chain + link types + a before/after node count.
--apply writes back (or --out to a new file).
"""
from __future__ import annotations
import argparse, json, math, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nav_edit as ne  # load/save/link_cost/nearest, identical on-disk format

NAV_FLAG_WATER = 1


def load_log(path, slot):
    recs, last_t = [], None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                o = json.loads(line)
            except ValueError:
                continue
            if o.get("type") != "input" or o.get("slot") != slot:
                continue
            if o["t"] == last_t:
                continue  # collapse duplicate usercmds logged for one frame
            last_t = o["t"]
            recs.append(o)
    recs.sort(key=lambda o: o["t"])
    return recs


def decimate(recs, spacing):
    """Emit waypoints >= spacing apart (3D), always keep first & last."""
    if not recs:
        return []
    out = [recs[0]]
    for r in recs[1:]:
        p = out[-1]
        if math.dist((r["x"], r["y"], r["z"]), (p["x"], p["y"], p["z"])) >= spacing:
            out.append(r)
    if out[-1] is not recs[-1]:
        out.append(recs[-1])
    return out


def find_or_add(nodes, pos, water):
    """Reuse a same-level node within 96u, else append. Returns (idx, created)."""
    for i, nd in enumerate(nodes):
        dx = nd["origin"][0] - pos[0]
        dy = nd["origin"][1] - pos[1]
        dz = nd["origin"][2] - pos[2]
        if abs(dz) > 48:
            continue
        if dx * dx + dy * dy + dz * dz < 96 * 96:
            if water and not (nd["flags"] & NAV_FLAG_WATER):
                nd["flags"] |= NAV_FLAG_WATER  # stamp water-ness onto the reused node
            return i, False
    nodes.append({"origin": tuple(pos),
                  "flags": NAV_FLAG_WATER if water else 0, "links": []})
    return len(nodes) - 1, True


def add_link(nodes, a, b, typ):
    if a == b:
        return False
    n = nodes[a]
    for l in n["links"]:
        if l["to"] == b:
            return False  # keep the pre-existing link/type
    if len(n["links"]) >= ne.NAV_MAX_LINKS:
        return False
    cost = ne.link_cost(n["origin"], nodes[b]["origin"], typ)
    n["links"].append({"to": b, "type": typ, "cost": cost})
    return True


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("nav")
    ap.add_argument("log")
    ap.add_argument("--slot", type=int, default=0)
    ap.add_argument("--spacing", type=float, default=80.0)
    ap.add_argument("--zthr", type=float, default=48.0,
                    help="dz beyond this = JUMP(up)/FALL(down) instead of WALK")
    ap.add_argument("--maxlink", type=float, default=220.0,
                    help="skip a link longer than this (decimation gap guard)")
    ap.add_argument("--ladder", action="store_true",
                    help="treat vertical hops as two-way climbable (reverse link too)")
    ap.add_argument("--bridge-only", action="store_true",
                    help="only add a link if it touches a node newly created this run "
                         "(attach new geometry; never lay a parallel shortcut between "
                         "two pre-existing nodes -- those steal from good overland routes)")
    ap.add_argument("--keep-leaves", action="store_true",
                    help="don't prune created dead-end (0-out-link) nodes (default prunes them)")
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--out")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args(argv[1:])

    nodes, hdr_flags = ne.load(a.nav)
    n0 = len(nodes)
    recs = load_log(a.log, a.slot)
    if not recs:
        sys.exit(f"FAIL: no slot-{a.slot} input records in {a.log}")
    wps = decimate(recs, a.spacing)
    print(f"{os.path.basename(a.log)}: {len(recs)} frames -> {len(wps)} waypoints "
          f"(spacing {a.spacing:.0f})")

    # resolve every waypoint to a node index first
    chain = []
    created = 0
    new_ids = set()
    for w in wps:
        pos = (w["x"], w["y"], w["z"])
        water = bool(w.get("water", 0))
        idx, isnew = find_or_add(nodes, pos, water)
        if isnew:
            created += 1
            new_ids.add(idx)
        chain.append((idx, water, w))

    added = 0
    counts = {"WALK": 0, "FALL": 0, "JUMP": 0, "WATER": 0}
    for (a_i, a_w, aw), (b_i, b_w, bw) in zip(chain, chain[1:]):
        if a_i == b_i:
            continue
        d = math.dist(nodes[a_i]["origin"], nodes[b_i]["origin"])
        if d > a.maxlink:
            if a.verbose:
                print(f"  SKIP {a_i}->{b_i} d={d:.0f} > maxlink")
            continue
        if a.bridge_only and a_i not in new_ids and b_i not in new_ids:
            if a.verbose:
                print(f"  SKIP {a_i}->{b_i} (both pre-existing; bridge-only)")
            continue
        dz = nodes[b_i]["origin"][2] - nodes[a_i]["origin"][2]
        if a_w or b_w:
            fwd, rev = ne.NAME_TO_TYPE["WATER"], ne.NAME_TO_TYPE["WATER"]
        elif dz > a.zthr:
            fwd, rev = ne.NAME_TO_TYPE["JUMP"], ne.NAME_TO_TYPE["FALL"]
        elif dz < -a.zthr:
            fwd, rev = ne.NAME_TO_TYPE["FALL"], ne.NAME_TO_TYPE["JUMP"]
        else:
            fwd, rev = ne.NAME_TO_TYPE["WALK"], ne.NAME_TO_TYPE["WALK"]
        if add_link(nodes, a_i, b_i, fwd):
            counts[ne.LINK_NAMES[fwd]] += 1; added += 1
        # reverse link: always for symmetric WALK/WATER; for vertical only w/ --ladder
        vertical = fwd in (ne.NAME_TO_TYPE["JUMP"], ne.NAME_TO_TYPE["FALL"])
        if (not vertical) or a.ladder:
            if add_link(nodes, b_i, a_i, rev):
                counts[ne.LINK_NAMES[rev]] += 1; added += 1
        if a.verbose:
            o1 = tuple(round(c) for c in nodes[a_i]["origin"])
            o2 = tuple(round(c) for c in nodes[b_i]["origin"])
            print(f"  {a_i}{o1} -> {b_i}{o2} {ne.LINK_NAMES[fwd]} d={d:.0f}")

    # prune trailing dead-ends: a recorded excursion often ends where the human
    # stopped, leaving the last created node a 0-out-link LEAF.  Bots A* down the
    # cheap one-way FALL/WALK links into it and STRAND (no way out) -- the biggest
    # single source of collection loss.  Iteratively drop created leaf nodes (and
    # the links into them), which can expose the upstream node as the next leaf;
    # never touch pre-existing graph nodes.
    pruned = 0
    if not a.keep_leaves:
        while True:
            dead = [i for i in new_ids
                    if i < len(nodes) and len(nodes[i]["links"]) == 0]
            if not dead:
                break
            drop = set(dead)
            keep = [i for i in range(len(nodes)) if i not in drop]
            remap = {old: new for new, old in enumerate(keep)}
            new_nodes = []
            for old in keep:
                nd = nodes[old]
                nd["links"] = [l for l in nd["links"] if l["to"] not in drop]
                for l in nd["links"]:
                    l["to"] = remap[l["to"]]
                new_nodes.append(nd)
            nodes = new_nodes
            new_ids = {remap[i] for i in new_ids if i not in drop}
            pruned += len(drop)

    print(f"nodes {n0} -> {len(nodes)} (+{created - pruned} new, {pruned} leaf-pruned), "
          f"links +{added}  " + " ".join(f"{k}={v}" for k, v in counts.items() if v))

    if a.apply or a.out:
        out = a.out or a.nav
        ne.save(out, nodes, hdr_flags)   # preserve FROZEN etc. across the edit
        print(f"WROTE {out}")
    else:
        print("(dry-run; pass --apply or --out to write)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

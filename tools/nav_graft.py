#!/usr/bin/env python3
"""
Graft routes to unreachable items from a donor nav into a base nav.

Swapping a whole nav for a gen_nav-grown one trades metrics (2026-10: grown
navs for q2dm4/5/6/8 routed new items but cost deathmatch pickups -1..-8%).
This takes only what the base lacks: for every item the base nav cannot route
to from any deathmatch spawn but the donor can, it copies the donor's
shortest path from the base's reachable region to that item -- its nodes
(merged onto base nodes within MERGE_DIST) and links (types/costs, plus the
reverse link where the donor has one).  Everything else in the base graph is
untouched, so the nav-file format and FROZEN bit are preserved.

Spawn and item positions come from the map's BSP entity lump (loose
engine/baseq2/maps/<map>.bsp, else inside engine/baseq2/pak*.pak).
Reachability here is plain graph connectivity over all link types, which is
looser than the DLL's reach oracle; always confirm with the DLL
(tools/gated_items.py) and an A/B before shipping a graft.

Usage:
    py tools/nav_graft.py <map> --base B.nav --donor D.nav --out O.nav [--dry-run]
"""

import argparse
import collections
import glob
import math
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import nav_edit

SNAP = 96.0         # an item / spawn belongs to the nearest node within this
MATCH = 32.0        # a donor node coincides with a base node within this
MERGE_DIST = 16.0   # grafted donor nodes merge onto base nodes this close
ITEM_PREFIX = ("item_", "weapon_", "ammo_")
# the DLL reach oracle's base mask: WATER (4) and PLAT (5) routes count as
# "gated", not reachable -- mirror it so the graft targets what the DLL calls
# no_path, and never grafts a route the oracle would not accept
GATED_TYPES = {4, 5}


def bsp_bytes(m):
    loose = os.path.join(REPO, "engine", "baseq2", "maps", f"{m}.bsp")
    if os.path.isfile(loose):
        return open(loose, "rb").read()
    for pak in sorted(glob.glob(os.path.join(REPO, "engine", "baseq2", "*.pak"))):
        with open(pak, "rb") as f:
            _id, off, ln = struct.unpack("<4sii", f.read(12))
            f.seek(off)
            d = f.read(ln)
            for i in range(0, ln, 64):
                name, o, l = struct.unpack("<56sii", d[i:i + 64])
                if name.split(b"\0")[0].decode() == f"maps/{m}.bsp":
                    f.seek(o)
                    return f.read(l)
    sys.exit(f"no BSP for {m}")


def entities(m):
    data = bsp_bytes(m)
    off, ln = struct.unpack_from("<ii", data, 8)        # lump 0 = entities
    text = data[off:off + ln].decode("latin1")
    out = []
    for block in re.findall(r"\{([^}]*)\}", text):
        kv = dict(re.findall(r'"([^"]*)"\s+"([^"]*)"', block))
        if "origin" in kv:
            kv["origin"] = tuple(float(x) for x in kv["origin"].split())
            out.append(kv)
    return out


def nearest(nodes, p, cap):
    best, bi = cap, -1
    for i, n in enumerate(nodes):
        d = math.dist(n["origin"], p)
        if d < best:
            best, bi = d, i
    return bi


def reachable(nodes, starts):
    seen, q = set(starts), collections.deque(starts)
    while q:
        i = q.popleft()
        for l in nodes[i]["links"]:
            if l["type"] in GATED_TYPES:
                continue
            if l["to"] not in seen:
                seen.add(l["to"])
                q.append(l["to"])
    return seen


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("map")
    ap.add_argument("--base", required=True)
    ap.add_argument("--donor", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    base, bflags = nav_edit.load(a.base)
    donor, _ = nav_edit.load(a.donor)
    ents = entities(a.map)
    spawns = [e["origin"] for e in ents if e.get("classname") == "info_player_deathmatch"]
    items = [(e["classname"], e["origin"]) for e in ents
             if e.get("classname", "").startswith(ITEM_PREFIX)]

    b_reach = reachable(base, [i for i in (nearest(base, s, SNAP) for s in spawns) if i >= 0])
    d_reach = reachable(donor, [i for i in (nearest(donor, s, SNAP) for s in spawns) if i >= 0])
    # donor nodes that coincide with a reachable base node: graft entry points
    entry = {}
    for di, dn in enumerate(donor):
        bi = nearest(base, dn["origin"], MATCH)
        if bi >= 0 and bi in b_reach:
            entry[di] = bi
    # reverse adjacency of the donor for back-search from each item node
    rev = collections.defaultdict(list)
    for i, n in enumerate(donor):
        for l in n["links"]:
            if l["type"] not in GATED_TYPES:
                rev[l["to"]].append((i, l))

    added_nodes = added_links = 0
    grafted = []
    for cls, pos in items:
        bi = nearest(base, pos, SNAP)
        if bi >= 0 and bi in b_reach:
            continue                    # base already routes it
        di = nearest(donor, pos, SNAP)
        if di < 0 or di not in d_reach:
            continue                    # donor can't either
        # BFS backwards from the item node to the nearest entry point
        prev, q, hit = {di: None}, collections.deque([di]), None
        while q and hit is None:
            u = q.popleft()
            if u in entry:
                hit = u
                break
            for v, l in rev[u]:
                if v not in prev:
                    prev[v] = (u, l)
                    q.append(v)
        if hit is None:
            continue
        # walk forward entry -> item, mapping donor nodes to base indices
        path, u = [], hit
        while u is not None:
            path.append(u)
            nxt = prev[u]
            u = nxt[0] if nxt else None
        idx = {}
        for k, dnode in enumerate(path):
            if k == 0:
                idx[dnode] = entry[dnode]
                continue
            m = nearest(base, donor[dnode]["origin"], MERGE_DIST)
            if m < 0:
                base.append({"origin": donor[dnode]["origin"], "flags": donor[dnode]["flags"], "links": []})
                m = len(base) - 1
                added_nodes += 1
            idx[dnode] = m
        for x, y in zip(path, path[1:]):
            for src, dst in ((x, y), (y, x)):
                link = next((l for l in donor[src]["links"] if l["to"] == dst), None)
                if link is None:
                    continue
                bsrc, bdst = idx[src], idx[dst]
                if bsrc == bdst or any(l["to"] == bdst for l in base[bsrc]["links"]):
                    continue
                base[bsrc]["links"].append({"to": bdst, "type": link["type"], "cost": link["cost"]})
                added_links += 1
        b_reach = reachable(base, [i for i in (nearest(base, s, SNAP) for s in spawns) if i >= 0])
        grafted.append(f"{cls}@({pos[0]:.0f},{pos[1]:.0f},{pos[2]:.0f}) via {len(path) - 1} hops")

    print(f"[graft] {a.map}: {len(grafted)} item route(s) grafted, +{added_nodes} nodes, +{added_links} links")
    for g in grafted:
        print(f"  {g}")
    if not a.dry_run and grafted:
        nav_edit.save(a.out, base, bflags)
        print(f"[graft] -> {a.out}")


if __name__ == "__main__":
    main()

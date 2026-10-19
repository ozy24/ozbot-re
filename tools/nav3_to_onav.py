#!/usr/bin/env python3
"""nav3_to_onav -- convert a Quake II Rerelease bot nav ("NAV3", the Kex
engine's bots/navigation/<map>.nav) into an ozbot nav (ONAV), FROZEN.

Maps that ship a Kex nav, or a mod that bakes one (Rocket Arena 2's
`sv nav_bake`, MuffMode's), get a graph ozbot can use without learning one:
on an arena map the learner has nothing to learn from, since bots only walk
where a mod puts them.  The format is documented in MuffMode's
docs-dev/nav-file-format.md.

  py tools/nav3_to_onav.py <in.nav> <out.nav> [--floor-origins]

Links map onto ozbot's types:
  Walk -> WALK; WalkOffLedge -> FALL; LongJump, BarrierJump, ManualJump -> JUMP;
  Elevator -> PLAT.  Teleport, Pusher and Train links are left out: ozbot seeds
  them from the map's entities at every load (NAV_LINK_DERIVED), as it would
  for a graph it learned.  Other types are dropped (reported).
Each node keeps at most NAV_MAX_LINKS (12) links, the shortest; costs are
ozbot's own (nav_edit.link_cost).

Origins: the stock files hold the player's standing origin, which is what
ozbot's nodes are too.  Some bakers write the floor instead (Rocket Arena 2:
standing origin - 23.5, so the Kex engine binds bots to start nodes);
--floor-origins lifts those back up.
"""
from __future__ import annotations

import argparse
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nav_edit as ne  # noqa: E402

NAV3_MAGIC = b"NAV3"
FLOOR_LIFT = 23.5  # RA2's ra2_navbake.cpp: floor_offset = PLAYER_MINS.z + 0.5
MAX_NODES = 2048   # bot_nav.h NAV_MAX_NODES

# NAV3 link type -> ozbot link type (None: rebuilt by ozbot at load, so skipped)
WALK, FALL, JUMP, PLAT = 0, 1, 2, 5
TYPE_MAP = {0: WALK, 2: FALL, 1: JUMP, 4: JUMP, 5: JUMP, 7: PLAT,
            3: None, 6: None, 8: None}
NAMES = {0: "Walk", 1: "LongJump", 2: "WalkOffLedge", 3: "Teleport", 4: "BarrierJump",
         5: "ManualJump", 6: "Pusher", 7: "Elevator", 8: "Train"}


def read_nav3(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != NAV3_MAGIC:
        sys.exit(f"FAIL: {path} is not a NAV3 file")
    version, nodes, links, travs = struct.unpack_from("<IIII", data, 4)
    if version != 6:
        sys.exit(f"FAIL: NAV3 version {version}, expected 6")
    off = 24
    table = [struct.unpack_from("<HHHH", data, off + 8 * i) for i in range(nodes)]
    off += 8 * nodes
    origins = [struct.unpack_from("<fff", data, off + 12 * i) for i in range(nodes)]
    off += 12 * nodes
    flat = [struct.unpack_from("<HBBH", data, off + 6 * i) for i in range(links)]
    off += 6 * links + 48 * travs
    if off + 4 > len(data):
        sys.exit(f"FAIL: {path} is truncated")
    return table, origins, flat


def convert(table, origins, flat, lift):
    if len(table) > MAX_NODES:
        sys.exit(f"FAIL: {len(table)} nodes; ozbot loads at most {MAX_NODES}")
    pts = [(x, y, z + lift) for (x, y, z) in origins]
    nodes = []
    dropped = {}
    capped = 0
    for i, (_flags, num, first, _radius) in enumerate(table):
        out = []
        for to, typ, _lflags, _trav in flat[first:first + num]:
            mapped = TYPE_MAP.get(typ, "unknown")
            if mapped is None:
                continue
            if mapped == "unknown" or to >= len(pts) or to == i:
                dropped[typ] = dropped.get(typ, 0) + 1
                continue
            if any(l["to"] == to for l in out):
                continue
            out.append({"to": to, "type": mapped, "cost": ne.link_cost(pts[i], pts[to], mapped)})
        if len(out) > ne.NAV_MAX_LINKS:
            capped += len(out) - ne.NAV_MAX_LINKS
            out.sort(key=lambda l: l["cost"])
            out = out[:ne.NAV_MAX_LINKS]
        nodes.append({"origin": pts[i], "flags": 0, "links": out})
    return nodes, dropped, capped


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--floor-origins", action="store_true",
                    help="the NAV3 holds floor points (RA2's baker): lift them to standing origins")
    args = ap.parse_args(argv[1:])

    table, origins, flat = read_nav3(args.input)
    nodes, dropped, capped = convert(table, origins, flat, FLOOR_LIFT if args.floor_origins else 0.0)
    ne.save(args.output, nodes, ne.NAVHDR_FROZEN)
    links = sum(len(n["links"]) for n in nodes)
    print(f"{args.output}: {len(nodes)} nodes, {links} links, FROZEN"
          + (f"; {capped} links over the per-node cap dropped" if capped else "")
          + "".join(f"; dropped {c} {NAMES.get(t, f'type {t}')}" for t, c in sorted(dropped.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

#!/usr/bin/env python3
"""
One-off ONAV v1 nav graph editor for ozbot-re surgical link/node edits.

Matches Nav_Load / Nav_Save in src/bot_nav.c:
  header: int32 magic ('ONAV'), version, count
  per node: float origin[3], uint8 flags, int32 num_links,
            then num_links * nav_link_t (12 bytes: int32 to, uint8 type,
            3 pad, float cost)

Link types: 0 WALK, 1 FALL, 2 JUMP, 3 TELEPORT, 4 WATER, 5 PLAT,
            6 PLAYBOOK, 7 TRAIN

Usage:
  py tools/nav_edit.py dump <file.nav> [--near x,y,z[,r]]
  py tools/nav_edit.py add-node <file.nav> x y z [--flags N] [--out out.nav]
  py tools/nav_edit.py add-link <file.nav> from to type [--out out.nav]
  py tools/nav_edit.py del-link <file.nav> from to [--out out.nav]
  py tools/nav_edit.py path <file.nav> x1,y1,z1 x2,y2,z2
"""

from __future__ import annotations

import argparse
import heapq
import math
import os
import struct
import sys

NAV_MAGIC = 0x56414E4F  # 'ONAV' little-endian
NAV_VERSION = 2  # v2 adds a uint32 header flags word after the node count
NAV_MAX_LINKS = 12

NAVHDR_FROZEN = 1  # baked graph: runtime learner won't grow/save it (bot_nav.h)

LINK_NAMES = {
    0: "WALK", 1: "FALL", 2: "JUMP", 3: "TELEPORT",
    4: "WATER", 5: "PLAT", 6: "PLAYBOOK", 7: "TRAIN",
}
NAME_TO_TYPE = {v: k for k, v in LINK_NAMES.items()}


def link_cost(a, b, typ):
    d = math.dist(a, b)
    if typ == 1:  # FALL
        return d * 1.2
    if typ == 2:  # JUMP
        return d * 1.5 + 32.0
    if typ == 3:  # TELEPORT
        return 64.0
    if typ == 4:  # WATER
        return d * 1.6
    if typ == 5:  # PLAT
        return d + 400.0
    if typ == 6:  # PLAYBOOK
        return 64.0 + d * 0.6
    if typ == 7:  # TRAIN
        return d + 1500.0
    return d  # WALK


def load(path):
    """Returns (nodes, flags). flags is the ONAV v2 header flags word (0 for v1)."""
    with open(path, "rb") as f:
        data = f.read()
    magic, ver, count = struct.unpack_from("<iii", data, 0)
    if magic != NAV_MAGIC or ver not in (1, 2):
        sys.exit(f"FAIL: bad header magic={magic:#x} ver={ver}")
    off = 12
    hdr_flags = 0
    if ver >= 2:
        (hdr_flags,) = struct.unpack_from("<i", data, off)
        off += 4
    nodes = []
    for _ in range(count):
        x, y, z = struct.unpack_from("<fff", data, off)
        off += 12
        flags = data[off]
        off += 1
        (nl,) = struct.unpack_from("<i", data, off)
        off += 4
        if nl < 0 or nl > NAV_MAX_LINKS:
            sys.exit(f"FAIL: bad num_links {nl}")
        links = []
        for _j in range(nl):
            (to,) = struct.unpack_from("<i", data, off)
            typ = data[off + 4]
            (cost,) = struct.unpack_from("<f", data, off + 8)
            off += 12
            links.append({"to": to, "type": typ, "cost": cost})
        nodes.append({"origin": (x, y, z), "flags": flags, "links": links})
    if off != len(data):
        sys.exit(f"FAIL: trailing bytes ({off} vs {len(data)})")
    return nodes, hdr_flags


def save(path, nodes, flags=0):
    parts = [struct.pack("<iiii", NAV_MAGIC, NAV_VERSION, len(nodes), flags)]
    for n in nodes:
        parts.append(struct.pack("<fff", *n["origin"]))
        parts.append(struct.pack("<B", n["flags"] & 0xFF))
        parts.append(struct.pack("<i", len(n["links"])))
        for l in n["links"]:
            # on-disk: int to, byte type, 3 pad, float cost
            parts.append(struct.pack("<iBxxx f", l["to"], l["type"] & 0xFF, float(l["cost"])))
    with open(path, "wb") as f:
        f.write(b"".join(parts))


def nearest(nodes, p, n=5, ztol=1e9):
    scored = []
    for i, nd in enumerate(nodes):
        dx = nd["origin"][0] - p[0]
        dy = nd["origin"][1] - p[1]
        dz = nd["origin"][2] - p[2]
        if abs(dz) > ztol:
            continue
        scored.append((math.sqrt(dx * dx + dy * dy + dz * dz), i))
    scored.sort()
    return scored[:n]


def parse_xyz(s):
    parts = [float(x) for x in s.split(",")]
    if len(parts) < 3:
        sys.exit(f"FAIL: need x,y,z got {s!r}")
    return tuple(parts[:3])


def cmd_dump(args):
    nodes, flags = load(args.file)
    frozen = " [FROZEN]" if flags & NAVHDR_FROZEN else ""
    print(f"{args.file}: {len(nodes)} nodes{frozen}")
    if args.near:
        bits = [float(x) for x in args.near.split(",")]
        p = tuple(bits[:3])
        r = bits[3] if len(bits) > 3 else 200.0
        hits = nearest(nodes, p, n=20, ztol=r)
        for d, i in hits:
            if d > r:
                break
            nd = nodes[i]
            outs = ", ".join(
                f"{l['to']}:{LINK_NAMES.get(l['type'], l['type'])}"
                for l in nd["links"]
            )
            o = nd["origin"]
            print(
                f"  n{i} d={d:.0f} ({o[0]:.0f},{o[1]:.0f},{o[2]:.0f}) "
                f"flags={nd['flags']} out={len(nd['links'])} -> [{outs}]"
            )
        return
    # summary by link type
    from collections import Counter
    lt = Counter()
    for nd in nodes:
        for l in nd["links"]:
            lt[l["type"]] += 1
    print("  links: " + ", ".join(
        f"{LINK_NAMES.get(k, k)}={lt[k]}" for k in sorted(lt)
    ))


def cmd_add_node(args):
    nodes, flags = load(args.file)
    origin = (args.x, args.y, args.z)
    # reuse nearby same-level node (Nav_AddNode density ~96, z tol 48)
    # unless --force (surgical mouth/landing seeds that must sit at an exact spot)
    if not args.force:
        for i, nd in enumerate(nodes):
            dx = nd["origin"][0] - origin[0]
            dy = nd["origin"][1] - origin[1]
            dz = nd["origin"][2] - origin[2]
            if abs(dz) > 48:
                continue
            if dx * dx + dy * dy + dz * dz < 96 * 96:
                print(f"reuse n{i} at {tuple(round(c) for c in nd['origin'])} (within density)")
                print(i)
                return
    nodes.append({"origin": origin, "flags": args.flags, "links": []})
    out = args.out or args.file
    save(out, nodes, flags)
    print(f"added n{len(nodes)-1} at ({args.x:.0f},{args.y:.0f},{args.z:.0f}) -> {out}")
    print(len(nodes) - 1)


def parse_type(s):
    if s.isdigit():
        return int(s)
    u = s.upper()
    if u not in NAME_TO_TYPE:
        sys.exit(f"FAIL: unknown type {s!r} (want {list(NAME_TO_TYPE)})")
    return NAME_TO_TYPE[u]


def cmd_add_link(args):
    nodes, flags = load(args.file)
    frm, to = args.frm, args.to
    if not (0 <= frm < len(nodes) and 0 <= to < len(nodes)):
        sys.exit(f"FAIL: node out of range (0..{len(nodes)-1})")
    if frm == to:
        sys.exit("FAIL: self-link")
    typ = parse_type(args.type)
    n = nodes[frm]
    for l in n["links"]:
        if l["to"] == to:
            print(f"already linked {frm}->{to} type={LINK_NAMES[l['type']]}")
            return
    if len(n["links"]) >= NAV_MAX_LINKS:
        sys.exit(f"FAIL: n{frm} already has {NAV_MAX_LINKS} links")
    cost = link_cost(n["origin"], nodes[to]["origin"], typ)
    n["links"].append({"to": to, "type": typ, "cost": cost})
    out = args.out or args.file
    save(out, nodes, flags)
    print(
        f"added {frm}->{to} {LINK_NAMES[typ]} cost={cost:.1f} "
        f"({tuple(round(c) for c in n['origin'])} -> "
        f"{tuple(round(c) for c in nodes[to]['origin'])}) -> {out}"
    )


def cmd_del_link(args):
    nodes, flags = load(args.file)
    n = nodes[args.frm]
    before = len(n["links"])
    n["links"] = [l for l in n["links"] if l["to"] != args.to]
    if len(n["links"]) == before:
        print(f"no link {args.frm}->{args.to}")
        return
    out = args.out or args.file
    save(out, nodes, flags)
    print(f"deleted {args.frm}->{args.to} -> {out}")


def cmd_freeze(args):
    nodes, flags = load(args.file)
    newflags = (flags & ~NAVHDR_FROZEN) if args.unfreeze else (flags | NAVHDR_FROZEN)
    out = args.out or args.file
    save(out, nodes, newflags)
    state = "unfrozen" if args.unfreeze else "FROZEN"
    print(f"{state} {out} ({len(nodes)} nodes, flags={newflags})")


def cmd_path(args):
    nodes, _flags = load(args.file)
    a = parse_xyz(args.start)
    b = parse_xyz(args.end)
    sa = nearest(nodes, a, n=1, ztol=120)
    sb = nearest(nodes, b, n=1, ztol=120)
    if not sa or not sb:
        sys.exit("FAIL: no nearby nodes")
    s, ds = sa[0][1], sa[0][0]
    g, dg = sb[0][1], sb[0][0]
    print(f"start n{s} d={ds:.0f}  goal n{g} d={dg:.0f}")
    dist = {s: 0.0}
    prev = {}
    pq = [(0.0, s)]
    while pq:
        c, u = heapq.heappop(pq)
        if c != dist.get(u):
            continue
        if u == g:
            break
        for l in nodes[u]["links"]:
            nc = c + l["cost"]
            if nc < dist.get(l["to"], 1e99):
                dist[l["to"]] = nc
                prev[l["to"]] = (u, l["type"])
                heapq.heappush(pq, (nc, l["to"]))
    if g not in dist:
        print("NO PATH")
        return
    path = []
    cur = g
    while cur != s:
        u, typ = prev[cur]
        path.append((u, cur, typ))
        cur = u
    path.reverse()
    print(f"YES cost={dist[g]:.0f} hops={len(path)}")
    for u, v, typ in path:
        print(f"  {u}->{v} {LINK_NAMES[typ]}")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("dump")
    p.add_argument("file")
    p.add_argument("--near", help="x,y,z[,radius]")
    p.set_defaults(func=cmd_dump)

    p = sub.add_parser("add-node")
    p.add_argument("file")
    p.add_argument("x", type=float)
    p.add_argument("y", type=float)
    p.add_argument("z", type=float)
    p.add_argument("--flags", type=int, default=0)
    p.add_argument("--force", action="store_true",
                   help="always create a node (skip 96u density reuse)")
    p.add_argument("--out")
    p.set_defaults(func=cmd_add_node)

    p = sub.add_parser("add-link")
    p.add_argument("file")
    p.add_argument("frm", type=int)
    p.add_argument("to", type=int)
    p.add_argument("type")
    p.add_argument("--out")
    p.set_defaults(func=cmd_add_link)

    p = sub.add_parser("del-link")
    p.add_argument("file")
    p.add_argument("frm", type=int)
    p.add_argument("to", type=int)
    p.add_argument("--out")
    p.set_defaults(func=cmd_del_link)

    p = sub.add_parser("path")
    p.add_argument("file")
    p.add_argument("start")
    p.add_argument("end")
    p.set_defaults(func=cmd_path)

    p = sub.add_parser("freeze", help="stamp the ONAV FROZEN header flag "
                                      "(runtime learner won't grow/save this graph)")
    p.add_argument("file")
    p.add_argument("--unfreeze", action="store_true", help="clear the flag instead")
    p.add_argument("--out")
    p.set_defaults(func=cmd_freeze)

    args = ap.parse_args(argv[1:])
    args.func(args)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

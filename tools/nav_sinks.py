#!/usr/bin/env python3
"""
Find nav SINKS -- strongly connected regions with no outgoing link to the rest
of the graph -- and propose BSP-verified exit links back to the main component.

A bot inside a sink can never route out: if it spawns or falls there it idles
with no goal (q2dm3: a spawn point on a 12-node island stranded 4-6 of 16 solo
instances for the whole run).  The learner never records the exit because it
only learns traversals the bot already makes.

Proposals are FALL links (drop <= --max-drop, below Q2 fall damage at ~188u)
or WALK links (|dz| <= 18, or a stair/ramp climb up to 0.6*h and 96u with
floor within 48u under every sample -- rejects ledges and gaps), checked with point line-of-sight at knee, body and
head height plus +/-14u lateral offsets -- an approximation of the player hull,
so every proposal must be validated in-engine (stranded.py + an A/B).

    python3 tools/nav_sinks.py <map.nav> <map.bsp> [--apply OUT.nav] [--per-sink 2]
"""
import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nav_edit as ne
from q2bsp import BSP


def sccs(adj):
    n = len(adj)
    index, low, on, st, comps, idx = {}, {}, set(), [], [], [0]
    for s in range(n):
        if s in index:
            continue
        index[s] = low[s] = idx[0]; idx[0] += 1; st.append(s); on.add(s)
        stack = [(s, 0)]
        while stack:
            v, i = stack[-1]
            if i < len(adj[v]):
                stack[-1] = (v, i + 1)
                w = adj[v][i]
                if w not in index:
                    index[w] = low[w] = idx[0]; idx[0] += 1; st.append(w); on.add(w)
                    stack.append((w, 0))
                elif w in on:
                    low[v] = min(low[v], index[w])
            else:
                stack.pop()
                if stack:
                    low[stack[-1][0]] = min(low[stack[-1][0]], low[v])
                if low[v] == index[v]:
                    comp = []
                    while True:
                        w = st.pop(); on.discard(w); comp.append(w)
                        if w == v:
                            break
                    comps.append(comp)
    return comps


def floored(bsp, a, c, below=48):
    """Solid floor within `below` units under every 16u sample of a->c."""
    n = max(2, int(math.dist(a, c) / 16))
    for k in range(n + 1):
        t = k / n
        p = [a[i] + (c[i] - a[i]) * t for i in range(3)]
        if not any(bsp.contents((p[0], p[1], p[2] - d)) & 1 for d in range(24, below + 1, 4)):
            return False
    return True


def clear(bsp, a, c, heights=(-16, 0, 24)):
    for dz in heights:
        for dx, dy in ((0, 0), (14, 0), (-14, 0), (0, 14), (0, -14)):
            if not bsp.los((a[0] + dx, a[1] + dy, a[2] + dz), (c[0] + dx, c[1] + dy, c[2] + dz)):
                return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nav")
    ap.add_argument("bsp")
    ap.add_argument("--apply", default=None)
    ap.add_argument("--per-sink", type=int, default=2)
    ap.add_argument("--max-drop", type=float, default=180)
    ap.add_argument("--max-h", type=float, default=300)
    ap.add_argument("--min-size", type=int, default=2)
    a = ap.parse_args()

    nodes, flags = ne.load(a.nav)
    bsp = BSP(a.bsp)
    n = len(nodes)
    adj = [[l["to"] for l in nd["links"] if 0 <= l["to"] < n] for nd in nodes]
    comps = sorted(sccs(adj), key=len, reverse=True)
    giant = set(comps[0])
    adds = []
    for comp in comps[1:]:
        cs = set(comp)
        if len(comp) < a.min_size or any(w not in cs for v in comp for w in adj[v]):
            continue
        cands = []
        for i in comp:
            A = nodes[i]["origin"]
            for j in giant:
                B = nodes[j]["origin"]
                h = math.hypot(B[0] - A[0], B[1] - A[1])
                dz = A[2] - B[2]
                if h < 16 or h > a.max_h:
                    continue
                if abs(dz) <= 18 or (-dz > 18 and -dz <= min(0.6 * h, 96)):
                    typ, mid = "WALK", None
                elif 30 <= dz <= a.max_drop:
                    typ, mid = "FALL", (B[0], B[1], A[2])
                else:
                    continue
                if mid is None:
                    flat = abs(dz) <= 18
                    ok = (clear(bsp, A, B) if flat else clear(bsp, A, B, (0, 24))) and floored(bsp, A, B)
                else:
                    ok = clear(bsp, A, mid) and clear(bsp, mid, B)
                if ok:
                    cands.append((h + abs(dz), i, j, typ, round(h), round(dz)))
        cands.sort()
        picked, used = [], set()
        for c in cands:
            if c[1] in used or c[2] in used:
                continue
            picked.append(c); used.update((c[1], c[2]))
            if len(picked) >= a.per_sink:
                break
        o = tuple(round(x) for x in nodes[comp[0]]["origin"])
        print(f"sink {len(comp)} nodes near {o}: "
              + (", ".join(f"{c[1]}->{c[2]} {c[3]} h{c[4]} dz{c[5]}" for c in picked) or "NO verified exit"))
        adds += picked
    if a.apply and adds:
        for c in adds:
            A, B = nodes[c[1]]["origin"], nodes[c[2]]["origin"]
            t = {"WALK": 0, "FALL": 1}[c[3]]
            nodes[c[1]]["links"].append({"to": c[2], "type": t, "cost": ne.link_cost(A, B, t)})
        ne.save(a.apply, nodes, flags)
        print(f"wrote {a.apply} (+{len(adds)} links)")


if __name__ == "__main__":
    main()

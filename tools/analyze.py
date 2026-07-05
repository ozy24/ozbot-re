#!/usr/bin/env python3
"""
ozbot telemetry analyzer (Phase 4).

Reads a JSONL telemetry file produced by the bot DLL and prints a report:
  - per-bot summary (movement, %goal, %fight, pickups, frags, K/D)
  - goal success and pickups-by-item
  - failure hotspots (giveup / pathfail / item_lost clustered by map cell)
  - time-to-pickup stats
  - weapon usage

It also writes top-down PNG heatmaps next to the log (no external deps; PNG is
encoded with the stdlib):
  - <name>_coverage.png : where bots spend time, with failures overlaid in red

Usage:
    python tools/analyze.py <telemetry.jsonl> [--no-png]
"""

import json
import math
import os
import struct
import sys
import zlib
from collections import defaultdict


# ----------------------------------------------------------------------------
# loading
# ----------------------------------------------------------------------------
def load(path):
    ticks, events, reach, bad = [], [], [], 0
    with open(path, "r", encoding="utf-8") as fp:
        for line in fp:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                bad += 1
                continue
            if rec.get("type") == "tick":
                ticks.append(rec)
            elif rec.get("type") == "event":
                events.append(rec)
            elif rec.get("type") == "reach":
                reach.append(rec)
    return ticks, events, reach, bad


# ----------------------------------------------------------------------------
# minimal PNG writer (8-bit RGB)
# ----------------------------------------------------------------------------
def write_png(path, width, height, pixels):
    """pixels: flat bytearray of length width*height*3 (RGB, row-major)."""
    def chunk(typ, data):
        body = typ + data
        return (struct.pack(">I", len(data)) + body +
                struct.pack(">I", zlib.crc32(body) & 0xffffffff))

    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)  # filter: none
        raw.extend(pixels[y * stride:(y + 1) * stride])

    out = bytearray()
    out += b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    out += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(out)


def heat_color(t):
    """t in [0,1] -> (r,g,b): black -> blue -> cyan -> green -> yellow."""
    if t <= 0:
        return (10, 10, 18)
    stops = [(0.0, (10, 10, 40)), (0.35, (0, 120, 200)),
             (0.6, (0, 200, 160)), (0.8, (120, 220, 60)), (1.0, (255, 230, 60))]
    for (a, ca), (b, cb) in zip(stops, stops[1:]):
        if t <= b:
            f = (t - a) / (b - a) if b > a else 0
            return tuple(int(ca[i] + (cb[i] - ca[i]) * f) for i in range(3))
    return stops[-1][1]


def make_coverage_png(ticks, fail_pts, out_path, size=320, margin=8):
    xs = [t["x"] for t in ticks]
    ys = [t["y"] for t in ticks]
    if not xs:
        return False
    minx, maxx, miny, maxy = min(xs), max(xs), min(ys), max(ys)
    spanx = max(maxx - minx, 1.0)
    spany = max(maxy - miny, 1.0)
    span = max(spanx, spany)  # keep aspect square

    grid_n = size - 2 * margin

    def to_px(x, y):
        gx = int((x - minx) / span * (grid_n - 1)) + margin
        # flip y so north is up
        gy = int((maxy - y) / span * (grid_n - 1)) + margin
        return gx, gy

    counts = [[0] * size for _ in range(size)]
    for t in ticks:
        gx, gy = to_px(t["x"], t["y"])
        if 0 <= gx < size and 0 <= gy < size:
            counts[gy][gx] += 1

    peak = max((max(row) for row in counts), default=1)
    logpeak = math.log1p(peak)

    pixels = bytearray(size * size * 3)
    for y in range(size):
        for x in range(size):
            c = counts[y][x]
            t = math.log1p(c) / logpeak if c and logpeak else 0
            r, g, b = heat_color(t)
            i = (y * size + x) * 3
            pixels[i], pixels[i + 1], pixels[i + 2] = r, g, b

    # overlay failures as red 3x3 dots
    for (fx, fy) in fail_pts:
        gx, gy = to_px(fx, fy)
        for dy in range(-1, 2):
            for dx in range(-1, 2):
                px, py = gx + dx, gy + dy
                if 0 <= px < size and 0 <= py < size:
                    i = (py * size + px) * 3
                    pixels[i], pixels[i + 1], pixels[i + 2] = 255, 40, 40

    write_png(out_path, size, size, pixels)
    return True


# ----------------------------------------------------------------------------
# report
# ----------------------------------------------------------------------------
def cluster(points, cell=128.0):
    """Group (x,y) points into grid cells; return [(count, cx, cy)] sorted desc."""
    cells = defaultdict(list)
    for (x, y) in points:
        cells[(round(x / cell), round(y / cell))].append((x, y))
    out = []
    for pts in cells.values():
        cx = sum(p[0] for p in pts) / len(pts)
        cy = sum(p[1] for p in pts) / len(pts)
        out.append((len(pts), cx, cy))
    out.sort(reverse=True)
    return out


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    flags = {a for a in argv[1:] if a.startswith("--")}
    if len(args) != 1:
        print("usage: python analyze.py <telemetry.jsonl> [--no-png]", file=sys.stderr)
        return 2

    path = args[0]
    ticks, events, reach, bad = load(path)

    print(f"file:         {path}")
    print(f"tick records: {len(ticks)}")
    print(f"event records:{len(events)}" + (f"   (malformed lines: {bad})" if bad else ""))
    if not ticks:
        print("FAIL: no tick records.")
        return 1

    times = [t["t"] for t in ticks]
    print(f"time span:    {min(times):.1f}s .. {max(times):.1f}s")
    nav_nodes = [t.get("nav_nodes") for t in ticks if t.get("nav_nodes") is not None]
    if nav_nodes:
        print(f"nav graph:    {min(nav_nodes)} -> {max(nav_nodes)} nodes")

    by_bot = defaultdict(list)
    for t in ticks:
        by_bot[t["bot"]].append(t)
    ev = defaultdict(lambda: defaultdict(int))
    for e in events:
        if "bot" not in e:
            continue    # world events (item respawn scheduling) aren't per-bot
        ev[e["bot"]][e.get("event", "?")] += 1

    # ---- per-bot summary ----
    print()
    print(f"{'bot':>4} {'ticks':>6} {'path':>9} {'%goal':>6} {'%fight':>7} "
          f"{'pickups':>8} {'frags':>6} {'deaths':>7} {'K/D':>5}")
    any_move = False
    tot_pick = tot_frag = tot_death = 0
    for bot in sorted(by_bot):
        rows = sorted(by_bot[bot], key=lambda r: r["t"])
        plen = 0.0
        goal = fight = frags = 0
        prev = None
        for r in rows:
            if prev:
                plen += math.dist((prev["x"], prev["y"], prev["z"]),
                                  (r["x"], r["y"], r["z"]))
            goal += 1 if r.get("mode") == 1 else 0
            fight += 1 if r.get("enemy", -1) >= 0 else 0
            frags = max(frags, r.get("score", 0))
            prev = r
        any_move = any_move or plen > 1
        deaths = ev[bot].get("death", 0)
        pickups = ev[bot].get("pickup", 0)
        kd = frags / deaths if deaths else float(frags)
        tot_pick += pickups
        tot_frag += frags
        tot_death += deaths
        print(f"{bot:>4} {len(rows):>6} {plen:>9.0f} "
              f"{100*goal/len(rows):>5.0f}% {100*fight/len(rows):>6.0f}% "
              f"{pickups:>8} {frags:>6} {deaths:>7} {kd:>5.1f}")
    print(f"{'all':>4} {'':>6} {'':>9} {'':>6} {'':>7} "
          f"{tot_pick:>8} {tot_frag:>6} {tot_death:>7}")

    if not any_move:
        print("FAIL: no bot moved.")
        return 1

    # ---- goal success ----
    attempts = sum(ev[b].get("goal_item", 0) + ev[b].get("goal", 0) for b in ev)
    reaches = sum(ev[b].get("reach", 0) for b in ev)
    print()
    if attempts:
        succ = tot_pick + reaches
        print(f"goal success: {succ}/{attempts} ({100*succ/attempts:.0f}%)  "
              f"[{tot_pick} pickups, {reaches} roam arrivals]")
    # item-touch completion: pickups / item-goal attempts (the metric that
    # actually measures reaching+touching items, excluding easy roam goals)
    item_attempts = sum(ev[b].get("goal_item", 0) for b in ev)
    if item_attempts:
        print(f"ITEM completion: {tot_pick}/{item_attempts} "
              f"({100*tot_pick/item_attempts:.0f}%) of item goals picked up")

    pickups_by_item = defaultdict(int)
    for e in events:
        if e.get("event") == "pickup":
            pickups_by_item[e.get("item", "?")] += 1
    if pickups_by_item:
        print("pickups by item: " +
              ", ".join(f"{n}x {it}" for it, n in
                        sorted(pickups_by_item.items(), key=lambda kv: -kv[1])))

    # ---- weapon usage (tick share) ----
    wuse = defaultdict(int)
    for t in ticks:
        wuse[t.get("weapon", "?")] += 1
    print("weapon usage:    " +
          ", ".join(f"{w} {100*n/len(ticks):.0f}%" for w, n in
                    sorted(wuse.items(), key=lambda kv: -kv[1])[:6]))

    # ---- failure hotspots ----
    fail_pts = [(e["x"], e["y"]) for e in events
                if e.get("event") in ("giveup", "pathfail", "item_lost")
                and "x" in e]
    by_type = defaultdict(int)
    for e in events:
        by_type[e.get("event", "?")] += 1
    print()
    print("failures: " + ", ".join(f"{by_type[k]} {k}" for k in
          ("giveup", "pathfail", "item_lost") if by_type.get(k)))
    if fail_pts:
        print("top failure hotspots (cluster -> count @ x,y):")
        for n, cx, cy in cluster(fail_pts)[:6]:
            print(f"   {n:>3}  @ ({cx:>7.0f}, {cy:>7.0f})")

    # ---- giveup diagnostics (why item runs fail) ----
    gv = [e for e in events if e.get("event") == "giveup" and "gdist" in e]
    if gv:
        n = len(gv)
        fighting = sum(1 for e in gv if e.get("fighting"))
        near = sum(1 for e in gv if e.get("gdist", 9999) < 80)
        dists = sorted(e.get("gdist", 0) for e in gv)
        gbest = sorted(e.get("gbest", 9999) for e in gv)
        reached_node = sum(1 for e in gv if e.get("gbest", 9999) < 60)
        # how far along the path did it get? pidx/plen
        prog = [100 * e["pidx"] / e["plen"] for e in gv
                if e.get("plen", 0) > 0]
        print(f"giveup diagnostics (n={n}): fighting={100*fighting/n:.0f}%  "
              f"within-80u-of-item={100*near/n:.0f}%  median-dist={dists[n//2]:.0f}u")
        print(f"   closest-to-goal-node: median={gbest[n//2]:.0f}u  "
              f"ever-reached-node(<60u)={100*reached_node/n:.0f}%")
        if prog:
            prog.sort()
            print(f"   path progress at giveup: median={prog[len(prog)//2]:.0f}% "
                  f"of waypoints")
        # oracle verdict at giveup time (navq; plans/nav-oracle.md Phase C):
        # "ok" = a route still existed (execution failure), "no_path" = the
        # route evaporated mid-attempt (penalization pruned it)
        navq = defaultdict(int)
        for e in gv:
            if e.get("navq"):
                navq[e["navq"]] += 1
        if navq:
            tot = sum(navq.values())
            print("   route at giveup: " + ", ".join(
                f"{k}={100*v/tot:.0f}%" for k, v in
                sorted(navq.items(), key=lambda kv: -kv[1])))

    # ---- map reachability (bot_reachlog oracle sweeps) ----
    # One sweep per instance per when-phase; dedupe to unique item spots and
    # report the modal verdict (instances can disagree as graphs mature).
    if reach:
        for when in ("load", "quit"):
            spots = {}
            for r in reach:
                if r.get("when", "load") != when:
                    continue
                key = (r.get("item"), round(r["x"]), round(r["y"]), round(r["z"]))
                spots.setdefault(key, []).append(r)
            if not spots:
                continue
            verdicts = defaultdict(int)
            problems = defaultdict(list)
            for key, rs in spots.items():
                codes = defaultdict(int)
                for r in rs:
                    c = r["code"] + (f":{r['gate']}" if r.get("gate") else "")
                    codes[c] += 1
                modal = max(codes.items(), key=lambda kv: kv[1])[0]
                verdicts[modal] += 1
                if modal != "ok":
                    problems[modal].append(key[0])
            print(f"map reachability [{when}] ({len(spots)} item spots): " +
                  ", ".join(f"{v} {k}" for k, v in
                            sorted(verdicts.items(), key=lambda kv: -kv[1])))
            for code, items in sorted(problems.items()):
                names = defaultdict(int)
                for it in items:
                    names[it] += 1
                print(f"   {code}: " + ", ".join(
                    (f"{n}x {it}" if n > 1 else it) for it, n in
                    sorted(names.items(), key=lambda kv: -kv[1])))

    # ---- time-to-pickup ----
    deltas = []
    pending = {}
    for e in sorted(events, key=lambda r: r["t"]):
        b = e.get("bot")
        if b is None:          # world events (item_respawned, pb_*) have no bot
            continue
        if e.get("event") == "goal_item":
            pending[b] = e["t"]
        elif e.get("event") == "pickup" and b in pending:
            deltas.append(e["t"] - pending.pop(b))
    if deltas:
        deltas.sort()
        print(f"time-to-pickup:  n={len(deltas)} "
              f"median={deltas[len(deltas)//2]:.1f}s "
              f"mean={sum(deltas)/len(deltas):.1f}s "
              f"max={deltas[-1]:.1f}s")

    # ---- heatmap png ----
    if "--no-png" not in flags:
        base = os.path.splitext(path)[0]
        out = base + "_coverage.png"
        try:
            if make_coverage_png(ticks, fail_pts, out):
                print(f"\nheatmap:      {out}  (occupancy; red = failure spots)")
        except Exception as e:  # noqa: BLE001
            print(f"heatmap: skipped ({e})")

    print("\nOK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

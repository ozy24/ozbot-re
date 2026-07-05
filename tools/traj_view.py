#!/usr/bin/env python3
"""
ozbot trajectory / stuck-spot viewer (path analysis, complements input_view.py).

input_view.py segments a bot_inputlog trace into *jumps* for strafe-jump work.
This tool answers the other question a human demo poses: "where did the player
pause, and what distinct paths did they walk out of that spot?" -- i.e. the
"bot gets stuck HERE, here are the ways through" recordings.

It reads the same {"type":"input",...} records (per-frame usercmd + position),
then:
  * finds "dwell" spots where the player stood still (low speed for >= --dwell s)
    -- these are the candidate stuck-spot markers; a lone in-place jump at a
    dwell spot is the strongest "look here" signal a human can leave.
  * given a hub (auto = the longest dwell, or --hub X Y Z), segments the trace
    into excursions: contiguous runs that leave the hub (> --near units) and
    return, reporting each one's duration, max distance, farthest point,
    heading, and z-range -- the "few different paths from that spot".
  * --coarse dumps the whole path sampled every --step seconds.

Usage:
    python tools/traj_view.py <telemetry.jsonl> [slot]
        [--hub X Y Z] [--near U] [--dwell S] [--coarse] [--step S]

    slot default 0 (the human recorder is usually slot 0).
"""

import json
import math
import argparse


def load(path, slot):
    recs = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                o = json.loads(line)
            except ValueError:
                continue
            if o.get("type") != "input":
                continue
            if slot is not None and o.get("slot") != slot:
                continue
            recs.append(o)
    recs.sort(key=lambda o: o["t"])
    return recs


def dwell_spots(recs, dwell_s, spd_max=40.0):
    """Contiguous runs where spd < spd_max for >= dwell_s seconds."""
    spots = []
    i, n = 0, len(recs)
    while i < n:
        if recs[i]["spd"] < spd_max:
            j = i
            while j < n and recs[j]["spd"] < spd_max:
                j += 1
            seg = recs[i:j]
            dur = seg[-1]["t"] - seg[0]["t"]
            if dur >= dwell_s:
                cx = sum(r["x"] for r in seg) / len(seg)
                cy = sum(r["y"] for r in seg) / len(seg)
                cz = sum(r["z"] for r in seg) / len(seg)
                jumped = any(r.get("up", 0) > 0 for r in seg)
                spots.append((seg[0]["t"], dur, cx, cy, cz, jumped))
            i = j
        else:
            i += 1
    return spots


def excursions(recs, hub, near, zband=40.0):
    hx, hy, hz = hub

    def d(r):
        return math.hypot(r["x"] - hx, r["y"] - hy)

    exc = []
    state = "near"
    start = None
    for idx, r in enumerate(recs):
        is_near = d(r) < near and abs(r["z"] - hz) < zband
        if state == "near" and not is_near:
            state, start = "far", idx
        elif state == "far" and is_near:
            state = "near"
            exc.append(recs[start:idx])
    return exc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("slot", nargs="?", type=int, default=0)
    ap.add_argument("--hub", nargs=3, type=float, default=None,
                    metavar=("X", "Y", "Z"))
    ap.add_argument("--near", type=float, default=120.0)
    ap.add_argument("--dwell", type=float, default=1.0)
    ap.add_argument("--coarse", action="store_true")
    ap.add_argument("--step", type=float, default=0.5)
    a = ap.parse_args()

    recs = load(a.path, a.slot)
    if not recs:
        print("no input records for slot %s (bot_inputlog 1?)" % a.slot)
        return
    print("%d input frames  t=%.2f..%.2f (slot %d)\n"
          % (len(recs), recs[0]["t"], recs[-1]["t"], a.slot))

    spots = dwell_spots(recs, a.dwell)
    print("=== dwell spots (stood still >= %.1fs) ===" % a.dwell)
    for t, dur, x, y, z, j in spots:
        print("  t=%6.2f dur%5.1fs  (%6.0f,%6.0f,%5.0f)%s"
              % (t, dur, x, y, z, "   <-- in-place JUMP (marker?)" if j else ""))

    if a.hub:
        hub = tuple(a.hub)
    elif spots:
        # default hub = a jump-marked dwell (the strongest "look here" signal)
        # if one exists, else the longest dwell.
        marked = [s for s in spots if s[5]]
        cand = (sorted(marked, key=lambda s: -s[1])[0] if marked
                else sorted(spots, key=lambda s: -s[1])[0])
        hub = (round(cand[2]), round(cand[3]), round(cand[4]))
    else:
        hub = (round(recs[0]["x"]), round(recs[0]["y"]), round(recs[0]["z"]))
    print("\n=== excursions from hub (%.0f,%.0f,%.0f), near<%.0f ==="
          % (hub[0], hub[1], hub[2], a.near))
    for k, seg in enumerate(excursions(recs, hub, a.near)):
        far = max(seg, key=lambda r: math.hypot(r["x"] - hub[0], r["y"] - hub[1]))
        maxd = math.hypot(far["x"] - hub[0], far["y"] - hub[1])
        ang = math.degrees(math.atan2(far["y"] - hub[1], far["x"] - hub[0]))
        zmin = min(r["z"] for r in seg)
        zmax = max(r["z"] for r in seg)
        print("  exc %2d: t=%6.2f-%6.2f dur%5.1fs  maxdist%5.0f "
              "far=(%5.0f,%5.0f,%4.0f) dir%+4.0fdeg  z[%.0f,%.0f]"
              % (k + 1, seg[0]["t"], seg[-1]["t"], seg[-1]["t"] - seg[0]["t"],
                 maxd, far["x"], far["y"], far["z"], ang, zmin, zmax))

    if a.coarse:
        print("\n=== coarse trajectory (every ~%.1fs) ===" % a.step)
        last = -1e9
        for r in recs:
            if r["t"] - last >= a.step:
                g = "g" if r.get("onground", True) else "A"
                print("  t=%6.2f (%6.0f,%6.0f,%5.0f) %s spd%4.0f yaw%4.0f"
                      % (r["t"], r["x"], r["y"], r["z"], g, r["spd"], r["yaw"]))
                last = r["t"]


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
ozbot input-trace viewer (strafe-jump analysis).

Reads a JSONL telemetry file that contains {"type":"input",...} records written
by the game DLL when `bot_inputlog 1` is set (see Bot_LogInput in bot_log.c).
Each record is one server frame of a REAL player's usercmd_t -- the raw inputs a
.dm2 demo does NOT carry: forward/side/up move, jump (up>0), attack, and the
per-frame view yaw/pitch that drives strafe-jump air-acceleration.

It segments the trace into individual jumps (airborne runs) and prints, per jump:
  - the key-hold timeline (forward/back, strafe L/R, jump, fire) frame by frame
  - the view-yaw sweep during the air phase (the "mouse turn")
  - the speed curve (takeoff -> peak -> landing), flagging air-accel

This is the "full picture of HOW the jumps are done" that feeds the box-hop +
strafe-jump capability design (plans/longjump).

Usage:
    python tools/input_view.py <telemetry.jsonl> [slot]

    slot : optional player slot to filter (default: all input records)
"""

import json
import sys


def angdiff(a, b):
    """Shortest signed angle a-b in (-180, 180]."""
    return (a - b + 180.0) % 360.0 - 180.0


def load(path, slot=None):
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


def segment_jumps(recs):
    """Return list of (ctx_start, air_start, air_end, ctx_end) index tuples,
    where [air_start, air_end) is a maximal run of onground==False and the ctx
    window pads two frames on each side for takeoff/landing context."""
    jumps = []
    n = len(recs)
    i = 0
    while i < n:
        if not recs[i].get("onground", True):
            j = i
            while j < n and not recs[j].get("onground", True):
                j += 1
            a, b = max(0, i - 2), min(n, j + 2)
            # skip non-jumps: spawn drops / idle falls where the player never
            # pressed jump and wasn't moving (no up input in the context window
            # and peak airborne speed is trivial).
            jumped = any(recs[m]["up"] > 0 for m in range(a, b))
            peak = max(recs[m]["spd"] for m in range(i, j)) if j > i else 0
            if jumped or peak > 100:
                jumps.append((a, i, j, b))
            i = j + 1
        else:
            i += 1
    return jumps


def fwd_glyph(v):
    return "F" if v > 50 else "B" if v < -50 else "."


def side_glyph(v):
    return "R" if v > 50 else "L" if v < -50 else "."


def show(recs, jumps):
    print("%d input frames, %d jump(s)\n" % (len(recs), len(jumps)))
    for k, (a, s, e, b) in enumerate(jumps):
        air = recs[s:e]
        if not air:
            continue
        t0 = recs[s]["t"]
        spd_takeoff = recs[s]["spd"]
        spd_land = recs[min(e, len(recs) - 1)]["spd"]
        peak_spd = max(r["spd"] for r in air)
        z0 = recs[s]["z"]
        peak_z = max(r["z"] for r in air)
        yaw0 = air[0]["yaw"]
        yaw1 = air[-1]["yaw"]
        # sum real per-usercmd msec (client sends ~30 Hz, ~3 cmds per 10 Hz game frame)
        airtime_ms = sum(r.get("msec", 0) for r in air)

        accel = ("AIR-ACCEL +%.0f" % (peak_spd - spd_takeoff)
                 if peak_spd > spd_takeoff + 20 else "no accel")
        print("=== jump %d @ t=%.2f   airtime~%dms ===" % (k + 1, t0, airtime_ms))
        print("  speed:  takeoff %.0f -> peak %.0f -> land %.0f   (%s)"
              % (spd_takeoff, peak_spd, spd_land, accel))
        print("  height: z %.0f -> peak %.0f   (rise %+.0f)" % (z0, peak_z, peak_z - z0))
        print("  view:   yaw %.0f -> %.0f   (turned %+.0f deg during air)"
              % (yaw0, yaw1, angdiff(yaw1, yaw0)))
        print("  timeline (F/B=fwd, L/R=strafe, ^=jump, *=fire):")
        print("    %6s %4s %5s %5s %4s %6s %5s  keys" %
              ("t", "grnd", "fwd", "side", "up", "yaw", "spd"))
        prev_yaw = None
        for r in recs[a:b]:
            keys = (fwd_glyph(r["fwd"]) + side_glyph(r["side"])
                    + ("^" if r["up"] > 0 else ".") + ("*" if r["atk"] else "."))
            g = "grnd" if r.get("onground", True) else "AIR"
            dyaw = "" if prev_yaw is None else "  d%+.0f" % angdiff(r["yaw"], prev_yaw)
            prev_yaw = r["yaw"]
            print("    %6.2f %4s %5d %5d %4d %6.0f %5.0f  %s%s" %
                  (r["t"], g, r["fwd"], r["side"], r["up"], r["yaw"], r["spd"], keys, dyaw))
        print()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    path = sys.argv[1]
    slot = int(sys.argv[2]) if len(sys.argv) > 2 else None
    recs = load(path, slot)
    if not recs:
        print("no {\"type\":\"input\"} records in %s "
              "(was the server run with bot_inputlog 1?)" % path)
        sys.exit(2)
    jumps = segment_jumps(recs)
    show(recs, jumps)


if __name__ == "__main__":
    main()

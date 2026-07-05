#!/usr/bin/env python3
"""
ozbot-re playbook baker (Phase R4).

Turns a per-tick input capture (bot_inputlog for humans via record_inputs.bat,
bot_cmdlog for bots) into a playbook entry the game replays via
NAV_LINK_PLAYBOOK (src/bot_playback.c).  The capture must come from a server
running at the tick rate the playbook will replay at (sv_fps 40 on the
ozbot-re rig): entries record their tickrate and the game skips mismatches.

Usage:
  py tools/make_playbook.py <log.jsonl> --slot N --start T0 --end T1 \
        --name mh_jump --out engine/ozbotre/playbooks/q2dm1.pbk
  py tools/make_playbook.py <log.jsonl> --slot N --auto --name test_run ...
        (--auto picks the longest fast grounded run -- pipeline validation)
  py tools/make_playbook.py <log.jsonl> --list
        (per-slot summary of speed/jump timeline, to pick --start/--end)

Appends to --out (a .pbk holds many entries).  Format: see bot_playback.c.

Stdlib only.
"""

import argparse
import json
import math
import os
import sys
from collections import defaultdict


def load_inputs(path):
    by_slot = defaultdict(list)
    tickrate = None
    with open(path, "r", encoding="utf-8") as fp:
        for line in fp:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            if rec.get("type") == "run":
                tickrate = rec.get("tick_rate", tickrate)
            elif rec.get("type") == "input":
                by_slot[rec.get("slot", 0)].append(rec)
    for slot in by_slot:
        by_slot[slot].sort(key=lambda r: r["t"])
    return tickrate, by_slot


def resample_to_grid(rs, tickrate):
    """Collapse a sub-tick-oversampled capture to one record per server tick.

    bot_cmdlog (bots) emits exactly one usercmd per server frame, already on
    the tick grid.  A human client (record_inputs.bat / bot_inputlog) sends
    usercmds at its *render* framerate (msec 6-13 => ~80-160Hz), so several
    records share one server-time `t` (the log stamps server time, not the
    sub-frame).  The playbook replays one 25ms usercmd per tick, so decimate
    each tick bucket to its last record (freshest committed view/buttons,
    tick-end position).  Idempotent on already-on-grid captures.
    """
    if not rs:
        return rs
    dt = 1.0 / tickrate
    buckets = {}
    for r in rs:
        k = round(r["t"] / dt)      # tick index; logged t is rounded to 0.01
        buckets.setdefault(k, []).append(r)
    out = []
    for k in sorted(buckets):
        grp = buckets[k]
        rep = dict(grp[-1])         # tick-end position + freshest view/held keys
        # buttons are momentary: a jump/attack pressed in ANY sub-frame of the
        # tick must survive decimation (else brief jump presses vanish and the
        # replay never leaves the ground).  OR them across the bucket.
        rep["up"] = max(r.get("up", 0) for r in grp)
        rep["atk"] = max(r.get("atk", 0) for r in grp)
        out.append(rep)
    return out


def list_slots(by_slot):
    for slot in sorted(by_slot):
        rs = by_slot[slot]
        jumps = sum(1 for a, b in zip(rs, rs[1:]) if a["up"] <= 0 and b["up"] > 0)
        print(f"slot {slot}: {len(rs)} ticks, t={rs[0]['t']:.1f}..{rs[-1]['t']:.1f}, "
              f"{jumps} jump presses, peak spd {max(r['spd'] for r in rs):.0f}")


def auto_segments(rs, min_dur=1.5, max_dur=4.0):
    """Fast, mostly-grounded runs, best-first (validation segments, not tricks)."""
    found = []
    i = 0
    while i < len(rs):
        # entry speed must be reachable by a normally-running bot (~300 cap),
        # else the ALIGN speed precondition can never be satisfied
        if rs[i]["spd"] > 310 or rs[i]["spd"] < 150 or not rs[i]["onground"]:
            i += 1
            continue
        j = i
        while (j < len(rs) and rs[j]["spd"] > 150
               and rs[j]["t"] - rs[i]["t"] < max_dur):
            j += 1
        if j > i:
            seg = rs[i:j]
            dur = seg[-1]["t"] - seg[0]["t"]
            grounded = sum(1 for r in seg if r["onground"]) / len(seg)
            disp = math.dist((seg[0]["x"], seg[0]["y"]),
                             (seg[-1]["x"], seg[-1]["y"]))
            if dur >= min_dur and grounded >= 0.7:
                found.append((disp, seg[0]["t"], seg[-1]["t"]))
        i = j + 1 if j > i else i + 1
    if not found:
        sys.exit("FAIL: no segment found (need >1.5s of >150ups mostly-grounded travel)")
    found.sort(reverse=True)
    return found


def bake(rs, t0, t1, tickrate, name, pos_tol, yaw_tol, drift=0.0, dwell=0.0):
    seg = [r for r in rs if t0 <= r["t"] <= t1]
    if len(seg) < 8:
        sys.exit(f"FAIL: only {len(seg)} ticks in [{t0},{t1}]")

    # tick cadence sanity: captures from the target rate are already on-grid
    dts = [round(b["t"] - a["t"], 3) for a, b in zip(seg, seg[1:])]
    med_dt = sorted(dts)[len(dts) // 2]
    want_dt = round(1.0 / tickrate, 3)
    if abs(med_dt - want_dt) > 0.011:
        sys.exit(f"FAIL: capture cadence {med_dt}s does not match tickrate {tickrate} "
                 f"({want_dt}s) -- record on a server running at the target sv_fps")

    a = seg[0]
    spd0 = a["spd"]
    if spd0 < 60:       # standing start
        min_speed, max_speed = 0.0, max(80.0, spd0 + 60)
    else:
        min_speed, max_speed = max(0.0, spd0 - 50), spd0 + 70

    lines = []
    lines.append(f"entry {name}")
    lines.append(f"tickrate {tickrate}")
    lines.append(f"anchor {a['x']:.1f} {a['y']:.1f} {a['z']:.1f} {a['yaw']:.1f} "
                 f"{pos_tol:.0f} {yaw_tol:.0f} {min_speed:.0f} {max_speed:.0f}")
    if drift > 0:
        lines.append(f"drift {drift:.0f}")
    if dwell > 0:
        lines.append(f"dwell {dwell:.2f}")
    lines.append(f"exit {seg[-1]['x']:.1f} {seg[-1]['y']:.1f} {seg[-1]['z']:.1f}")
    # each tick's drift reference is the position AFTER it executes = the next
    # record's position; the final record contributes only its position
    for cur, nxt in zip(seg, seg[1:]):
        lines.append(f"tick {cur['fwd']} {cur['side']} {cur['up']} "
                     f"{cur['yaw']:.2f} {cur['pitch']:.2f} "
                     f"{nxt['x']:.1f} {nxt['y']:.1f} {nxt['z']:.1f}")
    lines.append("end")
    return lines, len(seg) - 1


def main(argv):
    ap = argparse.ArgumentParser(description="Bake an input capture into a playbook entry.")
    ap.add_argument("log", help="telemetry JSONL with input records")
    ap.add_argument("--slot", type=int, default=None, help="client slot / bot id to bake")
    ap.add_argument("--start", type=float, help="segment start (game seconds)")
    ap.add_argument("--end", type=float, help="segment end (game seconds)")
    ap.add_argument("--auto", action="store_true",
                    help="auto-pick the longest fast grounded run(s) (validation)")
    ap.add_argument("--auto-n", type=int, default=1,
                    help="with --auto: bake the top N segments (default 1)")
    ap.add_argument("--list", action="store_true", help="summarize slots and exit")
    ap.add_argument("--name", default="entry1")
    ap.add_argument("--out", default=None, help=".pbk to append to "
                    "(default engine/ozbotre/playbooks/<map>.pbk next to the log)")
    ap.add_argument("--tickrate", type=int, default=None,
                    help="override tick rate (default: the log's run header)")
    ap.add_argument("--pos-tol", type=float, default=10.0)
    ap.add_argument("--yaw-tol", type=float, default=20.0)
    ap.add_argument("--drift", type=float, default=0.0,
                    help="per-entry drift-abort ceiling (0 => engine default 56u; "
                    "raise for narrow-walkway strafe runs that drift more)")
    ap.add_argument("--dwell", type=float, default=0.0,
                    help="rescue-only: engage only after a bot sits on the anchor "
                    "this long (s); for descents that should fire when a bot is "
                    "stuck, not pull a passing bot off its task")
    args = ap.parse_args(argv[1:])

    tickrate, by_slot = load_inputs(args.log)
    if args.tickrate:
        tickrate = args.tickrate
    if not by_slot:
        sys.exit("FAIL: no input records (bot_inputlog / bot_cmdlog was off?)")

    if args.list:
        print(f"tick_rate: {tickrate}")
        list_slots(by_slot)
        return 0

    slot = args.slot if args.slot is not None else sorted(by_slot)[0]
    if slot not in by_slot:
        sys.exit(f"FAIL: no input records for slot {slot} (have {sorted(by_slot)})")
    rs = by_slot[slot]
    if tickrate is None:
        sys.exit("FAIL: log has no run header; pass --tickrate")
    n_raw = len(rs)
    rs = resample_to_grid(rs, tickrate)
    if len(rs) < n_raw:
        print(f"resampled slot {slot} to tick grid: {n_raw} -> {len(rs)} "
              f"records ({n_raw/max(len(rs),1):.1f} usercmds/tick)")

    if args.auto:
        segs = auto_segments(rs)[:args.auto_n]
        baked = []
        for n, (disp, t0, t1) in enumerate(segs):
            print(f"auto segment {n}: t={t0:.2f}..{t1:.2f} (disp {disp:.0f}u)")
            name = args.name if len(segs) == 1 else f"{args.name}_{n}"
            baked.append(bake(rs, t0, t1, tickrate, name, args.pos_tol, args.yaw_tol, args.drift, args.dwell))
    else:
        if args.start is None or args.end is None:
            sys.exit("FAIL: need --start and --end (or --auto / --list)")
        baked = [bake(rs, args.start, args.end, tickrate, args.name,
                      args.pos_tol, args.yaw_tol, args.drift, args.dwell)]

    out = args.out
    if out is None:
        # derive map name from the log filename (<map>_<stamp>.jsonl)
        base = os.path.basename(args.log)
        mapname = base.split("_")[0] if "_" in base else "q2dm1"
        out = os.path.join(os.path.dirname(os.path.dirname(args.log)),
                           "playbooks", f"{mapname}.pbk")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    fresh = not os.path.isfile(out)
    with open(out, "a", encoding="ascii") as fp:
        if fresh:
            fp.write("playbook 1\n")
        for lines, nticks in baked:
            fp.write("\n".join(lines) + "\n")
            print(f"wrote entry ({nticks} ticks @ {tickrate}Hz) -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

#!/usr/bin/env python3
"""
Per-entry playbook outcomes: which recorded maneuvers actually pay.

Runs the pinned rig (baselines/nav_shipped + baselines/playbooks, or --pbk)
and attributes every pb_* event to its entry by the nearest anchor.  An entry
with many aligns and ~0 completions is pure overhead: bots route through its
cheap PLAYBOOK link, then burn the align timeout or drift out (q2dm1 2026-10:
dropping three such entries was +9.1% deathmatch pickups).  'silent' = engages
that ended without done/abort -- usually the goal item was collected mid-replay.

Usage:
    py tools/playbook_stats.py q2dm1 [--pbk FILE] [--seeds 700,900,1100] [--rigs dm,solo]
"""

import argparse
import collections
import json
import math
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ab_cvar
import rig_platform


def anchors(path):
    out, name = {}, None
    with open(path, encoding="latin1") as f:
        for line in f:
            p = line.split()
            if p and p[0] == "entry":
                name = p[1]
            elif p and p[0] == "anchor" and name:
                out[name] = (float(p[1]), float(p[2]), float(p[3]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("map")
    ap.add_argument("--pbk", default=None)
    ap.add_argument("--seeds", default="700,900,1100")
    ap.add_argument("--rigs", default="dm,solo")
    a = ap.parse_args()
    f = rig_platform.map_file(a.map)
    ab_cvar.stage(a.map)
    dst = os.path.join(ab_cvar.ENGINE, ab_cvar.AB_MOD, "playbooks", f"{f}.pbk")
    if a.pbk:
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(a.pbk, dst)
    if not os.path.isfile(dst):
        sys.exit(f"no playbook for {a.map}")
    anch = anchors(dst)
    pe = collections.defaultdict(collections.Counter)
    for rig in a.rigs.split(","):
        for seed in a.seeds.split(","):
            out = subprocess.run([sys.executable, os.path.join(HERE, "run_parallel.py"), "--repro", "--fastsim",
                                  "--map", a.map, "--instances", "16", "--seconds", "90",
                                  "--bots", str(ab_cvar.RIG_BOTS[rig]), "--seed", seed,
                                  "--mod", ab_cvar.AB_MOD, "--base-port", "28300",
                                  "--cvar", "sv_fps", "40", "--no-analyze"], capture_output=True, text=True).stdout
            path = re.search(r"-> (\S+\.jsonl)", out).group(1)
            last = {}
            with open(path) as fh:
                for line in fh:
                    if '"event": "pb_' not in line:
                        continue
                    r = json.loads(line)
                    if r["event"] == "pb_align":
                        last[r["bot"]] = min(anch, key=lambda k: math.dist((r["x"], r["y"], r["z"]), anch[k]))
                    pe[last.get(r["bot"], "?")][r["event"][3:]] += 1
            os.remove(path)
    print(f"{'entry':22s} {'align':>6s} {'engage':>6s} {'done':>5s} {'ab_align':>8s} {'ab_drift':>8s} "
          f"{'ab_stall':>8s} {'silent':>6s}  done/align")
    for k, v in sorted(pe.items(), key=lambda kv: -kv[1]["align"]):
        silent = v["engage"] - v["done"] - v["abort_drift"] - v["abort_stall"]
        print(f"{k:22s} {v['align']:6d} {v['engage']:6d} {v['done']:5d} {v['abort_align']:8d} "
              f"{v['abort_drift']:8d} {v['abort_stall']:8d} {silent:6d}  {100 * v['done'] / max(1, v['align']):5.1f}%")
    for k in anch:
        if k not in pe:
            print(f"{k:22s} never attempted")


if __name__ == "__main__":
    main()

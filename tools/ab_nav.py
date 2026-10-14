#!/usr/bin/env python3
"""
Paired same-seed A/B of two nav FILES for one map: the pinned benchmark
configuration (ab_cvar.stage: shipped nav + playbook) with the nav swapped for
a candidate in the treatment arm.  For surgical nav edits (nav_sinks.py exits,
nav_edit.py links) where the code is unchanged and only the graph differs.

    python3 tools/ab_nav.py q2dm3 demos/derived/navsinks/q2dm3.nav \\
        [--base baselines/nav_shipped/q2dm3.nav] [--rigs solo,dm] [--seeds ...]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ab_cvar
import benchmark as bm
import rig_platform
import stranded


def run(m, nav, bots, seed, args):
    ab_cvar.AB_MOD = "ozbotre_abnav"
    ab_cvar.stage(m)
    shutil.copy2(nav, os.path.join(ab_cvar.ENGINE, ab_cvar.AB_MOD, "nav",
                                   f"{rig_platform.map_file(m)}.nav"))
    cmd = [sys.executable, os.path.join(HERE, "run_parallel.py"), "--repro", "--fastsim",
           "--mod", ab_cvar.AB_MOD, "--map", m, "--bots", str(bots),
           "--instances", str(args.instances), "--seconds", str(args.seconds),
           "--seed", str(seed), "--base-port", str(args.base_port),
           "--cvar", "sv_fps", "40", "--no-analyze"]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    mt = re.search(r"-> (\S+\.jsonl)", out)
    if not mt:
        return None
    path = mt.group(1)
    met = bm.compute_metrics(path) or {}
    st, live, _ = stranded.stranded(path, 5.0)
    os.remove(path)
    return {"pickups": met.get("pickups", 0), "deaths": met.get("deaths", 0),
            "stranded": st / max(live, 1)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("map")
    ap.add_argument("nav")
    ap.add_argument("--base", default=None)
    ap.add_argument("--rigs", default="solo,dm")
    ap.add_argument("--seeds", default="700,900,1100,1300")
    ap.add_argument("--instances", type=int, default=16)
    ap.add_argument("--seconds", type=float, default=90)
    ap.add_argument("--base-port", type=int, default=28600)
    a = ap.parse_args()
    base = a.base or os.path.join(ab_cvar.REPO, "baselines", "nav_shipped",
                                  f"{rig_platform.map_file(a.map)}.nav")
    for rig in a.rigs.split(","):
        tot = {"base": [0, 0, 0.0], "cand": [0, 0, 0.0]}
        up = 0
        seeds = a.seeds.split(",")
        for s in seeds:
            r = {k: run(a.map, p, ab_cvar.RIG_BOTS[rig], int(s), a)
                 for k, p in (("base", base), ("cand", a.nav))}
            for k in r:
                tot[k][0] += r[k]["pickups"]; tot[k][1] += r[k]["deaths"]
                tot[k][2] += r[k]["stranded"] / len(seeds)
            up += r["cand"]["pickups"] > r["base"]["pickups"]
            print(f"{a.map} {rig:4s} s{s}  base {r['base']['pickups']}p  cand {r['cand']['pickups']}p")
        b, c = tot["base"], tot["cand"]
        print(f"  {a.map} {rig}: pickups {b[0]}->{c[0]} ({(c[0] - b[0]) / max(b[0], 1):+.1%}, "
              f"{up}/{len(seeds)} up)  deaths {b[1]}->{c[1]}  stranded {b[2]:.1%}->{c[2]:.1%}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
Paired same-seed A/B of one cvar on the benchmark's pinned navs + playbooks.

For every map x rig x seed bank, runs the control (cvar at its first value)
and each treatment value on the SAME seed, from a scratch gamedir staged with
baselines/nav_shipped/<map>.nav and baselines/playbooks/<map>.pbk -- the
benchmark's configuration, never the live (autosaving) gamedir.  Reports
summed pickups / giveups / deaths / frags per arm, the per-bank win count,
and the giveup taxonomy shift (tools/mine_giveups.py classes).

Usage:
    py tools/ab_cvar.py bot_budgetext 0 5 10 --maps q2dm1,q2dm2,q2dm8 \\
        --seeds 700,900,1100,1300 [--rigs dm,solo] [--json out.json]
"""

import argparse
import collections
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import benchmark as bm
import mine_giveups as mg
import rig_platform

ENGINE = os.path.join(REPO, "engine")
AB_MOD = "ozbotre_ab"
RIG_BOTS = {"dm": 5, "solo": 1, "duel": 2}


def stage(m):
    f = rig_platform.map_file(m)
    _exe, dll = rig_platform.repro_names(ENGINE)
    src = os.path.join(ENGINE, AB_MOD)
    shutil.rmtree(src, ignore_errors=True)
    os.makedirs(os.path.join(src, "nav"))
    shutil.copy2(os.path.join(REPO, "dist", dll), os.path.join(src, dll))
    for nav_dir in (os.path.join(REPO, "baselines", "nav_shipped"),
                    os.path.join(REPO, "baselines", "nav_pool")):
        nav = os.path.join(nav_dir, f"{f}.nav")
        if os.path.isfile(nav):
            shutil.copy2(nav, os.path.join(src, "nav", f"{f}.nav"))
            break
    pbk = os.path.join(REPO, "baselines", "playbooks", f"{f}.pbk")
    if os.path.isfile(pbk):
        os.makedirs(os.path.join(src, "playbooks"))
        shutil.copy2(pbk, os.path.join(src, "playbooks", f"{f}.pbk"))


def run(m, bots, seed, cvar, val, args):
    cmd = [sys.executable, os.path.join(HERE, "run_parallel.py"), "--repro", "--fastsim",
           "--mod", AB_MOD, "--map", m, "--bots", str(bots), "--instances", str(args.instances),
           "--seconds", str(args.seconds), "--seed", str(seed), "--base-port", str(args.base_port),
           "--cvar", "sv_fps", "40", "--cvar", cvar, val, "--no-analyze"]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    mt = re.search(r"-> (\S+\.jsonl)", out)
    if not mt:
        return None
    path = mt.group(1)
    met = bm.compute_metrics(path) or {}
    _map, gives, pb_aborts = mg.load(path)
    classes = collections.Counter(mg.classify(g, pb_aborts) for g in gives)
    os.remove(path)
    return {"pickups": met.get("pickups", 0), "deaths": met.get("deaths", 0),
            "frags": met.get("frags", 0), "giveups": len(gives), "classes": dict(classes)}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("cvar")
    ap.add_argument("values", nargs="+", help="control first, then treatments")
    ap.add_argument("--maps", default="q2dm1")
    ap.add_argument("--seeds", default="700,900,1100,1300")
    ap.add_argument("--rigs", default="dm,solo")
    ap.add_argument("--instances", type=int, default=16)
    ap.add_argument("--seconds", type=float, default=90)
    ap.add_argument("--base-port", type=int, default=28300)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()
    maps = args.maps.split(",")
    seeds = [int(s) for s in args.seeds.split(",")]
    rigs = args.rigs.split(",")
    ctrl, treats = args.values[0], args.values[1:]

    res = {}
    for m in maps:
        stage(m)
        for rig in rigs:
            for seed in seeds:
                arms = {v: run(m, RIG_BOTS[rig], seed, args.cvar, v, args) for v in args.values}
                res[f"{m}/{rig}/{seed}"] = arms
                print(f"{m:8s} {rig:4s} s{seed}  " + "  ".join(
                    f"{v}: {a['pickups'] if a else '—'}p/{a['giveups'] if a else '—'}g"
                    for v, a in arms.items()), flush=True)
    shutil.rmtree(os.path.join(ENGINE, AB_MOD), ignore_errors=True)

    print(f"\n== {args.cvar}: control {ctrl} vs " + ", ".join(treats))
    for m in maps + ["ALL"]:
        for rig in rigs:
            ks = [k for k in res if k.split("/")[1] == rig and (m == "ALL" or k.split("/")[0] == m)]
            ks = [k for k in ks if all(res[k].values())]
            if not ks:
                continue
            c = {f: sum(res[k][ctrl][f] for k in ks) for f in ("pickups", "giveups", "deaths", "frags")}
            for t in treats:
                tt = {f: sum(res[k][t][f] for k in ks) for f in c}
                up = sum(res[k][t]["pickups"] > res[k][ctrl]["pickups"] for k in ks)
                pct = lambda f: f"{100.0 * (tt[f] - c[f]) / c[f]:+.1f}%" if c[f] else "n/a"
                cls_c = collections.Counter(); cls_t = collections.Counter()
                for k in ks:
                    cls_c.update(res[k][ctrl]["classes"]); cls_t.update(res[k][t]["classes"])
                shift = " ".join(f"{cl} {cls_c[cl]}->{cls_t[cl]}" for cl in mg.CLASSES if cls_c[cl] or cls_t[cl])
                print(f"  {m:8s} {rig:4s} {args.cvar}={t}: pickups {c['pickups']}->{tt['pickups']} "
                      f"({pct('pickups')}, {up}/{len(ks)} up)  giveups {pct('giveups')}  "
                      f"deaths {pct('deaths')}  frags {pct('frags')}  | {shift}")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(res, f, indent=1)


if __name__ == "__main__":
    main()

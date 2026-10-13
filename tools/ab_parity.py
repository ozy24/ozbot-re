#!/usr/bin/env python3
"""
Paired id-parity strength A/B with a confidence interval.

The id-parity harness splits ONE match by bot id: even ids get the treatment,
odd ids are the control.  The raw even/odd kill share carries an id-position
bias, so every treatment run is paired with a control run on the same seed in
which the test arm is configured as a no-op; the effect is the SHIFT in the
even ids' kill share between the two.  Each server (worker) is one paired arm;
the report is the mean shift with a 95% CI (t-approximation) and the death
shift, per map and pooled.

Combat A/Bs belong on q2dm1/q2dm2/q2dm5/q2dm8 (hazard maps drive scores
negative -- see parity_frags.py).  At per-arm sd ~5pt you want >= 40 arms.

Usage:
    py tools/ab_parity.py --test bot_fleetunetest \\
        --treat bot_fleehp=60,bot_fleeratio=0.65 --control bot_fleehp=75,bot_fleeratio=0.65 \\
        [--maps q2dm1,q2dm2,q2dm5,q2dm8] [--seeds 700,900,1100] [--bots 6]
"""

import argparse
import collections
import json
import math
import os
import re
import statistics as st
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ab_cvar
import benchmark as bm

STRIDE = 1000   # run_parallel: worker i's bot b -> id i*STRIDE + b


def run(m, seed, cvars, args):
    cmd = [sys.executable, os.path.join(HERE, "run_parallel.py"), "--repro", "--fastsim",
           "--mod", ab_cvar.AB_MOD, "--map", m, "--bots", str(args.bots),
           "--instances", str(args.instances), "--seconds", str(args.seconds),
           "--seed", str(seed), "--base-port", str(args.base_port),
           "--cvar", "sv_fps", "40", "--no-analyze"]
    for k, v in cvars:
        cmd += ["--cvar", k, v]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    path = re.search(r"-> (\S+\.jsonl)", out).group(1)
    score, deaths = {}, collections.Counter()
    with open(path) as f:
        for line in f:
            if '"type": "tick"' in line:
                r = json.loads(line)
                score[r["bot"]] = r["score"]
            elif '"event": "death"' in line:
                deaths[json.loads(line)["bot"]] += 1
    os.remove(path)
    arms = {}
    for w in {b // STRIDE for b in score}:
        ev = sum(max(0, s) for b, s in score.items() if b // STRIDE == w and b % 2 == 0)
        od = sum(max(0, s) for b, s in score.items() if b // STRIDE == w and b % 2 == 1)
        de = sum(n for b, n in deaths.items() if b // STRIDE == w and b % 2 == 0)
        do = sum(n for b, n in deaths.items() if b // STRIDE == w and b % 2 == 1)
        arms[w] = {"share": ev / (ev + od) if ev + od else None,
                   "dshare": de / (de + do) if de + do else None, "kills": ev + od}
    return arms


def ci(xs):
    if len(xs) < 2:
        return (st.mean(xs) if xs else 0.0, float("nan"))
    return st.mean(xs), 1.96 * st.stdev(xs) / math.sqrt(len(xs))


def parse(s):
    return [tuple(kv.split("=", 1)) for kv in s.split(",") if kv]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--test", required=True, help="the id-parity cvar, set to 1 in both runs")
    ap.add_argument("--treat", required=True, help="name=val,... for the treatment run")
    ap.add_argument("--control", default="", help="name=val,... for the paired control (no-op arm)")
    ap.add_argument("--maps", default="q2dm1,q2dm2,q2dm5,q2dm8")
    ap.add_argument("--seeds", default="700,900,1100")
    ap.add_argument("--bots", type=int, default=6)
    ap.add_argument("--instances", type=int, default=16)
    ap.add_argument("--seconds", type=float, default=90)
    ap.add_argument("--base-port", type=int, default=28400)
    args = ap.parse_args()
    base = [(args.test, "1")]
    pooled_k, pooled_d = [], []
    for m in args.maps.split(","):
        ab_cvar.stage(m)
        ks, ds = [], []
        for seed in [int(s) for s in args.seeds.split(",")]:
            t = run(m, seed, base + parse(args.treat), args)
            c = run(m, seed, base + parse(args.control), args)
            for w in t:
                if w in c and t[w]["share"] is not None and c[w]["share"] is not None \
                        and t[w]["kills"] >= 6 and c[w]["kills"] >= 6:
                    ks.append(100 * (t[w]["share"] - c[w]["share"]))
                if w in c and t[w]["dshare"] is not None and c[w]["dshare"] is not None:
                    ds.append(100 * (t[w]["dshare"] - c[w]["dshare"]))
        mk, hk = ci(ks)
        md, hd = ci(ds)
        print(f"{m:7s} kill-share shift {mk:+.2f}pt [{mk - hk:+.2f}, {mk + hk:+.2f}]  "
              f"death-share shift {md:+.2f}pt  ({len(ks)} arms)", flush=True)
        pooled_k += ks
        pooled_d += ds
    mk, hk = ci(pooled_k)
    md, hd = ci(pooled_d)
    print(f"POOLED  kill-share shift {mk:+.2f}pt [{mk - hk:+.2f}, {mk + hk:+.2f}]  "
          f"death-share shift {md:+.2f}pt [{md - hd:+.2f}, {md + hd:+.2f}]  ({len(pooled_k)} arms)")


if __name__ == "__main__":
    main()

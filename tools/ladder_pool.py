#!/usr/bin/env python3
"""
Ladder-pool benchmark: the bot on the DM Duel Ladder's map pool.

The ladder's ranked duel server rotates 17 maps, most of them custom (MMREX
pool, campgrounds, bloodrun, kaktus) and none of those with a curated nav.
That is exactly the shipping strategy's long tail -- "ship FROZEN navs for the
rotation you care about, let the learner fill the rest" -- so this measures it
end to end, per map:

  1. nav: the curated shipped nav where one exists (baselines/nav_shipped/);
     otherwise grow one with gen_nav (mature-to-peak, FROZEN) into
     baselines/nav_pool/<map>.nav, reused on later runs (--regen to redo)
  2. solo rig (1 bot): nav-quality headline -- COLLECT% and pickups
  3. duel rig (2 bots): what the ladder actually plays

Each run appends a snapshot to baselines/ladder_pool_history.jsonl and renders
POOL.md.  Maps must be installed in engine/baseq2/maps (q64/* in a subfolder).
Runs from its own gamedir (engine/ozbotre_pool), so it cannot touch the live
navs or collide with another rig on the default gamedir.

Usage:
    py tools/ladder_pool.py --note "<what changed>" [--maps a,b] [--regen]
                            [--instances 16] [--seconds 90] [--seed 700]
"""

import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import benchmark as bm
import rig_platform

# the ranked duel server's pool (vault: "Ranked duel server for the live ladder")
POOL = ["q2dm1", "q2dm2", "q2dm3", "q64/dm3", "q64/dm4", "q64/dm8",
        "mm-aerow", "mm-biorust", "mm-fleshref", "mm-mcoil", "mm-pkill", "mm-ptrip",
        "mm-rage", "mm-reclam", "campgrounds", "bloodrun", "kaktus-re-b2"]
ENGINE = os.path.join(REPO, "engine")
POOL_MOD = "ozbotre_pool"
NAV_POOL = os.path.join(REPO, "baselines", "nav_pool")
NAV_SHIPPED = os.path.join(REPO, "baselines", "nav_shipped")
PBK = os.path.join(REPO, "baselines", "playbooks")
HISTORY = os.path.join(REPO, "baselines", "ladder_pool_history.jsonl")
POOL_MD = os.path.join(REPO, "POOL.md")
GEN_PORT = 28100        # gen_nav probes
RIG_PORT = 28200        # measurement rigs


def log(msg):
    print(f"[pool] {msg}", flush=True)


def ensure_nav(m, args):
    """Return (nav path, source) for map m, generating it if needed."""
    f = rig_platform.map_file(m)
    shipped = os.path.join(NAV_SHIPPED, f"{f}.nav")
    if os.path.isfile(shipped):
        return shipped, "shipped"
    out = os.path.join(NAV_POOL, f"{f}.nav")
    if os.path.isfile(out) and not args.regen:
        return out, "gen_nav (cached)"
    os.makedirs(NAV_POOL, exist_ok=True)
    log(f"{m}: no curated nav -- growing one with gen_nav...")
    rc = subprocess.call([sys.executable, os.path.join(HERE, "gen_nav.py"), m,
                          "--out", out, "--report", os.path.join(NAV_POOL, f"{f}.json"),
                          "--seed", str(args.seed), "--base-port", str(GEN_PORT)])
    if rc != 0 or not os.path.isfile(out):
        log(f"{m}: gen_nav failed (exit {rc})")
        return None, "gen_nav FAILED"
    return out, "gen_nav"


def stage(m, nav):
    """Fresh source gamedir holding the DLL, this map's nav and its playbook."""
    f = rig_platform.map_file(m)
    _exe, dll = rig_platform.repro_names(ENGINE)
    src = os.path.join(ENGINE, POOL_MOD)
    shutil.rmtree(src, ignore_errors=True)
    os.makedirs(os.path.join(src, "nav"))
    shutil.copy2(os.path.join(REPO, "dist", dll), os.path.join(src, dll))
    shutil.copy2(nav, os.path.join(src, "nav", f"{f}.nav"))
    pbk = os.path.join(PBK, f"{f}.pbk")
    if os.path.isfile(pbk):
        os.makedirs(os.path.join(src, "playbooks"))
        shutil.copy2(pbk, os.path.join(src, "playbooks", f"{f}.pbk"))


def run_rig(m, bots, args):
    cmd = [sys.executable, os.path.join(HERE, "run_parallel.py"), "--repro", "--fastsim",
           "--mod", POOL_MOD, "--map", m, "--bots", str(bots),
           "--instances", str(args.instances), "--seconds", str(args.seconds),
           "--seed", str(args.seed), "--base-port", str(RIG_PORT),
           "--cvar", "sv_fps", "40", "--no-analyze"]
    for name, val in (args.cvar or []):
        cmd += ["--cvar", name, val]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    mt = re.search(r"-> (\S+\.jsonl)", out)
    if not mt:
        return None
    path = mt.group(1)
    met = bm.compute_metrics(path) or {}
    reach = {"ok": 0, "gated": 0, "no_path": 0}
    h = hashlib.md5()
    with open(path, "rb") as fh:
        for line in fh:
            if b'"type": "tick"' in line or b'"type": "event"' in line:
                h.update(line)
            elif b'"when": "load"' in line:
                code = json.loads(line).get("code", "")
                key = "ok" if code == "ok" else ("no_path" if code == "no_path" else "gated")
                reach[key] += 1
    os.remove(path)
    # reach records repeat per worker; report one server's sweep
    reach = {k: v // max(1, args.instances) for k, v in reach.items()}
    keep = ("item_completion", "pickups", "attempts", "frags", "deaths", "nav_nodes")
    res = {k: met.get(k) for k in keep}
    res["reach"] = reach
    res["md5"] = h.hexdigest()[:8]
    return res


def fmt(v, pct=False):
    if v is None:
        return "—"
    return f"{v:.0f}%" if pct else str(v)


def render(history):
    latest = history[-1]
    L = ["# ozbot-re — ladder map pool benchmark", "",
         "_Auto-generated by `tools/ladder_pool.py`; do not edit by hand._", "",
         "The DM Duel Ladder's ranked duel pool. q2dm1-3 play their curated shipped "
         "navs; every other map plays a nav grown from cold by `gen_nav` (mature-to-peak, "
         "FROZEN) — the shipping strategy's long tail, measured. **solo** = 1 bot "
         "(nav quality, COLLECT%); **duel** = 2 bots (what the ladder plays). "
         "`reach` = load-time item reachability from a spawn (ok / gated / no path).", "",
         f"## Current state — {latest['date']}", "",
         f"**{latest.get('note', '')}**  ",
         f"commit `{latest.get('commit')}`" + (" ⚠️ dirty" if latest.get("dirty") else "")
         + f" · {latest['rig']['instances']}×{latest['rig']['seconds']:.0f}s game, "
         f"seed {latest['rig']['seed']}", "",
         "| Map | nav | nodes | reach ok/gated/no-path | solo COLLECT | solo pickups | "
         "duel ITEM | duel pickups | duel frags | duel deaths |",
         "|---|---|---:|---|---:|---:|---:|---:|---:|---:|"]
    for m in latest["maps"]:
        r = latest["maps"][m]
        s, d = r.get("solo") or {}, r.get("duel") or {}
        rc = s.get("reach") or {}
        L.append(f"| {m} | {r.get('nav_source', '—')} | {fmt(s.get('nav_nodes'))} | "
                 f"{rc.get('ok', '—')}/{rc.get('gated', '—')}/{rc.get('no_path', '—')} | "
                 f"{fmt(s.get('item_completion'), True)} | {fmt(s.get('pickups'))} | "
                 f"{fmt(d.get('item_completion'), True)} | {fmt(d.get('pickups'))} | "
                 f"{fmt(d.get('frags'))} | {fmt(d.get('deaths'))} |")
    L += ["", "## Solo pickups over time", "",
          "| Date | Note | " + " | ".join(latest["maps"]) + " | total |",
          "|---|---|" + "|".join(["---:"] * (len(latest["maps"]) + 1)) + "|"]
    for rec in history:
        cells, tot = [], 0
        for m in latest["maps"]:
            v = ((rec.get("maps", {}).get(m) or {}).get("solo") or {}).get("pickups")
            cells.append(fmt(v))
            tot += v or 0
        note = (rec.get("note", "") or "").replace("|", "/")[:40]
        L.append(f"| {rec['date'][:10]} | {note} | " + " | ".join(cells) + f" | {tot} |")
    with open(POOL_MD, "w") as f:
        f.write("\n".join(L) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--note", default="")
    ap.add_argument("--maps", default=",".join(POOL))
    ap.add_argument("--instances", type=int, default=16)
    ap.add_argument("--seconds", type=float, default=90)
    ap.add_argument("--seed", type=int, default=700)
    ap.add_argument("--cvar", nargs=2, action="append", metavar=("NAME", "VALUE"))
    ap.add_argument("--regen", action="store_true", help="regrow gen_nav navs even if cached")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--report-only", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if args.report_only:
        with open(HISTORY) as f:
            render([json.loads(l) for l in f if l.strip()])
        log(f"regenerated -> {POOL_MD}")
        return 0
    if not args.no_build and rig_platform.build_game(ENGINE) != 0:
        sys.exit("FAIL: game build failed")

    maps = [m for m in args.maps.split(",") if m]
    results, t0 = {}, time.time()
    for m in maps:
        bsp = os.path.join(ENGINE, "baseq2", "maps", f"{m}.bsp")
        if not os.path.isfile(bsp):
            log(f"{m}: not installed loose in engine/baseq2/maps (pak maps are fine) -- trying anyway")
        nav, source = ensure_nav(m, args)
        results[m] = {"nav_source": source}
        if not nav:
            continue
        stage(m, nav)
        for rig, bots in (("solo", 1), ("duel", 2)):
            log(f"{m} [{rig}] ({args.instances}x{args.seconds:.0f}s, {bots} bot(s), seed {args.seed})...")
            r = run_rig(m, bots, args)
            results[m][rig] = r
            if r:
                log(f"  {m} [{rig}]: {'COLLECT' if bots == 1 else 'ITEM'} "
                    f"{fmt(r['item_completion'], True)}  pickups {r['pickups']}  "
                    f"frags {r['frags']}  deaths {r['deaths']}  nav {r['nav_nodes']}  reach {r['reach']}")
            else:
                log(f"  {m} [{rig}]: NO TELEMETRY")
    shutil.rmtree(os.path.join(ENGINE, POOL_MOD), ignore_errors=True)

    rec = {"date": time.strftime("%Y-%m-%dT%H:%M:%S"),
           "commit": bm.git("rev-parse", "--short", "HEAD"),
           "dirty": bool(bm.git("status", "--porcelain")),
           "note": args.note,
           "rig": {"instances": args.instances, "seconds": args.seconds, "seed": args.seed,
                   "cvars": [["sv_fps", "40"]] + (args.cvar or [])},
           "maps": results, "wall_seconds": round(time.time() - t0, 1)}
    if args.dry_run:
        print(json.dumps(rec, indent=1))
        return 0
    with open(HISTORY, "a") as f:
        f.write(json.dumps(rec) + "\n")
    with open(HISTORY) as f:
        render([json.loads(l) for l in f if l.strip()])
    log(f"appended -> {HISTORY}; rendered -> {POOL_MD}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

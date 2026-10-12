#!/usr/bin/env python3
"""
Lever liveness audit: does each default-ON lever actually change anything?

For every map, run the shipped-default rig once, then once per lever with that
lever switched off, all on the SAME seed.  The rig is deterministic, so:

  * behaviour md5 unchanged  -> the lever's code path never altered a single
                                tick or event on that map: INERT there
  * md5 changed              -> live; the pickup/frag/death deltas say how much

No DLL instrumentation needed -- the md5 gate is the probe.  Some levers are
expected to be inert on stock maps (teleporter/jump-pad seeding, trains); an
unexpectedly inert lever is a dead-path suspect (cf. Item_RespawnEta, whose
timing path never fired for the project's whole life).

The deltas are ONE seed bank and one short rig: they flag size and direction,
they are not an A/B.  Use run_parallel/benchmark at 16 seeds before concluding
anything about a lever's value.

Usage:
    py tools/lever_audit.py [--maps q2dm1,q2dm3] [--levers bot_lift,bot_flee]
                            [--instances 8] [--seconds 60] [--bots 5] [--seed 700]
                            [--json baselines/lever_audit.json]
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import benchmark as bm      # compute_metrics

# Default-ON behaviour levers and the value that switches each one off.
# Parameters (bot_skill, bot_budgetcap, bot_forwardspeed, bot_stucktime,
# bot_pursuitcost) and pure logging (bot_reachlog) are deliberately absent.
# bot_aimerr/aimfire/aimreact/aimturn are excluded too: they are bot_aimtest
# sweep multipliers (1 = identity, applied only to the harness's even ids), so
# they are inert by design -- the first audit flagged them on all 8 maps.
LEVERS = [
    "bot_aimflick", "bot_aimprec",
    "bot_aimsmooth", "bot_aimtexture", "bot_ammoneed",
    "bot_blastertransit", "bot_claim", "bot_commit", "bot_decisive", "bot_fidget",
    "bot_flee", "bot_fov", "bot_gaze", "bot_gazelife", "bot_goalbudget",
    "bot_hazard", "bot_hop", "bot_itemfail", "bot_jumppad", "bot_ladder",
    "bot_lead", "bot_lift", "bot_liftcommit", "bot_losfinal", "bot_navlearn",
    "bot_pathcost", "bot_playbook", "bot_reroute", "bot_reroutemid",
    "bot_rollout", "bot_strafejump", "bot_swim", "bot_teleport", "bot_train",
    "bot_turnrate", "bot_watersight", "bot_wpnneed", "bot_wpntactic",
]
MAPS = ["q2dm1", "q2dm2", "q2dm3", "q2dm4", "q2dm5", "q2dm6", "q2dm7", "q2dm8"]


def run(mapname, args, extra=()):
    cmd = [sys.executable, os.path.join(HERE, "run_parallel.py"), "--repro", "--fastsim",
           "--map", mapname, "--instances", str(args.instances),
           "--seconds", str(args.seconds), "--bots", str(args.bots),
           "--seed", str(args.seed), "--cvar", "sv_fps", "40", "--no-analyze"]
    for name, val in extra:
        cmd += ["--cvar", name, val]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    m = re.search(r"-> (\S+\.jsonl)", out)
    if not m:
        return None
    path = m.group(1)
    h = hashlib.md5()
    with open(path, "rb") as f:
        for line in f:
            if b'"type": "tick"' in line or b'"type": "event"' in line:
                h.update(line)
    met = bm.compute_metrics(path) or {}
    os.remove(path)
    return {"md5": h.hexdigest()[:8], "pickups": met.get("pickups"),
            "frags": met.get("frags"), "deaths": met.get("deaths")}


def pct(a, b):
    if a is None or b in (None, 0):
        return "   n/a"
    return f"{100.0 * (a - b) / b:+6.1f}%"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--maps", default=",".join(MAPS))
    ap.add_argument("--levers", default=",".join(LEVERS))
    ap.add_argument("--instances", type=int, default=8)
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--bots", type=int, default=5)
    ap.add_argument("--seed", type=int, default=700)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()
    maps = [m for m in args.maps.split(",") if m]
    levers = [l for l in args.levers.split(",") if l]

    # run_parallel's workers get disposable nav copies, so no run (including
    # the bot_navlearn row) can perturb the live graphs or the next run.
    results = {}
    for m in maps:
        base = run(m, args)
        if base is None:
            print(f"[audit] {m}: baseline produced no telemetry -- skipped")
            continue
        results[m] = {"baseline": base, "levers": {}}
        print(f"\n[audit] {m}  baseline md5 {base['md5']}  pickups {base['pickups']}  "
              f"frags {base['frags']}  deaths {base['deaths']}")
        for lv in levers:
            r = run(m, args, extra=[(lv, "0")])
            if r is None:
                print(f"  {lv:20s} NO TELEMETRY")
                continue
            r["inert"] = r["md5"] == base["md5"]
            results[m]["levers"][lv] = r
            tag = "INERT" if r["inert"] else "live "
            print(f"  {lv:20s} {tag}  pickups {pct(r['pickups'], base['pickups'])}  "
                  f"frags {pct(r['frags'], base['frags'])}  deaths {pct(r['deaths'], base['deaths'])}"
                  "   (lever OFF vs default)")

    # cross-map summary: levers inert everywhere are the dead-path suspects
    print("\n[audit] summary (maps where the lever changed behaviour):")
    for lv in levers:
        live = [m for m in results if not results[m]["levers"].get(lv, {}).get("inert", True)]
        flag = "  <-- inert on every map" if not live else ""
        print(f"  {lv:20s} {len(live)}/{len(results)}  {','.join(live)}{flag}")

    if args.json:
        with open(args.json, "w") as f:
            json.dump({"rig": vars(args), "results": results}, f, indent=1)
        print(f"\n[audit] -> {args.json}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""q2red_navs -- bake frozen navs for a q2red map pool (the q2red counterpart
of gen_nav.py, which drives q2repro).

It runs the q2red dedicated server with ozbot (dist/ozbot_x64.so) and the
server's own game module, so navs are learned against the rules and geometry
the bots will actually play.  Per map, gen_nav's method ("mature to peak"):

  - grow the graph from scratch with --mature-bots bots, --interval game
    seconds at a time, up to --max-seconds (each checkpoint continues the last);
  - after each checkpoint, probe a FROZEN copy with one bot for
    --probe-seconds on --probe-seeds seeds and count pickups;
  - keep the checkpoint with the most pickups (item completion breaks ties),
    stop after --patience checkpoints without a new peak, and write it
    stamped FROZEN so a live server's learner cannot grow it past the peak.

Maps that already have a nav in the game dir are skipped unless --force
(so curated navs, e.g. baselines/nav_shipped for q2dm1-8, are kept).

q2red runs faster than real time under `timescale`, so a whole pool takes
minutes:  py tools/q2red_navs.py --rig <server dir> --maps pool.txt -j 6

Keep the rig's path SHORT (e.g. /tmp/q2bb): q2red builds map paths in a
128-byte buffer, so under a long directory every map with a longer name
fails with "Can't find maps/<name>.bsp" (after a Com_sprintf overflow).

The rig is a q2red server directory: q2server, Q2Game.kpf, the PlayFab
library, and <game>/ holding game_x64.so, ozbot_x64.so, botnames.txt and any
custom maps.  Each parallel slot gets its own working directory whose game
dir is a symlink to the rig's, so every nav lands in <rig>/<game>/nav/.
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import glob
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nav_edit as ne  # noqa: E402

LINKS = ["q2server", "Q2Game.kpf", "PlayFabMultiplayerWin_reconstruction.so"]


def map_file(mapname):
    # bot_log.c:Bot_MapFileName - subfolder maps ("q64/dm3") flatten to "q64_dm3"
    return mapname.replace("/", "_").replace("\\", "_")


class Slot:
    def __init__(self, rig, game, index, base_port):
        self.rig, self.game, self.index = rig, game, index
        self.port = base_port + index
        self.dir = os.path.join(rig, ".q2red_navs", str(index))
        os.makedirs(self.dir, exist_ok=True)
        for name in LINKS + [game]:
            dst = os.path.join(self.dir, name)
            if os.path.islink(dst) or os.path.exists(dst):
                continue
            os.symlink(os.path.join(rig, name), dst)

    def run(self, mapname, seconds, bots, seed, args, telemetry=False):
        cvars = [
            ("dedicated", "1"), ("logfile", "0"), ("port", str(self.port)),
            ("deathmatch", "1"), ("maxclients", str(max(bots + 1, 4))),
            ("fraglimit", "0"), ("timelimit", "0"), ("timescale", str(args.timescale)),
            ("sv_bot_brain", "ozbot"), ("bot_count", str(bots)), ("bot_seed", str(seed)),
            ("bot_quitafter", str(seconds)), ("bot_telemetry", "1" if telemetry else "0"),
            ("bot_skill", str(args.skill)), ("sv_status_file", "0"),
        ]
        if args.game != "baseq2":
            cvars.append(("game", args.game))
        for extra in args.set or []:
            k, v = extra.split("=", 1)
            cvars.append((k, v))
        # Settings go in a cfg: q2red allows only a few dozen command-line
        # arguments (MAX_NUM_ARGVS), and it reads cfgs from the real
        # executable's game dir, which is the rig's.
        cfg = f"q2red_navs_{self.index}.cfg"
        with open(os.path.join(self.rig, self.game, cfg), "w") as f:
            f.writelines(f'set {k} "{v}"\n' for k, v in cvars)
            # the map goes in the cfg too: on the command line a name with a
            # dash ("mm-rage") is split at it
            f.write(f'map "{mapname}"\n')
        argv = ["./q2server", "-stdout", "+set", "dedicated", "1", "+exec", cfg]
        env = dict(os.environ, LD_LIBRARY_PATH=self.dir)
        wall = seconds / max(args.timescale, 1) * 4 + 60
        with open(os.path.join(self.dir, "console.log"), "w") as out:
            try:
                return subprocess.run(argv, cwd=self.dir, env=env, stdout=out,
                                      stderr=subprocess.STDOUT, timeout=wall).returncode
            except subprocess.TimeoutExpired:
                return "timeout"


def telemetry_metrics(path):
    pickups = goals = 0
    with open(path) as f:
        for line in f:
            try:
                r = json.loads(line)
            except ValueError:
                continue
            ev = r.get("event")
            if ev == "pickup":
                pickups += 1
            elif ev == "goal_item":
                goals += 1
    return pickups, goals


def bake(mapname, slot, args, log):
    gamedir = os.path.join(args.rig, args.game)
    nav = os.path.join(gamedir, "nav", map_file(mapname) + ".nav")
    logs = os.path.join(gamedir, "logs")
    os.makedirs(os.path.dirname(nav), exist_ok=True)
    if os.path.exists(nav) and not args.force:
        log(f"{mapname}: kept existing nav")
        return {"map": mapname, "result": "kept"}
    if os.path.exists(nav):
        os.remove(nav)

    best = None
    curve = []
    stale = 0
    mature_total = 0.0
    work = nav + ".work"
    while mature_total + args.interval <= args.max_seconds + 1e-6:
        if os.path.exists(work):
            shutil.copy2(work, nav)
        code = slot.run(mapname, args.interval, args.mature_bots, args.seed + len(curve), args)
        mature_total += args.interval
        if code != 0 or not os.path.exists(nav):
            log(f"{mapname}: maturation run failed ({code}); see {slot.dir}/console.log")
            return {"map": mapname, "result": "failed", "code": str(code)}
        shutil.copy2(nav, work)
        nodes, flags = ne.load(work)

        ne.save(nav, nodes, flags | ne.NAVHDR_FROZEN)
        pickups = goals = 0
        for k in range(args.probe_seeds):
            for old in glob.glob(os.path.join(logs, map_file(mapname) + "_*.jsonl")):
                os.remove(old)
            slot.run(mapname, args.probe_seconds, 1, args.seed + 1000 + k, args, telemetry=True)
            for path in glob.glob(os.path.join(logs, map_file(mapname) + "_*.jsonl")):
                p, g = telemetry_metrics(path)
                pickups += p
                goals += g
                os.remove(path)
        collect = round(100.0 * pickups / goals, 1) if goals else 0.0
        cp = {"mature_seconds": mature_total, "nodes": len(nodes), "pickups": pickups,
              "collect": collect}
        curve.append(cp)
        log(f"{mapname}: {mature_total:.0f}s  nodes={len(nodes)}  pickups={pickups}  collect={collect}%")
        score = (pickups, collect)
        if best is None or score > best["score"]:
            best = {"score": score, "cp": cp, "nodes": nodes, "flags": flags}
            stale = 0
        else:
            stale += 1
            if stale >= args.patience:
                break

    if os.path.exists(work):
        os.remove(work)
    if not best or not best["nodes"]:
        if os.path.exists(nav):
            os.remove(nav)
        log(f"{mapname}: no nodes learned")
        return {"map": mapname, "result": "no-nodes", "curve": curve}
    ne.save(nav, best["nodes"], best["flags"] | ne.NAVHDR_FROZEN)
    log(f"{mapname}: wrote {nav} ({best['cp']['nodes']} nodes, {best['cp']['pickups']} pickups, FROZEN)")
    return {"map": mapname, "result": "baked", "peak": best["cp"], "curve": curve}


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--rig", required=True, help="q2red server directory")
    ap.add_argument("--game", default="baseq2", help="game dir inside the rig (default baseq2)")
    ap.add_argument("--maps", required=True, help="file with one map per line, or a comma list")
    ap.add_argument("-j", "--parallel", type=int, default=4)
    ap.add_argument("--timescale", type=float, default=24.0)
    ap.add_argument("--interval", type=float, default=90.0)
    ap.add_argument("--max-seconds", type=float, default=720.0)
    ap.add_argument("--patience", type=int, default=2)
    ap.add_argument("--mature-bots", type=int, default=11)
    ap.add_argument("--probe-seconds", type=float, default=90.0)
    ap.add_argument("--probe-seeds", type=int, default=4)
    ap.add_argument("--seed", type=int, default=700)
    ap.add_argument("--skill", type=float, default=0.5)
    ap.add_argument("--base-port", type=int, default=28100)
    ap.add_argument("--force", action="store_true", help="rebake maps that already have a nav")
    ap.add_argument("--set", action="append", help="extra server cvar, name=value (repeatable)")
    ap.add_argument("--report", help="write a JSON report here")
    args = ap.parse_args(argv[1:])
    args.rig = os.path.abspath(args.rig)

    if os.path.exists(args.maps):
        maps = [l.strip() for l in open(args.maps) if l.strip() and not l.startswith("#")]
    else:
        maps = [m for m in args.maps.split(",") if m]

    slots = [Slot(args.rig, args.game, i, args.base_port) for i in range(args.parallel)]
    free = list(slots)
    started = time.time()

    def log(msg):
        print(f"[{time.time() - started:6.0f}s] {msg}", flush=True)

    results = []
    with cf.ThreadPoolExecutor(max_workers=args.parallel) as pool:
        def job(mapname):
            slot = free.pop()
            try:
                return bake(mapname, slot, args, log)
            finally:
                free.append(slot)
        for r in pool.map(job, maps):
            results.append(r)

    if args.report:
        with open(args.report, "w") as f:
            json.dump(results, f, indent=1)
    bad = [r["map"] for r in results if r["result"] not in ("baked", "kept")]
    log(f"done: {sum(r['result'] == 'baked' for r in results)} baked, "
        f"{sum(r['result'] == 'kept' for r in results)} kept, {len(bad)} failed {bad}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

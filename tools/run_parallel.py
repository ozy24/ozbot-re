#!/usr/bin/env python3
"""
ozbot parallel sim harness.

The prebuilt engine runs the sim at ~real time (timescale caps at ~2x -- see
the ozbot-sim-timescale memory): SV_Frame() runs at most one game frame per
loop iteration and the loop sleeps between ticks. Two ways past that:

  * --fastsim (the fast path, ~hundreds of x): use the patched dedicated
    engine `engine/q2proded_fast.exe` (built from q2pro/ with `fastsim`, a
    cvar that skips the sleep and injects one tick per loop iteration, making
    the sim CPU-bound). With --fastsim, --seconds means *game* seconds: each
    server quits itself via the DLL's bot_quitafter cvar, so every seed
    simulates exactly the same game time no matter how loaded the CPU is.
    Build the engine with ozbot/build_engine.bat if the exe is missing.
  * without --fastsim: real-time servers, --seconds is wall time, and
    parallelism is the only lever (each instance is ~95% idle).

This script:
  1. Creates one isolated worker gamedir per instance (engine/ozbot_wN), each
     seeded with the deployed gamex86.dll and a *copy* of the matured <map>.nav.
     Isolation is required: the bot writes logs/<map>_<HHMMSS>.jsonl (instances
     starting in the same second would clobber one file) and autosaves the nav
     graph every ~30s (concurrent writers would corrupt the shared graph). With
     per-worker dirs the canonical engine/ozbot/ is never touched.
  2. Launches N `q2pro.exe +set dedicated 1` servers, each on its own net_port.
  3. Waits the requested wall-clock duration, then stops them.
  4. Merges every worker's telemetry into one JSONL (offsetting bot ids per
     instance so per-bot stats stay honest) under engine/ozbot/logs/.
  5. Runs tools/analyze.py on the merged log (unless --no-analyze).

Stdlib only. Example:
    python tools/run_parallel.py --instances 8 --seconds 90 --bots 5
"""

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))   # <repo>/tools
REPO = os.path.dirname(HERE)                         # the bot repo
ROOT = os.path.dirname(REPO)                         # umbrella root (holds engine/, demos/)
DEFAULT_ENGINE = os.path.join(ROOT, "engine")
BOT_ID_STRIDE = 1000   # worker i's bot b -> id i*STRIDE + b (keeps ids distinct)


def log(msg):
    print(f"[parallel] {msg}", flush=True)


def setup_worker(engine, src_mod, worker_mod, mapname, dll_name):
    """Create engine/<worker_mod> seeded with the DLL and a nav copy."""
    wdir = os.path.join(engine, worker_mod)
    # Start clean so a stale log from a previous run can't be mistaken for ours.
    shutil.rmtree(wdir, ignore_errors=True)
    os.makedirs(os.path.join(wdir, "logs"), exist_ok=True)
    os.makedirs(os.path.join(wdir, "nav"), exist_ok=True)

    dll = os.path.join(engine, src_mod, dll_name)
    if not os.path.isfile(dll):
        sys.exit(f"FAIL: {dll} not found -- run build.bat + deploy.bat first.")
    shutil.copy2(dll, os.path.join(wdir, dll_name))

    nav = os.path.join(engine, src_mod, "nav", f"{mapname}.nav")
    if os.path.isfile(nav):
        shutil.copy2(nav, os.path.join(wdir, "nav", f"{mapname}.nav"))
    else:
        log(f"warning: {nav} not found -- worker '{worker_mod}' starts cold.")

    # playbooks (ozbot-re Phase R4): recorded maneuvers the DLL loads at map
    # start; absent file = feature inert
    pbk = os.path.join(engine, src_mod, "playbooks", f"{mapname}.pbk")
    if os.path.isfile(pbk):
        os.makedirs(os.path.join(wdir, "playbooks"), exist_ok=True)
        shutil.copy2(pbk, os.path.join(wdir, "playbooks", f"{mapname}.pbk"))
    return wdir


def launch(engine, exe, worker_mod, port, seed, args):
    cmd = [exe]
    if args.repro:
        # never auto-detect Steam/GoG installs or an OneDrive homedir --
        # workers must stay hermetic inside the engine dir
        cmd += ["+set", "com_rerelease", "-1"]
    cmd += [
        "+set", "dedicated", "1",
        "+set", "game", worker_mod,
        "+set", "deathmatch", "1",
        "+set", "maxclients", "16",
        "+set", "bot_count", str(args.bots),
        "+set", "bot_skill", str(args.skill),
        "+set", "net_port", str(port),
        "+set", "timescale", str(args.timescale),
    ]
    if args.fastsim:
        # CPU-bound turbo loop + self-quit after --seconds of *game* time
        cmd += ["+set", "fastsim", "1",
                "+set", "bot_quitafter", str(args.seconds)]
    # With an explicit seed the whole run is reproducible; without one the DLL
    # auto-seeds from pid+time so the instances are independent samples.
    if seed is not None:
        cmd += ["+set", "bot_seed", str(seed)]
    for name, val in (args.cvar or []):
        cmd += ["+set", name, val]
    cmd += ["+map", args.map]
    flags = 0
    if os.name == "nt":
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    return subprocess.Popen(
        cmd, cwd=engine,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        creationflags=flags,
    )


def stop(procs):
    for p in procs:
        if p.poll() is None:
            p.terminate()
    deadline = time.time() + 5
    for p in procs:
        try:
            p.wait(timeout=max(0.0, deadline - time.time()))
        except subprocess.TimeoutExpired:
            p.kill()


def merge_logs(engine, worker_mods, mapname, out_path):
    """Concatenate worker telemetry, offsetting bot ids so they stay distinct.

    analyze.py groups by bot id and sums event counters for the aggregate
    metrics (ITEM completion, pickups, frags, coverage, hotspots). Distinct ids
    keep per-bot path length / K/D honest too; the global sums are unchanged.
    """
    ticks = events = workers_with_data = bad = 0
    with open(out_path, "w", encoding="utf-8") as out:
        for i, mod in enumerate(worker_mods, start=1):
            offset = i * BOT_ID_STRIDE
            files = sorted(glob.glob(
                os.path.join(engine, mod, "logs", f"{mapname}_*.jsonl")))
            if not files:
                log(f"warning: worker '{mod}' produced no telemetry.")
                continue
            workers_with_data += 1
            for fpath in files:
                with open(fpath, "r", encoding="utf-8") as fp:
                    for line in fp:
                        line = line.strip()
                        if not line:
                            continue
                        try:
                            rec = json.loads(line)
                        except json.JSONDecodeError:
                            bad += 1   # truncated last line from an abrupt stop
                            continue
                        if "bot" in rec:
                            rec["bot"] += offset
                        out.write(json.dumps(rec) + "\n")
                        if rec.get("type") == "tick":
                            ticks += 1
                        elif rec.get("type") == "event":
                            events += 1
    return ticks, events, workers_with_data, bad


def main(argv):
    ap = argparse.ArgumentParser(description="Run N headless ozbot sims in parallel and aggregate.")
    ap.add_argument("--instances", type=int, default=4, help="number of parallel servers (default 4)")
    ap.add_argument("--seconds", type=float, default=60.0,
                    help="run length (default 60): wall-clock normally, *game* seconds with --fastsim")
    ap.add_argument("--fastsim", action="store_true",
                    help="use engine/q2proded_fast.exe with the fastsim cvar: CPU-bound sim "
                         "(~hundreds of x realtime); --seconds becomes simulated game seconds")
    ap.add_argument("--map", default="q2dm1")
    ap.add_argument("--bots", type=int, default=5, help="bots per server (default 5)")
    ap.add_argument("--skill", type=float, default=0.5)
    ap.add_argument("--timescale", type=float, default=1.0, help="engine time accel (caps ~2x; default 1)")
    ap.add_argument("--engine", default=DEFAULT_ENGINE, help="engine dir (default repo/engine)")
    ap.add_argument("--repro", action="store_true",
                    help="ozbot-re rig: q2repro engine (q2reproded.exe, x64 gamex86_64.dll, "
                         "default --mod ozbotre, hermetic com_rerelease -1)")
    ap.add_argument("--mod", default=None, help="canonical gamedir to seed DLL + nav from "
                    "(default: ozbot, or ozbotre with --repro)")
    ap.add_argument("--base-port", type=int, default=27910)
    ap.add_argument("--seed", type=int, default=None,
                    help="base rng seed for a reproducible run (worker i gets seed+i); "
                         "omit for independent pid-seeded runs")
    ap.add_argument("--cvar", nargs=2, metavar=("NAME", "VALUE"), action="append",
                    help="extra cvar to +set on every server (repeatable), e.g. --cvar bot_skill 0.8")
    ap.add_argument("--keep", action="store_true", help="keep worker gamedirs after the run")
    ap.add_argument("--no-analyze", action="store_true", help="merge logs but skip analyze.py")
    args = ap.parse_args(argv[1:])

    engine = os.path.abspath(args.engine)
    if args.mod is None:
        args.mod = "ozbotre" if args.repro else "ozbot"
    if args.repro:
        # q2reproded carries the fastsim cvar itself (built by ozbot-re/build_engine.bat);
        # without --fastsim it just runs in real time like any dedicated server.
        exe = os.path.join(engine, "q2reproded.exe")
        if not os.path.isfile(exe):
            sys.exit(f"FAIL: {exe} not found -- build it with ozbot-re/build_engine.bat")
    elif args.fastsim:
        exe = os.path.join(engine, "q2proded_fast.exe")
        if not os.path.isfile(exe):
            sys.exit(f"FAIL: {exe} not found -- build it with ozbot/build_engine.bat")
    else:
        exe = os.path.join(engine, "q2pro.exe")
        if not os.path.isfile(exe):
            exe = os.path.join(engine, "quake2.exe")
        if not os.path.isfile(exe):
            sys.exit(f"FAIL: no q2pro.exe/quake2.exe in {engine}")
    dll_name = "gamex86_64.dll" if args.repro else "gamex86.dll"
    if args.instances < 1:
        sys.exit("FAIL: --instances must be >= 1")

    worker_mods = [f"{args.mod}_w{i}" for i in range(1, args.instances + 1)]

    log(f"{args.instances} servers x {args.bots} bots on {args.map} for {args.seconds:.0f}s "
        + ("of GAME time (fastsim)" if args.fastsim else f"wall (timescale {args.timescale})"))
    for mod in worker_mods:
        setup_worker(engine, args.mod, mod, args.map, dll_name)

    procs = []
    try:
        for i, mod in enumerate(worker_mods):
            seed = None if args.seed is None else args.seed + i
            procs.append(launch(engine, exe, mod, args.base_port + i, seed, args))
        log(f"launched {len(procs)} servers (ports {args.base_port}..{args.base_port + len(procs) - 1}); simulating...")
        started = time.time()
        # fastsim servers quit themselves at --seconds of game time; the wall
        # deadline is only a hung-server failsafe there.  Non-fastsim servers
        # run forever, so the deadline IS the run length.
        end = started + args.seconds + (120.0 if args.fastsim else 0.0)
        while time.time() < end:
            if all(p.poll() is not None for p in procs):
                if args.fastsim:
                    log(f"all servers finished in {time.time() - started:.1f}s wall.")
                else:
                    log("warning: all servers exited early.")
                break
            time.sleep(min(2.0, max(0.0, end - time.time())))
        else:
            if args.fastsim:
                log("warning: wall failsafe hit -- some fastsim servers never finished.")
    finally:
        stop(procs)
    dead = [i + 1 for i, p in enumerate(procs) if p.returncode not in (None, 0)]
    log(f"servers stopped." + (f" (nonzero exit from #{dead})" if dead else ""))

    stamp = time.strftime("%Y%m%d_%H%M%S")
    out_path = os.path.join(engine, args.mod, "logs", f"parallel_{args.map}_{stamp}.jsonl")
    ticks, events, nw, bad = merge_logs(engine, worker_mods, args.map, out_path)
    log(f"merged {ticks} ticks + {events} events from {nw}/{args.instances} servers "
        f"-> {out_path}" + (f"  ({bad} truncated lines skipped)" if bad else ""))

    if not args.keep:
        for mod in worker_mods:
            shutil.rmtree(os.path.join(engine, mod), ignore_errors=True)

    if ticks == 0:
        sys.exit("FAIL: no telemetry produced.")

    if not args.no_analyze:
        log("running analyze.py on the merged log:\n")
        rc = subprocess.call([sys.executable, os.path.join(HERE, "analyze.py"), out_path])
        return rc
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

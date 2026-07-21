#!/usr/bin/env python3
"""gen_nav -- generate a frozen, right-sized nav graph for ANY map.

Standalone: this script is frozen into `gen_nav.exe` (PyInstaller, see
tools/gen_nav.spec + build_gen_nav.bat) so server admins can bake navs for
their map rotation from a UI without a Python install.  It drives the mod's
own dedicated engine (q2reproded.exe + gamex86_64.dll), the same processes the
benchmark launches.

WHY "mature-to-peak" instead of "mature as long as possible": letting a graph
grow unbounded REGRESSES item collection (the documented node-count sweet spot,
KNOWN_ISSUES.md -- q2dm1 23%->15% past ~330 nodes).  Coverage is not the quality
signal.  So we grow the graph in checkpoints, measure solo item-collection at
each, and emit the PEAK graph -- each map gets its own right-sized nav.  The
output is stamped FROZEN (ONAV header flag) so a live server's always-on learner
can't grow it back past the peak.

CLI (UI-friendly): positional <map>; progress on stdout; a machine-readable
JSON report to --report; a clear exit code (0 ok, 2 no telemetry / engine
missing, 3 no nodes learned).

  gen_nav.exe q2dm3 --out engine/ozbotre/nav/q2dm3.nav --report q2dm3.json
  gen_nav.exe --maps q2dm1,q2dm3,city1        # batch a rotation
"""
from __future__ import annotations
import argparse
import json
import os
import shutil
import sys
import tempfile
import types

# ---- locate our sibling tools + the mod engine, frozen (exe) or source -----
if getattr(sys, "frozen", False):
    HERE = os.path.dirname(os.path.abspath(sys.executable))
    BUNDLE = getattr(sys, "_MEIPASS", HERE)
    sys.path.insert(0, BUNDLE)
else:
    HERE = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, HERE)

import run_parallel as rp          # noqa: E402
import benchmark as bench          # noqa: E402  (reuse mature/probe/metric plumbing)
import nav_edit as ne              # noqa: E402  (freeze stamp)

REPO = os.path.dirname(HERE)

# When frozen, benchmark.py's own __file__-derived REPO points INTO the
# PyInstaller bundle (a temp extract dir), so its dist-DLL / baselines paths are
# wrong.  Repoint them at the real install.  (In source mode these are identical
# to benchmark's own values, so this is a harmless no-op.)
bench.REPO = REPO
bench.PINNED_NAV = os.path.join(REPO, "baselines", "nav")
bench.PINNED_NAV_SHIPPED = os.path.join(REPO, "baselines", "nav_shipped")
bench.PINNED_PBK = os.path.join(REPO, "baselines", "playbooks")
bench.DEFAULT_ENGINE = os.path.join(REPO, "engine")


def resolve_dll(engine, mod, explicit):
    """Find gamex86_64.dll: --dll, then the dev dist/, then the installed
    gamedir (where a shipped mod actually keeps it)."""
    for c in (explicit,
              os.path.join(REPO, "dist", bench.DLL_NAME),
              os.path.join(engine, mod, bench.DLL_NAME),
              os.path.join(engine, "ozbotre", bench.DLL_NAME)):
        if c and os.path.isfile(c):
            return os.path.abspath(c)
    return None


def stage_repo(dll, engine, mod):
    """benchmark.mature_map/prepare_bench_mod copy the DLL from <REPO>/dist and
    playbooks from <REPO>/baselines/playbooks.  On an end-user install neither
    exists, so build a throwaway staging root with the resolved DLL and repoint
    benchmark at it.  Playbooks come from the live gamedir if present."""
    stage = tempfile.mkdtemp(prefix="gennav_stage_")
    os.makedirs(os.path.join(stage, "dist"), exist_ok=True)
    shutil.copy2(dll, os.path.join(stage, "dist", bench.DLL_NAME))
    bench.REPO = stage
    gpb = os.path.join(engine, mod, "playbooks")
    bench.PINNED_PBK = gpb if os.path.isdir(gpb) else os.path.join(stage, "playbooks")
    return stage


def find_engine(explicit):
    """Locate the dir holding q2reproded.exe + gamedir.  Search: --engine, then
    a few spots relative to this exe/script and the CWD, so a UI can invoke us
    with just a map name."""
    cands = []
    if explicit:
        cands.append(explicit)
    exedir = os.path.dirname(os.path.abspath(sys.executable if getattr(sys, "frozen", False) else __file__))
    cands += [
        os.environ.get("OZBOT_ENGINE", ""),
        os.path.join(REPO, "engine"),
        os.path.join(exedir, "engine"),
        os.path.join(exedir, "..", "engine"),
        os.path.join(os.getcwd(), "engine"),
    ]
    for c in cands:
        if c and os.path.isfile(os.path.join(c, "q2reproded.exe")):
            return os.path.abspath(c)
    return None


def _probe_args(seconds, instances, seed, skill, base_port):
    # run_map reads exactly these fields; bot_navlearn 0 keeps the measured
    # graph fixed during the probe (we want THIS checkpoint's quality, not a
    # graph that grows while we score it).
    return types.SimpleNamespace(
        instances=instances, seconds=seconds, skill=skill, seed=seed,
        base_port=base_port, cvar=[["bot_navlearn", "0"]],
    )


def probe(engine, exe, mapname, nav_file, seconds, instances, seed, skill, base_port):
    """Solo (1-bot, no combat) item-collection score for a specific nav file.
    Returns (metrics_dict_or_None, reach_records)."""
    tmp = tempfile.mkdtemp(prefix="gennav_probe_")
    try:
        shutil.copy2(nav_file, os.path.join(tmp, f"{mapname}.nav"))
        bench.prepare_bench_mod(engine, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    metrics = bench.run_map(engine, exe, mapname,
                            _probe_args(seconds, instances, seed, skill, base_port), bots=1)
    reach = []
    out_path = os.path.join(engine, bench.BENCH_MOD, "logs", f"bench_{mapname}.jsonl")
    if os.path.isfile(out_path):
        _t, _e, reach, _b = bench.rp_load(out_path)
    return metrics, reach


def gated_items(reach):
    """Collapse the reach oracle's per-item verdicts to the items the graph
    can't cleanly complete -- the human's to-do list (add a playbook / edit)."""
    best = {}
    rank = {"no_path": 3, "no_goal_node": 2, "gated": 1, "ok": 0}
    for r in reach:
        code = r.get("code", "ok")
        if code == "ok":
            continue
        it = r.get("item", "?")
        if rank.get(code, 0) >= rank.get(best.get(it, "ok"), 0):
            best[it] = code
    return [{"item": k, "code": v} for k, v in sorted(best.items())]


def mature_to_peak(engine, exe, mapname, args):
    """Grow the graph in increasing cold-maturation durations, probe solo
    collection at each, and return the peak checkpoint + full curve."""
    checkpoints = []
    best = None
    stale = 0
    scratch = tempfile.mkdtemp(prefix=f"gennav_{mapname}_")
    try:
        secs = args.interval
        step = 0
        while secs <= args.max_seconds + 1e-6:
            step += 1
            print(f"[gen_nav] {mapname} checkpoint {step}: maturing {secs:.0f}s "
                  f"({args.mature_bots} bots, cold, seed {args.seed})...", flush=True)
            staged, nodes = bench.mature_map(engine, exe, mapname, secs,
                                             args.mature_bots, args.seed)
            if not staged or not nodes:
                print(f"[gen_nav]   no nodes learned at {secs:.0f}s; growing more", flush=True)
                secs += args.interval
                continue
            snap = os.path.join(scratch, f"{mapname}.{step}.nav")
            shutil.copy2(staged, snap)

            metrics, reach = probe(engine, exe, mapname, snap, args.probe_seconds,
                                   args.probe_instances, args.seed, args.skill, args.base_port)
            collect = (metrics or {}).get("item_completion")
            pickups = (metrics or {}).get("pickups")
            cp = {"step": step, "mature_seconds": secs, "nodes": nodes,
                  "collect": collect, "pickups": pickups}
            checkpoints.append(cp)
            print(f"[gen_nav]   nodes={nodes} COLLECT={collect}% pickups={pickups}", flush=True)

            # Fitness = absolute PICKUPS, not COLLECT% (pickups/attempts).  COLLECT%
            # is a confounded ratio: a sparse graph that only reaches 3 easy items
            # and gets all 3 scores 100%, beating a graph that reaches 20 and gets
            # 17 (85%) -- i.e. it rewards LOW coverage.  Absolute pickups rewards
            # both coverage and completion, and still catches the node-count sweet
            # spot (past it, routing noise lowers throughput -> pickups decline).
            # COLLECT% is kept as the tie-breaker + reported for context.
            score = (pickups or 0, collect or 0)
            if best is None or score > best["score"]:
                best = {"score": score, "cp": cp, "snap": snap, "reach": reach}
                stale = 0
            else:
                stale += 1
                if stale >= args.patience:
                    print(f"[gen_nav]   pickups past peak for {stale} checkpoints; stopping", flush=True)
                    break
            secs += args.interval
    finally:
        keep = best["snap"] if best else None
        # leave `keep` in scratch until the caller copies it out
        if not best:
            shutil.rmtree(scratch, ignore_errors=True)
    return best, checkpoints, scratch


def generate_one(engine, exe, mapname, args):
    print(f"[gen_nav] === {mapname} ===", flush=True)
    best, checkpoints, scratch = mature_to_peak(engine, exe, mapname, args)
    if not best:
        print(f"[gen_nav] {mapname}: FAILED -- no nodes learned", flush=True)
        return {"map": mapname, "ok": False, "reason": "no-nodes",
                "checkpoints": checkpoints}, 3
    out = args.out or os.path.join(engine, bench.CANON_MOD, "nav", f"{mapname}.nav")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    shutil.copy2(best["snap"], out)
    shutil.rmtree(scratch, ignore_errors=True)
    # stamp FROZEN so a live server's learner can't grow it past this peak
    nodes, flags = ne.load(out)
    ne.save(out, nodes, flags | ne.NAVHDR_FROZEN)

    gated = gated_items(best["reach"])
    report = {
        "map": mapname, "ok": True, "out": out, "frozen": True,
        "peak": best["cp"], "checkpoints": checkpoints,
        "gated_items": gated,
        "note": "gated_items need a playbook or a surgical nav_edit.py link.",
    }
    cp = best["cp"]
    print(f"[gen_nav] {mapname}: PEAK pickups={cp['pickups']} nodes={cp['nodes']} "
          f"COLLECT={cp['collect']}% @ {cp['mature_seconds']:.0f}s -> {out} (frozen)", flush=True)
    if gated:
        print(f"[gen_nav]   {len(gated)} item(s) still gated/no_path: "
              + ", ".join(g["item"] for g in gated[:12]), flush=True)
    return report, 0


def main(argv):
    ap = argparse.ArgumentParser(
        description="Generate a frozen, mature-to-peak nav for a map (ships as gen_nav.exe).")
    ap.add_argument("map", nargs="?", help="map name (e.g. q2dm3, city1)")
    ap.add_argument("--maps", help="comma-separated batch of maps (overrides positional)")
    ap.add_argument("--out", help="output .nav path (default <gamedir>/nav/<map>.nav; "
                                  "ignored in --maps batch, which always writes the gamedir)")
    ap.add_argument("--engine", help="dir holding q2reproded.exe + gamedir (auto-detected)")
    ap.add_argument("--mod", default=bench.CANON_MOD, help="gamedir name (default ozbotre)")
    ap.add_argument("--dll", help="path to gamex86_64.dll (auto: dist/ or the gamedir)")
    ap.add_argument("--report", help="write a JSON report here (batch: one array)")
    ap.add_argument("--interval", type=float, default=90.0,
                    help="maturation seconds added per checkpoint (default 90)")
    ap.add_argument("--max-seconds", type=float, default=720.0,
                    help="cap on cold-maturation seconds (default 720)")
    ap.add_argument("--patience", type=int, default=2,
                    help="stop after COLLECT fails to beat the peak this many checkpoints")
    ap.add_argument("--mature-bots", type=int, default=11)
    ap.add_argument("--probe-seconds", type=float, default=90.0)
    ap.add_argument("--probe-instances", type=int, default=16)
    ap.add_argument("--seed", type=int, default=700)
    ap.add_argument("--skill", type=float, default=0.5)
    ap.add_argument("--base-port", type=int, default=27910)
    a = ap.parse_args(argv[1:])

    maps = [m.strip() for m in a.maps.split(",")] if a.maps else ([a.map] if a.map else [])
    if not maps:
        ap.error("give a map name or --maps")

    engine = find_engine(a.engine)
    if not engine:
        print("[gen_nav] FAIL: could not find q2reproded.exe (pass --engine)", file=sys.stderr)
        return 2
    exe = os.path.join(engine, "q2reproded.exe")
    dll = resolve_dll(engine, a.mod, a.dll)
    if not dll:
        print(f"[gen_nav] FAIL: {bench.DLL_NAME} not found in dist/ or the gamedir "
              f"(pass --dll)", file=sys.stderr)
        return 2
    stage = stage_repo(dll, engine, a.mod)

    reports, rc = [], 0
    try:
        for m in maps:
            rep, code = generate_one(engine, exe, m, a)
            reports.append(rep)
            rc = rc or code
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    if a.report:
        with open(a.report, "w", encoding="utf-8") as f:
            json.dump(reports if a.maps else reports[0], f, indent=2)
        print(f"[gen_nav] report -> {a.report}", flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))

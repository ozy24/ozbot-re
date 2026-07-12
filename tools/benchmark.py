#!/usr/bin/env python3
"""ozbot-re cross-map benchmark + stats tracker (40Hz / x64 / q2repro rig).

Runs the standard fastsim repro rig on each q2dm map from a PINNED nav baseline
(baselines/nav/<map>.nav) with a fixed seed, extracts the headline metrics
(ITEM completion, pickups, frags, deaths, K/D, nav nodes) per map, appends a
dated snapshot to baselines/benchmark_history.jsonl, and regenerates STATS.md.

This is the 40Hz sibling of ozbot/tools/benchmark.py.  Differences baked in:
  * --repro engine: q2reproded.exe + gamex86_64.dll + the ozbotre gamedir,
    com_rerelease -1 (hermetic).  sv_fps 40 is forced on every server (the 40Hz
    body is mandatory -- see CLAUDE.md); it is added to the recorded cvars.
  * Playbooks (recorded maneuvers the DLL loads per map, e.g. q2dm1's MH-jump)
    are pinned in baselines/playbooks/ and frozen into the bench + maturation
    gamedirs, so a playbook-gated route is in play during both measurement and
    maturation.
  * --mature writes ONLY to baselines/nav/ -- it does NOT overwrite the live,
    deliberately-seeded engine/ozbotre/nav/ graphs (unlike the ozbot tool).

Why a pinned nav baseline + fixed seed?  So two snapshots differ ONLY because
the CODE changed -- not because a nav matured further from live play, and not
because of RNG.  Record what changed with --note.  The rig freezes the built
DLL + pinned navs/playbooks into engine/ozbotre_bench (an isolated source
gamedir), so a concurrent play.bat can't perturb a run.

Every map is measured against TWO nav baselines each snapshot:
  * shipped -- the real bot on its hand-seeded navs (baselines/nav_shipped/, a
    frozen snapshot of the curated engine/ozbotre/nav/; cold-filled for the maps
    that never had a curated nav).  This is where the tuned bot actually stands.
  * cold    -- the same build self-learning from a scratch-matured graph
    (baselines/nav/, --mature).  This isolates nav-learning quality.
The gap between them is what the manual nav curation is worth.

Stdlib only.  Examples:
    py tools/benchmark.py --note "baseline: campaign-2026-07"
    py tools/benchmark.py --maps q2dm1,q2dm5 --note "bot_foo tweak"
    py tools/benchmark.py --pin-shipped    # refresh the shipped baseline from the live curated navs
    py tools/benchmark.py --mature         # regrow the cold baseline (all 8, from scratch)
    py tools/benchmark.py --report-only    # regenerate STATS.md from history, no sim
"""

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import time
import types

HERE = os.path.dirname(os.path.abspath(__file__))   # <repo>/tools
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import run_parallel as rp                            # reuse the sim harness plumbing

DEFAULT_ENGINE = os.path.join(REPO, "engine")
PINNED_NAV = os.path.join(REPO, "baselines", "nav")            # cold-normalized nav baseline
PINNED_NAV_SHIPPED = os.path.join(REPO, "baselines", "nav_shipped")  # hand-seeded (real) navs
PINNED_PBK = os.path.join(REPO, "baselines", "playbooks")      # frozen playbooks

# Every map is measured against BOTH baselines so a snapshot shows the real
# shipped bot AND what it self-learns from a cold graph.  "shipped" first =
# the headline column.
VARIANTS = [("shipped", PINNED_NAV_SHIPPED), ("cold", PINNED_NAV)]
HISTORY = os.path.join(REPO, "baselines", "benchmark_history.jsonl")
STATS_MD = os.path.join(REPO, "STATS.md")
BENCH_MOD = "ozbotre_bench"                          # frozen source gamedir
MATURE_SRC = "ozbotre_mature_src"                    # cold-nav source gamedir
CANON_MOD = "ozbotre"                                # the live gamedir
DLL_NAME = "gamex86_64.dll"
EXE_NAME = "q2reproded.exe"
REQUIRED_CVARS = [["sv_fps", "40"]]                  # 40Hz body -- mandatory on this rig

ALL_MAPS = ["q2dm1", "q2dm2", "q2dm3", "q2dm4", "q2dm5", "q2dm6", "q2dm7", "q2dm8"]


def log(msg):
    print(f"[bench] {msg}", flush=True)


# ----------------------------------------------------------------------------
# metric extraction -- mirrors tools/analyze.py exactly so the numbers match
# ----------------------------------------------------------------------------
def compute_metrics(path):
    from collections import defaultdict
    ticks, events, _reach, _bad = rp_load(path)
    if not ticks:
        return None

    by_bot = defaultdict(list)
    for t in ticks:
        by_bot[t["bot"]].append(t)
    ev = defaultdict(lambda: defaultdict(int))
    for e in events:
        if "bot" in e:
            ev[e["bot"]][e.get("event", "?")] += 1

    tot_pick = tot_frag = tot_death = 0
    for bot, rows in by_bot.items():
        frags = 0
        for r in rows:
            frags = max(frags, r.get("score", 0))
        tot_frag += frags
        tot_pick += ev[bot].get("pickup", 0)
        tot_death += ev[bot].get("death", 0)

    item_attempts = sum(ev[b].get("goal_item", 0) for b in ev)
    goal_attempts = sum(ev[b].get("goal_item", 0) + ev[b].get("goal", 0) for b in ev)
    reaches = sum(ev[b].get("reach", 0) for b in ev)

    pickups_by_item = defaultdict(int)
    for e in events:
        if e.get("event") == "pickup":
            pickups_by_item[e.get("item", "?")] += 1

    nav_nodes = [t.get("nav_nodes") for t in ticks if t.get("nav_nodes") is not None]
    times = [t["t"] for t in ticks]

    return {
        "bots": len(by_bot),
        "ticks": len(ticks),
        "timespan": round(max(times) - min(times), 1) if times else 0.0,
        "nav_nodes": max(nav_nodes) if nav_nodes else None,
        "pickups": tot_pick,
        "item_attempts": item_attempts,
        "item_completion": round(100 * tot_pick / item_attempts, 1) if item_attempts else None,
        "goal_attempts": goal_attempts,
        "goal_success": tot_pick + reaches,
        "goal_success_pct": round(100 * (tot_pick + reaches) / goal_attempts, 1) if goal_attempts else None,
        "frags": tot_frag,
        "deaths": tot_death,
        "kd": round(tot_frag / tot_death, 2) if tot_death else None,
        "pickups_by_item": dict(sorted(pickups_by_item.items(), key=lambda kv: -kv[1])),
    }


def rp_load(path):
    """analyze.py's loader (kept local so we don't import its argparse main)."""
    ticks, events, reach, bad = [], [], [], 0
    with open(path, "r", encoding="utf-8") as fp:
        for line in fp:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                bad += 1
                continue
            t = rec.get("type")
            if t == "tick":
                ticks.append(rec)
            elif t == "event":
                events.append(rec)
            elif t == "reach":
                reach.append(rec)
    return ticks, events, reach, bad


# ----------------------------------------------------------------------------
# sim orchestration -- freeze DLL+navs+playbooks into a source gamedir
# ----------------------------------------------------------------------------
def _seed_playbooks(mod_dir):
    """Copy the pinned playbooks into <mod>/playbooks so setup_worker can pick
    the per-map .pbk up (recorded maneuvers the DLL loads at map start)."""
    n = 0
    if os.path.isdir(PINNED_PBK):
        dst = os.path.join(mod_dir, "playbooks")
        os.makedirs(dst, exist_ok=True)
        for f in glob.glob(os.path.join(PINNED_PBK, "*.pbk")):
            shutil.copy2(f, os.path.join(dst, os.path.basename(f)))
            n += 1
    return n


def prepare_bench_mod(engine, nav_dir):
    """engine/ozbotre_bench = freshly built DLL + navs from nav_dir + playbooks."""
    bench = os.path.join(engine, BENCH_MOD)
    shutil.rmtree(bench, ignore_errors=True)
    os.makedirs(os.path.join(bench, "nav"), exist_ok=True)

    dll = os.path.join(REPO, "dist", DLL_NAME)
    if not os.path.isfile(dll):
        sys.exit(f"FAIL: {dll} not found -- run build.bat first (or drop --no-build).")
    shutil.copy2(dll, os.path.join(bench, DLL_NAME))

    navs = 0
    for f in glob.glob(os.path.join(nav_dir, "*.nav")):
        shutil.copy2(f, os.path.join(bench, "nav", os.path.basename(f)))
        navs += 1
    pbks = _seed_playbooks(bench)
    log(f"froze DLL + {navs} navs ({os.path.basename(nav_dir)}) + {pbks} playbooks into engine/{BENCH_MOD}")
    return bench


def pin_shipped(engine):
    """baselines/nav_shipped = the live hand-seeded engine navs, with cold-baseline
    fallback for maps that never had a curated nav (q2dm4/q2dm6)."""
    os.makedirs(PINNED_NAV_SHIPPED, exist_ok=True)
    live, filled = 0, 0
    for m in ALL_MAPS:
        live_nav = os.path.join(engine, CANON_MOD, "nav", f"{m}.nav")
        cold_nav = os.path.join(PINNED_NAV, f"{m}.nav")
        dst = os.path.join(PINNED_NAV_SHIPPED, f"{m}.nav")
        if os.path.isfile(live_nav):
            shutil.copy2(live_nav, dst); live += 1
        elif os.path.isfile(cold_nav):
            shutil.copy2(cold_nav, dst); filled += 1
    log(f"pinned shipped navs -> baselines/nav_shipped/ ({live} hand-seeded, "
        f"{filled} cold-filled where no curated nav exists)")


def nav_node_count(path):
    if not os.path.isfile(path):
        return None
    import struct
    with open(path, "rb") as fp:
        head = fp.read(12)
    if len(head) < 12:
        return None
    _magic, _ver, n = struct.unpack("<iii", head)
    return n


def mature_map(engine, exe, mapname, seconds, bots, seed):
    """Grow ONE coherent nav graph from COLD in a single fastsim server (with the
    playbook capability available) and return the saved .nav path (+ node count)."""
    src = os.path.join(engine, MATURE_SRC)   # DLL + playbooks, NO navs -> cold graph
    os.makedirs(src, exist_ok=True)
    shutil.copy2(os.path.join(REPO, "dist", DLL_NAME), os.path.join(src, DLL_NAME))
    _seed_playbooks(src)

    worker = "ozbotre_mature_w"
    rp.setup_worker(engine, MATURE_SRC, worker, mapname, DLL_NAME)  # warns "cold" -- intended
    sim_args = types.SimpleNamespace(
        repro=True, fastsim=True, bots=bots, skill=0.5,
        timescale=1.0, seconds=seconds, cvar=list(REQUIRED_CVARS), map=mapname,
    )
    proc = rp.launch(engine, exe, worker, 27950, seed, sim_args)
    end = time.time() + seconds + 300.0
    while time.time() < end and proc.poll() is None:
        time.sleep(1.0)
    rp.stop([proc])

    nav = os.path.join(engine, worker, "nav", f"{mapname}.nav")
    n = nav_node_count(nav)
    staged = None
    if n:
        staged = os.path.join(engine, MATURE_SRC, f"{mapname}.nav.staged")
        shutil.copy2(nav, staged)
    shutil.rmtree(os.path.join(engine, worker), ignore_errors=True)
    return staged, n


def run_map(engine, exe, mapname, args):
    """Run one map's rig and return its metrics dict (or None on no telemetry)."""
    worker_mods = [f"{BENCH_MOD}_w{i}" for i in range(1, args.instances + 1)]
    sim_args = types.SimpleNamespace(
        repro=True, fastsim=True, bots=args.bots, skill=args.skill,
        timescale=1.0, seconds=args.seconds, map=mapname,
        cvar=list(REQUIRED_CVARS) + (args.cvar or []),
    )
    for mod in worker_mods:
        rp.setup_worker(engine, BENCH_MOD, mod, mapname, DLL_NAME)

    procs = []
    try:
        for i, mod in enumerate(worker_mods):
            procs.append(rp.launch(engine, exe, mod, args.base_port + i, args.seed + i, sim_args))
        started = time.time()
        end = started + args.seconds + 120.0
        while time.time() < end:
            if all(p.poll() is not None for p in procs):
                break
            time.sleep(min(2.0, max(0.0, end - time.time())))
    finally:
        rp.stop(procs)

    logs_dir = os.path.join(engine, BENCH_MOD, "logs")
    os.makedirs(logs_dir, exist_ok=True)
    out_path = os.path.join(logs_dir, f"bench_{mapname}.jsonl")
    ticks, events, nw, bad = rp.merge_logs(engine, worker_mods, mapname, out_path)
    for mod in worker_mods:
        shutil.rmtree(os.path.join(engine, mod), ignore_errors=True)
    if ticks == 0:
        log(f"  {mapname}: WARNING no telemetry ({nw}/{args.instances} workers)")
        return None
    return compute_metrics(out_path)


# ----------------------------------------------------------------------------
# history + report
# ----------------------------------------------------------------------------
def git(*cmd):
    try:
        return subprocess.check_output(["git", *cmd], cwd=REPO,
                                       stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return ""


def load_history():
    if not os.path.isfile(HISTORY):
        return []
    out = []
    with open(HISTORY, "r", encoding="utf-8") as fp:
        for line in fp:
            line = line.strip()
            if line:
                out.append(json.loads(line))
    return out


def append_history(record):
    with open(HISTORY, "a", encoding="utf-8") as fp:
        fp.write(json.dumps(record) + "\n")


def fmt_pct(v):
    return "—" if v is None else f"{v:.0f}%"


def write_stats_md(history):
    maps = ALL_MAPS
    lines = []
    lines.append("# ozbot-re — cross-map benchmark stats (40Hz rig)")
    lines.append("")
    lines.append("_Auto-generated by `tools/benchmark.py`; do not edit by hand._ "
                 "Run `py tools/benchmark.py --note \"<what changed>\"` to add a snapshot.")
    lines.append("")
    lines.append("Each snapshot runs the standard fastsim **repro** rig (40Hz / `sv_fps 40`, "
                 "fixed seed, pinned playbooks) on every map against **two** nav baselines, so "
                 "differences between rows isolate **code** changes. Headline metric is "
                 "**ITEM completion** (pickups ÷ item-goal attempts):")
    lines.append("")
    lines.append("- **shipped** — the real bot on its hand-seeded navs (`baselines/nav_shipped/`, "
                 "a frozen snapshot of the curated `engine/ozbotre/nav/`; cold-filled for q2dm4/q2dm6 "
                 "which never had a curated nav). This is *where the tuned bot actually stands*.")
    lines.append("- **cold** — the same build self-learning from a scratch-matured graph "
                 "(`baselines/nav/`, `--mature`). This isolates *nav-learning* quality, with no "
                 "hand-curation advantage.")
    lines.append("")
    lines.append("The gap between them is what the manual nav curation is worth "
                 "(≈15 ITEM points on q2dm1). Note 40Hz ITEM% runs structurally below the 10Hz "
                 "figure — the higher kill intensity interrupts more item runs with death.")
    lines.append("")

    if not history:
        lines.append("_No snapshots recorded yet._")
        _write(STATS_MD, "\n".join(lines) + "\n")
        return

    latest = history[-1]
    rig = latest.get("rig", {})
    lines.append(f"## Current state — {latest.get('date','?')}")
    lines.append("")
    lines.append(f"**{latest.get('note','(no note)')}**  ")
    lines.append(f"commit `{latest.get('commit','?')}`"
                 + ("  ⚠️ working tree dirty" if latest.get("dirty") else "")
                 + f" · rig: {rig.get('instances')}×{rig.get('seconds')}s game, "
                 f"{rig.get('bots')} bots, skill {rig.get('skill')}, seed {rig.get('seed')}")
    lines.append("")
    lines.append("**ITEM shipped** = the real bot on its hand-seeded navs; "
                 "**ITEM cold** = the same build self-learning from a cold graph. "
                 "Pickups/Attempts/Frags/Deaths/K/D/Nav are from the *shipped* run.")
    lines.append("")
    lines.append("| Map | ITEM shipped | ITEM cold | Pickups | Attempts | Frags | Deaths | K/D | Nav |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    lm = latest.get("maps", {})
    for m in maps:
        s = _variant(lm, m, "shipped")
        c = _variant(lm, m, "cold")
        base = s or c   # detail metrics from the shipped run (fall back to cold)
        if not base:
            lines.append(f"| {m} | — | — | — | — | — | — | — | — |")
            continue
        lines.append(f"| {m} | {fmt_pct(s.get('item_completion') if s else None)} | "
                     f"{fmt_pct(c.get('item_completion') if c else None)} | "
                     f"{base.get('pickups','—')} | {base.get('item_attempts','—')} | "
                     f"{base.get('frags','—')} | {base.get('deaths','—')} | "
                     f"{base.get('kd') if base.get('kd') is not None else '—'} | {base.get('nav_nodes','—')} |")
    lines.append(f"| **mean** | **{fmt_pct(_mean_item(latest,'shipped'))}** | "
                 f"**{fmt_pct(_mean_item(latest,'cold'))}** | | | | | | |")
    lines.append("")

    # per-item breakdown for the latest snapshot (shipped run)
    lines.append("<details><summary>Per-item pickups (latest, shipped)</summary>")
    lines.append("")
    for m in maps:
        d = _variant(lm, m, "shipped")
        if not d or not d.get("pickups_by_item"):
            continue
        items = ", ".join(f"{n}× {it}" for it, n in d["pickups_by_item"].items())
        lines.append(f"- **{m}**: {items}")
    lines.append("")
    lines.append("</details>")
    lines.append("")

    header = "| Date | Note | " + " | ".join(maps) + " | mean |"
    sep = "|---|---|" + "|".join(["---:"] * (len(maps) + 1)) + "|"

    def _item_trend(variant):
        out = [header, sep]
        for rec in history:
            rm = rec.get("maps", {})
            cells = [fmt_pct(_variant(rm, m, variant).get("item_completion"))
                     if _variant(rm, m, variant) else "—" for m in maps]
            note = (rec.get("note", "") or "").replace("|", "/")
            if len(note) > 40:
                note = note[:37] + "..."
            out.append(f"| {rec.get('date','?')[:10]} | {note} | " + " | ".join(cells)
                       + f" | {fmt_pct(_mean_item(rec, variant))} |")
        return out

    # trend: ITEM% per map across every snapshot -- shipped is the headline
    lines.append("## ITEM completion over time (shipped)")
    lines.append("")
    lines += _item_trend("shipped")
    lines.append("")
    lines.append("<details><summary>ITEM completion over time (cold — from-scratch nav learning)</summary>")
    lines.append("")
    lines += _item_trend("cold")
    lines.append("")
    lines.append("</details>")
    lines.append("")

    # frags trend (shipped run; combat activity is symmetric self-play, informational)
    lines.append("<details><summary>Total frags over time (shipped; activity, not skill)</summary>")
    lines.append("")
    lines.append(header.replace(" mean ", " total "))
    lines.append(sep)
    for rec in history:
        rm = rec.get("maps", {})
        cells = [str(_variant(rm, m, "shipped").get("frags", "—"))
                 if _variant(rm, m, "shipped") else "—" for m in maps]
        tot = sum(_variant(rm, m, "shipped").get("frags", 0)
                  for m in maps if _variant(rm, m, "shipped"))
        note = (rec.get("note", "") or "").replace("|", "/")
        if len(note) > 40:
            note = note[:37] + "..."
        lines.append(f"| {rec.get('date','?')[:10]} | {note} | " + " | ".join(cells) + f" | {tot} |")
    lines.append("")
    lines.append("</details>")
    lines.append("")

    _write(STATS_MD, "\n".join(lines) + "\n")


def _variant(rec_maps, m, variant):
    d = rec_maps.get(m)
    return d.get(variant) if d else None


def _mean_item(rec, variant):
    vals = []
    for m in rec.get("maps", {}):
        met = _variant(rec.get("maps", {}), m, variant)
        if met and met.get("item_completion") is not None:
            vals.append(met["item_completion"])
    return round(sum(vals) / len(vals), 1) if vals else None


def _write(path, text):
    with open(path, "w", encoding="utf-8") as fp:
        fp.write(text)


# ----------------------------------------------------------------------------
def main(argv):
    ap = argparse.ArgumentParser(description="ozbot-re cross-map benchmark + stats tracker (40Hz).")
    ap.add_argument("--note", default="", help="what changed since the last snapshot (recorded in history)")
    ap.add_argument("--maps", default=",".join(ALL_MAPS),
                    help="comma-separated maps to run (default all 8)")
    ap.add_argument("--instances", type=int, default=16, help="parallel servers (default 16, the repro rig)")
    ap.add_argument("--seconds", type=float, default=90.0, help="game seconds per map (fastsim)")
    ap.add_argument("--bots", type=int, default=5)
    ap.add_argument("--skill", type=float, default=0.5)
    ap.add_argument("--seed", type=int, default=700, help="base seed (worker i gets seed+i); fixed for reproducibility")
    ap.add_argument("--base-port", type=int, default=27910)
    ap.add_argument("--cvar", nargs=2, metavar=("NAME", "VALUE"), action="append",
                    help="extra cvar passed to every server (repeatable); sv_fps 40 is always forced")
    ap.add_argument("--engine", default=DEFAULT_ENGINE)
    ap.add_argument("--no-build", action="store_true", help="skip build.bat (use existing dist/gamex86_64.dll)")
    ap.add_argument("--pin-shipped", action="store_true",
                    help="(re)snapshot the live engine/ozbotre/nav/*.nav into baselines/nav_shipped/ "
                         "(cold-filled for maps without a curated nav), then exit")
    ap.add_argument("--mature", action="store_true",
                    help="regenerate the normalized nav baseline: grow each map's graph from COLD "
                         "with one identical rig, write it to baselines/nav/ (live navs untouched), then exit")
    ap.add_argument("--mature-seconds", type=float, default=720.0,
                    help="game seconds of cold maturation per map (default 720)")
    ap.add_argument("--mature-bots", type=int, default=11,
                    help="bot population during maturation (default 11)")
    ap.add_argument("--report-only", action="store_true", help="regenerate STATS.md from history and exit")
    ap.add_argument("--dry-run", action="store_true", help="run sims + print metrics but do NOT append to history")
    args = ap.parse_args(argv[1:])

    if args.report_only:
        write_stats_md(load_history())
        log(f"regenerated {STATS_MD}")
        return 0

    engine = os.path.abspath(args.engine)

    if args.pin_shipped:
        pin_shipped(engine)
        return 0

    if not args.no_build:
        log("building (build.bat)...")
        rc = subprocess.call(["cmd", "/c", os.path.join(REPO, "build.bat")], cwd=REPO)
        if rc != 0:
            sys.exit(f"FAIL: build.bat returned {rc}")

    exe = os.path.join(engine, EXE_NAME)
    if not os.path.isfile(exe):
        sys.exit(f"FAIL: {exe} not found -- build it with build_engine.bat")

    maps = [m.strip() for m in args.maps.split(",") if m.strip()]

    if args.mature:
        os.makedirs(PINNED_NAV, exist_ok=True)
        log(f"maturing {len(maps)} maps from COLD "
            f"({args.mature_bots} bots x {args.mature_seconds:.0f}s game, seed {args.seed}, sv_fps 40)")
        for m in maps:
            staged, n = mature_map(engine, exe, m, args.mature_seconds, args.mature_bots, args.seed)
            if not staged:
                log(f"  {m}: WARNING produced no nav -- skipped")
                continue
            shutil.copy2(staged, os.path.join(PINNED_NAV, f"{m}.nav"))
            log(f"  {m}: {n} nodes -> baselines/nav/")
        shutil.rmtree(os.path.join(engine, MATURE_SRC), ignore_errors=True)
        log("normalized nav baseline rebuilt (live engine navs untouched). "
            "Now run `py tools/benchmark.py --note ...`")
        return 0

    results = {m: {} for m in maps}
    t0 = time.time()
    for vname, navdir in VARIANTS:
        if not glob.glob(os.path.join(navdir, "*.nav")):
            log(f"WARNING: no navs in {navdir} -- skipping '{vname}' variant "
                + ("(run --pin-shipped first)" if vname == "shipped" else "(run --mature first)"))
            continue
        prepare_bench_mod(engine, navdir)
        for m in maps:
            log(f"running {m} [{vname}] ({args.instances}x{args.seconds:.0f}s game, seed {args.seed})...")
            met = run_map(engine, exe, m, args)
            if met:
                log(f"  {m} [{vname}]: ITEM {fmt_pct(met.get('item_completion'))}  "
                    f"pickups {met['pickups']}  frags {met['frags']}  nav {met['nav_nodes']}")
            results[m][vname] = met
        shutil.rmtree(os.path.join(engine, BENCH_MOD), ignore_errors=True)

    record = {
        "date": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "commit": git("rev-parse", "--short", "HEAD"),
        "branch": git("rev-parse", "--abbrev-ref", "HEAD"),
        "dirty": bool(git("status", "--porcelain")),
        "note": args.note,
        "rig": {"instances": args.instances, "seconds": args.seconds, "bots": args.bots,
                "skill": args.skill, "seed": args.seed, "repro": True,
                "cvars": REQUIRED_CVARS + (args.cvar or [])},
        "maps": results,
        "wall_seconds": round(time.time() - t0, 1),
    }

    if args.dry_run:
        log("dry-run: not writing history. Record would be:")
        print(json.dumps(record, indent=2))
        return 0

    append_history(record)
    write_stats_md(load_history())
    log(f"appended snapshot -> {HISTORY}")
    log(f"regenerated -> {STATS_MD}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

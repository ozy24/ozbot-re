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

# Dual axis: nav baseline (shipped vs cold) × rig (solo vs deathmatch).  Each map
# is measured on all four.  A variant = (name, nav_dir, bots).
#   solo (1 bot, combat impossible) -> nav-QUALITY headline: isolates last-leg
#     item-collection with no combat interruption / respawn-teleport / contention.
#   deathmatch (5 bots) -> the integration/product metric (combat + contention).
#   shipped -> the real bot on hand-seeded navs; cold -> self-learned graph.
# Solo variants lead = the headline columns.  Combat is off purely by bot_count 1.
DM_BOTS = 5
SOLO_BOTS = 1
PARITY_BOTS = 6   # skill-parity axis: even ids (0,2,4) high skill vs odd (1,3,5) low; 3v3 balanced
V_SHIPPED_SOLO, V_COLD_SOLO = "shipped-solo", "cold-solo"
V_SHIPPED_DM,   V_COLD_DM   = "shipped-dm",   "cold-dm"
SOLO_VARIANTS = [V_SHIPPED_SOLO, V_COLD_SOLO]
DM_VARIANTS   = [V_SHIPPED_DM,   V_COLD_DM]
VARIANTS = [
    (V_SHIPPED_SOLO, PINNED_NAV_SHIPPED, SOLO_BOTS),
    (V_COLD_SOLO,    PINNED_NAV,         SOLO_BOTS),
    (V_SHIPPED_DM,   PINNED_NAV_SHIPPED, DM_BOTS),
    (V_COLD_DM,      PINNED_NAV,         DM_BOTS),
]
# legacy shim: pre-solo history rows used "shipped"/"cold" (both 5-bot deathmatch)
_LEGACY_VARIANT = {V_SHIPPED_DM: "shipped", V_COLD_DM: "cold"}
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
    kills_even = kills_odd = deaths_even = deaths_odd = 0
    for bot, rows in by_bot.items():
        frags = 0
        for r in rows:
            frags = max(frags, r.get("score", 0))
        tot_frag += frags
        tot_pick += ev[bot].get("pickup", 0)
        deaths = ev[bot].get("death", 0)
        tot_death += deaths
        # skill-parity axis: cohort by id parity (bot_skilltest gives even ids high
        # skill, odd ids low; run_parallel's BOT_ID_STRIDE=1000 keeps id%2 across merge)
        if bot % 2 == 0:
            kills_even += frags
            deaths_even += deaths
        else:
            kills_odd += frags
            deaths_odd += deaths

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
        "kills_even": kills_even,
        "kills_odd": kills_odd,
        "deaths_even": deaths_even,
        "deaths_odd": deaths_odd,
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


def run_map(engine, exe, mapname, args, bots, extra_cvars=None):
    """Run one map's rig at the given bot count and return its metrics dict
    (or None on no telemetry).  extra_cvars appends per-call cvars (e.g. the
    skill-parity axis' bot_skilltest) on top of the required + user cvars."""
    worker_mods = [f"{BENCH_MOD}_w{i}" for i in range(1, args.instances + 1)]
    sim_args = types.SimpleNamespace(
        repro=True, fastsim=True, bots=bots, skill=args.skill,
        timescale=1.0, seconds=args.seconds, map=mapname,
        cvar=list(REQUIRED_CVARS) + (args.cvar or []) + (extra_cvars or []),
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


def run_parity_map(engine, exe, mapname, args):
    """Skill-parity kill-share axis for ONE map.  Runs a treatment
    (bot_skilltest 1 -> even ids high skill, odd ids low) and a PAIRED control
    (bot_skilltest 0, all bots equal, SAME seed), then reports the SHIFT
    treatment_share - control_share to cancel the id-position bias baked into
    the raw even/odd split (~0.81 at low seed counts, washing to 0.5 at 16).

    run_map merges all args.instances workers (seed+0..) into one JSONL before
    compute_metrics, so kills_even/kills_odd are already pooled across the map's
    seeds."""
    t = run_map(engine, exe, mapname, args, PARITY_BOTS, extra_cvars=[["bot_skilltest", "1"]])
    c = run_map(engine, exe, mapname, args, PARITY_BOTS, extra_cvars=[["bot_skilltest", "0"]])
    if not t or not c:
        return None

    def _share(even, odd):
        tot = even + odd
        return (even / tot) if tot else None

    treatment_share = _share(t["kills_even"], t["kills_odd"])
    control_share = _share(c["kills_even"], c["kills_odd"])
    shift = (treatment_share - control_share
             if treatment_share is not None and control_share is not None else None)
    return {
        "kills_hi": t["kills_even"],
        "kills_lo": t["kills_odd"],
        "treatment_share": treatment_share,
        "ctrl_even": c["kills_even"],
        "ctrl_odd": c["kills_odd"],
        "control_share": control_share,
        "shift": shift,
    }


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
    lines.append("Each snapshot runs the fastsim **repro** rig (40Hz / `sv_fps 40`, fixed seed, "
                 "pinned playbooks) on every map across a **2×2** of nav baseline × rig, so rows "
                 "isolate **code** changes. Two axes:")
    lines.append("")
    lines.append("- **nav baseline** — **shipped** = the real bot on its hand-seeded navs "
                 "(`baselines/nav_shipped/`, frozen from the curated `engine/ozbotre/nav/`; "
                 "cold-filled for q2dm4/q2dm6). **cold** = the same build self-learning from a "
                 "scratch-matured graph (`baselines/nav/`, `--mature`) — isolates nav-learning.")
    lines.append("- **rig** — **solo** (1 bot, combat impossible) is the **nav-quality headline**: "
                 "it isolates last-leg item-collection with no combat interruption, respawn-teleport, "
                 "or contention. **deathmatch** (5 bots) is the integration/product metric — the only "
                 "place combat + contention wins (e.g. `bot_claim`) appear. Collection runs lower in "
                 "deathmatch because combat interrupts routes; that's expected, not a nav regression.")
    lines.append("")
    lines.append("So the four columns are **shipped-solo / cold-solo** (nav quality) and "
                 "**shipped-dm / cold-dm** (integration). Note 40Hz deathmatch ITEM% runs "
                 "structurally below the 10Hz figure — higher kill intensity interrupts more runs.")
    lines.append("")

    if not history:
        lines.append("_No snapshots recorded yet._")
        _write(STATS_MD, "\n".join(lines) + "\n")
        return

    latest = history[-1]
    rig = latest.get("rig", {})
    srig = latest.get("solo_rig", {})
    lm = latest.get("maps", {})
    lines.append(f"## Current state — {latest.get('date','?')}")
    lines.append("")
    lines.append(f"**{latest.get('note','(no note)')}**  ")
    lines.append(f"commit `{latest.get('commit','?')}`"
                 + ("  ⚠️ working tree dirty" if latest.get("dirty") else "")
                 + f" · rig: {rig.get('instances')}×{rig.get('seconds')}s game, "
                 f"solo {srig.get('bots', 1)} bot / deathmatch {rig.get('bots')} bots, "
                 f"skill {rig.get('skill')}, seed {rig.get('seed')}")
    lines.append("")

    # --- headline: solo nav-collection (shipped vs cold navs, no combat) ---
    lines.append("### Solo nav-collection (nav quality — 1 bot, no combat)")
    lines.append("")
    lines.append("| Map | shipped-solo | cold-solo | Pickups | Attempts | Nav |")
    lines.append("|---|---:|---:|---:|---:|---:|")
    for m in maps:
        s = _variant(lm, m, V_SHIPPED_SOLO)
        c = _variant(lm, m, V_COLD_SOLO)
        base = s or c
        if not base:
            lines.append(f"| {m} | — | — | — | — | — |")
            continue
        lines.append(f"| {m} | {fmt_pct(s.get('item_completion') if s else None)} | "
                     f"{fmt_pct(c.get('item_completion') if c else None)} | "
                     f"{base.get('pickups','—')} | {base.get('item_attempts','—')} | "
                     f"{base.get('nav_nodes','—')} |")
    lines.append(f"| **mean** | **{fmt_pct(_mean_item(latest, V_SHIPPED_SOLO))}** | "
                 f"**{fmt_pct(_mean_item(latest, V_COLD_SOLO))}** | | | |")
    lines.append("")

    # --- integration: deathmatch (shipped vs cold navs, 5 bots) ---
    lines.append("### Deathmatch (integration — 5 bots, combat + contention)")
    lines.append("")
    lines.append("| Map | shipped-dm | cold-dm | Pickups | Attempts | Frags | Deaths | K/D | Nav |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for m in maps:
        s = _variant(lm, m, V_SHIPPED_DM)
        c = _variant(lm, m, V_COLD_DM)
        base = s or c   # detail metrics from the shipped-dm run (fall back to cold-dm)
        if not base:
            lines.append(f"| {m} | — | — | — | — | — | — | — | — |")
            continue
        lines.append(f"| {m} | {fmt_pct(s.get('item_completion') if s else None)} | "
                     f"{fmt_pct(c.get('item_completion') if c else None)} | "
                     f"{base.get('pickups','—')} | {base.get('item_attempts','—')} | "
                     f"{base.get('frags','—')} | {base.get('deaths','—')} | "
                     f"{base.get('kd') if base.get('kd') is not None else '—'} | {base.get('nav_nodes','—')} |")
    lines.append(f"| **mean** | **{fmt_pct(_mean_item(latest, V_SHIPPED_DM))}** | "
                 f"**{fmt_pct(_mean_item(latest, V_COLD_DM))}** | | | | | | |")
    lines.append("")

    # per-item breakdown for the latest snapshot (shipped-dm run -- richest)
    lines.append("<details><summary>Per-item pickups (latest, shipped-dm)</summary>")
    lines.append("")
    for m in maps:
        d = _variant(lm, m, V_SHIPPED_DM)
        if not d or not d.get("pickups_by_item"):
            continue
        items = ", ".join(f"{n}× {it}" for it, n in d["pickups_by_item"].items())
        lines.append(f"- **{m}**: {items}")
    lines.append("")
    lines.append("</details>")
    lines.append("")

    # --- skill parity: id-parity kill-share (bot_skilltest, paired control) ---
    if latest.get("parity"):
        def _sh(v):   # share/shift fraction -> percentage cell
            return "—" if v is None else f"{v*100:.1f}%"

        def _shift(v):
            return "—" if v is None else f"{v*100:+.1f}pt"

        lines.append("### Skill parity (kill-share — bot_skilltest, 6 bots, paired control)")
        lines.append("")
        lines.append("Even ids get high skill (0.9), odd ids low (0.1). The raw even/odd kill "
                     "split carries a ~0.81 id-**position** bias at low seed counts (it washes "
                     "toward 0.5 at 16 seeds), so the headline is the **corrected shift** = "
                     "treatment_share − control_share (paired `bot_skilltest 0` run, same seed).")
        lines.append("")
        lines.append("| Map | high-skill share | control share | corrected shift | high kills | low kills |")
        lines.append("|---|---:|---:|---:|---:|---:|")
        for m in maps:
            p = _parity(latest, m)
            if not p:
                lines.append(f"| {m} | — | — | — | — | — |")
                continue
            lines.append(f"| {m} | {_sh(p.get('treatment_share'))} | {_sh(p.get('control_share'))} | "
                         f"{_shift(p.get('shift'))} | {p.get('kills_hi','—')} | {p.get('kills_lo','—')} |")
        lines.append(f"| **mean** | | | **{_shift(_mean_shift(latest))}** | | |")
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

    # --- headline trend: solo nav-collection over time ---
    lines.append("## Solo nav-collection over time (shipped)")
    lines.append("")
    lines += _item_trend(V_SHIPPED_SOLO)
    lines.append("")
    lines.append("<details><summary>Solo nav-collection over time (cold — from-scratch nav learning)</summary>")
    lines.append("")
    lines += _item_trend(V_COLD_SOLO)
    lines.append("")
    lines.append("</details>")
    lines.append("")

    # --- integration trend: deathmatch ITEM% over time ---
    lines.append("<details><summary>Deathmatch ITEM % over time (shipped-dm / cold-dm)</summary>")
    lines.append("")
    lines.append("_shipped-dm:_")
    lines += _item_trend(V_SHIPPED_DM)
    lines.append("")
    lines.append("_cold-dm:_")
    lines += _item_trend(V_COLD_DM)
    lines.append("")
    lines.append("</details>")
    lines.append("")

    # frags trend (shipped-dm run; symmetric self-play -> activity, not skill)
    lines.append("<details><summary>Total frags over time (shipped-dm; activity, not skill)</summary>")
    lines.append("")
    lines.append(header.replace(" mean ", " total "))
    lines.append(sep)
    for rec in history:
        rm = rec.get("maps", {})
        vs = [_variant(rm, m, V_SHIPPED_DM) for m in maps]
        cells = [str(d.get("frags", "—")) if d else "—" for d in vs]
        tot = sum(d.get("frags", 0) for d in vs if d)
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
    if not d:
        return None
    if variant in d:
        return d[variant]
    legacy = _LEGACY_VARIANT.get(variant)   # old shipped/cold rows == deathmatch
    return d.get(legacy) if legacy else None


def _mean_item(rec, variant):
    vals = []
    for m in rec.get("maps", {}):
        met = _variant(rec.get("maps", {}), m, variant)
        if met and met.get("item_completion") is not None:
            vals.append(met["item_completion"])
    return round(sum(vals) / len(vals), 1) if vals else None


def _parity(rec, m):
    return rec.get("parity", {}).get(m)


def _mean_shift(rec):
    vals = [p["shift"] for p in rec.get("parity", {}).values()
            if p and p.get("shift") is not None]
    return (sum(vals) / len(vals)) if vals else None


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
    ap.add_argument("--no-parity", action="store_true",
                    help="skip the skill-parity kill-share pass (bot_skilltest, 6 bots, paired control)")
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
    for vname, navdir, bots in VARIANTS:
        if not glob.glob(os.path.join(navdir, "*.nav")):
            log(f"WARNING: no navs in {navdir} -- skipping '{vname}' variant "
                + ("(run --pin-shipped first)" if navdir == PINNED_NAV_SHIPPED else "(run --mature first)"))
            continue
        is_solo = vname in SOLO_VARIANTS
        prepare_bench_mod(engine, navdir)
        for m in maps:
            log(f"running {m} [{vname}] ({args.instances}x{args.seconds:.0f}s game, "
                f"{bots} bot{'s' if bots != 1 else ''}, seed {args.seed})...")
            met = run_map(engine, exe, m, args, bots)
            if met:
                extra = "" if is_solo else f"  frags {met['frags']}"
                log(f"  {m} [{vname}]: {'COLLECT' if is_solo else 'ITEM'} "
                    f"{fmt_pct(met.get('item_completion'))}  pickups {met['pickups']}{extra}  "
                    f"nav {met['nav_nodes']}")
            results[m][vname] = met
        shutil.rmtree(os.path.join(engine, BENCH_MOD), ignore_errors=True)

    # skill-parity kill-share axis (default ON) -- run on the shipped pinned navs
    parity = {}
    if not args.no_parity and glob.glob(os.path.join(PINNED_NAV_SHIPPED, "*.nav")):
        prepare_bench_mod(engine, PINNED_NAV_SHIPPED)
        for m in maps:
            log(f"running {m} [parity] (bot_skilltest treatment + paired control, "
                f"{PARITY_BOTS} bots, seed {args.seed})...")
            p = run_parity_map(engine, exe, m, args)
            parity[m] = p
            if p and p.get("shift") is not None:
                log(f"  {m} [parity]: high-skill share {p['treatment_share']*100:.1f}% "
                    f"vs control {p['control_share']*100:.1f}% -> shift {p['shift']*100:+.1f}pt "
                    f"(hi {p['kills_hi']} / lo {p['kills_lo']} kills)")
        shutil.rmtree(os.path.join(engine, BENCH_MOD), ignore_errors=True)
    elif not args.no_parity:
        log("WARNING: no shipped navs -- skipping skill-parity pass (run --pin-shipped first)")

    record = {
        "date": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "commit": git("rev-parse", "--short", "HEAD"),
        "branch": git("rev-parse", "--abbrev-ref", "HEAD"),
        "dirty": bool(git("status", "--porcelain")),
        "note": args.note,
        "rig": {"instances": args.instances, "seconds": args.seconds, "bots": DM_BOTS,
                "skill": args.skill, "seed": args.seed, "repro": True,
                "cvars": REQUIRED_CVARS + (args.cvar or [])},
        "solo_rig": {"instances": args.instances, "seconds": args.seconds, "bots": SOLO_BOTS,
                     "skill": args.skill, "seed": args.seed, "repro": True,
                     "cvars": REQUIRED_CVARS + (args.cvar or [])},
        "maps": results,   # nested: {map: {shipped-solo, cold-solo, shipped-dm, cold-dm}}
        "parity": parity,  # nested: {map: {treatment_share, control_share, shift, kills_hi, ...}}
        "parity_rig": {"instances": args.instances, "seconds": args.seconds, "bots": PARITY_BOTS,
                       "skill": args.skill, "seed": args.seed, "repro": True, "paired_control": True,
                       "treatment_cvar": ["bot_skilltest", "1"], "control_cvar": ["bot_skilltest", "0"],
                       "nav": "shipped"},
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

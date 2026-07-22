#!/usr/bin/env python3
"""
ozbot-re joint cvar optimizer -- treat the sim as the black-box fitness function
it already is.

Every tuned constant in this bot (bot_commit 0.8, bot_budgetcap 15,
bot_stucktime 1.0, and the whole family of default-OFF levers) was chosen by
sweeping ONE knob while holding the rest fixed.  That yields a coordinate-wise
optimum, which is only a joint optimum if the knobs do not interact -- and
several of them provably do (bot_commit is a contention lever, bot_budgetcap
funds the routes it re-ranks, bot_lookahead's sign flips with nav density).  We
own a 400x-realtime, bit-exact, 16-way-parallel evaluator; this uses it as one.

Search: cross-entropy method (CEM).  Diagonal Gaussians for the continuous dims,
Bernoulli for the binary ones; sample a population, keep the elite fraction,
refit, repeat.  ~50 lines of actual algorithm, no dependency.  If CEM shows
signal worth refining, an Optuna/TPE pass is the natural follow-up.

Fitness is ECONOMY-led (total pickups over the training maps x rigs), because
that is the axis where this project's state features have confirmed wins and
where the metric is not noise-dominated.  Aim/accuracy cvars are deliberately
NOT in the search space: in a symmetric self-play sim, degrading everyone's aim
raises pickups by lowering death rate, which the optimizer would happily
exploit while making no bot better at anything.

Overfit guard, which is the whole point of running this rather than trusting a
best-of-N: the winner is re-scored on a HOLDOUT of disjoint seeds AND disjoint
maps.  A config only counts as a candidate if it beats baseline there; even then
it still owes a properly powered paired A/B before any default flips.

    py tools/optimize_cvars.py --iters 10 --pop 20
    py tools/optimize_cvars.py --report-only trace.jsonl
    py tools/optimize_cvars.py --eval-only "bot_commit=1.2,bot_budgetcap=20"

A null result is a real result: it is the definitive "is there headroom left in
the current layer" probe, and it is worth the wall-clock either way.

Stdlib only.
"""

import argparse
import glob
import json
import math
import os
import random
import shutil
import sys
import time
import types

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import run_parallel as rp
import benchmark as bm      # compute_metrics / prepare-mod conventions

DEFAULT_ENGINE = os.path.join(REPO, "engine")
OPT_MOD = "ozbotre_opt"                 # frozen source gamedir (never the live one)
DLL_NAME = "gamex86_64.dll"
EXE_NAME = "q2reproded.exe"
REQUIRED_CVARS = [["sv_fps", "40"]]

# Train / holdout split.  Disjoint maps AND disjoint seeds -- a config that only
# wins on the training maps is a nav-specific fluke, which is exactly the failure
# mode bot_lookahead exhibited (shipped-solo +4.5%, cold-solo -6.6%).
TRAIN_MAPS = ["q2dm1", "q2dm8"]
HOLDOUT_MAPS = ["q2dm2", "q2dm5"]
TRAIN_SEED = 700
HOLDOUT_SEED = 900

# rigs: (label, bots).  Both are scored -- solo is the nav/last-leg axis, dm adds
# contention and combat interruption, and a knob that helps one can hurt the other.
RIGS = [("solo", 1), ("dm", 5)]


class Dim:
    """One search dimension: a cvar, its range, and how to sample/report it."""

    def __init__(self, name, default, lo=None, hi=None, kind="float", step=None):
        self.name, self.default = name, default
        self.lo, self.hi, self.kind = lo, hi, kind
        self.step = step

    def clamp(self, v):
        if self.kind == "bool":
            return 1.0 if v >= 0.5 else 0.0
        v = max(self.lo, min(self.hi, v))
        if self.kind == "int":
            v = round(v)
        return v

    def fmt(self, v):
        if self.kind in ("int", "bool"):
            return str(int(round(v)))
        return f"{v:.3f}".rstrip("0").rstrip(".")


# The search space.  Continuous knobs first (where joint effects are plausible),
# then the default-OFF binaries that were each rejected in ISOLATION -- the
# premise of this phase is that "rejected while everything else was held fixed"
# is a weaker claim than it looked.  Deliberately excluded: aim/accuracy
# multipliers (degenerate in a symmetric sim, see the module docstring), bot_skill
# (changes difficulty, not quality), and the combat levers with standing
# rejection markers (bot_hearing, bot_combatmove, bot_dodge, bot_wpnselect).
SPACE = [
    # range widened after pass 1 pinned this dim at its 2.0 ceiling -- a boundary
    # optimum is not an optimum, it is an under-sized range
    Dim("bot_commit",      0.8,  0.0, 64.0),
    Dim("bot_budgetcap",   15,   8,    30,  kind="int"),
    Dim("bot_stucktime",   1.0,  0.4,  2.5),
    Dim("bot_lookahead",   0.0,  0.0,  0.8),
    Dim("bot_healthneed",  0,    kind="bool"),
    Dim("bot_navvalidate", 0,    kind="bool"),
    Dim("bot_failpersist", 0,    kind="bool"),
    Dim("bot_goalnode",    0,    kind="bool"),
    Dim("bot_wallslide",   0,    kind="bool"),
    Dim("bot_slimeescape", 0,    kind="bool"),
    # default-ON need model.  Present so a high bot_commit can be checked for
    # drowning it: the commit charge is added to `cost`, and score is
    # value*need/(1+cost/falloff), so a large enough charge makes the need term
    # irrelevant and silently turns these two shipped levers into no-ops.
    Dim("bot_control",     0.0,  0.0,  8.0),   # Phase 2: control-timing slack (secs)
    Dim("bot_danger",      0.0,  0.0,  4.0),   # Phase 3: death-heat route toll
    Dim("bot_wpnneed",     1,    kind="bool"),
    Dim("bot_ammoneed",    1,    kind="bool"),
]


def log(msg):
    print(f"[opt] {msg}", flush=True)


def vec_default():
    return [float(d.default) for d in SPACE]


def vec_to_cvars(vec):
    return [[d.name, d.fmt(d.clamp(v))] for d, v in zip(SPACE, vec)]


def vec_label(vec):
    return " ".join(f"{d.name}={d.fmt(d.clamp(v))}" for d, v in zip(SPACE, vec))


def vec_key(vec):
    return tuple(d.fmt(d.clamp(v)) for d, v in zip(SPACE, vec))


# ---------------------------------------------------------------------------
# evaluation
# ---------------------------------------------------------------------------

def prepare_mod(engine, nav_dir):
    """engine/ozbotre_opt = built DLL + pinned navs + playbooks.

    Frozen source gamedir so a live play.bat (which autosaves navs into the
    canonical engine/ozbotre) cannot perturb a search that runs for an hour.
    """
    mod = os.path.join(engine, OPT_MOD)
    shutil.rmtree(mod, ignore_errors=True)
    os.makedirs(os.path.join(mod, "nav"), exist_ok=True)

    dll = os.path.join(REPO, "dist", DLL_NAME)
    if not os.path.isfile(dll):
        sys.exit(f"FAIL: {dll} not found -- run build.bat + deploy.bat first.")
    shutil.copy2(dll, os.path.join(mod, DLL_NAME))

    navs = 0
    for f in glob.glob(os.path.join(nav_dir, "*.nav")):
        shutil.copy2(f, os.path.join(mod, "nav", os.path.basename(f)))
        navs += 1
    pbks = 0
    if os.path.isdir(bm.PINNED_PBK):
        dst = os.path.join(mod, "playbooks")
        os.makedirs(dst, exist_ok=True)
        for f in glob.glob(os.path.join(bm.PINNED_PBK, "*.pbk")):
            shutil.copy2(f, os.path.join(dst, os.path.basename(f)))
            pbks += 1
    log(f"froze DLL + {navs} navs + {pbks} playbooks into engine/{OPT_MOD}")
    return mod


def run_one(engine, exe, mapname, bots, seed, instances, seconds, cvars):
    """One rig on one map.  Returns compute_metrics() or None."""
    worker_mods = [f"{OPT_MOD}_w{i}" for i in range(1, instances + 1)]
    sim_args = types.SimpleNamespace(
        repro=True, fastsim=True, bots=bots, skill=0.6, timescale=1.0,
        seconds=seconds, map=mapname, cvar=list(REQUIRED_CVARS) + cvars)
    for mod in worker_mods:
        rp.setup_worker(engine, OPT_MOD, mod, mapname, DLL_NAME)

    procs = []
    try:
        for i, mod in enumerate(worker_mods):
            procs.append(rp.launch(engine, exe, mod, 27910 + i, seed + i, sim_args))
        end = time.time() + seconds + 120.0
        while time.time() < end:
            if all(p.poll() is not None for p in procs):
                break
            time.sleep(min(1.0, max(0.0, end - time.time())))
    finally:
        rp.stop(procs)

    logs_dir = os.path.join(engine, OPT_MOD, "logs")
    os.makedirs(logs_dir, exist_ok=True)
    out_path = os.path.join(logs_dir, f"opt_{mapname}_{bots}.jsonl")
    ticks, _ev, _nw, _bad = rp.merge_logs(engine, worker_mods, mapname, out_path)
    for mod in worker_mods:
        shutil.rmtree(os.path.join(engine, mod), ignore_errors=True)
    if ticks == 0:
        return None
    m = bm.compute_metrics(out_path)
    # these logs are ~60MB each and there are hundreds of evaluations
    try:
        os.remove(out_path)
    except OSError:
        pass
    return m


def evaluate(engine, exe, vec, maps, seed, args):
    """Fitness = total pickups over maps x rigs.  Returns (score, detail)."""
    cvars = vec_to_cvars(vec)
    total, detail = 0, {}
    for mapname in maps:
        for label, bots in RIGS:
            m = run_one(engine, exe, mapname, bots, seed,
                        args.instances, args.seconds, cvars)
            if m is None:
                log(f"  WARNING no telemetry for {mapname}/{label} -- scoring 0")
                detail[f"{mapname}-{label}"] = None
                continue
            total += m["pickups"]
            detail[f"{mapname}-{label}"] = {
                "pickups": m["pickups"], "item_completion": m["item_completion"],
                "frags": m["frags"], "deaths": m["deaths"],
                # composition guards against a degenerate win: raw pickups counts
                # a +2 Armor Shard the same as a Rocket Launcher, and a config
                # that biases hard toward the nearest item can raise the count
                # while collecting strictly less value (the COLLECT%-vs-pickups
                # trap from gen_nav, in the other direction)
                "by_item": m["pickups_by_item"],
            }
    return total, detail


# ---------------------------------------------------------------------------
# CEM
# ---------------------------------------------------------------------------

def cem(engine, exe, args, trace_fp, cache):
    n = len(SPACE)
    mu = vec_default()
    # start wide enough to actually explore, narrow enough to keep the first
    # generation from being all-noise: half the range per continuous dim
    sigma = [((d.hi - d.lo) * 0.5 if d.kind != "bool" else 0.0) for d in SPACE]
    p_on = [(0.5 if d.kind == "bool" else 0.0) for d in SPACE]
    n_elite = max(2, int(round(args.pop * args.elite)))
    best = (None, -1, None)

    for it in range(1, args.iters + 1):
        pop = []
        for k in range(args.pop):
            if it == 1 and k == 0:
                vec = vec_default()          # always evaluate the shipped config
            else:
                vec = []
                for i, d in enumerate(SPACE):
                    if d.kind == "bool":
                        vec.append(1.0 if random.random() < p_on[i] else 0.0)
                    else:
                        vec.append(d.clamp(random.gauss(mu[i], sigma[i])))
            pop.append([d.clamp(v) for d, v in zip(SPACE, vec)])

        scored = []
        for k, vec in enumerate(pop):
            key = vec_key(vec)
            if key in cache:
                score, detail = cache[key]
                log(f"it{it} #{k + 1}/{args.pop}  {score:>6}  (cached)  {vec_label(vec)}")
            else:
                t0 = time.time()
                score, detail = evaluate(engine, exe, vec, args.train_maps,
                                         args.train_seed, args)
                cache[key] = (score, detail)
                log(f"it{it} #{k + 1}/{args.pop}  {score:>6} pickups "
                    f"({time.time() - t0:.0f}s)  {vec_label(vec)}")
                trace_fp.write(json.dumps({
                    "iter": it, "n": k + 1, "score": score,
                    "cvars": dict(vec_to_cvars(vec)), "detail": detail}) + "\n")
                trace_fp.flush()
            scored.append((score, vec, detail))
            if score > best[1]:
                best = (vec, score, detail)

        scored.sort(key=lambda s: -s[0])
        elite = [s[1] for s in scored[:n_elite]]
        for i, d in enumerate(SPACE):
            vals = [e[i] for e in elite]
            m = sum(vals) / len(vals)
            if d.kind == "bool":
                p_on[i] = min(0.9, max(0.1, m))     # never fully collapse
            else:
                mu[i] = m
                var = sum((v - m) ** 2 for v in vals) / len(vals)
                # noise floor: keeps a dim from collapsing onto sim noise
                sigma[i] = max(math.sqrt(var), (d.hi - d.lo) * 0.05)
        log(f"it{it} elite mean -> " + vec_label(mu) + "   (p_on " +
            ",".join(f"{d.name.replace('bot_', '')}={p_on[i]:.2f}"
                     for i, d in enumerate(SPACE) if d.kind == "bool") + ")")
        log(f"it{it} best so far: {best[1]} pickups  {vec_label(best[0])}")

    return best


# ---------------------------------------------------------------------------

def holdout_check(engine, exe, best_vec, args):
    """Re-score winner vs shipped baseline on disjoint maps AND disjoint seeds."""
    log("")
    log(f"HOLDOUT: maps {','.join(args.holdout_maps)} at seed {args.holdout_seed} "
        f"(both disjoint from training)")
    base_s, base_d = evaluate(engine, exe, vec_default(), args.holdout_maps,
                              args.holdout_seed, args)
    log(f"  baseline (shipped defaults): {base_s} pickups")
    best_s, best_d = evaluate(engine, exe, best_vec, args.holdout_maps,
                              args.holdout_seed, args)
    log(f"  candidate:                   {best_s} pickups  {vec_label(best_vec)}")
    delta = best_s - base_s
    pct = 100.0 * delta / base_s if base_s else 0.0
    log(f"  holdout delta: {delta:+d} pickups ({pct:+.1f}%)")
    if delta <= 0:
        log("  VERDICT: the training win did NOT transfer -- overfit to the "
            "training maps/seeds.  No headroom found by this pass.")
    else:
        log("  VERDICT: candidate transfers.  This is NOT a ship signal yet -- "
            "it still owes a 16-seed paired A/B per map before any default moves.")
    return {"baseline": base_s, "candidate": best_s, "delta": delta,
            "pct": round(pct, 2), "baseline_detail": base_d,
            "candidate_detail": best_d}


def parse_assign(s):
    vec = vec_default()
    idx = {d.name: i for i, d in enumerate(SPACE)}
    for part in s.split(","):
        part = part.strip()
        if not part:
            continue
        k, _, v = part.partition("=")
        k = k.strip()
        if k not in idx:
            sys.exit(f"FAIL: {k} is not in the search space ({', '.join(idx)})")
        vec[idx[k]] = float(v)
    return [d.clamp(v) for d, v in zip(SPACE, vec)]


def report_only(path):
    rows = []
    with open(path, "r", encoding="utf-8") as fp:
        for line in fp:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    if not rows:
        sys.exit("FAIL: empty trace.")
    rows.sort(key=lambda r: -r["score"])
    base = next((r for r in rows if r["iter"] == 1 and r["n"] == 1), None)
    print(f"{len(rows)} evaluations; best {rows[0]['score']} pickups")
    if base:
        print(f"shipped defaults scored {base['score']}")
    print()
    for r in rows[:15]:
        d = " ".join(f"{k.replace('bot_', '')}={v}" for k, v in r["cvars"].items())
        print(f"  {r['score']:>6}  it{r['iter']:>2}#{r['n']:<3}  {d}")
    print("\nper-dim correlation with score (crude marginal, elite-vs-rest):")
    n_top = max(1, len(rows) // 4)
    top, rest = rows[:n_top], rows[n_top:]
    for d in SPACE:
        def avg(rs):
            vs = [float(r["cvars"][d.name]) for r in rs if d.name in r["cvars"]]
            return sum(vs) / len(vs) if vs else float("nan")
        print(f"  {d.name:<18} top-quartile mean {avg(top):>7.3f}   "
              f"rest {avg(rest):>7.3f}   (default {d.default})")


def main(argv):
    ap = argparse.ArgumentParser(
        description="Joint CEM search over ozbot-re's tunable cvars, with a "
                    "disjoint-map + disjoint-seed holdout guard.")
    ap.add_argument("--iters", type=int, default=8, help="CEM generations (default 8)")
    ap.add_argument("--pop", type=int, default=16, help="candidates per generation (default 16)")
    ap.add_argument("--elite", type=float, default=0.25, help="elite fraction (default 0.25)")
    ap.add_argument("--instances", type=int, default=16, help="parallel seeds per rig (default 16)")
    ap.add_argument("--seconds", type=float, default=90.0)
    ap.add_argument("--train-maps", default=",".join(TRAIN_MAPS))
    ap.add_argument("--holdout-maps", default=",".join(HOLDOUT_MAPS))
    ap.add_argument("--train-seed", type=int, default=TRAIN_SEED)
    ap.add_argument("--holdout-seed", type=int, default=HOLDOUT_SEED)
    ap.add_argument("--nav", default="shipped", choices=("shipped", "cold"))
    ap.add_argument("--engine", default=DEFAULT_ENGINE)
    ap.add_argument("--seed", type=int, default=1, help="rng seed for the SEARCH itself")
    ap.add_argument("--trace", default=os.path.join(REPO, "baselines", "optimize_trace.jsonl"))
    ap.add_argument("--resume", action="store_true",
                    help="seed the evaluation cache from an existing --trace")
    ap.add_argument("--report-only", default=None, metavar="TRACE",
                    help="re-render an existing trace and exit")
    ap.add_argument("--eval-only", default=None, metavar="ASSIGNMENTS",
                    help="score one config, e.g. \"bot_commit=1.2,bot_budgetcap=20\"")
    ap.add_argument("--no-holdout", action="store_true")
    args = ap.parse_args(argv[1:])

    if args.report_only:
        report_only(args.report_only)
        return 0

    args.train_maps = [m for m in args.train_maps.split(",") if m]
    args.holdout_maps = [m for m in args.holdout_maps.split(",") if m]
    random.seed(args.seed)

    engine = os.path.abspath(args.engine)
    exe = os.path.join(engine, EXE_NAME)
    if not os.path.isfile(exe):
        sys.exit(f"FAIL: {exe} not found -- build it with build_engine.bat")
    nav_dir = bm.PINNED_NAV_SHIPPED if args.nav == "shipped" else bm.PINNED_NAV
    prepare_mod(engine, nav_dir)

    if args.eval_only:
        vec = parse_assign(args.eval_only)
        score, detail = evaluate(engine, exe, vec, args.train_maps, args.train_seed, args)
        log(f"{score} pickups  {vec_label(vec)}")
        print(json.dumps(detail, indent=1))
        return 0

    cache = {}
    if args.resume and os.path.isfile(args.trace):
        idx = {d.name: i for i, d in enumerate(SPACE)}
        with open(args.trace, "r", encoding="utf-8") as fp:
            for line in fp:
                line = line.strip()
                if not line:
                    continue
                r = json.loads(line)
                vec = vec_default()
                for k, v in r["cvars"].items():
                    if k in idx:
                        vec[idx[k]] = float(v)
                cache[vec_key(vec)] = (r["score"], r.get("detail"))
        log(f"resume: {len(cache)} cached evaluations from {args.trace}")

    evals = args.iters * args.pop
    per_eval = len(args.train_maps) * len(RIGS)
    log(f"CEM: {args.iters} iters x {args.pop} pop = {evals} evaluations, "
        f"{per_eval} sims each ({evals * per_eval} sims total)")
    log(f"train: {','.join(args.train_maps)} @ seed {args.train_seed}, "
        f"{args.instances} instances x {args.seconds:.0f}s, nav={args.nav}")
    log(f"space: {len(SPACE)} dims -- " + ", ".join(d.name for d in SPACE))

    t0 = time.time()
    os.makedirs(os.path.dirname(args.trace), exist_ok=True)
    with open(args.trace, "a" if args.resume else "w", encoding="utf-8") as trace_fp:
        best_vec, best_score, best_detail = cem(engine, exe, args, trace_fp, cache)
    log("")
    log(f"search done in {(time.time() - t0) / 60:.1f} min")
    log(f"best training score: {best_score} pickups")
    log(f"best config:         {vec_label(best_vec)}")
    log(f"shipped defaults:    {cache.get(vec_key(vec_default()), ('?',))[0]} pickups")

    result = {"best": dict(vec_to_cvars(best_vec)), "best_score": best_score,
              "baseline_score": cache.get(vec_key(vec_default()), (None,))[0],
              "detail": best_detail}
    if not args.no_holdout:
        result["holdout"] = holdout_check(engine, exe, best_vec, args)
    out = os.path.splitext(args.trace)[0] + "_result.json"
    with open(out, "w", encoding="utf-8") as fp:
        json.dump(result, fp, indent=1)
    log(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

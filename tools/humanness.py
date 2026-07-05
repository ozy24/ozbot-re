#!/usr/bin/env python3
"""
Humanness profiler: how far do the bots' observable behavior distributions sit
from human ones?  (plans/humanization.md Phase 0)

Extracts the SAME feature set from two sources:
  - human: protocol-34 .dm2 demos (demos/sorted/<map>/*.dm2), via dm2parse's
    views/healths/sframes extension -- the recorder's own POV at 10Hz
  - bot:   telemetry JSONL (a run_parallel merged log or any bot log), using
    the tick records' origin/yaw/pitch

Both sources are reduced to identical 10Hz series (velocity by origin
differencing on BOTH sides, so the estimator is like-for-like by construction)
and compared per feature with KS and Wasserstein-1 distances.  The ranked
report is the humanization roadmap; the exported quantiles parameterize the
behavior phases.

Usage:
    py humanness.py human q2dm1 [--limit N] [--out human_q2dm1.json]
    py humanness.py bot <merged.jsonl> [--out bot.json]
    py humanness.py report <human.json> <bot.json>
    py humanness.py compare q2dm1 <merged.jsonl> [--limit N]   # all three

Feature caches are JSON so `report` re-runs are instant (the demo corpus scan
is the slow part; ~1300 demos take a few minutes even with all cores).
"""

import glob
import json
import math
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dm2parse

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEMOS_SORTED = os.path.join(ROOT, "demos", "sorted")
OUT_DIR = os.path.join(ROOT, "demos", "derived", "humanness")

DT = 0.1                    # both sources are 10Hz
TELEPORT_2D = 100.0         # units/frame horizontal jump = respawn/teleport
MOVING_2D = 100.0           # u/s: clearly translating (offset feature gate)
STILL_2D = 20.0             # u/s: standing still
JUMP_VZ_HI = 180.0          # u/s: rising fast enough to be a jump onset ...
JUMP_VZ_LO = 100.0          # u/s: ... coming from below this (not already rising)
STRAFE_MIN = 60.0           # u/s side speed on both sides of a reversal
PER_SOURCE_CAP = 400        # max samples per feature per demo/bot (long-file bias cap)

SAMPLE_FEATURES = [
    # name, description
    ("view_move_offset", "|view yaw - travel yaw| while moving (deg)"),
    ("yaw_rate", "|yaw change| per s (deg/s)"),
    ("yaw_rate_moving", "|yaw change| per s while moving (deg/s)"),
    ("pitch", "view pitch (deg, +down)"),
    ("pitch_rate", "|pitch change| per s (deg/s)"),
    ("speed2d", "horizontal speed (u/s)"),
    ("strafe_interval", "s between side-velocity reversals"),
    ("still_episode", "s per standing-still episode"),
]


def wrap180(d):
    while d > 180.0:
        d -= 360.0
    while d < -180.0:
        d += 360.0
    return d


def extract_features(pos, yaw, pitch, alive, contig):
    """The shared reducer.  pos: [(x,y,z)]; yaw/pitch: [deg]; alive: [bool];
    contig[i]: step i->i+1 is a genuine consecutive 10Hz step.  Returns
    (samples: name->list, scalars: name->(value, weight))."""
    n = len(pos)
    S = defaultdict(list)
    if n < 10:
        return S, {}

    # ---- step-wise derivations + validity ----
    ok = [False] * (n - 1)          # step usable at all
    vx = [0.0] * (n - 1)
    vy = [0.0] * (n - 1)
    vz = [0.0] * (n - 1)
    dyaw = [0.0] * (n - 1)
    dpitch = [0.0] * (n - 1)
    sp2 = [0.0] * (n - 1)
    for i in range(n - 1):
        if not (contig[i] and alive[i] and alive[i + 1]):
            continue
        dx = pos[i + 1][0] - pos[i][0]
        dy = pos[i + 1][1] - pos[i][1]
        dz = pos[i + 1][2] - pos[i][2]
        if dx * dx + dy * dy > TELEPORT_2D * TELEPORT_2D:
            continue                    # respawn/teleport
        # NOTE: no |dyaw| cutoff here -- respawn/teleport view snaps are already
        # excluded by the alive mask (health<=0 spans the death) and the origin
        # jump mask above, and a cutoff would one-sidedly censor the stock
        # bot's genuine 170-180 deg single-tick snaps (the very tell measured)
        dyw = wrap180(yaw[i + 1] - yaw[i])
        ok[i] = True
        vx[i] = dx / DT
        vy[i] = dy / DT
        vz[i] = dz / DT
        sp2[i] = math.hypot(dx, dy) / DT
        dyaw[i] = dyw
        dpitch[i] = pitch[i + 1] - pitch[i]

    # ---- per-step sample features ----
    for i in range(n - 1):
        if not ok[i]:
            continue
        S["yaw_rate"].append(abs(dyaw[i]) / DT)
        S["pitch_rate"].append(abs(dpitch[i]) / DT)
        S["pitch"].append(pitch[i])
        S["speed2d"].append(sp2[i])
        if sp2[i] > MOVING_2D:
            S["yaw_rate_moving"].append(abs(dyaw[i]) / DT)
            off = abs(wrap180(yaw[i] - math.degrees(math.atan2(vy[i], vx[i]))))
            S["view_move_offset"].append(off)

    # ---- episodic features (need runs of consecutive ok steps) ----
    total_ok = sum(1 for v in ok if v)
    jumps = 0
    still_start = None
    last_flip = None
    last_side_sign = 0
    for i in range(n - 1):
        if not ok[i]:
            still_start = None
            last_flip = None
            last_side_sign = 0
            continue
        # jump onsets: vertical velocity steps from "not rising" to "rising fast"
        if i > 0 and ok[i - 1] and vz[i] > JUMP_VZ_HI and vz[i - 1] < JUMP_VZ_LO:
            jumps += 1
        # standing still episodes
        if sp2[i] < STILL_2D:
            if still_start is None:
                still_start = i
        else:
            if still_start is not None:
                ep = (i - still_start) * DT
                if ep >= 0.2:
                    S["still_episode"].append(ep)
                still_start = None
        # strafe reversals: side velocity (in view frame) sign flips
        ryaw = math.radians(yaw[i])
        side = vx[i] * math.sin(ryaw) - vy[i] * math.cos(ryaw)   # view-right component
        sign = 1 if side > STRAFE_MIN else (-1 if side < -STRAFE_MIN else 0)
        if sign != 0:
            if last_side_sign != 0 and sign != last_side_sign and last_flip is not None:
                S["strafe_interval"].append((i - last_flip) * DT)
            if last_side_sign != 0 and sign != last_side_sign or last_flip is None:
                last_flip = i
            last_side_sign = sign

    # ---- scalars (value, weight) ----
    scalars = {}
    alive_secs = total_ok * DT
    if alive_secs > 5:
        scalars["jumps_per_min"] = (jumps / alive_secs * 60.0, alive_secs)
        still = sum(1 for i in range(n - 1) if ok[i] and sp2[i] < STILL_2D)
        scalars["pct_time_still"] = (100.0 * still / total_ok, total_ok)
        moving = [i for i in range(n - 1) if ok[i] and sp2[i] > MOVING_2D]
        if moving:
            runband = sum(1 for i in moving if 270.0 <= sp2[i] <= 330.0)
            scalars["pct_moving_at_runspeed"] = (100.0 * runband / len(moving), len(moving))
            offs = sum(1 for i in moving
                       if abs(wrap180(yaw[i] - math.degrees(math.atan2(vy[i], vx[i])))) > 30.0)
            scalars["pct_moving_looking_away"] = (100.0 * offs / len(moving), len(moving))
        # signed-yaw-rate lag autocorrelation over contiguous ok runs
        for lag in (1, 2, 3):
            num = den = cnt = 0.0
            mean = (sum(dyaw[i] for i in range(n - 1) if ok[i]) / total_ok) if total_ok else 0.0
            for i in range(n - 1 - lag):
                if all(ok[i + k] for k in range(lag + 1)):
                    num += (dyaw[i] - mean) * (dyaw[i + lag] - mean)
                    cnt += 1
            for i in range(n - 1):
                if ok[i]:
                    den += (dyaw[i] - mean) ** 2
            if den > 0 and cnt > 50:
                scalars[f"yaw_autocorr_lag{lag}"] = (num / den, cnt)

    return S, scalars


def cap_samples(S):
    """Uniform-stride cap so one long source can't dominate the pool."""
    out = {}
    for k, v in S.items():
        if len(v) > PER_SOURCE_CAP:
            step = len(v) / PER_SOURCE_CAP
            v = [v[int(i * step)] for i in range(PER_SOURCE_CAP)]
        out[k] = [round(x, 3) for x in v]
    return out


def trim_frozen(pos, view, healths, sframes):
    """Drop completely frozen head/tail (pre-spawn wait, end-of-match
    intermission: origin AND view byte-identical frame to frame)."""
    n = len(pos)
    a, b = 0, n
    while a + 1 < b and pos[a + 1] == pos[a] and view[a + 1] == view[a]:
        a += 1
    while b - 2 > a and pos[b - 2] == pos[b - 1] and view[b - 2] == view[b - 1]:
        b -= 1
    return pos[a:b], view[a:b], healths[a:b], sframes[a:b]


# -------------------------------------------------------------------------
# human side (one worker per demo; multiprocessing-friendly module function)
# -------------------------------------------------------------------------

def demo_worker(path):
    try:
        info = dm2parse.parse(path)
    except Exception:
        return None
    if not info["frames"] or len(info["frames"]) < 100:
        return None
    pos, view, healths, sframes = trim_frozen(
        info["frames"], info["views"], info["healths"], info["sframes"])
    n = len(pos)
    yaw = [v[1] for v in view]
    pitch = [v[0] for v in view]
    alive = [h > 0 for h in healths]
    contig = [sframes[i + 1] == sframes[i] + 1 for i in range(n - 1)]
    S, scalars = extract_features(pos, yaw, pitch, alive, contig)
    return {"map": info["map"], "samples": cap_samples(S), "scalars": scalars}


def run_human(mapname, limit=None, out=None, jobs=None):
    d = os.path.join(DEMOS_SORTED, mapname)
    files = sorted(glob.glob(os.path.join(d, "*.dm2")))
    if limit:
        files = files[:limit]
    if not files:
        sys.exit(f"no demos under {d}")
    print(f"[human] {len(files)} demos from {d}")

    results = []
    try:
        import concurrent.futures as cf
        with cf.ProcessPoolExecutor(max_workers=jobs) as ex:
            for i, r in enumerate(ex.map(demo_worker, files, chunksize=8)):
                if r is not None and (r["map"] or "").lower() == mapname.lower():
                    results.append(r)
                if (i + 1) % 100 == 0:
                    print(f"[human] parsed {i + 1}/{len(files)}")
    except (ImportError, OSError):
        for i, f in enumerate(files):
            r = demo_worker(f)
            if r is not None and (r["map"] or "").lower() == mapname.lower():
                results.append(r)

    return finalize("human", f"{mapname} x {len(results)} demos", results, out)


# -------------------------------------------------------------------------
# bot side
# -------------------------------------------------------------------------

def run_bot(logpath, out=None):
    ticks = defaultdict(list)          # bot id -> [(t,x,y,z,yaw,pitch,dead)]
    with open(logpath, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if '"tick"' not in line:
                continue
            try:
                r = json.loads(line)
            except ValueError:
                continue
            if r.get("type") != "tick" or "pitch" not in r:
                continue
            ticks[r["bot"]].append((r["t"], r["x"], r["y"], r["z"],
                                    r["yaw"], r["pitch"], r.get("dead", False)))
    if not ticks:
        sys.exit(f"no tick records with a pitch field in {logpath} "
                 "(needs a DLL built after the humanization telemetry change)")

    results = []
    for bot, rows in sorted(ticks.items()):
        rows.sort(key=lambda r: r[0])
        pos = [(r[1], r[2], r[3]) for r in rows]
        yaw = [r[4] for r in rows]
        pitch = [r[5] for r in rows]
        alive = [not r[6] for r in rows]
        contig = [abs(rows[i + 1][0] - rows[i][0] - DT) < 0.05
                  for i in range(len(rows) - 1)]
        S, scalars = extract_features(pos, yaw, pitch, alive, contig)
        results.append({"samples": cap_samples(S), "scalars": scalars})
    return finalize("bot", f"{os.path.basename(logpath)} x {len(results)} bots",
                    results, out)


# -------------------------------------------------------------------------
# pooling, distances, report
# -------------------------------------------------------------------------

def finalize(kind, desc, results, out):
    pooled = defaultdict(list)
    scal = defaultdict(lambda: [0.0, 0.0])
    for r in results:
        for k, v in r["samples"].items():
            pooled[k].extend(v)
        for k, (val, w) in r["scalars"].items():
            scal[k][0] += val * w
            scal[k][1] += w
    data = {
        "kind": kind, "desc": desc, "sources": len(results),
        "samples": {k: sorted(v) for k, v in pooled.items()},
        "scalars": {k: v[0] / v[1] for k, v in scal.items() if v[1] > 0},
    }
    if out:
        os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
        with open(out, "w") as f:
            json.dump(data, f)
        print(f"[{kind}] {desc} -> {out}")
    return data


def ks_distance(a, b):
    """Two-sample KS on pre-sorted lists."""
    i = j = 0
    na, nb = len(a), len(b)
    d = 0.0
    while i < na and j < nb:
        x = a[i] if a[i] <= b[j] else b[j]
        while i < na and a[i] <= x:
            i += 1
        while j < nb and b[j] <= x:
            j += 1
        d = max(d, abs(i / na - j / nb))
    return d


def wasserstein(a, b):
    """W1 between empirical distributions (pre-sorted), via quantile grid."""
    if not a or not b:
        return 0.0
    q = 200
    tot = 0.0
    for k in range(q):
        p = (k + 0.5) / q
        tot += abs(a[int(p * len(a))] - b[int(p * len(b))])
    return tot / q


def quantiles(xs, ps=(0.05, 0.25, 0.50, 0.75, 0.90, 0.95, 0.99)):
    if not xs:
        return {}
    return {f"p{int(p * 100):02d}": round(xs[min(len(xs) - 1, int(p * len(xs)))], 2)
            for p in ps}


def report(human, bot, out_json=None):
    print(f"\n=== humanness report ===")
    print(f"human: {human['desc']}   bot: {bot['desc']}\n")

    rows = []
    for name, desc in SAMPLE_FEATURES:
        h = human["samples"].get(name, [])
        b = bot["samples"].get(name, [])
        if len(h) < 100 or len(b) < 100:
            continue
        ks = ks_distance(h, b)
        w1 = wasserstein(h, b)
        hq, bq = quantiles(h), quantiles(b)
        rows.append((ks, w1, name, desc, hq, bq, len(h), len(b)))
    rows.sort(reverse=True)

    print(f"{'rank':4s} {'feature':18s} {'KS':>6s} {'W1':>9s}  "
          f"{'human p50':>10s} {'bot p50':>10s}   note")
    for rank, (ks, w1, name, desc, hq, bq, nh, nb) in enumerate(rows, 1):
        print(f"{rank:3d}. {name:18s} {ks:6.3f} {w1:9.2f}  "
              f"{hq.get('p50', 0):10.1f} {bq.get('p50', 0):10.1f}   {desc}")

    print("\n-- quantile detail (human | bot) --")
    for ks, w1, name, desc, hq, bq, nh, nb in rows:
        keys = sorted(hq.keys())
        print(f"  {name}  (n={nh}|{nb})")
        print("    human: " + "  ".join(f"{k}={hq[k]}" for k in keys))
        print("    bot:   " + "  ".join(f"{k}={bq[k]}" for k in keys))

    print("\n-- scalars (human | bot) --")
    allk = sorted(set(human["scalars"]) | set(bot["scalars"]))
    for k in allk:
        hv = human["scalars"].get(k)
        bv = bot["scalars"].get(k)
        hs = f"{hv:8.2f}" if hv is not None else "     n/a"
        bs = f"{bv:8.2f}" if bv is not None else "     n/a"
        print(f"  {k:28s} {hs} | {bs}")

    if out_json:
        payload = {
            "features": [
                {"rank": i + 1, "name": r[2], "ks": round(r[0], 4),
                 "w1": round(r[1], 3), "human_q": r[4], "bot_q": r[5]}
                for i, r in enumerate(rows)],
            "scalars": {k: {"human": human["scalars"].get(k),
                            "bot": bot["scalars"].get(k)} for k in allk},
        }
        with open(out_json, "w") as f:
            json.dump(payload, f, indent=1)
        print(f"\nreport JSON -> {out_json}")


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    cmd = args[0]

    def opt(name, default=None, cast=str):
        if name in args:
            i = args.index(name)
            return cast(args[i + 1])
        return default

    os.makedirs(OUT_DIR, exist_ok=True)
    if cmd == "human":
        mapname = args[1]
        run_human(mapname, limit=opt("--limit", None, int),
                  out=opt("--out", os.path.join(OUT_DIR, f"human_{mapname}.json")),
                  jobs=opt("--jobs", None, int))
    elif cmd == "bot":
        run_bot(args[1], out=opt("--out", os.path.join(OUT_DIR, "bot.json")))
    elif cmd == "report":
        with open(args[1]) as f:
            human = json.load(f)
        with open(args[2]) as f:
            bot = json.load(f)
        report(human, bot, out_json=opt("--out", None))
    elif cmd == "compare":
        mapname = args[1]
        hpath = os.path.join(OUT_DIR, f"human_{mapname}.json")
        if os.path.exists(hpath) and "--refresh" not in args:
            with open(hpath) as f:
                human = json.load(f)
            print(f"[human] cached {hpath} ({human['desc']})")
        else:
            human = run_human(mapname, limit=opt("--limit", None, int),
                              out=hpath, jobs=opt("--jobs", None, int))
        bot = run_bot(args[2], out=os.path.join(OUT_DIR, "bot_latest.json"))
        report(human, bot,
               out_json=os.path.join(OUT_DIR, f"report_{mapname}.json"))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()

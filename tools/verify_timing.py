#!/usr/bin/env python3
"""
ozbot-re variable-FPS timing-invariant checker (Phase R2 acceptance gate).

Game mechanics authored at 10Hz must not change when the server runs at 40Hz:
weapon fire intervals (FRAMESYNC gating in p_weapon/p_client) and the
seconds-based nextthink scheduler (item respawns).  This tool mines telemetry
for those observables so two runs (sv_fps 10 vs sv_fps 40) can be compared:

  py tools/verify_timing.py <log10.jsonl> <log40.jsonl>   # compare, PASS/FAIL
  py tools/verify_timing.py <log.jsonl>                   # just report one log

Observables:
  * per-weapon inter-fire gaps (from {"event":"fire"} records; the *mode* of
    the gap distribution is the weapon's cyclic fire period)
  * respawn scheduler error: {"event":"respawn_scheduled","delay":D} paired
    with the next {"event":"item_respawned"} on the same entity -- elapsed
    minus D is the scheduler error (must be within one authored frame, 0.1s)
  * tick cadence (dt between tick records -- documents the rate, not compared)

Stdlib only.
"""

import json
import sys
from collections import defaultdict


def load(path):
    fires = defaultdict(list)         # (bot, weapon) -> [t]
    sched = {}                        # ent -> (t, delay)
    resp_err = []                     # scheduler error samples (elapsed - delay)
    tickrate = None
    tick_dt = defaultdict(list)       # bot -> [t]
    last_t = 0.0
    with open(path, "r", encoding="utf-8") as fp:
        for line in fp:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            t = rec.get("type")
            if t == "run":
                tickrate = rec.get("tick_rate", tickrate)
                sched.clear()          # new worker file begins -- reset pairing
            elif t == "event":
                ev = rec.get("event")
                if ev == "fire":
                    fires[(rec.get("bot"), rec.get("weapon"))].append(rec["t"])
                elif ev == "respawn_scheduled":
                    sched[rec.get("ent")] = (rec["t"], rec.get("delay", 0.0))
                elif ev == "item_respawned":
                    hit = sched.pop(rec.get("ent"), None)
                    if hit is not None:
                        t0, delay = hit
                        if rec["t"] >= t0 and delay > 0:
                            resp_err.append(rec["t"] - t0 - delay)
            elif t == "tick":
                tick_dt[rec.get("bot")].append(rec["t"])
    return tickrate, fires, resp_err, tick_dt


def mode_gap_list(gaps, res=0.05):
    buckets = defaultdict(int)
    for gap in gaps:
        buckets[round(gap / res) * res] += 1
    if not buckets:
        return None, 0
    best = max(buckets.items(), key=lambda kv: kv[1])
    return best[0], best[1]


def weapon_periods(fires):
    """weapon -> (modal fire period, total gaps, modal count) pooled over shooters."""
    per_weapon = defaultdict(list)
    for (bot, weapon), times in fires.items():
        times = sorted(times)
        for a, b in zip(times, times[1:]):
            gap = b - a
            if 0 < gap <= 5.0:
                per_weapon[weapon].append(gap)
    out = {}
    for weapon, gaps in per_weapon.items():
        m, n = mode_gap_list(gaps)
        out[weapon] = (m, len(gaps), n)
    return out


def tick_cadence(tick_dt):
    dts = []
    for bot, times in tick_dt.items():
        times = sorted(times)
        dts += [round(b - a, 3) for a, b in zip(times, times[1:]) if b > a]
    if not dts:
        return None
    dts.sort()
    return dts[len(dts) // 2]


def summarize_err(errs):
    if not errs:
        return None
    errs = sorted(errs)
    return (errs[len(errs) // 2], max(abs(errs[0]), abs(errs[-1])), len(errs))


def report(path):
    tickrate, fires, resp_err, tick_dt = load(path)
    print(f"--- {path}")
    print(f"tick_rate: {tickrate}   tick dt (median): {tick_cadence(tick_dt)}")
    wp = weapon_periods(fires)
    for weapon in sorted(wp):
        m, total, n = wp[weapon]
        print(f"  fire period {weapon:28s} mode={m if m is None else round(m, 2)}s"
              f"  ({n}/{total} gaps)")
    err = summarize_err(resp_err)
    if err:
        med, worst, n = err
        print(f"  respawn scheduler error: median={med:+.2f}s worst={worst:.2f}s (n={n})")
    return tickrate, wp, err


def main(argv):
    if len(argv) == 2:
        report(argv[1])
        return 0
    if len(argv) != 3:
        print(__doc__)
        return 2

    _, wp_a, err_a = report(argv[1])
    _, wp_b, err_b = report(argv[2])

    print("\n--- comparison (A vs B)")
    failures = []
    for weapon in sorted(set(wp_a) & set(wp_b)):
        ma, ta, na = wp_a[weapon]
        mb, tb, nb = wp_b[weapon]
        if ma is None or mb is None or min(na, nb) < 10:
            continue    # not enough cyclic-fire samples to judge
        ok = abs(ma - mb) <= 0.051      # one 0.05 bucket of slack
        print(f"  {'OK  ' if ok else 'FAIL'} fire {weapon:28s} {ma:.2f}s vs {mb:.2f}s")
        if not ok:
            failures.append(f"fire period {weapon}: {ma:.2f} vs {mb:.2f}")
    for name, err in (("A", err_a), ("B", err_b)):
        if err is None:
            continue
        med, worst, n = err
        # a respawn think may only fire up to one authored frame late (plus
        # the 0.01s telemetry rounding)
        ok = worst <= 0.11
        print(f"  {'OK  ' if ok else 'FAIL'} respawn scheduler ({name})"
              f" median={med:+.2f}s worst={worst:.2f}s n={n}")
        if not ok:
            failures.append(f"respawn scheduler {name}: worst {worst:.2f}s")

    if failures:
        print(f"\nFAIL: {len(failures)} timing invariant(s) broken")
        return 1
    print("\nPASS: timing invariants hold")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

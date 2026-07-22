#!/usr/bin/env python3
"""
ozbot-re regret miner -- shame-signal taxonomy over standard run telemetry.

"Dumb" is a human judgment the headline metrics (pickups / ITEM% / frags) do not
measure: a bot can post a good pickup count while repeatedly running past a fight
it was winning, stepping over the health item that would have saved it, or
ping-ponging between two goals.  The hazard, decisive and outnumbered work all
started from exactly this kind of counterfactual mining, so this tool makes it a
first-class, repeatable readout: run the standard rig, mine the log, get a
frequency-ranked taxonomy per map with timestamps you can replay.

Tool-side only -- it reads the telemetry the shipped DLL already writes
(tick records, event records, and the map-load bot_reachlog item sweep).  No DLL
change, so it can be run against any historical log.

    py tools/mine_regret.py engine/ozbotre/logs/parallel_q2dm1_*.jsonl
    py tools/mine_regret.py <logs...> --json regret.json --top 8

Regret classes (each is a counterfactual, not a failure the bot already logs):

  ran_past_fight     an enemy has THIS bot as its target (so the sightline is
                     mutual and real -- telemetry carries no LOS, and this is how
                     we avoid counting through-wall pairs), the enemy sits inside
                     this bot's FOV cone and in range, and the bot never engages.
  passed_item        the bot walks within PASS_DIST of an item spot that is
                     available and that it needs by its own state (health/armor
                     low, weapon unowned) and does not take it.
  route_backtrack    while running at ONE unchanged goal, the bot comes back to
                     within BACKTRACK_DIST of a spot it held 1-4s earlier, having
                     travelled >BACKTRACK_EXCURSION away in between: it spent the
                     window returning to where it already was.  (A first cut used
                     a >120deg heading reversal instead and was ~all false
                     positives -- q2dm1 corridors are full of legitimate hairpins
                     that reverse the heading while making real path progress.
                     Revisiting a spot on an unchanged goal has no such excuse.)
  death_near_health  died with an available health item within DEATH_HEALTH_DIST.
                     Only COMBAT deaths count, and only when a `hazclass` record
                     is present to prove it (run the rig with `--cvar bot_hazlog
                     1`; it is log-only, so the off-state stays byte-identical).
                     Without those records the class is reported as unclassified,
                     because on q2dm3/6/7 most deaths are lava/slime suicides that
                     no health pickup would have prevented.
  goal_churn         >=CHURN_N item-goal switches inside CHURN_WINDOW seconds.

Known blind spots, stated so they are not mistaken for zeros:
  * tick records carry health/armor/weapon but NOT ammo counts, so ammo need is
    unmeasurable here -- ammo spots the bot passed are counted separately as
    `passed_ammo_unscored` and excluded from the taxonomy.
  * item availability is modelled (pickup near the spot -> unavailable for the
    item's observed respawn delay), because respawn events carry an entity index
    that the parallel merge does not namespace per worker.
  * FOV/range is a proxy for "could have fought"; the mutual-target requirement
    is what keeps it honest.

Stdlib only.
"""

import argparse
import glob
import json
import math
import os
import sys
from collections import defaultdict

BOT_ID_STRIDE = 1000     # run_parallel merge offset: worker i -> ids i*STRIDE + b

# --- ran_past_fight -------------------------------------------------------
FOV_DEG = 60.0           # half-angle of the "could see it" cone (client fov 90)
FIGHT_RANGE = 900.0      # beyond this, ignoring a foe is not obviously wrong
FIGHT_MIN_SECS = 0.5     # sustained, so a single tick of lag is not an incident

# --- passed_item ----------------------------------------------------------
PASS_DIST = 80.0         # plan's "within ~80u of the executed route"
PASS_DZ = 64.0           # ... and roughly on the same floor
PASS_GRACE = 3.0         # if it is picked up within this long, it was not skipped
DEFAULT_RESPAWN = 30.0

# --- route_backtrack ------------------------------------------------------
BACKTRACK_DIST = 96.0        # "same spot" radius
BACKTRACK_MIN_AGE = 1.0      # ... held at least this long ago
BACKTRACK_MAX_AGE = 4.0      # ... and at most this long ago
BACKTRACK_EXCURSION = 150.0  # must actually have gone somewhere in between
BACKTRACK_MIN_SPEED = 100.0
BACKTRACK_REFRACTORY = 2.0
BACKTRACK_SAMPLE = 0.1       # history/test resolution (seconds)

# --- death_near_health ----------------------------------------------------
DEATH_HEALTH_DIST = 400.0

# --- goal_churn -----------------------------------------------------------
CHURN_N = 4
CHURN_WINDOW = 10.0

HEALTH_ITEMS = {"Mega Health", "Large Health", "Small Health", "Health",
                "Adrenaline"}
ARMOR_ITEMS = {"Body Armor", "Combat Armor", "Jacket Armor", "Armor Shard",
               "Power Screen", "Power Shield"}
WEAPON_ITEMS = {"Shotgun", "Super Shotgun", "Machinegun", "Chaingun",
                "Grenade Launcher", "Rocket Launcher", "HyperBlaster",
                "Railgun", "BFG10K"}
# +2 pickups.  Passing one is technically a miss but worth ~nothing, and they
# come in clusters beside routes -- unsplit they swamp the class (642 of q2dm8's
# 789 passes were shards from a single cluster).  Reported separately.
MINOR_ITEMS = {"Armor Shard", "Small Health"}

# g_local.h MOD_*: deaths no health pickup could have prevented
ENV_MODS = {17, 18, 19, 20, 21, 22, 23, 25, 26, 27, 28, 29, 31}

CLASSES = ["ran_past_fight", "passed_item", "route_backtrack",
           "death_near_health", "goal_churn"]


def angdiff(a, b):
    """Smallest absolute difference between two yaw angles, in degrees."""
    d = (a - b) % 360.0
    return d if d <= 180.0 else 360.0 - d


def load(paths):
    """Read merged telemetry.  Returns (map, tick_rate, ticks, events, reach)."""
    ticks, events, reach = [], [], []
    mapname, tick_rate, bad = None, 10, 0
    for path in paths:
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
                kind = rec.get("type")
                if kind == "tick":
                    ticks.append(rec)
                elif kind == "event":
                    events.append(rec)
                elif kind == "reach":
                    reach.append(rec)
                elif kind == "run":
                    mapname = mapname or rec.get("map")
                    tick_rate = rec.get("tick_rate", tick_rate)
    return mapname or "unknown", tick_rate, ticks, events, reach, bad


def item_spots(reach):
    """Unique item spots from the map-load bot_reachlog sweep."""
    spots = {}
    for r in reach:
        if r.get("when", "load") != "load":
            continue
        key = (r.get("item", ""), round(r["x"]), round(r["y"]), round(r["z"]))
        if key not in spots:
            spots[key] = {"item": r.get("item", ""),
                          "classname": r.get("classname", ""),
                          "pos": (r["x"], r["y"], r["z"]),
                          "code": r.get("code", ""),
                          "gate": r.get("gate", "")}
    return list(spots.values())


def respawn_delays(events):
    """Observed respawn delay per item name (median), for the availability model."""
    per = defaultdict(list)
    for e in events:
        if e.get("event") == "respawn_scheduled" and "delay" in e:
            per[e.get("item", "")].append(e["delay"])
    out = {}
    for item, ds in per.items():
        ds.sort()
        out[item] = ds[len(ds) // 2]
    return out


def spot_grid(spots, cell=128.0):
    grid = defaultdict(list)
    for i, s in enumerate(spots):
        cx, cy = int(s["pos"][0] // cell), int(s["pos"][1] // cell)
        grid[(cx, cy)].append(i)
    return grid, cell


def nearby_spots(grid, cell, x, y):
    cx, cy = int(x // cell), int(y // cell)
    out = []
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            out.extend(grid.get((cx + dx, cy + dy), ()))
    return out


# ---------------------------------------------------------------------------
# regret classes
# ---------------------------------------------------------------------------

def mine_ran_past_fight(by_worker_time, incidents):
    """Mutual-sightline proxy: B targets A, A has A's FOV on B, A never engages."""
    open_ep = {}    # (worker, a, b) -> [t_start, t_last, n, dist_sum, pos]
    for worker in sorted(by_worker_time):
        times = sorted(by_worker_time[worker])
        for t in times:
            frame = by_worker_time[worker][t]
            # which bots are targeted by whom this frame (enemy is an EDICT index;
            # for the rig's dedicated servers client slot == bot id, so id = ent-1)
            targeted = defaultdict(list)
            for bid, r in frame.items():
                e = r.get("enemy", -1)
                if e >= 1:
                    targeted[worker * BOT_ID_STRIDE + (e - 1)].append(bid)
            seen = set()
            for aid, watchers in targeted.items():
                a = frame.get(aid)
                if not a or a.get("dead") or a.get("enemy", -1) >= 0:
                    continue    # dead, or already fighting somebody
                for bid in watchers:
                    b = frame.get(bid)
                    if not b or b.get("dead"):
                        continue
                    dx, dy = b["x"] - a["x"], b["y"] - a["y"]
                    dist = math.dist((a["x"], a["y"], a["z"]),
                                     (b["x"], b["y"], b["z"]))
                    if dist > FIGHT_RANGE or dist < 1.0:
                        continue
                    bearing = math.degrees(math.atan2(dy, dx))
                    if angdiff(bearing, a.get("yaw", 0.0)) > FOV_DEG:
                        continue
                    key = (worker, aid, bid)
                    seen.add(key)
                    ep = open_ep.get(key)
                    if ep is None or t - ep[1] > 0.2:
                        if ep is not None:
                            _close_fight(key, ep, incidents)
                        open_ep[key] = [t, t, 1, dist, (a["x"], a["y"], a["z"])]
                    else:
                        ep[1] = t
                        ep[2] += 1
                        ep[3] += dist
            for key in [k for k in open_ep if k[0] == worker and k not in seen]:
                _close_fight(key, open_ep.pop(key), incidents)
    for key, ep in open_ep.items():
        _close_fight(key, ep, incidents)


def _close_fight(key, ep, incidents):
    dur = ep[1] - ep[0]
    if dur < FIGHT_MIN_SECS:
        return
    worker, aid, bid = key
    incidents["ran_past_fight"].append({
        "worker": worker, "bot": aid, "t": round(ep[0], 2),
        "dur": round(dur, 2), "target": bid,
        "dist": round(ep[3] / ep[2]),
        "x": round(ep[4][0]), "y": round(ep[4][1]), "z": round(ep[4][2]),
    })


def mine_passed_item(ticks_by_bot, spots, delays, pickups, goals, incidents, counters):
    """Walked past an available item the bot's own state says it needed."""
    grid, cell = spot_grid(spots)
    # availability: per (worker, spot index) list of (from, until) unavailable spans
    unavail = defaultdict(list)
    for p in pickups:
        worker = p["bot"] // BOT_ID_STRIDE
        best, bestd = None, 96.0
        for i in nearby_spots(grid, cell, p["x"], p["y"]):
            s = spots[i]
            if s["item"] != p.get("item"):
                continue
            d = math.dist((p["x"], p["y"], p["z"]), s["pos"])
            if d < bestd:
                best, bestd = i, d
        if best is not None:
            delay = delays.get(p.get("item", ""), DEFAULT_RESPAWN)
            unavail[(worker, best)].append((p["t"], p["t"] + delay))
            unavail[(worker, best)].sort()

    def available(worker, idx, t):
        for lo, hi in unavail.get((worker, idx), ()):
            if lo - 0.1 <= t <= hi:
                return False
        return True

    # pickups keyed for the grace check
    pick_at = defaultdict(list)
    for p in pickups:
        pick_at[p["bot"]].append((p["t"], p.get("item", "")))
    for v in pick_at.values():
        v.sort()

    # what the bot was chasing at the time -- the last goal_item event before t.
    # "passed a needed Large Health while running at a Rocket Launcher" is a very
    # different regret from "passed it with no goal at all".
    goal_at = defaultdict(list)
    for g in goals:
        goal_at[g["bot"]].append((g["t"], g.get("item", "")))
    for v in goal_at.values():
        v.sort()

    for bot, rows in ticks_by_bot.items():
        worker = bot // BOT_ID_STRIDE
        owned = set()           # weapons seen held this life
        open_pass = {}
        last_alive = None
        for r in rows:
            if r.get("dead"):
                owned.clear()
                open_pass.clear()
                last_alive = None
                continue
            w = r.get("weapon", "")
            if w and w != "none":
                owned.add(w)
            t = r["t"]
            last_alive = r
            hits = set()
            for i in nearby_spots(grid, cell, r["x"], r["y"]):
                s = spots[i]
                if abs(s["pos"][2] - r["z"]) > PASS_DZ:
                    continue
                if math.dist((r["x"], r["y"], r["z"]), s["pos"]) > PASS_DIST:
                    continue
                name = s["item"]
                if name in HEALTH_ITEMS:
                    need = r.get("health", 100) < (100 if name != "Small Health" else 100) \
                        and r.get("health", 100) < 90
                elif name in ARMOR_ITEMS:
                    need = r.get("armor", 0) < 50
                elif name in WEAPON_ITEMS:
                    need = name not in owned
                else:
                    counters["passed_ammo_unscored"] += 1
                    continue
                if not need or not available(worker, i, t):
                    continue
                hits.add(i)
                ep = open_pass.get(i)
                if ep is None or t - ep[1] > 0.5:
                    if ep is not None:
                        _close_pass(bot, worker, spots, ep, pick_at, goal_at, incidents)
                    open_pass[i] = [t, t, i, (r["x"], r["y"], r["z"]),
                                    r.get("health", 0), r.get("armor", 0)]
                else:
                    ep[1] = t
            for i in [k for k in open_pass if k not in hits]:
                _close_pass(bot, worker, spots, open_pass.pop(i), pick_at, goal_at, incidents)
        for ep in open_pass.values():
            _close_pass(bot, worker, spots, ep, pick_at, goal_at, incidents)
        del last_alive


def _close_pass(bot, worker, spots, ep, pick_at, goal_at, incidents):
    t0, t1, idx = ep[0], ep[1], ep[2]
    name = spots[idx]["item"]
    for pt, pitem in pick_at.get(bot, ()):
        if pitem == name and t0 - 0.5 <= pt <= t1 + PASS_GRACE:
            return      # it did take it, just a frame or two later
    chasing = ""
    for gt, gitem in goal_at.get(bot, ()):
        if gt > t1:
            break
        chasing = gitem
    incidents["passed_item"].append({
        "worker": worker, "bot": bot, "t": round(t0, 2),
        "item": name, "chasing": chasing, "health": ep[4], "armor": ep[5],
        "x": round(ep[3][0]), "y": round(ep[3][1]), "z": round(ep[3][2]),
    })


def mine_backtrack(ticks_by_bot, incidents):
    """Returned to a spot it already held, on an unchanged goal -- wasted travel."""
    for bot, rows in ticks_by_bot.items():
        hist = []       # (t, x, y, z, goal_node), sampled at BACKTRACK_SAMPLE
        last = -99.0
        next_sample = -99.0
        for r in rows:
            if r.get("dead") or r.get("mode") != 1 or r.get("enemy", -1) >= 0:
                hist.clear()
                continue
            t = r["t"]
            if t < next_sample:
                continue
            next_sample = t + BACKTRACK_SAMPLE
            g = r.get("goal_node", -1)
            while hist and t - hist[0][0] > BACKTRACK_MAX_AGE:
                hist.pop(0)
            here = (r["x"], r["y"], r["z"])
            if (math.hypot(r.get("vx", 0.0), r.get("vy", 0.0)) >= BACKTRACK_MIN_SPEED
                    and t - last >= BACKTRACK_REFRACTORY):
                for i, (t0, x0, y0, z0, g0) in enumerate(hist):
                    if g0 != g or t - t0 < BACKTRACK_MIN_AGE:
                        continue
                    if math.dist(here, (x0, y0, z0)) > BACKTRACK_DIST:
                        continue
                    # it must have actually gone somewhere and come back
                    away = max((math.dist((x0, y0, z0), (h[1], h[2], h[3]))
                                for h in hist[i:]), default=0.0)
                    if away < BACKTRACK_EXCURSION:
                        continue
                    incidents["route_backtrack"].append({
                        "worker": bot // BOT_ID_STRIDE, "bot": bot,
                        "t": round(t0, 2), "dt": round(t - t0, 2),
                        "away": round(away), "goal_node": g,
                        "x": round(x0), "y": round(y0), "z": round(z0),
                    })
                    last = t
                    hist.clear()
                    break
            hist.append((t, r["x"], r["y"], r["z"], g))


def mine_death_near_health(deaths, hazclass, spots, delays, pickups, incidents,
                           counters):
    health_idx = [i for i, s in enumerate(spots) if s["item"] in HEALTH_ITEMS]
    if not health_idx:
        return
    # bot_hazlog records classify EVERY death by MOD; without them we cannot tell
    # a lost gunfight from a walk into lava, and on the suicide-heavy maps that is
    # most of the class.  Match by (bot, time) -- both records fire the same frame.
    cause = {}
    for h in hazclass:
        cause[(h["bot"], round(h["t"], 2))] = h.get("mod", 0)
    counters["deaths_classified"] = len(cause)
    unavail = defaultdict(list)
    for p in pickups:
        if p.get("item") not in HEALTH_ITEMS:
            continue
        worker = p["bot"] // BOT_ID_STRIDE
        for i in health_idx:
            if math.dist((p["x"], p["y"], p["z"]), spots[i]["pos"]) < 96.0:
                unavail[(worker, i)].append(
                    (p["t"], p["t"] + delays.get(p["item"], DEFAULT_RESPAWN)))
                break
    for d in deaths:
        mod = cause.get((d["bot"], round(d["t"], 2)))
        if mod is not None and mod in ENV_MODS:
            counters["death_env_excluded"] += 1
            continue
        worker = d["bot"] // BOT_ID_STRIDE
        best, bestd = None, DEATH_HEALTH_DIST
        for i in health_idx:
            if any(lo - 0.1 <= d["t"] <= hi for lo, hi in unavail.get((worker, i), ())):
                continue
            dd = math.dist((d["x"], d["y"], d["z"]), spots[i]["pos"])
            if dd < bestd:
                best, bestd = i, dd
        if best is None:
            continue
        incidents["death_near_health"].append({
            "worker": worker, "bot": d["bot"], "t": round(d["t"], 2),
            "item": spots[best]["item"], "dist": round(bestd),
            "mod": mod if mod is not None else -1,
            "x": round(d["x"]), "y": round(d["y"]), "z": round(d["z"]),
        })


def mine_goal_churn(goal_events, incidents):
    per = defaultdict(list)
    for e in goal_events:
        per[e["bot"]].append(e)
    for bot, evs in per.items():
        evs.sort(key=lambda e: e["t"])
        i = 0
        for j, e in enumerate(evs):
            while evs[j]["t"] - evs[i]["t"] > CHURN_WINDOW:
                i += 1
            if j - i + 1 >= CHURN_N:
                incidents["goal_churn"].append({
                    "worker": bot // BOT_ID_STRIDE, "bot": bot,
                    "t": round(evs[i]["t"], 2),
                    "n": j - i + 1,
                    "x": round(e.get("x", 0)), "y": round(e.get("y", 0)),
                    "z": round(e.get("z", 0)),
                })
                i = j + 1   # one incident per burst


# ---------------------------------------------------------------------------

def mine(paths):
    mapname, tick_rate, ticks, events, reach, bad = load(paths)
    if not ticks:
        return None

    ticks_by_bot = defaultdict(list)
    by_worker_time = defaultdict(lambda: defaultdict(dict))
    for r in ticks:
        ticks_by_bot[r["bot"]].append(r)
        by_worker_time[r["bot"] // BOT_ID_STRIDE][r["t"]][r["bot"]] = r
    for rows in ticks_by_bot.values():
        rows.sort(key=lambda r: r["t"])

    spots = item_spots(reach)
    delays = respawn_delays(events)
    pickups = [e for e in events if e.get("event") == "pickup" and "x" in e]
    deaths = [e for e in events if e.get("event") == "death" and "x" in e]
    goals = [e for e in events if e.get("event") == "goal_item"]
    hazclass = [e for e in events if e.get("event") == "hazclass"]

    incidents = {c: [] for c in CLASSES}
    counters = defaultdict(int)

    mine_ran_past_fight(by_worker_time, incidents)
    mine_passed_item(ticks_by_bot, spots, delays, pickups, goals, incidents, counters)
    mine_backtrack(ticks_by_bot, incidents)
    mine_death_near_health(deaths, hazclass, spots, delays, pickups, incidents,
                           counters)
    mine_goal_churn(goals, incidents)

    span = max(r["t"] for r in ticks) - min(r["t"] for r in ticks)
    bots = len(ticks_by_bot)
    botmins = max(1e-9, bots * span / 60.0)

    return {
        "map": mapname,
        "tick_rate": tick_rate,
        "files": [os.path.basename(p) for p in paths],
        "bots": bots,
        "workers": len(by_worker_time),
        "span": round(span, 1),
        "bot_minutes": round(botmins, 1),
        "item_spots": len(spots),
        "pickups": len(pickups),
        "deaths": len(deaths),
        "goal_item_events": len(goals),
        "malformed_lines": bad,
        "counters": dict(counters),
        "incidents": incidents,
    }


def _major_incidents(res, name):
    """Incidents worth acting on -- passed_item drops the +2 trinkets, whose
    clusters otherwise dominate both the top-N list and the hotspot map."""
    ev = res["incidents"][name]
    if name == "passed_item":
        ev = [e for e in ev if e["item"] not in MINOR_ITEMS]
    return ev


def report(res, top):
    print(f"file(s):      {', '.join(res['files'])}")
    print(f"map:          {res['map']}  ({res['tick_rate']}Hz)")
    print(f"population:   {res['workers']} workers x {res['bots'] // max(1, res['workers'])} bots"
          f"  ({res['bots']} bots, {res['span']:.0f}s, {res['bot_minutes']:.1f} bot-minutes)")
    print(f"context:      {res['item_spots']} item spots, {res['pickups']} pickups, "
          f"{res['deaths']} deaths, {res['goal_item_events']} item goals")
    print()

    rows = [(c, len(_major_incidents(res, c)), len(res["incidents"][c]))
            for c in CLASSES]
    rows.sort(key=lambda kv: -kv[1])
    bm = res["bot_minutes"]
    print(f"{'regret class':<20}{'incidents':>10}{'(raw)':>8}"
          f"{'per bot-min':>13}{'per pickup':>12}")
    for name, n, tot in rows:
        print(f"{name:<20}{n:>10}{('' if n == tot else str(tot)):>8}{n / bm:>13.2f}"
              f"{(n / res['pickups'] if res['pickups'] else 0):>12.2f}")
    print("(passed_item `incidents` excludes the +2 trinkets -- Armor Shard, "
          "Small Health; `raw` is every pass)")
    cl = res["counters"].get("deaths_classified", 0)
    if cl:
        print(f"death_near_health: {res['counters'].get('death_env_excluded', 0)} "
              f"environmental deaths excluded of {cl} classified (bot_hazlog)")
    else:
        print("death_near_health: UNCLASSIFIED -- no bot_hazlog records, so lava/"
              "slime suicides are still counted.  Re-run with --cvar bot_hazlog 1.")
    if res["counters"].get("passed_ammo_unscored"):
        print(f"\n(ammo spots passed but unscorable -- no ammo counts in tick "
              f"telemetry: {res['counters']['passed_ammo_unscored']} tick-hits)")

    # sub-splits: the composition is what says whether a class is actionable
    bt = res["incidents"]["route_backtrack"]
    if bt:
        aways = sorted(e["away"] for e in bt)
        print(f"\nroute_backtrack: median excursion {aways[len(aways) // 2]}u "
              f"(p90 {aways[int(0.9 * (len(aways) - 1))]}u) -- travel spent "
              f"returning to a spot already held")
    passed = res["incidents"]["passed_item"]
    if passed:
        by_item = defaultdict(int)
        for e in passed:
            by_item[e["item"]] += 1
        print("passed_item by item: " + ", ".join(
            f"{n}x {it}" for it, n in sorted(by_item.items(), key=lambda kv: -kv[1])[:8]))
        idle = sum(1 for e in passed if not e.get("chasing"))
        print(f"   {idle} passed with no item goal at all; "
              f"{len(passed) - idle} while committed elsewhere")

    for name, n, _raw in rows:
        if not n or not top:
            continue
        print(f"\n-- {name}: top {min(top, n)} of {n} (replay with the worker seed) --")
        # rank by whatever makes the incident worst, else just first-N by time
        ev = _major_incidents(res, name)
        if name == "ran_past_fight":
            ev = sorted(ev, key=lambda e: -e["dur"])
        elif name == "goal_churn":
            ev = sorted(ev, key=lambda e: -e["n"])
        elif name == "death_near_health":
            ev = sorted(ev, key=lambda e: e["dist"])
        elif name == "route_backtrack":
            ev = sorted(ev, key=lambda e: -e["away"])
        for e in ev[:top]:
            extra = " ".join(f"{k}={v}" for k, v in e.items()
                             if k not in ("worker", "bot", "t", "x", "y", "z"))
            print(f"   w{e['worker']:<3} bot {e['bot']:<6} t={e['t']:>6.2f}  "
                  f"@({e['x']:>6},{e['y']:>6},{e['z']:>6})  {extra}")

    print("\nhotspots (>=3 incidents in a 128u cell):")
    for name, _n, _raw in rows:
        cells = defaultdict(int)
        for e in _major_incidents(res, name):
            cells[(int(e["x"] // 128), int(e["y"] // 128))] += 1
        hot = sorted(cells.items(), key=lambda kv: -kv[1])[:3]
        hot = [h for h in hot if h[1] >= 3]
        if hot:
            print(f"   {name:<20}" + ", ".join(
                f"{n}x @({c[0] * 128 + 64},{c[1] * 128 + 64})" for c, n in hot))


def main(argv):
    ap = argparse.ArgumentParser(
        description="Mine a regret taxonomy (counterfactual mistakes) from ozbot telemetry.")
    ap.add_argument("logs", nargs="+", help="merged telemetry JSONL (globs ok)")
    ap.add_argument("--top", type=int, default=6,
                    help="incidents to print per class (0 = none; default 6)")
    ap.add_argument("--json", default=None, help="write the full taxonomy here")
    ap.add_argument("--per-file", action="store_true",
                    help="mine each log separately (default: one pooled report per map)")
    args = ap.parse_args(argv[1:])

    paths = []
    for pat in args.logs:
        hit = sorted(glob.glob(pat))
        paths.extend(hit or [pat])
    paths = [p for p in paths if os.path.isfile(p)]
    if not paths:
        sys.exit("FAIL: no telemetry files matched.")

    groups = ([[p] for p in paths] if args.per_file
              else list(_by_map(paths).values()))

    out = []
    for i, group in enumerate(groups):
        res = mine(group)
        if not res:
            print(f"warning: no tick records in {group}", file=sys.stderr)
            continue
        if i:
            print("\n" + "=" * 72 + "\n")
        report(res, args.top)
        out.append(res)

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fp:
            json.dump(out if len(out) != 1 else out[0], fp, indent=1)
        print(f"\nwrote {args.json}")
    return 0


def _by_map(paths):
    """Group log paths by the map token in parallel_<map>_<stamp>.jsonl."""
    groups = defaultdict(list)
    for p in paths:
        base = os.path.basename(p)
        parts = base.split("_")
        key = parts[1] if base.startswith("parallel_") and len(parts) > 2 else parts[0]
        groups[key].append(p)
    return groups


if __name__ == "__main__":
    sys.exit(main(sys.argv))

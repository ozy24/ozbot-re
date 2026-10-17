#!/usr/bin/env python3
"""
ozbot-re self-playbook miner -- the bot mines its OWN successful traversals.

Last-leg precision is the oldest standing bottleneck, and playbooks
(NAV_LINK_PLAYBOOK, src/bot_playback.c) fix exactly that -- but every playbook so
far cost a human recording session.  The bot's own inputs are already recordable
(`bot_cmdlog 1`, same schema as a human capture), and the executor already
self-selects: an aborted replay penalizes its own link.  So the loop closes
without a human:

    run the rig -> find takes where a bot COMPLETED an item it usually fails ->
    score the takes -> bake the best via make_playbook.py -> A/B the .pbk

Why the q2dm4/5 demo->nav negative does not apply here: that was HUMAN
trajectories grafted as nav edges (embodiment mismatch + detour
over-investment).  This is the bot's own inputs, replayed by the executor built
for them, with abort-penalty self-selection.

    py tools/mine_selfplay.py --run --map q2dm1 --seconds 90 --instances 8
    py tools/mine_selfplay.py --logs "engine/ozbotre_w*/logs/q2dm1_*.jsonl" --map q2dm1
    py tools/mine_selfplay.py ... --bake engine/ozbotre/playbooks/q2dm1.pbk

IMPORTANT -- why this mines PER-WORKER logs, not the merged one: `input` records
carry a `slot`, and run_parallel's merge offsets only the `bot` field, so a
merged log interleaves every worker's slot 0.  Per-worker logs are internally
consistent.  The bot-id -> slot mapping is then derived by MATCHING POSITIONS at
a shared timestamp rather than assuming id == slot.

Stdlib only.
"""

import argparse
import glob
import json
import math
import os
import subprocess
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# a take must look like a traversal, not a stumble
MIN_TAKE_SECS = 0.6
MAX_TAKE_SECS = 12.0
MIN_TAKE_DIST = 96.0


def load(path):
    """One worker's telemetry: ticks, events, inputs, reach."""
    ticks, events, inputs, reach = [], [], [], []
    for line in open(path, "r", encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        t = rec.get("type")
        if t == "tick":
            ticks.append(rec)
        elif t == "event":
            events.append(rec)
        elif t == "input":
            inputs.append(rec)
        elif t == "reach":
            reach.append(rec)
    return ticks, events, inputs, reach


def map_id_to_slot(ticks, inputs):
    """Match bot id -> input slot by position at a shared timestamp.

    id == slot happens to hold on the standard dedicated rig, but assuming it is
    exactly the kind of thing that silently mis-attributes a take to the wrong
    bot, so derive it instead.
    """
    tick_at = defaultdict(dict)     # t -> {bot: (x,y,z)}
    for r in ticks:
        tick_at[round(r["t"], 2)][r["bot"]] = (r["x"], r["y"], r["z"])
    votes = defaultdict(lambda: defaultdict(int))
    for r in inputs:
        frame = tick_at.get(round(r["t"], 2))
        if not frame:
            continue
        p = (r["x"], r["y"], r["z"])
        best, bestd = None, 2.0
        for bot, q in frame.items():
            d = math.dist(p, q)
            if d < bestd:
                best, bestd = bot, d
        if best is not None:
            votes[r["slot"]][best] += 1
    out = {}
    for slot, v in votes.items():
        bot = max(v.items(), key=lambda kv: kv[1])[0]
        out[bot] = slot
    return out


def target_items(events, reach, top_n):
    """Items worth a playbook: the ones bots keep failing, plus gated ones.

    A giveup record carries no item name, so attribute it to the bot's most
    recent goal_item -- that is the item it was committed to when it quit.
    """
    cur_goal = {}
    giveups = defaultdict(int)
    attempts = defaultdict(int)
    pickups = defaultdict(int)
    for e in sorted(events, key=lambda e: e.get("t", 0)):
        ev = e.get("event")
        if ev == "goal_item":
            cur_goal[e.get("bot")] = e.get("item", "")
            attempts[e.get("item", "")] += 1
        elif ev == "giveup":
            it = cur_goal.get(e.get("bot"))
            if it:
                giveups[it] += 1
        elif ev == "pickup":
            pickups[e.get("item", "")] += 1

    gated = set()
    for r in reach:
        if r.get("when", "load") == "load" and r.get("code") not in ("ok", None):
            gated.add(r.get("item", ""))

    scored = []
    for it, n in giveups.items():
        att = attempts.get(it, 0)
        rate = n / att if att else 0
        scored.append((n, rate, it))
    scored.sort(reverse=True)
    picked = [s[2] for s in scored[:top_n]]
    for g in gated:
        if g and g not in picked:
            picked.append(g)
    return picked, giveups, attempts, pickups, gated


def extract_takes(events, inputs, id2slot, targets, src):
    """A take = the input span from committing to an item until picking it up."""
    by_slot = defaultdict(list)
    for r in inputs:
        by_slot[r["slot"]].append(r)
    for v in by_slot.values():
        v.sort(key=lambda r: r["t"])

    cur = {}    # bot -> (item, t_commit)
    takes = []
    for e in sorted(events, key=lambda e: e.get("t", 0)):
        ev, bot = e.get("event"), e.get("bot")
        if ev == "goal_item":
            cur[bot] = (e.get("item", ""), e.get("t", 0))
        elif ev == "pickup":
            item = e.get("item", "")
            got = cur.pop(bot, None)
            if not got or got[0] != item or item not in targets:
                continue
            t0, t1 = got[1], e.get("t", 0)
            if not (MIN_TAKE_SECS <= t1 - t0 <= MAX_TAKE_SECS):
                continue
            slot = id2slot.get(bot)
            if slot is None:
                continue
            seg = [r for r in by_slot.get(slot, ()) if t0 <= r["t"] <= t1]
            if len(seg) < 8:
                continue
            seg = anchor_at_standstill(seg, t1)
            if not seg:
                continue
            t0 = seg[0]["t"]
            tk = score_take(item, bot, slot, t0, t1, seg)
            if tk:
                tk["src"] = src   # which worker log to bake from
            takes.append(tk)
    return [t for t in takes if t]


# The playbook executor aligns the bot to the entry's FIRST recorded state before
# replaying.  The measured recipe for this map family (see the q2dm2 playbook
# pilot) is to anchor from a STANDSTILL -- speed 0-80 -- with pos_tol 40; an
# anchor taken mid-run at ~300ups is a state the bot can essentially never
# reproduce, so every attempt burns an align and aborts.  The first cut of this
# miner started each take at the goal_item commit, which is mid-motion by
# construction, and the A/B was brutal: solo pickups -53%, aborts exceeding
# engagements.  So: rewind each take to its last standstill.
ANCHOR_MAX_SPEED = 80.0

def anchor_at_standstill(seg, t1):
    """Trim a take back to start at the latest standstill frame, or reject it."""
    best = None
    for i, r in enumerate(seg):
        if r.get("spd", 0.0) <= ANCHOR_MAX_SPEED and t1 - r["t"] >= MIN_TAKE_SECS:
            best = i
    if best is None:
        return None
    return seg[best:]


def score_take(item, bot, slot, t0, t1, seg):
    """Clean, fast, minimal correction -- the plan's three criteria, measured.

    * fast     -> mean 2D speed over the take
    * clean    -> fraction of frames actually moving (no stalls mid-traversal)
    * minimal  -> few large yaw corrections per second (a take full of
      correction   re-aiming is the bot fighting its own steering, which replays
                   badly)
    """
    dist = 0.0
    for a, b in zip(seg, seg[1:]):
        dist += math.dist((a["x"], a["y"], a["z"]), (b["x"], b["y"], b["z"]))
    if dist < MIN_TAKE_DIST:
        return None
    dur = t1 - t0
    speeds = [r.get("spd", 0.0) for r in seg]
    mean_spd = sum(speeds) / len(speeds)
    moving = sum(1 for s in speeds if s > 60.0) / len(speeds)

    # total absolute yaw travel per second.  A threshold-crossing count reads 0
    # for a bot: bot_turnrate slew-limits the view, so no single 40Hz frame ever
    # jumps far.  Cumulative turn rate does discriminate -- a take that re-aims
    # constantly racks it up, a clean traversal does not.
    turn = 0.0
    for a, b in zip(seg, seg[1:]):
        turn += abs((b.get("yaw", 0.0) - a.get("yaw", 0.0) + 180.0) % 360.0 - 180.0)
    corr_rate = turn / max(1e-6, dur)

    jumps = sum(1 for r in seg if r.get("up", 0) > 0)

    # higher is better; the weights are a starting point, deliberately simple
    quality = (mean_spd / 300.0) * moving / (1.0 + corr_rate / 90.0)

    return {
        "item": item, "bot": bot, "slot": slot,
        "start": round(t0, 2), "end": round(t1, 2), "dur": round(dur, 2),
        "dist": round(dist), "mean_spd": round(mean_spd),
        "moving": round(moving, 2), "corr_rate": round(corr_rate, 2),
        "jumps": jumps, "quality": round(quality, 3),
    }


def run_rig(args):
    """Run the standard rig with bot_cmdlog on, keeping worker dirs."""
    cmd = [sys.executable, os.path.join(HERE, "run_parallel.py"),
           "--repro", "--fastsim", "--instances", str(args.instances),
           "--seconds", str(args.seconds), "--bots", str(args.bots),
           "--seed", str(args.seed), "--map", args.map,
           "--cvar", "sv_fps", "40", "--cvar", "bot_cmdlog", "1",
           "--keep", "--no-analyze"]
    print("[selfplay] " + " ".join(cmd[1:]), flush=True)
    subprocess.check_call(cmd)


def main(argv):
    ap = argparse.ArgumentParser(
        description="Mine the bot's own successful traversals into playbook takes.")
    ap.add_argument("--map", default="q2dm1")
    ap.add_argument("--logs", default=None,
                    help="glob of PER-WORKER logs (default: engine/<mod>_w*/logs/<map>_*.jsonl)")
    ap.add_argument("--run", action="store_true", help="run the rig first (bot_cmdlog 1, --keep)")
    ap.add_argument("--instances", type=int, default=8)
    ap.add_argument("--seconds", type=float, default=90.0)
    ap.add_argument("--bots", type=int, default=5)
    ap.add_argument("--seed", type=int, default=700)
    ap.add_argument("--mod", default="ozbotre")
    ap.add_argument("--top-n", type=int, default=6, help="worst-N items to target")
    ap.add_argument("--per-item", type=int, default=1, help="takes to bake per item")
    ap.add_argument("--bake", default=None, metavar="PBK",
                    help="append the best takes to this .pbk via make_playbook.py")
    ap.add_argument("--pos-tol", type=float, default=40.0,
                    help="anchor tolerance passed to make_playbook (40 = the "
                         "q2dm1/q2dm2 standstill-anchor recipe)")
    ap.add_argument("--items", default=None,
                    help="comma-separated item names to bake (default: all targets)")
    ap.add_argument("--json", default=None)
    args = ap.parse_args(argv[1:])

    if args.run:
        run_rig(args)

    pattern = args.logs or os.path.join(
        REPO, "engine", f"{args.mod}_w*", "logs", f"{args.map}_*.jsonl")
    paths = sorted(glob.glob(pattern))
    if not paths:
        sys.exit(f"FAIL: no per-worker logs matched {pattern}\n"
                 f"      (run with --run, or point --logs at kept worker dirs)")

    all_takes, gv, att, pk, gated = [], defaultdict(int), defaultdict(int), defaultdict(int), set()
    n_inputs = 0
    for p in paths:
        ticks, events, inputs, reach = load(p)
        if not inputs:
            continue
        n_inputs += len(inputs)
        targets, g, a, k, gt = target_items(events, reach, args.top_n)
        for d, src in ((gv, g), (att, a), (pk, k)):
            for key, val in src.items():
                d[key] += val
        gated |= gt
        id2slot = map_id_to_slot(ticks, inputs)
        all_takes.extend(extract_takes(events, inputs, id2slot, set(targets), p))

    if not n_inputs:
        sys.exit("FAIL: logs contain no input records -- was bot_cmdlog 1 set?")

    # rebuild the global target list from pooled counts
    scored = sorted(((n, gv[i] / att[i] if att[i] else 0, i) for i, n in gv.items()),
                    reverse=True)
    targets = []
    for it in [s[2] for s in scored[:args.top_n]] + sorted(g for g in gated if g):
        if it not in targets:
            targets.append(it)

    print(f"[selfplay] {len(paths)} worker logs, {n_inputs} input records")
    print(f"[selfplay] targets (worst-{args.top_n} by giveups + gated):")
    for it in targets:
        print(f"    {it:<22} giveups {gv[it]:>4}  attempts {att[it]:>4}  "
              f"pickups {pk[it]:>4}" + ("   [gated]" if it in gated else ""))

    by_item = defaultdict(list)
    for t in all_takes:
        by_item[t["item"]].append(t)
    for v in by_item.values():
        v.sort(key=lambda t: -t["quality"])

    print(f"\n[selfplay] {len(all_takes)} candidate takes on target items:")
    print(f"{'item':<22}{'takes':>6}{'best q':>8}{'dur':>7}{'spd':>6}"
          f"{'moving':>8}{'deg/s':>8}{'jumps':>6}")
    for it in targets:
        v = by_item.get(it, [])
        if not v:
            print(f"{it:<22}{0:>6}      --   (no successful completion recorded)")
            continue
        b = v[0]
        print(f"{it:<22}{len(v):>6}{b['quality']:>8.3f}{b['dur']:>7.2f}"
              f"{b['mean_spd']:>6}{b['moving']:>8.2f}{b['corr_rate']:>8.2f}{b['jumps']:>6}")

    baked = []
    if args.bake:
        want = [x.strip() for x in args.items.split(",")] if args.items else targets
        for it in want:
            for i, tk in enumerate(by_item.get(it, [])[:args.per_item]):
                name = "sp_" + "".join(c if c.isalnum() else "_" for c in it).lower()
                if i:
                    name += f"_{i + 1}"
                cmd = [sys.executable, os.path.join(HERE, "make_playbook.py"),
                       tk["src"], "--slot", str(tk["slot"]),
                       "--start", str(tk["start"]), "--end", str(tk["end"]),
                       "--name", name, "--out", args.bake,
                       "--pos-tol", str(args.pos_tol)]
                print("[selfplay] bake: " + " ".join(cmd[2:]))
                subprocess.check_call(cmd)
                baked.append(name)
        print(f"[selfplay] baked {len(baked)} entries into {args.bake}")
        print("[selfplay] NEXT: A/B the .pbk (with vs without) before shipping it; "
              "watch pb_* telemetry for aborts -- >30% abort is the kill criterion.")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fp:
            json.dump({"map": args.map, "targets": targets,
                       "giveups": dict(gv), "attempts": dict(att),
                       "pickups": dict(pk), "gated": sorted(gated),
                       "takes": all_takes, "baked": baked}, fp, indent=1)
        print(f"[selfplay] wrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

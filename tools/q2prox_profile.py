#!/usr/bin/env python3
"""
q2prox_profile.py -- one movement/aim yardstick for three sources:

  q2prox  OpenTDM-X "q2pro-x" bot recordings (spectator .dm2, demos/q2prox/):
          every moving player from packet entities (origin + 8-bit entity angles).
          OpenTDM's chasecam is NOT first-person, so playerinfo is never used.
  pros    human .dm2 corpus (demos/sorted/<map>/): the recorder from playerinfo
          (exact origin + 16-bit view), the opponent from packet entities.
  bot     ozbot tick telemetry (run_parallel merged .jsonl): every bot, every
          tick; 40Hz logs are decimated to 10Hz so every rate is per 0.1s step.

All three are reduced to the same frame model (10Hz frames of
{player: (origin, (pitch, yaw) or None, alive)}) and run through the SAME
metric code.  A .dm2 only carries entities inside the viewer's PVS; bot
telemetry knows everything, so the aim metrics PVS-filter bot targets with the
map's own vis data (tools/q2bsp.py) to make "hidden" mean the same thing.

    py tools/q2prox_profile.py q2prox demos/q2prox/*.dm2 --json out/q2prox.json
    py tools/q2prox_profile.py pros demos/sorted/q2duel5 --n 60 --json out/pros.json
    py tools/q2prox_profile.py bot engine/ozbotre/logs/parallel_q2duel5_*.jsonl --json out/bot.json
    py tools/q2prox_profile.py report out/q2prox.json out/pros.json out/bot.json

Metric definitions (all on 10Hz steps, dead/teleport/gap steps excluded):
  speed      horizontal speed percentiles; share above run speed (>330), >400,
             still (<20); share of steps with |vz| > 50 (airborne-ish)
  arcs       airborne arcs (>=3 steps of |dz|>1, starting upward at >200u/s):
             horizontal speed gained start->end, share of arcs that gain,
             peak speed in arc.  The strafe-jump quality signal.
  dither     5s windows that travel >=700u but never get >160u from their start
             (revisit-based; heading-reversal counts are ~all false positives)
  view       yaw rate percentiles while moving (entity angles quantise to 1.4deg)
  aim        per shooter->target pair, target in shooter's PVS:
             error to target chest while VISIBLE (BSP line of sight), share
             <5/<15deg; same while hidden; error at visibility onset (after
             >=0.5s unseen) and time to get within 10deg; yaw rate while visible
History: built for the 2026-10-13 q2pro-x study (see the ozbot-re-q2prox-study
memory for the first-session numbers, whose raw demos were lost to a reboot).
"""

import argparse
import glob
import json
import math
import os
import random
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dm2parse                      # noqa: E402
import dm2_combat as dc              # noqa: E402
from q2bsp import BSP                # noqa: E402

DT = 0.1
EYE, CHEST = 22.0, 8.0
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# ---------------------------------------------------------------- dm2 decoding
class _EntState:
    """Packet-entity decoder that keeps angles as well as origins (dm2_combat's
    decoder is origin-only).  Field order is protocol 34 CL_ParseDelta."""

    def __init__(self):
        self.origins, self.angles = {}, {}

    def _delta(self, r, num, bits):
        if bits & dc.U_MODEL:  r.u8()
        if bits & dc.U_MODEL2: r.u8()
        if bits & dc.U_MODEL3: r.u8()
        if bits & dc.U_MODEL4: r.u8()
        if bits & dc.U_FRAME8:  r.u8()
        if bits & dc.U_FRAME16: r.u16()
        if (bits & dc.U_SKIN8) and (bits & dc.U_SKIN16): r.s32()
        elif bits & dc.U_SKIN8:  r.u8()
        elif bits & dc.U_SKIN16: r.u16()
        if (bits & dc.U_EFFECTS8) and (bits & dc.U_EFFECTS16): r.s32()
        elif bits & dc.U_EFFECTS8:  r.u8()
        elif bits & dc.U_EFFECTS16: r.u16()
        if (bits & dc.U_RENDERFX8) and (bits & dc.U_RENDERFX16): r.s32()
        elif bits & dc.U_RENDERFX8:  r.u8()
        elif bits & dc.U_RENDERFX16: r.u16()
        o = self.origins.setdefault(num, [0.0, 0.0, 0.0])
        if bits & dc.U_ORIGIN1: o[0] = r.s16() / 8.0
        if bits & dc.U_ORIGIN2: o[1] = r.s16() / 8.0
        if bits & dc.U_ORIGIN3: o[2] = r.s16() / 8.0
        a = self.angles.setdefault(num, [0.0, 0.0, 0.0])
        if bits & dc.U_ANGLE1: a[0] = r.u8() * 360.0 / 256
        if bits & dc.U_ANGLE2: a[1] = r.u8() * 360.0 / 256
        if bits & dc.U_ANGLE3: a[2] = r.u8() * 360.0 / 256
        if bits & dc.U_OLDORIGIN: r.skip(6)
        if bits & dc.U_SOUND: r.u8()
        if bits & dc.U_EVENT: r.u8()
        if bits & dc.U_SOLID: r.u16()

    def baseline(self, r):
        bits, num = dc._read_ent_bits(r)
        self._delta(r, num, bits)

    def packet(self, r):
        while True:
            bits, num = dc._read_ent_bits(r)
            if num == 0:
                return
            if num >= dc.MAX_EDICTS:
                raise ValueError("bad entity number")
            if bits & dc.U_REMOVE:
                self.origins.pop(num, None)
                self.angles.pop(num, None)
                continue
            self._delta(r, num, bits)


def _ent_view(a):
    """Player entity angles -> (pitch, yaw).  The server stores v_angle pitch/3
    in s.angles[PITCH] for player models (p_view.c)."""
    p = a[0] if a[0] < 180 else a[0] - 360
    return (p * 3.0, a[1])


def decode_dm2(path):
    """-> dict(names, playernum, frames=[{sf, rec_o, rec_va, rec_hp, ents{num:(o,a)}}])"""
    data = open(path, "rb").read()
    names, frames = {}, []
    es, stats = _EntState(), [0] * 32
    playernum, rec_o, rec_va = None, None, None
    for block in dm2parse.iter_blocks(data):
        if not block:
            continue
        r = dm2parse.Reader(block)
        while not r.eob():
            cmd = r.u8()
            if cmd == dc.SVC_CONFIGSTRING:
                idx = r.u16()
                s = r.string()
                if dc.CS_PLAYERSKINS <= idx < dc.CS_PLAYERSKINS + 256:
                    names[idx - dc.CS_PLAYERSKINS] = s.split("\\")[0]
            elif cmd == dc.SVC_SERVERDATA:
                r.s32(); r.s32(); r.u8(); r.string()
                playernum = r.s16()
                r.string()
            elif cmd == dc.SVC_PRINT:
                r.u8(); r.string()
            elif cmd in (dc.SVC_STUFFTEXT, dc.SVC_CENTERPRINT, dc.SVC_LAYOUT):
                r.string()
            elif cmd == dc.SVC_INVENTORY:
                r.skip(512)
            elif cmd == dc.SVC_NOP:
                continue
            elif cmd == dc.SVC_SPAWNBASELINE:
                try:
                    es.baseline(r)
                except (IndexError, ValueError):
                    break
            elif cmd == dc.SVC_FRAME:
                sf = r.s32(); r.s32(); r.u8(); r.skip(r.u8())
                if r.u8() != dc.SVC_PLAYERINFO:
                    break
                o, va, _wi, _ve = dc.read_playerinfo(r, stats)
                if o is not None:
                    rec_o = (o[0] / 8.0, o[1] / 8.0, o[2] / 8.0)
                if va is not None:
                    rec_va = (va[0], va[1])
                try:
                    if r.u8() == dc.SVC_PACKETENTITIES:
                        es.packet(r)
                except (IndexError, ValueError):
                    pass
                ents = {n: (tuple(es.origins[n]), tuple(es.angles.get(n, (0, 0, 0))))
                        for n in es.origins if 1 <= n <= 64 and (n - 1) in names}
                frames.append(dict(sf=sf, rec_o=rec_o, rec_va=rec_va,
                                   rec_hp=stats[dc.STAT_HEALTH], ents=ents))
                break
            else:
                break
    return dict(names=names, playernum=playernum, frames=frames)


# --------------------------------------------------------- unified frame model
# A "session" = list of 10Hz frames, each {player: (origin, view|None, alive)},
# plus `players` (who to profile) and `pairs` (shooter, target) for aim.

def session_q2prox(path):
    x = decode_dm2(path)
    names = x["names"]
    out, last = [], {}
    for f in x["frames"]:
        fr = {}
        for n, (o, a) in f["ents"].items():
            nm = names[n - 1]
            if nm == names.get(x["playernum"]):
                continue                     # ourselves (the spectator)
            # a spectating bot parks at the origin; a dead body stops moving --
            # 'alive' = moved recently or not parked at (0,0,0)
            alive = o != (0.0, 0.0, 0.0)
            fr[nm] = (o, _ent_view(a), alive)
        out.append(fr)
    # players worth profiling: ones that actually travel
    span = defaultdict(float)
    for a, b in zip(out, out[1:]):
        for nm in a.keys() & b.keys():
            if a[nm][2] and b[nm][2]:
                d = math.dist(a[nm][0][:2], b[nm][0][:2])
                if d < 80:
                    span[nm] += d
    players = [nm for nm, s in span.items() if s > 3000]
    pairs = [(a, b) for a in players for b in players if a != b]
    return dict(src="q2prox", file=os.path.basename(path), frames=out,
                players=players, pairs=pairs, filtered_pvs=True)


def session_pro(path):
    x = decode_dm2(path)
    names, me = x["names"], x["names"].get(x["playernum"])
    # the opponent = the other client entity present in the most frames
    # (spectators are not sent as player entities, but are in the names table)
    seen = defaultdict(int)
    for f in x["frames"]:
        for n in f["ents"]:
            if n - 1 != x["playernum"]:
                seen[n] += 1
    if not me or not seen:
        return None
    oe = max(seen, key=seen.get)
    if seen[oe] < 300:
        return None
    opp = [names[oe - 1]]
    out = []
    for f in x["frames"]:
        fr = {}
        if f["rec_o"] is not None:
            fr[me] = (f["rec_o"], f["rec_va"], f["rec_hp"] > 0)
        if oe in f["ents"]:
            o, a = f["ents"][oe]
            fr[opp[0]] = (o, _ent_view(a), True)
        out.append(fr)
    # movement from the recorder (exact); aim from the opponent's entity at the
    # recorder (both humans; this is the direction measured in the first study)
    return dict(src="pros", file=os.path.basename(path), frames=out,
                players=[me], pairs=[(opp[0], me)], filtered_pvs=True)


def sessions_bot(path, stride=1000):
    """One session per SERVER in a run_parallel merged log.  The merge offsets
    bot ids by worker*BOT_ID_STRIDE but every worker shares the same clock, so
    the worker (= bot // stride) must split sessions or aim pairs would cross
    servers.  40Hz -> every 4th tick."""
    ticks = defaultdict(lambda: defaultdict(dict))   # worker -> 40Hz tick -> {name: ...}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if '"tick"' not in line:
                continue
            try:
                r = json.loads(line)
            except ValueError:
                continue
            if r.get("type") != "tick" or "pitch" not in r:
                continue
            key = int(round(r["t"] * 40))          # 40Hz tick index (10Hz logs land every 4th)
            nm = f'{r.get("name", "bot")}#{r["bot"]}'
            ticks[r["bot"] // stride][key][nm] = (
                (r["x"], r["y"], r["z"]), (r["pitch"], r["yaw"]),
                not r.get("dead", False) and r.get("health", 1) > 0)
    out = []
    for w, tk in sorted(ticks.items()):
        keys = sorted(tk)
        frames = [tk.get(k, {}) for k in range(keys[0], keys[-1] + 1, 4)]
        players = sorted({nm for f in frames for nm in f})
        out.append(dict(src="bot", file=f"{os.path.basename(path)}#w{w}", frames=frames,
                        players=players,
                        pairs=[(a, b) for a in players for b in players if a != b],
                        filtered_pvs=False))
    return out


# --------------------------------------------------------------- metric pieces
def tracks(session, nm):
    """Continuous alive runs of one player's origins, broken on gaps/teleports."""
    out, cur = [], []
    for f in session["frames"]:
        e = f.get(nm)
        if e and e[2] and (not cur or math.dist(e[0], cur[-1][0]) <= 80):
            cur.append((e[0], e[1]))
        else:
            if len(cur) >= 20:
                out.append(cur)
            cur = [(e[0], e[1])] if e and e[2] else []
    if len(cur) >= 20:
        out.append(cur)
    return out


class Acc:
    """Accumulates raw samples across sessions for one source."""

    def __init__(self):
        self.s = defaultdict(list)
        self.n = defaultdict(int)

    def movement(self, trk):
        pts = [p for p, _ in trk]
        views = [v for _, v in trk]
        steps = []
        for i in range(1, len(pts)):
            h = math.hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]) / DT
            dz = pts[i][2] - pts[i - 1][2]
            steps.append((h, dz))
            self.s["speed"].append(h)
            self.n["steps"] += 1
            self.n["fast330"] += h > 330
            self.n["fast400"] += h > 400
            self.n["still"] += h < 20
            self.n["vz50"] += abs(dz / DT) > 50
            if views[i] and views[i - 1] and h > 20:
                d = (views[i][1] - views[i - 1][1] + 180) % 360 - 180
                self.s["yawrate_moving"].append(abs(d) / DT)
        # airborne arcs
        seg = []

        def flush():
            if len(seg) >= 3 and seg[0][1] > 2 and seg[0][0] > 200:
                self.s["arc_gain"].append(seg[-1][0] - seg[0][0])
                self.s["arc_peak"].append(max(h for h, _ in seg))
        for h, dz in steps:
            if abs(dz) > 1.0:
                seg.append((h, dz))
            else:
                flush()
                seg = []
        flush()
        # dithering, 5s windows every 0.5s
        W = 50
        for i in range(0, len(pts) - W, 5):
            w = pts[i:i + W]
            pl = sum(math.dist(w[j][:2], w[j + 1][:2]) for j in range(W - 1))
            if pl < 400:
                continue
            self.n["dither_win"] += 1
            far = max(math.dist(w[0][:2], p[:2]) for p in w)
            self.n["dither"] += pl >= 700 and far < 160

    def aim(self, session, sh, tg, bsp):
        rows = []
        for i, f in enumerate(session["frames"]):
            a, b = f.get(sh), f.get(tg)
            if not a or not b or not a[2] or not b[2] or a[1] is None:
                continue
            eye = (a[0][0], a[0][1], a[0][2] + EYE)
            teye = (b[0][0], b[0][1], b[0][2] + EYE)
            tch = (b[0][0], b[0][1], b[0][2] + CHEST)
            if not session["filtered_pvs"] and not bsp.pvs_box(eye, b[0]):
                continue                     # a dm2 would not have carried it
            vis = bsp.los(eye, teye) or bsp.los(eye, tch)
            err, dist = _ang_err(eye, a[1], tch)
            rows.append((i, err, dist, vis, a[1][1]))
        last_vis = -999
        for k, (i, e, d, v, y) in enumerate(rows):
            if d <= 64:
                continue
            self.s["err_vis" if v else "err_hid"].append(e)
            if v and i - last_vis >= 5:
                self.s["onset_err"].append(e)
                for (j, e2, _d2, v2, _y2) in rows[k:k + 30]:
                    if j - i > 30 or not v2:
                        break
                    if e2 < 10:
                        self.s["react"].append((j - i) * DT)
                        break
            if v:
                last_vis = i
        for (i, _e, _d, v, y), (j, _e2, _d2, v2, y2) in zip(rows, rows[1:]):
            if j == i + 1 and v and v2:
                self.s["yawrate_vis"].append(abs((y2 - y + 180) % 360 - 180) / DT)

    def summary(self, label, extra=None):
        S, N = self.s, self.n

        def p(k, qs):
            a = sorted(S[k])
            return [round(a[min(len(a) - 1, int(q / 100 * len(a)))], 1) for q in qs] if a else None

        def frac(k, f):
            a = S[k]
            return round(sum(f(x) for x in a) / len(a), 3) if a else None
        st = max(1, N["steps"])
        return dict(
            label=label, **(extra or {}),
            seconds_moving=round(N["steps"] * DT),
            speed_p10_50_90_99=p("speed", (10, 50, 90, 99)),
            share_gt330=round(N["fast330"] / st, 3), share_gt400=round(N["fast400"] / st, 3),
            share_still=round(N["still"] / st, 3), share_vz50=round(N["vz50"] / st, 3),
            arcs=len(S["arc_gain"]), arc_gain_p25_50_75_90=p("arc_gain", (25, 50, 75, 90)),
            arcs_gaining=frac("arc_gain", lambda g: g > 0),
            arc_peak_p50_90_99=p("arc_peak", (50, 90, 99)),
            dither_windows=N["dither_win"],
            dither_share=round(N["dither"] / max(1, N["dither_win"]), 4),
            yawrate_moving_p50_90_99=p("yawrate_moving", (50, 90, 99)),
            aim_visible_frames=len(S["err_vis"]),
            err_vis_p10_25_50_75=p("err_vis", (10, 25, 50, 75)),
            vis_lt5=frac("err_vis", lambda e: e < 5), vis_lt15=frac("err_vis", lambda e: e < 15),
            aim_hidden_frames=len(S["err_hid"]),
            err_hid_p10_25_50=p("err_hid", (10, 25, 50)),
            hid_lt15=frac("err_hid", lambda e: e < 15),
            onsets=len(S["onset_err"]), onset_err_p50=p("onset_err", (50,)),
            onset_lt15=frac("onset_err", lambda e: e < 15),
            react_s_p25_50_75=p("react", (25, 50, 75)),
            yawrate_vis_p50_90_99=p("yawrate_vis", (50, 90, 99)),
        )


def _ang_err(eye, view, tgt):
    pitch, yaw = math.radians(view[0]), math.radians(view[1])
    v = (math.cos(pitch) * math.cos(yaw), math.cos(pitch) * math.sin(yaw), -math.sin(pitch))
    d = (tgt[0] - eye[0], tgt[1] - eye[1], tgt[2] - eye[2])
    L = math.sqrt(sum(c * c for c in d)) or 1.0
    c = max(-1.0, min(1.0, sum(a * b / L for a, b in zip(v, d))))
    return math.degrees(math.acos(c)), L


def profile(sessions, bsp, label, per_player=False):
    acc, per = Acc(), defaultdict(Acc)
    for s in sessions:
        for nm in s["players"]:
            for t in tracks(s, nm):
                acc.movement(t)
                if per_player:
                    per[nm].movement(t)
        for sh, tg in s["pairs"]:
            acc.aim(s, sh, tg, bsp)
            if per_player:
                per[sh].aim(s, sh, tg, bsp)
    out = acc.summary(label, dict(sessions=len(sessions)))
    if per_player:
        out["per_player"] = {nm: a.summary(nm) for nm, a in sorted(per.items())}
    return out


# ------------------------------------------------------------------- commands
def _bsp_for(mapname, explicit):
    path = explicit or os.path.join(ROOT, "engine", "baseq2", "maps", f"{mapname}.bsp")
    return BSP(path)


def _pool_map(fn, items, procs=10):
    from multiprocessing import Pool
    with Pool(procs) as pl:
        return pl.map(fn, items)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["q2prox", "pros", "bot", "report"])
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("--map", default="q2duel5")
    ap.add_argument("--bsp", default=None)
    ap.add_argument("--n", type=int, default=60, help="pros: demos sampled (seeded)")
    ap.add_argument("--seed", type=int, default=11)
    ap.add_argument("--json", default=None)
    ap.add_argument("--per-player", action="store_true")
    a = ap.parse_args()

    if a.mode == "report":
        report([json.load(open(p)) for p in a.inputs])
        return
    bsp = _bsp_for(a.map, a.bsp)
    if a.mode == "q2prox":
        files = sorted(f for g in a.inputs for f in glob.glob(g))
        sessions = _pool_map(session_q2prox, files)
        out = profile(sessions, bsp, "q2pro-x", per_player=a.per_player)
    elif a.mode == "pros":
        files = sorted(glob.glob(os.path.join(a.inputs[0], "*.dm2")))
        random.seed(a.seed)
        files = random.sample(files, min(a.n, len(files)))
        sessions = [s for s in _pool_map(session_pro, files) if s]
        out = profile(sessions, bsp, f"pros ({len(sessions)} demos)")
    else:
        files = sorted(f for g in a.inputs for f in glob.glob(g))
        sessions = [s for f in files for s in sessions_bot(f)]
        out = profile(sessions, bsp, "ozbot", per_player=a.per_player)
    print(json.dumps({k: v for k, v in out.items() if k != "per_player"}, indent=1))
    if a.json:
        os.makedirs(os.path.dirname(os.path.abspath(a.json)), exist_ok=True)
        json.dump(out, open(a.json, "w"), indent=1)


ROWS = [
    ("seconds moving", "seconds_moving"),
    ("speed p10/50/90/99", "speed_p10_50_90_99"),
    ("share >330 u/s", "share_gt330"), ("share >400 u/s", "share_gt400"),
    ("share still", "share_still"), ("share |vz|>50", "share_vz50"),
    ("arcs", "arcs"), ("arc gain p25/50/75/90", "arc_gain_p25_50_75_90"),
    ("arcs gaining", "arcs_gaining"), ("arc peak p50/90/99", "arc_peak_p50_90_99"),
    ("dither share", "dither_share"),
    ("yaw rate moving p50/90/99", "yawrate_moving_p50_90_99"),
    ("aim: visible frames", "aim_visible_frames"),
    ("aim: err visible p10/25/50/75", "err_vis_p10_25_50_75"),
    ("aim: visible <5 / <15", ("vis_lt5", "vis_lt15")),
    ("aim: hidden <15", "hid_lt15"),
    ("aim: onsets", "onsets"), ("aim: onset err p50", "onset_err_p50"),
    ("aim: on target at onset (<15)", "onset_lt15"),
    ("aim: react s p25/50/75", "react_s_p25_50_75"),
    ("aim: yaw rate visible p50/90/99", "yawrate_vis_p50_90_99"),
]


def report(profiles):
    def fmt(v):
        if isinstance(v, list):
            return "/".join(f"{x:g}" for x in v)
        return "-" if v is None else f"{v:g}"
    hdr = ["metric"] + [p["label"] for p in profiles]
    print("| " + " | ".join(hdr) + " |")
    print("|" + "---|" * len(hdr))
    for lab, key in ROWS:
        cells = []
        for p in profiles:
            cells.append(" / ".join(fmt(p.get(k)) for k in key) if isinstance(key, tuple) else fmt(p.get(key)))
        print(f"| {lab} | " + " | ".join(cells) + " |")


if __name__ == "__main__":
    main()

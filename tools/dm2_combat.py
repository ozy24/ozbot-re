#!/usr/bin/env python3
"""
Extract combat/timing signal from pro .dm2 demos -- aim turn-rate, weapon
usage at kill time, and item-pickup patterns (what/when/at-what-health) --
to calibrate bot_combat.c / bot_goal.c parameters against real player
behavior. Movement/nav data from demos was already tried and found not to
transfer (see the ozbot-demo-import-finding memory: pro strafe-jumps/momentum
the bot's simple movement can't reproduce); this deliberately stays
movement-agnostic per that finding's own recommendation.

Reads demos straight out of the zip archives in demos/raw/ (never extracts or
modifies that directory -- same pattern as demo_to_nav.py / demo_coverage.py).

Usage:
    python dm2_combat.py scan [mapname] [--limit N]   -> aggregate stats
    python dm2_combat.py need [mapname] [--limit N]   -> resource-need thresholds
                                                          (all maps if mapname omitted)
    python dm2_combat.py tactics [mapname] [--limit N] -> per-weapon engagement
                                                          range + style (decodes
                                                          opponent positions)
    python dm2_combat.py one <demo.dm2>                -> single-demo debug dump

The `need` subcommand mines the human "resource need" curves the bot's goal
scorer wants (bot_goal.c Item_Score): at what health/armor/ammo level do pros
actually detour to pick a thing up.  It writes durable calibration targets to
demos/derived/combat_need/thresholds.json.
"""

import glob
import json
import math
import os
import re
import struct
import sys
import zipfile
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dm2parse
from dm2parse import Reader, iter_blocks

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RAW = os.path.join(ROOT, "demos", "raw")

# svc ids (protocol 34) not already needed by dm2parse
SVC_PRINT = 10
SVC_CONFIGSTRING = 13
SVC_SPAWNBASELINE = 14
SVC_FRAME = 20
SVC_PLAYERINFO = 17
SVC_SERVERDATA = 12
SVC_STUFFTEXT, SVC_CENTERPRINT, SVC_LAYOUT = 11, 15, 4
SVC_INVENTORY = 5
SVC_DOWNLOAD = 16
SVC_DISCONNECT, SVC_RECONNECT = 7, 8
SVC_NOP = 6

PS_M_TYPE, PS_M_ORIGIN, PS_M_VELOCITY = 1, 2, 4
PS_M_TIME, PS_M_FLAGS, PS_M_GRAVITY, PS_M_DELTA_ANGLES = 8, 16, 32, 64
PS_VIEWOFFSET, PS_VIEWANGLES, PS_KICKANGLES = 128, 256, 512
PS_BLEND, PS_FOV, PS_WEAPONINDEX, PS_WEAPONFRAME, PS_RDFLAGS = 1024, 2048, 4096, 8192, 16384

STAT_HEALTH, STAT_AMMO, STAT_ARMOR = 1, 3, 5
STAT_PICKUP_ICON, STAT_PICKUP_STRING, STAT_SELECTED_ITEM, STAT_FRAGS = 7, 8, 12, 14

SVC_PACKETENTITIES = 18

# entity-delta update bits (protocol 34, quake2 qcommon/qcommon.h U_*).  Used by
# the `tactics` subcommand to decode opponent origins from the packet-entities
# section (for per-weapon engagement RANGE); ignored by scan/need.
U_ORIGIN1, U_ORIGIN2, U_ANGLE2, U_ANGLE3 = 1 << 0, 1 << 1, 1 << 2, 1 << 3
U_FRAME8, U_EVENT, U_REMOVE, U_MOREBITS1 = 1 << 4, 1 << 5, 1 << 6, 1 << 7
U_NUMBER16, U_ORIGIN3, U_ANGLE1, U_MODEL = 1 << 8, 1 << 9, 1 << 10, 1 << 11
U_RENDERFX8, U_EFFECTS8, U_MOREBITS2 = 1 << 12, 1 << 14, 1 << 15
U_SKIN8, U_FRAME16, U_RENDERFX16, U_EFFECTS16 = 1 << 16, 1 << 17, 1 << 18, 1 << 19
U_MODEL2, U_MODEL3, U_MODEL4, U_MOREBITS3 = 1 << 20, 1 << 21, 1 << 22, 1 << 23
U_OLDORIGIN, U_SKIN16, U_SOUND, U_SOLID = 1 << 24, 1 << 25, 1 << 26, 1 << 27
MAX_EDICTS = 1024

CS_ITEMS = 1056     # = CS_LIGHTS + MAX_LIGHTSTYLES, per dm2parse's CS_PLAYERSKINS comment
CS_MODELS = 32
CS_PLAYERSKINS = 1312
MAX_CLIENTS = 256

VIEWMODEL_WEAPON = {
    "v_blast": "blaster", "v_shotg": "shotgun", "v_shotg2": "super shotgun",
    "v_machn": "machinegun", "v_chain": "chaingun", "v_handgr": "grenade launcher",
    "v_launch": "grenade launcher", "v_rocket": "rocket launcher",
    "v_hyperb": "hyperblaster", "v_rail": "railgun", "v_bfg": "bfg10k",
}

# ammo type each weapon consumes (canonical weapon names as resolve_weapon emits
# them).  Used to bucket "how much ammo did they have" by the ammo type that
# matters -- absolute counts are only comparable within a type (rockets cap 50,
# cells cap 200).  Blaster has no ammo.
WEAPON_AMMO = {
    "shotgun": "shells", "super shotgun": "shells",
    "machinegun": "bullets", "chaingun": "bullets",
    "grenade launcher": "grenades", "rocket launcher": "rockets",
    "hyperblaster": "cells", "bfg10k": "cells",
    "railgun": "slugs",
}

# substring -> ammo type, for classifying an ammo PICKUP item by its name
AMMO_ITEM_TYPE = [
    ("Shells", "shells"), ("Bullets", "bullets"), ("Cells", "cells"),
    ("Rockets", "rockets"), ("Slugs", "slugs"), ("Grenades", "grenades"),
]


def item_category(name):
    """Coarse item class from its configstring pickup name -- mirrors the
    vocabulary of bot_goal.c Item_BaseValue so the analysis and the bot agree."""
    n = name.lower()
    # ammo first: "Grenades" is ammo, the "Grenade Launcher" weapon is caught below
    for sub, _ in AMMO_ITEM_TYPE:
        if sub.lower() in n and "launcher" not in n:
            return "ammo"
    if "armor" in n or "shard" in n:
        return "armor"
    if any(w in n for w in (
            "shotgun", "machinegun", "chaingun", "launcher", "hyperblaster",
            "railgun", "bfg", "blaster", "grenade launcher")):
        return "weapon"
    if any(p in n for p in (
            "quad", "invulnerability", "silencer", "rebreather", "environment",
            "adrenaline", "bandolier", "pack", "power shield", "power screen")):
        return "powerup"
    if "health" in n or "mega" in n or "stimpack" in n or "medkit" in n:
        return "health"
    return "other"


def ammo_item_type(name):
    """Ammo type of an ammo PICKUP item (or None)."""
    n = name.lower()
    if "launcher" in n:
        return None
    for sub, atype in AMMO_ITEM_TYPE:
        if sub.lower() in n:
            return atype
    return None

# obituary vocabulary, from p_client.c's Obituary() -- "%s %s %s%s\n".
# Matched by substring search (see parse_obituary), not a generic regex --
# a loose regex over free-form print text false-positives constantly (matched
# HUD/ammo-warning strings in testing). message2 (weapon suffix) disambiguates
# a few shared messages; "feels ...'s pain" is a rare/unclear MOD, left "?".
OBIT_WEAPON = {
    "was blasted by": "blaster", "was gunned down by": "machinegun",
    "was blown away by": "super shotgun", "was machinegunned by": "machinegun",
    "was cut in half by": "chaingun", "was popped by": "grenade launcher",
    "was shredded by": "grenade launcher", "ate": "rocket launcher",
    "almost dodged": "rocket launcher", "was melted by": "hyperblaster",
    "was railed by": "railgun", "saw the pretty lights from": "bfg10k",
    "was disintegrated by": "bfg10k", "couldn't hide from": "bfg10k",
    "caught": "grenade launcher", "didn't see": "grenade launcher",
    "feels": "?",
    "tried to invade": "telefrag",
}
_OBIT_MSGS = sorted(OBIT_WEAPON.keys(), key=len, reverse=True)   # longest first


def parse_obituary(text):
    """Returns (victim, attacker, weapon) or None. text is the raw print line.
    Kill lines are "%s %s %s%s\n" (victim, msg, attacker, weapon-suffix) --
    NO trailing period (only the separate self-kill format has one)."""
    body = text.rstrip("\n")
    for msg in _OBIT_MSGS:
        marker = " " + msg + " "
        idx = body.find(marker)
        if idx <= 0:
            continue
        victim = body[:idx]
        rest = body[idx + len(marker):]
        apos = rest.find("'s ")
        attacker = rest[:apos] if apos >= 0 else rest
        if not victim or not attacker or " " in attacker or " " in victim:
            continue   # names are single tokens in this demo set; guards false positives
        return victim, attacker, OBIT_WEAPON[msg]
    return None


def angle16(v):
    return v * (360.0 / 65536.0)


def read_playerinfo(r, stats):
    """Decodes what dm2parse skips: viewangles, weaponindex, velocity, stats.
    Returns (origin, viewangles, weaponindex, velocity); each None if absent.
    origin is raw s16 (world = /8); velocity is world units/sec (already /8)."""
    flags = r.u16()
    origin = viewangles = weaponindex = velocity = None
    if flags & PS_M_TYPE:
        r.skip(1)
    if flags & PS_M_ORIGIN:
        origin = (r.s16(), r.s16(), r.s16())
    if flags & PS_M_VELOCITY:
        velocity = (r.s16() / 8.0, r.s16() / 8.0, r.s16() / 8.0)
    if flags & PS_M_TIME:
        r.skip(1)
    if flags & PS_M_FLAGS:
        r.skip(1)
    if flags & PS_M_GRAVITY:
        r.skip(2)
    if flags & PS_M_DELTA_ANGLES:
        r.skip(6)
    if flags & PS_VIEWOFFSET:
        r.skip(3)
    if flags & PS_VIEWANGLES:
        viewangles = (angle16(r.s16()), angle16(r.s16()), angle16(r.s16()))
    if flags & PS_KICKANGLES:
        r.skip(3)
    if flags & PS_WEAPONINDEX:
        weaponindex = r.u8()
    if flags & PS_WEAPONFRAME:
        r.skip(7)
    if flags & PS_BLEND:
        r.skip(4)
    if flags & PS_FOV:
        r.skip(1)
    if flags & PS_RDFLAGS:
        r.skip(1)
    statbits = r.u32()
    for i in range(32):
        if statbits & (1 << i):
            stats[i] = r.s16()
    return origin, viewangles, weaponindex, velocity


# ---- packet-entity (delta) decoding: opponent origins for the `tactics` scan ----

def _read_ent_bits(r):
    """Read an entity-delta header: the U_* bitmask (up to 4 bytes) + the entity
    number (byte, or short if U_NUMBER16).  Number 0 terminates the list."""
    bits = r.u8()
    if bits & U_MOREBITS1:
        bits |= r.u8() << 8
    if bits & U_MOREBITS2:
        bits |= r.u8() << 16
    if bits & U_MOREBITS3:
        bits |= r.u8() << 24
    num = r.u16() if (bits & U_NUMBER16) else r.u8()
    return bits, num


def _parse_delta_origin(r, bits):
    """Consume one entity delta's fields in protocol-34 order (CL_ParseDelta),
    returning (ox,oy,oz) world-unit coords for whichever origin components were
    sent (None for the rest).  Non-origin fields are read only to stay aligned."""
    if bits & U_MODEL:  r.u8()
    if bits & U_MODEL2: r.u8()
    if bits & U_MODEL3: r.u8()
    if bits & U_MODEL4: r.u8()
    if bits & U_FRAME8:  r.u8()
    if bits & U_FRAME16: r.u16()
    if (bits & U_SKIN8) and (bits & U_SKIN16): r.s32()
    elif bits & U_SKIN8:  r.u8()
    elif bits & U_SKIN16: r.u16()
    if (bits & U_EFFECTS8) and (bits & U_EFFECTS16): r.s32()
    elif bits & U_EFFECTS8:  r.u8()
    elif bits & U_EFFECTS16: r.u16()
    if (bits & U_RENDERFX8) and (bits & U_RENDERFX16): r.s32()
    elif bits & U_RENDERFX8:  r.u8()
    elif bits & U_RENDERFX16: r.u16()
    ox = r.s16() / 8.0 if (bits & U_ORIGIN1) else None
    oy = r.s16() / 8.0 if (bits & U_ORIGIN2) else None
    oz = r.s16() / 8.0 if (bits & U_ORIGIN3) else None
    if bits & U_ANGLE1: r.u8()
    if bits & U_ANGLE2: r.u8()
    if bits & U_ANGLE3: r.u8()
    if bits & U_OLDORIGIN: r.skip(6)
    if bits & U_SOUND: r.u8()
    if bits & U_EVENT: r.u8()
    if bits & U_SOLID: r.u16()
    return ox, oy, oz


def _apply_delta(origins, num, bits, r):
    ox, oy, oz = _parse_delta_origin(r, bits)
    cur = origins.get(num)
    if cur is None:
        cur = [0.0, 0.0, 0.0]
        origins[num] = cur
    if ox is not None: cur[0] = ox
    if oy is not None: cur[1] = oy
    if oz is not None: cur[2] = oz


def _parse_packetentities(r, origins):
    """Apply one frame's delta-entity list to the persistent origins dict.
    Deltas are incremental against the previous frame, so `origins` must persist
    across frames; unset fields keep their prior value.  U_REMOVE drops an entity."""
    while True:
        bits, num = _read_ent_bits(r)
        if num == 0:
            break
        if num >= MAX_EDICTS:
            raise ValueError("bad entity number")
        if bits & U_REMOVE:
            origins.pop(num, None)
            continue
        _apply_delta(origins, num, bits, r)


def _parse_baseline(r, origins):
    """svc_spawnbaseline = a single entity delta (same format); seed its origin."""
    bits, num = _read_ent_bits(r)
    if bits & U_REMOVE:
        return
    if 0 < num < MAX_EDICTS:
        _apply_delta(origins, num, bits, r)
    else:
        _parse_delta_origin(r, bits)   # consume fields to stay aligned


def _record_aim(info, frame_idx, last_origin, last_viewangles, weapon_idx,
                ammo, origins, opps, aim_state):
    """Log this frame's AIM geometry vs the nearest opponent: the angular error
    between the recorder's view and the exact bearing to the enemy, the range,
    and the enemy's LATERAL speed (velocity component perpendicular to the line
    of sight).  This is the human ground-truth for calibrating the bot's aim
    error -- how far a human's aim actually sits off a moving/distant target,
    per weapon.  Additive to info['aim']; only populated in entities mode, so
    scan/need/tactics are untouched.  Opponent velocity is differenced from the
    previous frame the same entity was seen (demos are 10Hz)."""
    if last_origin is None or last_viewangles is None or not opps:
        return
    rx, ry, rz = last_origin[0] / 8.0, last_origin[1] / 8.0, last_origin[2] / 8.0
    best = None
    for en in opps:
        o = origins.get(en)
        if o is None:
            continue
        d = math.hypot(o[0] - rx, o[1] - ry)
        if best is None or d < best[0]:
            best = (d, en, o)
    if best is None:
        return
    _, en, o = best
    ex, ey, ez = rx, ry, rz + 22.0          # recorder eye (viewheight ~22)
    ox, oy, oz = o[0], o[1], o[2] + 22.0     # opponent center approx
    dx, dy, dz = ox - ex, oy - ey, oz - ez
    horiz = math.hypot(dx, dy)
    rng = math.sqrt(dx * dx + dy * dy + dz * dz)
    if horiz < 1.0 or rng < 1.0:
        return
    bear_yaw = math.degrees(math.atan2(dy, dx))
    bear_pitch = -math.degrees(math.atan2(dz, horiz))   # Q2: +pitch looks down
    view_pitch, view_yaw = last_viewangles[0], last_viewangles[1]

    def wrap(a):
        a %= 360.0
        return a - 360.0 if a > 180.0 else a
    err_yaw = wrap(view_yaw - bear_yaw)
    err_pitch = wrap(view_pitch - bear_pitch)

    # opponent lateral speed: horizontal velocity perpendicular to the LOS
    lat = -1.0
    prev = aim_state["prev"].get(en)
    if prev is not None:
        pfr, po = prev
        dt = (frame_idx - pfr) * 0.1
        if dt > 1e-3:
            vx = (o[0] - po[0]) / dt
            vy = (o[1] - po[1]) / dt
            hx, hy = dx / horiz, dy / horiz          # LOS unit (horizontal)
            along = vx * hx + vy * hy
            px, py = vx - along * hx, vy - along * hy
            lat = math.hypot(px, py)
    aim_state["prev"][en] = (frame_idx, o)
    info["aim"].append((frame_idx, weapon_idx, rng, err_yaw, err_pitch, lat, ammo))


def _record_engage(info, frame_idx, last_origin, last_velocity, weapon_idx, origins, opps):
    """Log this frame's engagement geometry vs the nearest opponent: distance and
    the velocity component toward the enemy (>0 closing, <0 retreating)."""
    if last_origin is None or not opps:
        return
    rx, ry = last_origin[0] / 8.0, last_origin[1] / 8.0
    best = None
    for en in opps:
        o = origins.get(en)
        if o is None:
            continue
        d = math.hypot(o[0] - rx, o[1] - ry)
        if best is None or d < best[0]:
            best = (d, o)
    if best is None:
        return
    dist, o = best
    adv = speed = 0.0
    if last_velocity is not None:
        vx, vy = last_velocity[0], last_velocity[1]
        speed = math.hypot(vx, vy)
        tx, ty = o[0] - rx, o[1] - ry
        tl = math.hypot(tx, ty)
        if tl > 1e-3 and speed > 1e-3:
            adv = (vx * tx + vy * ty) / tl   # velocity toward enemy, units/sec
    info["engage"].append((frame_idx, weapon_idx, dist, adv, speed))


def parse_combat(data, entities=False):
    """Decode the recording player's per-frame state + pickups + kills.  With
    entities=True, also decode the packet-entities section for opponent origins
    (engagement RANGE / STYLE and kill-range) -- the `tactics` scan; scan/need
    call with entities=False, which is byte-for-byte the prior behavior."""
    info = {"map": None, "playernum": None, "names": {}, "items": {}, "models": {},
            "samples": [], "pickups": [], "kills": [], "engage": [], "killrange": [],
            "aim": []}
    aim_state = {"prev": {}}     # opp entnum -> (frame_idx, origin) for velocity
    m = re.search(rb"maps/([A-Za-z0-9_]+)\.bsp", data)
    if m:
        info["map"] = m.group(1).decode()

    stats = [0] * 32
    last_origin = None          # recorder origin, raw s16 (world = /8)
    last_viewangles = None
    last_weapon = None
    last_velocity = None
    origins = {}                # entity number -> [x,y,z] world units (entities mode)
    frame_idx = 0
    # one-frame-behind snapshot: the decision state BEFORE a pickup mutates it
    # (a health/ammo/weapon pickup bumps STAT_HEALTH/STAT_AMMO on the same frame
    # the pickup edge fires, so the pickup-frame value is post-pickup-inflated).
    prev_health = prev_armor = prev_ammo = 0
    prev_weapon = None

    def opp_entnums():
        me_ent = (info["playernum"] + 1) if info["playernum"] is not None else -1
        return [cn + 1 for cn in info["names"] if cn + 1 != me_ent]

    for block in iter_blocks(data):
        if not block:
            continue
        r = Reader(block)
        while not r.eob():
            cmd = r.u8()
            if cmd == SVC_SERVERDATA:
                r.s32(); r.s32(); r.u8()
                r.string()                       # gamedir
                info["playernum"] = r.s16()
                r.string()                       # levelname
            elif cmd == SVC_CONFIGSTRING:
                idx = r.u16()
                s = r.string()
                if CS_PLAYERSKINS <= idx < CS_PLAYERSKINS + MAX_CLIENTS:
                    info["names"][idx - CS_PLAYERSKINS] = s.split("\\")[0]
                elif CS_ITEMS <= idx < CS_ITEMS + 256:
                    info["items"][idx - CS_ITEMS] = s
                elif CS_MODELS <= idx < CS_ITEMS:
                    info["models"][idx - CS_MODELS] = s
            elif cmd == SVC_PRINT:
                r.u8()
                text = r.string()
                obit = parse_obituary(text)
                if obit:
                    victim, attacker, weapon = obit
                    info["kills"].append((frame_idx, victim, attacker, weapon))
                    # kill-range: recorder's own frags, vs the victim's last-known
                    # origin (from the previous frame's entities -- the obituary
                    # print precedes this block's packetentities).  Clean combat
                    # range signal, no firing gate needed.
                    if entities and last_origin is not None:
                        me = info["names"].get(info["playernum"], "")
                        if me and attacker.lower() == me.lower():
                            vent = next((cn + 1 for cn, nm in info["names"].items()
                                         if nm.lower() == victim.lower()), None)
                            vo = origins.get(vent) if vent is not None else None
                            if vo is not None:
                                info["killrange"].append((weapon, math.hypot(
                                    vo[0] - last_origin[0] / 8.0,
                                    vo[1] - last_origin[1] / 8.0)))
            elif cmd in (SVC_STUFFTEXT, SVC_CENTERPRINT, SVC_LAYOUT):
                r.string()
            elif cmd == SVC_INVENTORY:
                r.skip(2 * 256)
            elif cmd == SVC_NOP:
                continue
            elif cmd == SVC_DOWNLOAD:
                size = r.s16()
                if size >= 0:
                    r.u8(); r.skip(size)
            elif cmd == SVC_SPAWNBASELINE:
                if not entities:
                    break                        # scan/need: skip rest of block (as before)
                try:
                    _parse_baseline(r, origins)
                except (IndexError, ValueError):
                    break
            elif cmd == SVC_FRAME:
                r.s32(); r.s32(); r.u8()
                arealen = r.u8()
                r.skip(arealen)
                sub = r.u8()
                if sub == SVC_PLAYERINFO:
                    o, va, wi, ve = read_playerinfo(r, stats)
                    if o is not None:
                        last_origin = o
                    if va is not None:
                        last_viewangles = va
                    if wi is not None:
                        last_weapon = wi
                    if ve is not None:
                        last_velocity = ve
                    if stats[STAT_PICKUP_STRING]:
                        idx = stats[STAT_PICKUP_STRING] - CS_ITEMS
                        # log the PRE-pickup snapshot (the state that drove the
                        # decision to grab it), not the post-pickup inflated stats
                        info["pickups"].append(
                            (frame_idx, idx, prev_health, prev_armor,
                             prev_ammo, prev_weapon))
                        stats[STAT_PICKUP_STRING] = 0   # one-shot; only log the edge
                    if last_origin is not None and last_viewangles is not None:
                        info["samples"].append(
                            (frame_idx, last_viewangles[0], last_viewangles[1],
                             last_weapon, stats[STAT_HEALTH], stats[STAT_ARMOR],
                             stats[STAT_AMMO]))
                    if entities:
                        # this frame's packetentities follow the playerinfo; decode
                        # opponent origins, then log the engagement geometry.  A
                        # desync stays contained to this block (fresh Reader next).
                        try:
                            if r.u8() == SVC_PACKETENTITIES:
                                _parse_packetentities(r, origins)
                                opps = opp_entnums()
                                _record_engage(info, frame_idx, last_origin,
                                               last_velocity, last_weapon,
                                               origins, opps)
                                _record_aim(info, frame_idx, last_origin,
                                            last_viewangles, last_weapon,
                                            stats[STAT_AMMO], origins, opps,
                                            aim_state)
                        except (IndexError, ValueError):
                            pass
                    # snapshot this frame's state as the "before" for the next frame
                    prev_health = stats[STAT_HEALTH]
                    prev_armor = stats[STAT_ARMOR]
                    prev_ammo = stats[STAT_AMMO]
                    prev_weapon = last_weapon
                    frame_idx += 1
                break
            elif cmd in (SVC_DISCONNECT, SVC_RECONNECT):
                break
            else:
                break
    return info


def iter_zip_demos(mapname=None, limit=None):
    pattern = f"*{mapname}*.zip" if mapname else "*.zip"
    files = sorted(glob.glob(os.path.join(RAW, pattern)))
    if mapname:
        # filename globbing is substring-based; keep only exact map token matches
        # (e.g. "q2dm1" must not also match "q2dm10"-style or "ztn2dm1")
        files = [f for f in files
                 if re.search(rf'(?<![A-Za-z0-9]){re.escape(mapname)}(?![0-9])', os.path.basename(f))]
    if limit:
        files = files[:limit]
    n = 0
    for zp in files:
        try:
            z = zipfile.ZipFile(zp)
            dm2 = next((x for x in z.namelist() if x.lower().endswith(".dm2")), None)
            if not dm2:
                continue
            data = z.read(dm2)
        except (zipfile.BadZipFile, OSError):
            continue
        n += 1
        yield os.path.basename(zp), data
    print(f"# {n} demo(s) matched", file=sys.stderr)


def resolve_weapon(models, model_idx):
    if model_idx is None:
        return None
    path = models.get(model_idx, "")
    m = re.search(r"v_[a-z0-9]+", path)
    return VIEWMODEL_WEAPON.get(m.group(0)) if m else None


def scan(mapname, limit):
    turn_rates = []          # deg per 100ms tick (both yaw+pitch component-wise)
    equip_time = Counter()   # weapon -> sample count (proxy for time-equipped)
    kill_weapon_counts = Counter()
    pickup_item_counts = Counter()
    pickup_health = defaultdict(list)   # item name -> [health at pickup]
    pickup_gap = []           # seconds since previous pickup (this player)
    demos_ok = demos_bad = 0

    for name, data in iter_zip_demos(mapname, limit):
        try:
            info = parse_combat(data)
        except Exception:
            demos_bad += 1
            continue
        if not info["samples"]:
            demos_bad += 1
            continue
        demos_ok += 1

        samp = info["samples"]
        for i in range(1, len(samp)):
            f0, y0, p0, w0, h0, a0, am0 = samp[i - 1]
            f1, y1, p1, w1, h1, a1, am1 = samp[i]
            if f1 != f0 + 1:
                continue    # dropped/duplicated frame -- don't blend the rate
            dy = abs(((y1 - y0) + 180) % 360 - 180)
            dp = abs(p1 - p0)
            if dy + dp > 200:
                continue    # respawn/teleport view snap, not real aim movement
            turn_rates.append(dy + dp)

        for frame_idx, yaw, pitch, weapon_idx, health, armor, ammo in samp:
            w = resolve_weapon(info["models"], weapon_idx)
            if w:
                equip_time[w] += 1

        last_pickup_frame = None
        for frame_idx, idx, health, armor, ammo, weapon_idx in info["pickups"]:
            item = info["items"].get(idx, f"item#{idx}")
            pickup_item_counts[item] += 1
            pickup_health[item].append(health)
            if last_pickup_frame is not None:
                pickup_gap.append((frame_idx - last_pickup_frame) / 10.0)
            last_pickup_frame = frame_idx

        me = info["names"].get(info["playernum"], "").lower()
        for frame_idx, victim, attacker, weapon in info["kills"]:
            if me and attacker.lower() == me:
                kill_weapon_counts[weapon] += 1

    def pct(xs, p):
        if not xs:
            return 0.0
        xs = sorted(xs)
        k = int(len(xs) * p)
        k = min(k, len(xs) - 1)
        return xs[k]

    print(f"\n=== {mapname or 'ALL MAPS'}: {demos_ok} demos parsed, {demos_bad} skipped ===\n")

    print("-- aim turn rate (deg per 100ms tick, yaw+pitch combined) --")
    print(f"  n={len(turn_rates)}  median={pct(turn_rates,0.50):.1f}  "
          f"p75={pct(turn_rates,0.75):.1f}  p90={pct(turn_rates,0.90):.1f}  "
          f"p99={pct(turn_rates,0.99):.1f}  max={max(turn_rates) if turn_rates else 0:.1f}")
    print(f"  (as deg/sec: median={pct(turn_rates,0.50)*10:.0f}  p90={pct(turn_rates,0.90)*10:.0f}  "
          f"p99={pct(turn_rates,0.99)*10:.0f})")
    print(f"  current bot_combat.c turnstep range: 20-60 deg/tick (200-600 deg/sec) applied EVERY tick while engaged")

    print("\n-- weapon-equipped time (sample count while that weapon was out; a proxy for preference under real availability) --")
    total_eq = sum(equip_time.values())
    for w, c in equip_time.most_common():
        print(f"  {w:20s} {c:8d}  {100.0*c/total_eq:5.1f}%")

    print("\n-- weapon usage at (self) kill time --")
    total_kills = sum(kill_weapon_counts.values())
    for w, c in kill_weapon_counts.most_common():
        pct_ = 100.0 * c / total_kills if total_kills else 0
        print(f"  {w:20s} {c:6d}  {pct_:5.1f}%")

    print("\n-- item pickups (top 20 by frequency) --")
    for item, c in pickup_item_counts.most_common(20):
        hs = pickup_health[item]
        avg_h = sum(hs) / len(hs) if hs else 0
        print(f"  {item:20s} n={c:6d}  avg_health_at_pickup={avg_h:5.0f}")

    print(f"\n-- time between consecutive pickups (this player) --")
    print(f"  n={len(pickup_gap)}  median={pct(pickup_gap,0.5):.1f}s  p25={pct(pickup_gap,0.25):.1f}s")

    return {
        "turn_rates": turn_rates, "equip_time": dict(equip_time),
        "kill_weapon_counts": dict(kill_weapon_counts),
        "pickup_item_counts": dict(pickup_item_counts),
        "pickup_health": {k: v for k, v in pickup_health.items()},
    }


def _pctile(xs, p):
    if not xs:
        return 0.0
    xs = sorted(xs)
    k = min(int(len(xs) * p), len(xs) - 1)
    return xs[k]


def _summ(xs):
    return {
        "n": len(xs),
        "p10": _pctile(xs, 0.10), "p25": _pctile(xs, 0.25),
        "p50": _pctile(xs, 0.50), "p75": _pctile(xs, 0.75),
        "p90": _pctile(xs, 0.90),
        "mean": round(sum(xs) / len(xs), 1) if xs else 0.0,
    }


def need(mapname, limit):
    """Mine human resource-need thresholds across the corpus (all maps by
    default): at what health/armor/ammo did pros decide to grab each item
    category.  Writes demos/derived/combat_need/thresholds.json."""
    import datetime

    pickup_state = defaultdict(lambda: defaultdict(list))  # cat -> field -> [vals]
    ammo_by_type = defaultdict(list)   # ammo type -> [pre_ammo] on a matched top-up
    ammo_health = []                   # health at any ammo pickup
    ammo_match = ammo_mismatch = ammo_noweapon = 0
    wsw = {"n": 0, "switched": 0, "delays": []}
    wsw_by = defaultdict(lambda: {"n": 0, "switched": 0, "delays": []})
    equip_time = Counter()
    kill_weapon_counts = Counter()
    maps = Counter()
    demos_ok = demos_bad = 0

    for name, data in iter_zip_demos(mapname, limit):
        try:
            info = parse_combat(data)
        except Exception:
            demos_bad += 1
            continue
        if not info["samples"]:
            demos_bad += 1
            continue
        demos_ok += 1
        if info["map"]:
            maps[info["map"]] += 1
        models = info["models"]

        # weapon held per frame (for weapon-switch detection + equip-time share)
        frame_weap = {}
        for s in info["samples"]:
            fw = resolve_weapon(models, s[3])
            if fw:
                frame_weap[s[0]] = fw
                equip_time[fw] += 1

        for frame_idx, idx, pre_h, pre_a, pre_ammo, pre_widx in info["pickups"]:
            item = info["items"].get(idx, "")
            if not item:
                continue
            cat = item_category(item)
            held = resolve_weapon(models, pre_widx)
            if cat == "health":
                pickup_state["health"]["health"].append(pre_h)
                pickup_state["health"]["armor"].append(pre_a)
            elif cat == "armor":
                pickup_state["armor"]["armor"].append(pre_a)
                pickup_state["armor"]["health"].append(pre_h)
            elif cat == "weapon":
                pickup_state["weapon"]["health"].append(pre_h)
                picked = item.lower()      # matches resolve_weapon output
                wsw["n"] += 1
                wsw_by[picked]["n"] += 1
                for d in range(1, 21):     # scan forward up to ~2s (20 frames)
                    if frame_weap.get(frame_idx + d) == picked:
                        wsw["switched"] += 1
                        wsw["delays"].append(d * 100)
                        wsw_by[picked]["switched"] += 1
                        wsw_by[picked]["delays"].append(d * 100)
                        break
            elif cat == "ammo":
                ammo_health.append(pre_h)
                atype = ammo_item_type(item)
                held_ammo = WEAPON_AMMO.get(held) if held else None
                # stat 3 is ammo for the CURRENTLY-HELD weapon, so it's only the
                # right "how low was I" reading when the ammo they grabbed feeds
                # the gun in their hands -- the deliberate top-up we want to model.
                if held_ammo is None:
                    ammo_noweapon += 1
                elif atype and atype == held_ammo:
                    ammo_by_type[atype].append(pre_ammo)
                    ammo_match += 1
                else:
                    ammo_mismatch += 1

        me = info["names"].get(info["playernum"], "").lower()
        for frame_idx, victim, attacker, weapon in info["kills"]:
            if me and attacker.lower() == me:
                kill_weapon_counts[weapon] += 1

    total_eq = sum(equip_time.values()) or 1
    total_kills = sum(kill_weapon_counts.values()) or 1

    out = {
        "generated": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "corpus": {
            "demos_parsed": demos_ok, "demos_skipped": demos_bad,
            "maps": dict(maps.most_common()),
        },
        "pickup_need": {
            "health": {
                "health_at_pickup": _summ(pickup_state["health"]["health"]),
                "armor_at_pickup": _summ(pickup_state["health"]["armor"]),
            },
            "armor": {
                "armor_at_pickup": _summ(pickup_state["armor"]["armor"]),
                "health_at_pickup": _summ(pickup_state["armor"]["health"]),
            },
            "ammo": {
                "health_at_pickup": _summ(ammo_health),
                "matched_pickups": ammo_match,
                "mismatched_pickups": ammo_mismatch,
                "no_owned_weapon_pickups": ammo_noweapon,
                "by_ammo_type": {t: _summ(v) for t, v in sorted(ammo_by_type.items())},
            },
            "weapon": {
                "health_at_pickup": _summ(pickup_state["weapon"]["health"]),
            },
        },
        "weapon_switch": {
            "n_weapon_pickups": wsw["n"],
            "switched_within_2s_pct":
                round(100.0 * wsw["switched"] / wsw["n"], 1) if wsw["n"] else 0.0,
            "switch_delay_ms": {"p50": _pctile(wsw["delays"], 0.5),
                                "p90": _pctile(wsw["delays"], 0.9)},
            "by_weapon": {
                w: {"n": d["n"],
                    "switched_pct":
                        round(100.0 * d["switched"] / d["n"], 1) if d["n"] else 0.0,
                    "delay_ms_p50": _pctile(d["delays"], 0.5)}
                for w, d in sorted(wsw_by.items(), key=lambda kv: -kv[1]["n"])
            },
        },
        "weapon_equipped_pct":
            {w: round(100.0 * c / total_eq, 1) for w, c in equip_time.most_common()},
        "weapon_at_kill_pct":
            {w: round(100.0 * c / total_kills, 1) for w, c in kill_weapon_counts.most_common()},
        "calibration": {
            "bot_healthneed": {
                "urgency_health_p50": _pctile(pickup_state["health"]["health"], 0.5),
                "note": "median health at which pros top up; urgency curve should already pull by here",
            },
            "bot_ammoneed": {
                "low_threshold_by_ammo":
                    {t: _pctile(v, 0.5) for t, v in sorted(ammo_by_type.items())},
                "note": "p50 of ammo-for-held-weapon at the moment they refilled that ammo type",
            },
            "bot_wpnneed": {
                "kill_rank": [w for w, _ in kill_weapon_counts.most_common()],
            },
        },
    }

    outdir = os.path.join(ROOT, "demos", "derived", "combat_need")
    os.makedirs(outdir, exist_ok=True)
    outpath = os.path.join(outdir, "thresholds.json")
    with open(outpath, "w") as f:
        json.dump(out, f, indent=2)

    # ---- console summary ----
    print(f"\n=== {mapname or 'ALL MAPS'}: {demos_ok} demos parsed, {demos_bad} skipped ===")
    print(f"maps: {dict(maps.most_common(8))}{' ...' if len(maps) > 8 else ''}\n")

    def line(tag, s):
        print(f"  {tag:22s} n={s['n']:7d}  p25={s['p25']:5.0f} p50={s['p50']:5.0f} "
              f"p75={s['p75']:5.0f}  mean={s['mean']:5.1f}")

    print("-- HEALTH: player state when they picked up a health item --")
    line("health-at-pickup", _summ(pickup_state["health"]["health"]))
    line("armor-at-pickup", _summ(pickup_state["health"]["armor"]))
    print("\n-- ARMOR: player state when they picked up an armor item --")
    line("armor-at-pickup", _summ(pickup_state["armor"]["armor"]))
    line("health-at-pickup", _summ(pickup_state["armor"]["health"]))
    print("\n-- AMMO: ammo-for-held-weapon when refilling THAT ammo type --")
    print(f"  (matched top-ups={ammo_match}  mismatched={ammo_mismatch}  "
          f"no-owned-weapon={ammo_noweapon})")
    for t, v in sorted(ammo_by_type.items()):
        line(t, _summ(v))
    line("health-at-ammo-pickup", _summ(ammo_health))
    print("\n-- WEAPON: switch to a freshly-grabbed gun within 2s --")
    print(f"  {out['weapon_switch']['switched_within_2s_pct']:.1f}% of "
          f"{wsw['n']} weapon pickups; delay p50="
          f"{out['weapon_switch']['switch_delay_ms']['p50']}ms")
    print("  weapon-at-kill %: " + ", ".join(
        f"{w} {p}" for w, p in list(out["weapon_at_kill_pct"].items())[:8]))
    print(f"\n-> wrote {outpath}")
    return out


def tactics(mapname, limit):
    """Per-weapon engagement RANGE + STYLE from the pro corpus (opponent origins
    decoded from packet entities).  Writes demos/derived/combat_tactics/
    weapon_profiles.json -- the calibration source for the bot's weapon-aware
    combat (preferred-range bands + advance/retreat style)."""
    import datetime
    ENGAGE_CAP = 1200.0   # count engage frames only when an opponent is this near (in-fight)
    ADV_THRESH = 60.0     # |velocity toward enemy| (u/s) to call advance/retreat vs neutral

    krange = defaultdict(list)                 # weapon -> [kill distance]
    erange = defaultdict(list)                 # weapon -> [engage distance < CAP]
    espeed = defaultdict(list)                 # weapon -> [speed while engaged]
    estyle = defaultdict(lambda: [0, 0, 0])    # weapon -> [advance, retreat, neutral]
    maps = Counter()
    demos_ok = demos_bad = 0

    for name, data in iter_zip_demos(mapname, limit):
        try:
            info = parse_combat(data, entities=True)
        except Exception:
            demos_bad += 1
            continue
        if not info["samples"]:
            demos_bad += 1
            continue
        demos_ok += 1
        if info["map"]:
            maps[info["map"]] += 1

        for weapon, dist in info["killrange"]:
            if weapon in ("telefrag", "?"):
                continue
            krange[weapon].append(dist)

        for frame_idx, widx, dist, adv, speed in info["engage"]:
            if dist > ENGAGE_CAP:
                continue
            w = resolve_weapon(info["models"], widx)
            if not w:
                continue
            erange[w].append(dist)
            espeed[w].append(speed)
            s = estyle[w]
            if adv > ADV_THRESH:
                s[0] += 1
            elif adv < -ADV_THRESH:
                s[1] += 1
            else:
                s[2] += 1

    weapons = sorted(set(krange) | set(erange))
    out = {
        "generated": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "corpus": {"demos_parsed": demos_ok, "demos_skipped": demos_bad,
                   "maps": dict(maps.most_common())},
        "params": {"engage_cap": ENGAGE_CAP, "advance_thresh": ADV_THRESH},
        "weapons": {}, "calibration": {"bands": {}, "style": {}},
    }
    for w in weapons:
        st = estyle[w]
        tot = sum(st) or 1
        adv_f, ret_f, neu_f = (round(st[0] / tot, 3), round(st[1] / tot, 3),
                               round(st[2] / tot, 3))
        out["weapons"][w] = {
            "kill_range": _summ(krange[w]),
            "engage_range": _summ(erange[w]),
            "speed": {"p50": _pctile(espeed[w], 0.5), "p90": _pctile(espeed[w], 0.9)},
            "advance_frac": adv_f, "retreat_frac": ret_f, "neutral_frac": neu_f,
            "n_style": sum(st),
        }
        # bands prefer kill-range (clean combat range); fall back to engage-range if sparse
        src = _summ(krange[w]) if len(krange[w]) >= 30 else _summ(erange[w])
        out["calibration"]["bands"][w] = {"lo": src["p25"], "center": src["p50"], "hi": src["p75"]}
        out["calibration"]["style"][w] = ("press" if adv_f - ret_f > 0.1 else
                                          "defensive" if ret_f - adv_f > 0.1 else "hold")

    outdir = os.path.join(ROOT, "demos", "derived", "combat_tactics")
    os.makedirs(outdir, exist_ok=True)
    outpath = os.path.join(outdir, "weapon_profiles.json")
    with open(outpath, "w") as f:
        json.dump(out, f, indent=2)

    print(f"\n=== {mapname or 'ALL MAPS'}: {demos_ok} demos parsed, {demos_bad} skipped ===")
    print(f"maps: {dict(maps.most_common(8))}{' ...' if len(maps) > 8 else ''}\n")
    print(f"{'weapon':16s} {'killR_p50':>9} {'engR_p50':>9} {'band lo/hi':>13}  "
          f"{'style':9s} adv/ret")
    for w in sorted(weapons, key=lambda x: -out["weapons"][x]["kill_range"]["n"]):
        d = out["weapons"][w]
        b = out["calibration"]["bands"][w]
        print(f"{w:16s} {d['kill_range']['p50']:9.0f} {d['engage_range']['p50']:9.0f} "
              f"{b['lo']:5.0f}/{b['hi']:<6.0f} {out['calibration']['style'][w]:9s} "
              f"{d['advance_frac']:.2f}/{d['retreat_frac']:.2f}  (killN={d['kill_range']['n']})")
    print(f"\n-> wrote {outpath}")
    return out


def aim(mapname, limit):
    """Human aim-error ground truth (calibration for the bot's Combat_Aim error).
    For every engaged frame (holding a weapon, an opponent present, view within
    ~45deg of the enemy) measure |view - exact-bearing| yaw error, bucketed by
    weapon x range x opponent-lateral-speed.  Also isolates RAILGUN FIRE frames
    (detected by a slug decrement) -- the cleanest 'how far off is a committed
    shot' signal.  Writes demos/derived/combat_aim/aim_profiles.json."""
    import datetime
    WEAPONS = ["railgun", "blaster", "hyperblaster", "chaingun", "machinegun",
               "super shotgun"]
    RB = [("near<300", 0, 300), ("mid300-600", 300, 600), ("far>600", 600, 1e9)]
    ENG_CONE = 45.0     # view within this of the enemy bearing = "engaging"
    LAT_SPLIT = 150.0   # opponent lateral speed slow/fast boundary (u/s)

    # weapon -> range-bucket -> lat-class(slow/fast/any) -> [abs err_yaw]
    buckets = {w: {rb[0]: {"slow": [], "fast": [], "any": []} for rb in RB}
               for w in WEAPONS}
    fire_err = defaultdict(list)      # weapon -> [abs err_yaw at fire]
    latpairs = defaultdict(list)      # weapon -> [(lat, abs err_yaw)]
    demos_ok = demos_bad = 0

    for name, data in iter_zip_demos(mapname, limit):
        try:
            info = parse_combat(data, entities=True)
        except Exception:
            demos_bad += 1
            continue
        if not info["aim"]:
            demos_bad += 1
            continue
        demos_ok += 1
        prev_rail_ammo = None
        for (fr, widx, rng, eyaw, epitch, lat, ammo) in info["aim"]:
            w = resolve_weapon(info["models"], widx)
            if w not in buckets:
                # still track railgun-fire even if not in the table set
                if w != "railgun":
                    continue
            aey = abs(eyaw)
            # railgun fire detection via slug decrement
            if w == "railgun":
                if prev_rail_ammo is not None and ammo < prev_rail_ammo \
                        and aey < 90.0 and rng < 2000:
                    fire_err["railgun"].append(aey)
                prev_rail_ammo = ammo
            if w not in buckets or aey > ENG_CONE or rng > 1600:
                continue
            # find range bucket
            rbname = next((n for (n, lo, hi) in RB if lo <= rng < hi), None)
            if not rbname:
                continue
            slot = buckets[w][rbname]
            slot["any"].append(aey)
            if lat >= 0:
                latpairs[w].append((lat, aey))
                slot["fast" if lat >= LAT_SPLIT else "slow"].append(aey)

    out = {
        "generated": datetime.datetime.now(datetime.timezone.utc)
            .strftime("%Y-%m-%dT%H:%M:%SZ"),
        "corpus": {"demos_parsed": demos_ok, "demos_skipped": demos_bad},
        "params": {"engage_cone_deg": ENG_CONE, "lat_split": LAT_SPLIT,
                   "range_buckets": [r[0] for r in RB]},
        "note": "err = |view_yaw - exact_bearing_yaw| in degrees; human ground truth",
        "weapons": {}, "railgun_fire": _summ(fire_err["railgun"]),
    }
    for w in WEAPONS:
        wd = {}
        for (rbname, _, _) in RB:
            s = buckets[w][rbname]
            wd[rbname] = {
                "n": len(s["any"]),
                "err_p50": round(_pctile(s["any"], 0.5), 2),
                "err_p90": round(_pctile(s["any"], 0.9), 2),
                "slow_p50": round(_pctile(s["slow"], 0.5), 2),
                "slow_n": len(s["slow"]),
                "fast_p50": round(_pctile(s["fast"], 0.5), 2),
                "fast_n": len(s["fast"]),
            }
        out["weapons"][w] = wd

    outdir = os.path.join(ROOT, "demos", "derived", "combat_aim")
    os.makedirs(outdir, exist_ok=True)
    outpath = os.path.join(outdir, "aim_profiles.json")
    with open(outpath, "w") as f:
        json.dump(out, f, indent=2)

    print(f"\n=== {mapname or 'ALL MAPS'}: {demos_ok} demos parsed, "
          f"{demos_bad} skipped -- HUMAN aim-error (deg) ===")
    print(f"railgun FIRE-frame |err_yaw|: {_summ(fire_err['railgun'])}\n")
    print(f"{'weapon':14s} {'range':11s} {'n':>6} {'|err|p50':>9} {'p90':>7}  "
          f"{'slow_p50':>8}(n) {'fast_p50':>8}(n)")
    for w in WEAPONS:
        for (rbname, _, _) in RB:
            d = out["weapons"][w][rbname]
            if d["n"] < 20:
                continue
            print(f"{w:14s} {rbname:11s} {d['n']:6d} {d['err_p50']:9.2f} "
                  f"{d['err_p90']:7.2f}  {d['slow_p50']:8.2f}({d['slow_n']:>5}) "
                  f"{d['fast_p50']:8.2f}({d['fast_n']:>5})")
    print(f"\n-> wrote {outpath}")
    return out


def one(path):
    data = open(path, "rb").read()
    info = parse_combat(data)
    print(f"map={info['map']} player={info['names'].get(info['playernum'])}")
    print(f"samples={len(info['samples'])} pickups={len(info['pickups'])} "
          f"kills={len(info['kills'])}")
    print("items seen:", {k: v for k, v in list(info["items"].items())[:10]})
    for p in info["pickups"][:10]:
        idx = p[1]
        w = resolve_weapon(info["models"], p[5])
        print("pickup:", info["items"].get(idx, f"#{idx}"),
              "pre_health=", p[2], "pre_armor=", p[3], "pre_ammo=", p[4],
              "holding=", w)
    for k in info["kills"][:10]:
        print("kill:", k)
    weapons = Counter()
    for s in info["samples"]:
        w = resolve_weapon(info["models"], s[3])
        if w:
            weapons[w] += 1
    print("weapon-equipped samples:", weapons.most_common())


def timing(mapname, limit):
    """Item-timing / respawn-cycle analysis.  For the POV pro in each demo, the
    interval between successive pickups of the SAME major timed item (Mega Health,
    the armors, power-ups) is bounded below by that item's respawn time -- so the
    interval distribution reveals BOTH the effective respawn cadence (its lower
    tail) and how disciplined the pro's re-timing is (how tightly intervals cluster
    at that floor).  STAT_PICKUP_STRING is the POV player's own HUD notify, so this
    is exactly the pro whose timing we want to model.  Writes
    demos/derived/combat_timing/timing.json.  This is an ANALYSIS artifact -- no
    bot lever is calibrated from it yet; it quantifies how much predictive
    item-timing pros actually do, per item and per (duel) map, so the value of a
    predictive pre-positioning behavior can be judged before it is built."""
    import datetime

    # major timed items worth pre-positioning for (substring match on pickup name)
    TIMED = ["Mega Health", "Body Armor", "Combat Armor", "Jacket Armor",
             "Quad Damage", "Invulnerability", "Power Shield", "Power Screen"]

    def timed_name(item):
        low = item.lower()
        for t in TIMED:
            if t.lower() in low:
                return t
        return None

    intervals = defaultdict(list)                      # item -> [dt seconds]
    per_map = defaultdict(lambda: defaultdict(list))   # map -> item -> [dt]
    demos_with_repeat = defaultdict(int)               # item -> #demos with >=2
    pickups_total = defaultdict(int)                   # item -> total POV pickups
    demos_seen_item = defaultdict(int)                 # item -> #demos with >=1
    demos_ok = demos_bad = 0
    maps = Counter()

    for name, data in iter_zip_demos(mapname, limit):
        try:
            info = parse_combat(data)
        except Exception:
            demos_bad += 1
            continue
        if not info["pickups"]:
            demos_bad += 1
            continue
        demos_ok += 1
        mp = info["map"] or "?"
        maps[mp] += 1
        seq = defaultdict(list)   # item -> [frame_idx]
        for frame_idx, idx, ph, pa, pam, pw in info["pickups"]:
            tn = timed_name(info["items"].get(idx, ""))
            if tn:
                seq[tn].append(frame_idx)
        for tn, frames in seq.items():
            frames.sort()
            pickups_total[tn] += len(frames)
            demos_seen_item[tn] += 1
            if len(frames) >= 2:
                demos_with_repeat[tn] += 1
            for a, b in zip(frames, frames[1:]):
                dt = (b - a) * 0.1   # 10Hz demo frames -> seconds
                if 0 < dt < 600:     # drop absurd gaps (level changes, parse holes)
                    intervals[tn].append(dt)
                    per_map[mp][tn].append(dt)

    def tightness(xs):
        """Fraction of intervals within 1.35x the p10 'respawn floor' -- high means
        the pro re-grabs close to when the item comes back (deliberate timing)."""
        if len(xs) < 8:
            return None
        floor = _pctile(xs, 0.10)
        if floor <= 0:
            return None
        near = sum(1 for v in xs if v <= floor * 1.35)
        return round(near / len(xs), 3)

    items_out = {}
    for tn in TIMED:
        iv = intervals.get(tn, [])
        items_out[tn] = {
            "pov_pickups": pickups_total.get(tn, 0),
            "demos_with_item": demos_seen_item.get(tn, 0),
            "demos_with_repeat": demos_with_repeat.get(tn, 0),
            "repeat_demo_pct": round(100.0 * demos_with_repeat.get(tn, 0)
                                     / max(1, demos_seen_item.get(tn, 0)), 1),
            "interval_s": _summ(iv) if iv else None,
            "respawn_floor_s": round(_pctile(iv, 0.10), 1) if iv else None,
            "timing_tightness": tightness(iv),
        }

    # per-map cadence for the maps with the most timed-item intervals (duel maps)
    map_rank = sorted(per_map.keys(),
                      key=lambda m: -sum(len(v) for v in per_map[m].values()))
    maps_out = {}
    for mp in map_rank[:8]:
        entry = {}
        for tn, iv in per_map[mp].items():
            if len(iv) >= 8:
                entry[tn] = {
                    "n": len(iv),
                    "floor_s": round(_pctile(iv, 0.10), 1),
                    "p50_s": round(_pctile(iv, 0.50), 1),
                    "tightness": tightness(iv),
                }
        if entry:
            maps_out[mp] = entry

    out = {
        "generated": datetime.datetime.now(datetime.timezone.utc).strftime(
            "%Y-%m-%dT%H:%M:%SZ"),
        "corpus": {"demos_parsed": demos_ok, "demos_skipped": demos_bad,
                   "maps": dict(maps.most_common())},
        "method": ("interval between successive POV pickups of the same timed item; "
                   "floor=p10~respawn, tightness=frac within 1.35x floor"),
        "items": items_out,
        "by_map": maps_out,
    }

    outdir = os.path.join(ROOT, "demos", "derived", "combat_timing")
    os.makedirs(outdir, exist_ok=True)
    outpath = os.path.join(outdir, "timing.json")
    with open(outpath, "w") as f:
        json.dump(out, f, indent=2)

    print(f"\n=== {mapname or 'ALL MAPS'} item-timing: {demos_ok} demos, "
          f"{demos_bad} skipped ===")
    print(f"maps: {dict(maps.most_common(6))}{' ...' if len(maps) > 6 else ''}\n")
    print(f"  {'item':16s} {'povPick':>7s} {'rptDemo%':>8s} {'floor_s':>7s} "
          f"{'p50_s':>6s} {'tight':>6s}")
    for tn in TIMED:
        e = items_out[tn]
        iv = e["interval_s"]
        if not iv:
            print(f"  {tn:16s} {e['pov_pickups']:7d} {'-':>8s} {'-':>7s} "
                  f"{'-':>6s} {'-':>6s}   (no repeat intervals)")
            continue
        print(f"  {tn:16s} {e['pov_pickups']:7d} {e['repeat_demo_pct']:8.1f} "
              f"{e['respawn_floor_s']:7.1f} {iv['p50']:6.0f} "
              f"{(e['timing_tightness'] if e['timing_tightness'] is not None else 0):6.2f}")
    print("\n  per-map floor/p50/tightness (top duel maps):")
    for mp, entry in list(maps_out.items())[:6]:
        parts = [f"{tn.split()[0]}:{d['floor_s']:.0f}/{d['p50_s']:.0f}/"
                 f"{(d['tightness'] if d['tightness'] is not None else 0):.2f}"
                 for tn, d in entry.items()]
        print(f"    {mp:10s} " + "  ".join(parts))
    print(f"\n-> wrote {outpath}")
    return out


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "scan"
    if cmd == "one":
        one(sys.argv[2])
    else:
        mapname = None
        limit = None
        rest = sys.argv[2:]
        args = [a for a in rest if not a.startswith("--limit")]
        if args:
            mapname = args[0]
        for a in rest:
            if a.startswith("--limit"):
                limit = int(a.split("=", 1)[1]) if "=" in a else int(rest[rest.index(a) + 1])
        if cmd == "need":
            result = need(mapname, limit)
        elif cmd == "timing":
            result = timing(mapname, limit)
        elif cmd == "tactics":
            result = tactics(mapname, limit)
        elif cmd == "aim":
            result = aim(mapname, limit)
        else:
            result = scan(mapname, limit)

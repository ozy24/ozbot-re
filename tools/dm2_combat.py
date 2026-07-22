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
    python dm2_combat.py pursue [mapname] [--limit N] [--dump K]
                                                       -> sight-loss/chase behavior
                                                          + combat-move cadence
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


def _record_track(info, frame_idx, last_origin, last_velocity, last_viewangles,
                  weapon_idx, stats, origins, opps):
    """Per-frame recorder state + WHICH opponents are present in the packet-entity
    set this frame.  A demo only carries entities inside the recorder's PVS, so an
    opponent dropping out of that set IS the sight-loss event -- this presence
    timeline is what the `pursue` scan segments into sight-loss episodes.  Presence
    is exact: the delta protocol never re-sends an unchanged entity, but it always
    writes an explicit U_REMOVE when one leaves (handled in _parse_packetentities),
    so `origins` membership tracks visibility rather than last-changed."""
    if last_origin is None:
        return
    rx, ry, rz = last_origin[0] / 8.0, last_origin[1] / 8.0, last_origin[2] / 8.0
    vx, vy, vz = last_velocity if last_velocity is not None else (0.0, 0.0, 0.0)
    yaw = last_viewangles[1] if last_viewangles is not None else 0.0
    info["self"].append((frame_idx, rx, ry, rz, vx, vy, vz, yaw, weapon_idx,
                         stats[STAT_HEALTH], stats[STAT_ARMOR], stats[STAT_AMMO]))
    for en in opps:
        o = origins.get(en)
        if o is not None:
            info["opp"].append((frame_idx, en, o[0], o[1], o[2]))


def parse_combat(data, entities=False, track=False):
    """Decode the recording player's per-frame state + pickups + kills.  With
    entities=True, also decode the packet-entities section for opponent origins
    (engagement RANGE / STYLE and kill-range) -- the `tactics` scan; scan/need
    call with entities=False, which is byte-for-byte the prior behavior.
    track=True additionally logs the per-frame recorder/opponent PRESENCE
    timeline (the `pursue` scan); it implies entities and is otherwise inert."""
    if track:
        entities = True
    info = {"map": None, "playernum": None, "names": {}, "items": {}, "models": {},
            "samples": [], "pickups": [], "kills": [], "engage": [], "killrange": [],
            "aim": [], "self": [], "opp": []}
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
                                if track:
                                    _record_track(info, frame_idx, last_origin,
                                                  last_velocity, last_viewangles,
                                                  last_weapon, stats, origins, opps)
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


# ---- `pursue`: sight-loss / chase behavior + combat-movement cadence ----

# episode-detection constants (all in demo frames @ 10Hz unless noted)
PU_LOSS_GAP = 3          # opponent absent >=0.3s => a sight loss
PU_ENGAGE_DIST = 1200.0  # opponent must be this near at loss to count as a fight
PU_WINDOW = 15           # 1.5s classification window after the loss
PU_STATIC_PATH = 40.0    # moved less than this over the window => neither class
PU_DOT = 0.5             # path-weighted mean dot => "pursued"
PU_ABANDON_S = 10.0      # no re-sight within this => abandoned
PU_FIRE_LOOKBACK = 20    # 2.0s: recorder fired this recently => engaged
PU_FACE_CONE = 30.0      # or was facing the opponent this tightly at loss
PU_DEATH_GUARD = 10      # +-1.0s: opponent died => not a sight loss, a kill
PU_EXTRAP_CLAMP = 200.0  # the bot clamps its extrapolation to this (plan spec)
PU_EXTRAP_TS = [i / 10.0 for i in range(0, 16)]     # 0.0 .. 1.5s
PU_RESIGHT_FRESH = 3.0   # only short gaps calibrate the extrapolation window
PU_SUSTAINED_S = 1.0     # a >=1s absence is a real break, not PVS flicker
PU_REACH = 100.0         # within this of the LKP = the chase arrived

# combat-movement (Feature 3) mining constants
CM_MIN_SPEED = 100.0     # recorder must be moving to have a strafe direction
CM_LAT_DEAD = 60.0       # |lateral speed| below this is not a committed strafe
CM_HIGH_DZ = 64.0        # height advantage threshold
CM_LEVEL_DZ = 32.0
CM_AIM_CONE = 30.0       # facing the opponent this tightly = really fighting
CM_AIM_LO, CM_AIM_HI = 150.0, 900.0    # ...and inside a combat range band


def _wrap180(a):
    a %= 360.0
    return a - 360.0 if a > 180.0 else a


def _pu_episodes(info, ep_out, cm_out, demo_name):
    """Segment one demo's presence timeline into sight-loss episodes and
    accumulate the combat-movement cadence samples.  Appends to ep_out/cm_out."""
    self_rows = {r[0]: r for r in info["self"]}
    if len(self_rows) < 50:
        return
    pres = defaultdict(dict)          # opp entnum -> {frame: (x,y,z)}
    by_frame = defaultdict(list)      # frame -> [(entnum, (x,y,z))]
    for (f, en, x, y, z) in info["opp"]:
        pres[en][f] = (x, y, z)
        by_frame[f].append((en, (x, y, z)))

    # frames on which the recorder fired: ammo for the HELD weapon dropped while
    # that same weapon stayed out (a switch also changes the stat, hence the guard)
    fired = set()
    prev = None
    for r in info["self"]:
        if prev is not None and r[0] == prev[0] + 1 and r[8] == prev[8] \
                and r[11] < prev[11]:
            fired.add(r[0])
        prev = r

    # frames on which each client NAME died (an opponent's death is not sight loss)
    died = defaultdict(list)
    for (f, victim, attacker, weapon) in info["kills"]:
        died[victim.lower()].append(f)
    ent_name = {cn + 1: nm.lower() for cn, nm in info["names"].items()}

    # ---------- combat-movement cadence (visible engagements only) ----------
    last_sign = 0
    last_flip = None
    for r in info["self"]:
        f, rx, ry, rz, vx, vy, vz = r[0], r[1], r[2], r[3], r[4], r[5], r[6]
        near = None
        for en, o in by_frame.get(f, ()):
            d = math.hypot(o[0] - rx, o[1] - ry)
            if near is None or d < near[0]:
                near = (d, o)
        if near is None or near[0] > PU_ENGAGE_DIST:
            last_sign, last_flip = 0, None      # fight over; don't span the gap
            continue
        dist, o = near
        speed = math.hypot(vx, vy)
        dz = rz - o[2]
        bucket = ("high" if dz >= CM_HIGH_DZ else
                  "low" if dz <= -CM_HIGH_DZ else
                  "level" if abs(dz) <= CM_LEVEL_DZ else None)
        if bucket:
            cm_out["speed"][bucket].append(speed)
            # "aimed" = actually fighting (in a combat range band AND looking at
            # them), not merely co-present on a vertical map.  The unfiltered
            # bucket above is dominated by travel, which is what makes the raw
            # high/low split look symmetric.
            if CM_AIM_LO <= dist <= CM_AIM_HI:
                bear = math.degrees(math.atan2(o[1] - ry, o[0] - rx))
                if abs(_wrap180(r[7] - bear)) <= CM_AIM_CONE:
                    cm_out["speed_aimed"][bucket].append(speed)
        if dist < 1e-3 or speed < CM_MIN_SPEED:
            continue
        # lateral = recorder velocity perpendicular to the horizontal line of sight
        hx, hy = (o[0] - rx) / dist, (o[1] - ry) / dist
        lat = vx * (-hy) + vy * hx
        if abs(lat) < CM_LAT_DEAD:
            continue
        sign = 1 if lat > 0 else -1
        if last_sign and sign != last_sign and last_flip is not None:
            iv = (f - last_flip) * 0.1
            if 0.1 <= iv <= 8.0:
                cm_out["reversal_s"].append(iv)
        if sign != last_sign:
            last_flip = f
        last_sign = sign

    # ---------- sight-loss episodes ----------
    for en, seen in pres.items():
        fl = sorted(seen)
        if len(fl) < 10:
            continue
        # (loss frame, re-sight frame or None) pairs
        pairs = [(fl[i], fl[i + 1]) for i in range(len(fl) - 1)
                 if fl[i + 1] - fl[i] >= PU_LOSS_GAP]
        if fl[-1] + PU_WINDOW + 20 in self_rows:      # vanished, demo continues
            pairs.append((fl[-1], None))

        for f_a, f_b in pairs:
            ra = self_rows.get(f_a)
            if ra is None:
                continue
            lkp = seen[f_a]
            rx, ry, rz = ra[1], ra[2], ra[3]
            dist0 = math.hypot(lkp[0] - rx, lkp[1] - ry)
            if dist0 > PU_ENGAGE_DIST:
                continue
            nm = ent_name.get(en)
            if nm and any(abs(df - f_a) <= PU_DEATH_GUARD for df in died.get(nm, ())):
                continue                       # they died, they didn't slip away

            # engagement signal: fired recently, or squarely facing them at loss
            fired_recent = any(f in fired for f in range(f_a - PU_FIRE_LOOKBACK, f_a + 1))
            bear = math.degrees(math.atan2(lkp[1] - ry, lkp[0] - rx))
            facing = abs(_wrap180(ra[7] - bear)) <= PU_FACE_CONE
            if not (fired_recent or facing):
                continue

            # opponent velocity at the moment of loss (finite difference, <=0.3s)
            lvel = (0.0, 0.0, 0.0)
            for back in (1, 2, 3):
                pf = f_a - back
                if pf in seen:
                    dt = back * 0.1
                    po = seen[pf]
                    lvel = ((lkp[0] - po[0]) / dt, (lkp[1] - po[1]) / dt,
                            (lkp[2] - po[2]) / dt)
                    break

            # (a) pursue vs disengage over the next 1.5s
            path = wdot = 0.0
            px, py = rx, ry
            for f in range(f_a + 1, f_a + 1 + PU_WINDOW):
                r = self_rows.get(f)
                if r is None:
                    break
                sx, sy = r[1] - px, r[2] - py
                sl = math.hypot(sx, sy)
                tx, ty = lkp[0] - px, lkp[1] - py
                tl = math.hypot(tx, ty)
                if sl > 1e-3 and tl > 1e-3:
                    wdot += (sx * tx + sy * ty) / tl
                path += sl
                px, py = r[1], r[2]
            if path < 1e-3:
                continue
            score = wdot / path
            cls = ("static" if path < PU_STATIC_PATH else
                   "pursued" if score > PU_DOT else "disengaged")

            # (b) gap to re-sight, (c) path walked while the gap lasted
            cap = f_a + int(PU_ABANDON_S * 10)
            end = min(f_b, cap) if f_b is not None else cap
            resight = f_b is not None and f_b <= cap
            gap = (f_b - f_a) * 0.1 if resight else PU_ABANDON_S
            # invest = total path over the gap; reach = the path actually spent
            # getting to the LKP (the direct analogue of the bot's A* g-cost cap
            # on the route to the LKP node -- `invest` alone is dominated by the
            # 10s abandon cap, since a pro who gives up keeps running the map)
            invest = 0.0
            reach = None
            close_path, mind = 0.0, dist0
            px, py = rx, ry
            for f in range(f_a + 1, end + 1):
                r = self_rows.get(f)
                if r is None:
                    break
                invest += math.hypot(r[1] - px, r[2] - py)
                px, py = r[1], r[2]
                d = math.hypot(lkp[0] - px, lkp[1] - py)
                if reach is None and d <= PU_REACH:
                    reach = invest
                if d < mind:                    # still closing on the LKP
                    mind, close_path = d, invest

            ep = {
                "demo": demo_name, "map": info.get("map"), "frame": f_a,
                "dist_at_loss": round(dist0, 1), "cls": cls,
                "score": round(score, 3), "path_1p5s": round(path, 1),
                "gap_s": round(gap, 1), "resight": resight,
                "sustained": (not resight) or gap >= PU_SUSTAINED_S,
                "invest": round(invest, 1),
                "reach": round(reach, 1) if reach is not None else None,
                # path spent while still CLOSING on the LKP (up to the closest
                # approach).  Pros commit toward a last-known position but seldom
                # walk onto the exact spot, so `reach` only fires on a minority of
                # chases -- this is the well-powered analogue of the bot's A*
                # cost cap on the route to the LKP node.
                "close_path": round(close_path, 1),
                "closed_frac": round((dist0 - mind) / dist0, 3) if dist0 > 1 else 0.0,
                "health": ra[9], "armor": ra[10],
                "fired_recent": fired_recent, "facing": facing,
                "lkp": [round(v, 1) for v in lkp],
                "lvel": [round(v, 1) for v in lvel],
            }
            if resight:
                ro = seen[f_b]
                ep["reappear"] = [round(v, 1) for v in ro]
                if gap <= PU_RESIGHT_FRESH:
                    offs = []
                    for T in PU_EXTRAP_TS:
                        ex = [lkp[i] + lvel[i] * T for i in range(3)]
                        vl = math.sqrt(sum((ex[i] - lkp[i]) ** 2 for i in range(3)))
                        if vl > PU_EXTRAP_CLAMP:            # bot clamps the guess
                            k = PU_EXTRAP_CLAMP / vl
                            ex = [lkp[i] + (ex[i] - lkp[i]) * k for i in range(3)]
                        offs.append(math.sqrt(sum((ro[i] - ex[i]) ** 2
                                                  for i in range(3))))
                    ep["extrap_off"] = [round(v, 1) for v in offs]
            ep["pos"] = [round(rx, 1), round(ry, 1), round(rz, 1)]
            ep_out.append(ep)


def pursue(mapname, limit, dump=0):
    """Mine pro sight-loss behavior (do they chase, how far, for how long, and
    where does the opponent actually reappear) plus the combat-movement cadence
    that calibrates the engagement-movement styles.  Writes
    demos/derived/combat_pursuit/pursuit.json."""
    import datetime

    eps = []
    cm = {"reversal_s": [],
          "speed": {"high": [], "level": [], "low": []},
          "speed_aimed": {"high": [], "level": [], "low": []}}
    dumps = []
    maps = Counter()
    demos_ok = demos_bad = 0

    for name, data in iter_zip_demos(mapname, limit):
        try:
            info = parse_combat(data, track=True)
        except Exception:
            demos_bad += 1
            continue
        if not info["self"] or not info["opp"]:
            demos_bad += 1
            continue
        demos_ok += 1
        if info["map"]:
            maps[info["map"]] += 1
        try:
            _pu_episodes(info, eps, cm, name)
        except Exception:
            pass

    n = len(eps)
    by_cls = Counter(e["cls"] for e in eps)
    moved = [e for e in eps if e["cls"] != "static"]
    pursued = [e for e in eps if e["cls"] == "pursued"]
    diseng = [e for e in eps if e["cls"] == "disengaged"]
    pursue_rate = (len(pursued) / len(moved)) if moved else 0.0

    # SUSTAINED = the opponent was gone >=1s.  A 0.3-0.6s dropout is PVS flicker
    # around a doorway, not a tactical sight loss, and those short episodes both
    # dominate the raw counts and make the pursue-rate meaningless (you cannot
    # "decide to chase" someone who is already back).  Every constant the bot
    # takes from this scan is derived from the sustained subset.
    sus = [e for e in eps if e["sustained"]]
    sus_moved = [e for e in sus if e["cls"] != "static"]
    sus_pursued = [e for e in sus if e["cls"] == "pursued"]
    sus_diseng = [e for e in sus if e["cls"] == "disengaged"]
    sus_rate = (len(sus_pursued) / len(sus_moved)) if sus_moved else 0.0

    resights = [e for e in sus_pursued if e["resight"]]
    gaps = [e["gap_s"] for e in resights]
    invest = [e["invest"] for e in sus_pursued]
    reached = [e["reach"] for e in sus_pursued if e["reach"] is not None]
    reach_rate = (len(reached) / len(sus_pursued)) if sus_pursued else 0.0
    closep = [e["close_path"] for e in sus_pursued if e["close_path"] > 0]
    closef = [e["closed_frac"] for e in sus_pursued]
    strength = lambda e: e["health"] + e["armor"]

    def weak_pct(xs):
        return round(100.0 * sum(1 for e in xs if strength(e) < 70) / len(xs), 1) \
            if xs else 0.0

    # extrapolation window that minimizes the MEDIAN reappearance offset
    extrap = []
    fresh = [e for e in eps if "extrap_off" in e]
    for i, T in enumerate(PU_EXTRAP_TS):
        offs = [e["extrap_off"][i] for e in fresh]
        extrap.append({"t": T, "median_off": round(_pctile(offs, 0.5), 1),
                       "p25_off": round(_pctile(offs, 0.25), 1)})
    best_t = min(extrap, key=lambda d: d["median_off"])["t"] if fresh else None

    enough = len(sus) >= 200
    out = {
        "generated": datetime.datetime.now(datetime.timezone.utc)
            .strftime("%Y-%m-%dT%H:%M:%SZ"),
        "corpus": {"demos_parsed": demos_ok, "demos_skipped": demos_bad,
                   "maps": dict(maps.most_common())},
        "params": {
            "loss_gap_s": PU_LOSS_GAP / 10.0, "engage_dist": PU_ENGAGE_DIST,
            "classify_window_s": PU_WINDOW / 10.0, "pursue_dot": PU_DOT,
            "static_path": PU_STATIC_PATH, "abandon_s": PU_ABANDON_S,
            "extrap_clamp": PU_EXTRAP_CLAMP,
        },
        "method": ("a demo carries only entities in the recorder's PVS, so an "
                   "opponent leaving the packet-entity set is the sight-loss "
                   "event; episodes are gated on engagement (recent fire or "
                   "facing) and exclude opponent deaths"),
        "episodes": {
            "n_all": n, "by_class_all": dict(by_cls),
            "pursue_rate_all": round(pursue_rate, 3),
            "n": len(sus), "sufficient": enough,
            "by_class": dict(Counter(e["cls"] for e in sus)),
            "pursue_rate": round(sus_rate, 3),
            "dist_at_loss": _summ([e["dist_at_loss"] for e in sus]),
            "gap_to_resight_s": _summ(gaps),
            "chase_investment": _summ(invest),
            "reach_path": _summ(reached), "reach_rate": round(reach_rate, 3),
            "close_path": _summ(closep), "closed_frac": _summ(closef),
            "resight_rate": round(len(resights) / len(sus_pursued), 3)
                            if sus_pursued else 0.0,
        },
        "strength_at_loss": {
            "pursued": _summ([strength(e) for e in sus_pursued]),
            "disengaged": _summ([strength(e) for e in sus_diseng]),
            "pursued_health": _summ([e["health"] for e in sus_pursued]),
            "disengaged_health": _summ([e["health"] for e in sus_diseng]),
            "pursued_weak_pct": weak_pct(sus_pursued),
            "disengaged_weak_pct": weak_pct(sus_diseng),
            "note": ("if pursued/disengaged strength distributions coincide, the "
                     "corpus does NOT support gating pursuit on strength"),
        },
        "extrapolation": {"curve": extrap, "best_t": best_t, "n": len(fresh)},
        "combat_move": {
            "strafe_reversal_s": _summ(cm["reversal_s"]),
            "speed_by_height": {k: _summ(v) for k, v in cm["speed"].items()},
            "speed_by_height_aimed":
                {k: _summ(v) for k, v in cm["speed_aimed"].items()},
        },
        "calibration": {
            "bot_pursuitcost": round(_pctile(closep, 0.75)) if enough and closep else 700.0,
            "pursuit_secs": round(_pctile(gaps, 0.75), 1) if enough else 4.5,
            "extrapolate_secs": best_t if enough else 0.6,
            "cm_until_s": round(_pctile(cm["reversal_s"], 0.5), 2)
                          if cm["reversal_s"] else 2.0,
            "note": ("priors (700/4.5/0.6/2.0) are substituted verbatim when "
                     "fewer than 200 clean episodes are mined"),
        },
    }

    outdir = os.path.join(ROOT, "demos", "derived", "combat_pursuit")
    os.makedirs(outdir, exist_ok=True)
    outpath = os.path.join(outdir, "pursuit.json")
    with open(outpath, "w") as f:
        json.dump(out, f, indent=2)

    # ---- console summary ----
    print(f"\n=== {mapname or 'ALL MAPS'} sight-loss/pursuit: {demos_ok} demos "
          f"parsed, {demos_bad} skipped ===")
    print(f"maps: {dict(maps.most_common(6))}{' ...' if len(maps) > 6 else ''}\n")
    print(f"all sight losses (>=0.3s absence)  n={n}  classes={dict(by_cls)}  "
          f"pursue-rate={pursue_rate*100:.1f}%")
    print(f"SUSTAINED (>=1.0s, the real breaks) n={len(sus)} "
          f"({'SUFFICIENT' if enough else 'INSUFFICIENT (<200) -> use priors'})")
    print(f"  classes: {out['episodes']['by_class']}   pursue-rate (of moving)="
          f"{sus_rate*100:.1f}%   re-sight rate={out['episodes']['resight_rate']*100:.1f}%"
          f"   reach-LKP rate={reach_rate*100:.1f}%")
    for tag, s in (("dist at loss", out["episodes"]["dist_at_loss"]),
                   ("gap to re-sight s", out["episodes"]["gap_to_resight_s"]),
                   ("chase investment u", out["episodes"]["chase_investment"]),
                   ("path to reach LKP u", out["episodes"]["reach_path"]),
                   ("path while closing u", out["episodes"]["close_path"]),
                   ("frac of gap closed", out["episodes"]["closed_frac"])):
        print(f"  {tag:20s} n={s['n']:6d}  p25={s['p25']:6.1f} p50={s['p50']:6.1f} "
              f"p75={s['p75']:6.1f} p90={s['p90']:6.1f}")
    print("\n-- strength (health+armor) at sight loss --")
    for tag in ("pursued", "disengaged"):
        s = out["strength_at_loss"][tag]
        print(f"  {tag:12s} n={s['n']:6d}  p25={s['p25']:5.0f} p50={s['p50']:5.0f} "
              f"p75={s['p75']:5.0f}  mean={s['mean']:5.1f}  "
              f"weak(<70)={out['strength_at_loss'][tag + '_weak_pct']:.1f}%")
    print(f"\n-- reappearance offset vs extrapolation window (n={len(fresh)}) --")
    print("   " + "  ".join(f"{d['t']:.1f}s:{d['median_off']:.0f}" for d in extrap))
    print(f"   best window = {best_t}s")
    print("\n-- combat movement --")
    s = out["combat_move"]["strafe_reversal_s"]
    print(f"  strafe reversal interval  n={s['n']:6d}  p25={s['p25']:.2f} "
          f"p50={s['p50']:.2f} p75={s['p75']:.2f}s")
    for k in ("high", "level", "low"):
        s = out["combat_move"]["speed_by_height"][k]
        a = out["combat_move"]["speed_by_height_aimed"][k]
        print(f"  speed dz={k:5s}  co-present n={s['n']:7d} p50={s['p50']:5.0f}"
              f"   |  AIMED n={a['n']:7d} p50={a['p50']:5.0f} mean={a['mean']:5.0f}")
    print("\n-- calibration -> C constants --")
    for k, v in out["calibration"].items():
        if k != "note":
            print(f"  {k:20s} {v}")
    if dump and sus:
        # spread the sample across the corpus, not the first demo's episodes
        stride = max(1, len(sus) // dump)
        print(f"\n-- sample sustained episodes (hand spot-check) --")
        for e in sus[::stride][:dump]:
            print(f"  {e['demo'][:30]:30s} f={e['frame']:5d} {e['cls']:10s} "
                  f"score={e['score']:+.2f} dist={e['dist_at_loss']:6.0f} "
                  f"gap={e['gap_s']:4.1f}s reach={str(e['reach']):>7s} "
                  f"hp={e['health']:3d}/{e['armor']:3d} "
                  f"{'fire' if e['fired_recent'] else '    '}"
                  f"{'/face' if e['facing'] else ''}\n"
                  f"{'':34s}pos={e['pos']} -> lkp={e['lkp']} vel={e['lvel']}")
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
    import argparse
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", nargs="?", default="scan",
                    choices=["scan", "need", "timing", "tactics", "aim", "pursue", "one"])
    ap.add_argument("mapname", nargs="?", default=None,
                    help="map token filter (or the .dm2 path for `one`)")
    ap.add_argument("--limit", type=int, default=None, help="max demos to read")
    ap.add_argument("--dump", type=int, default=0,
                    help="pursue: print N sample episodes for hand spot-checking")
    a = ap.parse_args()
    if a.cmd == "one":
        one(a.mapname)
    elif a.cmd == "need":
        result = need(a.mapname, a.limit)
    elif a.cmd == "timing":
        result = timing(a.mapname, a.limit)
    elif a.cmd == "tactics":
        result = tactics(a.mapname, a.limit)
    elif a.cmd == "aim":
        result = aim(a.mapname, a.limit)
    elif a.cmd == "pursue":
        result = pursue(a.mapname, a.limit, a.dump)
    else:
        result = scan(a.mapname, a.limit)

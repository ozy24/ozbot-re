#!/usr/bin/env python3
"""
demo_pov.py -- rewrite a spectator .dm2 into a first-person demo from one
player's eyes, using only what the demo already carries about that player.

OpenTDM's chasecam (and any third-person spectator demo) records the CAMERA,
not the player.  But every frame still carries the player's packet entity:
origin (1/8u), angles (8-bit; pitch stored as v_angle/3 for player models) and
the vwep weapon in skinnum's high byte.  This tool, per frame:

  * replaces the recorder's playerinfo origin / viewangles / viewoffset with the
    target's eye (origin + 22, smoothed angles -- 8-bit angles step 1.4deg yaw /
    4.2deg pitch, so a centered [1,2,1] filter is applied to the unwrapped series),
  * sets the gun to the view model of the weapon the target visibly holds,
    cycling its idle frames (no firing animation: the demo has no weapon frames
    for another player),
  * rewrites svc_serverdata's playernum to the target's slot, so the client
    treats the target's entity as "self" and does not draw its body around the
    camera.

Everything else (entities, sounds, temp entities, prints, HUD stats) is copied
byte-for-byte; only the svc_playerinfo bytes and one s16 in svc_serverdata
change, and block lengths are re-written.

    py tools/demo_pov.py <in.dm2> <player name> <out.dm2>

Approximations to state when showing the result: eye height is always standing
(22u), angles are quantised/smoothed, no weapon kick or fire animation.
"""

import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dm2parse                      # noqa: E402
import dm2_combat as dc              # noqa: E402

PS_M_TYPE, PS_M_ORIGIN, PS_M_VELOCITY = 1, 2, 4
PS_M_TIME, PS_M_FLAGS, PS_M_GRAVITY, PS_M_DELTA_ANGLES = 8, 16, 32, 64
PS_VIEWOFFSET, PS_VIEWANGLES, PS_KICKANGLES = 128, 256, 512
PS_BLEND, PS_FOV, PS_WEAPONINDEX, PS_WEAPONFRAME, PS_RDFLAGS = 1024, 2048, 4096, 8192, 16384
EYE = 22.0

# vwep world model -> (view model, first idle frame, last idle frame)
# idle frames from p_weapon.c Weapon_Generic(FRAME_ACTIVATE_LAST, FRAME_FIRE_LAST,
# FRAME_IDLE_LAST, ...): idle runs FIRE_LAST+1 .. IDLE_LAST
VIEW = {
    "w_blaster": ("models/weapons/v_blast/tris.md2", 9, 52),
    "w_shotgun": ("models/weapons/v_shotg/tris.md2", 19, 36),
    "w_sshotgun": ("models/weapons/v_shotg2/tris.md2", 18, 57),
    "w_machinegun": ("models/weapons/v_machn/tris.md2", 6, 45),
    "w_chaingun": ("models/weapons/v_chain/tris.md2", 32, 61),
    "w_glauncher": ("models/weapons/v_launch/tris.md2", 17, 59),
    "w_rlauncher": ("models/weapons/v_rocket/tris.md2", 13, 50),
    "w_hyperblaster": ("models/weapons/v_hyperb/tris.md2", 21, 49),
    "w_railgun": ("models/weapons/v_rail/tris.md2", 19, 56),
    "w_bfg": ("models/weapons/v_bfg/tris.md2", 33, 55),
}


class _Ents:
    """Packet-entity state for ONE entity number: origin, angles, skinnum."""

    def __init__(self, target):
        self.t = target
        self.origin = None
        self.angles = [0.0, 0.0, 0.0]
        self.skin = 0
        self.present = False

    def _delta(self, r, num, bits):
        mine = num == self.t
        if bits & dc.U_MODEL:  r.u8()
        if bits & dc.U_MODEL2: r.u8()
        if bits & dc.U_MODEL3: r.u8()
        if bits & dc.U_MODEL4: r.u8()
        if bits & dc.U_FRAME8:  r.u8()
        if bits & dc.U_FRAME16: r.u16()
        skin = None
        if (bits & dc.U_SKIN8) and (bits & dc.U_SKIN16): skin = r.s32()
        elif bits & dc.U_SKIN8:  skin = r.u8()
        elif bits & dc.U_SKIN16: skin = r.u16()
        if (bits & dc.U_EFFECTS8) and (bits & dc.U_EFFECTS16): r.s32()
        elif bits & dc.U_EFFECTS8:  r.u8()
        elif bits & dc.U_EFFECTS16: r.u16()
        if (bits & dc.U_RENDERFX8) and (bits & dc.U_RENDERFX16): r.s32()
        elif bits & dc.U_RENDERFX8:  r.u8()
        elif bits & dc.U_RENDERFX16: r.u16()
        o = list(self.origin) if (mine and self.origin) else [0.0, 0.0, 0.0]
        if bits & dc.U_ORIGIN1: o[0] = r.s16() / 8.0
        if bits & dc.U_ORIGIN2: o[1] = r.s16() / 8.0
        if bits & dc.U_ORIGIN3: o[2] = r.s16() / 8.0
        a = list(self.angles) if mine else [0.0, 0.0, 0.0]
        if bits & dc.U_ANGLE1: a[0] = r.u8() * 360.0 / 256
        if bits & dc.U_ANGLE2: a[1] = r.u8() * 360.0 / 256
        if bits & dc.U_ANGLE3: a[2] = r.u8() * 360.0 / 256
        if bits & dc.U_OLDORIGIN: r.skip(6)
        if bits & dc.U_SOUND: r.u8()
        if bits & dc.U_EVENT: r.u8()
        if bits & dc.U_SOLID: r.u16()
        if mine:
            self.origin, self.angles, self.present = o, a, True
            if skin is not None:
                self.skin = skin

    def baseline(self, r):
        bits, num = dc._read_ent_bits(r)
        self._delta(r, num, bits)

    def packet(self, r):
        while True:
            bits, num = dc._read_ent_bits(r)
            if num == 0:
                return
            if bits & dc.U_REMOVE:
                if num == self.t:
                    self.present = False
                continue
            self._delta(r, num, bits)


def _parse_pi(r):
    """Parse svc_playerinfo into an ordered dict of raw field values."""
    f = r.u16()
    v = {}
    if f & PS_M_TYPE: v["type"] = r.u8()
    if f & PS_M_ORIGIN: v["origin"] = (r.s16(), r.s16(), r.s16())
    if f & PS_M_VELOCITY: v["vel"] = (r.s16(), r.s16(), r.s16())
    if f & PS_M_TIME: v["time"] = r.u8()
    if f & PS_M_FLAGS: v["pmflags"] = r.u8()
    if f & PS_M_GRAVITY: v["grav"] = r.s16()
    if f & PS_M_DELTA_ANGLES: v["delta"] = (r.s16(), r.s16(), r.s16())
    if f & PS_VIEWOFFSET: v["voff"] = (r.s8(), r.s8(), r.s8())
    if f & PS_VIEWANGLES: v["vang"] = (r.s16(), r.s16(), r.s16())
    if f & PS_KICKANGLES: v["kick"] = (r.s8(), r.s8(), r.s8())
    if f & PS_WEAPONINDEX: v["gun"] = r.u8()
    if f & PS_WEAPONFRAME: v["gunframe"] = (r.u8(), r.s8(), r.s8(), r.s8(), r.s8(), r.s8(), r.s8())
    if f & PS_BLEND: v["blend"] = (r.u8(), r.u8(), r.u8(), r.u8())
    if f & PS_FOV: v["fov"] = r.u8()
    if f & PS_RDFLAGS: v["rdflags"] = r.u8()
    sb = r.u32()
    stats = [(i, r.s16()) for i in range(32) if sb & (1 << i)]
    return v, sb, stats


def _write_pi(v, sb, stats):
    f = 0
    out = bytearray()
    order = [("type", PS_M_TYPE, "<B"), ("origin", PS_M_ORIGIN, "<hhh"), ("vel", PS_M_VELOCITY, "<hhh"),
             ("time", PS_M_TIME, "<B"), ("pmflags", PS_M_FLAGS, "<B"), ("grav", PS_M_GRAVITY, "<h"),
             ("delta", PS_M_DELTA_ANGLES, "<hhh"), ("voff", PS_VIEWOFFSET, "<bbb"),
             ("vang", PS_VIEWANGLES, "<hhh"), ("kick", PS_KICKANGLES, "<bbb"),
             ("gun", PS_WEAPONINDEX, "<B"), ("gunframe", PS_WEAPONFRAME, "<Bbbbbbb"),
             ("blend", PS_BLEND, "<BBBB"), ("fov", PS_FOV, "<B"), ("rdflags", PS_RDFLAGS, "<B")]
    for key, bit, fmt in order:
        if key in v:
            f |= bit
            val = v[key]
            out += struct.pack(fmt, *(val if isinstance(val, tuple) else (val,)))
    hdr = struct.pack("<H", f)
    tail = struct.pack("<I", sb) + b"".join(struct.pack("<h", s) for _, s in stats)
    return hdr + bytes(out) + tail


def _a16(deg):
    v = int(round(deg * 65536.0 / 360.0)) & 0xFFFF
    return v - 65536 if v >= 32768 else v


def _smooth(series):
    """Centered [1,2,1] on an angle series in degrees (unwrapped first)."""
    if not series:
        return series
    un = [series[0]]
    for a in series[1:]:
        d = (a - un[-1] + 180) % 360 - 180
        un.append(un[-1] + d)
    out = []
    for i in range(len(un)):
        a, b, c = un[max(0, i - 1)], un[i], un[min(len(un) - 1, i + 1)]
        out.append((a + 2 * b + c) / 4.0)
    return out


def _skip_to_frame(r):
    """Advance through a block's leading messages to svc_frame's playerinfo.
    Returns (serverdata_playernum_offset or None, True if positioned at
    playerinfo).  Mirrors the message set the other dm2 tools understand."""
    sd_off = None
    while not r.eob():
        cmd = r.u8()
        if cmd == dc.SVC_SERVERDATA:
            r.s32(); r.s32(); r.u8(); r.string()
            sd_off = r.o
            r.s16(); r.string()
        elif cmd == dc.SVC_CONFIGSTRING:
            r.u16(); r.string()
        elif cmd == dc.SVC_PRINT:
            r.u8(); r.string()
        elif cmd in (dc.SVC_STUFFTEXT, dc.SVC_CENTERPRINT, dc.SVC_LAYOUT):
            r.string()
        elif cmd == dc.SVC_INVENTORY:
            r.skip(512)
        elif cmd == dc.SVC_NOP:
            continue
        elif cmd == dc.SVC_SPAWNBASELINE:
            return sd_off, False          # baselines: nothing to rewrite after
        elif cmd == dc.SVC_FRAME:
            r.s32(); r.s32(); r.u8(); r.skip(r.u8())
            return sd_off, r.u8() == dc.SVC_PLAYERINFO
        else:
            return sd_off, False
    return sd_off, False


def main():
    src, who, dst = sys.argv[1], sys.argv[2], sys.argv[3]
    data = open(src, "rb").read()

    # ---- pass 1: names, configstring models, and the target's per-frame state
    names, models = {}, {}
    for block in dm2parse.iter_blocks(data):
        r = dm2parse.Reader(block)
        while not r.eob():
            cmd = r.u8()
            if cmd == dc.SVC_CONFIGSTRING:
                idx = r.u16(); s = r.string()
                if dc.CS_PLAYERSKINS <= idx < dc.CS_PLAYERSKINS + 256:
                    names[idx - dc.CS_PLAYERSKINS] = s.split("\\")[0]
                elif dc.CS_MODELS <= idx < dc.CS_MODELS + 256:
                    models[idx - dc.CS_MODELS] = s
            elif cmd == dc.SVC_SERVERDATA:
                r.s32(); r.s32(); r.u8(); r.string(); r.s16(); r.string()
            elif cmd == dc.SVC_PRINT: r.u8(); r.string()
            elif cmd in (dc.SVC_STUFFTEXT, dc.SVC_CENTERPRINT, dc.SVC_LAYOUT): r.string()
            elif cmd == dc.SVC_INVENTORY: r.skip(512)
            elif cmd == dc.SVC_NOP: continue
            else: break
    slot = next((k for k, v in names.items() if v == who), None)
    if slot is None:
        sys.exit(f"player '{who}' not in demo; players: {sorted(names.values())}")
    ent = slot + 1
    vweps = [m.lstrip("#").rsplit(".", 1)[0] for _, m in sorted(models.items()) if m.startswith("#")]
    view_idx = {m: i for i, m in models.items()}
    print(f"target '{who}' = slot {slot}; vwep list: {vweps}")

    es = _Ents(ent)
    states = []                       # per frame: (origin or None, angles, skin)
    for block in dm2parse.iter_blocks(data):
        r = dm2parse.Reader(block)
        while not r.eob():
            cmd = r.u8()
            if cmd == dc.SVC_CONFIGSTRING: r.u16(); r.string()
            elif cmd == dc.SVC_SERVERDATA: r.s32(); r.s32(); r.u8(); r.string(); r.s16(); r.string()
            elif cmd == dc.SVC_PRINT: r.u8(); r.string()
            elif cmd in (dc.SVC_STUFFTEXT, dc.SVC_CENTERPRINT, dc.SVC_LAYOUT): r.string()
            elif cmd == dc.SVC_INVENTORY: r.skip(512)
            elif cmd == dc.SVC_NOP: continue
            elif cmd == dc.SVC_SPAWNBASELINE:
                try: es.baseline(r)
                except (IndexError, ValueError): break
            elif cmd == dc.SVC_FRAME:
                r.s32(); r.s32(); r.u8(); r.skip(r.u8())
                if r.u8() != dc.SVC_PLAYERINFO: break
                _parse_pi(r)
                try:
                    if r.u8() == dc.SVC_PACKETENTITIES:
                        es.packet(r)
                except (IndexError, ValueError):
                    pass
                states.append((tuple(es.origin) if (es.present and es.origin) else None,
                               tuple(es.angles), es.skin))
                break
            else:
                break
    yaw = _smooth([s[1][1] for s in states])
    pitch = _smooth([(s[1][0] if s[1][0] < 180 else s[1][0] - 360) * 3.0 for s in states])
    print(f"frames: {len(states)}, target present in {sum(s[0] is not None for s in states)}")

    # ---- pass 2: rewrite
    out = bytearray()
    fi, rewritten, sd_fixed = 0, 0, 0
    last_o = None
    off = 0
    while off + 4 <= len(data):
        (blen,) = struct.unpack_from("<i", data, off)
        if blen < 0 or off + 4 + blen > len(data):
            out += data[off:]
            break
        block = bytearray(data[off + 4:off + 4 + blen])
        off += 4 + blen
        r = dm2parse.Reader(bytes(block))
        sd_off, at_pi = _skip_to_frame(r)
        if sd_off is not None:
            struct.pack_into("<h", block, sd_off, slot)
            sd_fixed += 1
        if at_pi and fi < len(states):
            start = r.o
            v, sb, stats = _parse_pi(r)
            end = r.o
            o, _a, skin = states[fi]
            if o is not None:
                last_o = o
            if last_o is not None:
                v["origin"] = tuple(int(round(c * 8)) for c in last_o)
                v["vang"] = (_a16(pitch[fi]), _a16(yaw[fi]), 0)
                v["voff"] = (0, 0, int(EYE * 4))
                v["delta"] = (0, 0, 0)
                v["kick"] = (0, 0, 0)
                v["type"] = 0                       # PM_NORMAL: plain first-person view
                v["fov"] = v.get("fov", 90)
                w = (skin >> 8) & 0xFF
                # client weaponmodel[0] is the default weapon.md2; '#' models follow
                # in precache order = WEAP_* (1-based), so skinnum>>8 indexes from 1
                name = vweps[w - 1] if 1 <= w <= len(vweps) else None
                if name in VIEW and VIEW[name][0] in view_idx:
                    model, lo, hi = VIEW[name]
                    v["gun"] = view_idx[model]
                    v["gunframe"] = (lo + fi % (hi - lo + 1), 0, 0, 0, 0, 0, 0)
                else:
                    v["gun"] = 0
                block = block[:start] + _write_pi(v, sb, stats) + block[end:]
                rewritten += 1
            fi += 1
        elif at_pi:
            fi += 1
        out += struct.pack("<i", len(block)) + block
    open(dst, "wb").write(bytes(out))
    print(f"wrote {dst}: {rewritten} frames re-viewed, serverdata playernum set in {sd_fixed} block(s)")


if __name__ == "__main__":
    main()

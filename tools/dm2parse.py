#!/usr/bin/env python3
"""
Quake 2 protocol-34 .dm2 parser for the ozbot demo->navmesh importer.

A .dm2 is a series of length-prefixed blocks (int32 length, then that many bytes
of svc_* messages; length == -1 ends the demo).  In each server packet the
reliable messages and svc_frame come first, and unreliable sounds/temp-entities
come *after* the frame.  Since blocks are length-delimited, we only need to
parse each block up to svc_frame's playerinfo (which carries the recording
player's pmove.origin), then skip to the next block -- so we never have to parse
the complex sound / temp_entity messages, and a misparse can't desync the stream.

Output: the recording player's position trajectory (origin/8 per frame) + map +
player names, for feeding into the nav-graph learner.

Usage:
    python dm2parse.py inspect <demo.dm2>
    python dm2parse.py traj <demo.dm2> [out.csv]
"""

import csv
import os
import re
import struct
import sys

# svc message ids (protocol 34)
SVC_MUZZLEFLASH = 1
SVC_MUZZLEFLASH2 = 2
SVC_TEMP_ENTITY = 3
SVC_LAYOUT = 4
SVC_INVENTORY = 5
SVC_NOP = 6
SVC_DISCONNECT = 7
SVC_RECONNECT = 8
SVC_SOUND = 9
SVC_PRINT = 10
SVC_STUFFTEXT = 11
SVC_SERVERDATA = 12
SVC_CONFIGSTRING = 13
SVC_SPAWNBASELINE = 14
SVC_CENTERPRINT = 15
SVC_DOWNLOAD = 16
SVC_PLAYERINFO = 17
SVC_FRAME = 20

# playerstate flags
PS_M_TYPE, PS_M_ORIGIN, PS_M_VELOCITY = 1, 2, 4
PS_M_TIME, PS_M_FLAGS, PS_M_GRAVITY, PS_M_DELTA_ANGLES = 8, 16, 32, 64
PS_VIEWOFFSET, PS_VIEWANGLES, PS_KICKANGLES = 128, 256, 512
PS_BLEND, PS_FOV, PS_WEAPONINDEX, PS_WEAPONFRAME, PS_RDFLAGS = 1024, 2048, 4096, 8192, 16384

MAX_STATS = 32
CS_MODELS = 32
CS_PLAYERSKINS = 1312   # CS_ITEMS(1056) + MAX_ITEMS(256)
MAX_CLIENTS = 256
STAT_HEALTH = 1

ANGLE16 = 360.0 / 65536.0


class Reader:
    __slots__ = ("b", "o")

    def __init__(self, buf):
        self.b = buf
        self.o = 0

    def eob(self):
        return self.o >= len(self.b)

    def u8(self):
        v = self.b[self.o]; self.o += 1; return v

    def s8(self):
        v = self.u8(); return v - 256 if v >= 128 else v

    def u16(self):
        b = self.b; o = self.o; self.o += 2
        return b[o] | (b[o + 1] << 8)

    def s16(self):
        v = self.u16(); return v - 65536 if v >= 32768 else v

    def u32(self):
        b = self.b; o = self.o; self.o += 4
        return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)

    def s32(self):
        v = self.u32(); return v - (1 << 32) if v >= (1 << 31) else v

    def string(self):
        b = self.b; start = self.o
        n = len(b)
        while self.o < n and b[self.o] != 0:
            self.o += 1
        s = b[start:self.o].decode("latin-1")
        if self.o < n:
            self.o += 1  # skip null
        return s

    def skip(self, n):
        self.o += n


def iter_blocks(data):
    off = 0
    while off + 4 <= len(data):
        (blen,) = struct.unpack_from("<i", data, off)
        off += 4
        if blen == -1 or blen < 0 or off + blen > len(data):
            return
        yield data[off:off + blen]
        off += blen


def parse_playerinfo(r, stats=None):
    """Returns (origin, viewangles): origin (x,y,z in 1/8 units) if PS_M_ORIGIN
    present else None; viewangles (pitch,yaw,roll in degrees) if PS_VIEWANGLES
    present else None.  If 'stats' is a 32-slot list, playerstate stat deltas
    are decoded into it (STAT_HEALTH lives at index 1)."""
    flags = r.u16()
    origin = viewangles = None
    if flags & PS_M_TYPE:
        r.skip(1)
    if flags & PS_M_ORIGIN:
        origin = (r.s16(), r.s16(), r.s16())
    if flags & PS_M_VELOCITY:
        r.skip(6)
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
        viewangles = (r.s16() * ANGLE16, r.s16() * ANGLE16, r.s16() * ANGLE16)
    if flags & PS_KICKANGLES:
        r.skip(3)
    if flags & PS_WEAPONINDEX:
        r.skip(1)
    if flags & PS_WEAPONFRAME:
        r.skip(7)            # gunframe(1) + gunoffset(3) + gunangles(3)
    if flags & PS_BLEND:
        r.skip(4)
    if flags & PS_FOV:
        r.skip(1)
    if flags & PS_RDFLAGS:
        r.skip(1)
    statbits = r.u32()
    if stats is None:
        r.skip(2 * bin(statbits).count("1"))
    else:
        for i in range(32):
            if statbits & (1 << i):
                stats[i] = r.s16()
    return origin, viewangles


def parse(path):
    return parse_data(open(path, "rb").read())


def parse_data(data):
    # "frames" stays a plain (x,y,z) tuple list for the older consumers
    # (demo_to_nav / demo_coverage / nav_add_route); the humanness profiler's
    # extras ride in parallel lists aligned index-for-index with it:
    #   views    -- (pitch, yaw) degrees, carried forward when unchanged
    #   healths  -- recorder's STAT_HEALTH, carried forward
    #   sframes  -- serverframe number (consecutive == no dropped frames)
    info = {"map": None, "protocol": None, "playernum": None,
            "levelname": None, "gamedir": None, "names": {}, "frames": [],
            "views": [], "healths": [], "sframes": []}

    m = re.search(rb"maps/([A-Za-z0-9_]+)\.bsp", data)
    if m:
        info["map"] = m.group(1).decode()

    stats = [0] * MAX_STATS
    last_origin = None
    last_view = (0.0, 0.0)
    for block in iter_blocks(data):
        if not block:
            continue
        r = Reader(block)
        while not r.eob():
            cmd = r.u8()
            if cmd == SVC_SERVERDATA:
                info["protocol"] = r.s32()
                r.s32()                     # servercount
                r.u8()                      # attractloop
                info["gamedir"] = r.string()
                info["playernum"] = r.s16()
                info["levelname"] = r.string()
            elif cmd == SVC_CONFIGSTRING:
                idx = r.u16()
                s = r.string()
                if CS_PLAYERSKINS <= idx < CS_PLAYERSKINS + MAX_CLIENTS:
                    info["names"][idx - CS_PLAYERSKINS] = s.split("\\")[0]
            elif cmd == SVC_PRINT:
                r.u8(); r.string()
            elif cmd in (SVC_STUFFTEXT, SVC_CENTERPRINT, SVC_LAYOUT):
                r.string()
            elif cmd == SVC_INVENTORY:
                r.skip(2 * 256)
            elif cmd == SVC_NOP:
                continue
            elif cmd in (SVC_MUZZLEFLASH, SVC_MUZZLEFLASH2):
                r.skip(3)               # entity short + weapon byte
            elif cmd == SVC_DOWNLOAD:
                size = r.s16()
                if size >= 0:
                    r.u8(); r.skip(size)
            elif cmd == SVC_FRAME:
                sframe = r.s32()        # serverframe
                r.s32()                 # deltaframe
                r.u8()                  # surpresscount (proto != 26)
                arealen = r.u8()
                r.skip(arealen)
                sub = r.u8()            # should be SVC_PLAYERINFO
                if sub == SVC_PLAYERINFO:
                    o, va = parse_playerinfo(r, stats)
                    if o is not None:
                        last_origin = o
                    if va is not None:
                        last_view = (va[0], va[1])
                    if last_origin is not None:
                        info["frames"].append(
                            (last_origin[0] / 8.0,
                             last_origin[1] / 8.0,
                             last_origin[2] / 8.0))
                        info["views"].append(last_view)
                        info["healths"].append(stats[STAT_HEALTH])
                        info["sframes"].append(sframe)
                break                   # rest of block = entities + datagram; skip
            elif cmd in (SVC_DISCONNECT, SVC_RECONNECT):
                break
            else:
                # spawnbaseline / sound / temp_entity / unknown:
                # everything we need precedes these, so skip to the next block
                break
    return info


def inspect(path):
    info = parse(path)
    print(f"map        = {info['map']}")
    print(f"protocol   = {info['protocol']}")
    print(f"gamedir    = {info['gamedir']}")
    print(f"levelname  = {info['levelname']}")
    print(f"playernum  = {info['playernum']}  ({info['names'].get(info['playernum'])})")
    print(f"players    = {info['names']}")
    fr = info["frames"]
    print(f"frames w/ position = {len(fr)}")
    if fr:
        xs = [p[0] for p in fr]; ys = [p[1] for p in fr]; zs = [p[2] for p in fr]
        print(f"x range {min(xs):.0f}..{max(xs):.0f}  "
              f"y range {min(ys):.0f}..{max(ys):.0f}  "
              f"z range {min(zs):.0f}..{max(zs):.0f}")
        print("first 5 positions:", [tuple(round(c) for c in p) for p in fr[:5]])
        # total path length as a sanity signal
        import math
        plen = sum(math.dist(fr[i - 1], fr[i]) for i in range(1, len(fr)))
        print(f"path length = {plen:.0f} units")
        vw = info["views"]
        ps = [v[0] for v in vw]
        print(f"pitch range {min(ps):.0f}..{max(ps):.0f}  "
              f"health range {min(info['healths'])}..{max(info['healths'])}  "
              f"first view {vw[0]}  last view {vw[-1]}")


def traj(path, out=None):
    info = parse(path)
    out = out or (path.rsplit(".", 1)[0] + ".traj.csv")
    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["x", "y", "z"])
        w.writerows([(round(p[0], 1), round(p[1], 1), round(p[2], 1))
                     for p in info["frames"]])
    print(f"map={info['map']} player={info['names'].get(info['playernum'])} "
          f"frames={len(info['frames'])} -> {out}")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "inspect"
    _root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    path = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.join(_root, "demos", "work", "sample_q2dm1.dm2")
    if cmd == "traj":
        traj(path, sys.argv[3] if len(sys.argv) > 3 else None)
    else:
        inspect(path)

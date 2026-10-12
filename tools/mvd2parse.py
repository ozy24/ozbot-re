#!/usr/bin/env python3
"""
Reader for Q2REX multi-view recordings (.mvd2): the DM Duel Ladder's
server-side match demos.  Unlike a protocol-34 .dm2 (the recorder's own POV,
10Hz, opponent only while in PVS), an MVD carries EVERY player's full
player_state at the server tick rate -- 40Hz on the ladder.

Format (frozen; authoritative copy is the header comment of Q2REX
source/server/sv_mvd.cpp, and the ladder site's replay/mvd.js reader this is
ported from): "MVD2" magic, then packets of u16 LE length + payload, each
payload starting with a command byte; a zero length ends the stream.

    4  serverdata   u32 major(37) u16 minor(3039) u16 flags u32 spawncount
                    u16 tick_rate u16 maxclients u16 maxentities i16 view
                    str gamedir str map                (str = u16 len + bytes)
    5  configstring u32 frame u16 index str value
    6  frame begin  u32 frame u64 server_time_ms str portal_state
    19 frame end    u32 frame
    20 player       u32 frame u16 slot u8 flags(bit0 present) u16 len state
    21 entity       (skipped)   22 multicast (skipped)   23 unicast (skipped)
    24 print        u32 frame i16 slot u8 reliable i16 level str text
    25 gamestate end

Player records are delta coded: after the first full frame a slot is only
written when its state changes, so the last state is carried forward.  Only
player records, configstrings and prints are decoded -- entity and message
payloads are skipped by length, which keeps a 10-minute (22 MB) demo to a
couple of seconds.

Usage:
    py mvd2parse.py inspect <file.mvd2[.gz]>
"""

import gzip
import struct
import sys

CMD_SERVERDATA, CMD_CONFIGSTRING, CMD_FRAME_BEGIN = 4, 5, 6
CMD_FRAME_END, CMD_PLAYER, CMD_PRINT, CMD_GAMESTATE_END = 19, 20, 24, 25

# Canonical Q2RE player_state record, field by field (mvd.js decodePlayer).
PLAYER_FMT = struct.Struct(
    "<i"        # pm_type
    "3f3f"      # origin, velocity
    "HHh"       # pm_flags, pm_time, gravity
    "3f"        # delta_angles
    "b"         # viewheight
    "3f3f3f"    # viewangles, viewoffset, kick_angles
    "24x"       # gun angles + offset
    "i"         # gunindex
    "12x"       # gun skin, frame, rate
    "32x"       # screen + damage blend
    "f"         # fov
    "B"         # rdflags
    "64h"       # stats
    "B")        # team id
PLAYER_STATE_BYTES = 289
assert PLAYER_FMT.size == PLAYER_STATE_BYTES

# Configstring layout (mmrex/src/shared/game.hpp) and stats indices
# (mmrex/src/shared/gameplay.hpp), as used by the ladder's replay reader.
# NOTE: STAT_SCORE does not hold the frag score in ladder demos (checked
# against the match record); count deaths from health instead.
CS_PLAYERSKINS = 11582
STAT_HEALTH, STAT_AMMO, STAT_ARMOR, STAT_SCORE, STAT_SPECTATOR = 1, 3, 5, 14, 17
PM_NORMAL = 0           # pmtype_t: PM_NORMAL; spectator/dead/freeze are > 0


class PlayerState:
    __slots__ = ("pm_type", "origin", "velocity", "pm_flags", "pm_time",
                 "viewheight", "viewangles", "gunindex", "fov", "stats")

    def __init__(self, f):
        self.pm_type = f[0]
        self.origin = f[1:4]
        self.velocity = f[4:7]
        self.pm_flags, self.pm_time = f[7], f[8]
        self.viewheight = f[13]
        self.viewangles = f[14:17]
        self.gunindex = f[23]
        self.fov = f[24]
        self.stats = f[26:90]

    @property
    def health(self):
        return self.stats[STAT_HEALTH]

    @property
    def armor(self):
        return self.stats[STAT_ARMOR]

    @property
    def spectator(self):
        return self.stats[STAT_SPECTATOR] != 0


def _str(data, o):
    n = struct.unpack_from("<H", data, o)[0]
    return data[o + 2:o + 2 + n].decode("utf-8", "replace"), o + 2 + n


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    return data


def parse(path):
    return parse_data(load(path))


def parse_data(data):
    """Decode a recording.  Returns a dict:

        header        serverdata fields (protocol, minor, tick_rate, map, ...)
        frames        [(frame_num, server_time_ms)] per completed frame
        players       {slot: [PlayerState | None per frame index]}
        names         {slot: name} from the player-skin configstrings
        configstrings final value per index
        prints        [(frame_index, slot, level, text)]
    """
    if data[:4] != b"MVD2":
        raise ValueError(f"not a multi-view recording (magic {data[:4]!r})")

    header = None
    cs = {}
    frames = []
    players = {}            # slot -> per-frame list
    live = {}               # slot -> current PlayerState (carried forward)
    prints = []
    cur = None              # (num, time) of the frame being assembled
    unpack = PLAYER_FMT.unpack_from
    o, end = 4, len(data)

    while o + 2 <= end:
        length = struct.unpack_from("<H", data, o)[0]
        o += 2
        if length == 0:
            break
        p = o
        o += length
        if o > end:
            break                           # truncated final packet
        cmd = data[p]
        p += 1
        if cmd == CMD_PLAYER:
            slot, flags, size = struct.unpack_from("<HBH", data, p + 4)
            if flags & 1:
                if size != PLAYER_STATE_BYTES:
                    raise ValueError(f"player state of {size} bytes")
                live[slot] = PlayerState(unpack(data, p + 9))
            else:
                live[slot] = None
        elif cmd == CMD_FRAME_BEGIN:
            num, t = struct.unpack_from("<IQ", data, p)
            cur = (num, t)
        elif cmd == CMD_FRAME_END:
            if cur is not None:
                idx = len(frames)
                frames.append(cur)
                for slot, st in live.items():
                    lst = players.get(slot)
                    if lst is None:
                        lst = players[slot] = [None] * idx
                    lst.append(st)
                for slot, lst in players.items():
                    if len(lst) <= idx:     # slot dropped out of `live`
                        lst.append(None)
                cur = None
        elif cmd == CMD_CONFIGSTRING:
            index = struct.unpack_from("<H", data, p + 4)[0]
            cs[index], _ = _str(data, p + 6)
        elif cmd == CMD_PRINT:
            slot, _rel, level = struct.unpack_from("<hBh", data, p + 4)
            text, _ = _str(data, p + 9)
            prints.append((len(frames), slot, level, text))
        elif cmd == CMD_SERVERDATA:
            (major, minor, flags, spawn, tick, maxcl, maxent,
             view) = struct.unpack_from("<IHHIHHHh", data, p)
            gamedir, q = _str(data, p + 20)
            mapname, _ = _str(data, q)
            header = {"protocol": major, "minor": minor, "flags": flags,
                      "tick_rate": tick, "maxclients": maxcl,
                      "maxentities": maxent, "gamedir": gamedir,
                      "map": mapname}
        # entity / multicast / unicast / gamestate end: skipped by length

    names = {}
    for slot in players:
        v = cs.get(CS_PLAYERSKINS + slot)
        if v:
            names[slot] = v.split("\\")[0]
    return {"header": header, "frames": frames, "players": players,
            "names": names, "configstrings": cs, "prints": prints}


def inspect(path):
    d = parse(path)
    h, fr = d["header"], d["frames"]
    print(f"{path}")
    print(f"  protocol {h['protocol']}.{h['minor']}  tick {h['tick_rate']}Hz  "
          f"map {h['map']}  gamedir {h['gamedir']}")
    if fr:
        print(f"  frames {len(fr)}  span {(fr[-1][1] - fr[0][1]) / 1000:.1f}s")
    for slot, lst in sorted(d["players"].items()):
        present = [s for s in lst if s is not None]
        playing = [s for s in present if not s.spectator and s.pm_type == PM_NORMAL]
        deaths = sum(1 for a, b in zip(lst, lst[1:])
                     if a is not None and b is not None
                     and a.health > 0 >= b.health)
        print(f"  slot {slot} {d['names'].get(slot, '?'):<16s} "
              f"present {len(present):6d}  playing {len(playing):6d}  "
              f"deaths {deaths}")
    print(f"  prints {len(d['prints'])}")


def main():
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == "inspect":
        for p in args[1:]:
            inspect(p)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()

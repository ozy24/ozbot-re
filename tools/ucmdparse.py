#!/usr/bin/env python3
"""
Reader for the duel ladder's usercmd sidecars (.ucmd): every command the
server executed for a playing slot during a recorded match, written by Q2RED
beside the match's .mvd2 when sv_mvd_usercmds is 1 (Q2RED divergence A47;
format spec: Q2RED docs/reverse-engineering/evidence-catalogs/server/
mvd-usercmd-sidecar-2026-10-13.md).  The .mvd2 carries both players' states
(tools/mvd2parse.py); the .ucmd carries what they pressed -- together they are
state/action pairs.

Format (UCMD v1, little endian): "UCMD", then packets of u16 length + payload,
a zero length ends a clean stream.  Payload type 1 = header, type 2 = one
command: u32 frame, u8 slot, u8 mask, then the fields whose mask bit is set,
each written only when it changed since that slot's previous command:
bit0 u8 msec, bit1 u8 buttons, bit2-4 i16 angle deltas (1/65536 turn,
wrapping), bit5 i16 forwardmove, bit6 i16 sidemove, bit7 u8 lag in frames.

Usage:
    py ucmdparse.py inspect <file.ucmd[.gz]> [more ...]
"""

import gzip
import struct
import sys

UCMD_MAGIC = b"UCMD"
# rerelease buttons (game.h button_t)
BUTTON_ATTACK, BUTTON_USE, BUTTON_HOLSTER, BUTTON_JUMP, BUTTON_CROUCH = 1, 2, 4, 8, 16


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    return data


def parse(path):
    return parse_data(load(path))


def parse_data(data):
    """Returns {"header": {...}, "commands": [...], "complete": bool}.

    Each command is a dict: frame, slot, msec, buttons, angles (pitch, yaw,
    roll in degrees, 0..360), forward, side, acked_frame (the server frame
    the client had seen: frame - lag).  "complete" is False when the stream
    was cut short (the records before the cut are still valid)."""
    if data[:4] != UCMD_MAGIC:
        raise ValueError(f"not a usercmd sidecar (magic {data[:4]!r})")
    header, commands, complete = None, [], False
    state = {}          # slot -> [msec, buttons, a0, a1, a2, forward, side, lag]
    o, end = 4, len(data)
    while o + 2 <= end:
        (length,) = struct.unpack_from("<H", data, o)
        o += 2
        if length == 0:
            complete = True
            break
        if o + length > end:
            break
        p, stop = o, o + length
        o = stop
        kind = data[p]
        p += 1
        if kind == 1:
            version, tick, capacity, spawn, first = struct.unpack_from("<HHHII", data, p)
            p += 14
            (ml,) = struct.unpack_from("<H", data, p)
            mapname = data[p + 2:p + 2 + ml].decode("utf-8", "replace")
            p += 2 + ml
            (dl,) = struct.unpack_from("<H", data, p)
            demo = data[p + 2:p + 2 + dl].decode("utf-8", "replace")
            header = {"version": version, "tick_rate": tick, "player_capacity": capacity,
                      "spawncount": spawn, "first_frame": first, "map": mapname, "demo": demo}
            if version != 1:
                raise ValueError(f"unsupported UCMD version {version}")
            continue
        if kind != 2:
            continue                    # unknown record type: skip by length
        frame, slot, mask = struct.unpack_from("<IBB", data, p)
        p += 6
        s = state.setdefault(slot, [0, 0, 0, 0, 0, 0, 0, 0])
        if mask & 1:
            s[0] = data[p]
            p += 1
        if mask & 2:
            s[1] = data[p]
            p += 1
        for i in range(3):
            if mask & (4 << i):
                (delta,) = struct.unpack_from("<h", data, p)
                s[2 + i] = (s[2 + i] + delta) & 0xFFFF
                p += 2
        if mask & 32:
            (s[5],) = struct.unpack_from("<h", data, p)
            p += 2
        if mask & 64:
            (s[6],) = struct.unpack_from("<h", data, p)
            p += 2
        if mask & 128:
            s[7] = data[p]
            p += 1
        if p != stop:
            raise ValueError(f"malformed command record at byte {stop - length - 2}")
        commands.append({
            "frame": frame, "slot": slot, "msec": s[0], "buttons": s[1],
            "angles": tuple(v * 360.0 / 65536.0 for v in s[2:5]),
            "forward": s[5], "side": s[6], "acked_frame": frame - s[7],
        })
    return {"header": header, "commands": commands, "complete": complete}


def by_slot(parsed):
    """{slot: [commands in order]} -- convenient for per-player analysis."""
    out = {}
    for c in parsed["commands"]:
        out.setdefault(c["slot"], []).append(c)
    return out


def inspect(path):
    d = parse(path)
    h = d["header"] or {}
    print(path)
    print(f"  UCMD v{h.get('version')}  map {h.get('map')}  demo {h.get('demo')}  "
          f"tick {h.get('tick_rate')}Hz  first frame {h.get('first_frame')}  "
          f"{'complete' if d['complete'] else 'CUT SHORT'}")
    for slot, cmds in sorted(by_slot(d).items()):
        frames = cmds[-1]["frame"] - cmds[0]["frame"] + 1
        rate = len(cmds) / (frames / max(1, h.get("tick_rate") or 40))
        jumps = sum(1 for a, b in zip(cmds, cmds[1:])
                    if b["buttons"] & BUTTON_JUMP and not a["buttons"] & BUTTON_JUMP)
        lag = sorted(c["frame"] - c["acked_frame"] for c in cmds)
        print(f"  slot {slot}: {len(cmds)} commands over {frames} frames (~{rate:.0f}/s), "
              f"{jumps} jump presses, lag p50 {lag[len(lag) // 2]} frames")


def main():
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == "inspect":
        for p in args[1:]:
            inspect(p)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()

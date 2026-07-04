# Player input logging — implementation & port to the Kex (Q2 remaster) engine

This documents how ozbot-re logs a **real player's per-frame `usercmd_t`** (the `bot_inputlog`
feature) and exactly what changes to port it into the **Kex / Q2-remaster re-release game DLL**
(Paril's `quake2-rerelease-dll`, C++). A `.dm2` demo records *positions and view angles* but
**not the raw inputs** (move axes, jump/attack buttons); this feature captures those inputs, one
JSON record per client frame, so a human maneuver can be replayed or analyzed tick-by-tick.

Source of truth (classic implementation): `src/bot_log.c`, `src/p_client.c` (`ClientThink`),
`src/bot_main.c` (cvar + `Bot_IsClient`). Consumer: `tools/input_view.py`, `tools/make_playbook.py`.

---

## 1. What it does (behavioural spec — must be preserved on Kex)

- One JSON object per line (JSONL) written to `<gamedir>/logs/<map>_<timestamp>.jsonl`.
- The **first** line of every file is a `run` header (map, tick rate, maxclients).
- For **every real player**, **every client frame** (i.e. every `ClientThink` call), one `input`
  record capturing that frame's `usercmd_t` plus the state the player is acting from
  (origin, velocity, on-ground, water level).
- **Bots are excluded** (they are driven through the same `ClientThink`) — in ozbot via
  `!Bot_IsClient(ent)`. On Kex with no bots, this gate is simply "is this a real client."
- Gated by a cvar (`bot_inputlog`, default 0), so it costs nothing when off.
- Buffered stdio, flushed ~1 Hz to bound data loss without per-line I/O cost; closed on map end.

The whole point is fidelity of the **inputs**: `forwardmove`/`sidemove`/(jump), `attack`, and the
per-frame **view yaw/pitch sweep** — the things a demo can't give you.

---

## 2. The record schema (keep field names identical so `tools/` still parse)

### `run` header (one per file, first line)
```json
{"type":"run","map":"q2dm1","tick_rate":40,"maxclients":16}
```
- `tick_rate` = `round(1 / FRAMETIME)` — 10 on stock, 40 on the ozbot-re 40Hz port. Analyzers
  default to 10 if absent. **No wall-clock fields** (a same-seed byte-exact hash gate includes
  this line).

### `input` record (one per real player per client frame)
```json
{"type":"input","t":12.34,"slot":0,"msec":25,"fwd":400,"side":0,"up":200,"atk":0,
 "yaw":137.02,"pitch":-3.51,"x":688.0,"y":1168.1,"z":792.1,
 "vx":300.0,"vy":0.0,"vz":0.0,"spd":300.0,"onground":true,"water":0}
```

| field | meaning | classic source | **Kex source (changes!)** |
|---|---|---|---|
| `t` | game time, seconds | `level.time` (float) | `level.time` is **`gtime_t`** → use `.seconds()` (see §4) |
| `slot` | client index (0-based) | `ent - g_edicts - 1` | same idea; `ent` is a `gentity_t*` |
| `msec` | frame duration ms | `ucmd->msec` | `cmd.msec` (still `byte`) |
| `fwd` | forward move | `ucmd->forwardmove` (`short`) | `cmd.forwardmove` (**`float`**) |
| `side` | side move | `ucmd->sidemove` (`short`) | `cmd.sidemove` (**`float`**) |
| `up` | **jump/up** | `ucmd->upmove` (`short`, jump = `>0`) | **`upmove` REMOVED** — emit `(cmd.buttons & BUTTON_JUMP) ? <nonzero> : 0`; consider also logging crouch (`BUTTON_CROUCH`) |
| `atk` | attack held | `(ucmd->buttons & BUTTON_ATTACK)?1:0` | same, `BUTTON_ATTACK == BIT(0)` |
| `yaw` | view yaw, degrees | `SHORT2ANGLE(ucmd->angles[YAW])` | `cmd.angles[YAW]` — **already float degrees, no conversion** |
| `pitch` | view pitch, degrees | `SHORT2ANGLE(ucmd->angles[PITCH])` | `cmd.angles[PITCH]` — **already float degrees** |
| `x,y,z` | origin | `ent->s.origin` | `ent->s.origin` (`vec3_t`) |
| `vx,vy,vz` | velocity | `ent->velocity` | `ent->velocity` |
| `spd` | horizontal speed | `sqrt(vx²+vy²)` | same |
| `onground` | grounded | `ent->groundentity != NULL` | `ent->groundentity` (verify null semantics) |
| `water` | water level | `ent->waterlevel` | `ent->waterlevel` (an enum in Kex — cast to int) |

**`tools/` compatibility:** `input_view.py` / `make_playbook.py` filter on `"type":"input"` and
read `up` as "jump if `up>0`". If you emit jump as `(BUTTON_JUMP ? 200 : 0)` the existing tools
work unchanged. Keep the field set/names exactly.

---

## 3. The classic implementation (annotated — this is what you're porting)

### 3a. The hook — `ClientThink`, top of function (`src/p_client.c`)
```c
void ClientThink (edict_t *ent, usercmd_t *ucmd)
{
    ...
    // capture a real player's raw usercmd trace. Bots are driven through this
    // same ClientThink; exclude them.
    if (bot_inputlog->value != 0 && !Bot_IsClient (ent))
        Bot_LogInput (ent, ucmd);
    ...
```
`ClientThink` is called once per received client frame — this is the *only* place the raw
`usercmd_t` is available, so the hook must live here (a `.dm2` is downstream of it and has already
lost the inputs). It runs **before** movement/prediction so it logs the command as sent.

### 3b. The writer — `Bot_LogInput` (`src/bot_log.c`)
```c
void Bot_LogInput (edict_t *ent, usercmd_t *ucmd)
{
    int   slot;
    float vx, vy, vz, spd;

    if (!log_fp || !ent || !ent->client || !ucmd)
        return;

    slot = (int)(ent - g_edicts) - 1;
    vx = ent->velocity[0]; vy = ent->velocity[1]; vz = ent->velocity[2];
    spd = (float)sqrt (vx*vx + vy*vy);

    fprintf (log_fp,
        "{\"type\":\"input\",\"t\":%.2f,\"slot\":%d,\"msec\":%d,"
        "\"fwd\":%d,\"side\":%d,\"up\":%d,\"atk\":%d,"
        "\"yaw\":%.2f,\"pitch\":%.2f,"
        "\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,"
        "\"vx\":%.1f,\"vy\":%.1f,\"vz\":%.1f,\"spd\":%.1f,"
        "\"onground\":%s,\"water\":%d}\n",
        level.time, slot, (int)ucmd->msec,
        (int)ucmd->forwardmove, (int)ucmd->sidemove, (int)ucmd->upmove,
        (ucmd->buttons & BUTTON_ATTACK) ? 1 : 0,
        SHORT2ANGLE (ucmd->angles[YAW]), SHORT2ANGLE (ucmd->angles[PITCH]),
        ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
        vx, vy, vz, spd,
        ent->groundentity ? "true" : "false",
        (int)ent->waterlevel);
}
```

### 3c. File lifecycle (`src/bot_log.c`)
- **Open** in `Bot_LogBeginLevel(mapname)` — called once per map load (in ozbot from
  `Bot_RunFrame` on map change; on Kex call it from `InitGame`/`SpawnEntities`/level-start):
  - resolve dir `"<game cvar>/logs"`, `mkdir` it (ignore "exists");
  - filename `"<map>_<YYYYmmdd_HHMMSS>.jsonl"` via `localtime`/`strftime`;
  - `fopen(path,"w")`;
  - write the `run` header with `tick_rate = (int)(1.0/FRAMETIME + 0.5)`.
- **Flush** in `Bot_LogMaybeFlush()` — `fflush` when `level.time - log_last_flush >= 1.0`
  (call once per server frame).
- **Close** in `Bot_LogEndLevel()` — `fclose`; called at level end / shutdown.
- `Bot_GameDir()` returns the `game` cvar string (the mod dir), else `"baseq2"`.

### 3d. The cvar & bot gate (`src/bot_main.c`)
```c
bot_inputlog = gi.cvar ("bot_inputlog", "0", 0);   // register once in InitGame
...
qboolean Bot_IsClient (edict_t *ent) { ... }        // true for DLL-driven bots
```

---

## 4. Porting to Kex / re-release (the actual work)

The re-release game DLL is **C++**, natively variable-tick, and changes the API in four ways that
matter here. Everything below assumes Paril's `quake2-rerelease-dll` conventions (verify names
against the initialized submodule — this repo's copy is not checked out).

### 4a. `usercmd_t` is different — THE headline change
Re-release `usercmd_t` (from `q2repro/inc/shared/shared.h`, which mirrors the Kex game API):
```c
typedef struct {
    byte     msec;
    button_t buttons;        // uint8_t bitfield
    vec3_t   angles;         // FLOAT degrees (no ANGLE2SHORT)
    float    forwardmove, sidemove;   // floats; NO upmove
    uint32_t server_frame;
} usercmd_t;
// BUTTON_ATTACK=BIT(0)  BUTTON_USE=BIT(1)  BUTTON_JUMP=BIT(3)  BUTTON_CROUCH=BIT(4)
```
Consequences for the writer:
- `up` (jump): there is **no `upmove`**. Emit jump from the button:
  `int up = (cmd.buttons & BUTTON_JUMP) ? 200 : 0;` (200 keeps parity with the classic magnitude
  the tools expect; the value is arbitrary as long as `>0` means jump). Optionally also record
  crouch: add `"crouch":(cmd.buttons&BUTTON_CROUCH)?1:0`.
- `yaw`/`pitch`: **use `cmd.angles[YAW]` / `cmd.angles[PITCH]` directly** — they're already float
  degrees. Drop `SHORT2ANGLE`.
- `fwd`/`side`: now floats — print with `%.1f` or cast to int; either is fine for the tools,
  but if you want sub-unit fidelity change the format to `%.1f` and the tools still parse.

### 4b. `level.time` is `gtime_t`, not float
The `t` field must stay a JSON number in **seconds**. In the re-release:
```c
// gtime_t is a strong millisecond type
fprintf(fp, "...\"t\":%.3f...", level.time.seconds());   // or (float)level.time.milliseconds()/1000.0f
```
Same for the flush throttle: compare `gtime_t` values, e.g. `if (level.time - log_last_flush >=
gtime_t::from_sec(1))`. Keep `t` monotonic per file.

### 4c. Tick rate
The re-release exposes the frame time — use it for the `run` header:
`tick_rate = (int)roundf(1.0f / gi.frame_time_s)` (or the game's `FRAME_TIME_S` / `game.tickrate`;
confirm the exact symbol). The remaster runs 10/20/30/40 Hz depending on `sv_tick_rate`.

### 4d. The hook site
`ClientThink(gentity_t *ent, usercmd_t *cmd)` still exists in the re-release game and is still the
one place the raw command is available. Add at the top:
```c
extern cvar_t *bot_inputlog;   // or your own cvar name
if (bot_inputlog->integer && ent->client && !ent->svflags & SVF_BOT /* real player */)
    LogInput(ent, *cmd);
```
Notes:
- The re-release marks bots/monsters with `SVF_BOT` / `svflags`; a real player is a `gentity_t`
  with a `client` and **not** `SVF_BOT`. (If your Kex build has no bots at all, the gate is just
  `ent->client`.)
- `cmd` may be passed by reference/pointer — adapt.

### 4e. cvar registration
Register once in `InitGame` (or `SpawnEntities`). The re-release cvar API is `gi.cvar(name,
default, flags)` returning a `cvar_t*`; use `->integer` for the on/off test. Confirm signature.

### 4f. Field access on `gentity_t`
`ent->s.origin`, `ent->velocity`, `ent->groundentity`, `ent->waterlevel`, `ent->client` all exist
in the re-release. `waterlevel` is an **enum** (`water_level_t`) — cast to `int`. `groundentity`
is a `gentity_t*` (null when airborne) — same truthiness.

### 4g. File I/O
Plain C `stdio` works from the C++ DLL. `mkdir`: use `std::filesystem::create_directories` (C++17,
which the re-release uses) instead of `_mkdir`. Resolve the gamedir the same way (the `game` cvar,
or the re-release's `gi.cvar("game", ...)`). Keep the `<gamedir>/logs/<map>_<stamp>.jsonl` layout.

---

## 5. Reference Kex writer (drop-in shape)
```cpp
static FILE *log_fp = nullptr;
static gtime_t log_last_flush;

void LogInput(gentity_t *ent, const usercmd_t &cmd)
{
    if (!log_fp || !ent->client) return;
    int   slot = (int)(ent - g_edicts) - 1;                 // or ent->s.number - 1
    vec3_t v; VectorCopy(ent->velocity, v);
    float spd = sqrtf(v[0]*v[0] + v[1]*v[1]);
    int   up  = (cmd.buttons & BUTTON_JUMP) ? 200 : 0;      // upmove is gone in Kex

    fprintf(log_fp,
        "{\"type\":\"input\",\"t\":%.3f,\"slot\":%d,\"msec\":%d,"
        "\"fwd\":%.1f,\"side\":%.1f,\"up\":%d,\"atk\":%d,"
        "\"yaw\":%.2f,\"pitch\":%.2f,"
        "\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,"
        "\"vx\":%.1f,\"vy\":%.1f,\"vz\":%.1f,\"spd\":%.1f,"
        "\"onground\":%s,\"water\":%d}\n",
        level.time.seconds(), slot, (int)cmd.msec,
        cmd.forwardmove, cmd.sidemove, up,
        (cmd.buttons & BUTTON_ATTACK) ? 1 : 0,
        cmd.angles[YAW], cmd.angles[PITCH],                 // already float degrees
        ent->s.origin[0], ent->s.origin[1], ent->s.origin[2],
        v[0], v[1], v[2], spd,
        ent->groundentity ? "true" : "false",
        (int)ent->waterlevel);
}
```

---

## 6. Validation (same as ozbot)
1. Build the Kex game DLL, set `bot_inputlog 1` (or your cvar), play a bit, `quit`.
2. Confirm `<gamedir>/logs/<map>_*.jsonl` exists, first line is a `run` header with the right
   `tick_rate`, and subsequent lines are `input` records with changing `x,y,z` and non-zero
   `fwd`/`yaw` as you move.
3. Run `py tools/input_view.py <log.jsonl>` — it segments the trace into jumps and prints each
   jump's key-hold timeline / view-yaw sweep / speed curve. If it parses and the jumps line up
   with what you did, the port is faithful. (`up>0` must correspond to your jumps — this is the
   one field that changed semantics, so eyeball it specifically.)

## 7. Gotchas
- **Jump is a button now, not `upmove`.** This is the single most likely thing to get wrong.
  Everything downstream (`make_playbook.py` treats `up>0` as a jump press and OR's it across the
  tick during resampling) depends on `up>0` meaning "jumped this frame."
- **Angles are already degrees** in Kex — don't `SHORT2ANGLE` them (you'd get garbage).
- **`level.time` is a type, not a float** — always go through `.seconds()`; a raw print will be
  milliseconds or a compile error.
- **Client render rate ≠ tick rate.** A human client sends usercmds at its *render* FPS, so you'll
  get multiple `input` records per server tick with the same `t`. That's expected — the baker
  (`make_playbook.py`) resamples to the tick grid and OR's momentary buttons. Log every call; do
  not dedup in the DLL.
- Keep the `run` header first and free of wall-clock fields if you rely on any byte-exact
  same-seed hashing.
```

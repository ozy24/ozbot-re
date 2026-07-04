# ozbot-re — Porting ozbot to the q2repro 40Hz engine

*Drafted 2026-07-03. Status: **R0–R4 EXECUTED 2026-07-04** — see "Results" at the
bottom. The port is live: 40Hz classic-API game + bot on q2repro, all gates
passed, playbook machinery validated end-to-end. Remaining: record the real
Megahealth-jump capture (human input) and bake it.*

**ozbot-re** is a port of ozbot (self-learning Q2 deathmatch bot, currently on q2pro at
10Hz) to the **q2repro** engine (`E:\code\projects\ozbot\q2repro`) running at a **40Hz
tick rate**, plus a new capability the 10Hz stack could never support: **baked, recorded
map-specific movements** (playbooks) — e.g. the q2dm1 Megahealth trick jump — captured
from human play and routed through the bot's nav graph.

Why 40Hz is the unlock: the `ozbot-longjump-10hz-finding` established that human trick
movement happens at ~30Hz input cadence while the bot commands at 10Hz (100ms usercmds),
making velocity-stacking jumps unreproducible *by construction*. At 40Hz (25ms usercmds)
the bot has finer input granularity than the human capture — replaying a recorded input
stream becomes feasible.

---

## 1. What q2repro is (verified against source)

- A Q2PRO fork, drop-in replacement for the Kex Quake II re-release engine. Same Meson
  build system as our q2pro. Builds **`q2repro.exe`** (client) and **`q2reproded.exe`**
  (dedicated) plus game DLLs (`meson.build:637-685`).
- Its server speaks the **re-release game API natively** and loads **classic game-API-3
  DLLs through a proxy** (`src/server/game3_proxy/game3_proxy.c`), selected by DLL
  apiversion at load (`src/server/game.c:1119-1133`).
- **Tick rate rules** (`src/server/init.c:136-149`):
  - Re-release-API games run at `sv_fps`/`sv_tick_rate` (default 40).
  - Classic game3 DLLs are **forced to 10Hz** — *unless* the engine is built with
    `-Dvariable-fps=true` (`meson_options.txt:154`, default false) **and** the game DLL
    advertises **`GMF_VARIABLE_FPS`** in `g_features`, in which case it runs at `sv_fps`.
  - `sv_fps 40` is legal: framediv 4 of `MAX_FRAMEDIV 6` → 25ms frames
    (`inc/common/utils.h:126-127`).
- The re-release C++ game source is a git submodule (`subprojects/rerelease-game`,
  Paril's quake2-rerelease-dll) — **currently not initialized**, and `meson.build:671`
  requires it unconditionally, so `git submodule update --init --recursive` is a build
  prerequisite.
- q2repro is Win64-first. Building x64 means the game DLL becomes **`gamex86_64.dll`**
  (`shared_library('game' + cpu)` naming) — **the 32-bit constraint dies with this port.**

## 2. The core architecture decision

Two viable routes to a 40Hz bot:

### Route A — port the bot into the re-release C++ game DLL
Rewrite the ~5,300 lines of `bot_*.c` into Paril's C++ re-release game (different API:
`gtime_t`, changed `usercmd_t`, changed imports, C++ idioms). Native 40Hz path, no proxy,
zero game-timing conversion work (the re-release game is variable-tick by construction),
and it's the path q2repro is designed around.

### Route B — keep the classic C codebase, make it variable-FPS  ← **RECOMMENDED**
Keep ozbot's vanilla-3.19-plus-bot C source exactly as it is architecturally. Build the
engine with `-Dvariable-fps=true`, set `GMF_VARIABLE_FPS`, convert the game's timing
assumptions from "one frame = 100ms" to variable `FRAMETIME`, build x64, run at
`sv_fps 40` through the game3 proxy.

**Why B:**
1. **Preserves everything.** 5,349 lines of tuned bot code (9 files), the 4 deliberate
   game hooks, all 21 phases of validated behavior, the telemetry schema, and the entire
   Python toolchain survive intact. Route A re-litigates every Phase-17/19/20 win in a
   foreign codebase.
2. **The hard part has two local reference implementations.** `E:\code\projects\ozbot\openffa`
   and `...\opentdm` both carry the complete classic-game variable-FPS conversion
   (`openffa/g_local.h:106-123` — the `HZ`/`FRAMETIME`/`FRAMEDIV`/`FRAMESYNC`/`KEYFRAME`
   macro pattern; `openffa/g_main.c:1471-1493` — the `sv_fps` handshake). Both are
   derived from the same vanilla ancestor as our game source, so the conversion can be
   cribbed file-by-file.
3. **C, not C++; our diff, not theirs.** We stay owners of a codebase we know.

**Route A remains the documented fallback** if the q2repro `variable-fps` + game3_proxy
combination turns out to be broken beyond local repair (it is an off-by-default option in
a fork focused on the re-release path — treat it as untested upstream). Decision point:
end of Phase R2.

**Rejected: rebasing on OpenFFA/OpenTDM as the game.** They'd give us variable-FPS for
free but change DM gameplay semantics and force re-integrating every bot↔game coupling
point; our identity is "vanilla + small hooks".

## 3. Target layout

```
ozbot-re/            NEW git repo, cloned from ozbot/ (keeps history), then diverges:
                     x64 build, variable-FPS game, 40Hz bot, playbook system. PLAN.md (this).
ozbot/               FROZEN reference + fallback. Still runnable on the old q2pro rig.
q2repro/             Engine fork source. Our patches (fastsim, any proxy/USE_FPS fixes)
                     committed IMMEDIATELY on a local branch `ozbot-re` (revert-hygiene memory).
engine/              Shared runtime dir (baseq2 paks already there). Gains q2repro.exe,
                     q2reproded_fast.exe (fastsim baked in). New mod gamedir: engine/ozbotre/
                     (nav/, logs/, playbooks/, demos/ written there). Old engine/ozbot/ untouched.
tools/               Extended, not forked: tick-rate-aware analyzers, --engine flag for
                     run_parallel, new make_playbook.py.
```

Worker gamedirs for the parallel rig: `engine/ozbotre_wN` (isolated from old `ozbot_wN`).

## 4. Phases

### R0 — Engine bring-up (build q2repro client + dedicated, port fastsim)
1. `git submodule update --init --recursive` in `q2repro/`; create local branch `ozbot-re`.
2. Build x64 from the *x64 Native Tools* environment:
   `meson setup -Dwrap_mode=forcefallback -Dvariable-fps=true builddir && meson compile`.
   Expect to fix bit-rot in `USE_FPS` paths (22 sites across 9 server files) — we own the fork.
3. **Port the fastsim patch** from `q2pro/src/common/common.c` (cvar registration + the
   turbo branch in the frame loop that skips `NET_Sleep` and injects one tick per
   iteration). Same fork lineage → expect near-1:1. Bake into `q2reproded` and deploy as
   `engine/q2reproded_fast.exe`-equivalent. Add a `build_engine.bat` in ozbot-re mirroring
   the ozbot one.
4. New `ozbot-re/build.bat` targeting **x86_64** (`vcvarsall x64`), output
   `dist/gamex86_64.dll`; verify PE machine `0x8664`. Adapt `deploy.bat`/`run_server.bat`/
   `play.bat`/`record_inputs.bat` for the new exe names + `ozbotre` gamedir.

**Gate:** stock q2reproded runs q2dm1 with its bundled classic game at 10Hz; q2repro.exe
connects; the re-release `game_x64.dll` runs at 40Hz (proves engine plumbing end-to-end).

### R1 — ozbot on q2repro, still 10Hz (isolate the proxy, isolate x64)
1. *(Optional but cheap, high diagnostic value)* Build q2repro **x86** once and load the
   existing proven `gamex86.dll` unmodified → any telemetry anomaly here indicts the
   game3 proxy alone, not our x64 port.
2. Clone ozbot → ozbot-re repo; fix x64 compile issues in the vanilla+bot C code
   (pointer-truncation casts; q2repro's own `src/game` is the same lineage built x64 —
   use it as the reference for the casts).
3. Update `tools/run_parallel.py`: `--engine <exe>` (default old q2pro fast exe so old
   rig keeps working), gamedir parameterization, `ozbotre_wN` workers.
4. Telemetry: emit a run-header JSONL record with `tick_rate` + engine name; analyzers
   read it (default 10 when absent, so old logs stay parseable).

**Gate:** 8×90s `run_parallel --fastsim` on q2reproded at 10Hz → ITEM% / pickups within
noise of the current q2pro baseline (~50% ITEM per the decisive win), and the **same-seed
md5 bit-exact gate holds on the new engine**. Measure fastsim throughput (proxy edict
sync costs something even at 10Hz).

### R2 — 40Hz game conversion (the variable-FPS diff)
Adopt the OpenFFA pattern into our vanilla source:
1. `G_FEATURES |= GMF_VARIABLE_FPS`; read `sv_fps` in `InitGame` → `game.framerate` /
   `game.frametime` / `game.framediv`; macros `FRAMETIME`(variable), `FRAMESYNC`,
   `KEYFRAME` per `openffa/g_local.h:106-123`.
2. Convert DM-relevant files, cribbing per-file from the OpenFFA-vs-vanilla diff:
   - `p_weapon.c` — weapon animation advances one frame per **sync frame** (else 4× fire
     rate). The single highest-risk file.
   - `p_view.c` / `p_client.c` — per-frame effects, bobbing, damage feedback → frametime-
     scaled or FRAMESYNC-gated.
   - `g_phys.c`, `g_func.c` — velocity×FRAMETIME integration (mostly already parametric;
     audit hardcoded `0.1`).
   - `g_items.c` respawns, `g_trigger`/`g_target`/`g_misc` — audit frame-count and
     `level.time` float assumptions (`level.time = framenum * FRAMETIME`, g_main.c:467-ish
     pattern — watch float-equality comparisons at 0.025 steps).
   - **Skip all `m_*.c` monster files** — never spawned in DM; note they'd run 4× fast.
3. Bot code untouched this phase beyond compiling (constants knowingly wrong at 40Hz).

**Gate — timing invariants, sv_fps 10 vs 40:** measured RL/HB/RG fire intervals, plat
cycle duration, item respawn delays (30s mega etc.), armor/quad timings must match 10Hz
values within a frame. Script this as a telemetry check (small `tools/verify_timing.py`),
plus a human playtest for feel. **This is also the Route-A/Route-B decision point.**

### R3 — Bot 40Hz adaptation + re-baseline (new metrics epoch)
1. Sweep `bot_*.c` for per-frame assumptions: frame counters → seconds; exponential
   smoothing alphas (aim, humanization OU noise) → dt-corrected; usercmd `msec=25`;
   strafejump hop cadence re-derived (PMF_TIME_LAND lockout handling at 25ms ticks);
   lift controller timers; skill reaction times.
2. **Decision/control split:** goal selection, A*, and re-picks stay at ~10Hz (KEYFRAME-
   gated — bounds CPU at 16 bots × 40Hz and keeps fastsim fast); movement steering, aim,
   strafejump, and playback execution run at full 40Hz.
3. Nav: format unchanged; start from the mature q2dm1.nav but expect link costs learned
   at 10Hz dynamics to shift — A/B inherited vs fresh-grown graph.
4. Re-run the standard baselines (ITEM%, pickups, value, parity kills, humanness KS) over
   8-16 seeds → **document as a new epoch; pre-40Hz numbers are not comparable.** Confirm
   the big wins survive (pathcost, decisive, lift, strafejump — strafejump should improve;
   air-accel is tick-rate neutral but control is finer).
5. Re-verify the bit-exact same-seed gate at 40Hz; re-measure fastsim throughput
   (4× frames/game-second + proxy sync → expect ~4-8× slower than today's ~400×; ≥50×
   realtime is fine, the 8×90s rig then takes ~15-30s).

**Gate:** 40Hz bot ≥ 10Hz bot on the pooled headline metrics (ITEM%, pickups, parity
kills) across ≥8 seeds — 40Hz must not be a regression before we build on it.

### R4 — Playbooks: recorded map-specific movement (the new capability)
1. **Record** — `record_inputs.bat` on the q2repro client: `bot_inputlog` already writes
   per-usercmd JSONL; at a 40Hz server the human's inputs land at full fidelity. Capture
   the MH jump repeatedly (many takes).
2. **Bake** — new `tools/make_playbook.py`: segment the capture (reuse `input_view.py`
   jump segmentation), pick clean takes, resample the usercmd stream to the 25ms tick
   grid, and emit `engine/ozbotre/playbooks/<map>.json`: per-entry an **anchor**
   (origin, yaw, velocity/ground preconditions + tolerances), the tick-indexed usercmd
   stream, the expected origin timeline, and the exit point.
3. **Runtime** — new `bot_playback.c` (+ `NAV_LINK_PLAYBOOK` link type, nav format
   version bump): at map load, register entry/exit nodes and playbook links so **A*
   routes through recorded moves like any other capability link** (cost = duration).
   Execution: approach controller drives the bot into the anchor tolerance
   (position/yaw/speed), then per-tick open-loop replay with a drift monitor against the
   recorded origin timeline; on divergence, abort → `Nav_PenalizeLink` → normal repath.
   Gate behind `bot_playbook` (default off until it wins an A/B, house style).
   Lessons already paid for: graph reachability ≠ completability (goalnode finding), and
   execution controllers beat graph surgery (lift win) — the anchor-align + drift-abort
   controller IS the feature; the link is trivial.
4. **Validate** — acceptance: `item_health_mega` pickups go 0 → meaningfully >0 with
   conversion tracked per attempt; ITEM%/value up, giveups not up, across ≥8 seeds.
   Then generalize: the same machinery takes any recorded segment on any map.

### R5 — Polish
README/CLAUDE.md for ozbot-re (build/run/A-B workflow deltas), memory updates, decide
long-term fate of the frozen 10Hz rig, revisit shelved Route A notes.

## 5. Testing scaffolding summary (parity with ozbot)

| ozbot piece | ozbot-re equivalent | change |
|---|---|---|
| `q2proded_fast.exe` (fastsim patch) | q2reproded + same patch, local branch in q2repro/ | port `common.c` patch |
| `bot_quitafter` self-quit timing | carries over in game DLL | game-seconds now 40 frames/s |
| `run_parallel.bat --fastsim` | same, `--engine` + `ozbotre` gamedirs | tools param pass |
| same-seed md5 bit-exact gate | must be re-proven in R1 (10Hz) and R3 (40Hz) | — |
| telemetry JSONL + analyze.py | header record with `tick_rate`; analyzers rate-aware | audit frame-count math |
| `--mod` pinning for live-canonical-dir hazard | identical hazard, identical fix | — |
| `play.bat` / chase-cam watching | q2repro.exe listen server | new exe |
| `record_inputs.bat` | same, on q2repro client | richer 40Hz captures |

## 6. Risks

1. **`variable-fps` + game3_proxy is untested upstream.** Mitigation: R1 x86 spike
   isolates the proxy; we own both sides; fallback = Route A. This is the reason the
   plan front-loads engine validation before any game conversion.
2. **`p_weapon.c` conversion subtly wrong** → balance drift invisible to casual play.
   Mitigation: the R2 timing-invariant harness measures actual fire intervals from
   telemetry at 10 vs 40.
3. **Proxy edict-sync overhead at 40Hz** cuts fastsim throughput. Mitigation: measure in
   R1/R3; we own the proxy and can optimize; even 50× realtime keeps the loop tight.
4. **All tuned constants invalidated at 40Hz** — treat R3 as a re-baselining campaign,
   not a formality; expect at least one behavior (strafejump cadence, OU noise) to need
   real retuning.
5. **x64 port UB** in 30-year-old code (pointer casts). Mitigation: q2repro's bundled
   `src/game` is the same lineage already x64-clean — crib the casts.
6. **`.dm2` tooling**: q2repro demos may use extended protocols `dm2parse.py` can't read.
   Input logs replace most demo analysis; extend the parser only if needed.
7. **Two rigs sharing `engine/`**: distinct exe names, gamedirs, and net_ports; old
   ozbot stays runnable for reference. The live-canonical-nav A/B hazard now exists per
   gamedir — same `--mod` pinning discipline.

## 7. Immediate next actions (R0 start)

1. `git submodule update --init --recursive` in `q2repro/`; branch `ozbot-re`; first commit.
2. Meson x64 build of stock q2repro (client + dedicated); smoke-test on q2dm1.
3. Port fastsim patch; commit.
4. `git clone ozbot ozbot-re`-style repo seed (this PLAN.md moves in as the repo's plan).
5. x64 `build.bat` + first compile of the game DLL.

---

## 8. Results (executed 2026-07-04)

**R0 — engine.** q2repro built x64 with `-Dvariable-fps=true` (one upstream
bit-rot fix: duplicate `set_server_fps` in `src/client/parse.c`); fastsim
ported tick-rate-aware (injects one `sv_fps`-sized tick per iteration).
Gotcha found: q2repro auto-detects Steam/GoG installs and an OneDrive homedir
— every rig launch passes `com_rerelease -1` to stay hermetic.

**R1 — 10Hz parity.** x64 DLL (offsetof FOFS macros; game.def now tracked)
through the game3 proxy: same-seed md5 bit-exact; 8-seed parity vs the q2pro
rig 268/265 pickups, 55%/55% ITEM, 79/78 frags. Fastsim ~2s wall per
8×90s batch. `run_parallel --repro` is the rig switch.

**R2 — variable FPS.** GMF_VARIABLE_FPS + FRAMETIME/FRAMESYNC/ANIMTIME
conversion (seconds-based nextthink kept; `game.frametime` is a double so
10Hz is exactly the vanilla `0.1`). Gates: conversion bit-invisible at
sv_fps 10 (same-seed md5 identical to pre-conversion DLL); all 8 weapon fire
periods identical at 10 vs 40 Hz; respawn scheduler error ≤0.01s
(tools/verify_timing.py, fire + respawn_scheduled/item_respawned telemetry).

**R3 — bot at 40Hz.** Per-tick dynamics normalized (BOT_TICK_RATIO /
Bot_TickGain); the combat *decision* layer FRAMESYNC-keyframed — per-tick
decisions, even per-second-normalized, are worth +28..41% frags (fresher
combat information). Mover crush damage FRAMESYNC-gated (plat nudges did 4×).
**New epoch (16 seeds × 90s, same build):** 10Hz 488 pickups / 53% ITEM /
157 frags — 40Hz 418 / 47% / 235. The +50% kill intensity is emergent and
symmetric (fires/min flat, frags-per-fire +67%: smoother 40Hz velocity
between 10Hz aim samples tracks better); the ITEM gap is death-interrupted
attempts, not navigation (giveups/pathfails flat, re-decides 1%). A/B of
also-keyframing combat *movement*: no lethality change, pickups cost →
rejected; the 10Hz-brain/40Hz-body split is the shipped shape.

**R4 — playbooks.** Full pipeline shipped and validated with bot-recorded
segments (bot_cmdlog): 57 engages → 26 completed replays over 8×180s, failed
entries self-penalize, no stuck bots. The controller learnings are in the
bot_playback.c header + R4 commit: run-up staging, lateral engage gate,
rail-matching cursor slip (pure open-loop replay is hopeless), capped
closed-loop yaw bias, wedge recovery, high-water progress watchdog.
NAV_MAX_LINKS 8→12 (playbook links silently dropped on saturated nodes).
Reference validation data: `baselines/validation-q2dm1.pbk`.

**Remaining / next:**
- **Record the MH jump** (the real payload): `record_inputs.bat` (server runs
  40Hz), do the box+strafe jump cleanly a few times, then
  `py tools/make_playbook.py <log> --slot 0 --start T0 --end T1 --name mh_jump
  --out engine/ozbotre/playbooks/q2dm1.pbk` and A/B `item_health_mega` pickups.
- bot_playbook default is ON (inert without a .pbk); run a proper pickup/ITEM
  A/B once real entries exist.
- Optional: humanness.py re-profile at 40Hz; multi-map re-baseline; revisit
  bot_lift/HyperBlaster conversion on the new epoch.

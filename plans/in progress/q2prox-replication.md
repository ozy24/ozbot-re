# Close the gap to q2pro-x (OpenTDM-X bots)

Status: **step 0 (measurement) done 2026-10-13**; steps 1–3 not started.
Background memory: `ozbot-re-q2prox-study` (first-session numbers whose raw demos
were lost) and `scratchpad-is-volatile`.

## What q2pro-x is

Server-side bots on `91.227.18.67:27950` ("OpenTDM-X bots duel", gamedir
`opentdm-x`, 10Hz server, no download). They are named after real players
(Purri-X, Polosatiy-X, God-X[250], Noob-X[50] ...), each with a skill tag and an
announced style (default / defence / chaotic / arcade). Watching them, they move like
strong humans — chained strafe-jumps everywhere, no visible thrashing — and aim
very precisely. Example: Polosatiy-X beat a human (Spojler) 13+–0, mostly with rail.

How they are built is unknown. Evidence so far:
- **No verbatim replay of public demos.** 2s path windows match the 599 human
  q2duel5 demos *worse* than humans match each other (p50 32u vs 23.6u).
- **Probably not per-player cloning.** Purri-X ranks real Purri 11/18 on map occupancy.
  Humans self-rank top-2 on that test.
- **Skills that transfer across maps look computed:** per-tick optimal strafe and
  tight tracking.
- **Map knowledge may not transfer.** The pre-aim edge seen on q2duel5 vanished on
  ztn2dm3.

**Goal:** replicate the *observable behaviour* (distributions), not their code.
Stay sight-legal: no omniscience is to be added; see `bot_enemymodel` in CLAUDE.md.

## Measurements

All measurements come from `tools/q2prox_profile.py`: one yardstick, 10Hz steps,
BSP line-of-sight and PVS. The `report` subcommand prints these tables.

### q2duel5 (Mortal Coil)

| | pros (60 demos) | ozbot (16 duels × 180s, seed 700) | q2pro-x (session 1, lost; memory) |
|---|---|---|---|
| speed p50/90/99 | 301/392/488 | 300/**332**/422 | 295/378/463 |
| share >330 u/s | 27% | **11%** | 19% |
| strafe gain per airborne arc, p50 | +13 | **+2** | +47 |
| arcs gaining | 59% | 53% | 76% |
| dither share | 0.20% | 0.18% | 0.82% |
| aim err visible p50 / p75 | 16.8° / 38° | 11.2° / **73°** | 6.4° / 23° |
| hidden-in-PVS within 15° | 20% | **6%** | 27% |
| on target at visibility onset (<15°) | 43% | **10%** | 68% (n=19, weak) |
| react time to <10°, p50 | 0.1s | 0.2s | 0.0s |
| yaw rate while visible, p90 | 211°/s | 343°/s | 309°/s |

### ztn2dm3 (The Rage): q2pro-x chunk 1 (769s moving, 134 onsets) vs 60 pro demos

| | pros | q2pro-x |
|---|---|---|
| speed p90 | 410 | 370 |
| share >330 u/s | 31% | 19% |
| arc gain p50 | +21 | +69 (89% gaining in partial checks) |
| aim err visible p50 | 14.1° | 6.3° |
| on target at onset | 50% | 44% |
| hidden within 15° | 29% | 47% |

Ozbot has **not** been profiled on ztn2dm3 yet. It needs a nav: run
`py tools/gen_nav.py ztn2dm3`; the map is already in `engine/baseq2/maps/`.

### What the numbers say

1. **Movement is the clearest gap.**
   - Ozbot almost never builds speed in the air (arc gain p50 +2).
   - `bot_strafejump` already has the right per-tick law, `θ* = acos(cap/speed)`
     in `SJ_FacingYaw` (`src/bot_move.c`).
   - But `SJ_QualifyRunway` only engages on straight, flat, collinear WALK spines
     at least 256u long.
   - `Bot_StrafeThink` aborts on `b->enemy` (`abort_enemy`).
   - q2pro-x is airborne about 40% of the time, through curves and fights.
2. **Thrashing is NOT a gap.** Ozbot dithers at the human rate, thanks to
   `bot_commit 6`. Don't work on it.
3. **Aim: precision is fine; orientation and anticipation are not.**
   - The p75 of 73° says ozbot often doesn't face a visible enemy at all.
   - First suspect: `Combat_FindEnemy` only acquires inside the ±60° `bot_fov`
     cone (`BOT_FOV_HALF`, `src/bot_combat.c`). An enemy beside or behind the bot
     stays unacquired until the bot happens to turn.
     - Measure it before changing anything: the share of visible-target frames with
       `enemy == -1` in the tick log, and their angle.
     - Humans can't see behind either. The pros' p75 is 38°, so the question is
       turn/acquire latency, not wallhacks.
   - Pre-aim (10% vs pros 43%) is the second gap.
   - Sight-legal ingredients already exist: last-known-position fields from
     `bot_pursuit` (`Bot_PursueTry`, LKP on `bot_t`) and the nav graph.

## Work plan (in order)

### Step 1: strafe-jump as the default travel mode

- **Replace runway qualification with per-tick air steering.**
  - Keep the `SJ_FacingYaw` law.
  - Choose the side and offset so the velocity curves toward a lookahead point on
    the committed A* path.
  - Re-jump on the first grounded tick; the PMF_TIME_LAND clear is already there.
- **Safety:** predict the arc before committing a hop.
  - Use the ballistic integration pattern of `Nav_SeedOnePush` / `bot_airhazard`.
  - Refuse arcs that hit a wall or leave the path corridor.
  - Treat lava as a veto. Slime is NOT a veto (CLAUDE.md: the airhazard lessons).
- **Fights:** stay in the hop chain with an enemy (the dodge) once that's safe, as
  a separate bit so it can be A/B'd on its own.
- **Tick rate:** physics and steering run at 40Hz. Scale every constant with
  `BOT_TICK_RATIO` / `Bot_TickGain`.
- **Gate it behind a new cvar value or bitmask.** The off-state must be
  byte-identical (same-seed telemetry md5).
- **Success criteria:**
  - q2duel5 arc gain p50 moves from +2 toward +13…+47, and share >330 from 11%
    toward 19–27%;
  - pickups not down: `tools/ab_cvar.py` solo + dm, pinned navs, 8 q2dm maps;
  - hazard deaths not up on q2dm3/4/6/7 (`mine_regret.py` with `--cvar bot_hazlog 1`);
  - humanness not worse: `tools/humanness.py` vs the ladder MVD cache.

### Step 2: smoothed lines

- Fit a smooth curve through the A* path and steer the air controller along it,
  hull-trace checked.
- Earlier work: `bot_lookahead` gained shipped-solo +4.5% but lost cold-solo −6.6%,
  because off-graph cuts are only safe on dense navs. Any fix must check the curve
  against the map with traces, not trust the graph.

### Step 3: aim orientation, then sight-legal pre-aim

- **3a. Measure the acquisition gap first** (see point 3 above). If it's the FOV
  cone, test the remedies below with the parity harness:
  - turning toward noises or damage (careful: `bot_hearing` was negative because a
    noise has no heading);
  - faster idle scanning;
  - a slightly wider cone.
- **3b. Pre-aim while the enemy is unseen.**
  - Predict the enemy's likely route from their last-known position and velocity
    over the nav graph.
  - Aim at the first point on that route that is visible from the bot's
    position/path.
  - Metric: "on target at onset" and "hidden within 15°".
- **Strength A/B: `tools/ab_parity.py`.**
  - Use **40+ paired arms**, replicate on fresh seeds, and stick to
    q2dm1/2/5/8 (scores go negative elsewhere).
  - Pickups can't see aim changes in symmetric self-play.
  - Never put aim cvars into `optimize_cvars.py`.

## Tools and assets (built 2026-10-13, uncommitted at handover)

| path | what |
|---|---|
| `tools/q2prox_profile.py` | `q2prox` / `pros` / `bot` / `report` modes. Bot mode splits merged logs per server (`bot // 1000`) and PVS-filters targets. See the docstring for metric definitions |
| `tools/q2bsp.py` | BSP v38 reader: `contents`, `los` (4u sampling), `pvs`, `pvs_box`. pvs_box agrees with what the server sent 92.5% of the time; servers also send PHS/sound entities |
| `tools/demo_pov.py` | rewrites a spectator .dm2 into one player's first-person view (from entity origin/angles + vwep) |
| `engine/ozbotre/nav/q2duel5.nav` | gen_nav, frozen, 121 nodes, 78.8% solo COLLECT. Quad unreachable (`baselines/gen_nav_q2duel5.json`) |
| `engine/baseq2/maps/{q2duel5,ztn2dm3}.bsp` | stock files (md5 16d872…bd83 / 5a3b9f…683e) |
| `demos/q2prox/q2prox_20261013_*.dm2` | ~32 min of q2pro-x on ztn2dm3 (chunk 3 cut short by a map vote) |
| `demos/derived/q2prox/*.json` | profile caches: pros q2duel5/ztn2dm3, ozbot q2duel5, q2prox ztn2dm3 chunk 1 |
| `~/Games/q2pro/` | q2pro client (github.com/q2pro/q2pro @601a8df). `record_session.sh <chunks> <min>` spectates and records into `demos/q2prox/`; `render_demo.sh <demo> <mm:ss> <secs> <out.mp4>` renders offscreen (Xvfb) |

Reproduce the ozbot baseline:

    py tools/run_parallel.py --repro --fastsim --instances 16 --seconds 180 --bots 2 \
        --seed 700 --map q2duel5 --cvar sv_fps 40 --no-analyze
    py tools/q2prox_profile.py bot engine/ozbotre/logs/parallel_q2duel5_<stamp>.jsonl --json out.json
    py tools/q2prox_profile.py report demos/derived/q2prox/pros_q2duel5_60.json out.json

## Rules and gotchas

- **q2duel5 and ztn2dm3 are not in the pinned benchmark navs.**
  - Before any A/B on them, copy the navs into a pinned/scratch gamedir; never A/B
    on the live gamedir (CLAUDE.md).
  - The headline gates stay the standard 8-map benchmark / `ab_cvar` on pinned navs.
- **The q2pro-x side is thin.** Session-1 q2duel5 numbers survive only in memory, and
  the q2duel5 pre-aim number has n=19 against a human.
  - Do not tune toward a single q2pro-x number.
  - Treat it as a direction: "more airborne speed, more anticipation".
- **Recording more needs the user's go-ahead.** Connecting to the external server
  auto-seats you and holds their matches in warmup.
  - `record_session.sh` re-spectates automatically.
  - It does **not** restart recording after a map change. Fix that first.
  - The most valuable missing data: q2pro-x vs humans on q2duel5, and bot-vs-bot on
    q2duel5, so opponent type matches across maps.
- **Keep captures and code out of the scratchpad.** `/tmp` is tmpfs and a reboot
  already destroyed one session.
- **Off-state byte-identical, default OFF until measured, and commit the tools
  before building on them.**

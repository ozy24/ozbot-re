# ozbot — a self-learning Quake II deathmatch bot, built entirely by AI

**ozbot** is a deathmatch bot for Quake II (primarily q2dm1, "The Edge") that lives entirely
inside the game DLL (`gamex86.dll`). It was designed, written, measured, and tuned **entirely by
an AI agent** (Anthropic's Claude) working in an autonomous build → simulate → analyze → improve
loop. No human wrote bot code, placed a waypoint, or drew a route.

Two things make it different from classic Quake II bots:

1. **Nothing is hand-authored.** There is no precomputed navmesh, no shipped waypoint file, no
   per-map item table, and no scripted routes. The bot **learns its navigation graph by
   playing**: nodes are dropped where bots actually stand, links are recorded only between nodes
   a bot actually traversed (so every edge is traversable by construction), failing links are
   penalized and pruned, and the graph is persisted per map and matures across runs. Items are
   discovered by scanning live entities, never from hardcoded coordinates — the same DLL works
   on any map with zero configuration.
2. **Every behavior change is empirically validated.** There is no unit-test suite; the bot is
   its own experiment. Each candidate change ships behind a cvar and must win a seeded,
   reproducible A/B across many parallel headless servers before it becomes a default. Changes
   that lose are reverted and the negative result is documented. Most changes lose — that
   history is below, because the failures shaped the design as much as the wins.

## How it works

The bot uses the ACEBot integration approach — a bot is an ordinary client-slot edict spawned
through the real `ClientConnect` → `ClientBegin` path, then driven each frame by synthesizing a
`usercmd_t` and calling `ClientThink`. No engine changes; the vanilla Quake II v3.19 game code is
hooked in only four small places. All bot logic is in `src/bot_*.c` + `bot.h` / `bot_nav.h`.

Per-frame pipeline (movement is deliberately **decoupled from aim**, so a bot can run for an item
while shooting at an enemy elsewhere):

1. **Navigate** (`bot_main.c`, `bot_move.c`, `bot_nav.c`) — learn the graph, run the goal state
   machine (explore ↔ goal), follow A* paths, and set a world-space movement intent.
2. **Combat** (`bot_combat.c`) — pick the nearest visible enemy, track toward it with a
   skill-scaled turn rate, reaction delay, and aim error; select the best owned weapon; fire.
3. **Apply** — project the movement intent onto the chosen facing to produce the final usercmd.

Goal selection (`bot_goal.c`) scores each discovered item by `value × need / route-cost`, where
route cost is the **actual A\* path cost** (jump/fall/water links cost more), skips items another
bot has already claimed, verifies reachability before committing, and gives each goal a time
budget proportional to its route cost. When wedged, a short-horizon planner (`Bot_RolloutRecover`)
simulates candidate input sequences through the real movement code (`gi.Pmove`) and commits to
whichever makes real progress.

## The measurement loop

Throughput comes from two levers: a patched dedicated engine (`q2proded_fast.exe`, built from the
`q2pro/` source by `build_engine.bat`) whose `fastsim` cvar removes the per-tick sleep so the sim
runs CPU-bound at **hundreds of × real time**, and parallelism on top. The standard rig is
**8 headless servers × 90 *game*-seconds × 5 bots** (`run_parallel.bat --fastsim`, a couple of
wall-seconds end-to-end), each worker on an isolated gamedir with a fixed RNG seed (`bot_seed`),
merged into one telemetry set. Fastsim is bit-exact: with the same seed it reproduces the
real-time engine's telemetry byte-for-byte, just faster. Per-tick JSONL telemetry feeds an analyzer that
reports per-bot movement/goals/pickups/frags, failure clustering, and a coverage/failure heatmap.

- The headline navigation metric is **ITEM completion**: pickups ÷ item-goal attempts.
- Every A/B holds the seeds fixed and flips exactly one cvar; results are pooled across 5–7
  seeds because single-seed reads mislead.
- Combat changes can't be judged by symmetric self-play (total frags just measure activity), so
  they use an **id-parity head-to-head**: bots split into treatment/control by bot-id parity
  *within the same match*, and the two populations' kills are compared directly.

## Results over time

ITEM completion on q2dm1, same measurement rig throughout:

| Milestone | ITEM completion | What changed |
|---|---|---|
| Item goals first working (Phase 2) | ~20% | value/need/distance scoring over discovered items |
| Locomotion tuning plateau (Phase 6) | 18–22% | many locomotion fixes tried; most failed A/B |
| Goal contention fix (`bot_claim`) | ~21% | stop bots piling onto the same item (+12% pickups) |
| Physics rollout recovery (`bot_rollout`) | ~23% | forward-simulated unstick (+12% pickups) |
| Route-cost scoring (`bot_pathcost`) | ~29% | score items by A* route cost, not straight-line distance (+52% pickups, 5/5 seeds) |
| Route-cost time budgets (`bot_goalbudget`) | **~33%** | goal timeout scaled to route cost; ITEM% up in 7/7 seeds |
| Completability economics (`bot_itemfail` + `bot_budgetcap`) | ~33% (**pickups +14%**) | items bots keep failing get an escalating shared blacklist, and the budget cap is trimmed to what successful runs actually use (pickups p95 ≈ 11s); value-weighted pickups +10%, map-general on q2dm3 |

Other validated improvements along the way: goal-node reach rate 38% → 59% (Phase 1 tuning);
combat unfroze — %time-in-combat dropped from a pathological 87–99% (bots stuck staring at each
other on Blasters) to a healthy 0–50% with diversified weapon usage once movement was decoupled
from aim; hazard avoidance cut q2dm3 deaths 30 → 4; map generality validated by letting the bot
learn q2dm3 from scratch. The skill model is confirmed real: skill 0.9 bots get **45% more
kills** than skill 0.1 bots fighting in the same matches (6-seed id-parity test). **Projectile
target-leading** (`bot_lead`) moved the leading population's kill ratio from 0.81 to 1.27
(~57% relative gain, 6/6 seeds, paired id-parity test) — the largest combat improvement so far.
**Fight-or-flight** (`bot_flee`: retreat while firing when clearly outmatched) added another
+23% relative kills with no nav cost — retreating breaks losing fights, so bots keep their
weapons and re-engage on their own terms.

Cross-map picture (self-learned nav, standard rig, 3 seeds/map): the ITEM-completion ceiling
tracks map *verticality*, not item logic — vertical maps q2dm1/q2dm2 sit at ~33%/~26% while
flatter q2dm5/q2dm8 reach **~62%/~55%**; the Railgun that never completes on q2dm1 completes at
67% on q2dm8. An aim-formula constant sweep (16-seed id-parity per axis) found the hand-tuned
skill model near a local optimum — halving reaction or error buys only ~8% kills, faster turn
rate nothing — so combat gains come from behaviors (leading, fleeing), not precision tuning.

## What was tried and rejected (and why it matters)

Documented negative results, each from a controlled A/B — these are load-bearing project
knowledge, not failures to hide:

| Experiment | Result | Lesson |
|---|---|---|
| Import navigation from ~1300 pro demos | bot got **worse** | pro routes assume pro movement (strafe-jumps, momentum) the bot can't execute |
| Calibrate weapon priority from pro demo kill-efficiency | dead tie (171 vs 170 frags) | pro data bakes in pro *execution* skill; it doesn't transfer to a bot without that ceiling |
| More nav-graph maturation (more bot-hours) | ITEM% regressed 23% → 15% | past coverage, extra nodes add routing noise, not capability |
| Progress watchdog (abandon stalled goals early) | ITEM% crashed to 14% | faster recycling floods attempts when re-picks are equally unreachable |
| Ledge-jump primitive | flat, helped some seeds, hurt others | the vertical failures were 100–140u platforms, not single ledges |
| Lift/vertical-arrival fix (4 architecturally distinct attempts) | lost or tied every time, even though pathing verifiably worked | finding routes ≠ completing them in-budget; the constraint was route *economics* |
| Learned per-link traversal times | wash live; consistent loss when transplanted onto the canonical graph | measured costs only inflate (floor + frame quantization); static distance×type costs were already sufficient |
| Soft-penalize claimed items instead of hard-skip | pooled loss, `item_lost` rose | contested items are contested for a reason |
| Flee-and-fetch-health (abandon current goal when fleeing) | won combat but cost ~7 ITEM% points | the survival value is in the retreat movement, not the health fetch; goal churn is expensive |
| Directed rocket dodging (2 variants, 16-seed A/Bs) | wash / slight loss | constant strafing already captures the dodge value; a directed override just disrupts it |

The through-line: six locomotion-layer fixes failed before the real levers turned out to be
**goal-selection contention** (bots with no mutual awareness converging on the same item) and
**route economics** (an item behind a lift genuinely costs more seconds than an easier one, and
both the scoring and the time budget must reflect that). Diagnosing *which layer* is the
bottleneck was worth more than any individual mechanism.

## Building and running

The engine is 32-bit, so the DLL **must be built x86** (MSVC via `vcvarsall.bat x86`; VS2022).

```bat
build.bat          :: compile src/*.c -> dist/gamex86.dll (x86)
deploy.bat         :: copy dist/gamex86.dll -> %Q2DIR%/ozbot/
run_server.bat     :: build + deploy + launch a dedicated server with bots
play.bat           :: launch a listen server you can play IN against the bots
run_parallel.bat   :: build + deploy + N parallel headless sims + merged analysis
build_engine.bat   :: build ../q2pro (x86, meson) -> %Q2DIR%/q2proded_fast.exe (fastsim engine)
```

`%Q2DIR%` points at a Quake II install (a q2pro engine dir with `baseq2` paks). The vanilla game
source headers are expected at `../quake2-source` (id's GPL Quake II v3.19 release); the analysis
tooling (`analyze.py`, `run_parallel.py`, demo parsers — pure Python stdlib) lives in `../tools`.

Typical A/B run:

```bat
run_parallel.bat --fastsim --instances 8 --seconds 90 --bots 5 --seed 200 --cvar bot_pathcost 0
```

With `--fastsim`, `--seconds` counts *simulated game seconds* (each server quits itself via
`bot_quitafter`), so every seed simulates exactly the same game time; without it the servers run
at real time and `--seconds` is wall-clock.

### Runtime knobs

| Cvar | Default | Meaning |
|---|---|---|
| `bot_count` | 0 | target bot population (auto-maintained) |
| `bot_skill` | 0.6 | 0..1, scales aim reaction/turn rate/error |
| `bot_pathcost` | 1 | score items by A* route cost, not straight-line distance |
| `bot_goalbudget` | 1 | goal timeout scaled to route cost, not flat 12s |
| `bot_budgetcap` | 15 | max seconds to fund any one goal route (pickups p95 ≈ 11s) |
| `bot_itemfail` | 1 | escalating shared blacklist (20/40/80/160s) for items bots keep giving up on |
| `bot_claim` | 1 | skip items another bot is already going for |
| `bot_rollout` | 1 | physics-forward rollout recovery when stuck |
| `bot_lead` | 1 | lead moving targets by projectile flight time (skill-scaled) |
| `bot_flee` | 1 | retreat (while firing) when clearly outmatched |
| `bot_seed` | 0 | >0 = deterministic RNG for reproducible runs |
| `bot_quitafter` | 0 | >0 = quit the server after N *game* seconds (timed fastsim runs) |
| `bot_debug` | 0 | draw nav paths / enemy lines via temp-entity beams |
| `bot_skilltest` | 0 | diagnostic: id-parity skill split (0.9 vs 0.1) within one match |
| `bot_leadtest` | 0 | diagnostic: id-parity lead split (even ids lead, odd don't) |
| `bot_fleetest` | 0 | diagnostic: id-parity flee split (even ids flee, odd don't) |
| `bot_aimtest` | 0 | diagnostic: even ids apply the `bot_aimreact/aimturn/aimerr/aimfire` multipliers (each default 1) for aim-formula sweeps |

Server console: `sv bot_add N` / `sv bot_remove N` / `sv bot_clear`. The learned graph is saved
to `<gamedir>/nav/<map>.nav` (autosaved ~30s); telemetry to `<gamedir>/logs/<map>_<ts>.jsonl`.
To bootstrap a brand-new map, just run bots on it — the graph self-builds.

## License

Based on the id Software Quake II v3.19 game source, licensed under the GNU General Public
License v2. The bot code (`src/bot_*`) is released under the same license.

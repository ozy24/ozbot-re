# CLAUDE.md — ozbot-re

**ozbot-re** is the q2repro (40Hz) port of ozbot. This is a fully self-contained
folder: its own `tools/` (versioned), plus the in-repo unversioned (gitignored)
infra — `engine/` runtime, its own `demos/` corpus copy, the `q2repro/` engine
source, and `quake2-source/` reference. The 10Hz/x86 original is a separate
self-contained folder at `../ozbot` (own CLAUDE.md). Everything below OVERRIDES
the legacy ozbot specifics when working in this repo.

## What changed vs ozbot

- **Engine:** q2repro (`q2repro`, local branch `ozbot-re` carrying the
  fastsim patch + variable-fps fixes), built x64 by `build_engine.bat` →
  `engine/q2repro.exe` (client) + `engine/q2reproded.exe` (dedicated, fastsim
  cvar built in). **The 32-bit constraint is dead here**: `build.bat` produces
  `dist/gamex86_64.dll` (MSVC x64; verify PE `0x8664` if in doubt).
- **Gamedir:** `engine/ozbotre/` (nav/, logs/, playbooks/). The old
  `engine/ozbot/` q2pro rig still works and is the frozen reference.
- **Hermeticity:** q2repro auto-detects Steam/GoG installs and an OneDrive
  homedir. EVERY launch must pass `+set com_rerelease -1` (all scripts and
  `run_parallel --repro` do).
- **Tick rate:** the game exports GMF_VARIABLE_FPS and reads `sv_fps` at
  InitGame. `+set sv_fps 40` is the ozbot-re standard; without it (or on an
  engine without variable-fps) everything runs the vanilla 10Hz and is
  **bit-identical** to the pre-port DLL (that was the port's regression gate).
  Never flip sv_fps mid-session — it is read once per InitGame.
- **10Hz brain, 40Hz body:** vanilla game logic authored per-frame
  (weapons, animations, damage cadences) and the bot's combat DECISION layer
  run on FRAMESYNC keyframes (every FRAMEDIV-th frame = 10Hz); physics,
  steering, and movement execution run at the full tick rate. When adding
  per-tick bot logic, scale constants with BOT_TICK_RATIO / Bot_TickGain
  (see bot.h) or keyframe it with FRAMESYNC — never leave a raw per-tick
  constant, it will be 4x off at 40Hz.

## Sim / measurement rig

```
py tools/run_parallel.py --repro --fastsim --instances 16 --seconds 90 --bots 5 \
      --seed 700 --cvar sv_fps 40
```
`--repro` = q2reproded.exe + gamex86_64.dll + ozbotre gamedir + com_rerelease -1.
Same rules as ozbot: pass `--seed` for A/Bs, prefer 16 seeds, the same-seed
telemetry-md5 gate is the bit-exact check, and pin `--mod` with a scratch
gamedir if the canonical one may be live. Pinned baselines (nav snapshot,
validation playbook) live in `baselines/`.

**Metrics epoch:** 40Hz numbers are NOT comparable to ozbot-era (10Hz) history.
40Hz baseline (16 seeds x 90s): ~418 pickups / 47% ITEM / 235 frags vs the
same build at 10Hz: 488 / 53% / 157. The higher kill intensity is an emergent,
verified-symmetric property of the finer simulation (fire rates and weapon
timing are provably unchanged — `py tools/verify_timing.py <log10> <log40>`).

**Cross-map stats tracker (`tools/benchmark.py` → `STATS.md`).** Tracks item
collection / pickups / frags / K/D / nav nodes across all 8 q2dm maps and how
they move as the code changes. It runs the standard repro rig (forces
`sv_fps 40`) + pinned playbooks (`baselines/playbooks/`) at a fixed seed on every
map across a **2×2** of nav baseline × rig, so two snapshots differ only because
the code changed. Nav baseline:
- **shipped** — the real bot on its hand-seeded navs (`baselines/nav_shipped/`, a
  frozen snapshot of the curated `engine/ozbotre/nav/`; cold-filled for q2dm4/
  q2dm6 which never had a curated nav). Where the tuned bot actually stands.
- **cold** — the same build self-learning from a scratch-matured graph
  (`baselines/nav/`, `--mature`). Isolates nav-learning quality.

Each baseline is run under two **rigs**:
- **solo** (1 bot, combat impossible) — the nav-quality **headline**: isolates
  last-leg item-collection with no combat interruption / respawn-teleport /
  contention.
- **deathmatch** (5 bots) — the integration metric (combat + contention, where
  wins like `bot_claim` show up). Collection reads lower here because combat
  interrupts routes.

So four columns — **shipped-solo / cold-solo** (nav quality) and **shipped-dm /
cold-dm** (integration); `STATS.md` leads with the solo pair.

The shipped−cold gap is what the manual nav curation is worth — and it's
**map-specific**: big on q2dm1 (deathmatch 54% vs 39%) and q2dm7, but on q2dm2/3/5
the uniform cold nav matches or beats the hand-seeded one (a signal those live navs
may be stale vs current-DLL maturation). Each run appends a *variant-nested* snapshot
(commit + `--note`) to `baselines/benchmark_history.jsonl` and regenerates
`STATS.md` (solo table/trend on top, deathmatch as the integration section). Freezes
DLL+navs+playbooks into `engine/ozbotre_bench`, so a live `play.bat` can't perturb a run.
`py tools/benchmark.py --note "<what changed>"`; `--report-only` re-renders;
`--pin-shipped` refreshes the shipped baseline from the live curated navs;
`--mature` regrows the cold baseline (11 bots × 720s, `sv_fps 40`) into
`baselines/nav/` **only** (live `engine/ozbotre/nav/` untouched). Playbooks are
present during maturation, so q2dm1's MH-jump Megahealth is rediscovered cold.
Note 40Hz ITEM% sits structurally below 10Hz (death-interrupted attempts). See
`STATS.md`.

## Nav shipping & generation

**Shipping strategy.** Runtime nav learning is always-on and autosaves, so the
plan is: ship pre-baked **FROZEN** navs for the map rotation you care about and
let the always-on learner auto-fill the long tail (unbaked maps self-build +
autosave on first server run). Navs are loose files under
`<gamedir>/nav/<map>.nav` (raw fopen, not the pak/VFS), so shipping = bundle
those files (+ optional `<gamedir>/playbooks/<map>.pbk`) next to the DLL — sizes
are tiny (tens of KB/map). The good node count is **map-specific** — NOT bounded
by a global cap or a map-size formula — and freezing a baked nav stops a
long-running live server from over-growing it past its tuned sweet spot.

**Three features:**
- **`bot_navlearn`** (default 1) — set 0 to disable all learning + autosave (play
  a fixed graph). The ONAV nav-file format is now **v2**: a `uint32` header flags
  word follows the node count; v1 files still load (treated as learnable). A graph
  with the `NAVHDR_FROZEN` header bit is played as-is — never grown, never re-saved
  — regardless of `bot_navlearn`. `py tools/nav_edit.py freeze <file.nav>`
  (`--unfreeze`) sets it; `dump` shows `[FROZEN]`.
- **`bot_teleport`** (default 1) — at map load seeds one-way `NAV_LINK_TELEPORT`
  links from each teleporter pad to its destination: both the point-entity
  `misc_teleporter`→`misc_teleporter_dest` (id/base form) and the **brush
  `trigger_teleport`** most custom DM maps use (source = the brush volume's floor
  center). The teleport fires automatically on touch (no ride controller) — this
  is purely the routing edge the learner can't capture, since the pad jumps the
  bot >200u and the learn-guard rejects it. Regenerated each load, never saved
  (like TRAIN/PLAYBOOK). NOTE: this base previously had **no `trigger_teleport`
  spawn function**, so those teleporters were inert (broken for everyone) —
  `SP_trigger_teleport` (g_misc.c, reusing `teleporter_touch`) now makes them work.
  Verified net-POSITIVE on absolute pickups (a fixed graph collected 78 vs 56
  items with routing on), though COLLECT% often *drops* because more of the map
  becomes reachable (harder attempts). Stock q2dm1-8 have no teleporters.
- **`bot_jumppad`** (default 1) — at map load seeds one-way `NAV_LINK_PUSH` links
  from each `trigger_push` jump pad to its BALLISTIC landing spot. Unlike a
  teleporter the pad has no destination entity, so `Nav_SeedPushLinks`
  (`bot_nav.c`) predicts the landing: launch velocity = the exact touch physics
  (`movedir * speed * 10`), integrated as a parabola under `sv_gravity` with
  per-step player-box traces until it lands on a walkable floor; source + landing
  snap to nav nodes. Regenerated each load, never saved (like TELEPORT). The push
  fires automatically on touch (no ride controller) — this is purely the routing
  edge the >200u learn-guard rejects. Adding link type 8 required widening
  `NAV_MASK_ALL` 0xff→**0x1ff** (else A* silently drops every push link). Stock
  q2dm1-8 have no jump pads (0 links, byte-identical); test maps: `custom_maps/
  mm-aerow` (4 pads + 4 brush teleporters), mm-reclam, mm-recycler. NOTE: seeding
  + regression are validated, but the behavioral pad-traversal pickup A/B is still
  pending (needs a curated nav on a pad-gated-item map).
- **`gen_nav.exe`** — standalone (no-Python) map-agnostic nav generator, built
  from `tools/gen_nav.py` by `build_gen_nav.bat` → `dist/gen_nav.exe` (needs
  `py -m pip install pyinstaller`). End users/admins drive the mod's
  `q2reproded.exe` from it without a Python install. It **matures-to-peak**: grows
  the graph in cold-maturation checkpoints of increasing duration, probes solo
  collection at each with the graph frozen, and emits the checkpoint with the most
  **absolute pickups** stamped FROZEN — baking in the node-count sweet spot per map
  (past it, routing noise lowers throughput). Fitness is pickups, **NOT** COLLECT%
  (pickups/attempts): COLLECT% rewards LOW coverage — a sparse graph reaching 3 easy
  items at 100% beats a rich one reaching 20 at 85%. Validated on 10 custom `mm-*`
  maps (mean ~84% COLLECT, all frozen). Auto-detects the engine + DLL (from `dist/`
  or the gamedir). Writes a JSON report with the peak (pickups, nodes, COLLECT%),
  the checkpoint curve, and
  a **gated-item list** (reach-oracle items still `no_path`/gated — the human's
  to-do list for playbooks or surgical `nav_edit.py` links). CLI:
  `gen_nav.exe <map> [--out P] [--report J] [--maps a,b,c] [--engine DIR]
  [--dll P] [--interval S] [--max-seconds S] [--patience N] [--probe-instances N]
  [--seed N]`. Exit codes: 0 ok, 2 engine/DLL missing, 3 no nodes learned.

## Resource-need calibration (demo-mined)

The goal scorer's `bot_ammoneed` (per-ammo-type low-fill urgency, via
`Combat_AmmoFracForItem` in `bot_combat.c`) and `bot_wpnneed` (unowned-weapon
value by pro kill-rank, `Weapon_KillRankWeight` in `bot_goal.c`) are
**default-ON**. Their thresholds are mined from the pro corpus by
`tools/dm2_combat.py need` → `demos/derived/combat_need/thresholds.json`
(5859 demos) and **baked in as constants** — regenerate in-repo against this
folder's own `demos/` copy, then update the constants here. There
is no runtime dependency on the JSON. `bot_healthneed` exists but is
**default-OFF** (health-seeking is asymmetric-negative, same as `bot_survive`).
See the `ozbot-re-resource-need-win` memory for the A/B results. The same
mine → bake → no-runtime-JSON pattern produced the sight-loss pursuit constants
(`tools/dm2_combat.py pursue`; see "Perception" below).

`bot_commit` (default **6**, ON — retuned from 0.8, see below) is the goal
scorer's travel-cost commitment
discount: a re-rank pass in `Goal_Select` (`bot_goal.c`) after the `bot_pathcost`
A*-cost pick, charging each reachable candidate the **marginal detour past the
cheapest reachable option** (`cost += bot_commit * max(0, pcost - mincost)`), so a
far/hard item only wins when nothing cheaper is nearby (bot idle) or it is itself
near-cheapest. Attacks "reachable != completable" — it steers **selection only**
(`b->goal_cost` still funds a committed route at raw A* cost). It's a
*contention* lever (like `bot_claim`): ~flat on shipped-solo (rarely 2+ candidates
to arbitrate), **+7-12% pickups on cold-solo / shipped-dm / cold-dm**. `bot_commit
0` = pre-lever pick, byte-identical. Keeper telemetry: the `goal_commit` event.
See the `ozbot-re-commit-win` memory.

**Retuned 0.8 → 6** by the joint search (`tools/optimize_cvars.py`); 0.8 was a
coordinate-wise pick made with everything else held fixed and was badly
under-set. Averaged over 8 maps × {solo,dm} × 2 seed banks, vs 0.8:

| bot_commit | pickups | value collected | value per pickup |
|---|---|---|---|
| 3 | +7.8% | +4.7% | 28.1 |
| **6** | **+11.4%** | **+5.6%** | **27.4** |
| 8 | +12.7% | +6.3% | 27.2 |
| 64 | +16.4% | +7.4% | 26.6 |

There is **no interior optimum** — the metric rises monotonically toward the
degenerate limit (`commit → ∞` = strictly-cheapest-first, value and need
ignored). The two columns disagree on purpose: raw pickups rise 16% while
*value* rises only ~7%, so average pickup quality falls the whole way. 6 takes
the bulk of the real gain while staying clear of the limit where
`Item_BaseValue` and the need model would become decorative. **Checked, not
assumed:** `bot_wpnneed`/`bot_ammoneed` are worth *more* at high commit
(+4450 value at 8 vs +2536 at 0.8), so the retune does not drown the need model.
Benchmark 2×2 at 6: shipped-solo **+16.1%**, cold-solo +6.6%, shipped-dm +8.4%,
cold-dm +8.7% (6/8 maps up; q2dm6 −7% on all four — the lava map).

## Hazard: the airborne residual (`bot_airhazard`, default OFF)

`bot_hazard` is default-ON and still leaves environmental deaths as the MAJORITY
on q2dm3/4/6/7 (58-71%) — because `Bot_HazardInDir` probes the **ground** ≤128u
ahead and its brake is gated on `groundentity`, while **80-94% of those deaths are
AIRBORNE**. `bot_airhazard` (bitmask: 1 = refuse to commit to an arc landing in
lava, 2 = mid-air steer, 4 = treat slime as fatal too) integrates the ballistic
arc instead, mirroring `Nav_SeedOnePush`.

**It works and it does not pay.** Lava deaths −30.6%, pickups **flat (−0.3%)**;
q2dm3 +6.6% / q2dm6 +1.8% / q2dm7 0.0% / **q2dm4 −7.5%**. The lesson:
*dying in lava costs about the same as refusing to go near it* — respawn is fast
and lands you near items, while braking at a ledge burns goal budget. Default
**0**; opt-in `1` for a lava-heavy rotation without q2dm4's route-through-lava
geometry. Two sub-results not to re-derive: **slime must not be vetoed** (it is a
survivable A*-priced wade — vetoing it cost q2dm7 pickups for nothing), and
**mid-air salvage (bit 2) is a hard reject** (30519 steers bought −5.2% deaths;
Quake air-control cannot move a committed arc). Telemetry: `bot_airhazlog`.

## Diagnosis + search tooling

Two tools that answer questions the headline metrics can't.

**`tools/mine_regret.py` — counterfactual regret taxonomy.** Runs over ordinary
run telemetry (no DLL change, works on historical logs) and ranks the moments
where a *counterfactual was obviously better*, per map, with replayable
timestamps. Classes: `ran_past_fight`, `passed_item`, `route_backtrack`,
`death_near_health`, `goal_churn`. Pickups/ITEM%/frags cannot see "dumb"; this
is how hazard/decisive/outnumbered-style work gets its next target.

    py tools/mine_regret.py engine/ozbotre/logs/parallel_q2dm1_*.jsonl --top 6
    py tools/mine_regret.py <logs...> --json baselines/regret_baseline.json

Run it with `--cvar bot_hazlog 1` (log-only, off-state byte-identical) or the
death classes stay unclassified — and the **death-cause readout is the highest-
value thing it produces**. Two definition lessons are baked into the tool and
worth not re-learning: a heading-reversal test for ping-pong is ~all false
positives (q2dm1 corridors are full of legitimate hairpins that reverse heading
while making real path progress — hence the revisit-based `route_backtrack`), and
`death_near_health` is meaningless without MOD classification because most deaths
on q2dm3/4/6/7 are lava/slime. Blind spot to remember: tick telemetry carries no
ammo counts, so ammo need is unscorable (those passes are counted separately).

**`tools/optimize_cvars.py` — joint CEM search over the tunable cvar vector.**
Every constant here was tuned coordinate-wise; this searches them jointly, using
the fastsim rig as the black-box fitness function it already is.

    py tools/optimize_cvars.py --iters 10 --pop 20      # ~70 min, 200 evals
    py tools/optimize_cvars.py --eval-only "bot_commit=6" --train-maps q2dm1,q2dm8
    py tools/optimize_cvars.py --report-only baselines/optimize_trace.jsonl

Rules the tool encodes, all learned the hard way:
- **Overfit guard is the point**: holdout is disjoint maps AND disjoint seeds
  (train q2dm1/q2dm8 @700, holdout q2dm2/q2dm5 @900). A training win that does
  not transfer is the expected outcome, not a surprise.
- **Aim/accuracy cvars are excluded from the space.** In a symmetric self-play
  sim, degrading everyone's aim raises pickups by cutting the death rate — the
  optimizer will happily exploit that while making no bot better at anything.
- `detail` carries `pickups_by_item`, because raw pickups score a +2 Armor Shard
  like a Rocket Launcher. **Always check composition before believing a win.**
- **A boundary optimum is not an optimum, it is an under-sized range.** Pass 1
  pinned `bot_commit` at its ceiling twice before the range was widened enough to
  show the curve had no interior peak at all.
- **Do not read CEM marginals as attribution.** The search "converged" on
  `bot_navvalidate`/`bot_slimeescape` (Bernoulli p→0.9) that a single-knob
  ablation showed were worth ~0. Ablate the winner knob-by-knob; in the pass that
  produced the retune, *all* of the +6.8% holdout win was `bot_commit` alone.

## Perception: sight-loss pursuit (demo-mined)

`bot_pursuit` (default **0**, OFF — behavior built and bounded, benefit unproven)
gives the bot a memory of where an enemy was.
While a target is visible the 10Hz combat keyframe stamps its origin/velocity/time
(the "last-known position", LKP fields on `bot_t`); on sight loss the bot may
commit a **bounded** chase to a short velocity-extrapolated point near that spot,
routed through the ordinary GOAL machinery (`Bot_PursueTry` in `bot_main.c`).
Re-acquisition is then free via `Combat_FindEnemy`; arrival/timeout hands the
frame back to the item economy.

The bounds are the feature — an unbounded chase is just "reachable !=
completable" in a new costume. A chase requires: route affordable
(`bot_pursuitcost`, A* g-cost), `Combat_Strength >= 70`, not fleeing, not
blaster-only, no closer available item, and a hard wall clock. **One sight loss
buys exactly one chase.** A chase owns no item, so it never blacklists and never
enters the item-giveup ladder; its clock freezes alongside the goal budget
whenever a traversal controller (lift/train/playbook/ladder) owns the frame.

Constants are mined by `tools/dm2_combat.py pursue` →
`demos/derived/combat_pursuit/pursuit.json` and **baked in** (no runtime JSON
dependency, same as the need thresholds): cost cap **670**, wall clock **3.5s**,
extrapolation **0.3s**. The mining exploits the fact that a demo carries only
entities in the recorder's PVS, so an opponent leaving the packet-entity set *is*
the sight-loss event (5788 demos → 359182 sustained episodes).

**A/B status — a real effect, but not the one the parity harness measures.**
Kill share is flat: **+0.55pt, 95% CI [−2.10, +3.20]** at 16 valid paired arms
(an initial 4-arm read said +2.55pt and was under-powered *and* drawn from the
two most favourable maps). The benchmark ON-vs-OFF pair shows what it actually
does, consistently across both nav baselines:

| | pickups | frags | deaths | K/D |
|---|---|---|---|---|
| shipped-dm | **−3.5%** | +9.8% | +7.9% | 0.521 vs 0.513 |
| cold-dm | **−3.6%** | +5.4% | +4.2% | 0.543 vs 0.537 |

It **converts item-collection time into combat, symmetrically** — everyone kills
more and dies more, so K/D and kill *share* barely move. That is precisely the
quantity the parity axis measures, so the parity harness is structurally blind to
this lever. Default OFF because it costs the project's north-star metric
(pickups, 7/8 maps down) without making a bot better at fighting; turn it on if
you want higher combat intensity. Telemetry: `pursue_start` / `pursue_end`. See
the `ozbot-re-pursuit` memory.

⚠️ **Two harness lessons, both cost a wrong conclusion here:**
1. **Power the test.** At the observed per-arm sd (~5.4pt) a +2pt effect needs
   ~40 paired arms; 4–8 arms resolves nothing finer than ~±5pt. Arms cost ~6s —
   there is no reason to under-power.
2. **`score` goes NEGATIVE on suicides.** On q2dm3/4/6/7 most bots finish below
   zero, so an even/(even+odd) kill *share* divides by ~zero (observed "shifts"
   of +420pt on a ±100 scale). `tools/parity_frags.py` now refuses to score a
   degenerate pool. Combat A/Bs belong on q2dm1/q2dm2/q2dm5/q2dm8; deaths are
   event counts and stay valid everywhere.

**Three levers built alongside it and NOT enabled** (all byte-identical off,
kept as "don't re-derive this" markers):
- `bot_hearing` — noise taxonomy (fire/pickup/pain/footsteps, radius + `inPHS`
  gated) feeding the same LKP slot. Rejected: mean **−2.33pt** over 4 seed-bases
  × 2 maps, deaths up. A noise carries no heading, so the bot walks into a fight
  it cannot see. Sight-driven pursuit wins *because* it knows a velocity.
- `bot_combatmove` — engagement movement styles (bitmask). The stand-ground style
  was **pre-gated out by the corpus** (aimed movement speed is flat across height
  advantage: p50 300/301/300); the committed circle-strafe style A/B'd at
  **−1.36pt**. The corpus explains it: pro strafe-reversal cadence is p50 0.6s,
  which shipped `bot_hop` already produces (0.7s legs) — commitment only costs
  unpredictability.
- `bot_lookahead` — corner-cut steering blend (value = max weight, try 0.4).
  **Nav-dependent**, so not default-ON: shipped-solo pickups **+4.5%** (5/8 maps)
  but cold-solo **−6.6%** (1/8). Dense curated navs make the off-graph cut safe;
  sparse self-learned graphs put it outside the channel the learner validated.
  Worth enabling on a server running curated shipped navs.

## Playbooks (the new capability)

Recorded input sequences replayed as nav links (NAV_LINK_PLAYBOOK):
1. **Record**: `record_inputs.bat` launches a 40Hz listen server; in-game,
   `capture start <name>` / `capture stop` (server command, `src/g_cmds.c`)
   carves a granular, user-named take — `ozbotre/logs/<name>.jsonl` + a synced
   `ozbotre/demos/<name>.dm2` — overwriting on name collision, so you can
   record the same maneuver clean multiple times in one session without
   relaunching. Bots use `--cvar bot_cmdlog 1` on a 40Hz server instead.
2. **Bake**: `py tools/make_playbook.py <log.jsonl> --slot N --start T0 --end T1
   --name mh_jump --out engine/ozbotre/playbooks/q2dm1.pbk` (`--list` to scan,
   `--auto [--auto-n N]` for validation segments).
3. **Run**: bot_playbook is default ON and inert without a .pbk. Aborted
   replays penalize their link (self-selection); watch `pb_*` telemetry events.
The executor is `src/bot_playback.c` — read its header before touching it; the
align/replay design decisions were all measured, not guessed.

**Analyzing a human/bot input log** before you bake: `tools/input_view.py
<log.jsonl> [slot]` segments *jumps* (strafe-jump/air-accel work);
`tools/traj_view.py <log.jsonl> [--coarse]` finds *dwell spots* (flags in-place
jumps the human left as "look here" markers) and segments the trace into
*excursions* out of the marked spot — the right tool for "bot gets stuck HERE,
here are the paths through" recordings.

The q2dm1 **Megahealth jump** is the intended first real entry — record it,
bake it, and A/B `item_health_mega` pickups (they are 0 without it).

# Known issues and limitations

Honest list of what the bot currently cannot do, with the evidence behind each item.
Most of these are *measured* limitations with failed fix attempts documented in
`PLAN.md` — read that (and the A/B protocol in `README.md`) before re-attempting one.

## Navigation / traversal

### Narrow-walkway items still fail (lift-gated items are SOLVED)
What used to be "the big one" — items behind lifts — was fixed in Phase 17: `bot_lift`
(default ON) turns learned lift columns into `PLAT` links and runs a wait/board/ride
controller, taking q2dm1's Grenade Launcher from ~5% to ~41% and the Chaingun from 0% to
~55% completion. Two capability wins now share the same shape: `bot_swim` (Railgun, Phase
15) and `bot_lift` (GL/CG, Phase 17), both from instrumented diagnosis of one named
failure rather than generic movement work. History of the failed attempts (ledge-jump,
two steering-only lift fixes, rollout ascent, demo-route surgery) is in `PLAN.md`
Phases 6–16.

**Megahealth (q2dm1) is SOLVED** as of 2026-07-04 via a recorded **playbook**
(`engine/ozbotre/playbooks/q2dm1.pbk`, `bot_playback.c`): a human strafe-jump capture routed as
a `NAV_LINK_PLAYBOOK`. `item_health_mega` 0 → 10 pickups / 8 seeds, ~85% climb success. The trick-
jump the bot could never do is now a replayed input stream. Count mega via **health-jump** (+100
near the ledge z>900) — the item's rot countdown under-logs the `pickup` event. Note: raw pickup
*count* is capped by the single item's respawn + bot contention on it, not by execution.

What remains vertically-gated:
- **Upper Rocket Launcher (q2dm1) is SOLVED** as of 2026-07-04 via the `rl_walkway`
  playbook (`q2dm1.pbk`): a human strafe-jump run across the z912 north walkway
  (300→483 ups, mined from a full-game `bot_inputlog` capture, `--start 222.4 --end
  226.65`). The upper RL (704,104,912) was **`no_path`** — the graph had no inbound edge
  at all — so the playbook link is what connects it. A/B (16 seeds, sv_fps 40): upper-RL
  pickups **0 → 5 per seed-base**, node `no_path → ok` at quit, **ITEM completion flat**
  (46 vs 46.5 pooled — cheaper than the reverted lift-demo attempt, which cost 3 pts).
  Caveat: ~65% drift-abort rate (the walkway's 180–250u drops are unforgiving to open-loop
  replay), so it converts ~1-in-4 engages — tuning headroom, but the pickups are already
  free. This is the first *narrow-walkway* playbook and validates the capability.
- **HyperBlaster (q2dm1) is SOLVED** as of 2026-07-05 (~20 pickups/8-seed-sim, was ~8).
  Two compounding bugs, both fixed: (1) the east ramp's UP-link was missing from the nav
  (bots only ever came DOWN it — the learner skips climbs), so A* had no real route and had
  learned through-wall flukes → added `377→293` walk + removed the 4 west fluke links to the
  HB in `baselines/q2dm1.nav`; (2) `bot_losfinal` (default on, **map-general**) — the
  final-approach override homed straight at any same-level item within 200u with **no
  clear-shot check**, driving the bot into the wall; added a knee-height LOS trace so a walled
  item makes the bot follow the nav path (the ramp) instead. A/B 3 seeds: west-wall pin-stall
  28s→0s, ITEM neutral. Real route: platform → east ramp up to z1048 (GL level) → drop onto
  the HB. (User pinpointed the route from a recording; `bot_losfinal` generalizes the fix to
  any walled same-level item on any map.)
- **q2dm2 (Tokay's Towers)** is still at its ~26% ceiling; its vertical items have not
  been re-measured since `bot_lift` landed — its plats may need the same treatment
  validated there.
- **Conditional shaft-mouth links**: links learned *across* an open lift shaft at the top
  level are only walkable while the plat is parked up; bots routing across the mouth when
  it's down fall through (observed in Phase-17 diagnosis). Harmless-ish (they land at the
  bottom and replan) but it wastes goal budget; a plat-aware link check would fix it.

Phase-17 diagnosis footnote, because it corrects Phase 16's conclusion: the dominant GL
failure was never boarding mechanics — it was the final-approach override homing on items
by *2D* distance, which trapped bots directly under the GL (567u below it) with path
progress frozen. `bot_lift` gates that override to the item's own level. The boarding
state machine mattered too (a bot standing in a plat's footprint holds the plat up
forever via the shaft-high touch trigger — waiting must happen *outside* the footprint),
but the override was the binding constraint.

### The graph can claim routes the bot can't execute
Root cause of the above: links are learned from any successful traversal (including lucky
falls or combat-shoved movement), so A* occasionally sells a path whose reverse or repeat
execution is beyond the bot. `Nav_PenalizeLink` prunes links a bot visibly fails on, but
only when the follower detects being stuck — orbiting near an unreachable node (the common
vertical-failure mode) never triggers it.

### Nav maturation has a quality ceiling
Letting the graph grow past ~330 nodes on q2dm1 *regresses* item completion (23% → 15%).
Coverage is not the bottleneck; extra nodes add routing noise. The canonical `q2dm1.nav`
(~349 nodes) is deliberately kept as-is; don't "improve" it by running bots for hours.
Importing pro-demo trajectories as nav data also makes the bot worse (movement mismatch).

### Small traversal TODOs never done
Teleporter links are not learned as links (bots that wander in get relocated, which also
resets the link-learning chain). Corner-scrape detection (bot grinding a corner at an angle
that defeats the stuck detector's 24u threshold) was identified in Phase 1 and never built.

## Goal layer

### Failure knowledge does not persist
`bot_itemfail`'s failure counts are in-memory only and reset on map change. Every fresh
run re-pays ~1 full-budget giveup per hard item before the blacklist kicks in. Persisting
per-item completability alongside the nav graph was considered and deferred (it would
change the A/B methodology, which assumes graph-only persistent state).

### ~20% of item attempts end in the bot's death
Combat interruption, not a nav failure — but it sits in the ITEM-completion denominator.
The nav-true ceiling at current combat intensity is ~46%, so headline ITEM% conflates
navigation quality with combat pressure. Keep this in mind when reading A/B results
(and see the measurement notes in `README.md`).

### Constants are tuned on q2dm1
Goal economics constants (`BOT_GOAL_BUDGET_BASE/SPEED`, `bot_budgetcap 15`, the 512u score
falloff, cooldown lengths) were tuned and validated on q2dm1, spot-checked on q2dm3 (2-seed
A/Bs, same win signature). Other maps inherit them unverified. Hazard avoidance
(`Bot_StepIsSafe`) is explore-only by design, so learned drop links still work — but it
also means a *routed* path can still walk into lava if a bad link was ever learned there.

## Combat

### No human-style movement execution
No strafe-jumping, bunny-hopping, rocket-jumping, or momentum preservation. This is why
pro-demo knowledge doesn't transfer (both the movement import and the weapon-priority
calibration were null/negative: pro data bakes in execution skill the bot doesn't have).

### Target-leading gaps
`bot_lead` models rocket (650u/s) and blaster/hyperblaster bolts (1000u/s). Grenade arcs
are unmodeled (grenades are simply not led), and leading aims at the target's current
velocity — it doesn't anticipate dodges or strafe reversals.

### Dodging is emergent only
Directed rocket dodging was tried twice (16-seed A/Bs) and removed: constant combat strafe
already captures the value. Consequence: bots do not deliberately break line-of-sight or
use cover; survivability comes from `bot_flee`'s retreat and the strafe pattern.

## Humanization (Phase 18)

The humanization stack closed most of the measured bot-vs-human distribution gaps
(mean KS across 8 features 0.355 → 0.218 vs 1,299 pro demos), but honestly:

- **Strafe-reversal texture regressed** (KS 0.16 → 0.29): `bot_hop`'s demo-fitted
  strafe legs are right by construction, but the *observable* side-velocity flips now
  include hop-landing physics jitter the metric can't separate. Fixing the metric
  (filter airborne frames) would be flattering ourselves; left as measured.
- **Stillness only partially fixed** (26% → 23% of time vs 12% human): the remaining
  standing-still time is lift WAITs (deliberate and load-bearing — fidgeting near a
  plat footprint can hold the lift up, so `bot_fidget` excludes lift states) and
  explore-mode wall encounters the turn-away only shortens.
  - **40Hz stillness breakdown (2026-07-04, still ~20% vs human 4.3%):** categorized bot
    still-frames (spd<10) by cause — **combat plant-and-shoot is #1 at 46%** (goal-mode with
    a live target, standing to aim), lift footprint wait/board 39% (mostly legit plat cycles),
    airborne apex/fall 9%, explore wall-stall 5%. The biggest lever is COMBAT: the move/aim
    decoupling lets the bot strafe while firing, but with no travel goal it plants — humans
    keep moving. A "combat jitter when stationary-and-engaged" is the untried fix (bot_flee/
    strafe already blends when there's move intent; the gap is the no-goal stationary fight).
    Lift-wait stillness is mostly irreducible (the plat cycle); the CG descent-trap slice of
    it was halved by the Bot_LiftThink ascent-gate above.
- **Speed texture is capped by capability**: humans exceed 300 u/s via strafe-jump
  momentum (their p90 is ~428); the bot can't without the movement-capability work
  this plan explicitly excludes. KS ~0.36 is the floor here.
- **Open maps lose combat tempo**: on q2dm5 the full stack cut frags ~40% (deaths
  −28%, K/D flat, ITEM +3.2pt) — FOV'd, hopping, glancing bots simply engage less on
  long sightlines. Distributed across behaviors, not one cvar. q2dm1/q2dm8 show
  −2%/−12%. This is the humanness trade expressing per-map; revisit only if a map
  feels dead to play.
- **Asymmetric cost vs omniscient bots**: in mixed matches (`bot_humantest`),
  humanized bots run a kill ratio of 0.818 vs the 0.946 control (~13.5% relative;
  final binary, 16+16 seeds). Hearing (weapon noise acquires through the cone)
  already halved the raw FOV cost; the residual is the blind spot working as
  intended.
- The parity harness needs an **even** `bot_count` (odd counts split 3:2 by id parity
  — measured as a phantom 1.53 kill ratio before the fix to the sweep rig).

## Variable-FPS conversion (fixed 40Hz port bugs, kept here as a checklist)

The port converted per-frame game logic from the fixed 10Hz assumption to variable FPS, but
three movers/spectator paths were missed and only bit at 40Hz (all fixed 2026-07-04):

- **Accelerative movers (`func_plat`/`func_door` with speed≠accel) stopped at 1/4 travel.**
  `Think_AccelMove` and its helpers (`plat_Accelerate`, `plat_CalcAcceleratedMove`) are
  authored in **per-10Hz-frame units** (`current_speed`, `remaining_distance -= current_speed`,
  velocity `*10` = ÷0.1s), but the think reschedules every server frame → at 40Hz it steps 4×
  per 10Hz tick, draining `remaining_distance` 4× too fast → the mover halts at 25% of its
  travel with NO blocker. This was q2dm1's "lift stuck half-way at 40Hz." Fix: gate the accel
  accounting to `FRAMESYNC` (step once per 10Hz keyframe), keep re-thinking each frame so the
  velocity stays applied. **Any map with accelerative movers hits this** if run at 40Hz on an
  un-patched DLL. Constant-velocity movers (`Move_Begin`) were already correct.
- **`plat_blocked` / `door_blocked` reversed direction every frame** (the crush *damage* was
  `FRAMESYNC`-gated, the *reversal* below it wasn't) → a blocked mover buzzes in place at 40Hz.
  Fixed by gating the reversal too. Rarer than the accel bug (needs a blocker).
- **Accelerative-mover-adjacent: lift controller trapped DESCENDING bots at the shaft
  top** (fixed 2026-07-04, `bot_move.c Bot_LiftThink`). `Bot_UpcomingHop` engages the lift
  whenever a PLAT link is within 160u of the path, without checking direction. A bot
  transiting/descending the q2dm1 NE upper platform (z920, above the Chaingun) got captured
  into a WAIT for the CG lift's top landing (node 251). But standing at the shaft top holds
  the plat UP via the shaft-high touch trigger, so the ride never comes — the bot burned the
  full 10s `LIFT_WAIT_TIMEOUT` with stuck-detection and the goal budget both frozen, then
  re-routed straight back in. This was the user-reported "bots stuck on the floor above the
  chaingun": #1 stall cluster (1408,1152,896), ~45s dead-standing/run, 8 `lift_timeout`/run.
  Fix: gate the fresh engage to ASCENTS — if the bot is already at/above the plat column top
  (`origin[2] > top_node_z - 24`), stand aside and let normal follow + stuck-penalize erode
  the (fluke) descent link. A/B 2 seeds @40Hz: lift_timeout 27→14 (−48%), CG-region stall
  72s→35s (−51%), **ITEM% flat** (45.5→45.3), giveups flat. Descent-by-riding-a-plat-down is
  not a q2dm1 route; revisit the gate if a map needs it. (Diagnosis method: measure — stall
  clustering + `lift_timeout` event coords + the 9.4s≈10s-timeout stall trace nailed it.)
  **Follow-up (`cg_descend` playbook, 2026-07-05):** the lift-gate killed the 10s freeze but
  left bots fumbling ~2.5s (they had no clean WAY DOWN — the descent links are flukes). A
  human-recorded descent off the HB floor to the arena (`q2dm1_000235`, t=35.83-38.4, anchor =
  node 61, the exact stuck node) bakes that missing route. First attempt over-fired (immediate
  engage pulled bots pausing there off their upper task: −1.6pt ITEM, +item_lost), so added a
  per-entry **`dwell`** option (`bot_playback.c`: engage only after the bot sits on the anchor
  ≥N s — rescue-only, not a through-route magnet). Sweet spot `dwell 0.75`: **ITEM flat**
  (45.7→45.4), CG-stall −24% (55→42s), giveups −7% (149→138). dwell 0/0.4 both cost ITEM;
  0.75 filters the false engages. Bots now cleanly descend instead of fumbling.
- **Chase-cam / eyecam spectator crashed** on a `game3_proxy` assert (`origin[2]`
  VERIFY_UNCHANGED) — an engine-side (`q2repro`) assert, not our DLL. The classic chase-cam
  legitimately teleports the spectator edict to the chased entity; excluded client edicts from
  the origin/angles asserts. See `q2repro/BUGREPORT_game3proxy_chasecam_assert.md`. Eyecam
  spectating works now. NOTE: `sv_fps 40` must be set for playbooks to replay (10Hz = tickrate-
  mismatch skip); all launch bats set it.

## Infrastructure / docs

- **Nav files saved by a `bot_lift`-ON run carry `PLAT`-typed links** (type 5, cost
  includes a +400 wait allowance). Loading such a graph with `bot_lift 0` is safe but not
  byte-equivalent to the pre-lift baseline (A* sees the +400 costs). The canonical
  `engine/ozbot/nav/q2dm1.nav` was deliberately left untagged — tagging recomputes from
  entities at every map load — but any non-parallel run (e.g. `run_server.bat`) will
  autosave tags into it. For strict A/Bs against pre-Phase-17 baselines, restore a
  `q2dm1.nav.*` backup first.
- `CHANGELOG.md` and `src/README.md` are stale Quake-2-mod-template boilerplate predating
  the bot. The living docs are `README.md`, this file, `PLAN.md`, and the analysis
  tooling in `tools/` (note: `PLAN.md` and `tools/` live outside this git repo).
- The demo-archive downloader (`tools/fetch_demos.py`) disables TLS verification because
  the archive site's certificate is expired.
- Real spectator/player interaction has known-fixed gotchas (slot collisions, respawn edge
  cases) — see the memory notes referenced in `PLAN.md` if a new one appears.

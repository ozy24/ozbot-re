# Known issues and limitations

Honest list of what the bot currently cannot do, with the evidence behind each item.
Most of these are *measured* limitations with failed fix attempts documented in
`../PLAN.md` — read that (and the A/B protocol in `README.md`) before re-attempting one.

## Navigation / traversal

### Narrow-walkway items still fail (lift-gated items are SOLVED)
What used to be "the big one" — items behind lifts — was fixed in Phase 17: `bot_lift`
(default ON) turns learned lift columns into `PLAT` links and runs a wait/board/ride
controller, taking q2dm1's Grenade Launcher from ~5% to ~41% and the Chaingun from 0% to
~55% completion. Two capability wins now share the same shape: `bot_swim` (Railgun, Phase
15) and `bot_lift` (GL/CG, Phase 17), both from instrumented diagnosis of one named
failure rather than generic movement work. History of the failed attempts (ledge-jump,
two steering-only lift fixes, rollout ascent, demo-route surgery) is in `../PLAN.md`
Phases 6–16.

What remains vertically-gated:
- **HyperBlaster / upper Rocket Launcher (q2dm1)**: ~5–10% completion, unchanged. These
  need *narrow-ledge walking precision* — the z920 walkway has 180–250u drops on both
  sides, and even the human recording the reference demo fell off it. A ledge-centering
  path follower is the missing capability (declared out of scope in the lift plan).
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

## Infrastructure / docs

- **Nav files saved by a `bot_lift`-ON run carry `PLAT`-typed links** (type 5, cost
  includes a +400 wait allowance). Loading such a graph with `bot_lift 0` is safe but not
  byte-equivalent to the pre-lift baseline (A* sees the +400 costs). The canonical
  `engine/ozbot/nav/q2dm1.nav` was deliberately left untagged — tagging recomputes from
  entities at every map load — but any non-parallel run (e.g. `run_server.bat`) will
  autosave tags into it. For strict A/Bs against pre-Phase-17 baselines, restore a
  `q2dm1.nav.*` backup first.
- `CHANGELOG.md` and `src/README.md` are stale Quake-2-mod-template boilerplate predating
  the bot. The living docs are `README.md`, this file, `../PLAN.md`, and the analysis
  tooling in `../tools/` (note: `../PLAN.md` and `../tools/` live outside this git repo).
- The demo-archive downloader (`../tools/fetch_demos.py`) disables TLS verification because
  the archive site's certificate is expired.
- Real spectator/player interaction has known-fixed gotchas (slot collisions, respawn edge
  cases) — see the memory notes referenced in `../PLAN.md` if a new one appears.

# Known issues and limitations

Honest list of what the bot currently cannot do, with the evidence behind each item.
Most of these are *measured* limitations with failed fix attempts documented in
`../PLAN.md` — read that (and the A/B protocol in `README.md`) before re-attempting one.

## Navigation / traversal

### Vertically-gated items are avoided, not collected
The big one. Items reachable only via lifts or multi-level platform ascents have near-zero
completion: on q2dm1 the Chaingun ~4%, HyperBlaster ~5%, Grenade Launcher ~6%, while
flat-route items complete at 50–100%. The learned graph + A* *find* routes to these spots
(paths exist, so goals commit), but the bot cannot reliably execute the ascent — bots
strand below the item and burn the full goal budget. Four architecturally distinct
traversal fixes (ledge-jump primitive, two lift/vertical-arrival fixes, rollout-based
ascent) all lost controlled A/Bs even when they demonstrably improved pathing diagnostics.

`bot_itemfail` (escalating shared blacklist after giveups) makes the bots *economically
avoid* these items instead of repeatedly failing them — good for throughput, but it means
the bot concedes some map control where strong items are gated. Fixing this needs a
genuine locomotion capability (reliable lift riding / platform ascent), not more tuning.

Confirmed systemic by the Phase-14 multi-map sweep: the ceiling tracks map verticality —
q2dm1 ~33% / q2dm2 (Tokay's Towers) ~26% ITEM with the same item-above giveup signature,
versus ~62% / ~55% on flatter q2dm5 / q2dm8. The bot's item logic is fine; the ascent
capability is the binding constraint on vertical maps.

Encouraging precedent (Phase 15): q2dm1's Railgun looked like this issue but was actually
**swim-gated** (water tube + vertical shaft), and a small targeted capability — `bot_swim`,
3D steering in water — took it from 0% to ~48% completion. The remaining offenders are
lift/platform ascents in air, a different (harder) capability.

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

- `CHANGELOG.md` and `src/README.md` are stale Quake-2-mod-template boilerplate predating
  the bot. The living docs are `README.md`, this file, `../PLAN.md`, and the analysis
  tooling in `../tools/` (note: `../PLAN.md` and `../tools/` live outside this git repo).
- The demo-archive downloader (`../tools/fetch_demos.py`) disables TLS verification because
  the archive site's certificate is expired.
- Real spectator/player interaction has known-fixed gotchas (slot collisions, respawn edge
  cases) — see the memory notes referenced in `../PLAN.md` if a new one appears.

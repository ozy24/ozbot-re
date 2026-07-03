# Plan: ride the lift, get the Grenade Launcher

Status: **DONE** (2026-07-03, PLAN.md Phase 17) — target beaten: GL 5.2% → **41.1%**
pooled over the 5-seed rig (plus Chaingun 0% → 54.5% for free), ITEM +3.7pt, no
regression on q2dm3/q2dm5/q2dm8. `bot_lift` is default ON.
Phase 0's diagnosis *overturned* the reframe below in one important way: the dominant
failure was the <200u **2D** final-approach override trapping bots directly under the
GL with path progress frozen — the wait-punishing subsystems in the table were real but
secondary. The state machine below was still built (boarding needs the footprint-clear
wait; standing in the footprint provably holds the plat up via the shaft-high trigger),
with one addition: a 2.5s BOARD no-movement failover for railed-off approaches.
Target: q2dm1 Grenade Launcher completion ~5–7% → **>30%** via the lift route,
with no regression on the standard 5-seed rig.
Prior art: PLAN.md Phases 15–16, memories `ozbot-swim-win` / `ozbot-lift-demo-findings`.

## Background — what we know (all measured, Phase 16)

- The nav graph **already contains the lift ride**: a learned column of links at fixed
  (1776, 1072), z 537→1037 (nodes 241→285), recorded when a goal-less wandering bot once
  stood on the plat by accident. A* routes GL attempts up it (cost ~1409). The top exit
  link (285 → GL node 33) exists.
- Trajectory mining of GL attempts: **bots never board** — every failure stalls 119–279u
  from the shaft. The ride was never attempted; the approach/boarding step is where it dies.
- `bot_lift` (vertical-context steering + 3D waypoint arrival, the land analogue of the
  swim fix) did NOT convert GL. Steering geometry wasn't the missing piece.
- Demo-route graph surgery (making the ramp route routable) didn't convert either —
  reachability ≠ executability. Reverted.

## The reframe

Boarding a Quake 2 `func_plat` is physically trivial: plats rest at the bottom flush with
the floor; walk on, the touch trigger fires, it carries you up. Bots are real clients, so
all of this works for them — that's how the column got *learned*. Goal-driven bots fail
because **every subsystem punishes stillness**:

| subsystem | what it does to a waiting bot |
|---|---|
| `Bot_UpdateStuck` (1.0s, <24u) | declares it stuck |
| `Bot_RolloutRecover` / `Bot_Unstick` | wiggles it away from the shaft |
| replan (1.5s) + `Nav_PenalizeLink` | penalizes, then deletes, the column link |
| `bot_goalbudget` clock | burns the budget while it waits |

A lift ride needs 2–8 seconds of deliberate waiting — the one thing the bot is
architecturally forbidden to do. **Don't make the bot move better; teach it that near a
lift, not moving is the move.**

## Phase 0 — Instrumented diagnosis (half a session; do first)

Throwaway `bot_liftlog` cvar: while a bot's goal is the GL and it is within ~300u of the
shaft, log per tick: bot position, `groundentity` classname, and the plat entity's
`moveinfo.state`. One fastsim run answers, with evidence instead of inference:

1. Does the bot ever enter the plat footprint?
2. Is the plat up or down when it arrives?
3. Which subsystem (unstick / replan / budget) yanks it away first?

Also confirm in `g_func.c`: q2dm1's plat rests at `STATE_BOTTOM`, and `Touch_Plat_Center`
fires for bot clients. This validates the reframe before building on it. If the evidence
contradicts the table above (e.g. bots never even reach the alcove), stop and re-diagnose.

## Phase 1 — Make the graph tell the truth about plats

- New link type `NAV_LINK_PLAT` (bot_nav.h; nav format v1 already stores a type byte, so
  no format bump).
- Tag at **learn time**: in `Nav_LearnStep`, if `ent->groundentity` is a `func_plat`, the
  link being recorded is a plat link (robust, no geometry heuristics).
- **Load-time reclassification** pass for the existing canonical graph: walk links with
  dz > 48 and horizontal < 30 become PLAT (the column signature; Phase 16 found 28 such
  links across q2dm1's two lifts).
- `Nav_LinkCost`: plat links get distance + a **wait allowance** (~+400 cost units) so
  `bot_goalbudget` funds the ride and `bot_pathcost` prices it fairly against alternatives.

## Phase 2 — The board/ride state machine (the real work, ~150 lines)

Per-bot lift state, entered when the follower's next hop is a PLAT link:

- **WAIT** — hold at a staging point (the path node before the column base). If the plat
  isn't at the bottom, stand still. Critically: suppress stuck-recovery, replanning, and
  link penalization; freeze `progress_time` so waiting counts as progress. Hard wait
  timeout (~6s) falls through to the normal giveup path so it can't deadlock.
- **BOARD** — plat at `STATE_BOTTOM` → walk to the column x,y on the plat surface.
- **RIDE** — while `groundentity` is the plat: zero movement intent and let it carry.
  The existing `bot_lift` 3D-arrival logic already advances column waypoints as z rises.
  Suppress the combat *movement* blend during the ride (keep aiming/firing — the
  movement/aim decoupling makes "shoot while riding" free); accept sitting-duck risk in v1.
- **EXIT** — top column node arrives in 3D → resume normal path following (285→33→GL).

Finding the plat entity: when entering WAIT, scan for a `func_plat` whose horizontal
bounds contain the column x,y (q2dm1 has two; cache per column). Telemetry: emit
`lift_wait` / `lift_ride` / `lift_exit` events so the miner can attribute failures to a
sub-state.

Gate everything behind one cvar (reuse `bot_lift` — it becomes "the lift capability"
rather than just the steering tweak), default OFF until validated.

## Phase 3 — Validation ladder

1. **Micro**: lift events show WAIT→BOARD→RIDE→EXIT completing; trajectories climb the
   shaft (compare against the gl1 demo ride profile).
2. **Target metric**: GL completion >30% (Railgun precedent: 0% → 48% once the capability
   landed).
3. **Standard 5-seed q2dm1 A/B** (flip the cvar only): no overall ITEM/pickup regression
   allowed. Extend to 13 seeds if borderline — the Phase-16 `bot_lift` experience showed
   3-seed reads mislead (+3.7pt on 3 seeds shrank to +0.7pt on 13).
4. **Regression check** on q2dm3/q2dm5/q2dm8 (expect bit-identical runs where no plat is
   ever pathed, as `bot_swim` was on q2dm8).
5. If accepted: default ON, README/PLAN/KNOWN_ISSUES updates, memory entry, the usual.

## Out of scope

- **HyperBlaster / upper-RL**: those need narrow-ledge walking precision (bots — and the
  human recording gl2 — fall off the z920 walkway). Different capability, separate plan.
- **Graph surgery**: the demo-route splice stays reverted; this plan needs no graph edits
  beyond link reclassification (the column and its approach are already in-graph).

## Risks (measure, don't pre-engineer)

- **Multi-bot plat interference**: two riders, or a bot beneath a descending plat causing
  block-reverse ping-pong. `bot_claim` limits simultaneous GL-seekers but not other plat
  traffic. If the A/B shows it, consider a shared "plat busy" claim.
- **Wait-time inflation**: time-to-pickup on lift routes will grow; the wait allowance in
  link cost should keep the budget honest. Watch giveup-at-budget rates on GL attempts.
- **Combat during WAIT**: a waiting bot is exposed; flee logic may pull it off the
  staging point (fleeing overrides movement intent). Acceptable in v1; mine `lift_wait`
  terminations to size the problem.

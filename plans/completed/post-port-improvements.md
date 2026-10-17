# ozbot-re — Post-port improvement roadmap

*Drafted 2026-07-04, after R0–R4 shipped (see `../../PLAN.md` §8 and `../../CLAUDE.md`).
Status: PROPOSED. Three phases, gated, executed top-to-bottom. User has confirmed they
can record human input captures on a 40Hz server (so P1a is record+bake, not blocked).*

## Framing: the port bought two capabilities; neither is cashed in yet

ozbot-re is validated end-to-end but has not yet produced a **single new pickup that the
10Hz bot couldn't already get**. R4 only exercised the playbook executor with
*bot-recorded* segments (`baselines/validation-q2dm1.pbk`); `engine/ozbotre/playbooks/
q2dm1.pbk` does not exist. The 40Hz epoch also *regressed* ITEM 53%→47% (R3 traced this to
death-interrupted fetches — combat runs ~+50% hotter at 40Hz and was never re-tuned for it).

40Hz baseline to beat (16 seeds × 90s, same build): **~418 pickups / 47% ITEM / 235 frags.**
These numbers are NOT comparable to any 10Hz history — new epoch.

This roadmap realizes the two shelved capabilities and recovers the regression:
- **P1** turns the playbook machinery into real pickups on the two items every prior arc failed.
- **P2** re-tunes combat for 40Hz, attacking the death-during-fetch that suppresses ITEM.
- **P3** confirms nothing silently regressed across the map suite and pays down cheap debt.

All work uses the standard rig and A/B discipline (see "Method" at the bottom).

---

## Phase P1 — Playbooks: cash in the two hardest q2dm1 items

Target the items that beat nav-growth, economics, lift, and demo-surgery every time:
`item_health_mega` (0 pickups, ever) and HyperBlaster / upper-Rocket-Launcher (~5–10%,
declared out of scope in the lift plan for want of a "ledge-centering follower").

### P1a — Megahealth jump (record + bake) — ✅ DONE 2026-07-04 (megahealth 0 → 10 / 8 seeds)
`item_health_mega` @ (480,1376,912), on a ~120u trick-jump ledge.

**RESULT:** The user recorded 7 clean takes (`q2dm1_20260704_085303.jsonl`). The route is a
pure strafe-jump (no rocket): a vertical climb from the launch pad **(688,1168,792)** at a near-
standstill (a fixed corner every take used) up to the upper ledge z920, then a strafe-jump leap
across landing on the mega. Baked take @ t≈53.3 → `engine/ozbotre/playbooks/q2dm1.pbk`.
**Megahealth pickups 0 → 10 across 8 seeds × 90s** (control literally 0 — unreachable without the
replay), ~10 of 13 engages convert (~85% climb success; count capped by the single item's respawn
+ contention), headline metrics flat (ITEM +0.5, pickups flat, giveups −5, deaths flat — no
regression). `bot_playbook` default ON; the deployed `.pbk` makes it live in normal play.

**Two real bugs fixed to get here** (both were latent — R4 only validated *grounded* bot-recorded
runs, so the jump/standing-start paths were never exercised):
1. `tools/make_playbook.py` never resampled human captures. A human client sends usercmds at its
   *render* rate (~80-160Hz, msec 6-13), so 2-3 records share one 40Hz server tick. Added
   `resample_to_grid`: one record per tick, **OR-ing the momentary buttons** (jump/attack) across
   the bucket — decimating to the last sub-frame silently dropped brief jump presses.
2. `bot_playback.c` replay cursor was pure position-matching. A standing/jump start barely moves
   for its first ticks, so the cursor matched the pre-launch tick forever and the bot never left
   the ground (`pb_abort_stall`, frozen at the anchor). Made the cursor **time-driven** (one tick
   per frame — correct for a 40Hz replay of a 40Hz capture) with position only slipping it within
   `[-PB_MAX_LAG, +PB_MAX_LEAD]`; the clock floor guarantees the launch ticks fire. `pb_frame`
   field added. Drift monitor now the primary abort (correct).

**Telemetry note:** the megahealth's "rot" countdown keeps its entity `avail` briefly after the
grab, so the `pickup` *event* under-logs mega grabs. Count mega via **health-jump** (+100 near the
ledge, z>900, within ~70u of (480,1376)) — see the A/B analysis one-liner in the R4-completion
commit. `analyze.py` also fixed (crashed on `pb_*`/world events that lack a `bot` field).

1. **(Optional free spike, no user dependency) Synthesis attempt.** The
   `ozbot-longjump-10hz-finding` blamed 10Hz command granularity: the jump is airborne
   ~0.675s and needs ~30Hz input. At 40Hz the bot commands *finer than a human*. Try a
   `gi.Pmove` forward-rollout search from the ledge-approach node for an input sequence that
   lands on the ledge (reuse the rollout planner's pre-sim harness). If it finds one, bake it
   as a synthetic playbook entry — no capture needed. Time-box to a short spike; fall back to
   record+bake on failure. (Recording is known-good; synthesis is a bonus.)
2. **Record.** `record_inputs.bat` (server at `sv_fps 40`), do the box+strafe jump cleanly
   several times, `quit` to flush. `bot_inputlog` writes per-usercmd JSONL at full 40Hz fidelity.
3. **Bake.** `py tools/make_playbook.py <log.jsonl> --list` to find the takes, then
   `--slot 0 --start T0 --end T1 --name mh_jump --out engine/ozbotre/playbooks/q2dm1.pbk`.
   The baker resamples to the 25ms grid, sets the anchor (origin/yaw/velocity/ground + tolerances),
   the tick-indexed usercmd stream, the expected-origin timeline, and the exit node.
4. **Route.** Confirm A* registers the entry/exit nodes + `NAV_LINK_PLAYBOOK` at load and
   routes MH attempts through the recorded move (cost = duration). `bot_playbook` is default ON
   and inert until the .pbk exists, so this is the flip.

**Acceptance:** `item_health_mega` pickups go 0 → meaningfully >0 across ≥8 seeds, conversion
tracked per attempt (`pb_*` telemetry), with ITEM%/value up and giveups NOT up. If the drift
monitor aborts most replays, tighten the anchor tolerance / add takes before abandoning.

### P1b — Narrow-walkway playbooks (HyperBlaster / upper-RL) — upper-RL ✅ DONE 2026-07-04
The z920 walkway has 180–250u drops both sides; the missing capability is ledge-centering
precision. A **recorded traversal is that follower** — this is the port's own "execution
controller beats graph surgery" lesson applied to the last vertical q2dm1 offenders.

**Upper-RL DONE (`rl_walkway`, 2026-07-04):** mined from a full-game `bot_inputlog` capture
(`record_game.bat`, 492s) rather than a targeted take — found the human's z912 walkway
strafe-run (300→483 ups) by searching input frames near the `no_path` upper-RL coord and
tracing the 4s approach, then `make_playbook --start 222.4 --end 226.65 --slot 0 --pos-tol 24
--yaw-tol 30`. The upper RL (704,104,912) was **`no_path`** (zero inbound edges), so the
playbook link is what connects it. A/B 16 seeds @ sv_fps 40: **upper-RL 0 → 5/seed-base**,
node `no_path→ok` at quit, **pooled ITEM flat** (46 vs 46.5). ~65% drift-abort (open-loop
replay vs the 180-250u drops) → ~1-in-4 conversion; free pickups, tuning headroom. Installed
canonical + `baselines/q2dm1.pbk`. **HB still open** — same-shape capture, human approaches at
t=388.3 / t=490.5 in the same log (`q2dm1_20260704_220049.jsonl`); the mid-approach hesitation
in those takes needs a cleaner sub-segment or a fresh take.

Original recipe (for HB and future maps):
1. **Record** a clean human walk across the walkway to the item (or mine a full-game capture).
2. **Bake** each as a playbook entry; the anchor is the walkway mouth, exit is the item's node.
   Playbook links are penalize-protected and self-selecting (a bad replay penalizes its own link).
3. **Route + validate** as P1a. These items are also `bot_itemfail`-blacklisted historically, so
   verify the blacklist doesn't suppress the new route before it gets a chance (may need a
   completability reset once a playbook link exists for an item). *(rl_walkway fired fine without
   a blacklist reset — the no_path→ok node was never blacklist-suppressed.)*

**Acceptance:** HB and/or upper-RL completion rises from ~5–10% to a clear >0 delta over ≥8 seeds
without an ITEM regression elsewhere (the maturation-regression trap: more attempts at hard items
that still fail mid-route *dropped* overall ITEM in Phase 16 — watch pooled ITEM, not just the
target item).

**P1 GATE:** ≥1 of {MH, HB, upper-RL} converts at a rate that lifts pooled ITEM% or value over the
40Hz baseline across ≥8 seeds; no giveup/death-rate regression. This is the "the port produced a
new win" gate — if all three fail to convert, the playbook capability itself is in question and we
stop and re-diagnose rather than pushing P2/P3.

---

## Phase P2 — Re-tune combat for the 40Hz epoch (recover the ITEM regression)

R3 established the 47%-vs-53% ITEM gap is death-interrupted fetches, and combat is ~+50% hotter
at 40Hz — yet aim (`react/turn/error`), `bot_lead` projectile speeds, and `bot_flee` thresholds
are all 10Hz-tuned. All A/Bs here use the **id-parity head-to-head harness** (even `bot_count`,
paired controls, `tools/parity_frags.py`; the raw even/odd read has a baseline bias — read the
*paired ratio shift*).

### P2a — Aim / lead re-sweep at 40Hz
Re-run the `bot_aimtest` / `bot_leadtest` sweeps (16 seeds) at `sv_fps 40`. The finer body changes
the optimal lead, and the 10Hz-keyframed *decision* layer now interacts with 40Hz *aim* sampling in
ways the 10Hz sweep never saw (R3 already found per-tick combat decisions worth +28–41% frags).
Fold in only changes that win the paired ratio consistently; keep the diagnostics as permanent
infra (house style — like `bot_skilltest`).

### P2b — Directed rocket dodging, re-tested
Rejected twice at 10Hz **specifically because 100ms reaction was too coarse to time a sidestep**.
At 25ms ticks this may finally clear the bar. Re-introduce the perpendicular-sidestep dodge behind
a cvar, 16-seed paired parity, measured against the death-during-fetch metric (does it reduce
deaths-mid-attempt, not just win duels?). Accept only on a clean win; this is the lever most
directly aimed at the ITEM regression's stated cause.

### P2c — Flee thresholds at 40Hz
`bot_flee`'s toughness thresholds (75 / 0.65× / recovery hysteresis) were set at 10Hz combat tempo.
At +50% intensity a bot commits to more losing fights before fleeing. Sweep the thresholds; the
health-fetch variant stays cut (it cost 7 ITEM pts) unless paired with a *reachable-recovery-item*
gate as noted in Phase 12.

**P2 GATE:** pooled 40Hz ITEM% recovers measurably toward the 10Hz 53% (the gap is death-driven, so
the honest target is "deaths-per-attempt down + ITEM up," not matching 53% outright), parity kills
flat-or-up, across ≥16 seeds. No single lever ships without its own clean paired win.

---

## Phase P3 — Re-baseline the map suite + pay down cheap debt

Lower ceiling; this is how we prove the port + P1/P2 didn't silently regress a map, plus two
self-contained known-issue fixes.

### P3a — Multi-map 40Hz re-baseline
Bootstrap/refresh nav for q2dm2/3/5/8 at 40Hz (distinct net_ports per bootstrap server), run the
standard rig ×3 seeds/map, confirm the big wins survive per-map (pathcost, decisive, lift,
strafejump, swim). Re-measure **q2dm2's vertical items with `bot_lift`** (never done — its plats
may need the same treatment that fixed q2dm1's). Document as the 40Hz map epoch.

### P3b — Persist failure knowledge across map changes
Known issue: `bot_itemfail`'s completability counts are in-memory and reset on map change, so every
fresh run re-pays a full-budget giveup per hard item. Persist per-item completability alongside the
nav graph (or a sidecar). Note this changes A/B methodology (adds persistent non-graph state) — gate
it and measure the first-N-seconds giveup cost, not just steady state.

### P3c — (Optional) Humanness re-profile at 40Hz
The OU/smoothing alphas were dt-corrected in R3 but never re-measured. Re-run `tools/humanness.py`
at 40Hz vs the 1,299-demo reference; the smoother 40Hz body may have moved the KS distances (aim
texture especially). Only retune if a distribution regressed; the stack already trades ≤10% frags.

**P3 GATE:** every map's headline wins reproduce at 40Hz (no silent per-map regression); P3b/P3c
each ship only on their own clean measurement.

---

## Method (applies to every phase)

- **Rig:** `py tools/run_parallel.py --repro --fastsim --instances 16 --seconds 90 --bots 5
  --seed <S> --cvar sv_fps 40`. `--repro` = q2reproded + gamex86_64.dll + ozbotre gamedir +
  `com_rerelease -1`. `--seconds` is game-seconds (self-quit via `bot_quitafter`).
- **Seeds:** always A/B across ≥8 (nav) / ≥16 (combat) seeds; single-seed reads mislead. Worker *i*
  gets `seed+i`.
- **Bit-exact gate:** same-seed telemetry md5 must match for a no-op change; cross-binary bit
  identity is unattainable (FP layout jitter) — flip cvars **within one binary** for A/Bs.
- **Live-canonical hazard:** if you may be playing (`play.bat`) while measuring, pin `--mod` with a
  scratch gamedir holding a fixed DLL + nav/playbook snapshot (`baselines/` has the pinned nav +
  validation .pbk). A live server autosaves nav and breaks same-seed comparisons.
- **Combat A/Bs:** even `bot_count`, paired parity controls, read the ratio shift (`parity_frags.py`).
- **House style:** new capability behind a cvar, default OFF until it wins an A/B (playbook links are
  the exception — `bot_playbook` defaults ON but is inert without a .pbk). Reverted experiments are
  removed from source, tools kept.
- **Miners:** `mine_goals.py` (giveup/attempt breakdown, key by (file,bot) on merged logs),
  `mine_indecision.py` (standing re-decides), `verify_timing.py` (tick invariants),
  `humanness.py` (KS vs demos).
- **On landing:** each phase that ships becomes a new `../../PLAN.md` phase entry + a memory update;
  move this doc's completed sections to `plans/completed/`.

## Sequencing rationale

P1 first because it's the only work that makes the port a *visible* win and P1b needs no combat
retune to land. P2 second because it recovers the regression the port introduced and P1's extra
item routes will themselves take more combat pressure (more reason to fix death-during-fetch). P3
last because it's confirmation + debt, and it should run *after* P1/P2 so the map re-baseline
captures the improved bot, not the pre-improvement one.

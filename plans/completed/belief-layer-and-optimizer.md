# ozbot-re belief layer + optimizer plan (2026-07-22)

Where to take the bot next. Diagnosis behind this plan: the reactive/execution
layer is at its local ceiling — the rejection log proves it (bot_combatmove,
bot_wpnselect, dodge ×3, bot_hearing, demo-movement import all lost; all were
stateless execution tweaks). The features that *won* gave the bot **state it
didn't have** — claim (contention), commit (marginal cost), hazard (death
feedback), need (stock model). So this plan adds a belief/state tier on top of
the existing goal machinery — no rearchitecture — plus two process-level plays
that exploit infra we already have (fastsim as a black-box optimizer; the
playbook executor as a self-improvement sink).

**Sobering prior from bot_pursuit (read `ozbot-re-pursuit` memory first):** the
one *combat-side* state feature, properly powered, is indistinguishable from
zero (+0.55pt, CI [−2.10, +3.20]) and sits default-OFF. All the confirmed
state-feature wins are on the **economy axis**. This plan therefore leads with
economy-side state (control timing, danger routing, self-playbooks) and treats
combat-side belief work as higher-risk — every combat A/B here must be powered
per the harness lessons below, and a null outcome is a plausible result to
budget for, not a surprise.

Also read: `ozbot-re-commit-win`, `ozbot-re-parity-axis` memories; the
Perception section of CLAUDE.md. **Do not re-derive bot_hearing /
bot_combatmove / bot_dodge**, and demo-mined combat *execution* transfer is
dead (three strikes).

## Methodology invariants (every phase)

- New behavior ships behind a cvar, **default OFF**, off-state **byte-identical**
  (same-seed telemetry-md5 gate) before any A/B.
- `deploy.bat` between build and sim (see `ozbot-re-deploy-gotcha`).
- Economy A/Bs: 16 seeds, paired, `--repro --fastsim --cvar sv_fps 40`.
- **Combat A/Bs — the two pursuit lessons are law:**
  - **Power the test.** Per-arm sd is ~5.4pt kill share; a +2pt effect needs
    **~40 paired arms**. Arms cost ~6s — never conclude from 4–8 arms; they
    resolve nothing finer than ~±5pt.
  - **Kill-share only on q2dm1/q2dm2/q2dm5/q2dm8** (`score` goes negative on
    suicide-heavy q2dm3/4/6/7 and share becomes garbage; `parity_frags.py`
    guards this). Death *counts* stay valid everywhere.
- Ship = flip default ON + benchmark snapshot (`benchmark.py --note`) + memory
  entry (win or rejection — rejections are markers).
- Every phase has a **kill criterion**. Kill fast, write it down, move on.

## Phase 0 — Regret miner (tool-side, do first, cheap)

"Dumb" is a human judgment the headline metrics may not measure. Before
building anything, mine the telemetry for moments where a counterfactual was
obviously better. The hazard, decisive, and outnumbered work all started from
exactly this kind of shame-signal mining.

- **Build:** `tools/mine_regret.py` over the standard run logs. Event classes:
  - enemy in FOV + in range, never engaged (ran past a fight);
  - item within ~80u of the executed route, not collected, needed
    (ammo/health low by the need thresholds);
  - heading reversals within <1s (residual ping-pong, post-bot_decisive);
  - death within ~2s travel of an untaken health item;
  - goal churn (switches per minute; spikes = indecision).
- **Output:** frequency-ranked taxonomy per map, with timestamps so specific
  incidents can be replayed/watched.
- **Purpose:** re-rank Phases 2–6. If one regret class dominates, it jumps the
  queue. Also gives each later phase a before/after regret readout.
- **No DLL change.** May need one or two extra telemetry events (log-only,
  off-state md5 unchanged when the log cvar is off).

## Phase 1 — Joint cvar optimization (tool-side, runs in parallel with 0)

Every tuned constant (bot_commit 0.8, pursuit bounds, hop legs 0.7s, budgets,
need thresholds…) was optimized **holding the others fixed** — a
coordinate-wise optimum, not a joint one. We own a 400×-realtime, bit-exact,
16-way-parallel fitness function; use it as one.

- **Build:** `tools/optimize_cvars.py` — black-box search (CEM first: ~50
  lines, no dependency; Optuna if CEM shows signal worth refining) over the
  vector of constants that are **already cvars**. Baked constants stay baked
  in pass 1; cvar-expose more only if pass 1 finds signal.
- **Fitness:** economy-led composite — solo + dm pickups on the fixed seed
  bank. Kill share may join the composite only from the four valid maps, and
  given its noise (sd ~5.4pt/arm) weight it low or use deaths (event counts,
  valid everywhere) instead.
- **Overfit guard:** holdout = disjoint seed set AND holdout maps (train
  q2dm1/q2dm8, validate q2dm2/q2dm5). A config only "wins" if it beats
  baseline on the holdout, confirmed by a properly powered paired A/B.
- **Success:** any confirmed holdout win. **Equally valuable if it finds
  nothing:** that is the definitive is-there-headroom-in-the-current-layer
  probe. Budget it (overnight wall-clock) and report either way.

## Phase 2 — Item timing → control play (extend `goal_timing`) — first DLL phase

Economy axis, where state features have actually won. The machinery
half-exists: `Item_RespawnEta`/`goal_timing` in `Goal_Select`
(bot_goal.c:629-737) already pre-positions — but only inside a fixed
`ITEM_PREEMPT_SECS 4` window. Real control play times the *route*, not the
last 4 seconds.

- **Change:** a control item (≥ ITEM_CONTROL_VALUE) becomes a candidate when
  `eta <= route_travel_estimate + slack`, where route time derives from the
  A* cost the scorer already computes (calibrate cost→seconds from telemetry
  once). Arrive at T−1, deny the cycle. Cvar `bot_control` scales the slack
  (0 = today's fixed 4s behavior, byte-identical).
- **Contention guards:** timing waits respect bot_claim (one timer per item);
  cap wait-on-spot time (a bot camped on a spawn that never pays is worse
  than economy); a timing goal loses to any available item whose score beats
  the discounted control value (bot_commit already provides the arbitration
  shape). Note bot_train's rejection reason was exactly "expensive round trip
  to a respawning platform" — this phase is also the missing
  "item-availability awareness" that could revive bot_train later.
- **Measure:** control-item pickup share (mega/top-weapon per-item counts via
  Bot_ItemName telemetry), benchmark dm columns, new `timing_wait` telemetry
  (duration + paid/unpaid). **Kill criterion:** dm pickups down, or a
  nontrivial share of timing waits unpaid.

## Phase 3 — Danger heatmap (`bot_danger`, DLL)

Generalize bot_hazard from "lava kills" to "fights kill". Per-nav-node
death/kill accumulation → route-cost modulation by health state. Emergent map
sense; still mostly an economy/survival lever, measurable by event counts.

- **v1 (in-match only, no persistence):** accumulate deaths (own + observed)
  and kills per nearest nav node, decaying over minutes. Route cost: when
  weak (below the existing strength gate), multiply hot-node edge cost
  (capped — hazard's lesson: price, don't forbid; SLIME 4× precedent); when
  strong, a mild discount to patrol hot zones.
- **v2 (only if v1 wins):** persist across runs as a **sidecar** next to the
  .nav — NOT inside the .nav (keeps md5/A-B gates and FROZEN semantics
  untouched).
- **Measure:** deaths (counts — valid on all maps; suicide-telltale
  deaths−frags), dm pickups; kill share on the four valid maps at 40 arms if
  the death read looks real. Solo must be ~byte-identical (no combat → no
  heat). **Kill criterion:** pickups down >2% (over-detouring) or no death
  reduction at proper power. Remember the comprehensive-plan finding: global
  death rate in a symmetric sim is near zero-sum — the honest win is the
  paired parity read, not everyone dying less.

## Phase 4 — Self-playbooks (bot mines its own successes)

Last-leg precision is the oldest standing bottleneck. Playbooks fix exactly
that, but every playbook so far cost a human recording session. The bot's own
inputs are already recordable (`bot_cmdlog 1`) and abort-penalty
self-selection already prunes bad links.

- **Pipeline:** `tools/mine_selfplay.py`:
  1. run the standard rig with `--cvar bot_cmdlog 1`;
  2. join telemetry (pickups, giveups) with cmdlog segments to find
     **successful completions of historically-failing attempts** — target
     list = gen_nav/benchmark gated-item lists + top giveup items per map;
  3. score takes (clean, fast, minimal correction), bake the best via the
     existing make_playbook.py machinery;
  4. per-link validation A/B before it enters the shipped playbook set.
- **Why the q2dm4/5 negative doesn't apply:** that was *human* trajectories
  grafted as nav edges — embodiment mismatch + detour over-investment (since
  addressed by bot_commit). This is the bot's own inputs, replayed by the
  executor built for exactly this, with self-penalty on abort.
- **Measure:** per-item pickup counts on targeted items; giveup rate; solo
  benchmark columns. **Kill criterion:** baked links abort >30%, or solo
  pickups flat after 3–4 targeted items (the residual last-leg failures
  aren't replayable-input-shaped).

## Phase 5 — Enemy belief differential (`bot_enemymodel`, DLL) — combat axis, eyes open

The conceptual continuation of pursuit: pursuit stores where an enemy *was*;
this stores what the enemy *is*. Every combat gate today conditions on own
strength only (`Combat_Strength >= 70`); the upgrade is conditioning on the
**differential**. Deliberately sequenced *after* the economy phases: pursuit's
powered null says combat-side effects on this axis are ≤ a couple of points —
detectable only with the 40-arm discipline, and possibly not there at all.

- **State:** small per-enemy record keyed like the LKP (`lkp_*` fields,
  bot.h:215-228), stamped on the 10Hz combat keyframe while visible:
  - **est. health:** spawn baseline − damage *this bot* dealt (hook the
    attacker-known T_Damage path); bump on seeing them take health/armor
    (item vanishes near them while visible); reset on observed death/respawn.
  - **est. weapon:** visible weapon model while sighted — only honest,
    sight-derived info, same discipline as pursuit.
  - decay to unknown after N seconds unseen.
- **Consumers (existing gates only, no new machinery):** engage/hold, flee
  threshold, and the pursue cost cap all shift by the differential. (Watch
  for bot_survive's failure shape — its negative was health-*seeking*, but
  any timidity coupling can reproduce it.)
- **Telemetry:** `belief_engage` / `belief_refuse` with the estimated
  differential AND sim ground truth, so the estimator is validated offline
  before the behavior A/B (a bad estimator invalidates the whole test).
- **A/B:** **40+ paired arms**, q2dm1/q2dm2/q2dm5/q2dm8, kill share + death
  counts. **Kill criterion:** CI includes zero at 40 arms, or deaths up.
  Pursuit's own gates (sweep the `>= 70` strength gate — the corpus refuted
  it) could get re-swept for free here, since the differential subsumes it.

## Phase 6 — Learned last-leg micro-controller (conditional spike)

Only if Phase 4 leaves a measurable last-leg residual. The one contained spot
where a learned policy fits: the final ~100u approach to an item.

- Tiny policy (start with a table / linear over local features: relative item
  pos, velocity, ledge/step geometry probes), trained in fastsim, reward =
  pickup success, hard fallback to existing steering on low confidence.
- This is the "is a rearchitecture worth anything" hypothesis tested in
  miniature on the known bottleneck. Timebox it; a null here is strong
  evidence the current architecture isn't the binding constraint.

## Sequencing

```
Phase 0 (regret miner)   ─┐  tool-side, parallel, ~immediate
Phase 1 (cvar optimizer)  ┘
   ↓ (0 may re-rank what follows)
Phase 2 (control timing)    — economy axis, machinery half-exists
Phase 3 (danger heatmap)    — economy/survival axis, independent
Phase 4 (self-playbooks)    — economy axis, tool-heavy, independent
Phase 5 (enemy belief)      — combat axis, powered A/B or nothing
Phase 6 (learned last-leg)  — gated on Phase 4's residual
```

Economy phases lead because that's where state features have confirmed wins;
the combat phase runs last with the powered-A/B discipline pursuit taught us.
Each shipped phase gets a benchmark snapshot and a memory entry; each rejected
one gets a rejection-marker memory so it never gets re-derived.

---

# RESULTS (completed 2026-07-22, branch `belief-layer`)

**One phase shipped, five rejected, one not built (gate not met).** Every rejected
lever is left in the tree default-OFF and byte-identical (same-seed q2dm1
telemetry md5 `38503d1a99964b23c74d2f437c06e807` verified after each), so none of
them get re-derived.

## Phase 0 — regret miner — SHIPPED (`tools/mine_regret.py`)

Five counterfactual classes, per map, with replayable timestamps. Two definitions
had to be rebuilt after their first cut was mostly false positives, and both
lessons are baked into the tool:

- a **heading-reversal** test for ping-pong flagged 1556 incidents on q2dm1 that
  were corridor **hairpins** (heading reverses, path progress is real) → redefined
  as *revisiting a spot on an unchanged goal* → 128.
- **`death_near_health`** is meaningless without MOD classification, because most
  deaths on q2dm3/4/6/7 are lava/slime.

Findings that re-ranked the rest of the plan:
1. **`ran_past_fight` ≈ 0** — 9 incidents on q2dm1, 0 on seven other maps across
   960 bot-minutes. Combat acquisition is not a regret source; this is independent
   support for sequencing the combat phase last.
2. **Environmental deaths are still the majority on four maps with `bot_hazard`
   default-ON** — q2dm6 71%, q2dm3 62%, q2dm4 60%, q2dm7 58% — and **80-94% of
   them are AIRBORNE**, which the ground-gated steer veto structurally cannot see.
   This produced an extra phase (below).

## Phase 1 — joint cvar optimizer — SHIPPED (`tools/optimize_cvars.py`), found real headroom

CEM over 10 dims, 200 evals, holdout on disjoint maps AND seeds. Training
1578→1932 pickups; **holdout +6.8%** — and single-knob ablation attributed
**all** of it to `bot_commit` alone. The CEM's apparent convergence on
`bot_navvalidate`/`bot_slimeescape` was noise: worth ~0 in isolation.
**→ `bot_commit` retuned 0.8 → 6 and SHIPPED: benchmark +9.0% total pickups,
all four columns up (shipped-solo +16.1%), 6/8 maps.**

The curve has **no interior optimum** — it rises monotonically toward the
degenerate cheapest-first limit — so the choice of 6 is a judgement: raw count
rises 16% to the limit while collected *value* rises only ~7%, i.e. average pickup
quality falls the whole way. Checked rather than assumed: the need model is worth
*more* at high commit, so the retune does not drown `bot_wpnneed`/`bot_ammoneed`.

## Phase 1b (inserted, from Phase 0's finding) — `bot_airhazard` — REJECTED

Ballistic arc integration (mirrors `Nav_SeedOnePush`; liquids aren't in
`MASK_PLAYERSOLID`, so the arc is point-sampled for contents). Lava deaths
**−30.6%**, q2dm3 pickups +6.6% / q2dm6 +1.8% — but **q2dm4 −7.5%** and overall
pickups flat (−0.3%). **Finding: dying in lava costs about as much as refusing to
go near it** — respawn is fast and lands you near items; braking at a ledge burns
goal budget. Sub-results: slime must NOT be vetoed (it's an A*-priced wade), and
mid-air salvage is a hard reject (30519 steers bought −5.2%).

## Phase 2 — control timing (`bot_control`) — REJECTED, and it found a dead-code bug

⚠️ **`Item_RespawnEta` gated on `FL_RESPAWN`, which `Touch_Item` CLEARS on pickup**
(there it means "world item, don't free the edict"). It returned 99999 for every
respawning item, so **the entire shipped `goal_timing` / `ITEM_PREEMPT_SECS`
pre-positioning path had never once fired.** A plain A/B reported "inert" and
would have concluded the idea doesn't work; a **telemetry funnel**
(`timing_cand` → `timing_pick` → `timing_wait`) is what distinguished "never picks
a timing goal" from "picks them and arrives late".

With the mechanism working: control items **+5.0-5.4%**, waits **83% paid** — but
total pickups **−1.7 to −2.0%** for flat value. It reallocates collection rather
than adding any. Rejected on the stated criterion.

## Phase 3 — danger heatmap (`bot_danger`) — REJECTED

Heat kept **out of `nav_node_t` and never saved** (format/FROZEN/md5 untouched);
**combat-only deaths**, which makes solo byte-identical with the lever ON (verified
— 1646 pickups at every value). Symmetric read: deaths **not** reduced. Because a
symmetric sim can't show this (everyone avoids hot ground → equilibrium shifts →
deaths ~zero-sum), `bot_dangertest` added the id-parity read: at **24 paired arms**
kill-share **+0.09pt, CI [−1.54, +1.72]**, deaths bias-corrected slightly UP.
An 8-arm read had said −0.90pt and was noise. Likely cause of the null: **heat is
a lagging indicator** and detouring around it costs position.

## Phase 4 — self-playbooks (`tools/mine_selfplay.py`) — REJECTED

First cut anchored takes at the `goal_item` commit — mid-run at ~300ups — which
the executor can never align to: **solo −52.9%**, aborts exceeding engagements.
Re-anchored to a standstill per the measured recipe; still **solo −6.8% / dm
−12.6%**, abort 31-71%, completions *down*.

**Structural finding: self-play can only capture traversals the bot ALREADY
completes, so it unlocks nothing and only adds abort overhead.** Every playbook
that won encodes a maneuver the bot *couldn't* do. The mining output proves it —
with a valid anchor, the hard items (Railgun, Slugs, SSG) yield **zero takes**.

## Phase 5 — enemy belief (`bot_enemymodel`) — REJECTED as a win, kept as a humanness lever

**The plan's premise needed correcting:** the flee gate already reads
`Combat_Strength(ENEMY)` — omnisciently. So the real question was what removing
that costs. Estimator validated first (|error| p50 **7**, 91.3% flee-decision
agreement, biased +10.5 optimistic). A/B at **40 paired arms**: kill-share
**+0.14pt, CI [−1.25, +1.53]**; deaths bias-corrected −2.4pt. CI includes zero →
rejected. **But the null means removing the bot's omniscience costs nothing
measurable — a free humanness option.**

## Phase 6 — learned last-leg controller — NOT BUILT (gate not met)

The plan gates this on "a measurable last-leg residual". Measured, on q2dm1 dm
(1063 item goals): 630 pickups, **173 died mid-route**, **113 gave up mid-route**
(median **345u** from the item, 43% along the path), 84 lost to contention, and
only **~9 (0.8%)** failed within 80u of the item. **Final-approach precision is
not the bottleneck** — route abandonment and death are. Building a last-leg
micro-controller would have optimized 0.8% of failures.

## The through-line

Three phases in a row (2, 3, 1b) produced a **working mechanism with no
north-star movement**, and Phase 5 joined them. The only thing that paid was
re-tuning a constant that was already in the scorer. That is worth weighing
before the next round of new behavior: this bot's remaining headroom looks more
like *mid-route survival and route abandonment* (289 of 433 failures) than like
any new belief tier.

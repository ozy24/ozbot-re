# Plan: humanization — make the bots walk, look, and fight like humans

Status: **proposed** (not started)
Target: the bot's *observable behavior distributions* (gaze, turning, aim texture,
combat rhythm) move measurably toward human demo distributions, under an explicit
strength budget (below). This is a **style** goal, not a strength goal.
Prior art: PLAN.md Phases 16–17, memories `ozbot-demo-import-finding`,
`ozbot-demo-combat-calibration`, `ozbot-lift-win`.

## Why this is not the failed demo experiments again

Both prior demo transfers moved **capability** and failed for the same reason:
pro data bakes in execution skill the bot doesn't have (route import — movement
mismatch; weapon-priority calibration — dead tie). This plan transfers **style**:
distributions of observable behavior (where you look, how fast you turn, when you
jump), applied *within* the bot's own execution limits. Nothing here asks the bot
to strafe-jump.

Mechanical fit: demos record the player at the same 10Hz server framerate as our
telemetry, so bot-vs-human feature comparisons are like-for-like by construction —
whatever differences exist at 10Hz are exactly the observable ones.

**Corpus**: `../demos/sorted/` — 5000+ demos sorted by map (q2dm1 alone has 800+),
protocol 34. `tools/dm2parse.py` already extracts the recorder's trajectory; it
currently *skips* `PS_VIEWANGLES` (exposing view angles is a few-line extension).

## The tells (ranked by how loudly each screams "bot"; from code inspection)

| # | tell | where |
|---|---|---|
| 1 | **360° vision** — enemy acquisition has no field-of-view check; bots react instantly to enemies directly behind them | `Combat_FindEnemy`, bot_combat.c |
| 2 | **View bolted to velocity** — out of combat, facing = move direction exactly, pitch locked 0; never sweeps corners, glances at items, or leads turns | `Bot_Think` (`facing_yaw = b->move_yaw`), bot_main.c |
| 3 | **Instant turns** — outside combat the desired yaw is applied in one 0.1s tick (180° snaps); no mouse dynamics | `Bot_Think` angle write, bot_main.c |
| 4 | **Robotic aim texture** — constant-rate linear tracking + *per-frame white noise* (`crandom()*err`) = 10Hz vibration around the target; human error is autocorrelated (pursuit lag, overshoot on reversals, correction) | `Combat_Aim`, bot_combat.c |
| 5 | **Metronome combat movement** — strafe re-picked uniformly every 0.5–1.1s at constant full speed; jumps only from a 3%/frame dice roll; half-navigates while fighting | `Combat_Aim` blend + dodge, bot_combat.c |
| 6 | **Uniform locomotion** — always exactly full speed, straight node-to-node polylines, pivot corners, statue-stillness while waiting (item timing, lift WAIT) | bot_move.c |

## Acceptance rule (user-approved 2026-07-03: strength MAY be traded for humanness)

- **Humanness metric** (defined by Phase 0) must improve for the feature the change
  targets, measured on the standard rig's telemetry vs the demo corpus.
- **Strength budget**: each individual behavior ≤5% relative pooled-frags loss and
  ≤1.5pt ITEM loss on the standard 5-seed rig; the **full stack ≤10% frags / ≤3pt
  ITEM** vs the pre-humanization baseline. Combat-affecting changes additionally get
  an id-parity read (`bot_aimtest` pattern) since self-play totals hide asymmetries.
- `bot_skill` remains the difficulty lever; humanization must not be a stealth
  difficulty change beyond the budget.

## Phase 0 — the humanness profiler (measure first; NO bot behavior changes)

1. `dm2parse.py`: extract `PS_VIEWANGLES` (and keep origins as today). Derive
   velocity by differencing origins.
2. Telemetry: add view **pitch** to tick records (log field only, no behavior).
3. New `tools/humanness.py`: identical feature extraction from (a) a demo corpus
   (per map) and (b) bot telemetry JSONL:
   - view-vs-velocity yaw offset distribution (ozbot: a spike at 0 out of combat)
   - yaw angular-velocity distribution + autocorrelation (turn dynamics)
   - pitch distribution and pitch activity
   - jump rate, conditioned on moving vs fighting (fighting proxy: high angular
     velocity window / weapon firing where derivable)
   - strafe-reversal interval distribution (lateral velocity sign changes)
   - speed histogram; stillness-episode lengths
   - per-feature statistical distance (KS or Wasserstein) bot↔human = the
     **humanness score**, tracked per feature like ITEM completion
4. Deliverable: a ranked report of the worst tells **with numbers**, which becomes
   the roadmap. If the measured ranking contradicts the table above, follow the
   measurements.

## Phase 1 — gaze layer + turn dynamics (`bot_gaze`, `bot_turnrate`)

- Out-of-combat facing decoupled from movement (the architecture already supports
  this — movement/aim decoupling makes `facing_yaw` a free channel): look ahead
  down the path and *lead* upcoming corners; glance at items/openings passed;
  occasional shoulder checks; pitch follows target/slope instead of locked 0.
- All facing changes slew-limited with an accel/decel envelope sampled from the
  demo yaw-velocity stats — kills the 180°-snap tell everywhere, combat included.
- Mostly cosmetic alone (guardrail should be a wash); it is the *enabler* for
  Phase 3.

## Phase 2 — combat aim texture (`bot_aimtexture`)

- Replace per-frame white noise with an autocorrelated error process (e.g.
  Ornstein–Uhlenbeck: smooth wander around the true aim point), overshoot +
  correction on target direction reversals, reaction delay on direction *changes*
  (today reaction only gates acquisition).
- `bot_skill` scales the process parameters (sigma / correction speed) so the
  difficulty lever survives. Validate strength with id-parity, texture with the
  angular-velocity autocorrelation feature.

## Phase 3 — human vision (`bot_fov`) — the big one, costs strength by design

- Enemy acquisition requires the target inside a ~120° view cone **or** a recent
  damage/pain event (getting shot turns you around — implement the turn-toward-
  attacker reflex with Phase 1's turn dynamics, not a snap).
- Ties directly into the gaze layer: scanning is what acquires targets now, so
  Phases 1+3 must be validated as a pair — FOV without gaze would tank combat far
  past the budget.
- This is where the approved strength trade is expected to be spent.

## Phase 4 — combat movement rhythm (`bot_hop`, dodge rhythm)

- Jump frequency in combat sampled from demo context stats (humans jump a LOT in
  Q2 fights — likely strength-*positive*: airborne targets are harder to hit).
- Strafe-reversal intervals sampled from the demo distribution instead of uniform
  0.5–1.1s; momentum-aware reversals (brief speed dip) instead of instant flips.
- Optional: fight commitment — reduce the nav blend at close range so bots stop
  half-jogging toward items mid-duel.

## Phase 5 — locomotion texture (lowest priority; only if Phase 0 ranks it high)

- Speed variation, corner arcs (steering look-ahead), idle fidget while waiting.
  Measure first; may not be worth code.

## Validation ladder

1. **Per behavior**: humanness feature moves toward human distribution; strength
   guardrail holds (5-seed rig; 13 seeds if borderline; id-parity for combat
   changes). One cvar per behavior, default OFF until validated.
2. **Full stack**: humanness score across all features; total strength within the
   budget; q2dm3/q2dm5/q2dm8 spot check (guardrail only — humanness profiles are
   map-conditioned where the corpus is large enough, else pooled).
3. **Eyeball test**: user spectates (`bot_debug` off) and/or plays; optionally a
   blinded A/B — mixed bot/human demo clips, can the user pick the bot? This is
   the actual goal; the metrics exist to make progress on it measurable.
4. Accepted behaviors flip default ON together as the "humanization stack";
   README/PLAN/KNOWN_ISSUES + memory as usual.

## Out of scope

- Movement *capability* (strafe-jump, bunny-hop, rocket-jump) — different problem,
  known trap.
- Inverse-dynamics trajectory replay of demos — the movement-mismatch trap again.
  **Sample distributions, never replay trajectories.**
- Chat/taunts, name/skin variety (cheap cosmetics, but not this plan).
- Sub-10Hz view smoothness — demos are 10Hz snapshots too; observers see both
  through the same sampling.

## Risks (measure, don't pre-engineer)

- **Pro-flavored corpus**: the archive skews strong duel play; gaze/turn/jump
  dynamics are human-universal, but rhythm stats carry "very good player" flavor.
  Note per-feature; if "average human" texture is wanted later, reweight or filter
  the corpus (per-player stats exist in the demos).
- **Feature interactions**: FOV without gaze, or gaze without turn-rate limits,
  each looks wrong or breaks the budget — validate the pairs named above together.
- **Guardrail vs goal tension**: the budget says how much strength we may spend;
  it does not require spending it. If a humanness feature can't fit the budget,
  it gets a cvar default OFF and a documented negative, per house rules.
- **Overfitting to q2dm1**: profile per map where n is large (q2dm1 800+ demos);
  validate the stack on at least one other corpus-rich map.

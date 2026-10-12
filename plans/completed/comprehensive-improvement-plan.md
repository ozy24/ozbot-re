# ozbot-re comprehensive improvement plan (2026-07-05)

A holistic assessment of the bot's current state and a prioritized roadmap.
Written after a session of measurement + targeted fixes; grounds every item in
data, not priors. Read `KNOWN_ISSUES.md` and the `ozbot-re-pivot` memory first.

## 1. Where the bot is now (measured, q2dm1, 40Hz)

The bot is **mature**. On q2dm1 (the only map with a matured nav — no other q2dm
maps ship in this tree's paks), 8×90-120s fastsim samples give:

- **ITEM completion ~47%** (pickups / item-goal-attempts). This is at/near the
  nav-true ceiling given combat pressure (~46% established earlier).
- **Goal-attempt outcomes:** pickup 47% · **death 25-30%** · **giveup 14%** ·
  **item_lost 10%** · pathfail 0%.
- Every hard item is reachable+picked: Railgun (swim), GL/CG/HB (lift), upper RL
  (rl_walkway playbook), Megahealth (mh_jump playbook).
- Combat aim is now **smooth** (bot_aimsmooth) with a mild look-away texture
  (bot_gazelife); lead/flee/skill/humanization all live.

**The central finding of this assessment:** the three failure modes above are
each hard to move *in a symmetric bot-vs-bot sim*:
- **death (biggest)** is near **zero-sum** — you cannot lower everyone's death
  rate at once (5 bots, someone always dies). Survivability work washes out.
- **item_lost** is largely irreducible contention (5 bots, finite items).
- **giveup** is the one genuinely non-zero-sum nav lever (execution failures).

So further headline-metric gains on q2dm1 are capped by the measurement, not the
bot. **The highest-value unlocks are infrastructure and inputs, not more
q2dm1 tuning** (see §4).

## 2. Delivered this session (all committed)

| Change | Effect | Default |
|---|---|---|
| `Bot_LiftThink` ascent-gate | "stuck above chaingun" 10s freeze gone; lift_timeout −48% | on |
| `cg_descend` playbook (+`dwell`) | the missing way down; CG-stall −24%, ITEM flat | on |
| `rl_walkway` playbook | unlocks the `no_path` upper RL (0→5/seed) | on |
| `bot_aimsmooth` | 40Hz view glide; robotic view-teleports −75%, kills neutral | on |
| `bot_gazelife` | mild between-shot look-away glances; kills neutral | on |
| `bot_reroute` | penalize the stalled hop on pure-nav giveup; giveup −0.5pt | on |
| `bot_survive` | health-urgency + low-hp flee; null in symmetric sim | **off** |
| `bot_aimflick` | turn-cap multiplier; null (cap rarely binds) | 1 (no-op) |
| per-entry playbook `drift`/`dwell` | tolerances for walkway/rescue playbooks | — |

## 3. Prioritized roadmap — autonomously doable now

### P1. Execution-failure reroute — DONE (bot_reroute), extend it
`bot_reroute` prunes the fluke hop on a pure-nav giveup. **Next:** a *mid-attempt*
version is riskier (combat/detour false-positives erode good links) — gate it on
`path_idx` not advancing for N seconds **while `!enemy`** (pure-nav) so it can
reroute before burning the whole budget, not just after. Expected: another
0.5-1pt off giveup. Guard carefully against graph damage (penalize doubles cost,
removes after 3 — a link the bot usually passes won't accumulate).

### P2. Nav-quality pass (map-general, compounding)
The graph carries fluke links learned from lucky falls/shoves. `bot_reroute`
sheds them slowly. **Add a save-time validator:** on autosave, drop links whose
`link_fails` counter is high OR whose geometry is implausible (e.g. a `walk` link
with |dz|>64 and no matching `fall`/`plat` sibling — the 322→251 cost-90 fluke
pattern). One-shot cleanup + ongoing hygiene. Metric: giveup, and same-seed nav
node/link counts. Map-general.

### P3. Failure-knowledge persistence
`bot_itemfail`'s completability counts are in-memory and reset each map load, so
every fresh run re-pays ~1 full-budget giveup per hard item. Persist per-item
completability alongside the `.nav` (a small sidecar or extra section). Metric:
early-run giveup count. Note: changes A/B methodology (adds persistent state) —
gate behind a cvar and document.

### P4. Combat weapon/range tactics (re-examine)
Weapon-priority calibration was a null historically, but *range-appropriate*
weapon choice (rail/rocket at range, SSG up close) was never isolated from
priority. Worth one head-to-head (`bot_aimtest`-style parity) A/B. Low
confidence; cheap to test.

## 4. The real unlocks — need infra or inputs (recommended next)

These are where the bot can actually get **better**, not just re-tuned:

### U1. **Asymmetric skill harness** — DONE (`bot_survivetest`, 2026-07-05)
Built the id-parity head-to-head (even ids run the behavior, odd control) read
against a paired control run that nets out the harness odd-bias, via a general
`Bot_Survives(b)`-style gate. It immediately resolved **bot_survive**: even/odd
death ratio 0.959 → 1.073 = survive bots **die +11.8% MORE**, frags flat. So
survivability is not merely zero-sum on q2dm1 — it's **counterproductive**
(fleeing in tight spaces = dying); bot_survive stays OFF with hard evidence.
**The harness is the keeper** — apply the same gate to any future skill lever
(range-tactics, kiting, positioning) to measure it. Reuse: copy the
`bot_*test` + `Bot_*(b)` parity pattern, run test vs control, read the ratio
shift with `parity_frags.py` or an inline death/frag-by-parity count (use an
**even** bot_count — 6 splits 3:3; 5 splits 3:2 and biases the raw counts).

### U2. Multi-map generality (needs maps + navs)
Only q2dm1 is navved and only q2dm1 ships here. To generalize: source the other
q2dm `.bsp`s, generate navs (run bots to mature each), then re-measure ITEM and
the q2dm1-tuned constants (goal budget, falloff, cooldowns) per map. High value
for robustness; blocked on map availability.

### U3. Human-movement playbooks (needs user captures)
The remaining vertical/precision gaps and the human "look-away 45% = backpedal"
kiting texture both want recorded human input. The pipeline is proven
(record_inputs → make_playbook → A/B). Needs the user in the loop for captures.

### U4. Kiting movement (the humanness gap gazelife couldn't reach)
Human combat look-away is mostly **backpedal** (move away while facing the
enemy), a movement trait. A `bot_kite` that biases the combat dodge toward
retreat-while-firing when at mid-range would close it — but it changes combat
positioning/lethality, so it needs U1 (asymmetric harness) to validate without a
kill regression. Sequence after U1.

## 5. Recommended sequence

1. **U1 (asymmetric harness)** — unblocks everything zero-sum. Build first.
2. Re-validate **bot_survive** and test **U4 (kiting)** on U1.
3. **P2 (nav-quality validator)** — compounding, map-general, safe.
4. **P3 (persistence)** — saves re-paid giveups.
5. **P1 mid-attempt reroute**, **P4 range-tactics** — smaller, cheap.
6. **U2/U3** when maps/captures are available (user or external inputs).

The honest headline: q2dm1 symmetric metrics are near their ceiling; the growth
is in (a) an asymmetric harness that makes skill measurable, (b) map generality,
and (c) recorded human movement. Items P1-P4 are safe incremental hygiene worth
doing; U1 is the lever that reopens real gains.

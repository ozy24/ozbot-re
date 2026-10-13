# ozbot-re general improvement plan (2026-10-12)

Status: **IN PROGRESS** (branch `campaign-2026-10`). This supersedes the two July plans in this folder.
`post-port-improvements.md` and `comprehensive-improvement-plan.md` are delivered or
absorbed below. Move them to `completed/` when this plan starts.

## Diagnosis: where the headroom actually is

Five findings from the record set the direction.

1. **New behaviour levers have stopped paying. Fixes and measurement keep paying.**
   - The last eight behaviour levers were all rejected or left OFF: `bot_pursuit`,
     `bot_hearing`, `bot_combatmove`, `bot_airhazard`, `bot_control`, `bot_danger`,
     `mine_selfplay` and `bot_enemymodel`. Each was a working mechanism that didn't
     move the north-star metric.
   - The only recent win was retuning a constant that already existed (`bot_commit`,
     +9% pickups).
   - Meanwhile, finding things that silently didn't work has been the
     highest-yield work. Examples:
     - `Item_RespawnEta`: `goal_timing` never fired, for the project's whole life.
     - `trigger_teleport` had no spawn function.
     - `bot_losfinal` drove bots into walls.
     - The HyperBlaster ramp was missing its up-link.
     - Found 2026-10-11: humanness had no bot-side data at 40Hz, and the playbook
       ALIGN phase snaps the view.
2. **Item-goal failures are in the middle of the route, not at the item.** On q2dm1
   deathmatch there were 433 failures:

   | Failure | Count |
   |---|---|
   | died mid-route | 173 |
   | gave up mid-route (median 345u from the item, 43% along the path) | 113 |
   | lost to contention | 84 |
   | failed within 80u of the item | ~9 |

   No phase so far has explained *why* a bot gives up mid-route.
3. **The humanization stack has never been measured at 40Hz.** It was tuned at 10Hz,
   and the 40Hz profiler was blind until 2026-10-11. The first 40Hz read, against
   the ladder MVDs:
   - fast yaw reversals: 0.73 per second for the bot vs 0.08 for humans
   - yaw autocorrelation at 0.1s: 0.13 vs 0.43
   - single-tick ~176° snaps on 1.5% of the bot's ticks
4. **Deferred 40Hz combat work was never done.** That's the aim/lead re-sweep (old
   P2a) and the flee thresholds (old P2c). Grenades are not led at all, yet grenades
   are among the most-picked-up items on every map.
5. **The benchmark baseline is stale.** The last snapshot is from 2026-07-22
   (`6c2e9c0`, dirty tree), and the deployed DLL dates from the same day.

**Principle for this campaign: fix, measure, and explain before adding behaviour.**
Every phase ends in a gate. A lever is built only to target a counted failure class,
with funnel telemetry, never "an idea that might help". The methodology invariants in
`CLAUDE.md` still apply: seeds, md5 gate, 40-arm parity, composition checks.

---

## Phase 0: make the loop runnable from ozy1 (prerequisite)

- **0a/0b. Native Linux rig: DONE 2026-10-12** (`ebc6e4d`). It replaces the
  planned winbuild recipe. `./build_linux.sh` builds `engine/q2reproded` and
  `dist/gamex86_64.so` in seconds, and every tool picks the native pair
  (`tools/rig_platform.py`).
  - Same-seed native runs are bit-identical: q2dm1 16×90 s700 md5 `6e15918d`.
  - Behaviour matches the Windows DLL within noise.
  - An engine bug blocked the GCC build. It is fixed on q2repro `ozbot-re`
    (`4c168775`).
  - winbuild is still how the playable Windows client gets built.
- **0c. Fresh baseline.** Build HEAD, deploy it, and run a `benchmark.py` snapshot
  (note "2026-10 campaign baseline"). Record the same-seed q2dm1 md5. Everything
  below is measured against this snapshot, not July's.
- **0d. Track humanness like pickups.** Add a 40Hz humanness row to the benchmark: a
  q2dm1 deathmatch log vs `human_mvd_all.json`, with mean KS, microflips per second
  and snap %. Then a humanness regression shows up in `STATS.md` like a pickup
  regression.

**Gate:** a DLL built from a commit on ozy1 reproduces the expected md5, and `STATS.md`
has a fresh snapshot that includes humanness.

## Phase 1: correctness sweep (cheap, near-certain value)

- **1a. Lever liveness audit.** For every **default-ON** cvar, count per map and per
  bot-minute how often its code path actually changes a decision.
  - Most levers already emit events; add a single `bot_leverlog` (off-state
    byte-identical) for the rest.
  - Tool: `tools/lever_audit.py`.
  - Any default-ON lever reading ~0 on every map gets investigated as a possible
    dead path. The `Item_RespawnEta` precedent says this is where hidden wins sit.
- **1b. Playbook ALIGN snaps.**
  - First trace one align attempt tick by tick, to confirm the flip between
    `vectoyaw(start node)` and `anchor_yaw` across `pos_tol` (`bot_playback.c`,
    standing-start branch).
  - Then slew the facing during PB_ALIGN only. REPLAY must stay exact.
  - Gate: snaps fall from 1.5% to ≤0.1% of ticks, Megahealth and upper-RL pickups
    don't fall, and engage/done/abort rates are flat or better (16 seeds).
- **1c. 40Hz humanness re-profile of the full stack** (the old P3c, now possible).
  - For each feature that drifted from the MVD 40Hz reference, find the per-tick
    term responsible. Suspects: OU aim texture, gaze fixation, glance timing.
  - Retune only those features, within the standing acceptance rule (strength may
    be traded for humanness, ≤10%).

**Gate:** every default-ON lever is shown live, or is fixed or removed. The snap
artifact is gone, and the 40Hz humanness mean KS improves without a pickup regression.

## Phase 2: explain and cut mid-route failures (the measured headroom: 286 of 433)

- **2a. Giveup taxonomy.** Give every `goal_end` a **reason**:
  - budget expiry, with the route estimate vs time actually spent
  - stall (`bot_reroutemid` already spent)
  - path invalidated
  - re-ranked to a better item
  - enemy appeared
  - stuck

  Run it on q2dm1/2/8 deathmatch and solo.
  - **Hypothesis to test:** most of the 113 are budget timeouts on routes slower
    than `BOT_GOAL_BUDGET_BASE/SPEED` expects. Those constants were tuned on q2dm1
    and assume distance, not A* cost.
  - Only then pick the lever. If it is budget expiry, the natural one is a budget
    funded from A* cost-to-go and extended on real path progress.
- **2b. Mid-route deaths.**
  - Split the 173 into environmental vs combat, using the `bot_hazlog` classes that
    `mine_regret` already reads.
  - For the combat deaths, also split by:
    - was the bot engaged or hit from behind?
    - how far from the item was it?
    - how much health and armor did it have?
  - Since `ran_past_fight` ≈ 0, acquisition isn't the issue. The candidate levers,
    each behind a funnel:
    - **finish-the-grab:** keep the goal through a fight when the item is near and
      is health or armor
    - **40Hz flee-threshold re-sweep** (the old P2c)
    - a short **retreat-to-route** after a won fight, instead of a fresh goal pick
  - Health-seeking stays off unless it's paired with a reachable-recovery gate. It
    has been asymmetric-negative twice.
- **Gate per lever:** pickups up on the benchmark (shipped-dm and cold-dm), and the
  targeted failure count down. Combat-touching levers also need parity kill share
  flat or better at ≥40 paired arms.

## Phase 3: combat at 40Hz

- **3a. Grenade lead.** Grenades currently aren't led. Add a ballistic solve for the
  GL, reusing the parabola integration from `Nav_SeedOnePush`. A/B on parity at 40
  arms, plus GL accuracy from `bot_aimlog`.
- **3b. Aim/lead re-sweep at 40Hz** (the old P2a): `bot_aimtest`/`bot_leadtest` at
  `sv_fps 40`. Keep only paired wins. Aim cvars stay out of `optimize_cvars`.
- **3c. Engaged stillness.** Planting to shoot is 46% of the bot's still frames, and
  humans keep moving. Add a small move-while-engaged step **only when the bot has no
  travel goal**. This is distinct from the rejected committed circle-strafe. It's a
  humanness lever first: measure stillness % and parity.

**Gate:** the parity CI excludes zero for a strength lever. For a humanness lever, the
humanness gain must come at ≤10% strength cost.

## Phase 4: beyond q2dm1

- **4a. Gated-item to-do list per map.**
  - Run `gen_nav` reports for the gated items on q2dm2–8. Re-measure q2dm2's
    vertical items with `bot_lift`, which has never been done.
  - Playbooks are the only lever that has reliably *created* pickups. They encode
    moves the bot can't do itself. The output here is a short list of specific
    takes for Chris to record.
- **4b. Human-vs-bot coverage diff from the ladder MVDs** (q2dm1 ×4, q2dm3 ×2,
  q2dm2 ×1).
  - Rasterize the floor cells the human duellists occupied vs the cells the bot's
    nav reaches.
  - Spots humans use and the bot never does become candidate missing links or
    playbook sites. This needs only `mvd2parse.py` and the nav files.
- **4c. Per-map constants check.** Use `optimize_cvars --eval-only` sweeps of the
  goal-budget constants on held-out maps, since the constants were tuned on q2dm1.
- Deliberately deprioritized: q2dm6's lava K/D. `bot_airhazard` showed that dying in
  lava costs about the same as avoiding it.

**Gate:** at least one new item per non-q2dm1 map becomes collectable, without a
pooled regression.

## Phase 5 (optional, outside this repo): usercmds in the ladder MVDs

Record usercmds in Q2RED's MVD recorder as a new optional record type with a flag bit,
so existing readers keep working. That gives input-level human data: the key cadence of
strafe-jumps, and mouse texture finer than 40Hz. It also enables ladder-side
forensics. It won't make bot mirroring exact, because the physics differ. Separate
repo; Chris's call.

---

## Sequencing

0 → 1 → 2 → (3, 4 in parallel) → 5 optional.
- Phase 1 comes first because it's cheap and history says it's where the hidden wins
  are.
- Phase 2 comes before more combat work because 286 of 433 failures are mid-route.
  Each lever there is chosen from a counted reason, never ahead of one.

## Open questions for Chris

- **Line endings.** Nearly every tracked file shows as modified purely because of
  CRLF↔LF. Normalize with a `.gitattributes` commit before this campaign, or leave
  it?
- **Captures for 4a.** Are you willing to record targeted playbook takes once the
  per-map list exists?

---

# RESULTS (in progress, branch `campaign-2026-10`)

## Phase 0: DONE
- **Native Linux rig** (`ebc6e4d`, `4c168775` on q2repro): about 2–4 s per 16×90 run.
  Same-seed runs are bit-identical. Results match the Windows DLL within noise on all 8
  maps (the baseline snapshot matches July's: shipped-solo mean 71% vs 72%).
- **Fresh baseline:** `a3ee586`.
- **Humanness row:** mean KS 0.166, micro-flips 0.89/s (human 0.085), snaps 1.89%.

## Phase 1: done except 1c
- **1a lever audit** (`tools/lever_audit.py`, 8 maps × 42 levers, same-seed md5
  probe): **no dead paths.**
  - Every inert reading is explained by the map.
  - `bot_aimerr`/`aimfire`/`aimreact`/`aimturn` are inert by design: they are
    `bot_aimtest` multipliers.
- **1b `bot_pbalign`: built and diagnosed, left OFF (opt-in).**
  - **Finding:** the playbook ALIGN phase limit-cycles on the `pos_tol` boundary. 20%
    of align ticks are >90° snaps, and **37% of q2dm1 giveups follow a playbook
    abort** (`tools/mine_giveups.py`).
  - **v1:** broke wall-jammed anchors (engages 140→72).
  - **v2:** solo −23% on the pinned nav. Its view slew broke moving-start run-ups.
  - **v3, 6 banks × 2 navs:** deathmatch **+4.1% / +10.2%** (11/12 banks up), align
    aborts 179→7, snaps →0.2%. But **solo −10.6% on the pinned nav** (0/6 up), so the
    headline regresses.
  - **The open question for v3 is solo around the GL/lift area.** That's where the
    loss sits (Grenades −27, GL −16).
- **1c 40Hz humanness re-profile:** TODO. Note that with `bot_pbalign=1`, micro-flips
  fall 0.89→0.27/s, so **most of the bot's 40Hz view jitter IS the ALIGN bug.** Fixing
  1b properly is also the biggest humanness lever.

## Phase 2: 2a done, 2b TODO
- **2a giveup taxonomy, q2dm1:** pb_abort 37%, slow 32%, stalled 25%, at_node 4%,
  fight 2%.
- **`bot_budgetext`: REJECTED.**
  - The idea: extend an expired budget while the bot is still progressing.
  - Giveups fell 18–36%, but pickups fell 2–7% on every map and rig.
  - Extended attempts end at the goal node with the item gone (at_node 69→197).
  - Same lesson as `bot_commit`: drop a slow item and take a closer one. **Giveups
    are not the cost; the time is.** The 15 s cap stands.

## Phase 4 (ladder pool, added at Chris's request): baseline DONE
- **`tools/ladder_pool.py` + `POOL.md`:** all 17 ranked-duel pool maps load and play.
- **Long-tail navs:** the 14 maps with `gen_nav` navs (FROZEN, `baselines/nav_pool/`)
  reach **78–90% solo COLLECT**. That's at or above the curated q2dm1–3 navs.
- **The gap is reachability, not execution:**
  - mm-fleshref: 22/33 items no-path
  - q64/dm4: 13 no-path
  - mm-rage: 17 gated
- **Fixed on the way:** maps in subfolders (`q64/dm3`) silently lost nav saves and
  telemetry. `Bot_MapFileName` now flattens them to `q64_dm3`.

---

# FINAL (2026-10-13): every phase implemented and measured

## Net effect vs the campaign baseline (`a3ee586` → `0d1f8db`, benchmark 16×90, seed 700)

| | baseline → shipped |
|---|---|
| shipped-solo pickups (headline) | 991 → 1056 (**+6.6%**) |
| cold-solo pickups | 785 → 858 (**+9.3%**) |
| shipped-dm / cold-dm pickups | +0.2% / −1.0% (flat) |
| humanness mean KS vs MVD humans | 0.166 → **0.152** |
| yaw micro-flips per second (human 0.085) | 0.886 → **0.301** |
| single-tick >90° snaps | 1.89% → **0.39%** |

The skill-parity mean shift went 19.1 → 14.4pt. That table measures how much
`bot_skill` separates bots, not overall strength, and it's a single seed bank.

## What shipped (3)
1. **Curated q2dm1 playbook file:** three dead entries dropped. Deathmatch +9.1%
   (6/6 banks).
2. **`bot_gazefix 2`:** 40Hz humanness re-profile.
3. **`bot_budgetcap 15 → 12`:** 8-map sweep, solo +2.2% (22/32 map-banks up).

## Phase by phase
- **0:** native Linux rig, fresh baseline, humanness row. DONE.
- **1a:** lever audit, no dead paths. DONE.
- **1b:** `bot_pbalign` bitmask, OFF. The real fix turned out to be playbook
  curation (shipped).
- **1c:** `bot_gazefix 2` SHIPPED; `bot_gazedead` rejected.
- **2a:** giveup taxonomy. DONE.
- **2b:** death taxonomy, done. Combat deaths equal frags in symmetric self-play,
  so they're a strength question. Flee re-sweep: flat; the false positive didn't
  replicate. `bot_budgetext` and `bot_pbfailfast` rejected.
- **3a:** grenade lead. The bot never fired the GL or BFG at all.
  - `bot_wpnall` built: borderline strength gain, −1.3% pickups, OFF.
  - `bot_glead` rejected.
- **3b:** aim timing and lead re-sweep: flat. The 10Hz values hold.
- **3c:** engaged stillness re-measured at **1%** of still frames. The July premise
  is obsolete, so no lever was needed.
- **4a:** `GATED.md`, after fixing the reach sweep's first-spawn bias.
- **4b:** `COVERAGE.md`. Humans spend 91–100% of grounded time within 128u of a nav
  node.
- **4c:** `bot_budgetcap` per-map sweep, 12 shipped.
- **Pool:** the `ladder_pool` baseline and the shipped-set snapshot are in `POOL.md`.

## Open for Chris
- **Recording to-do lists:** `GATED.md` (q2dm6 25, q2dm4 21, q2dm5 13, q2dm8 12
  unreachable items) and `COVERAGE.md` (human-only spots).
- **q2dm3 `mh_ladder`:** 0 completions in 51 aligns. Its anchor needs a fresh
  capture.
- **Opt-ins worth a decision:** `bot_pbalign 2` (deathmatch +5.1%, solo −7.7%) and
  `bot_wpnall 1` (stronger, −1.3% pickups).


---

# ADDENDUM (2026-10-13, after review): remaining gates closed

- **Phase 1b gate MET: `bot_pbalign 24`, default-ON.** These are view-only fixes:
  - **bit 8:** slew the view during ALIGN, but evaluate the engage gate against the
    *commanded* heading, so engage timing is unchanged.
  - **bit 16:** replay wedge recovery keeps the recorded view and steers back onto
    the rail with forward/side input.

  Results, q2dm1:
  - Snaps 0.41% → **0.07%** in deathmatch and 0.80% → **0.03%** solo (gate ≤ 0.1%).
  - Pickups: deathmatch −0.5%, solo +1.7% (noise). q2dm3: deathmatch +1.0%,
    solo +0.4%.
  - Humanness KS 0.152 → 0.146.
- **Phase 4c done:** `bot_budgetbase`/`bot_budgetspeed` are exposed and swept on 8
  maps (base 3/9, speed 70/140). Flat, ±1%; the shipped 12 s cap binds. Defaults
  stay.
- **Phase 4a, automated half:**
  - `GATED.md` now uses end-of-run reach verdicts.
  - `gen_nav`-grown navs for q2dm4/5/6/8 make new items routable (q2dm4 BFG10K,
    q2dm5 Chaingun, q2dm8 Cells/Chaingun) but cost deathmatch pickups on all four
    (−1% to −8%), so they were not adopted.
  - The gate's other half, recorded playbooks for the no_path items, needs human
    captures.
- **Phase 5:** optional and outside this repo (the Q2RED recorder). Not done.

## Final shipped set vs campaign baseline (`a3ee586`)

| | |
|---|---|
| shipped-solo | **+7.7%** |
| cold-solo | **+9.8%** |
| shipped-dm / cold-dm | +1.2% / −1.0% |
| humanness KS | 0.166 → **0.146** |
| micro-flips per second | 0.89 → **0.24** (human 0.085) |
| snaps | 1.89% → **0.07%** |

Shipped levers: curated q2dm1 playbook file, `bot_gazefix 2`,
`bot_budgetcap 12`, `bot_pbalign 24`.


# ADDENDUM 2 (2026-10-13): Phase 4a via nav grafts, and the slot bug

- **Phase 4a without recordings: `tools/nav_graft.py`.** It copies only a donor
  nav's route to each item the base nav can't reach, under the DLL reach mask.
  Every graft was DLL-verified and A/B'd over 6 banks.
  - **Shipped:**
    - q2dm4: 5 items newly routable; deathmatch +2.3%, solo +0.2%.
    - q2dm5: 3 items; deathmatch +2.3%, solo +3.2%.
    - q2dm7: 3 items incl. the Rocket Launcher; deathmatch +1.2%, solo −1.2%.
    - q2dm8: 7 items; deathmatch +3.4%, solo +2.3%.
    - q64/dm8: 2 items.
    - mm-fleshref: 1 item; duel +4.3%.
    - kaktus-re-b2: Rocket Launcher + rockets; duel +2.2%, solo +1.9%.
  - **Held back:** mm-ptrip (solo −3.4%) and the first q2dm5 graft (solo −8.2%).
  - **Remaining stock maps:** q2dm2 has nothing unrouted; q2dm3/q2dm6 are behind
    lava, by design.
- **`bot_slottop` (default 1), found while watching a live server:** bots took
  client slots bottom-up, so every human joining a dedicated server took over a
  bot's slot. Bots now allocate top-down, and a client landing in a bot slot
  evicts it cleanly. Byte-identical at 0; neutral on the bot-only rig.
- **Phase 5 not done:** it lives in the Q2RED repo, behind PR rules and a live
  server deploy, so it's Chris's call. The proposal stands: an optional usercmd
  record type plus a header flag; existing readers skip unknown record types.

## Final shipped set vs campaign baseline

| | |
|---|---|
| shipped-solo | **+7.2%** |
| cold-solo | **+9.8%** |
| shipped-dm | **+2.8%** |
| cold-dm | **+1.7%** |
| humanness KS | 0.166 → 0.146–0.156 (seed-dependent) |
| snaps | 1.89% → **0.08%** |

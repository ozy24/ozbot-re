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
See the `ozbot-re-resource-need-win` memory for the A/B results.

`bot_commit` (default **0.8**, ON) is the goal scorer's travel-cost commitment
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

# Plan: nav taxonomy, capability-masked A*, and the reachability oracle

Status: **PROPOSED** (2026-07-03). Prior art: q2repro's re-release nav API
(`q2repro/inc/shared/game.h` — `PathRequest`/`PathInfo`/`PathFlags`,
`inc/server/nav.h` — link taxonomy, `src/server/nav.c` — 1.5k-line reference
implementation with pluggable weight/accessibility callbacks).

**The flip:** the re-release engine's `GetPathToGoal` is valuable for two
separable reasons — (1) authored ground-truth nav data, (2) a clean
*architecture*: typed traversal links, capability flags that filter pathing,
principled return codes, and a follower contract ("next segment is an Elevator
traversal → engage that controller"). Reason 2 needs no engine, no authored
data, and no x64: it ports straight onto our self-learned graph. Reason 1 we
use only offline, as a grading tool the bot never reads — the
no-prebuilt-nav identity stays intact.

Why this is worth doing (grounded in our own findings, not theory):

- **A known wart gets fixed outright.** Nav files saved by lift-ON runs
  persist PLAT costs and contaminate lift-OFF A/Bs (`ozbot-lift-win` memory
  caveat). Capability-masked A* makes a lift-OFF bot *route around* PLAT
  links instead of pricing/attempting them — the graph becomes one shared
  artifact, interpretation happens at path time.
- **Every locomotion capability so far converged on the same shape** (typed
  link + controller that owns movement intent: swim, lift, strafejump).
  Today only PLAT is link-typed and it's special-cased at three sites in
  `bot_move.c` (455/459/560). A dispatch contract makes the next capability
  a table entry, and is the same interface a future q2repro port would use.
- **Geometric mysteries stop needing the user.** Railgun (swim-gated) and
  Megahealth (unreachable) each burned sessions before map knowledge cracked
  them. An oracle that re-runs A* under different capability masks labels
  gated items automatically, on any map, at load time.

Constraint honored throughout: **no .nav layout change** — new link types are
new values of the existing type byte; `NAV_VERSION` stays 1; the matured
canonical `q2dm1.nav` keeps loading. Nothing here adds nodes (the >330-node
maturation regression, `ozbot-nav-maturation-finding`, stays untouched).

Verification standard, per project convention:
- *Refactor phases* (no intended behavior change) prove it with **same-seed
  bit-exact telemetry**: fastsim is deterministic per seed, so
  `run_parallel.bat --fastsim --seed N` before/after must produce
  byte-identical merged logs (modulo timestamps in filenames).
- *Behavior phases* land behind a default-OFF cvar, flip only after the
  standard A/B: 8 seeds × 90s, pooled ITEM% / pickups / giveups, plus a
  q2dm3/q2dm5/q2dm8 spot-check whenever graph semantics change.

---

## Phase A — capability-masked A* (`bot_navmask`) — smallest, do first

> **RESULT (2026-07-03): mechanism landed, default stays OFF.** Bit-exact
> gates passed (mask off, and mask on with all capabilities on, both md5-equal
> to baseline). But the runtime value case FAILED its 8-seed A/B: with
> `bot_swim 0`, masking water routes cost 235→190 pickups / ITEM 54→48%
> (bots still paddle through water via raw pmove and collect the Railgun
> opportunistically — deliberate 3D steering is what bot_swim adds, water is
> not *untraversable* without it); with `bot_lift 0` the mask was a wash
> (ITEM 47→47%). **Capability-off ≠ link-impossible: traversal is
> probabilistic, not binary**, so hard-masking runtime routing removes real
> (degraded) pickup opportunities along with the doomed attempts. Two
> findings for the record: (1) the learner NEVER stamps `NAV_LINK_WATER` —
> water-ness lives on node flags, so the WATER mask bit excludes links into
> `NAV_FLAG_WATER` nodes instead; (2) the mask stays valuable as Phase C
> oracle infrastructure, where it is used *hypothetically* (classify what
> unlocks an item) rather than to steer live routing.

The `PathFlags` idea: A* may only expand links whose type the querying bot
can execute.

- `bot_nav.h`: `#define NAV_MASK_ALL 0xff` etc.; mask bit = `1 << link_type`.
- `bot_nav.c`: `int Nav_FindPathMasked (int start, int goal, int mask, int *out, int max)`
  — identical to `Nav_FindPath` plus one `if (!(mask & (1 << l->type))) continue;`
  in the neighbor loop. `Nav_FindPath` becomes the `NAV_MASK_ALL` wrapper so
  no callsite churn.
- `Bot_NavMask (bot_t *b)` in bot_main.c derives the mask from capability
  cvars: `bot_swim 0` drops WATER, `bot_lift 0` drops PLAT. WALK/FALL/JUMP/
  TELEPORT always allowed (core locomotion). Per-bot so a future
  `bot_skilltest`-style capability-parity harness works for free.
- All pathing callsites that act on behalf of a bot (goal selection's
  reachability probe in `Goal_Select`, the three replans in bot_main.c) pass
  the bot's mask, gated on `bot_navmask` (default 0 until proven).

Validation:
1. Default config (all capabilities ON → mask = ALL): **bit-exact** vs
   baseline, same seed. This must hold by construction.
2. The value case: `--cvar bot_lift 0 --cvar bot_navmask 1` vs
   `--cvar bot_lift 0` on the canonical (lift-matured) graph — expect
   giveups on GL/CG to drop (bots stop committing budget to routes they
   can't ride) with ITEM% ≥ neutral. Same shape for `bot_swim 0` / Railgun.
3. Standard 8-seed default-config A/B: expect exact neutrality.

~60 lines. Flip `bot_navmask` default ON when (2) shows the expected effect
and (3) is clean — from then on capability A/Bs stop needing pristine
per-config nav copies.

## Phase B — the traversal contract (follower dispatch on link type)

> **RESULT (2026-07-03): landed smaller than planned, bit-exact.** The
> premise "PLAT is special-cased at three scattered sites" was wrong on
> closer read: all three live *inside* the lift controller, and the
> controller-ownership convention (Bot_LiftThink/Bot_StrafeThink return true
> while owning the frame) already IS the dispatch contract. What was
> genuinely missing was the reusable piece: `Lift_UpcomingPlatHop` was the
> generic typed-hop query with PLAT hardcoded. It is now `Bot_UpcomingHop
> (b, type, engage, release, engaged)` in bot.h — the query any future
> typed-link controller engages from. A dispatch table over two controllers
> (one of them runway-based, not link-typed) would have been speculative
> structure; skipped. The `bot_navlearn2` teleport-stamping rider is also
> skipped: unverifiable on q2dm1 (no teleporters) and nothing consumes it yet.

The `PathInfo.pathLinkType` idea: the follower always knows the type of the
upcoming path segment and hands frame ownership to that type's controller,
instead of each capability sniffing the path independently.

- `Bot_NextLinkType (bot_t *b)` — type of the link `path[path_idx-1] →
  path[path_idx]` (and a lookahead variant for the `path_idx+1` engage case
  the lift code needs).
- One dispatch site in the follower replaces the three ad-hoc PLAT checks at
  bot_move.c:455/459/560: PLAT → `Bot_LiftThink` engage window, WATER →
  swim steering context, default → walk/strafejump-eligible. The controller
  ownership convention (`returns true while it owns movement intent`)
  already exists — this phase routes *entry* through one table instead of
  scattered special cases.
- Strafejump stays runway-qualified (trace-based), not link-typed — Phase 19
  proved that design; no change. Its qualifier already consumes link types
  (`SJ_DIAG_LINK` stops at non-WALK links) and keeps working unmodified.
- **No intended behavior change → bit-exact same-seed verification**, plus a
  5-seed sanity run. Deliverable is negative diff: fewer special cases,
  same bytes.

Optional rider, only if bit-exactness is achieved first — richer learn-time
stamping behind `bot_navlearn2` (default 0): record TELEPORT links when the
position discontinuity in `Nav_LearnStep` matches a `trigger_teleport`
traversal instead of silently dropping it (today the 200u adjacency check
discards teleports — irrelevant on q2dm1, real on teleporter maps). Any
stamping change writes to nav files → same A/B-hygiene rule that governed
PLAT tagging: gated, and worker gamedirs already isolate copies.

## Phase C — the oracle: principled reachability + return codes

The `PathReturnCode` idea, pointed at our biggest diagnostic gap: today
"unreachable" is inferred from giveup statistics after budget is burned.

- `bot_nav.c`: `int Nav_QueryPath (vec3_t from, vec3_t to, int mask, float *cost)`
  returning `NAVQ_OK / NAVQ_NO_START_NODE / NAVQ_NO_GOAL_NODE /
  NAVQ_NO_PATH / NAVQ_GATED`. `NAVQ_GATED` = fails with the given mask,
  succeeds with `NAV_MASK_ALL`; a second pass re-adds one masked type at a
  time to name the *minimal unlocking capability* (bounded: ≤ 6 A* runs, and
  it only runs in the consumers below, never in the per-frame hot path).
- Consumers, cheapest first:
  1. **Map-load reachability sweep** (`bot_reachlog`, default 1 — pure
     diagnostics): after `Goal_SeedNavNodes`, one JSONL record + console
     line per item: `{item, reachable, gated_by, cost}` from a central
     spawn-node sample. On q2dm1 with the canonical graph this must
     reproduce, with zero tuning, what took us sessions to learn: Railgun
     WATER-gated, GL/CG PLAT-gated, Megahealth NO_PATH. That's the
     acceptance test. `analyze.py` gains a "map reachability" header block.
  2. **Giveup enrichment**: `Bot_LogGiveup` gains the query code at giveup
     time — separates "route evaporated" (penalization ate a link) from
     "route existed, execution failed" in failure mining.
  3. **`sv nav_query <classname|entnum>`**: prints the verdict per
     capability mask and draws the found path with the existing `bot_debug`
     beam plumbing — the `nav_debug 1` flip, for interactive sessions.
  4. **Smarter itemfail** (`bot_itemfail 2`, the only behavior change here,
     own A/B): NO_PATH items get the long-blacklist-and-let-explore-fix-it
     treatment immediately (no budget burned proving it 4 times); GATED
     items are skipped outright while the capability config stands; only
     "route exists, execution failed" keeps the escalating 20/40/80/160s
     ladder. Expect: wasted attempt time ↓ (mine with `tools/mine_goals.py`),
     pickups ≥ neutral. If the A/B is a wash, keep mode 2 as diagnostic
     truth but leave default at 1 — consumers 1–3 justify the phase alone.

## Phase D — offline grader vs re-release ground truth (tools only, anytime)

Layer-0 flip: use authored nav *to grade the learner*, never to drive the bot.

- `tools/renav.py` (stdlib): parser for the remaster `.nav` format, written
  against q2repro's loader in `src/server/nav.c` as the format reference.
- `tools/nav_grade.py`: diff re-release ground truth vs our learned
  `<map>.nav` — (a) items ground truth reaches via Elevator/LongJump/etc.
  that our sweep calls NO_PATH (candidate future capabilities, ranked by
  item value), (b) ground-truth links absent near our giveup hotspots,
  (c) node-coverage ratio per map.
- **Prerequisite / go-check:** remaster nav data ships inside the rerelease's
  `Q2Game.kpf` (a zip; q2repro reads it from a Steam/GoG install). If we
  don't have the rerelease, skip the phase — nothing downstream depends on
  it, and q2repro itself with `nav_debug 1` already works as a free
  visualizer for eyeballing.

## Phase E — self-generated ground truth (DEFERRED; explicit go/no-go)

The ambitious flip: an offline pmove flood-fill from spawn points generating
our *own* reachability layer (self-generated ≠ pre-authored; identity
preserved). **Do not build speculatively.** Go only if C+D show the learned
graph's "not learned yet vs unreachable" ambiguity is costing measurable
ITEM% that explore-time can't recover — that evidence doesn't exist yet, and
the build cost (a mini nav compiler, simulating plat rides offline) is the
largest in this plan.

---

## Order, effort, success criteria

| Phase | effort | gate to next | success looks like |
|---|---|---|---|
| A `bot_navmask` | ~half session | bit-exact default; gated-config giveups ↓ | capability A/Bs stop needing nav-copy hygiene |
| B contract | ~half–1 session | bit-exact | 3 special-case sites become 1 dispatch |
| C oracle | ~1 session | sweep reproduces known q2dm1 truths untouched | new maps self-report their gated items on first load |
| D grader | ~1 session | needs rerelease data (else skip) | ranked capability-gap list feeding future phases |
| E flood-fill | large | evidence from C+D | — |

A→B→C is the recommended contiguous arc (each lands value alone; C reuses
A's masks). D is parallelizable whenever the data question is settled. Every
default flip follows its A/B, never precedes it.

# ozbot on q2red

This directory builds ozbot-re's brain (`src/bot_*.c`, unchanged) as a **q2red bot brain**:
`dist/ozbot_x64.so`, loaded by the q2red dedicated server. Bots then play on any
Quake II Rerelease game module the server runs: id's stock game, MuffMode or MMREX. No
module needs changing.

```bash
./build_q2red.sh                                 # -> dist/ozbot_x64.so
./build_q2red.sh install ~/q2red-server/baseq2   # ...and copy it + botnames.txt there
```

Copy the navs you want to ship into `<gamedir>/nav/` (for example from
`baselines/nav_shipped/`, renamed to lower case) and playbooks into `<gamedir>/playbooks/`.
Paths are relative to the server's working directory, as on q2repro. Then start the
server:

```bash
./q2server +set dedicated 1 +set deathmatch 1 +set sv_bot_brain ozbot +set bot_count 4 +map q2dm1
```

## Controlling the bots

| Setting | What it does |
|---|---|
| `sv_bot_brain ozbot` | q2red loads `ozbot_x64.so` from the game directory, then the base directory, at the next map. Empty (the default) means no bots. |
| `bot_count N` | A fixed population. |
| `bot_minplayers N` | **q2red only.** Fill mode: humans plus bots are kept at N, and bots leave as people join (one slot is always kept free). Overrides `bot_count`; 0 is off. |
| `addbot` / `kickbot` | The server's own commands. They raise or lower `bot_count` by one. |
| `bot_add N` / `bot_remove N` / `bot_clear` / `nav_query <item>` | ozbot's commands. On q2repro they were `sv bot_add` and so on. |
| a server-side `kick <slot>` | **q2red only.** Removing one of ozbot's bots this way (an operator, or a mod that picks its own bots, such as Rocket Arena 2's `ra2_bots`) lowers `bot_count` by one, so the bot isn't replaced. A mod that owns the numbers runs with `bot_count 0` and `bot_minplayers 0` and uses `addbot` and `kick`. In fill mode `bot_minplayers` still decides. |
| `bot_telemetry 1` | **q2red only.** Turns on the per-tick JSONL telemetry in `<gamedir>/logs/`. It is off by default because a q2red server runs unattended and the logs grow without limit. Every other cvar keeps its q2repro default. |

## How it fits together

On q2repro the bot lives inside a 3.19 game DLL. It creates fake clients and reads that
DLL's edicts directly. On q2red the game is a rerelease module this code never sees.

**The server's side.** q2red admits each bot as a real client, the way retail's engine
admits its own bots: `ClientConnect(..., "me_a_bot", is_bot = true)`. Every server frame
it asks the brain for each bot's usercmd and hands that to the game's `ClientThink`. The
contract is `q2red_bot_brain.h`, a pinned copy of q2red's `src/include/q2red_bot_brain.h`.
Re-copy it whenever q2red bumps `Q2RED_BOT_BRAIN_API_VERSION`.

**Our side.** `oz_host.c` rebuilds, each frame, the 3.19 world the brain was written
against:

- **Entities.** A mirror `g_edicts[]` / `game.clients[]`, filled from the server's
  entity snapshots. Every rerelease module publishes health, items, inventory, weapon,
  plat state and so on in `sv_entity_t` for engine bots.
- **Items.** `itemlist[]` is matched by pickup name and mapped to the running mod's own
  item ids (`Bot_GetItemID`), so inventories translate in both directions.
- **Unpublished keys.** `target`, `movedir`, `speed` and `dmg` come from the map's
  entity string, which feeds the teleporter, jump-pad, train and hurt-volume seeding.
- **Imports.** `gi` runs over the server's traces and the **game module's own Pmove**,
  so the brain's movement predictions are the real rules.

The outputs go back the other way:

- **Commands.** 3.19 shorts become rerelease floats. `upmove` becomes the jump or crouch
  button.
- **Weapons.** `client->newweapon` becomes the game's `Bot_SetWeapon`.
- **Perception.** The 3.19 game hooks the brain relied on (`player_pain`, `player_die`,
  `PlayerNoise`) don't exist here. They are rebuilt from what the server can see:
  - gunfire from the attack button the game publishes;
  - pain and death from health drops;
  - footsteps from entity events;
  - pickups from item-channel sounds.

**What changed in the shared code.** The few hosted-only lines in `bot_main.c` and
`bot_log.c` are under `#ifdef OZBOT_Q2RED`. They cover:

- the server choosing the slot;
- bot identity coming from the server rather than the netname;
- bots kept across `gamemap`, since the server keeps them connected;
- telemetry being opt-in.

Without the macro the q2repro build is unchanged. This was checked on 2026-10-17:
every object file compiled from `src/` is byte-identical with and without the q2red
work.

## Measured (2026-10-17, q2dm1, 5 bots, 90 s at 40 Hz, `bot_seed 7`)

| Module | Pickups | Item goals completed | Deaths |
|---|---|---|---|
| id's rerelease | 55 | 70% | 10 |
| MuffMode 0.60 | 42 | 58% | 10 |
| MMREX 0.85.15 | 45 | 64% | 12 |

Cost: about 5% of one core and 26 MB for 5 bots, including the game simulating five
more players. These are rerelease rules, not q2repro's 3.19 game code, so the numbers
are a new epoch. They are not comparable with `STATS.md`.

## Navs for a map pool (`tools/q2red_navs.py`)

`py tools/q2red_navs.py --rig <server dir> --maps pool.txt -j 6` bakes a FROZEN nav for every
map in a pool on q2red itself, with the server's own game module, using gen_nav's
mature-to-peak method. q2red runs faster than real time under `timescale`, so 64 maps take
about 20 minutes. Maps that already have a nav are kept (use `--force` to rebake). Keep the
rig path short: q2red builds map paths in a 128-byte buffer.

**CTF maps need a CTF bake.** CTF spawns everyone in their own base and the learner only
grows the graph where bots go, so a CTF-rules bake from nothing gives two bases with no
link between them, and a deathmatch bake knows the middle but not the flag stands.
Seed the CTF bake with the deathmatch navs and rank checkpoints by flag runs:

```bash
py tools/q2red_navs.py --rig /tmp/q2bb --maps ctfpool.txt -j 6 --force --score ctf \
    --seed-dir <deathmatch navs> --max-seconds 1080 --patience 3 \
    --set g_entity_override_dir=maps --set g_gametype=4 --set g_dm_do_warmup=0 \
    --set g_dm_do_readyup=0 --set g_dm_allow_no_humans=1 --set g_match_lock=0 \
    --set capturelimit=0
```

The score line only ranks checkpoints, so check the result for a flag-to-flag route, not
by the score.

**Maps that come with a Kex nav** (`bots/navigation/<map>.nav`, NAV3), such as Rocket
Arena 2's, which its `sv nav_bake` writes, are converted instead of learned. On an arena
map the learner has little to learn from, because bots walk only where a mod puts them:

```bash
py tools/nav3_to_onav.py <game>/bots/navigation/ra2map1.nav <game>/nav/ra2map1.nav --floor-origins
```

`--floor-origins` is for bakers that write floor points (RA2's), not the standing origins
the stock files hold. Teleport, pusher and train links are left out, because ozbot seeds
them at load.

`baselines/nav_q2red_bloodyblastard/` holds the 64 navs deployed to Bloody Blastard on
2026-10-17. They were baked against MuffMode 0.60 with its entity overrides. `POOL.txt` is
the union of every `g_map_list` and `g_map_pool` in its `gt-*.cfg`. q2dm1-8 are the curated
`nav_shipped` graphs, still learnable. Weak maps after a 4-bot, 90 s play test: `rdm4` (14
pickups), `q2ctf1-3` (9-25); everything else collects 20-90. The 15 maps of its CTF
pool were rebaked on 2026-10-17 under CTF rules, seeded with those navs (see above).


- **Teams.** Bots never target a teammate, and they ignore teammates' noise. Two
  clients are teammates when the server publishes the same non-zero `sv_entity_t`
  team for both. That covers TDM, CTF and arena; coop-style modes put every player on
  one team. Free-for-all publishes team 0, so everyone stays fair game.
- **Monsters.** In Horde and coop-style modes, bots target living monsters as they do
  players: nearest visible first, with the same view cone. A monster that is attacking
  the bot is acquired even from behind, the way gunfire is. Measured on 2026-10-17: on
  MuffMode Horde, 91 of 92 sampled engagements were against monsters; on MMREX, 75 of
  75 (team checks: every TDM engagement on both mods was cross-team).
- **Capture the flag** (`oz_ctf.c`). On a map with flags each bot takes, in order:
  1. carrying the enemy flag: run home and capture, or wait on its own stand if its
     flag is not home;
  2. its own flag lying dropped: go and return it;
  3. an enemy carrying its flag: hunt the carrier;
  4. the enemy flag home or dropped: go and take it. Only one bot per team attacks a
     home flag, and it fetches a weapon first unless the flag is close.

  Otherwise it plays items as in deathmatch, and in a fight the enemy carrier comes
  first. A flag the graph cannot route to gets an approach instead: the bot routes to
  the node nearest the flag, then walks straight at it. This works with id's,
  MuffMode's and MMREX's flags (`item_flag_team1`/`2`, `item_flag_team_red`/`blue`).
  Items don't publish their spawnflags, so the home flag is told from a dropped one
  by remembering the first flag entity of each team seen on the map. It is inert on
  maps without flags.
- **Capture Strike** (MuffMode `g_gametype 7`, the same maps): rounds play out, and
  attackers take the flag. The rerelease doesn't publish which side is attacking, so
  defenders also try to take the flag until they have stood on it without getting it.
  After that they leave it alone for 30 s.
- **Arenas** (`oz_arena.c`, Rocket Arena 2 and the like). On a map with no items and no
  flags, each bot hunts the nearest live enemy in its own arena: the CTF carrier hunt,
  re-routed as the target moves. With nobody to hunt it roams only inside its arena. An
  arena is the part of the graph joined without teleport links, so bots never wander
  through a lobby's teleporters. While its own player is a spectator (in a mod's lobby,
  or observing a round) or frozen, a bot sends no buttons or moves; those would work
  the mod's menus and cameras.
- **No Horde tactics** beyond fighting.
- **MMREX Horde needs a human.** MMREX ends a Horde match that has no playing human
  ("ALL FIGHTERS LOST", `MM_Horde_CheckDesertionDefeat`), so bots alone cannot run it.
  That is MMREX's design, not a bot bug.

## Known limits

- **No CTF on q2ctf1.** Its bases join the middle only through underwater channels and
  a ladder. ozbot's learner has never produced a water route on any map in the pool, so
  the baked graph has the two bases cut off, and bots there roam for items. The other
  14 maps in Bloody Blastard's CTF pool are joined base to base (CTF bake of
  2026-10-17). The gaps could be closed by adding the missing links to the graph by
  hand with `tools/nav_edit.py add-link`, following the rerelease's own
  `bots/navigation/q2ctf1.nav`.

- **Frags are not counted on MuffMode or MMREX.** Telemetry `score` reads `STAT_FRAGS`
  at id's index (14), which those mods lay out differently. The kills happen (see the
  obituaries); only the counter is wrong.
- **Precision of the movement sims.** The sims run through 3.19's 12.3 fixed-point
  `pmove_state_t`, which reaches only ±4096 units. On a larger rerelease map the
  predictions are off, but the real movement is not.
- **No landing workaround.** ozbot's strafe-jump landing hack (clearing
  `PMF_TIME_LAND`) can't reach the game's state here. Bots hop under the same landing
  rule as human players on that server.
- **Attackers are guessed.** The rerelease publishes when a player was hit, not by
  whom. The pain reflex picks the nearest visible client that fired in the last 0.3
  s.
- **Death causes are partly guessed.** Lava, slime and drowning are inferred from
  where the bot died. Falls and `trigger_hurt` deaths read as combat deaths, so
  `bot_hazard` learns less from them than on q2repro.
- **Large maps.** Only the first 1024 entities are mirrored (3.19's `MAX_EDICTS`).
  Anything beyond reads as the world.
- **Engine goal requests.** `Bot_MoveToPoint` / `Bot_FollowActor`, used for MMREX's
  freeze-tag rescue and arena room-pick, are not answered.

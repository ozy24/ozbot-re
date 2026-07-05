# CLAUDE.md — ozbot-re

**ozbot-re** is the q2repro (40Hz) port of ozbot. This is a self-contained git
repo with its own `tools/` (versioned). The umbrella `../CLAUDE.md` covers only
the **shared** infra — `../engine/` runtime, the `../demos/` corpus (read-only
here; ozbot is its single writer), engine sources, and the persistent memory.
Everything below OVERRIDES the legacy ozbot specifics when working in this repo.

## What changed vs ozbot

- **Engine:** q2repro (`../q2repro`, local branch `ozbot-re` carrying the
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

## Playbooks (the new capability)

Recorded input sequences replayed as nav links (NAV_LINK_PLAYBOOK):
1. **Record**: `record_inputs.bat` (human, bot_inputlog) or
   `--cvar bot_cmdlog 1` (bots) on a 40Hz server.
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

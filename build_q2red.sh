#!/usr/bin/env bash
# ozbot-re - the q2red build: the same brain (src/bot_*.c) as a q2red bot
# brain, dist/ozbot_x64.so, instead of a 3.19 game DLL.
#
#   ./build_q2red.sh                    # build dist/ozbot_x64.so
#   ./build_q2red.sh install <gamedir>  # ...and copy it (plus botnames.txt)
#                                       # into a q2red game directory
#
# Load it on a q2red server with `+set sv_bot_brain ozbot`; `bot_count N`
# (or `addbot` / `kickbot`) sets the population.  See q2red/README.md.
#
# Only the bot files and q_shared.c are compiled: the game itself is whatever
# rerelease module the server runs (id's, MuffMode, MMREX).  q2red/oz_host.c
# stands in for the 3.19 game the brain was written against.  -DOZBOT_Q2RED
# selects the handful of hosted-only lines in bot_main.c / bot_log.c; without
# it (build.bat, build_linux.sh) nothing changes.
set -euo pipefail
cd "$(dirname "$0")"
what="${1:-build}"

mkdir -p dist
gcc -O2 -fPIC -shared -std=gnu11 -fno-strict-aliasing -fvisibility=hidden \
    -DNDEBUG -DC_ONLY -DOZBOT_Q2RED -Wall -Wno-unused-variable -Wno-unused-but-set-variable \
    -Wno-misleading-indentation -Wno-dangling-else -Wno-parentheses -Wno-missing-braces \
    -Wno-unused-function -Wl,--no-undefined \
    -o dist/ozbot_x64.so src/bot_*.c src/q_shared.c q2red/oz_host.c q2red/oz_ctf.c q2red/oz_arena.c -lm
nm -D --defined-only dist/ozbot_x64.so | grep -q ' Q2RED_GetBotBrainAPI$' \
    || { echo "[ozbot-re] Q2RED_GetBotBrainAPI is not exported"; exit 1; }
echo "[ozbot-re] Built dist/ozbot_x64.so"

if [ "$what" = install ]; then
    dest="${2:?usage: ./build_q2red.sh install <q2red game directory>}"
    install -m 755 dist/ozbot_x64.so "$dest/ozbot_x64.so"
    install -m 644 data/botnames.txt "$dest/botnames.txt"
    echo "[ozbot-re] Installed into $dest"
fi

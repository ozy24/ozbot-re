#!/usr/bin/env bash
# ozbot-re - native Linux build (the counterpart of build.bat / build_engine.bat /
# deploy.bat).  Builds the headless rig only: the q2reproded dedicated server
# (with the fastsim patch, from q2repro/ branch ozbot-re) and the game as
# dist/gamex86_64.so, then deploys both into engine/.
#
#   ./build_linux.sh            # game + deploy (the usual inner loop)
#   ./build_linux.sh engine     # dedicated server only (+ deploy)
#   ./build_linux.sh all        # both
#
# The engine needs meson + ninja (MESON=/path/to/meson to override, else
# `meson` on PATH; `pip install --user meson` if missing).  The game needs gcc.
# The tools pick the native pair automatically when engine/q2reproded exists
# (tools/rig_platform.py); otherwise they fall back to the .exe under Wine.
set -euo pipefail
cd "$(dirname "$0")"
Q2DIR="${Q2DIR:-$PWD/engine}"
what="${1:-game}"

build_engine() {
    local meson="${MESON:-meson}"
    command -v "$meson" >/dev/null || { echo "[ozbot-re] meson not found (pip install --user meson, or set MESON=)"; exit 1; }
    if [ ! -d q2repro/build-linux ]; then
        # mirrors build_engine.bat; client-side deps are off, only q2reproded is built
        "$meson" setup q2repro/build-linux q2repro -Dwrap_mode=forcefallback -Dvariable-fps=true \
            -Davcodec=disabled -Dlibcurl=disabled -Dsdl2=disabled -Dopenal=disabled \
            -Dx11=disabled -Dwayland=disabled -Dlibjpeg=disabled
    fi
    "$meson" compile -C q2repro/build-linux q2reproded
    install -m 755 q2repro/build-linux/q2reproded "$Q2DIR/q2reproded"
    echo "[ozbot-re] Deployed q2reproded to $Q2DIR"
}

build_game() {
    mkdir -p dist
    # Same defines as build.bat.  -fvisibility stays default: GetGameAPI must be
    # exported (game.def does that job on Windows).
    gcc -O2 -fPIC -shared -std=gnu11 -fno-strict-aliasing \
        -DNDEBUG -DC_ONLY -Wall -Wno-unused-variable -Wno-unused-but-set-variable \
        -Wno-misleading-indentation -Wno-dangling-else -Wno-parentheses -Wno-missing-braces \
        -o dist/gamex86_64.so src/*.c -lm
    # (no `grep -q` here: under pipefail its early exit SIGPIPEs nm)
    nm -D dist/gamex86_64.so | grep " T GetGameAPI" >/dev/null \
        || { echo "[ozbot-re] GetGameAPI not exported"; exit 1; }
    mkdir -p "$Q2DIR/ozbotre"
    install -m 755 dist/gamex86_64.so "$Q2DIR/ozbotre/gamex86_64.so"
    echo "[ozbot-re] Built dist/gamex86_64.so, deployed to $Q2DIR/ozbotre"
}

case "$what" in
    game)   build_game ;;
    engine) build_engine ;;
    all)    build_engine; build_game ;;
    *)      echo "usage: $0 [game|engine|all]"; exit 2 ;;
esac

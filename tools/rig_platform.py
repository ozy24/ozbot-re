"""
Which engine binary + game library the headless rig uses on this host.

  Windows            q2reproded.exe + gamex86_64.dll   (build.bat / build_engine.bat)
  Linux, native      q2reproded     + gamex86_64.so    (build_linux.sh)
  Linux, fallback    q2reproded.exe + gamex86_64.dll under Wine

The engine and game library are chosen as a PAIR: a native engine only loads
the .so and the Windows engine only loads the .dll.  Native wins whenever
<engine>/q2reproded exists.

Native and Wine/Windows runs are different binaries, so same-seed telemetry
md5s are NOT comparable across them -- keep an A/B on one rig.
"""

import os
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ENGINE = os.path.join(REPO, "engine")


def native_available(engine=DEFAULT_ENGINE):
    return os.name != "nt" and os.path.isfile(os.path.join(engine, "q2reproded"))


def repro_names(engine=DEFAULT_ENGINE):
    """(engine executable name, game library name) for the --repro rig."""
    if native_available(engine):
        return "q2reproded", "gamex86_64.so"
    return "q2reproded.exe", "gamex86_64.dll"


def launch_prefix(exe):
    """Command prefix needed to run `exe` here: Wine for a .exe off Windows."""
    if os.name != "nt" and exe.lower().endswith(".exe"):
        return ["wine"]
    return []


def wine_env():
    env = dict(os.environ)
    env.setdefault("WINEDEBUG", "-all")
    return env


def build_game(engine=DEFAULT_ENGINE):
    """Build dist/<game library> for the rig in use.  Returns the exit code."""
    if os.name == "nt":
        return subprocess.call(["cmd", "/c", os.path.join(REPO, "build.bat")], cwd=REPO)
    if native_available(engine):
        return subprocess.call([os.path.join(REPO, "build_linux.sh"), "game"], cwd=REPO)
    # Wine rig: the .dll needs MSVC -- build it on the Windows box (winbuild)
    print("[rig] no native engine/q2reproded and no MSVC here: run ./build_linux.sh all, "
          "or build the .dll on Windows")
    return 1

# -*- mode: python ; coding: utf-8 -*-
# PyInstaller spec for gen_nav.exe -- the standalone (no-Python) nav generator.
# Build:  build_gen_nav.bat   (or: py -m PyInstaller tools/gen_nav.spec)
#
# gen_nav.py imports its sibling tools (run_parallel, benchmark, nav_edit) as
# top-level modules, so `pathex` points at tools/ and they're listed as
# hiddenimports to guarantee they're bundled.  One-file, console app.
import os

HERE = os.path.join(os.getcwd(), "tools")

a = Analysis(
    [os.path.join(HERE, "gen_nav.py")],
    pathex=[HERE],
    binaries=[],
    datas=[],
    hiddenimports=["run_parallel", "benchmark", "nav_edit"],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=["tkinter", "numpy", "matplotlib", "PIL"],  # keep the exe small
    noarchive=False,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name="gen_nav",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=True,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)

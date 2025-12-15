# Quake 2 Mod Template

This is a complete, working template mod for Quake 2 based on the original game source code. It provides a solid foundation for building custom mods.

## Structure

```
mod/
├── src/          # Source code files (.c and .h)
├── dist/         # Build output directory (gamex86.dll)
├── build.bat     # Build script (Windows)
└── README.md     # This file
```

## Quick Start

1. **Copy this template** to create a new mod:
   ```bash
   cp -r mod/ my_new_mod/
   ```

2. **Modify the source files** in `src/` to customize behavior

3. **Build the mod**:
   ```bash
   cd my_new_mod
   .\build.bat
   ```

4. **Copy the DLL** from `dist/gamex86.dll` to your Quake 2 game directory

5. **Run Quake 2** with the mod:
   ```
   quake2.exe +set game my_new_mod
   ```

## Source Files

The `src/` directory contains all the game source files:

### Core Game Files
- `g_main.c` - Main game initialization and API entry point
- `g_spawn.c` - Entity spawning system
- `g_utils.c` - Entity management utilities
- `g_combat.c` - Combat and damage handling
- `g_weapon.c` - Weapon logic
- `g_items.c` - Item spawning and pickup
- `g_func.c` - Func entities (doors, buttons, etc.)
- `g_trigger.c` - Trigger entities
- `g_target.c` - Target entities
- `g_phys.c` - Physics and movement
- `g_misc.c` - Miscellaneous entities
- `g_cmds.c` - Player commands
- `g_svcmds.c` - Server commands
- `g_chase.c` - Spectator chase cam
- `g_save.c` - Save/load functionality

### Player Files
- `p_client.c` - Player client code
- `p_view.c` - Player view/camera
- `p_weapon.c` - Player weapon handling
- `p_hud.c` - HUD display
- `p_trail.c` - Player trail system

### Monster/AI Files
- `g_ai.c` - AI functions
- `g_monster.c` - Monster base code
- `m_move.c` - Monster movement
- `m_*.c` - Individual monster implementations
- `g_turret.c` - Turret entities

### Headers
- `g_local.h` - Game structures and local definitions
- `game.h` - Game API definitions
- `q_shared.h` - Shared type definitions
- `m_player.h` - Player structure definitions
- `m_*.h` - Monster headers

## Building

### Windows

The `build.bat` script automatically:
- Detects GCC or MSVC compiler
- Compiles all `.c` files in `src/`
- Outputs `gamex86.dll` to `dist/`

Simply run:
```bash
.\build.bat
```

### Manual Build (GCC)
```bash
cd src
gcc -m32 -shared -o ../dist/gamex86.dll *.c -DWIN32 -D_WINDOWS -DNDEBUG -DC_ONLY -I../quake2-source/game -I../quake2-source/qcommon -I. -std=c99
```

### Manual Build (MSVC)
```bash
cl /nologo /MT /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "C_ONLY" /c src\*.c /I..\quake2-source\game /I..\quake2-source\qcommon /I. src
link /nologo /base:0x20000000 /subsystem:windows /dll /machine:I386 /def:src\game.def /out:dist\gamex86.dll *.obj kernel32.lib user32.lib winmm.lib
```

## Customization

### Adding Custom Initialization Message

Edit `src/g_save.c`, in the `InitGame()` function:
```c
void InitGame (void)
{
    gi.dprintf ("==== InitGame ====\n");
    gi.dprintf ("Your Custom Mod Name v1.0 loaded.\n");
    // ... rest of initialization
}
```

### Adding New Source Files

1. Add your `.c` file to `src/`
2. Add corresponding `.h` file if needed
3. Rebuild with `build.bat` - it will automatically compile all `.c` files

### Modifying Game Behavior

Edit any of the source files in `src/` to change game behavior:
- Modify weapons in `g_weapon.c` or `p_weapon.c`
- Change item behavior in `g_items.c`
- Adjust physics in `g_phys.c`
- Customize monsters in `g_monster.c` or individual `m_*.c` files

## Testing

1. Build the mod: `.\build.bat`
2. Copy `dist/gamex86.dll` to your Quake 2 game directory (e.g., `baseq2/` or a custom mod folder)
3. Run Quake 2: `quake2.exe +set game your_mod_name`
4. Check the console for "Custom mod loaded." message
5. Load a map: `map q2dm1`

## Notes

- The mod is based on Quake 2 v3.19 source code
- All files are licensed under the GPL
- The template includes the full base game functionality
- You can remove monster files (`m_*.c`, `g_monster.c`, `g_ai.c`) if you don't need them
- The build script compiles all `.c` files in `src/` automatically

## Troubleshooting

**Build fails with "undefined reference" errors:**
- Make sure all required source files are in `src/`
- Check that header files are present

**Mod doesn't load:**
- Verify `gamex86.dll` is in the correct game directory
- Check that the DLL was built for 32-bit (x86)
- Ensure `game.def` exports `GetGameAPI`

**Map crashes on load:**
- Make sure `g_spawn.c` is present and compiled
- Verify all entity spawning functions are available

## License

This template is based on the Quake 2 source code, which is licensed under the GNU General Public License (GPL) version 2 or later.


# Changelog

All notable changes to this mod template will be documented in this file.

## [Template v1.0] - Initial Release

### Added
- Complete Quake 2 base game source code
- Automated build script (`build.bat`) for Windows
- Support for both GCC and MSVC compilers
- Custom initialization message ("Custom mod loaded.")
- All core game functionality:
  - Entity spawning system
  - Player movement and controls
  - Weapons and items
  - Combat system
  - Physics and movement
  - Monster AI and spawning
  - Save/load functionality
  - Server commands

### Structure
- `src/` - All source code files
- `dist/` - Build output directory
- `build.bat` - Automated build script
- `README.md` - Documentation
- `.gitignore` - Git ignore rules

### Notes
- Based on Quake 2 v3.19 source code
- Licensed under GPL v2+
- Ready to use as a template for new mods

---

## Template Usage

When creating a new mod from this template:

1. Copy the entire `mod/` directory
2. Rename it to your mod name
3. Modify source files in `src/` as needed
4. Update this changelog with your mod's changes
5. Update `README.md` with your mod-specific information

### Example Entry Format

```markdown
## [Your Mod Name v1.0] - Date

### Added
- Feature 1
- Feature 2

### Changed
- Modified behavior X
- Updated Y

### Fixed
- Bug fix 1
- Bug fix 2
```


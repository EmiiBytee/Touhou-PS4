# Changelog

All notable changes to the PS4 ports are documented here.

## 1.1.0

### Touhou 8: Imperishable Night (new)

- Recompiled the N0zoM1z0/th08 reconstruction's native 64-bit runtime for PlayStation 4.
- Added a Direct3D 8 device on OpenGNM with VideoOut presentation.
- Added PS4-safe configuration, score, replay and log paths under `/data/touhou/th08/`.
- Fixed structures cleared with 32-bit sizes on a 64-bit target, including the Spell Practice
  retry crash.
- Fixed the boss life bar disappearing between phases, following the original executable's
  death-mode handling.
- Fixed practice scores not being saved and lasers not being drawn.
- Added thcrap-lite support for IN's dialogue scripts, boss titles and names, spell card names,
  Music Room titles and comments, menu and help text, translated textures and the translation's
  Latin font.
- Replaced the original Fullscreen/Windowed option with a 4:3/16:9 selector.
- Added the ZUN cheat code on the controller.

### All ports

- Log output is written by a background thread, so logging no longer costs frame time.
- `stage_thcrap.py` selects version-specific patch files for the reconstructed game version.

## 1.0.0

Initial public source release.

### Touhou 6: The Embodiment of Scarlet Devil

- Recompiled the portable TH06 decompilation for PlayStation 4.
- Added native OpenGNM rendering and VideoOut presentation.
- Added SDL2 controller, audio, timing and USB-keyboard support.
- Added PS4-safe configuration, score, replay and log paths.
- Added lightweight support for staged thcrap translation data.
- Replaced the original Fullscreen/Windowed option with a 4:3/16:9 selector.
- Added the tested DualShock 4 control layout.

### Touhou 7: Perfect Cherry Blossom

- Recompiled the portable TH07 decompilation for PlayStation 4.
- Added native OpenGNM rendering with corrected 2D/3D state, blending and depth behavior.
- Added SDL2 controller, audio and timing support.
- Added PS4-safe configuration, score, replay and log paths.
- Added lightweight thcrap support for dialogue, spell cards, menus, results and textures.
- Added wrapping for translated dialogue that exceeds the original line width.
- Replaced the original Fullscreen/Windowed option with a 4:3/16:9 selector.
- Fixed replay loading and enforced the tested PS4 control layout.

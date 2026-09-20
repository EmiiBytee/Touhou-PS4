# Third-party software and credits

This project combines independently licensed components. The repository-level GPL-3.0 license
does not replace the notices or terms of those components.

## Decompilation projects

### Touhou 6

- [happyhavoc/th06](https://github.com/happyhavoc/th06), `portable` branch — GPL-3.0.
- [GensokyoClub/th06](https://github.com/GensokyoClub/th06) — original TH06 decompilation work.
- The upstream source and GPL text are retained under `src/th06/`.

All TH06 PS4 source changes are distributed under GPL-3.0.

### Touhou 7

- [cardanawandra/th07](https://github.com/cardanawandra/th07), `portable` branch — CC0 1.0.
- Its upstream credits include the GensokyoClub TH06 and TH08 decompositions and EstexNT's
  `var_order` pragma work. Those notices remain in `src/th07/README.md`.
- The original CC0 text is retained as `src/th07/LICENSE`.

## PS4 toolchain and graphics

- [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) — compiler,
  headers, library stubs and packaging tools; see the upstream repository for its license.
- [PacBrew packages and PS4 portlibs](https://github.com/PacBrew/ps4-openorbis-portlibs) — PS4
  builds of SDL2 and related libraries; each packaged project retains its own license.
- [OpenGNM](https://github.com/PS4-OpenGNM/opengnm) — native PS4 GNM library, MIT.
- [freegnm-examples](https://github.com/PS4-OpenGNM/freegnm-examples) — display, allocation and
  utility support used at build time, MIT except for separately identified vendored components.
- [lateleite/psbc](https://github.com/lateleite/psbc) — SPIR-V to PS4 shader-binary compiler;
  original psbc code is MIT and its Mesa-derived portions retain Mesa licenses.
- Shaderc `glslc` — GLSL to SPIR-V compiler; see the Shaderc/Khronos license files.

OpenGNM, freegnm-examples, psbc and the OpenOrbis/PacBrew toolchains are external build
dependencies. Their repositories are not vendored here.

## Runtime and translation components

- [SDL2](https://github.com/libsdl-org/SDL),
  [SDL2_image](https://github.com/libsdl-org/SDL_image) and
  [SDL2_ttf](https://github.com/libsdl-org/SDL_ttf) — zlib license.
- `miniaudio.h`, retained in the TH07 upstream source — dual-licensed public domain or MIT-0;
  see the header itself.
- `NotoSans-Regular.ttf`, retained from the TH06 upstream source — SIL Open Font License 1.1.
  It is used by the upstream desktop configuration utility; the license is retained in
  `LICENSES/OFL-1.1.txt`. No proprietary Microsoft font is included.
- [thcrap](https://github.com/thpatch/thcrap) and translation data from
  [Touhou Patch Center](https://www.thpatch.net/) — the thcrap engine is released to the public
  domain unless stated otherwise. Individual translation repositories and assets may carry their
  own notices. They are not included here.
- GoldHEN — used for package installation, FTP deployment and runtime logging on test hardware.

## Original games and platform files

Touhou 6, Touhou 7, their data archives, music, graphics and other original assets are copyright
Team Shanghai Alice / ZUN and are not distributed by this project.

Microsoft fonts and Sony PS4 firmware/system modules are not distributed. Users must source any
required files from systems or software they are legally entitled to use.

The package artwork used during private development was omitted because its redistribution rights
were not established. Public builds use the OpenOrbis sample icon unless the builder supplies
local artwork.

## Development assistance

Claude Code and OpenAI Codex were used as development assistants for research, code iteration,
debugging, hardware-log/capture analysis and documentation. Final builds and gameplay behavior
were tested on a real PlayStation 4 by Emii.

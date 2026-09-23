# 東方永夜抄 ～ Imperishable Night — reconstruction source for the PS4 port

This directory holds the part of the TH08 reconstruction that the PS4 build compiles:

- `src/` — the game source, the reconstruction's Linux compatibility layer
  (`src/modern/linux/`) and the PS4 Direct3D 8 renderer and translation hooks
  (`src/modern/ps4/`);
- `config/i18n.csv` and `scripts/generate_i18n.py` — the message table and the script that turns
  it into `i18n.hpp` at build time;
- `LICENSE` — the upstream MIT license.

It is taken from [N0zoM1z0/th08](https://github.com/N0zoM1z0/th08), branch
`port/portable-64bit` (the native 64-bit runtime), at commit
[`861bec9`](https://github.com/N0zoM1z0/th08/commit/861bec908b84fa4658382d7526e5a0075f520846).
That project is an independent continuation of the public
[GensokyoClub/th08](https://github.com/GensokyoClub/th08) decompilation and preserves its history,
authorship and license; it also credits @EstexNT for porting the `var_order` pragma to MSVC7 and
builds on the workflow of the [N0zoM1z0/th07](https://github.com/N0zoM1z0/th07) reconstruction.
The full reconstruction — documentation, target mappings, verification tooling and the desktop
builds — lives in that repository.

PS4 changes to the game source are guarded by `TH_PS4`, `TH_THPATCH`, `TH08_MODERN_PORT` or
`TH08_PORTABLE_NATIVE_LAYOUT`, except where a fix brings the reconstruction closer to the original
executable (the boss death-mode handling in `EnemyManagerUpdate.cpp`). Changes to the
compatibility layer in `src/modern/linux/` that also benefit the desktop build (for example
DirectSound buffer locking) are not guarded.

Rights to the original game, executable and game data remain with Team Shanghai Alice / ZUN.

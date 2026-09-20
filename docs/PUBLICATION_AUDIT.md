# Publication audit

This document records the release boundary used to create the public tree.

## Included

| Content | Reason |
|---|---|
| `src/th06/` | GPL-3.0 portable decompilation source and PS4 modifications |
| `src/th07/` | CC0 portable decompilation source and PS4 modifications |
| `ps4/common/` | Original PS4 platform, VideoOut, allocator and thpatch-lite code |
| `ps4/th06/`, `ps4/th07/` | CMake targets and package metadata logic |
| `ps4/tools/` source/scripts | Build, patch-staging, label-generation and FTP helpers |
| Documentation and license files | Build/install guidance, attribution and release policy |

## Excluded from the working folder

| Local content | Why it must not be published |
|---|---|
| `Touhou 6 - .../`, `Touhou 7 - .../` | Complete commercial game installations |
| `dist/ps4-data/` | Copied game archives, music and fonts |
| `*-FULL-PERSONAL.pkg` | Packages containing commercial game data |
| All other `.pkg`, `.self`, `.sprx`, `.prx` outputs | Generated binaries; some include platform components |
| `ps4/private/modules/` | Sony Piglet/Shacc firmware modules |
| `ps4/private/save-backup/` | Personal score and replay files |
| `ps4/private/overlay/` | Locally generated images based on game/translation assets |
| `evidence/`, videos and screenshots | Debug captures, logs and personal test material |
| `msgothic.ttc`, `latin.ttf` | Microsoft fonts or local substitutes without redistribution clearance |
| staged thcrap repositories/patch trees | Downloaded translation content with separate provenance |
| development handoff/transcript files | Internal notes, stale state and local network details |
| private package icons/backgrounds | Redistribution rights were not established |

## Release rules

Before publishing a commit or GitHub release:

1. Check `git status --short` and inspect every untracked file.
2. Confirm no game directory, font, translation cache, save, replay, module or media file is
   tracked.
3. Never attach a `FULL-PERSONAL` package.
4. If publishing a thin PKG, inspect its package contents and ensure it contains no `/assets`
   game data or `/modules` copied from a console/firmware.
5. Use only artwork with explicit redistribution permission; otherwise keep the OpenOrbis sample
   icon or supply artwork locally.
6. Remove IP addresses, usernames, tokens and personal paths from logs before attaching them.

This audit is a conservative project policy, not legal advice.

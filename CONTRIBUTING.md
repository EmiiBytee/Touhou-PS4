# Contributing

Contributions are welcome, especially reproducible PS4 hardware reports, renderer fixes and
documentation improvements.

## Before opening an issue

Include:

- the game (`th06` or `th07`) and build/commit;
- PS4 model and firmware;
- GoldHEN version;
- 4:3 or 16:9 mode;
- exact steps to reproduce;
- relevant lines from `/data/touhou/<game>/ps4_log.txt`.

Do not attach game archives, music, fonts, Sony modules, full PKGs, saves containing personal
information or files from a thcrap repository unless their license permits redistribution.

## Pull requests

Keep TH06 and TH07 behavior separate where their engines differ. Verify at least:

```bash
bash ps4/build.sh th06
bash ps4/build.sh th07
```

For graphics changes, test both 4:3 and 16:9 and check menus, gameplay, pause, bombs, spell-card
effects, stage transitions and replays. For translation changes, check dialogue, spell names,
Music Room and Results.

Do not commit anything ignored by `.gitignore`. Preserve all upstream copyright and license
notices.

# Installation and translations

## Install the source-only package

Install the matching PKG through GoldHEN's Package Installer. You can transfer it by USB or place
it in `/data/pkg/` over GoldHEN FTP (normally port `2121`).

Example:

```bash
curl --ftp-create-dirs -T dist/IV0001-THSP00006_00-THSP000060100000.pkg \
  ftp://PS4_IP:2121/data/pkg/IV0001-THSP00006_00-THSP000060100000.pkg
```

Use `THSP00007` for Touhou 7 and `THSP00008` for Touhou 8.

## Copy your game files

Touhou 6 v1.02h goes in `/data/touhou/th06/`:

```text
紅魔郷CM.DAT  紅魔郷ED.DAT  紅魔郷IN.DAT
紅魔郷MD.DAT  紅魔郷ST.DAT  紅魔郷TL.DAT
bgm/
msgothic.ttc
```

Touhou 7 v1.00b goes in `/data/touhou/th07/`:

```text
th07.dat
thbgm.dat
msgothic.ttc
```

Touhou 8 v1.00d goes in `/data/touhou/th08/`:

```text
th08.dat
thbgm.dat
msgothic.ttc
```

The directory is also used for configuration, `score.dat`, `replay/` and `ps4_log.txt`.

## Optional thcrap translation

Configure the language patch normally against your legally obtained PC installation. From this
repository, stage the active patch stack into a flat runtime directory:

```bash
python3 ps4/tools/stage_thcrap.py games/th06/thcrap th06 dist/th06-patch
python3 ps4/tools/stage_thcrap.py games/th07/thcrap th07 dist/th07-patch
python3 ps4/tools/stage_thcrap.py games/th08/thcrap th08 dist/th08-patch
```

For Touhou 8, keep `th08.exe` in the game folder next to `thcrap/` while staging. Menu
descriptions, help lines and similar text live in the executable; thcrap finds them by address,
which a recompiled port does not have, so the script reads the original strings out of your
`th08.exe` and writes a content-keyed `stringtable.js`. The executable itself is not copied into
the staged directory. Boss names shown in dialogue are written to `msgnames.js`.

Upload the result:

```bash
python3 ps4/tools/upload_ftp_tree.py PS4_IP 2121 \
  dist/th06-patch /data/touhou/th06/patch

python3 ps4/tools/upload_ftp_tree.py PS4_IP 2121 \
  dist/th07-patch /data/touhou/th07/patch

python3 ps4/tools/upload_ftp_tree.py PS4_IP 2121 \
  dist/th08-patch /data/touhou/th08/patch
```

The lightweight runtime supports translated dialogue, string tables, spell names, BGM titles,
Music Room comments and compatible texture replacements. For Touhou 8 it also covers the menu and
help text read from the executable, and draws English text with the patch's Latin font, narrowing
lines that would overrun their dialogue box. It is not a Windows DLL loader and does
not run thcrap itself on the PS4.

## 4:3 / 16:9 labels

On PS4, the original Fullscreen/Windowed setting changes the presentation aspect ratio instead:

- `4:3` pillarboxes the original image;
- `16:9` fills the display using the port's widescreen presentation.

The behavior is implemented in source. Because the original menu labels are graphics owned by the
game, replacement label images are not committed. Generate them locally from files extracted from
your own game/patch:

```powershell
# TH06: input directory contains title04.png and title04s.png
pwsh ps4/tools/make_aspect_labels.ps1 INPUT_DIR ps4/private/overlay/th06

# TH07: input is the extracted title01.png
pwsh ps4/tools/make_th07_aspect_labels.ps1 INPUT_TITLE01.png ps4/private/overlay/th07

# TH08: input is the texture sheet of title01.anm (see below)
pwsh ps4/tools/make_th08_aspect_labels.ps1 INPUT_TITLE01.png ps4/private/overlay/th08
```

For Touhou 8, extract `title01.anm` from your `th08.dat` with a TH08-capable archive tool such as
[thtk](https://github.com/thpatch/thtk) (`thdat -x 8 th08.dat title01.anm`), then dump its sheet:

```bash
python3 ps4/tools/th08_anm_dump.py title01.anm INPUT_DIR
```

Then stage the translation again, passing the generated overlay as the fourth argument:

```bash
python3 ps4/tools/stage_thcrap.py games/th06/thcrap th06 dist/th06-patch \
  ps4/private/overlay/th06

python3 ps4/tools/stage_thcrap.py games/th07/thcrap th07 dist/th07-patch \
  ps4/private/overlay/th07

python3 ps4/tools/stage_thcrap.py games/th08/thcrap th08 dist/th08-patch \
  ps4/private/overlay/th08
```

`ps4/private/` is ignored by Git.

## Troubleshooting

If the game closes or fails to load, inspect:

```text
/data/touhou/th06/ps4_log.txt
/data/touhou/th07/ps4_log.txt
/data/touhou/th08/ps4_log.txt
```

Check file names and versions first. Do not reuse a PC configuration file.

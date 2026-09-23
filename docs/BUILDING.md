# Building

## Tested host setup

- WSL2/Linux
- PacBrew OpenOrbis toolchain and PS4 portlibs
- CMake 3.20 or later and Ninja
- Python 3
- Shaderc `glslc`
- Zig and [lateleite/psbc](https://github.com/lateleite/psbc)
- [OpenGNM stack](https://github.com/PS4-OpenGNM/opengnm-stack)

The final ports use OpenGNM and VideoOut. They do not need Piglet or Sony shader-compiler modules
at runtime.

The hardware-tested dependency revisions were:

```text
opengnm          c51243f333a1a964894903da080a508f7af4d91f
freegnm-examples ffd055563e5b849ebef1b276ba29e730199670f8
lateleite/psbc   3b1ea85bc25315d6e611f58f51a8639003a291e8
```

Newer compatible revisions may work, but those hashes describe the completed hardware-tested
build.

## PacBrew/OpenOrbis

```bash
sudo pacbrew-pacman -Sy
sudo pacbrew-pacman -S ps4-openorbis ps4-openorbis-portlibs
sudo chmod a+x /opt/pacbrew/ps4/openorbis/ps4vars.sh
```

The build script sources `/opt/pacbrew/ps4/openorbis/ps4vars.sh`.

## Graphics dependencies

```bash
git clone --recursive https://github.com/PS4-OpenGNM/opengnm-stack.git "$HOME/opengnm-stack"
cd "$HOME/opengnm-stack/opengnm"
cp config.orbis.mak config.mak
make

git clone https://github.com/lateleite/psbc.git "$HOME/old-psbc"
cd "$HOME/old-psbc"
zig build -Doptimize=ReleaseFast
```

Install `glslc` from your distribution's Shaderc package. The build performs:

```text
GLSL -> SPIR-V (glslc) -> PS4 shader binary (psbc)
```

The older `lateleite/psbc` is deliberate for this renderer because it preserves the resource
descriptor-table layout expected by the current shaders.

## Build commands

```bash
cd Touhou-PS4
bash ps4/build.sh th06
bash ps4/build.sh th07
bash ps4/build.sh th08
```

Touhou 8 also needs Python 3 at build time: its message table (`i18n.hpp`) is generated from
`src/th08/config/i18n.csv`.

Defaults can be overridden:

```bash
OPENGNM_STACK=/work/opengnm-stack \
PSBC=/work/psbc/zig-out/bin/psbc \
bash ps4/build.sh th07
```

The output is copied to `dist/`.

By default, intermediate files stay in the checkout's ignored `build/` directory. Set
`THPS4_BUILD_ROOT` to a Linux-native path if faster WSL builds are desired:

```bash
THPS4_BUILD_ROOT="$HOME/thps4-build" bash ps4/build.sh th06
```

## Package artwork

Each game's package icon, `ps4/<game>/pkg/sce_sys/icon0.png` (512x512), is included and is cropped
from that game's own artwork. A 1920x1080 home-screen background is optional and local-only:

```text
ps4/th06/pkg/sce_sys/pic1.png
ps4/th07/pkg/sce_sys/pic1.png
ps4/th08/pkg/sce_sys/pic1.png
```

These `pic1.png` files are ignored by Git. If an `icon0.png` is missing, the build falls back to
the OpenOrbis sample icon.

## Personal full packages

For local testing only, put your data in `games/th06/`, `games/th07/` or `games/th08/` and add
`--full`:

```bash
bash ps4/build.sh th06 --full
bash ps4/build.sh th07 --full
bash ps4/build.sh th08 --full
```

If a `thcrap/` folder is present in the game folder, the full build stages the translation into
the package as well (for Touhou 8, with `th08.exe` next to it as described in
[Installation](INSTALLATION.md)).

You can point elsewhere without copying the files:

```bash
TH06_ASSETS_DIR=/path/to/th06 bash ps4/build.sh th06 --full
TH07_ASSETS_DIR=/path/to/th07 bash ps4/build.sh th07 --full
TH08_ASSETS_DIR=/path/to/th08 bash ps4/build.sh th08 --full
```

Never distribute a `FULL-PERSONAL` package.

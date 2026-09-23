#!/bin/bash
# Builds a PS4 port inside WSL.
# Usage: ps4/build.sh <th06|th07|th08> [--full] [ninja targets...]
#   --full  bundle your own game files into a personal pkg. Never share that pkg: it
#           contains copyrighted game data and may contain a locally supplied font.
set -e
GAME=${1:-th06}; shift || true
SUFFIX=""; BUNDLE=OFF; PKG_SUFFIX=""
if [ "$1" = "--full" ]; then SUFFIX=-full; BUNDLE=ON; PKG_SUFFIX=-FULL-PERSONAL; shift; fi
ROOT=$(cd "$(dirname "$0")/.." && pwd)

case $GAME in
    th06) ASSETS="${TH06_ASSETS_DIR:-$ROOT/games/th06}" ;;
    th07) ASSETS="${TH07_ASSETS_DIR:-$ROOT/games/th07}" ;;
    th08) ASSETS="${TH08_ASSETS_DIR:-$ROOT/games/th08}" ;;
    *) echo "unknown game: $GAME"; exit 1 ;;
esac

source /opt/pacbrew/ps4/openorbis/ps4vars.sh
if ldd "$OPENORBIS/bin/ld.lld" | grep -q "libxml2.so.2 => not found"; then
    [ -f "$ROOT/ps4/tools/lib/libxml2.so.2" ] || bash "$ROOT/ps4/tools/libxml2-stub.sh"
    export LD_LIBRARY_PATH="$ROOT/ps4/tools/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

# All release ports use the native OpenGNM + VideoOut backend and are packaged as normal
# recordable games. The older Piglet/software experiments are intentionally not the default.
BIG=ON
SUFFIX="$SUFFIX-big"
# Keep CMake state checkout-local. This avoids source-path cache collisions when several clones
# are built on the same machine. Override only when a Linux-native build volume is preferred.
BUILD_ROOT=${THPS4_BUILD_ROOT:-$ROOT/build}
BUILD=$BUILD_ROOT/$GAME$SUFFIX
mkdir -p "$BUILD"
# The pkg staging dir is refreshed at configure time, so always repack.
rm -f "$BUILD"/*.pkg
CMAKE_EXTRA=()
[ -n "${OPENGNM_STACK:-}" ] && CMAKE_EXTRA+=("-DOPENGNM_STACK=$OPENGNM_STACK")
[ -n "${PSBC:-}" ] && CMAKE_EXTRA+=("-DPSBC=$PSBC")
openorbis-cmake -G Ninja -S "$ROOT/ps4/$GAME" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
    -DTH_BUNDLE_ASSETS=$BUNDLE -DTH_ASSETS_DIR="$ASSETS" -DTH_PS4_BIG_APP=$BIG \
    "${CMAKE_EXTRA[@]}" > "$BUILD.configure.log" 2>&1 \
    || { cat "$BUILD.configure.log"; exit 1; }
grep -E "Bundling|No Piglet|WARNING" "$BUILD.configure.log" || true
ninja -C "$BUILD" "$@"

mkdir -p "$ROOT/dist"
for pkg in "$BUILD"/*.pkg; do
    name=$(basename "$pkg" .pkg)
    cp "$pkg" "$ROOT/dist/$name$PKG_SUFFIX.pkg"
done
ls -la "$ROOT/dist"/*.pkg

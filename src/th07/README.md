# th07

A cross-platform port of 東方妖々夢　～ Perfect Cherry Blossom 1.00b by Team Shanghai Alice using SDL2 and OpenGL ES.

This is the portable branch of the Touhou 7 decompilation. Unless you're looking specifically for an attempted cross-platform port of th07, you probably want the [main branch](https://github.com/some100/th07/tree/main).

This is a drop-in replacement for the original Touhou 7 binary that plays identically to the original, but is more portable to other platforms outside of Windows. As a result, the actual behavior was kept as unchanged as possible. For an even more portable and featureful port, see the [reallyportable branch](https://github.com/some100/th07/tree/reallyportable).

There are a few bugs/incompatibilities though, namely:

* You cannot load into stages on big endian machines. This is because the way ecl files, stg files, etc. are loaded in the original game is not endian independent, resulting in it breaking on any system not on little endian.
* Text rendering looks off. To be clear it does "work" but the text looks too big.
* Some features that the original game had, like 16 bit color mode, midi output, etc. are outright unimplemented. This may or may not be "fixed" later, but the focus currently is to produce a playable game.

Work is currently being done to transition the game over to being more platform-independent.

## Building

### Dependencies

* cmake
* SDL2 (SDL2, SDL2_ttf, and SDL2_image)
* OpenGL ES 3.0+
* A compiler that supports C++17
* A little endian machine

Run cmake on this repo, then build with whatever generator you chose.

You will also need to add a copy of `msgothic.ttc` into your game directory if you are not running this on Windows or otherwise don't have the "ＭＳ ゴシック" font installed.

## Todo

* Try to get the text rendering closer to the original
* Make the game endian independent

## Credits

* The earlier [decompilation for th06](https://github.com/GensokyoClub/th06), used as a source of shared types, function names, file names, source organization, basically everything. Because EoSD and PCB are so similar architecturally, the pre-existing th06 decompilation could be used as a direct reference for reverse engineering th07.

* The [decompilation for th08](https://github.com/GensokyoClub/th08) for the complete and actually readable LZSS implementation. Basically nothing changed from th07 to th08 at least in this regard, so it made it much simpler.

* EstexNT for porting the [var_order pragma](https://gist.github.com/EstexNT/e98a1384b906a3eedaaa3eeb7e58cd9d) to MSVC 7, which is used extensively throughout this project.

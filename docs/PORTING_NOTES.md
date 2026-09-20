# Porting notes

## Architecture

Both games execute their recompiled native C/C++ logic directly on PS4. The Windows DirectX path
is replaced with a GNM backend that records PS4 GPU command buffers, while VideoOut owns the
display buffers and flips. SDL2 remains responsible for controller input, audio callbacks and
timing; SDL2_ttf handles dynamic text.

The renderer was developed with OpenGNM/freegnm tools and shader disassembly (`gcn-dis` and
`psb-dis`). Hardware logs and GoldHEN captures were used to diagnose command-buffer completion,
descriptor layout, batching, blend/depth state and texture sampling.

## Touhou 6

The portable OpenGL abstraction was extended with GNM and software backends. The final PS4 build
uses GNM. Its original window-mode option now selects presentation layout: 4:3 pillarboxed or
16:9. Switching modes clears both display buffers so stale pixels cannot remain in the side areas.

Additional work covers filesystem rebasing, persistent saves/replays, PS4 controller and keyboard
input, a direct-memory allocator for normal game packages and thcrap-compatible text/image data.

## Touhou 7

TH07 uses its own GNM renderer. Correctness depended on flushing queued sprites whenever blend,
depth-mask or depth-function state changes; without those boundaries, 2D and 3D work shared the
wrong pipeline state. Texture-patch application also operates per ANM sprite rectangle so
transparent patch areas do not erase unrelated art while translated text fully replaces its
Japanese source.

The port includes PS4-safe replay enumeration, SDL-driven audio output, fixed controller defaults,
the same 4:3/16:9 Options-menu conversion and a thcrap-lite path for spell cards, dialogue, Music
Room and Results. Long one-line translations are split over the engine's two dialogue rows.

## Packaging model

Public packages are executable-only. All writable state and user-owned data live under
`/data/touhou/th06/` or `/data/touhou/th07/`. A private `--full` mode exists only to simplify local
testing and is deliberately named `FULL-PERSONAL`.

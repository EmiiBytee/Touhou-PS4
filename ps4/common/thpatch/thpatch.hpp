// "thcrap-lite": applies thcrap translation patches (thpatch lang_en etc.) to the decomps
// without binary hacking. Patch files are staged and merged in priority order (preserving
// any per-game subdirectories) and read from there at runtime.
#pragma once

#include <cstdint>

// Call once at startup. `dir` is relative to the game dir (e.g. "patch").
void THPatch_Init(const char *dir);
bool THPatch_Enabled();


// Whole-file replacement: returns a malloc'd copy of <dir>/<name>, or NULL.
uint8_t *THPatch_LoadReplacement(const char *name, uint32_t *size);
// Whether the last THPatch_LoadReplacement() for this name found a file.
bool THPatch_WasReplaced(const char *name);

// Applies a <name>.jdiff to a freshly loaded game file (MSG, ending scripts).
// Takes ownership of `data`; returns the (possibly new, malloc'd) buffer.
uint8_t *THPatch_Transform(const char *name, uint8_t *data, uint32_t *size);

// String tables. Each returns `fallback` when the patch has no entry.
const char *THPatch_StringDef(const char *id, const char *fallback);
const char *THPatch_Spell(int id, const char *fallback);
const char *THPatch_Stage(int stage, int line, const char *fallback);
// BGM titles from themes.js, keyed by the track's file name without extension ("th06_01").
const char *THPatch_Theme(const char *id, const char *fallback);
// Music Room comments from musiccmt.js. `track` is one-based and `line` is zero-based.
const char *THPatch_MusicComment(int track, int line, const char *fallback);

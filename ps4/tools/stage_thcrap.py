#!/usr/bin/env python3
"""Stages a thcrap patch stack into one directory for the PS4 ports' thpatch module.

usage: stage_thcrap.py <thcrap dir> <game id, e.g. th06> <out dir> [overlay dir]

Patches are applied in the order of the thcrap run config (the first config/*.js with a
"patches" list), later patches overriding earlier ones. Game files come from
<patch>/<game>/, and the patch-wide stringdefs.js files are merged key by key.
The game's directory tree is preserved (needed by PCB's replacement images). Files in the
optional overlay dir (PS4-specific edits) are copied last.
"""
import json
import os
import re
import shutil
import struct
import sys
import zlib

SKIP = {"Thumbs.db", "desktop.ini"}
# The executable version each port reconstructs. thcrap names version-specific files
# "<name>.v<version>.<ext>" (IN's title02.v1.00d.png, text.v0.03.anm for the trial): the one
# for this version stands in for the plain name, and those for other versions are left out.
GAME_VERSIONS = {"th06": "1.02h", "th07": "1.00b", "th08": "1.00d"}
VERSIONED = re.compile(r"^(?P<stem>.+)\.v(?P<version>\d+\.\d+[a-z]?)(?P<ext>\.[^.]+)$")
# Patch-wide string tables merged key by key across the stack.
MERGED = ("stringdefs.js", "themes.js")
# thcrap's script_latin renders translations with Arial; so do the ports (as latin.ttf).
LATIN_FONTS = ("/mnt/c/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/arial.ttf")



# --- PNG layering -----------------------------------------------------------------------
# thcrap composites images through the patch stack: a higher patch's PNG usually carries
# only the pieces it changes and leaves the rest transparent. Copying just the top one
# would drop everything the layers below provide -- that is how PCB lost the HUD digits,
# which live in script_latin while lang_en only adds two words.

PNG_MAGIC = bytes((137, 80, 78, 71, 13, 10, 26, 10))


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png_rgba(path):
    """Decodes a PNG into (width, height, bytearray of RGBA). None if unsupported."""
    data = open(path, "rb").read()
    if data[:8] != PNG_MAGIC:
        return None
    pos, idat, plte, trns = 8, bytearray(), None, None
    width = height = depth = color = interlace = 0
    while pos + 8 <= len(data):
        length, ctype = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif ctype == b"PLTE":
            plte = body
        elif ctype == b"tRNS":
            trns = body
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
    if depth != 8 or interlace != 0 or color not in (0, 2, 3, 4, 6):
        return None

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(height * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        filt = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = prev[i]
            upleft = prev[i - channels] if i >= channels else 0
            if filt == 1:
                line[i] = (line[i] + left) & 0xFF
            elif filt == 2:
                line[i] = (line[i] + up) & 0xFF
            elif filt == 3:
                line[i] = (line[i] + ((left + up) >> 1)) & 0xFF
            elif filt == 4:
                line[i] = (line[i] + _paeth(left, up, upleft)) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line

    rgba = bytearray(width * height * 4)
    for i in range(width * height):
        px = out[i * channels:(i + 1) * channels]
        if color == 3:
            idx = px[0]
            r, g, b = plte[idx * 3:idx * 3 + 3] if plte else (0, 0, 0)
            a = trns[idx] if trns and idx < len(trns) else 255
        elif color == 0:
            r = g = b = px[0]
            a = 255
        elif color == 4:
            r = g = b = px[0]
            a = px[1]
        elif color == 2:
            r, g, b = px
            a = 255
        else:
            r, g, b, a = px
        rgba[i * 4:i * 4 + 4] = bytes((r, g, b, a))
    return width, height, rgba


def write_png_rgba(path, width, height, rgba):
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # no filter: these are small and it keeps the writer simple
        raw += rgba[y * width * 4:(y + 1) * width * 4]

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))

    png = PNG_MAGIC
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def layer_png(base_path, top_path, dst_path):
    """Draws top over base, both anchored at the top left. Layers are often different
    sizes: script_latin ships PCB's numerals as a 240x32 strip while lang_en ships a
    256x256 image with two words, and dropping either of them costs the game its digits.
    False if a PNG can't be decoded."""
    base = read_png_rgba(base_path)
    top = read_png_rgba(top_path)
    if not base or not top:
        return False
    width = max(base[0], top[0])
    height = max(base[1], top[1])
    out = bytearray(width * height * 4)
    for src_w, src_h, src in (base, top):
        for y in range(src_h):
            row = y * src_w * 4
            dst_row = y * width * 4
            for x in range(src_w):
                i, j = dst_row + x * 4, row + x * 4
                sa = src[j + 3]
                if sa == 255:
                    out[i:i + 4] = src[j:j + 4]
                elif sa:
                    da = out[i + 3]
                    na = sa + da * (255 - sa) // 255
                    for c in range(3):
                        out[i + c] = (src[j + c] * sa + out[i + c] * da * (255 - sa) // 255) // max(na, 1)
                    out[i + 3] = na
    write_png_rgba(dst_path, width, height, out)
    return True


def find_game_font(patches, game):
    """The font thcrap renders this game's text with: the last "font" set in a
    <patch>/<game>.js, looked up among the font files the patches ship."""
    name = None
    for patch in patches:
        js = os.path.join(patch, game + ".js")
        if os.path.isfile(js):
            name = load_json(js).get("font", name)
    if not name:
        return None
    for patch in reversed(patches):
        for ext in (".ttf", ".otf"):
            path = os.path.join(patch, name + ext)
            if os.path.isfile(path):
                return path
    return None


def fix_th06_text_anm(path):
    """base_tsa's text.anm parks the bomb (script 6) and boss spell card (script 7) name
    sprites off-screen, relying on thcrap binhacks (bomb_pos / spell_pos) to place them.
    The ports align text inside the sprite themselves, so move them back on screen: bomb
    names left-aligned from x=64, spell card names right-aligned up to x=416."""
    data = bytearray(open(path, "rb").read())
    nsprites, nscripts = struct.unpack_from("<ii", data, 0)
    table = 64 + nsprites * 4
    moves = {6: (576.0, 320.0), 7: (-128.0, 160.0)}
    for i in range(nscripts):
        script_id, off = struct.unpack_from("<II", data, table + i * 8)
        if script_id not in moves:
            continue
        old_x, new_x = moves[script_id]
        p = off
        while p + 4 <= len(data):
            _, op, argsize = struct.unpack_from("<hBB", data, p)
            if op in (17, 18, 19, 20) and argsize >= 4 and struct.unpack_from("<f", data, p + 4)[0] == old_x:
                struct.pack_into("<f", data, p + 4, new_x)
            p += 4 + argsize
            if op in (0, 15, 21, 24):
                break
    open(path, "wb").write(data)


def load_json(path):
    with open(path, encoding="utf-8-sig") as f:
        text = f.read()
    text = re.sub(r"(?m)^\s*//.*$", "", text)
    text = re.sub(r",(\s*[}\]])", r"\1", text)
    return json.loads(text)


def patch_order(thcrap):
    config_dir = os.path.join(thcrap, "config")
    for name in sorted(os.listdir(config_dir)):
        if not name.endswith(".js"):
            continue
        try:
            cfg = load_json(os.path.join(config_dir, name))
        except ValueError:
            continue
        if isinstance(cfg, dict) and "patches" in cfg:
            return [os.path.normpath(os.path.join(thcrap, p["archive"])) for p in cfg["patches"]]
    sys.exit("no thcrap run config with a patch list in " + config_dir)


def pe_string_reader(exe_path):
    """Returns a function reading the NUL-terminated string at a relative virtual address."""
    exe = open(exe_path, "rb").read()
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    sections = struct.unpack_from("<H", exe, pe + 6)[0]
    optional = struct.unpack_from("<H", exe, pe + 20)[0]
    table = []
    for i in range(sections):
        o = pe + 24 + optional + i * 40
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", exe, o + 8)
        table.append((vaddr, max(vsize, rsize), raddr))

    def read(rva):
        for vaddr, size, raddr in table:
            if vaddr <= rva < vaddr + size:
                start = rva - vaddr + raddr
                return exe[start:exe.index(b"\0", start)]
        return None
    return read


def write_string_table(out, exe_path):
    """Builds stringtable.js: the executable's own strings, translated.

    thcrap finds the strings it translates by their address in the executable (stringlocs,
    "Rx<rva>" -> stringdefs id). The PS4 ports run recompiled code, so there are no addresses;
    instead each string is read out of the user's executable here and the table maps its
    Shift-JIS bytes (as hex) to the English text. The game's text drawing looks strings up by
    content at runtime. The executable itself never goes into the package.
    """
    locs_path = os.path.join(out, "stringlocs.js")
    defs_path = os.path.join(out, "stringdefs.js")
    if not (os.path.isfile(locs_path) and os.path.isfile(defs_path) and os.path.isfile(exe_path)):
        return
    locs = load_json(locs_path)
    defs = load_json(defs_path)
    read = pe_string_reader(exe_path)
    table = {}
    for loc, key in locs.items():
        match = re.match(r"^Rx([0-9a-fA-F]+)$", loc)
        text = defs.get(key) if match else None
        if not isinstance(text, str) or not text:
            continue
        original = read(int(match.group(1), 16))
        # Entries the patch leaves as they are (names, "？？？？") would only swap in a folded
        # copy of the same text.
        if original and original.decode("cp932", "replace") != text:
            table[original.hex()] = text
    with open(os.path.join(out, "stringtable.js"), "w", encoding="utf-8") as f:
        json.dump(table, f, ensure_ascii=False, indent=1)
    print(f"  executable strings: {len(table)} of {len(locs)} translated")


GAME_EXECUTABLES = {"th08": "th08.exe"}


def load_pbgz_reader():
    """The TH08 archive reader of the local reconstruction tools, when they are present."""
    here = os.path.dirname(os.path.abspath(__file__))
    tools = os.path.normpath(os.path.join(here, "..", "..", "evidence", "th08-psp-native", "tools"))
    if not os.path.isfile(os.path.join(tools, "th08_stock_font_profile.py")):
        return None
    sys.path.insert(0, tools)
    try:
        import th08_stock_font_profile
    except Exception:
        return None
    return th08_stock_font_profile


def msg08_title_boxes(data):
    """IN msg: {entry: {"<time>_h1_<n>": [Shift-JIS line, ...]}}, grouped as thpatch does."""
    count = struct.unpack_from("<i", data, 0)[0]
    offsets = struct.unpack_from("<%dI" % count, data, 4)
    result = {}
    for entry, offset in enumerate(offsets):
        boxes, per_time = {}, {}
        key, last = None, 0
        p = offset
        while p + 4 <= len(data):
            time, op, size = struct.unpack_from("<HBB", data, p)
            args = data[p + 4:p + 4 + size]
            p += 4 + size
            if op in (0, 4, 15):
                key = None
            elif op == 8 and size >= 4:
                line = struct.unpack_from("<h", args, 2)[0]
                if key is None or line <= last:
                    prefix = "%d_h1_" % time
                    key = prefix + str(per_time.get(prefix, 0))
                    per_time[prefix] = per_time.get(prefix, 0) + 1
                    boxes[key] = []
                last = line
                text = bytearray()
                for c in args[4:]:
                    if c ^ 0x77 == 0:
                        break
                    text.append(c ^ 0x77)
                boxes[key].append(bytes(text))
            if op == 0:
                break
        result[str(entry)] = boxes
    return result


MSG_NAME_FALLBACKS = {
    "リグル・ナイトバグ": "Wriggle Nightbug",
    "ミスティア・ローレライ": "Mystia Lorelei",
    "ミスティア・ローラレイ": "Mystia Lorelei",
    "上白沢　慧音": "Keine Kamishirasawa",
    "霊夢": "Reimu",
    "魔理沙": "Marisa",
    "鈴仙": "Reisen",
    "永琳": "Eirin",
    "輝夜": "Kaguya",
}


def write_msg_names(out, dat_path):
    """Builds msgnames.js: boss titles and names (IN's msg opcode 8), Shift-JIS hex -> English.

    lang_en translates these only in some of a stage's four scripts; the same Japanese text in
    the other scripts is filled in from here at runtime. Pairing the patch's lines with the
    Japanese ones needs the game archive and a local TH08 archive reader; without them only
    the character names below are written.
    """
    names = {}
    reader = load_pbgz_reader()
    archive = None
    if reader is not None and os.path.isfile(dat_path):
        archive = reader.PbgzArchive(__import__("pathlib").Path(dat_path))
    for jdiff_name in sorted(os.listdir(out)) if archive is not None else []:
        match = re.match(r"^(msg\w+\.dat)\.jdiff$", jdiff_name)
        if not match:
            continue
        try:
            original = archive.extract(match.group(1))
        except Exception:
            continue
        diff = load_json(os.path.join(out, jdiff_name))
        diff = diff.get("entries", diff)
        for entry, boxes in msg08_title_boxes(original).items():
            translated = diff.get(entry, {})
            for key, lines in boxes.items():
                english = translated.get(key, {}).get("lines", [])
                for jp, en in zip(lines, english):
                    if isinstance(en, str) and en.strip() and jp:
                        names.setdefault(jp.hex(), en)
    # Character names, as the patch spells them; some are translated in no script at all.
    for jp, en in MSG_NAME_FALLBACKS.items():
        names.setdefault(jp.encode("cp932").hex(), en)
    with open(os.path.join(out, "msgnames.js"), "w", encoding="utf-8") as f:
        json.dump(names, f, ensure_ascii=False, indent=1)
    print(f"  boss titles and names: {len(names)}")


def main():
    thcrap, game, out = sys.argv[1:4]
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    merged = {name: {} for name in MERGED}
    patches = patch_order(thcrap)
    for patch in patches:
        game_dir = os.path.join(patch, game)
        count = 0
        layered = 0
        if os.path.isdir(game_dir):
            files = []
            for root, _, names in os.walk(game_dir):
                for name in names:
                    if name in SKIP:
                        continue
                    match = VERSIONED.match(name)
                    if match is None:
                        files.append((False, os.path.join(root, name), name))
                    elif match.group("version") == GAME_VERSIONS.get(game):
                        files.append((True, os.path.join(root, name), match.group("stem") + match.group("ext")))
            # A version-specific file overrides the plain one from the same patch.
            files.sort(key=lambda f: f[0])
            for _, src, name in files:
                rel = os.path.join(os.path.relpath(os.path.dirname(src), game_dir), name)
                dst = os.path.join(out, rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if name.lower().endswith(".png") and os.path.isfile(dst):
                    if layer_png(dst, src, dst):
                        layered += 1
                        count += 1
                        continue
                shutil.copyfile(src, dst)
                count += 1
        for name in MERGED:
            path = os.path.join(patch, name)
            if os.path.isfile(path):
                merged[name].update(load_json(path))
        print(f"  {os.path.basename(patch)}: {count} files" +
              (f" ({layered} layered over lower patches)" if layered else ""))
    for name, table in merged.items():
        with open(os.path.join(out, name), "w", encoding="utf-8") as f:
            json.dump(table, f, ensure_ascii=False, indent=1)
    font = find_game_font(patches, game)
    for candidate in ([font] if font else []) + [f for f in LATIN_FONTS if os.path.isfile(f)]:
        shutil.copyfile(candidate, os.path.join(out, "latin.ttf"))
        print("  latin font: " + candidate)
        break
    if game == "th06" and os.path.isfile(os.path.join(out, "text.anm")):
        fix_th06_text_anm(os.path.join(out, "text.anm"))
    if game in GAME_EXECUTABLES:
        write_string_table(out, os.path.join(thcrap, "..", GAME_EXECUTABLES[game]))
    if game == "th08":
        write_msg_names(out, os.path.join(thcrap, "..", "th08.dat"))
    if len(sys.argv) > 4 and os.path.isdir(sys.argv[4]):
        count = 0
        overlay = sys.argv[4]
        for root, _, names in os.walk(overlay):
            for name in names:
                src = os.path.join(root, name)
                rel = os.path.relpath(src, overlay)
                dst = os.path.join(out, rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copyfile(src, dst)
                count += 1
        print(f"  overlay: {count} files")


if __name__ == "__main__":
    main()

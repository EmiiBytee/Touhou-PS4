#include "thpatch.hpp"

#include "json.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

void PS4_Log(const char *fmt, ...);

using thpatch::Json;

namespace
{
// THPatch_Init() runs from a static constructor on PS4, possibly before this file's
// globals are constructed, so all state lives in a function-local static.
struct State
{
    std::string dir;
    bool enabled = false;
    std::set<std::string> replaced;
    Json stringDefs;
    Json spells;
    Json stages;
    Json themes;
    Json musicComments;
    // The executable's own strings (their Shift-JIS bytes) and their translation, already in
    // the form the game draws. Built by stage_thcrap.py as stringtable.js.
    std::map<std::string, std::string> executableStrings;
    // Boss titles and names from the msg scripts (Shift-JIS bytes -> English as written in the
    // patch), for the scripts whose jdiff leaves them out. Built as msgnames.js.
    std::map<std::string, std::string> msgNames;
};

// Reads a {"<hex bytes>": "text"} table into `out`, keyed by the decoded bytes.
size_t LoadHexTable(const std::string &path, std::map<std::string, std::string> *out,
                    std::string (*convert)(const std::string &))
{
    Json table;
    if (!Json::ParseFile(path, &table) || table.type != Json::Object)
    {
        return 0;
    }
    for (const auto &entry : table.object)
    {
        if (!entry.second.IsString() || entry.first.size() % 2 != 0)
        {
            continue;
        }
        std::string original;
        for (size_t i = 0; i < entry.first.size(); i += 2)
        {
            original += (char)std::strtoul(entry.first.substr(i, 2).c_str(), nullptr, 16);
        }
        (*out)[original] = convert != nullptr ? convert(entry.second.string) : entry.second.string;
    }
    return out->size();
}

State &S()
{
    static State state;
    return state;
}

#define g_Dir (S().dir)
#define g_Enabled (S().enabled)
#define g_Replaced (S().replaced)
#define g_StringDefs (S().stringDefs)
#define g_Spells (S().spells)
#define g_Stages (S().stages)
#define g_Themes (S().themes)
#define g_MusicComments (S().musicComments)

std::string PatchPath(const char *name)
{
    return g_Dir + "/" + name;
}

std::string NormalizeName(const char *name)
{
    std::string out = name != nullptr ? name : "";
    while (out.size() >= 2 && out[0] == '.' && (out[1] == '/' || out[1] == '\\'))
    {
        out.erase(0, 2);
    }
    for (char &c : out)
    {
        if (c == '\\')
        {
            c = '/';
        }
    }
    return out;
}

std::string BaseName(const std::string &name)
{
    const size_t slash = name.find_last_of('/');
    return slash == std::string::npos ? name : name.substr(slash + 1);
}


bool ReadFile(const std::string &path, std::vector<uint8_t> &out)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
    {
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? size : 0);
    size_t got = size > 0 ? std::fread(out.data(), 1, size, f) : 0;
    std::fclose(f);
    return got == out.size();
}

uint8_t *ToMalloc(const std::vector<uint8_t> &bytes, uint32_t *size)
{
    uint8_t *buf = (uint8_t *)std::malloc(bytes.size() > 0 ? bytes.size() : 1);
    if (!bytes.empty())
    {
        std::memcpy(buf, bytes.data(), bytes.size());
    }
    *size = (uint32_t)bytes.size();
    return buf;
}

uint16_t Rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}
uint32_t Rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
void Put16(std::vector<uint8_t> &v, uint16_t x)
{
    v.push_back(x & 0xff);
    v.push_back(x >> 8);
}
void Put32(std::vector<uint8_t> &v, uint32_t x)
{
    for (int i = 0; i < 4; i++)
    {
        v.push_back((x >> (i * 8)) & 0xff);
    }
}

// ---------------------------------------------------------------------------------------
// MSG
//
// File: i32 entryCount; u32 entryOffsets[entryCount]; then per entry a stream of
//   { u16 time; u8 opcode; u8 argSize; u8 args[argSize] } ending with opcode 0.
// The container is the same in EoSD/PCB ("msg06") and IN ("msg08"). What differs is which
// opcodes carry text, and that IN XORs every byte of its text with 0x77, terminator included:
//
//   msg06: 3 (dialogue) and 8 (boss intro title, "h1") carry { i16 color; i16 line; char text[] },
//          the row in `line`. Boxes end at 0, 4 and 13.
//   msg08: 3 carries { i16 color; i16 line; text } as above; 16 (speaker), 19 and 20 (top and
//          bottom lines) carry only { text } and fill rows in order. Boxes end at 0, 4 and 15.
//          IN's opcode 8 shows the boss title as an image, not text.
//
// thcrap groups text lines into boxes and keys each box "<time>_<n>" (or "<time>_h1_<n>"),
// where time is the time of the box's first line and n counts boxes starting at that time.
// A box ends at one of the opcodes above, at the end of the entry, or when a line with an
// explicit row does not go below the previous one. The opcode tables are thcrap's own
// (base_tsa/formats.js).
// ---------------------------------------------------------------------------------------

#ifdef TH_THPATCH_MSG08
constexpr bool kMsg08 = true;
#else
constexpr bool kMsg08 = false;
#endif

struct MsgInstr
{
    uint16_t time;
    uint8_t opcode;
    std::vector<uint8_t> args;
};

enum class MsgOp
{
    Other,
    HardLine,   // { i16 color; i16 line; text }
    HardLineH1, // the same, boxed separately as "h1"
    AutoLine,   // { text }, next row
    BoxEnd,
};

MsgOp ClassifyMsgOp(uint8_t op)
{
    if (op == 0 || op == 4) return MsgOp::BoxEnd;
    if (op == 3) return MsgOp::HardLine;
    if (op == 8) return MsgOp::HardLineH1; // boss title and name, both games
    if (kMsg08)
    {
        if (op == 16 || op == 19 || op == 20) return MsgOp::AutoLine;
        if (op == 15) return MsgOp::BoxEnd;
    }
    else
    {
        if (op == 13) return MsgOp::BoxEnd;
    }
    return MsgOp::Other;
}

// IN draws its script text as Shift-JIS. The English lines are plain ASCII apart from a few
// typographic characters, which are folded to their ASCII forms: passed through as UTF-8,
// their bytes would be read as Japanese.
std::string ToGameText(const std::string &utf8)
{
    std::string out;
    for (size_t i = 0; i < utf8.size();)
    {
        const unsigned char c = (unsigned char)utf8[i];
        if (c < 0x80)
        {
            out += (char)c;
            i++;
            continue;
        }
        size_t len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        uint32_t cp = len == 1 ? 0 : c & (0x7F >> len);
        for (size_t k = 1; k < len && i + k < utf8.size(); k++)
        {
            cp = (cp << 6) | ((unsigned char)utf8[i + k] & 0x3F);
        }
        i += len;
        switch (cp)
        {
        case 0x2018: case 0x2019: out += '\''; break;
        case 0x201C: case 0x201D: out += '"'; break;
        case 0x2013: out += '-'; break;
        case 0x2014: out += "--"; break;
        case 0x2026: out += "..."; break;
        case 0x3000: out += "\x81\x40"; break; // ideographic space, as Shift-JIS
        case 0x00A0: out += ' '; break;
        case 0x00D7: out += "\x81\x7e"; break; // the rest are Shift-JIS symbols
        case 0x2190: out += "\x81\xa9"; break;
        case 0x2191: out += "\x81\xaa"; break;
        case 0x2192: out += "\x81\xa8"; break;
        case 0x2193: out += "\x81\xab"; break;
        case 0x2605: out += "\x81\x9a"; break;
        case 0x2606: out += "\x81\x99"; break;
        case 0x266A: out += "\x81\xf4"; break;
        case 0x30FB: out += "\x81\x45"; break;
        default:
            // Full-width ASCII (（ ） ／ ？ ０-９ ...) as its plain form, which the Latin
            // font draws; anything else has no form the game can show.
            out += cp >= 0xFF01 && cp <= 0xFF5E ? (char)(cp - 0xFEE0) : '?';
            break;
        }
    }
    return out;
}

void AppendMsgText(std::vector<uint8_t> &args, const std::string &text)
{
    const std::string game = kMsg08 ? ToGameText(text) : text;
    for (char ch : game)
    {
        args.push_back(kMsg08 ? (uint8_t)((uint8_t)ch ^ 0x77) : (uint8_t)ch);
    }
    args.push_back(kMsg08 ? 0x77 : 0);
}

size_t Utf8Length(const std::string &s)
{
    size_t n = 0;
    for (unsigned char c : s)
    {
        n += (c & 0xC0) != 0x80;
    }
    return n;
}

// thcrap line markup -> plain text the game's renderer can draw:
//   <c$text$ref>  text centered over the width of ref (padded with spaces)
//   <i$text>      italics (not supported by the renderer: kept as plain text)
//   <l$text>, <tl$text>  a label in a tabulated line (the statistics screen): the label,
//                 padded so the columns after it roughly line up
std::string FormatLine(const std::string &in)
{
    // thcrap appends translator notes after an 0x14 control byte. The full thcrap
    // runtime handles those separately; the lite renderer must not draw them as dialogue.
    const size_t note = in.find('\x14');
    const std::string source = in.substr(0, note);
    std::string out;
    size_t pos = 0;
    while (pos < source.size())
    {
        size_t open = source.find('<', pos);
        // The tag name is one to three lowercase letters followed by '$'.
        size_t dollar = open == std::string::npos ? source.size() : open + 1;
        while (dollar < source.size() && dollar - open <= 3 && source[dollar] >= 'a' && source[dollar] <= 'z')
        {
            dollar++;
        }
        if (open == std::string::npos || dollar == open + 1 || dollar >= source.size() || source[dollar] != '$')
        {
            out.append(source, pos,
                       open == std::string::npos ? std::string::npos : open + 1 - pos);
            pos = open == std::string::npos ? source.size() : open + 1;
            continue;
        }
        size_t close = source.find('>', open);
        if (close == std::string::npos)
        {
            out.append(source, pos, std::string::npos);
            break;
        }
        out.append(source, pos, open - pos);
        const std::string kind = source.substr(open + 1, dollar - open - 1);
        std::string body = source.substr(dollar + 1, close - dollar - 1);
        size_t sep = body.find('$');
        if (kind == "l" || kind == "tl")
        {
            std::string label = sep != std::string::npos ? body.substr(0, sep) : body;
            while (!label.empty() && (label.back() == '\t' || label.back() == ' '))
            {
                label.pop_back();
            }
            out += label;
            const size_t len = Utf8Length(label);
            out.append(len < 14 ? 14 - len : 1, ' ');
        }
        else if (kind == "c" && sep != std::string::npos)
        {
            std::string text = body.substr(0, sep);
            size_t width = Utf8Length(body.substr(sep + 1));
            size_t len = Utf8Length(text);
            size_t pad = width > len ? width - len : 0;
            out.append(pad / 2, ' ');
            out += text;
            out.append(pad - pad / 2, ' ');
        }
        else
        {
            out += sep != std::string::npos ? body.substr(0, sep) : body;
        }
        pos = close + 1;
    }
    return out;
}

std::vector<std::string> WrapDialogueLine(const std::string &raw)
{
    constexpr size_t kMaxLogicalChars = 40;
    std::string text = FormatLine(raw);
    if (Utf8Length(text) <= kMaxLogicalChars)
    {
        return {text};
    }

    size_t best = std::string::npos;
    size_t bestImbalance = (size_t)-1;
    for (size_t space = text.find(' '); space != std::string::npos;
         space = text.find(' ', space + 1))
    {
        size_t left = Utf8Length(text.substr(0, space));
        size_t rightStart = text.find_first_not_of(' ', space);
        if (rightStart == std::string::npos)
        {
            continue;
        }
        size_t right = Utf8Length(text.substr(rightStart));
        if (left <= kMaxLogicalChars && right <= kMaxLogicalChars)
        {
            size_t imbalance = left > right ? left - right : right - left;
            if (imbalance < bestImbalance)
            {
                best = space;
                bestImbalance = imbalance;
            }
        }
    }

    // Extremely long unbreakable text is left alone; shrinking it is preferable to
    // generating an invalid third dialogue row (PCB only owns two row sprites).
    if (best == std::string::npos)
    {
        return {text};
    }
    size_t rightStart = text.find_first_not_of(' ', best);
    return {text.substr(0, best), text.substr(rightStart)};
}

std::vector<uint8_t> MakeTextArgs(MsgOp kind, int16_t color, int16_t line, const std::string &rawText)
{
    std::string text = FormatLine(rawText);
    std::vector<uint8_t> args;
    if (kind != MsgOp::AutoLine)
    {
        Put16(args, (uint16_t)color);
        Put16(args, (uint16_t)line);
    }
    AppendMsgText(args, text);
    return args;
}

std::vector<MsgInstr> PatchMsgEntry(const std::vector<MsgInstr> &in, const Json &diff)
{
    // Pass 1: assign every text instruction to a box.
    struct Box
    {
        std::string key;
        std::vector<size_t> instrs;
    };
    std::vector<Box> boxes;
    std::vector<int> boxOf(in.size(), -1);
    std::map<std::string, int> boxesAtTime;
    int openBox[3] = {-1, -1, -1}; // per type: dialogue, h1, IN's explicit rows
    int lastLine[3] = {0, 0, 0};

    for (size_t i = 0; i < in.size(); i++)
    {
        const MsgInstr &ins = in[i];
        const MsgOp kind = ClassifyMsgOp(ins.opcode);
        if (kind == MsgOp::BoxEnd)
        {
            // IN's explicit rows outlive the box ends between them: thcrap keys a row that
            // comes back after a pause (Wriggle's "I was here!" once she shows up) to the box
            // the first row opened, and only a row that does not go further down starts anew.
            openBox[0] = openBox[1] = -1;
            if (!kMsg08) openBox[2] = -1;
            continue;
        }
        if (kind == MsgOp::Other || (kind != MsgOp::AutoLine && ins.args.size() < 4))
        {
            continue;
        }
        int type = kind == MsgOp::HardLineH1 ? 1 : (kMsg08 && kind == MsgOp::HardLine ? 2 : 0);
        bool newBox = openBox[type] < 0;
        if (kind != MsgOp::AutoLine)
        {
            // An explicit row that does not go below the previous one starts a new box; auto
            // lines just fill the next row of the current one.
            int line = (int16_t)Rd16(&ins.args[2]);
            newBox = newBox || line <= lastLine[type];
            lastLine[type] = line;
        }
        if (newBox)
        {
            std::string prefix = std::to_string(ins.time) + (type == 1 ? "_h1_" : "_");
            int n = boxesAtTime[prefix]++;
            boxes.push_back({prefix + std::to_string(n), {}});
            openBox[type] = (int)boxes.size() - 1;
        }
        boxes[openBox[type]].instrs.push_back(i);
        boxOf[i] = openBox[type];
    }

    // The Windows thcrap runtime applies its own text layout hooks. The native port does
    // not have those hooks, so wrap only translated one-line dialogue boxes into PCB's
    // second built-in row. Existing two-line layouts and boss-introduction text stay exact.
    std::vector<std::vector<std::string>> translated(boxes.size());
    for (size_t b = 0; b < boxes.size(); b++)
    {
        const Json *tl = diff.Get(boxes[b].key);
        const Json *lines = tl != nullptr ? tl->Get("lines") : nullptr;
        if (lines == nullptr || lines->type != Json::Array)
        {
            continue;
        }
        for (const Json &line : lines->array)
        {
            translated[b].push_back(line.IsString() ? FormatLine(line.string) : "");
        }
        // PCB only: IN's English lines are written to fit its (base_tsa-widened) rows.
        if (!kMsg08 && translated[b].size() == 1 && !boxes[b].instrs.empty() &&
            in[boxes[b].instrs[0]].opcode == 3)
        {
            translated[b] = WrapDialogueLine(translated[b][0]);
        }
    }

    // Pass 2: emit, replacing translated boxes line by line.
    std::vector<MsgInstr> out;
    std::vector<size_t> posInBox(boxes.size(), 0);
    for (size_t i = 0; i < in.size(); i++)
    {
        int b = boxOf[i];
        if (b >= 0 && translated[b].empty() && ClassifyMsgOp(in[i].opcode) == MsgOp::HardLineH1 &&
            in[i].args.size() > 4 && !S().msgNames.empty())
        {
            // A boss title or name this script's jdiff leaves out: the same Japanese text is
            // translated in another script of the stage (msgnames.js).
            std::string original;
            for (size_t k = 4; k < in[i].args.size(); k++)
            {
                const char ch = kMsg08 ? (char)(in[i].args[k] ^ 0x77) : (char)in[i].args[k];
                if (ch == 0) break;
                original += ch;
            }
            const auto found = S().msgNames.find(original);
            if (found != S().msgNames.end())
            {
                const MsgInstr &ins = in[i];
                out.push_back({ins.time, ins.opcode,
                               MakeTextArgs(MsgOp::HardLineH1, (int16_t)Rd16(&ins.args[0]),
                                            (int16_t)Rd16(&ins.args[2]), found->second)});
                continue;
            }
        }
        if (b < 0 || translated[b].empty())
        {
            out.push_back(in[i]);
            continue;
        }

        const MsgInstr &ins = in[i];
        const MsgOp kind = ClassifyMsgOp(ins.opcode);
        int16_t color = kind == MsgOp::AutoLine ? 0 : (int16_t)Rd16(&ins.args[0]);
        size_t k = posInBox[b]++;
        const std::string text = k < translated[b].size() ? translated[b][k] : "";
        out.push_back({ins.time, ins.opcode, MakeTextArgs(kind, color, (int16_t)k, text)});

        // The translation may need more lines than the original box had.
        if (k + 1 == boxes[b].instrs.size())
        {
            for (size_t extra = k + 1; extra < translated[b].size(); extra++)
            {
                out.push_back({ins.time, ins.opcode,
                               MakeTextArgs(kind, color, (int16_t)extra, translated[b][extra])});
            }
        }
    }
    return out;
}

uint8_t *PatchMsg(const Json &jdiff, uint8_t *data, uint32_t *size)
{
    if (*size < 4)
    {
        return data;
    }
    int32_t count = (int32_t)Rd32(data);
    if (count <= 0 || 4 + (uint64_t)count * 4 > *size)
    {
        return data;
    }

    std::vector<uint8_t> out(4 + count * 4, 0);
    std::memcpy(out.data(), data, 4);
    std::map<uint32_t, uint32_t> relocated; // entries may share an offset

    for (int32_t e = 0; e < count; e++)
    {
        uint32_t off = Rd32(data + 4 + e * 4);
        uint32_t newOff;
        auto it = relocated.find(off);
        if (it != relocated.end())
        {
            newOff = it->second;
        }
        else
        {
            std::vector<MsgInstr> instrs;
            uint32_t p = off;
            while (p + 4 <= *size)
            {
                MsgInstr ins{Rd16(data + p), data[p + 2], {}};
                uint8_t argSize = data[p + 3];
                if (p + 4 + argSize > *size)
                {
                    break;
                }
                ins.args.assign(data + p + 4, data + p + 4 + argSize);
                instrs.push_back(std::move(ins));
                p += 4 + argSize;
                if (instrs.back().opcode == 0)
                {
                    break;
                }
            }

            const Json *diff = jdiff.Get(std::to_string(e));
            if (diff != nullptr)
            {
                instrs = PatchMsgEntry(instrs, *diff);
            }

            newOff = (uint32_t)out.size();
            for (const MsgInstr &ins : instrs)
            {
                // Text longer than 255 bytes cannot be encoded; thcrap lines never get close.
                size_t argSize = ins.args.size() > 255 ? 255 : ins.args.size();
                Put16(out, ins.time);
                out.push_back(ins.opcode);
                out.push_back((uint8_t)argSize);
                out.insert(out.end(), ins.args.begin(), ins.args.begin() + argSize);
            }
            relocated[off] = newOff;
        }
        std::memcpy(&out[4 + e * 4], &newOff, 4);
    }

    std::free(data);
    return ToMalloc(out, size);
}

bool EndsWith(const std::string &s, const char *suffix)
{
    size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool StartsWith(const std::string &s, const char *prefix)
{
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

void LoadJson(const char *name, Json *out)
{
    if (Json::ParseFile(PatchPath(name), out))
    {
        PS4_Log("thpatch: loaded %s", name);
    }
}
} // namespace

void THPatch_Init(const char *dir)
{
    g_Dir = dir;
    Json probe;
    g_Enabled = Json::ParseFile(PatchPath("stringdefs.js"), &g_StringDefs) ||
                Json::ParseFile(PatchPath("spells.js"), &probe);
    if (!g_Enabled)
    {
        PS4_Log("thpatch: no patch data in %s", dir);
        return;
    }
    LoadJson("spells.js", &g_Spells);
    LoadJson("stages.js", &g_Stages);
    LoadJson("themes.js", &g_Themes);
    LoadJson("musiccmt.js", &g_MusicComments);

    if (LoadHexTable(PatchPath("stringtable.js"), &S().executableStrings,
                     [](const std::string &text) { return ToGameText(FormatLine(text)); }) > 0)
    {
        PS4_Log("thpatch: %u executable strings", (unsigned)S().executableStrings.size());
    }
    if (LoadHexTable(PatchPath("msgnames.js"), &S().msgNames, nullptr) > 0)
    {
        PS4_Log("thpatch: %u boss titles and names", (unsigned)S().msgNames.size());
    }
    PS4_Log("thpatch: enabled (%s)", dir);
}

const char *THPatch_TranslateText(const char *text)
{
    if (!g_Enabled || text == nullptr || S().executableStrings.empty())
    {
        return text;
    }
    const auto found = S().executableStrings.find(text);
    return found != S().executableStrings.end() ? found->second.c_str() : text;
}

bool THPatch_Enabled()
{
    return g_Enabled;
}

uint8_t *THPatch_LoadReplacement(const char *name, uint32_t *size)
{
    if (!g_Enabled)
    {
        return nullptr;
    }
    const std::string normalized = NormalizeName(name);
    const std::string base = BaseName(normalized);
    std::vector<uint8_t> bytes;
    if (!ReadFile(PatchPath(normalized.c_str()), bytes) &&
        (base == normalized || !ReadFile(PatchPath(base.c_str()), bytes)))
    {
        g_Replaced.erase(normalized);
        g_Replaced.erase(base);
        return nullptr;
    }
    g_Replaced.insert(normalized);
    g_Replaced.insert(base);
    PS4_Log("thpatch: replaced %s (%u bytes)", normalized.c_str(), (unsigned)bytes.size());
    return ToMalloc(bytes, size);
}

bool THPatch_WasReplaced(const char *name)
{
    const std::string normalized = NormalizeName(name);
    return g_Replaced.count(normalized) != 0 || g_Replaced.count(BaseName(normalized)) != 0;
}

uint8_t *THPatch_Transform(const char *name, uint8_t *data, uint32_t *size)
{
    if (!g_Enabled || data == nullptr)
    {
        return data;
    }
    std::string n = NormalizeName(name);
    std::string base = BaseName(n);
    Json jdiff;
    if (!Json::ParseFile(PatchPath((n + ".jdiff").c_str()), &jdiff) &&
        (base == n || !Json::ParseFile(PatchPath((base + ".jdiff").c_str()), &jdiff)))
    {
        return data;
    }
    if (StartsWith(base, "msg") && EndsWith(base, ".dat"))
    {
        PS4_Log("thpatch: patching dialogue %s", name);
        return PatchMsg(jdiff, data, size);
    }
    return data;
}

const char *THPatch_StringDef(const char *id, const char *fallback)
{
    const Json *s = g_StringDefs.Get(id);
    return s != nullptr && s->IsString() ? s->string.c_str() : fallback;
}

const char *THPatch_Spell(int id, const char *fallback)
{
    const Json *s = g_Spells.Get(std::to_string(id));
    return s != nullptr && s->IsString() ? s->string.c_str() : fallback;
}

const char *THPatch_GameText(const char *text, const char *fallback)
{
    if (text == nullptr || text[0] == '\0')
    {
        return fallback;
    }
    static std::map<std::string, std::string> cache;
    const auto cached = cache.find(text);
    if (cached != cache.end())
    {
        return cached->second.c_str();
    }
    return cache.emplace(text, ToGameText(FormatLine(text))).first->second.c_str();
}

const char *THPatch_SpellText(int id, const char *fallback)
{
    static std::map<int, std::string> cache;
    const auto cached = cache.find(id);
    if (cached != cache.end())
    {
        return cached->second.c_str();
    }
    const Json *s = g_Spells.Get(std::to_string(id));
    if (s == nullptr || !s->IsString() || s->string.empty())
    {
        return fallback;
    }
    return cache.emplace(id, ToGameText(FormatLine(s->string))).first->second.c_str();
}

const char *THPatch_Theme(const char *id, const char *fallback)
{
    const Json *s = g_Themes.Get(id);
    return s != nullptr && s->IsString() ? s->string.c_str() : fallback;
}

const char *THPatch_MusicComment(int track, int line, const char *fallback)
{
    const Json *entry = g_MusicComments.Get(std::to_string(track));
    // Element 0 is the original musiccmt marker ("@"); translated prose starts at 1.
    const Json *s = entry != nullptr ? entry->At((size_t)line + 1) : nullptr;
    return s != nullptr && s->IsString() ? s->string.c_str() : fallback;
}

const char *THPatch_Stage(int stage, int line, const char *fallback)
{
    const Json *s = g_Stages.Get(std::to_string(stage));
    if (s == nullptr)
    {
        return fallback;
    }
    if (s->IsString())
    {
        return line == 0 ? s->string.c_str() : fallback;
    }
    const Json *l = s->At(line);
    return l != nullptr && l->IsString() ? l->string.c_str() : fallback;
}

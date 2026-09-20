#include "json.hpp"

#include <cstdio>
#include <cstdlib>

namespace thpatch
{

const Json *Json::Get(const std::string &key) const
{
    if (type != Object)
    {
        return nullptr;
    }
    for (const auto &[k, v] : object)
    {
        if (k == key)
        {
            return &v;
        }
    }
    return nullptr;
}

const Json *Json::At(size_t index) const
{
    return type == Array && index < array.size() ? &array[index] : nullptr;
}

namespace
{
struct Parser
{
    const std::string &s;
    size_t pos = 0;

    void SkipWs()
    {
        while (pos < s.size())
        {
            char c = s[pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            {
                pos++;
            }
            else if (c == '/' && pos + 1 < s.size() && s[pos + 1] == '/')
            {
                while (pos < s.size() && s[pos] != '\n')
                {
                    pos++;
                }
            }
            else if (c == '/' && pos + 1 < s.size() && s[pos + 1] == '*')
            {
                size_t end = s.find("*/", pos + 2);
                pos = end == std::string::npos ? s.size() : end + 2;
            }
            else
            {
                break;
            }
        }
    }

    static void AppendUtf8(std::string &out, unsigned cp)
    {
        if (cp < 0x80)
        {
            out += (char)cp;
        }
        else if (cp < 0x800)
        {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else
        {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    unsigned Hex4()
    {
        if (pos + 4 > s.size())
        {
            return 0;
        }
        unsigned v = (unsigned)std::strtoul(s.substr(pos, 4).c_str(), nullptr, 16);
        pos += 4;
        return v;
    }

    bool ParseString(std::string &out)
    {
        pos++; // opening quote
        while (pos < s.size())
        {
            char c = s[pos++];
            if (c == '"')
            {
                return true;
            }
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (pos >= s.size())
            {
                return false;
            }
            char e = s[pos++];
            switch (e)
            {
            case 'n':
                out += '\n';
                break;
            case 't':
                out += '\t';
                break;
            case 'r':
                out += '\r';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'u': {
                unsigned cp = Hex4();
                if (cp >= 0xD800 && cp < 0xDC00 && pos + 1 < s.size() && s[pos] == '\\' && s[pos + 1] == 'u')
                {
                    pos += 2;
                    unsigned lo = Hex4();
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                AppendUtf8(out, cp);
                break;
            }
            default:
                out += e;
            }
        }
        return false;
    }

    bool ParseValue(Json &out)
    {
        SkipWs();
        if (pos >= s.size())
        {
            return false;
        }
        char c = s[pos];
        if (c == '{')
        {
            out.type = Json::Object;
            pos++;
            for (;;)
            {
                SkipWs();
                if (pos < s.size() && s[pos] == '}')
                {
                    pos++;
                    return true;
                }
                if (pos >= s.size() || s[pos] != '"')
                {
                    return false;
                }
                std::string key;
                if (!ParseString(key))
                {
                    return false;
                }
                SkipWs();
                if (pos >= s.size() || s[pos] != ':')
                {
                    return false;
                }
                pos++;
                Json value;
                if (!ParseValue(value))
                {
                    return false;
                }
                out.object.emplace_back(std::move(key), std::move(value));
                SkipWs();
                if (pos < s.size() && s[pos] == ',')
                {
                    pos++;
                }
            }
        }
        if (c == '[')
        {
            out.type = Json::Array;
            pos++;
            for (;;)
            {
                SkipWs();
                if (pos < s.size() && s[pos] == ']')
                {
                    pos++;
                    return true;
                }
                Json value;
                if (!ParseValue(value))
                {
                    return false;
                }
                out.array.push_back(std::move(value));
                SkipWs();
                if (pos < s.size() && s[pos] == ',')
                {
                    pos++;
                }
            }
        }
        if (c == '"')
        {
            out.type = Json::String;
            return ParseString(out.string);
        }
        if (s.compare(pos, 4, "true") == 0)
        {
            out.type = Json::Bool;
            out.boolean = true;
            pos += 4;
            return true;
        }
        if (s.compare(pos, 5, "false") == 0)
        {
            out.type = Json::Bool;
            pos += 5;
            return true;
        }
        if (s.compare(pos, 4, "null") == 0)
        {
            out.type = Json::Null;
            pos += 4;
            return true;
        }
        char *end = nullptr;
        out.number = std::strtod(s.c_str() + pos, &end);
        if (end == s.c_str() + pos)
        {
            return false;
        }
        out.type = Json::Number;
        pos = end - s.c_str();
        return true;
    }
};
} // namespace

bool Json::Parse(const std::string &text, Json *out)
{
    Parser p{text};
    // Skip a UTF-8 BOM.
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0)
    {
        p.pos = 3;
    }
    Json result;
    if (!p.ParseValue(result))
    {
        *out = Json();
        return false;
    }
    *out = std::move(result);
    return true;
}

bool Json::ParseFile(const std::string &path, Json *out)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
    {
        return false;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
    {
        text.append(buf, n);
    }
    std::fclose(f);
    return Parse(text, out);
}

} // namespace thpatch

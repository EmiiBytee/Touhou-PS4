// Minimal JSON reader for thcrap patch files (.js / .jdiff).
// Accepts what thcrap accepts in practice: // and /* */ comments and trailing commas.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace thpatch
{

struct Json
{
    enum Type
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };

    Type type = Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Json> array;
    // Keys keep file order, which some formats (e.g. musiccmt) rely on.
    std::vector<std::pair<std::string, Json>> object;

    const Json *Get(const std::string &key) const;
    const Json *At(size_t index) const;
    bool IsString() const
    {
        return type == String;
    }

    // Parses text; returns false (and leaves *out as Null) on malformed input.
    static bool Parse(const std::string &text, Json *out);
    // Reads and parses a file through fopen (so the PS4 path rebasing applies).
    static bool ParseFile(const std::string &path, Json *out);
};

} // namespace thpatch

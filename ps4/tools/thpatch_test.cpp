// Host test for the thpatch module: applies a patch dir to an extracted game file.
// usage: thpatch_test <patch dir> <file> <out>
#include "thpatch.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

void PS4_Log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
}

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr, "usage: %s <patch dir> <file> <out>\n", argv[0]);
        return 1;
    }
    THPatch_Init(argv[1]);
    FILE *f = std::fopen(argv[2], "rb");
    if (f == nullptr)
    {
        return 1;
    }
    std::fseek(f, 0, SEEK_END);
    uint32_t size = (uint32_t)std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)std::malloc(size);
    std::fread(data, 1, size, f);
    std::fclose(f);

    const char *base = std::strrchr(argv[2], '/');
    base = base != nullptr ? base + 1 : argv[2];
    data = THPatch_Transform(base, data, &size);

    f = std::fopen(argv[3], "wb");
    std::fwrite(data, 1, size, f);
    std::fclose(f);
    std::free(data);
    std::printf("spell 0: %s\nbomb: %s\n", THPatch_Spell(0, "(none)"), THPatch_StringDef("th06 Bomb Reimu A", "(none)"));
    return 0;
}

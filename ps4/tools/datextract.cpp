// Host tool: dumps every entry of an EoSD PBG3 archive (.DAT) using the decomp's own reader.
// Build: g++ -std=c++20 -I src/th06/src ps4/tools/datextract.cpp src/th06/src/pbg3/*.cpp -o datextract
// Usage: datextract <archive.DAT> <outdir>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

#define private public
#include "pbg3/Pbg3Archive.hpp"
#undef private
#include "FileSystem.hpp"

// FileAbstraction.cpp only needs this piece of FileSystem.
FILE *FileSystem::FopenUTF8(const char *filepath, const char *mode)
{
    return std::fopen(filepath, mode);
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: %s <archive.DAT> <outdir>\n", argv[0]);
        return 1;
    }
    Pbg3Archive archive;
    if (!archive.Load(argv[1]))
    {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 1;
    }
    mkdir(argv[2], 0755);
    for (u32 i = 0; i < archive.numOfEntries; i++)
    {
        const char *name = archive.entries[i].filename;
        u8 *data = archive.ReadDecompressEntry(i, name);
        u32 size = archive.GetEntrySize(i);
        std::string out = std::string(argv[2]) + "/" + name;
        FILE *f = std::fopen(out.c_str(), "wb");
        if (data != NULL && f != NULL)
        {
            std::fwrite(data, 1, size, f);
            std::printf("%8u %s\n", size, name);
        }
        if (f != NULL)
        {
            std::fclose(f);
        }
        std::free(data);
    }
    return 0;
}

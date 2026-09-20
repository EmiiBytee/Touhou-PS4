#pragma once

#include <cstdio>

#include "inttypes.hpp"

extern const u32 g_SeekModes[3];
extern const char *g_AccessModes[3];

// VTABLE: TH07 0x0049526c
struct IPbg4File
{
    IPbg4File()
    {
    }

    // SYNTHETIC: TH07 0x0047e90b
    // _purecall

    virtual bool Open(const char *path, const char *mode) = 0;
    virtual void Close() = 0;
    virtual u32 Read(void *data, u32 len) = 0;
    virtual bool Write(void *data, u32 len) = 0;
    virtual u32 Tell() = 0;
    virtual u32 GetSize() = 0;
    virtual bool Seek(u32 offset, u32 seekFrom) = 0;
    virtual ~IPbg4File()
    {
    }

    // SYNTHETIC: TH07 0x0045e520
    // IPbg4File::`scalar deleting destructor'
};

// VTABLE: TH07 0x00495248
struct Pbg4File : IPbg4File
{
    Pbg4File();
    virtual ~Pbg4File();

    // SYNTHETIC: TH07 0x0045e590
    // Pbg4File::`scalar deleting destructor'

    virtual bool Open(const char *path, const char *mode);
    virtual void Close();
    virtual u32 Read(void *data, u32 len);
    virtual bool Write(void *data, u32 len);
    virtual u32 Tell();
    virtual u32 GetSize();
    virtual bool Seek(u32 offset, u32 seekFrom);
    virtual void *ReadRemaining(u32 max);

    static void GetFullPath(char *out, const char *filename);

    FILE *file;
    const char *access;
};

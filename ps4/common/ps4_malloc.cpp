// Replacement heap for the OpenOrbis libc (linked with --wrap for the malloc family).
//
// The OpenOrbis malloc lazily maps a fixed 2.5 GB of *flexible* memory. That works for a
// mini app, but a real game (param.sfo category "gd", launched as a big app) gets a much
// smaller flexible memory budget: the mapping fails, malloc carries on with a NULL mspace
// and the first allocation crashes. Games are meant to use *direct* memory instead, so
// this heap lives in a block of direct memory and is managed with the system's mspace API.

#include <orbis/libkernel.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

void PS4_Notify(const char *message);

extern "C"
{
    // SceLibcInternal mspace API (the OpenOrbis headers declare these without prototypes).
    void *sceLibcMspaceCreate(const char *name, void *base, size_t capacity, uint32_t flag);
    void *sceLibcMspaceMalloc(void *msp, size_t size);
    int sceLibcMspaceFree(void *msp, void *ptr);
    void *sceLibcMspaceCalloc(void *msp, size_t count, size_t size);
    void *sceLibcMspaceRealloc(void *msp, void *ptr, size_t size);
    void *sceLibcMspaceMemalign(void *msp, size_t alignment, size_t size);
}

namespace
{
constexpr int kMemoryTypeWbOnion = 0; // SCE_KERNEL_WB_ONION: CPU cached
constexpr int kProtCpuReadWrite = 0x03;
constexpr size_t kAlignment = 2 * 1024 * 1024;
// Tried in order; EoSD/PCB need far less than the smallest.
constexpr size_t kHeapSizes[] = {512u << 20, 256u << 20, 128u << 20};

void *g_Mspace;

void *Heap()
{
    if (g_Mspace != nullptr)
    {
        return g_Mspace;
    }
    for (size_t size : kHeapSizes)
    {
        off_t phys = 0;
        if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, kAlignment, kMemoryTypeWbOnion,
                                          &phys) < 0)
        {
            continue;
        }
        void *base = nullptr;
        if (sceKernelMapDirectMemory(&base, size, kProtCpuReadWrite, 0, phys, kAlignment) < 0)
        {
            continue;
        }
        g_Mspace = sceLibcMspaceCreate("th-ps4 heap", base, size, 0);
        if (g_Mspace != nullptr)
        {
            return g_Mspace;
        }
    }
    PS4_Notify("Touhou: could not allocate memory");
    return nullptr;
}
} // namespace

extern "C"
{
    void *__wrap_malloc(size_t size)
    {
        void *heap = Heap();
        return heap != nullptr ? sceLibcMspaceMalloc(heap, size) : nullptr;
    }

    void __wrap_free(void *ptr)
    {
        if (ptr != nullptr && g_Mspace != nullptr)
        {
            sceLibcMspaceFree(g_Mspace, ptr);
        }
    }

    void *__wrap_calloc(size_t count, size_t size)
    {
        void *heap = Heap();
        return heap != nullptr ? sceLibcMspaceCalloc(heap, count, size) : nullptr;
    }

    void *__wrap_realloc(void *ptr, size_t size)
    {
        void *heap = Heap();
        return heap != nullptr ? sceLibcMspaceRealloc(heap, ptr, size) : nullptr;
    }

    void *__wrap_aligned_alloc(size_t alignment, size_t size)
    {
        void *heap = Heap();
        return heap != nullptr ? sceLibcMspaceMemalign(heap, alignment, size) : nullptr;
    }

    void *__wrap_memalign(size_t alignment, size_t size)
    {
        return __wrap_aligned_alloc(alignment, size);
    }

    // The toolchain's stubs don't export sceLibcMspacePosixMemalign.
    int __wrap_posix_memalign(void **ptr, size_t alignment, size_t size)
    {
        if (alignment == 0 || (alignment & (alignment - 1)) != 0 || alignment % sizeof(void *) != 0)
        {
            return EINVAL;
        }
        void *p = __wrap_aligned_alloc(alignment, size);
        if (p == nullptr)
        {
            return ENOMEM;
        }
        *ptr = p;
        return 0;
    }

    // With the references wrapped nothing may pull libc's versions in anymore, but the
    // wrapping leaves the names undefined in the symbol table, which create-fself rejects.
    // Weak, so a definition the program already links (e.g. from libc++) takes precedence.
    __attribute__((weak)) void *aligned_alloc(size_t alignment, size_t size)
    {
        return __wrap_aligned_alloc(alignment, size);
    }

    __attribute__((weak)) int posix_memalign(void **ptr, size_t alignment, size_t size)
    {
        return __wrap_posix_memalign(ptr, alignment, size);
    }
}

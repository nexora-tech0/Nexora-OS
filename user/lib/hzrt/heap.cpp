/*
 * Nexora OS - user/lib/hzrt/heap.cpp
 * The process heap: malloc/calloc/realloc/free and C++ operator new/delete.
 *
 * Small blocks (up to 4 KiB including a 16-byte header) come from power-of-two size classes
 * carved out of 64 KiB chunks reserved with NtAllocateVirtualMemory; freed blocks go back to the
 * class's free list. Larger blocks get their own allocation (released with NtFreeVirtualMemory).
 * Every block starts with a header carrying a magic value, its class and its size, so free and
 * realloc can find their way and detect foreign pointers. One spin lock (yielding while busy)
 * serializes threads.
 */
#include "hzsys.h"

namespace {

constexpr uint32_t kMagic = 0x48706150; /* "PapH" */
constexpr uint32_t kLarge = 0xFFFFFFFF;
constexpr int kClasses = 8;             /* 32 .. 4096-byte slots (header included) */
constexpr size_t kChunk = 64 * 1024;

struct alignas(16) Header {
    uint32_t Magic;
    uint32_t Class;
    uint64_t Size;  /* usable bytes */
};

struct FreeBlock {
    uint64_t Unused; /* overlays Header.Magic/Class: Magic stays 0 while the block is free */
    FreeBlock *Next; /* overlays Header.Size */
};

FreeBlock *g_Free[kClasses];
volatile uint32_t g_Lock;

void Lock()
{
    while (__atomic_exchange_n(&g_Lock, 1, __ATOMIC_ACQUIRE))
        NtYieldExecution();
}

void Unlock()
{
    __atomic_store_n(&g_Lock, 0, __ATOMIC_RELEASE);
}

size_t ClassSize(int Class)
{
    return (size_t)32 << Class;
}

void *AllocatePages(size_t Bytes)
{
    void *Base = nullptr;
    uint64_t Size = Bytes;
    if (!HZ_SUCCESS(NtAllocateVirtualMemory(HZ_CURRENT_PROCESS, &Base, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)))
        return nullptr;
    return Base;
}

bool Refill(int Class)
{
    uint8_t *Chunk = (uint8_t *)AllocatePages(kChunk);
    if (!Chunk)
        return false;
    size_t Slot = ClassSize(Class);
    for (size_t Offset = 0; Offset + Slot <= kChunk; Offset += Slot) {
        FreeBlock *B = (FreeBlock *)(Chunk + Offset);
        B->Next = g_Free[Class];
        g_Free[Class] = B;
    }
    return true;
}

}  // namespace

extern "C" void *malloc(size_t Size)
{
    size_t Needed = Size + sizeof(Header);
    if (Needed < Size)
        return nullptr;
    int Class = 0;
    while (Class < kClasses && ClassSize(Class) < Needed)
        Class++;
    Header *H;
    if (Class == kClasses) {
        H = (Header *)AllocatePages(Needed);
        if (!H)
            return nullptr;
        H->Class = kLarge;
    } else {
        Lock();
        if (!g_Free[Class] && !Refill(Class)) {
            Unlock();
            return nullptr;
        }
        FreeBlock *B = g_Free[Class];
        g_Free[Class] = B->Next;
        Unlock();
        H = (Header *)B;
        H->Class = (uint32_t)Class;
    }
    H->Magic = kMagic;
    H->Size = Size;
    return H + 1;
}

extern "C" void free(void *Pointer)
{
    if (!Pointer)
        return;
    Header *H = (Header *)Pointer - 1;
    if (H->Magic != kMagic) {
        HzPrint("heap: free of an invalid pointer %p", Pointer);
        HzExitProcess(STATUS_INVALID_PARAMETER);
    }
    H->Magic = 0; /* a second free of the same block is caught above */
    uint32_t Class = H->Class;
    if (Class == kLarge) {
        NtFreeVirtualMemory(HZ_CURRENT_PROCESS, H, MEM_RELEASE);
        return;
    }
    Lock();
    FreeBlock *B = (FreeBlock *)H;
    B->Next = g_Free[Class];
    g_Free[Class] = B;
    Unlock();
}

extern "C" void *calloc(size_t Count, size_t Size)
{
    size_t Bytes = Count * Size;
    if (Size && Bytes / Size != Count)
        return nullptr;
    void *P = malloc(Bytes);
    if (P)
        memset(P, 0, Bytes);
    return P;
}

extern "C" void *realloc(void *Pointer, size_t Size)
{
    if (!Pointer)
        return malloc(Size);
    Header *H = (Header *)Pointer - 1;
    if (H->Class != kLarge && ClassSize((int)H->Class) - sizeof(Header) >= Size) {
        H->Size = Size;
        return Pointer;
    }
    void *P = malloc(Size);
    if (P) {
        memcpy(P, Pointer, H->Size < Size ? H->Size : Size);
        free(Pointer);
    }
    return P;
}

void *operator new(size_t Size)
{
    void *P = malloc(Size);
    if (!P) {
        HzPrint("heap: out of memory (%llu bytes)", (unsigned long long)Size);
        HzExitProcess(STATUS_NO_MEMORY);
    }
    return P;
}

void *operator new[](size_t Size)
{
    return operator new(Size);
}

void operator delete(void *P) noexcept
{
    free(P);
}

void operator delete[](void *P) noexcept
{
    free(P);
}

void operator delete(void *P, size_t) noexcept
{
    free(P);
}

void operator delete[](void *P, size_t) noexcept
{
    free(P);
}

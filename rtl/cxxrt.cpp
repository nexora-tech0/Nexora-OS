/*
 * Nexora OS - rtl/cxxrt.cpp
 * Minimal C++ runtime for kernel-mode C++20 (win32k, later the kernel-side UI pieces):
 * static initializers, pure-virtual trap, and operator new/delete on the non-paged pool.
 * Exceptions and RTTI are disabled; a failing plain `new` stops the system with
 * INSUFFICIENT_RESOURCES, while `new (std::nothrow)`-style callers use the nothrow overloads.
 */
#include "ex.h"
#include "kcxx.h"
#include "ke.h"
#include "rtl.h"

typedef void (*RTL_INITIALIZER)(void);

#define CXX_POOL_TAG HZ_POOL_TAG('C', 'x', 'x', ' ')

namespace std {
const nothrow_t nothrow{};
}  // namespace std

extern "C" {
extern RTL_INITIALIZER __InitArrayStart[];
extern RTL_INITIALIZER __InitArrayEnd[];

/* Kernel global destructors never run; the kernel is not unloaded. */
void *__dso_handle = nullptr;

int __cxa_atexit(void (*)(void *), void *, void *)
{
    return 0;
}

[[noreturn]] void __cxa_pure_virtual()
{
    KeBugCheck(PURE_VIRTUAL_FUNCTION_CALL);
}

void RtlRunStaticInitializers(void)
{
    for (RTL_INITIALIZER *Fn = __InitArrayStart; Fn < __InitArrayEnd; Fn++)
        (*Fn)();
}
}

static void *CxxAllocate(size_t Size)
{
    void *P = ExAllocatePoolWithTag(NonPagedPool, Size, CXX_POOL_TAG);
    if (!P)
        KeBugCheckEx(INSUFFICIENT_RESOURCES, Size, CXX_POOL_TAG, 0, 0);
    return P;
}

void *operator new(size_t Size)
{
    return CxxAllocate(Size);
}

void *operator new[](size_t Size)
{
    return CxxAllocate(Size);
}

void *operator new(size_t Size, const std::nothrow_t &) noexcept
{
    return ExAllocatePoolWithTag(NonPagedPool, Size, CXX_POOL_TAG);
}

void *operator new[](size_t Size, const std::nothrow_t &) noexcept
{
    return ExAllocatePoolWithTag(NonPagedPool, Size, CXX_POOL_TAG);
}

void operator delete(void *P) noexcept
{
    ExFreePool(P);
}

void operator delete[](void *P) noexcept
{
    ExFreePool(P);
}

void operator delete(void *P, size_t) noexcept
{
    ExFreePool(P);
}

void operator delete[](void *P, size_t) noexcept
{
    ExFreePool(P);
}

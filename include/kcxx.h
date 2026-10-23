/*
 * Nexora OS - include/kcxx.h
 * Kernel C++ support declarations (implemented in rtl/cxxrt.cpp): std::nothrow, the nothrow and
 * placement forms of operator new. Kernel C++ has no standard library; plain `new` stops the
 * system when the pool is exhausted, `new (std::nothrow)` returns nullptr instead.
 */
#ifndef KCXX_H
#define KCXX_H

#ifdef __cplusplus

#include <stddef.h>

namespace std {
struct nothrow_t {
    explicit nothrow_t() = default;
};
extern const nothrow_t nothrow;
}  // namespace std

void *operator new(size_t Size, const std::nothrow_t &) noexcept;
void *operator new[](size_t Size, const std::nothrow_t &) noexcept;

inline void *operator new(size_t, void *Where) noexcept
{
    return Where;
}

#endif /* __cplusplus */

#endif /* KCXX_H */

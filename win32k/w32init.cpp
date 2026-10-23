/*
 * Nexora OS - win32k/w32init.cpp
 * win32k bring-up. M0 proves that kernel-mode C++20 builds and links: global constructors via
 * .init_array, virtual dispatch, templates, concepts and constexpr evaluation.
 */
#include "w32k.h"

namespace {

template <typename T>
concept Arithmetic = __is_arithmetic(T);

template <Arithmetic T>
constexpr T Clamp(T Value, T Lo, T Hi)
{
    return Value < Lo ? Lo : (Value > Hi ? Hi : Value);
}

static_assert(Clamp(300, 0, 255) == 255, "constexpr evaluation");

class Surface {
public:
    virtual ~Surface() = default;
    virtual int Width() const = 0;
};

class ProbeSurface final : public Surface {
public:
    explicit ProbeSurface(int Width) : Width_(Width) {}
    int Width() const override { return Width_; }

private:
    int Width_;
};

/* Constructed by RtlRunStaticInitializers(); stays zero if .init_array was not processed. */
struct StaticProbe {
    StaticProbe() : Magic(0x57314B) {}
    unsigned Magic;
};

StaticProbe g_StaticProbe;
ProbeSurface g_ProbeSurface(1920);

}  // namespace

extern "C" bool W32kCppSelfTest(void)
{
    const Surface &S = g_ProbeSurface;
    bool Ok = g_StaticProbe.Magic == 0x57314B && S.Width() == 1920 && Clamp<long>(-5, 0, 10) == 0;

    /* Heap objects through operator new/delete (non-paged pool) with virtual destruction. */
    Surface *Heap = new ProbeSurface(2560);
    int *Array = new int[300];
    for (int i = 0; i < 300; i++)
        Array[i] = i * i;
    Ok = Ok && Heap->Width() == 2560 && Array[299] == 299 * 299;
    delete[] Array;
    delete Heap;
    return Ok;
}

extern "C" const char *W32kDescribeCppRuntime(void)
{
    return "C++20, no exceptions/RTTI, static constructors and pool-backed new/delete OK";
}

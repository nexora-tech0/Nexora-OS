/*
 * Nexora OS - include/w32k.h
 * Kernel-mode window manager and graphics subsystem (win32k-style). C interface used by the
 * rest of the kernel; the implementation is C++20 (the .cpp files in win32k/) plus the Rust region
 * library (win32k/rgn). M0 only provides link-time self-tests for both languages.
 */
#ifndef W32K_H
#define W32K_H

#include "hzbase.h"

HZ_EXTERN_C_BEGIN

/* Returns true if C++ global constructors ran and C++20 features behave as expected. */
bool W32kCppSelfTest(void);
const char *W32kDescribeCppRuntime(void);

/* Phase 1, after the drivers: window manager, compositor, input threads, win32k services. */
void W32kInitialize(void);
/* Hands the screen from the boot display to the compositor. */
void W32kShowDesktop(void);
/* Checks the window manager starts with no windows or surface sections (win32k/w32test.cpp). */
bool W32kRunSelfTests(void);
uint32_t W32kQueryWindowCount(void);
/* Tells the shell (if one is registered) that a process was ended by an exception (M11). */
void W32kNotifyProcessCrash(uint32_t ProcessId, STATUS Status);

/* Rust region library (win32k/rgn). */
typedef struct _RGN_RECT {
    int32_t Left, Top, Right, Bottom; /* half-open: [Left, Right) x [Top, Bottom) */
} RGN_RECT;

/*
 * Regions (win32k/rgn/src/region.rs): canonical y-x banded rectangle lists. RGN is an opaque
 * handle; every operation needs IRQL <= DISPATCH_LEVEL (non-paged pool) and external locking.
 * Allocation failure stops the system (RUST_PANIC).
 */
typedef struct _RGN RGN;

#define RGN_OP_UNION 1
#define RGN_OP_INTERSECT 2
#define RGN_OP_SUBTRACT 3
#define RGN_OP_XOR 4

RGN *RgnCreate(void);
RGN *RgnCreateRect(const RGN_RECT *Rect);
void RgnDestroy(RGN *Region);
void RgnSetRect(RGN *Region, const RGN_RECT *Rect); /* NULL = empty */
void RgnCopy(RGN *Destination, const RGN *Source);
/* Destination = A op B; Destination may be A or B. */
void RgnCombine(RGN *Destination, const RGN *A, const RGN *B, uint32_t Op);
void RgnCombineRect(RGN *Region, const RGN_RECT *Rect, uint32_t Op);
void RgnOffset(RGN *Region, int32_t Dx, int32_t Dy);
bool RgnIsEmpty(const RGN *Region);
bool RgnContainsPoint(const RGN *Region, int32_t X, int32_t Y);
bool RgnIntersectsRect(const RGN *Region, const RGN_RECT *Rect);
void RgnGetBounds(const RGN *Region, RGN_RECT *Bounds);
/* Rectangles in band order; valid until the region changes. */
size_t RgnGetRects(const RGN *Region, const RGN_RECT **Rects);
uint64_t RgnArea(const RGN *Region);
bool RgnEqual(const RGN *A, const RGN *B);

/* Returns the area of the union of Count rectangles (Rust implementation). */
uint64_t RgnUnionArea(const RGN_RECT *Rects, size_t Count);
/* Returns 0 on success, otherwise the number of the failing check (fixed cases, then 200
   randomized property tests against a bitmap model). */
uint32_t RgnSelfTest(void);
/* Deliberately panics (diagnostics: exercises the Rust panic -> stop screen path). */
HZ_NORETURN void RgnTriggerPanic(void);

HZ_EXTERN_C_END

#endif /* W32K_H */

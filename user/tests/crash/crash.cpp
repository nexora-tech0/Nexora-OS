/*
 * Nexora OS - user/tests/crash/crash.cpp
 * Commits exactly one fault, chosen by the first argument, so the kernel's process self-test can
 * check that each kind of user-mode fault ends this process with the right status and leaves
 * the system running:
 *
 *   null    write through a null pointer             -> STATUS_ACCESS_VIOLATION
 *   kernel  read a kernel address                     -> STATUS_ACCESS_VIOLATION
 *   cli     privileged instruction                    -> STATUS_PRIVILEGED_INSTRUCTION
 *   divide  integer division by zero                  -> STATUS_INTEGER_DIVIDE_BY_ZERO
 *   ud2     undefined instruction                     -> STATUS_ILLEGAL_INSTRUCTION
 *   stack   unbounded recursion into the guard page   -> STATUS_STACK_OVERFLOW
 *   nx      call into a non-executable data page      -> STATUS_ACCESS_VIOLATION
 *   int3    breakpoint: logged, execution continues   -> exit code 3
 */
#include "hzsys.h"

#define HZ_NOINLINE __attribute__((noinline))

namespace {

/* Frames stay well under one page: there are no stack probes, so a larger frame could step
   over the single guard page. */
HZ_NOINLINE uint64_t Recurse(volatile uint64_t *Depth)
{
    volatile uint8_t Frame[512];
    Frame[0] = static_cast<uint8_t>(*Depth);
    *Depth = *Depth + 1;
    if (*Depth > (1ull << 40))
        return 0;
    return Recurse(Depth) + Frame[0];
}

uint8_t g_DataCode[16] = {0xC3}; /* "ret" in .data (mapped no-execute) */

/* Read at run time so the compiler cannot see the null pointer (and turn the store into a trap). */
volatile uintptr_t g_NullAddress = 0;

}  // namespace

extern "C" int main(int argc, char **argv)
{
    if (argc < 2) {
        HzPrint("usage: crash null|kernel|cli|divide|ud2|stack|nx|int3");
        return 1;
    }
    const char *Mode = argv[1];
    HzPrint("crash: %s", Mode);

    if (strcmp(Mode, "null") == 0) {
        *reinterpret_cast<volatile int *>(g_NullAddress) = 1;
    } else if (strcmp(Mode, "kernel") == 0) {
        volatile uint64_t Value = *reinterpret_cast<volatile uint64_t *>(0xFFFFFFFF80000000ull);
        (void)Value;
    } else if (strcmp(Mode, "cli") == 0) {
        __asm__ volatile("cli");
    } else if (strcmp(Mode, "divide") == 0) {
        volatile int Dividend = 1, Divisor = 0;
        volatile int Quotient = Dividend / Divisor;
        (void)Quotient;
    } else if (strcmp(Mode, "ud2") == 0) {
        __asm__ volatile("ud2");
    } else if (strcmp(Mode, "stack") == 0) {
        static volatile uint64_t Depth;
        Recurse(&Depth);
    } else if (strcmp(Mode, "nx") == 0) {
        auto Routine = reinterpret_cast<void (*)()>(reinterpret_cast<uintptr_t>(g_DataCode));
        Routine();
    } else if (strcmp(Mode, "int3") == 0) {
        __asm__ volatile("int3");
        HzPrint("crash: survived int3");
        return 3;
    } else {
        HzPrint("crash: unknown mode %s", Mode);
        return 1;
    }
    HzPrint("crash: %s did not fault", Mode);
    return 2;
}

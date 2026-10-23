/*
 * Nexora OS - user/tests/hello/hello.cpp
 * The first user-mode program: greets through NtDisplayString with a few facts only user mode
 * can see (its PEB, TEB and stack), then returns 42, which the kernel's process self-test checks.
 */
#include "hzsys.h"

extern "C" int main(int argc, char **argv)
{
    HZ_TEB *Teb = HzCurrentTeb();
    HZ_PEB *Peb = Teb->Peb;
    int Local = 0;
    HzPrint("Hello from Nexora OS user mode! %s (PID %llu, thread %llu), image at 0x%llx%s%s%s", Peb->ImageName,
            static_cast<unsigned long long>(Peb->ProcessId), static_cast<unsigned long long>(Teb->ThreadId),
            static_cast<unsigned long long>(Peb->ImageBase), argc > 1 ? " [" : "", argc > 1 ? argv[1] : "",
            argc > 1 ? "]" : "");
    HzPrint("  stack 0x%llx-0x%llx, local at 0x%llx, TEB at 0x%llx",
            static_cast<unsigned long long>(Teb->StackLimit), static_cast<unsigned long long>(Teb->StackBase),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&Local)),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(Teb)));
    return 42;
}

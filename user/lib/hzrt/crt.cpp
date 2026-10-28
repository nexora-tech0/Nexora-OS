/*
 * Nexora OS - user/lib/hzrt/crt.cpp
 * Minimal user-mode C/C++ runtime: the PE entry point (HzProcessStartup), the thread start-up
 * thunk, formatted output and process exit. rtl/string.c and rtl/format.c are compiled into the
 * same library, so programs get memcpy/memset/strlen and printf-style formatting; heap.cpp adds
 * the heap and hzcore.cpp the out-of-line parts of hzcore.h.
 *
 * Global constructors (M7): the compiler puts a pointer to each translation unit's dynamic
 * initializer in a section .CRT$XCU; the linker sorts .CRT$XC* sections by name, so everything
 * between our markers in .CRT$XCA and .CRT$XCZ runs before main. Destructors registered with
 * atexit run after main returns (not when a process is terminated from elsewhere). Programs are
 * built with -fno-threadsafe-statics: function-local statics are initialized without guards
 * against other threads.
 */
#include "hzsys.h"
#include "hzfile.h"
#include "hzui.h"
#include "rtl.h"

extern "C" int main(int argc, char **argv);

/* Referenced by MSVC-target code that uses floating point. */
extern "C" {
int _fltused = 0;
extern const uint32_t HzSysStubCount; /* user/lib/hzsys/hzsys.asm */
}

typedef void (*HZ_INITIALIZER)(void);

extern "C" {
__attribute__((section(".CRT$XCA"), used)) const HZ_INITIALIZER HzpInitializersBegin = nullptr;
__attribute__((section(".CRT$XCZ"), used)) const HZ_INITIALIZER HzpInitializersEnd = nullptr;
}

    static_assert(HzSysServiceCount == 53, "update the SERVICE list in user/lib/hzsys/hzsys.asm");
static_assert(sizeof(HANDLE) == 8 && offsetof(HZ_TEB, TlsSlots) == 0x148 && sizeof(HZ_PEB) == 888, "ABI layout");

namespace {

constexpr int kMaxArguments = 16;
char g_CommandLine[sizeof(HZ_PEB::CommandLine)];
char *g_Argv[kMaxArguments + 1];

constexpr int kMaxExitHandlers = 64;
void (*g_ExitHandlers[kMaxExitHandlers])(void);
int g_ExitHandlerCount;

void RunInitializers()
{
    /* Walk addresses, not the two objects: the compiler must not assume they are unrelated. */
    uintptr_t Begin = reinterpret_cast<uintptr_t>(&HzpInitializersBegin) + sizeof(HZ_INITIALIZER);
    uintptr_t End = reinterpret_cast<uintptr_t>(&HzpInitializersEnd);
    __asm__ volatile("" : "+r"(Begin), "+r"(End));
    for (uintptr_t P = Begin; P < End; P += sizeof(HZ_INITIALIZER)) {
        HZ_INITIALIZER Initializer = *reinterpret_cast<const volatile HZ_INITIALIZER *>(P);
        if (Initializer)
            Initializer();
    }
}

void RunExitHandlers()
{
    while (g_ExitHandlerCount > 0)
        g_ExitHandlers[--g_ExitHandlerCount]();
}

/* Splits on spaces; "double quotes" group words. Modifies Line in place. */
int SplitCommandLine(char *Line, char **Argv)
{
    int Count = 0;
    char *p = Line;
    while (*p && Count < kMaxArguments) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        bool Quoted = *p == '"';
        if (Quoted)
            p++;
        Argv[Count++] = p;
        while (*p && (Quoted ? *p != '"' : (*p != ' ' && *p != '\t')))
            p++;
        if (*p)
            *p++ = '\0';
    }
    Argv[Count] = nullptr;
    return Count;
}

}  // namespace

extern "C" [[noreturn]] void HzExitProcess(STATUS ExitStatus)
{
    for (;;) {
        NtTerminateProcess(HZ_CURRENT_PROCESS, ExitStatus);
        NtTerminateThread(HZ_CURRENT_THREAD, ExitStatus); /* only reached if the process call failed */
    }
}

/* PE entry point: rcx = PEB (set up by the kernel's process creation). */
extern "C" [[noreturn]] void HzProcessStartup(HZ_PEB *Peb)
{
    if (HzSysStubCount != HzSysServiceCount)
        HzExitProcess(STATUS_INVALID_SYSTEM_SERVICE);
    memcpy(g_CommandLine, Peb->CommandLine, sizeof(g_CommandLine));
    g_CommandLine[sizeof(g_CommandLine) - 1] = '\0';
    int Argc = SplitCommandLine(g_CommandLine, g_Argv);
    RunInitializers();
    hzui::Theme::RefreshFromSettings();
    int Result = main(Argc, g_Argv);
    RunExitHandlers();
    HzExitProcess(Result);
}

extern "C" int atexit(void (*Handler)(void))
{
    if (g_ExitHandlerCount >= kMaxExitHandlers)
        return -1;
    g_ExitHandlers[g_ExitHandlerCount++] = Handler;
    return 0;
}

/* A pure virtual function was called (an object used during construction or destruction). */
extern "C" int _purecall(void)
{
    HzPrint("hzrt: pure virtual function call");
    HzExitProcess(STATUS_ASSERTION_FAILURE);
}

/* New threads start here: rip = this function, rcx = Routine, rdx = Parameter. */
extern "C" [[noreturn]] void HzpThreadStartup(HZ_THREAD_ROUTINE Routine, void *Parameter)
{
    uint32_t ExitStatus = Routine(Parameter);
    for (;;)
        NtTerminateThread(HZ_CURRENT_THREAD, static_cast<STATUS>(ExitStatus));
}

extern "C" STATUS HzCreateThread(HZ_THREAD_ROUTINE Routine, void *Parameter, HANDLE *Thread, uint32_t *ThreadId)
{
    return NtCreateThread(HZ_CURRENT_PROCESS, reinterpret_cast<void *>(&HzpThreadStartup),
                          reinterpret_cast<uint64_t>(Routine), reinterpret_cast<uint64_t>(Parameter), Thread, ThreadId);
}

extern "C" int HzFormat(char *Buffer, size_t Size, const char *Format, ...)
{
    va_list Args;
    va_start(Args, Format);
    int Length = RtlFormatStringV(Buffer, Size, Format, Args);
    va_end(Args);
    return Length;
}

extern "C" HANDLE HzGetStandardHandle(uint32_t Which)
{
    HZ_PEB *Peb = HzCurrentPeb();
    uint64_t Handle = Which == HZ_STD_INPUT ? Peb->StandardInput
                                            : (Which == HZ_STD_OUTPUT ? Peb->StandardOutput : Peb->StandardError);
    return reinterpret_cast<HANDLE>(Handle);
}

extern "C" STATUS HzWriteStandard(uint32_t Which, const char *Text, size_t Length)
{
    HANDLE Out = HzGetStandardHandle(Which);
    if (!Out)
        return STATUS_INVALID_HANDLE;
    uint32_t Written = 0;
    return NtWriteFile(Out, Text, static_cast<uint32_t>(Length), nullptr, &Written);
}

/* One line: to the standard output if the creator gave one (a pipe, a file), else to the kernel
   log (and the boot console). */
extern "C" int HzPrint(const char *Format, ...)
{
    char Buffer[512];
    va_list Args;
    va_start(Args, Format);
    int Length = RtlFormatStringV(Buffer, sizeof(Buffer) - 1, Format, Args);
    va_end(Args);
    size_t Size = strlen(Buffer);
    if (HzGetStandardHandle(HZ_STD_OUTPUT)) {
        Buffer[Size] = '\n';
        if (HZ_SUCCESS(HzWriteStandard(HZ_STD_OUTPUT, Buffer, Size + 1)))
            return Length;
        Buffer[Size] = '\0';
    }
    NtDisplayString(Buffer, Size);
    return Length;
}

extern "C" int HzLog(const char *Format, ...)
{
    char Buffer[512];
    va_list Args;
    va_start(Args, Format);
    int Length = RtlFormatStringV(Buffer, sizeof(Buffer), Format, Args);
    va_end(Args);
    NtDisplayString(Buffer, strlen(Buffer));
    return Length;
}

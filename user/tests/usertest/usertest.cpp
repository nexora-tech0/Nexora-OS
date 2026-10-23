/*
 * Nexora OS - user/tests/usertest/usertest.cpp
 * User-mode test of the M4 system interface. Every check prints only on failure; the process
 * exit code is the number of failed checks (0 = all passed), which the kernel self-test checks.
 *
 *   usertest.exe        run all tests
 *   usertest.exe spin   loop forever (a child for the remote-termination test)
 */
#include "hzsys.h"

namespace {

int g_Checks;
int g_Failures;
uint32_t g_ProcessId;

void Check(bool Ok, const char *What)
{
    g_Checks++;
    if (!Ok) {
        g_Failures++;
        HzPrint("usertest: FAIL %s", What);
    }
}

void CheckStatus(STATUS Got, STATUS Expected, const char *What)
{
    g_Checks++;
    if (Got != Expected) {
        g_Failures++;
        HzPrint("usertest: FAIL %s: status 0x%08X, expected 0x%08X", What, static_cast<uint32_t>(Got),
                static_cast<uint32_t>(Expected));
    }
}

/* A system call with an arbitrary service number (for the invalid-number check). */
STATUS RawSystemCall(uint32_t Number)
{
    STATUS Result;
    __asm__ volatile("syscall" : "=a"(Result) : "a"(Number) : "rcx", "r11", "memory");
    return Result;
}

const uint64_t kNoWait = 0;
const uint64_t kFiveSeconds = 5000000;

/* ---- Environment ------------------------------------------------------------------------------ */

void TestEnvironment()
{
    HZ_TEB *Teb = HzCurrentTeb();
    HZ_PEB *Peb = Teb->Peb;
    int Local = 0;
    uintptr_t Here = reinterpret_cast<uintptr_t>(&Local);
    uintptr_t Code = reinterpret_cast<uintptr_t>(&TestEnvironment);
    Check(Teb->Self == Teb, "TEB self pointer (gs:[0])");
    Check(strcmp(Peb->ImageName, "usertest.exe") == 0, "PEB image name");
    Check(Teb->ProcessId == Peb->ProcessId && Peb->ProcessId != 0, "TEB/PEB process id");
    Check(Here >= Teb->StackLimit && Here < Teb->StackBase, "local variable lies on the TEB's stack");
    Check(Code >= Peb->ImageBase && Code < Peb->ImageBase + Peb->ImageSize, "code lies inside the image");
    g_ProcessId = static_cast<uint32_t>(Peb->ProcessId);
}

/* ---- Bad calls ---------------------------------------------------------------------------------- */

void TestBadCalls()
{
    CheckStatus(RawSystemCall(0x7FFF), STATUS_INVALID_SYSTEM_SERVICE, "invalid service number");
    CheckStatus(NtQueryUptime(nullptr), STATUS_ACCESS_VIOLATION, "null output pointer");
    CheckStatus(NtQueryUptime(reinterpret_cast<uint64_t *>(0xFFFF800000001000ull)), STATUS_ACCESS_VIOLATION,
                "kernel output pointer");
    CheckStatus(NtQueryUptime(reinterpret_cast<uint64_t *>(0x00007FFFFFFFFFFCull)), STATUS_ACCESS_VIOLATION,
                "output pointer straddling the top of user space");
    CheckStatus(NtDisplayString(reinterpret_cast<const char *>(0xFFFFFFFF80000000ull), 16), STATUS_ACCESS_VIOLATION,
                "kernel input pointer");
    CheckStatus(NtDisplayString("x", 5000), STATUS_INVALID_PARAMETER, "oversized string");
    CheckStatus(NtClose(reinterpret_cast<HANDLE>(0x1234)), STATUS_INVALID_HANDLE, "close of an unused handle");
    CheckStatus(NtClose(reinterpret_cast<HANDLE>(0x13)), STATUS_INVALID_HANDLE, "close of a misaligned handle");
    CheckStatus(NtCreateEvent(nullptr, HZ_NOTIFICATION_EVENT, 0), STATUS_ACCESS_VIOLATION, "event handle to null");
    CheckStatus(NtCreateProcess(reinterpret_cast<const char *>(0x10), nullptr, nullptr, nullptr),
                STATUS_ACCESS_VIOLATION, "process path at an unmapped address");
    uint64_t Uptime = 0;
    CheckStatus(NtQueryUptime(&Uptime), STATUS_SUCCESS, "uptime");
    Check(Uptime > 0, "uptime is positive");
}

/* ---- Events and waits ------------------------------------------------------------------------- */

void TestEvents()
{
    HANDLE Event = nullptr;
    CheckStatus(NtCreateEvent(&Event, HZ_NOTIFICATION_EVENT, 0), STATUS_SUCCESS, "create notification event");
    CheckStatus(NtWaitForSingleObject(Event, 0, &kNoWait), STATUS_TIMEOUT, "poll of a non-signaled event");
    int32_t Previous = -1;
    CheckStatus(NtSetEvent(Event, &Previous), STATUS_SUCCESS, "set event");
    Check(Previous == 0, "previous event state");
    CheckStatus(NtWaitForSingleObject(Event, 0, &kNoWait), STATUS_WAIT_0, "wait on a signaled event");
    CheckStatus(NtWaitForSingleObject(Event, 0, &kNoWait), STATUS_WAIT_0, "notification event stays signaled");
    CheckStatus(NtResetEvent(Event), STATUS_SUCCESS, "reset event");
    CheckStatus(NtWaitForSingleObject(Event, 0, &kNoWait), STATUS_TIMEOUT, "poll after reset");
    CheckStatus(NtSetEvent(HZ_CURRENT_PROCESS, nullptr), STATUS_OBJECT_TYPE_MISMATCH, "set event on a process");
    CheckStatus(NtClose(Event), STATUS_SUCCESS, "close event");
    CheckStatus(NtClose(Event), STATUS_INVALID_HANDLE, "second close");
    CheckStatus(NtSetEvent(Event, nullptr), STATUS_INVALID_HANDLE, "use after close");

    HANDLE Pair[2];
    CheckStatus(NtCreateEvent(&Pair[0], HZ_SYNCHRONIZATION_EVENT, 0), STATUS_SUCCESS, "create event 1");
    CheckStatus(NtCreateEvent(&Pair[1], HZ_SYNCHRONIZATION_EVENT, 0), STATUS_SUCCESS, "create event 2");
    NtSetEvent(Pair[1], nullptr);
    CheckStatus(NtWaitForMultipleObjects(2, Pair, HZ_WAIT_ANY, &kNoWait), STATUS_WAIT_0 + 1, "wait any");
    CheckStatus(NtWaitForMultipleObjects(2, Pair, HZ_WAIT_ANY, &kNoWait), STATUS_TIMEOUT,
                "synchronization event was consumed");
    NtSetEvent(Pair[0], nullptr);
    CheckStatus(NtWaitForMultipleObjects(2, Pair, HZ_WAIT_ALL, &kNoWait), STATUS_TIMEOUT, "wait all, one signaled");
    NtSetEvent(Pair[1], nullptr);
    CheckStatus(NtWaitForMultipleObjects(2, Pair, HZ_WAIT_ALL, &kNoWait), STATUS_WAIT_0, "wait all, both signaled");
    CheckStatus(NtWaitForMultipleObjects(0, Pair, HZ_WAIT_ALL, &kNoWait), STATUS_INVALID_PARAMETER, "wait on nothing");
    NtClose(Pair[0]);
    NtClose(Pair[1]);

    CheckStatus(NtWaitForSingleObject(HZ_CURRENT_PROCESS, 0, &kNoWait), STATUS_TIMEOUT,
                "own process is not signaled while running");
    uint64_t Before = 0, After = 0;
    NtQueryUptime(&Before);
    CheckStatus(NtDelayExecution(20000), STATUS_SUCCESS, "delay 20 ms");
    NtQueryUptime(&After);
    Check(After - Before >= 20000, "delay lasted at least 20 ms");
}

/* ---- Virtual memory ----------------------------------------------------------------------------- */

void TestMemory()
{
    HZ_MEMORY_BASIC_INFORMATION Info;
    void *Base = nullptr;
    uint64_t Size = 64 * 1024;
    CheckStatus(NtAllocateVirtualMemory(HZ_CURRENT_PROCESS, &Base, &Size, MEM_RESERVE, PAGE_READWRITE), STATUS_SUCCESS,
                "reserve 64 KiB");
    Check(Base != nullptr && Size == 64 * 1024, "reservation base and size");

    uint8_t *Middle = static_cast<uint8_t *>(Base) + 16 * 1024;
    void *CommitBase = Middle;
    uint64_t CommitSize = 8 * 1024;
    CheckStatus(NtAllocateVirtualMemory(HZ_CURRENT_PROCESS, &CommitBase, &CommitSize, MEM_COMMIT, PAGE_READWRITE),
                STATUS_SUCCESS, "commit 8 KiB inside the reservation");
    Middle[0] = 0x5A;
    Middle[4096 + 100] = 0xA5;
    Check(Middle[0] == 0x5A && Middle[4096 + 100] == 0xA5 && Middle[1] == 0, "committed pages are zeroed and writable");

    CheckStatus(NtQueryVirtualMemory(HZ_CURRENT_PROCESS, Middle + 10, &Info, sizeof(Info)), STATUS_SUCCESS,
                "query committed region");
    Check(Info.BaseAddress == reinterpret_cast<uint64_t>(Middle) && Info.RegionSize == 8192 &&
              Info.AllocationBase == reinterpret_cast<uint64_t>(Base) && Info.State == MEM_COMMIT &&
              Info.Protect == PAGE_READWRITE && Info.Type == MEM_PRIVATE,
          "committed region description");
    CheckStatus(NtQueryVirtualMemory(HZ_CURRENT_PROCESS, Base, &Info, sizeof(Info)), STATUS_SUCCESS,
                "query reserved region");
    Check(Info.State == MEM_RESERVE && Info.RegionSize == 16 * 1024, "reserved region description");
    CheckStatus(NtQueryVirtualMemory(HZ_CURRENT_PROCESS, Base, &Info, 8), STATUS_INFO_LENGTH_MISMATCH,
                "query with a short buffer");

    uint32_t Old = 0;
    CheckStatus(NtProtectVirtualMemory(HZ_CURRENT_PROCESS, Middle, 4096, PAGE_READONLY, &Old), STATUS_SUCCESS,
                "make one page read-only");
    Check(Old == PAGE_READWRITE, "previous protection");
    NtQueryVirtualMemory(HZ_CURRENT_PROCESS, Middle, &Info, sizeof(Info));
    Check(Info.Protect == PAGE_READONLY && Info.RegionSize == 4096, "read-only page is its own region");
    Check(Middle[0] == 0x5A, "read-only page is still readable");
    CheckStatus(NtProtectVirtualMemory(HZ_CURRENT_PROCESS, Middle, 4096, 0x3, nullptr), STATUS_INVALID_PAGE_PROTECTION,
                "invalid protection value");

    CheckStatus(NtFreeVirtualMemory(HZ_CURRENT_PROCESS, Middle, MEM_RELEASE), STATUS_INVALID_PARAMETER,
                "release from the middle of a reservation");
    CheckStatus(NtFreeVirtualMemory(HZ_CURRENT_PROCESS, Base, MEM_RELEASE), STATUS_SUCCESS, "release reservation");
    NtQueryVirtualMemory(HZ_CURRENT_PROCESS, Middle, &Info, sizeof(Info));
    Check(Info.State == MEM_FREE, "released range is free");

    HZ_PEB *Peb = HzCurrentPeb();
    CheckStatus(NtFreeVirtualMemory(HZ_CURRENT_PROCESS, reinterpret_cast<void *>(Peb->ImageBase), MEM_RELEASE),
                STATUS_CONFLICTING_ADDRESSES, "release of the image");
    NtQueryVirtualMemory(HZ_CURRENT_PROCESS, reinterpret_cast<void *>(Peb->ImageBase), &Info, sizeof(Info));
    Check(Info.Type == MEM_IMAGE && Info.Protect == PAGE_READONLY, "image headers are read-only");
    NtQueryVirtualMemory(HZ_CURRENT_PROCESS, reinterpret_cast<void *>(&TestMemory), &Info, sizeof(Info));
    Check(Info.Type == MEM_IMAGE && Info.Protect == PAGE_EXECUTE_READ, "code is execute-read");

    void *Kernel = reinterpret_cast<void *>(0xFFFF800000000000ull);
    Size = 4096;
    CheckStatus(NtAllocateVirtualMemory(HZ_CURRENT_PROCESS, &Kernel, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE),
                STATUS_INVALID_PARAMETER, "allocation at a kernel address");
    void *Fixed = reinterpret_cast<void *>(0x50000000ull);
    Size = 4096;
    CheckStatus(NtAllocateVirtualMemory(HZ_CURRENT_PROCESS, &Fixed, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE),
                STATUS_SUCCESS, "allocation at a chosen address");
    void *Again = Fixed;
    CheckStatus(NtAllocateVirtualMemory(HZ_CURRENT_PROCESS, &Again, &Size, MEM_RESERVE, PAGE_READWRITE),
                STATUS_CONFLICTING_ADDRESSES, "overlapping reservation");
    NtFreeVirtualMemory(HZ_CURRENT_PROCESS, Fixed, MEM_RELEASE);

    /* Sections: two views of the same pages. */
    HANDLE Section = nullptr;
    CheckStatus(NtCreateSection(&Section, 3 * 4096 + 100), STATUS_SUCCESS, "create a section");
    void *View1 = nullptr, *View2 = nullptr;
    uint64_t ViewSize = 0;
    CheckStatus(NtMapViewOfSection(Section, HZ_CURRENT_PROCESS, &View1, &ViewSize, PAGE_READWRITE), STATUS_SUCCESS,
                "map a view");
    Check(ViewSize == 4 * 4096, "view size is rounded to pages");
    CheckStatus(NtMapViewOfSection(Section, HZ_CURRENT_PROCESS, &View2, nullptr, PAGE_READWRITE), STATUS_SUCCESS,
                "map a second view");
    if (View1 && View2) {
        static_cast<volatile uint32_t *>(View1)[1500] = 0xC0FFEE;
        Check(static_cast<volatile uint32_t *>(View2)[1500] == 0xC0FFEE && View1 != View2, "views share their pages");
        NtQueryVirtualMemory(HZ_CURRENT_PROCESS, View1, &Info, sizeof(Info));
        Check(Info.Type == MEM_MAPPED && Info.RegionSize == 4 * 4096, "view reported as MEM_MAPPED");
        CheckStatus(NtFreeVirtualMemory(HZ_CURRENT_PROCESS, View1, MEM_RELEASE), STATUS_CONFLICTING_ADDRESSES,
                    "views cannot be released as private memory");
    }
    NtClose(Section); /* the views keep the section alive */
    if (View2)
        Check(static_cast<volatile uint32_t *>(View2)[1500] == 0xC0FFEE, "pages survive the handle");
    CheckStatus(NtUnmapViewOfSection(HZ_CURRENT_PROCESS, View1), STATUS_SUCCESS, "unmap the first view");
    CheckStatus(NtUnmapViewOfSection(HZ_CURRENT_PROCESS, View1), STATUS_INVALID_PARAMETER, "unmap twice");
    CheckStatus(NtUnmapViewOfSection(HZ_CURRENT_PROCESS, View2), STATUS_SUCCESS, "unmap the second view");
    CheckStatus(NtCreateSection(&Section, 0), STATUS_INVALID_PARAMETER, "empty section");
}

/* ---- Threads ------------------------------------------------------------------------------------ */

struct SignalContext {
    HANDLE Event;
    volatile uint32_t Ran;
};

uint32_t SignalRoutine(void *Parameter)
{
    auto *Context = static_cast<SignalContext *>(Parameter);
    Context->Ran = 1;
    NtSetEvent(Context->Event, nullptr);
    return 7;
}

uint32_t SpinRoutine(void *Parameter)
{
    for (;;)
        NtYieldExecution();
}

uint32_t BlockRoutine(void *Parameter)
{
    HANDLE Never = static_cast<HANDLE>(Parameter);
    NtWaitForSingleObject(Never, 0, nullptr);
    return 1; /* not reached: terminated while waiting */
}

volatile uint32_t g_Counter;

uint32_t CountRoutine(void *Parameter)
{
    for (int i = 0; i < 1000; i++) {
        __atomic_add_fetch(&g_Counter, 1, __ATOMIC_RELAXED);
        if (i % 100 == 0)
            NtYieldExecution();
    }
    return 0;
}

STATUS ThreadExitStatus(HANDLE Thread)
{
    HZ_THREAD_BASIC_INFORMATION Info;
    STATUS Status = NtQueryInformationThread(Thread, HZ_THREAD_BASIC_INFORMATION_CLASS, &Info, sizeof(Info));
    return HZ_SUCCESS(Status) ? Info.ExitStatus : Status;
}

void TestThreads()
{
    SignalContext Context = {nullptr, 0};
    NtCreateEvent(&Context.Event, HZ_NOTIFICATION_EVENT, 0);
    HANDLE Thread = nullptr;
    uint32_t ThreadId = 0;
    CheckStatus(HzCreateThread(SignalRoutine, &Context, &Thread, &ThreadId), STATUS_SUCCESS, "create thread");
    CheckStatus(NtWaitForSingleObject(Thread, 0, &kFiveSeconds), STATUS_WAIT_0, "wait for thread exit");
    Check(Context.Ran == 1, "thread ran");
    CheckStatus(ThreadExitStatus(Thread), 7, "thread exit status = routine result");
    CheckStatus(NtWaitForSingleObject(Context.Event, 0, &kNoWait), STATUS_WAIT_0, "thread signaled the event");
    HZ_THREAD_BASIC_INFORMATION Info;
    NtQueryInformationThread(Thread, HZ_THREAD_BASIC_INFORMATION_CLASS, &Info, sizeof(Info));
    Check(Info.ThreadId == ThreadId && Info.ProcessId == g_ProcessId, "thread information");
    NtClose(Thread);

    /* Terminating a thread that keeps running in user mode. */
    CheckStatus(HzCreateThread(SpinRoutine, nullptr, &Thread, nullptr), STATUS_SUCCESS, "create spinning thread");
    NtDelayExecution(5000);
    CheckStatus(ThreadExitStatus(Thread), STATUS_PENDING, "spinning thread is alive");
    CheckStatus(NtTerminateThread(Thread, 0x1234), STATUS_SUCCESS, "terminate spinning thread");
    CheckStatus(NtWaitForSingleObject(Thread, 0, &kFiveSeconds), STATUS_WAIT_0, "spinning thread ended");
    CheckStatus(ThreadExitStatus(Thread), 0x1234, "spinning thread exit status");
    NtClose(Thread);

    /* Terminating a thread blocked in a wait that would never end. */
    HANDLE Never;
    NtCreateEvent(&Never, HZ_NOTIFICATION_EVENT, 0);
    CheckStatus(HzCreateThread(BlockRoutine, Never, &Thread, nullptr), STATUS_SUCCESS, "create blocked thread");
    NtDelayExecution(5000);
    CheckStatus(NtTerminateThread(Thread, 0x4321), STATUS_SUCCESS, "terminate blocked thread");
    CheckStatus(NtWaitForSingleObject(Thread, 0, &kFiveSeconds), STATUS_WAIT_0, "blocked thread ended");
    CheckStatus(ThreadExitStatus(Thread), 0x4321, "blocked thread exit status");
    NtClose(Thread);
    NtClose(Never);
    NtClose(Context.Event);

    /* Eight threads at once; waiting on all of them uses more than the built-in wait blocks. */
    HANDLE Workers[8];
    g_Counter = 0;
    bool Created = true;
    for (HANDLE &Worker : Workers)
        Created = Created && HZ_SUCCESS(HzCreateThread(CountRoutine, nullptr, &Worker, nullptr));
    Check(Created, "create 8 threads");
    if (Created) {
        CheckStatus(NtWaitForMultipleObjects(8, Workers, HZ_WAIT_ALL, &kFiveSeconds), STATUS_WAIT_0,
                    "wait for all 8 threads");
        Check(g_Counter == 8000, "all increments happened");
        for (HANDLE Worker : Workers)
            NtClose(Worker);
    }
}

/* ---- Extended processor state --------------------------------------------------------------- */

/* Loads xmm0-xmm15 with a pattern, yields to other threads with the registers live, and checks
   that the kernel's context switches preserved them; also the MXCSR and x87 control words. */
bool SseSurvivesSwitches(uint64_t Seed)
{
    alignas(16) uint64_t In[32], Out[32];
    for (int i = 0; i < 32; i++)
        In[i] = Seed * 0x9E3779B97F4A7C15ull + static_cast<uint64_t>(i);
    __asm__ volatile(
        "movdqu 0(%[in]), %%xmm0\n\tmovdqu 16(%[in]), %%xmm1\n\tmovdqu 32(%[in]), %%xmm2\n\t"
        "movdqu 48(%[in]), %%xmm3\n\tmovdqu 64(%[in]), %%xmm4\n\tmovdqu 80(%[in]), %%xmm5\n\t"
        "movdqu 96(%[in]), %%xmm6\n\tmovdqu 112(%[in]), %%xmm7\n\tmovdqu 128(%[in]), %%xmm8\n\t"
        "movdqu 144(%[in]), %%xmm9\n\tmovdqu 160(%[in]), %%xmm10\n\tmovdqu 176(%[in]), %%xmm11\n\t"
        "movdqu 192(%[in]), %%xmm12\n\tmovdqu 208(%[in]), %%xmm13\n\tmovdqu 224(%[in]), %%xmm14\n\t"
        "movdqu 240(%[in]), %%xmm15\n\t"
        "mov %[yield], %%eax\n\tsyscall\n\t"
        "mov %[yield], %%eax\n\tsyscall\n\t"
        "movdqu %%xmm0, 0(%[out])\n\tmovdqu %%xmm1, 16(%[out])\n\tmovdqu %%xmm2, 32(%[out])\n\t"
        "movdqu %%xmm3, 48(%[out])\n\tmovdqu %%xmm4, 64(%[out])\n\tmovdqu %%xmm5, 80(%[out])\n\t"
        "movdqu %%xmm6, 96(%[out])\n\tmovdqu %%xmm7, 112(%[out])\n\tmovdqu %%xmm8, 128(%[out])\n\t"
        "movdqu %%xmm9, 144(%[out])\n\tmovdqu %%xmm10, 160(%[out])\n\tmovdqu %%xmm11, 176(%[out])\n\t"
        "movdqu %%xmm12, 192(%[out])\n\tmovdqu %%xmm13, 208(%[out])\n\tmovdqu %%xmm14, 224(%[out])\n\t"
        "movdqu %%xmm15, 240(%[out])\n\t"
        :
        : [in] "r"(In), [out] "r"(Out), [yield] "i"(HzSysYieldExecution)
        : "rax", "rcx", "r11", "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8",
          "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
    return memcmp(In, Out, sizeof(In)) == 0;
}

bool ControlWordsSurviveSwitches(uint32_t Mxcsr, uint16_t FpuControl)
{
    uint32_t SavedMxcsr, GotMxcsr;
    uint16_t SavedFpu, GotFpu;
    __asm__ volatile("stmxcsr %0\n\tfnstcw %1" : "=m"(SavedMxcsr), "=m"(SavedFpu));
    __asm__ volatile("ldmxcsr %0\n\tfldcw %1" : : "m"(Mxcsr), "m"(FpuControl));
    for (int i = 0; i < 4; i++)
        NtYieldExecution();
    __asm__ volatile("stmxcsr %0\n\tfnstcw %1" : "=m"(GotMxcsr), "=m"(GotFpu));
    __asm__ volatile("ldmxcsr %0\n\tfldcw %1" : : "m"(SavedMxcsr), "m"(SavedFpu));
    return GotMxcsr == Mxcsr && GotFpu == FpuControl;
}

bool CpuHasUsableAvx()
{
    uint32_t A, B, C, D;
    __asm__ volatile("cpuid" : "=a"(A), "=b"(B), "=c"(C), "=d"(D) : "a"(1), "c"(0));
    if (!(C & (1u << 27)) || !(C & (1u << 28)))
        return false; /* no OSXSAVE or no AVX */
    uint32_t Lo, Hi;
    __asm__ volatile("xgetbv" : "=a"(Lo), "=d"(Hi) : "c"(0));
    return (Lo & 6) == 6; /* the OS enabled SSE and AVX state */
}

bool AvxSurvivesSwitches(uint64_t Seed)
{
    alignas(32) uint64_t In[64], Out[64];
    for (int i = 0; i < 64; i++)
        In[i] = Seed * 0xD1B54A32D192ED03ull + static_cast<uint64_t>(i);
    __asm__ volatile(
        "vmovdqu 0(%[in]), %%ymm0\n\tvmovdqu 32(%[in]), %%ymm1\n\tvmovdqu 64(%[in]), %%ymm2\n\t"
        "vmovdqu 96(%[in]), %%ymm3\n\tvmovdqu 128(%[in]), %%ymm4\n\tvmovdqu 160(%[in]), %%ymm5\n\t"
        "vmovdqu 192(%[in]), %%ymm6\n\tvmovdqu 224(%[in]), %%ymm7\n\tvmovdqu 256(%[in]), %%ymm8\n\t"
        "vmovdqu 288(%[in]), %%ymm9\n\tvmovdqu 320(%[in]), %%ymm10\n\tvmovdqu 352(%[in]), %%ymm11\n\t"
        "vmovdqu 384(%[in]), %%ymm12\n\tvmovdqu 416(%[in]), %%ymm13\n\tvmovdqu 448(%[in]), %%ymm14\n\t"
        "vmovdqu 480(%[in]), %%ymm15\n\t"
        "mov %[yield], %%eax\n\tsyscall\n\t"
        "mov %[yield], %%eax\n\tsyscall\n\t"
        "vmovdqu %%ymm0, 0(%[out])\n\tvmovdqu %%ymm1, 32(%[out])\n\tvmovdqu %%ymm2, 64(%[out])\n\t"
        "vmovdqu %%ymm3, 96(%[out])\n\tvmovdqu %%ymm4, 128(%[out])\n\tvmovdqu %%ymm5, 160(%[out])\n\t"
        "vmovdqu %%ymm6, 192(%[out])\n\tvmovdqu %%ymm7, 224(%[out])\n\tvmovdqu %%ymm8, 256(%[out])\n\t"
        "vmovdqu %%ymm9, 288(%[out])\n\tvmovdqu %%ymm10, 320(%[out])\n\tvmovdqu %%ymm11, 352(%[out])\n\t"
        "vmovdqu %%ymm12, 384(%[out])\n\tvmovdqu %%ymm13, 416(%[out])\n\tvmovdqu %%ymm14, 448(%[out])\n\t"
        "vmovdqu %%ymm15, 480(%[out])\n\tvzeroupper\n\t"
        :
        : [in] "r"(In), [out] "r"(Out), [yield] "i"(HzSysYieldExecution)
        : "rax", "rcx", "r11", "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8",
          "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
    return memcmp(In, Out, sizeof(In)) == 0;
}

struct StateContext {
    uint64_t Seed;
    bool Avx;
    volatile uint32_t Failures;
};

uint32_t StateRoutine(void *Parameter)
{
    auto *Context = static_cast<StateContext *>(Parameter);
    for (int i = 0; i < 50; i++) {
        if (!SseSurvivesSwitches(Context->Seed + static_cast<uint64_t>(i)))
            Context->Failures = Context->Failures + 1;
        if (Context->Avx && !AvxSurvivesSwitches(Context->Seed + static_cast<uint64_t>(i)))
            Context->Failures = Context->Failures + 1;
    }
    /* Distinct control words per thread: round-toward-zero vs. default, 53- vs. 64-bit x87. */
    if (!ControlWordsSurviveSwitches(Context->Seed & 1 ? 0x7F80u : 0x1F80u, Context->Seed & 1 ? 0x027F : 0x037F))
        Context->Failures = Context->Failures + 1;
    return 0;
}

void TestExtendedState()
{
    bool Avx = CpuHasUsableAvx();
    StateContext Contexts[3] = {{11, Avx, 0}, {22, Avx, 0}, {33, Avx, 0}};
    HANDLE Threads[2];
    bool Created = HZ_SUCCESS(HzCreateThread(StateRoutine, &Contexts[0], &Threads[0], nullptr)) &&
                   HZ_SUCCESS(HzCreateThread(StateRoutine, &Contexts[1], &Threads[1], nullptr));
    Check(Created, "create FPU test threads");
    StateRoutine(&Contexts[2]); /* the main thread takes part too */
    if (Created) {
        NtWaitForMultipleObjects(2, Threads, HZ_WAIT_ALL, &kFiveSeconds);
        NtClose(Threads[0]);
        NtClose(Threads[1]);
    }
    Check(Contexts[0].Failures + Contexts[1].Failures + Contexts[2].Failures == 0,
          Avx ? "SSE/AVX registers and control words survive context switches"
              : "SSE registers and control words survive context switches");
}

/* ---- System information and processes ------------------------------------------------------- */

void TestSystemInformation()
{
    HZ_SYSTEM_BASIC_INFORMATION Basic;
    uint32_t Returned = 0;
    CheckStatus(NtQuerySystemInformation(HZ_SYSTEM_BASIC_INFORMATION_CLASS, &Basic, sizeof(Basic), &Returned),
                STATUS_SUCCESS, "basic system information");
    Check(Basic.PageSize == 4096 && Basic.ProcessCount >= 2 && Basic.ThreadCount >= 3 && Returned == sizeof(Basic),
          "basic system information values");
    CheckStatus(NtQuerySystemInformation(HZ_SYSTEM_BASIC_INFORMATION_CLASS, &Basic, 4, nullptr), STATUS_INFO_LENGTH_MISMATCH,
                "basic information with a short buffer");
    CheckStatus(NtQuerySystemInformation(99, &Basic, sizeof(Basic), nullptr), STATUS_INVALID_INFO_CLASS,
                "unknown information class");

    HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION Interfaces[HZ_MAX_NETWORK_INTERFACES] = {};
    CheckStatus(NtQuerySystemInformation(HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION_CLASS, Interfaces, sizeof(Interfaces),
                                         &Returned),
                STATUS_SUCCESS, "network interface information");
    bool NetworkInfoValid = Returned <= sizeof(Interfaces) && Returned % sizeof(Interfaces[0]) == 0;
    uint32_t InterfaceCount = NetworkInfoValid ? Returned / sizeof(Interfaces[0]) : 0;
    for (uint32_t i = 0; i < InterfaceCount; i++) {
        const HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION &Interface = Interfaces[i];
        bool MacValid = !(Interface.MacAddress[0] & 1) &&
                        (Interface.MacAddress[0] | Interface.MacAddress[1] | Interface.MacAddress[2] |
                         Interface.MacAddress[3] | Interface.MacAddress[4] | Interface.MacAddress[5]);
        NetworkInfoValid = NetworkInfoValid && Interface.Name[0] && MacValid && Interface.LinkUp <= 1 &&
                           Interface.Mtu >= 576 && Interface.Mtu <= 1500;
    }
    Check(NetworkInfoValid, "network interface information has bounded, valid entries");

    HZ_SYSTEM_PROCESS_INFORMATION Processes[16];
    CheckStatus(NtQuerySystemInformation(HZ_SYSTEM_PROCESS_INFORMATION_CLASS, Processes, sizeof(Processes), &Returned),
                STATUS_SUCCESS, "process list");
    bool FoundSelf = false, FoundSystem = false;
    for (uint32_t i = 0; i < Returned / sizeof(Processes[0]) && i < 16; i++) {
        FoundSelf = FoundSelf || (Processes[i].ProcessId == g_ProcessId && strcmp(Processes[i].ImageName, "usertest.exe") == 0);
        FoundSystem = FoundSystem || (Processes[i].ProcessId == 4 && strcmp(Processes[i].ImageName, "System") == 0);
    }
    Check(FoundSelf && FoundSystem, "process list contains System and usertest.exe");
}

STATUS ProcessExitStatus(HANDLE Process, HZ_PROCESS_BASIC_INFORMATION *Info)
{
    STATUS Status = NtQueryInformationProcess(Process, HZ_PROCESS_BASIC_INFORMATION_CLASS, Info, sizeof(*Info));
    return HZ_SUCCESS(Status) ? Info->ExitStatus : Status;
}

void TestChildProcesses()
{
    HZ_PROCESS_BASIC_INFORMATION Info;
    HANDLE Child = nullptr;
    uint32_t ChildId = 0;
    CheckStatus(NtCreateProcess("C:\\Nexora\\System\\hello.exe", "hello.exe child", &Child, &ChildId), STATUS_SUCCESS,
                "start hello.exe");
    CheckStatus(NtWaitForSingleObject(Child, 0, &kFiveSeconds), STATUS_WAIT_0, "wait for hello.exe");
    CheckStatus(ProcessExitStatus(Child, &Info), 42, "hello.exe exit status");
    Check(Info.ProcessId == ChildId && Info.ParentProcessId == g_ProcessId && Info.ThreadCount == 0,
          "child process information");
    NtClose(Child);

    CheckStatus(NtCreateProcess("C:\\Nexora\\System\\missing.exe", nullptr, &Child, nullptr),
                STATUS_OBJECT_NAME_NOT_FOUND, "start a missing image");
    CheckStatus(NtCreateProcess("C:\\Nexora\\Licenses\\Inter-OFL.txt", nullptr, &Child, nullptr),
                STATUS_INVALID_IMAGE_NOT_MZ, "start a text file");
    CheckStatus(NtCreateProcess("\\Device\\Nothing\\x.exe", nullptr, &Child, nullptr), STATUS_OBJECT_PATH_NOT_FOUND,
                "start from a missing device");

    /* A child that never ends on its own, terminated from here. */
    CheckStatus(NtCreateProcess("C:\\Nexora\\System\\usertest.exe", "usertest.exe spin", &Child, nullptr),
                STATUS_SUCCESS, "start spinning child");
    NtDelayExecution(10000);
    CheckStatus(ProcessExitStatus(Child, &Info), STATUS_PENDING, "spinning child is running");
    CheckStatus(NtTerminateProcess(Child, 0x77), STATUS_SUCCESS, "terminate child");
    CheckStatus(NtWaitForSingleObject(Child, 0, &kFiveSeconds), STATUS_WAIT_0, "terminated child is signaled");
    CheckStatus(ProcessExitStatus(Child, &Info), 0x77, "terminated child exit status");
    CheckStatus(NtTerminateProcess(Child, 0x78), STATUS_PROCESS_IS_TERMINATING, "terminate an exited child");
    NtClose(Child);
}

}  // namespace

bool Contains(const char *Text, const char *Part)
{
    size_t N = strlen(Part);
    for (; *Text; Text++)
        if (strncmp(Text, Part, N) == 0)
            return true;
    return false;
}

/* Pipes, standard handles and opening other processes (M9). */
uint32_t PipeWriter(void *Parameter)
{
    static char Block[4000];
    for (size_t i = 0; i < sizeof(Block); i++)
        Block[i] = (char)('a' + i % 26);
    uint32_t Written = 0;
    for (int i = 0; i < 10; i++) /* 40000 bytes through a 16 KiB pipe: the writer has to wait */
        if (!HZ_SUCCESS(NtWriteFile((HANDLE)Parameter, Block, sizeof(Block), nullptr, &Written)) || Written != sizeof(Block))
            return 1;
    NtClose((HANDLE)Parameter);
    return 0;
}

void TestPipes()
{
    HANDLE Read = 0, Write = 0;
    CheckStatus(NtCreatePipe(&Read, &Write, 0), STATUS_SUCCESS, "create a pipe");
    uint32_t Moved = 0;
    CheckStatus(NtWriteFile(Write, "hello", 5, nullptr, &Moved), STATUS_SUCCESS, "write to a pipe");
    char Text[64] = {};
    CheckStatus(NtReadFile(Read, Text, 3, nullptr, &Moved), STATUS_SUCCESS, "read part of it");
    Check(Moved == 3 && memcmp(Text, "hel", 3) == 0, "pipe data in order");
    CheckStatus(NtReadFile(Read, Text, sizeof(Text), nullptr, &Moved), STATUS_SUCCESS, "read the rest");
    Check(Moved == 2 && memcmp(Text, "lo", 2) == 0, "a read returns what is there");
    CheckStatus(NtReadFile(Write, Text, 1, nullptr, &Moved), STATUS_ACCESS_DENIED, "reading the write end");
    CheckStatus(NtWriteFile(Read, "x", 1, nullptr, &Moved), STATUS_ACCESS_DENIED, "writing the read end");

    /* A writer thread pushes more than the pipe holds; the reader collects everything, then EOF. */
    HANDLE Thread = 0;
    CheckStatus(HzCreateThread(PipeWriter, Write, &Thread, nullptr), STATUS_SUCCESS, "start a writer thread");
    uint32_t Total = 0;
    bool Ordered = true;
    STATUS Status;
    static char Chunk[1500];
    while (HZ_SUCCESS(Status = NtReadFile(Read, Chunk, sizeof(Chunk), nullptr, &Moved)) && Status != STATUS_ALERTED) {
        for (uint32_t i = 0; i < Moved; i++)
            Ordered = Ordered && Chunk[i] == (char)('a' + ((Total + i) % 4000) % 26);
        Total += Moved;
    }
    NtWaitForSingleObject(Thread, 0, nullptr);
    HZ_THREAD_BASIC_INFORMATION Info = {};
    NtQueryInformationThread(Thread, HZ_THREAD_BASIC_INFORMATION_CLASS, &Info, sizeof(Info));
    NtClose(Thread);
    Check(Status == STATUS_END_OF_FILE && Total == 40000 && Ordered && Info.ExitStatus == 0,
          "40000 bytes through a pipe, then end of file when the writer closes");
    NtClose(Read);

    CheckStatus(NtCreatePipe(&Read, &Write, 0), STATUS_SUCCESS, "another pipe");
    NtClose(Read);
    CheckStatus(NtWriteFile(Write, "x", 1, nullptr, &Moved), STATUS_PIPE_BROKEN, "writing with no reader");
    NtClose(Write);
    CheckStatus(NtCreatePipe(&Read, &Write, 2 * 1024 * 1024), STATUS_INVALID_PARAMETER, "an oversized pipe");

    /* A child's standard output is the pipe's write end: HzPrint lines arrive here. */
    CheckStatus(NtCreatePipe(&Read, &Write, 0), STATUS_SUCCESS, "a pipe for a child");
    HZ_PROCESS_PARAMETERS Parameters = {};
    Parameters.StandardOutput = Write;
    HANDLE Child = 0;
    uint32_t ChildId = 0;
    CheckStatus(NtCreateProcessEx("C:\\Nexora\\System\\hello.exe", "hello.exe piped", &Parameters, &Child, &ChildId),
                STATUS_SUCCESS, "start a child with standard output");
    NtClose(Write); /* only the child holds the write end now */
    static char Output[2048];
    uint32_t Length = 0;
    while (Length < sizeof(Output) - 1 &&
           NtReadFile(Read, Output + Length, sizeof(Output) - 1 - Length, nullptr, &Moved) == STATUS_SUCCESS)
        Length += Moved;
    Output[Length] = '\0';
    NtClose(Read);
    NtWaitForSingleObject(Child, 0, nullptr);
    HZ_PROCESS_BASIC_INFORMATION ChildInfo = {};
    NtQueryInformationProcess(Child, HZ_PROCESS_BASIC_INFORMATION_CLASS, &ChildInfo, sizeof(ChildInfo));
    NtClose(Child);
    Check(strncmp(Output, "Hello from Nexora OS user mode! hello.exe", 41) == 0 && Contains(Output, "[piped]") &&
              Contains(Output, "\n  stack 0x"),
          "the child's output arrives through the pipe, line by line");
    Check(ChildInfo.ExitStatus == 42, "and the child still exits normally");
    HZ_PROCESS_PARAMETERS Bad = {};
    Bad.StandardOutput = (HANDLE)0x7FF0;
    CheckStatus(NtCreateProcessEx("C:\\Nexora\\System\\hello.exe", nullptr, &Bad, &Child, nullptr), STATUS_INVALID_HANDLE,
                "an invalid standard handle");
    HANDLE Event = 0;
    NtCreateEvent(&Event, HZ_NOTIFICATION_EVENT, 0);
    Bad.StandardOutput = Event;
    CheckStatus(NtCreateProcessEx("C:\\Nexora\\System\\hello.exe", nullptr, &Bad, &Child, nullptr),
                STATUS_OBJECT_TYPE_MISMATCH, "a standard handle that is not a file");
    NtClose(Event);

    /* Opening another process by id, and ending it. */
    CheckStatus(NtCreateProcess("C:\\Nexora\\System\\usertest.exe", "usertest.exe spin", &Child, &ChildId), STATUS_SUCCESS,
                "start a spinning child");
    HANDLE Opened = 0;
    CheckStatus(NtOpenProcess(&Opened, PROCESS_TERMINATE | SYNCHRONIZE, ChildId), STATUS_SUCCESS, "open it by id");
    CheckStatus(NtTerminateProcess(Opened, 0x1234), STATUS_SUCCESS, "end it through that handle");
    NtWaitForSingleObject(Child, 0, nullptr);
    NtQueryInformationProcess(Child, HZ_PROCESS_BASIC_INFORMATION_CLASS, &ChildInfo, sizeof(ChildInfo));
    Check(ChildInfo.ExitStatus == 0x1234, "it ended with that status");
    NtClose(Opened);
    NtClose(Child);
    CheckStatus(NtOpenProcess(&Opened, PROCESS_QUERY_INFORMATION, ChildId), STATUS_INVALID_CID, "an ended process");
    CheckStatus(NtOpenProcess(&Opened, PROCESS_TERMINATE, 4), STATUS_ACCESS_DENIED, "the System process cannot be ended");
    CheckStatus(NtOpenProcess(&Opened, PROCESS_QUERY_INFORMATION, 4), STATUS_SUCCESS, "but can be queried");
    NtClose(Opened);
    CheckStatus(NtOpenProcess(&Opened, 0x80000000u, g_ProcessId), STATUS_INVALID_PARAMETER, "unknown access rights");
}

extern "C" int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "spin") == 0) {
        for (;;)
            NtYieldExecution();
    }

    TestEnvironment();
    TestBadCalls();
    TestEvents();
    TestMemory();
    TestThreads();
    TestExtendedState();
    TestSystemInformation();
    TestChildProcesses();
    TestPipes();

    /* Leave work behind: process exit must end these threads and close these handles. */
    HANDLE Leftover, Never;
    NtCreateEvent(&Never, HZ_NOTIFICATION_EVENT, 0);
    HzCreateThread(SpinRoutine, nullptr, &Leftover, nullptr);
    HzCreateThread(BlockRoutine, Never, nullptr, nullptr);

    HzPrint("usertest: %d checks, %d failed%s", g_Checks, g_Failures, CpuHasUsableAvx() ? " (AVX state tested)" : "");
    return g_Failures;
}

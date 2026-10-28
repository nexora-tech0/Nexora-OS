/* A tiny PE32+ console image importing a bounded Win32 and CRT surface. No Nexora runtime,
   Nexora headers, or linked Nexora CRT are used: this exercises foreign PE imports. */
typedef void *HANDLE;
typedef unsigned long DWORD;
typedef unsigned long long SIZE_T;
typedef struct { long long QuadPart; } LARGE_INTEGER;
typedef struct { DWORD LowDateTime, HighDateTime; } FILETIME;
typedef struct { unsigned long long First, Second; } SLIST_HEADER;

__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD StandardHandle);
__declspec(dllimport) HANDLE __stdcall CreateFileW(const unsigned short *Path, DWORD DesiredAccess,
                                                    DWORD ShareMode, void *SecurityAttributes,
                                                    DWORD CreationDisposition, DWORD Flags,
                                                    HANDLE TemplateFile);
__declspec(dllimport) DWORD __stdcall GetFileAttributesW(const unsigned short *Path);
__declspec(dllimport) int __stdcall WriteFile(HANDLE File, const void *Buffer, DWORD Length,
                                               DWORD *Written, void *Overlapped);
__declspec(dllimport) __declspec(noreturn) void __stdcall ExitProcess(DWORD ExitCode);
__declspec(dllimport) DWORD __stdcall GetLastError(void);
__declspec(dllimport) void __stdcall SetLastError(DWORD Error);
__declspec(dllimport) int __stdcall CloseHandle(HANDLE Handle);
__declspec(dllimport) DWORD __stdcall GetCurrentProcessId(void);
__declspec(dllimport) DWORD __stdcall GetCurrentThreadId(void);
__declspec(dllimport) HANDLE __stdcall GetCurrentProcess(void);
__declspec(dllimport) HANDLE __stdcall GetCurrentThread(void);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(LARGE_INTEGER *Counter);
__declspec(dllimport) void __stdcall GetSystemTimeAsFileTime(FILETIME *Time);
__declspec(dllimport) void __stdcall InitializeSListHead(SLIST_HEADER *ListHead);
__declspec(dllimport) void *__stdcall GetModuleHandleW(const unsigned short *ModuleName);
__declspec(dllimport) void *__stdcall GetModuleHandleA(const char *ModuleName);
__declspec(dllimport) void *__stdcall GetProcAddress(void *Module, const char *Name);
__declspec(dllimport) DWORD __stdcall GetTickCount(void);
__declspec(dllimport) unsigned long long __stdcall GetTickCount64(void);
__declspec(dllimport) char *__stdcall GetCommandLineA(void);
__declspec(dllimport) const unsigned short *__stdcall GetCommandLineW(void);
__declspec(dllimport) HANDLE __stdcall GetProcessHeap(void);
__declspec(dllimport) void *__stdcall HeapAlloc(HANDLE Heap, DWORD Flags, SIZE_T Bytes);
__declspec(dllimport) int __stdcall HeapFree(HANDLE Heap, DWORD Flags, void *Memory);
__declspec(dllimport) void *__cdecl memcpy(void *Destination, const void *Source, SIZE_T Count);
__declspec(dllimport) void *__cdecl memset(void *Destination, int Value, SIZE_T Count);
__declspec(dllimport) SIZE_T __cdecl strlen(const char *String);
__declspec(dllimport) void *__cdecl malloc(SIZE_T Bytes);
__declspec(dllimport) void __cdecl free(void *Memory);

unsigned long __cdecl WinCompatExportProbe(void)
{
    return 0xC0DEC0DEu;
}

void WinCompatProbe(void)
{
    typedef void *(__cdecl *MEMCPY_FUNCTION)(void *, const void *, SIZE_T);
    typedef void *(__cdecl *MEMSET_FUNCTION)(void *, int, SIZE_T);
    typedef SIZE_T (__cdecl *STRLEN_FUNCTION)(const char *);
    typedef void *(__cdecl *MALLOC_FUNCTION)(SIZE_T);
    typedef void (__cdecl *FREE_FUNCTION)(void *);
    MEMCPY_FUNCTION volatile CopyBytes = memcpy;
    MEMSET_FUNCTION volatile FillBytes = memset;
    STRLEN_FUNCTION volatile StringLength = strlen;
    MALLOC_FUNCTION volatile AllocateMemory = malloc;
    FREE_FUNCTION volatile FreeMemory = free;
    static const char RuntimeSource[] = "crt-import-probe";
    char RuntimeDestination[32];
    FillBytes(RuntimeDestination, 0xA5, sizeof(RuntimeDestination));
    CopyBytes(RuntimeDestination, RuntimeSource, sizeof(RuntimeSource));
    if (StringLength(RuntimeDestination) != sizeof(RuntimeSource) - 1)
        ExitProcess(29);
    for (unsigned i = 0; i < sizeof(RuntimeSource); ++i)
        if (RuntimeDestination[i] != RuntimeSource[i])
            ExitProcess(30);
    if ((unsigned char)RuntimeDestination[sizeof(RuntimeSource)] != 0xA5)
        ExitProcess(31);
    unsigned char *CrtMemory = (unsigned char *)AllocateMemory(24);
    if (!CrtMemory)
        ExitProcess(32);
    FillBytes(CrtMemory, 0x6D, 24);
    for (unsigned i = 0; i < 24; ++i)
        if (CrtMemory[i] != 0x6D)
            ExitProcess(33);
    FreeMemory(CrtMemory);

    static const char Message[] = "nexora-wincompat-probe";
    DWORD Written = 0;
    HANDLE Output = GetStdHandle((DWORD)-11);
    /* start launches the probe without inherited stdio handles; null is expected in this test. */
    if (Output != 0)
        ExitProcess(10);
    char *CommandLine = GetCommandLineA();
    if (!CommandLine || CommandLine[0] == '\0')
        ExitProcess(18);
    const unsigned short *WideCommandLine = GetCommandLineW();
    if (!WideCommandLine || WideCommandLine[0] == 0)
        ExitProcess(40);
    SetLastError(0x1234);
    if (GetLastError() != 0x1234)
        ExitProcess(12);
    if (!GetModuleHandleW(0))
        ExitProcess(23);
    static const unsigned short MainModule[] = {
        'C', ':', '\\', 'N', 'e', 'x', 'o', 'r', 'a', '\\', 'S', 'y', 's', 't', 'e', 'm', '\\',
        'w', 'i', 'n', 'c', 'o', 'm', 'p', 'a', 't', '-', 'p', 'r', 'o', 'b', 'e', '.', 'e', 'x', 'e', 0
    };
    if (!GetModuleHandleW(MainModule) ||
        !GetModuleHandleA("C:\\Nexora\\System\\wincompat-probe.exe"))
        ExitProcess(28);
    if (!GetModuleHandleA(0))
        ExitProcess(26);
    typedef unsigned long (__cdecl *EXPORT_PROBE_FUNCTION)(void);
    EXPORT_PROBE_FUNCTION ResolveExport = (EXPORT_PROBE_FUNCTION)
        GetProcAddress(GetModuleHandleA(0), "WinCompatExportProbe");
    if (!ResolveExport || ResolveExport() != 0xC0DEC0DEu)
        ExitProcess(34);
    EXPORT_PROBE_FUNCTION ResolveOrdinal = (EXPORT_PROBE_FUNCTION)
        GetProcAddress(GetModuleHandleA(0), (const char *)1);
    if (!ResolveOrdinal || ResolveOrdinal() != 0xC0DEC0DEu)
        ExitProcess(35);
    static const unsigned short MissingModule[] = {
        'n', 'e', 'x', 'o', 'r', 'a', '-', 'm', 'i', 's', 's', 'i', 'n', 'g', '.', 'd', 'l', 'l', 0
    };
    if (GetModuleHandleW(MissingModule) != 0 || GetLastError() != 126)
        ExitProcess(24);
    DWORD TickCount = GetTickCount();
    unsigned long long TickCount64 = GetTickCount64();
    if (TickCount == 0 || TickCount64 < TickCount || TickCount64 > 0xFFFFFFFFull)
        ExitProcess(25);
    if (GetCurrentProcessId() == 0 || GetCurrentThreadId() == 0)
        ExitProcess(14);
    if (GetCurrentProcess() != (HANDLE)(long long)-1 || GetCurrentThread() != (HANDLE)(long long)-2)
        ExitProcess(27);
    LARGE_INTEGER Counter = {0};
    FILETIME Time = {0};
    if (!QueryPerformanceCounter(&Counter) || Counter.QuadPart == 0)
        ExitProcess(15);
    GetSystemTimeAsFileTime(&Time);
    if (Time.LowDateTime == 0 && Time.HighDateTime == 0)
        ExitProcess(16);
    SLIST_HEADER ListHead = {1, 1};
    InitializeSListHead(&ListHead);
    if (ListHead.First != 0 || ListHead.Second != 0)
        ExitProcess(17);
    HANDLE Heap = GetProcessHeap();
    if (!Heap)
        ExitProcess(19);
    unsigned char *Memory = (unsigned char *)HeapAlloc(Heap, 0, 37);
    if (!Memory)
        ExitProcess(20);
    for (unsigned i = 0; i < 37; i++)
        Memory[i] = (unsigned char)(i ^ 0xA5);
    for (unsigned i = 0; i < 37; i++)
        if (Memory[i] != (unsigned char)(i ^ 0xA5))
            ExitProcess(21);
    if (!HeapFree(Heap, 0, Memory) || HeapFree(Heap, 0, Memory))
        ExitProcess(22);
    /* Invalid-handle WriteFile must return FALSE through the Win32 BOOL thunk. */
    if (WriteFile(0, Message, sizeof(Message) - 1, &Written, 0))
        ExitProcess(11);
    if (CloseHandle(0))
        ExitProcess(13);
    static const unsigned short ProbeFile[] = {
        'C', ':', '\\', 'N', 'e', 'x', 'o', 'r', 'a', '\\', 'S', 'y', 's', 't', 'e', 'm', '\\',
        'w', 'i', 'n', 'c', 'o', 'm', 'p', 'a', 't', '-', 'p', 'r', 'o', 'b', 'e', '.', 'e', 'x', 'e', 0
    };
    HANDLE File = CreateFileW(ProbeFile, 0x80000000u, 1, 0, 3, 0x80, 0);
    if (File == (HANDLE)(long long)-1 || !CloseHandle(File))
        ExitProcess(36);
    static const unsigned short CreatedFilePath[] = {
        'D', ':', '\\',
        'w', 'i', 'n', 'c', 'o', 'm', 'p', 'a', 't', '-', 'f', 'i', 'l', 'e', '-', 'p', 'r', 'o', 'b', 'e', '.', 't', 'm', 'p', 0
    };
    static const char FileContent[] = "CreateFileW-write-probe";
    File = CreateFileW(CreatedFilePath, 0x40000000u, 1, 0, 2, 0x80, 0);
    if (File == (HANDLE)(long long)-1)
        ExitProcess(37);
    Written = 0;
    if (!WriteFile(File, FileContent, sizeof(FileContent) - 1, &Written, 0) ||
        Written != sizeof(FileContent) - 1 || !CloseHandle(File))
        ExitProcess(38);
    if (GetFileAttributesW(CreatedFilePath) == 0xFFFFFFFFu)
        ExitProcess(39);
    ExitProcess(0);
}
